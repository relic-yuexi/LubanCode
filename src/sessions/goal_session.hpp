// goal 事件行(持久目标单第 4 期):session JSONL 的 goal_v1 / goal_iteration_v1 /
// goal_evidence_v1 / goal_checkpoint_v1 / goal_evaluation_v1 序列化与解析。
//
// 事件形状(单子"session JSONL"节原文):
//   {"type":"goal_v1","event":"created","goal_id":"goal-3","revision":1,...}
//   {"type":"goal_iteration_v1","event":"scheduled","goal_id":...,"iteration_id":...}
//   {"type":"goal_evidence_v1","event":"observed",...}
//   {"type":"goal_checkpoint_v1","goal_id":...,"iteration_id":...,"checkpoint":{},...}
//   {"type":"goal_evaluation_v1","goal_id":...,"iteration_id":...,"evaluation_id":...}
//
// 分层与 tool_trace 同款:本头是纯数据 + 纯函数(不碰磁盘),SessionStore
// 的 AppendGoalEvent 是落盘薄壳;回放按文件序整收 LoadedSession,折叠/重建
// 在 runtime 侧的 GoalCoordinator::ReplayEvent。坏行跳过,不废整场(事件
// 行通用约定);老版本读到 goal 行当坏行跳过,消息账无损。
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

// 事件 -> 一行 JSON(不带换行符)。ts 是落盘时刻,与其余事件行同款。
std::string SerializeGoalEvent(const GoalSessionEvent& event, const std::string& ts);

// 一行 JSON -> 事件。不是合法 JSON、type 不认得、缺 goal_id/event,给
// nullopt——坏行调用方跳过,不废整场。
std::optional<GoalSessionEvent> ParseGoalEvent(const std::string& line);

// 这一行是不是 goal 事件行(顶层 type 粗筛,省 JSON 解析;ParseGoalEvent
// 再真验)。
bool IsGoalEventLine(const std::string& line);

// 一份证据(goal_evidence_v1 的中立形状;fields 与 runtime::goal::GoalEvidence
// 一一对应,但不引 runtime 头)。
struct GoalEvidenceRecord {
    std::string id;
    std::string kind;            // 稳定字符串(tool_result/command_exit/…)
    std::string goal_id;
    std::string iteration_id;
    std::string tool_use_id;
    std::string producer;
    nlohmann::json facts;
    std::string content_sha256;
    std::int64_t observed_at_ms = 0;
    bool fresh = true;
    bool truncated = false;
};

std::string SerializeGoalEvidence(const GoalEvidenceRecord& evidence, const std::string& ts);
std::optional<GoalEvidenceRecord> ParseGoalEvidence(const std::string& line);

}  // namespace lubancode::sessions
