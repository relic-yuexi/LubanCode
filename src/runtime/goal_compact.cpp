// goal snapshot 守恒实现(纯函数;测试钉 tests/test_goal_compact.cpp)。

#include "runtime/goal_compact.hpp"

#include <sstream>

#include "hooks/hash.hpp"

namespace lubancode::runtime::goal {

nlohmann::json GoalSnapshot::to_json() const {
    nlohmann::json j;
    j["goal_id"] = goal_id;
    j["revision"] = revision;
    j["objective_sha256"] = objective_sha256;
    j["criterion_ids"] = criterion_ids;
    j["state"] = state;
    j["iteration_index"] = iteration_index;
    j["checkpoint"] = checkpoint.to_json();
    j["fresh_evidence_ids"] = fresh_evidence_ids;
    j["stale_evidence_ids"] = stale_evidence_ids;
    j["blocker_streak"] = blocker_streak;
    j["no_progress_streak"] = no_progress_streak;
    j["budget"] = budget;
    j["usage"] = usage;
    if (pending_question.has_value()) j["pending_question"] = *pending_question;
    j["workspace_identity"] = workspace_identity;
    return j;
}

GoalSnapshot GoalSnapshot::from_json(const nlohmann::json& j) {
    GoalSnapshot s;
    auto read_str = [&j](const char* key, std::string& out) {
        if (j.contains(key) && j.at(key).is_string()) out = j.at(key).get<std::string>();
    };
    read_str("goal_id", s.goal_id);
    if (j.contains("revision") && j.at("revision").is_number_integer()) {
        s.revision = j.at("revision").get<int>();
    }
    read_str("objective_sha256", s.objective_sha256);
    auto read_list = [&j](const char* key, std::vector<std::string>& out) {
        out.clear();
        if (!j.contains(key) || !j.at(key).is_array()) return;
        for (const auto& item : j.at(key)) {
            if (item.is_string()) out.push_back(item.get<std::string>());
        }
    };
    read_list("criterion_ids", s.criterion_ids);
    read_str("state", s.state);
    if (j.contains("iteration_index") && j.at("iteration_index").is_number_integer()) {
        s.iteration_index = j.at("iteration_index").get<int>();
    }
    if (j.contains("checkpoint")) s.checkpoint = GoalCheckpoint::from_json(j.at("checkpoint"));
    read_list("fresh_evidence_ids", s.fresh_evidence_ids);
    read_list("stale_evidence_ids", s.stale_evidence_ids);
    if (j.contains("blocker_streak") && j.at("blocker_streak").is_number_integer()) {
        s.blocker_streak = j.at("blocker_streak").get<int>();
    }
    if (j.contains("no_progress_streak") && j.at("no_progress_streak").is_number_integer()) {
        s.no_progress_streak = j.at("no_progress_streak").get<int>();
    }
    if (j.contains("budget") && j.at("budget").is_object()) s.budget = j.at("budget");
    if (j.contains("usage") && j.at("usage").is_object()) s.usage = j.at("usage");
    if (j.contains("pending_question") && j.at("pending_question").is_string()) {
        s.pending_question = j.at("pending_question").get<std::string>();
    }
    read_str("workspace_identity", s.workspace_identity);
    return s;
}

std::optional<GoalSnapshot> BuildGoalSnapshot(const GoalCoordinator& coordinator) {
    const GoalTask* task = coordinator.task();
    if (task == nullptr) return std::nullopt;

    GoalSnapshot snapshot;
    snapshot.goal_id = task->id;
    snapshot.revision = task->revision;
    snapshot.objective_sha256 = task->objective_sha256;
    for (const auto& criterion : task->contract.criteria) {
        snapshot.criterion_ids.push_back(criterion.id);
    }
    snapshot.state = ToString(task->state);
    snapshot.iteration_index = task->counters.iterations_started;  // 最近一轮序号
    snapshot.checkpoint = task->checkpoint;
    // 证据账:fresh 与 stale 分列(只带 id,不复制正文——digest/退出码在
    // evidence 行,摘要只留指引)。
    snapshot.fresh_evidence_ids.clear();
    snapshot.stale_evidence_ids.clear();
    for (int i = 0; i < static_cast<int>(coordinator.evidence_count()) + 0; ++i) {
        // evidence_count 只给总数;id 遍历由 coordinator 提供?——不,快照要
        // 的是"仍有效证据 id"清单;这里从 task 侧最近 checkpoint 引用出发,
        // 外加上一轮判词引用。保守做法:快照只带 checkpoint 与判词引用的
        // evidence id(它们是 evaluator 下轮还要对账的);全量账在 ledger。
    }
    // checkpoint 引用的证据(仍有效就归 fresh,否则 stale 的判定在 ledger
    // 侧;快照只记 id 与 fresh 旗标,由查表回调补)。
    snapshot.fresh_evidence_ids = task->checkpoint.evidence_ids;
    snapshot.blocker_streak = task->counters.same_blocker_streak;
    snapshot.no_progress_streak = task->counters.no_progress_streak;
    snapshot.budget = task->budget.to_json();
    snapshot.usage = task->usage.to_json();
    if (task->last_evaluation.has_value() && task->last_evaluation->question.has_value()) {
        snapshot.pending_question = task->last_evaluation->question;
    }
    snapshot.workspace_identity = task->workspace_identity;
    return snapshot;
}

GoalSnapshotValidation ValidateGoalSnapshot(const GoalSnapshot& before, const GoalSnapshot& after) {
    GoalSnapshotValidation v;
    const auto fail = [&v](const std::string& text) { v.failures.push_back(text); };
    if (before.goal_id != after.goal_id) fail("goal id 变了: " + before.goal_id + " -> " + after.goal_id);
    if (before.revision != after.revision) {
        fail("revision 变了: " + std::to_string(before.revision) + " -> " +
             std::to_string(after.revision) + "(compact 不许改目标)");
    }
    if (before.objective_sha256 != after.objective_sha256) fail("objective hash 变了");
    if (before.criterion_ids != after.criterion_ids) {
        fail("criteria id 清单变了(少一条判词就对不上账)");
    }
    if (before.state != after.state) {
        fail("state 变了: " + before.state + " -> " + after.state);
    }
    // checkpoint:下一轮路标不许 compact 丢。
    if (before.checkpoint.summary != after.checkpoint.summary) {
        fail("最近 checkpoint 摘要丢了");
    }
    for (const std::string& ev_id : before.checkpoint.evidence_ids) {
        bool found = false;
        for (const std::string& other : after.checkpoint.evidence_ids) {
            if (other == ev_id) {
                found = true;
                break;
            }
        }
        if (!found) fail("checkpoint 引用的证据 id 丢了: " + ev_id);
    }
    // streak 只许降(用户处置后清零),不许 compact 无中生有地涨。
    if (after.blocker_streak > before.blocker_streak) fail("blocker streak 莫名变多");
    if (after.no_progress_streak > before.no_progress_streak) fail("no-progress streak 莫名变多");
    // 预算闸不许放宽(只可收紧)。
    if (before.budget.contains("max_iterations") && after.budget.contains("max_iterations") &&
        after.budget.at("max_iterations").get<int>() > before.budget.at("max_iterations").get<int>()) {
        fail("iteration 上限被放宽(compact 不许改预算)");
    }
    if (before.pending_question.has_value() && !after.pending_question.has_value()) {
        fail("pending question 丢了");
    }
    if (before.workspace_identity != after.workspace_identity) fail("workspace identity 变了");
    v.ok = v.failures.empty();
    return v;
}

std::string GoalSnapshotConservationSha256(const GoalSnapshot& snapshot) {
    // 守恒面:预算 hash 只盖 max_* 闸,不盖 usage(usage 每轮必增)。
    nlohmann::json j;
    j["goal_id"] = snapshot.goal_id;
    j["revision"] = snapshot.revision;
    j["objective_sha256"] = snapshot.objective_sha256;
    j["criterion_ids"] = snapshot.criterion_ids;
    j["state"] = snapshot.state;
    j["checkpoint_summary"] = snapshot.checkpoint.summary;
    j["checkpoint_evidence_ids"] = snapshot.checkpoint.evidence_ids;
    j["workspace_identity"] = snapshot.workspace_identity;
    return hooks::Sha256Hex(j.dump());
}

}  // namespace lubancode::runtime::goal
