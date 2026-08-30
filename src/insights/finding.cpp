#include "insights/finding.hpp"

namespace lubancode::insights {
namespace {

bool ReadString(const nlohmann::json& json, const char* key, std::string* out) {
    return json.contains(key) && json.at(key).is_string() &&
           (*out = json.at(key).get<std::string>(), true);
}

}  // namespace

const char* FindingSeverityName(FindingSeverity severity) {
    switch (severity) {
        case FindingSeverity::Info:
            return "info";
        case FindingSeverity::Warning:
            return "warning";
        case FindingSeverity::High:
            return "high";
    }
    return "";
}

std::optional<FindingSeverity> FindingSeverityFromName(std::string_view name) {
    if (name == "info") return FindingSeverity::Info;
    if (name == "warning") return FindingSeverity::Warning;
    if (name == "high") return FindingSeverity::High;
    return std::nullopt;
}

const char* FindingConfidenceName(FindingConfidence confidence) {
    switch (confidence) {
        case FindingConfidence::Low:
            return "low";
        case FindingConfidence::Medium:
            return "medium";
        case FindingConfidence::High:
            return "high";
    }
    return "";
}

std::optional<FindingConfidence> FindingConfidenceFromName(std::string_view name) {
    if (name == "low") return FindingConfidence::Low;
    if (name == "medium") return FindingConfidence::Medium;
    if (name == "high") return FindingConfidence::High;
    return std::nullopt;
}

const char* FindingOriginName(FindingOrigin origin) {
    switch (origin) {
        case FindingOrigin::DeterministicRule:
            return "deterministic_rule";
        case FindingOrigin::ModelReview:
            return "reviewed_suggestion";
    }
    return "";
}

std::optional<FindingOrigin> FindingOriginFromName(std::string_view name) {
    if (name == "deterministic_rule") return FindingOrigin::DeterministicRule;
    if (name == "reviewed_suggestion") return FindingOrigin::ModelReview;
    return std::nullopt;
}

nlohmann::json EvidenceItem::ToJson() const {
    nlohmann::json json = nlohmann::json::object();
    if (session_id.has_value()) {
        json["session_id"] = *session_id;
    }
    if (event_id.has_value()) {
        json["event_id"] = *event_id;
    }
    json["metric"] = metric;
    json["value"] = value;
    return json;
}

std::optional<EvidenceItem> EvidenceItem::FromJsonStrict(const nlohmann::json& json,
                                                         std::string* error) {
    if (!json.is_object()) {
        *error = "evidence 条目须是 object";
        return std::nullopt;
    }
    EvidenceItem item;
    if (!ReadString(json, "metric", &item.metric) || item.metric.empty()) {
        *error = "evidence.metric 必填";
        return std::nullopt;
    }
    if (!json.contains("value") || json.at("value").is_null()) {
        *error = "evidence.value 必填";
        return std::nullopt;
    }
    item.value = json.at("value");
    if (json.contains("session_id")) {
        if (!json.at("session_id").is_string()) {
            *error = "evidence.session_id 须是字符串";
            return std::nullopt;
        }
        item.session_id = json.at("session_id").get<std::string>();
    }
    if (json.contains("event_id")) {
        if (!json.at("event_id").is_string()) {
            *error = "evidence.event_id 须是字符串";
            return std::nullopt;
        }
        item.event_id = json.at("event_id").get<std::string>();
    }
    if (json.size() > 4) {
        *error = "evidence 未知键";
        return std::nullopt;
    }
    return item;
}

nlohmann::json Finding::ToJson() const {
    nlohmann::json json = nlohmann::json::object();
    json["finding_id"] = finding_id;
    json["category"] = category;
    json["severity"] = FindingSeverityName(severity);
    json["confidence"] = FindingConfidenceName(confidence);
    json["scope"] = scope;
    nlohmann::json evidence_json = nlohmann::json::array();
    for (const auto& item : evidence) {
        evidence_json.push_back(item.ToJson());
    }
    json["evidence"] = std::move(evidence_json);
    nlohmann::json counter_json = nlohmann::json::array();
    for (const auto& item : counter_evidence) {
        counter_json.push_back(item.ToJson());
    }
    json["counter_evidence"] = std::move(counter_json);
    json["summary"] = summary;
    json["recommendation"] = recommendation;
    json["origin"] = FindingOriginName(origin);
    json["rule_version"] = rule_version;
    return json;
}

std::optional<Finding> Finding::FromJsonStrict(const nlohmann::json& json, std::string* error) {
    if (!json.is_object()) {
        *error = "finding 须是 object";
        return std::nullopt;
    }
    Finding finding;
    if (!ReadString(json, "finding_id", &finding.finding_id) ||
        !ReadString(json, "category", &finding.category) || !ReadString(json, "scope", &finding.scope) ||
        !ReadString(json, "summary", &finding.summary) ||
        !ReadString(json, "recommendation", &finding.recommendation) ||
        !ReadString(json, "rule_version", &finding.rule_version)) {
        *error = "finding 缺必填字段";
        return std::nullopt;
    }
    std::string severity_name;
    std::string confidence_name;
    std::string origin_name;
    if (!ReadString(json, "severity", &severity_name) ||
        !ReadString(json, "confidence", &confidence_name) ||
        !ReadString(json, "origin", &origin_name)) {
        *error = "finding 缺 severity/confidence/origin";
        return std::nullopt;
    }
    const auto severity = FindingSeverityFromName(severity_name);
    const auto confidence = FindingConfidenceFromName(confidence_name);
    const auto origin = FindingOriginFromName(origin_name);
    if (!severity.has_value() || !confidence.has_value() || !origin.has_value()) {
        *error = "finding 枚举字段认不得";
        return std::nullopt;
    }
    finding.severity = *severity;
    finding.confidence = *confidence;
    finding.origin = *origin;
    for (const char* key : {"evidence", "counter_evidence"}) {
        if (!json.contains(key) || !json.at(key).is_array()) {
            *error = std::string(key) + " 须是数组";
            return std::nullopt;
        }
        for (const auto& item : json.at(key)) {
            const auto parsed = EvidenceItem::FromJsonStrict(item, error);
            if (!parsed.has_value()) {
                return std::nullopt;
            }
            (key == std::string_view("evidence") ? finding.evidence : finding.counter_evidence)
                .push_back(std::move(*parsed));
        }
    }
    if (json.size() != 11) {
        *error = "finding 未知键";
        return std::nullopt;
    }
    return finding;
}

}  // namespace lubancode::insights
