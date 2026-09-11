// goal 事件行实现(纯函数;读侧钉 tests/unit/runtime/test_goal_restore.cpp)。
// 写侧(SerializeGoalEvent/SerializeGoalEvidence)已按 §4.67.6 收敛口径
// 删除:v3 起 goal 持久账走 trajectory 的 state.goal.applied + 不可变快照
// (runtime/goal_service.hpp),不再写 goal_v1 顶层类型。

#include "sessions/goal_session.hpp"

namespace lubancode::sessions {

namespace {

bool IsKnownGoalType(const std::string& type) {
    return type == "goal_v1" || type == "goal_iteration_v1" || type == "goal_evidence_v1" ||
           type == "goal_checkpoint_v1" || type == "goal_evaluation_v1";
}

}  // namespace

bool IsGoalEventLine(const std::string& line) {
    // 粗筛:顶层 type 是 goal 族才可能(子串快筛,真验在 ParseGoalEvent)。
    for (const char* prefix : {"\"type\":\"goal_v1\"", "\"type\": \"goal_v1\"",
                               "\"type\":\"goal_iteration_v1\"", "\"type\": \"goal_iteration_v1\"",
                               "\"type\":\"goal_evidence_v1\"", "\"type\": \"goal_evidence_v1\"",
                               "\"type\":\"goal_checkpoint_v1\"", "\"type\": \"goal_checkpoint_v1\"",
                               "\"type\":\"goal_evaluation_v1\"", "\"type\": \"goal_evaluation_v1\""}) {
        if (line.find(prefix) != std::string::npos) return true;
    }
    return false;
}

std::optional<GoalSessionEvent> ParseGoalEvent(const std::string& line) {
    const nlohmann::json j = nlohmann::json::parse(line, nullptr, /*allow_exceptions=*/false);
    if (!j.is_object()) return std::nullopt;
    if (!j.contains("type") || !j.at("type").is_string()) return std::nullopt;
    const std::string type = j.at("type").get<std::string>();
    if (!IsKnownGoalType(type)) return std::nullopt;
    if (!j.contains("event") || !j.at("event").is_string()) return std::nullopt;
    if (!j.contains("goal_id") || !j.at("goal_id").is_string()) return std::nullopt;

    GoalSessionEvent event;
    event.type = type;
    event.event = j.at("event").get<std::string>();
    event.goal_id = j.at("goal_id").get<std::string>();
    if (j.contains("iteration_id") && j.at("iteration_id").is_string()) {
        event.iteration_id = j.at("iteration_id").get<std::string>();
    }
    if (j.contains("revision") && j.at("revision").is_number_integer()) {
        event.revision = j.at("revision").get<int>();
    }
    if (j.contains("payload") && j.at("payload").is_object()) {
        event.payload = j.at("payload");
    }
    if (j.contains("timestamp_ms") && j.at("timestamp_ms").is_number()) {
        event.timestamp_ms = j.at("timestamp_ms").get<std::int64_t>();
    }
    // 证据行(goal_evidence_v1)的领域字段在顶层(evidence_id/kind/facts/
    // sha256/…):镜像进 payload,消费方(回放重建证据账)按 payload 取,
    // 不必二次解析原文。
    for (const char* key : {"evidence_id", "kind", "tool_use_id", "producer", "facts", "sha256",
                            "observed_at_ms", "fresh", "truncated", "goal_id", "iteration_id"}) {
        if (j.contains(key)) event.payload[key] = j.at(key);
    }
    return event;
}

}  // namespace lubancode::sessions
