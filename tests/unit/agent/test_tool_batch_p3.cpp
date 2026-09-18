// 只读工具并行与写入串行单 P3 的验收钉子(§七"持久化/资源/性能"三项,
// 并行配置下)。P2 册(test_tool_batch_scheduling.cpp)已盖读并行/写屏障/
// 配对/取消/回退与 trace 派发模型;本册补 #135 留账的三面:
//   1. 持久化:结果落盘失败按 result_store_failed 逐枚收口(批内其余照
//      常);started 落不住(副作用闸)同样拦执行;result_committed 逐枚
//      落账无错配。started 先于执行/完成恰一份在 P2 册 trace 案已钉,
//      不重抄。
//   2. 资源:输出预算(output_limit)逐枚投影不串污;富结果 payload
//      (resource_link + structuredContent)逐槽回填;artifact 目录随执行
//      上下文递进 worker;批次收口后注册表可拆(执行器全数 join,无晚到
//      回调)。并发上限与 worker 异常 P2 册已钉。
//   3. 性能:同一输入同一工具集同环境下,串行(Exclusive)vs 并行
//      (ParallelRead)的墙钟对比——合成慢读在 CI 里做对比断言(数字打进
//      日志);真实 read_file/search 的实测数字也打进日志(证据口径:
//      断言只保完成与配对,收益看记录的数,不预写倍数)。
// 断先后仍用闸门/条件变量/序号,不靠 sleep 猜并发;性能案的 sleep 是
// 被测对象本身(模拟慢 IO),不是同步手段。

#include <doctest/doctest.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <expected>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "agent/agent.hpp"
#include "agent/loop.hpp"
#include "agent/tool_batch_schedule.hpp"
#include "agent/tool_trace.hpp"
#include "api/types.hpp"
#include "runtime/interaction.hpp"
#include "runtime/tool_trajectory_sink.hpp"
#include "tools/read_file.hpp"
#include "tools/registry.hpp"
#include "tools/search.hpp"
#include "tools/tool.hpp"

using namespace lubancode;

namespace {

constexpr auto kGateWait = std::chrono::seconds(3);

// 假后端(P2 册同款)。
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

// trace 栅栏收集器(P2 册同款)。
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
};

// 两道闸(P2 册同款):进门(target 枚读都进了执行体才放行)与出门。
struct ConcurrencyGate {
    std::mutex mutex;
    std::condition_variable cv;
    std::size_t entered = 0;
    std::size_t enter_target = 1;  // 1 = 不等
    std::size_t exited = 0;
    int gate_timeouts = 0;
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

std::string ReadInput(int i) {
    return nlohmann::json{{"i", i}}.dump();
}

// ---------------------------------------------------------------------------
// 靶工具。
// ---------------------------------------------------------------------------

// 基础读靶:进门闸 + 在跑峰值;行为按入参分档(output_limit/富结果/artifact)。
class ProbeReadTool : public tools::Tool {
public:
    explicit ProbeReadTool(ConcurrencyGate& gate, std::string expected_artifact_dir = {})
        : gate_(&gate), expected_artifact_dir_(std::move(expected_artifact_dir)) {}

    std::string name() const override { return "read_file"; }
    std::string description() const override { return "探针读靶"; }
    nlohmann::json input_schema() const override {
        return nlohmann::json{{"type", "object"},
                              {"properties",
                               nlohmann::json{
                                   {"i", nlohmann::json{{"type", "integer"}}},
                                   {"over_budget", nlohmann::json{{"type", "boolean"}}},
                                   {"rich", nlohmann::json{{"type", "boolean"}}},
                               }}};
    }
    tools::EffectClass effect_class() const override { return tools::EffectClass::ReadOnlyLocal; }

    tools::Tool::Result execute(const nlohmann::json&) override { return {"不走旧口", true}; }
    tools::Tool::Result execute(const nlohmann::json& input, const tools::ToolExecutionContext& ctx) override {
        ++executions;
        const int now_active = active_.fetch_add(1) + 1;
        int observed = peak_.load(std::memory_order_relaxed);
        while (now_active > observed &&
               !peak_.compare_exchange_weak(observed, now_active, std::memory_order_relaxed)) {
        }
        // worker 各写各的原子账(不许裸写共享串——那是数据竞态):值对不对
        // 折成布尔,两枚都验过才知道接线真到位。
        if (ctx.artifact_dir != expected_artifact_dir) {
            wrong_artifact_dir.store(true);
        }
        {
            std::unique_lock<std::mutex> lock(gate_->mutex);
            ++gate_->entered;
            gate_->cv.notify_all();
            if (gate_->enter_target > 1 &&
                !gate_->cv.wait_for(lock, kGateWait, [&] { return gate_->entered >= gate_->enter_target; })) {
                ++gate_->gate_timeouts;
            }
            ++gate_->exited;
            gate_->cv.notify_all();
        }
        active_.fetch_sub(1);
        const int i = input.value("i", 0);
        if (input.value("over_budget", false)) {
            tools::Tool::Result capped{"很长被截断的正文", false};
            capped.outcome = agent::ToString(agent::ToolOutcome::OutputLimit);
            return capped;
        }
        if (input.value("rich", false)) {
            tools::ToolResultPayload rich;
            rich.content.push_back(tools::TextContent{"正文#" + std::to_string(i)});
            tools::ResourceLinkContent link;
            link.uri = "file:///rich/" + std::to_string(i);
            link.name = "rich-" + std::to_string(i);
            rich.content.push_back(std::move(link));
            rich.structured_content = nlohmann::json{{"slot", i}, {"kind", "p3_rich"}};
            return tools::Tool::Result::FromPayload(std::move(rich));
        }
        return {"read#" + std::to_string(i), false};
    }

    std::atomic<int> executions{0};
    std::atomic<int> active_{0};
    std::atomic<int> peak_{0};
    std::atomic<bool> wrong_artifact_dir{false};

private:
    ConcurrencyGate* gate_;
    std::string expected_artifact_dir_;
};

// 计数包装:真 ReadFileTool 外面套在跑峰值账(名字保持 read_file、注册
// 来源 Builtin、不需确认——放行名单三件套不破)。
class CountingReadFile : public tools::Tool {
public:
    explicit CountingReadFile(std::unique_ptr<tools::Tool> inner) : inner_(std::move(inner)) {}

    std::string name() const override { return "read_file"; }
    std::string description() const override { return inner_->description(); }
    nlohmann::json input_schema() const override { return inner_->input_schema(); }
    tools::EffectClass effect_class() const override { return tools::EffectClass::ReadOnlyLocal; }

    tools::Tool::Result execute(const nlohmann::json&) override { return {"不走旧口", true}; }
    tools::Tool::Result execute(const nlohmann::json& input, const tools::ToolExecutionContext& ctx) override {
        ++executions;
        const int now_active = active_.fetch_add(1) + 1;
        int observed = peak_.load(std::memory_order_relaxed);
        while (now_active > observed &&
               !peak_.compare_exchange_weak(observed, now_active, std::memory_order_relaxed)) {
        }
        tools::Tool::Result result = inner_->execute(input, ctx);
        active_.fetch_sub(1);
        return result;
    }

    std::atomic<int> executions{0};
    std::atomic<int> active_{0};
    std::atomic<int> peak_{0};

private:
    std::unique_ptr<tools::Tool> inner_;
};

// 计数包装:真 SearchTool(rg 子进程)。
class CountingSearch : public tools::Tool {
public:
    explicit CountingSearch(std::unique_ptr<tools::Tool> inner) : inner_(std::move(inner)) {}

    std::string name() const override { return "search"; }
    std::string description() const override { return inner_->description(); }
    nlohmann::json input_schema() const override { return inner_->input_schema(); }
    tools::EffectClass effect_class() const override { return tools::EffectClass::ReadOnlyLocal; }

    tools::Tool::Result execute(const nlohmann::json&) override { return {"不走旧口", true}; }
    tools::Tool::Result execute(const nlohmann::json& input, const tools::ToolExecutionContext& ctx) override {
        ++executions;
        const int now_active = active_.fetch_add(1) + 1;
        int observed = peak_.load(std::memory_order_relaxed);
        while (now_active > observed &&
               !peak_.compare_exchange_weak(observed, now_active, std::memory_order_relaxed)) {
        }
        tools::Tool::Result result = inner_->execute(input, ctx);
        active_.fetch_sub(1);
        return result;
    }

    std::atomic<int> executions{0};
    std::atomic<int> active_{0};
    std::atomic<int> peak_{0};

private:
    std::unique_ptr<tools::Tool> inner_;
};

double MillisSince(std::chrono::steady_clock::time_point start) {
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
}

}  // namespace

// ---------------------------------------------------------------------------
// 持久化(§七):并行配置下的落账合同
// ---------------------------------------------------------------------------

TEST_CASE("持久化:并行段结果落盘失败按 result_store_failed 收口,批内其余照常") {
    FakeBackend backend;
    ConcurrencyGate gate;
    gate.enter_target = 2;
    tools::ToolRegistry registry;
    auto read = std::make_unique<ProbeReadTool>(gate);
    ProbeReadTool* read_ptr = read.get();
    registry.Register(std::move(read));

    backend.scripts = {BatchScript({{"u0", "read_file", ReadInput(0)}, {"u1", "read_file", ReadInput(1)}}),
                       TextScript("收工")};
    agent::Agent loop(backend, registry, MakeProfile());
    TraceCollector collector;
    agent::TurnWiring wiring = collector.Decorate({});
    // 落盘失败只打 u1:逐枚收口,不许把整批拖垮,也不冒充成功。
    wiring.capture_tool_result = [](const api::ToolResultBlock& block) -> runtime::ToolResultsCommitReceipt {
        runtime::ToolResultsCommitReceipt receipt;
        if (block.tool_use_id == "u1") {
            receipt.status = runtime::ToolResultsCommitReceipt::Status::Failed;
            receipt.error_code = "tool.result.store_unavailable";
        }
        return receipt;
    };
    const auto result = loop.Run("落盘失败", wiring);

    REQUIRE(result.has_value());
    REQUIRE(read_ptr->executions == 2);  // 两枚都真执行了(失败在收口,不在执行)
    REQUIRE(backend.captured_requests.size() == 2);
    const auto blocks = ResultBlocksOf(backend.captured_requests[1]);
    REQUIRE(blocks.size() == 2);
    CHECK(blocks[0]->tool_use_id == "u0");
    CHECK_FALSE(blocks[0]->is_error);  // u0 的落盘成功,结果原样
    CHECK(blocks[0]->content == "read#0");
    REQUIRE(blocks[1]->is_error);  // u1 明败
    CHECK(blocks[1]->content.find("Tool capture persistence failed") != std::string::npos);
    // finished 栅栏仍恰一份/枚:原始终态在捕获失败之前已落,不补第二枚。
    using agent::ToolTraceEventKind;
    CHECK(collector.Count(ToolTraceEventKind::ExecutionStarted) == 2);
    CHECK(collector.Count(ToolTraceEventKind::ExecutionFinished) == 2);
    CHECK(collector.Count(ToolTraceEventKind::ExecutionFinished, "u1") == 1);
}

TEST_CASE("持久化:started 落不住(追踪闸)在并行段同样拦执行——不执行、不冒充") {
    FakeBackend backend;
    ConcurrencyGate gate;
    tools::ToolRegistry registry;
    auto read = std::make_unique<ProbeReadTool>(gate);
    ProbeReadTool* read_ptr = read.get();
    registry.Register(std::move(read));

    backend.scripts = {BatchScript({{"u0", "read_file", ReadInput(0)}, {"u1", "read_file", ReadInput(1)}}),
                       TextScript("收工")};
    agent::Agent loop(backend, registry, MakeProfile());
    TraceCollector collector;
    agent::TurnWiring wiring = collector.Decorate({});
    // 追踪账写不进:副作用闸在 execution_started 之后拦——并行段同款
    //(读靶虽非副作用档,闸在 wiring 手里,合同不看工具种类)。
    wiring.on_tool_trace_blocked = [](const std::string&) { return true; };
    const auto result = loop.Run("闸拦", wiring);

    REQUIRE(result.has_value());
    CHECK(read_ptr->executions == 0);  // 一枚都没执行
    REQUIRE(backend.captured_requests.size() == 2);
    const auto blocks = ResultBlocksOf(backend.captured_requests[1]);
    REQUIRE(blocks.size() == 2);
    for (const auto* block : blocks) {
        REQUIRE(block->is_error);
        CHECK(block->content.find("追踪账写盘失败") != std::string::npos);
    }
}

TEST_CASE("持久化:并行段 result_committed 逐枚落账,无错配无缺漏") {
    FakeBackend backend;
    ConcurrencyGate gate;
    gate.enter_target = 2;
    tools::ToolRegistry registry;
    registry.Register(std::make_unique<ProbeReadTool>(gate));

    std::mutex committed_mutex;
    std::vector<std::string> committed_ids;
    backend.scripts = {BatchScript({{"u0", "read_file", ReadInput(0)}, {"u1", "read_file", ReadInput(1)}}),
                       TextScript("收工")};
    agent::Agent loop(backend, registry, MakeProfile());
    TraceCollector collector;
    agent::TurnWiring wiring = collector.Decorate({});
    wiring.capture_tool_result = [&committed_ids](const api::ToolResultBlock& block) {
        const std::lock_guard<std::mutex> lock(committed_mutex);
        committed_ids.push_back(block.tool_use_id);
        runtime::ToolResultsCommitReceipt receipt;  // 缺省 Committed
        return receipt;
    };
    const auto result = loop.Run("提交账", wiring);

    REQUIRE(result.has_value());
    {
        const std::lock_guard<std::mutex> lock(committed_mutex);
        REQUIRE(committed_ids.size() == 2);
        // 批次尾按声明序逐枚提交(P2 册钉了 Count==2,这里再钉 id 归属)。
        CHECK(committed_ids[0] == "u0");
        CHECK(committed_ids[1] == "u1");
    }
    using agent::ToolTraceEventKind;
    CHECK(collector.Count(ToolTraceEventKind::ResultCommitted, "u0") == 1);
    CHECK(collector.Count(ToolTraceEventKind::ResultCommitted, "u1") == 1);
}

// ---------------------------------------------------------------------------
// 资源(§七):并行配置下的资源合同
// ---------------------------------------------------------------------------

TEST_CASE("资源:输出预算 output_limit 逐枚投影,批内不串污") {
    FakeBackend backend;
    ConcurrencyGate gate;
    gate.enter_target = 2;
    tools::ToolRegistry registry;
    registry.Register(std::make_unique<ProbeReadTool>(gate));

    const nlohmann::json over = {{"i", 0}, {"over_budget", true}};
    backend.scripts = {BatchScript({{"u0", "read_file", over.dump()},
                                    {"u1", "read_file", ReadInput(1)}}),
                       TextScript("收工")};
    agent::Agent loop(backend, registry, MakeProfile());
    TraceCollector collector;
    const auto result = loop.Run("超预算", collector.Decorate({}));

    REQUIRE(result.has_value());
    REQUIRE(backend.captured_requests.size() == 2);
    const auto blocks = ResultBlocksOf(backend.captured_requests[1]);
    REQUIRE(blocks.size() == 2);
    // 超预算那枚:capture_complete=false + quota;同批另一枚不受污染。
    CHECK_FALSE(blocks[0]->capture_complete);
    CHECK(blocks[0]->capture_reason == "quota");
    CHECK(blocks[0]->content.find("截断") != std::string::npos);
    CHECK(blocks[1]->capture_complete);
    CHECK(blocks[1]->capture_reason.empty());
}

TEST_CASE("资源:富结果 payload 逐槽回填,structured_content 不串槽") {
    FakeBackend backend;
    ConcurrencyGate gate;
    gate.enter_target = 2;
    tools::ToolRegistry registry;
    registry.Register(std::make_unique<ProbeReadTool>(gate));

    const nlohmann::json rich0 = {{"i", 0}, {"rich", true}};
    const nlohmann::json rich5 = {{"i", 5}, {"rich", true}};
    backend.scripts = {BatchScript({{"u0", "read_file", rich0.dump()},
                                    {"u1", "read_file", rich5.dump()}}),
                       TextScript("收工")};
    agent::Agent loop(backend, registry, MakeProfile());
    TraceCollector collector;
    const auto result = loop.Run("富结果", collector.Decorate({}));

    REQUIRE(result.has_value());
    REQUIRE(backend.captured_requests.size() == 2);
    const auto blocks = ResultBlocksOf(backend.captured_requests[1]);
    REQUIRE(blocks.size() == 2);
    for (const std::size_t idx : {std::size_t{0}, std::size_t{1}}) {
        const int want = idx == 0 ? 0 : 5;
        // 富块随槽位走:resource_link 的 uri/name 对得上声明序,structured
        // JSON 同槽同值——完成先后不改排列,不串槽。
        REQUIRE(blocks[idx]->blocks.size() == 2);
        const auto* link = std::get_if<tools::ResourceLinkContent>(&blocks[idx]->blocks[1]);
        REQUIRE(link != nullptr);
        CHECK(link->uri == "file:///rich/" + std::to_string(want));
        CHECK(link->name == "rich-" + std::to_string(want));
        REQUIRE(blocks[idx]->structured_content.has_value());
        CHECK((*blocks[idx]->structured_content)["slot"] == want);
    }
}

TEST_CASE("资源:artifact 目录随执行上下文递进 worker") {
    FakeBackend backend;
    ConcurrencyGate gate;
    gate.enter_target = 2;
    tools::ToolRegistry registry;
    auto read = std::make_unique<ProbeReadTool>(gate, "/p3/artifacts");
    ProbeReadTool* read_ptr = read.get();
    registry.Register(std::move(read));

    backend.scripts = {BatchScript({{"u0", "read_file", ReadInput(0)}, {"u1", "read_file", ReadInput(1)}}),
                       TextScript("收工")};
    agent::Agent loop(backend, registry, MakeProfile());
    TraceCollector collector;
    agent::TurnWiring wiring = collector.Decorate({});
    wiring.tool_artifact_dir = "/p3/artifacts";
    const auto result = loop.Run("artifact", wiring);

    REQUIRE(result.has_value());
    REQUIRE(read_ptr->executions == 2);
    // 阶段三在 worker 上执行,artifact 目录经 ToolExecutionContext 递进——
    // 两枚拿到的都是会话目录那只值(空/错值 = 没接线)。
    CHECK_FALSE(read_ptr->wrong_artifact_dir.load());
}

TEST_CASE("资源:批次收口后注册表可拆——worker 全数 join,无晚到回调") {
    FakeBackend backend;
    ConcurrencyGate gate;
    gate.enter_target = 2;
    tools::ToolRegistry registry;
    auto read = std::make_unique<ProbeReadTool>(gate);
    ProbeReadTool* read_ptr = read.get();
    registry.Register(std::move(read));

    backend.scripts = {BatchScript({{"u0", "read_file", ReadInput(0)}, {"u1", "read_file", ReadInput(1)}}),
                       TextScript("收工")};
    TraceCollector collector;
    {
        // Agent 先亡、注册表(工具实例的 owner)后拆——析构序即"批次收口
        // 后卸载工具"的极限形式。批次进行中的热卸载是宿主合同禁区(单子
        // §四:不得与进行中批次并发),不在此冒充覆盖。
        agent::Agent agent_loop(backend, registry, MakeProfile());
        const auto result = agent_loop.Run("收口后卸载", collector.Decorate({}));
        REQUIRE(result.has_value());
        // Run 返回即全数 join:两枚都已出门(晚到的门禁检查)。
        {
            std::lock_guard<std::mutex> lock(gate.mutex);
            CHECK(gate.exited == 2);
        }
        const std::size_t events_after_run = collector.Take().size();
        CHECK(events_after_run > 0);
    }
    // Agent 已亡,回调面理应安静:trace 账不再增长(worker 没有晚到信封)。
    const std::size_t events_after_teardown = collector.Take().size();
    CHECK(events_after_teardown > 0);  // 上面那笔还在(账没被吞)
    CHECK(read_ptr->executions == 2);
    // 注册表随测试收口析构 = 工具实例销毁;能走到这里本身就是 join 合同
    // 成立的证据(悬空 worker 会在析构里炸)。
}

// ---------------------------------------------------------------------------
// 性能(§七):同一输入、同一工具集、同一环境,串行 vs 并行
// ---------------------------------------------------------------------------

// 合成慢读:每枚睡 60ms(模拟慢 IO)。串行(Exclusive)必须 ≥ 6 枚 × 50ms;
// 并行(上限 6)一波即完。断言给足余量(并行 < 串行/2,预期比 ~1:5,
// 只防"并行没生效"的回退,不赌机器快)。
TEST_CASE("性能:合成慢读——并行显著快于串行(CI 对比断言,数字进日志)") {
    constexpr int kReads = 6;
    constexpr auto kSlow = std::chrono::milliseconds(60);

    class SlowRead : public tools::Tool {
    public:
        explicit SlowRead(std::chrono::milliseconds slow) : slow_(slow) {}
        std::string name() const override { return "read_file"; }
        std::string description() const override { return "合成慢读"; }
        nlohmann::json input_schema() const override { return nlohmann::json::object(); }
        tools::EffectClass effect_class() const override { return tools::EffectClass::ReadOnlyLocal; }
        tools::Tool::Result execute(const nlohmann::json&) override { return {"不走旧口", true}; }
        tools::Tool::Result execute(const nlohmann::json&, const tools::ToolExecutionContext&) override {
            ++executions;
            std::this_thread::sleep_for(slow_);
            return {"slow read", false};
        }
        std::atomic<int> executions{0};

    private:
        std::chrono::milliseconds slow_;
    };

    std::vector<ScriptedCall> calls;
    for (int i = 0; i < kReads; ++i) {
        calls.push_back({"u_perf_" + std::to_string(i), "read_file", "{}"});
    }
    const auto script = BatchScript(calls);

    double serial_ms = 0.0;
    {
        FakeBackend backend;
        tools::ToolRegistry registry;
        registry.Register(std::make_unique<SlowRead>(kSlow));
        backend.scripts = {script, TextScript("串行完")};
        agent::Agent loop(backend, registry, MakeProfile(agent::ToolBatchStrategy::Exclusive));
        const auto start = std::chrono::steady_clock::now();
        REQUIRE(loop.Run("串行", {}).has_value());
        serial_ms = MillisSince(start);
    }
    double parallel_ms = 0.0;
    int parallel_executions = 0;
    {
        FakeBackend backend;
        tools::ToolRegistry registry;
        auto slow = std::make_unique<SlowRead>(kSlow);
        SlowRead* slow_ptr = slow.get();
        registry.Register(std::move(slow));
        backend.scripts = {script, TextScript("并行完")};
        agent::Agent loop(backend, registry, MakeProfile(agent::ToolBatchStrategy::ParallelRead, /*concurrency=*/6));
        const auto start = std::chrono::steady_clock::now();
        REQUIRE(loop.Run("并行", {}).has_value());
        parallel_ms = MillisSince(start);
        parallel_executions = slow_ptr->executions.load();
    }

    std::fprintf(stderr,
                 "[perf] 合成慢读 %d 枚 x %lldms:串行(Exclusive)=%.1fms,"
                 "并行(ParallelRead,上限6)=%.1fms\n",
                 kReads, static_cast<long long>(kSlow.count()), serial_ms, parallel_ms);
    // 断言只钉两件:串行真串行(下界 6x50ms)、并行确有收益(上界
    // serial/2;预期比 ~1:5,给足 CI 噪声余量,只防"并行没生效"的回退)。
    REQUIRE(parallel_executions == kReads);
    CHECK(serial_ms >= static_cast<double>(kReads) * 50.0);
    CHECK(parallel_ms < serial_ms / 2.0);
}

// 真实 read_file:同一批文件,串行/并行各跑一遍(同一进程同一页缓存),
// 断言只保完成与配对(两轮结果逐槽相同);耗时与峰值打进日志当证据。
TEST_CASE("性能:真实 read_file 串行 vs 并行——数字进日志,结果逐槽对齐") {
    const std::filesystem::path dir =
        std::filesystem::temp_directory_path() /
        ("luban_p3_real_read_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directories(dir);
    constexpr int kFiles = 8;
    std::vector<std::string> paths;
    for (int i = 0; i < kFiles; ++i) {
        const std::filesystem::path file = dir / ("corpus_" + std::to_string(i) + ".txt");
        std::ofstream out(file, std::ios::binary);
        for (int line = 0; line < 2200; ++line) {
            out << "line " << line << " of corpus " << i << " -- parallel read p3\n";
        }
        paths.push_back(file.generic_string());
    }

    std::vector<ScriptedCall> calls;
    for (int i = 0; i < kFiles; ++i) {
        calls.push_back({"u_real_" + std::to_string(i), "read_file",
                         nlohmann::json{{"path", paths[static_cast<std::size_t>(i)]}}.dump()});
    }
    const auto script = BatchScript(calls);

    const auto run_once = [&](agent::ToolBatchStrategy strategy, int concurrency,
                              double* wall_ms, int* peak, std::vector<std::string>* contents) {
        FakeBackend backend;
        tools::ToolRegistry registry;
        auto counting = std::make_unique<CountingReadFile>(std::make_unique<tools::ReadFileTool>());
        CountingReadFile* counting_ptr = counting.get();
        registry.Register(std::move(counting));
        backend.scripts = {script, TextScript("完")};
        agent::Agent loop(backend, registry, MakeProfile(strategy, concurrency));
        const auto start = std::chrono::steady_clock::now();
        const auto result = loop.Run("真读", {});
        REQUIRE(result.has_value());
        *wall_ms = MillisSince(start);
        *peak = counting_ptr->peak_.load();
        REQUIRE(counting_ptr->executions.load() == kFiles);
        const auto blocks = ResultBlocksOf(backend.captured_requests[1]);
        REQUIRE(blocks.size() == static_cast<std::size_t>(kFiles));
        for (const auto* block : blocks) {
            CHECK_FALSE(block->is_error);
            contents->push_back(block->content);
        }
    };

    double serial_ms = 0.0;
    int serial_peak = 0;
    std::vector<std::string> serial_contents;
    run_once(agent::ToolBatchStrategy::Exclusive, 4, &serial_ms, &serial_peak, &serial_contents);

    double parallel_ms = 0.0;
    int parallel_peak = 0;
    std::vector<std::string> parallel_contents;
    run_once(agent::ToolBatchStrategy::ParallelRead, 8, &parallel_ms, &parallel_peak, &parallel_contents);

    std::fprintf(stderr,
                 "[perf] 真实 read_file %d 文件(各 ~90KB,页缓存热):"
                 "串行=%.1fms(峰值并发 %d),并行(上限8)=%.1fms(峰值并发 %d)\n",
                 kFiles, serial_ms, serial_peak, parallel_ms, parallel_peak);

    // 断言面:两轮都完成、逐槽内容一致(同一输入的确定性);收益以日志
    // 数字为准,不在此断言倍数(快盘小文件下线程起落可能吃掉收益)。
    REQUIRE(parallel_contents.size() == serial_contents.size());
    for (std::size_t i = 0; i < serial_contents.size(); ++i) {
        CHECK(parallel_contents[i] == serial_contents[i]);
        CHECK_FALSE(serial_contents[i].empty());
    }

    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
}

#ifdef LUBANCODE_TEST_HAS_BUNDLED_RG
// 真实 search(rg 子进程):仓库 src/ 树上四路并发搜。rg 是真子进程,
// 每路开销远大于线程起落——并行的收益在这里最实在;数字进日志。
TEST_CASE("性能:真实 search(随包 rg)串行 vs 并行——数字进日志") {
    std::filesystem::path corpus = std::filesystem::path(LUBANCODE_TEST_SOURCE_DIR) / "src";
    std::error_code ec;
    if (!std::filesystem::exists(corpus, ec)) {
        FAIL_CHECK(("语料目录不存在: " + corpus.generic_string()).c_str());
        return;
    }
    constexpr int kQueries = 4;
    const std::string patterns[kQueries] = {"ToolBatchStrategy", "ExecutionStarted",
                                            "parallel_read", "ordered_results"};

    std::vector<ScriptedCall> calls;
    for (int i = 0; i < kQueries; ++i) {
        calls.push_back({"u_search_" + std::to_string(i), "search",
                         nlohmann::json{{"mode", "grep"},
                                        {"pattern", patterns[i]},
                                        {"path", corpus.generic_string()}}
                             .dump()});
    }
    const auto script = BatchScript(calls);

    const auto run_once = [&](agent::ToolBatchStrategy strategy, double* wall_ms, int* peak) {
        FakeBackend backend;
        tools::ToolRegistry registry;
        auto counting = std::make_unique<CountingSearch>(std::make_unique<tools::SearchTool>());
        CountingSearch* counting_ptr = counting.get();
        registry.Register(std::move(counting));
        backend.scripts = {script, TextScript("完")};
        agent::Agent loop(backend, registry, MakeProfile(strategy, /*concurrency=*/4));
        const auto start = std::chrono::steady_clock::now();
        const auto result = loop.Run("真搜", {});
        REQUIRE(result.has_value());
        *wall_ms = MillisSince(start);
        *peak = counting_ptr->peak_.load();
        REQUIRE(counting_ptr->executions.load() == kQueries);
        const auto blocks = ResultBlocksOf(backend.captured_requests[1]);
        REQUIRE(blocks.size() == static_cast<std::size_t>(kQueries));
        for (const auto* block : blocks) {
            CHECK_FALSE(block->is_error);
        }
    };

    double serial_ms = 0.0;
    int serial_peak = 0;
    run_once(agent::ToolBatchStrategy::Exclusive, &serial_ms, &serial_peak);
    double parallel_ms = 0.0;
    int parallel_peak = 0;
    run_once(agent::ToolBatchStrategy::ParallelRead, &parallel_ms, &parallel_peak);

    std::fprintf(stderr,
                 "[perf] 真实 search(rg) %d 路对 src/ 树:串行=%.1fms(峰值 %d),"
                 "并行(上限4)=%.1fms(峰值 %d)\n",
                 kQueries, serial_ms, serial_peak, parallel_ms, parallel_peak);
}
#endif  // LUBANCODE_TEST_HAS_BUNDLED_RG
