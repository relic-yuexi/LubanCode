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
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "app/commands/goal_commands.hpp"
#include "app/wirings/goal_session_wiring.hpp"
#include "cli/theme.hpp"
#include "config/config.hpp"
#include "runtime/trajectory_session.hpp"
#include "trajectory/v3/reader.hpp"
#include "trajectory/v3/session_switch.hpp"
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
    // 主轮 usage 注入口(§4.67.7 归账测试用):空 = 装配层没有 turn 视图,
    // 泵如实跳过归账。
    std::function<std::optional<lubancode::runtime::TurnMetrics>()> turn_metrics;

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
        host.start_turn = [this](const std::string& text, bool* failed, bool* cancelled) {
            turn_texts.push_back(text);
            if (failed != nullptr) *failed = false;
            if (cancelled != nullptr) *cancelled = false;
        };
        host.last_turn_metrics = [this]() { return turn_metrics ? turn_metrics() : std::nullopt; };
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

TEST_CASE("v3 edit 后按新合同续跑:edit 落新工作项,泵下一拍开新轮") {
    EnvGuard guard("LUBANCODE_TRAJECTORY_V3_NEW_SESSIONS", "1");
    GoalV3Fixture fixture;
    fixture.wiring.Ensure(fixture.config);
    GoalWiring pack = fixture.Pack();
    REQUIRE(lubancode::app::HandleGoalCommand(
                ParseAction(GoalCommandAction::Create, "旧目标正文"), pack) ==
            lubancode::app::CommandFlow::Continue);

    // 第一轮按 c1 跑完(无评估口:EndIteration 销账收口)。
    fixture.wiring.PumpContinuation(0);
    REQUIRE(fixture.turn_texts.size() == 1);
    CHECK(fixture.turn_texts[0].find("旧目标正文") != std::string::npos);

    // pause 后 edit:AmendContract 清随旧合同作废的意图,命令面按 c2 重拟
    // 新工作项——目标 preparing 但有班可上,泵自动续跑,无需 resume。
    REQUIRE(lubancode::app::HandleGoalCommand(ParseAction(GoalCommandAction::Pause), pack) ==
            lubancode::app::CommandFlow::Continue);
    REQUIRE(lubancode::app::HandleGoalCommand(
                ParseAction(GoalCommandAction::Edit, "新目标正文-改成补文档"), pack) ==
            lubancode::app::CommandFlow::Continue);
    const goalns::GoalStateSnapshot* current = pack.goal_service->current();
    REQUIRE(current != nullptr);
    CHECK(current->contract_revision == 2);
    CHECK(current->lifecycle == goalns::GoalLifecycle::Preparing);
    CHECK(current->pending_intent.at("workItemId") == "goal-1/wi-c2");
    CHECK(current->pending_intent.at("contractRevision") == 2);

    // 泵下一拍:按新合同开第二轮(轮正文带新目标,不带旧目标)。
    fixture.wiring.PumpContinuation(0);
    REQUIRE(fixture.turn_texts.size() == 2);
    CHECK(fixture.turn_texts[1].find("新目标正文") != std::string::npos);
    CHECK(fixture.turn_texts[1].find("旧目标正文") == std::string::npos);
    current = pack.goal_service->current();
    REQUIRE(current != nullptr);
    CHECK(current->counters.iterations_started == 2);
    CHECK(current->pending_intent.empty());  // 收口销账,不再死锁
}

TEST_CASE("v3 edit 在途拒:意图已认领时 edit 等安全边界(goal.busy)") {
    EnvGuard guard("LUBANCODE_TRAJECTORY_V3_NEW_SESSIONS", "1");
    GoalV3Fixture fixture;
    fixture.wiring.Ensure(fixture.config);
    GoalWiring pack = fixture.Pack();
    REQUIRE(lubancode::app::HandleGoalCommand(
                ParseAction(GoalCommandAction::Create, "在途目标"), pack) ==
            lubancode::app::CommandFlow::Continue);
    // 模拟在途:认领 + 开轮(泵还没跑 turn)。
    goalns::GoalService* service = pack.goal_service;
    REQUIRE(service->ClaimPendingIntent("run-test", service->current()->state_revision, {}).ok);
    REQUIRE(service->BeginIteration(service->current()->state_revision, {}).ok);

    REQUIRE(lubancode::app::HandleGoalCommand(
                ParseAction(GoalCommandAction::Edit, "半途想改"), pack) ==
            lubancode::app::CommandFlow::Continue);
    // 明确终态:合同一字未动,错误面有 goal.busy 的人话引导。
    CHECK(service->current()->contract_revision == 1);
    CHECK(service->current()->phase == goalns::GoalPhase::Running);
    bool saw_busy_note = false;
    for (const std::string& note : fixture.notes) {
        if (note.find("目标正在跑") != std::string::npos) saw_busy_note = true;
    }
    CHECK(saw_busy_note);
}

TEST_CASE("v3 resume 修不可推进的 preparing:崩溃窗口(edit 后没排上意图)重排") {
    EnvGuard guard("LUBANCODE_TRAJECTORY_V3_NEW_SESSIONS", "1");
    GoalV3Fixture fixture;
    fixture.wiring.Ensure(fixture.config);
    GoalWiring pack = fixture.Pack();
    goalns::GoalService* service = pack.goal_service;
    REQUIRE(lubancode::app::HandleGoalCommand(
                ParseAction(GoalCommandAction::Create, "改版目标"), pack) ==
            lubancode::app::CommandFlow::Continue);
    // 直接服务面改版(绕过命令面的补排步)= edit 两笔提交间崩溃的等价态:
    // preparing + 意图空(随旧合同作废)。
    goalns::GoalContract contract = service->current()->contract;
    contract.objective = "改版后的目标";
    REQUIRE(service->AmendContract(contract, service->current()->state_revision,
                                   service->current()->contract_revision, {})
                 .ok);
    CHECK(service->current()->pending_intent.empty());

    // resume 不再对"无班可上"的 preparing 早退:转 active 并补排新工作项。
    REQUIRE(lubancode::app::HandleGoalCommand(ParseAction(GoalCommandAction::Resume), pack) ==
            lubancode::app::CommandFlow::Continue);
    const goalns::GoalStateSnapshot* current = service->current();
    REQUIRE(current != nullptr);
    CHECK(current->lifecycle == goalns::GoalLifecycle::Active);
    CHECK(current->pending_intent.at("contractRevision") == 2);
    bool saw_work_note = false;
    for (const std::string& note : fixture.notes) {
        if (note.find("已排") != std::string::npos) saw_work_note = true;
    }
    CHECK(saw_work_note);

    // 泵接着按新合同开轮。
    fixture.wiring.PumpContinuation(0);
    REQUIRE(fixture.turn_texts.size() == 1);
    CHECK(fixture.turn_texts[0].find("改版后的目标") != std::string::npos);
}

TEST_CASE("v3 resume 解 waiting:已认领收口位恢复 running 续收口,不重开轮") {
    EnvGuard guard("LUBANCODE_TRAJECTORY_V3_NEW_SESSIONS", "1");
    GoalV3Fixture fixture;
    fixture.wiring.Ensure(fixture.config);
    GoalWiring pack = fixture.Pack();
    REQUIRE(lubancode::app::HandleGoalCommand(
                ParseAction(GoalCommandAction::Create, "等后台的目标"), pack) ==
            lubancode::app::CommandFlow::Continue);
    // 直进收口位等待:认领 + 开轮后转 waiting(泵路里等价于收口被本轮
    // 派生的在跑子代理截走)。
    goalns::GoalService* service = pack.goal_service;
    REQUIRE(service->ClaimPendingIntent("run-test", service->current()->state_revision, {}).ok);
    REQUIRE(service->BeginIteration(service->current()->state_revision, {}).ok);
    REQUIRE(service->EnterWaiting({"subagent-4"}, service->current()->state_revision,
                                  nlohmann::json{{"source", "test"}})
                .ok);
    CHECK(service->current()->lifecycle == goalns::GoalLifecycle::Waiting);

    // 修复前:resume 落 active/idle,intent 已认领 → 泵判"已认领且不在待开
    // 轮相位"死锁;修复后:走 ResolveWaiting 恢复 active/running,收口续跑。
    REQUIRE(lubancode::app::HandleGoalCommand(ParseAction(GoalCommandAction::Resume), pack) ==
            lubancode::app::CommandFlow::Continue);
    const goalns::GoalStateSnapshot* current = service->current();
    REQUIRE(current != nullptr);
    CHECK(current->lifecycle == goalns::GoalLifecycle::Active);
    CHECK(current->phase == goalns::GoalPhase::Running);  // 收口位:续收口不开新轮
    CHECK(current->wait_task_refs.empty());
    // 工作项还认领着,不会被新意图顶掉(收口材料不动)。
    CHECK(current->pending_intent.at("claimed") == true);
    CHECK(current->counters.iterations_started == 1);  // 没开第二轮
    CHECK(fixture.turn_texts.empty());
    bool saw_release_note = false;
    for (const std::string& note : fixture.notes) {
        if (note.find("等待解除") != std::string::npos) saw_release_note = true;
    }
    CHECK(saw_release_note);
}

TEST_CASE("v3 主轮 usage 归账:泵收口把本轮模型用量记入 goal 账") {
    EnvGuard guard("LUBANCODE_TRAJECTORY_V3_NEW_SESSIONS", "1");
    GoalV3Fixture fixture;
    lubancode::runtime::TurnMetrics metrics;
    metrics.request_count = 2;
    metrics.input_tokens = 120;
    metrics.output_tokens = 30;
    metrics.cache_read_tokens = 8;
    fixture.turn_metrics = [metrics]() { return metrics; };
    fixture.wiring.Ensure(fixture.config);
    GoalWiring pack = fixture.Pack();
    REQUIRE(lubancode::app::HandleGoalCommand(
                ParseAction(GoalCommandAction::Create, "记账目标"), pack) ==
            lubancode::app::CommandFlow::Continue);

    fixture.wiring.PumpContinuation(0);
    REQUIRE(fixture.turn_texts.size() == 1);
    const goalns::GoalStateSnapshot* current = pack.goal_service->current();
    REQUIRE(current != nullptr);
    // 快照 usage 只增:主轮 120/30 + cache 8 全入账,实报置位。
    CHECK(current->usage.input_tokens == 120);
    CHECK(current->usage.output_tokens == 30);
    CHECK(current->usage.cache_read_tokens == 8);
    CHECK(current->usage.request_count == 2);
    CHECK(current->usage.usage_reported);
    // 事实行 goal.usage.recorded 在链上,requestId=iterationId,source=main_turn。
    const auto stream =
        lubancode::trajectory::v3::FindV3SessionStream(fixture.ledger->session_dir());
    REQUIRE(stream.has_value());
    const auto ledger = lubancode::trajectory::v3::ReadV3Ledger(*stream);
    REQUIRE(ledger.has_value());
    bool saw_usage_row = false;
    for (const auto& event : ledger->events) {
        if (event.kind != lubancode::trajectory::v3::EventKindV3::GoalUsageRecorded) continue;
        if (event.payload.value("requestId", std::string()) == "goal-1/iter-1" &&
            event.payload.value("source", std::string()) == "main_turn") {
            saw_usage_row = true;
        }
    }
    CHECK(saw_usage_row);

    // 第二轮:新 iterationId 各记各的(计费去重按 (sessionId,requestId),
    // 同轮重复通知幂等;这里跨轮各一笔,累计只增)。
    goalns::GoalPendingIntent next;
    next.work_item_id = "goal-1/wi-1";
    next.contract_revision = 1;
    next.continuation_ordinal = 1;
    REQUIRE(pack.goal_service
                 ->SetPendingIntent(next, current->state_revision, nlohmann::json{{"t", true}})
                 .ok);
    lubancode::runtime::TurnMetrics second = metrics;
    second.request_count = 1;
    second.input_tokens = 40;
    second.output_tokens = 10;
    second.cache_read_tokens = 0;
    fixture.turn_metrics = [second]() { return second; };
    fixture.wiring.PumpContinuation(0);
    REQUIRE(fixture.turn_texts.size() == 2);
    current = pack.goal_service->current();
    REQUIRE(current != nullptr);
    CHECK(current->usage.input_tokens == 160);  // 120 + 40,只增
    CHECK(current->usage.request_count == 3);
}
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
