// resume/clear 的 runtime 接线测试(P0-3):
//   - TrajectorySessionLedger::Open(resume_at_launch):--continue 启动路开
//     start_reason=resume 的新场,history 投影可取,source 只读;
//   - ResumeInteractive:/resume 七步经账本走,requested 落旧场、新场带
//     跨 session command.completed;
//   - ClearSession:八步换账后账本指新场,选段器重置;
//   - ProjectHistoryFromReplay:ReplayState -> api::Message 投影形状;
//   - FoldMainReplay / ExactReplayMain:writer 持柄时照读,hash 确定。
#include <doctest/doctest.h>

#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "api/types.hpp"
#include "runtime/trajectory_session.hpp"
#include "trajectory/journal.hpp"
#include "trajectory/replay.hpp"

using namespace lubancode;
using namespace lubancode::runtime;

namespace {

std::filesystem::path MakeRoot(const char* tag) {
    const std::filesystem::path root =
        std::filesystem::temp_directory_path() / ("lubancode-traj-resume-wiring-" + std::string(tag));
    std::error_code ec;
    std::filesystem::remove_all(root, ec);
    std::filesystem::create_directories(root, ec);
    return root;
}

TrajectorySessionLedger::Options LedgerOptions(const std::filesystem::path& root) {
    TrajectorySessionLedger::Options options;
    options.workspace_root = root / "ws";
    options.trajectories_root = root / "trajectories";
    options.readable_workspace_name = "接线测试";
    options.launch_cwd = "D:/tmp/ws";
    options.lubancode_version = "0.26.138-test";
    return options;
}

// 用账本自己的桥写一轮真 turn(比手拼事件更贴运行时路径)。
void DriveOneTurn(TrajectorySessionLedger& ledger) {
    auto bridge = ledger.NewTurnBridge({});
    REQUIRE(bridge != nullptr);
    bridge->BeginTurn("turn-0001", "external_user");
    api::Message user;
    user.role = api::Role::User;
    user.content.push_back(api::TextBlock{.text = "接线一轮"});
    bridge->RecordInput(user);
    api::Request request;
    request.model = "demo-model";
    const std::string request_id = bridge->OnRequestPrepared(request, agent::RequestPreparedContext{});
    REQUIRE_FALSE(request_id.empty());
    bridge->OnRequestSent(request_id);
    api::Message assistant;
    assistant.role = api::Role::Assistant;
    assistant.content.push_back(api::TextBlock{.text = "接好了"});
    REQUIRE(bridge->OnOutputCompleted(request_id, assistant, "end_turn", "resp_demo"));
    bridge->EndTurn(/*ok=*/true, /*cancelled=*/false, {});
}

}  // namespace

TEST_CASE("ProjectHistoryFromReplay: 三角色投影与 call 配对") {
    // golden fixture 折账后投影:user/assistant/tool/assistant 四条。
    const auto fixture = std::filesystem::path(LUBANCODE_SOURCE_DIR) / "tests" / "fixtures" /
                         "trajectory" / "v1" / "golden_main.jsonl";
    const auto fold = trajectory::FoldStreamReplay(fixture);
    REQUIRE(fold.ok());
    const std::vector<api::Message> history = ProjectHistoryFromReplay(fold.state);
    REQUIRE(history.size() == 4);
    CHECK(history[0].role == api::Role::User);
    CHECK(history[1].role == api::Role::Assistant);
    // assistant 的 tool_call 块还原成 ToolUseBlock。
    bool has_tool_use = false;
    for (const auto& block : history[1].content) {
        if (const auto* call = std::get_if<api::ToolUseBlock>(&block)) {
            has_tool_use = call->id == "call-0001" && call->name == "read_file";
        }
    }
    CHECK(has_tool_use);
    // tool result:ToolResultBlock,tool_use_id 配对。
    const auto* result = std::get_if<api::ToolResultBlock>(&history[2].content.front());
    REQUIRE(result != nullptr);
    CHECK(result->tool_use_id == "call-0001");
    CHECK(history[3].role == api::Role::Assistant);
}

TEST_CASE("--continue 启动路: resume_at_launch 开新场,history 投影可取") {
    const auto root = MakeRoot("launch");
    // 先开一场写一轮,封口(exit)。
    std::string source_id;
    {
        auto ledger = TrajectorySessionLedger::Open(LedgerOptions(root));
        REQUIRE(ledger.has_value());
        source_id = ledger->session_id();
        DriveOneTurn(*ledger);
        const auto closed = ledger->CloseSession("exit");
        REQUIRE(closed.error_code.empty());
    }
    // source main.jsonl 存在,记下字节数。
    std::filesystem::path main_path;
    {
        const auto workspaces = root / "trajectories" / "workspaces";
        for (const auto& workspace : std::filesystem::directory_iterator(workspaces)) {
            for (const auto& session : std::filesystem::directory_iterator(workspace.path() / "sessions")) {
                if (session.path().filename().generic_string() == source_id) {
                    main_path = session.path() / "main.jsonl";
                }
            }
        }
    }
    REQUIRE_FALSE(main_path.empty());
    std::error_code ec;
    const auto bytes_before = std::filesystem::file_size(main_path, ec);
    REQUIRE_FALSE(ec);

    // --continue:同一 workspace 重开,resume_at_launch。
    auto options = LedgerOptions(root);
    options.resume_at_launch = true;
    auto ledger = TrajectorySessionLedger::Open(std::move(options));
    REQUIRE(ledger.has_value());
    CHECK(ledger->resumed_at_launch());
    CHECK(ledger->session_id() != source_id);  // 新 session,绝不复用 id
    const std::vector<api::Message> history = ledger->LaunchResumeHistory();
    REQUIRE(history.size() == 2);  // user + assistant
    CHECK(history[0].role == api::Role::User);
    CHECK(history[1].role == api::Role::Assistant);
    // source 只读:字节数不变(永不 reopen append)。
    CHECK(std::filesystem::file_size(main_path, ec) == bytes_before);

    // 新场折叠:run.started(resume) + resume.source.attached 在头两条。
    const auto fold = ledger->FoldMainReplay();
    REQUIRE(fold.ok());
    CHECK(fold.state.start_reason == "resume");
    CHECK(fold.state.control.resumed_from_session_id.value_or("") == source_id);
    // exact replay 口:hash 确定,折两次一致。
    const auto exact_a = ledger->ExactReplayMain();
    const auto exact_b = ledger->ExactReplayMain();
    REQUIRE(exact_a.ok);
    CHECK(exact_a.state_hash == exact_b.state_hash);  // 折两次同 hash(§10.2)
    CHECK(exact_a.state_hash != fold.state.integrity.last_event_hash);  // 两种 hash 各是各的
}

TEST_CASE("ResumeInteractive: 旧场封口 + 新场七步 + 跨 session command") {
    const auto root = MakeRoot("interactive");
    std::string source_id;
    {
        auto ledger = TrajectorySessionLedger::Open(LedgerOptions(root));
        REQUIRE(ledger.has_value());
        source_id = ledger->session_id();
        DriveOneTurn(*ledger);
        REQUIRE(ledger->CloseSession("exit").error_code.empty());
    }
    // 新进程视角:先开一场(模拟"当前会话"),再 /resume source。
    auto ledger = TrajectorySessionLedger::Open(LedgerOptions(root));
    REQUIRE(ledger.has_value());
    const std::string current_id = ledger->session_id();
    DriveOneTurn(*ledger);

    const TrajectoryResumeSummary summary = ledger->ResumeInteractive(source_id, "resume");
    REQUIRE(summary.outcome.error_code.empty());
    CHECK(summary.outcome.source_session_id == source_id);
    CHECK(summary.outcome.new_session_id != source_id);
    CHECK(summary.outcome.new_session_id != current_id);
    REQUIRE(summary.history.size() == 2);
    CHECK(summary.history[0].role == api::Role::User);

    // 新场账面:run.started(resume) → resume.source.attached → 跨 session
    // control.command.completed。
    const auto fold = ledger->FoldMainReplay();
    REQUIRE(fold.ok());
    CHECK(fold.state.start_reason == "resume");
    CHECK(fold.state.control.resumed_from_session_id.value_or("") == source_id);
    // 旧场封口为 switch_to_resume。
    bool old_ended = false;
    {
        const auto workspaces = root / "trajectories" / "workspaces";
        for (const auto& workspace : std::filesystem::directory_iterator(workspaces)) {
            for (const auto& session : std::filesystem::directory_iterator(workspace.path() / "sessions")) {
                if (session.path().filename().generic_string() != current_id) {
                    continue;
                }
                const auto lines = trajectory::ReadJournalLines(session.path() / "main.jsonl");
                if (!lines.has_value()) {
                    continue;
                }
                for (const std::string& line : *lines) {
                    const auto event = nlohmann::json::parse(line, nullptr, false);
                    if (event.is_discarded()) {
                        continue;
                    }
                    if (event.at("kind").get<std::string>() == "session.ended" &&
                        event.at("payload").at("reason").get<std::string>() == "switch_to_resume") {
                        old_ended = true;
                    }
                }
            }
        }
    }
    CHECK(old_ended);
}

TEST_CASE("ClearSession: 八步换账后账本指新场,选段器重置") {
    const auto root = MakeRoot("clear");
    auto ledger = TrajectorySessionLedger::Open(LedgerOptions(root));
    REQUIRE(ledger.has_value());
    const std::string old_id = ledger->session_id();
    DriveOneTurn(*ledger);
    // 起一份选段(clear 第 3 步要先封 interrupted)。
    REQUIRE(ledger->record_selection().Start("换账选段", "目标", {}, "验收").empty());
    CHECK(ledger->record_selection().active());

    trajectory::ClearRequest request;
    request.reason = "user_clear";
    trajectory::NullClearParticipant participant;
    const auto outcome = ledger->ClearSession(request, &participant);
    REQUIRE(outcome.error_code.empty());
    CHECK(outcome.old_session_id == old_id);
    CHECK(outcome.new_session_id != old_id);
    CHECK(outcome.active_switched);
    CHECK(ledger->session_id() == outcome.new_session_id);
    // 选段器重置:新场无活动 selection。
    CHECK_FALSE(ledger->record_selection().active());
    // 新场 run.started(start_reason=clear) 反指旧终态。
    const auto fold = ledger->FoldMainReplay();
    REQUIRE(fold.ok());
    CHECK(fold.state.start_reason == "clear");
    // 旧场封链:session.ended + close_quality 记录在案。
    CHECK(outcome.old_close_quality == "clean");
}
