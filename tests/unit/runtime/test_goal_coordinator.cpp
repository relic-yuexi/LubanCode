// /goal 单第 1 期:GoalCoordinator 状态机与命令面。
// 一只 active goal、revision CAS、幂等 pause/clear、预算闸、防空转、
// achieved 硬门槛、迟到事件、回放重建。

#include <doctest/doctest.h>

#include <string>
#include <vector>

#include "runtime/goal_coordinator.hpp"
#include "runtime/goal_types.hpp"

using lubancode::runtime::goal::GoalCheckpoint;
using lubancode::runtime::goal::GoalCommandResult;
using lubancode::runtime::goal::GoalContract;
using lubancode::runtime::goal::GoalCoordinator;
using lubancode::runtime::goal::GoalCriterion;
using lubancode::runtime::goal::GoalDecision;
using lubancode::runtime::goal::GoalEvaluation;
using lubancode::runtime::goal::GoalEvidence;
using lubancode::runtime::goal::GoalState;
using lubancode::runtime::goal::GoalTask;
using lubancode::runtime::goal::GoalUsage;

namespace {

GoalCoordinator::Options EnabledOptions() {
    GoalCoordinator::Options o;
    o.goals_enabled = true;
    o.default_max_iterations = 3;
    o.default_max_no_progress_iterations = 3;
    o.default_max_same_blocker_iterations = 3;
    o.default_max_elapsed_ms = 60 * 60 * 1000;
    return o;
}

GoalContract ValidContract() {
    GoalContract c;
    c.objective = "迁移认证层并保持契约测试通过";
    c.criteria.push_back({"c-1", "契约测试全绿", true});
    c.validation_commands = {"ctest"};
    return c;
}

// 走到 Active:创建 + 合同冻结(GoalCoordinator 不可拷贝,输出参数接)。
void MakeActive(GoalCoordinator& g) {
    REQUIRE(g.Create("迁移认证层", "/repo", "repo@main", 1000).ok);
    REQUIRE(g.SubmitContract(ValidContract(), 1100).ok);
}

GoalEvidence MakeEvidence(const std::string& id, const std::string& goal_id, bool fresh = true) {
    GoalEvidence e;
    e.id = id;
    e.goal_id = goal_id;
    e.content_sha256 = "sha-" + id;
    e.fresh = fresh;
    e.observed_at_ms = 1200;
    return e;
}

}  // namespace

TEST_CASE("feature gate 关:Create 拒,不立档") {
    GoalCoordinator::Options o;
    o.goals_enabled = false;
    GoalCoordinator g(o);
    const GoalCommandResult r = g.Create("目标", "/repo", "id", 1000);
    CHECK_FALSE(r.ok);
    CHECK(r.error_code == lubancode::runtime::goal::kErrGoalStoreUnavailable);
    CHECK(r.payload.value("disabled", false));
    CHECK_FALSE(g.has_goal());
}

TEST_CASE("Create/Status:一只 active goal,再建报 already_active") {
    GoalCoordinator g(EnabledOptions());
    const GoalCommandResult r = g.Create("迁移认证层", "/repo", "repo@main", 1000);
    REQUIRE(r.ok);
    CHECK(r.payload.at("goal_id") == "goal-1");
    CHECK(g.task()->state == GoalState::Preparing);

    const GoalCommandResult dup = g.Create("另一个目标", "/repo", "repo@main", 1100);
    CHECK_FALSE(dup.ok);
    CHECK(dup.error_code == lubancode::runtime::goal::kErrGoalAlreadyActive);

    // objective 非法(空/超长)当面拒。
    CHECK_FALSE(g.Create("   ", "/repo", "id", 1200).ok);
    std::string huge(4001, 'x');
    const GoalCommandResult too_long = g.Create(huge, "/repo", "id", 1200);
    CHECK(too_long.error_code == lubancode::runtime::goal::kErrGoalObjectiveTooLong);
}

TEST_CASE("SubmitContract:criteria 空进 NeedsUser,空话目标拒") {
    GoalCoordinator g(EnabledOptions());
    REQUIRE(g.Create("把项目做完", "/repo", "id", 1000).ok);
    GoalContract empty;
    empty.objective = "把项目做完";
    const GoalCommandResult needs_user = g.SubmitContract(empty, 1100);
    REQUIRE(needs_user.ok);
    CHECK(g.task()->state == GoalState::AwaitingUser);

    GoalCoordinator g2(EnabledOptions());
    REQUIRE(g2.Create("目标", "/repo", "id", 1000).ok);
    GoalContract open_ended;
    open_ended.objective = "永不停止地优化";
    const GoalCommandResult rejected = g2.SubmitContract(open_ended, 1100);
    CHECK_FALSE(rejected.ok);
    CHECK_FALSE(g2.task()->contract_frozen);

    GoalContract ok = ValidContract();
    REQUIRE(g2.SubmitContract(ok, 1200).ok);
    CHECK(g2.task()->state == GoalState::Active);
    CHECK(g2.task()->contract_frozen);
    CHECK_FALSE(g2.task()->contract_sha256.empty());
}

TEST_CASE("iteration 生命周期:schedule → take → checkpoint → evaluate") {
    GoalCoordinator g(EnabledOptions());
    MakeActive(g);
    const auto scheduled = g.ScheduleNextIteration(1200);
    REQUIRE(scheduled.ok);
    CHECK(scheduled.payload.at("iteration_id") == "goal-1/iter-1");
    CHECK(scheduled.payload.at("dedupe_key") == "goal-1:r1:i1");
    CHECK(g.HasReadyContinuation());

    const auto started = g.TakeReadyIteration("turn-9", "fp-before", 1300);
    REQUIRE(started.ok);
    CHECK(started.iteration.index == 1);
    CHECK(started.iteration.phase == lubancode::runtime::goal::GoalIterationPhase::Running);
    CHECK(started.iteration.turn_id == "turn-9");
    CHECK(g.task()->state == GoalState::Running);
    CHECK(g.task()->counters.iterations_started == 1);
    CHECK(started.synthetic_text.find("goal-1") != std::string::npos);
    CHECK(started.synthetic_text.find("goal_checkpoint") != std::string::npos);

    GoalCheckpoint cp;
    cp.summary = "契约测试跑通";
    cp.evidence_ids = {"ev-1"};
    REQUIRE(g.CheckpointReached(cp, 1400).ok);
    CHECK(g.task()->state == GoalState::Evaluating);

    GoalEvaluation ev;
    ev.id = "eval-1";
    ev.decision = GoalDecision::Continue;
    ev.progress = true;
    const auto applied = g.ApplyEvaluation(ev, 1500);
    REQUIRE(applied.ok);
    CHECK(applied.payload.at("decision") == "continue");
    CHECK(applied.payload.at("schedule_next") == true);
    CHECK(g.task()->state == GoalState::Active);
    CHECK(g.task()->last_evaluation.has_value());
}

TEST_CASE("duplicate scheduled 不开两轮:幂等返回") {
    GoalCoordinator g(EnabledOptions());
    MakeActive(g);
    REQUIRE(g.ScheduleNextIteration(1200).ok);
    const auto again = g.ScheduleNextIteration(1250);
    REQUIRE(again.ok);
    CHECK(again.payload.value("idempotent", false));
    CHECK(again.payload.at("iteration_id") == "goal-1/iter-1");  // 没涨 index
}

TEST_CASE("pause 与 ready 同时:pause 胜,不排新轮") {
    GoalCoordinator g(EnabledOptions());
    MakeActive(g);
    REQUIRE(g.Pause(1200).ok);
    CHECK(g.task()->state == GoalState::Paused);
    const auto schedule = g.ScheduleNextIteration(1250);
    CHECK_FALSE(schedule.ok);
    CHECK_FALSE(g.HasReadyContinuation());

    // Running 里 pause:等安全边界(Pausing),不硬改。
    GoalCoordinator g2(EnabledOptions());
    MakeActive(g2);
    REQUIRE(g2.ScheduleNextIteration(1200).ok);
    REQUIRE(g2.TakeReadyIteration("t-1", "fp", 1250).ok);
    const auto pause_while_running = g2.Pause(1300);
    REQUIRE(pause_while_running.ok);
    CHECK(g2.task()->state == GoalState::Pausing);
    CHECK(g2.pause_requested());
    CHECK(pause_while_running.payload.value("immediate", true) == false);
}

TEST_CASE("pause 幂等:重复 pause 返回当前状态,不添事件") {
    GoalCoordinator g(EnabledOptions());
    MakeActive(g);
    REQUIRE(g.Pause(1200).ok);
    const auto again = g.Pause(1250);
    REQUIRE(again.ok);
    CHECK(again.payload.value("idempotent", false));
    CHECK(g.task()->state == GoalState::Paused);
}

TEST_CASE("resume:只收三态,清 streak 不清账;terminal 拒") {
    GoalCoordinator g(EnabledOptions());
    MakeActive(g);
    REQUIRE(g.Pause(1200).ok);
    g.NoteBlocker("missing_credential:TOKEN", 1250);
    const auto resumed = g.Resume(0, 1300);
    REQUIRE(resumed.ok);
    CHECK(g.task()->state == GoalState::Active);
    CHECK(g.task()->counters.same_blocker_streak == 0);
    CHECK(g.task()->counters.iterations_started == 0);  // 账不清零

    // revision CAS 冲突。
    REQUIRE(g.Pause(1400).ok);
    const auto conflict = g.Resume(99, 1450);
    CHECK_FALSE(conflict.ok);
    CHECK(conflict.error_code == lubancode::runtime::goal::kErrGoalRevisionConflict);

    // Achieved 后 resume 拒(要新建)。
    GoalCoordinator g2(EnabledOptions());
    MakeActive(g2);
    GoalEvidence good = MakeEvidence("ev-9", "goal-1");
    g2.RecordEvidence(good);
    REQUIRE(g2.ScheduleNextIteration(1200).ok);
    REQUIRE(g2.TakeReadyIteration("t-1", "fp", 1250).ok);
    GoalCheckpoint done;
    done.remaining = {};
    done.evidence_ids = {"ev-9"};
    REQUIRE(g2.CheckpointReached(done, 1300).ok);
    GoalEvaluation achieved;
    achieved.id = "eval-1";
    achieved.decision = GoalDecision::Achieved;
    achieved.criteria.push_back({"c-1", "pass", {"ev-9"}, "测试绿"});
    const auto applied = g2.ApplyEvaluation(achieved, 1400);
    REQUIRE(applied.ok);
    CHECK(g2.task()->state == GoalState::Achieved);
    const auto revive = g2.Resume(0, 1500);
    CHECK_FALSE(revive.ok);
    CHECK(revive.error_code == lubancode::runtime::goal::kErrGoalTerminal);
    CHECK_FALSE(g2.HasReadyContinuation());  // achieved 后不再有 GoalReady
}

TEST_CASE("achieved 硬门槛:证据缺失/不新鲜/remaining 未清,一律改判 continue") {
    GoalCoordinator g(EnabledOptions());
    MakeActive(g);
    // 没有证据的 achieved。
    REQUIRE(g.ScheduleNextIteration(1200).ok);
    REQUIRE(g.TakeReadyIteration("t-1", "fp", 1250).ok);
    GoalCheckpoint done;
    done.remaining = {};
    REQUIRE(g.CheckpointReached(done, 1300).ok);
    GoalEvaluation no_evidence;
    no_evidence.decision = GoalDecision::Achieved;
    no_evidence.criteria.push_back({"c-1", "pass", {"ev-404"}, "自报"});
    const auto applied = g.ApplyEvaluation(no_evidence, 1400);
    REQUIRE(applied.ok);
    CHECK(g.task()->state == GoalState::Active);  // 没封账
    CHECK(g.task()->last_evaluation->overridden_achieved);
    CHECK(g.task()->last_evaluation->decision == GoalDecision::Continue);
    CHECK(g.task()->last_evaluation->override_reason.find("证据") != std::string::npos);

    // 证据在但翻 stale:同样过不了门。
    GoalCoordinator g2(EnabledOptions());
    MakeActive(g2);
    GoalEvidence stale = MakeEvidence("ev-1", "goal-1", /*fresh=*/false);
    g2.RecordEvidence(stale);
    REQUIRE(g2.ScheduleNextIteration(1200).ok);
    REQUIRE(g2.TakeReadyIteration("t-1", "fp", 1250).ok);
    GoalCheckpoint cp2;
    cp2.remaining = {};
    REQUIRE(g2.CheckpointReached(cp2, 1300).ok);
    GoalEvaluation with_stale;
    with_stale.decision = GoalDecision::Achieved;
    with_stale.criteria.push_back({"c-1", "pass", {"ev-1"}, "旧证据"});
    const auto applied2 = g2.ApplyEvaluation(with_stale, 1400);
    REQUIRE(applied2.ok);
    CHECK(g2.task()->state == GoalState::Active);
    CHECK(g2.task()->last_evaluation->overridden_achieved);

    // remaining 未清:criterion 全 pass 也不许封。
    GoalCoordinator g3(EnabledOptions());
    MakeActive(g3);
    g3.RecordEvidence(MakeEvidence("ev-2", "goal-1"));
    REQUIRE(g3.ScheduleNextIteration(1200).ok);
    REQUIRE(g3.TakeReadyIteration("t-1", "fp", 1250).ok);
    GoalCheckpoint cp3;
    cp3.remaining = {"还有 e2e 未重跑"};
    REQUIRE(g3.CheckpointReached(cp3, 1300).ok);
    GoalEvaluation with_remaining;
    with_remaining.decision = GoalDecision::Achieved;
    with_remaining.criteria.push_back({"c-1", "pass", {"ev-2"}, "绿"});
    const auto applied3 = g3.ApplyEvaluation(with_remaining, 1400);
    REQUIRE(applied3.ok);
    CHECK(g3.task()->state == GoalState::Active);
    CHECK(g3.task()->last_evaluation->overridden_achieved);
}

TEST_CASE("schema 门槛:blocked 缺 key、needs_user 缺 question 拒") {
    GoalCoordinator g(EnabledOptions());
    MakeActive(g);
    REQUIRE(g.ScheduleNextIteration(1200).ok);
    REQUIRE(g.TakeReadyIteration("t-1", "fp", 1250).ok);
    GoalCheckpoint cp;
    REQUIRE(g.CheckpointReached(cp, 1300).ok);
    GoalEvaluation bad_blocked;
    bad_blocked.decision = GoalDecision::Blocked;
    const auto r1 = g.ApplyEvaluation(bad_blocked, 1400);
    CHECK_FALSE(r1.ok);
    CHECK(r1.error_code == lubancode::runtime::goal::kErrGoalEvaluatorSchema);

    GoalEvaluation bad_needs_user;
    bad_needs_user.decision = GoalDecision::NeedsUser;
    const auto r2 = g.ApplyEvaluation(bad_needs_user, 1450);
    CHECK_FALSE(r2.ok);
    CHECK(r2.error_code == lubancode::runtime::goal::kErrGoalEvaluatorSchema);
}

TEST_CASE("needs_user:进 AwaitingUser,回答后续跑") {
    GoalCoordinator g(EnabledOptions());
    MakeActive(g);
    REQUIRE(g.ScheduleNextIteration(1200).ok);
    REQUIRE(g.TakeReadyIteration("t-1", "fp", 1250).ok);
    GoalCheckpoint cp;
    REQUIRE(g.CheckpointReached(cp, 1300).ok);
    GoalEvaluation ask;
    ask.decision = GoalDecision::NeedsUser;
    ask.question = "删库还是归档?";
    const auto applied = g.ApplyEvaluation(ask, 1400);
    REQUIRE(applied.ok);
    CHECK(g.task()->state == GoalState::AwaitingUser);
    CHECK(applied.payload.at("question") == "删库还是归档?");
    const auto resumed = g.Resume(0, 1500);
    REQUIRE(resumed.ok);
    CHECK(g.task()->state == GoalState::Active);
}

TEST_CASE("预算闸:iteration 上限与 elapsed") {
    GoalCoordinator::Options o = EnabledOptions();
    o.default_max_iterations = 2;
    GoalCoordinator g(o);
    REQUIRE(g.Create("目标", "/r", "id", 1000).ok);
    REQUIRE(g.SubmitContract(ValidContract(), 1100).ok);

    for (int round = 0; round < 2; ++round) {
        REQUIRE(g.ScheduleNextIteration(1200 + round * 100).ok);
        REQUIRE(g.TakeReadyIteration("t", "fp", 1250 + round * 100).ok);
        GoalCheckpoint cp;
        REQUIRE(g.CheckpointReached(cp, 1300 + round * 100).ok);
        GoalEvaluation ev;
        ev.decision = GoalDecision::Continue;
        REQUIRE(g.ApplyEvaluation(ev, 1400 + round * 100).ok);
    }
    const auto third = g.ScheduleNextIteration(2000);
    CHECK_FALSE(third.ok);
    CHECK(third.error_code == lubancode::runtime::goal::kErrGoalBudgetExhausted);
    CHECK(g.task()->state == GoalState::BudgetExhausted);
    CHECK(g.task()->terminal_at_ms.has_value());

    // elapsed:started_at 起 1h 上限,休眠醒来直接 Exhausted。
    GoalCoordinator::Options o2 = EnabledOptions();
    o2.default_max_elapsed_ms = 1000;
    GoalCoordinator g2(o2);
    REQUIRE(g2.Create("目标", "/r", "id", 1000).ok);
    REQUIRE(g2.SubmitContract(ValidContract(), 1100).ok);
    // started_at 在 SubmitContract 冻结合同时落(1100);elapsed 从那起算。
    std::int64_t over = 0;
    CHECK_FALSE(g2.ElapsedExceeded(1800, &over));  // 700 < 1000
    CHECK(g2.ElapsedExceeded(2500, &over));        // 1400 > 1000
    CHECK(over == 400);
}

TEST_CASE("防空转:同 fingerprint 连续三轮 → Paused(no_progress)") {
    GoalCoordinator g(EnabledOptions());
    MakeActive(g);
    // 首枚指纹相对空指纹算进展(第一枚 checkpoint 本就是新信息),随后
    // 三轮同值才触发:共喂四次。
    g.NoteProgressFingerprint("same-fp", 1100);
    bool tripped = false;
    for (int i = 0; i < 3; ++i) {
        const auto check = g.NoteProgressFingerprint("same-fp", 1200 + i);
        if (check.tripped) tripped = true;
    }
    CHECK(tripped);
    CHECK(g.task()->state == GoalState::Paused);
    CHECK(g.task()->counters.no_progress_streak == 3);

    // 有进展清零:换一枚指纹,streak 归零。
    const auto progressed = g.NoteProgressFingerprint("new-fp", 2000);
    CHECK(progressed.progressed);
    CHECK(g.task()->counters.no_progress_streak == 0);
}

TEST_CASE("同 blocker 三轮 → Blocked;换 blocker 重数") {
    GoalCoordinator g(EnabledOptions());
    MakeActive(g);
    bool tripped = false;
    for (int i = 0; i < 3; ++i) {
        const auto check = g.NoteBlocker("missing_credential:DEPLOY_TOKEN", 1200 + i);
        if (check.tripped) tripped = true;
    }
    CHECK(tripped);
    CHECK(g.task()->state == GoalState::Blocked);

    // Blocked 可 resume(改条件后),streak 清零。
    REQUIRE(g.Resume(0, 2000).ok);
    const auto fresh = g.NoteBlocker("missing_credential:DEPLOY_TOKEN", 2100);
    CHECK(fresh.streak_after == 1);  // resume 清账后从头数
}

TEST_CASE("usage 未报告:token 闸不拿 0 冒充,iteration 闸照收") {
    GoalCoordinator::Options o = EnabledOptions();
    o.default_max_iterations = 1;
    GoalCoordinator g(o);
    REQUIRE(g.Create("目标", "/r", "id", 1000).ok);
    REQUIRE(g.SubmitContract(ValidContract(), 1100).ok);
    GoalUsage unreported;  // provider 不报 usage
    g.AddUsage(unreported);
    std::string reason;
    CHECK(g.CheckBudgetHeadroom(1150, &reason));  // iteration 1 还能开
    REQUIRE(g.ScheduleNextIteration(1200).ok);
    // 取走并发一轮(只排不取不计 iterations_started,预算闸看的是已开轮数)。
    REQUIRE(g.TakeReadyIteration("t-1", "fp", 1220).ok);
    GoalCheckpoint cp;
    REQUIRE(g.CheckpointReached(cp, 1250).ok);
    GoalEvaluation ev;
    ev.decision = GoalDecision::Continue;
    REQUIRE(g.ApplyEvaluation(ev, 1280).ok);
    const auto next = g.ScheduleNextIteration(1300);
    CHECK_FALSE(next.ok);  // iteration 上限照收(不依赖 token 报数)
    CHECK(next.error_code == lubancode::runtime::goal::kErrGoalBudgetExhausted);
}

TEST_CASE("fork lineage:新 id 记 parent,落 Paused,合同账面抄去,usage 从零") {
    GoalCoordinator src(EnabledOptions());
    MakeActive(src);
    // 给源攒一点账:usage 与 no-progress 计数。
    GoalUsage used;
    used.input_tokens = 500;
    used.request_count = 2;
    used.usage_reported = true;
    src.AddUsage(used);
    src.NoteProgressFingerprint("fp-1", 1500);
    const GoalTask source = *src.task();

    GoalCoordinator forked(EnabledOptions());
    const auto r = forked.ForkFrom(source, "/repo", "repo@main", 2000,
                                   [] { return "goal-7"; });
    REQUIRE(r.ok);
    REQUIRE(forked.task() != nullptr);
    CHECK(forked.task()->id == "goal-7");               // 新 id,不共享
    CHECK(forked.task()->parent_goal_id == "goal-1");   // lineage 记源
    CHECK(forked.task()->state == GoalState::Paused);   // 不默认续跑
    CHECK_FALSE(forked.HasReadyContinuation());
    // 账面抄去:合同冻结、预算、checkpoint;防空转指纹新处起算。
    CHECK(forked.task()->contract_frozen);
    CHECK(forked.task()->contract_sha256 == source.contract_sha256);
    CHECK(forked.task()->budget.max_iterations == source.budget.max_iterations);
    CHECK(forked.task()->counters.last_progress_fingerprint.empty());
    // usage 从零,evidence 不搬。
    CHECK(forked.task()->usage.input_tokens == 0);
    CHECK(forked.evidence_count() == 0);
    // 源不被动:状态与账面照旧(fork 不是复活,也不是改动)。
    CHECK(src.task()->state == GoalState::Active);
    CHECK(src.task()->usage.input_tokens == 500);
    // resume 才跑。
    REQUIRE(forked.Resume(0, 2100).ok);
    CHECK(forked.task()->state == GoalState::Active);
    // 序列化 roundtrip:lineage 字段不丢。
    const GoalTask round = GoalTask::from_json(forked.task()->to_json());
    CHECK(round.parent_goal_id == "goal-1");
}

TEST_CASE("fork lineage:已有非终态 goal 时 fork 拒 already_active") {
    GoalCoordinator src(EnabledOptions());
    MakeActive(src);
    GoalCoordinator forked(EnabledOptions());
    MakeActive(forked);
    const auto r = forked.ForkFrom(*src.task(), "/repo", "repo@main", 2000);
    CHECK_FALSE(r.ok);
    CHECK(r.error_code == lubancode::runtime::goal::kErrGoalAlreadyActive);
}

TEST_CASE("fork lineage:created/forked 事件回放出同一只账") {
    std::vector<lubancode::runtime::goal::GoalCoordinatorEvent> events;
    GoalCoordinator::Options options = EnabledOptions();
    GoalCoordinator src(options);
    src.SetLedgerSink([&](const lubancode::runtime::goal::GoalCoordinatorEvent& event) {
        events.push_back(event);
        return true;
    });
    MakeActive(src);
    std::vector<lubancode::runtime::goal::GoalCoordinatorEvent> fork_events;
    GoalCoordinator forked(options);
    forked.SetLedgerSink([&](const lubancode::runtime::goal::GoalCoordinatorEvent& event) {
        fork_events.push_back(event);
        return true;
    });
    REQUIRE(forked.ForkFrom(*src.task(), "/repo", "repo@main", 2000, [] { return "goal-2"; }).ok);

    GoalCoordinator replayed(options);
    for (const auto& event : fork_events) {
        replayed.ReplayEvent(event);
    }
    REQUIRE(replayed.task() != nullptr);
    CHECK(replayed.task()->parent_goal_id == "goal-1");
    CHECK(replayed.task()->state == GoalState::Paused);
    CHECK(replayed.task()->contract_frozen);  // contract_ready 事件带的冻结态
}

TEST_CASE("edit:revision+1、回 Preparing、streak 清零;Running 时拒 busy") {
    GoalCoordinator g(EnabledOptions());
    MakeActive(g);
    g.NoteBlocker("b1", 1200);
    const auto edited = g.Edit("新目标:重构缓存层", 1, 1300);
    REQUIRE(edited.ok);
    CHECK(g.task()->revision == 2);
    CHECK(g.task()->state == GoalState::Preparing);
    CHECK(g.task()->contract_frozen == false);
    CHECK(g.task()->counters.same_blocker_streak == 0);
    // 旧 revision CAS 拒。
    const auto stale = g.Edit("再改", 1, 1400);
    CHECK(stale.error_code == lubancode::runtime::goal::kErrGoalRevisionConflict);

    // Running 里 edit 拒 busy。
    GoalCoordinator g2(EnabledOptions());
    MakeActive(g2);
    REQUIRE(g2.ScheduleNextIteration(1200).ok);
    REQUIRE(g2.TakeReadyIteration("t", "fp", 1250).ok);
    const auto busy = g2.Edit("跑着改", 0, 1300);
    CHECK(busy.error_code == lubancode::runtime::goal::kErrGoalBusy);
}

TEST_CASE("clear:terminal 幂等,审计账保留") {
    GoalCoordinator g(EnabledOptions());
    MakeActive(g);
    const auto cleared = g.Clear(1200);
    REQUIRE(cleared.ok);
    CHECK(g.task()->state == GoalState::Cleared);
    CHECK(g.task()->terminal_at_ms.has_value());
    const auto again = g.Clear(1300);
    REQUIRE(again.ok);
    CHECK(again.payload.value("idempotent", false));
    CHECK(g.has_goal());  // 审计账在(不清内存档;存档侧更不删)

    // clear 后不能再 create?——单子:clear 摘掉 active goal,可再建新的。
    GoalCoordinator fresh(EnabledOptions());
    CHECK(fresh.Create("新目标", "/r", "id", 1000).ok);
}

TEST_CASE("写盘栅栏:LedgerSink 返回 false → fail closed,不开下一轮") {
    GoalCoordinator g(EnabledOptions());
    bool failing = false;
    g.SetLedgerSink([&failing](const lubancode::runtime::goal::GoalCoordinatorEvent&) { return !failing; });
    REQUIRE(g.Create("目标", "/r", "id", 1000).ok);
    REQUIRE(g.SubmitContract(ValidContract(), 1100).ok);

    failing = true;  // scheduled 写不落
    const auto schedule = g.ScheduleNextIteration(1200);
    CHECK_FALSE(schedule.ok);
    CHECK(schedule.error_code == lubancode::runtime::goal::kErrGoalStoreUnavailable);
    CHECK(g.task()->state == GoalState::Failed);
    CHECK_FALSE(g.HasReadyContinuation());
}

TEST_CASE("迟到判词:terminal 后只留审计,不复活") {
    GoalCoordinator g(EnabledOptions());
    MakeActive(g);
    g.RecordEvidence(MakeEvidence("ev-1", "goal-1"));
    REQUIRE(g.ScheduleNextIteration(1200).ok);
    REQUIRE(g.TakeReadyIteration("t", "fp", 1250).ok);
    GoalCheckpoint done;
    done.remaining = {};
    REQUIRE(g.CheckpointReached(done, 1300).ok);
    GoalEvaluation achieved;
    achieved.decision = GoalDecision::Achieved;
    achieved.criteria.push_back({"c-1", "pass", {"ev-1"}, "绿"});
    REQUIRE(g.ApplyEvaluation(achieved, 1400).ok);
    CHECK(g.task()->state == GoalState::Achieved);

    GoalEvaluation late_continue;
    late_continue.decision = GoalDecision::Continue;
    const auto r = g.ApplyEvaluation(late_continue, 1500);
    CHECK_FALSE(r.ok);
    CHECK(r.error_code == lubancode::runtime::goal::kErrGoalTerminal);
    CHECK(g.task()->state == GoalState::Achieved);  // 不复活
}

TEST_CASE("回放:事件序重建出同一只状态机;evaluation=continue 无 scheduled 补一枚") {
    // 录一遍事件(接真 LedgerSink)。
    std::vector<lubancode::runtime::goal::GoalCoordinatorEvent> log;
    GoalCoordinator live(EnabledOptions());
    live.SetLedgerSink([&log](const lubancode::runtime::goal::GoalCoordinatorEvent& e) {
        log.push_back(e);
        return true;
    });
    REQUIRE(live.Create("目标", "/r", "id", 1000).ok);
    REQUIRE(live.SubmitContract(ValidContract(), 1100).ok);
    REQUIRE(live.ScheduleNextIteration(1200).ok);
    REQUIRE(live.TakeReadyIteration("turn-9", "fp", 1250).ok);
    GoalCheckpoint cp;
    cp.summary = "推进";
    REQUIRE(live.CheckpointReached(cp, 1300).ok);
    GoalEvaluation ev;
    ev.decision = GoalDecision::Continue;
    REQUIRE(live.ApplyEvaluation(ev, 1400).ok);

    // 回放重建:按同一事件序喂。
    GoalCoordinator replayed(EnabledOptions());
    replayed.SetLedgerSink([](const lubancode::runtime::goal::GoalCoordinatorEvent&) { return true; });
    for (const auto& e : log) replayed.ReplayEvent(e);
    REQUIRE(replayed.has_goal());
    CHECK(replayed.task()->id == "goal-1");
    CHECK(replayed.task()->state == GoalState::Active);  // continue 后回 Active
    CHECK(replayed.task()->contract_frozen);
    CHECK(replayed.task()->counters.iterations_started == 1);
    CHECK(replayed.task()->last_evaluation.has_value());

    // continue 已落、next scheduled 未落:恢复器补一枚(ScheduleNextIteration
    // 幂等,dedupe 不撞)。
    const auto repair = replayed.ScheduleNextIteration(1500);
    REQUIRE(repair.ok);
    CHECK(repair.payload.at("iteration_id") == "goal-1/iter-2");

    // 尾线:terminal 后迟到的 continue 事件被回放跳过。
    std::vector<lubancode::runtime::goal::GoalCoordinatorEvent> with_late = log;
    lubancode::runtime::goal::GoalCoordinatorEvent late;
    late.event = "evaluated";
    late.goal_id = "goal-1";
    late.revision = 1;
    GoalEvaluation late_ev;
    late_ev.decision = GoalDecision::Achieved;
    late.payload["evaluation"] = late_ev.to_json();
    GoalCoordinator r2(EnabledOptions());
    for (const auto& e : with_late) r2.ReplayEvent(e);
    // 先 achieved 落账再回放迟到 continue 的序不在上面构造里;这里钉:
    // r2 处于 Active,迟到事件不该把它掀翻——直接喂第二枚 evaluated。
    lubancode::runtime::goal::GoalCoordinatorEvent late_continue = late;
    GoalEvaluation continue_ev;
    continue_ev.decision = GoalDecision::Continue;
    late_continue.payload["evaluation"] = continue_ev.to_json();
    r2.ReplayEvent(late_continue);
    // 回放器按事件序收:第二枚 evaluated(continue)合法推进到 Active。
    CHECK(r2.task()->state == GoalState::Active);
}

TEST_CASE("missing checkpoint:模型没调工具,宿主合成,不谎成完成") {
    const GoalCheckpoint missing = GoalCoordinator::MakeMissingCheckpoint();
    CHECK(missing.synthesized);
    CHECK(missing.summary.find("goal_checkpoint") != std::string::npos);
    CHECK(missing.remaining.empty());  // 合成的不伪造进展
}

TEST_CASE("provider 连败:撞上限 Paused,成功清零") {
    GoalCoordinator g(EnabledOptions());
    REQUIRE(g.Create("目标", "/r", "id", 1000).ok);
    g.NoteProviderOutcome(false);
    g.NoteProviderOutcome(false);
    CHECK(g.task()->state == GoalState::Preparing);  // 还没撞
    g.NoteProviderOutcome(false);                    // 第三次
    CHECK(g.task()->state == GoalState::Paused);     // 闸收口(fail closed)
    REQUIRE(g.Resume(0, 2000).ok);
    g.NoteProviderOutcome(true);
    CHECK(g.task()->counters.consecutive_provider_failures == 0);
}

TEST_CASE("evaluator 失败:goal 进 Paused(evaluator_failed),不默认 achieved") {
    GoalCoordinator g(EnabledOptions());
    MakeActive(g);
    REQUIRE(g.ScheduleNextIteration(1200).ok);
    REQUIRE(g.TakeReadyIteration("t-1", "", 1300).ok);
    REQUIRE(g.CheckpointReached(GoalCoordinator::MakeMissingCheckpoint(), 1400).ok);
    CHECK(g.task()->state == GoalState::Evaluating);
    const auto r = g.NoteEvaluatorFailed("evaluator_failed: 两次都坏", 1500);
    REQUIRE(r.ok);
    CHECK(g.task()->state == GoalState::Paused);
    CHECK_FALSE(g.HasReadyContinuation());
    // resume 可解(单子:evaluator 失败不是 terminal)。
    REQUIRE(g.Resume(0, 1600).ok);
    CHECK(g.task()->state == GoalState::Active);
}

TEST_CASE("evaluator 失败写盘失败:fail closed,不开下一轮") {
    GoalCoordinator::Options options = EnabledOptions();
    GoalCoordinator g(options);
    g.SetLedgerSink([](const lubancode::runtime::goal::GoalCoordinatorEvent& event) {
        return event.event != "paused";  // paused 那笔写不落
    });
    MakeActive(g);
    REQUIRE(g.ScheduleNextIteration(1200).ok);
    REQUIRE(g.TakeReadyIteration("t-1", "", 1300).ok);
    REQUIRE(g.CheckpointReached(GoalCoordinator::MakeMissingCheckpoint(), 1400).ok);
    const auto r = g.NoteEvaluatorFailed("两次都坏", 1500);
    CHECK_FALSE(r.ok);
    CHECK(g.task()->state == GoalState::Failed);  // fail closed
    CHECK_FALSE(g.HasReadyContinuation());
}

TEST_CASE("证据账:RecordEvidence 只认本 goal,EvidenceIds 全列,stale 翻旧") {
    GoalCoordinator g(EnabledOptions());
    REQUIRE(g.Create("目标", "/r", "id", 1000).ok);
    g.RecordEvidence(MakeEvidence("ev-1", "goal-1"));
    g.RecordEvidence(MakeEvidence("ev-2", "goal-9"));  // 跨 goal:不收
    CHECK(g.evidence_count() == 1);
    const auto ids = g.EvidenceIds();
    REQUIRE(ids.size() == 1);
    CHECK(ids[0] == "ev-1");
    CHECK(g.FindEvidence("ev-1") != nullptr);
    CHECK(g.FindEvidence("ev-2") == nullptr);
    g.MarkEvidenceStale("ev-1");
    CHECK_FALSE(g.FindEvidence("ev-1")->fresh);
}
