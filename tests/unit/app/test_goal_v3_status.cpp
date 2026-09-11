// 跨壳统一状态投影(轨迹 v3 §4.67 G3):
//   - GoalV3LifecycleCode/BuildGoalV3HeadLine:短码与首行是 /goal status、
//     状态栏、resume 通知三处共用的唯一折法;
//   - BuildGoalLoopStatusSegment v3 重载:v3 快照在场吃 v3(短码/版本号),
//     空指针回落 v1(v2 场一字不变);
//   - FormatGoalV3Status:等待/巡检/停止意图/fork 来路各行如实;缺口第一
//     行明报;
//   - /goal resume 预算增量解析:iterations/tokens/elapsed_min 三键,坏键
//     坏值 Invalid,分钟换毫秒落账。
#include <doctest/doctest.h>

#include <string>

#include <nlohmann/json.hpp>

#include "app/commands/goal_commands.hpp"
#include "cli/slash_commands.hpp"
#include "runtime/goal_service.hpp"

namespace goalns = lubancode::runtime::goal;
using goalns::GoalLifecycle;
using goalns::GoalPhase;
using goalns::GoalStateSnapshot;

namespace {

GoalStateSnapshot SnapshotWith(GoalLifecycle lifecycle, GoalPhase phase,
                               std::uint64_t state_revision = 5,
                               std::uint64_t contract_revision = 2, int iterations = 3) {
    GoalStateSnapshot snapshot;
    snapshot.goal_id = "goal-1";
    snapshot.session_id = "s1";
    snapshot.run_id = "run-s1";
    snapshot.state_revision = state_revision;
    snapshot.contract_revision = contract_revision;
    snapshot.objective = "修好 auth 模块";
    snapshot.contract.objective = "修好 auth 模块";
    snapshot.lifecycle = lifecycle;
    snapshot.phase = phase;
    if (lifecycle == GoalLifecycle::Waiting || lifecycle == GoalLifecycle::Paused ||
        lifecycle == GoalLifecycle::Blocked || lifecycle == GoalLifecycle::AwaitingUser ||
        lifecycle == GoalLifecycle::BudgetExhausted ||
        lifecycle == GoalLifecycle::SuspendedByPolicy) {
        snapshot.stop_reason = "test";
    }
    if (lifecycle == GoalLifecycle::Blocked) snapshot.blocker_key = "cred";
    if (lifecycle == GoalLifecycle::AwaitingUser) snapshot.pending_question = "哪条路?";
    snapshot.counters.iterations_started = iterations;
    return snapshot;
}

}  // namespace

TEST_CASE("短码映射:lifecycle×phase -> 稳定短码") {
    using L = GoalLifecycle;
    using P = GoalPhase;
    CHECK(lubancode::app::GoalV3LifecycleCode(L::Preparing, P::Idle) == "run");
    CHECK(lubancode::app::GoalV3LifecycleCode(L::Active, P::Idle) == "run");
    CHECK(lubancode::app::GoalV3LifecycleCode(L::Active, P::Queued) == "run");
    CHECK(lubancode::app::GoalV3LifecycleCode(L::Active, P::Running) == "run");
    CHECK(lubancode::app::GoalV3LifecycleCode(L::Active, P::Evaluating) == "eval");
    CHECK(lubancode::app::GoalV3LifecycleCode(L::Waiting, P::Idle) == "wait");
    CHECK(lubancode::app::GoalV3LifecycleCode(L::Paused, P::Idle) == "pause");
    CHECK(lubancode::app::GoalV3LifecycleCode(L::AwaitingUser, P::Idle) == "pause");
    CHECK(lubancode::app::GoalV3LifecycleCode(L::SuspendedByPolicy, P::Idle) == "pause");
    CHECK(lubancode::app::GoalV3LifecycleCode(L::Blocked, P::Idle) == "blocked");
    CHECK(lubancode::app::GoalV3LifecycleCode(L::Achieved, P::Idle) == "done");
    CHECK(lubancode::app::GoalV3LifecycleCode(L::BudgetExhausted, P::Idle) == "budget");
    CHECK(lubancode::app::GoalV3LifecycleCode(L::Failed, P::Idle) == "x");
    CHECK(lubancode::app::GoalV3LifecycleCode(L::Cleared, P::Idle) == "x");
}

TEST_CASE("首行 = 三处同一投影:goalId·短码·r/c·iter") {
    const auto snapshot = SnapshotWith(GoalLifecycle::Waiting, GoalPhase::Idle);
    const std::string head = lubancode::app::BuildGoalV3HeadLine(snapshot);
    CHECK(head == "goal-1 · wait · r5 c2 · iter 3");
    // 未开轮不带 iter 段。
    const auto fresh = SnapshotWith(GoalLifecycle::Active, GoalPhase::Idle, 1, 1, 0);
    CHECK(lubancode::app::BuildGoalV3HeadLine(fresh) == "goal-1 · run · r1 c1");
}

TEST_CASE("状态栏段:v3 在场吃 v3,空指针回落 v1") {
    const auto snapshot = SnapshotWith(GoalLifecycle::Active, GoalPhase::Evaluating);
    // v3 在场:v1 coordinator 空也出段(短码 eval)。
    const std::string v3_segment =
        lubancode::app::BuildGoalLoopStatusSegment(&snapshot, nullptr, nullptr);
    CHECK(v3_segment == "goal eval·iter3·r5");
    // v3 为空:回落 v1 老折法(两样都没有给空串)。
    CHECK(lubancode::app::BuildGoalLoopStatusSegment(nullptr, nullptr, nullptr).empty());
}

TEST_CASE("FormatGoalV3Status:等待/巡检/停止意图/fork 来路如实") {
    goalns::GoalLineageProjection lineage;
    lineage.found = true;
    lineage.projection.has_goal = true;
    lineage.projection.gap = goalns::GoalProjectionGap::None;
    lineage.projection.goal_id = "goal-1";
    lineage.projection.session_id = "s1";

    SUBCASE("等待态带巡检账") {
        auto snapshot = SnapshotWith(GoalLifecycle::Waiting, GoalPhase::Idle);
        snapshot.wait_task_refs = {"subagent-3", "subagent-9"};
        snapshot.wait_plan.polls_done = 1;
        snapshot.wait_plan.max_polls = 3;
        snapshot.wait_plan.next_due_ms = 1700005400000;
        lineage.projection.snapshot = snapshot;
        const auto outcome = lubancode::app::FormatGoalV3Status(lineage);
        REQUIRE(outcome.ok);
        bool has_wait = false;
        bool has_poll = false;
        for (const auto& line : outcome.lines) {
            if (line.find("等待: 2 项后台任务") != std::string::npos) has_wait = true;
            if (line.find("巡检 1/3") != std::string::npos) has_poll = true;
        }
        CHECK(has_wait);
        CHECK(has_poll);
    }

    SUBCASE("巡检到上限:停排文案") {
        auto snapshot = SnapshotWith(GoalLifecycle::Waiting, GoalPhase::Idle);
        snapshot.wait_task_refs = {"subagent-3"};
        snapshot.wait_plan.polls_done = 3;
        snapshot.wait_plan.max_polls = 3;
        snapshot.wait_plan.next_due_ms = 0;
        lineage.projection.snapshot = snapshot;
        const auto outcome = lubancode::app::FormatGoalV3Status(lineage);
        REQUIRE(outcome.ok);
        bool has_cap = false;
        for (const auto& line : outcome.lines) {
            if (line.find("巡检到上限") != std::string::npos) has_cap = true;
        }
        CHECK(has_cap);
    }

    SUBCASE("停止意图在账:明示") {
        auto snapshot = SnapshotWith(GoalLifecycle::Active, GoalPhase::Idle);
        snapshot.stop_requested = true;
        lineage.projection.snapshot = snapshot;
        const auto outcome = lubancode::app::FormatGoalV3Status(lineage);
        REQUIRE(outcome.ok);
        bool has_stop = false;
        for (const auto& line : outcome.lines) {
            if (line.find("停止意图在账") != std::string::npos) has_stop = true;
        }
        CHECK(has_stop);
    }

    SUBCASE("fork 来路:parent 明示") {
        auto snapshot = SnapshotWith(GoalLifecycle::Paused, GoalPhase::Idle);
        snapshot.parent_goal_id = "goal-0";
        lineage.projection.snapshot = snapshot;
        const auto outcome = lubancode::app::FormatGoalV3Status(lineage);
        REQUIRE(outcome.ok);
        bool has_parent = false;
        for (const auto& line : outcome.lines) {
            if (line.find("fork 自 goal-0") != std::string::npos) has_parent = true;
        }
        CHECK(has_parent);
    }
}

TEST_CASE("/goal resume 预算增量解析:三键认得,坏键坏值 Invalid") {
    using lubancode::cli::GoalCommandAction;
    using lubancode::cli::ParseGoalCommand;

    // 纯 resume:不带增量。
    {
        const auto parsed = ParseGoalCommand("resume");
        REQUIRE(parsed.action == GoalCommandAction::Resume);
        CHECK_FALSE(parsed.has_budget_addition());
    }
    // 三键齐上:分钟换毫秒落账。
    {
        const auto parsed = ParseGoalCommand("resume iterations=30 tokens=200000 elapsed_min=90");
        REQUIRE(parsed.action == GoalCommandAction::Resume);
        REQUIRE(parsed.has_budget_addition());
        CHECK(*parsed.budget_iterations == 30);
        CHECK(*parsed.budget_tokens == 200000);
        CHECK(*parsed.budget_elapsed_ms == 90LL * 60 * 1000);
    }
    // 单键也成。
    {
        const auto parsed = ParseGoalCommand("resume iterations=5");
        REQUIRE(parsed.action == GoalCommandAction::Resume);
        CHECK(*parsed.budget_iterations == 5);
        CHECK_FALSE(parsed.budget_tokens.has_value());
    }
    // 坏键/坏值/非 key=value 一律 Invalid(不静默吞)。
    CHECK(ParseGoalCommand("resume rounds=3").action == GoalCommandAction::Invalid);
    CHECK(ParseGoalCommand("resume iterations=abc").action == GoalCommandAction::Invalid);
    CHECK(ParseGoalCommand("resume tokens=-5").action == GoalCommandAction::Invalid);
    CHECK(ParseGoalCommand("resume 30").action == GoalCommandAction::Invalid);
    // Create/Edit 不受影响。
    CHECK(ParseGoalCommand("修好登录页").action == GoalCommandAction::Create);
    CHECK(ParseGoalCommand("edit 修好登录页与注册页").action == GoalCommandAction::Edit);
}
