#include "insights/friction_classifier.hpp"

#include <algorithm>
#include <map>
#include <string_view>

#include "trajectory/v3/envelope.hpp"

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

// ---------------------------------------------------------------------------
// v3 半场(T14)
// ---------------------------------------------------------------------------

const std::vector<std::string>& UnsupportedFrictionCategoriesV3() {
    static const std::vector<std::string> kUnsupported = {
        // 宿主审批事实(ConfirmToolUse 档)未入 v3(T11 域):渠道审批事件
        // (channel.approval.*)是 QQ 渠道账,不是宿主工具审批,不混用。
        "approval.wait",
        // verification/outcome 事实未入 v3(T11 域):不判分,也不把
        // "没有 verification 记录"推断成"任务没验证/已完成"。
        "verification.failure",
        "verification.missing",
    };
    return kUnsupported;
}

std::vector<FrictionOccurrence> ClassifyFrictionV3(const std::vector<V3SessionFacts>& sessions) {
    std::vector<FrictionOccurrence> out;
    // 重试证据底账:同 (session, turn, 工具) 的失败次数;同 actionId 的
    // 失败 attempt 也算(v3 重试按 attempt 记,§4.15)。
    std::map<std::string, std::map<std::string, int>> failures_per_turn;
    std::map<std::string, int> failures_per_action;
    for (const auto& session : sessions) {
        for (const auto& failure : session.tool_failures) {
            if (failure.phase != "execution") {
                continue;  // 落盘失败单列,不进重试底账
            }
            const std::string tool = failure.tool_name.empty() ? failure.action_id
                                                               : failure.tool_name;
            ++failures_per_turn[failure.session_id + "/" + failure.turn_id][tool];
            ++failures_per_action[failure.session_id + "/" + failure.action_id];
        }
    }
    for (const auto& session : sessions) {
        // ---- 工具失败(执行/落盘分开;按 reason 词表保守归类)----
        for (const auto& failure : session.tool_failures) {
            FrictionOccurrence occurrence;
            if (failure.phase == "persist") {
                // 落盘失败:结果没立起来,与执行失败分开表达(§3.3)。
                occurrence.category = "tool.execution_failure";
                occurrence.evidence.metric = "tool_persist_failed";
            } else {
                occurrence.category =
                    ClassifyToolFailureReason(failure.reason);
                occurrence.evidence.metric = "tool_failure";
            }
            occurrence.evidence.value = nlohmann::json{
                {"tool", failure.tool_name},
                {"action", failure.action_id},
                {"attempt", failure.attempt},
                {"phase", failure.phase},
                {"reason", failure.reason}};
            occurrence.evidence.session_id = failure.session_id;
            occurrence.evidence.event_id = failure.event_id;
            if (failure.seq > 0) {
                occurrence.evidence.seq = failure.seq;
            }
            occurrence.rule_version = std::string(kFrictionRuleVersionV3) + ":F02";
            out.push_back(std::move(occurrence));
            if (failure.phase != "execution") {
                continue;
            }
            const std::string tool = failure.tool_name.empty() ? failure.action_id
                                                               : failure.tool_name;
            const int in_turn =
                failures_per_turn[failure.session_id + "/" + failure.turn_id][tool];
            const int in_action =
                failures_per_action[failure.session_id + "/" + failure.action_id];
            if (in_turn >= 2 || in_action >= 2) {
                FrictionOccurrence retry;
                retry.category = "tool.repeated_retry";
                retry.evidence.metric = "tool_retry_count";
                retry.evidence.value = nlohmann::json{
                    {"tool", tool},
                    {"action", failure.action_id},
                    {"failures_in_turn", in_turn},
                    {"failures_in_action", in_action}};
                retry.evidence.session_id = failure.session_id;
                retry.evidence.event_id = failure.event_id;
                if (failure.seq > 0) {
                    retry.evidence.seq = failure.seq;
                }
                retry.rule_version = std::string(kFrictionRuleVersionV3) + ":F03";
                out.push_back(std::move(retry));
            }
        }
        // ---- 取消:折叠快照里 cancelled 的尝试,每枚一次 ----
        for (const auto& action : session.tool_actions) {
            for (const auto& attempt : action.snapshot.attempts) {
                if (attempt.status != "cancelled") {
                    continue;
                }
                FrictionOccurrence occurrence;
                occurrence.category = "cancelled";
                occurrence.evidence.metric = "tool_attempt_cancelled";
                occurrence.evidence.value = nlohmann::json{
                    {"tool", action.snapshot.tool_name.value_or("")},
                    {"action", action.snapshot.tool_call_id},
                    {"attempt", attempt.attempt}};
                occurrence.evidence.session_id = action.session_id;
                occurrence.evidence.event_id =
                    attempt.event_ids.empty() ? std::string() : attempt.event_ids.front();
                occurrence.rule_version = std::string(kFrictionRuleVersionV3) + ":F05";
                out.push_back(std::move(occurrence));
            }
        }
        // ---- 模型请求失败/取消(requests 的 outcome 折账)----
        for (const auto& request : session.requests) {
            if (request.outcome != "failed" && request.outcome != "cancelled") {
                continue;
            }
            FrictionOccurrence occurrence;
            occurrence.category =
                request.outcome == "failed" ? "provider.failure" : "cancelled";
            occurrence.evidence.metric =
                request.outcome == "failed" ? "model_request_failed" : "model_request_cancelled";
            occurrence.evidence.value = nlohmann::json{{"request", request.request_id},
                                                       {"purpose", request.purpose}};
            occurrence.evidence.session_id = request.session_id;
            // 失败/取消证据锚在终态事件行(没有就退回 prepared 行)。
            occurrence.evidence.event_id =
                request.outcome_event_id.empty() ? request.event_id : request.outcome_event_id;
            const std::uint64_t outcome_seq =
                request.outcome_event_id.empty() ? request.seq : request.outcome_seq;
            if (outcome_seq > 0) {
                occurrence.evidence.seq = outcome_seq;
            }
            occurrence.rule_version = std::string(kFrictionRuleVersionV3) +
                                      (request.outcome == "failed" ? ":F04" : ":F05");
            out.push_back(std::move(occurrence));
        }
    }
    // approval.wait / verification.*:UnsupportedFrictionCategoriesV3() 在册,
    // v3 无事实不判——不因缺记录推断"没有审批/没有验证"。
    return out;
}

}  // namespace lubancode::insights
