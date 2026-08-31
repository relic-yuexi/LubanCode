#include "insights/friction_classifier.hpp"

#include <algorithm>
#include <map>
#include <string_view>

namespace lubancode::insights {
namespace {

const std::vector<std::string>& CategoryTable() {
    static const std::vector<std::string> kCategories = {
        "approval.wait",
        "budget.limit",
        "cancelled",
        "context.churn",
        "context.loss",
        "instruction.conflict",
        "permission.denied",
        "provider.failure",
        "request.ambiguity",
        "tool.execution_failure",
        "tool.invalid_input",
        "tool.repeated_retry",
        "unknown",
        "user.correction",
        "verification.failure",
        "verification.missing",
    };
    return kCategories;
}

bool ReasonMentions(std::string_view reason, std::string_view needle) {
    return reason.find(needle) != std::string_view::npos;
}

// 失败原因 -> 归哪类(词表保守:认不出的落 tool.execution_failure,
// 不冒充更细的类)。
const char* ClassifyToolFailureReason(std::string_view reason) {
    if (ReasonMentions(reason, "permission") || ReasonMentions(reason, "权限") ||
        ReasonMentions(reason, "denied") || ReasonMentions(reason, "not allowed")) {
        return "permission.denied";
    }
    if (ReasonMentions(reason, "invalid") || ReasonMentions(reason, "schema") ||
        ReasonMentions(reason, "argument") || ReasonMentions(reason, "参数")) {
        return "tool.invalid_input";
    }
    return "tool.execution_failure";
}

std::string Trimmed(std::string_view text, std::size_t limit) {
    if (text.size() <= limit) {
        return std::string(text);
    }
    return std::string(text.substr(0, limit)) + "…";
}

EvidenceItem ToolEvidence(const trajectory::EventEnvelope& envelope) {
    EvidenceItem item;
    item.metric = "tool_failure";
    item.value = Trimmed(envelope.payload.value("reason", ""), 80);
    item.event_id = envelope.event_id;
    return item;
}

}  // namespace

const std::vector<std::string>& AllFrictionCategories() {
    return CategoryTable();
}

std::vector<FrictionOccurrence> ClassifyFriction(
    const std::vector<trajectory::EventEnvelope>& events) {
    std::vector<FrictionOccurrence> out;
    // 同 turn 内按工具名记失败次数(repeated_retry 的证据底账)。
    std::map<std::string, std::map<std::string, int>> failures_per_turn;
    // 同 turn 内是否有过 verification(verification.missing 的判据)。
    std::map<std::string, bool> turn_verified;
    for (const auto& envelope : events) {
        if (envelope.kind == trajectory::EventKind::VerificationRecorded) {
            const std::string turn = envelope.turn_id.value_or("");
            turn_verified[turn] = true;
            if (!envelope.payload.value("passed", false)) {
                FrictionOccurrence occurrence;
                occurrence.category = "verification.failure";
                occurrence.evidence.metric = "verification_failed";
                occurrence.evidence.value = envelope.payload.value("kind", "");
                occurrence.evidence.event_id = envelope.event_id;
                occurrence.rule_version = std::string(kFrictionRuleVersion) + ":F01";
                out.push_back(std::move(occurrence));
            }
        }
    }
    for (const auto& envelope : events) {
        switch (envelope.kind) {
            case trajectory::EventKind::ToolExecutionFailed: {
                const std::string turn = envelope.turn_id.value_or("");
                const std::string tool = envelope.payload.value("tool_name", "");
                const char* category = ClassifyToolFailureReason(
                    envelope.payload.value("reason", ""));
                FrictionOccurrence occurrence;
                occurrence.category = category;
                occurrence.evidence = ToolEvidence(envelope);
                occurrence.evidence.value = nlohmann::json{{"tool", tool},
                                                           {"reason", occurrence.evidence.value}};
                occurrence.rule_version = std::string(kFrictionRuleVersion) + ":F02";
                out.push_back(std::move(occurrence));
                const int count = ++failures_per_turn[turn][tool];
                if (count >= 2) {
                    FrictionOccurrence retry;
                    retry.category = "tool.repeated_retry";
                    retry.evidence.metric = "tool_retry_count";
                    retry.evidence.value = nlohmann::json{{"tool", tool},
                                                          {"failures_in_turn", count}};
                    retry.evidence.event_id = envelope.event_id;
                    retry.rule_version = std::string(kFrictionRuleVersion) + ":F03";
                    out.push_back(std::move(retry));
                }
                break;
            }
            case trajectory::EventKind::ModelOutputFailed: {
                FrictionOccurrence occurrence;
                occurrence.category = "provider.failure";
                occurrence.evidence.metric = "model_output_failed";
                occurrence.evidence.value = Trimmed(envelope.payload.value("reason", ""), 80);
                occurrence.evidence.event_id = envelope.event_id;
                occurrence.rule_version = std::string(kFrictionRuleVersion) + ":F04";
                out.push_back(std::move(occurrence));
                break;
            }
            case trajectory::EventKind::ModelOutputCancelled:
            case trajectory::EventKind::TurnCancelled:
            case trajectory::EventKind::RunCancelled: {
                FrictionOccurrence occurrence;
                occurrence.category = "cancelled";
                occurrence.evidence.metric = "cancelled";
                occurrence.evidence.value = trajectory::EventKindName(envelope.kind);
                occurrence.evidence.event_id = envelope.event_id;
                occurrence.rule_version = std::string(kFrictionRuleVersion) + ":F05";
                out.push_back(std::move(occurrence));
                break;
            }
            case trajectory::EventKind::ControlApprovalRequested:
            case trajectory::EventKind::ControlApprovalExpired: {
                FrictionOccurrence occurrence;
                occurrence.category = "approval.wait";
                occurrence.evidence.metric = "approval";
                occurrence.evidence.value = trajectory::EventKindName(envelope.kind);
                occurrence.evidence.event_id = envelope.event_id;
                occurrence.rule_version = std::string(kFrictionRuleVersion) + ":F06";
                out.push_back(std::move(occurrence));
                break;
            }
            case trajectory::EventKind::OutcomeAssessed: {
                // 假完成信号只对 outcome=passed 判"缺验证":partial/failed
                // 本就没自称成功(§8.3"只能说明证据缺失")。
                const std::string outcome = envelope.payload.value("outcome", "");
                const std::string turn = envelope.turn_id.value_or("");
                if (outcome == "passed" && !turn_verified[turn]) {
                    FrictionOccurrence occurrence;
                    occurrence.category = "verification.missing";
                    occurrence.evidence.metric = "outcome_passed_without_verification";
                    occurrence.evidence.value = turn;
                    occurrence.evidence.event_id = envelope.event_id;
                    occurrence.rule_version = std::string(kFrictionRuleVersion) + ":F07";
                    out.push_back(std::move(occurrence));
                }
                break;
            }
            default:
                break;
        }
    }
    return out;
}

}  // namespace lubancode::insights
