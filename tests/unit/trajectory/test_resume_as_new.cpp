// resume-as-new 七步测试(P0-3 §10.4/§16.4):
//   1. source 验账(链+父子边)不过/活锁在外进程 → 明拒;
//   2. checkpoint 高水位可用则用,没有从头折;
//   3. 折叠出 effective conversation/control/state hash;
//   4. 尾部悬空工具三道账明确,unknown 副作用不可重跑;
//   5. 新 session 开张:run.started(start_reason=resume) 是首条;
//   6. resume.source.attached 带 source id/末 hash/replay 版本/imported
//      state hash/checkpoint ref/qualified refs;交互路补跨 session
//      command.completed;
//   7. 新场新命名空间:session id/run id/seq 全新,source 永不 reopen
//      append(字节数不变),已完成 child 不内联进新 main。
#include <doctest/doctest.h>

#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "platform/process.hpp"
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
        char buffer[16];
        std::snprintf(buffer, sizeof(buffer), "R%05d", random_calls);
        return buffer;
    }
};

std::filesystem::path MakeRoot(const char* tag) {
    const std::filesystem::path root =
        std::filesystem::temp_directory_path() / ("lubancode-traj-resume-" + std::string(tag));
    std::error_code ec;
    std::filesystem::remove_all(root, ec);
    std::filesystem::create_directories(root, ec);
    return root;
}

SessionManagerOptions Opts(const std::filesystem::path& root) {
    SessionManagerOptions options;
    options.workspaces_root = root / "workspaces";
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

std::vector<nlohmann::json> Events(const std::filesystem::path& stream) {
    const auto lines = ReadJournalLines(stream);
    REQUIRE(lines.has_value());
    std::vector<nlohmann::json> events;
    for (const std::string& line : *lines) {
        events.push_back(nlohmann::json::parse(line, nullptr, false));
        REQUIRE_FALSE(events.back().is_discarded());
    }
    return events;
}

std::uintmax_t FileSize(const std::filesystem::path& path) {
    std::error_code ec;
    const auto size = std::filesystem::file_size(path, ec);
    REQUIRE_FALSE(ec);
    return size;
}

// 在 active session 里写一场完整 turn(user 输入 → 模型一问一答),子代理
// 派工边界可选。回写到 main 账后由调用方 Close。
void WriteFullTurn(ActiveSession& session, const char* user_text) {
    TrajectoryRecorder& main = *session.main;
    EventScope turn = main.base_scope();
    turn.turn_id = "turn-0001";
    turn.actor = Actor::User;
    turn.origin = Origin::ExternalUser;
    REQUIRE(Put(main, EventKind::TurnStarted, turn, nlohmann::json{{"trigger", "external_user"}})
                .status == RecordReceipt::Status::Committed);
    REQUIRE(Put(main, EventKind::InputReceived, turn,
                nlohmann::json{{"input_id", "input-0001"},
                               {"content",
                                nlohmann::json::array({nlohmann::json{{"type", "text"},
                                                                      {"text", user_text}}})},
                               {"channel", "terminal"},
                               {"sender", nlohmann::json{{"kind", "local_user"}}}})
                .status == RecordReceipt::Status::Committed);
    EventScope model = main.base_scope();
    model.turn_id = "turn-0001";
    model.request_id = "req-0001";
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
                nlohmann::json{{"output_id", "output-0001"},
                               {"blocks", nlohmann::json::array({nlohmann::json{
                                   {"type", "text"}, {"text", "答完了"}}})},
                               {"stop_reason", "end_turn"}})
                .status == RecordReceipt::Status::Committed);
    REQUIRE(Put(main, EventKind::TurnCompleted, turn, nlohmann::json{{"outcome", "succeeded"}})
                .status == RecordReceipt::Status::Committed);
}

// 开一场、写满、封口;回 source session 的目录与 id。
struct SourceSession {
    FakeClock clock;
    std::unique_ptr<SessionManager> manager;
    std::string id;
    std::filesystem::path dir;
    std::string main_run_id;
    std::filesystem::path root;

    explicit SourceSession(const char* tag, bool with_child = false)
        : root(MakeRoot(tag)) {
        manager = std::make_unique<SessionManager>(Opts(root), &clock);
        auto* active = manager->LaunchSession().value_or(nullptr);
        REQUIRE(active != nullptr);
        id = active->session_id();
        dir = active->session_dir();
        main_run_id = active->manifest.main_run_id;
        WriteFullTurn(*active, "数一数文件");
        if (with_child) {
            // 一只已完成的子代理:父侧派发(started+终态带 hash),子账自
            // 己 run.started/terminal。
            const auto stream = active->directory.ReserveSubagentStream("agent-0001");
            REQUIRE(stream.has_value());
            EventScope child_scope = active->main->base_scope();
            child_scope.run_id = "agent-0001";
            child_scope.run_kind = RunKind::Subagent;
            auto child =
                TrajectoryRecorder::Start(*stream, active->directory.artifacts_root(), child_scope);
            REQUIRE(child.has_value());
            EventLinks owner;
            owner.parent_run_id = main_run_id;
            REQUIRE(child
                        ->WriteRunStarted(nlohmann::json{{"run_kind", "subagent"},
                                                         {"agent_run_id", "agent-0001"},
                                                         {"owner_run_id", main_run_id}},
                                          Durability::PowerLoss, std::move(owner))
                        .status == RecordReceipt::Status::Committed);
            const auto finished =
                child->FinishRun(EventKind::RunCompleted, "done", Durability::PowerLoss);
            REQUIRE(finished.status == RecordReceipt::Status::Committed);
            REQUIRE(child->Close().has_value());
            // 父侧工具边界:planned/effective/started/finished(child hash)。
            EventScope turn2 = active->main->base_scope();
            turn2.turn_id = "turn-0002";
            turn2.actor = Actor::User;
            turn2.origin = Origin::ExternalUser;
            REQUIRE(Put(*active->main, EventKind::TurnStarted, turn2,
                        nlohmann::json{{"trigger", "external_user"}})
                        .status == RecordReceipt::Status::Committed);
            REQUIRE(Put(*active->main, EventKind::InputReceived, turn2,
                        nlohmann::json{{"input_id", "input-0002"},
                                       {"content", nlohmann::json::array({"派个帮手"})},
                                       {"channel", "terminal"},
                                       {"sender", nlohmann::json{{"kind", "local_user"}}}})
                        .status == RecordReceipt::Status::Committed);
            EventScope model2 = active->main->base_scope();
            model2.turn_id = "turn-0002";
            model2.request_id = "req-0002";
            model2.actor = Actor::Model;
            model2.origin = Origin::ProviderModel;
            const auto prepared2 =
                Put(*active->main, EventKind::ModelRequestPrepared, model2,
                    nlohmann::json{{"model", "demo-model"},
                                   {"provider", "demo"},
                                   {"wire", "responses"},
                                   {"message_refs", nlohmann::json::array({"evt-00000002"})}});
            REQUIRE(prepared2.status == RecordReceipt::Status::Committed);
            REQUIRE(Put(*active->main, EventKind::ModelRequestSent, model2,
                        nlohmann::json{{"prepared_event_id", prepared2.event_id}})
                        .status == RecordReceipt::Status::Committed);
            REQUIRE(Put(*active->main, EventKind::ModelOutputCompleted, model2,
                        nlohmann::json{{"output_id", "output-0002"},
                                       {"blocks",
                                        nlohmann::json::array({nlohmann::json{
                                            {"type", "tool_call"}, {"call_id", "call-agent-1"},
                                            {"name", "agent"},
                                            {"arguments", nlohmann::json{{"prompt", "干活"}}}}})},
                                       {"stop_reason", "tool_use"}})
                        .status == RecordReceipt::Status::Committed);
            EventScope tool = active->main->base_scope();
            tool.turn_id = "turn-0002";
            tool.request_id = "req-0002";
            tool.call_id = "call-agent-1";
            tool.actor = Actor::Tool;
            tool.origin = Origin::SubagentTool;
            REQUIRE(Put(*active->main, EventKind::ToolExecutionPlanned, tool,
                        nlohmann::json{{"call_id", "call-agent-1"}, {"tool_name", "agent"}})
                        .status == RecordReceipt::Status::Committed);
            REQUIRE(Put(*active->main, EventKind::ToolInputEffective, tool,
                        nlohmann::json{{"call_id", "call-agent-1"},
                                       {"tool_name", "agent"},
                                       {"source_kind", "builtin"},
                                       {"effect_class", "spawn_run"},
                                       {"effective_arguments", nlohmann::json::object()},
                                       {"effective_arguments_sha256", std::string(64, 'c')}})
                        .status == RecordReceipt::Status::Committed);
            EventLinks child_edge;
            child_edge.child_run_id = "agent-0001";
            REQUIRE(Put(*active->main, EventKind::ToolExecutionStarted, tool,
                        nlohmann::json{{"call_id", "call-agent-1"}}, child_edge,
                        Durability::PowerLoss)
                        .status == RecordReceipt::Status::Committed);
            REQUIRE(Put(*active->main, EventKind::ToolExecutionFinished, tool,
                        nlohmann::json{{"outcome", "succeeded"},
                                       {"duration_ms", 9},
                                       {"result_ref",
                                        nlohmann::json{{"kind", "child_stream"},
                                                       {"child_run_id", "agent-0001"},
                                                       {"child_terminal_event_hash",
                                                        finished.event_hash}}},
                                       {"side_effects", nlohmann::json::array()}},
                        child_edge, Durability::PowerLoss)
                        .status == RecordReceipt::Status::Committed);
            REQUIRE(Put(*active->main, EventKind::ToolResultCommitted, tool,
                        nlohmann::json{{"call_id", "call-agent-1"},
                                       {"content", nlohmann::json::array({nlohmann::json{
                                           {"type", "text"}, {"text", "子代理干完了"}}})},
                                       {"is_error", false}})
                        .status == RecordReceipt::Status::Committed);
            REQUIRE(Put(*active->main, EventKind::TurnCompleted, turn2,
                        nlohmann::json{{"outcome", "succeeded"}})
                        .status == RecordReceipt::Status::Committed);
        }
        // 封口(exit)。
        NullClearParticipant participant;
        const auto closed = manager->Close({"exit"}, &participant);
        REQUIRE(closed.error_code.empty());
    }
};

}  // namespace

TEST_CASE("七步: 干净 source 的 resume-as-new 全程") {
    SourceSession source("clean", /*with_child=*/true);
    const std::uintmax_t source_main_bytes = FileSize(source.dir / "main.jsonl");
    const std::uintmax_t source_child_bytes = FileSize(source.dir / "subagents" / "agent-0001.jsonl");

    // 七步开张(同一间 workspace:root 复用,不再清盘)。
    SessionManager manager(Opts(source.root));
    ResumeRequest request;
    request.source_session_id = source.id;
    const ResumeOutcome outcome = manager.ResumeAsNew(request);
    CAPTURE(outcome.error_code);
    CAPTURE(outcome.message);

    REQUIRE(outcome.error_code.empty());
    CHECK(outcome.source_verified);
    CHECK_FALSE(outcome.source_truncated_tail);
    CHECK(IsHex64(outcome.imported_state_hash));
    CHECK(outcome.replay_version == std::to_string(kReplayProjectionVersion));
    CHECK(outcome.source_event_count > 0);
    CHECK(outcome.source_main_last_event_hash.size() == 64);
    CHECK(outcome.dangling_tools.empty());  // 干净账无悬空
    CHECK(outcome.new_session_running);
    CHECK(outcome.active_switched);

    // 第 5 步:新 session 与 source 不同;run.started(resume) 是新 main 首条。
    CHECK(outcome.new_session_id != source.id);
    CHECK(outcome.new_main_run_id != source.main_run_id);
    ActiveSession* active = manager.active();
    REQUIRE(active != nullptr);
    CHECK(active->session_id() == outcome.new_session_id);
    const auto events = Events(active->directory.main_stream_path());
    REQUIRE_FALSE(events.empty());
    CHECK(events[0].at("kind").get<std::string>() == "run.started");
    CHECK(events[0].at("payload").at("start_reason").get<std::string>() == "resume");
    CHECK(events[0].at("payload").at("resumed_from_session_id").get<std::string>() == source.id);
    CHECK(events[0].at("payload").at("caused_by_event_ref").at("session_id").get<std::string>() ==
          source.id);
    CHECK(events[0].at("seq").get<std::uint64_t>() == 1);  // 新命名空间从 1 起号
    CHECK(events[0].at("run_id").get<std::string>() == outcome.new_main_run_id);

    // 第 6 步:resume.source.attached 是第二条,payload 四必填 + qualified refs。
    REQUIRE(events.size() >= 2);
    CHECK(events[1].at("kind").get<std::string>() == "resume.source.attached");
    const auto& attached = events[1].at("payload");
    CHECK(attached.at("source_session_id").get<std::string>() == source.id);
    CHECK(attached.at("source_terminal_event_hash").get<std::string>() ==
          outcome.source_main_last_event_hash);
    CHECK(attached.at("replay_version").get<std::string>() == outcome.replay_version);
    CHECK(attached.at("imported_state_hash").get<std::string>() == outcome.imported_state_hash);
    CHECK(attached.at("qualified_event_refs").is_array());

    // source 只读:两本 Journal 字节数不变(source Journal 永不 reopen append)。
    CHECK(FileSize(source.dir / "main.jsonl") == source_main_bytes);
    CHECK(FileSize(source.dir / "subagents" / "agent-0001.jsonl") == source_child_bytes);
    // source 目录还在原地,没搬没删。
    CHECK(std::filesystem::exists(source.dir / "main.jsonl"));

    // 不内联已完成 child:投影只收 main 流事件。子账 run.started 归
    // agent-0001 流,绝不会混进 main 的 effective conversation(§10.4
    //"不把正文灌进新 main.jsonl")。两轮 turn 各有 user+assistant,第二
    // 轮的 assistant 声明子代理调用,末条是父账的子结果边界。
    REQUIRE(outcome.effective_conversation.size() == 5);
    for (const auto& message : outcome.effective_conversation) {
        CHECK(message.source_event_id.find("agent-0001:") != 0);  // 全部来自 main 流
    }
    CHECK(outcome.effective_conversation[0].role == ReplayMessage::Role::User);
    CHECK(outcome.effective_conversation[1].role == ReplayMessage::Role::Assistant);
    CHECK(outcome.effective_conversation[2].role == ReplayMessage::Role::User);
    CHECK(outcome.effective_conversation[3].role == ReplayMessage::Role::Assistant);
    // 子结果来自父账 tool.result.committed(带子代理的边界文本),非子账正文。
    CHECK(outcome.effective_conversation.back().role == ReplayMessage::Role::Tool);

    // 第 7 步:新场收口后可再验(链与边完好)。
    const auto verify_new = VerifySessionDir(active->directory.session_dir());
    CHECK(verify_new.ok);
    CHECK(verify_new.child_edges.empty());  // 新场还没派过 child
}

TEST_CASE("七步: 交互路带跨 session command.completed") {
    SourceSession source("interactive");
    SessionManager manager(Opts(source.root));
    // 交互路的旧场:这里以 manager 自己开一场再封口,再 resume source,
    // 模拟 /resume 的"当前场封口 → 新场开张"。
    auto* current = manager.LaunchSession().value_or(nullptr);
    REQUIRE(current != nullptr);
    const std::string current_id = current->session_id();
    // requested 落旧场(ResumeInteractive 的活;这里手写同一形状)。
    RecordRequest requested;
    requested.kind = EventKind::ControlCommandRequested;
    requested.scope = current->main->base_scope();
    requested.scope.actor = Actor::User;
    requested.scope.origin = Origin::ExternalUser;
    requested.scope.visibility = {Visibility::HostOnly};
    requested.scope.training_policy = TrainingPolicy::Exclude;
    requested.payload["command_id"] = "cmd-resume-0001";
    requested.payload["command_name"] = "resume";
    requested.payload["action_name"] = "resume";
    requested.payload["effect_class"] = "session_boundary";
    requested.payload["args_ref"] = nlohmann::json{{"source_session_id", source.id}};
    const auto requested_receipt = current->main->Record(requested, Durability::PowerLoss);
    REQUIRE(requested_receipt.status == RecordReceipt::Status::Committed);
    NullClearParticipant participant;
    REQUIRE(manager.Close({"switch_to_resume"}, &participant).error_code.empty());

    ResumeRequest request;
    request.source_session_id = source.id;
    request.interactive = true;
    request.previous_session_id = current_id;
    request.boundary_command.command_id = "cmd-resume-0001";
    request.boundary_command.requested_session_id = current_id;
    request.boundary_command.requested_event_id = requested_receipt.event_id;
    request.boundary_command.boundary_operation_id = current_id + ":resume";
    const ResumeOutcome outcome = manager.ResumeAsNew(request);
    CAPTURE(outcome.error_code);
    CAPTURE(outcome.message);
    REQUIRE(outcome.error_code.empty());
    CHECK(outcome.command_completed_event_id.size() > 0);

    // 新 main:run.started → resume.source.attached → 跨 session completed
    //(qualified ref 指回旧 requested,同带 boundary_operation_id)。
    const auto events = Events(manager.active()->directory.main_stream_path());
    REQUIRE(events.size() >= 3);
    CHECK(events[2].at("kind").get<std::string>() == "control.command.completed");
    const auto& completed = events[2].at("payload");
    CHECK(completed.at("qualified_requested_ref").at("session_id").get<std::string>() == current_id);
    CHECK(completed.at("qualified_requested_ref").at("event_id").get<std::string>() ==
          requested_receipt.event_id);
    CHECK(completed.at("boundary_operation_id").get<std::string>() == current_id + ":resume");
    // run.started 的 previous_session_id 指向刚封口的当前场,resumed_from 指向 source。
    CHECK(events[0].at("payload").at("previous_session_id").get<std::string>() == current_id);
    CHECK(events[0].at("payload").at("resumed_from_session_id").get<std::string>() == source.id);
}

TEST_CASE("第 1 步拒路: 活锁在外进程 / corrupt source / 找不到场") {
    SUBCASE("source 不存在") {
        SessionManager manager(Opts(MakeRoot("missing")));
        ResumeRequest request;
        request.source_session_id = "20990101-000000-NOPE00";
        const auto outcome = manager.ResumeAsNew(request);
        CHECK(outcome.error_code == "resume.source_not_found");
    }
    SUBCASE("本 workspace 没有可恢复场") {
        SessionManager manager(Opts(MakeRoot("empty")));
        ResumeRequest request;  // source_session_id 空 = 取最近一场
        const auto outcome = manager.ResumeAsNew(request);
        CHECK(outcome.error_code == "resume.source_not_found");
    }
    SUBCASE("source 被外进程持活锁") {
        SourceSession source("locked");
        // 上一进程崩了留下的锁由恢复器清;这里模拟"别人正握着":手工占一把
        // 活锁(owner = 本进程,身份对得上 = 活)。
        SessionLockOwner owner;
        owner.pid = lubancode::platform::CurrentProcessId();
        owner.process_start_token = CurrentProcessStartToken();
        owner.acquired_at_ms = 1759000000000LL;
        const auto lock = SessionLock::Acquire(source.dir, owner);
        REQUIRE(lock.has_value());
        SessionManager manager(Opts(source.root));
        ResumeRequest request;
        request.source_session_id = source.id;
        const auto outcome = manager.ResumeAsNew(request);
        CHECK(outcome.error_code == "resume.source_locked");
    }
    SUBCASE("source 链断(corrupt)") {
        SourceSession source("corrupt");
        // 抽走中段一行:seq 跳号 + 链断。
        const auto lines = ReadJournalLines(source.dir / "main.jsonl");
        REQUIRE(lines.has_value());
        REQUIRE(lines->size() > 4);
        {
            std::ofstream out(source.dir / "main.jsonl", std::ios::binary | std::ios::trunc);
            for (std::size_t i = 0; i < lines->size(); ++i) {
                if (i != 3) {
                    out << (*lines)[i] << "\n";
                }
            }
        }
        SessionManager manager(Opts(source.root));
        ResumeRequest request;
        request.source_session_id = source.id;
        const auto outcome = manager.ResumeAsNew(request);
        CHECK(outcome.error_code == "resume.source_corrupt");
    }
}

TEST_CASE("第 2/4 步: checkpoint 高水位与悬空工具分档") {
    // 悬空账:工具 started 未终态,文件完整(非截断),run 未封——正是崩溃
    // 半场留下的 incomplete。
    const auto root = MakeRoot("dangling");
    {
        FakeClock clock;
        SessionManager manager(Opts(root), &clock);
        auto* active = manager.LaunchSession().value_or(nullptr);
        REQUIRE(active != nullptr);
        TrajectoryRecorder& main = *active->main;
        EventScope turn = main.base_scope();
        turn.turn_id = "turn-0001";
        turn.actor = Actor::User;
        turn.origin = Origin::ExternalUser;
        REQUIRE(Put(main, EventKind::TurnStarted, turn,
                    nlohmann::json{{"trigger", "external_user"}})
                    .status == RecordReceipt::Status::Committed);
        REQUIRE(Put(main, EventKind::InputReceived, turn,
                    nlohmann::json{{"input_id", "input-0001"},
                                   {"content", nlohmann::json::array({"改文件"})},
                                   {"channel", "terminal"},
                                   {"sender", nlohmann::json{{"kind", "local_user"}}}})
                    .status == RecordReceipt::Status::Committed);
        EventScope model = main.base_scope();
        model.turn_id = "turn-0001";
        model.request_id = "req-0001";
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
                    nlohmann::json{{"output_id", "output-0001"},
                                   {"blocks",
                                    nlohmann::json::array({nlohmann::json{
                                        {"type", "tool_call"}, {"call_id", "call-0001"},
                                        {"name", "edit_file"},
                                        {"arguments", nlohmann::json{{"path", "a.txt"}}}}})},
                                   {"stop_reason", "tool_use"}})
                    .status == RecordReceipt::Status::Committed);
        EventScope tool = main.base_scope();
        tool.turn_id = "turn-0001";
        tool.request_id = "req-0001";
        tool.call_id = "call-0001";
        tool.actor = Actor::Tool;
        tool.origin = Origin::BuiltinTool;
        REQUIRE(Put(main, EventKind::ToolExecutionPlanned, tool,
                    nlohmann::json{{"call_id", "call-0001"}, {"tool_name", "edit_file"}})
                    .status == RecordReceipt::Status::Committed);
        REQUIRE(Put(main, EventKind::ToolInputEffective, tool,
                    nlohmann::json{{"call_id", "call-0001"},
                                   {"tool_name", "edit_file"},
                                   {"source_kind", "builtin"},
                                   {"effect_class", "external_write"},
                                   {"effective_arguments", nlohmann::json::object()},
                                   {"effective_arguments_sha256", std::string(64, 'f')}})
                    .status == RecordReceipt::Status::Committed);
        REQUIRE(Put(main, EventKind::ToolExecutionStarted, tool,
                    nlohmann::json{{"call_id", "call-0001"}}, {}, Durability::PowerLoss)
                    .status == RecordReceipt::Status::Committed);
        // 不封口:manager 析构放锁走人(崩溃现场:账完整、run 未终)。
    }

    // 找回 session id(session.json 还在)。
    std::string source_id;
    {
        const auto sessions = std::filesystem::temp_directory_path() /
                              "lubancode-traj-resume-dangling" / "workspaces";
        for (const auto& workspace : std::filesystem::directory_iterator(sessions)) {
            for (const auto& session : std::filesystem::directory_iterator(workspace.path() / "sessions")) {
                source_id = session.path().filename().generic_string();
            }
        }
    }
    REQUIRE_FALSE(source_id.empty());

    SessionManager manager(Opts(root));
    ResumeRequest request;
    request.source_session_id = source_id;
    const ResumeOutcome outcome = manager.ResumeAsNew(request);
    REQUIRE(outcome.error_code.empty());
    // 悬空工具:started 无终态,分档明确;不算 unknown(没到终态)。
    REQUIRE(outcome.dangling_tools.size() == 1);
    CHECK(outcome.dangling_tools[0].call_id == "call-0001");
    CHECK(outcome.dangling_tools[0].state == "started_no_terminal");
    // 投影照样有 user 输入(悬空前的账都在)。
    REQUIRE_FALSE(outcome.effective_conversation.empty());
    CHECK(outcome.effective_conversation[0].role == ReplayMessage::Role::User);
    // checkpoint:源里没有,from_checkpoint=false(从头折叠)。
    CHECK_FALSE(outcome.from_checkpoint);

    // 给 source 落一枚可用 checkpoint 再 resume 一场:from_checkpoint=true,
    // imported hash 与从头折相同(checkpoint 不得改变结果)。
    {
        const auto dir = manager.SessionDirOf(source_id);
        const auto stream = dir / "main.jsonl";
        const auto fold = FoldStreamReplay(stream);
        REQUIRE(fold.ok());
        ReplayCheckpoint checkpoint;
        checkpoint.stream_name = "main";
        checkpoint.source_seq = fold.state.folded_seq;
        checkpoint.source_event_hash = fold.state.integrity.last_event_hash;
        checkpoint.state_hash = ComputeReplayStateHash(fold.state);
        checkpoint.folded = fold.state;
        REQUIRE(WriteReplayCheckpoint(dir, checkpoint).has_value());
        // 封掉上一场 resume 开出的新场,才能再 resume。
        NullClearParticipant participant;
        REQUIRE(manager.Close({"switch_to_resume"}, &participant).error_code.empty());
        ResumeRequest again;
        again.source_session_id = source_id;
        const ResumeOutcome second = manager.ResumeAsNew(again);
        CAPTURE(second.error_code);
        CAPTURE(second.message);
        REQUIRE(second.error_code.empty());
        CHECK(second.from_checkpoint);
        CHECK(second.checkpoint_seq == fold.state.folded_seq);
        CHECK(second.imported_state_hash == ComputeReplayStateHash(fold.state));
    }
}
