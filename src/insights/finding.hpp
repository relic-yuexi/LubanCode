// Finding 合同(Token 账本单 §8.4 A0 冻结)。
//
// Prompt Audit 与 Insights 的每条发现统一长这一样:证据、置信、严重度、
// 建议各有各的栏,不许混。严重度讲影响(info/warning/high),置信讲证据
// (low/medium/high),两把尺子不换算。本地规则先跑(origin=
// deterministic_rule);模型评议只能给 reviewed_suggestion,不能把怀疑
// 升成事实——引用不存在 finding_id 的评议,renderer 丢弃并记错。
#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

namespace lubancode::insights {

enum class FindingSeverity { Info, Warning, High };
const char* FindingSeverityName(FindingSeverity severity);
std::optional<FindingSeverity> FindingSeverityFromName(std::string_view name);

enum class FindingConfidence { Low, Medium, High };
const char* FindingConfidenceName(FindingConfidence confidence);
std::optional<FindingConfidence> FindingConfidenceFromName(std::string_view name);

enum class FindingOrigin { DeterministicRule, ModelReview };
const char* FindingOriginName(FindingOrigin origin);
std::optional<FindingOrigin> FindingOriginFromName(std::string_view name);

// 一条证据:metric + value 必有,session/event 引用可选。
struct EvidenceItem {
    std::string metric;
    nlohmann::json value;                  // 数值/字符串/枚举,随 metric 定
    std::optional<std::string> session_id; // 证据落在哪场 session
    std::optional<std::string> event_id;   // 证据落在哪枚事件

    nlohmann::json ToJson() const;
    static std::optional<EvidenceItem> FromJsonStrict(const nlohmann::json& json,
                                                      std::string* error);
};

struct Finding {
    std::string finding_id;      // "P-AUD-017" / "INS-0003"
    std::string category;        // "cache.prefix_churn" / "instruction.conflict" / …
    FindingSeverity severity = FindingSeverity::Info;
    FindingConfidence confidence = FindingConfidence::Low;
    std::string scope;           // "session" / "workspace"
    std::vector<EvidenceItem> evidence;
    std::vector<EvidenceItem> counter_evidence;
    std::string summary;
    std::string recommendation;
    FindingOrigin origin = FindingOrigin::DeterministicRule;
    std::string rule_version;    // "prompt-audit-v1:P017"

    nlohmann::json ToJson() const;
    static std::optional<Finding> FromJsonStrict(const nlohmann::json& json, std::string* error);
};

}  // namespace lubancode::insights
