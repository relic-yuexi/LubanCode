// GoalService 测试(轨迹 v3 §4.67 G3:后台等待、预算预留、取消、
// compact/resume 守恒与 fork):
//   - 后台等待:EnterWaiting(active->waiting、taskRefs/巡检计划入快照、
//     goal.wait.registered 事实行)、巡检退避 30/60/120 与上限停排(计数
//     入快照,新写者接管不归零)、ResolveWaiting(waiting->active、收口位
//     恢复 running、goal.wait.resolved 事实行)、pause 后迟到交付拒
//     (kErrGoalNotWaiting,只留审计不拉起新轮);
//   - 停止意图:RequestStop 落旗后泵不认领(EvaluateGoalWork 拒)、
//     continue 判词撞停止意图照采但不续排(落 paused)、显式恢复清旗;
//   - 预算:开轮撞轮数帽落 budget_exhausted(旧费用保留)、continue 判词
//     撞帽不排下一轮、AddBudget 只抬帽不清账、token 帽对实报+预留
//     (ReserveBudget/EvaluateBudget/RecordGoalUsage 释放)、active 时长帽
//     (resume 不拿新计时器归零旧余额);
//   - usage 计费去重:RecordGoalUsage 同 requestId 第二次幂等(账上恰一
//     行、快照只加一次)、接管喂去重底、usage 复核缺口如实带出;
//   - fork:CreateForkedGoal 另发 goalId 默认 paused、证据全翻待复核、
//     原预算不带/usage 归零、parent_goal_id 记源;
//   - compact/resume 守恒:合同/计数/问题/预算/证据跨两次接管不丢
//     (compact 只改模型上下文,goal 快照与 applied 不动;resume-as-new
//     两次沿链各接管一次,快照字段 roundtrip)。
// G0/G1/G2 面在 test_goal_service*.cpp 不动;本册只钉 G3 面。
#include <doctest/doctest.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "runtime/goal_service.hpp"
#include "trajectory/v3/reader.hpp"
#include "trajectory/v3/writer.hpp"

namespace goalns = lubancode::runtime::goal;
using goalns::GoalLifecycle;
using goalns::GoalPendingIntent;
using goalns::GoalPhase;
using goalns::GoalService;
using goalns::GoalStateSnapshot;
using goalns::GoalTransitionCandidate;
using goalns::GoalUsage;
using goalns::GoalVerdictKind;
using lubancode::trajectory::v3::EventKindV3;
using lubancode::trajectory::v3::V3Writer;

namespace {

// ctest 钉 LUBANCODE_TRAJECTORY_V3_NEW_SESSIONS=0;v3 册显式开回 1(同款见
// test_goal_service.cpp)。
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

std::filesystem::path FreshDir(const std::string& tag) {
    const std::filesystem::path dir =
        std::filesystem::temp_directory_path() / ("lubancode-goal-g3-" + tag);
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    std::filesystem::create_directories(dir, ec);
    return dir;
}

// 可拨的固定钟(active 时长帽的账要走真墙钟差)。
std::int64_t g_now_ms = 1700000000000;
std::int64_t MutableClock() { return g_now_ms; }

struct Volume {
    std::filesystem::path dir;
    std::string session_id;
    std::optional<V3Writer> writer;

    Volume(const std::string& tag, const std::string& sid) : dir(FreshDir(tag)), session_id(sid) {
        auto started = V3Writer::Start(dir / (sid + ".jsonl"), sid, "run-" + sid, "system prompt");
        REQUIRE(started.has_value());
        writer = std::move(*started);
    }

    std::filesystem::path jsonl() const { return dir / (session_id + ".jsonl"); }
};

GoalService::Options ServiceOptionsFor(const std::filesystem::path& session_dir) {
    GoalService::Options options;
    options.session_dir = session_dir;
    options.clock = &MutableClock;
    return options;
}

GoalStateSnapshot DraftObjective(std::string objective) {
    GoalStateSnapshot draft;
    draft.objective = std::move(objective);
    draft.workspace_root = "/repo";
    draft.workspace_identity = "repo@main";
    return draft;
}

GoalPendingIntent FirstIntent() {
    GoalPendingIntent intent;
    intent.work_item_id = "wi-1";
    intent.contract_revision = 1;
    intent.predecessor_iteration_id = "";
    intent.continuation_ordinal = 1;
    return intent;
}

goalns::EvaluationVerdict ContinueVerdict(const std::string& predecessor) {
    goalns::EvaluationVerdict verdict;
    verdict.evaluation_id = "eval-goal-1/iter-1";
    verdict.kind = GoalVerdictKind::Continue;
    GoalPendingIntent intent;
    intent.work_item_id = "goal-1/wi-1";
    intent.contract_revision = 1;
    intent.predecessor_iteration_id = predecessor;
    intent.continuation_ordinal = 1;
    verdict.next_intent = intent;
    return verdict;
}

// 写一枚 session.json(lineage 走 ReadSessionJson;必选键按 schema,同 G1 册)。
void WriteSessionJson(const std::filesystem::path& session_dir, const std::string& session_id,
                      const std::string& start_reason, const std::string& previous_session_id) {
    nlohmann::json manifest = nlohmann::json::object();
    manifest["schema_version"] = 2;
    manifest["workspace_key"] = "wk-test";
    manifest["session_id"] = session_id;
    manifest["main_run_id"] = "run-" + session_id;
    manifest["status"] = "running";
    manifest["start_reason"] = start_reason;
    if (!previous_session_id.empty()) {
        manifest["previous_session_id"] = previous_session_id;
    }
    std::ofstream file(session_dir / "session.json", std::ios::binary | std::ios::trunc);
    file << manifest.dump();
}

// 建目标 + 认领 + 开轮(直进执行轮)。
void RunToRunning(GoalService& service, const char* objective = "修好 auth 模块") {
    GoalStateSnapshot draft = DraftObjective(objective);
    draft.contract.criteria.push_back({"c-1", "ctest -R auth 全过", true});
    draft.pending_intent = FirstIntent().ToJson();
    auto created = service.CreateGoal(std::move(draft), nlohmann::json{{"source", "test"}});
    REQUIRE(created.ok);
    auto claimed = service.ClaimPendingIntent("run-s1", created.payload.at("stateRevision"),
                                              nlohmann::json{{"source", "test"}});
    REQUIRE(claimed.ok);
    auto began = service.BeginIteration(claimed.payload.at("stateRevision"),
                                        nlohmann::json{{"source", "test"}});
    REQUIRE(began.ok);
}

int CountEvents(const Volume& volume, EventKindV3 kind) {
    const auto ledger = lubancode::trajectory::v3::ReadV3Ledger(volume.jsonl());
    REQUIRE(ledger.has_value());
    int count = 0;
    for (const auto& event : ledger->events) {
        if (event.kind == kind) ++count;
    }
    return count;
}

}  // namespace

// ---------------------------------------------------------------------------
// 后台等待(§4.67.7)
// ---------------------------------------------------------------------------

TEST_CASE("EnterWaiting:active->waiting,taskRefs/巡检计划入快照 + 事实行") {
    EnvGuard guard("LUBANCODE_TRAJECTORY_V3_NEW_SESSIONS", "1");
    Volume volume("wait-enter", "s1");
    GoalService service(&*volume.writer, ServiceOptionsFor(volume.dir));
    RunToRunning(service);

    auto waiting = service.EnterWaiting({"subagent-3", "subagent-7"},
                                         service.current()->state_revision,
                                         nlohmann::json{{"source", "test"}});
    REQUIRE(waiting.ok);
    const GoalStateSnapshot* snapshot = service.current();
    REQUIRE(snapshot != nullptr);
    CHECK(snapshot->lifecycle == GoalLifecycle::Waiting);
    CHECK(snapshot->phase == GoalPhase::Idle);  // 停态相位回 idle;iteration 原地保留
    REQUIRE(snapshot->iteration_id.has_value());
    CHECK_FALSE(snapshot->iteration_id->empty());
    REQUIRE(snapshot->wait_task_refs.size() == 2);
    CHECK(snapshot->wait_task_refs[0] == "subagent-3");
    CHECK(snapshot->wait_plan.polls_done == 0);
    CHECK(snapshot->wait_plan.max_polls == 3);
    CHECK(snapshot->wait_plan.next_due_ms ==
            g_now_ms + goalns::GoalWaitBackoffMs(0));  // 30 分钟退避起
    CHECK(goalns::GoalWaitBackoffMs(0) == 30LL * 60 * 1000);
    CHECK(goalns::GoalWaitBackoffMs(1) == 60LL * 60 * 1000);
    CHECK(goalns::GoalWaitBackoffMs(2) == 120LL * 60 * 1000);
    // 事实行:登记材料(taskRefs/去重键/巡检计划);生效看 applied。
    CHECK(CountEvents(volume, EventKindV3::GoalWaitRegistered) == 1);
    // 等待中泵不认领(§4.67.4"排验收前先走等待路径")。
    const auto view = goalns::EvaluateGoalWork(*snapshot, "run-s1");
    CHECK_FALSE(view.claimable);

    SUBCASE("空 taskRefs 拒(无关进程不进等待账)") {
        auto refused = service.EnterWaiting({}, service.current()->state_revision,
                                             nlohmann::json{{"source", "test"}});
        REQUIRE_FALSE(refused.ok);
        CHECK(refused.error_code == goalns::kErrGoalCandidateInvalid);
    }
}

TEST_CASE("巡检:退避三拍到上限停排,计数入快照重启不归零") {
    EnvGuard guard("LUBANCODE_TRAJECTORY_V3_NEW_SESSIONS", "1");
    Volume volume("wait-poll", "s1");
    GoalService service(&*volume.writer, ServiceOptionsFor(volume.dir));
    RunToRunning(service);
    REQUIRE(service.EnterWaiting({"subagent-3"}, service.current()->state_revision,
                                  nlohmann::json{{"source", "test"}})
                .ok);

    // 未到点不巡。
    CHECK_FALSE(service.WaitInspectionDue(g_now_ms + 1000));
    // 到点(30 分钟):第一拍,退避升 60 分钟。
    REQUIRE(service.WaitInspectionDue(g_now_ms + goalns::GoalWaitBackoffMs(0)));
    auto first = service.RecordWaitInspection(service.current()->state_revision,
                                              g_now_ms + goalns::GoalWaitBackoffMs(0),
                                              nlohmann::json{{"source", "test"}});
    REQUIRE(first.ok);
    CHECK(service.current()->wait_plan.polls_done == 1);
    CHECK_FALSE(first.payload.value("stopped", false));
    CHECK(service.current()->wait_plan.next_due_ms ==
          g_now_ms + goalns::GoalWaitBackoffMs(0) + goalns::GoalWaitBackoffMs(1));
    // 第二拍(再过 60 分钟)。
    const std::int64_t after_second = g_now_ms + goalns::GoalWaitBackoffMs(0) +
                                      goalns::GoalWaitBackoffMs(1) + goalns::GoalWaitBackoffMs(1);
    REQUIRE(service.WaitInspectionDue(after_second));
    REQUIRE(service.RecordWaitInspection(service.current()->state_revision, after_second,
                                         nlohmann::json{{"source", "test"}})
                .ok);
    // 第三拍:到上限,stopped 标 + nextDue 清零(目标仍 waiting,真实完成
    // 仍可唤醒)。
    const std::int64_t after_third = after_second + goalns::GoalWaitBackoffMs(2);
    REQUIRE(service.WaitInspectionDue(after_third));
    auto third = service.RecordWaitInspection(service.current()->state_revision, after_third,
                                              nlohmann::json{{"source", "test"}});
    REQUIRE(third.ok);
    CHECK(third.payload.value("stopped", false));
    CHECK(service.current()->wait_plan.polls_done == 3);
    CHECK(service.current()->wait_plan.next_due_ms == 0);
    CHECK_FALSE(service.WaitInspectionDue(after_third + 999999));
    // 上限后再记:拒。
    auto refused = service.RecordWaitInspection(service.current()->state_revision,
                                                after_third + 1, nlohmann::json{{"source", "test"}});
    REQUIRE_FALSE(refused.ok);
    CHECK(service.current()->lifecycle == GoalLifecycle::Waiting);

    // 重启不归零(§4.67.7"次数和 nextDue 入快照"):重新投影同一卷,
    // 巡检计数照旧、不再从 0 起(新写者接管吃的就是这份快照)。
    const auto ledger = lubancode::trajectory::v3::ReadV3Ledger(volume.jsonl());
    REQUIRE(ledger.has_value());
    const auto projection = goalns::ProjectGoalState(*ledger, volume.dir);
    REQUIRE(projection.gap == goalns::GoalProjectionGap::None);
    CHECK(projection.snapshot.wait_plan.polls_done == 3);
    CHECK(projection.snapshot.wait_plan.next_due_ms == 0);
}

TEST_CASE("ResolveWaiting:waiting->active,收口位恢复 running;迟到交付拒") {
    EnvGuard guard("LUBANCODE_TRAJECTORY_V3_NEW_SESSIONS", "1");
    Volume volume("wait-resolve", "s1");
    GoalService service(&*volume.writer, ServiceOptionsFor(volume.dir));
    RunToRunning(service);
    REQUIRE(service.EnterWaiting({"subagent-3"}, service.current()->state_revision,
                                  nlohmann::json{{"source", "test"}})
                .ok);

    // 真实完成通知唤醒:waiting->active;iteration 在途(意图已认领)恢复
    // running,收口可续(不重开轮)。
    auto resolved = service.ResolveWaiting("subagent-3", service.current()->state_revision,
                                           nlohmann::json{{"source", "test"}});
    REQUIRE(resolved.ok);
    const GoalStateSnapshot* snapshot = service.current();
    CHECK(snapshot->lifecycle == GoalLifecycle::Active);
    CHECK(snapshot->phase == GoalPhase::Running);  // 收口位等待恢复 running
    CHECK(snapshot->wait_task_refs.empty());
    CHECK(snapshot->wait_plan.next_due_ms == 0);
    CHECK(CountEvents(volume, EventKindV3::GoalWaitResolved) == 1);
    // 恢复后可排验收(BeginEvaluation 吃 phase=running)。
    auto began = service.BeginEvaluation(snapshot->state_revision, std::nullopt,
                                         std::vector<goalns::GoalEvidenceRef>{},
                                         std::vector<std::string>{},
                                         nlohmann::json{{"source", "test"}});
    REQUIRE(began.ok);

    SUBCASE("pause 之后的迟到后台报告:拒,只留审计不拉起新轮") {
        // 重新等一轮再 pause。
        REQUIRE(service.CompleteIterationWithEvaluation(
                    service.current()->state_revision, ContinueVerdict("goal-1/iter-1"),
                    nlohmann::json{{"source", "test"}})
                    .ok);
        REQUIRE(service.ClaimPendingIntent("run-s1", service.current()->state_revision,
                                           nlohmann::json{{"source", "test"}})
                    .ok);
        REQUIRE(service.BeginIteration(service.current()->state_revision,
                                       nlohmann::json{{"source", "test"}})
                    .ok);
        REQUIRE(service.EnterWaiting({"subagent-9"}, service.current()->state_revision,
                                     nlohmann::json{{"source", "test"}})
                    .ok);
        GoalTransitionCandidate pause;
        pause.goal_id = service.current()->goal_id;
        pause.expected_state_revision = service.current()->state_revision;
        pause.to_lifecycle = GoalLifecycle::Paused;
        pause.to_phase = GoalPhase::Idle;
        pause.stop_reason = "user_pause";
        REQUIRE(service.ApplyTransition(pause).ok);
        auto late = service.ResolveWaiting("subagent-9", service.current()->state_revision,
                                           nlohmann::json{{"source", "test"}});
        REQUIRE_FALSE(late.ok);
        CHECK(late.error_code == goalns::kErrGoalNotWaiting);
        CHECK(service.current()->lifecycle == GoalLifecycle::Paused);  // 停止意图优先
    }
}

// ---------------------------------------------------------------------------
// 取消与停止意图(§4.67.3 stopRequested / §4.67.10 竞态行)
// ---------------------------------------------------------------------------

TEST_CASE("RequestStop:旗落账泵不认领,continue 判词照采但不续排,恢复清旗") {
    EnvGuard guard("LUBANCODE_TRAJECTORY_V3_NEW_SESSIONS", "1");
    Volume volume("stop-flag", "s1");
    GoalService service(&*volume.writer, ServiceOptionsFor(volume.dir));
    RunToRunning(service);

    auto stopped = service.RequestStop(service.current()->state_revision,
                                       nlohmann::json{{"source", "test"}, {"reason", "esc"}});
    REQUIRE(stopped.ok);
    CHECK(service.current()->stop_requested);
    // 幂等:已置位再请求不空耗 revision。
    const std::uint64_t revision_before = service.current()->state_revision;
    auto again = service.RequestStop(revision_before, nlohmann::json{{"source", "test"}});
    REQUIRE(again.ok);
    CHECK(again.payload.value("idempotent", false));
    CHECK(service.current()->state_revision == revision_before);
    // 泵不认领:停止意图优先(EvaluateGoalWork 拒)。
    auto view = goalns::EvaluateGoalWork(*service.current(), "run-s1");
    CHECK_FALSE(view.claimable);

    // continue 判词:照采(evaluationId 落账),但不排下一轮——落 paused。
    auto began = service.BeginEvaluation(service.current()->state_revision, std::nullopt,
                                         std::vector<goalns::GoalEvidenceRef>{},
                                         std::vector<std::string>{},
                                         nlohmann::json{{"source", "test"}});
    REQUIRE(began.ok);
    auto closed = service.CompleteIterationWithEvaluation(
        began.payload.at("stateRevision"), ContinueVerdict("goal-1/iter-1"),
        nlohmann::json{{"source", "test"}});
    REQUIRE(closed.ok);
    CHECK(closed.payload.at("verdictKind") == "continue");
    CHECK(closed.payload.contains("parked"));
    CHECK_FALSE(closed.payload.contains("nextWorkItemId"));
    const GoalStateSnapshot* snapshot = service.current();
    CHECK(snapshot->lifecycle == GoalLifecycle::Paused);
    CHECK(snapshot->stop_reason.rfind("stop_requested", 0) == 0);
    REQUIRE(snapshot->applied_evaluation_id.has_value());  // 判词已采
    CHECK(snapshot->pending_intent.is_object() && snapshot->pending_intent.empty());

    // 显式恢复:paused->active 清旗(§4.67.3"明确续跑后才恢复")。
    GoalTransitionCandidate resume;
    resume.goal_id = snapshot->goal_id;
    resume.expected_state_revision = snapshot->state_revision;
    resume.to_lifecycle = GoalLifecycle::Active;
    resume.to_phase = GoalPhase::Idle;
    REQUIRE(service.ApplyTransition(resume).ok);
    CHECK_FALSE(service.current()->stop_requested);
}

// ---------------------------------------------------------------------------
// 预算(§4.67.7)
// ---------------------------------------------------------------------------

TEST_CASE("开轮撞轮数帽:落 budget_exhausted,AddBudget 后显式恢复") {
    EnvGuard guard("LUBANCODE_TRAJECTORY_V3_NEW_SESSIONS", "1");
    Volume volume("budget-iterations", "s1");
    GoalService service(&*volume.writer, ServiceOptionsFor(volume.dir));
    GoalStateSnapshot draft = DraftObjective("修好 auth");
    draft.contract.criteria.push_back({"c-1", "ctest 全过", true});
    draft.pending_intent = FirstIntent().ToJson();
    draft.budget.max_iterations = 1;
    auto created = service.CreateGoal(std::move(draft), nlohmann::json{{"source", "test"}});
    REQUIRE(created.ok);
    REQUIRE(service.ClaimPendingIntent("run-s1", created.payload.at("stateRevision"),
                                       nlohmann::json{{"source", "test"}})
                .ok);
    // 第 1 轮:过(max_iterations=1)。
    REQUIRE(service.BeginIteration(service.current()->state_revision,
                                   nlohmann::json{{"source", "test"}})
                .ok);
    // 收口排下一轮:continue 撞帽(下一轮 index 2 > 1)——判词照采但不排,
    // 落 budget_exhausted。
    auto began = service.BeginEvaluation(service.current()->state_revision, std::nullopt,
                                         std::vector<goalns::GoalEvidenceRef>{},
                                         std::vector<std::string>{},
                                         nlohmann::json{{"source", "test"}});
    REQUIRE(began.ok);
    auto closed = service.CompleteIterationWithEvaluation(
        began.payload.at("stateRevision"), ContinueVerdict("goal-1/iter-1"),
        nlohmann::json{{"source", "test"}});
    REQUIRE(closed.ok);
    CHECK(service.current()->lifecycle == GoalLifecycle::BudgetExhausted);
    CHECK(service.current()->stop_reason.rfind("budget_exhausted", 0) == 0);

    // 旧费用保留:usage 不因停态清账;AddBudget 只抬帽。
    GoalUsage spent;
    spent.input_tokens = 100;
    spent.usage_reported = true;
    auto recorded = service.RecordGoalUsage("req-1", "execution", spent,
                                            service.current()->state_revision,
                                            nlohmann::json{{"source", "test"}});
    REQUIRE(recorded.ok);
    goalns::GoalBudgetAddition addition;
    addition.iterations = 5;
    auto added = service.AddBudget(addition, service.current()->state_revision,
                                   nlohmann::json{{"source", "test"}});
    REQUIRE(added.ok);
    CHECK(added.payload.at("maxIterations") == 5);
    CHECK(service.current()->budget.max_iterations.has_value());
    CHECK(*service.current()->budget.max_iterations == 5);
    CHECK(service.current()->usage.input_tokens == 100);  // 旧费用保留

    // 显式恢复(budget_exhausted -> active):复核后可推进。
    GoalTransitionCandidate resume;
    resume.goal_id = service.current()->goal_id;
    resume.expected_state_revision = service.current()->state_revision;
    resume.to_lifecycle = GoalLifecycle::Active;
    resume.to_phase = GoalPhase::Idle;
    REQUIRE(service.ApplyTransition(resume).ok);
    CHECK(service.current()->lifecycle == GoalLifecycle::Active);

    SUBCASE("空增量拒") {
        goalns::GoalBudgetAddition empty;
        auto refused = service.AddBudget(empty, service.current()->state_revision,
                                         nlohmann::json{{"source", "test"}});
        REQUIRE_FALSE(refused.ok);
    }
}

TEST_CASE("token 帽:实报 + 预留共用余额,RecordGoalUsage 释放预留") {
    EnvGuard guard("LUBANCODE_TRAJECTORY_V3_NEW_SESSIONS", "1");
    Volume volume("budget-tokens", "s1");
    GoalService service(&*volume.writer, ServiceOptionsFor(volume.dir));
    GoalStateSnapshot draft = DraftObjective("修好 auth");
    draft.pending_intent = FirstIntent().ToJson();
    draft.budget.max_total_tokens = 1000;
    auto created = service.CreateGoal(std::move(draft), nlohmann::json{{"source", "test"}});
    REQUIRE(created.ok);
    // usage 未报:token 尺没账可对,预留放行(§4.67.7 不能拿 0 冒充没花)。
    auto reserved_unknown = service.ReserveBudget("req-a", "evaluator", 5000);
    REQUIRE(reserved_unknown.ok);
    REQUIRE(service.ReleaseBudgetReservation("req-a").ok);

    // 实报 600 后:再预留 500 撞帽(600+500 > 1000);预留 300 过。
    GoalUsage spent;
    spent.input_tokens = 400;
    spent.output_tokens = 200;
    spent.usage_reported = true;
    auto recorded = service.RecordGoalUsage("req-1", "execution", spent,
                                            service.current()->state_revision,
                                            nlohmann::json{{"source", "test"}});
    REQUIRE(recorded.ok);
    CHECK_FALSE(recorded.payload.value("deduped", false));
    auto view = service.EvaluateBudget(0);
    CHECK_FALSE(view.exhausted);
    CHECK(view.used_tokens == 600);
    auto refused = service.ReserveBudget("req-b", "subagent", 500);
    REQUIRE_FALSE(refused.ok);
    CHECK(refused.error_code == goalns::kErrGoalReservationRejected);
    // 再来一笔 500 也算撞(would_exhaust);恰好 400 装得下。
    CHECK(service.EvaluateBudget(500).would_exhaust);
    CHECK_FALSE(service.EvaluateBudget(400).would_exhaust);
    REQUIRE(service.ReserveBudget("req-b", "subagent", 400).ok);
    // 同名预留拒((sessionId,requestId) 去重)。
    auto dup = service.ReserveBudget("req-b", "subagent", 1);
    REQUIRE_FALSE(dup.ok);
    // 预留占位:600+400=1000,再来 1 token 都撞。
    CHECK(service.EvaluateBudget(1).would_exhaust);
    // 实报到手释放同名预留:占位回落。
    GoalUsage more;
    more.input_tokens = 100;
    more.usage_reported = true;
    REQUIRE(service.RecordGoalUsage("req-b", "subagent", more,
                                    service.current()->state_revision,
                                    nlohmann::json{{"source", "test"}})
                .ok);
    CHECK(service.reservations().empty());
    CHECK_FALSE(service.EvaluateBudget(1).would_exhaust);
    // 快照 usage 只增:600+100。
    CHECK(service.current()->usage.input_tokens == 500);
    CHECK(service.current()->usage.output_tokens == 200);
}

TEST_CASE("active 时长帽:activeElapsed 入快照,跨接管不归零") {
    EnvGuard guard("LUBANCODE_TRAJECTORY_V3_NEW_SESSIONS", "1");
    Volume volume("budget-elapsed", "s1");
    GoalService service(&*volume.writer, ServiceOptionsFor(volume.dir));
    GoalStateSnapshot draft = DraftObjective("修好 auth");
    draft.pending_intent = FirstIntent().ToJson();
    draft.budget.max_elapsed_ms = 60 * 60 * 1000;  // 60 分钟
    auto created = service.CreateGoal(std::move(draft), nlohmann::json{{"source", "test"}});
    REQUIRE(created.ok);
    REQUIRE(service.ClaimPendingIntent("run-s1", created.payload.at("stateRevision"),
                                       nlohmann::json{{"source", "test"}})
                .ok);
    REQUIRE(service.BeginIteration(service.current()->state_revision,
                                   nlohmann::json{{"source", "test"}})
                .ok);
    CHECK(service.current()->active_elapsed_ms == 0);  // preparing 段不计
    // 第一轮判 continue 排下一轮,active 段推进 61 分钟。
    auto began = service.BeginEvaluation(service.current()->state_revision, std::nullopt,
                                         std::vector<goalns::GoalEvidenceRef>{},
                                         std::vector<std::string>{},
                                         nlohmann::json{{"source", "test"}});
    REQUIRE(began.ok);
    REQUIRE(service.CompleteIterationWithEvaluation(
                began.payload.at("stateRevision"), ContinueVerdict("goal-1/iter-1"),
                nlohmann::json{{"source", "test"}})
                .ok);
    g_now_ms += 61 * 60 * 1000;
    // 认领这笔提交先把 61 分钟的 active 段计入快照(prev=active)。
    REQUIRE(service.ClaimPendingIntent("run-s1", service.current()->state_revision,
                                       nlohmann::json{{"source", "test"}})
                .ok);
    CHECK(service.current()->active_elapsed_ms >= 61 * 60 * 1000);
    // 开新轮撞时长帽:落 budget_exhausted,resume 不拿新计时器归零旧余额。
    auto refused = service.BeginIteration(service.current()->state_revision,
                                          nlohmann::json{{"source", "test"}});
    REQUIRE_FALSE(refused.ok);
    CHECK(refused.error_code == goalns::kErrGoalBudgetExhausted);
    CHECK(service.current()->lifecycle == GoalLifecycle::BudgetExhausted);
    CHECK(service.current()->stop_reason.find("active") != std::string::npos);
    // 投影侧守恒:接管后余额仍在(61 分钟不因换进程清零)。
    const auto ledger = lubancode::trajectory::v3::ReadV3Ledger(volume.jsonl());
    REQUIRE(ledger.has_value());
    const auto projection = goalns::ProjectGoalState(*ledger, volume.dir);
    REQUIRE(projection.gap == goalns::GoalProjectionGap::None);
    CHECK(projection.snapshot.active_elapsed_ms >= 61 * 60 * 1000);
}

// ---------------------------------------------------------------------------
// usage 计费去重(§4.67.7/§4.67.10"重复结果通知只交付一次")
// ---------------------------------------------------------------------------

TEST_CASE("RecordGoalUsage:同 requestId 去重,接管喂底,复核缺口如实带出") {
    EnvGuard guard("LUBANCODE_TRAJECTORY_V3_NEW_SESSIONS", "1");
    Volume volume("usage-dedup-src", "s1");
    {
        GoalService service(&*volume.writer, ServiceOptionsFor(volume.dir));
        RunToRunning(service);
        GoalUsage first;
        first.input_tokens = 100;
        first.request_count = 2;
        first.usage_reported = true;
        REQUIRE(service.RecordGoalUsage("subagent-5", "subagent", first,
                                        service.current()->state_revision,
                                        nlohmann::json{{"source", "test"}})
                    .ok);
        // 重复结果通知:同 key 第二次幂等,不落事实行、不加账。
        auto again = service.RecordGoalUsage("subagent-5", "subagent", first,
                                             service.current()->state_revision,
                                             nlohmann::json{{"source", "test"}});
        REQUIRE(again.ok);
        CHECK(again.payload.value("deduped", false));
        CHECK(CountEvents(volume, EventKindV3::GoalUsageRecorded) == 1);
        CHECK(service.current()->usage.input_tokens == 100);
        CHECK(service.current()->usage.request_count == 2);
    }
    // 新写者接管(重启面):计费去重底从投影喂——同 requestId 仍不重复计费。
    {
        const auto ledger = lubancode::trajectory::v3::ReadV3Ledger(volume.jsonl());
        REQUIRE(ledger.has_value());
        auto projection = goalns::ProjectGoalState(*ledger, volume.dir);
        REQUIRE(projection.gap == goalns::GoalProjectionGap::None);
        REQUIRE(projection.usage_request_ids.size() == 1);
        CHECK(projection.usage_request_ids[0] == "subagent-5");
        CHECK(projection.usage_recorded.input_tokens == 100);

        Volume second("usage-dedup-next", "s2");
        WriteSessionJson(second.dir, "s2", "resume", "s1");
        GoalService service2(&*second.writer, ServiceOptionsFor(second.dir));
        REQUIRE(service2.AdoptFromProjection(projection).ok);
        GoalUsage repeat;
        repeat.input_tokens = 100;
        repeat.usage_reported = true;
        auto again = service2.RecordGoalUsage("subagent-5", "subagent", repeat,
                                              service2.current()->state_revision,
                                              nlohmann::json{{"source", "test"}});
        REQUIRE(again.ok);
        CHECK(again.payload.value("deduped", false));
        CHECK(service2.current()->usage.input_tokens == 100);  // 没有双计
    }
}

TEST_CASE("usage 复核:事实比快照多时接管结果如实带缺口") {
    EnvGuard guard("LUBANCODE_TRAJECTORY_V3_NEW_SESSIONS", "1");
    Volume volume("usage-gap-src", "s1");
    GoalService service(&*volume.writer, ServiceOptionsFor(volume.dir));
    RunToRunning(service);
    GoalUsage spent;
    spent.input_tokens = 50;
    spent.usage_reported = true;
    REQUIRE(service.RecordGoalUsage("req-x", "subagent", spent,
                                    service.current()->state_revision,
                                    nlohmann::json{{"source", "test"}})
                .ok);
    // 正常接管:快照与事实一致,不带缺口。
    {
        const auto ledger = lubancode::trajectory::v3::ReadV3Ledger(volume.jsonl());
        REQUIRE(ledger.has_value());
        auto projection = goalns::ProjectGoalState(*ledger, volume.dir);
        REQUIRE(projection.gap == goalns::GoalProjectionGap::None);
        Volume adopter("usage-gap-a2", "s2");
        WriteSessionJson(adopter.dir, "s2", "resume", "s1");
        GoalService service2(&*adopter.writer, ServiceOptionsFor(adopter.dir));
        auto adopted = service2.AdoptFromProjection(projection);
        REQUIRE(adopted.ok);
        CHECK_FALSE(adopted.payload.value("usageGap", false));
    }
    // 缺口路径:直接构造投影(usage_recorded 拔高)——复核如实带出。
    {
        const auto ledger = lubancode::trajectory::v3::ReadV3Ledger(volume.jsonl());
        REQUIRE(ledger.has_value());
        auto projection = goalns::ProjectGoalState(*ledger, volume.dir);
        projection.usage_recorded.input_tokens += 75;  // 模拟"有事实没赶上提交"
        Volume adopter("usage-gap-a3", "s3");
        WriteSessionJson(adopter.dir, "s3", "resume", "s1");
        GoalService service3(&*adopter.writer, ServiceOptionsFor(adopter.dir));
        auto adopted = service3.AdoptFromProjection(projection);
        REQUIRE(adopted.ok);
        CHECK(adopted.payload.value("usageGap", false));
        CHECK(adopted.payload.at("usageFactsTotalTokens") == 125);
    }
}

// ---------------------------------------------------------------------------
// fork(§4.67.8)
// ---------------------------------------------------------------------------

TEST_CASE("CreateForkedGoal:另发 id 默认 paused,证据待复核,预算不带") {
    EnvGuard guard("LUBANCODE_TRAJECTORY_V3_NEW_SESSIONS", "1");
    Volume source_volume("fork-source", "s1");
    GoalService source_service(&*source_volume.writer, ServiceOptionsFor(source_volume.dir));
    RunToRunning(source_service);
    // 源:落证据、花预算、撞一轮,留问题面。
    goalns::GoalEvidenceRef ref;
    ref.id = "ev-1";
    ref.kind = "command_exit";
    ref.source = goalns::GoalEvidenceSource::ToolAction;
    ref.source_ref = "action-000001";
    ref.session_id = "s1";
    ref.run_id = "run-s1";
    ref.content_sha256 = std::string(64, 'c');
    ref.observed_at_ms = g_now_ms;
    ref.fresh = true;
    auto began = source_service.BeginEvaluation(source_service.current()->state_revision,
                                                std::nullopt, {ref}, {},
                                                nlohmann::json{{"source", "test"}});
    REQUIRE(began.ok);
    goalns::EvaluationVerdict verdict;
    verdict.evaluation_id = "eval-goal-1/iter-1";
    verdict.kind = GoalVerdictKind::NeedsUser;
    verdict.pending_question = "删库还是归档?";
    verdict.stop_reason = "needs_user";
    GoalUsage spent;
    spent.input_tokens = 700;
    spent.usage_reported = true;
    verdict.usage_addition = spent;
    REQUIRE(source_service.CompleteIterationWithEvaluation(
                began.payload.at("stateRevision"), verdict, nlohmann::json{{"source", "test"}})
                .ok);
    // 源再抬一顶预算帽,验 fork"原预算不带"有真东西可丢。
    goalns::GoalBudgetAddition raise;
    raise.iterations = 9;
    raise.total_tokens = 999999;
    REQUIRE(source_service.AddBudget(raise, source_service.current()->state_revision,
                                     nlohmann::json{{"source", "test"}})
                .ok);

    // fork:落在新 session 的服务上(源不动)。
    Volume fork_volume("fork-branch", "s2");
    GoalService fork_service(&*fork_volume.writer, ServiceOptionsFor(fork_volume.dir));
    auto forked = fork_service.CreateForkedGoal(*source_service.current(),
                                                nlohmann::json{{"source", "test"}, {"fork", true}});
    REQUIRE(forked.ok);
    const GoalStateSnapshot* branch = fork_service.current();
    REQUIRE(branch != nullptr);
    CHECK(branch->goal_id != source_service.current()->goal_id);  // 身份独立
    CHECK(branch->parent_goal_id == source_service.current()->goal_id);
    CHECK(branch->lifecycle == GoalLifecycle::Paused);  // 默认暂停,显式启动后才跑
    CHECK(branch->contract_revision == source_service.current()->contract_revision);
    CHECK(branch->objective == source_service.current()->objective);
    CHECK(branch->counters.iterations_started ==
          source_service.current()->counters.iterations_started);  // 进度来路
    REQUIRE(branch->evidence_refs.size() == 1);
    CHECK_FALSE(branch->evidence_refs[0].fresh);  // 继承证据全标待复核
    CHECK(branch->usage.input_tokens == 0);       // 原费用不带:独立累计
    CHECK_FALSE(branch->usage.usage_reported);
    CHECK_FALSE(branch->budget.max_iterations.has_value());   // 原预算不带
    CHECK_FALSE(branch->budget.max_total_tokens.has_value()); //(帽清空,费用独立累计)
    CHECK(branch->stop_reason.rfind("forked", 0) == 0);
    // 源不受扰:仍在 awaiting_user,预算帽照旧。
    CHECK(source_service.current()->lifecycle == GoalLifecycle::AwaitingUser);
    CHECK(*source_service.current()->budget.max_iterations == 9);
}

// ---------------------------------------------------------------------------
// compact/resume 守恒(§4.67.8:compact 两次 resume 两次,goal 不丢)
// ---------------------------------------------------------------------------

TEST_CASE("跨两次接管的守恒:合同/计数/问题/预算/证据不丢,字段 roundtrip") {
    EnvGuard guard("LUBANCODE_TRAJECTORY_V3_NEW_SESSIONS", "1");
    // v1 卷:立 goal,攒齐合同/计数/问题/预算/证据/停止意图/等待计划。
    Volume v1("keep-v1", "sv1");
    {
        GoalService service(&*v1.writer, ServiceOptionsFor(v1.dir));
        GoalStateSnapshot draft = DraftObjective("修好 auth;ctest 全过");
        draft.contract.criteria.push_back({"c-1", "ctest -R auth 全过", true});
        draft.contract.constraints.push_back("不得删除测试");
        draft.pending_intent = FirstIntent().ToJson();
        draft.budget.max_iterations = 20;
        draft.budget.max_total_tokens = 500000;
        auto created = service.CreateGoal(std::move(draft), nlohmann::json{{"source", "test"}});
        REQUIRE(created.ok);
        REQUIRE(service.ClaimPendingIntent("run-sv1", created.payload.at("stateRevision"),
                                           nlohmann::json{{"source", "test"}})
                    .ok);
        REQUIRE(service.BeginIteration(service.current()->state_revision,
                                       nlohmann::json{{"source", "test"}})
                .ok);
        goalns::GoalEvidenceRef ref;
        ref.id = "ev-1";
        ref.kind = "test_report";
        ref.source_ref = "action-000002";
        ref.session_id = "sv1";
        ref.run_id = "run-sv1";
        ref.content_sha256 = std::string(64, 't');
        ref.observed_at_ms = g_now_ms;
        auto began = service.BeginEvaluation(service.current()->state_revision, "evt-ckpt-1",
                                             {ref}, {}, nlohmann::json{{"source", "test"}});
        REQUIRE(began.ok);
        goalns::EvaluationVerdict verdict;
        verdict.evaluation_id = "eval-goal-1/iter-1";
        verdict.kind = GoalVerdictKind::NeedsUser;
        verdict.pending_question = "先修哪个模块?";
        verdict.stop_reason = "needs_user";
        REQUIRE(service.CompleteIterationWithEvaluation(
                    began.payload.at("stateRevision"), verdict, nlohmann::json{{"source", "test"}})
                    .ok);
    }
    WriteSessionJson(v1.dir, "sv1", "new", "");
    // "compact 一次":compact 只改模型上下文——goal 账不动,这里直接验
    // 快照字段在两次接管后逐项守恒(compact 本身不写 goal 行)。
    const auto ledger1 = lubancode::trajectory::v3::ReadV3Ledger(v1.jsonl());
    REQUIRE(ledger1.has_value());
    auto projection1 = goalns::ProjectGoalState(*ledger1, v1.dir);
    REQUIRE(projection1.gap == goalns::GoalProjectionGap::None);

    // resume 第一次:sv2 接管,立刻转回 active 再停(写一笔本卷提交)。
    nlohmann::json kept_json;
    {
        Volume v2("keep-v2", "sv2");
        WriteSessionJson(v2.dir, "sv2", "resume", "sv1");
        GoalService service2(&*v2.writer, ServiceOptionsFor(v2.dir));
        auto adopted = service2.AdoptFromProjection(projection1);
        REQUIRE(adopted.ok);
        kept_json = service2.current()->ToJson();  // 守恒基线
        // 接管后写一笔(active 转回):adoptedFrom 凭据走 Commit。
        GoalTransitionCandidate resume;
        resume.goal_id = service2.current()->goal_id;
        resume.expected_state_revision = service2.current()->state_revision;
        resume.to_lifecycle = GoalLifecycle::Active;
        resume.to_phase = GoalPhase::Idle;
        REQUIRE(service2.ApplyTransition(resume).ok);
        GoalTransitionCandidate pause;
        pause.goal_id = service2.current()->goal_id;
        pause.expected_state_revision = service2.current()->state_revision;
        pause.to_lifecycle = GoalLifecycle::Paused;
        pause.to_phase = GoalPhase::Idle;
        pause.stop_reason = "compact_test_pause";
        REQUIRE(service2.ApplyTransition(pause).ok);
    }
    // resume 第二次:sv3 沿链(sv2 -> sv1)接管,逐项对账。
    {
        Volume v3("keep-v3", "sv3");
        WriteSessionJson(v3.dir, "sv3", "resume", "sv2");
        const auto lineage = goalns::ProjectGoalLineage(v3.dir);
        REQUIRE(lineage.found);
        REQUIRE(lineage.projection.gap == goalns::GoalProjectionGap::None);
        // 证据判材料只从 head 卷回放(§4.67 G3:goal.evidence.recorded 事实行
        // 谁写谁卷里;祖先卷的不沿链追)。head(sv2)没跑过收口(flow 才落
        // 事实行,本测用 service API 直驱),回放为空——旧证据缺材料只会
        // 让验收更保守,快照 evidenceRefs 仍逐项守恒(下面断言)。
        REQUIRE(lineage.evidence_material.empty());
        GoalService service3(&*v3.writer, ServiceOptionsFor(v3.dir));
        REQUIRE(service3.AdoptFromProjection(lineage.projection).ok);
        const GoalStateSnapshot* kept = service3.current();
        REQUIRE(kept != nullptr);
        CHECK(kept->goal_id == projection1.snapshot.goal_id);
        CHECK(kept->objective == projection1.snapshot.objective);
        CHECK(kept->contract.constraints.size() == 1);
        CHECK(kept->contract.criteria.size() == 1);
        CHECK(kept->counters.iterations_started == 1);
        CHECK(kept->pending_question == "先修哪个模块?");
        CHECK(kept->budget.max_iterations.has_value());
        CHECK(*kept->budget.max_iterations == 20);
        CHECK(kept->budget.max_total_tokens.has_value());
        CHECK(*kept->budget.max_total_tokens == 500000);
        REQUIRE(kept->evidence_refs.size() == 1);
        CHECK(kept->evidence_refs[0].kind == "test_report");
        REQUIRE(kept->checkpoint_ref.has_value());
        CHECK(*kept->checkpoint_ref == "evt-ckpt-1");  // 请求引用可还原
        REQUIRE(kept->applied_evaluation_id.has_value());
        CHECK(*kept->applied_evaluation_id == "eval-goal-1/iter-1");
        CHECK(kept->contract_revision == projection1.snapshot.contract_revision);
    }
}

TEST_CASE("两个服务同读一份投影:同一状态与预算,单活动写者不双跑") {
    EnvGuard guard("LUBANCODE_TRAJECTORY_V3_NEW_SESSIONS", "1");
    Volume volume("two-readers-src", "s1");
    {
        GoalService service(&*volume.writer, ServiceOptionsFor(volume.dir));
        RunToRunning(service);
    }
    const auto ledger = lubancode::trajectory::v3::ReadV3Ledger(volume.jsonl());
    REQUIRE(ledger.has_value());
    const auto projection = goalns::ProjectGoalState(*ledger, volume.dir);
    REQUIRE(projection.gap == goalns::GoalProjectionGap::None);
    // CLI 与 AppServer 同一只读面:两份服务各自接管,看到的快照逐字节一致。
    Volume a("two-readers-cli", "sa");
    WriteSessionJson(a.dir, "sa", "resume", "s1");
    Volume b("two-readers-app", "sb");
    WriteSessionJson(b.dir, "sb", "resume", "s1");
    GoalService service_a(&*a.writer, ServiceOptionsFor(a.dir));
    GoalService service_b(&*b.writer, ServiceOptionsFor(b.dir));
    REQUIRE(service_a.AdoptFromProjection(projection).ok);
    REQUIRE(service_b.AdoptFromProjection(projection).ok);
    CHECK(service_a.current()->ToJson() == service_b.current()->ToJson());
    CHECK(service_a.current()->usage.input_tokens == service_b.current()->usage.input_tokens);
    CHECK(service_a.current()->budget.max_iterations == service_b.current()->budget.max_iterations);
}
