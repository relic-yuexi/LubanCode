// 子代理 x 停止失效单:取消令牌贯通工具进程的可验证钉子。
// 断的链从前是:面板 x -> task->cancel -> CancelChain 合并旗 -> AgentLoop
// 只在工具之间查旗,RunOneTool 不收 cancel,Tool::execute 没有取消口——
// 正在跑的 run_command 进程树收不到停止信号,只能等超时/墙钟。
// 本册钉四件事:
//   1. RunOneTool 把 cancel 指针原样递进 ToolExecutionContext(指针身份);
//   2. 只实现旧口 execute(input) 的工具走默认适配,行为不变;
//   3. 转发壳(DeferredTool)不洗掉 context;
//   4. 端到端:子代理的工具跑着,面板 x 置 cancel -> 合并旗置位 -> 正在
//      执行的工具从 context 里看见置位、按能力收口 -> 任务收场 Cancelled,
//      台账 stop_requested(面板"停止中"回执的根)前后分明。

#include <doctest/doctest.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <nlohmann/json.hpp>

#include "agent/loop.hpp"
#include "api/backend.hpp"
#include "api/types.hpp"
#include "app/agent_panel_presenter.hpp"
#include "cli/agent_panel.hpp"
#include "cli/theme.hpp"
#include "tools/agent_tool.hpp"
#include "tools/registry.hpp"
#include "tools/task_ledger.hpp"
#include "tools/tool.hpp"
#include "tools/tool_search.hpp"

using namespace lubancode;

namespace {

// 探针工具:记录 RunOneTool 递进来的 context.cancel 指针与当时的取值。
class ProbeTool : public tools::Tool {
public:
    std::string name() const override { return "probe"; }
    std::string description() const override { return "probe"; }
    nlohmann::json input_schema() const override { return nlohmann::json::object(); }

    tools::Tool::Result execute(const nlohmann::json&) override {
        legacy_calls++;
        return {"legacy", false};
    }
    tools::Tool::Result execute(const nlohmann::json&, const tools::ToolExecutionContext& context) override {
        context_calls++;
        seen_cancel_ptr = context.cancel;
        if (context.cancel != nullptr) {
            seen_cancel_value = context.cancel->load(std::memory_order_acquire);
        }
        return {"probed", false};
    }

    int legacy_calls = 0;
    int context_calls = 0;
    const std::atomic<bool>* seen_cancel_ptr = nullptr;
    bool seen_cancel_value = false;
};

// 等取消旗才放行的工具:置位前挂住(有界),置位后收口。端到端用例里它
// 扮演"正在跑的长工具"(run_command 的替身):面板 x 的停止信号必须穿透
// 到它的 context.cancel 才放行。
class CancelWaitTool : public tools::Tool {
public:
    std::string name() const override { return "cancel_wait"; }
    std::string description() const override { return "wait for cancel"; }
    nlohmann::json input_schema() const override { return nlohmann::json::object(); }

    tools::Tool::Result execute(const nlohmann::json&) override {
        return {"旧口没有取消源,本测试不走这条", true};
    }
    tools::Tool::Result execute(const nlohmann::json&, const tools::ToolExecutionContext& context) override {
        {
            std::lock_guard<std::mutex> lock(mutex);
            entered = true;
        }
        ready.notify_all();
        if (context.cancel == nullptr) {
            return {"工具没收到取消旗(链路断)", true};
        }
        // 有界等待:10 秒内旗没置位按失败收口,测试由外层断言逮住。
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        while (!context.cancel->load(std::memory_order_acquire)) {
            if (std::chrono::steady_clock::now() > deadline) {
                return {"等取消旗超时(链路断或迟过 10s)", true};
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        saw_cancel.store(true, std::memory_order_release);
        return {"收到停止信号,工具按能力收口", true};
    }

    std::mutex mutex;
    std::condition_variable ready;
    bool entered = false;
    std::atomic<bool> saw_cancel{false};
};

// 按脚本吐事件的假后端(与 test_agent_tool.cpp 同款,只留本册要的骨架)。
class ScriptBackend : public api::Backend {
public:
    std::vector<std::vector<api::StreamEvent>> scripts;
    std::size_t requests = 0;

    std::expected<void, api::Error> send_stream(
        const api::Request&, const std::function<void(const api::StreamEvent&)>& on_event,
        const std::atomic<bool>* = nullptr) override {
        const std::size_t idx = requests++;
        if (idx >= scripts.size()) {
            return std::unexpected(api::Error{api::ErrorKind::Api, "脚本用完了", 0});
        }
        for (const auto& event : scripts[idx]) {
            on_event(event);
        }
        return {};
    }
};

std::vector<api::StreamEvent> ToolUseScript(const std::string& tool_id, const std::string& tool_name) {
    return {
        api::MessageStart{"msg", "model"},
        api::ToolUseStart{0, tool_id, tool_name},
        api::ToolUseInputDelta{0, "{}"},
        api::ContentBlockDone{0},
        api::MessageDone{"tool_use", api::Usage{}},
    };
}

}  // namespace

// ---------------------------------------------------------------------------
// RunOneTool 的取消贯通(指针身份)
// ---------------------------------------------------------------------------

TEST_CASE("RunOneTool:cancel 指针原样递进 ToolExecutionContext") {
    tools::ToolRegistry registry;
    auto probe = std::make_unique<ProbeTool>();
    ProbeTool* probe_ptr = probe.get();
    registry.Register(std::move(probe));

    std::atomic<bool> cancel_flag{false};
    api::ToolUseBlock call;
    call.id = "toolu_1";
    call.name = "probe";
    agent::TurnWiring wiring;
    const tools::Tool::Result result = agent::RunOneTool(registry, call, wiring,
                                                          /*tool_filter=*/nullptr, std::string(),
                                                          /*trace=*/nullptr, &cancel_flag);
    CHECK_FALSE(result.is_error);
    REQUIRE(probe_ptr->context_calls == 1);
    CHECK(probe_ptr->seen_cancel_ptr == &cancel_flag);
    CHECK_FALSE(probe_ptr->seen_cancel_value);  // 没置位:只递指针,不误伤
    CHECK(probe_ptr->legacy_calls == 0);        // 新口被调,不走旧口
}

TEST_CASE("RunOneTool:不传 cancel 时 context.cancel 是空指针(旧行为)") {
    tools::ToolRegistry registry;
    auto probe = std::make_unique<ProbeTool>();
    ProbeTool* probe_ptr = probe.get();
    registry.Register(std::move(probe));

    api::ToolUseBlock call;
    call.id = "toolu_1";
    call.name = "probe";
    agent::TurnWiring wiring;
    CHECK_FALSE(agent::RunOneTool(registry, call, wiring, nullptr).is_error);
    REQUIRE(probe_ptr->context_calls == 1);
    CHECK(probe_ptr->seen_cancel_ptr == nullptr);
}

TEST_CASE("RunOneTool:cancel 已置位时工具当场看见真值") {
    tools::ToolRegistry registry;
    auto probe = std::make_unique<ProbeTool>();
    ProbeTool* probe_ptr = probe.get();
    registry.Register(std::move(probe));

    std::atomic<bool> cancel_flag{true};
    api::ToolUseBlock call;
    call.id = "toolu_1";
    call.name = "probe";
    agent::TurnWiring wiring;
    CHECK_FALSE(agent::RunOneTool(registry, call, wiring, nullptr, std::string(), nullptr, &cancel_flag).is_error);
    REQUIRE(probe_ptr->context_calls == 1);
    CHECK(probe_ptr->seen_cancel_ptr == &cancel_flag);
    CHECK(probe_ptr->seen_cancel_value);
}

// 只实现旧口的工具:默认适配忽略 context、照旧跑(不肯合作取消的工具
// 行为一字不差,规格"不能一夜之间假装全支持")。
class LegacyTool : public tools::Tool {
public:
    std::string name() const override { return "legacy"; }
    std::string description() const override { return "legacy"; }
    nlohmann::json input_schema() const override { return nlohmann::json::object(); }
    tools::Tool::Result execute(const nlohmann::json&) override {
        ++calls;
        return {"ok", false};
    }
    int calls = 0;
};

TEST_CASE("RunOneTool:只实现旧口的工具走默认适配,行为不变") {
    tools::ToolRegistry registry;
    auto legacy = std::make_unique<LegacyTool>();
    LegacyTool* legacy_ptr = legacy.get();
    registry.Register(std::move(legacy));

    std::atomic<bool> cancel_flag{true};
    api::ToolUseBlock call;
    call.id = "toolu_1";
    call.name = "legacy";
    agent::TurnWiring wiring;
    const tools::Tool::Result result =
        agent::RunOneTool(registry, call, wiring, nullptr, std::string(), nullptr, &cancel_flag);
    CHECK_FALSE(result.is_error);
    CHECK(legacy_ptr->calls == 1);
}

TEST_CASE("DeferredTool 转发壳不洗掉 context(延迟挂载的工具也收得到取消旗)") {
    tools::ToolRegistry registry;
    auto probe = std::make_unique<ProbeTool>();
    ProbeTool* probe_ptr = probe.get();
    registry.Register(std::make_unique<tools::DeferredTool>(std::move(probe)));

    std::atomic<bool> cancel_flag{false};
    api::ToolUseBlock call;
    call.id = "toolu_1";
    call.name = "probe";
    agent::TurnWiring wiring;
    CHECK_FALSE(agent::RunOneTool(registry, call, wiring, nullptr, std::string(), nullptr, &cancel_flag).is_error);
    REQUIRE(probe_ptr->context_calls == 1);
    CHECK(probe_ptr->seen_cancel_ptr == &cancel_flag);
}

// ---------------------------------------------------------------------------
// 端到端:面板 x -> CancelChain -> 正在执行的工具 -> 任务收场 Cancelled
// ---------------------------------------------------------------------------

TEST_CASE("子代理 x:停止信号穿透到正在执行的工具,任务收场 Cancelled(回执齐全)") {
    ScriptBackend backend;
    backend.scripts = {ToolUseScript("toolu_1", "cancel_wait")};
    tools::ToolRegistry sub_registry;
    auto wait_tool = std::make_unique<CancelWaitTool>();
    CancelWaitTool* wait_ptr = wait_tool.get();
    sub_registry.Register(std::move(wait_tool));
    tools::AgentTool agent_tool(backend, sub_registry, "/work/dir");

    std::thread runner([&] {
        const tools::Tool::Result r =
            agent_tool.execute(nlohmann::json{{"title", "跑长命令的"}, {"prompt", "查"}});
        // 用户中止不是崩溃:结果带"用户中止"说明(结构化 stopped)。
        CHECK(r.content.find("用户中止") != std::string::npos);
    });
    // 等工具真跑起来(子代理首轮模型流 -> 工具执行)。
    {
        std::unique_lock<std::mutex> lock(wait_ptr->mutex);
        REQUIRE(wait_ptr->ready.wait_for(lock, std::chrono::seconds(10), [&] { return wait_ptr->entered; }));
    }
    // x 之前:运行中,没有停止请求。
    const auto before = agent_tool.TaskSummaries();
    REQUIRE(before.size() == 1);
    CHECK(before[0].state == tools::AgentTaskState::Running);
    CHECK_FALSE(before[0].stop_requested);

    // 面板 x:发停止信号。返回 true = 已受理(按下即回执的那枚 bool)。
    CHECK(agent_tool.CancelTask(before[0].id));
    // 发出之后、任务线程收口之前:stop_requested 置位,面板行显"停止中"。
    const auto stopping = agent_tool.TaskSummaries();
    REQUIRE(stopping.size() == 1);
    CHECK(stopping[0].state == tools::AgentTaskState::Running);
    CHECK(stopping[0].stop_requested);

    runner.join();

    // 工具真收到了取消旗(不是等超时/脚本用完才退)。
    CHECK(wait_ptr->saw_cancel.load(std::memory_order_acquire));
    // 台账收场:Cancelled + 用户中止,不是 Failed 也不是静默 Running。
    const auto after = agent_tool.TaskSummaries();
    REQUIRE(after.size() == 1);
    CHECK(after[0].state == tools::AgentTaskState::Cancelled);
    CHECK(after[0].outcome_reason == tools::TaskOutcomeReason::UserStop);
    // 只发过一次模型请求:工具收口后循环按打断收场,不再起第二轮。
    CHECK(backend.requests == 1);
}

TEST_CASE("子代理 x 的面板回执:x 之后坞行的状态词进'停止中',不是死 Running") {
    ScriptBackend backend;
    backend.scripts = {ToolUseScript("toolu_1", "cancel_wait")};
    tools::ToolRegistry sub_registry;
    auto wait_tool = std::make_unique<CancelWaitTool>();
    CancelWaitTool* wait_ptr = wait_tool.get();
    sub_registry.Register(std::move(wait_tool));
    tools::AgentTool agent_tool(backend, sub_registry, "/work/dir");

    lubancode::cli::Theme theme;
    lubancode::app::AgentPanelPresenter presenter(theme);

    std::thread runner([&] {
        agent_tool.execute(nlohmann::json{{"title", "面板回执"}, {"prompt", "查"}});
    });
    {
        std::unique_lock<std::mutex> lock(wait_ptr->mutex);
        REQUIRE(wait_ptr->ready.wait_for(lock, std::chrono::seconds(10), [&] { return wait_ptr->entered; }));
    }
    // x 之前:运行中行(不许已显"停止中")。
    bool saw_stopping = false;
    for (const auto& entry : presenter.Entries(&agent_tool)) {
        if (entry.state.find("停止中") != std::string::npos) {
            saw_stopping = true;
        }
    }
    CHECK_FALSE(saw_stopping);

    CHECK(agent_tool.CancelTask(agent_tool.TaskSummaries().front().id));
    // x 之后:面板行按 stop_requested 显"停止中"(100ms 内可见的那拍,
    // 这里直接断 presenter 的输出)。
    saw_stopping = false;
    for (const auto& entry : presenter.Entries(&agent_tool)) {
        if (entry.running && entry.state.find("停止中") != std::string::npos) {
            saw_stopping = true;
        }
    }
    CHECK(saw_stopping);

    runner.join();
    // 收口之后:终态行带"停下 · 用户中止",停止中退场。
    bool saw_stopped_receipt = false;
    for (const auto& entry : presenter.Entries(&agent_tool)) {
        if (entry.cancelled && entry.state.find("用户中止") != std::string::npos) {
            saw_stopped_receipt = true;
        }
        CHECK(entry.state.find("停止中") == std::string::npos);
    }
    CHECK(saw_stopped_receipt);
}

// ---------------------------------------------------------------------------
// 台账直接钉:stop_requested 的生命周期与 CancelTask 的受理回执
// ---------------------------------------------------------------------------

TEST_CASE("台账:CancelTask 只对运行中任务受理,终态/陌生号明拒") {
    tools::TaskLedger ledger;
    tools::AgentTaskSnapshot snapshot;
    snapshot.title = "直钉台账";
    snapshot.state = tools::AgentTaskState::Running;
    snapshot.start_time = std::chrono::steady_clock::now();
    const auto task = ledger.Register(std::move(snapshot));

    CHECK_FALSE(ledger.Summaries().front().stop_requested);
    CHECK(ledger.CancelTask(task->snapshot.id));
    CHECK(ledger.Summaries().front().stop_requested);   // x 后、收口前:停止中
    CHECK(ledger.CancelTask(task->snapshot.id));        // 幂等:连按也受理(不重复发景)
    CHECK(ledger.CancelTask(9999) == false);            // 陌生号:明拒

    ledger.FinalizeFromToolResult(task, "[stopped] 用户中止了这只子代理", true);
    const auto settled = ledger.Summaries();
    REQUIRE(settled.size() == 1);
    CHECK(settled[0].state == tools::AgentTaskState::Cancelled);
    CHECK(settled[0].outcome_reason == tools::TaskOutcomeReason::UserStop);
    CHECK(ledger.CancelTask(settled[0].id) == false);   // 终态:不再受理
    CHECK_FALSE(ledger.HasRunningTasks());
}
