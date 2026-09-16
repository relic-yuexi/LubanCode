// T11-D / T11-E / V3-GAP-06(Session v3 旧设计清理单):验证/迟到/恢复注记
// 与容量/预算。
//
// T11-D 断点:v3 轮桥把 Verification/RecoveryMarker/McpLateResponse 三类
// trace 一概丢弃("v3 无对应 kind,不伪造"),验证账整场不落,stale
// invalidation 也没有 v3 路——旧测试给新代码作证无从谈起。
//   本册钉:tool.verification.recorded 关联工具(actionId)与产物/事实;
//   失效对账落 tool.verification.invalidated(reason);迟到响应/恢复注记
//   只记观察(tool.observation.late / recovery.note.recorded),不改已提交
//   终态;读面 FoldVerificationFacts 的 fresh 折算——重复/迟到/变更后旧
//   验证不封新目标,也不触发工具重做(纯读,工具事件数不变)。
//
// T11-E 断点:发送前容量压力在 v3 一笔不落(v2 才有 context.pressure.
// recorded);降档裁决只进日志。
//   本册钉:context.pressure.recorded 的三项账数字 + verdict 三分 + 剩余
//   量;禁携带累计用量字段(schema 拒);读面 FoldPressureFacts 对账;
//   用量唯一 owner 仍是 assistant message(pressure 行不是第二份账)。
#include <doctest/doctest.h>

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "accounting/purpose.hpp"
#include "agent/context.hpp"  // ContextPressure
#include "agent/loop.hpp"     // RequestPreparedContext
#include "agent/tool_trace.hpp"
#include "api/types.hpp"
#include "platform/paths.hpp"
#include "runtime/trajectory_session.hpp"
#include "trajectory/v3/envelope.hpp"
#include "trajectory/v3/reader.hpp"
#include "trajectory/v3/schema3.hpp"
#include "workspace/identity.hpp"

namespace platform = lubancode::platform;
using namespace lubancode;
using lubancode::runtime::TrajectorySessionLedger;
using lubancode::runtime::TrajectoryTurnBridge;

namespace {

namespace fs = std::filesystem;

struct EnvGuard {
    explicit EnvGuard(const char* name, const char* value) : name_(name) {
#ifdef _WIN32
        _putenv((std::string(name_) + "=" + value).c_str());
#else
        setenv(name_, value, 1);
#endif
    }
    ~EnvGuard() {
#ifdef _WIN32
        _putenv((std::string(name_) + "=").c_str());
#else
        unsetenv(name_);
#endif
    }
    const char* name_;
};

fs::path FreshRoot(const char* tag) {
    const auto dir =
        fs::temp_directory_path() / ("lubancode-v3-t11-verify-budget-" + std::string(tag));
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir, ec);
    return dir;
}

TrajectorySessionLedger::Options LedgerOptions(const fs::path& root) {
    TrajectorySessionLedger::Options options;
    options.workspaces_root = root / "workspaces";
    options.workspace_root = root / "repo";
    options.workspace_identity = lubancode::workspace::MakeFallbackIdentity(root / "repo");
    options.launch_cwd = "D:/tmp/repo";
    options.lubancode_version = "0.26.269-test";
    options.v3_system_content = "你是 LubanCode。";
    return options;
}

fs::path V3StreamOf(const TrajectorySessionLedger& ledger) {
    return ledger.session_dir() /
           platform::Utf8ToPath(platform::PathToUtf8(ledger.session_dir().filename()) + ".jsonl");
}

std::vector<nlohmann::json> ReadLines(const fs::path& stream) {
    std::vector<nlohmann::json> rows;
    std::ifstream file(stream, std::ios::binary);
    std::string line;
    while (std::getline(file, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        if (line.empty()) {
            continue;
        }
        rows.push_back(nlohmann::json::parse(line, nullptr, /*allow_exceptions=*/false));
        REQUIRE_FALSE(rows.back().is_discarded());
    }
    return rows;
}

std::vector<const nlohmann::json*> RowsOfKind(const std::vector<nlohmann::json>& rows,
                                              const char* kind) {
    std::vector<const nlohmann::json*> out;
    for (const auto& row : rows) {
        if (row.value("kind", std::string()) == kind) {
            out.push_back(&row);
        }
    }
    return out;
}

std::size_t CountKind(const std::vector<nlohmann::json>& rows, const char* kind) {
    return RowsOfKind(rows, kind).size();
}

api::Message UserMessage(const std::string& text) {
    api::Message message;
    message.role = api::Role::User;
    message.content.push_back(api::TextBlock{text});
    return message;
}

api::Message AssistantWithToolCall(const std::string& call_id) {
    api::Message message;
    message.role = api::Role::Assistant;
    message.content.push_back(api::TextBlock{"我写一下文件。"});
    api::ToolUseBlock call;
    call.id = call_id;
    call.name = "write_file";
    call.input = nlohmann::json{{"path", "src/app/main.cpp"}};
    message.content.push_back(std::move(call));
    return message;
}

// 同一 assistant 声明两枚调用(验证案:call_w2 执行+验证,call_w3 再改)。
api::Message AssistantWithTwoToolCalls(const std::string& first_id, const std::string& second_id) {
    api::Message message;
    message.role = api::Role::Assistant;
    message.content.push_back(api::TextBlock{"分两步改。"});
    api::ToolUseBlock first;
    first.id = first_id;
    first.name = "write_file";
    first.input = nlohmann::json{{"path", "src/app/main.cpp"}};
    message.content.push_back(std::move(first));
    api::ToolUseBlock second;
    second.id = second_id;
    second.name = "write_file";
    second.input = nlohmann::json{{"path", "src/app/main.cpp"}};
    message.content.push_back(std::move(second));
    return message;
}

agent::ToolTraceEvent TraceEvent(agent::ToolTraceEventKind kind, const std::string& call_id) {
    agent::ToolTraceEvent event;
    event.kind = kind;
    event.tool_use_id = call_id;
    event.tool_name = "write_file";
    event.execution_id = "exec-" + call_id;
    event.effective_input_sha256 = "aaaa1111aaaa1111aaaa1111aaaa1111aaaa1111aaaa1111aaaa1111aaaa1111";
    event.effective_arguments = nlohmann::json{{"path", "src/app/main.cpp"}};
    return event;
}

// 一轮"输入 -> 请求/回复(带工具声明) -> 执行(finished,undo.path 指定
// 被改文件) -> 结果回喂"的最小完整流;返回这轮请求 id。end_turn=false
// 时收口留给调用方(回合内还要补观察事件——回合门只放行本回合的账)。
std::string DriveWriteToolTurn(TrajectoryTurnBridge& bridge, const std::string& call_id,
                               const std::string& mutated_path, bool end_turn = true) {
    bridge.BeginTurn("turn-1", "external_user");
    bridge.RecordInput(UserMessage("改一下入口"));
    agent::RequestPreparedContext ctx;
    ctx.purpose = accounting::RequestPurpose::MainTurn;
    api::Request request;
    request.model = "kimi-k2.6";
    request.system = "SYSTEM-X";
    request.messages.push_back(UserMessage("改一下入口"));
    const std::string request_id = bridge.OnRequestPrepared(request, ctx);
    REQUIRE_FALSE(request_id.empty());
    REQUIRE(bridge.OnRequestSent(request_id));
    api::Usage usage;
    usage.input_tokens = 1100;
    usage.output_tokens = 30;
    bridge.OnUsageRecorded(request_id, usage, /*reported_by_provider=*/true, "resp-1");
    REQUIRE(bridge.OnOutputCompleted(request_id, AssistantWithToolCall(call_id), "tool_calls",
                                     "resp-1"));
    bridge.OnToolTrace(TraceEvent(agent::ToolTraceEventKind::Scheduled, call_id));
    bridge.OnToolTrace(TraceEvent(agent::ToolTraceEventKind::ExecutionStarted, call_id));
    agent::ToolTraceEvent finished =
        TraceEvent(agent::ToolTraceEventKind::ExecutionFinished, call_id);
    finished.outcome = agent::ToolOutcome::Succeeded;
    finished.duration_ms = 50;
    finished.undo.path = mutated_path;
    bridge.OnToolTrace(finished);
    api::Message results;
    results.role = api::Role::User;
    api::ToolResultBlock result;
    result.tool_use_id = call_id;
    result.content = "written";
    results.content.push_back(std::move(result));
    bridge.OnToolResultsCommitted("batch-1", results);
    if (end_turn) {
        bridge.EndTurn(/*ok=*/true, /*cancelled=*/false, "");
    }
    return request_id;
}

// ---- schema 合同层(纯构造) ----

nlohmann::json EventJson(const char* kind, nlohmann::json payload) {
    nlohmann::json json = nlohmann::json::object();
    json["type"] = "event";
    json["schemaVersion"] = 3;
    json["sessionId"] = "20260916-090000-PC0002";
    json["runId"] = "run-000001";
    json["seq"] = 6;
    json["timestamp"] = "2026-09-16T01:00:00.000Z";
    json["eventId"] = "evt-000005";
    json["kind"] = kind;
    json["payload"] = std::move(payload);
    json["prevHash"] = std::string(lubancode::trajectory::v3::kGenesisHash);
    json["lineHash"] = std::string(lubancode::trajectory::v3::kGenesisHash);
    return json;
}

std::optional<lubancode::trajectory::v3::Schema3Error> CheckEvent(const char* kind,
                                                                  nlohmann::json payload) {
    nlohmann::json json = EventJson(kind, std::move(payload));
    std::string ec, msg;
    auto parsed = lubancode::trajectory::v3::EventLine::FromJsonStrict(json, &ec, &msg);
    if (!parsed.has_value()) {
        return lubancode::trajectory::v3::Schema3Error{ec, msg};
    }
    return lubancode::trajectory::v3::ValidateEventLine(*parsed);
}

bool HasCode(const std::optional<lubancode::trajectory::v3::Schema3Error>& error, const char* code) {
    return error.has_value() && error->code == code;
}

}  // namespace

// ---------------------------------------------------------------------------
// schema 合同
// ---------------------------------------------------------------------------

TEST_CASE("T11-D/E schema: 观察族 statusless,载荷合同正反例") {
    using lubancode::trajectory::v3::EventKindV3;
    // statusless 注册。
    for (const EventKindV3 kind :
         {EventKindV3::ToolVerificationRecorded, EventKindV3::ToolVerificationInvalidated,
          EventKindV3::ToolObservationLate, EventKindV3::RecoveryNoteRecorded,
          EventKindV3::ContextPressureRecorded}) {
        CHECK_FALSE(lubancode::trajectory::v3::RequiredStatusForKind(kind).has_value());
    }
    // 验证事实:verificationId/kind/passed/producer 必填。
    CHECK_FALSE(CheckEvent("tool.verification.recorded",
                           nlohmann::json{{"verificationId", "verify-1"},
                                          {"kind", "file_contains"},
                                          {"passed", true},
                                          {"producer", "tool_trace"}})
                    .has_value());
    CHECK(HasCode(CheckEvent("tool.verification.recorded",
                             nlohmann::json{{"kind", "file_contains"},
                                            {"passed", true},
                                            {"producer", "tool_trace"}}),
                  "schema3.missing_field"));
    // 失效:verificationId + reason。
    CHECK_FALSE(CheckEvent("tool.verification.invalidated",
                           nlohmann::json{{"verificationId", "verify-1"},
                                          {"reason", "subject_modified"}})
                    .has_value());
    // 迟到观察:cause 必填,jsonrpcRequestId 非负。
    CHECK_FALSE(CheckEvent("tool.observation.late",
                           nlohmann::json{{"cause", "mcp_timeout_dropped"},
                                          {"jsonrpcRequestId", 7}})
                    .has_value());
    CHECK(HasCode(CheckEvent("tool.observation.late", nlohmann::json{{"server", "fs"}}),
                  "schema3.missing_field"));
    // 恢复注记。
    CHECK_FALSE(CheckEvent("recovery.note.recorded",
                           nlohmann::json{{"note", "unknown_side_effects_not_rerun"}})
                    .has_value());
    // 容量压力:四项数字 + verdict;禁累计用量字段。
    const nlohmann::json pressure_base = nlohmann::json{{"phase", "preflight"},
                                                        {"verdict", "exceeded_denied"},
                                                        {"estimatedInputTokens", 100},
                                                        {"reservedOutputTokens", 32},
                                                        {"protocolHeadroomTokens", 4},
                                                        {"windowTokens", 128},
                                                        {"remainingTokens", 0}};
    CHECK_FALSE(CheckEvent("context.pressure.recorded", pressure_base).has_value());
    nlohmann::json pressure_usage = pressure_base;
    pressure_usage["usage"] = nlohmann::json{{"input_tokens", 100}};
    CHECK(HasCode(CheckEvent("context.pressure.recorded", pressure_usage), "schema3.bad_type"));
    nlohmann::json pressure_bad_verdict = pressure_base;
    pressure_bad_verdict["verdict"] = "maybe";
    CHECK(HasCode(CheckEvent("context.pressure.recorded", pressure_bad_verdict),
                  "schema3.bad_type"));
}

// ---------------------------------------------------------------------------
// T11-D 写路:验证关联工具、失效对账、迟到观察不改终态
// ---------------------------------------------------------------------------

TEST_CASE("T11-D 验证: 关联 actionId;被改文件命中的验证失效,读面 fresh 折算") {
    EnvGuard guard("LUBANCODE_TRAJECTORY_V3_NEW_SESSIONS", "1");
    const auto root = FreshRoot("verify");
    auto ledger = TrajectorySessionLedger::Open(LedgerOptions(root));
    REQUIRE(ledger.has_value());
    const fs::path stream = V3StreamOf(*ledger);

    {
        auto bridge = ledger->NewTurnBridge({"moonshot", "openai-chat-completions", "terminal"});
        REQUIRE(bridge != nullptr);
        DriveWriteToolTurn(*bridge, "call_w1", "src/app/main.cpp");
    }
    {
        // 第二轮:assistant 先声明两枚调用(ownership 门只认本回合声明过的
        // call),call_w2 执行+验证,call_w3 再改同文件把 path 验证打失效。
        auto bridge = ledger->NewTurnBridge({"moonshot", "openai-chat-completions", "terminal"});
        bridge->BeginTurn("turn-2", "external_user");
        bridge->RecordInput(UserMessage("再改再验"));
        agent::RequestPreparedContext ctx;
        ctx.purpose = accounting::RequestPurpose::MainTurn;
        api::Request request;
        request.model = "kimi-k2.6";
        request.system = "SYSTEM-X";
        request.messages.push_back(UserMessage("再改再验"));
        const std::string request_id = bridge->OnRequestPrepared(request, ctx);
        REQUIRE_FALSE(request_id.empty());
        REQUIRE(bridge->OnRequestSent(request_id));
        REQUIRE(bridge->OnOutputCompleted(request_id, AssistantWithTwoToolCalls("call_w2", "call_w3"),
                                          "tool_calls", "resp-2"));
        bridge->OnToolTrace(TraceEvent(agent::ToolTraceEventKind::Scheduled, "call_w2"));
        bridge->OnToolTrace(TraceEvent(agent::ToolTraceEventKind::ExecutionStarted, "call_w2"));
        agent::ToolTraceEvent finished =
            TraceEvent(agent::ToolTraceEventKind::ExecutionFinished, "call_w2");
        finished.outcome = agent::ToolOutcome::Succeeded;
        finished.undo.path = "src/app/main.cpp";
        bridge->OnToolTrace(finished);
        // trace 验证点(工具锚定):挂在 call_w2 之后,同回合过 ownership 门。
        agent::ToolTraceEvent verify =
            TraceEvent(agent::ToolTraceEventKind::Verification, "call_w2");
        verify.label = "file_contains";
        verify.passed = true;
        verify.after_execution_id = "exec-call_w2";
        verify.verify_detail = "入口含 main";
        bridge->OnToolTrace(verify);
        // path 锚定验证(工具无关):subject 是文件路径。
        const std::string verify_id =
            bridge->BeginVerification("file_hash_match", "src/app/main.cpp", "host");
        REQUIRE_FALSE(verify_id.empty());
        bridge->FinishVerification(verify_id, /*passed=*/true,
                                   nlohmann::json{{"sha256", std::string(64, 'b')}},
                                   nlohmann::json{}, {"artifacts/verify-1.txt"});
        // 再改同文件:path 验证命中失效对账(trace 验证的 subject 是执行
        // 号,不按路径失效)。
        bridge->OnToolTrace(TraceEvent(agent::ToolTraceEventKind::Scheduled, "call_w3"));
        bridge->OnToolTrace(TraceEvent(agent::ToolTraceEventKind::ExecutionStarted, "call_w3"));
        agent::ToolTraceEvent mutate =
            TraceEvent(agent::ToolTraceEventKind::ExecutionFinished, "call_w3");
        mutate.outcome = agent::ToolOutcome::Succeeded;
        mutate.undo.path = "src/app/main.cpp";
        bridge->OnToolTrace(mutate);
        bridge->EndTurn(true, false, "");
    }

    const auto rows = ReadLines(stream);
    // 验证事实两枚:trace 路(带 actionId)+ path 路(artifactRefs 带上)。
    const auto recorded = RowsOfKind(rows, "tool.verification.recorded");
    REQUIRE(recorded.size() == 2);
    bool saw_action_linked = false;
    bool saw_path_linked = false;
    for (const auto* row : recorded) {
        const std::string id = row->at("payload").value("verificationId", std::string());
        if (row->contains("actionId") && !row->at("actionId").is_null()) {
            saw_action_linked = true;
            CHECK_FALSE(row->at("actionId").get<std::string>().empty());
            CHECK(row->at("payload").value("producer", std::string()) == "tool_trace");
        } else {
            saw_path_linked = true;
            CHECK(row->at("payload").contains("artifactRefs"));
            CHECK(row->at("payload").value("producer", std::string()) == "host");
        }
        CHECK(row->at("payload").value("passed", false) == true);
        CHECK_FALSE(id.empty());
    }
    CHECK(saw_action_linked);
    CHECK(saw_path_linked);
    // 失效:path 验证被后续改动命中(subject_modified);旧 recorded 行原样在。
    const auto invalidated = RowsOfKind(rows, "tool.verification.invalidated");
    REQUIRE(invalidated.size() == 1);
    CHECK(invalidated[0]->at("payload").value("reason", std::string()) == "subject_modified");
    CHECK(recorded.size() == 2);  // 不改写旧行(append-only)
    CHECK(lubancode::trajectory::v3::VerifyV3File(stream).ok);

    // 读面 fresh 折算:变更后的旧验证不封新目标(fresh=false);trace 验证
    // subject 是执行号,不因路径改动失效(fresh=true)。纯读,不触发工具
    // 重做——tool.execution.* 计数折叠前后不变。
    const auto ledger_data = lubancode::trajectory::v3::ReadV3Ledger(stream);
    REQUIRE(ledger_data.has_value());
    const std::size_t tool_events_before = std::count_if(
        ledger_data->events.begin(), ledger_data->events.end(), [](const auto& event) {
            return event.kind == lubancode::trajectory::v3::EventKindV3::ToolExecutionStarted;
        });
    const auto facts = lubancode::trajectory::v3::FoldVerificationFacts(*ledger_data);
    REQUIRE(facts.size() == 2);
    int fresh_count = 0;
    for (const auto& fact : facts) {
        if (fact.invalidated_reason.empty() && fact.fresh) {
            ++fresh_count;
        } else {
            CHECK(fact.fresh == false);
            CHECK(fact.invalidated_reason == "subject_modified");
        }
    }
    CHECK(fresh_count == 1);
    const auto again = lubancode::trajectory::v3::FoldVerificationFacts(*ledger_data);
    const std::size_t tool_events_after = std::count_if(
        ledger_data->events.begin(), ledger_data->events.end(), [](const auto& event) {
            return event.kind == lubancode::trajectory::v3::EventKindV3::ToolExecutionStarted;
        });
    CHECK(tool_events_before == tool_events_after);  // 重读不触发重做
    CHECK(again.size() == facts.size());
}

TEST_CASE("T11-D 迟到: tool.observation.late 只记观察,已提交终态不被覆盖") {
    EnvGuard guard("LUBANCODE_TRAJECTORY_V3_NEW_SESSIONS", "1");
    const auto root = FreshRoot("late");
    auto ledger = TrajectorySessionLedger::Open(LedgerOptions(root));
    REQUIRE(ledger.has_value());
    const fs::path stream = V3StreamOf(*ledger);

    {
        auto bridge = ledger->NewTurnBridge({"moonshot", "openai-chat-completions", "terminal"});
        REQUIRE(bridge != nullptr);
        DriveWriteToolTurn(*bridge, "call_l1", "docs/a.md", /*end_turn=*/false);
        // 终态已提交(本回合内),MCP 迟到响应到达:丢响应,留观察。
        agent::ToolTraceEvent late =
            TraceEvent(agent::ToolTraceEventKind::McpLateResponse, "call_l1");
        late.source_kind = agent::ToolSourceKind::Mcp;
        late.source_instance = "fs";
        late.jsonrpc_request_id = 42;
        bridge->OnToolTrace(late);
        // 恢复注记:恢复侧补的观察。
        agent::ToolTraceEvent note =
            TraceEvent(agent::ToolTraceEventKind::RecoveryMarker, "call_l1");
        note.note = "unknown_side_effects_not_rerun";
        bridge->OnToolTrace(note);
        bridge->EndTurn(true, false, "");
    }

    const auto rows = ReadLines(stream);
    // 终态唯一:finished 恰一枚,迟到响应不改写、不新增终态。
    CHECK(CountKind(rows, "tool.execution.finished") == 1);
    const auto late_rows = RowsOfKind(rows, "tool.observation.late");
    REQUIRE(late_rows.size() == 1);
    CHECK(late_rows[0]->at("payload").value("cause", std::string()) == "mcp_timeout_dropped");
    CHECK(late_rows[0]->at("payload").value("jsonrpcRequestId", -1) == 42);
    CHECK(late_rows[0]->at("payload").value("server", std::string()) == "fs");
    CHECK(late_rows[0]->contains("status") == false);
    const auto note_rows = RowsOfKind(rows, "recovery.note.recorded");
    REQUIRE(note_rows.size() == 1);
    CHECK(note_rows[0]->at("payload").value("note", std::string()) == "unknown_side_effects_not_rerun");
    CHECK(lubancode::trajectory::v3::VerifyV3File(stream).ok);
}

// ---------------------------------------------------------------------------
// T11-E 写路:发送前压力与裁决
// ---------------------------------------------------------------------------

TEST_CASE("T11-E 压力: 三种裁决落账,数字可对账,不带累计用量") {
    EnvGuard guard("LUBANCODE_TRAJECTORY_V3_NEW_SESSIONS", "1");
    const auto root = FreshRoot("pressure");
    auto ledger = TrajectorySessionLedger::Open(LedgerOptions(root));
    REQUIRE(ledger.has_value());
    const fs::path stream = V3StreamOf(*ledger);

    {
        auto bridge = ledger->NewTurnBridge({"moonshot", "openai-chat-completions", "terminal"});
        REQUIRE(bridge != nullptr);
        bridge->BeginTurn("turn-1", "external_user");
        bridge->RecordInput(UserMessage("大活"));
        agent::RequestPreparedContext ctx;
        ctx.purpose = accounting::RequestPurpose::MainTurn;
        api::Request request;
        request.model = "kimi-k2.6";
        request.system = "SYSTEM-X";
        request.messages.push_back(UserMessage("大活"));
        const std::string request_id = bridge->OnRequestPrepared(request, ctx);
        REQUIRE_FALSE(request_id.empty());
        REQUIRE(bridge->OnRequestSent(request_id));
        api::Usage usage;
        usage.input_tokens = 2048;
        usage.output_tokens = 64;
        bridge->OnUsageRecorded(request_id, usage, true, "resp-1");
        api::Message assistant;
        assistant.role = api::Role::Assistant;
        assistant.content.push_back(api::TextBlock{"收到。"});
        REQUIRE(bridge->OnOutputCompleted(request_id, assistant, "end_turn", "resp-1"));

        // 三种裁决都在回合内落(发送前判定发生在回合开着的窗口里)。
        // 应急收窄放行(reserve_clamped)。
        agent::ContextPressure clamped;
        clamped.phase = agent::ContextPressure::Phase::PreflightExceeded;
        clamped.estimated_input_tokens = 1000;
        clamped.reserved_output_tokens = 256;
        clamped.protocol_headroom_tokens = 32;
        clamped.window_tokens = 1280;
        clamped.reserve_clamped = true;
        bridge->OnContextPressure(clamped);
        // 拒发(exceeded_denied)。
        agent::ContextPressure denied;
        denied.phase = agent::ContextPressure::Phase::PreflightExceeded;
        denied.estimated_input_tokens = 1200;
        denied.reserved_output_tokens = 64;
        denied.protocol_headroom_tokens = 32;
        denied.window_tokens = 1280;
        bridge->OnContextPressure(denied);
        // 优雅降档(max_tokens_degraded)。
        agent::ContextPressure degraded;
        degraded.phase = agent::ContextPressure::Phase::PreflightDegraded;
        degraded.estimated_input_tokens = 1100;
        degraded.reserved_output_tokens = 200;
        degraded.protocol_headroom_tokens = 32;
        degraded.window_tokens = 1280;
        bridge->OnContextPressure(degraded);
        bridge->EndTurn(true, false, "");
    }

    const auto rows = ReadLines(stream);
    const auto pressure = RowsOfKind(rows, "context.pressure.recorded");
    REQUIRE(pressure.size() == 3);
    CHECK(pressure[0]->at("payload").value("verdict", std::string()) == "reserve_clamped");
    CHECK(pressure[1]->at("payload").value("verdict", std::string()) == "exceeded_denied");
    CHECK(pressure[2]->at("payload").value("verdict", std::string()) == "max_tokens_degraded");
    // 数字账对表:clamped 的 1000+256+32=1288 已超窗 1280 → 余量钳 0;
    // denied 的 1200+64+32=1296 同超窗 → 钳 0(不落无符号下溢)。
    CHECK(pressure[0]->at("payload").value("remainingTokens", std::uint64_t{99}) == 0);
    CHECK(pressure[1]->at("payload").value("remainingTokens", std::uint64_t{99}) == 0);
    CHECK(pressure[0]->value("turnId", std::string()) == "turn-1");
    // 不复制第二份累计用量:pressure 行无 usage 键;用量 owner 在 assistant。
    for (const auto* row : pressure) {
        CHECK_FALSE(row->at("payload").contains("usage"));
    }
    int assistant_usage_rows = 0;
    for (const auto& row : rows) {
        if (row.value("type", std::string()) == "message" &&
            row.at("message").value("role", std::string()) == "assistant" && row.contains("usage")) {
            ++assistant_usage_rows;
        }
    }
    CHECK(assistant_usage_rows == 1);
    CHECK(lubancode::trajectory::v3::VerifyV3File(stream).ok);

    // 读面:逐枚裁决可对账。
    const auto ledger_data = lubancode::trajectory::v3::ReadV3Ledger(stream);
    REQUIRE(ledger_data.has_value());
    const auto facts = lubancode::trajectory::v3::FoldPressureFacts(*ledger_data);
    REQUIRE(facts.size() == 3);
    CHECK(facts[0].verdict == "reserve_clamped");
    CHECK(facts[0].window_tokens == 1280);
    CHECK(facts[0].estimated_input_tokens == 1000);
    CHECK(facts[2].verdict == "max_tokens_degraded");
    CHECK(facts[2].turn_id.has_value());
}
