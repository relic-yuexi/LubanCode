// /goal 的 v3 命令接线与恢复去重(轨迹 v3 §4.67 G1,app 层):
//   - v3 卷上的会话:Ensure 后 goal_service 非空,命令材料包带上服务/预算/
//     投影口;v2 场(没接账)goal_service 空,照旧走 v1 coordinator;
//   - HandleGoalCommandV3 七动作:create(合同+首轮意图入快照)、status
//     (lineage 投影单一读面)、pause/resume(转换表边)、edit(AmendContract)、
//     clear(二次确认+Cleared);
//   - 泵路:认领→开轮(iterationId 落快照)→synthetic turn→收工(intent
//     销账);第二拍不再开轮(去重);
//   - HandleSessionCleared:clear 换场后内存接管态清空。
// runtime 面(意图合同/claim/跨卷/lineage)在 tests/unit/runtime/
// test_goal_service_g1.cpp;本册只钉接线与命令行为。
#include <doctest/doctest.h>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "app/commands/goal_commands.hpp"
#include "app/wirings/goal_session_wiring.hpp"
#include "cli/theme.hpp"
#include "config/config.hpp"
#include "runtime/trajectory_session.hpp"
#include "workspace/identity.hpp"

namespace goalns = lubancode::runtime::goal;
using lubancode::app::GoalSessionWiring;
using lubancode::app::GoalWiring;
using lubancode::cli::GoalCommandAction;
using lubancode::cli::ParsedGoalCommand;

namespace {

// ctest 钉 LUBANCODE_TRAJECTORY_V3_NEW_SESSIONS=0;v3 册显式开回 1——
// 账本按产品缺省开 v3 卷,goal 命令才走 G1 路线。
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

ParsedGoalCommand ParseAction(GoalCommandAction action, std::string objective = "") {
    ParsedGoalCommand parsed;
    parsed.action = action;
    parsed.objective = std::move(objective);
    return parsed;
}

// 真账本夹具:临时根下开一场 TrajectorySessionLedger(v3 卷)。
struct GoalV3Fixture {
    std::filesystem::path dir;
    std::optional<lubancode::runtime::TrajectorySessionLedger> ledger;
    lubancode::cli::Theme theme;
    lubancode::config::Config config;
    GoalSessionWiring wiring;
    std::vector<std::string> turn_texts;
    std::vector<std::string> notes;

    explicit GoalV3Fixture(bool with_ledger = true)
        : dir(std::filesystem::temp_directory_path() /
              ("lubancode-goal-v3-cmd-" +
               std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) +
               "-" + std::to_string(reinterpret_cast<std::uintptr_t>(this)))) {
        std::error_code ec;
        std::filesystem::create_directories(dir, ec);
        std::filesystem::create_directories(dir / "repo", ec);
        config.features_goals = true;  // 正门开(env 总闸测试环境不设)
        if (with_ledger) {
            lubancode::runtime::TrajectorySessionLedger::Options options;
            options.workspaces_root = dir / "workspaces";
            options.workspace_identity = lubancode::workspace::MakeFallbackIdentity(dir / "repo");
            options.lubancode_version = "test";
            auto opened = lubancode::runtime::TrajectorySessionLedger::Open(options);
            REQUIRE(opened.has_value());
            ledger.emplace(std::move(*opened));
        }
        GoalSessionWiring::Host host;
        host.theme = &theme;
        host.config = &config;
        host.current_model = std::make_shared<std::string>("test-model");
        host.trajectory = ledger.has_value() ? &*ledger : nullptr;
        host.start_turn = [this](const std::string& text, bool* failed) {
            turn_texts.push_back(text);
            if (failed != nullptr) *failed = false;
        };
        host.notify = [this](bool is_error, const std::string& text) {
            notes.push_back(std::string(is_error ? "E: " : "N: ") + text);
        };
        wiring.AttachHost(std::move(host));
    }

    ~GoalV3Fixture() {
        ledger.reset();
        std::error_code ec;
        std::filesystem::remove_all(dir, ec);
    }

    GoalV3Fixture(const GoalV3Fixture&) = delete;
    GoalV3Fixture& operator=(const GoalV3Fixture&) = delete;

    GoalWiring Pack() { return wiring.MakeCommandWiring(nullptr, nullptr); }
};

}  // namespace

TEST_CASE("v3 卷:Ensure 绑服务,命令七动作走 GoalService") {
    EnvGuard guard("LUBANCODE_TRAJECTORY_V3_NEW_SESSIONS", "1");
    GoalV3Fixture fixture;
    fixture.wiring.Ensure(fixture.config);
    REQUIRE(fixture.wiring.goal_service() != nullptr);
    // 二次 Ensure 幂等(同卷不重建)。
    fixture.wiring.Ensure(fixture.config);

    GoalWiring pack = fixture.Pack();
    REQUIRE(pack.goal_service != nullptr);
    REQUIRE(pack.project_goal != nullptr);
    REQUIRE(pack.goals_config != nullptr);

    // create:首轮意图随快照落账。
    auto flow = lubancode::app::HandleGoalCommand(
        ParseAction(GoalCommandAction::Create, "修好 auth;ctest -R auth 全过"), pack);
    CHECK(flow == lubancode::app::CommandFlow::Continue);
    const goalns::GoalStateSnapshot* current = pack.goal_service->current();
    REQUIRE(current != nullptr);
    CHECK(current->goal_id == "goal-1");
    CHECK(current->pending_intent.at("workItemId") == "wi-1");
    CHECK(current->budget.max_iterations == fixture.config.goals.max_iterations);

    // status:单一读面(lineage 投影)能看到刚落的目标。
    flow = lubancode::app::HandleGoalCommand(ParseAction(GoalCommandAction::Status), pack);
    CHECK(flow == lubancode::app::CommandFlow::Continue);
    const auto lineage = pack.project_goal();
    CHECK(lineage.found);
    CHECK(lineage.projection.gap == goalns::GoalProjectionGap::None);
    CHECK(lineage.projection.snapshot.goal_id == "goal-1");

    // pause:转换表边(Active 尚未到——create 落 preparing;先开轮到位)。
    auto claim = pack.goal_service->ClaimPendingIntent(
        "run-test", current->state_revision, nlohmann::json{{"test", true}});
    REQUIRE(claim.ok);
    REQUIRE(pack.goal_service->BeginIteration(pack.goal_service->current()->state_revision, {})
                 .ok);
    REQUIRE(pack.goal_service->EndIteration(pack.goal_service->current()->state_revision, {}).ok);

    // pause → paused(停因 user_pause)。
    flow = lubancode::app::HandleGoalCommand(ParseAction(GoalCommandAction::Pause), pack);
    CHECK(flow == lubancode::app::CommandFlow::Continue);
    CHECK(pack.goal_service->current()->lifecycle == goalns::GoalLifecycle::Paused);
    CHECK(pack.goal_service->current()->stop_reason == "user_pause");
    // 再 pause:幂等提示,不报错。
    flow = lubancode::app::HandleGoalCommand(ParseAction(GoalCommandAction::Pause), pack);
    CHECK(flow == lubancode::app::CommandFlow::Continue);

    // resume → active。
    flow = lubancode::app::HandleGoalCommand(ParseAction(GoalCommandAction::Resume), pack);
    CHECK(flow == lubancode::app::CommandFlow::Continue);
    CHECK(pack.goal_service->current()->lifecycle == goalns::GoalLifecycle::Active);
    // 没停时 resume:提示"未停"。
    flow = lubancode::app::HandleGoalCommand(ParseAction(GoalCommandAction::Resume), pack);
    CHECK(flow == lubancode::app::CommandFlow::Continue);

    // edit:AmendContract(c2),旧证据翻 stale。
    flow = lubancode::app::HandleGoalCommand(
        ParseAction(GoalCommandAction::Edit, "修好 auth 并补文档;ctest 全过"), pack);
    CHECK(flow == lubancode::app::CommandFlow::Continue);
    CHECK(pack.goal_service->current()->contract_revision == 2);
    CHECK(pack.goal_service->current()->contract.objective.find("补文档") != std::string::npos);

    // 已有未收账 goal 再 create:goal.already_active。
    flow = lubancode::app::HandleGoalCommand(
        ParseAction(GoalCommandAction::Create, "第二只目标"), pack);
    CHECK(flow == lubancode::app::CommandFlow::Continue);
    CHECK(pack.goal_service->current()->goal_id == "goal-1");  // 没被替换
}

TEST_CASE("v3 泵路:认领→开轮→synthetic turn→收工,第二拍不再开轮") {
    EnvGuard guard("LUBANCODE_TRAJECTORY_V3_NEW_SESSIONS", "1");
    GoalV3Fixture fixture;
    fixture.wiring.Ensure(fixture.config);
    GoalWiring pack = fixture.Pack();
    REQUIRE(lubancode::app::HandleGoalCommand(
                ParseAction(GoalCommandAction::Create, "写一份 README"), pack) ==
            lubancode::app::CommandFlow::Continue);

    // 探针:未认领意图 → 候选带 workItemId。
    const auto probe = fixture.wiring.work_source().ProbeWork();
    REQUIRE(probe.has_value());
    CHECK(probe->payload.at("work_item_id") == "wi-1");

    // 泵一拍:claim + Begin + turn + End。iterationId 落快照(不恒 null)。
    fixture.wiring.PumpContinuation(0);
    REQUIRE(fixture.turn_texts.size() == 1);
    CHECK(fixture.turn_texts[0].find("goal-1") != std::string::npos);
    CHECK(fixture.turn_texts[0].find("写一份 README") != std::string::npos);
    const goalns::GoalStateSnapshot* current = pack.goal_service->current();
    REQUIRE(current != nullptr);
    CHECK(current->lifecycle == goalns::GoalLifecycle::Active);
    CHECK(current->phase == goalns::GoalPhase::Idle);
    CHECK(current->iteration_id.has_value());
    CHECK(*current->iteration_id == "goal-1/iter-1");
    CHECK(current->counters.iterations_started == 1);
    CHECK(current->pending_intent.empty());  // intent 销账

    // 第二拍:没有意图可认领,不再开轮(去重:恰好消费一次)。
    fixture.wiring.PumpContinuation(0);
    CHECK(fixture.turn_texts.size() == 1);
    CHECK_FALSE(fixture.wiring.work_source().ProbeWork().has_value());
}

TEST_CASE("v3 恢复:RestoreFromArchive 接管 + clear 换场清内存") {
    EnvGuard guard("LUBANCODE_TRAJECTORY_V3_NEW_SESSIONS", "1");
    GoalV3Fixture fixture;
    fixture.wiring.Ensure(fixture.config);
    GoalWiring pack = fixture.Pack();
    REQUIRE(lubancode::app::HandleGoalCommand(
                ParseAction(GoalCommandAction::Create, "跨恢复的目标"), pack) ==
            lubancode::app::CommandFlow::Continue);

    // clear 命令二次确认走 ReadLine,这里直接钉服务层状态:HandleSession-
    // Cleared 清内存接管态(命令面的 y/N 交互在人工验收路)。
    fixture.wiring.HandleSessionCleared();
    CHECK(fixture.wiring.goal_service() == nullptr);
    // Ensure 重绑(同卷),但不再有内存 current——新 clear 场不带旧 goal,
    // 状态读面由 lineage 投影按卷重判。
    fixture.wiring.Ensure(fixture.config);
    CHECK(fixture.wiring.goal_service() != nullptr);
    CHECK(fixture.wiring.goal_service()->current() == nullptr);

    // RestoreFromArchive:同卷(lineage 读到本卷 goal 账)接管成功。
    fixture.wiring.RestoreFromArchive();
    CHECK(fixture.wiring.goal_service() != nullptr);
    CHECK(fixture.wiring.goal_service()->current() != nullptr);
    CHECK(fixture.wiring.goal_service()->current()->goal_id == "goal-1");
    bool saw_restore_note = false;
    for (const std::string& note : fixture.notes) {
        if (note.find("goal 已恢复") != std::string::npos) saw_restore_note = true;
    }
    CHECK(saw_restore_note);
}

TEST_CASE("v2 场:没接账本,命令照旧走 v1 coordinator") {
    EnvGuard guard("LUBANCODE_TRAJECTORY_V3_NEW_SESSIONS", "1");
    GoalV3Fixture fixture(/*with_ledger=*/false);
    fixture.wiring.Ensure(fixture.config);
    // v1 coordinator 在位;v3 服务没安家(没接账本)。
    CHECK(fixture.wiring.coordinator() != nullptr);
    CHECK(fixture.wiring.goal_service() == nullptr);
    GoalWiring pack = fixture.Pack();
    CHECK(pack.goal_service == nullptr);
    CHECK(pack.coordinator != nullptr);
    // v1 create 走老路(goals_enabled 由 config 正门控)。
    const auto flow = lubancode::app::HandleGoalCommand(
        ParseAction(GoalCommandAction::Create, "v1 老路"), pack);
    CHECK(flow == lubancode::app::CommandFlow::Continue);
}
