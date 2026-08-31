// P0-4 环境与证据的接线冒烟(§9.3/§5.5/§12.2):真 recorder + 临时目录,
// 一轮里把 side-effect 细账(file pre/post hash、command argv/exit、mcp
// 身份)、verification.recorded → 改文件 → invalidated → outcome.assessed
// 引 fresh evidence、排队账(control.queue.item.*)与环境快照
// (run.environment.captured)整链走通,末尾验账。
#include <doctest/doctest.h>

#include <algorithm>
#include <filesystem>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "agent/loop.hpp"  // RequestPreparedContext(合并缝合:P0-4 测试补 A1 新增的 ctx 参)
#include "agent/tool_trace.hpp"
#include "api/types.hpp"
#include "runtime/trajectory_session.hpp"
#include "trajectory/blob_store.hpp"
#include "trajectory/journal.hpp"
#include "trajectory/recorder.hpp"
#include "trajectory/replay.hpp"

using namespace lubancode;
using namespace lubancode::runtime;
using trajectory::Actor;
using trajectory::Durability;
using trajectory::EventKind;
using trajectory::RecordReceipt;

namespace {

std::filesystem::path FreshDir(const std::string& name) {
    const auto dir = std::filesystem::temp_directory_path() / name;
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    std::filesystem::create_directories(dir, ec);
    return dir;
}

trajectory::EventScope MainScope() {
    trajectory::EventScope scope;
    scope.workspace_key = "demo-000000000000";
    scope.session_id = "20260831-000001-AAAAAA";
    scope.run_id = "main-0001";
    scope.run_kind = trajectory::RunKind::MainSession;
    scope.visibility = {trajectory::Visibility::HostOnly};
    return scope;
}

api::Message UserMessage(const std::string& text) {
    api::Message message;
    message.role = api::Role::User;
    message.content.push_back(api::TextBlock{text});
    return message;
}

api::Message AssistantWithToolCall(const std::string& call_id, const std::string& tool) {
    api::Message message;
    message.role = api::Role::Assistant;
    message.content.push_back(api::TextBlock{"干活。"});
    api::ToolUseBlock call;
    call.id = call_id;
    call.name = tool;
    call.input = nlohmann::json{{"path", "src/main.cpp"}};
    message.content.push_back(std::move(call));
    return message;
}

agent::ToolTraceEvent StartedEvent(const std::string& call_id, const std::string& tool,
                                   nlohmann::json effective_arguments,
                                   agent::EffectClass effect_class) {
    agent::ToolTraceEvent event;
    event.kind = agent::ToolTraceEventKind::ExecutionStarted;
    event.execution_id = "item-" + call_id;
    event.tool_use_id = call_id;
    event.tool_name = tool;
    event.batch_id = "batch-1";
    event.sequence_in_batch = 0;
    event.effective_input_sha256 = std::string(64, '0');
    event.effect_class = effect_class;
    event.effective_arguments = std::move(effective_arguments);
    event.timestamp_ms = 1759000000000LL;
    return event;
}

agent::ToolTraceEvent FinishedEvent(const std::string& call_id, const std::string& tool) {
    agent::ToolTraceEvent event;
    event.kind = agent::ToolTraceEventKind::ExecutionFinished;
    event.execution_id = "item-" + call_id;
    event.tool_use_id = call_id;
    event.tool_name = tool;
    event.batch_id = "batch-1";
    event.sequence_in_batch = 0;
    event.outcome = agent::ToolOutcome::Succeeded;
    event.duration_ms = 42;
    event.result_ref.kind = agent::ToolResultRef::Kind::Inline;
    event.result_ref.sha256 = std::string(64, '1');
    event.result_ref.bytes = 12;
    event.timestamp_ms = 1759000000042LL;
    return event;
}

// 从 JSONL 里按 kind 抓第一枚事件的 payload。
nlohmann::json FirstPayloadOf(const std::filesystem::path& stream, const std::string& kind) {
    const auto lines = trajectory::ReadJournalLines(stream);
    REQUIRE(lines.has_value());
    for (const std::string& line : *lines) {
        const auto parsed = nlohmann::json::parse(line, nullptr, false);
        if (parsed.is_discarded() || parsed.value("kind", std::string()) != kind) {
            continue;
        }
        return parsed["payload"];
    }
    return nlohmann::json();
}

std::vector<std::string> KindsOf(const std::filesystem::path& stream) {
    std::vector<std::string> kinds;
    const auto lines = trajectory::ReadJournalLines(stream);
    REQUIRE(lines.has_value());
    for (const std::string& line : *lines) {
        const auto parsed = nlohmann::json::parse(line, nullptr, false);
        kinds.push_back(parsed.is_discarded() ? "<bad>" : parsed.value("kind", std::string()));
    }
    return kinds;
}

}  // namespace

TEST_CASE("side-effect 细账:file 的 pre/post hash 与 undo、command 的 argv/exit、mcp 身份") {
    const auto dir = FreshDir("lubancode-p4-side-effects");
    auto recorder = trajectory::TrajectoryRecorder::Start(
        dir / "main.jsonl", dir / "artifacts", MainScope(),
        [] {
            trajectory::RecorderOptions options;
            options.event_schema_version = 2;
            return options;
        }());
    REQUIRE(recorder.has_value());
    REQUIRE(recorder->WriteRunStarted(nlohmann::json{{"run_kind", "main_session"}},
                                      Durability::PowerLoss)
                .status == RecordReceipt::Status::Committed);
    auto bridge = std::make_unique<TrajectoryTurnBridge>(*recorder, MainScope(),
                                                         TrajectoryTurnBridge::Identity{});
    bridge->BeginTurn("turn-1", "external_user");
    bridge->RecordInput(UserMessage("改文件、跑测试"));

    // ---- 文件工具:write_file 带 undo token ----
    const std::string file_call = "call-file";
    const std::string file_request = bridge->OnRequestPrepared(api::Request{}, agent::RequestPreparedContext{});
    REQUIRE_FALSE(file_request.empty());
    bridge->OnRequestSent(file_request);
    REQUIRE(bridge->OnOutputCompleted(file_request, AssistantWithToolCall(file_call, "write_file"),
                                      "tool_use", "resp-1"));
    bridge->OnToolTrace(StartedEvent(file_call, "write_file",
                                     nlohmann::json{{"path", "src/main.cpp"}, {"content", "int main(){}"}},
                                     agent::EffectClass::LocalReversible));
    agent::ToolTraceEvent file_finished = FinishedEvent(file_call, "write_file");
    file_finished.undo.path = "src/main.cpp";
    file_finished.undo.preimage_sha256 = std::string(64, 'a');
    file_finished.undo.postimage_sha256 = std::string(64, 'b');
    file_finished.undo.created_new_file = false;
    file_finished.undo.preimage = "int main(){return 1;}";
    bridge->OnToolTrace(file_finished);
    // 下一请求前把 result 提交上(§7.4:tool result 记不住,不发下一次
    // 模型请求——状态机的硬门,测试照真路走)。
    api::Message file_result;
    file_result.role = api::Role::User;
    file_result.content.push_back(api::ToolResultBlock{file_call, "已写入。", false});
    bridge->OnToolResultsCommitted("batch-1", file_result);

    // ---- 命令工具:run_command 带 exit code ----
    const std::string cmd_call = "call-cmd";
    const std::string cmd_request = bridge->OnRequestPrepared(api::Request{}, agent::RequestPreparedContext{});
    REQUIRE_FALSE(cmd_request.empty());
    bridge->OnRequestSent(cmd_request);
    REQUIRE(bridge->OnOutputCompleted(cmd_request, AssistantWithToolCall(cmd_call, "run_command"),
                                      "tool_use", "resp-2"));
    bridge->OnToolTrace(StartedEvent(cmd_call, "run_command",
                                     nlohmann::json{{"command", "cmake --build build"},
                                                    {"shell", "powershell"},
                                                    {"timeout_ms", 120000}},
                                     agent::EffectClass::LocalProcessUnknown));
    agent::ToolTraceEvent cmd_finished = FinishedEvent(cmd_call, "run_command");
    cmd_finished.details = nlohmann::json{{"exit_code", 0}};
    bridge->OnToolTrace(cmd_finished);
    api::Message cmd_result;
    cmd_result.role = api::Role::User;
    cmd_result.content.push_back(api::ToolResultBlock{cmd_call, "[退出码 0]", false});
    bridge->OnToolResultsCommitted("batch-1", cmd_result);

    // ---- MCP 调用:server 身份与 jsonrpc id ----
    const std::string mcp_call = "call-mcp";
    const std::string mcp_request = bridge->OnRequestPrepared(api::Request{}, agent::RequestPreparedContext{});
    REQUIRE_FALSE(mcp_request.empty());
    bridge->OnRequestSent(mcp_request);
    REQUIRE(bridge->OnOutputCompleted(mcp_request, AssistantWithToolCall(mcp_call, "mcp_query"),
                                      "tool_use", "resp-3"));
    agent::ToolTraceEvent mcp_started =
        StartedEvent(mcp_call, "mcp_query", nlohmann::json{{"q", "demo"}},
                     agent::EffectClass::RemoteIdempotent);
    mcp_started.source_kind = agent::ToolSourceKind::Mcp;
    mcp_started.source_instance = "demo-server";
    bridge->OnToolTrace(mcp_started);
    agent::ToolTraceEvent mcp_finished = FinishedEvent(mcp_call, "mcp_query");
    mcp_finished.source_kind = agent::ToolSourceKind::Mcp;
    mcp_finished.source_instance = "demo-server";
    mcp_finished.jsonrpc_request_id = 7;
    bridge->OnToolTrace(mcp_finished);
    api::Message mcp_result;
    mcp_result.role = api::Role::User;
    mcp_result.content.push_back(api::ToolResultBlock{mcp_call, "query ok", false});
    bridge->OnToolResultsCommitted("batch-1", mcp_result);

    bridge->EndTurn(true, false, "done");

    // 文件细账:path、pre/post hash、undo_ref 带 preimage(§9.3)。
    const auto file_payload = FirstPayloadOf(dir / "main.jsonl", "tool.execution.finished");
    REQUIRE(file_payload["side_effects"].size() == 1);
    const auto& file_effect = file_payload["side_effects"][0];
    CHECK(file_effect["kind"] == "file");
    CHECK(file_effect["path"] == "src/main.cpp");
    CHECK(file_effect["preimage_sha256"] == std::string(64, 'a'));
    CHECK(file_effect["postimage_sha256"] == std::string(64, 'b'));
    CHECK(file_effect["undo_ref"]["preimage_bytes"] == 21);
    CHECK(file_effect["undo_ref"]["text"] == "int main(){return 1;}");

    // 命令细账:argv/shell/timeout/exit code,合并输出指 result_ref。
    const auto lines = trajectory::ReadJournalLines(dir / "main.jsonl");
    REQUIRE(lines.has_value());
    nlohmann::json cmd_payload;
    for (const std::string& line : *lines) {
        const auto parsed = nlohmann::json::parse(line, nullptr, false);
        if (parsed.is_discarded() || parsed.value("kind", std::string()) != "tool.execution.finished") {
            continue;
        }
        if (parsed["payload"]["side_effects"].size() == 1 &&
            parsed["payload"]["side_effects"][0]["kind"] == "command") {
            cmd_payload = parsed["payload"];
        }
    }
    REQUIRE_FALSE(cmd_payload.is_null());
    CHECK(cmd_payload["exit_code"] == 0);
    CHECK(cmd_payload["side_effects"][0]["command"] == "cmake --build build");
    CHECK(cmd_payload["side_effects"][0]["shell_mode"] == "powershell");
    CHECK(cmd_payload["side_effects"][0]["timeout_ms"] == 120000);
    CHECK(cmd_payload["side_effects"][0]["combined_output_ref"]["sha256"] == std::string(64, '1'));

    // MCP 细账:server 身份与 jsonrpc 关联。
    nlohmann::json mcp_payload;
    for (const std::string& line : *lines) {
        const auto parsed = nlohmann::json::parse(line, nullptr, false);
        if (parsed.is_discarded() || parsed.value("kind", std::string()) != "tool.execution.finished") {
            continue;
        }
        if (parsed["payload"]["side_effects"].size() == 1 &&
            parsed["payload"]["side_effects"][0]["kind"] == "mcp_call") {
            mcp_payload = parsed["payload"];
        }
    }
    REQUIRE_FALSE(mcp_payload.is_null());
    CHECK(mcp_payload["side_effects"][0]["server"] == "demo-server");
    CHECK(mcp_payload["side_effects"][0]["jsonrpc_request_id"] == 7);

    CHECK(trajectory::VerifyJournalFile(dir / "main.jsonl").ok);
}

TEST_CASE("verification 链:recorded → 改文件 invalidated → outcome.assessed 只引 fresh") {
    const auto dir = FreshDir("lubancode-p4-verification");
    auto recorder = trajectory::TrajectoryRecorder::Start(
        dir / "main.jsonl", dir / "artifacts", MainScope(),
        [] {
            trajectory::RecorderOptions options;
            options.event_schema_version = 2;
            return options;
        }());
    REQUIRE(recorder.has_value());
    REQUIRE(recorder->WriteRunStarted(nlohmann::json{{"run_kind", "main_session"}},
                                      Durability::PowerLoss)
                .status == RecordReceipt::Status::Committed);
    auto bridge = std::make_unique<TrajectoryTurnBridge>(*recorder, MainScope(),
                                                         TrajectoryTurnBridge::Identity{});
    bridge->BeginTurn("turn-1", "external_user");
    bridge->RecordInput(UserMessage("跑测试,过就把产物收好"));

    // 两个验证点:一个盯将被打磨的文件,一个盯不动的文件。
    const std::string stale_id =
        bridge->BeginVerification("build_check", "build/src/main.cpp", "run_command");
    REQUIRE_FALSE(stale_id.empty());
    bridge->FinishVerification(stale_id, true, nlohmann::json{{"exit_code", 0}},
                               nlohmann::json{{"command", "cmake --build build"}});

    const std::string fresh_id = bridge->BeginVerification("test_suite", "build/tests.exe", "ctest");
    REQUIRE_FALSE(fresh_id.empty());
    bridge->FinishVerification(fresh_id, true, nlohmann::json{{"passed", 104}, {"failed", 0}});

    // 模型改了 stale 验证盯着的文件:undo token 带 path,终态落稳后宿主
    // 自动落 verification.invalidated(§5.5)。
    const std::string call_id = "call-edit";
    const std::string edit_request = bridge->OnRequestPrepared(api::Request{}, agent::RequestPreparedContext{});
    REQUIRE_FALSE(edit_request.empty());
    bridge->OnRequestSent(edit_request);
    REQUIRE(bridge->OnOutputCompleted(edit_request, AssistantWithToolCall(call_id, "edit_file"),
                                      "tool_use", "resp-1"));
    bridge->OnToolTrace(StartedEvent(call_id, "edit_file",
                                     nlohmann::json{{"path", "build/src/main.cpp"}},
                                     agent::EffectClass::LocalReversible));
    agent::ToolTraceEvent finished = FinishedEvent(call_id, "edit_file");
    finished.undo.path = "build/src/main.cpp";
    finished.undo.preimage_sha256 = std::string(64, 'c');
    finished.undo.postimage_sha256 = std::string(64, 'd');
    bridge->OnToolTrace(finished);

    bridge->EndTurn(true, false, "done");

    const auto kinds = KindsOf(dir / "main.jsonl");
    CHECK(std::find(kinds.begin(), kinds.end(), "verification.started") != kinds.end());
    CHECK(std::find(kinds.begin(), kinds.end(), "verification.recorded") != kinds.end());

    // invalidated 只打在 stale 那枚上,reason/引事件齐全。
    const auto invalidated = FirstPayloadOf(dir / "main.jsonl", "verification.invalidated");
    REQUIRE_FALSE(invalidated.is_null());
    CHECK(invalidated["verification_id"] == stale_id);
    CHECK(invalidated["reason"] == "subject_modified");
    CHECK_FALSE(invalidated["invalidated_by_event"].get<std::string>().empty());

    // outcome.assessed 引 fresh evidence:stale 那枚不在引用表里。
    const auto assessed = FirstPayloadOf(dir / "main.jsonl", "outcome.assessed");
    REQUIRE_FALSE(assessed.is_null());
    CHECK(assessed["outcome"] == "succeeded");
    REQUIRE(assessed["evidence_refs"].size() == 1);
    CHECK(assessed["evidence_refs"][0]["verification_id"] == fresh_id);
    CHECK(assessed["evidence_refs"][0]["fresh"] == true);
    CHECK(assessed["criteria"][0] == "test_suite");

    CHECK(trajectory::VerifyJournalFile(dir / "main.jsonl").ok);

    // 折叠侧:evidence 账三枚(两 recorded + 一 invalidated),fresh 只一枚。
    const auto fold = trajectory::FoldStreamReplay(dir / "main.jsonl");
    REQUIRE(fold.ok());
    REQUIRE(fold.state.evidence.size() == 2);
    int invalidated_count = 0;
    for (const auto& entry : fold.state.evidence) {
        if (entry.invalidated) {
            ++invalidated_count;
        }
    }
    CHECK(invalidated_count == 1);
}

TEST_CASE("没验过的 turn 不落 outcome.assessed(§11.5 成功门的账面前提)") {
    const auto dir = FreshDir("lubancode-p4-no-evidence");
    auto recorder = trajectory::TrajectoryRecorder::Start(
        dir / "main.jsonl", dir / "artifacts", MainScope(),
        [] {
            trajectory::RecorderOptions options;
            options.event_schema_version = 2;
            return options;
        }());
    REQUIRE(recorder.has_value());
    REQUIRE(recorder->WriteRunStarted(nlohmann::json{{"run_kind", "main_session"}},
                                      Durability::PowerLoss)
                .status == RecordReceipt::Status::Committed);
    auto bridge = std::make_unique<TrajectoryTurnBridge>(*recorder, MainScope(),
                                                         TrajectoryTurnBridge::Identity{});
    bridge->BeginTurn("turn-1", "external_user");
    bridge->RecordInput(UserMessage("聊两句"));
    const std::string request_id = bridge->OnRequestPrepared(api::Request{}, agent::RequestPreparedContext{});
    REQUIRE_FALSE(request_id.empty());
    bridge->OnRequestSent(request_id);
    REQUIRE(bridge->OnOutputCompleted(request_id, UserMessage("好。"), "end_turn", "resp-1"));
    bridge->EndTurn(true, false, "done");

    const auto kinds = KindsOf(dir / "main.jsonl");
    CHECK(std::find(kinds.begin(), kinds.end(), "outcome.assessed") == kinds.end());
    CHECK(trajectory::VerifyJournalFile(dir / "main.jsonl").ok);
}

TEST_CASE("ledger 排队账:enqueued/dequeued/cancelled/expired 全链进 Journal") {
    const auto root = FreshDir("lubancode-p4-queue");
    TrajectorySessionLedger::Options options;
    options.trajectories_root = root / "trajectories";
    options.workspace_root = root / "repo";
    options.lubancode_version = "test";
    std::error_code ec;
    std::filesystem::create_directories(root / "repo", ec);
    auto ledger = TrajectorySessionLedger::Open(options);
    REQUIRE(ledger.has_value());

    ledger->NoteQueueEnqueued("q-1", "main", "busy_enqueue");
    ledger->NoteQueueEnqueued("q-2", "#3", "busy_enqueue");
    ledger->NoteQueueDequeued("q-1", "tool_boundary_delivery");
    ledger->NoteQueueCancelled("q-2", "session_clear");
    ledger->NoteQueueExpired("q-3", "");  // 未 enqueued 的 expired:状态机拒,不入账

    const auto kinds = KindsOf(ledger->session_dir() / "main.jsonl");
    CHECK(std::count(kinds.begin(), kinds.end(), "control.queue.item.enqueued") == 2);
    CHECK(std::count(kinds.begin(), kinds.end(), "control.queue.item.dequeued") == 1);
    CHECK(std::count(kinds.begin(), kinds.end(), "control.queue.item.cancelled") == 1);
    CHECK(std::count(kinds.begin(), kinds.end(), "control.queue.item.expired") == 0);
    CHECK(trajectory::VerifyJournalFile(ledger->session_dir() / "main.jsonl").ok);

    // 折叠侧:open queue 清干净(两枚都终态)。
    const auto fold = trajectory::FoldStreamReplay(ledger->session_dir() / "main.jsonl");
    REQUIRE(fold.ok());
    CHECK(fold.state.control.open_queue_items.empty());
}

TEST_CASE("ledger 环境快照:非 git 仓如实降档,字段全账可回读") {
    const auto root = FreshDir("lubancode-p4-environment");
    TrajectorySessionLedger::Options options;
    options.trajectories_root = root / "trajectories";
    options.workspace_root = root / "repo";  // 非 git 仓:in_repo=false 如实记
    options.lubancode_version = "0.26.148-test";
    std::error_code ec;
    std::filesystem::create_directories(root / "repo", ec);
    auto ledger = TrajectorySessionLedger::Open(options);
    REQUIRE(ledger.has_value());

    TrajectorySessionLedger::EnvironmentFacts facts;
    facts.provider = "demo";
    facts.wire = "responses";
    facts.model = "demo-large";
    facts.system_prompt = "你是 LubanCode。";
    facts.toolset.toolset_sha256 = std::string(64, 'e');
    facts.toolset.tool_count = 42;
    facts.project_instruction_refs = {"AGENTS.md"};
    facts.config_snapshot_redacted = nlohmann::json{{"language", "zh"}};
    CHECK(ledger->CaptureEnvironment(facts).empty());
    // 幂等:第二次是 no-op。
    CHECK(ledger->CaptureEnvironment(facts).empty());

    const auto kinds = KindsOf(ledger->session_dir() / "main.jsonl");
    CHECK(std::count(kinds.begin(), kinds.end(), "run.environment.captured") == 1);

    // 事件 payload 三键(snapshot_ref/replay_level/gaps,schema 钉死)。
    const auto payload = FirstPayloadOf(ledger->session_dir() / "main.jsonl",
                                        "run.environment.captured");
    REQUIRE(payload.contains("snapshot_ref"));
    CHECK(payload["replay_level"] == "input_only");  // 非 git 仓:source 轴缺
    const auto gaps = payload["gaps"];
    CHECK(std::find(gaps.begin(), gaps.end(), "not_a_git_repository") != gaps.end());

    // 快照 blob 落在 artifacts/,读得回、字段齐。
    const auto snapshot_ref = trajectory::BlobRef::FromJson(payload["snapshot_ref"]);
    REQUIRE(snapshot_ref.has_value());
    trajectory::BlobStore blobs(ledger->session_dir() / "artifacts");
    const auto snapshot = blobs.ReadVerified(*snapshot_ref);
    REQUIRE(snapshot.has_value());
    const auto parsed = nlohmann::json::parse(*snapshot, nullptr, false);
    REQUIRE_FALSE(parsed.is_discarded());
    CHECK(parsed["lubancode_version"] == "0.26.148-test");
    CHECK(parsed["provider"] == "demo");
    CHECK(parsed["model"] == "demo-large");
    CHECK(parsed["toolset"]["tool_count"] == 42);
    CHECK(parsed.contains("system_prompt_ref"));
    CHECK(parsed["git"]["in_repo"] == false);
    CHECK(parsed["env_allowlist"].is_object());
    CHECK(trajectory::VerifyJournalFile(ledger->session_dir() / "main.jsonl").ok);
}
