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

#include "platform/paths.hpp"

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <sstream>
#include <set>
#include <vector>

#include "agent/agent.hpp"
#include "agent/loop.hpp"
#include "agent/tool_trace.hpp"
#include "platform/text_encoding.hpp"
#include "runtime/id_authority.hpp"
#include "runtime/tool_trace_hub.hpp"
#include "api/backend.hpp"
#include "api/types.hpp"
#include "tools/registry.hpp"
#include "tools/tool.hpp"
#include "mcp/client.hpp"
#include "tools/write_file.hpp"
#include "tools/undo_file_edit.hpp"
#include "tools/agent_tool.hpp"

using namespace lubancode;

namespace {

class TempDir {
public:
    explicit TempDir(const std::string& label) {
        path_ = std::filesystem::temp_directory_path() /
                ("lubancode_tool_trace_" + label + "_" +
                 std::to_string(reinterpret_cast<std::uintptr_t>(this)));
        std::error_code ec;
        std::filesystem::remove_all(path_, ec);
        std::filesystem::create_directories(path_, ec);
    }

    ~TempDir() {
        std::error_code ec;
        std::filesystem::remove_all(path_, ec);
    }

    const std::filesystem::path& Get() const { return path_; }

private:
    std::filesystem::path path_;
};

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
    // 块序号字段是 int:花括号初始化里 size_t->int 是窄化,MSVC/clang
    // 当错拦(gcc 只警告)——显式转。
    for (std::size_t i = 0; i < ids.size(); ++i) {
        script.push_back(api::ToolUseStart{static_cast<int>(i), ids[i], name});
        script.push_back(api::ToolUseInputDelta{static_cast<int>(i), "{}"});
        script.push_back(api::ContentBlockDone{static_cast<int>(i)});
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
    agent::TurnWiring Decorate(agent::TurnWiring callbacks) {
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

TEST_CASE("tool_trace: 中文结果截头尾不劈 UTF-8,完成事件仍可序列化") {
    // 160 字节刀口落在“先”字腰上,200 字节刀口落在“后”字腰上。
    // 旧代码先留下 E5 85,再接省略号 E2 80 A6,复现 type_error.316:
    // invalid UTF-8 byte at index 160: 0xE2。
    const std::string content = std::string(158, 'a') + "先" + std::string(37, 'b') + "后" +
                                std::string(200, 'c');
    REQUIRE(content.size() > 320);

    tools::ToolRegistry registry;
    registry.Register(std::make_unique<FakeTool>("probe", tools::Tool::Result{content, false}));
    api::ToolUseBlock call;
    call.id = "u_utf8";
    call.name = "probe";
    call.input = nlohmann::json::object();

    TraceCollector collector;
    agent::TurnWiring callbacks = collector.Decorate({});
    agent::ToolTraceContext ctx;
    ctx.execution_id = "e_utf8";
    ctx.batch_id = "b_utf8";
    ctx.sequence_in_batch = 0;
    const auto result = agent::RunOneTool(registry, call, callbacks, nullptr, "", &ctx);
    CHECK_FALSE(result.is_error);
    REQUIRE(collector.events.size() == 2);

    const auto& finished = collector.events.back();
    REQUIRE(finished.kind == agent::ToolTraceEventKind::ExecutionFinished);
    CHECK(platform::IsValidUtf8(finished.fallback_message));
    CHECK(platform::IsValidUtf8(finished.result_ref.preview));
    CHECK(finished.fallback_message.size() == 198);

    const std::string line = agent::SerializeToolTraceEvent(finished, "2026-08-24 11:23:37");
    CHECK(platform::IsValidUtf8(line));
    CHECK(agent::ParseToolTraceEvent(line).has_value());
}

TEST_CASE("tool_trace: 序列化出口会清洗漏入事件的坏 UTF-8") {
    agent::ToolTraceEvent event;
    event.kind = agent::ToolTraceEventKind::ExecutionFinished;
    event.execution_id = "e_bad_utf8";
    event.tool_name = "foreign_tool";
    event.outcome = agent::ToolOutcome::Succeeded;
    event.fallback_message = std::string("外来结果") + static_cast<char>(0xE2);
    event.result_ref.kind = agent::ToolResultRef::Kind::Inline;
    event.result_ref.content = event.fallback_message;

    const std::string line = agent::SerializeToolTraceEvent(event, "2026-08-24 11:23:37");
    CHECK(platform::IsValidUtf8(line));
    CHECK(agent::ParseToolTraceEvent(line).has_value());
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

    agent::Agent loop(backend, registry, agent::AgentProfile{.request{.model = "m"}, .system_prompt = "sys"});
    TraceCollector collector;
    agent::TurnWiring callbacks;
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

    agent::Agent loop(backend, registry, agent::AgentProfile{.request{.model = "m"}, .system_prompt = "sys"});
    TraceCollector collector;
    agent::TurnWiring callbacks;
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

    agent::Agent loop(backend, registry, agent::AgentProfile{.request{.model = "m"}, .system_prompt = "sys"});
    TraceCollector collector;
    agent::TurnWiring callbacks;
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

    agent::Agent loop(backend, registry, agent::AgentProfile{.request{.model = "m"}, .system_prompt = "sys"});
    TraceCollector collector;
    agent::TurnWiring callbacks;
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

// (P0-6:trace-aware 修补(RepairToolPairsWithTrace)的用例已删——旧档
// 恢复路退场;新账的悬空工具按 trajectory 三道账封存。)

// (P0-6:tool_trace_v1 行的落盘回读用例已删——持久账走 trajectory
// Journal 的 typed 工具事件。)

TEST_CASE("消息落盘次序: assistant 先落、五结果一条 user message 后落") {
    FakeBackend backend;
    backend.scripts = {
        FiveToolScript({"a1", "a2", "a3", "a4", "a5"}, "probe"),
        TextOnlyScript("done"),
    };
    tools::ToolRegistry registry;
    registry.Register(std::make_unique<FakeTool>("probe", tools::Tool::Result{"ok", false}));

    agent::Agent loop(backend, registry, agent::AgentProfile{.request{.model = "m"}, .system_prompt = "sys"});
    std::vector<std::string> persist_log;
    TraceCollector collector;
    agent::TurnWiring callbacks;
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
    agent::TurnWiring callbacks;
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
    agent::TurnWiring deny_callbacks;
    deny_callbacks.on_pre_tool_use_hook = [](const std::string&, const std::string&,
                                             const nlohmann::json&) -> runtime::ToolHookDecision {
        runtime::ToolHookDecision decision;
        decision.decision = runtime::ToolHookDecision::Decision::Deny;
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
    agent::TurnWiring declined_callbacks;
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
    TempDir tmp("undo_token");
    const std::string target = (tmp.Get() / "f.txt").string();

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

// ---------------------------------------------------------------------------
// 第四期:条件式撤销的执行侧(undo_file_edit)
// ---------------------------------------------------------------------------

TEST_CASE("undo_file_edit: 改后未再动,恢复 preimage") {
    TempDir tmp("undo_exec");
    const std::string target = (tmp.Get() / "f.txt").string();

    // write 两枚(第一枚新建,第二枚覆盖——token 是"first -> second"这
    // 枚覆盖,preimage 才有正文),再按 token 撤销。
    tools::WriteFileTool writer;
    nlohmann::json input;
    input["path"] = target;
    input["content"] = "first";
    REQUIRE_FALSE(writer.execute(input).is_error);
    input["content"] = "second";
    auto written = writer.execute(input);
    REQUIRE_FALSE(written.is_error);
    REQUIRE(written.undo_available());
    CHECK(written.undo_preimage == "first");

    agent::ToolUndoToken token;
    token.path = written.undo_path;
    token.preimage_sha256 = written.undo_preimage_sha256;
    token.postimage_sha256 = written.undo_postimage_sha256;
    token.preimage = written.undo_preimage;
    token.created_new_file = written.undo_created_new_file;

    const auto undone = tools::ApplyConditionalUndo(std::filesystem::path(target), token);
    CHECK(undone.performed);
    CHECK_FALSE(undone.is_error);
    // 内容回到 preimage。
    std::ifstream in(std::filesystem::path(target), std::ios::binary);
    std::stringstream buffer;
    buffer << in.rdbuf();
    CHECK(buffer.str() == written.undo_preimage);

}

TEST_CASE("undo_file_edit: 改后用户又改,拒绝自动撤销并给三方对照") {
    TempDir tmp("undo_refuse");
    const std::string target = (tmp.Get() / "f.txt").string();

    tools::WriteFileTool writer;
    nlohmann::json input;
    input["path"] = target;
    input["content"] = "second";
    auto written = writer.execute(input);
    REQUIRE_FALSE(written.is_error);

    // 用户(或后续工具)又改了文件:postimage 对不上。
    {
        std::ofstream out(std::filesystem::path(target), std::ios::binary | std::ios::trunc);
        out << "third-party edit";
    }

    agent::ToolUndoToken token;
    token.path = written.undo_path;
    token.preimage_sha256 = written.undo_preimage_sha256;
    token.postimage_sha256 = written.undo_postimage_sha256;
    token.preimage = written.undo_preimage;
    token.created_new_file = written.undo_created_new_file;

    const auto refused = tools::ApplyConditionalUndo(std::filesystem::path(target), token);
    CHECK_FALSE(refused.performed);
    CHECK(refused.is_error);
    CHECK(refused.message.find("又被改过") != std::string::npos);
    CHECK(refused.message.find("preimage") != std::string::npos);  // 三方对照给了
    // 文件内容不动。
    {
        std::ifstream in(std::filesystem::path(target), std::ios::binary);
        std::stringstream buffer;
        buffer << in.rdbuf();
        CHECK(buffer.str() == "third-party edit");
    }

}

TEST_CASE("undo_file_edit: 新建文件内容未再变,整枚移走") {
    TempDir tmp("undo_newfile");
    const std::string target = (tmp.Get() / "new.txt").string();

    tools::WriteFileTool writer;
    nlohmann::json input;
    input["path"] = target;
    input["content"] = "brand new";
    auto written = writer.execute(input);
    REQUIRE_FALSE(written.is_error);
    REQUIRE(written.undo_created_new_file);

    agent::ToolUndoToken token;
    token.path = written.undo_path;
    token.postimage_sha256 = written.undo_postimage_sha256;
    token.created_new_file = true;

    const auto removed = tools::ApplyConditionalUndo(std::filesystem::path(target), token);
    CHECK(removed.performed);
    CHECK_FALSE(std::filesystem::exists(std::filesystem::path(target)));

}

TEST_CASE("undo_file_edit: 账本查不到凭据/凭据不完整,如实说不猜") {
    tools::UndoTokenLookup empty_lookup;  // 没接查表函数
    tools::UndoFileEditTool tool(std::move(empty_lookup));
    nlohmann::json input;
    input["execution_id"] = "item-404";
    const auto result = tool.execute(input);
    CHECK(result.is_error);
    CHECK(result.content.find("追踪账") != std::string::npos);
    CHECK(tool.last_compensates().empty());

    // 接了查表但账里没有。
    tools::UndoTokenLookup miss;
    miss.find = [](const std::string&) { return std::optional<agent::ToolUndoToken>(); };
    tools::UndoFileEditTool miss_tool(std::move(miss));
    const auto miss_result = miss_tool.execute(input);
    CHECK(miss_result.is_error);
    CHECK(miss_result.content.find("凭据") != std::string::npos);

    // 凭据不完整(preimage 超限没内联)。
    tools::UndoTokenLookup partial;
    partial.find = [](const std::string&) {
        agent::ToolUndoToken token;
        token.path = "some/file.txt";
        token.postimage_sha256 = "ff";
        token.created_new_file = false;
        // preimage 空:available() 为 false。
        return std::optional<agent::ToolUndoToken>(token);
    };
    partial.owner_of = [](const std::string& id) { return id; };
    tools::UndoFileEditTool partial_tool(std::move(partial));
    const auto partial_result = partial_tool.execute(input);
    CHECK(partial_result.is_error);
    CHECK(partial_result.content.find("preimage") != std::string::npos);
    // compensates 关系照样报(execute 查到了 token 就有边)。
    CHECK(partial_tool.last_compensates() == "item-404");
}

TEST_CASE("undo_file_edit: needs_confirm 恒真,补偿关系边随 finished 落账") {
    tools::ToolRegistry registry;
    tools::UndoTokenLookup run_lookup;
    run_lookup.find = [](const std::string&) { return std::optional<agent::ToolUndoToken>(); };
    run_lookup.owner_of = [](const std::string& id) { return id; };
    registry.Register(std::make_unique<tools::UndoFileEditTool>(std::move(run_lookup)));

    api::ToolUseBlock call;
    call.id = "u_undo";
    call.name = "undo_file_edit";
    call.input = nlohmann::json{{"execution_id", "item-17"}};

    TraceCollector collector;
    agent::TurnWiring callbacks;
    callbacks.on_tool_compensates = [&registry](const std::string&, const std::string& tool_name) {
        if (tool_name != "undo_file_edit") {
            return std::string();
        }
        const auto* tool = registry.Find(tool_name);
        const auto* undo_tool = dynamic_cast<const tools::UndoFileEditTool*>(tool);
        return undo_tool != nullptr ? undo_tool->last_compensates() : std::string();
    };
    callbacks = collector.Decorate(callbacks);

    agent::ToolTraceContext ctx;
    ctx.execution_id = "e_undo";
    ctx.batch_id = "b1";
    ctx.sequence_in_batch = 0;
    const auto result = agent::RunOneTool(registry, call, callbacks, nullptr, "", &ctx);
    CHECK(result.is_error);  // 查表 miss,如实失败

    // needs_confirm 恒真(单子:undo 本身也是工具调用,须确认)。
    const auto* registered = registry.Find("undo_file_edit");
    REQUIRE(registered != nullptr);
    CHECK(registered->needs_confirm());
    CHECK(registered->effect_class() == tools::EffectClass::LocalReversible);

    REQUIRE_FALSE(collector.events.empty());
    const auto& finished = collector.events.back();
    CHECK(finished.kind == agent::ToolTraceEventKind::ExecutionFinished);
    CHECK(finished.compensates == "item-17");  // 关系边落账
    CHECK(finished.execution_id == "e_undo");
}

// ---------------------------------------------------------------------------
// 后台子代理经只读 sink 并轨(单子 agent/PTC 节)
// ---------------------------------------------------------------------------

TEST_CASE("子代理内层工具带 parent_execution_id;hub 只读并轨不交错") {
    // 直接验 AgentTool 的转发闭包形状:构造 Hooks 带 on_tool_trace 与
    // parent_execution_id_getter,确认事件被透传且 parent 边补上。
    // (真跑 AgentTool 需要 backend 全链,那里已有 test_agent_tool 钉;
    // 这里钉的是"转发补边"这一环。)
    std::vector<agent::ToolTraceEvent> seen;
    tools::AgentTool::Hooks hooks;
    std::string current_parent = "item-99";  // hub 钉的当前 agent 调用
    hooks.on_tool_trace = [&seen](const agent::ToolTraceEvent& event) { seen.push_back(event); };
    hooks.parent_execution_id_getter = [&current_parent]() { return current_parent; };

    // 复刻 RunTask 里的转发闭包(同一形状:延迟取 getter,透传事件)。
    const std::function<void(const agent::ToolTraceEvent&)> forward =
        [&hooks](const agent::ToolTraceEvent& event) {
            agent::ToolTraceEvent forwarded = event;
            if (hooks.parent_execution_id_getter) {
                forwarded.parent_execution_id = hooks.parent_execution_id_getter();
            }
            hooks.on_tool_trace(forwarded);
        };

    agent::ToolTraceEvent inner;
    inner.kind = agent::ToolTraceEventKind::ExecutionFinished;
    inner.execution_id = "item-100";
    inner.tool_use_id = "sub_u1";
    inner.tool_name = "read_file";
    forward(inner);
    REQUIRE(seen.size() == 1);
    CHECK(seen[0].execution_id == "item-100");
    CHECK(seen[0].parent_execution_id == "item-99");

    // agent 调用收尾后 getter 清空:后续事件 parent 如实缺边。
    current_parent.clear();
    forward(inner);
    REQUIRE(seen.size() == 2);
    CHECK(seen[0].parent_execution_id == "item-99");
    CHECK(seen[1].parent_execution_id.empty());
}

TEST_CASE("hub 的 agent 执行区间:scheduled 期间 parent 可查,finished 清位") {
    lubancode::runtime::IdAuthority ids;
    lubancode::runtime::ToolTraceHub hub(ids);  // 只走内存账
    CHECK(hub.current_agent_execution().empty());

    agent::ToolTraceEvent started;
    started.kind = agent::ToolTraceEventKind::ExecutionStarted;
    started.execution_id = "item-7";
    started.tool_name = "agent";
    hub.OnTrace(started);
    CHECK(hub.current_agent_execution() == "item-7");

    // 子代理事件在此区间投递,装配层取到的就是这枚 agent 调用。
    agent::ToolTraceEvent sub_finished;
    sub_finished.kind = agent::ToolTraceEventKind::ExecutionFinished;
    sub_finished.execution_id = "item-8";
    sub_finished.tool_name = "read_file";
    hub.OnTrace(sub_finished);

    agent::ToolTraceEvent agent_finished;
    agent_finished.kind = agent::ToolTraceEventKind::ExecutionFinished;
    agent_finished.execution_id = "item-7";
    agent_finished.tool_name = "agent";
    hub.OnTrace(agent_finished);
    CHECK(hub.current_agent_execution().empty());
}
