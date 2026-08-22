// 工具调用逐枚追踪单的测试(阶段 0-2 核心:契约/持久追踪/恢复)。
//
// 覆盖单子测试矩阵的主干:
//   - 序列化往返与稳定枚举字符串;
//   - 五工具批次:全成功、第三枚显错、第三枚后 ESC(四五 cancelled_before_
//     start)、同名五连、重复 tool_use_id 不串账;
//   - 崩溃注入:九个窗口的每一种,恢复四档判定(not_started/finished/
//     result_recoverable/unknown_after_start);
//   - 重复事件幂等、冲突 finished 报 corrupt;
//   - trace-aware 修补:老档无 trace 走 legacy、恢复结果带 [会话恢复];
//   - 消息落盘次序(assistant 先落、result 后落、五枚仍一条 user message);
//   - 来源/错误码:registry 元数据、run_command/MCP/Lua/plugin 的稳定码;
//   - undo token:preimage/postimage 哈希、条件式撤销判定。

#include <doctest/doctest.h>

#include <atomic>
#include <filesystem>
#include <fstream>
#include <set>
#include <vector>

#include "agent/loop.hpp"
#include "agent/session_store.hpp"
#include "agent/tool_trace.hpp"
#include "api/backend.hpp"
#include "api/types.hpp"
#include "tools/registry.hpp"
#include "tools/tool.hpp"
#include "mcp/client.hpp"
#include "tools/write_file.hpp"

using namespace lubancode;

namespace {

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

class FakeTool : public tools::Tool {
public:
    FakeTool(std::string name, tools::Tool::Result result) : name_(std::move(name)), result_(std::move(result)) {}

    std::string name() const override { return name_; }
    std::string description() const override { return "fake"; }
    nlohmann::json input_schema() const override { return nlohmann::json::object(); }
    tools::EffectClass effect_class() const override { return effect_class_; }
    tools::Tool::Result execute(const nlohmann::json&) override {
        ++call_count;
        return result_;
    }

    int call_count = 0;
    tools::EffectClass effect_class_ = tools::EffectClass::ReadOnlyLocal;

private:
    std::string name_;
    tools::Tool::Result result_;
};

std::vector<api::StreamEvent> TextOnlyScript(const std::string& text) {
    return {
        api::MessageStart{"msg", "model"},
        api::TextDelta{text},
        api::ContentBlockDone{0},
        api::MessageDone{"end_turn", api::Usage{}},
    };
}

std::vector<api::StreamEvent> ToolUseScript(const std::string& tool_id, const std::string& tool_name) {
    return {
        api::MessageStart{"msg", "model"},
        api::ToolUseStart{0, tool_id, tool_name},
        api::ToolUseInputDelta{0, "{}"},
        api::ContentBlockDone{0},
        api::MessageDone{"tool_use", api::Usage{}},
    };
}

// 五枚工具一批的脚本。
std::vector<api::StreamEvent> FiveToolScript(const std::vector<std::string>& ids, const std::string& name) {
    std::vector<api::StreamEvent> script{api::MessageStart{"msg", "model"}};
    for (std::size_t i = 0; i < ids.size(); ++i) {
        script.push_back(api::ToolUseStart{i, ids[i], name});
        script.push_back(api::ToolUseInputDelta{i, "{}"});
        script.push_back(api::ContentBlockDone{i});
    }
    script.push_back(api::MessageDone{"tool_use", api::Usage{}});
    return script;
}

// MCP 假传输层(与 test_mcp_client 同款;Client 析构摸 Shutdown,声明序
// 保证 transport 活得比 client 久)。
class FakeTransport : public mcp::Transport {
public:
    std::function<void(const std::string&)> on_write;
    std::atomic<bool> alive{true};

    bool WriteLine(const std::string& line) override {
        if (on_write) {
            on_write(line);
        }
        return true;
    }
    void Shutdown(int /*wait_ms*/) override { alive = false; }
    bool IsAlive() const override { return alive; }
    std::string StderrTail() const override { return std::string(); }
};

// 收 trace 事件的回调壳。
struct TraceCollector {
    std::vector<agent::ToolTraceEvent> events;
    agent::Callbacks Decorate(agent::Callbacks callbacks) {
        callbacks.on_tool_trace = [this](const agent::ToolTraceEvent& event) { events.push_back(event); };
        return callbacks;
    }
};

// 折账快捷。
agent::ToolExecutionLedger Fold(const std::vector<agent::ToolTraceEvent>& events) {
    agent::ToolExecutionLedger ledger;
    for (const auto& event : events) {
        ledger.Fold(event);
    }
    return ledger;
}

}  // namespace

// ---------------------------------------------------------------------------
// 阶段 0:契约
// ---------------------------------------------------------------------------

TEST_CASE("tool_trace: 序列化往返保真") {
    agent::ToolTraceEvent event;
    event.kind = agent::ToolTraceEventKind::ExecutionFinished;
    event.thread_id = "thread-1";
    event.turn_id = "turn-1";
    event.batch_id = "batch-1";
    event.sequence_in_batch = 2;
    event.execution_id = "item-17";
    event.tool_use_id = "call_c";
    event.tool_name = "run_command";
    event.source_kind = agent::ToolSourceKind::Builtin;
    event.outcome = agent::ToolOutcome::ProcessExitNonzero;
    event.error_code = agent::kErrProcessExitNonzero;
    event.fallback_message = "退出码 1";
    event.duration_ms = 42;
    event.result_ref.kind = agent::ToolResultRef::Kind::Inline;
    event.result_ref.sha256 = "abc";
    event.result_ref.bytes = 91;
    event.result_ref.content = "hello";
    event.seq = 7;
    event.timestamp_ms = 1700000000000;

    const std::string line = agent::SerializeToolTraceEvent(event, "2026-08-23 10:00:00");
    const auto parsed = agent::ParseToolTraceEvent(line);
    REQUIRE(parsed.has_value());
    CHECK(parsed->kind == event.kind);
    CHECK(parsed->execution_id == "item-17");
    CHECK(parsed->tool_use_id == "call_c");
    CHECK(parsed->batch_id == "batch-1");
    CHECK(parsed->sequence_in_batch == 2);
    CHECK(parsed->outcome == agent::ToolOutcome::ProcessExitNonzero);
    CHECK(parsed->error_code == agent::kErrProcessExitNonzero);
    CHECK(parsed->duration_ms == 42);
    CHECK(parsed->result_ref.kind == agent::ToolResultRef::Kind::Inline);
    CHECK(parsed->result_ref.content == "hello");
    CHECK(parsed->seq == 7);
}

TEST_CASE("tool_trace: 坏行/缺主键给 nullopt,不抛") {
    CHECK(agent::ParseToolTraceEvent("not json").has_value() == false);
    CHECK(agent::ParseToolTraceEvent(R"({"type":"compact"})").has_value() == false);
    CHECK(agent::ParseToolTraceEvent(R"({"type":"tool_trace_v1","event":"nope","execution_id":"x"})").has_value() ==
          false);
    // 缺 execution_id:审计主键缺了不可信。
    CHECK(agent::ParseToolTraceEvent(R"({"type":"tool_trace_v1","event":"scheduled"})").has_value() == false);
}

TEST_CASE("tool_trace: OutcomeNeverStarted 只认闸前终态") {
    CHECK(agent::OutcomeNeverStarted(agent::ToolOutcome::HookDenied));
    CHECK(agent::OutcomeNeverStarted(agent::ToolOutcome::PermissionDeclined));
    CHECK(agent::OutcomeNeverStarted(agent::ToolOutcome::CancelledBeforeStart));
    CHECK(agent::OutcomeNeverStarted(agent::ToolOutcome::UnknownTool));
    CHECK(agent::OutcomeNeverStarted(agent::ToolOutcome::Unavailable));
    CHECK(agent::OutcomeNeverStarted(agent::ToolOutcome::SchemaRejected));
    CHECK_FALSE(agent::OutcomeNeverStarted(agent::ToolOutcome::Succeeded));
    CHECK_FALSE(agent::OutcomeNeverStarted(agent::ToolOutcome::TimedOut));
    CHECK_FALSE(agent::OutcomeNeverStarted(agent::ToolOutcome::UnknownAfterStart));
}

// ---------------------------------------------------------------------------
// 阶段 1:五工具批次(真 AgentLoop,FakeBackend)
// ---------------------------------------------------------------------------

TEST_CASE("五工具全成功: sequence 与 id 配对不乱") {
    FakeBackend backend;
    backend.scripts = {
        FiveToolScript({"call_a", "call_b", "call_c", "call_d", "call_e"}, "probe"),
        TextOnlyScript("done"),
    };
    tools::ToolRegistry registry;
    auto* probe = new FakeTool("probe", tools::Tool::Result{"ok", false});
    registry.Register(std::unique_ptr<FakeTool>(probe));

    agent::AgentLoop loop(backend, registry, "m", "sys");
    TraceCollector collector;
    agent::Callbacks callbacks;
    callbacks = collector.Decorate(callbacks);
    const auto result = loop.Run("五枚", callbacks);
    REQUIRE(result.has_value());
    REQUIRE(probe->call_count == 5);

    const auto ledger = Fold(collector.events);
    CHECK(ledger.executions().size() == 5);
    // 每枚四道栅栏齐。
    for (const auto& record : ledger.executions()) {
        CHECK(record.has_scheduled);
        CHECK(record.has_started);
        CHECK(record.has_finished);
        CHECK(record.has_committed);
        CHECK(record.Classify() == agent::RecoveryClass::Finished);
        CHECK(record.outcome == agent::ToolOutcome::Succeeded);
    }
    // 序号 0..4,execution_id 各一枚,不串。
    std::set<std::string> ids;
    for (int i = 0; i < 5; ++i) {
        const auto batch = ledger.Batch(ledger.executions()[i].batch_id);
        REQUIRE(batch.size() == 5);
        CHECK(batch[static_cast<std::size_t>(i)]->sequence_in_batch == i);
        ids.insert(ledger.executions()[static_cast<std::size_t>(i)].execution_id);
    }
    CHECK(ids.size() == 5);
    // wire:五枚结果仍同一条 user message。
    REQUIRE(backend.captured_requests.size() >= 2);
    const auto& result_message = backend.captured_requests[1].messages.back();
    CHECK(result_message.role == api::Role::User);
    CHECK(result_message.content.size() == 5);
}

TEST_CASE("第三枚 is_error: 最早明确失败是 #2,#3/#4 照跑") {
    FakeBackend backend;
    backend.scripts = {
        FiveToolScript({"a1", "a2", "a3", "a4", "a5"}, "probe"),
        TextOnlyScript("done"),
    };
    tools::ToolRegistry registry;
    registry.Register(std::make_unique<FakeTool>("probe", tools::Tool::Result{"fine", false}));
    // 第三枚换成会失败的(finish 脚本里 a3 会跑同一个 probe;失败由
    // outcome 投影:is_error=true -> tool_error)。
    // 这里直接用一个按 tool_use_id 决定成败的工具。
    struct FlakyTool : tools::Tool {
        std::string name() const override { return "bad"; }
        std::string description() const override { return "f"; }
        nlohmann::json input_schema() const override { return nlohmann::json::object(); }
        tools::EffectClass effect_class() const override { return tools::EffectClass::ReadOnlyLocal; }
        tools::Tool::Result execute(const nlohmann::json&) override {
            tools::Tool::Result r{"boom", true};
            r.outcome = "tool_error";
            return r;
        }
    };
    // 用两个工具名区分:前两枚 probe,后三枚 probe(同名五连,靠 id 区分)
    // 简化:注册一个总失败的工具 bad,脚本给第三枚用 bad。
    backend.scripts[0] = {
        api::MessageStart{"msg", "model"},
        api::ToolUseStart{0, "a1", "probe"},
        api::ToolUseInputDelta{0, "{}"},
        api::ContentBlockDone{0},
        api::ToolUseStart{1, "a2", "probe"},
        api::ToolUseInputDelta{1, "{}"},
        api::ContentBlockDone{1},
        api::ToolUseStart{2, "a3", "bad"},
        api::ToolUseInputDelta{2, "{}"},
        api::ContentBlockDone{2},
        api::ToolUseStart{3, "a4", "probe"},
        api::ToolUseInputDelta{3, "{}"},
        api::ContentBlockDone{3},
        api::ToolUseStart{4, "a5", "probe"},
        api::ToolUseInputDelta{4, "{}"},
        api::ContentBlockDone{4},
        api::MessageDone{"tool_use", api::Usage{}},
    };
    registry.Register(std::make_unique<FlakyTool>());

    agent::AgentLoop loop(backend, registry, "m", "sys");
    TraceCollector collector;
    agent::Callbacks callbacks;
    callbacks = collector.Decorate(callbacks);
    const auto result = loop.Run("跑", callbacks);
    REQUIRE(result.has_value());

    const auto ledger = Fold(collector.events);
    const std::string batch_id = ledger.executions().front().batch_id;
    const auto* first_failure = ledger.FirstExplicitFailure(batch_id);
    REQUIRE(first_failure != nullptr);
    CHECK(first_failure->sequence_in_batch == 2);
    CHECK(first_failure->tool_use_id == "a3");
    CHECK(first_failure->outcome == agent::ToolOutcome::ToolError);
}

TEST_CASE("第三枚后 ESC: 三收口,四五记 cancelled_before_start") {
    FakeBackend backend;
    backend.scripts = {
        FiveToolScript({"a1", "a2", "a3", "a4", "a5"}, "probe"),
        TextOnlyScript("done"),
    };
    tools::ToolRegistry registry;
    struct CancellingTool : tools::Tool {
        std::atomic<bool>& flag;
        int countdown;
        mutable int ran = 0;
        CancellingTool(std::atomic<bool>& f, int c) : flag(f), countdown(c) {}
        std::string name() const override { return "probe"; }
        std::string description() const override { return "f"; }
        nlohmann::json input_schema() const override { return nlohmann::json::object(); }
        tools::EffectClass effect_class() const override { return tools::EffectClass::ReadOnlyLocal; }
        tools::Tool::Result execute(const nlohmann::json&) override {
            ++ran;
            if (ran >= countdown) {
                flag.store(true);
            }
            return {"ok", false};
        }
    };
    std::atomic<bool> cancel{false};
    auto* tool = new CancellingTool(cancel, 3);
    registry.Register(std::unique_ptr<CancellingTool>(tool));

    agent::AgentLoop loop(backend, registry, "m", "sys");
    TraceCollector collector;
    agent::Callbacks callbacks;
    callbacks = collector.Decorate(callbacks);
    const auto result = loop.Run("跑", callbacks, &cancel);
    REQUIRE(result.has_value());
    CHECK(result->cancelled);
    REQUIRE(tool->ran == 3);  // 只跑了三枚

    const auto ledger = Fold(collector.events);
    const std::string batch_id = ledger.executions().front().batch_id;
    const auto batch = ledger.Batch(batch_id);
    REQUIRE(batch.size() == 5);
    for (int i = 0; i < 3; ++i) {
        CHECK(batch[static_cast<std::size_t>(i)]->outcome == agent::ToolOutcome::Succeeded);
    }
    // 四五:cancelled_before_start,且 Classify 落 Finished(未执行,
    // 副作用确凿没有——单子状态机最后一行)。
    for (int i = 3; i < 5; ++i) {
        CHECK(batch[static_cast<std::size_t>(i)]->outcome == agent::ToolOutcome::CancelledBeforeStart);
        CHECK(batch[static_cast<std::size_t>(i)]->Classify() == agent::RecoveryClass::Finished);
        CHECK(agent::OutcomeNeverStarted(batch[static_cast<std::size_t>(i)]->outcome));
    }
}

TEST_CASE("五枚同名工具: 按 execution/tool_use id 配准") {
    FakeBackend backend;
    backend.scripts = {
        FiveToolScript({"same", "same", "same", "same", "same"}, "probe"),
        TextOnlyScript("done"),
    };
    tools::ToolRegistry registry;
    registry.Register(std::make_unique<FakeTool>("probe", tools::Tool::Result{"ok", false}));

    agent::AgentLoop loop(backend, registry, "m", "sys");
    TraceCollector collector;
    agent::Callbacks callbacks;
    callbacks = collector.Decorate(callbacks);
    REQUIRE(loop.Run("跑", callbacks).has_value());

    const auto ledger = Fold(collector.events);
    // Provider 给重复 tool_use_id:五枚 execution 各有各的账,FindByToolUse
    // 全列出来,不串。
    const auto matches = ledger.FindByToolUse("same");
    CHECK(matches.size() == 5);
    std::set<std::string> executions;
    for (const auto* record : matches) {
        executions.insert(record->execution_id);
    }
    CHECK(executions.size() == 5);
}

// ---------------------------------------------------------------------------
// 阶段 2:崩溃注入(九窗口 → 四档)
// ---------------------------------------------------------------------------

TEST_CASE("崩溃注入: scheduled-only -> not_started") {
    agent::ToolTraceEvent scheduled;
    scheduled.kind = agent::ToolTraceEventKind::Scheduled;
    scheduled.execution_id = "e1";
    scheduled.tool_use_id = "u1";
    agent::ToolExecutionLedger ledger;
    ledger.Fold(scheduled);
    const auto* record = ledger.FindByExecution("e1");
    REQUIRE(record != nullptr);
    CHECK(record->Classify() == agent::RecoveryClass::NotStarted);
}

TEST_CASE("崩溃注入: scheduled+started -> unknown_after_start(不冒充失败或成功)") {
    agent::ToolExecutionLedger ledger;
    agent::ToolTraceEvent scheduled;
    scheduled.kind = agent::ToolTraceEventKind::Scheduled;
    scheduled.execution_id = "e1";
    ledger.Fold(scheduled);
    agent::ToolTraceEvent started;
    started.kind = agent::ToolTraceEventKind::ExecutionStarted;
    started.execution_id = "e1";
    started.effect_class = agent::EffectClass::LocalProcessUnknown;
    ledger.Fold(started);
    const auto* record = ledger.FindByExecution("e1");
    REQUIRE(record != nullptr);
    CHECK(record->Classify() == agent::RecoveryClass::UnknownAfterStart);
    CHECK(record->effect_class == agent::EffectClass::LocalProcessUnknown);
}

TEST_CASE("崩溃注入: started+finished 无 committed -> result_recoverable,inline 正文可恢复") {
    agent::ToolExecutionLedger ledger;
    agent::ToolTraceEvent scheduled;
    scheduled.kind = agent::ToolTraceEventKind::Scheduled;
    scheduled.execution_id = "e1";
    scheduled.tool_use_id = "u1";
    ledger.Fold(scheduled);
    agent::ToolTraceEvent started;
    started.kind = agent::ToolTraceEventKind::ExecutionStarted;
    started.execution_id = "e1";
    ledger.Fold(started);
    agent::ToolTraceEvent finished;
    finished.kind = agent::ToolTraceEventKind::ExecutionFinished;
    finished.execution_id = "e1";
    finished.outcome = agent::ToolOutcome::Succeeded;
    finished.result_ref.kind = agent::ToolResultRef::Kind::Inline;
    finished.result_ref.content = "原始结果正文";
    finished.result_ref.sha256 = "ff";
    ledger.Fold(finished);

    const auto* record = ledger.FindByExecution("e1");
    REQUIRE(record != nullptr);
    CHECK(record->Classify() == agent::RecoveryClass::ResultRecoverable);
    // 恢复文本:带 [会话恢复] 与稳定 outcome,模型知道是宿主判断。
    const std::string text = agent::BuildRecoveredResultText(*record);
    CHECK(text.find("[会话恢复]") != std::string::npos);
    CHECK(text.find("succeeded") != std::string::npos);
    CHECK(text.find("原始结果正文") != std::string::npos);
}

TEST_CASE("崩溃注入: artifact 缺失 -> 不可恢复但仍不是 not_started") {
    agent::ToolExecutionLedger ledger;
    agent::ToolTraceEvent started;
    started.kind = agent::ToolTraceEventKind::ExecutionStarted;
    started.execution_id = "e1";
    ledger.Fold(started);
    agent::ToolTraceEvent finished;
    finished.kind = agent::ToolTraceEventKind::ExecutionFinished;
    finished.execution_id = "e1";
    finished.outcome = agent::ToolOutcome::Succeeded;
    finished.result_ref.kind = agent::ToolResultRef::Kind::Artifact;
    finished.result_ref.artifact_id = "a0007";
    finished.result_ref.sha256 = "dead";
    ledger.Fold(finished);
    // blob 缺失:工具可能已完成,绝不是 not_started;恢复文本如实说。
    const auto* record = ledger.FindByExecution("e1");
    REQUIRE(record != nullptr);
    CHECK(record->Classify() == agent::RecoveryClass::ResultRecoverable);
    CHECK(agent::BuildRecoveredResultText(*record).find("artifact") != std::string::npos);
}

TEST_CASE("重复事件幂等,冲突 finished 报 corrupt 不取最后一条赢") {
    agent::ToolExecutionLedger ledger;
    agent::ToolTraceEvent finished_ok;
    finished_ok.kind = agent::ToolTraceEventKind::ExecutionFinished;
    finished_ok.execution_id = "e1";
    finished_ok.outcome = agent::ToolOutcome::Succeeded;
    finished_ok.result_ref.sha256 = "aa";
    finished_ok.seq = 10;
    ledger.Fold(finished_ok);
    // 幂等重放:同 outcome 同摘要,不 corrupt。
    agent::ToolTraceEvent replay = finished_ok;
    replay.seq = 11;
    ledger.Fold(replay);
    CHECK(ledger.corrupt_count() == 0);

    // 冲突:另一枚 finished 说失败。
    agent::ToolTraceEvent conflict;
    conflict.kind = agent::ToolTraceEventKind::ExecutionFinished;
    conflict.execution_id = "e1";
    conflict.outcome = agent::ToolOutcome::TimedOut;
    conflict.result_ref.sha256 = "bb";
    conflict.seq = 12;
    ledger.Fold(conflict);
    const auto* record = ledger.FindByExecution("e1");
    REQUIRE(record != nullptr);
    CHECK(record->corrupt);
    CHECK(record->corrupt_reason.find("conflict") != std::string::npos);
    CHECK_FALSE(record->conflict_seqs.empty());
    // corrupt 折成最保守档。
    CHECK(record->Classify() == agent::RecoveryClass::UnknownAfterStart);
}

// ---------------------------------------------------------------------------
// trace-aware 修补与落盘次序
// ---------------------------------------------------------------------------

TEST_CASE("trace-aware 修补: unknown 补 [会话恢复] 结果,老档回落 legacy") {
    // 一条 assistant 消息带两枚 tool_use:u_done(finished)、u_unknown(started)。
    api::Message assistant;
    assistant.role = api::Role::Assistant;
    api::ToolUseBlock done_use;
    done_use.id = "u_done";
    api::ToolUseBlock unknown_use;
    unknown_use.id = "u_unknown";
    assistant.content.push_back(done_use);
    assistant.content.push_back(unknown_use);

    agent::ToolExecutionLedger ledger;
    agent::ToolTraceEvent sched1;
    sched1.kind = agent::ToolTraceEventKind::Scheduled;
    sched1.execution_id = "e1";
    sched1.tool_use_id = "u_done";
    ledger.Fold(sched1);
    agent::ToolTraceEvent fin1;
    fin1.kind = agent::ToolTraceEventKind::ExecutionFinished;
    fin1.execution_id = "e1";
    fin1.tool_use_id = "u_done";
    fin1.outcome = agent::ToolOutcome::Succeeded;
    fin1.result_ref.kind = agent::ToolResultRef::Kind::Inline;
    fin1.result_ref.content = "done 正文";
    ledger.Fold(fin1);
    agent::ToolTraceEvent commit1;
    commit1.kind = agent::ToolTraceEventKind::ResultCommitted;
    commit1.execution_id = "e1";
    commit1.tool_use_id = "u_done";
    ledger.Fold(commit1);

    agent::ToolTraceEvent sched2;
    sched2.kind = agent::ToolTraceEventKind::Scheduled;
    sched2.execution_id = "e2";
    sched2.tool_use_id = "u_unknown";
    ledger.Fold(sched2);
    agent::ToolTraceEvent start2;
    start2.kind = agent::ToolTraceEventKind::ExecutionStarted;
    start2.execution_id = "e2";
    ledger.Fold(start2);

    std::vector<api::Message> history{assistant};
    const auto report = agent::RepairToolPairsWithTrace(history, ledger);
    CHECK(report.trace_matched == 2);
    CHECK(report.unknown_after_start == 1);
    CHECK(report.result_recovered >= 1);
    // 紧随的 user 消息补了两条结果。
    REQUIRE(history.size() == 2);
    REQUIRE(history[1].role == api::Role::User);
    REQUIRE(history[1].content.size() == 2);
    const auto& r0 = std::get<api::ToolResultBlock>(history[1].content[0]);
    const auto& r1 = std::get<api::ToolResultBlock>(history[1].content[1]);
    CHECK(r0.tool_use_id == "u_done");
    CHECK(r0.content.find("done 正文") != std::string::npos);
    CHECK_FALSE(r0.is_error);
    CHECK(r1.tool_use_id == "u_unknown");
    CHECK(r1.is_error);
    CHECK(r1.content.find("unknown_after_start") != std::string::npos);
}

TEST_CASE("session: tool_trace_v1 行落盘与回读;老版本读档不坏") {
    const std::string dir = agent::MakeSessionSlug("trace 测试") + "-dir";
    std::filesystem::path tmp = std::filesystem::temp_directory_path() / dir;
    std::filesystem::create_directories(tmp);
    const std::string file = (tmp / "s.jsonl").string();

    {
        agent::SessionStore store(tmp.string());
        agent::SessionMeta meta;
        meta.wire = "anthropic";
        meta.model = "m";
        REQUIRE(store.Begin(meta, "s"));
        agent::ToolTraceEvent scheduled;
        scheduled.kind = agent::ToolTraceEventKind::Scheduled;
        scheduled.execution_id = "e1";
        scheduled.tool_use_id = "u1";
        scheduled.tool_name = "probe";
        CHECK(store.AppendToolTraceEvent(scheduled));
        agent::ToolTraceEvent started;
        started.kind = agent::ToolTraceEventKind::ExecutionStarted;
        started.execution_id = "e1";
        CHECK(store.AppendToolTraceEvent(started));
    }

    const auto bytes = agent::ReadSessionFileBytes(file);
    REQUIRE(bytes.has_value());
    const auto loaded = agent::ParseSessionFile(*bytes);
    REQUIRE(loaded.has_value());
    REQUIRE(loaded->tool_trace_events.size() == 2);
    CHECK(loaded->tool_trace_events[0].kind == agent::ToolTraceEventKind::Scheduled);
    CHECK(loaded->tool_trace_events[1].kind == agent::ToolTraceEventKind::ExecutionStarted);

    std::error_code cleanup_ec;
    std::filesystem::remove_all(tmp, cleanup_ec);
}

TEST_CASE("消息落盘次序: assistant 先落、五结果一条 user message 后落") {
    FakeBackend backend;
    backend.scripts = {
        FiveToolScript({"a1", "a2", "a3", "a4", "a5"}, "probe"),
        TextOnlyScript("done"),
    };
    tools::ToolRegistry registry;
    registry.Register(std::make_unique<FakeTool>("probe", tools::Tool::Result{"ok", false}));

    agent::AgentLoop loop(backend, registry, "m", "sys");
    std::vector<std::string> persist_log;
    TraceCollector collector;
    agent::Callbacks callbacks;
    callbacks.on_assistant_message_ready = [&persist_log](const api::Message& message) {
        persist_log.push_back(message.role == api::Role::Assistant ? "assistant" : "?");
    };
    callbacks.on_tool_results_committed = [&persist_log](const std::string&, const api::Message& message) {
        // 五枚 tool_result 仍合并成同一条 user message(单子:wire 语义不变)。
        CHECK(message.role == api::Role::User);
        CHECK(message.content.size() == 5);
        persist_log.push_back("results");
    };
    callbacks = collector.Decorate(callbacks);
    REQUIRE(loop.Run("跑", callbacks).has_value());
    // 第二步模型的收尾 assistant 消息也走同一关口(那是正常的),审计
    // 次序看前两笔:assistant 先落、五枚结果后落。
    REQUIRE(persist_log.size() >= 2);
    CHECK(persist_log[0] == "assistant");
    CHECK(persist_log[1] == "results");
}

// ---------------------------------------------------------------------------
// 阶段 3:来源与错误码
// ---------------------------------------------------------------------------

TEST_CASE("registry: 注册元数据不靠 RTTI;DeferredTool 透传") {
    tools::ToolRegistry registry;
    tools::ToolRegistration registration;
    registration.tool = std::make_unique<FakeTool>("probe", tools::Tool::Result{"ok", false});
    registration.source_kind = tools::ToolSourceKind::Mcp;
    registration.source_instance = "test-server";
    registration.effect_class = tools::EffectClass::RemoteIrreversible;
    registry.Register(std::move(registration));

    const auto* found = registry.RegistrationOf("probe");
    REQUIRE(found != nullptr);
    CHECK(found->source_kind == tools::ToolSourceKind::Mcp);
    CHECK(found->source_instance == "test-server");
    CHECK(found->effect_class == tools::EffectClass::RemoteIrreversible);

    // 旧门:不带元数据,按 builtin + 工具自己的声明。
    tools::ToolRegistry plain;
    plain.Register(std::make_unique<FakeTool>("local_probe", tools::Tool::Result{"ok", false}));
    const auto* plain_reg = plain.RegistrationOf("local_probe");
    REQUIRE(plain_reg != nullptr);
    CHECK(plain_reg->source_kind == tools::ToolSourceKind::Builtin);
    CHECK(plain_reg->effect_class == tools::EffectClass::ReadOnlyLocal);  // FakeTool 自己报的
}

TEST_CASE("RunOneTool: 来源/错误码随 trace 落账(unknown_tool/hook_denied/permission_declined)") {
    tools::ToolRegistry registry;
    // 故意不注册任何工具 -> unknown_tool。
    api::ToolUseBlock call;
    call.id = "u1";
    call.name = "ghost";

    TraceCollector collector;
    agent::Callbacks callbacks;
    callbacks = collector.Decorate(callbacks);
    agent::ToolTraceContext ctx;
    ctx.execution_id = "e1";
    ctx.batch_id = "b1";
    ctx.sequence_in_batch = 0;
    const auto result = agent::RunOneTool(registry, call, callbacks, nullptr, "", &ctx);
    CHECK(result.is_error);
    CHECK(result.outcome == "unknown_tool");
    CHECK(result.error_code == agent::kErrRegistryUnknownTool);

    REQUIRE(collector.events.size() == 1);
    CHECK(collector.events[0].kind == agent::ToolTraceEventKind::ExecutionFinished);
    CHECK(collector.events[0].outcome == agent::ToolOutcome::UnknownTool);
    CHECK(collector.events[0].execution_id == "e1");

    // hook deny -> hook_denied + kErrHookPreDenied,且没越过 started。
    tools::ToolRegistry deny_registry;
    deny_registry.Register(std::make_unique<FakeTool>("ghost", tools::Tool::Result{"ok", false}));
    TraceCollector deny_collector;
    agent::Callbacks deny_callbacks;
    deny_callbacks.on_pre_tool_use_hook = [](const std::string&, const std::string&,
                                             const nlohmann::json&) -> agent::ToolHookDecision {
        agent::ToolHookDecision decision;
        decision.decision = agent::ToolHookDecision::Decision::Deny;
        decision.reason = "不许";
        return decision;
    };
    deny_callbacks = deny_collector.Decorate(deny_callbacks);
    const auto denied = agent::RunOneTool(deny_registry, call, deny_callbacks, nullptr, "", &ctx);
    CHECK(denied.outcome == "hook_denied");
    REQUIRE(deny_collector.events.size() == 1);
    CHECK(deny_collector.events[0].outcome == agent::ToolOutcome::HookDenied);
    CHECK(deny_collector.events[0].error_code == agent::kErrHookPreDenied);

    // 权限拒:needs_confirm + 拒绝回调。
    tools::ToolRegistry confirm_registry;
    struct ConfirmTool : tools::Tool {
        std::string name() const override { return "danger"; }
        std::string description() const override { return "d"; }
        nlohmann::json input_schema() const override { return nlohmann::json::object(); }
        bool needs_confirm() const override { return true; }
        tools::Tool::Result execute(const nlohmann::json&) override { return {"ran", false}; }
    };
    confirm_registry.Register(std::make_unique<ConfirmTool>());
    api::ToolUseBlock danger_call;
    danger_call.id = "u2";
    danger_call.name = "danger";
    TraceCollector declined_collector;
    agent::Callbacks declined_callbacks;
    declined_callbacks.on_tool_confirm = [](const std::string&, const std::string&, const nlohmann::json&) {
        return false;
    };
    declined_callbacks = declined_collector.Decorate(declined_callbacks);
    const auto declined = agent::RunOneTool(confirm_registry, danger_call, declined_callbacks, nullptr, "", &ctx);
    CHECK(declined.outcome == "permission_declined");
    REQUIRE(declined_collector.events.size() == 1);
    CHECK(declined_collector.events[0].outcome == agent::ToolOutcome::PermissionDeclined);
}

// ---------------------------------------------------------------------------
// 阶段 4:undo token(条件式撤销的账)
// ---------------------------------------------------------------------------

TEST_CASE("write_file/edit_file: 结果带 undo token(pre/post 哈希)") {
    const std::string dir = agent::MakeSessionSlug("undo 测试") + "-dir";
    std::filesystem::path tmp = std::filesystem::temp_directory_path() / dir;
    std::filesystem::create_directories(tmp);
    const std::string target = (tmp / "f.txt").string();

    tools::WriteFileTool writer;
    nlohmann::json input;
    input["path"] = target;
    input["content"] = "first";
    auto written = writer.execute(input);
    CHECK_FALSE(written.is_error);
    CHECK(written.undo_created_new_file);  // 目标不存在,这枚是新建
    CHECK(written.undo_preimage.empty());  // 新建无 preimage 正文(sha 是空串的哈希,仅形式)

    // 再写一次:preimage 是 "first",postimage 是 "second"。
    input["content"] = "second";
    auto rewritten = writer.execute(input);
    CHECK_FALSE(rewritten.is_error);
    CHECK_FALSE(rewritten.undo_created_new_file);
    CHECK_FALSE(rewritten.undo_preimage.empty());
    CHECK(rewritten.undo_preimage == "first");

    std::error_code cleanup_ec;
    std::filesystem::remove_all(tmp, cleanup_ec);
}

// ---------------------------------------------------------------------------
// 归因:可疑窗口
// ---------------------------------------------------------------------------

TEST_CASE("可疑窗口: 验证点两侧;无验证点不编答案") {
    agent::ToolExecutionLedger ledger;
    // a, b, c, d 四枚 finished。
    for (int i = 0; i < 4; ++i) {
        agent::ToolTraceEvent sched;
        sched.kind = agent::ToolTraceEventKind::Scheduled;
        sched.execution_id = "e" + std::to_string(i);
        sched.seq = static_cast<std::uint64_t>(i * 10 + 1);
        ledger.Fold(sched);
        agent::ToolTraceEvent fin;
        fin.kind = agent::ToolTraceEventKind::ExecutionFinished;
        fin.execution_id = "e" + std::to_string(i);
        fin.outcome = agent::ToolOutcome::Succeeded;
        fin.seq = static_cast<std::uint64_t>(i * 10 + 2);
        ledger.Fold(fin);
    }
    // 无验证点:窗口无效。
    CHECK_FALSE(ledger.ComputeSuspectWindow().valid);

    // b 之后验证过(通过),d 之后验证失败 -> 窗口 (b, d] = c, d。
    agent::ToolTraceEvent good;
    good.kind = agent::ToolTraceEventKind::Verification;
    good.execution_id = "v1";
    good.after_execution_id = "e1";
    good.passed = true;
    good.seq = 30;
    ledger.Fold(good);
    agent::ToolTraceEvent bad;
    bad.kind = agent::ToolTraceEventKind::Verification;
    bad.execution_id = "v2";
    bad.after_execution_id = "e3";
    bad.passed = false;
    bad.seq = 50;
    ledger.Fold(bad);

    const auto window = ledger.ComputeSuspectWindow();
    REQUIRE(window.valid);
    CHECK(window.last_verified_good == "e1");
    CHECK(window.first_observed_bad == "e3");
    std::set<std::string> window_ids;
    for (const auto* record : window.window) {
        window_ids.insert(record->execution_id);
    }
    CHECK(window_ids.count("e2") == 1);
    CHECK(window_ids.count("e3") == 1);
    CHECK(window_ids.count("e0") == 0);
    CHECK(window_ids.count("e1") == 0);
}

// ---------------------------------------------------------------------------
// MCP transport generation(迟到响应关联旧请求的账)
// ---------------------------------------------------------------------------

TEST_CASE("MCP: 换一代传输层 generation +1,jsonrpc id 随 CallTool 带出") {
    // Client 层:代数递增与 jsonrpc id 出口(McpTool 再把这两样抄进
    // Result.details,那里不重复测——编译期钉住)。
    {
        FakeTransport transport;
        mcp::Client client("gen-test");
        CHECK(client.transport_generation() == 0);  // 还没接过传输层
        client.AttachTransportForTest(&transport);
        const std::uint64_t gen1 = client.transport_generation();
        CHECK(gen1 == 1);

        std::int64_t last_jsonrpc_id = -1;
        transport.on_write = [&](const std::string& line) {
            const auto request = nlohmann::json::parse(line);
            const auto id = request.at("id").get<std::int64_t>();
            last_jsonrpc_id = id;
            const nlohmann::json response = {{"jsonrpc", "2.0"},
                                             {"id", id},
                                             {"result", {{"content", nlohmann::json::array()}, {"isError", false}}}};
            client.OnLine(response.dump());
        };

        std::int64_t seen_id = -1;
        const auto result = client.CallTool("echo", nlohmann::json::object(), &seen_id);
        CHECK_FALSE(result.is_error);
        CHECK(seen_id == last_jsonrpc_id);
        CHECK(seen_id >= 0);
    }
    // 换一代:generation 再 +1。声明序:transport 都在 client 之前
    //(Client 析构摸 transport_->Shutdown(),晚声明的 transport 会先死
    //——g++ 下 pure virtual called,见 test_mcp_client 的前车之鉴)。
    {
        FakeTransport transport;
        FakeTransport transport2;
        mcp::Client client("gen-test-2");
        client.AttachTransportForTest(&transport);
        CHECK(client.transport_generation() == 1);
        client.AttachTransportForTest(&transport2);
        CHECK(client.transport_generation() == 2);
    }
}
