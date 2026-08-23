// goal_types 实现:枚举映射、objective 校验、序列化、转换表。
// 纯函数,零 IO;测试钉 test_goal_types.cpp。

#include "runtime/goal_types.hpp"

#include <algorithm>
#include <map>

namespace lubancode::runtime::goal {

// ---------------------------------------------------------------------------
// 枚举 <-> 稳定字符串
// ---------------------------------------------------------------------------

std::string ToString(GoalState state) {
    switch (state) {
        case GoalState::Preparing: return "preparing";
        case GoalState::Active: return "active";
        case GoalState::Running: return "running";
        case GoalState::Evaluating: return "evaluating";
        case GoalState::Pausing: return "pausing";
        case GoalState::Paused: return "paused";
        case GoalState::AwaitingApproval: return "awaiting_approval";
        case GoalState::AwaitingUser: return "awaiting_user";
        case GoalState::Blocked: return "blocked";
        case GoalState::Achieved: return "achieved";
        case GoalState::BudgetExhausted: return "budget_exhausted";
        case GoalState::SuspendedByPolicy: return "suspended_by_policy";
        case GoalState::Failed: return "failed";
        case GoalState::Cleared: return "cleared";
    }
    return "unknown";
}

bool ParseGoalState(const std::string& s, GoalState& out) {
    static const std::map<std::string, GoalState, std::less<>> kMap = {
        {"preparing", GoalState::Preparing},         {"active", GoalState::Active},
        {"running", GoalState::Running},             {"evaluating", GoalState::Evaluating},
        {"pausing", GoalState::Pausing},             {"paused", GoalState::Paused},
        {"awaiting_approval", GoalState::AwaitingApproval}, {"awaiting_user", GoalState::AwaitingUser},
        {"blocked", GoalState::Blocked},             {"achieved", GoalState::Achieved},
        {"budget_exhausted", GoalState::BudgetExhausted}, {"suspended_by_policy", GoalState::SuspendedByPolicy},
        {"failed", GoalState::Failed},               {"cleared", GoalState::Cleared},
    };
    const auto it = kMap.find(s);
    if (it == kMap.end()) return false;
    out = it->second;
    return true;
}

bool IsGoalTerminal(GoalState state) {
    switch (state) {
        case GoalState::Achieved:
        case GoalState::BudgetExhausted:
        case GoalState::SuspendedByPolicy:
        case GoalState::Failed:
        case GoalState::Cleared:
            return true;
        default:
            return false;
    }
}

bool IsGoalResumable(GoalState state) {
    return state == GoalState::Paused || state == GoalState::AwaitingUser ||
           state == GoalState::Blocked;
}

std::string ToString(GoalIterationPhase phase) {
    switch (phase) {
        case GoalIterationPhase::Scheduled: return "scheduled";
        case GoalIterationPhase::Running: return "running";
        case GoalIterationPhase::Checkpointed: return "checkpointed";
        case GoalIterationPhase::Evaluating: return "evaluating";
        case GoalIterationPhase::Finished: return "finished";
        case GoalIterationPhase::Interrupted: return "interrupted";
    }
    return "unknown";
}

bool ParseGoalIterationPhase(const std::string& s, GoalIterationPhase& out) {
    static const std::map<std::string, GoalIterationPhase, std::less<>> kMap = {
        {"scheduled", GoalIterationPhase::Scheduled},   {"running", GoalIterationPhase::Running},
        {"checkpointed", GoalIterationPhase::Checkpointed}, {"evaluating", GoalIterationPhase::Evaluating},
        {"finished", GoalIterationPhase::Finished},     {"interrupted", GoalIterationPhase::Interrupted},
    };
    const auto it = kMap.find(s);
    if (it == kMap.end()) return false;
    out = it->second;
    return true;
}

std::string ToString(CheckpointStatus status) {
    switch (status) {
        case CheckpointStatus::Progress: return "progress";
        case CheckpointStatus::ReadyForEvaluation: return "ready_for_evaluation";
        case CheckpointStatus::Blocked: return "blocked";
        case CheckpointStatus::NeedsUser: return "needs_user";
    }
    return "unknown";
}

bool ParseCheckpointStatus(const std::string& s, CheckpointStatus& out) {
    static const std::map<std::string, CheckpointStatus, std::less<>> kMap = {
        {"progress", CheckpointStatus::Progress},
        {"ready_for_evaluation", CheckpointStatus::ReadyForEvaluation},
        {"blocked", CheckpointStatus::Blocked},
        {"needs_user", CheckpointStatus::NeedsUser},
    };
    const auto it = kMap.find(s);
    if (it == kMap.end()) return false;
    out = it->second;
    return true;
}

std::string ToString(GoalDecision decision) {
    switch (decision) {
        case GoalDecision::Continue: return "continue";
        case GoalDecision::Achieved: return "achieved";
        case GoalDecision::Blocked: return "blocked";
        case GoalDecision::NeedsUser: return "needs_user";
    }
    return "unknown";
}

bool ParseGoalDecision(const std::string& s, GoalDecision& out) {
    static const std::map<std::string, GoalDecision, std::less<>> kMap = {
        {"continue", GoalDecision::Continue}, {"achieved", GoalDecision::Achieved},
        {"blocked", GoalDecision::Blocked},   {"needs_user", GoalDecision::NeedsUser},
    };
    const auto it = kMap.find(s);
    if (it == kMap.end()) return false;
    out = it->second;
    return true;
}

std::string ToString(EvidenceKind kind) {
    switch (kind) {
        case EvidenceKind::ToolResult: return "tool_result";
        case EvidenceKind::CommandExit: return "command_exit";
        case EvidenceKind::TestReport: return "test_report";
        case EvidenceKind::FileDigest: return "file_digest";
        case EvidenceKind::GitDiffSummary: return "git_diff_summary";
        case EvidenceKind::Artifact: return "artifact";
        case EvidenceKind::UserDecision: return "user_decision";
        case EvidenceKind::RuntimeError: return "runtime_error";
    }
    return "unknown";
}

bool ParseEvidenceKind(const std::string& s, EvidenceKind& out) {
    static const std::map<std::string, EvidenceKind, std::less<>> kMap = {
        {"tool_result", EvidenceKind::ToolResult},
        {"command_exit", EvidenceKind::CommandExit},
        {"test_report", EvidenceKind::TestReport},
        {"file_digest", EvidenceKind::FileDigest},
        {"git_diff_summary", EvidenceKind::GitDiffSummary},
        {"artifact", EvidenceKind::Artifact},
        {"user_decision", EvidenceKind::UserDecision},
        {"runtime_error", EvidenceKind::RuntimeError},
    };
    const auto it = kMap.find(s);
    if (it == kMap.end()) return false;
    out = it->second;
    return true;
}

// ---------------------------------------------------------------------------
// objective 校验
// ---------------------------------------------------------------------------

namespace {

std::size_t Utf8LeadLen(unsigned char lead) {
    if ((lead & 0x80U) == 0x00U) return 1;
    if ((lead & 0xE0U) == 0xC0U) return 2;
    if ((lead & 0xF0U) == 0xE0U) return 3;
    if ((lead & 0xF8U) == 0xF0U) return 4;
    return 1;  // 非法首字节按 1 前进,不死循环
}

}  // namespace

std::size_t CountGoalObjectiveChars(const std::string& text) {
    std::size_t count = 0;
    for (std::size_t i = 0; i < text.size();) {
        i += Utf8LeadLen(static_cast<unsigned char>(text[i]));
        ++count;
    }
    return count;
}

std::string ValidateGoalObjective(const std::string& objective) {
    // trim(只剥 ASCII 空白;CRLF 归一进正文,不算空白尾巴的 \r 由调用方去)。
    std::size_t begin = 0;
    std::size_t end = objective.size();
    while (begin < end) {
        const char c = objective[begin];
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
            ++begin;
        } else {
            break;
        }
    }
    while (end > begin) {
        const char c = objective[end - 1];
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
            --end;
        } else {
            break;
        }
    }
    if (begin >= end) return kErrGoalObjectiveEmpty;
    if (CountGoalObjectiveChars(objective) > kGoalObjectiveMaxChars) {
        return kErrGoalObjectiveTooLong;
    }
    return std::string();
}

// ---------------------------------------------------------------------------
// 序列化
// ---------------------------------------------------------------------------

nlohmann::json GoalContract::to_json() const {
    nlohmann::json j;
    j["objective"] = objective;
    j["in_scope"] = in_scope;
    j["out_of_scope"] = out_of_scope;
    nlohmann::json criteria_arr = nlohmann::json::array();
    for (const auto& c : criteria) {
        nlohmann::json cj;
        cj["id"] = c.id;
        cj["text"] = c.text;
        cj["required"] = c.required;
        criteria_arr.push_back(std::move(cj));
    }
    j["criteria"] = std::move(criteria_arr);
    j["validation_commands"] = validation_commands;
    j["required_artifacts"] = required_artifacts;
    j["constraints"] = constraints;
    j["checkpoints"] = checkpoints;
    j["pause_conditions"] = pause_conditions;
    return j;
}

GoalContract GoalContract::from_json(const nlohmann::json& j) {
    GoalContract c;
    if (j.contains("objective")) c.objective = j.at("objective").get<std::string>();
    auto read_list = [&j](const char* key, std::vector<std::string>& out) {
        out.clear();
        if (!j.contains(key) || !j.at(key).is_array()) return;
        for (const auto& item : j.at(key)) {
            if (item.is_string()) out.push_back(item.get<std::string>());
        }
    };
    read_list("in_scope", c.in_scope);
    read_list("out_of_scope", c.out_of_scope);
    if (j.contains("criteria") && j.at("criteria").is_array()) {
        for (const auto& item : j.at("criteria")) {
            if (!item.is_object()) continue;
            GoalCriterion gc;
            if (item.contains("id") && item.at("id").is_string()) gc.id = item.at("id").get<std::string>();
            if (item.contains("text") && item.at("text").is_string()) gc.text = item.at("text").get<std::string>();
            if (item.contains("required") && item.at("required").is_boolean()) {
                gc.required = item.at("required").get<bool>();
            }
            if (!gc.id.empty()) c.criteria.push_back(std::move(gc));
        }
    }
    read_list("validation_commands", c.validation_commands);
    read_list("required_artifacts", c.required_artifacts);
    read_list("constraints", c.constraints);
    read_list("checkpoints", c.checkpoints);
    read_list("pause_conditions", c.pause_conditions);
    return c;
}

nlohmann::json GoalBudget::to_json() const {
    nlohmann::json j = nlohmann::json::object();
    if (max_total_tokens.has_value()) j["max_total_tokens"] = *max_total_tokens;
    if (max_elapsed_ms.has_value()) j["max_elapsed_ms"] = *max_elapsed_ms;
    if (max_iterations.has_value()) j["max_iterations"] = *max_iterations;
    if (max_cost_micros.has_value()) j["max_cost_micros"] = *max_cost_micros;
    j["max_no_progress_iterations"] = max_no_progress_iterations;
    j["max_same_blocker_iterations"] = max_same_blocker_iterations;
    j["max_consecutive_provider_failures"] = max_consecutive_provider_failures;
    return j;
}

GoalBudget GoalBudget::from_json(const nlohmann::json& j) {
    GoalBudget b;
    auto opt_int = [&j](const char* key) -> std::optional<std::int64_t> {
        if (!j.contains(key)) return std::nullopt;
        const auto& v = j.at(key);
        if (v.is_number_integer()) return v.get<std::int64_t>();
        if (v.is_number_unsigned()) return static_cast<std::int64_t>(v.get<std::uint64_t>());
        return std::nullopt;
    };
    b.max_total_tokens = opt_int("max_total_tokens");
    b.max_elapsed_ms = opt_int("max_elapsed_ms");
    b.max_cost_micros = opt_int("max_cost_micros");
    if (j.contains("max_iterations") && j.at("max_iterations").is_number_integer()) {
        b.max_iterations = j.at("max_iterations").get<int>();
    }
    auto opt_int_field = [&j](const char* key, int fallback) {
        if (j.contains(key) && j.at(key).is_number_integer()) return j.at(key).get<int>();
        return fallback;
    };
    b.max_no_progress_iterations = opt_int_field("max_no_progress_iterations", b.max_no_progress_iterations);
    b.max_same_blocker_iterations = opt_int_field("max_same_blocker_iterations", b.max_same_blocker_iterations);
    b.max_consecutive_provider_failures =
        opt_int_field("max_consecutive_provider_failures", b.max_consecutive_provider_failures);
    return b;
}

void GoalUsage::Add(const GoalUsage& other) {
    input_tokens += other.input_tokens;
    output_tokens += other.output_tokens;
    cache_read_tokens += other.cache_read_tokens;
    cache_creation_tokens += other.cache_creation_tokens;
    reasoning_tokens += other.reasoning_tokens;
    request_count += other.request_count;
    duration_ms += other.duration_ms;
    usage_reported = usage_reported || other.usage_reported;
}

nlohmann::json GoalUsage::to_json() const {
    nlohmann::json j;
    j["input_tokens"] = input_tokens;
    j["output_tokens"] = output_tokens;
    j["cache_read_tokens"] = cache_read_tokens;
    j["cache_creation_tokens"] = cache_creation_tokens;
    j["reasoning_tokens"] = reasoning_tokens;
    j["request_count"] = request_count;
    j["duration_ms"] = duration_ms;
    j["usage_reported"] = usage_reported;
    return j;
}

GoalUsage GoalUsage::from_json(const nlohmann::json& j) {
    GoalUsage u;
    auto read_i64 = [&j](const char* key, std::int64_t& out) {
        if (j.contains(key) && j.at(key).is_number()) out = j.at(key).get<std::int64_t>();
    };
    read_i64("input_tokens", u.input_tokens);
    read_i64("output_tokens", u.output_tokens);
    read_i64("cache_read_tokens", u.cache_read_tokens);
    read_i64("cache_creation_tokens", u.cache_creation_tokens);
    read_i64("reasoning_tokens", u.reasoning_tokens);
    read_i64("request_count", u.request_count);
    read_i64("duration_ms", u.duration_ms);
    if (j.contains("usage_reported") && j.at("usage_reported").is_boolean()) {
        u.usage_reported = j.at("usage_reported").get<bool>();
    }
    return u;
}

nlohmann::json GoalCheckpoint::to_json() const {
    nlohmann::json j;
    j["version"] = version;
    j["summary"] = summary;
    j["completed"] = completed;
    j["remaining"] = remaining;
    j["validations"] = validations;
    j["evidence_ids"] = evidence_ids;
    j["next_action"] = next_action;
    if (blocker_key.has_value()) j["blocker_key"] = *blocker_key;
    if (question.has_value()) j["question"] = *question;
    j["progress_fingerprint"] = progress_fingerprint;
    j["synthesized"] = synthesized;
    return j;
}

GoalCheckpoint GoalCheckpoint::from_json(const nlohmann::json& j) {
    GoalCheckpoint c;
    if (j.contains("version") && j.at("version").is_number_integer()) c.version = j.at("version").get<int>();
    auto read_str = [&j](const char* key, std::string& out) {
        if (j.contains(key) && j.at(key).is_string()) out = j.at(key).get<std::string>();
    };
    read_str("summary", c.summary);
    auto read_list = [&j](const char* key, std::vector<std::string>& out) {
        out.clear();
        if (!j.contains(key) || !j.at(key).is_array()) return;
        for (const auto& item : j.at(key)) {
            if (item.is_string()) out.push_back(item.get<std::string>());
        }
    };
    read_list("completed", c.completed);
    read_list("remaining", c.remaining);
    read_list("validations", c.validations);
    read_list("evidence_ids", c.evidence_ids);
    read_str("next_action", c.next_action);
    if (j.contains("blocker_key") && j.at("blocker_key").is_string()) {
        c.blocker_key = j.at("blocker_key").get<std::string>();
    }
    if (j.contains("question") && j.at("question").is_string()) {
        c.question = j.at("question").get<std::string>();
    }
    read_str("progress_fingerprint", c.progress_fingerprint);
    if (j.contains("synthesized") && j.at("synthesized").is_boolean()) {
        c.synthesized = j.at("synthesized").get<bool>();
    }
    return c;
}

nlohmann::json GoalEvaluation::to_json() const {
    nlohmann::json j;
    j["id"] = id;
    j["decision"] = ToString(decision);
    j["summary"] = summary;
    j["progress"] = progress;
    nlohmann::json arr = nlohmann::json::array();
    for (const auto& v : criteria) {
        nlohmann::json vj;
        vj["id"] = v.id;
        vj["status"] = v.status;
        vj["evidence_ids"] = v.evidence_ids;
        vj["reason"] = v.reason;
        arr.push_back(std::move(vj));
    }
    j["criteria"] = std::move(arr);
    j["next_action"] = next_action;
    if (blocker_key.has_value()) j["blocker_key"] = *blocker_key;
    if (question.has_value()) j["question"] = *question;
    j["confidence"] = confidence;
    j["overridden_achieved"] = overridden_achieved;
    j["override_reason"] = override_reason;
    return j;
}

GoalEvaluation GoalEvaluation::from_json(const nlohmann::json& j) {
    GoalEvaluation e;
    auto read_str = [&j](const char* key, std::string& out) {
        if (j.contains(key) && j.at(key).is_string()) out = j.at(key).get<std::string>();
    };
    read_str("id", e.id);
    std::string decision;
    read_str("decision", decision);
    if (!decision.empty()) ParseGoalDecision(decision, e.decision);
    read_str("summary", e.summary);
    if (j.contains("progress") && j.at("progress").is_boolean()) e.progress = j.at("progress").get<bool>();
    if (j.contains("criteria") && j.at("criteria").is_array()) {
        for (const auto& item : j.at("criteria")) {
            if (!item.is_object()) continue;
            CriterionVerdict v;
            if (item.contains("id") && item.at("id").is_string()) v.id = item.at("id").get<std::string>();
            if (item.contains("status") && item.at("status").is_string()) v.status = item.at("status").get<std::string>();
            if (item.contains("evidence_ids") && item.at("evidence_ids").is_array()) {
                for (const auto& ev : item.at("evidence_ids")) {
                    if (ev.is_string()) v.evidence_ids.push_back(ev.get<std::string>());
                }
            }
            if (item.contains("reason") && item.at("reason").is_string()) v.reason = item.at("reason").get<std::string>();
            if (!v.id.empty()) e.criteria.push_back(std::move(v));
        }
    }
    read_str("next_action", e.next_action);
    if (j.contains("blocker_key") && j.at("blocker_key").is_string()) {
        e.blocker_key = j.at("blocker_key").get<std::string>();
    }
    if (j.contains("question") && j.at("question").is_string()) {
        e.question = j.at("question").get<std::string>();
    }
    if (j.contains("confidence") && j.at("confidence").is_number()) {
        e.confidence = j.at("confidence").get<double>();
    }
    if (j.contains("overridden_achieved") && j.at("overridden_achieved").is_boolean()) {
        e.overridden_achieved = j.at("overridden_achieved").get<bool>();
    }
    read_str("override_reason", e.override_reason);
    return e;
}

nlohmann::json GoalEvidence::to_json() const {
    nlohmann::json j;
    j["id"] = id;
    j["kind"] = ToString(kind);
    j["goal_id"] = goal_id;
    j["iteration_id"] = iteration_id;
    j["tool_use_id"] = tool_use_id;
    j["producer"] = producer;
    j["facts"] = facts;
    j["content_sha256"] = content_sha256;
    j["observed_at_ms"] = observed_at_ms;
    j["fresh"] = fresh;
    j["truncated"] = truncated;
    return j;
}

GoalEvidence GoalEvidence::from_json(const nlohmann::json& j) {
    GoalEvidence e;
    auto read_str = [&j](const char* key, std::string& out) {
        if (j.contains(key) && j.at(key).is_string()) out = j.at(key).get<std::string>();
    };
    read_str("id", e.id);
    std::string kind;
    read_str("kind", kind);
    if (!kind.empty()) ParseEvidenceKind(kind, e.kind);
    read_str("goal_id", e.goal_id);
    read_str("iteration_id", e.iteration_id);
    read_str("tool_use_id", e.tool_use_id);
    read_str("producer", e.producer);
    if (j.contains("facts") && j.at("facts").is_object()) e.facts = j.at("facts");
    read_str("content_sha256", e.content_sha256);
    if (j.contains("observed_at_ms") && j.at("observed_at_ms").is_number()) {
        e.observed_at_ms = j.at("observed_at_ms").get<std::int64_t>();
    }
    if (j.contains("fresh") && j.at("fresh").is_boolean()) e.fresh = j.at("fresh").get<bool>();
    if (j.contains("truncated") && j.at("truncated").is_boolean()) e.truncated = j.at("truncated").get<bool>();
    return e;
}

nlohmann::json GoalIteration::to_json() const {
    nlohmann::json j;
    j["id"] = id;
    j["goal_id"] = goal_id;
    j["index"] = index;
    j["goal_revision"] = goal_revision;
    j["turn_id"] = turn_id;
    j["phase"] = ToString(phase);
    j["attempt"] = attempt;
    j["started_at_ms"] = started_at_ms;
    j["finished_at_ms"] = finished_at_ms;
    j["usage"] = usage.to_json();
    j["before_workspace_fingerprint"] = before_workspace_fingerprint;
    j["after_workspace_fingerprint"] = after_workspace_fingerprint;
    j["evidence_ids"] = evidence_ids;
    if (checkpoint.has_value()) j["checkpoint"] = checkpoint->to_json();
    return j;
}

GoalIteration GoalIteration::from_json(const nlohmann::json& j) {
    GoalIteration it;
    auto read_str = [&j](const char* key, std::string& out) {
        if (j.contains(key) && j.at(key).is_string()) out = j.at(key).get<std::string>();
    };
    read_str("id", it.id);
    read_str("goal_id", it.goal_id);
    if (j.contains("index") && j.at("index").is_number_integer()) it.index = j.at("index").get<int>();
    if (j.contains("goal_revision") && j.at("goal_revision").is_number_integer()) {
        it.goal_revision = j.at("goal_revision").get<int>();
    }
    read_str("turn_id", it.turn_id);
    std::string phase;
    read_str("phase", phase);
    if (!phase.empty()) ParseGoalIterationPhase(phase, it.phase);
    if (j.contains("attempt") && j.at("attempt").is_number_integer()) it.attempt = j.at("attempt").get<int>();
    if (j.contains("started_at_ms") && j.at("started_at_ms").is_number()) {
        it.started_at_ms = j.at("started_at_ms").get<std::int64_t>();
    }
    if (j.contains("finished_at_ms") && j.at("finished_at_ms").is_number()) {
        it.finished_at_ms = j.at("finished_at_ms").get<std::int64_t>();
    }
    if (j.contains("usage")) it.usage = GoalUsage::from_json(j.at("usage"));
    read_str("before_workspace_fingerprint", it.before_workspace_fingerprint);
    read_str("after_workspace_fingerprint", it.after_workspace_fingerprint);
    if (j.contains("evidence_ids") && j.at("evidence_ids").is_array()) {
        for (const auto& ev : j.at("evidence_ids")) {
            if (ev.is_string()) it.evidence_ids.push_back(ev.get<std::string>());
        }
    }
    if (j.contains("checkpoint")) it.checkpoint = GoalCheckpoint::from_json(j.at("checkpoint"));
    return it;
}

nlohmann::json GoalCounters::to_json() const {
    nlohmann::json j;
    j["iterations_started"] = iterations_started;
    j["no_progress_streak"] = no_progress_streak;
    j["same_blocker_streak"] = same_blocker_streak;
    j["last_blocker_key"] = last_blocker_key;
    j["last_progress_fingerprint"] = last_progress_fingerprint;
    j["consecutive_provider_failures"] = consecutive_provider_failures;
    return j;
}

GoalCounters GoalCounters::from_json(const nlohmann::json& j) {
    GoalCounters c;
    auto read_int = [&j](const char* key, int& out) {
        if (j.contains(key) && j.at(key).is_number_integer()) out = j.at(key).get<int>();
    };
    read_int("iterations_started", c.iterations_started);
    read_int("no_progress_streak", c.no_progress_streak);
    read_int("same_blocker_streak", c.same_blocker_streak);
    read_int("consecutive_provider_failures", c.consecutive_provider_failures);
    if (j.contains("last_blocker_key") && j.at("last_blocker_key").is_string()) {
        c.last_blocker_key = j.at("last_blocker_key").get<std::string>();
    }
    if (j.contains("last_progress_fingerprint") && j.at("last_progress_fingerprint").is_string()) {
        c.last_progress_fingerprint = j.at("last_progress_fingerprint").get<std::string>();
    }
    return c;
}

nlohmann::json GoalTask::to_json() const {
    nlohmann::json j;
    j["id"] = id;
    j["revision"] = revision;
    j["objective"] = objective;
    j["objective_sha256"] = objective_sha256;
    j["contract"] = contract.to_json();
    j["contract_frozen"] = contract_frozen;
    j["contract_sha256"] = contract_sha256;
    j["state"] = ToString(state);
    j["budget"] = budget.to_json();
    j["counters"] = counters.to_json();
    j["checkpoint"] = checkpoint.to_json();
    if (last_evaluation.has_value()) j["last_evaluation"] = last_evaluation->to_json();
    j["workspace_root"] = workspace_root;
    j["workspace_identity"] = workspace_identity;
    j["created_at_ms"] = created_at_ms;
    j["updated_at_ms"] = updated_at_ms;
    if (started_at_ms.has_value()) j["started_at_ms"] = *started_at_ms;
    if (terminal_at_ms.has_value()) j["terminal_at_ms"] = *terminal_at_ms;
    j["usage"] = usage.to_json();
    return j;
}

GoalTask GoalTask::from_json(const nlohmann::json& j) {
    GoalTask t;
    auto read_str = [&j](const char* key, std::string& out) {
        if (j.contains(key) && j.at(key).is_string()) out = j.at(key).get<std::string>();
    };
    read_str("id", t.id);
    if (j.contains("revision") && j.at("revision").is_number_integer()) t.revision = j.at("revision").get<int>();
    read_str("objective", t.objective);
    read_str("objective_sha256", t.objective_sha256);
    if (j.contains("contract")) t.contract = GoalContract::from_json(j.at("contract"));
    if (j.contains("contract_frozen") && j.at("contract_frozen").is_boolean()) {
        t.contract_frozen = j.at("contract_frozen").get<bool>();
    }
    read_str("contract_sha256", t.contract_sha256);
    std::string state;
    read_str("state", state);
    if (!state.empty()) ParseGoalState(state, t.state);
    if (j.contains("budget")) t.budget = GoalBudget::from_json(j.at("budget"));
    if (j.contains("counters")) t.counters = GoalCounters::from_json(j.at("counters"));
    if (j.contains("checkpoint")) t.checkpoint = GoalCheckpoint::from_json(j.at("checkpoint"));
    if (j.contains("last_evaluation")) t.last_evaluation = GoalEvaluation::from_json(j.at("last_evaluation"));
    read_str("workspace_root", t.workspace_root);
    read_str("workspace_identity", t.workspace_identity);
    if (j.contains("created_at_ms") && j.at("created_at_ms").is_number()) {
        t.created_at_ms = j.at("created_at_ms").get<std::int64_t>();
    }
    if (j.contains("updated_at_ms") && j.at("updated_at_ms").is_number()) {
        t.updated_at_ms = j.at("updated_at_ms").get<std::int64_t>();
    }
    if (j.contains("started_at_ms") && j.at("started_at_ms").is_number()) {
        t.started_at_ms = j.at("started_at_ms").get<std::int64_t>();
    }
    if (j.contains("terminal_at_ms") && j.at("terminal_at_ms").is_number()) {
        t.terminal_at_ms = j.at("terminal_at_ms").get<std::int64_t>();
    }
    if (j.contains("usage")) t.usage = GoalUsage::from_json(j.at("usage"));
    return t;
}

// ---------------------------------------------------------------------------
// 转换表
// ---------------------------------------------------------------------------

std::vector<GoalState> AllowedTransitions(GoalState from) {
    switch (from) {
        case GoalState::Preparing:
            return {GoalState::Active, GoalState::Paused, GoalState::AwaitingUser,
                    GoalState::Cleared, GoalState::Failed, GoalState::SuspendedByPolicy};
        case GoalState::Active:
            return {GoalState::Running, GoalState::Paused, GoalState::Cleared,
                    GoalState::Failed, GoalState::SuspendedByPolicy};
        case GoalState::Running:
            return {GoalState::Evaluating, GoalState::Pausing,
                    GoalState::Paused, GoalState::AwaitingApproval, GoalState::AwaitingUser,
                    GoalState::Cleared, GoalState::Failed};
        case GoalState::Evaluating:
            return {GoalState::Active, GoalState::Blocked, GoalState::AwaitingUser,
                    GoalState::Achieved, GoalState::BudgetExhausted, GoalState::Pausing,
                    GoalState::Paused, GoalState::Cleared, GoalState::Failed};
        case GoalState::Pausing:
            return {GoalState::Paused, GoalState::Evaluating, GoalState::Cleared, GoalState::Failed};
        case GoalState::Paused:
            return {GoalState::Active, GoalState::Cleared, GoalState::Failed,
                    GoalState::SuspendedByPolicy};
        case GoalState::AwaitingApproval:
            // 审批答了回 Running(继续这枚 iteration);pause/clear 收悬问。
            return {GoalState::Running, GoalState::Paused, GoalState::Cleared, GoalState::Failed};
        case GoalState::AwaitingUser:
            return {GoalState::Active, GoalState::Running, GoalState::Blocked,
                    GoalState::Paused, GoalState::Cleared, GoalState::Failed,
                    GoalState::SuspendedByPolicy};
        case GoalState::Blocked:
            return {GoalState::Active, GoalState::Paused, GoalState::Cleared,
                    GoalState::Failed, GoalState::SuspendedByPolicy};
        // terminal:一律不可再动(迟到事件只留 evidence)。resume 不进
        // terminal;Achieved 后要再跑,新建目标。
        case GoalState::Achieved:
        case GoalState::BudgetExhausted:
        case GoalState::SuspendedByPolicy:
        case GoalState::Failed:
        case GoalState::Cleared:
            return {};
    }
    return {};
}

bool IsValidTransition(GoalState from, GoalState to) {
    const auto allowed = AllowedTransitions(from);
    return std::find(allowed.begin(), allowed.end(), to) != allowed.end();
}

PauseOutcome PauseTransition(GoalState from) {
    PauseOutcome out;
    switch (from) {
        case GoalState::Preparing:
        case GoalState::Active:
        case GoalState::AwaitingApproval:
        case GoalState::AwaitingUser:
            out.target = GoalState::Paused;
            out.immediate = true;
            out.allowed = true;
            return out;
        case GoalState::Running:
        case GoalState::Evaluating:
            // 记 pause_requested,下一个 tool/step 安全边界收口。Evaluating
            // 首版选"等回但不续"(单子 pause 节)。
            out.target = GoalState::Pausing;
            out.immediate = false;
            out.allowed = true;
            return out;
        case GoalState::Pausing:
            out.target = GoalState::Pausing;
            out.immediate = false;
            out.allowed = true;  // 幂等:重复 pause 返回当前状态
            return out;
        case GoalState::Paused:
            out.target = GoalState::Paused;
            out.immediate = true;
            out.allowed = true;  // 幂等
            return out;
        default:
            // terminal 不动(报 kErrGoalTerminal)。
            out.allowed = false;
            out.target = from;
            out.immediate = true;
            return out;
    }
}

}  // namespace lubancode::runtime::goal
