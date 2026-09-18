// 连续读段调度、有界执行器与独占屏障(P2:只读工具并行与写入串行单)的
// 行为钉子。断先后一律用闸门/条件变量/全局序号,不靠 sleep 猜并发:
//   1. 划批与策略:纯函数 PlanToolBatchSegments/ParseToolBatchStrategy/
//      ClampParallelReadConcurrency 与放行名单(含插件影子名/需确认档/
//      web_fetch 保守串行/tool_invoke 按真实目标判);
//   2. 有界执行器:峰值并发不超上限、结果按任务序、worker 异常不传播;
//   3. 读并行:两读同进执行体(进门闸证明真并发,串行会等闸超时);
//   4. 写屏障:R,R,W,R,R,W 断言读写区间不重叠、段间先后成立;write_file
//      与 edit_file 互不重叠;
//   5. 写后读:同路径先写后读读到新值(真工具真文件);
//   6. 配对:后声明的先完成(慢首枚),模型结果仍按声明序、ID 无错配;
//   7. 回退:默认 Exclusive/并发 1/Hook 在场/混入 job_handle = 旧串行;
//   8. 取消:段内已启动按真实终态、未启动补 cancelled_before_start,
//      每枚调用恰一份 tool_result;
//   9. 异常:worker 抛异常折成稳定错误(tool.execute.threw),不冒充成功,
//      批内其余照常收口;
//  10. 持久化:两枚 started 都先于任一 finished(finished 恰一份);
//  11. 逐调用审批账加锁后并发可用(衔接点 2 的烟测)。

#include <doctest/doctest.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <system_error>
#include <thread>
#include <tuple>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "agent/agent.hpp"
#include "agent/loop.hpp"
#include "agent/tool_batch_schedule.hpp"
#include "agent/tool_trace.hpp"
#include "api/types.hpp"
#include "app/tool_call_scope.hpp"
#include "runtime/interaction.hpp"
#include "tools/deferred_tool_resolver.hpp"
#include "tools/read_file.hpp"
#include "tools/registry.hpp"
#include "tools/tool.hpp"
#include "tools/write_file.hpp"

using namespace lubancode;

namespace {

// 等闸上限:进门闸在正确行为下立即满足(条件变量叫醒),只有调度坏成
// 串行才会吃满——那是失败路径,慢一点可忍。
constexpr auto kGateWait = std::chrono::seconds(3);

// ---------------------------------------------------------------------------
// 量具:活动账(enter/exit 全局序号判区间重叠)与并发闸(进门/出门两道)。
// ---------------------------------------------------------------------------

struct ActivitySpan {
    std::string name;
    std::uint64_t enter = 0;
    std::uint64_t exit = 0;
};

class ActivityLog {
public:
    std::uint64_t Tick() { return tick_.fetch_add(1, std::memory_order_relaxed); }
    void Record(std::string name, std::uint64_t enter, std::uint64_t exit) {
        const std::lock_guard<std::mutex> lock(mutex_);
        spans_.push_back(ActivitySpan{std::move(name), enter, exit});
    }
    std::vector<ActivitySpan> spans() const {
        const std::lock_guard<std::mutex> lock(mutex_);
        return spans_;
    }
    static bool Overlaps(const ActivitySpan& a, const ActivitySpan& b) {
        return a.enter < b.exit && b.enter < a.exit;
    }
    std::optional<ActivitySpan> Find(const std::string& name) const {
        const std::lock_guard<std::mutex> lock(mutex_);
        for (const auto& span : spans_) {
            if (span.name == name) {
                return span;
            }
        }
        return std::nullopt;
    }
    // 名字以 a_prefix/b_prefix 开头的区间里,有没有任何一对重叠。
    bool AnyOverlapBetween(const std::string& a_prefix, const std::string& b_prefix) const {
        for (const auto& a : spans()) {
            if (a.name.rfind(a_prefix, 0) != 0) continue;
            for (const auto& b : spans()) {
                if (b.name.rfind(b_prefix, 0) != 0) continue;
                if (a.name != b.name && Overlaps(a, b)) {
                    return true;
                }
            }
        }
        return false;
    }

private:
    std::atomic<std::uint64_t> tick_{0};
    mutable std::mutex mutex_;
    std::vector<ActivitySpan> spans_;
};

// 两道闸:进门(target 枚读都进了执行体才放行——真并行的证明)与出门
// (慢首枚等后面的都退完才退——完成序与声明序脱钩)。
struct ConcurrencyGate {
    std::mutex mutex;
    std::condition_variable cv;
    std::size_t entered = 0;
    std::size_t enter_target = 1;  // 1 = 不等
    std::size_t exited = 0;
    std::size_t exit_target = 0;   // 0 = 不等
    int gate_timeouts = 0;
};

// ---------------------------------------------------------------------------
// 假后端与脚本。
// ---------------------------------------------------------------------------

class FakeBackend : public api::Backend {
public:
    std::vector<std::vector<api::StreamEvent>> scripts;
    std::vector<api::Request> captured_requests;

    std::expected<void, api::Error> send_stream(
        const api::Request& request, const std::function<void(const api::StreamEvent&)>& on_event,
        const std::atomic<bool>* /*cancel*/ = nullptr) override {
        captured_requests.push_back(request);
        const std::size_t idx = captured_requests.size() - 1;
        if (idx >= scripts.size()) {
            return std::unexpected(api::Error{api::ErrorKind::Api, "FakeBackend: 脚本用完了", 0});
        }
        for (const auto& event : scripts[idx]) {
            on_event(event);
        }
        return {};
    }
};

using ScriptedCall = std::tuple<std::string, std::string, std::string>;  // (id, name, input_json)

std::vector<api::StreamEvent> BatchScript(const std::vector<ScriptedCall>& calls) {
    std::vector<api::StreamEvent> events;
    events.push_back(api::MessageStart{"msg", "model"});
    for (std::size_t i = 0; i < calls.size(); ++i) {
        events.push_back(api::ToolUseStart{static_cast<int>(i), std::get<0>(calls[i]), std::get<1>(calls[i])});
        events.push_back(api::ToolUseInputDelta{static_cast<int>(i), std::get<2>(calls[i])});
        events.push_back(api::ContentBlockDone{static_cast<int>(i)});
    }
    events.push_back(api::MessageDone{"tool_use", api::Usage{}});
    return events;
}

std::vector<api::StreamEvent> TextScript(const std::string& text) {
    return {
        api::MessageStart{"msg", "model"},
        api::TextDelta{text},
        api::ContentBlockDone{0},
        api::MessageDone{"end_turn", api::Usage{}},
    };
}

// 第二次请求里那条 tool_result 消息(按声明序)。
std::vector<const api::ToolResultBlock*> ResultBlocksOf(const api::Request& request) {
    std::vector<const api::ToolResultBlock*> out;
    for (const auto& message : request.messages) {
        if (message.role != api::Role::User) continue;
        for (const auto& block : message.content) {
            if (const auto* result = std::get_if<api::ToolResultBlock>(&block)) {
                out.push_back(result);
            }
        }
    }
    return out;
}

// 历史里的 tool_result 块(取消收场的轮次不发第二份请求,结果只在历史里)。
std::vector<const api::ToolResultBlock*> ResultBlocksOfHistory(const std::vector<api::Message>& history) {
    std::vector<const api::ToolResultBlock*> out;
    for (const auto& message : history) {
        if (message.role != api::Role::User) continue;
        for (const auto& block : message.content) {
            if (const auto* result = std::get_if<api::ToolResultBlock>(&block)) {
                out.push_back(result);
            }
        }
    }
    return out;
}

// ---------------------------------------------------------------------------
// 靶工具。
// ---------------------------------------------------------------------------

// 可闸的读靶:名字 read_file(进段资格靠它),进门/出门两道闸 + 活动账 +
// 在跑峰值。i 参数进 span 名,断言哪一枚读何时跑。
class GatedReadTool : public tools::Tool {
public:
    GatedReadTool(ActivityLog& log, ConcurrencyGate& gate) : log_(&log), gate_(&gate) {}

    std::string name() const override { return "read_file"; }
    std::string description() const override { return "可闸读靶"; }
    nlohmann::json input_schema() const override {
        return nlohmann::json{{"type", "object"},
                              {"properties",
                               nlohmann::json{
                                   {"i", nlohmann::json{{"type", "integer"}}},
                                   {"slow_first", nlohmann::json{{"type", "boolean"}}},
                               }}};
    }
    tools::EffectClass effect_class() const override { return tools::EffectClass::ReadOnlyLocal; }

    tools::Tool::Result execute(const nlohmann::json&) override { return {"不走旧口", true}; }
    tools::Tool::Result execute(const nlohmann::json& input, const tools::ToolExecutionContext&) override {
        ++executions;
        const int now_active = active_.fetch_add(1) + 1;
        int observed = peak_.load(std::memory_order_relaxed);
        while (now_active > observed &&
               !peak_.compare_exchange_weak(observed, now_active, std::memory_order_relaxed)) {
        }
        const std::string span = "read:" + std::to_string(input.value("i", 0));
        const std::uint64_t enter = log_->Tick();
        {
            std::unique_lock<std::mutex> lock(gate_->mutex);
            ++gate_->entered;
            gate_->cv.notify_all();
            if (gate_->enter_target > 1 &&
                !gate_->cv.wait_for(lock, kGateWait, [&] { return gate_->entered >= gate_->enter_target; })) {
                ++gate_->gate_timeouts;
            }
            if (input.value("slow_first", false) && gate_->exit_target > 0 &&
                !gate_->cv.wait_for(lock, kGateWait, [&] { return gate_->exited >= gate_->exit_target; })) {
                ++gate_->gate_timeouts;
            }
            ++gate_->exited;
            gate_->cv.notify_all();
        }
        const std::uint64_t exit = log_->Tick();
        log_->Record(span, enter, exit);
        active_.fetch_sub(1);
        return {"read#" + std::to_string(input.value("i", 0)), false};
    }

    std::atomic<int> executions{0};
    std::atomic<int> active_{0};
    std::atomic<int> peak_{0};

private:
    ActivityLog* log_;
    ConcurrencyGate* gate_;
};

// 跑完顺手升取消旗的读靶:取消时序的现场制造者。
class CancellingReadTool : public tools::Tool {
public:
    CancellingReadTool(std::atomic<bool>& cancel) : cancel_(cancel) {}
    std::string name() const override { return "read_file"; }
    std::string description() const override { return "收尾升取消旗的读靶"; }
    nlohmann::json input_schema() const override { return nlohmann::json::object(); }
    tools::EffectClass effect_class() const override { return tools::EffectClass::ReadOnlyLocal; }
    tools::Tool::Result execute(const nlohmann::json&) override { return {"不走旧口", true}; }
    tools::Tool::Result execute(const nlohmann::json&, const tools::ToolExecutionContext&) override {
        ++executions;
        cancel_.store(true);
        return {"跑完了", false};
    }
    std::atomic<int> executions{0};

private:
    std::atomic<bool>& cancel_;
};

// 会抛异常的读靶:worker 异常折算的现场。
class ThrowingReadTool : public tools::Tool {
public:
    std::string name() const override { return "read_file"; }
    std::string description() const override { return "抛异常的读靶"; }
    nlohmann::json input_schema() const override { return nlohmann::json::object(); }
    tools::EffectClass effect_class() const override { return tools::EffectClass::ReadOnlyLocal; }
    tools::Tool::Result execute(const nlohmann::json&) override {
        throw std::runtime_error("读靶炸了");
    }
};

// 独占靶:记活动区间(write_file/edit_file/job_cancel 之类,名字可配)。
class SpanTool : public tools::Tool {
public:
    SpanTool(std::string name, ActivityLog& log) : name_(std::move(name)), log_(&log) {}
    std::string name() const override { return name_; }
    std::string description() const override { return "独占靶 " + name_; }
    nlohmann::json input_schema() const override {
        return nlohmann::json{{"type", "object"},
                              {"properties", nlohmann::json{{"i", nlohmann::json{{"type", "integer"}}}}}};
    }
    tools::Tool::Result execute(const nlohmann::json&) override { return {"不走旧口", true}; }
    tools::Tool::Result execute(const nlohmann::json& input, const tools::ToolExecutionContext&) override {
        ++executions;
        const std::string span = name_ + ":" + std::to_string(input.value("i", 0));
        const std::uint64_t enter = log_->Tick();
        const std::uint64_t exit = log_->Tick();
        log_->Record(span, enter, exit);
        return {"did " + span, false};
    }
    std::atomic<int> executions{0};

private:
    std::string name_;
    ActivityLog* log_;
};

// trace 栅栏收集器。
struct TraceCollector {
    mutable std::mutex mutex;
    std::vector<agent::ToolTraceEvent> events;
    agent::TurnWiring Decorate(agent::TurnWiring wiring) {
        wiring.on_tool_trace = [this](const agent::ToolTraceEvent& event) {
            const std::lock_guard<std::mutex> lock(mutex);
            events.push_back(event);
        };
        return wiring;
    }
    std::vector<agent::ToolTraceEvent> Take() const {
        const std::lock_guard<std::mutex> lock(mutex);
        return events;
    }
    std::size_t Count(agent::ToolTraceEventKind kind, const std::string& tool_use_id = std::string()) const {
        std::size_t n = 0;
        for (const auto& event : Take()) {
            if (event.kind == kind && (tool_use_id.empty() || event.tool_use_id == tool_use_id)) {
                ++n;
            }
        }
        return n;
    }
    std::optional<std::size_t> Position(agent::ToolTraceEventKind kind, const std::string& tool_use_id) const {
        const auto all = Take();
        for (std::size_t i = 0; i < all.size(); ++i) {
            if (all[i].kind == kind && all[i].tool_use_id == tool_use_id) {
                return i;
            }
        }
        return std::nullopt;
    }
};

agent::AgentProfile MakeProfile(agent::ToolBatchStrategy strategy = agent::ToolBatchStrategy::ParallelRead,
                                int concurrency = 4) {
    agent::AgentProfile profile;
    profile.request.model = "test-model";
    profile.system_prompt = "system prompt";
    profile.runtime.tool_batch_strategy = strategy;
    profile.runtime.parallel_read_concurrency = concurrency;
    return profile;
}

std::string ReadInput(int i, bool slow_first = false) {
    nlohmann::json input{{"i", i}};
    if (slow_first) {
        input["slow_first"] = true;
    }
    return input.dump();
}

}  // namespace

// ---------------------------------------------------------------------------
// 划批与策略(纯函数)
// ---------------------------------------------------------------------------

TEST_CASE("划批:eligible 连续段成并行段,不 eligible 各自成独占节点") {
    const std::vector<agent::ToolBatchSegment> segments =
        agent::PlanToolBatchSegments({1, 1, 0, 1, 1, 0});
    REQUIRE(segments.size() == 4);
    CHECK(segments[0].parallel);
    CHECK(segments[0].begin == 0);
    CHECK(segments[0].end == 2);
    CHECK_FALSE(segments[1].parallel);
    CHECK(segments[1].begin == 2);
    CHECK(segments[1].end == 3);
    CHECK(segments[2].parallel);
    CHECK(segments[2].begin == 3);
    CHECK(segments[2].end == 5);
    CHECK_FALSE(segments[3].parallel);
    CHECK(segments[3].begin == 5);
    CHECK(segments[3].end == 6);

    // 边角:全独占/全并行/空。
    CHECK(agent::PlanToolBatchSegments({0, 0}).size() == 2);
    CHECK(agent::PlanToolBatchSegments({1, 1, 1}).size() == 1);
    CHECK(agent::PlanToolBatchSegments({}).empty());
    // 单枚读自成一段(段长 1,经执行器同口跑,行为一致)。
    const auto single = agent::PlanToolBatchSegments({0, 1, 0});
    REQUIRE(single.size() == 3);
    CHECK(single[1].parallel);
    CHECK(single[1].begin == 1);
    CHECK(single[1].end == 2);
}

TEST_CASE("策略:两档解析/缺省/并发钳制") {
    CHECK(agent::ParseToolBatchStrategy("exclusive") == agent::ToolBatchStrategy::Exclusive);
    CHECK(agent::ParseToolBatchStrategy("parallel_read") == agent::ToolBatchStrategy::ParallelRead);
    CHECK_FALSE(agent::ParseToolBatchStrategy("ParallelRead").has_value());  // 大小写敏感
    CHECK_FALSE(agent::ParseToolBatchStrategy("bogus").has_value());
    CHECK(agent::ToolBatchStrategyName(agent::ToolBatchStrategy::ParallelRead) == "parallel_read");

    CHECK(agent::ClampParallelReadConcurrency(-3) == 1);
    CHECK(agent::ClampParallelReadConcurrency(0) == 1);
    CHECK(agent::ClampParallelReadConcurrency(1) == 1);
    CHECK(agent::ClampParallelReadConcurrency(4) == 4);
    CHECK(agent::ClampParallelReadConcurrency(999) == agent::kMaxParallelReadConcurrency);
}

TEST_CASE("放行名单:只认审定过的内置 read_file/search,插件影子/需确认/未审一律不放") {
    ActivityLog log;
    tools::ToolRegistry registry;
    registry.Register(std::make_unique<SpanTool>("read_file", log));  // 内置门注册
    registry.Register(std::make_unique<SpanTool>("search", log));
    registry.Register(std::make_unique<SpanTool>("web_fetch", log));
    registry.Register(std::make_unique<SpanTool>("web_search", log));
    registry.Register(std::make_unique<SpanTool>("job_cancel", log));
    registry.Register(std::make_unique<SpanTool>("read_status", log));  // 名字含 read 也不放

    CHECK(agent::IsParallelReadAllowlistedBuiltin(registry, "read_file"));
    CHECK(agent::IsParallelReadAllowlistedBuiltin(registry, "search"));
    CHECK_FALSE(agent::IsParallelReadAllowlistedBuiltin(registry, "web_fetch"));   // 首版保守串行
    CHECK_FALSE(agent::IsParallelReadAllowlistedBuiltin(registry, "web_search"));  // 同上
    CHECK_FALSE(agent::IsParallelReadAllowlistedBuiltin(registry, "job_cancel"));  // 宿主状态操作
    CHECK_FALSE(agent::IsParallelReadAllowlistedBuiltin(registry, "read_status")); // 不按名字含 read 放行
    CHECK_FALSE(agent::IsParallelReadAllowlistedBuiltin(registry, "no_such_tool"));

    // 插件来源的同名工具不许影子放行(注册元数据 source_kind=PluginLua)。
    tools::ToolRegistration plugin_registration;
    plugin_registration.tool = std::make_unique<SpanTool>("read_file", log);
    plugin_registration.source_kind = tools::ToolSourceKind::PluginLua;
    tools::ToolRegistry shadow_registry;
    shadow_registry.Register(std::move(plugin_registration));
    CHECK_FALSE(agent::IsParallelReadAllowlistedBuiltin(shadow_registry, "read_file"));
}

TEST_CASE("tool_invoke 探针:按真实目标判;解不开/没装 resolver 按独占收口") {
    ActivityLog log;
    tools::ToolRegistry registry;
    registry.Register(std::make_unique<SpanTool>("read_file", log));

    // 没装 resolver:tool_invoke 不判(独占路原样处理)。
    api::ToolUseBlock wire;
    wire.id = "u_proxy";
    wire.name = "tool_invoke";
    wire.input = nlohmann::json{{"tool_ref", "ref-1"}, {"arguments", nlohmann::json::object()}};
    CHECK_FALSE(agent::ProbeParallelReadEligibility(registry, /*resolver=*/nullptr, wire).eligible);

    // 装上 resolver、discover 一枚 read_file:解引用后按真实目标放行。
    tools::DeferredToolResolver resolver("test");
    const tools::DeferredToolRefRecord record =
        resolver.Discover(*registry.Find("read_file"), registry.RegistrationOf("read_file"),
                          /*catalog_revision=*/"r1", /*discovered_event_id=*/"e1", /*schema_expanded=*/true);
    api::ToolUseBlock invoke = wire;
    invoke.input = nlohmann::json{{"tool_ref", record.tool_ref},
                                  {"arguments", nlohmann::json{{"i", 7}}}};
    const agent::ParallelReadEligibility probe = agent::ProbeParallelReadEligibility(registry, &resolver, invoke);
    REQUIRE(probe.eligible);
    CHECK(probe.via_proxy);
    CHECK(probe.resolved_call.name == "read_file");
    CHECK(probe.resolved_call.id == "u_proxy");  // 配对 id 沿用 wire 那枚
    CHECK(probe.resolved_call.input == nlohmann::json{{"i", 7}});
    CHECK(probe.proxy.transport_name == "tool_invoke");

    // 伪拼 ref:解不开,按独占收口(原错误路径在串行路出)。
    api::ToolUseBlock forged = invoke;
    forged.input = nlohmann::json{{"tool_ref", "ref-forged"}, {"arguments", nlohmann::json::object()}};
    CHECK_FALSE(agent::ProbeParallelReadEligibility(registry, &resolver, forged).eligible);

    // 直呼放行名单外的工具:不 eligible。
    api::ToolUseBlock plain;
    plain.id = "u_plain";
    plain.name = "web_fetch";
    CHECK_FALSE(agent::ProbeParallelReadEligibility(registry, &resolver, plain).eligible);
}

// ---------------------------------------------------------------------------
// 有界执行器
// ---------------------------------------------------------------------------

TEST_CASE("执行器:峰值并发不超上限,结果按任务序回填") {
    for (const int limit : {1, 2, 3}) {
        ConcurrencyGate gate;
        gate.enter_target = static_cast<std::size_t>(limit);
        agent::BoundedParallelExecutor executor;
        std::vector<std::function<tools::Tool::Result()>> tasks;
        constexpr int kTasks = 7;
        for (int i = 0; i < kTasks; ++i) {
            tasks.push_back([&gate, i] {
                std::unique_lock<std::mutex> lock(gate.mutex);
                ++gate.entered;
                gate.cv.notify_all();
                if (!gate.cv.wait_for(lock, kGateWait,
                                      [&] { return gate.entered >= gate.enter_target; })) {
                    ++gate.gate_timeouts;
                }
                return tools::Tool::Result{"task#" + std::to_string(i), false};
            });
        }
        const agent::BoundedParallelExecutor::RunOutcome run = executor.RunAll(limit, std::move(tasks));
        REQUIRE(run.results.size() == kTasks);
        for (int i = 0; i < kTasks; ++i) {
            CHECK(run.results[static_cast<std::size_t>(i)].content == "task#" + std::to_string(i));
        }
        CHECK(run.peak_concurrency == static_cast<std::size_t>(limit));
        CHECK(gate.gate_timeouts == 0);
    }
}

TEST_CASE("执行器:worker 异常不传播,exception_ptr 交回主线程") {
    agent::BoundedParallelExecutor executor;
    std::vector<std::function<tools::Tool::Result()>> tasks;
    tasks.push_back([] { return tools::Tool::Result{"ok-0", false}; });
    tasks.push_back([]() -> tools::Tool::Result { throw std::runtime_error("boom"); });
    tasks.push_back([] { return tools::Tool::Result{"ok-2", false}; });
    const agent::BoundedParallelExecutor::RunOutcome run = executor.RunAll(3, std::move(tasks));
    CHECK(run.results[0].content == "ok-0");
    CHECK(run.results[2].content == "ok-2");
    REQUIRE(run.failures.size() == 3);
    CHECK(run.failures[1] != nullptr);
    CHECK(run.failures[0] == nullptr);
    CHECK(run.failures[2] == nullptr);

    // 空任务直接空账返回。
    agent::BoundedParallelExecutor empty_executor;
    const auto empty = empty_executor.RunAll(4, {});
    CHECK(empty.results.empty());
    CHECK(empty.peak_concurrency == 0);
}

// ---------------------------------------------------------------------------
// 端到端:AgentLoop::Run 的批次第二遍
// ---------------------------------------------------------------------------

TEST_CASE("读并行:两读同进执行体,结果按声明序配对回填") {
    FakeBackend backend;
    ActivityLog log;
    ConcurrencyGate gate;
    gate.enter_target = 2;  // 两枚都进了执行体才放行:串行路必吃等闸超时
    tools::ToolRegistry registry;
    auto read = std::make_unique<GatedReadTool>(log, gate);
    GatedReadTool* read_ptr = read.get();
    registry.Register(std::move(read));

    backend.scripts = {
        BatchScript({{"u_read_0", "read_file", ReadInput(0)}, {"u_read_1", "read_file", ReadInput(1)}}),
        TextScript("收工"),
    };
    agent::Agent loop(backend, registry, MakeProfile());
    TraceCollector collector;
    agent::TurnWiring wiring = collector.Decorate({});
    const auto result = loop.Run("两枚读", wiring);

    REQUIRE(result.has_value());
    REQUIRE(read_ptr->executions == 2);
    CHECK(gate.gate_timeouts == 0);              // 真并发:进门闸没人超时
    CHECK(read_ptr->peak_.load() == 2);          // 峰值在跑恰 2
    const auto spans = log.spans();
    REQUIRE(spans.size() == 2);
    CHECK(ActivityLog::Overlaps(spans[0], spans[1]));  // 区间重叠 = 同时在跑

    // 结果按声明序、id 配对、内容对号。
    REQUIRE(backend.captured_requests.size() == 2);
    const auto blocks = ResultBlocksOf(backend.captured_requests[1]);
    REQUIRE(blocks.size() == 2);
    CHECK(blocks[0]->tool_use_id == "u_read_0");
    CHECK(blocks[0]->content == "read#0");
    CHECK(blocks[1]->tool_use_id == "u_read_1");
    CHECK(blocks[1]->content == "read#1");
}

TEST_CASE("读并行:并发上限生效(6 读上限 2,峰值不超 2 且确有并行)") {
    FakeBackend backend;
    ActivityLog log;
    ConcurrencyGate gate;
    gate.enter_target = 2;  // 上限 2:每对读同进同出,不等人多
    tools::ToolRegistry registry;
    auto read = std::make_unique<GatedReadTool>(log, gate);
    GatedReadTool* read_ptr = read.get();
    registry.Register(std::move(read));

    std::vector<ScriptedCall> calls;
    for (int i = 0; i < 6; ++i) {
        calls.push_back({"u_read_" + std::to_string(i), "read_file", ReadInput(i)});
    }
    backend.scripts = {BatchScript(calls), TextScript("收工")};
    agent::Agent loop(backend, registry, MakeProfile(agent::ToolBatchStrategy::ParallelRead, /*concurrency=*/2));
    TraceCollector collector;
    const auto result = loop.Run("六枚读", collector.Decorate({}));

    REQUIRE(result.has_value());
    REQUIRE(read_ptr->executions == 6);
    CHECK(gate.gate_timeouts == 0);
    CHECK(read_ptr->peak_.load() == 2);  // 恰好 2,不超也不少于配置
    REQUIRE(backend.captured_requests.size() == 2);
    REQUIRE(ResultBlocksOf(backend.captured_requests[1]).size() == 6);
}

TEST_CASE("写屏障:R,R,W,R,R,W——读写区间不重叠,段间先后成立") {
    FakeBackend backend;
    ActivityLog log;
    ConcurrencyGate gate;
    gate.enter_target = 2;
    tools::ToolRegistry registry;
    auto read = std::make_unique<GatedReadTool>(log, gate);
    GatedReadTool* read_ptr = read.get();
    registry.Register(std::move(read));
    registry.Register(std::make_unique<SpanTool>("write_file", log));

    backend.scripts = {BatchScript({{"u0", "read_file", ReadInput(0)},
                                    {"u1", "read_file", ReadInput(1)},
                                    {"u2", "write_file", R"({"i":2})"},
                                    {"u3", "read_file", ReadInput(3)},
                                    {"u4", "read_file", ReadInput(4)},
                                    {"u5", "write_file", R"({"i":5})"}}),
                       TextScript("收工")};
    agent::Agent loop(backend, registry, MakeProfile());
    TraceCollector collector;
    const auto result = loop.Run("读写混编", collector.Decorate({}));

    REQUIRE(result.has_value());
    REQUIRE(read_ptr->executions == 4);
    // 任何读区间与任何写区间不重叠;两写之间也不重叠(独占节点串行)。
    CHECK_FALSE(log.AnyOverlapBetween("read:", "write_file:"));
    const auto write_first = log.Find("write_file:2");
    const auto write_second = log.Find("write_file:5");
    REQUIRE(write_first.has_value());
    REQUIRE(write_second.has_value());
    CHECK_FALSE(ActivityLog::Overlaps(*write_first, *write_second));
    // 段间屏障:前段读全退了写才进;写退了下段读才进。后面的 read 没有
    // 被提到前面的 write 之前。
    for (const std::string& early : {"read:0", "read:1"}) {
        const auto span = log.Find(early);
        REQUIRE(span.has_value());
        CHECK(span->exit < write_first->enter);
    }
    for (const std::string& late : {"read:3", "read:4"}) {
        const auto span = log.Find(late);
        REQUIRE(span.has_value());
        CHECK(span->enter > write_first->exit);
        CHECK(span->exit < write_second->enter);
    }
    // 同段两读确有重叠(真并行,不是被写屏障吓得串行)。
    const auto r0 = log.Find("read:0");
    const auto r1 = log.Find("read:1");
    REQUIRE(r0.has_value());
    REQUIRE(r1.has_value());
    CHECK(ActivityLog::Overlaps(*r0, *r1));
    CHECK(gate.gate_timeouts == 0);
    REQUIRE(backend.captured_requests.size() == 2);
    REQUIRE(ResultBlocksOf(backend.captured_requests[1]).size() == 6);
}

TEST_CASE("写屏障:write_file 与 edit_file 互不重叠") {
    FakeBackend backend;
    ActivityLog log;
    ConcurrencyGate gate;
    tools::ToolRegistry registry;
    registry.Register(std::make_unique<SpanTool>("write_file", log));
    registry.Register(std::make_unique<SpanTool>("edit_file", log));

    backend.scripts = {BatchScript({{"u0", "write_file", R"({"i":0})"},
                                    {"u1", "edit_file", R"({"i":1})"},
                                    {"u2", "write_file", R"({"i":2})"}}),
                       TextScript("收工")};
    agent::Agent loop(backend, registry, MakeProfile());
    TraceCollector collector;
    const auto result = loop.Run("两写一编", collector.Decorate({}));

    REQUIRE(result.has_value());
    CHECK_FALSE(log.AnyOverlapBetween("write_file:", "edit_file:"));
    CHECK_FALSE(log.AnyOverlapBetween("write_file:", "write_file:"));
}

TEST_CASE("写后读:同路径先写后读,读到新值(真工具真文件)") {
    FakeBackend backend;
    const std::filesystem::path dir =
        std::filesystem::temp_directory_path() /
        ("luban_p2_write_read_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directories(dir);
    const std::string path = (dir / "target.txt").generic_string();

    tools::ToolRegistry registry;
    registry.Register(std::make_unique<tools::ReadFileTool>());
    registry.Register(std::make_unique<tools::WriteFileTool>());

    const nlohmann::json write_input = nlohmann::json{{"path", path}, {"content", "新内容一行\n第二行\n"}};
    const nlohmann::json read_input = nlohmann::json{{"path", path}};
    backend.scripts = {BatchScript({{"u_write", "write_file", write_input.dump()},
                                    {"u_read", "read_file", read_input.dump()}}),
                       TextScript("收工")};
    agent::Agent loop(backend, registry, MakeProfile());
    agent::TurnWiring wiring;
    wiring.on_permission_evaluate = [](const std::string&, const std::string&, tools::ApprovalClass,
                                       const nlohmann::json&,
                                       const runtime::ToolHookDecision&) -> runtime::PermissionVerdict {
        runtime::PermissionVerdict verdict;
        verdict.action = runtime::PermissionVerdict::Action::Allow;  // 写靶放行,聚焦调度断言
        return verdict;
    };
    const auto result = loop.Run("先写后读", wiring);

    REQUIRE(result.has_value());
    REQUIRE(backend.captured_requests.size() == 2);
    const auto blocks = ResultBlocksOf(backend.captured_requests[1]);
    REQUIRE(blocks.size() == 2);
    CHECK(blocks[0]->tool_use_id == "u_write");
    CHECK_FALSE(blocks[0]->is_error);
    CHECK(blocks[1]->tool_use_id == "u_read");
    CHECK_FALSE(blocks[1]->is_error);
    // 读到的是写之后的新内容(写前没有这个文件;若读被提到写前,结果是
    // "文件不存在"错误)。
    CHECK(blocks[1]->content.find("新内容一行") != std::string::npos);
    CHECK(blocks[1]->content.find("第二行") != std::string::npos);

    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
}

TEST_CASE("配对:后声明的先完成(慢首枚),模型结果仍按声明序") {
    FakeBackend backend;
    ActivityLog log;
    ConcurrencyGate gate;
    gate.exit_target = 2;  // 首枚(声明序 0)等后两枚都退完才退
    tools::ToolRegistry registry;
    auto read = std::make_unique<GatedReadTool>(log, gate);
    GatedReadTool* read_ptr = read.get();
    registry.Register(std::move(read));

    backend.scripts = {BatchScript({{"u0", "read_file", ReadInput(0, /*slow_first=*/true)},
                                    {"u1", "read_file", ReadInput(1)},
                                    {"u2", "read_file", ReadInput(2)}}),
                       TextScript("收工")};
    agent::Agent loop(backend, registry, MakeProfile());
    TraceCollector collector;
    const auto result = loop.Run("慢首枚", collector.Decorate({}));

    REQUIRE(result.has_value());
    REQUIRE(read_ptr->executions == 3);
    CHECK(gate.gate_timeouts == 0);
    // 完成序脱钩:首枚最后退(区间最后收口)。
    const auto r0 = log.Find("read:0");
    const auto r1 = log.Find("read:1");
    const auto r2 = log.Find("read:2");
    REQUIRE(r0.has_value());
    REQUIRE(r1.has_value());
    REQUIRE(r2.has_value());
    CHECK(r0->exit > r1->exit);
    CHECK(r0->exit > r2->exit);
    // 模型结果仍按声明序,ID 无错配、无缺。
    REQUIRE(backend.captured_requests.size() == 2);
    const auto blocks = ResultBlocksOf(backend.captured_requests[1]);
    REQUIRE(blocks.size() == 3);
    CHECK(blocks[0]->tool_use_id == "u0");
    CHECK(blocks[0]->content == "read#0");
    CHECK(blocks[1]->tool_use_id == "u1");
    CHECK(blocks[1]->content == "read#1");
    CHECK(blocks[2]->tool_use_id == "u2");
    CHECK(blocks[2]->content == "read#2");
}

TEST_CASE("持久化:两枚 started 都先于任一 finished,finished 恰一份") {
    FakeBackend backend;
    ActivityLog log;
    ConcurrencyGate gate;
    gate.enter_target = 2;
    tools::ToolRegistry registry;
    registry.Register(std::make_unique<GatedReadTool>(log, gate));

    backend.scripts = {BatchScript({{"u0", "read_file", ReadInput(0)}, {"u1", "read_file", ReadInput(1)}}),
                       TextScript("收工")};
    agent::Agent loop(backend, registry, MakeProfile());
    TraceCollector collector;
    REQUIRE(loop.Run("trace", collector.Decorate({})).has_value());

    using agent::ToolTraceEventKind;
    CHECK(collector.Count(ToolTraceEventKind::ExecutionStarted) == 2);
    CHECK(collector.Count(ToolTraceEventKind::ExecutionFinished) == 2);
    CHECK(collector.Count(ToolTraceEventKind::ExecutionStarted, "u0") == 1);
    CHECK(collector.Count(ToolTraceEventKind::ExecutionFinished, "u0") == 1);
    CHECK(collector.Count(ToolTraceEventKind::ExecutionFinished, "u1") == 1);
    // 派发模型:两枚 started 都已落账,才有 finished(串行路是 start/finish
    // 交替,这里必须 start,start,finish,finish)。
    const auto started_u0 = collector.Position(ToolTraceEventKind::ExecutionStarted, "u0");
    const auto started_u1 = collector.Position(ToolTraceEventKind::ExecutionStarted, "u1");
    const auto finished_u0 = collector.Position(ToolTraceEventKind::ExecutionFinished, "u0");
    const auto finished_u1 = collector.Position(ToolTraceEventKind::ExecutionFinished, "u1");
    REQUIRE(started_u0.has_value());
    REQUIRE(started_u1.has_value());
    REQUIRE(finished_u0.has_value());
    REQUIRE(finished_u1.has_value());
    CHECK(*started_u0 < *finished_u0);
    CHECK(*started_u1 < *finished_u0);
    CHECK(*started_u1 < *finished_u1);
    // 批次尾:两枚 result_committed。
    CHECK(collector.Count(ToolTraceEventKind::ResultCommitted) == 2);
}

TEST_CASE("异常:worker 抛异常折成稳定错误,批内其余照常收口") {
    FakeBackend backend;
    tools::ToolRegistry registry;
    registry.Register(std::make_unique<ThrowingReadTool>());

    backend.scripts = {BatchScript({{"u0", "read_file", ReadInput(0)}, {"u1", "read_file", ReadInput(1)}}),
                       TextScript("收工")};
    agent::Agent loop(backend, registry, MakeProfile());
    TraceCollector collector;
    const auto result = loop.Run("炸一枚", collector.Decorate({}));

    REQUIRE(result.has_value());
    REQUIRE(backend.captured_requests.size() == 2);
    const auto blocks = ResultBlocksOf(backend.captured_requests[1]);
    REQUIRE(blocks.size() == 2);
    for (const auto* block : blocks) {
        REQUIRE(block->is_error);
        CHECK(block->content.find("工具在并行执行线程抛出异常") != std::string::npos);
        CHECK(block->content.find("读靶炸了") != std::string::npos);
    }
    using agent::ToolTraceEventKind;
    CHECK(collector.Count(ToolTraceEventKind::ExecutionStarted) == 2);
    CHECK(collector.Count(ToolTraceEventKind::ExecutionFinished) == 2);
}

TEST_CASE("策略:未知工具独占收口,读段照常并行,配对完整") {
    FakeBackend backend;
    ActivityLog log;
    ConcurrencyGate gate;  // target=1:单读不等
    tools::ToolRegistry registry;
    auto read = std::make_unique<GatedReadTool>(log, gate);
    GatedReadTool* read_ptr = read.get();
    registry.Register(std::move(read));

    backend.scripts = {BatchScript({{"u0", "read_file", ReadInput(0)},
                                    {"u1", "no_such_tool", R"({})"},
                                    {"u2", "read_file", ReadInput(2)}}),
                       TextScript("收工")};
    agent::Agent loop(backend, registry, MakeProfile());
    TraceCollector collector;
    const auto result = loop.Run("未知工具", collector.Decorate({}));

    REQUIRE(result.has_value());
    REQUIRE(read_ptr->executions == 2);
    REQUIRE(backend.captured_requests.size() == 2);
    const auto blocks = ResultBlocksOf(backend.captured_requests[1]);
    REQUIRE(blocks.size() == 3);
    CHECK(blocks[1]->tool_use_id == "u1");
    REQUIRE(blocks[1]->is_error);
    CHECK(blocks[1]->content.find("未知工具") != std::string::npos);
}

TEST_CASE("策略:宿主状态操作串行——读段收口后才轮到它,后面的读不提前") {
    FakeBackend backend;
    ActivityLog log;
    ConcurrencyGate gate;
    tools::ToolRegistry registry;
    registry.Register(std::make_unique<GatedReadTool>(log, gate));
    registry.Register(std::make_unique<SpanTool>("job_cancel", log));

    backend.scripts = {BatchScript({{"u0", "read_file", ReadInput(0)},
                                    {"u1", "job_cancel", R"({"i":1})"},
                                    {"u2", "read_file", ReadInput(2)}}),
                       TextScript("收工")};
    agent::Agent loop(backend, registry, MakeProfile());
    TraceCollector collector;
    const auto result = loop.Run("状态操作", collector.Decorate({}));

    REQUIRE(result.has_value());
    CHECK_FALSE(log.AnyOverlapBetween("read:", "job_cancel:"));
    const auto state = log.Find("job_cancel:1");
    const auto r0 = log.Find("read:0");
    const auto r2 = log.Find("read:2");
    REQUIRE(state.has_value());
    REQUIRE(r0.has_value());
    REQUIRE(r2.has_value());
    CHECK(r0->exit < state->enter);  // 前读全收口,状态操作才进
    CHECK(r2->enter > state->exit);  // 状态操作收口,后读才启
    REQUIRE(backend.captured_requests.size() == 2);
    REQUIRE(ResultBlocksOf(backend.captured_requests[1]).size() == 3);
}

// ---------------------------------------------------------------------------
// 回退:默认 Exclusive / 并发 1 / Hook 在场 / 混入 job_handle = 旧串行
// ---------------------------------------------------------------------------

TEST_CASE("回退:默认策略(Exclusive)两读串行——区间不重叠") {
    FakeBackend backend;
    ActivityLog log;
    ConcurrencyGate gate;  // target=1,不等
    tools::ToolRegistry registry;
    auto read = std::make_unique<GatedReadTool>(log, gate);
    GatedReadTool* read_ptr = read.get();
    registry.Register(std::move(read));

    backend.scripts = {BatchScript({{"u0", "read_file", ReadInput(0)}, {"u1", "read_file", ReadInput(1)}}),
                       TextScript("收工")};
    agent::Agent loop(backend, registry, MakeProfile(agent::ToolBatchStrategy::Exclusive));
    TraceCollector collector;
    const auto result = loop.Run("默认串行", collector.Decorate({}));

    REQUIRE(result.has_value());
    REQUIRE(read_ptr->executions == 2);
    CHECK(read_ptr->peak_.load() == 1);  // 峰值 1 = 逐枚串跑
    CHECK_FALSE(log.AnyOverlapBetween("read:", "read:"));
}

TEST_CASE("回退:并发上限 1 = 调度不接管,完整串行语义") {
    FakeBackend backend;
    ActivityLog log;
    ConcurrencyGate gate;
    tools::ToolRegistry registry;
    auto read = std::make_unique<GatedReadTool>(log, gate);
    GatedReadTool* read_ptr = read.get();
    registry.Register(std::move(read));

    backend.scripts = {BatchScript({{"u0", "read_file", ReadInput(0)}, {"u1", "read_file", ReadInput(1)}}),
                       TextScript("收工")};
    agent::Agent loop(backend, registry, MakeProfile(agent::ToolBatchStrategy::ParallelRead, /*concurrency=*/1));
    TraceCollector collector;
    const auto result = loop.Run("并发一", collector.Decorate({}));

    REQUIRE(result.has_value());
    REQUIRE(read_ptr->executions == 2);
    CHECK(read_ptr->peak_.load() == 1);
    CHECK_FALSE(log.AnyOverlapBetween("read:", "read:"));
    CHECK(gate.gate_timeouts == 0);
}

TEST_CASE("回退:PreToolUse Hook 在场整批回退串行(把 Hook 摁主线程不够)") {
    FakeBackend backend;
    ActivityLog log;
    ConcurrencyGate gate;
    tools::ToolRegistry registry;
    auto read = std::make_unique<GatedReadTool>(log, gate);
    GatedReadTool* read_ptr = read.get();
    registry.Register(std::move(read));

    backend.scripts = {BatchScript({{"u0", "read_file", ReadInput(0)}, {"u1", "read_file", ReadInput(1)}}),
                       TextScript("收工")};
    agent::Agent loop(backend, registry, MakeProfile());
    TraceCollector collector;
    agent::TurnWiring wiring = collector.Decorate({});
    wiring.on_pre_tool_use_hook = [](const std::string&, const std::string&,
                                     const nlohmann::json&) -> runtime::ToolHookDecision {
        runtime::ToolHookDecision decision;
        decision.decision = runtime::ToolHookDecision::Decision::Allow;  // 放行,但它的在场就是降级信号
        return decision;
    };
    const auto result = loop.Run("带 Hook", wiring);

    REQUIRE(result.has_value());
    REQUIRE(read_ptr->executions == 2);
    CHECK(read_ptr->peak_.load() == 1);  // 整批回退串行
    CHECK_FALSE(log.AnyOverlapBetween("read:", "read:"));
}

TEST_CASE("回退:混入 job_handle 调用禁用本次并行,inline 读照旧串行") {
    class FakeBatchGate : public agent::ToolBatchGate {
    public:
        bool OnCallItemComplete(const api::ToolUseBlock&, const StreamCallContext&) override { return false; }
        std::vector<agent::ToolCallAdjudication> AdjudicateBatch(
            const std::vector<api::ToolUseBlock>& calls) override {
            std::vector<agent::ToolCallAdjudication> out(calls.size());
            out[0].mode = agent::ToolProtocolMode::JobHandle;  // 首枚走接单,余下 inline
            return out;
        }
        std::optional<tools::Tool::Result> TakeJobOrder(const api::ToolUseBlock&,
                                                        const agent::ToolCallAdjudication&) override {
            return tools::Tool::Result{"接单回执", false};
        }
        void PumpBatchBoundary() override {}
    };

    FakeBackend backend;
    ActivityLog log;
    ConcurrencyGate gate;
    tools::ToolRegistry registry;
    auto read = std::make_unique<GatedReadTool>(log, gate);
    GatedReadTool* read_ptr = read.get();
    registry.Register(std::move(read));

    backend.scripts = {BatchScript({{"u0", "read_file", ReadInput(0)},
                                    {"u1", "read_file", ReadInput(1)},
                                    {"u2", "read_file", ReadInput(2)}}),
                       TextScript("收工")};
    FakeBatchGate batch_gate;
    agent::Agent loop(backend, registry, MakeProfile());
    TraceCollector collector;
    agent::TurnWiring wiring = collector.Decorate({});
    wiring.tool_batch_gate = &batch_gate;
    const auto result = loop.Run("混异步", wiring);

    REQUIRE(result.has_value());
    REQUIRE(read_ptr->executions == 2);  // u0 被协调器接单,真执行的是 u1/u2
    CHECK(read_ptr->peak_.load() == 1);  // 整批串行
    CHECK_FALSE(log.AnyOverlapBetween("read:", "read:"));
    REQUIRE(backend.captured_requests.size() == 2);
    const auto blocks = ResultBlocksOf(backend.captured_requests[1]);
    REQUIRE(blocks.size() == 3);
    CHECK(blocks[0]->tool_use_id == "u0");
    CHECK(blocks[0]->content == "接单回执");
    CHECK(blocks[0]->job_admission);
}

// ---------------------------------------------------------------------------
// 取消:已启动按真实终态;未启动补 cancelled_before_start,恰一份结果
// ---------------------------------------------------------------------------

TEST_CASE("取消:段内已启动的读按真实终态收口,Run 报 cancelled") {
    FakeBackend backend;
    std::atomic<bool> cancel_flag{false};
    tools::ToolRegistry registry;
    auto read = std::make_unique<CancellingReadTool>(cancel_flag);
    CancellingReadTool* read_ptr = read.get();
    registry.Register(std::move(read));

    backend.scripts = {BatchScript({{"u0", "read_file", ReadInput(0)}, {"u1", "read_file", ReadInput(1)}}),
                       TextScript("收工")};
    agent::Agent loop(backend, registry, MakeProfile());
    TraceCollector collector;
    const auto result = loop.Run("段内取消", collector.Decorate({}), &cancel_flag);

    REQUIRE(result.has_value());
    CHECK(result->cancelled);
    // 两枚都已越过 started(阶段二整段先落账),按真实终态:都真执行了。
    REQUIRE(read_ptr->executions == 2);
    using agent::ToolTraceEventKind;
    CHECK(collector.Count(ToolTraceEventKind::ExecutionStarted) == 2);
    CHECK(collector.Count(ToolTraceEventKind::ExecutionFinished) == 2);
    CHECK(collector.Count(ToolTraceEventKind::ExecutionFinished, "u0") == 1);
    CHECK(collector.Count(ToolTraceEventKind::ExecutionFinished, "u1") == 1);
    // 取消收场不发第二份请求:每枚调用恰一份 tool_result,都在历史里。
    REQUIRE(backend.captured_requests.size() == 1);
    const auto blocks = ResultBlocksOfHistory(loop.history());
    REQUIRE(blocks.size() == 2);
    CHECK(blocks[0]->tool_use_id == "u0");
    CHECK_FALSE(blocks[0]->is_error);
    CHECK(blocks[1]->tool_use_id == "u1");
    CHECK_FALSE(blocks[1]->is_error);
}

TEST_CASE("取消:读段收口后取消——未启动的独占节点补 cancelled_before_start") {
    FakeBackend backend;
    ActivityLog log;
    std::atomic<bool> cancel_flag{false};
    tools::ToolRegistry registry;
    registry.Register(std::make_unique<CancellingReadTool>(cancel_flag));
    registry.Register(std::make_unique<SpanTool>("write_file", log));

    backend.scripts = {BatchScript({{"u0", "read_file", ReadInput(0)},
                                    {"u1", "write_file", R"({"i":1})"},
                                    {"u2", "write_file", R"({"i":2})"}}),
                       TextScript("收工")};
    agent::Agent loop(backend, registry, MakeProfile());
    TraceCollector collector;
    const auto result = loop.Run("跨段取消", collector.Decorate({}), &cancel_flag);

    REQUIRE(result.has_value());
    CHECK(result->cancelled);
    // 读真执行并升了旗;两枚写未启动:补 cancelled_before_start,不冒充执行。
    REQUIRE(log.spans().empty());  // 写靶一次都没跑
    using agent::ToolTraceEventKind;
    CHECK(collector.Count(ToolTraceEventKind::ExecutionStarted, "u0") == 1);
    CHECK(collector.Count(ToolTraceEventKind::ExecutionStarted, "u1") == 0);
    CHECK(collector.Count(ToolTraceEventKind::ExecutionStarted, "u2") == 0);
    CHECK(collector.Count(ToolTraceEventKind::ExecutionFinished, "u1") == 1);
    CHECK(collector.Count(ToolTraceEventKind::ExecutionFinished, "u2") == 1);
    // 每枚调用恰一份 tool_result(取消收场,结果只进历史)。
    REQUIRE(backend.captured_requests.size() == 1);
    const auto blocks = ResultBlocksOfHistory(loop.history());
    REQUIRE(blocks.size() == 3);
    CHECK(blocks[0]->tool_use_id == "u0");
    CHECK_FALSE(blocks[0]->is_error);
    for (const std::size_t i : {std::size_t{1}, std::size_t{2}}) {
        REQUIRE(blocks[i]->is_error);
        CHECK(blocks[i]->content.find("未执行") != std::string::npos);
    }
}

// ---------------------------------------------------------------------------
// 衔接点 2:逐调用审批账加锁后并发可用
// ---------------------------------------------------------------------------

TEST_CASE("审批账:并发 Record/Take 各拿各的,不串槽不崩") {
    app::ToolCallScopeTable table;
    constexpr int kPairs = 64;
    std::vector<std::thread> workers;
    for (int i = 0; i < kPairs; ++i) {
        workers.emplace_back([&table, i] {
            const std::string id = "toolu_" + std::to_string(i);
            runtime::ToolHookDecision pre;
            pre.reason = "reason-" + std::to_string(i);
            table.RecordPre(id, pre);
            table.RecordApprovalClass(id, tools::ApprovalClass::FileEdit);
        });
    }
    for (auto& worker : workers) {
        worker.join();
    }
    for (int i = 0; i < kPairs; ++i) {
        const app::ToolCallScope scope = table.Take("toolu_" + std::to_string(i));
        CHECK(scope.pre.reason == "reason-" + std::to_string(i));
        CHECK(scope.approval_class == tools::ApprovalClass::FileEdit);
    }
}
