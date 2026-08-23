// goal snapshot 守恒(持久目标单第 6 期):compact 只压模型上下文,不压
// GoalTask/event ledger——真本不在聊天摘要里。
//
// 单子"每次 compact 必守恒"清单在这里落成一只纯函数对:
//   BuildGoalSnapshot:从 coordinator 的真账拼一份结构化快照(goal id/
//     revision/objective hash/contract criteria id/state/iteration index/
//     最近 checkpoint/仍有效 evidence id 与摘要/blocker 与 no-progress
//     streak/已用剩余预算/pending question/workspace identity);
//   ValidateGoalSnapshot:reduce 后的快照对原快照逐项核,少一项便拒稿
//     (manifest 守恒 validator 同款取舍)。
//
// compact_v2 的 manifest 里写 goal_snapshot + snapshot_sha256;resume 时与
// goal ledger 对账(hash 不合 = 摘要被动过,以 ledger 真本为准并记 warning)。

#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "runtime/goal_coordinator.hpp"
#include "runtime/goal_types.hpp"

namespace lubancode::runtime::goal {

// 一份 goal 快照(manifest 里带的守恒面)。
struct GoalSnapshot {
    std::string goal_id;
    int revision = 0;
    std::string objective_sha256;
    std::vector<std::string> criterion_ids;
    std::string state;                    // 稳定字符串
    int iteration_index = 0;              // 最近一枚 iteration 的 index
    GoalCheckpoint checkpoint;            // 最近 checkpoint(下一轮路标)
    std::vector<std::string> fresh_evidence_ids;
    std::vector<std::string> stale_evidence_ids;
    int blocker_streak = 0;
    int no_progress_streak = 0;
    nlohmann::json budget;                // GoalBudget::to_json()
    nlohmann::json usage;                 // GoalUsage::to_json()
    std::optional<std::string> pending_question;
    std::string workspace_identity;

    nlohmann::json to_json() const;
    static GoalSnapshot from_json(const nlohmann::json& j);
};

// 从 coordinator 真账拼快照(无 goal 给 nullopt——普通会话不带 goal 段)。
std::optional<GoalSnapshot> BuildGoalSnapshot(const GoalCoordinator& coordinator);

// 守恒校验:after 与 before 逐项对。放行清单外的字段可变(如 usage 增长),
// 守恒面(goal id/revision/objective hash/criteria id/state/checkpoint 的
// evidence 引用/streak 不增)少一项或被改就 failure。空 failures = 过。
struct GoalSnapshotValidation {
    bool ok = false;
    std::vector<std::string> failures;
};
GoalSnapshotValidation ValidateGoalSnapshot(const GoalSnapshot& before, const GoalSnapshot& after);

// snapshot 的守恒 hash(对守恒面的规范序列化取 sha256;usage 一类增长字段
// 不进 hash,免得每次轮次推进 hash 必变)。
std::string GoalSnapshotConservationSha256(const GoalSnapshot& snapshot);

}  // namespace lubancode::runtime::goal
