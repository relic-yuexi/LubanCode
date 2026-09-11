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
#include "platform/paths.hpp"
#include "trajectory/directory.hpp"
#include "trajectory/v3/reader.hpp"
#include "trajectory/v3/session_switch.hpp"
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

std::optional<std::string> ReadFileBytes(const std::filesystem::path& path, std::string* error) {
    std::ifstream file(path, std::ios::binary);
    if (!file.is_open()) {
        *error = "快照文件打不开: " + path.string();
        return std::nullopt;
    }
    return std::string((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
}

// 不可变快照落盘(次序同结果仓 §4.16):临时文件 -> 落稳 -> 改不可变名。
// 不可变名已存在时按字节比:同字节 = 上次提交在"快照落稳、applied 未落"
// 之间崩了的同款重试(§4.55 崩溃窗口),复用这枚候选文件接着补 applied,
// 不算冲突;字节不同才是真撞(状态回卷 CAS 已拦,真撞说明账面乱,报错
// 不修)。
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
        std::string existing_error;
        const auto existing = ReadFileBytes(final_path, &existing_error);
        if (existing.has_value() && *existing == data) {
            return true;  // 幂等重试:候选文件已是这份内容,直接进 applied 步
        }
        *error = "goal 快照不可变名已存在且内容不同(不覆盖): " + final_path.string();
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
            // G1 补 preparing -> paused:§4.67.2 pause 行不设前置(刚立的目标
            // 也许要立刻停排;v1 PauseOutcome 同款 Preparing→Paused 立刻)。
            return {L::Active, L::Paused, L::AwaitingUser, L::Cleared, L::Failed,
                    L::SuspendedByPolicy};
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

GoalEvidenceRef EvidenceRefFromTrace(const GoalEvidence& evidence, const std::string& session_id,
                                     const std::string& run_id,
                                     const std::string& workspace_baseline) {
    GoalEvidenceRef ref;
    ref.id = evidence.id;
    ref.kind = ToString(evidence.kind);
    ref.source = GoalEvidenceSource::ToolAction;
    // sourceRef 回指工具调用(§4.67.5"至少回指 session/run、toolCallId 或
    // messageId");v1 采证没带 tool_use_id 的(宿主合成一类)以 producer
    // 兜底,不为凑非空造引用。
    ref.source_ref = !evidence.tool_use_id.empty()
                         ? evidence.tool_use_id
                         : ("host:" + (evidence.producer.empty() ? "unknown" : evidence.producer));
    ref.session_id = session_id;
    ref.run_id = run_id;
    ref.content_sha256 = !evidence.content_sha256.empty()
                             ? evidence.content_sha256
                             : hooks::Sha256Hex(evidence.facts.dump());
    ref.observed_at_ms = evidence.observed_at_ms;
    ref.workspace_baseline = workspace_baseline;
    ref.fresh = evidence.fresh;
    ref.truncated = evidence.truncated;
    ref.criterion_id.clear();  // 绑定验收项归 checkpoint/判词对账,不在采证时填
    return ref;
}

// ---------------------------------------------------------------------------
// 快照 schema
// ---------------------------------------------------------------------------

nlohmann::json GoalWaitPlan::ToJson() const {
    nlohmann::json j = nlohmann::json::object();
    j["pollsDone"] = polls_done;
    j["maxPolls"] = max_polls;
    j["nextDueMs"] = next_due_ms;
    return j;
}

GoalWaitPlan GoalWaitPlan::FromJson(const nlohmann::json& j) {
    GoalWaitPlan plan;
    if (!j.is_object()) return plan;
    if (j.contains("pollsDone") && j["pollsDone"].is_number_integer()) {
        plan.polls_done = j["pollsDone"].get<int>();
    }
    if (j.contains("maxPolls") && j["maxPolls"].is_number_integer()) {
        plan.max_polls = j["maxPolls"].get<int>();
    }
    if (j.contains("nextDueMs") && j["nextDueMs"].is_number_integer()) {
        plan.next_due_ms = j["nextDueMs"].get<std::int64_t>();
    }
    return plan;
}

std::int64_t GoalWaitBackoffMs(int poll_index) {
    // §4.67.7 首版建议:30/60/120 分钟退避;越界按最后一档(120)。
    switch (poll_index) {
        case 0: return 30LL * 60 * 1000;
        case 1: return 60LL * 60 * 1000;
        default: return 120LL * 60 * 1000;
    }
}

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
    j["waitPlan"] = wait_plan.ToJson();
    j["appliedEvaluationId"] = applied_evaluation_id.has_value()
                                   ? nlohmann::json(*applied_evaluation_id)
                                   : nlohmann::json(nullptr);
    j["appliedEvaluation"] = applied_evaluation;
    j["stopRequested"] = stop_requested;
    j["budget"] = BudgetToJson(budget);
    j["usage"] = UsageToJson(usage);
    j["counters"] = CountersToJson(counters);
    j["activeElapsedMs"] = active_elapsed_ms;
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
    if (j.contains("appliedEvaluation")) {
        if (!j["appliedEvaluation"].is_null() && !j["appliedEvaluation"].is_object())
            return fail("appliedEvaluation must be an object or null");
        s.applied_evaluation = j["appliedEvaluation"];
    }
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
    if (j.contains("waitPlan")) s.wait_plan = GoalWaitPlan::FromJson(j.at("waitPlan"));
    if (j.contains("stopRequested") && j["stopRequested"].is_boolean()) {
        s.stop_requested = j["stopRequested"].get<bool>();
    }
    if (j.contains("activeElapsedMs") && j["activeElapsedMs"].is_number_integer()) {
        s.active_elapsed_ms = j["activeElapsedMs"].get<std::int64_t>();
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
// continuation 意图(§4.67.4/G1)
// ---------------------------------------------------------------------------

std::string GoalPendingIntent::DedupeKey(const std::string& goal_id) const {
    // 续跑去重键四元组(§4.67.4):goalId/contractRevision/predecessor/
    // ordinal。'|' 不出现在 id 文法里,拼串即键。
    return goal_id + "|" + std::to_string(contract_revision) + "|" + predecessor_iteration_id +
           "|" + std::to_string(continuation_ordinal);
}

nlohmann::json GoalPendingIntent::ToJson() const {
    nlohmann::json j = nlohmann::json::object();
    j["workItemId"] = work_item_id;
    j["contractRevision"] = contract_revision;
    j["predecessorIterationId"] = predecessor_iteration_id;
    j["continuationOrdinal"] = continuation_ordinal;
    j["triggerRef"] = trigger_ref;
    j["nextActionRef"] = next_action_ref;
    j["claimed"] = claimed;
    j["writerEpoch"] = writer_epoch;
    j["claimedAtMs"] = claimed_at_ms;
    return j;
}

std::optional<GoalPendingIntent> GoalPendingIntent::FromJson(const nlohmann::json& j,
                                                             std::string* error) {
    if (const std::string invalid = ValidatePendingIntent(j); !invalid.empty()) {
        if (error != nullptr) *error = invalid;
        return std::nullopt;
    }
    GoalPendingIntent intent;
    intent.work_item_id = j.at("workItemId").get<std::string>();
    intent.contract_revision = j.at("contractRevision").get<std::uint64_t>();
    intent.predecessor_iteration_id = j.at("predecessorIterationId").get<std::string>();
    intent.continuation_ordinal = j.at("continuationOrdinal").get<int>();
    intent.trigger_ref = j.at("triggerRef").get<std::string>();
    intent.next_action_ref = j.at("nextActionRef").get<std::string>();
    intent.claimed = j.at("claimed").get<bool>();
    intent.writer_epoch = j.at("writerEpoch").get<std::string>();
    intent.claimed_at_ms = j.at("claimedAtMs").get<std::int64_t>();
    return intent;
}

std::string ValidatePendingIntent(const nlohmann::json& j) {
    const auto fail = [](const std::string& text) { return text; };
    if (!j.is_object()) return fail("pendingIntent 须为 object");
    for (const char* key : {"workItemId", "contractRevision", "predecessorIterationId",
                            "continuationOrdinal", "triggerRef", "nextActionRef", "claimed",
                            "writerEpoch", "claimedAtMs"}) {
        if (!j.contains(key)) return fail(std::string("pendingIntent 缺字段: ") + key);
    }
    if (!j.at("workItemId").is_string() || j.at("workItemId").get<std::string>().empty()) {
        return fail("workItemId 须为非空 string(恢复按它补队列)");
    }
    if (!j.at("contractRevision").is_number_unsigned() ||
        j.at("contractRevision").get<std::uint64_t>() < 1) {
        return fail("contractRevision 须为 >= 1 的无符号整数");
    }
    if (!j.at("predecessorIterationId").is_string()) {
        return fail("predecessorIterationId 须为 string(空 = 首轮)");
    }
    if (!j.at("continuationOrdinal").is_number_integer() ||
        j.at("continuationOrdinal").get<int>() < 1) {
        return fail("continuationOrdinal 须为 >= 1 的整数");
    }
    for (const char* key : {"triggerRef", "nextActionRef", "writerEpoch"}) {
        if (!j.at(key).is_string()) return fail(std::string(key) + std::string(" 须为 string"));
    }
    if (!j.at("claimed").is_boolean()) return fail("claimed 须为 boolean");
    if (!j.at("claimedAtMs").is_number_integer() || j.at("claimedAtMs").get<std::int64_t>() < 0) {
        return fail("claimedAtMs 须为非负整数");
    }
    if (j.at("claimed").get<bool>()) {
        if (j.at("writerEpoch").get<std::string>().empty()) {
            return fail("claimed=true 须带 writerEpoch(认领者)");
        }
        if (j.at("claimedAtMs").get<std::int64_t>() <= 0) {
            return fail("claimed=true 须带 claimedAtMs(认领时点)");
        }
    } else if (!j.at("writerEpoch").get<std::string>().empty() ||
               j.at("claimedAtMs").get<std::int64_t>() != 0) {
        return fail("claimed=false 时 writerEpoch/claimedAtMs 须为空/0");
    }
    return std::string();
}

GoalWorkView EvaluateGoalWork(const GoalStateSnapshot& snapshot, const std::string& writer_epoch) {
    GoalWorkView view;
    if (snapshot.pending_intent.is_object() && !snapshot.pending_intent.empty()) {
        std::string parse_error;
        const auto intent = GoalPendingIntent::FromJson(snapshot.pending_intent, &parse_error);
        if (!intent.has_value()) {
            view.reason = "pendingIntent 不合合同: " + parse_error;
            return view;
        }
        view.has_intent = true;
        view.intent = *intent;
        if (intent->claimed) {
            if (intent->writer_epoch != writer_epoch) {
                // §4.67.4 恢复核验(G2 收口):claim 落账后、开轮(Begin-
                // Iteration,先于模型发送)没落 = 账面证据"确认未发送"
                //(phase=queued)——本写者可接管沿用原 workItemId 续原请求,
                // 不重放副作用;已开轮/在评(running/evaluating)是他者
                // 在途,如实标 claimed_by_other,等他者的下一笔 applied。
                if (snapshot.phase != GoalPhase::Queued) {
                    view.claimed_by_other = true;
                    view.reason =
                        "工作项 " + intent->work_item_id + " 已被写者 " + intent->writer_epoch +
                        " 认领且在途(phase=" + ToString(snapshot.phase) + ");等他者收口";
                    return view;
                }
                view.claimable = true;
                view.reason = "他写者认领后未开轮(phase=queued,确认未发送):"
                              "接管沿用原工作项 " +
                              intent->work_item_id;
                return view;
            }
            if (snapshot.phase != GoalPhase::Queued) {
                view.reason = "工作项 " + intent->work_item_id + " 已认领且不在待开轮相位(" +
                              ToString(snapshot.phase) + ")";
                return view;
            }
            view.claimable = true;  // claim 后、开轮前:沿用原项(claim 幂等)
            return view;
        }
        if (intent->contract_revision != snapshot.contract_revision) {
            view.reason = "工作项 " + intent->work_item_id + " 对着旧合同(r" +
                          std::to_string(snapshot.contract_revision) + " 在账,意图带 r" +
                          std::to_string(intent->contract_revision) + ")";
            return view;
        }
    } else {
        view.reason = "没有待续意图";
        return view;
    }
    if (IsLifecycleTerminal(snapshot.lifecycle)) {
        view.reason = "目标已收账(" + ToString(snapshot.lifecycle) + ")";
        return view;
    }
    const bool stopped = snapshot.lifecycle == GoalLifecycle::Waiting ||
                         snapshot.lifecycle == GoalLifecycle::Paused ||
                         snapshot.lifecycle == GoalLifecycle::AwaitingUser ||
                         snapshot.lifecycle == GoalLifecycle::Blocked ||
                         snapshot.lifecycle == GoalLifecycle::BudgetExhausted ||
                         snapshot.lifecycle == GoalLifecycle::SuspendedByPolicy;
    if (stopped) {
        view.reason = "目标停态(" + ToString(snapshot.lifecycle) + "),明确恢复后才排";
        return view;
    }
    if (snapshot.stop_requested) {
        // G3(§4.67.10 竞态行):停止意图优先——Esc/pause 已在账,迟到结果
        // 不拉起新轮;显式 resume(转回 active)清旗后再排。
        view.reason = "停止意图在账(Esc/pause 先行),显式 resume 后再排";
        return view;
    }
    view.claimable = true;
    return view;
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
    // G1:首轮调度意图随初始快照提交(§4.67.4 接纳);形状先验,坏草稿不落盘。
    if (draft.pending_intent.is_object() && !draft.pending_intent.empty()) {
        if (const std::string invalid = ValidatePendingIntent(draft.pending_intent);
            !invalid.empty()) {
            return Fail(kErrGoalCandidateInvalid, "draft.pendingIntent 不合合同: " + invalid);
        }
        std::string parse_error;
        const auto intent = GoalPendingIntent::FromJson(draft.pending_intent, &parse_error);
        if (intent.has_value() && intent->claimed) {
            return Fail(kErrGoalCandidateInvalid, "首轮意图不能预置 claimed");
        }
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
    draft.blocker_key.clear();
    draft.pending_question.clear();
    draft.wait_task_refs.clear();   // 新 goal 不带等待账(G3)
    draft.wait_plan = GoalWaitPlan{};
    draft.stop_requested = false;
    draft.active_elapsed_ms = 0;
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
    // 同态改版合法:preparing 原地改合同(contractRevision/stateRevision 都
    // 在动,不是"没变化的假提交"——投影对同 goal 的 applied 只在 lifecycle
    // 变了时才验转换)。刚立未开跑的目标立即 /goal edit 是正路;其余态仍按
    // 转换表(budget_exhausted/suspended_by_policy 回不到 preparing,拒)。
    if (current_->lifecycle != GoalLifecycle::Preparing &&
        !IsValidLifecycleTransition(current_->lifecycle, GoalLifecycle::Preparing)) {
        return Fail(kErrGoalInvalidTransition,
                    "当前 lifecycle(" + ToString(current_->lifecycle) + ")不受理合同改版");
    }
    if (contract.objective.empty()) {
        return Fail(kErrGoalCandidateInvalid, "改版合同缺 objective");
    }
    // 在账意图分路(§4.67.2 edit 行"安全边界提交"+ §4.67.4"旧 Goal 工作
    // 项不挤过排在边界前的 pause/edit/clear"+ §4.67.10 改版撞迟到判词行):
    //   - 已认领且工作轮在途(running/queued):edit 等安全边界——拒,先
    //     pause 或等本轮收口(收口即销账 intent),再 edit。终态明确可恢复,
    //     不在工作轮中途换合同、拍掉在途相位。
    //   - 已认领但评估在途(evaluating):放行(设计矩阵 M4"合同改版撞上
    //     迟到判词"):改版落 preparing/idle,开评时冻结的 stateRevision
    //     让迟到判词被 CAS/相位拒;认领随迟到判词一并作废清空,不滞留。
    //   - 未认领:意图指向的合同已不存在,随合同作废清空。两种放行情都
    //     由命令面按新 contractRevision 重拟(与 resume 补意图同款命名),
    //     泵下一拍即按新合同开轮——不留"意图对着旧合同永不 claimable"
    //     的死锁。
    if (current_->pending_intent.is_object() && !current_->pending_intent.empty()) {
        std::string intent_error;
        const auto previous = GoalPendingIntent::FromJson(current_->pending_intent, &intent_error);
        if (!previous.has_value()) {
            return Fail(kErrGoalCandidateInvalid, "在账意图读不出(不受理改版): " + intent_error);
        }
        if (previous->claimed && current_->phase != GoalPhase::Evaluating) {
            return Fail(kErrGoalBusy,
                        "工作项 " + previous->work_item_id + " 已被认领(phase=" +
                            ToString(current_->phase) + ");edit 等安全边界——先 /goal pause 或等本轮收口");
        }
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
    next.pending_intent = nlohmann::json::object();  // 旧意图随合同作废(见上)
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

// ---- §4.67 G1:意图提交 / 认领 / 开轮 / 收工 --------------------------------

namespace {

// 停态判式(与 EvaluateGoalWork 同一张表;服务侧提交校验用)。
bool LifecycleIsStopped(GoalLifecycle lifecycle) {
    switch (lifecycle) {
        case GoalLifecycle::Waiting:
        case GoalLifecycle::Paused:
        case GoalLifecycle::AwaitingUser:
        case GoalLifecycle::Blocked:
        case GoalLifecycle::BudgetExhausted:
        case GoalLifecycle::SuspendedByPolicy:
            return true;
        default:
            return false;
    }
}

}  // namespace

GoalServiceResult GoalService::SetPendingIntent(GoalPendingIntent intent,
                                                std::uint64_t expected_state_revision,
                                                nlohmann::json cause_ref) {
    if (broken_) {
        return Fail(kErrGoalStoreUnavailable, "goal 写口已锁(此前写盘失败);fail closed");
    }
    if (writer_ == nullptr || options_.session_dir.empty()) {
        return Fail(kErrGoalStoreUnavailable, "GoalService 写口未接线");
    }
    if (!current_.has_value()) return Fail(kErrGoalNotFound, "没有 goal");
    if (IsLifecycleTerminal(current_->lifecycle)) {
        return Fail(kErrGoalTerminal, "目标已收账,不受理新意图");
    }
    if (expected_state_revision != current_->state_revision) {
        return Fail(kErrGoalRevisionConflict, "stateRevision 冲突:状态已被改过");
    }
    if (const std::string invalid = ValidatePendingIntent(intent.ToJson()); !invalid.empty()) {
        return Fail(kErrGoalCandidateInvalid, "意图不合合同: " + invalid);
    }
    if (intent.claimed) {
        return Fail(kErrGoalCandidateInvalid, "SetPendingIntent 只交未认领意图(claim 走 ClaimPendingIntent)");
    }
    if (intent.contract_revision != current_->contract_revision) {
        return Fail(kErrGoalIntentConflict,
                    "意图对着旧合同(在账 r" + std::to_string(current_->contract_revision) +
                        ",意图带 r" + std::to_string(intent.contract_revision) + ")");
    }
    // 前一枚未认领的意图不许静默覆盖:欠队列的账不能丢(§4.67.4 旧工作项
    // 不挤过边界命令;先 claim/consume 或先改合同)。
    if (current_->pending_intent.is_object() && !current_->pending_intent.empty()) {
        std::string parse_error;
        const auto previous = GoalPendingIntent::FromJson(current_->pending_intent, &parse_error);
        if (!previous.has_value()) {
            return Fail(kErrGoalCandidateInvalid, "在账意图读不出(不覆盖坏账): " + parse_error);
        }
        if (!previous->claimed &&
            previous->DedupeKey(current_->goal_id) != intent.DedupeKey(current_->goal_id)) {
            return Fail(kErrGoalIntentConflict,
                        "前一枚意图 " + previous->work_item_id + " 未认领,不许覆盖(先认领或改合同)");
        }
    }
    GoalStateSnapshot next = *current_;
    next.state_revision += 1;
    next.pending_intent = intent.ToJson();
    next.updated_at_ms = Now();
    return Commit(std::move(next), std::move(cause_ref));
}

GoalServiceResult GoalService::ClaimPendingIntent(std::string writer_epoch,
                                                   std::uint64_t expected_state_revision,
                                                   nlohmann::json cause_ref) {
    if (broken_) {
        return Fail(kErrGoalStoreUnavailable, "goal 写口已锁(此前写盘失败);fail closed");
    }
    if (writer_ == nullptr || options_.session_dir.empty()) {
        return Fail(kErrGoalStoreUnavailable, "GoalService 写口未接线");
    }
    if (!current_.has_value()) return Fail(kErrGoalNotFound, "没有 goal");
    if (expected_state_revision != current_->state_revision) {
        return Fail(kErrGoalRevisionConflict, "stateRevision 冲突:状态已被改过");
    }
    if (writer_epoch.empty()) {
        return Fail(kErrGoalCandidateInvalid, "writerEpoch 不能为空(单写者接管凭据)");
    }
    if (current_->pending_intent.is_null() ||
        (current_->pending_intent.is_object() && current_->pending_intent.empty())) {
        return Fail(kErrGoalIntentMissing, "没有待认领的意图");
    }
    std::string parse_error;
    const auto intent = GoalPendingIntent::FromJson(current_->pending_intent, &parse_error);
    if (!intent.has_value()) {
        return Fail(kErrGoalCandidateInvalid, "在账意图不合合同: " + parse_error);
    }
    if (IsLifecycleTerminal(current_->lifecycle)) {
        return Fail(kErrGoalTerminal, "目标已收账,迟到认领拒(不复活)");
    }
    if (intent->claimed) {
        if (intent->writer_epoch == writer_epoch) {
            // 同写者幂等:claim 落过账、调用方没等到回执的重试。
            GoalServiceResult r;
            r.ok = true;
            r.payload["goalId"] = current_->goal_id;
            r.payload["stateRevision"] = current_->state_revision;
            r.payload["contractRevision"] = current_->contract_revision;
            r.payload["workItemId"] = intent->work_item_id;
            r.payload["idempotent"] = true;
            return r;
        }
        if (current_->phase == GoalPhase::Queued) {
            // §4.67.4 恢复核验(G2 收口):他写者 claim 落账后开轮没落
            //(phase=queued,开轮先于模型发送)= 账面证据"确认未发送"
            // ——本写者接管沿用原 workItemId 续原请求,不重放副作用;
            // 接管本身落 applied,原写者迟到的开轮会被 CAS 拒。
            GoalStateSnapshot next = *current_;
            next.state_revision += 1;
            GoalPendingIntent taken = *intent;
            taken.writer_epoch = writer_epoch;
            taken.claimed_at_ms = Now();
            next.pending_intent = taken.ToJson();
            next.updated_at_ms = Now();
            GoalServiceResult r = Commit(std::move(next), cause_ref);
            if (r.ok) {
                r.payload["workItemId"] = taken.work_item_id;
                r.payload["adoptedFromEpoch"] = intent->writer_epoch;
            }
            return r;
        }
        GoalServiceResult r = Fail(kErrGoalIntentAlreadyClaimed,
                                   "工作项 " + intent->work_item_id + " 已被写者 " +
                                       intent->writer_epoch + " 认领且在途(phase=" +
                                       ToString(current_->phase) + ");不盲重放");
        r.payload["workItemId"] = intent->work_item_id;
        r.payload["writerEpoch"] = intent->writer_epoch;
        return r;
    }
    if (LifecycleIsStopped(current_->lifecycle)) {
        return Fail(kErrGoalCandidateInvalid,
                    "停态(" + ToString(current_->lifecycle) + ")不认领;明确恢复后再取");
    }
    GoalStateSnapshot next = *current_;
    next.state_revision += 1;
    GoalPendingIntent claimed = *intent;
    claimed.claimed = true;
    claimed.writer_epoch = writer_epoch;
    claimed.claimed_at_ms = Now();
    next.pending_intent = claimed.ToJson();
    next.phase = GoalPhase::Queued;
    next.updated_at_ms = Now();
    GoalServiceResult r = Commit(std::move(next), std::move(cause_ref));
    if (r.ok) r.payload["workItemId"] = claimed.work_item_id;
    return r;
}

GoalServiceResult GoalService::BeginIteration(std::uint64_t expected_state_revision,
                                              nlohmann::json cause_ref) {
    if (broken_) {
        return Fail(kErrGoalStoreUnavailable, "goal 写口已锁(此前写盘失败);fail closed");
    }
    if (writer_ == nullptr || options_.session_dir.empty()) {
        return Fail(kErrGoalStoreUnavailable, "GoalService 写口未接线");
    }
    if (!current_.has_value()) return Fail(kErrGoalNotFound, "没有 goal");
    if (expected_state_revision != current_->state_revision) {
        return Fail(kErrGoalRevisionConflict, "stateRevision 冲突:状态已被改过");
    }
    if (IsLifecycleTerminal(current_->lifecycle)) {
        return Fail(kErrGoalTerminal, "目标已收账,不开了");
    }
    if (LifecycleIsStopped(current_->lifecycle)) {
        return Fail(kErrGoalCandidateInvalid,
                    "停态(" + ToString(current_->lifecycle) + ")不开轮");
    }
    if (current_->phase == GoalPhase::Running) {
        return Fail(kErrGoalCandidateInvalid, "已有轮在跑(收口走 EndIteration)");
    }
    // G3 预算闸(§4.67.7"请求前和工具派发前检查预算"):开新一轮前核三尺
    //(轮数/token+预留/active 时长);撞帽不 silently 拒——落 budget_
    // exhausted 快照(旧费用保留),显式加预算后可恢复。
    if (const std::string budget_reason =
            BudgetStopReason(*current_, /*next_tokens=*/0, /*counting_next_iteration=*/true);
        !budget_reason.empty()) {
        GoalStateSnapshot halted = *current_;
        halted.state_revision += 1;
        if (IsValidLifecycleTransition(current_->lifecycle, GoalLifecycle::BudgetExhausted)) {
            halted.lifecycle = GoalLifecycle::BudgetExhausted;
        } else {
            halted.lifecycle = GoalLifecycle::Paused;  // 停态再撞帽:保持停态语义
        }
        halted.phase = GoalPhase::Idle;
        halted.stop_reason = "budget_exhausted: " + budget_reason;
        halted.updated_at_ms = Now();
        const auto halted_result = Commit(std::move(halted), cause_ref);
        if (!halted_result.ok) return halted_result;
        return Fail(kErrGoalBudgetExhausted, "预算已尽: " + budget_reason);
    }
    GoalStateSnapshot next = *current_;
    next.state_revision += 1;
    if (current_->lifecycle == GoalLifecycle::Preparing) {
        // 首轮即合同拟定期:objective 已在手即底稿(真 preflight/冻结归 G2),
        // 取轮即转 active(§4.67.3 preparing 出口)。
        if (!IsValidLifecycleTransition(GoalLifecycle::Preparing, GoalLifecycle::Active)) {
            return Fail(kErrGoalInvalidTransition, "preparing -> active 转换被改坏");
        }
        next.lifecycle = GoalLifecycle::Active;
    }
    // iterationId 归属(§4.67.4):goal-<n>/iter-<m>,m 从 1 起;开轮才发号,
    // 重试请求不冒充新 iteration。
    next.counters.iterations_started += 1;
    next.iteration_id = next.goal_id + "/iter-" + std::to_string(next.counters.iterations_started);
    next.phase = GoalPhase::Running;
    next.updated_at_ms = Now();
    GoalServiceResult r = Commit(std::move(next), std::move(cause_ref));
    if (r.ok) {
        r.payload["iterationId"] = *current_->iteration_id;
        r.payload["iterationIndex"] = current_->counters.iterations_started;
    }
    return r;
}

GoalServiceResult GoalService::EndIteration(std::uint64_t expected_state_revision,
                                            nlohmann::json cause_ref) {
    if (broken_) {
        return Fail(kErrGoalStoreUnavailable, "goal 写口已锁(此前写盘失败);fail closed");
    }
    if (writer_ == nullptr || options_.session_dir.empty()) {
        return Fail(kErrGoalStoreUnavailable, "GoalService 写口未接线");
    }
    if (!current_.has_value()) return Fail(kErrGoalNotFound, "没有 goal");
    if (expected_state_revision != current_->state_revision) {
        return Fail(kErrGoalRevisionConflict, "stateRevision 冲突:状态已被改过");
    }
    if (current_->phase != GoalPhase::Running) {
        return Fail(kErrGoalCandidateInvalid,
                    "不在执行轮收口位(phase=" + ToString(current_->phase) + ")");
    }
    GoalStateSnapshot next = *current_;
    next.state_revision += 1;
    next.phase = GoalPhase::Idle;
    // 认领过的工作项销账:pendingIntent 清空,下一次 resume 不再补队列
    //(§4.67.4 已入队已收口;判词与续排意图归 G2,不在这假装评过)。
    next.pending_intent = nlohmann::json::object();
    next.updated_at_ms = Now();
    GoalServiceResult r = Commit(std::move(next), std::move(cause_ref));
    if (r.ok && current_->iteration_id.has_value()) {
        r.payload["iterationId"] = *current_->iteration_id;
    }
    return r;
}

GoalServiceResult GoalService::BeginEvaluation(std::uint64_t expected_state_revision,
                                               std::optional<std::string> checkpoint_ref,
                                               std::vector<GoalEvidenceRef> evidence_additions,
                                               std::vector<std::string> evidence_stale_ids,
                                               nlohmann::json cause_ref) {
    if (broken_) {
        return Fail(kErrGoalStoreUnavailable, "goal 写口已锁(此前写盘失败);fail closed");
    }
    if (writer_ == nullptr || options_.session_dir.empty()) {
        return Fail(kErrGoalStoreUnavailable, "GoalService 写口未接线");
    }
    if (!current_.has_value()) return Fail(kErrGoalNotFound, "没有 goal");
    if (expected_state_revision != current_->state_revision) {
        return Fail(kErrGoalRevisionConflict, "stateRevision 冲突:状态已被改过");
    }
    if (IsLifecycleTerminal(current_->lifecycle)) {
        return Fail(kErrGoalTerminal, "目标已收账,不受理验收(clear 前的迟到收口拒)");
    }
    if (LifecycleIsStopped(current_->lifecycle)) {
        return Fail(kErrGoalCandidateInvalid,
                    "停态(" + ToString(current_->lifecycle) + ")不排验收;明确恢复后再评");
    }
    if (current_->phase != GoalPhase::Running) {
        return Fail(kErrGoalCandidateInvalid,
                    "不在执行轮收口位(phase=" + ToString(current_->phase) + ")");
    }
    for (const auto& ref : evidence_additions) {
        if (const std::string invalid = ValidateEvidenceRef(ref.ToJson()); !invalid.empty()) {
            return Fail(kErrGoalCandidateInvalid, "evidence 候选坏项: " + invalid);
        }
    }
    GoalStateSnapshot next = *current_;
    next.state_revision += 1;
    next.phase = GoalPhase::Evaluating;
    if (checkpoint_ref.has_value() && !checkpoint_ref->empty()) {
        next.checkpoint_ref = std::move(checkpoint_ref);
    }
    for (auto& ref : evidence_additions) {
        next.evidence_refs.push_back(std::move(ref));
    }
    // 证据有效期:按 id 翻旧(快照每版全量,在副本上整改;未知 id 静默
    // 略过——翻旧是保守动作,不为它拒提交)。
    for (const auto& stale_id : evidence_stale_ids) {
        for (auto& ref : next.evidence_refs) {
            if (ref.id == stale_id) ref.fresh = false;
        }
    }
    next.updated_at_ms = Now();
    GoalServiceResult r = Commit(std::move(next), std::move(cause_ref));
    if (r.ok && current_->iteration_id.has_value()) {
        r.payload["iterationId"] = *current_->iteration_id;
        r.payload["evaluationId"] = "eval-" + *current_->iteration_id;
    }
    return r;
}

GoalServiceResult GoalService::CompleteIterationWithEvaluation(
    std::uint64_t expected_state_revision, const EvaluationVerdict& verdict,
    nlohmann::json cause_ref) {
    if (broken_) {
        return Fail(kErrGoalStoreUnavailable, "goal 写口已锁(此前写盘失败);fail closed");
    }
    if (writer_ == nullptr || options_.session_dir.empty()) {
        return Fail(kErrGoalStoreUnavailable, "GoalService 写口未接线");
    }
    if (!current_.has_value()) return Fail(kErrGoalNotFound, "没有 goal");
    if (expected_state_revision != current_->state_revision) {
        return Fail(kErrGoalRevisionConflict, "stateRevision 冲突:状态已被改过");
    }
    if (current_->phase != GoalPhase::Evaluating) {
        return Fail(kErrGoalCandidateInvalid,
                    "不在验收收口位(phase=" + ToString(current_->phase) + ";BeginEvaluation 先落)");
    }
    if (verdict.evaluation_id.empty()) {
        return Fail(kErrGoalCandidateInvalid, "判词采用缺 evaluationId(绑定提交的锚)");
    }
    // 分路先验(坏候选不落盘,fail closed 前置)。
    GoalLifecycle to_lifecycle = current_->lifecycle;
    switch (verdict.kind) {
        case GoalVerdictKind::Continue:
            if (!verdict.next_intent.has_value()) {
                return Fail(kErrGoalCandidateInvalid,
                            "continue 判词必带下一轮意图(§4.67.6 同一快照提交)");
            }
            if (verdict.next_intent->contract_revision != current_->contract_revision) {
                return Fail(kErrGoalCandidateInvalid,
                            "续排意图对着旧合同(在账 r" +
                                std::to_string(current_->contract_revision) + ")");
            }
            break;
        case GoalVerdictKind::Achieved:
            to_lifecycle = GoalLifecycle::Achieved;
            break;
        case GoalVerdictKind::Blocked:
            to_lifecycle = GoalLifecycle::Blocked;
            if (verdict.blocker_key.empty()) {
                return Fail(kErrGoalCandidateInvalid, "blocked 判词缺 blockerKey");
            }
            break;
        case GoalVerdictKind::NeedsUser:
            to_lifecycle = GoalLifecycle::AwaitingUser;
            if (verdict.pending_question.empty()) {
                return Fail(kErrGoalCandidateInvalid, "needs_user 判词缺 pendingQuestion");
            }
            break;
        case GoalVerdictKind::EvaluatorFailed:
            // 评估故障(超时/二次坏判词):暂停收口,保留停因(§4.67.5)。
            to_lifecycle = GoalLifecycle::Paused;
            if (verdict.stop_reason.empty()) {
                return Fail(kErrGoalCandidateInvalid, "evaluator_failed 收口缺 stopReason");
            }
            break;
    }
    if (to_lifecycle != current_->lifecycle &&
        !IsValidLifecycleTransition(current_->lifecycle, to_lifecycle)) {
        return Fail(kErrGoalInvalidTransition,
                    "lifecycle 转换非法: " + ToString(current_->lifecycle) + " -> " +
                        ToString(to_lifecycle));
    }
    const bool to_stopped = to_lifecycle == GoalLifecycle::Paused ||
                            to_lifecycle == GoalLifecycle::Blocked ||
                            to_lifecycle == GoalLifecycle::AwaitingUser;
    if (to_stopped && verdict.stop_reason.empty()) {
        return Fail(kErrGoalCandidateInvalid, "停态判词缺 stopReason");
    }
    if (verdict.next_intent.has_value()) {
        if (const std::string invalid = ValidatePendingIntent(verdict.next_intent->ToJson());
            !invalid.empty()) {
            return Fail(kErrGoalCandidateInvalid, "续排意图不合合同: " + invalid);
        }
    }

    // 一次提交收齐(§4.67.6"采用判词与下一轮意图写在同一快照提交中"):
    // 收口(phase->idle、本轮 intent 销账)+ appliedEvaluationId 绑定 +
    // lifecycle 分路 + 续排意图 + usage 只增。evaluator_failed 不绑
    // evaluationId——没有判词可采,它的审计锚是账上 rejected 事实行。
    // G3 两条收口岔路(§4.67.7/§4.67.10):continue 判词撞上停止意图或
    // 预算撞帽 → 判词照采(evaluationId/usage 落账),但不排下一轮——
    // 停止意图优先,迟到结果不拉起新轮。
    GoalCounters counters = current_->counters;
    if (verdict.kind == GoalVerdictKind::Continue && !verdict.progress_fingerprint.empty()) {
        if (counters.last_progress_fingerprint == verdict.progress_fingerprint)
            ++counters.no_progress_streak;
        else {
            counters.last_progress_fingerprint = verdict.progress_fingerprint;
            counters.no_progress_streak = 0;
        }
    }
    bool park_instead_of_continue = false;
    std::string park_reason;
    if (verdict.kind == GoalVerdictKind::Continue) {
        if (current_->stop_requested) {
            park_instead_of_continue = true;
            park_reason = "stop_requested: 停止意图在账,判词已采但不自动续排";
        } else if (!verdict.progress_fingerprint.empty() &&
                   current_->budget.max_no_progress_iterations > 0 &&
                   counters.no_progress_streak >= current_->budget.max_no_progress_iterations) {
            park_instead_of_continue = true;
            park_reason = "no_progress: unchanged evidence and criterion status";
        } else if (const std::string budget_reason =
                       BudgetStopReason(*current_, /*next_tokens=*/0,
                                        /*counting_next_iteration=*/true);
                   !budget_reason.empty()) {
            park_instead_of_continue = true;
            park_reason = "budget_exhausted: " + budget_reason;
        }
    }
    GoalStateSnapshot next = *current_;
    next.state_revision += 1;
    next.phase = GoalPhase::Idle;
    if (verdict.kind != GoalVerdictKind::EvaluatorFailed) {
        next.applied_evaluation_id = verdict.evaluation_id;
        next.applied_evaluation = verdict.evaluation;
    }
    next.usage.Add(verdict.usage_addition);
    next.counters = std::move(counters);
    if (verdict.kind == GoalVerdictKind::Continue && !park_instead_of_continue) {
        next.pending_intent = verdict.next_intent->ToJson();
    } else {
        next.pending_intent = nlohmann::json::object();  // 停态不排(恢复另提交)
    }
    if (park_instead_of_continue) {
        GoalLifecycle park_to = park_reason.rfind("budget_exhausted", 0) == 0
                                    ? GoalLifecycle::BudgetExhausted
                                    : GoalLifecycle::Paused;
        if (!IsValidLifecycleTransition(current_->lifecycle, park_to)) {
            park_to = GoalLifecycle::Paused;
        }
        if (IsValidLifecycleTransition(current_->lifecycle, park_to)) {
            next.lifecycle = park_to;
        }
        next.stop_reason = park_reason;
        next.blocker_key.clear();
        next.pending_question.clear();
    }
    if (to_lifecycle != current_->lifecycle) {
        next.lifecycle = to_lifecycle;
        next.stop_reason = to_stopped ? verdict.stop_reason : std::string();
        next.blocker_key = verdict.kind == GoalVerdictKind::Blocked ? verdict.blocker_key
                                                                    : std::string();
        next.pending_question = verdict.kind == GoalVerdictKind::NeedsUser
                                    ? verdict.pending_question
                                    : std::string();
    }
    next.updated_at_ms = Now();
    GoalServiceResult r = Commit(std::move(next), std::move(cause_ref));
    if (r.ok) {
        r.payload["evaluationId"] = verdict.evaluation_id;
        r.payload["verdictKind"] = [kind = verdict.kind] {
            switch (kind) {
                case GoalVerdictKind::Continue: return "continue";
                case GoalVerdictKind::Achieved: return "achieved";
                case GoalVerdictKind::Blocked: return "blocked";
                case GoalVerdictKind::NeedsUser: return "needs_user";
                case GoalVerdictKind::EvaluatorFailed: return "evaluator_failed";
            }
            return "unknown";
        }();
        if (verdict.kind == GoalVerdictKind::Continue && !park_instead_of_continue) {
            r.payload["nextWorkItemId"] = verdict.next_intent->work_item_id;
        }
        if (park_instead_of_continue) {
            r.payload["parked"] = park_reason;
        }
    }
    return r;
}

// ---- §4.67 G3:停止意图、后台等待、预算预留、fork ---------------------------

GoalServiceResult GoalService::RequestStop(std::uint64_t expected_state_revision,
                                            nlohmann::json cause_ref) {
    if (broken_) {
        return Fail(kErrGoalStoreUnavailable, "goal 写口已锁(此前写盘失败);fail closed");
    }
    if (writer_ == nullptr || options_.session_dir.empty()) {
        return Fail(kErrGoalStoreUnavailable, "GoalService 写口未接线");
    }
    if (!current_.has_value()) return Fail(kErrGoalNotFound, "没有 goal");
    if (IsLifecycleTerminal(current_->lifecycle)) {
        return Fail(kErrGoalTerminal, "目标已收账,停止意图无的放矢");
    }
    if (current_->stop_requested) {
        // 幂等:旗已落账,不空耗 revision。
        GoalServiceResult r;
        r.ok = true;
        r.payload["goalId"] = current_->goal_id;
        r.payload["stateRevision"] = current_->state_revision;
        r.payload["idempotent"] = true;
        return r;
    }
    if (expected_state_revision != current_->state_revision) {
        return Fail(kErrGoalRevisionConflict, "stateRevision 冲突:状态已被改过");
    }
    GoalStateSnapshot next = *current_;
    next.state_revision += 1;
    next.stop_requested = true;
    next.updated_at_ms = Now();
    return Commit(std::move(next), std::move(cause_ref));
}

GoalServiceResult GoalService::EnterWaiting(std::vector<std::string> task_refs,
                                             std::uint64_t expected_state_revision,
                                             nlohmann::json cause_ref) {
    if (broken_) {
        return Fail(kErrGoalStoreUnavailable, "goal 写口已锁(此前写盘失败);fail closed");
    }
    if (writer_ == nullptr || options_.session_dir.empty()) {
        return Fail(kErrGoalStoreUnavailable, "GoalService 写口未接线");
    }
    if (!current_.has_value()) return Fail(kErrGoalNotFound, "没有 goal");
    if (task_refs.empty()) {
        return Fail(kErrGoalCandidateInvalid,
                    "登记等待须带 taskRefs(无关进程不进等待账,空表该走验收)");
    }
    for (const auto& ref : task_refs) {
        if (ref.empty()) {
            return Fail(kErrGoalCandidateInvalid, "taskRefs 坏项:空 string");
        }
    }
    if (expected_state_revision != current_->state_revision) {
        return Fail(kErrGoalRevisionConflict, "stateRevision 冲突:状态已被改过");
    }
    if (IsLifecycleTerminal(current_->lifecycle)) {
        return Fail(kErrGoalTerminal, "目标已收账,不受理等待登记");
    }
    if (!IsValidLifecycleTransition(current_->lifecycle, GoalLifecycle::Waiting)) {
        return Fail(kErrGoalInvalidTransition,
                    "lifecycle 转换非法: " + ToString(current_->lifecycle) + " -> waiting");
    }
    // 巡检计划(§4.67.7):30 分钟起退避,次数入快照(重启不归零)。
    const std::int64_t now = Now();
    GoalWaitPlan plan;
    plan.polls_done = 0;
    plan.max_polls = 3;
    plan.next_due_ms = now + GoalWaitBackoffMs(0);
    // 事实行先落(登记材料);等待是否生效仍看随后的 applied。
    {
        std::string joined;
        for (const auto& ref : task_refs) {
            if (!joined.empty()) joined += "|";
            joined += ref;
        }
        EventDraft registered;
        registered.kind = EventKindV3::GoalWaitRegistered;
        registered.payload["goalId"] = current_->goal_id;
        registered.payload["taskRefs"] = task_refs;
        registered.payload["notifyDedupeKey"] = hooks::Sha256Hex(joined);
        registered.payload["inspectionPlan"] = plan.ToJson();
        const auto receipt = writer_->AppendEvent(std::move(registered), Durability::ProcessCrash);
        if (receipt.status != trajectory::v3::WriteReceipt::Status::Committed) {
            return Fail(kErrGoalStoreUnavailable,
                        "goal.wait.registered 落账失败(" + receipt.error_code + "): " +
                            receipt.error_message);
        }
    }
    GoalStateSnapshot next = *current_;
    next.state_revision += 1;
    next.lifecycle = GoalLifecycle::Waiting;
    next.phase = GoalPhase::Idle;  // 停态相位回 idle;iteration 原地保留
    next.stop_reason = "waiting:background_tasks";
    next.wait_task_refs = std::move(task_refs);
    next.wait_plan = plan;
    next.updated_at_ms = now;
    return Commit(std::move(next), std::move(cause_ref));
}

bool GoalService::WaitInspectionDue(std::int64_t now_ms) const {
    if (!current_.has_value()) return false;
    if (current_->lifecycle != GoalLifecycle::Waiting) return false;
    if (current_->wait_plan.next_due_ms <= 0) return false;  // 未排/已到上限
    if (current_->wait_plan.polls_done >= current_->wait_plan.max_polls) return false;
    return now_ms >= current_->wait_plan.next_due_ms;
}

GoalServiceResult GoalService::RecordWaitInspection(std::uint64_t expected_state_revision,
                                                    std::int64_t now_ms, nlohmann::json cause_ref) {
    if (broken_) {
        return Fail(kErrGoalStoreUnavailable, "goal 写口已锁(此前写盘失败);fail closed");
    }
    if (writer_ == nullptr || options_.session_dir.empty()) {
        return Fail(kErrGoalStoreUnavailable, "GoalService 写口未接线");
    }
    if (!current_.has_value()) return Fail(kErrGoalNotFound, "没有 goal");
    if (current_->lifecycle != GoalLifecycle::Waiting) {
        return Fail(kErrGoalCandidateInvalid,
                    "不在等待态(" + ToString(current_->lifecycle) + "),无巡检可记");
    }
    if (expected_state_revision != current_->state_revision) {
        return Fail(kErrGoalRevisionConflict, "stateRevision 冲突:状态已被改过");
    }
    if (current_->wait_plan.polls_done >= current_->wait_plan.max_polls) {
        return Fail(kErrGoalCandidateInvalid, "巡检已到次数上限(真实完成通知仍可唤醒)");
    }
    GoalStateSnapshot next = *current_;
    next.state_revision += 1;
    next.wait_plan.polls_done += 1;
    // 到上限:停排巡检(nextDue=0),目标仍 waiting——真实完成仍可唤醒;
    // 未到:按 30/60/120 退避排下一拍。次数入快照,重启不归零(§4.67.7)。
    next.wait_plan.next_due_ms =
        next.wait_plan.polls_done >= next.wait_plan.max_polls
            ? 0
            : now_ms + GoalWaitBackoffMs(next.wait_plan.polls_done);
    next.updated_at_ms = Now();
    GoalServiceResult r = Commit(std::move(next), std::move(cause_ref));
    if (r.ok && current_->wait_plan.next_due_ms == 0) {
        r.payload["stopped"] = true;  // 这一拍到顶:调用方通知一次
    }
    return r;
}

GoalServiceResult GoalService::ResolveWaiting(const std::string& delivery_key,
                                              std::uint64_t expected_state_revision,
                                              nlohmann::json cause_ref,
                                              const std::string& reason) {
    if (broken_) {
        return Fail(kErrGoalStoreUnavailable, "goal 写口已锁(此前写盘失败);fail closed");
    }
    if (writer_ == nullptr || options_.session_dir.empty()) {
        return Fail(kErrGoalStoreUnavailable, "GoalService 写口未接线");
    }
    if (!current_.has_value()) return Fail(kErrGoalNotFound, "没有 goal");
    if (delivery_key.empty()) {
        return Fail(kErrGoalCandidateInvalid, "deliveryKey 不能为空(通知去重键)");
    }
    if (expected_state_revision != current_->state_revision) {
        return Fail(kErrGoalRevisionConflict, "stateRevision 冲突:状态已被改过");
    }
    if (current_->lifecycle != GoalLifecycle::Waiting) {
        // pause/clear/终态之后的迟到后台报告:留调用方审计,不拉起新轮
        //(§4.67.10 竞态行)。
        return Fail(kErrGoalNotWaiting,
                    "不在等待态(" + ToString(current_->lifecycle) + ");迟到交付只留审计");
    }
    {
        EventDraft resolved;
        resolved.kind = EventKindV3::GoalWaitResolved;
        resolved.payload["goalId"] = current_->goal_id;
        resolved.payload["deliveryKey"] = delivery_key;
        resolved.payload["reason"] = reason.empty() ? std::string("background_task_finished")
                                                    : reason;
        const auto receipt = writer_->AppendEvent(std::move(resolved), Durability::ProcessCrash);
        if (receipt.status != trajectory::v3::WriteReceipt::Status::Committed) {
            return Fail(kErrGoalStoreUnavailable,
                        "goal.wait.resolved 落账失败(" + receipt.error_code + "): " +
                            receipt.error_message);
        }
    }
    GoalStateSnapshot next = *current_;
    next.state_revision += 1;
    next.lifecycle = GoalLifecycle::Active;
    next.stop_reason.clear();
    next.wait_task_refs.clear();
    next.wait_plan = GoalWaitPlan{};
    // 收口位等待(iteration 在途、工作项已认领)恢复 running 供收口续跑;
    // 其余恢复 idle(等待发生在排队/间歇)。
    next.phase = current_->pending_intent.is_object() && !current_->pending_intent.empty()
                     ? GoalPhase::Running
                     : GoalPhase::Idle;
    next.updated_at_ms = Now();
    return Commit(std::move(next), std::move(cause_ref));
}

GoalServiceResult GoalService::AddBudget(const GoalBudgetAddition& addition,
                                         std::uint64_t expected_state_revision,
                                         nlohmann::json cause_ref) {
    if (broken_) {
        return Fail(kErrGoalStoreUnavailable, "goal 写口已锁(此前写盘失败);fail closed");
    }
    if (writer_ == nullptr || options_.session_dir.empty()) {
        return Fail(kErrGoalStoreUnavailable, "GoalService 写口未接线");
    }
    if (!current_.has_value()) return Fail(kErrGoalNotFound, "没有 goal");
    if (IsLifecycleTerminal(current_->lifecycle)) {
        return Fail(kErrGoalTerminal, "目标已收账,预算不再受理");
    }
    if (addition.empty()) {
        return Fail(kErrGoalCandidateInvalid, "加预算至少给一项(iterations/tokens/elapsed)");
    }
    if (addition.iterations.has_value() && *addition.iterations < 0) {
        return Fail(kErrGoalCandidateInvalid, "iterations 增量须非负");
    }
    if (addition.total_tokens.has_value() && *addition.total_tokens < 0) {
        return Fail(kErrGoalCandidateInvalid, "tokens 增量须非负");
    }
    if (addition.elapsed_ms.has_value() && *addition.elapsed_ms < 0) {
        return Fail(kErrGoalCandidateInvalid, "elapsed 增量须非负");
    }
    if (expected_state_revision != current_->state_revision) {
        return Fail(kErrGoalRevisionConflict, "stateRevision 冲突:状态已被改过");
    }
    // 只抬帽不清账(§4.67.2 budget_exhausted 行):每字段取 max(旧帽,新增),
    // 旧费用保留;没到的字段不动。加完由调用方走显式恢复路径。
    GoalStateSnapshot next = *current_;
    next.state_revision += 1;
    if (addition.iterations.has_value()) {
        const std::int64_t raised = std::max<std::int64_t>(
            next.budget.max_iterations.value_or(0), *addition.iterations);
        next.budget.max_iterations = static_cast<int>(raised);
    }
    if (addition.total_tokens.has_value()) {
        next.budget.max_total_tokens =
            std::max<std::int64_t>(next.budget.max_total_tokens.value_or(0), *addition.total_tokens);
    }
    if (addition.elapsed_ms.has_value()) {
        next.budget.max_elapsed_ms =
            std::max<std::int64_t>(next.budget.max_elapsed_ms.value_or(0), *addition.elapsed_ms);
    }
    next.updated_at_ms = Now();
    GoalServiceResult r = Commit(std::move(next), std::move(cause_ref));
    if (r.ok) {
        if (current_->budget.max_iterations.has_value()) {
            r.payload["maxIterations"] = *current_->budget.max_iterations;
        }
        if (current_->budget.max_total_tokens.has_value()) {
            r.payload["maxTotalTokens"] = *current_->budget.max_total_tokens;
        }
        if (current_->budget.max_elapsed_ms.has_value()) {
            r.payload["maxElapsedMs"] = *current_->budget.max_elapsed_ms;
        }
    }
    return r;
}

std::string GoalService::BudgetStopReason(const GoalStateSnapshot& snapshot,
                                          std::int64_t next_tokens,
                                          bool counting_next_iteration) const {
    const GoalBudget& budget = snapshot.budget;
    // 轮数尺:开新一轮前问(counting_next_iteration),已完成收口的对账不问。
    if (counting_next_iteration && budget.max_iterations.has_value()) {
        const std::int64_t next_index = snapshot.counters.iterations_started + 1;
        if (next_index > *budget.max_iterations) {
            return "轮数帽 " + std::to_string(*budget.max_iterations) + " 轮已尽(已跑 " +
                   std::to_string(snapshot.counters.iterations_started) + " 轮)";
        }
    }
    // token 尺:实报 + 在途预留一起对帽;usage 未报时这把尺没账可对
    //(§4.67.7 不能拿 0 冒充没花),跳过不拦。
    if (budget.max_total_tokens.has_value() && snapshot.usage.usage_reported) {
        std::int64_t reserved = 0;
        for (const auto& reservation : reservations_) {
            reserved += reservation.tokens;
        }
        const std::int64_t used = snapshot.usage.input_tokens + snapshot.usage.output_tokens;
        if (used + reserved + next_tokens > *budget.max_total_tokens) {
            return "token 帽 " + std::to_string(*budget.max_total_tokens) + " 已尽(实报 " +
                   std::to_string(used) + " + 预留 " + std::to_string(reserved) + ")";
        }
    }
    // active 时长尺:activeElapsed 入快照,resume 不归零(§4.67.7)。
    if (budget.max_elapsed_ms.has_value() && snapshot.active_elapsed_ms > 0 &&
        snapshot.active_elapsed_ms >= *budget.max_elapsed_ms) {
        return "active 时长帽 " + std::to_string(*budget.max_elapsed_ms) + "ms 已尽(active " +
               std::to_string(snapshot.active_elapsed_ms) + "ms)";
    }
    return std::string();
}

GoalService::GoalBudgetView GoalService::EvaluateBudget(std::int64_t next_tokens) const {
    GoalBudgetView view;
    if (!current_.has_value()) return view;
    view.used_tokens =
        current_->usage.usage_reported ? current_->usage.input_tokens + current_->usage.output_tokens : 0;
    for (const auto& reservation : reservations_) {
        view.reserved_tokens += reservation.tokens;
    }
    if (!BudgetStopReason(*current_, 0, /*counting_next_iteration=*/false).empty()) {
        view.exhausted = true;
        view.reason = BudgetStopReason(*current_, 0, false);
        view.would_exhaust = true;
        return view;
    }
    const std::string next_reason = BudgetStopReason(*current_, next_tokens, false);
    if (!next_reason.empty()) {
        view.would_exhaust = true;
        view.reason = next_reason;
    }
    return view;
}

GoalServiceResult GoalService::ReserveBudget(std::string request_id, std::string owner,
                                             std::int64_t tokens) {
    if (!current_.has_value()) return Fail(kErrGoalNotFound, "没有 goal");
    if (request_id.empty()) {
        return Fail(kErrGoalCandidateInvalid, "预留 requestId 不能为空((sessionId,requestId) 去重键)");
    }
    if (tokens < 0) {
        return Fail(kErrGoalCandidateInvalid, "预留 tokens 须非负");
    }
    for (const auto& reservation : reservations_) {
        if (reservation.request_id == request_id) {
            return Fail(kErrGoalReservationRejected, "预留 requestId 重复: " + request_id);
        }
    }
    // 撞帽拒(并发子任务共用余额,不是各花整份):预留 + 实报一起对帽。
    if (const std::string reason = BudgetStopReason(*current_, tokens, false); !reason.empty()) {
        GoalServiceResult r = Fail(kErrGoalReservationRejected, "预留撞帽: " + reason);
        r.payload["reason_detail"] = reason;
        return r;
    }
    GoalReservation reservation;
    reservation.request_id = std::move(request_id);
    reservation.owner = std::move(owner);
    reservation.tokens = tokens;
    reservation.created_at_ms = Now();
    reservations_.push_back(std::move(reservation));
    GoalServiceResult r;
    r.ok = true;
    r.payload["reserved"] = tokens;
    return r;
}

GoalServiceResult GoalService::ReleaseBudgetReservation(const std::string& request_id) {
    for (auto it = reservations_.begin(); it != reservations_.end(); ++it) {
        if (it->request_id == request_id) {
            reservations_.erase(it);
            GoalServiceResult r;
            r.ok = true;
            return r;
        }
    }
    GoalServiceResult r;  // 未知 id:幂等成功(取消/收场路径不因账面缺项报错)
    r.ok = true;
    r.payload["idempotent"] = true;
    return r;
}

GoalServiceResult GoalService::RecordGoalUsage(const std::string& request_id,
                                               const std::string& source, const GoalUsage& usage,
                                               std::uint64_t expected_state_revision,
                                               nlohmann::json cause_ref) {
    if (broken_) {
        return Fail(kErrGoalStoreUnavailable, "goal 写口已锁(此前写盘失败);fail closed");
    }
    if (writer_ == nullptr || options_.session_dir.empty()) {
        return Fail(kErrGoalStoreUnavailable, "GoalService 写口未接线");
    }
    if (!current_.has_value()) return Fail(kErrGoalNotFound, "没有 goal");
    if (request_id.empty() || source.empty()) {
        return Fail(kErrGoalCandidateInvalid, "usage 归属缺 requestId/source");
    }
    // (sessionId, requestId) 计费去重:重复通知(同 key)第二次起幂等返回,
    // 不落事实行、不加账——一次结果只交付一次(§4.67.10)。
    for (const auto& recorded : recorded_request_ids_) {
        if (recorded == request_id) {
            GoalServiceResult r;
            r.ok = true;
            r.payload["deduped"] = true;
            return r;
        }
    }
    if (IsLifecycleTerminal(current_->lifecycle)) {
        return Fail(kErrGoalTerminal, "目标已收账,迟到 usage 只留调用方审计");
    }
    if (expected_state_revision != current_->state_revision) {
        return Fail(kErrGoalRevisionConflict, "stateRevision 冲突:状态已被改过");
    }
    {
        EventDraft recorded;
        recorded.kind = EventKindV3::GoalUsageRecorded;
        recorded.payload["goalId"] = current_->goal_id;
        recorded.payload["requestId"] = request_id;
        recorded.payload["source"] = source;
        recorded.payload["usage"] = UsageToJson(usage);
        const auto receipt = writer_->AppendEvent(std::move(recorded), Durability::ProcessCrash);
        if (receipt.status != trajectory::v3::WriteReceipt::Status::Committed) {
            return Fail(kErrGoalStoreUnavailable,
                        "goal.usage.recorded 落账失败(" + receipt.error_code + "): " +
                            receipt.error_message);
        }
    }
    recorded_request_ids_.push_back(request_id);
    ReleaseBudgetReservation(request_id);  // 实报到手:同名预留释放
    GoalStateSnapshot next = *current_;
    next.state_revision += 1;
    next.usage.Add(usage);
    next.updated_at_ms = Now();
    GoalServiceResult r = Commit(std::move(next), std::move(cause_ref));
    if (r.ok) {
        r.payload["requestId"] = request_id;
        r.payload["source"] = source;
    }
    return r;
}

GoalServiceResult GoalService::CreateForkedGoal(const GoalStateSnapshot& source,
                                                nlohmann::json cause_ref) {
    if (broken_) {
        return Fail(kErrGoalStoreUnavailable, "goal 写口已锁(此前写盘失败);fail closed");
    }
    if (writer_ == nullptr || options_.session_dir.empty()) {
        return Fail(kErrGoalStoreUnavailable, "GoalService 写口未接线");
    }
    if (source.goal_id.empty()) {
        return Fail(kErrGoalCandidateInvalid, "fork 源缺 goalId");
    }
    if (current_.has_value() && !IsLifecycleTerminal(current_->lifecycle)) {
        return Fail(kErrGoalAlreadyActive,
                    "本卷已有未收账 goal;fork 落在新 session 的服务上,不该撞号");
    }
    if (const std::string invalid = ValidateGoalObjective(source.objective); !invalid.empty()) {
        return Fail(kErrGoalCandidateInvalid, "fork 源 objective 不合合同: " + invalid);
    }
    GoalStateSnapshot fork;
    fork.goal_id = "goal-" + std::to_string(++next_goal_number_);
    fork.parent_goal_id = source.goal_id;  // fork lineage(§4.67.8 来路)
    fork.session_id = writer_->session_id();
    fork.run_id = writer_->run_id();
    fork.state_revision = 1;
    fork.contract_revision = source.contract_revision;  // 合同照抄,版本随源
    fork.objective = source.objective;
    fork.objective_sha256 = source.objective_sha256;
    fork.contract = source.contract;
    fork.contract_frozen = source.contract_frozen;
    fork.lifecycle = GoalLifecycle::Paused;  // 默认 paused:不让两支同追一目标
    fork.phase = GoalPhase::Idle;
    fork.stop_reason = "forked: 分支默认暂停,显式启动后才跑(§4.67.8)";
    fork.counters = source.counters;              // 进度来路
    fork.evidence_refs = source.evidence_refs;    // 继承证据……
    for (auto& ref : fork.evidence_refs) {
        ref.fresh = false;  // ……全标待复核(版本过了 forks 点,旧验不算数)
    }
    fork.budget = GoalBudget{};  // 原预算不带:帽清空(三尺不限),费用独立累计
    fork.usage = GoalUsage{};    // 原 usage 不带:只作来源展示(源账在源卷)
    fork.workspace_root = source.workspace_root;
    fork.workspace_identity = source.workspace_identity;
    fork.created_at_ms = Now();
    fork.updated_at_ms = fork.created_at_ms;
    return Commit(std::move(fork), std::move(cause_ref));
}

GoalServiceResult GoalService::Commit(GoalStateSnapshot next, const nlohmann::json& cause_ref) {
    // activeElapsed 累计(§4.67.7 三笔时间的 active 笔):上一版在 active
    // 态时,距上次提交的墙钟计入;暂停/等待/离线不占它,resume 不归零。
    if (current_.has_value() && current_->goal_id == next.goal_id &&
        current_->lifecycle == GoalLifecycle::Active) {
        const std::int64_t delta = Now() - current_->updated_at_ms;
        if (delta > 0) next.active_elapsed_ms = current_->active_elapsed_ms + delta;
    }
    // 转回 active = 显式恢复:停止意图清旗(§4.67.3"明确续跑后才恢复")。
    if (next.lifecycle == GoalLifecycle::Active &&
        (!current_ || current_->lifecycle != GoalLifecycle::Active)) {
        next.stop_requested = false;
    }
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
    if (adopted_carry_ && same_goal) {
        // 跨卷接管后的首次提交(G1):resume-as-new 的新卷上,这只 goal 的
        // 首条 applied fromStateRevision != 0——带 adoptedFrom 凭据,单一卷
        // 的 ProjectGoalState 凭它认"半路续接"的合法首条。
        draft.payload["adoptedFrom"] = nlohmann::json{
            {"sessionId", adopted_from_session_},
            {"stateRevision", current_->state_revision}};
    }
    const auto receipt = writer_->AppendEvent(std::move(draft), Durability::PowerLoss);
    if (receipt.status != trajectory::v3::WriteReceipt::Status::Committed) {
        // applied 未落:快照只是候选文件,不生效(不删,留审计);写口锁死。
        broken_ = true;
        return Fail(kErrGoalStoreUnavailable,
                    "state.goal.applied 落账失败(" + receipt.error_code + "): " +
                        receipt.error_message);
    }
    adopted_carry_ = false;
    adopted_from_session_.clear();
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
    // G3:计费去重底从账上喂(事实行已记过的 requestId 不再记);预留是
    // 内存账,跨进程不迁——预留不是实报,丢了不丢真账。
    recorded_request_ids_ = projection.usage_request_ids;
    reservations_.clear();
    // 跨卷接管凭据(G1):这只 goal 接下来在本写者卷上的首条 applied 带
    // adoptedFrom(来源卷 + 接管时 revision),单卷投影凭它认半路续接。
    adopted_carry_ = true;
    adopted_from_session_ = projection.session_id;
    GoalServiceResult r;
    r.ok = true;
    r.payload["goalId"] = current_->goal_id;
    r.payload["stateRevision"] = current_->state_revision;
    // 预算复核(§4.67.8"usage watermark"):事实行累计比快照多 = 有事实
    // 没赶上最后一笔提交——如实带出,补账/告警归调用方,不静默吞。
    if (projection.usage_recorded.usage_reported &&
        current_->usage.input_tokens + current_->usage.output_tokens <
            projection.usage_recorded.input_tokens + projection.usage_recorded.output_tokens) {
        r.payload["usageGap"] = true;
        r.payload["usageFactsTotalTokens"] =
            projection.usage_recorded.input_tokens + projection.usage_recorded.output_tokens;
    }
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
                // 同 goal:版本须衔接,lifecycle 变了才验转换(terminal 复活
                // 在这里露馅)。G1 起 stateRevision 也随 phase/意图/usage 类
                // 提交递增(claim/set/begin/end 不动 lifecycle),同态的
                // applied 是合法的状态提交,不是"没变化的假提交"。
                if (!applied.empty() &&
                    view.from_state_revision != applied.back().to_state_revision) {
                    bad("同 goal 的 applied revision 不衔接");
                    return out;
                }
                if (view.lifecycle != last_lifecycle &&
                    !IsValidLifecycleTransition(last_lifecycle, view.lifecycle)) {
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
                // 跨卷续接(G1):resume-as-new 后 goal 从来源卷接管,本卷首条
                // from = 接管时 revision(>0)。须带 adoptedFrom 凭据且
                // stateRevision 严丝合缝;没有凭据的半路首条按非法序列报缺口。
                if (!p.contains("adoptedFrom") || !p.at("adoptedFrom").is_object()) {
                    bad("首条 applied 的 fromStateRevision != 0 且缺 adoptedFrom(半路续接无凭据)");
                    return out;
                }
                const auto& adopted = p.at("adoptedFrom");
                if (!adopted.contains("stateRevision") ||
                    !adopted.at("stateRevision").is_number_integer() ||
                    static_cast<std::uint64_t>(adopted.at("stateRevision").get<std::int64_t>()) !=
                        view.from_state_revision) {
                    bad("adoptedFrom.stateRevision 须等于首条 fromStateRevision");
                    return out;
                }
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
    out.session_id = ledger.session_id;  // 这份投影来自哪一卷(跨卷接管凭据用)
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
    // G3:usage 事实账(goal.usage.recorded 逐条 (sessionId,requestId) 去重
    // 累计)——计费去重底与 resume 复核的输入。判词/验收请求的 usage 走
    // G2 的逐次请求账,不在此列(避免双计)。
    for (const auto& event : ledger.events) {
        if (event.kind != EventKindV3::GoalUsageRecorded) continue;
        const auto& p = event.payload;
        if (!p.contains("requestId") || !p.at("requestId").is_string()) continue;
        const std::string request_id = p.at("requestId").get<std::string>();
        if (request_id.empty()) continue;
        bool seen = false;
        for (const auto& recorded : out.usage_request_ids) {
            if (recorded == request_id) {
                seen = true;  // 账上重复行:留行,不重复累计(投影去重)
                break;
            }
        }
        if (seen) continue;
        out.usage_request_ids.push_back(request_id);
        if (p.contains("usage") && p.at("usage").is_object()) {
            out.usage_recorded.Add(UsageFromJson(p.at("usage")));
        }
    }
    out.snapshot = std::move(*snapshot);
    return out;
}

std::vector<GoalEvidence> EvidenceMaterialFromLedger(const trajectory::v3::V3Ledger& ledger,
                                                     const std::string& goal_id) {
    // §4.67 G3:resume 后证据判材料的账面回放。逐条 goal.evidence.recorded
    // 翻回 v1 采证形状(ref.kind -> EvidenceKind、facts 随行);同 id 后写
    // 覆盖前写(stale 翻旧以账上最后一笔为准)。
    std::map<std::string, GoalEvidence> material;
    for (const auto& event : ledger.events) {
        if (event.kind != EventKindV3::GoalEvidenceRecorded) continue;
        const auto& p = event.payload;
        if (!p.contains("goalId") || !p.at("goalId").is_string() ||
            p.at("goalId").get<std::string>() != goal_id) {
            continue;
        }
        if (!p.contains("evidenceId") || !p.at("evidenceId").is_string()) continue;
        if (!p.contains("evidence") || !p.at("evidence").is_object()) continue;
        std::string ref_error;
        const auto ref = GoalEvidenceRef::FromJson(p.at("evidence"), &ref_error);
        if (!ref.has_value()) continue;  // 坏行保守跳过,不放缺口也不假造
        GoalEvidence evidence;
        evidence.id = ref->id;
        if (!ParseEvidenceKind(ref->kind, evidence.kind)) continue;
        evidence.goal_id = goal_id;
        evidence.iteration_id =
            p.contains("iterationId") && p.at("iterationId").is_string()
                ? p.at("iterationId").get<std::string>()
                : std::string();
        evidence.tool_use_id = ref->source_ref;
        evidence.producer = "ledger";
        if (p.contains("facts") && p.at("facts").is_object()) {
            evidence.facts = p.at("facts");
        }
        evidence.content_sha256 = ref->content_sha256;
        evidence.observed_at_ms = ref->observed_at_ms;
        evidence.fresh = ref->fresh;
        evidence.truncated = ref->truncated;
        material[evidence.id] = std::move(evidence);
    }
    std::vector<GoalEvidence> out;
    out.reserve(material.size());
    for (auto& [id, evidence] : material) {
        (void)id;
        out.push_back(std::move(evidence));
    }
    return out;
}

GoalLineageProjection ProjectGoalLineage(const std::filesystem::path& current_session_dir) {
    // §4.67.8:goal 沿 session lineage 持久保存,resume 不清。从本场卷起
    // 逐卷投;本场没有 goal 账且本场是 resume 开的,才沿 previousSessionId
    // 向上找最近一份。clear/fork 开的新场不带旧 goal(§4.67.2 clear 才撤
    // goal;fork 另发 goalId)——链在这里断,不猜。目录名即 session id
    //(FindV3SessionStream 同一约定);深度护栏 32 跳、防 id 回环。
    GoalLineageProjection out;
    constexpr int kMaxHops = 32;
    std::vector<std::string> visited;
    std::filesystem::path dir = current_session_dir;
    for (int hop = 0; hop < kMaxHops; ++hop) {
        std::error_code ec;
        if (!std::filesystem::is_directory(dir, ec)) {
            out.detail = "来源场目录不在: " + platform::PathToUtf8(dir);
            break;
        }
        const std::string session_id = platform::PathToUtf8(dir.filename());
        if (session_id.empty()) {
            out.detail = "来源场目录没有名字,链停";
            break;
        }
        for (const auto& seen : visited) {
            if (seen == session_id) {
                out.detail = "来源链回环(" + session_id + "),链停";
                out.projection.gap = GoalProjectionGap::NoGoal;
                return out;
            }
        }
        visited.push_back(session_id);
        if (const auto stream = trajectory::v3::FindV3SessionStream(dir); stream.has_value()) {
            const auto ledger = trajectory::v3::ReadV3Ledger(*stream);
            if (!ledger.has_value()) {
                // 验卷不过:卷坏如实报——把错误装进 gap_detail,不猜。
                out.found = true;
                out.projection.gap = GoalProjectionGap::IllegalTransition;
                out.projection.gap_detail = "卷验不过(" + ledger.error() + ")";
                out.walked = std::move(visited);
                out.detail = "在 " + session_id + " 验卷失败";
                return out;
            }
            GoalProjection projection = ProjectGoalState(*ledger, dir);
            if (projection.gap != GoalProjectionGap::NoGoal) {
                // 有 goal 账(含缺口)即止:这是最近一份,缺口如实上报。
                // 顺带回放证据判材料(§4.67 G3):resume 后内存证据从这本
                // 卷的 goal.evidence.recorded 补齐(缺材料只会让验收更保守)。
                out.found = true;
                out.projection = std::move(projection);
                if (out.projection.gap == GoalProjectionGap::None && out.projection.has_goal) {
                    out.evidence_material =
                        EvidenceMaterialFromLedger(*ledger, out.projection.goal_id);
                }
                out.walked = std::move(visited);
                out.detail = "head 在卷 " + session_id;
                return out;
            }
        }
        // 本卷没有 goal 账:只有 resume 开的场才向上穿(clear/fork 断链)。
        const auto manifest = trajectory::ReadSessionJson(dir);
        if (!manifest.has_value()) {
            out.detail = "session.json 读不动(" + session_id + "),链停";
            break;
        }
        if (manifest->start_reason != "resume" || !manifest->previous_session_id.has_value() ||
            manifest->previous_session_id->empty()) {
            out.detail = "链在 " + session_id + "(start_reason=" + manifest->start_reason + ")止";
            break;
        }
        dir = dir.parent_path() /
              platform::Utf8ToPath(*manifest->previous_session_id);
    }
    if (out.detail.empty()) out.detail = "来源链超过 " + std::to_string(kMaxHops) + " 跳,护栏止";
    // 走到头也没撞见 goal 账:缺口如实报 NoGoal(与单卷空账同一口径),
    // 不留默认 None 冒充"投影健康"。
    if (!out.found) out.projection.gap = GoalProjectionGap::NoGoal;
    out.walked = std::move(visited);
    return out;
}

}  // namespace lubancode::runtime::goal
