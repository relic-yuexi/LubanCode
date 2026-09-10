// 轨迹 v3 收尾棒:/verify 与 /doctor 的 v3 子账树遍历。
//   - VerifySession(runtime)与 VerifySessionDir(CLI 引擎)对 v3 场:主账
//     v3 卷整卷验链 + WalkSessionTree 递归 subagents/<child>/<child>.jsonl,
//     streams/child_edges 与 v2 同形状;linked 边过、终态如实;
//   - 孤儿边(父账声明派发、子卷缺失)明报 edge.child_stream_missing;
//   - linked 但子账首行 spawnEventRef 五键对不上 → edge.spawn_ref_mismatch;
//   - BuildSessionDoctorReport(/doctor trajectory)认 v3 场:main(v3) +
//     agent:<child> 两笔健康账,未收口数如实。
#include <doctest/doctest.h>

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "agent/loop.hpp"
#include "agent/tool_trace.hpp"
#include "api/types.hpp"
#include "platform/paths.hpp"
#include "runtime/trajectory_session.hpp"
#include "trajectory/metrics.hpp"
#include "trajectory/replay.hpp"
#include "trajectory/v3/reader.hpp"
#include "trajectory/v3/writer.hpp"
#include "workspace/identity.hpp"

namespace platform = lubancode::platform;
using namespace lubancode;
using lubancode::runtime::TrajectorySessionLedger;

namespace {

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

std::filesystem::path FreshRoot(const char* tag) {
    const auto dir = std::filesystem::temp_directory_path() /
                     ("lubancode-v3-verify-tree-" + std::string(tag));
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    std::filesystem::create_directories(dir, ec);
    return dir;
}

TrajectorySessionLedger::Options LedgerOptions(const std::filesystem::path& root) {
    TrajectorySessionLedger::Options options;
    options.workspaces_root = root / "workspaces";
    options.workspace_root = root / "ws";
    options.workspace_identity = lubancode::workspace::MakeFallbackIdentity(root / "ws");
    options.launch_cwd = "D:/tmp/ws";
    options.lubancode_version = "0.26.238-test";
    options.v3_system_content = "你是 LubanCode,读写跑都走工具。";
    return options;
}

api::Message UserMessage(const std::string& text) {
    api::Message message;
    message.role = api::Role::User;
    message.content.push_back(api::TextBlock{text});
    return message;
}

api::Message AssistantText(const std::string& text) {
    api::Message message;
    message.role = api::Role::Assistant;
    message.content.push_back(api::TextBlock{text});
    return message;
}

api::Message AssistantWithToolCall(const std::string& call_id) {
    api::Message message;
    message.role = api::Role::Assistant;
    message.content.push_back(api::TextBlock{"帮我派个代理。"});
    api::ToolUseBlock call;
    call.id = call_id;
    call.name = "agent";
    call.input = nlohmann::json{{"task", "查文档"}};
    message.content.push_back(std::move(call));
    return message;
}

api::Message ToolResults(const std::string& call_id, const std::string& content) {
    api::Message message;
    message.role = api::Role::User;
    api::ToolResultBlock result;
    result.tool_use_id = call_id;
    result.content = content;
    message.content.push_back(std::move(result));
    return message;
}

api::Request MakeRequest(const std::string& system, const std::vector<api::Message>& messages) {
    api::Request request;
    request.model = "kimi-k2.6";
    request.system = system;
    request.messages = messages;
    return request;
}

agent::RequestPreparedContext PreparedContext() { return agent::RequestPreparedContext{}; }

agent::ToolTraceEvent TraceEvent(agent::ToolTraceEventKind kind, const std::string& call_id) {
    agent::ToolTraceEvent event;
    event.kind = kind;
    event.tool_use_id = call_id;
    event.tool_name = "agent";
    event.execution_id = "exec-" + call_id;
    event.effective_input_sha256 = "aaaa1111aaaa1111aaaa1111aaaa1111aaaa1111aaaa1111aaaa1111aaaa1111";
    event.effective_arguments = nlohmann::json{{"task", "查文档"}};
    return event;
}

std::filesystem::path V3StreamOf(const TrajectorySessionLedger& ledger) {
    return ledger.session_dir() /
           platform::Utf8ToPath(platform::PathToUtf8(ledger.session_dir().filename()) + ".jsonl");
}

}  // namespace

// ---------------------------------------------------------------------------
// 活场端到端:主账 + 子账树全过
// ---------------------------------------------------------------------------

TEST_CASE("v3 VerifySession/VerifySessionDir: 主账与子账树都验,linked 边过") {
    EnvGuard guard("LUBANCODE_TRAJECTORY_V3_NEW_SESSIONS", "1");
    const auto root = FreshRoot("full");
    auto ledger = TrajectorySessionLedger::Open(LedgerOptions(root));
    REQUIRE(ledger.has_value());
    const std::string call_id = "call_prov_verify";
    auto bridge = ledger->NewTurnBridge({"moonshot", "openai-chat-completions", "terminal"});
    REQUIRE(bridge != nullptr);
    bridge->BeginTurn("turn-1", "external_user");
    bridge->RecordInput(UserMessage("帮我派个代理查文档"));
    const std::string request_id = bridge->OnRequestPrepared(
        MakeRequest("你是 LubanCode,读写跑都走工具。", {UserMessage("帮我派个代理查文档")}),
        PreparedContext());
    REQUIRE_FALSE(request_id.empty());
    REQUIRE(bridge->OnOutputCompleted(request_id, AssistantWithToolCall(call_id), "tool_calls",
                                      "resp-a"));
    bridge->OnToolTrace(TraceEvent(agent::ToolTraceEventKind::Scheduled, call_id));
    bridge->OnToolTrace(TraceEvent(agent::ToolTraceEventKind::ExecutionStarted, call_id));

    auto child = ledger->SpawnSubagent(call_id, "查文档里的入口说明", "");
    REQUIRE(child.has_value());
    auto& child_bridge = (*child)->turn_bridge();
    child_bridge.BeginTurn("turn-child-1", "peer_agent");
    const std::string child_request = child_bridge.OnRequestPrepared(
        MakeRequest("你是 LubanCode,读写跑都走工具。", {UserMessage("查文档里的入口说明")}),
        PreparedContext());
    REQUIRE_FALSE(child_request.empty());
    REQUIRE(child_bridge.OnOutputCompleted(child_request, AssistantText("文档说入口在 main.cpp"),
                                           "end_turn", "resp-child"));
    child_bridge.EndTurn(true, false, "");
    const std::string child_hash = (*child)->Finish(/*ok=*/true, "done");
    REQUIRE_FALSE(child_hash.empty());

    {
        agent::ToolTraceEvent finished =
            TraceEvent(agent::ToolTraceEventKind::ExecutionFinished, call_id);
        finished.outcome = agent::ToolOutcome::Succeeded;
        bridge->OnToolTrace(finished);
        bridge->OnToolResultsCommitted("batch-2", ToolResults(call_id, "文档说入口在 main.cpp"));
        bridge->EndTurn(true, false, "");
    }
    CHECK(ledger->CloseSession("exit").error_code.empty());

    // ---- runtime 口(VerifySession):主账 + 子账,边全过。
    const auto report = ledger->VerifySession();
    CHECK(report.ok);
    REQUIRE(report.streams.size() == 2);
    bool main_terminal = false;
    bool child_terminal = false;
    std::string child_relative;
    for (const auto& stream : report.streams) {
        if (stream.run_kind == trajectory::RunKind::MainSession) {
            main_terminal = stream.run_terminal;
            CHECK(stream.ok);
            CHECK(stream.events > 0);
        } else {
            child_terminal = stream.run_terminal;
            child_relative = stream.relative_path;
            CHECK(stream.ok);
            CHECK(stream.parent_run_id == report.streams[0].run_id);
        }
    }
    CHECK(main_terminal);   // CloseSession 落了 session.ended
    CHECK(child_terminal);  // Finish 落了 session.ended
    CHECK(child_relative.find("subagents/") == 0);
    REQUIRE(report.child_edges.size() == 1);
    CHECK(report.child_edges[0].error_code.empty());
    CHECK(report.child_edges[0].spawn_reference_found);
    CHECK(report.child_edges[0].accepted_once);

    // ---- CLI 引擎同口径(trajectory verify 的引擎体)。
    const auto dir_report = trajectory::VerifySessionDir(ledger->session_dir());
    CHECK(dir_report.ok);
    CHECK(dir_report.streams.size() == 2);
    CHECK(dir_report.child_edges.size() == 1);
    CHECK(dir_report.child_edges[0].error_code.empty());

    // ---- /doctor trajectory 的账面折叠:main(v3) + agent:<child>。
    const auto doctor = trajectory::BuildSessionDoctorReport(ledger->session_dir());
    REQUIRE(doctor.streams.size() == 2);
    bool saw_v3_main = false;
    bool saw_agent = false;
    for (const auto& stream : doctor.streams) {
        if (stream.label == "main(v3)") {
            saw_v3_main = true;
            CHECK(stream.exists);
            CHECK(stream.verify.ok);
            CHECK(stream.verify.events > 0);
            CHECK(stream.run_terminal);
        } else if (stream.label.rfind("agent:", 0) == 0) {
            saw_agent = true;
            CHECK(stream.exists);
            CHECK(stream.verify.ok);
            CHECK(stream.run_terminal);
        }
    }
    CHECK(saw_v3_main);
    CHECK(saw_agent);
    CHECK(doctor.unterminated_stream_count == 0);
}

// ---------------------------------------------------------------------------
// 手搓账:孤儿边与五键 mismatch(verify 的明报路)
// ---------------------------------------------------------------------------

namespace {

// 搓一间 v3 场目录:sessions/<id>/<id>.jsonl(首行 system + session.started
// 由 V3Writer::Start 落),再按需补 spawn/linked 事件。
trajectory::v3::V3Writer CraftParent(const std::filesystem::path& sessions_root,
                                     const std::string& session_id) {
    const auto dir = sessions_root / platform::Utf8ToPath(session_id);
    std::filesystem::create_directories(dir);
    auto writer = trajectory::v3::V3Writer::Start(
        dir / platform::Utf8ToPath(session_id + ".jsonl"), session_id,
        "run-" + session_id, "SYSTEM-CRAFT");
    REQUIRE(writer.has_value());
    return std::move(*writer);
}

void AppendSpawnRequested(trajectory::v3::V3Writer& parent, const std::string& action_id,
                          const std::string& task_id, const std::string& child_session,
                          const std::string& child_journal_path) {
    trajectory::v3::EventDraft spawn;
    spawn.kind = trajectory::v3::EventKindV3::SubagentSpawnRequested;
    spawn.action_id = action_id;
    spawn.task_id = task_id;
    spawn.payload = nlohmann::json{
        {"taskId", task_id},
        {"childSessionRef",
         nlohmann::json{{"sessionId", child_session},
                        {"runId", "run-" + child_session},
                        {"journalPath", child_journal_path}}},
        {"attempt", 1},
        {"parentActionRef",
         nlohmann::json{{"sessionId", parent.session_id()},
                        {"runId", parent.run_id()},
                        {"turnId", "turn-1"},
                        {"stepId", "step-1"},
                        {"actionId", action_id},
                        {"declaredMessageRef", ""}}},
        {"taskArgs", nlohmann::json{{"taskLabel", "手工账"}}},
        {"configSnapshot", nlohmann::json::object()},
        {"asyncStart", false}};
    const auto receipt = parent.AppendEvent(std::move(spawn), trajectory::Durability::PowerLoss);
    REQUIRE(receipt.status == trajectory::v3::WriteReceipt::Status::Committed);
}

}  // namespace

TEST_CASE("v3 VerifySessionDir: 孤儿边明报 edge.child_stream_missing") {
    const auto root = FreshRoot("orphan");
    const auto sessions_root = root / "sessions";
    auto parent = CraftParent(sessions_root, "p-orphan");
    AppendSpawnRequested(parent, "action-1", "task-1", "c-missing",
                         "subagents/c-missing/c-missing.jsonl");

    const auto report = trajectory::VerifySessionDir(sessions_root / "p-orphan");
    CHECK_FALSE(report.ok);
    REQUIRE(report.child_edges.size() == 1);
    CHECK(report.child_edges[0].child_run_id == "run-c-missing");
    CHECK_FALSE(report.child_edges[0].child_stream_found);
    CHECK(report.child_edges[0].error_code == "edge.child_stream_missing");
    // 主账自身是好的:streams[0] 过,坏在边。
    REQUIRE_FALSE(report.streams.empty());
    CHECK(report.streams[0].ok);
}

TEST_CASE("v3 VerifySessionDir: linked 但五键对不上,明报 edge.spawn_ref_mismatch") {
    const auto root = FreshRoot("mismatch");
    const auto sessions_root = root / "sessions";
    auto parent = CraftParent(sessions_root, "p-mismatch");
    AppendSpawnRequested(parent, "action-1", "task-1", "c-bad-ref",
                         "subagents/c-bad-ref/c-bad-ref.jsonl");
    // 子账:落在父场目录的 subagents/ 下(WalkSessionTree 按父账目录解析
    // journalPath 相对路径),首行 system 的 spawnEventRef 五键给错 hash。
    const auto child_dir = sessions_root / "p-mismatch" / "subagents" / "c-bad-ref";
    std::filesystem::create_directories(child_dir);
    nlohmann::json spawn_ref = nlohmann::json{{"sessionId", std::string("p-mismatch")},
                                              {"runId", std::string("run-p-mismatch")},
                                              {"seq", 3},
                                              {"id", std::string("evt-spawn")},
                                              {"hash", std::string(64, 'f') /*错的*/}};
    nlohmann::json system_extra = nlohmann::json{{"cause", "subagent_spawn"},
                                                 {"spawnEventRef", std::move(spawn_ref)},
                                                 {"parentActionRef", nlohmann::json::object()},
                                                 {"taskId", "task-1"}};
    auto child = trajectory::v3::V3Writer::Start(
        child_dir / "c-bad-ref.jsonl", "c-bad-ref", "run-c-bad-ref", "SYSTEM-CHILD",
        std::move(system_extra));
    REQUIRE(child.has_value());
    // 父账 subagent.linked(检查点指子账末行)。
    const std::string child_last_hash = child->last_line_hash();
    trajectory::v3::EventDraft linked;
    linked.kind = trajectory::v3::EventKindV3::SubagentLinked;
    linked.status = trajectory::v3::OpStatus::Done;
    linked.action_id = "action-1";
    linked.task_id = "task-1";
    linked.payload = nlohmann::json{
        {"taskId", "task-1"},
        {"childCheckpointRef",
         nlohmann::json{{"sessionId", "c-bad-ref"},
                        {"runId", "run-c-bad-ref"},
                        {"seq", child->next_seq() - 1},
                        {"lineHash", child_last_hash}}}};
    const auto linked_receipt =
        parent.AppendEvent(std::move(linked), trajectory::Durability::PowerLoss);
    REQUIRE(linked_receipt.status == trajectory::v3::WriteReceipt::Status::Committed);

    const auto report = trajectory::VerifySessionDir(sessions_root / "p-mismatch");
    CHECK_FALSE(report.ok);
    REQUIRE(report.child_edges.size() == 1);
    CHECK(report.child_edges[0].child_stream_found);
    CHECK(report.child_edges[0].error_code == "edge.spawn_ref_mismatch");
}

TEST_CASE("v2 场目录不走 v3 引擎(回归钉): main.jsonl 在即 v2 布局") {
    // 只验分派:有 main.jsonl 的目录不会被误认 v3。空目录两头都不是,
    // 按 v2 老路报 verify.no_streams,不进 v3 引擎。
    const auto root = FreshRoot("dispatch");
    const auto session_dir = root / "sessions" / "p-v2";
    std::filesystem::create_directories(session_dir);
    const auto report = trajectory::VerifySessionDir(session_dir);
    CHECK_FALSE(report.ok);
    CHECK(report.error_code == "verify.no_streams");  // v2 老路的码,不是 verify.no_v3_stream
}
