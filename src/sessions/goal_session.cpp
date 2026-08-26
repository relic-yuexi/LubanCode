// goal 事件行实现(纯函数;测试钉 tests/test_goal_session.cpp)。

#include "sessions/goal_session.hpp"

#include "platform/json_safe.hpp"  // DumpJsonSanitized:落档行的编码窄边界

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

std::string SerializeGoalEvent(const GoalSessionEvent& event, const std::string& ts) {
    nlohmann::json j;
    j["type"] = event.type;
    j["event"] = event.event;
    j["goal_id"] = event.goal_id;
    if (!event.iteration_id.empty()) j["iteration_id"] = event.iteration_id;
    if (event.revision != 0) j["revision"] = event.revision;
    j["payload"] = event.payload;
    j["ts"] = ts;
    if (event.timestamp_ms != 0) j["timestamp_ms"] = event.timestamp_ms;
    // 落档行(session JSONL,要被 /resume 重新读):坏串窄边界,同
    // SerializeSessionMessage 的成例——payload 里混进坏 UTF-8(工具输出
    // 截断劈进字腰那类)不许抛 316 穿透顶层,宁可替换字符洗过。
    return platform::DumpJsonSanitized(j);
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

std::string SerializeGoalEvidence(const GoalEvidenceRecord& evidence, const std::string& ts) {
    nlohmann::json j;
    j["type"] = "goal_evidence_v1";
    j["event"] = "observed";
    j["evidence_id"] = evidence.id;
    j["kind"] = evidence.kind;
    j["goal_id"] = evidence.goal_id;
    j["iteration_id"] = evidence.iteration_id;
    j["tool_use_id"] = evidence.tool_use_id;
    j["producer"] = evidence.producer;
    j["facts"] = evidence.facts;
    j["sha256"] = evidence.content_sha256;
    j["observed_at_ms"] = evidence.observed_at_ms;
    j["fresh"] = evidence.fresh;
    j["truncated"] = evidence.truncated;
    j["ts"] = ts;
    // 证据行同样是落档行:facts 从工具输出采来,坏串窄边界同上。
    return platform::DumpJsonSanitized(j);
}

std::optional<GoalEvidenceRecord> ParseGoalEvidence(const std::string& line) {
    const nlohmann::json j = nlohmann::json::parse(line, nullptr, /*allow_exceptions=*/false);
    if (!j.is_object()) return std::nullopt;
    if (!j.contains("type") || !j.at("type").is_string()) return std::nullopt;
    if (j.at("type").get<std::string>() != "goal_evidence_v1") return std::nullopt;
    if (!j.contains("evidence_id") || !j.at("evidence_id").is_string()) return std::nullopt;

    GoalEvidenceRecord e;
    e.id = j.at("evidence_id").get<std::string>();
    auto read_str = [&j](const char* key, std::string& out) {
        if (j.contains(key) && j.at(key).is_string()) out = j.at(key).get<std::string>();
    };
    read_str("kind", e.kind);
    read_str("goal_id", e.goal_id);
    read_str("iteration_id", e.iteration_id);
    read_str("tool_use_id", e.tool_use_id);
    read_str("producer", e.producer);
    if (j.contains("facts") && j.at("facts").is_object()) e.facts = j.at("facts");
    read_str("sha256", e.content_sha256);
    if (j.contains("observed_at_ms") && j.at("observed_at_ms").is_number()) {
        e.observed_at_ms = j.at("observed_at_ms").get<std::int64_t>();
    }
    if (j.contains("fresh") && j.at("fresh").is_boolean()) e.fresh = j.at("fresh").get<bool>();
    if (j.contains("truncated") && j.at("truncated").is_boolean()) {
        e.truncated = j.at("truncated").get<bool>();
    }
    return e;
}

}  // namespace lubancode::sessions
