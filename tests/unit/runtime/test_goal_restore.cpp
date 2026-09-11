// /goal 单第 4/6 期:存档事件 → coordinator 重建 / feature 关落
// SuspendedByPolicy / goal checkpoint 工具条目 → runtime checkpoint 转写。

#include <doctest/doctest.h>

#include <memory>
#include <string>
#include <vector>

#include "sessions/goal_session.hpp"
#include "runtime/goal_coordinator.hpp"
#include "runtime/goal_types.hpp"
#include "tools/goal_checkpoint_tool.hpp"

using lubancode::sessions::GoalSessionEvent;
using lubancode::runtime::goal::GoalCheckpoint;
using lubancode::runtime::goal::GoalContract;
using lubancode::runtime::goal::GoalCoordinatorEvent;
using lubancode::runtime::goal::GoalCoordinator;
using lubancode::runtime::goal::GoalDecision;
using lubancode::runtime::goal::GoalEvaluation;
using lubancode::runtime::goal::GoalState;

namespace {

GoalCoordinator::Options Opts() {
    GoalCoordinator::Options o;
    o.goals_enabled = true;
    o.default_max_iterations = 5;
    return o;
}

// 录一遍完整生命周期(created→contract→scheduled→started→checkpoint→
// evaluated(continue)),返回事件行(goal_session 形状)。
std::vector<GoalSessionEvent> RecordLifecycle() {
    std::vector<GoalCoordinatorEvent> raw;
    GoalCoordinator live(Opts());
    live.SetLedgerSink([&raw](const GoalCoordinatorEvent& e) {
        raw.push_back(e);
        return true;
    });
    REQUIRE(live.Create("迁移认证层", "/repo", "repo@main", 1000).ok);
    GoalContract contract;
    contract.objective = "迁移认证层";
    contract.criteria.push_back({"c-1", "契约测试全绿", true});
    REQUIRE(live.SubmitContract(contract, 1100).ok);
    REQUIRE(live.ScheduleNextIteration(1200).ok);
    REQUIRE(live.TakeReadyIteration("turn-1", "fp", 1250).ok);
    GoalCheckpoint cp;
    cp.summary = "契约测试跑通";
    cp.remaining = {"重跑 e2e"};
    REQUIRE(live.CheckpointReached(cp, 1300).ok);
    GoalEvaluation ev;
    ev.decision = GoalDecision::Continue;
    ev.progress = true;
    REQUIRE(live.ApplyEvaluation(ev, 1400).ok);

    std::vector<GoalSessionEvent> lines;
    for (const auto& e : raw) {
        GoalSessionEvent line;
        line.event = e.event;
        line.goal_id = e.goal_id;
        line.iteration_id = e.iteration_id;
        line.revision = e.revision;
        line.payload = e.payload;
        line.timestamp_ms = e.timestamp_ms;
        line.type = e.iteration_id.empty() ? "goal_v1" : "goal_iteration_v1";
        lines.push_back(line);
    }
    return lines;
}

}  // namespace

TEST_CASE("RestoreFromArchive:整账重建,状态与 checkpoint 对上") {
    const auto lines = RecordLifecycle();
    GoalCoordinator restored(Opts());
    const auto stats = restored.RestoreFromArchive(lines);
    CHECK(stats.replayed == static_cast<int>(lines.size()));
    CHECK(stats.skipped == 0);
    REQUIRE(restored.has_goal());
    CHECK(restored.task()->id == "goal-1");
    CHECK(restored.task()->state == GoalState::Active);  // continue 后回 Active
    CHECK(restored.task()->contract_frozen);
    CHECK(restored.task()->counters.iterations_started == 1);
    CHECK(restored.task()->checkpoint.summary == "契约测试跑通");
    CHECK(restored.task()->checkpoint.remaining ==
          std::vector<std::string>{"重跑 e2e"});
    CHECK(restored.task()->last_evaluation.has_value());
    CHECK(restored.task()->last_evaluation->decision == GoalDecision::Continue);
    // continue 已落、下一轮未排:恢复侧补一枚从 iter-2 起。
    const auto next = restored.ScheduleNextIteration(2000);
    REQUIRE(next.ok);
    CHECK(next.payload.at("iteration_id") == "goal-1/iter-2");
}

TEST_CASE("ParseGoalEvent:旧档 goal_v1 行读侧兼容(写侧已随 §4.67.6 收敛)") {
    // 写侧 SerializeGoalEvent 已删(v3 起 goal 持久账走 state.goal.applied +
    // 不可变快照);这里钉 ledger sink 年代落过盘的旧档行解析仍认,
    // /resume 与 evolution 只读观察不断粮。手写行按旧 to_json 键序。
    const std::vector<std::string> legacy_lines = {
        R"({"event":"created","goal_id":"goal-1","payload":{"objective":"迁移认证层","objective_sha256":"h","budget":{}},"revision":1,"timestamp_ms":1000,"type":"goal_v1"})",
        R"({"event":"contract_ready","goal_id":"goal-1","payload":{"contract":{"criteria":[{"id":"c-1","required":true,"text":"契约测试全绿"}],"objective":"迁移认证层"},"contract_sha256":"h"},"revision":1,"timestamp_ms":1100,"type":"goal_v1"})",
        R"({"event":"cleared","goal_id":"goal-1","payload":{"from_state":"active"},"revision":1,"timestamp_ms":1200,"type":"goal_v1"})",
    };
    std::vector<GoalSessionEvent> parsed_lines;
    for (const auto& text : legacy_lines) {
        REQUIRE(lubancode::sessions::IsGoalEventLine(text));
        const auto parsed = lubancode::sessions::ParseGoalEvent(text);
        REQUIRE(parsed.has_value());
        CHECK(parsed->goal_id == "goal-1");
        parsed_lines.push_back(*parsed);
    }
    GoalCoordinator restored(Opts());
    const auto stats = restored.RestoreFromArchive(parsed_lines);
    CHECK(stats.skipped == 0);
    REQUIRE(restored.has_goal());
    CHECK(restored.task()->id == "goal-1");
    CHECK(restored.task()->state == GoalState::Cleared);
    CHECK(restored.task()->contract_frozen);
    // 坏行跳过不废整场:缺 goal_id、未知 type、非 goal 行。
    CHECK_FALSE(lubancode::sessions::ParseGoalEvent(
                    R"({"event":"created","type":"goal_v1"})")
                    .has_value());
    CHECK_FALSE(lubancode::sessions::ParseGoalEvent(
                    R"({"event":"x","goal_id":"g","type":"goal_v9"})")
                    .has_value());
    CHECK_FALSE(lubancode::sessions::IsGoalEventLine(R"({"type":"message"})"));
}

TEST_CASE("RestoreFromArchive:feature 关,active goal 落 SuspendedByPolicy") {
    const auto lines = RecordLifecycle();
    GoalCoordinator::Options off = Opts();
    off.goals_enabled = false;
    GoalCoordinator restored(off);
    const auto stats = restored.RestoreFromArchive(lines);
    CHECK(stats.suspended_by_policy);
    REQUIRE(restored.has_goal());
    CHECK(restored.task()->state == GoalState::SuspendedByPolicy);
    CHECK(restored.task()->contract_frozen);  // 账保留:可查、可导出、可 clear
    CHECK_FALSE(restored.HasReadyContinuation());  // 不自动续跑
    // clear 仍可用(用户处置口),create 不行。
    CHECK(restored.Clear(2000).ok);
}

TEST_CASE("RestoreFromArchive:terminal 档不复活,clear 幂等") {
    auto lines = RecordLifecycle();
    // 尾上补一条 cleared(goal 级事件)。
    GoalSessionEvent cleared;
    cleared.type = "goal_v1";
    cleared.event = "cleared";
    cleared.goal_id = "goal-1";
    cleared.revision = 1;
    cleared.timestamp_ms = 1500;
    lines.push_back(cleared);

    GoalCoordinator restored(Opts());
    restored.RestoreFromArchive(lines);
    CHECK(restored.task()->state == GoalState::Cleared);
    CHECK_FALSE(restored.HasActiveGoal());
    // terminal 后 resume 拒。
    const auto r = restored.Resume(0, 2000);
    CHECK_FALSE(r.ok);
}

TEST_CASE("checkpoint 工具条目 → runtime GoalCheckpoint 转写(装配层的桥)") {
    // 工具产 tools::GoalCheckpointEntry;runtime 侧吃 GoalCheckpoint。这枚
    // 转写是纯数据搬运,装配层一行 inline;这里钉字段对齐的契约。
    auto state = std::make_shared<lubancode::tools::GoalCheckpointState>();
    state->goal_id = "goal-1";
    state->iteration_id = "goal-1/iter-1";
    state->valid_evidence_ids = {"ev-1"};
    lubancode::tools::GoalCheckpointTool tool(state);
    nlohmann::json input;
    input["status"] = "ready_for_evaluation";
    input["summary"] = "契约测试全绿";
    input["completed"] = nlohmann::json::array({"迁移完成"});
    input["remaining"] = nlohmann::json::array();
    input["next_action"] = "验收";
    input["evidence_ids"] = nlohmann::json::array({"ev-1"});
    const auto result = tool.execute(input);
    REQUIRE_FALSE(result.is_error);

    const auto candidate = state->Candidate();
    REQUIRE(candidate.has_value());
    // 装配层把 tools 条目翻成 runtime checkpoint(status 映射 + 字段直搬)。
    GoalCheckpoint cp;
    cp.summary = candidate->summary;
    cp.completed = candidate->completed;
    cp.remaining = candidate->remaining;
    cp.next_action = candidate->next_action;
    cp.evidence_ids = candidate->evidence_ids;
    CHECK(cp.summary == "契约测试全绿");
    CHECK(cp.remaining.empty());
    CHECK(cp.evidence_ids == std::vector<std::string>{"ev-1"});
    CHECK_FALSE(cp.synthesized);  // 模型真调了工具,不是宿主合成
}
