// 只读并行单 P3 宿主验收·CLI 交互与 one-shot 共用执行面(RunTurn):开
// ParallelRead + 并发 2 的配置,经真 TurnRunner 装配(TurnWiring:审批账/
// 拒绝文案/Hook 接线/事件流出水)跑带确认工具的批次——钉"并行配置下宿主
// 面不缺环":
//   1. CLI 交互档:两枚读真并发(进门闸),会话级放行账(always_allowed)
//      预授权的写在读段收口后执行(屏障);作用域闸(scope_gate)拒掉的
//      写不执行、拒绝文案随 tool_result 回模型;
//   2. one-shot/--yes 档:auto_confirm 全放,同款批次照常并行收口;
//   3. 审计:事件流(唯一出水口)里每枚工具 ItemStarted/ItemCompleted
//      各恰一份,批次边界事件在。
// 配置→profile 的折档轴(agent.tool_execution)在
// tests/unit/config/test_runtime_profile.cpp 已钉,这里不重抄。Hook 在场
// 整批回退串行是 loop 级合同(P2 册),宿主面不另测。
// 断先后用进门闸(串行路必吃等闸超时),不靠 sleep 猜并发。
#include <doctest/doctest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <expected>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "agent/agent.hpp"
#include "agent/tool_batch_schedule.hpp"
#include "api/backend.hpp"
#include "api/types.hpp"
#include "app/turn_runner.hpp"
#include "cli/context_tracker.hpp"
#include "cli/theme.hpp"
#include "cli/transcript.hpp"
#include "runtime/event.hpp"
#include "runtime/id_authority.hpp"
#include "runtime/turn_event_adapter.hpp"
#include "tools/registry.hpp"
#include "tools/tool.hpp"
#include "tools/todo_tool.hpp"

using namespace lubancode;

namespace {

constexpr auto kGateWait = std::chrono::seconds(3);

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

using ScriptedCall = std::tuple<std::string, std::string, std::string>;

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

// 进门闸:两枚读都进执行体才放行(串行装配必吃等闸超时,测试因此红)。
struct ConcurrencyGate {
    std::mutex mutex;
    std::condition_variable cv;
    std::size_t entered = 0;
    std::size_t enter_target = 1;
    int gate_timeouts = 0;
};

// 读靶:名字 read_file(放行名单认它),带进门闸与在跑峰值。
class GatedRead : public tools::Tool {
public:
    explicit GatedRead(ConcurrencyGate& gate) : gate_(&gate) {}
    std::string name() const override { return "read_file"; }
    std::string description() const override { return "宿主缝读靶"; }
    nlohmann::json input_schema() const override { return nlohmann::json::object(); }
    tools::EffectClass effect_class() const override { return tools::EffectClass::ReadOnlyLocal; }
    tools::Tool::Result execute(const nlohmann::json&) override { return {"不走旧口", true}; }
    tools::Tool::Result execute(const nlohmann::json&, const tools::ToolExecutionContext&) override {
        ++executions;
        const int now_active = active_.fetch_add(1) + 1;
        int observed = peak_.load(std::memory_order_relaxed);
        while (now_active > observed &&
               !peak_.compare_exchange_weak(observed, now_active, std::memory_order_relaxed)) {
        }
        {
            std::unique_lock<std::mutex> lock(gate_->mutex);
            ++gate_->entered;
            gate_->cv.notify_all();
            if (gate_->enter_target > 1 &&
                !gate_->cv.wait_for(lock, kGateWait, [&] { return gate_->entered >= gate_->enter_target; })) {
                ++gate_->gate_timeouts;
            }
        }
        active_.fetch_sub(1);
        return {"read ok", false};
    }
    std::atomic<int> executions{0};
    std::atomic<int> active_{0};
    std::atomic<int> peak_{0};

private:
    ConcurrencyGate* gate_;
};

// 写靶:needs_confirm(确认门是本案的主角),执行计数。
class ConfirmWrite : public tools::Tool {
public:
    std::string name() const override { return "write_file"; }
    std::string description() const override { return "宿主缝确认写靶"; }
    nlohmann::json input_schema() const override { return nlohmann::json::object(); }
    bool needs_confirm() const override { return true; }
    tools::EffectClass effect_class() const override { return tools::EffectClass::LocalReversible; }
    tools::Tool::Result execute(const nlohmann::json&) override {
        ++executions;
        return {"written", false};
    }
    std::atomic<int> executions{0};
};

// 宿主缝整线:真 RunTurn + 假后端(交互/单发共用这一只执行面,档位差异
// 在 TurnContext 的字段上表达)。
struct TurnRig {
    FakeBackend backend;
    tools::ToolRegistry registry;
    std::unique_ptr<agent::Agent> loop;
    cli::ContextTracker tracker{100000};
    std::vector<cli::TranscriptItem> transcript;
    std::shared_ptr<tools::TodoListState> todo = std::make_shared<tools::TodoListState>();
    cli::Theme theme{};
    std::atomic<bool> expanded{false};
    std::set<std::string> session_allowed;

    runtime::IdAuthority ids;
    runtime::TurnEventAdapter adapter;
    std::vector<runtime::ServerEvent> events;

    explicit TurnRig(agent::ToolBatchStrategy strategy, int concurrency)
        : adapter("p3-host-test", ids) {
        agent::AgentProfile profile;
        profile.request.model = "test-model";
        profile.system_prompt = "system prompt";
        profile.runtime.tool_batch_strategy = strategy;
        profile.runtime.parallel_read_concurrency = concurrency;
        loop = std::make_unique<agent::Agent>(backend, registry, std::move(profile));
        adapter.Attach([this](const runtime::ServerEvent& event) { events.push_back(event); });
    }

    app::TurnContext MakeContext(const std::string& input) {
        app::TurnContext ctx;
        ctx.loop = loop.get();
        ctx.user_input = input;
        ctx.always_allowed_tools = &session_allowed;
        ctx.theme = theme;
        ctx.context_tracker = &tracker;
        ctx.registry = &registry;
        ctx.is_console = false;
        ctx.transcript = &transcript;
        ctx.todo_state = todo;
        ctx.transcript_expanded = &expanded;
        ctx.silent = true;
        ctx.turn_events = &adapter;
        return ctx;
    }

    std::size_t ToolItemCount(runtime::ServerEventKind kind) const {
        return static_cast<std::size_t>(std::count_if(
            events.begin(), events.end(), [kind](const runtime::ServerEvent& e) {
                // item 层事件按 item_kind 分账;batch 层事件不带 item 语义,
                // 只按 kind 数(item_kind 缺省 Tool,不能拿来筛)。
                return e.kind == kind &&
                       (kind == runtime::ServerEventKind::ToolBatchStarted ||
                        kind == runtime::ServerEventKind::ToolBatchFinished ||
                        e.item_kind == runtime::ItemKind::Tool);
            }));
    }
};

}  // namespace

TEST_CASE("宿主(CLI 交互):并行读 + 会话放行的确认写,屏障与审计不缺环") {
    ConcurrencyGate gate;
    gate.enter_target = 2;  // 两枚读同进执行体:装配若退回串行,等闸超时红
    TurnRig rig(agent::ToolBatchStrategy::ParallelRead, /*concurrency=*/2);
    auto read = std::make_unique<GatedRead>(gate);
    GatedRead* read_ptr = read.get();
    auto write = std::make_unique<ConfirmWrite>();
    ConfirmWrite* write_ptr = write.get();
    rig.registry.Register(std::move(read));
    rig.registry.Register(std::move(write));
    rig.session_allowed.insert("write_file");  // 会话级放行账(a 档)

    rig.backend.scripts = {
        BatchScript({{"u0", "read_file", "{}"},
                     {"u1", "read_file", "{}"},
                     {"u2", "write_file", R"({"path":"a.txt"})"}}),
        TextScript("办完了"),
    };
    app::TurnContext ctx = rig.MakeContext("读写各办");
    const app::RunTurnResult result = app::RunTurn(ctx);

    CHECK(result.status == 0);
    // 读并行:进门闸没人超时、峰值恰 2。
    CHECK(gate.gate_timeouts == 0);
    REQUIRE(read_ptr->executions == 2);
    CHECK(read_ptr->peak_.load() == 2);
    // 确认门不缺环:会话放行账预授权的写真执行了(独占节点)。
    REQUIRE(write_ptr->executions == 1);
    // 配对:三枚结果按声明序;写的 approval 走 on_permission_evaluate 的
    // Allow(预授权),不弹问。
    REQUIRE(rig.backend.captured_requests.size() == 2);
    const auto blocks = ResultBlocksOf(rig.backend.captured_requests[1]);
    REQUIRE(blocks.size() == 3);
    CHECK(blocks[2]->tool_use_id == "u2");
    CHECK_FALSE(blocks[2]->is_error);
    // 审计(唯一出水口的事件流):三枚工具 ItemStarted/Completed 各恰一份,
    // 批次边界事件各一。
    CHECK(rig.ToolItemCount(runtime::ServerEventKind::ItemStarted) == 3);
    CHECK(rig.ToolItemCount(runtime::ServerEventKind::ItemCompleted) == 3);
    CHECK(rig.ToolItemCount(runtime::ServerEventKind::ToolBatchStarted) == 1);
    CHECK(rig.ToolItemCount(runtime::ServerEventKind::ToolBatchFinished) == 1);
}

TEST_CASE("宿主(CLI 交互):作用域闸拒写——拒绝文案随 tool_result 回模型,读照常并行") {
    ConcurrencyGate gate;
    gate.enter_target = 2;
    TurnRig rig(agent::ToolBatchStrategy::ParallelRead, /*concurrency=*/2);
    auto read = std::make_unique<GatedRead>(gate);
    GatedRead* read_ptr = read.get();
    auto write = std::make_unique<ConfirmWrite>();
    ConfirmWrite* write_ptr = write.get();
    rig.registry.Register(std::move(read));
    rig.registry.Register(std::move(write));
    // 注意:write_file 不进会话放行账——本次它被作用域闸拦在确认门之前,
    // 不会走到"问用户"(管道档读 stdin 的事不许发生在单测里)。

    rig.backend.scripts = {
        BatchScript({{"u0", "read_file", "{}"}, {"u1", "read_file", "{}"}, {"u2", "write_file", "{}"}}),
        TextScript("收到拒绝"),
    };
    app::TurnContext ctx = rig.MakeContext("试试写");
    ctx.scope_gate = [](const std::string& name, const nlohmann::json&) -> std::optional<std::string> {
        if (name == "write_file") {
            return "AGENTS.md 作用域未批:本次工作目录不在写许可清单内,未执行。";
        }
        return std::nullopt;
    };
    const app::RunTurnResult result = app::RunTurn(ctx);

    CHECK(result.status == 0);
    CHECK(gate.gate_timeouts == 0);  // 读并行未被写挡前的段影响
    REQUIRE(read_ptr->executions == 2);
    CHECK(write_ptr->executions == 0);  // 闸拦 = 未执行
    REQUIRE(rig.backend.captured_requests.size() == 2);
    const auto blocks = ResultBlocksOf(rig.backend.captured_requests[1]);
    REQUIRE(blocks.size() == 3);
    REQUIRE(blocks[2]->is_error);
    CHECK(blocks[2]->content.find("作用域未批") != std::string::npos);  // 宿主拒绝文案如实回模型
    CHECK(blocks[2]->content.find("未执行") != std::string::npos);
}

TEST_CASE("宿主(one-shot/--yes):auto_confirm 全放,批次照常并行收口") {
    ConcurrencyGate gate;
    gate.enter_target = 2;
    TurnRig rig(agent::ToolBatchStrategy::ParallelRead, /*concurrency=*/2);
    auto read = std::make_unique<GatedRead>(gate);
    GatedRead* read_ptr = read.get();
    auto write = std::make_unique<ConfirmWrite>();
    ConfirmWrite* write_ptr = write.get();
    rig.registry.Register(std::move(read));
    rig.registry.Register(std::move(write));
    // --yes 档:不进会话放行账也放(auto_confirm 是独立的显式全放)。

    rig.backend.scripts = {
        BatchScript({{"u0", "read_file", "{}"},
                     {"u1", "read_file", "{}"},
                     {"u2", "write_file", R"({"path":"b.txt"})"}}),
        TextScript("单发完成"),
    };
    app::TurnContext ctx = rig.MakeContext("--yes 跑");
    ctx.auto_confirm = true;
    const app::RunTurnResult result = app::RunTurn(ctx);

    CHECK(result.status == 0);
    CHECK(gate.gate_timeouts == 0);
    REQUIRE(read_ptr->executions == 2);
    CHECK(read_ptr->peak_.load() == 2);
    REQUIRE(write_ptr->executions == 1);
    REQUIRE(rig.backend.captured_requests.size() == 2);
    const auto blocks = ResultBlocksOf(rig.backend.captured_requests[1]);
    REQUIRE(blocks.size() == 3);
    CHECK_FALSE(blocks[2]->is_error);
    CHECK(rig.ToolItemCount(runtime::ServerEventKind::ItemCompleted) == 3);
}
