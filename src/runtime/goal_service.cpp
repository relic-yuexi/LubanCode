// GoalService 实现(§4.67 G0):快照落盘、state.goal.applied 落账、CAS、
// 只读投影。IO 只有两处:不可变快照文件(先临时再改名,不覆盖)与
// V3Writer::AppendEvent(账侧单写者);其余纯函数。
//
// 提交次序钉 §4.55:校验 -> 快照落稳 -> 提交口再核对 -> applied 落稳 ->
// 发布内存。任何一步失败都不发布;applied 未落时快照只是候选文件,
// 不生效也不删除(§4.67.6"候选文件落稳但 applied 未落时不生效")。

#include "runtime/goal_service.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <map>
#include <utility>

#include "hooks/hash.hpp"
#include "platform/json_safe.hpp"
#include "trajectory/v3/reader.hpp"
#include "trajectory/v3/writer.hpp"

namespace lubancode::runtime::goal {

namespace {

using trajectory::v3::Durability;
using trajectory::v3::EventDraft;
using trajectory::v3::EventKindV3;

// ---------------------------------------------------------------------------
// 复用类型的 camelCase 折算(v3 域键风格;不动 goal_types 的 v1 序列化)
// ---------------------------------------------------------------------------

nlohmann::json ContractToJson(const GoalContract& c) {
    nlohmann::json j = nlohmann::json::object();
    j["objective"] = c.objective;
    j["inScope"] = c.in_scope;
    j["outOfScope"] = c.out_of_scope;
    nlohmann::json criteria = nlohmann::json::array();
    for (const auto& item : c.criteria) {
        criteria.push_back(nlohmann::json{{"id", item.id}, {"text", item.text},
                                          {"required", item.required}});
    }
    j["criteria"] = std::move(criteria);
    j["validationCommands"] = c.validation_commands;
    j["requiredArtifacts"] = c.required_artifacts;
    j["constraints"] = c.constraints;
    j["checkpoints"] = c.checkpoints;
    j["pauseConditions"] = c.pause_conditions;
    return j;
}

GoalContract ContractFromJson(const nlohmann::json& j) {
    GoalContract c;
    if (j.contains("objective") && j["objective"].is_string()) {
        c.objective = j["objective"].get<std::string>();
    }
    auto read_strings = [&j](const char* key, std::vector<std::string>& out) {
        if (!j.contains(key) || !j[key].is_array()) return;
        out.clear();
        for (const auto& item : j[key]) {
            if (item.is_string()) out.push_back(item.get<std::string>());
        }
    };
    read_strings("inScope", c.in_scope);
    read_strings("outOfScope", c.out_of_scope);
    if (j.contains("criteria") && j["criteria"].is_array()) {
        for (const auto& item : j["criteria"]) {
            if (!item.is_object()) continue;
            GoalCriterion criterion;
            if (item.contains("id") && item["id"].is_string()) {
                criterion.id = item["id"].get<std::string>();
            }
            if (item.contains("text") && item["text"].is_string()) {
                criterion.text = item["text"].get<std::string>();
            }
            if (item.contains("required") && item["required"].is_boolean()) {
                criterion.required = item["required"].get<bool>();
            }
            c.criteria.push_back(std::move(criterion));
        }
    }
    read_strings("validationCommands", c.validation_commands);
    read_strings("requiredArtifacts", c.required_artifacts);
    read_strings("constraints", c.constraints);
    read_strings("checkpoints", c.checkpoints);
    read_strings("pauseConditions", c.pause_conditions);
    return c;
}

nlohmann::json BudgetToJson(const GoalBudget& b) {
    nlohmann::json j = nlohmann::json::object();
    if (b.max_total_tokens.has_value()) j["maxTotalTokens"] = *b.max_total_tokens;
    if (b.max_elapsed_ms.has_value()) j["maxElapsedMs"] = *b.max_elapsed_ms;
    if (b.max_iterations.has_value()) j["maxIterations"] = *b.max_iterations;
    if (b.max_cost_micros.has_value()) j["maxCostMicros"] = *b.max_cost_micros;
    j["maxNoProgressIterations"] = b.max_no_progress_iterations;
    j["maxSameBlockerIterations"] = b.max_same_blocker_iterations;
    j["maxConsecutiveProviderFailures"] = b.max_consecutive_provider_failures;
    return j;
}

GoalBudget BudgetFromJson(const nlohmann::json& j) {
    GoalBudget b;
    auto read_i64 = [&j](const char* key, std::optional<std::int64_t>& out) {
        if (j.contains(key) && j[key].is_number_integer()) {
            out = j[key].get<std::int64_t>();
        }
    };
    read_i64("maxTotalTokens", b.max_total_tokens);
    read_i64("maxElapsedMs", b.max_elapsed_ms);
    if (j.contains("maxIterations") && j["maxIterations"].is_number_integer()) {
        b.max_iterations = j["maxIterations"].get<int>();
    }
    read_i64("maxCostMicros", b.max_cost_micros);
    if (j.contains("maxNoProgressIterations") && j["maxNoProgressIterations"].is_number_integer()) {
        b.max_no_progress_iterations = j["maxNoProgressIterations"].get<int>();
    }
    if (j.contains("maxSameBlockerIterations") && j["maxSameBlockerIterations"].is_number_integer()) {
        b.max_same_blocker_iterations = j["maxSameBlockerIterations"].get<int>();
    }
    if (j.contains("maxConsecutiveProviderFailures") &&
        j["maxConsecutiveProviderFailures"].is_number_integer()) {
        b.max_consecutive_provider_failures = j["maxConsecutiveProviderFailures"].get<int>();
    }
    return b;
}

nlohmann::json UsageToJson(const GoalUsage& u) {
    nlohmann::json j = nlohmann::json::object();
    j["inputTokens"] = u.input_tokens;
    j["outputTokens"] = u.output_tokens;
    j["cacheReadTokens"] = u.cache_read_tokens;
    j["cacheCreationTokens"] = u.cache_creation_tokens;
    j["reasoningTokens"] = u.reasoning_tokens;
    j["requestCount"] = u.request_count;
    j["durationMs"] = u.duration_ms;
    j["usageReported"] = u.usage_reported;
    return j;
}

GoalUsage UsageFromJson(const nlohmann::json& j) {
    GoalUsage u;
    auto read_i64 = [&j](const char* key, std::int64_t& out) {
        if (j.contains(key) && j[key].is_number_integer()) out = j[key].get<std::int64_t>();
    };
    read_i64("inputTokens", u.input_tokens);
    read_i64("outputTokens", u.output_tokens);
    read_i64("cacheReadTokens", u.cache_read_tokens);
    read_i64("cacheCreationTokens", u.cache_creation_tokens);
    read_i64("reasoningTokens", u.reasoning_tokens);
    read_i64("requestCount", u.request_count);
    read_i64("durationMs", u.duration_ms);
    if (j.contains("usageReported") && j["usageReported"].is_boolean()) {
        u.usage_reported = j["usageReported"].get<bool>();
    }
    return u;
}

nlohmann::json CountersToJson(const GoalCounters& c) {
    nlohmann::json j = nlohmann::json::object();
    j["iterationsStarted"] = c.iterations_started;
    j["noProgressStreak"] = c.no_progress_streak;
    j["sameBlockerStreak"] = c.same_blocker_streak;
    j["consecutiveProviderFailures"] = c.consecutive_provider_failures;
    j["lastBlockerKey"] = c.last_blocker_key;
    j["lastProgressFingerprint"] = c.last_progress_fingerprint;
    return j;
}

GoalCounters CountersFromJson(const nlohmann::json& j) {
    GoalCounters c;
    auto read_int = [&j](const char* key, int& out) {
        if (j.contains(key) && j[key].is_number_integer()) out = j[key].get<int>();
    };
    auto read_str = [&j](const char* key, std::string& out) {
        if (j.contains(key) && j[key].is_string()) out = j[key].get<std::string>();
    };
    read_int("iterationsStarted", c.iterations_started);
    read_int("noProgressStreak", c.no_progress_streak);
    read_int("sameBlockerStreak", c.same_blocker_streak);
    read_int("consecutiveProviderFailures", c.consecutive_provider_failures);
    read_str("lastBlockerKey", c.last_blocker_key);
    read_str("lastProgressFingerprint", c.last_progress_fingerprint);
    return c;
}

// 不可变快照落盘(次序同结果仓 §4.16):临时文件 -> 落稳 -> 改不可变名;
// 不可变名已存在即冲突,不覆盖(同 revision 二次提交是状态回卷,CAS 拦,
// 真撞说明账面乱,报错不修)。
bool WriteSnapshotFile(const std::filesystem::path& final_path, const std::string& data,
                       std::string* error) {
    std::error_code ec;
    // 父目录:不在就建(同 AtomicWriteFile 合同)。快照树 state/goals/<id>/
    // 归本服务管,调用方不替它铺目录;纯文件名无父段则不建。
    const std::filesystem::path parent = final_path.parent_path();
    if (!parent.empty()) {
        std::filesystem::create_directories(parent, ec);
        if (ec) {
            *error = "goal 快照目录建不成: " + parent.string() + ": " + ec.message();
            return false;
        }
    }
    if (std::filesystem::exists(final_path, ec)) {
        *error = "goal 快照不可变名已存在(不覆盖): " + final_path.string();
        return false;
    }
    std::filesystem::path temp = final_path;
    temp += ".tmp";
    {
        std::ofstream file(temp, std::ios::binary | std::ios::trunc);
        if (!file.is_open()) {
            *error = "goal 快照开不了临时文件: " + temp.string();
            return false;
        }
        file.write(data.data(), static_cast<std::streamsize>(data.size()));
        file.flush();
        if (!file.good()) {
            *error = "goal 快照写临时文件失败: " + temp.string();
            return false;
        }
    }
    std::filesystem::rename(temp, final_path, ec);
    if (ec) {
        std::filesystem::remove(temp, ec);
        *error = "goal 快照发布不可变名失败: " + final_path.string();
        return false;
    }
    return true;
}

std::optional<std::string> ReadFileBytes(const std::filesystem::path& path, std::string* error) {
    std::ifstream file(path, std::ios::binary);
    if (!file.is_open()) {
        *error = "快照文件打不开: " + path.string();
        return std::nullopt;
    }
    return std::string((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
}

std::int64_t DefaultClock() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

}  // namespace

// ---------------------------------------------------------------------------
// lifecycle / phase 枚举与转换表
// ---------------------------------------------------------------------------

std::string ToString(GoalLifecycle lifecycle) {
    switch (lifecycle) {
        case GoalLifecycle::Preparing: return "preparing";
        case GoalLifecycle::Active: return "active";
        case GoalLifecycle::Waiting: return "waiting";
        case GoalLifecycle::Paused: return "paused";
        case GoalLifecycle::AwaitingUser: return "awaiting_user";
        case GoalLifecycle::Blocked: return "blocked";
        case GoalLifecycle::BudgetExhausted: return "budget_exhausted";
        case GoalLifecycle::SuspendedByPolicy: return "suspended_by_policy";
        case GoalLifecycle::Achieved: return "achieved";
        case GoalLifecycle::Cleared: return "cleared";
        case GoalLifecycle::Failed: return "failed";
    }
    return "unknown";
}

bool ParseGoalLifecycle(const std::string& s, GoalLifecycle& out) {
    static const std::map<std::string, GoalLifecycle, std::less<>> kMap = {
        {"preparing", GoalLifecycle::Preparing},
        {"active", GoalLifecycle::Active},
        {"waiting", GoalLifecycle::Waiting},
        {"paused", GoalLifecycle::Paused},
        {"awaiting_user", GoalLifecycle::AwaitingUser},
        {"blocked", GoalLifecycle::Blocked},
        {"budget_exhausted", GoalLifecycle::BudgetExhausted},
        {"suspended_by_policy", GoalLifecycle::SuspendedByPolicy},
        {"achieved", GoalLifecycle::Achieved},
        {"cleared", GoalLifecycle::Cleared},
        {"failed", GoalLifecycle::Failed},
    };
    const auto it = kMap.find(s);
    if (it == kMap.end()) return false;
    out = it->second;
    return true;
}

bool IsLifecycleTerminal(GoalLifecycle lifecycle) {
    // §4.67.3 末行:achieved/cleared/failed 不自动复活。budget_exhausted/
    // suspended_by_policy/blocked 可经显式路径恢复或处置,不算终态。
    switch (lifecycle) {
        case GoalLifecycle::Achieved:
        case GoalLifecycle::Cleared:
        case GoalLifecycle::Failed:
            return true;
        default:
            return false;
    }
}

std::string ToString(GoalPhase phase) {
    switch (phase) {
        case GoalPhase::Idle: return "idle";
        case GoalPhase::Queued: return "queued";
        case GoalPhase::Running: return "running";
        case GoalPhase::Evaluating: return "evaluating";
    }
    return "unknown";
}

bool ParseGoalPhase(const std::string& s, GoalPhase& out) {
    static const std::map<std::string, GoalPhase, std::less<>> kMap = {
        {"idle", GoalPhase::Idle},
        {"queued", GoalPhase::Queued},
        {"running", GoalPhase::Running},
        {"evaluating", GoalPhase::Evaluating},
    };
    const auto it = kMap.find(s);
    if (it == kMap.end()) return false;
    out = it->second;
    return true;
}

std::vector<GoalLifecycle> AllowedLifecycleTransitions(GoalLifecycle from) {
    using L = GoalLifecycle;
    switch (from) {
        case L::Preparing:
            return {L::Active, L::AwaitingUser, L::Cleared, L::Failed, L::SuspendedByPolicy};
        case L::Active:
            return {L::Preparing, L::Waiting, L::Paused, L::AwaitingUser, L::Blocked,
                    L::BudgetExhausted, L::SuspendedByPolicy, L::Achieved, L::Cleared, L::Failed};
        case L::Waiting:
            return {L::Preparing, L::Active, L::Paused, L::AwaitingUser, L::Blocked,
                    L::BudgetExhausted, L::SuspendedByPolicy, L::Cleared, L::Failed};
        case L::Paused:
            return {L::Preparing, L::Active, L::SuspendedByPolicy, L::Cleared, L::Failed};
        case L::AwaitingUser:
            return {L::Preparing, L::Active, L::Paused, L::Blocked, L::SuspendedByPolicy,
                    L::Cleared, L::Failed};
        case L::Blocked:
            return {L::Preparing, L::Active, L::Paused, L::SuspendedByPolicy, L::Cleared,
                    L::Failed};
        case L::BudgetExhausted:
            return {L::Active, L::SuspendedByPolicy, L::Cleared, L::Failed};
        case L::SuspendedByPolicy:
            return {L::Cleared};
        case L::Achieved:
        case L::Cleared:
        case L::Failed:
            return {};
    }
    return {};
}

bool IsValidLifecycleTransition(GoalLifecycle from, GoalLifecycle to) {
    if (from == to) return false;  // 状态提交必有变化;幂等由 CAS 层吸收
    const auto allowed = AllowedLifecycleTransitions(from);
    return std::find(allowed.begin(), allowed.end(), to) != allowed.end();
}

// ---------------------------------------------------------------------------
// 证据引用
// ---------------------------------------------------------------------------

std::string ToString(GoalEvidenceSource source) {
    switch (source) {
        case GoalEvidenceSource::ToolAction: return "tool_action";
        case GoalEvidenceSource::Message: return "message";
        case GoalEvidenceSource::Artifact: return "artifact";
    }
    return "unknown";
}

bool ParseGoalEvidenceSource(const std::string& s, GoalEvidenceSource& out) {
    static const std::map<std::string, GoalEvidenceSource, std::less<>> kMap = {
        {"tool_action", GoalEvidenceSource::ToolAction},
        {"message", GoalEvidenceSource::Message},
        {"artifact", GoalEvidenceSource::Artifact},
    };
    const auto it = kMap.find(s);
    if (it == kMap.end()) return false;
    out = it->second;
    return true;
}

nlohmann::json GoalEvidenceRef::ToJson() const {
    nlohmann::json j = nlohmann::json::object();
    j["evidenceId"] = id;
    j["kind"] = kind;
    j["source"] = ToString(source);
    if (source == GoalEvidenceSource::Artifact) {
        j["artifactRef"] = artifact_ref;
        j["sourceRef"] = "";
    } else {
        j["sourceRef"] = source_ref;
    }
    j["sessionId"] = session_id;
    j["runId"] = run_id;
    j["contentSha256"] = content_sha256;
    j["observedAtMs"] = observed_at_ms;
    j["workspaceBaseline"] = workspace_baseline;
    j["criterionId"] = criterion_id;
    j["fresh"] = fresh;
    j["truncated"] = truncated;
    return j;
}

std::optional<GoalEvidenceRef> GoalEvidenceRef::FromJson(const nlohmann::json& j,
                                                        std::string* error) {
    if (const std::string invalid = ValidateEvidenceRef(j); !invalid.empty()) {
        if (error != nullptr) *error = invalid;
        return std::nullopt;
    }
    GoalEvidenceRef ref;
    ref.id = j.at("evidenceId").get<std::string>();
    ref.kind = j.at("kind").get<std::string>();
    GoalEvidenceSource source = GoalEvidenceSource::ToolAction;
    ParseGoalEvidenceSource(j.at("source").get<std::string>(), source);
    ref.source = source;
    if (source == GoalEvidenceSource::Artifact) {
        ref.artifact_ref = j.at("artifactRef");
    } else {
        ref.source_ref = j.at("sourceRef").get<std::string>();
    }
    ref.session_id = j.at("sessionId").get<std::string>();
    ref.run_id = j.at("runId").get<std::string>();
    ref.content_sha256 = j.at("contentSha256").get<std::string>();
    ref.observed_at_ms = j.at("observedAtMs").get<std::int64_t>();
    if (j.contains("workspaceBaseline") && j["workspaceBaseline"].is_string()) {
        ref.workspace_baseline = j["workspaceBaseline"].get<std::string>();
    }
    if (j.contains("criterionId") && j["criterionId"].is_string()) {
        ref.criterion_id = j["criterionId"].get<std::string>();
    }
    if (j.contains("fresh") && j["fresh"].is_boolean()) ref.fresh = j["fresh"].get<bool>();
    if (j.contains("truncated") && j["truncated"].is_boolean()) {
        ref.truncated = j["truncated"].get<bool>();
    }
    return ref;
}

std::string ValidateEvidenceRef(const nlohmann::json& j) {
    const auto fail = [](const std::string& text) { return text; };
    if (!j.is_object()) return fail("evidenceRef 须为 object");
    for (const char* key : {"evidenceId", "kind", "source", "sessionId", "runId",
                            "contentSha256", "observedAtMs"}) {
        if (!j.contains(key)) return fail(std::string("evidenceRef 缺字段: ") + key);
    }
    if (!j.at("evidenceId").is_string() || j.at("evidenceId").get<std::string>().empty()) {
        return fail("evidenceId 须为非空 string");
    }
    if (!j.at("kind").is_string()) return fail("kind 须为 string");
    if (!j.at("sessionId").is_string() || j.at("sessionId").get<std::string>().empty()) {
        return fail("sessionId 须为非空 string");
    }
    if (!j.at("runId").is_string() || j.at("runId").get<std::string>().empty()) {
        return fail("runId 须为非空 string");
    }
    if (!j.at("contentSha256").is_string() ||
        !trajectory::v3::IsHex64(j.at("contentSha256").get<std::string>())) {
        return fail("contentSha256 须为 64 位十六进制");
    }
    if (!j.at("observedAtMs").is_number_integer() || j.at("observedAtMs").get<std::int64_t>() < 0) {
        return fail("observedAtMs 须为非负整数");
    }
    if (!j.at("source").is_string()) return fail("source 须为 string");
    GoalEvidenceSource source = GoalEvidenceSource::ToolAction;
    if (!ParseGoalEvidenceSource(j.at("source").get<std::string>(), source)) {
        return fail("source 枚举不认得");
    }
    if (source == GoalEvidenceSource::Artifact) {
        if (!j.contains("artifactRef")) return fail("source=artifact 缺 artifactRef");
        if (auto error = trajectory::v3::ValidateArtifactRef("evidenceRef", j.at("artifactRef"))) {
            return fail("artifactRef 六键不合: " + error->message);
        }
    } else {
        if (!j.contains("sourceRef") || !j.at("sourceRef").is_string() ||
            j.at("sourceRef").get<std::string>().empty()) {
            return fail("source=tool_action/message 缺非空 sourceRef");
        }
    }
    return std::string();
}

// ---------------------------------------------------------------------------
// 快照 schema
// ---------------------------------------------------------------------------

nlohmann::json GoalStateSnapshot::ToJson() const {
    nlohmann::json j = nlohmann::json::object();
    j["snapshotVersion"] = 1;
    j["goalId"] = goal_id;
    if (!parent_goal_id.empty()) j["parentGoalId"] = parent_goal_id;
    j["sessionId"] = session_id;
    j["runId"] = run_id;
    j["stateRevision"] = state_revision;
    j["contractRevision"] = contract_revision;
    j["objective"] = objective;
    j["objectiveSha256"] = objective_sha256;
    j["contract"] = ContractToJson(contract);
    j["contractFrozen"] = contract_frozen;
    j["lifecycle"] = ToString(lifecycle);
    j["phase"] = ToString(phase);
    j["stopReason"] = stop_reason;
    j["blockerKey"] = blocker_key;
    j["pendingQuestion"] = pending_question;
    j["iterationId"] = iteration_id.has_value() ? nlohmann::json(*iteration_id) : nlohmann::json(nullptr);
    j["checkpointRef"] = checkpoint_ref.has_value() ? nlohmann::json(*checkpoint_ref)
                                                    : nlohmann::json(nullptr);
    nlohmann::json evidence = nlohmann::json::array();
    for (const auto& ref : evidence_refs) evidence.push_back(ref.ToJson());
    j["evidenceRefs"] = std::move(evidence);
    j["waitTaskRefs"] = wait_task_refs;
    j["appliedEvaluationId"] = applied_evaluation_id.has_value()
                                   ? nlohmann::json(*applied_evaluation_id)
                                   : nlohmann::json(nullptr);
    j["budget"] = BudgetToJson(budget);
    j["usage"] = UsageToJson(usage);
    j["counters"] = CountersToJson(counters);
    j["pendingIntent"] = pending_intent;
    j["workspaceRoot"] = workspace_root;
    j["workspaceIdentity"] = workspace_identity;
    j["createdAtMs"] = created_at_ms;
    j["updatedAtMs"] = updated_at_ms;
    return j;
}

std::optional<GoalStateSnapshot> GoalStateSnapshot::FromJson(const nlohmann::json& j,
                                                             std::string* error) {
    const auto fail = [error](const std::string& text) {
        if (error != nullptr) *error = text;
        return std::optional<GoalStateSnapshot>{};
    };
    if (!j.is_object()) return fail("goal 快照须为 object");
    if (!j.contains("snapshotVersion") || !j.at("snapshotVersion").is_number_integer() ||
        j.at("snapshotVersion").get<int>() != 1) {
        return fail("goal 快照 snapshotVersion 须为 1");
    }
    for (const char* key : {"goalId", "sessionId", "runId", "stateRevision", "contractRevision",
                            "objective", "lifecycle", "phase"}) {
        if (!j.contains(key)) return fail(std::string("goal 快照缺字段: ") + key);
    }
    GoalStateSnapshot s;
    if (!j.at("goalId").is_string() || j.at("goalId").get<std::string>().empty()) {
        return fail("goalId 须为非空 string");
    }
    s.goal_id = j.at("goalId").get<std::string>();
    if (j.contains("parentGoalId") && j["parentGoalId"].is_string()) {
        s.parent_goal_id = j["parentGoalId"].get<std::string>();
    }
    if (!j.at("sessionId").is_string() || j.at("sessionId").get<std::string>().empty()) {
        return fail("sessionId 须为非空 string");
    }
    s.session_id = j.at("sessionId").get<std::string>();
    if (!j.at("runId").is_string() || j.at("runId").get<std::string>().empty()) {
        return fail("runId 须为非空 string");
    }
    s.run_id = j.at("runId").get<std::string>();
    if (!j.at("stateRevision").is_number_integer() ||
        j.at("stateRevision").get<std::int64_t>() < 1) {
        return fail("stateRevision 须为 >= 1 的整数");
    }
    s.state_revision = j.at("stateRevision").get<std::uint64_t>();
    if (!j.at("contractRevision").is_number_integer() ||
        j.at("contractRevision").get<std::int64_t>() < 1) {
        return fail("contractRevision 须为 >= 1 的整数");
    }
    s.contract_revision = j.at("contractRevision").get<std::uint64_t>();
    if (!j.at("objective").is_string() || j.at("objective").get<std::string>().empty()) {
        return fail("objective 须为非空 string");
    }
    s.objective = j.at("objective").get<std::string>();
    s.objective_sha256 = hooks::Sha256Hex(s.objective);
    if (j.contains("objectiveSha256") && j["objectiveSha256"].is_string()) {
        s.objective_sha256 = j["objectiveSha256"].get<std::string>();
    }
    if (!j.at("lifecycle").is_string() || !ParseGoalLifecycle(j.at("lifecycle").get<std::string>(), s.lifecycle)) {
        return fail("lifecycle 枚举不认得");
    }
    if (!j.at("phase").is_string() || !ParseGoalPhase(j.at("phase").get<std::string>(), s.phase)) {
        return fail("phase 枚举不认得");
    }
    if (j.contains("contract")) s.contract = ContractFromJson(j.at("contract"));
    if (j.contains("contractFrozen") && j["contractFrozen"].is_boolean()) {
        s.contract_frozen = j["contractFrozen"].get<bool>();
    }
    auto read_str = [&j](const char* key, std::string& out) {
        if (j.contains(key) && j[key].is_string()) out = j[key].get<std::string>();
    };
    read_str("stopReason", s.stop_reason);
    read_str("blockerKey", s.blocker_key);
    read_str("pendingQuestion", s.pending_question);
    auto read_opt_str = [&j](const char* key, std::optional<std::string>& out) {
        if (j.contains(key)) {
            if (j[key].is_string()) {
                out = j[key].get<std::string>();
            } else if (!j[key].is_null()) {
                out = std::nullopt;
            }
        }
    };
    read_opt_str("iterationId", s.iteration_id);
    read_opt_str("checkpointRef", s.checkpoint_ref);
    read_opt_str("appliedEvaluationId", s.applied_evaluation_id);
    if (j.contains("evidenceRefs") && j["evidenceRefs"].is_array()) {
        for (const auto& item : j["evidenceRefs"]) {
            std::string evidence_error;
            auto ref = GoalEvidenceRef::FromJson(item, &evidence_error);
            if (!ref.has_value()) return fail("evidenceRefs 坏项: " + evidence_error);
            s.evidence_refs.push_back(std::move(*ref));
        }
    }
    if (j.contains("waitTaskRefs") && j["waitTaskRefs"].is_array()) {
        for (const auto& item : j["waitTaskRefs"]) {
            if (item.is_string()) s.wait_task_refs.push_back(item.get<std::string>());
        }
    }
    if (j.contains("budget")) s.budget = BudgetFromJson(j.at("budget"));
    if (j.contains("usage")) s.usage = UsageFromJson(j.at("usage"));
    if (j.contains("counters")) s.counters = CountersFromJson(j.at("counters"));
    if (j.contains("pendingIntent") && j["pendingIntent"].is_object()) {
        s.pending_intent = j["pendingIntent"];
    }
    read_str("workspaceRoot", s.workspace_root);
    read_str("workspaceIdentity", s.workspace_identity);
    auto read_ms = [&j](const char* key, std::int64_t& out) {
        if (j.contains(key) && j[key].is_number_integer()) out = j[key].get<std::int64_t>();
    };
    read_ms("createdAtMs", s.created_at_ms);
    read_ms("updatedAtMs", s.updated_at_ms);
    // 停态约束(§4.67.3 各态保存的停因/问题/blocker):
    if (s.lifecycle == GoalLifecycle::Blocked && s.blocker_key.empty()) {
        return fail("lifecycle=blocked 须带 blockerKey");
    }
    if (s.lifecycle == GoalLifecycle::AwaitingUser && s.pending_question.empty()) {
        return fail("lifecycle=awaiting_user 须带 pendingQuestion");
    }
    if (IsLifecycleTerminal(s.lifecycle) || s.lifecycle == GoalLifecycle::Waiting ||
        s.lifecycle == GoalLifecycle::Paused || s.lifecycle == GoalLifecycle::AwaitingUser ||
        s.lifecycle == GoalLifecycle::Blocked ||
        s.lifecycle == GoalLifecycle::BudgetExhausted ||
        s.lifecycle == GoalLifecycle::SuspendedByPolicy) {
        if (s.stop_reason.empty()) {
            return fail("lifecycle=" + ToString(s.lifecycle) + " 须带 stopReason");
        }
        if (s.phase != GoalPhase::Idle) {
            return fail("停态下 phase 须为 idle");
        }
    }
    return s;
}

std::string SnapshotBytes(const GoalStateSnapshot& snapshot) {
    // 快照要被 resume/投影重新读:坏串窄边界(同 v1 落档行规矩),宁可
    // 替换字符洗过也不落解不开的文件;hash 对文件真实字节算,写读一致。
    return platform::DumpJsonSanitized(snapshot.ToJson());
}

std::string SnapshotRefPath(const std::string& goal_id, std::uint64_t state_revision) {
    char revision[32];
    std::snprintf(revision, sizeof(revision), "%06llu",
                  static_cast<unsigned long long>(state_revision));
    return "state/goals/" + goal_id + "/rev-" + revision + ".json";
}

// ---------------------------------------------------------------------------
// GoalService
// ---------------------------------------------------------------------------

GoalService::GoalService(trajectory::v3::V3Writer* writer, Options options)
    : writer_(writer), options_(std::move(options)) {}

GoalService::~GoalService() = default;

GoalServiceResult GoalService::Fail(const char* code, const std::string& message) {
    GoalServiceResult r;
    r.ok = false;
    r.error_code = code;
    r.error_message = message;
    return r;
}

std::int64_t GoalService::Now() const { return options_.clock ? options_.clock() : DefaultClock(); }

GoalServiceResult GoalService::CreateGoal(GoalStateSnapshot draft, nlohmann::json cause_ref) {
    if (broken_) {
        return Fail(kErrGoalStoreUnavailable, "goal 写口已锁(此前写盘失败);fail closed");
    }
    if (writer_ == nullptr) {
        return Fail(kErrGoalStoreUnavailable, "GoalService 未接 v3 writer(只读)");
    }
    if (options_.session_dir.empty()) {
        return Fail(kErrGoalStoreUnavailable, "GoalService 未配 session_dir");
    }
    if (current_.has_value() && !IsLifecycleTerminal(current_->lifecycle)) {
        return Fail(kErrGoalAlreadyActive, "已有一枚未收账 goal;用 edit/clear,不暗中替换");
    }
    if (const std::string invalid = ValidateGoalObjective(draft.objective); !invalid.empty()) {
        return Fail(invalid.c_str(), invalid == kErrGoalObjectiveEmpty
                                         ? "objective 不能为空"
                                         : "objective 超过 4000 字符上限");
    }
    if (draft.goal_id.empty()) {
        draft.goal_id = "goal-" + std::to_string(++next_goal_number_);
    }
    if (draft.session_id.empty()) draft.session_id = writer_->session_id();
    if (draft.run_id.empty()) draft.run_id = writer_->run_id();
    draft.state_revision = 1;
    draft.contract_revision = 1;
    draft.lifecycle = GoalLifecycle::Preparing;
    draft.phase = GoalPhase::Idle;
    draft.stop_reason.clear();  // preparing 不带停因(停态约束在 FromJson/Commit)
    draft.created_at_ms = Now();
    draft.updated_at_ms = draft.created_at_ms;
    return Commit(std::move(draft), std::move(cause_ref));
}

GoalServiceResult GoalService::ApplyTransition(const GoalTransitionCandidate& candidate) {
    if (broken_) {
        return Fail(kErrGoalStoreUnavailable, "goal 写口已锁(此前写盘失败);fail closed");
    }
    if (writer_ == nullptr || options_.session_dir.empty()) {
        return Fail(kErrGoalStoreUnavailable, "GoalService 写口未接线");
    }
    if (!current_.has_value()) {
        return Fail(kErrGoalNotFound, "没有 goal");
    }
    if (candidate.goal_id != current_->goal_id) {
        return Fail(kErrGoalNotFound, "goalId 对不上(候选不是本 goal 的)");
    }
    if (candidate.expected_state_revision == 0) {
        return Fail(kErrGoalRevisionConflict, "expectedStateRevision 必须显式给(CAS)");
    }
    if (candidate.expected_state_revision != current_->state_revision) {
        GoalServiceResult r = Fail(kErrGoalRevisionConflict, "stateRevision 冲突:状态已被改过");
        r.payload["currentStateRevision"] = current_->state_revision;
        return r;
    }
    if (candidate.expected_contract_revision != 0 &&
        candidate.expected_contract_revision != current_->contract_revision) {
        GoalServiceResult r = Fail(kErrGoalRevisionConflict, "contractRevision 冲突");
        r.payload["currentContractRevision"] = current_->contract_revision;
        return r;
    }
    if (IsLifecycleTerminal(current_->lifecycle)) {
        // terminal 后迟到候选:只留调用方审计,不改账(§4.67.6)。
        return Fail(kErrGoalTerminal, "目标已收账,迟到候选拒(不复活)");
    }
    if (!IsValidLifecycleTransition(current_->lifecycle, candidate.to_lifecycle)) {
        return Fail(kErrGoalInvalidTransition,
                    "lifecycle 转换非法: " + ToString(current_->lifecycle) + " -> " +
                        ToString(candidate.to_lifecycle));
    }
    if (candidate.to_lifecycle == GoalLifecycle::Blocked && candidate.blocker_key.empty()) {
        return Fail(kErrGoalCandidateInvalid, "blocked 候选缺 blockerKey");
    }
    if (candidate.to_lifecycle == GoalLifecycle::AwaitingUser &&
        candidate.pending_question.empty()) {
        return Fail(kErrGoalCandidateInvalid, "awaiting_user 候选缺 pendingQuestion");
    }
    const bool to_stopped = IsLifecycleTerminal(candidate.to_lifecycle) ||
                            candidate.to_lifecycle == GoalLifecycle::Waiting ||
                            candidate.to_lifecycle == GoalLifecycle::Paused ||
                            candidate.to_lifecycle == GoalLifecycle::AwaitingUser ||
                            candidate.to_lifecycle == GoalLifecycle::Blocked ||
                            candidate.to_lifecycle == GoalLifecycle::BudgetExhausted ||
                            candidate.to_lifecycle == GoalLifecycle::SuspendedByPolicy;
    if (to_stopped && candidate.stop_reason.empty()) {
        return Fail(kErrGoalCandidateInvalid,
                    "lifecycle=" + ToString(candidate.to_lifecycle) + " 候选缺 stopReason");
    }
    if (to_stopped && candidate.to_phase != GoalPhase::Idle) {
        return Fail(kErrGoalCandidateInvalid, "停态候选 phase 须为 idle");
    }

    GoalStateSnapshot next = *current_;
    next.state_revision += 1;
    next.lifecycle = candidate.to_lifecycle;
    next.phase = candidate.to_phase;
    next.stop_reason = candidate.stop_reason;
    next.blocker_key = candidate.blocker_key;
    next.pending_question = candidate.pending_question;
    if (candidate.to_lifecycle != GoalLifecycle::Blocked) next.blocker_key.clear();
    if (candidate.to_lifecycle != GoalLifecycle::AwaitingUser) next.pending_question.clear();
    if (candidate.iteration_id.has_value()) next.iteration_id = candidate.iteration_id;
    if (candidate.applied_evaluation_id.has_value()) {
        next.applied_evaluation_id = candidate.applied_evaluation_id;
    }
    for (const auto& ref : candidate.evidence_additions) {
        // 引用合同先验(坏候选不落盘)。
        if (const std::string invalid = ValidateEvidenceRef(ref.ToJson()); !invalid.empty()) {
            return Fail(kErrGoalCandidateInvalid, "evidence 候选坏项: " + invalid);
        }
        next.evidence_refs.push_back(ref);
    }
    next.usage.Add(candidate.usage_addition);
    next.updated_at_ms = Now();
    return Commit(std::move(next), candidate.cause_ref);
}

GoalServiceResult GoalService::AmendContract(const GoalContract& contract,
                                             std::uint64_t expected_state_revision,
                                             std::uint64_t expected_contract_revision,
                                             nlohmann::json cause_ref) {
    if (broken_) {
        return Fail(kErrGoalStoreUnavailable, "goal 写口已锁(此前写盘失败);fail closed");
    }
    if (writer_ == nullptr || options_.session_dir.empty()) {
        return Fail(kErrGoalStoreUnavailable, "GoalService 写口未接线");
    }
    if (!current_.has_value()) return Fail(kErrGoalNotFound, "没有 goal");
    if (IsLifecycleTerminal(current_->lifecycle)) {
        return Fail(kErrGoalTerminal, "目标已收账,合同不再受理");
    }
    if (expected_state_revision != current_->state_revision) {
        return Fail(kErrGoalRevisionConflict, "stateRevision 冲突");
    }
    if (expected_contract_revision != current_->contract_revision) {
        return Fail(kErrGoalRevisionConflict, "contractRevision 冲突");
    }
    if (!IsValidLifecycleTransition(current_->lifecycle, GoalLifecycle::Preparing)) {
        return Fail(kErrGoalInvalidTransition,
                    "当前 lifecycle(" + ToString(current_->lifecycle) + ")不受理合同改版");
    }
    if (contract.objective.empty()) {
        return Fail(kErrGoalCandidateInvalid, "改版合同缺 objective");
    }
    GoalStateSnapshot next = *current_;
    next.state_revision += 1;
    next.contract_revision += 1;
    next.contract = contract;
    next.objective = contract.objective;
    next.objective_sha256 = hooks::Sha256Hex(contract.objective);
    next.contract_frozen = false;  // 重拟;通过校验后回 active(G1 命令面)
    next.lifecycle = GoalLifecycle::Preparing;
    next.phase = GoalPhase::Idle;
    next.stop_reason = "contract_amended";
    next.blocker_key.clear();
    next.pending_question.clear();
    // §4.67.2 edit 行:相关旧证据重新判有效期——保守全翻 stale,由 G2 的
    // 证据有效期核验重判;计数器防空转重起。
    for (auto& ref : next.evidence_refs) ref.fresh = false;
    next.counters.no_progress_streak = 0;
    next.counters.same_blocker_streak = 0;
    next.counters.last_blocker_key.clear();
    next.updated_at_ms = Now();
    return Commit(std::move(next), std::move(cause_ref));
}

GoalServiceResult GoalService::Commit(GoalStateSnapshot next, const nlohmann::json& cause_ref) {
    // 停态/证据合同先整体验一次(FromJson 是同一份 schema 校验)。
    std::string schema_error;
    if (!GoalStateSnapshot::FromJson(next.ToJson(), &schema_error).has_value()) {
        return Fail(kErrGoalCandidateInvalid, "快照不合 schema: " + schema_error);
    }
    // 提交口再核对(§4.55):current 自上次校验后未变(CAS 已拦并发;这里
    // 挡编程错误导致的 revision 回卷)。新 goal(goalId 换了)从 1 起,不
    // 与内存里的 terminal 旧 goal 对账。
    if (current_.has_value() && current_->goal_id == next.goal_id &&
        next.state_revision != current_->state_revision + 1) {
        return Fail(kErrGoalRevisionConflict, "stateRevision 必须恰好 +1");
    }

    const std::string bytes = SnapshotBytes(next);
    const std::string hash = hooks::Sha256Hex(bytes);
    const std::filesystem::path final_path =
        options_.session_dir / SnapshotRefPath(next.goal_id, next.state_revision);
    std::string io_error;
    if (!WriteSnapshotFile(final_path, bytes, &io_error)) {
        broken_ = true;  // 写不下快照:锁内存执行门,报错不封账(§4.67.3)
        return Fail(kErrGoalStoreUnavailable, io_error);
    }

    EventDraft draft;
    draft.kind = EventKindV3::StateGoalApplied;
    draft.payload["goalId"] = next.goal_id;
    // 首条(新 goal)从 0 起;同 goal 续接用前版 revision。
    const bool same_goal = current_.has_value() && current_->goal_id == next.goal_id;
    draft.payload["fromStateRevision"] =
        same_goal ? nlohmann::json(current_->state_revision) : nlohmann::json(0);
    draft.payload["toStateRevision"] = next.state_revision;
    draft.payload["contractRevision"] = next.contract_revision;
    draft.payload["snapshotRef"] = SnapshotRefPath(next.goal_id, next.state_revision);
    draft.payload["snapshotSha256"] = hash;
    draft.payload["lifecycle"] = ToString(next.lifecycle);
    if (!cause_ref.is_null()) draft.payload["causeRef"] = cause_ref;
    const auto receipt = writer_->AppendEvent(std::move(draft), Durability::PowerLoss);
    if (receipt.status != trajectory::v3::WriteReceipt::Status::Committed) {
        // applied 未落:快照只是候选文件,不生效(不删,留审计);写口锁死。
        broken_ = true;
        return Fail(kErrGoalStoreUnavailable,
                    "state.goal.applied 落账失败(" + receipt.error_code + "): " +
                        receipt.error_message);
    }
    // 发布内存 + 账面锚。
    applied_event_id_ = receipt.id;
    applied_line_hash_ = receipt.line_hash;
    applied_seq_ = receipt.seq;
    // goal-<n> 发号抬底(显式 id 也认:恢复后接着发不撞号)。
    static const std::string kPrefix = "goal-";
    if (next.goal_id.size() > kPrefix.size() &&
        next.goal_id.compare(0, kPrefix.size(), kPrefix) == 0) {
        std::uint64_t number = 0;
        bool numeric = true;
        for (std::size_t i = kPrefix.size(); i < next.goal_id.size(); ++i) {
            if (next.goal_id[i] < '0' || next.goal_id[i] > '9') {
                numeric = false;
                break;
            }
            number = number * 10 + static_cast<std::uint64_t>(next.goal_id[i] - '0');
        }
        if (numeric && number > next_goal_number_) next_goal_number_ = number;
    }
    current_ = std::move(next);
    GoalServiceResult r;
    r.ok = true;
    r.payload["goalId"] = current_->goal_id;
    r.payload["stateRevision"] = current_->state_revision;
    r.payload["contractRevision"] = current_->contract_revision;
    r.payload["lifecycle"] = ToString(current_->lifecycle);
    r.payload["snapshotRef"] = SnapshotRefPath(current_->goal_id, current_->state_revision);
    r.payload["appliedEventId"] = applied_event_id_;
    return r;
}

GoalServiceResult GoalService::AdoptFromProjection(const GoalProjection& projection) {
    if (projection.gap != GoalProjectionGap::None) {
        GoalServiceResult r = Fail(kErrGoalProjectionGap, ToString(projection.gap) + ": " +
                                                              projection.gap_detail);
        r.payload["gap"] = ToString(projection.gap);
        return r;
    }
    if (!projection.has_goal) {
        return Fail(kErrGoalNotFound, "投影里没有 goal");
    }
    if (current_.has_value() && current_->goal_id == projection.goal_id &&
        current_->state_revision > projection.snapshot.state_revision) {
        // 内存比账新:写盘失败过的在途状态,不回退(§4.67.8 恢复以已提交
        // 版本为准,但已发布内存不倒改)。
        return Fail(kErrGoalRevisionConflict, "内存 stateRevision 比投影新,不回退");
    }
    current_ = projection.snapshot;
    applied_event_id_ = projection.applied_event_id;
    applied_line_hash_ = projection.applied_line_hash;
    applied_seq_ = projection.applied_seq;
    GoalServiceResult r;
    r.ok = true;
    r.payload["goalId"] = current_->goal_id;
    r.payload["stateRevision"] = current_->state_revision;
    return r;
}

// ---------------------------------------------------------------------------
// 只读投影
// ---------------------------------------------------------------------------

std::string ToString(GoalProjectionGap gap) {
    switch (gap) {
        case GoalProjectionGap::None: return "none";
        case GoalProjectionGap::NoGoal: return "no_goal";
        case GoalProjectionGap::SnapshotMissing: return "snapshot_missing";
        case GoalProjectionGap::SnapshotUnreadable: return "snapshot_unreadable";
        case GoalProjectionGap::HashMismatch: return "snapshot_hash_mismatch";
        case GoalProjectionGap::RevisionMismatch: return "snapshot_revision_mismatch";
        case GoalProjectionGap::IllegalTransition: return "illegal_transition";
    }
    return "unknown";
}

GoalProjection ProjectGoalState(const trajectory::v3::V3Ledger& ledger,
                                const std::filesystem::path& session_dir) {
    using trajectory::v3::EventKindV3;
    // 逐条 applied(落盘序):验载荷合同、revision 递增、goalId 单活动与
    // lifecycle 转换合法;head = 最后一条。
    struct AppliedView {
        std::string goal_id;
        std::uint64_t from_state_revision = 0;
        std::uint64_t to_state_revision = 0;
        std::uint64_t contract_revision = 0;
        std::string snapshot_ref;
        std::string snapshot_sha256;
        GoalLifecycle lifecycle = GoalLifecycle::Preparing;
        std::uint64_t seq = 0;
        std::string event_id;
        std::string line_hash;
    };
    std::vector<AppliedView> applied;
    GoalProjection out;
    std::string last_goal_id;
    GoalLifecycle last_lifecycle = GoalLifecycle::Preparing;
    bool have_last = false;
    for (const auto& event : ledger.events) {
        if (event.kind != EventKindV3::StateGoalApplied) continue;
        AppliedView view;
        const auto& p = event.payload;
        const auto bad = [&](const std::string& detail) {
            out.gap = GoalProjectionGap::IllegalTransition;
            out.gap_detail = "seq " + std::to_string(event.seq) + ": " + detail;
            return true;
        };
        if (!p.contains("goalId") || !p.at("goalId").is_string() ||
            p.at("goalId").get<std::string>().empty()) {
            bad("applied 缺 goalId");
            return out;
        }
        view.goal_id = p.at("goalId").get<std::string>();
        for (const char* key : {"fromStateRevision", "toStateRevision", "contractRevision"}) {
            if (!p.contains(key) || !p.at(key).is_number_integer()) {
                bad(std::string("applied 缺 ") + key);
                return out;
            }
        }
        // 先按 int64 收(负数在此露馅),再转 uint64 对账。
        const std::int64_t from_rev = p.at("fromStateRevision").get<std::int64_t>();
        const std::int64_t to_rev = p.at("toStateRevision").get<std::int64_t>();
        const std::int64_t contract_rev = p.at("contractRevision").get<std::int64_t>();
        if (from_rev < 0 || to_rev < 1 || contract_rev < 1) {
            bad("applied 的 revision 须非负(to/contract >= 1)");
            return out;
        }
        if (to_rev != from_rev + 1) {
            bad("toStateRevision 须 = fromStateRevision + 1");
            return out;
        }
        view.from_state_revision = static_cast<std::uint64_t>(from_rev);
        view.to_state_revision = static_cast<std::uint64_t>(to_rev);
        view.contract_revision = static_cast<std::uint64_t>(contract_rev);
        if (!p.contains("snapshotRef") || !p.at("snapshotRef").is_string() ||
            p.at("snapshotRef").get<std::string>().empty()) {
            bad("applied 缺 snapshotRef");
            return out;
        }
        view.snapshot_ref = p.at("snapshotRef").get<std::string>();
        if (!p.contains("snapshotSha256") || !p.at("snapshotSha256").is_string() ||
            !trajectory::v3::IsHex64(p.at("snapshotSha256").get<std::string>())) {
            bad("applied 缺合法 snapshotSha256");
            return out;
        }
        view.snapshot_sha256 = p.at("snapshotSha256").get<std::string>();
        if (!p.contains("lifecycle") || !p.at("lifecycle").is_string() ||
            !ParseGoalLifecycle(p.at("lifecycle").get<std::string>(), view.lifecycle)) {
            bad("applied 缺合法 lifecycle");
            return out;
        }
        view.seq = event.seq;
        view.event_id = event.event_id;
        view.line_hash = event.line_hash;

        if (have_last) {
            if (view.goal_id == last_goal_id) {
                // 同 goal:版本须衔接,转换须合法(terminal 复活在这里露馅)。
                if (!applied.empty() &&
                    view.from_state_revision != applied.back().to_state_revision) {
                    bad("同 goal 的 applied revision 不衔接");
                    return out;
                }
                if (!IsValidLifecycleTransition(last_lifecycle, view.lifecycle)) {
                    bad("lifecycle 转换非法: " + ToString(last_lifecycle) + " -> " +
                        ToString(view.lifecycle) + "(terminal 不复活)");
                    return out;
                }
            } else {
                // 换 goal:前一枚必须已收账(§4.67.3 一链一枚未收账)。
                if (!IsLifecycleTerminal(last_lifecycle)) {
                    bad("前一 goal(" + last_goal_id + ")未收账就开新 goal(" + view.goal_id + ")");
                    return out;
                }
                if (view.from_state_revision != 0) {
                    bad("新 goal 首条 fromStateRevision 应为 0");
                    return out;
                }
            }
        } else {
            if (view.from_state_revision != 0) {
                bad("首条 applied 的 fromStateRevision 应为 0");
                return out;
            }
        }
        last_goal_id = view.goal_id;
        last_lifecycle = view.lifecycle;
        have_last = true;
        applied.push_back(std::move(view));
    }
    if (applied.empty()) {
        out.gap = GoalProjectionGap::NoGoal;
        return out;
    }
    const AppliedView& head = applied.back();
    out.has_goal = true;
    out.goal_id = head.goal_id;
    out.applied_seq = head.seq;
    out.applied_event_id = head.event_id;
    out.applied_line_hash = head.line_hash;
    out.snapshot_ref = head.snapshot_ref;

    // 快照实探:存在 -> 读 -> 验 hash -> 验 revision(§4.55"状态损坏"。
    // 不用摘要猜,不静默重建)。session_dir 未给:按缺失报,不假装验过。
    if (session_dir.empty()) {
        out.gap = GoalProjectionGap::SnapshotMissing;
        out.gap_detail = "未给 session_dir,快照无从实探";
        return out;
    }
    const std::filesystem::path snapshot_path = session_dir / head.snapshot_ref;
    std::error_code ec;
    if (!std::filesystem::exists(snapshot_path, ec)) {
        out.gap = GoalProjectionGap::SnapshotMissing;
        out.gap_detail = head.snapshot_ref + " 不在";
        return out;
    }
    std::string io_error;
    const auto bytes = ReadFileBytes(snapshot_path, &io_error);
    if (!bytes.has_value()) {
        out.gap = GoalProjectionGap::SnapshotUnreadable;
        out.gap_detail = io_error;
        return out;
    }
    if (hooks::Sha256Hex(*bytes) != head.snapshot_sha256) {
        out.gap = GoalProjectionGap::HashMismatch;
        out.gap_detail = head.snapshot_ref + " 内容 hash 对不上 applied 所记";
        return out;
    }
    const nlohmann::json parsed = nlohmann::json::parse(*bytes, nullptr,
                                                        /*allow_exceptions=*/false);
    if (!parsed.is_object()) {
        out.gap = GoalProjectionGap::SnapshotUnreadable;
        out.gap_detail = head.snapshot_ref + " 不是合法 JSON object";
        return out;
    }
    std::string schema_error;
    auto snapshot = GoalStateSnapshot::FromJson(parsed, &schema_error);
    if (!snapshot.has_value()) {
        out.gap = GoalProjectionGap::SnapshotUnreadable;
        out.gap_detail = head.snapshot_ref + ": " + schema_error;
        return out;
    }
    if (snapshot->goal_id != head.goal_id || snapshot->state_revision != head.to_state_revision ||
        snapshot->contract_revision != head.contract_revision) {
        out.gap = GoalProjectionGap::RevisionMismatch;
        out.gap_detail = "快照 revision(" + std::to_string(snapshot->state_revision) + "/" +
                         std::to_string(snapshot->contract_revision) + ")与 applied(" +
                         std::to_string(head.to_state_revision) + "/" +
                         std::to_string(head.contract_revision) + ")不一致";
        return out;
    }
    out.snapshot = std::move(*snapshot);
    return out;
}

}  // namespace lubancode::runtime::goal
