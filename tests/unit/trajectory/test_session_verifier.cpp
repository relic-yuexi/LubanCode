// session verifier 测试(P0-3 §十四 verify/§3.9 五条/§16.4):
//   - 扫目录:main/subagents 各 stream 逐文件验链;
//   - 父子边交叉核:child owner 对上、spawn 引用存在、父接受时 child 已
//     有终态、child final hash 对上(父侧留空的后台派工由 verifier 实读
//     子文件回填核对——P0-2 遗留#5)、同一 child 至多接受一次;
//   - 坏边明报:child 文件缺、hash 不合、无父派发引用。
#include <doctest/doctest.h>

#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "trajectory/journal.hpp"
#include "trajectory/recorder.hpp"
#include "trajectory/replay.hpp"
#include "trajectory/session_manager.hpp"

using namespace lubancode::trajectory;

namespace {

struct FakeClock : SessionManagerClock {
    std::int64_t wall = 1759000000000LL;
    mutable int random_calls = 0;
    std::int64_t WallMs() const override { return wall; }
    std::int64_t MonotonicNs() const override { return 7000LL + random_calls; }
    std::string Random6() const override {
        ++random_calls;
        char buffer[8];
        std::snprintf(buffer, sizeof(buffer), "V%05d", random_calls);
        return buffer;
    }
};

std::filesystem::path MakeRoot(const char* tag) {
    const std::filesystem::path root = std::filesystem::temp_directory_path() /
                                       ("lubancode-traj-verify-" + std::string(tag));
    std::error_code ec;
    std::filesystem::remove_all(root, ec);
    std::filesystem::create_directories(root, ec);
    return root;
}

SessionManagerOptions Opts(const std::filesystem::path& root) {
    SessionManagerOptions options;
    options.trajectories_root = root / "trajectories";
    options.workspace_root = root / "ws";
    options.launch_cwd = "D:/tmp/ws";
    options.lubancode_version = "0.26.138-test";
    return options;
}

RecordReceipt Put(TrajectoryRecorder& recorder, EventKind kind, EventScope scope,
                  nlohmann::json payload, EventLinks links = {},
                  Durability durability = Durability::ProcessCrash) {
    RecordRequest request;
    request.kind = kind;
    request.scope = std::move(scope);
    request.payload = std::move(payload);
    request.links = std::move(links);
    return recorder.Record(std::move(request), durability);
}

// 父账里一道"派子代理"的工具调用:模型问答回合声明 call → planned →
// effective → started(relations.child_run_id)→ terminal(result_ref.
// child_terminal_event_hash)→ result。accept=true 落终态与 hash;
// accept=false 模拟后台派工(started 后不落终态,hash 留空)。
void WriteParentDispatch(TrajectoryRecorder& main, const std::string& call_id,
                         const std::string& child_run_id, const std::string& child_terminal_hash,
                         bool accept) {
    // 模型回合:planned 引用 output 声明过的 call_id(§6.1)。
    EventScope model = main.base_scope();
    model.turn_id = "turn-0001";
    model.request_id = "req-" + call_id;
    model.actor = Actor::Model;
    model.origin = Origin::ProviderModel;
    const auto prepared =
        Put(main, EventKind::ModelRequestPrepared, model,
            nlohmann::json{{"model", "demo-model"},
                           {"provider", "demo"},
                           {"wire", "responses"},
                           {"message_refs", nlohmann::json::array({"evt-00000002"})}});
    REQUIRE(prepared.status == RecordReceipt::Status::Committed);
    REQUIRE(Put(main, EventKind::ModelRequestSent, model,
                nlohmann::json{{"prepared_event_id", prepared.event_id}})
                .status == RecordReceipt::Status::Committed);
    REQUIRE(Put(main, EventKind::ModelOutputCompleted, model,
                nlohmann::json{{"output_id", "output-" + call_id},
                               {"blocks",
                                nlohmann::json::array({nlohmann::json{
                                    {"type", "tool_call"}, {"call_id", call_id},
                                    {"name", "agent"},
                                    {"arguments", nlohmann::json{{"prompt", "干活"}}}}})},
                               {"stop_reason", "tool_use"}})
                .status == RecordReceipt::Status::Committed);
    EventScope tool = main.base_scope();
    tool.turn_id = "turn-0001";
    tool.request_id = "req-" + call_id;
    tool.call_id = call_id;
    tool.actor = Actor::Tool;
    tool.origin = Origin::SubagentTool;
    REQUIRE(Put(main, EventKind::ToolExecutionPlanned, tool,
                nlohmann::json{{"call_id", call_id}, {"tool_name", "agent"}})
                .status == RecordReceipt::Status::Committed);
    REQUIRE(Put(main, EventKind::ToolInputEffective, tool,
                nlohmann::json{{"call_id", call_id},
                               {"tool_name", "agent"},
                               {"source_kind", "builtin"},
                               {"effect_class", "spawn_run"},
                               {"effective_arguments", nlohmann::json::object()},
                               {"effective_arguments_sha256", std::string(64, 'b')}})
                .status == RecordReceipt::Status::Committed);
    EventLinks child_link;
    child_link.child_run_id = child_run_id;
    REQUIRE(Put(main, EventKind::ToolExecutionStarted, tool,
                nlohmann::json{{"call_id", call_id}}, child_link, Durability::PowerLoss)
                .status == RecordReceipt::Status::Committed);
    if (!accept) {
        return;  // 后台派工:父侧先把 started durable,终态等子账收口后再报
    }
    nlohmann::json payload{{"outcome", "succeeded"},
                           {"duration_ms", 5},
                           {"result_ref",
                            nlohmann::json{{"kind", "child_stream"},
                                           {"child_run_id", child_run_id},
                                           {"child_terminal_event_hash", child_terminal_hash}}},
                           {"side_effects", nlohmann::json::array()}};
    REQUIRE(Put(main, EventKind::ToolExecutionFinished, tool, std::move(payload), child_link,
                Durability::PowerLoss)
                .status == RecordReceipt::Status::Committed);
    REQUIRE(Put(main, EventKind::ToolResultCommitted, tool,
                nlohmann::json{{"call_id", call_id},
                               {"content", nlohmann::json::array({nlohmann::json{
                                   {"type", "text"}, {"text", "子代理收口"}}})},
                               {"is_error", false}})
                .status == RecordReceipt::Status::Committed);
}

// 开一只子账:run.started(relations.parent_run_id;parent_call_id 非空 =
// 前台派工)+ 可选 terminal,回终态事件 hash。
std::string WriteChildStream(ActiveSession& session, const std::string& child_run_id,
                             const std::string& parent_run_id, bool with_terminal,
                             const std::string& parent_call_id = std::string()) {
    auto stream = session.directory.ReserveSubagentStream(child_run_id);
    REQUIRE(stream.has_value());
    EventScope scope = session.main->base_scope();
    scope.run_id = child_run_id;
    scope.run_kind = RunKind::Subagent;
    auto child = TrajectoryRecorder::Start(*stream, session.directory.artifacts_root(), scope);
    REQUIRE(child.has_value());
    EventLinks links;
    links.parent_run_id = parent_run_id;
    if (!parent_call_id.empty()) {
        links.parent_call_id = parent_call_id;
    }
    nlohmann::json payload{{"run_kind", "subagent"},
                           {"agent_run_id", child_run_id},
                           {"owner_run_id", parent_run_id},
                           {"start_reason", "agent_tool_dispatch"}};
    REQUIRE(child->WriteRunStarted(std::move(payload), Durability::PowerLoss, std::move(links)).status ==
            RecordReceipt::Status::Committed);
    std::string terminal_hash;
    if (with_terminal) {
        const auto receipt = child->FinishRun(EventKind::RunCompleted, "task done", Durability::PowerLoss);
        REQUIRE(receipt.status == RecordReceipt::Status::Committed);
        terminal_hash = receipt.event_hash;
    }
    REQUIRE(child->Close().has_value());
    return terminal_hash;
}

struct ParentFixture {
    FakeClock clock;
    std::unique_ptr<SessionManager> manager;
    ActiveSession* active = nullptr;

    explicit ParentFixture(const char* tag) {
        manager = std::make_unique<SessionManager>(Opts(MakeRoot(tag)), &clock);
        active = manager->LaunchSession().value_or(nullptr);
        REQUIRE(active != nullptr);
        // 一场 turn 的前半:turn.started + input。
        EventScope turn = active->main->base_scope();
        turn.turn_id = "turn-0001";
        turn.actor = Actor::User;
        turn.origin = Origin::ExternalUser;
        REQUIRE(Put(*active->main, EventKind::TurnStarted, turn,
                    nlohmann::json{{"trigger", "external_user"}})
                    .status == RecordReceipt::Status::Committed);
        REQUIRE(Put(*active->main, EventKind::InputReceived, turn,
                    nlohmann::json{{"input_id", "input-0001"},
                                   {"content", nlohmann::json::array({"派个帮手"})},
                                   {"channel", "terminal"},
                                   {"sender", nlohmann::json{{"kind", "local_user"}}}})
                    .status == RecordReceipt::Status::Committed);
    }
};

}  // namespace

TEST_CASE("好账: main + 前台子代理,父子边全对") {
    ParentFixture fixture("good");
    ActiveSession& session = *fixture.active;
    const std::string child_hash =
        WriteChildStream(session, "agent-0001", session.manifest.main_run_id, /*with_terminal=*/true,
                         /*parent_call_id=*/"call-0001");
    WriteParentDispatch(*session.main, "call-0001", "agent-0001", child_hash, /*accept=*/true);

    const auto report = VerifySessionDir(session.session_dir());
    CHECK(report.ok);
    REQUIRE(report.streams.size() == 2);  // main + subagents/agent-0001
    for (const auto& stream : report.streams) {
        CHECK(stream.ok);
    }
    REQUIRE(report.child_edges.size() == 1);
    const ChildEdgeReport& edge = report.child_edges[0];
    CHECK(edge.child_run_id == "agent-0001");
    CHECK(edge.parent_run_id == session.manifest.main_run_id);
    CHECK_FALSE(edge.background_spawn);  // 带 parent_call_id = 前台派工
    CHECK(edge.owner_matches);
    CHECK(edge.spawn_reference_found);
    CHECK(edge.child_has_terminal);
    CHECK(edge.parent_recorded_hash == child_hash);
    CHECK(edge.child_terminal_hash == child_hash);
    CHECK(edge.hash_matches);  // 父记 hash == 子文件实读
    CHECK(edge.accepted_once);
    CHECK(edge.error_code.empty());
}

TEST_CASE("后台派工: 父侧 hash 留空,verifier 实读子文件回填核对(遗留#5)") {
    ParentFixture fixture("background");
    ActiveSession& session = *fixture.active;
    // 子账先收口;父侧只有 started(后台派工,终态由后台回流补,此刻未落)。
    WriteChildStream(session, "agent-bg-1", session.manifest.main_run_id, /*with_terminal=*/true);
    WriteParentDispatch(*session.main, "call-0001", "agent-bg-1", /*child_terminal_hash=*/"",
                        /*accept=*/false);

    const auto report = VerifySessionDir(session.session_dir());
    // 父 main 未收口(活账)不判坏——verify 只验链与边;这里 main 链完好。
    REQUIRE(report.child_edges.size() == 1);
    const ChildEdgeReport& edge = report.child_edges[0];
    CHECK(edge.child_has_terminal);
    CHECK(edge.child_terminal_hash.size() == 64);  // 回填:子文件实读
    CHECK(edge.parent_recorded_hash.empty());      // 父侧留空(后台)
    CHECK(edge.hash_matches);  // 后台口径:核"子账有终态"即过
    CHECK(edge.owner_matches);
    CHECK(edge.spawn_reference_found);
}

TEST_CASE("坏边: 父侧记错 child 终态 hash") {
    ParentFixture fixture("wronghash");
    ActiveSession& session = *fixture.active;
    const std::string child_hash =
        WriteChildStream(session, "agent-0002", session.manifest.main_run_id, /*with_terminal=*/true,
                         /*parent_call_id=*/"call-0002");
    // 父侧记一枚对不上的 hash(手工伪造:recorder 只管形状,hash 内容归调用方)。
    WriteParentDispatch(*session.main, "call-0002", "agent-0002",
                        std::string(64, 'd') /* 错的 */, /*accept=*/true);

    const auto report = VerifySessionDir(session.session_dir());
    REQUIRE_FALSE(report.ok);
    REQUIRE(report.child_edges.size() == 1);
    CHECK(report.child_edges[0].error_code == "edge.child_hash_mismatch");
    CHECK_FALSE(report.child_edges[0].hash_matches);
}

TEST_CASE("坏边: 子文件缺失(父账声明派发却没有 child stream)") {
    ParentFixture fixture("missing");
    ActiveSession& session = *fixture.active;
    WriteParentDispatch(*session.main, "call-0003", "agent-ghost",
                        std::string(64, 'e'), /*accept=*/true);

    const auto report = VerifySessionDir(session.session_dir());
    REQUIRE_FALSE(report.ok);
    REQUIRE_FALSE(report.child_edges.empty());
    bool found_missing = false;
    for (const auto& edge : report.child_edges) {
        if (edge.child_run_id == "agent-ghost") {
            found_missing = true;
            CHECK_FALSE(edge.child_stream_found);
            CHECK(edge.error_code == "edge.child_stream_missing");
        }
    }
    CHECK(found_missing);
}

TEST_CASE("坏边: 前台子账在,父账没有派发引用(edge.no_parent_dispatch)") {
    ParentFixture fixture("nodispatch");
    ActiveSession& session = *fixture.active;
    // 前台子账(relations 带 parent_call_id)声明 owner 是 main,但 main
    // 从未写过带 child_run_id 的事件——账缺派发事实,明报。
    WriteChildStream(session, "agent-orphan", session.manifest.main_run_id, /*with_terminal=*/true,
                     /*parent_call_id=*/"call-orphan");
    const auto report = VerifySessionDir(session.session_dir());
    REQUIRE_FALSE(report.ok);
    REQUIRE(report.child_edges.size() == 1);
    CHECK(report.child_edges[0].error_code == "edge.no_parent_dispatch");
    CHECK(report.child_edges[0].owner_matches);
    CHECK_FALSE(report.child_edges[0].spawn_reference_found);
}

TEST_CASE("后台子账(relations 无 parent_call_id)不落父侧派发边,不算坏账") {
    ParentFixture fixture("bgshape");
    ActiveSession& session = *fixture.active;
    // 后台形状:子账只有 parent_run_id(父轮收口在先,父账不落边)。
    WriteChildStream(session, "agent-bg-ok", session.manifest.main_run_id,
                     /*with_terminal=*/true);
    const auto report = VerifySessionDir(session.session_dir());
    REQUIRE(report.child_edges.size() == 1);
    const ChildEdgeReport& edge = report.child_edges[0];
    CHECK(edge.background_spawn);
    CHECK(edge.child_has_terminal);
    CHECK(edge.child_terminal_hash.size() == 64);  // 实读回填
    CHECK(edge.error_code.empty());  // 后台无父侧边是合同形状,不是坏账
}

TEST_CASE("坏链: 子账尾行截断,该 stream 明报不影响别的边") {
    ParentFixture fixture("truncated");
    ActiveSession& session = *fixture.active;
    const std::string child_hash =
        WriteChildStream(session, "agent-0004", session.manifest.main_run_id, /*with_terminal=*/true,
                         /*parent_call_id=*/"call-0004");
    WriteParentDispatch(*session.main, "call-0004", "agent-0004", child_hash, /*accept=*/true);
    // 子账截掉尾换行:verify.truncated_tail。
    {
        const auto stream = session.session_dir() / "subagents" / "agent-0004.jsonl";
        std::ifstream file(stream, std::ios::binary);
        std::stringstream buffer;
        buffer << file.rdbuf();
        const std::string text = buffer.str();
        std::ofstream out(stream, std::ios::binary | std::ios::trunc);
        out.write(text.data(), static_cast<std::streamsize>(text.size() - 1));
    }
    const auto report = VerifySessionDir(session.session_dir());
    REQUIRE_FALSE(report.ok);
    bool child_reported = false;
    for (const auto& stream : report.streams) {
        if (stream.relative_path.find("agent-0004") != std::string::npos) {
            child_reported = true;
            CHECK_FALSE(stream.ok);
            CHECK(stream.error_code == "verify.truncated_tail");
        } else {
            CHECK(stream.ok);
        }
    }
    CHECK(child_reported);
}
