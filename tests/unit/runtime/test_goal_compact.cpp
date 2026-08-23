// /goal 单第 6 期:goal snapshot 守恒(compact 只压上下文,不压真账)。

#include <doctest/doctest.h>

#include <string>
#include <vector>

#include "runtime/goal_compact.hpp"
#include "runtime/goal_coordinator.hpp"
#include "runtime/goal_types.hpp"

using lubancode::runtime::goal::BuildGoalSnapshot;
using lubancode::runtime::goal::GoalCheckpoint;
using lubancode::runtime::goal::GoalContract;
using lubancode::runtime::goal::GoalCoordinator;
using lubancode::runtime::goal::GoalEvaluation;
using lubancode::runtime::goal::GoalDecision;
using lubancode::runtime::goal::GoalSnapshot;
using lubancode::runtime::goal::GoalSnapshotConservationSha256;
using lubancode::runtime::goal::ValidateGoalSnapshot;

namespace {

GoalCoordinator::Options Opts() {
    GoalCoordinator::Options o;
    o.goals_enabled = true;
    o.default_max_iterations = 10;
    return o;
}

GoalSnapshot SampleSnapshot() {
    GoalSnapshot s;
    s.goal_id = "goal-1";
    s.revision = 2;
    s.objective_sha256 = "abc";
    s.criterion_ids = {"c-1", "c-2"};
    s.state = "active";
    s.iteration_index = 3;
    s.checkpoint.summary = "契约测试跑通";
    s.checkpoint.evidence_ids = {"ev-1", "ev-2"};
    s.fresh_evidence_ids = {"ev-1", "ev-2"};
    s.blocker_streak = 0;
    s.no_progress_streak = 1;
    s.budget = nlohmann::json{{"max_iterations", 10}};
    s.usage = nlohmann::json{{"input_tokens", 100}};
    s.workspace_identity = "repo@main";
    return s;
}

}  // namespace

TEST_CASE("BuildGoalSnapshot:从真账拼,无 goal 给 nullopt") {
    GoalCoordinator empty(Opts());
    CHECK_FALSE(BuildGoalSnapshot(empty).has_value());

    GoalCoordinator g(Opts());
    REQUIRE(g.Create("迁移认证层", "/r", "repo@main", 1000).ok);
    GoalContract contract;
    contract.objective = "迁移认证层";
    contract.criteria.push_back({"c-1", "契约测试全绿", true});
    REQUIRE(g.SubmitContract(contract, 1100).ok);
    const auto snapshot = BuildGoalSnapshot(g);
    REQUIRE(snapshot.has_value());
    CHECK(snapshot->goal_id == "goal-1");
    CHECK(snapshot->revision == 1);
    CHECK(snapshot->state == "active");
    CHECK(snapshot->criterion_ids == std::vector<std::string>{"c-1"});
    CHECK(snapshot->workspace_identity == "repo@main");
}

TEST_CASE("守恒:原样快照过,逐项守恒") {
    const GoalSnapshot before = SampleSnapshot();
    const auto pass = ValidateGoalSnapshot(before, before);
    CHECK(pass.ok);
    CHECK(pass.failures.empty());

    // usage 增长不算破坏(它在守恒面之外)。
    GoalSnapshot after = before;
    after.usage["input_tokens"] = 99999;
    CHECK(ValidateGoalSnapshot(before, after).ok);
}

TEST_CASE("守恒:少一项或被改就拒") {
    const GoalSnapshot before = SampleSnapshot();

    GoalSnapshot lost_criterion = before;
    lost_criterion.criterion_ids.pop_back();
    CHECK_FALSE(ValidateGoalSnapshot(before, lost_criterion).ok);

    GoalSnapshot bumped_revision = before;
    bumped_revision.revision = 3;
    CHECK_FALSE(ValidateGoalSnapshot(before, bumped_revision).ok);

    GoalSnapshot lost_checkpoint = before;
    lost_checkpoint.checkpoint.summary.clear();
    CHECK_FALSE(ValidateGoalSnapshot(before, lost_checkpoint).ok);

    GoalSnapshot lost_evidence = before;
    lost_evidence.checkpoint.evidence_ids.pop_back();
    const auto r = ValidateGoalSnapshot(before, lost_evidence);
    CHECK_FALSE(r.ok);
    CHECK(!r.failures.empty());

    GoalSnapshot state_changed = before;
    state_changed.state = "paused";
    CHECK_FALSE(ValidateGoalSnapshot(before, state_changed).ok);

    GoalSnapshot budget_widened = before;
    budget_widened.budget["max_iterations"] = 999;
    CHECK_FALSE(ValidateGoalSnapshot(before, budget_widened).ok);

    GoalSnapshot streak_grew = before;
    streak_grew.no_progress_streak = 5;
    CHECK_FALSE(ValidateGoalSnapshot(before, streak_grew).ok);
    // streak 只许降(用户处置后清零)。
    GoalSnapshot streak_cleared = before;
    streak_cleared.no_progress_streak = 0;
    CHECK(ValidateGoalSnapshot(before, streak_cleared).ok);
}

TEST_CASE("守恒 hash:同守恒面同 hash,usage 涨不动 hash") {
    const GoalSnapshot a = SampleSnapshot();
    GoalSnapshot b = a;
    b.usage["input_tokens"] = 888888;
    CHECK(GoalSnapshotConservationSha256(a) == GoalSnapshotConservationSha256(b));
    b.checkpoint.summary = "被摘要改了";
    CHECK(GoalSnapshotConservationSha256(a) != GoalSnapshotConservationSha256(b));
}

TEST_CASE("快照序列化 roundtrip") {
    const GoalSnapshot before = SampleSnapshot();
    const GoalSnapshot after = GoalSnapshot::from_json(before.to_json());
    CHECK(after.goal_id == before.goal_id);
    CHECK(after.revision == 2);
    CHECK(after.criterion_ids.size() == 2);
    CHECK(after.checkpoint.summary == "契约测试跑通");
    CHECK(after.checkpoint.evidence_ids.size() == 2);
    CHECK(after.budget.at("max_iterations") == 10);
    CHECK(after.workspace_identity == "repo@main");
}
