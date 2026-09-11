// goal 事件行(goal 单第 4 期;写侧已按 §4.67.6 收敛口径删除):旧档
// goal_v1 族行的只读解析。
//
// 收敛口径(轨迹 v3 §4.67.6):不再写 goal_v1 顶层类型,不另造一份与
// Session 争真值的可变 goal 日志。goal 的持久账自 v3 起走 trajectory 的
// state.goal.applied 提交锚 + sessions/<id>/state/goals/ 快照
// (runtime/goal_service.hpp)。本件只剩读侧:旧档(ledger sink 尚在的
// 年代落过盘的 goal_v1 族)在 /resume 与 evolution 只读观察里仍要能认,
// 坏行跳过不废整场(事件行通用约定);消息账无损。
//
// 旧事件形状(读侧认账用):
//   {"type":"goal_v1","event":"created","goal_id":"goal-3","revision":1,...}
//   {"type":"goal_iteration_v1","event":"scheduled","goal_id":...,"iteration_id":...}
//   {"type":"goal_evidence_v1","event":"observed",...}
//   {"type":"goal_checkpoint_v1","goal_id":...,"iteration_id":...,"checkpoint":{},...}
//   {"type":"goal_evaluation_v1","goal_id":...,"iteration_id":...,"evaluation_id":...}
//
// sessions/ 不反向依赖 runtime/(老规矩):所以这一层只认 nlohmann + 标准库,
// 领域字段用中立的 nlohmann::json 载(payload 的 shape 由 runtime 侧的
// GoalCoordinatorEvent 定,这里不复制 runtime 类型)。

#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace lubancode::sessions {

// 一行 goal 事件(中立形状:type/event/goal_id/revision/iteration_id/
// payload/timestamp_ms)。
struct GoalSessionEvent {
    std::string type;          // goal_v1 / goal_iteration_v1 / goal_evidence_v1 /
                               // goal_checkpoint_v1 / goal_evaluation_v1
    std::string event;         // created/contract_ready/scheduled/started/
                               // checkpoint/evaluated/finished/paused/cleared/...
    std::string goal_id;
    std::string iteration_id;  // 迭代类事件带;goal 级事件空
    int revision = 0;
    nlohmann::json payload = nlohmann::json::object();
    std::int64_t timestamp_ms = 0;

    // observed 之类的 evidence 事件,evidence 原文在 payload["evidence"]。
    // checkpoint 事件带 payload["checkpoint"];evaluation 带
    // payload["evaluation"]。
};

// 一行 JSON -> 事件。不是合法 JSON、type 不认得、缺 goal_id/event,给
// nullopt——坏行调用方跳过,不废整场。(写侧 SerializeGoalEvent 已随
// goal_v1 收敛删除:新账不落这种行。)
std::optional<GoalSessionEvent> ParseGoalEvent(const std::string& line);

// 这一行是不是 goal 事件行(顶层 type 粗筛,省 JSON 解析;ParseGoalEvent
// 再真验)。
bool IsGoalEventLine(const std::string& line);

}  // namespace lubancode::sessions
