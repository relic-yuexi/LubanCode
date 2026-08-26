// `/loop` 会话定时循环(loop 单第 0 期):领域类型实现。
// 纯函数 + to/from_json,不碰磁盘、不认终端。

#include "runtime/loop_types.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <limits>

#include "runtime/budget_gate.hpp"

namespace lubancode::runtime::loop {

namespace {

// json 里 interval 落毫秒(int64),内存里是 seconds。
constexpr std::int64_t kMsPerSec = 1000;

std::chrono::seconds SecondsFromMs(std::int64_t ms) {
    return std::chrono::seconds(static_cast<std::int64_t>(ms / kMsPerSec));
}

std::int64_t MsFromSeconds(std::chrono::seconds s) {
    return static_cast<std::int64_t>(s.count()) * kMsPerSec;
}

}  // namespace

// ---------------------------------------------------------------------------
// 枚举 <-> 稳定字符串
// ---------------------------------------------------------------------------

std::string ToString(LoopTaskState state) {
    switch (state) {
        case LoopTaskState::Active: return "active";
        case LoopTaskState::Paused: return "paused";
        case LoopTaskState::Due: return "due";
        case LoopTaskState::Running: return "running";
        case LoopTaskState::WaitingPermission: return "waiting_permission";
        case LoopTaskState::BackingOff: return "backing_off";
        case LoopTaskState::Completed: return "completed";
        case LoopTaskState::Cancelled: return "cancelled";
        case LoopTaskState::Expired: return "expired";
        case LoopTaskState::Broken: return "broken";
    }
    return "active";
}

bool ParseLoopTaskState(const std::string& s, LoopTaskState& out) {
    if (s == "active") { out = LoopTaskState::Active; return true; }
    if (s == "paused") { out = LoopTaskState::Paused; return true; }
    if (s == "due") { out = LoopTaskState::Due; return true; }
    if (s == "running") { out = LoopTaskState::Running; return true; }
    if (s == "waiting_permission") { out = LoopTaskState::WaitingPermission; return true; }
    if (s == "backing_off") { out = LoopTaskState::BackingOff; return true; }
    if (s == "completed") { out = LoopTaskState::Completed; return true; }
    if (s == "cancelled") { out = LoopTaskState::Cancelled; return true; }
    if (s == "expired") { out = LoopTaskState::Expired; return true; }
    if (s == "broken") { out = LoopTaskState::Broken; return true; }
    return false;
}

bool IsLoopTerminal(LoopTaskState state) {
    switch (state) {
        case LoopTaskState::Completed:
        case LoopTaskState::Cancelled:
        case LoopTaskState::Expired:
        case LoopTaskState::Broken:
            return true;
        default:
            return false;
    }
}

bool IsLoopResumable(LoopTaskState state) {
    // 只收 Paused:Due/Running/Waiting/BackingOff 是运行中间态,恢复时
    // 走 Interrupted 的专门账,不落在"resume"这只口。
    return state == LoopTaskState::Paused;
}

bool IsLoopConsumable(LoopTaskState state) {
    return state == LoopTaskState::Active || state == LoopTaskState::Due;
}

std::string ToString(MissedTickPolicy) { return "coalesce_latest"; }
bool ParseMissedTickPolicy(const std::string& s, MissedTickPolicy& out) {
    if (s == "coalesce_latest") { out = MissedTickPolicy::CoalesceLatest; return true; }
    return false;
}

std::string ToString(OverlapPolicy) { return "single_flight"; }
bool ParseOverlapPolicy(const std::string& s, OverlapPolicy& out) {
    if (s == "single_flight") { out = OverlapPolicy::SingleFlight; return true; }
    return false;
}

std::string ToString(LoopPromptSource source) {
    switch (source) {
        case LoopPromptSource::Inline: return "inline";
        case LoopPromptSource::ProjectFile: return "project_file";
        case LoopPromptSource::UserFile: return "user_file";
        case LoopPromptSource::Builtin: return "builtin";
    }
    return "inline";
}

bool ParseLoopPromptSource(const std::string& s, LoopPromptSource& out) {
    if (s == "inline") { out = LoopPromptSource::Inline; return true; }
    if (s == "project_file") { out = LoopPromptSource::ProjectFile; return true; }
    if (s == "user_file") { out = LoopPromptSource::UserFile; return true; }
    if (s == "builtin") { out = LoopPromptSource::Builtin; return true; }
    return false;
}

std::string ToString(LoopTickOutcome outcome) {
    switch (outcome) {
        case LoopTickOutcome::Pending: return "pending";
        case LoopTickOutcome::Succeeded: return "succeeded";
        case LoopTickOutcome::Declined: return "declined";
        case LoopTickOutcome::Cancelled: return "cancelled";
        case LoopTickOutcome::ProviderError: return "provider_error";
        case LoopTickOutcome::RateLimited: return "rate_limited";
        case LoopTickOutcome::PermissionWaitTimeout: return "permission_wait_timeout";
        case LoopTickOutcome::PromptSourceMissing: return "prompt_source_missing";
        case LoopTickOutcome::SessionClosed: return "session_closed";
        case LoopTickOutcome::Expired: return "expired";
        case LoopTickOutcome::Corrupt: return "corrupt";
        case LoopTickOutcome::UnknownAfterStart: return "unknown_after_start";
    }
    return "pending";
}

bool ParseLoopTickOutcome(const std::string& s, LoopTickOutcome& out) {
    if (s == "pending") { out = LoopTickOutcome::Pending; return true; }
    if (s == "succeeded") { out = LoopTickOutcome::Succeeded; return true; }
    if (s == "declined") { out = LoopTickOutcome::Declined; return true; }
    if (s == "cancelled") { out = LoopTickOutcome::Cancelled; return true; }
    if (s == "provider_error") { out = LoopTickOutcome::ProviderError; return true; }
    if (s == "rate_limited") { out = LoopTickOutcome::RateLimited; return true; }
    if (s == "permission_wait_timeout") { out = LoopTickOutcome::PermissionWaitTimeout; return true; }
    if (s == "prompt_source_missing") { out = LoopTickOutcome::PromptSourceMissing; return true; }
    if (s == "session_closed") { out = LoopTickOutcome::SessionClosed; return true; }
    if (s == "expired") { out = LoopTickOutcome::Expired; return true; }
    if (s == "corrupt") { out = LoopTickOutcome::Corrupt; return true; }
    if (s == "unknown_after_start") { out = LoopTickOutcome::UnknownAfterStart; return true; }
    return false;
}

// ---------------------------------------------------------------------------
// LoopTask / LoopTick 的 json 往返
// ---------------------------------------------------------------------------

nlohmann::json LoopTask::to_json() const {
    nlohmann::json j;
    j["task_id"] = task_id;
    j["session_id"] = session_id;
    j["prompt"] = prompt;
    j["prompt_source"] = ToString(prompt_source);
    if (!prompt_file.empty()) {
        j["prompt_file"] = prompt_file;
    }
    j["prompt_sha256"] = prompt_sha256;
    j["interval_ms"] = MsFromSeconds(interval);
    j["created_at_ms"] = created_at_ms;
    j["expires_at_ms"] = expires_at_ms;
    j["next_due_at_ms"] = next_due_at_ms;
    j["state"] = ToString(state);
    j["tick_seq"] = tick_seq;
    j["run_count"] = run_count;
    j["skipped_count"] = skipped_count;
    j["consecutive_failures"] = consecutive_failures;
    j["consecutive_denials"] = consecutive_denials;
    if (active_turn_id.has_value()) {
        j["active_turn_id"] = *active_turn_id;
    }
    if (!cwd_identity.empty()) {
        j["cwd_identity"] = cwd_identity;
    }
    j["creation_seq"] = creation_seq;
    return j;
}

LoopTask LoopTask::from_json(const nlohmann::json& j) {
    LoopTask t;
    t.task_id = j.at("task_id").get<std::string>();
    t.session_id = j.value("session_id", std::string());
    t.prompt = j.value("prompt", std::string());
    if (!ParseLoopPromptSource(j.value("prompt_source", "inline"), t.prompt_source)) {
        t.prompt_source = LoopPromptSource::Inline;
    }
    t.prompt_file = j.value("prompt_file", std::string());
    t.prompt_sha256 = j.value("prompt_sha256", std::string());
    t.interval = SecondsFromMs(j.value("interval_ms", static_cast<std::int64_t>(600000)));
    t.created_at_ms = j.value("created_at_ms", static_cast<WallTimeMs>(0));
    t.expires_at_ms = j.value("expires_at_ms", static_cast<WallTimeMs>(0));
    t.next_due_at_ms = j.value("next_due_at_ms", static_cast<WallTimeMs>(0));
    if (!ParseLoopTaskState(j.value("state", "active"), t.state)) {
        t.state = LoopTaskState::Active;
    }
    t.tick_seq = j.value("tick_seq", static_cast<std::uint64_t>(0));
    t.run_count = j.value("run_count", static_cast<std::uint64_t>(0));
    t.skipped_count = j.value("skipped_count", static_cast<std::uint64_t>(0));
    t.consecutive_failures = j.value("consecutive_failures", static_cast<std::uint64_t>(0));
    t.consecutive_denials = j.value("consecutive_denials", static_cast<std::uint64_t>(0));
    if (j.contains("active_turn_id")) {
        t.active_turn_id = j.at("active_turn_id").get<std::string>();
    }
    t.cwd_identity = j.value("cwd_identity", std::string());
    t.creation_seq = j.value("creation_seq", static_cast<std::uint64_t>(0));
    return t;
}

nlohmann::json LoopTick::to_json() const {
    nlohmann::json j;
    j["task_id"] = task_id;
    j["tick_id"] = tick_id;
    j["tick_no"] = tick_no;
    j["scheduled_at_ms"] = scheduled_at_ms;
    j["dispatched_at_ms"] = dispatched_at_ms;
    j["finished_at_ms"] = finished_at_ms;
    j["prompt_sha256"] = prompt_sha256;
    j["source"] = ToString(source);
    if (!turn_id.empty()) {
        j["turn_id"] = turn_id;
    }
    j["outcome"] = ToString(outcome);
    if (!error_code.empty()) {
        j["error_code"] = error_code;
    }
    j["next_due_at_ms"] = next_due_at_ms;
    j["missed_count"] = missed_count;
    j["attempts"] = attempts;
    return j;
}

LoopTick LoopTick::from_json(const nlohmann::json& j) {
    LoopTick t;
    t.task_id = j.at("task_id").get<std::string>();
    t.tick_id = j.value("tick_id", std::string());
    t.tick_no = j.value("tick_no", static_cast<std::uint64_t>(0));
    t.scheduled_at_ms = j.value("scheduled_at_ms", static_cast<WallTimeMs>(0));
    t.dispatched_at_ms = j.value("dispatched_at_ms", static_cast<WallTimeMs>(0));
    t.finished_at_ms = j.value("finished_at_ms", static_cast<WallTimeMs>(0));
    t.prompt_sha256 = j.value("prompt_sha256", std::string());
    if (!ParseLoopPromptSource(j.value("source", "inline"), t.source)) {
        t.source = LoopPromptSource::Inline;
    }
    t.turn_id = j.value("turn_id", std::string());
    if (!ParseLoopTickOutcome(j.value("outcome", "pending"), t.outcome)) {
        t.outcome = LoopTickOutcome::Pending;
    }
    t.error_code = j.value("error_code", std::string());
    t.next_due_at_ms = j.value("next_due_at_ms", static_cast<WallTimeMs>(0));
    t.missed_count = j.value("missed_count", static_cast<std::uint32_t>(0));
    t.attempts = j.value("attempts", static_cast<std::uint32_t>(1));
    return t;
}

// ---------------------------------------------------------------------------
// 状态机转换表
// ---------------------------------------------------------------------------

std::vector<std::string> LoopTransitionEvents() {
    return {"tick_due", "tick_started", "tick_finished", "permission_wait",
            "permission_resolved", "pause", "resume", "stop", "complete",
            "expire", "break", "backoff", "backoff_done"};
}

bool IsLoopTransitionAllowed(LoopTaskState from, const std::string& event, LoopTaskState to) {
    using S = LoopTaskState;
    // terminal 不自动复活(任何 event 都不许从 terminal 出去)。
    if (IsLoopTerminal(from) && from != to) {
        return false;
    }
    if (IsLoopTerminal(from) && from == to) {
        return event == "tick_finished" || event == "stop" || event == "expire";
        // 同态回写只在收口补账的场合;别的 event 不许原地踏步。
    }
    if (event == "tick_due") {
        // Active -> Due:到点了。别的态到不了 Due(single-flight:Running
        // 时到点只记 coalesced,不换态)。
        return from == S::Active && to == S::Due;
    }
    if (event == "tick_started") {
        // Due -> Running:主泵取走,开 turn。
        return from == S::Due && to == S::Running;
    }
    if (event == "tick_finished") {
        // Running/WaitingPermission/BackingOff -> Active(下一拍照排)。
        return (from == S::Running || from == S::WaitingPermission || from == S::BackingOff) &&
               to == S::Active;
    }
    if (event == "permission_wait") {
        // Running -> WaitingPermission:悬在审批上。
        return from == S::Running && to == S::WaitingPermission;
    }
    if (event == "permission_resolved") {
        // WaitingPermission -> Running:答了,继续跑本拍。
        return from == S::WaitingPermission && to == S::Running;
    }
    if (event == "pause") {
        // 任意非终态 -> Paused(用户命令;幂等:Paused -> Paused 也收)。
        return to == S::Paused && (from == S::Paused || !IsLoopTerminal(from));
    }
    if (event == "resume") {
        // Paused -> Active:从 now + interval 重排。
        return from == S::Paused && to == S::Active;
    }
    if (event == "stop") {
        // 任意非终态 -> Cancelled(用户 stop)。
        return to == S::Cancelled && !IsLoopTerminal(from);
    }
    if (event == "complete") {
        // 模型经 loop_control 声明完成:Due/Running -> Completed。
        return to == S::Completed && (from == S::Due || from == S::Running ||
                                      from == S::WaitingPermission);
    }
    if (event == "expire") {
        // 任意非终态 -> Expired(seven-day expiry)。
        return to == S::Expired && !IsLoopTerminal(from);
    }
    if (event == "break") {
        // prompt 源没了/恢复时命令不存在:任意非终态 -> Broken。
        return to == S::Broken && !IsLoopTerminal(from);
    }
    if (event == "backoff") {
        // Running -> BackingOff:rate limit 带 retry-after / provider 连败。
        return from == S::Running && to == S::BackingOff;
    }
    if (event == "backoff_done") {
        // BackingOff -> Active:退避期满,回正常 cadence。
        return from == S::BackingOff && to == S::Active;
    }
    return false;
}

// ---------------------------------------------------------------------------
// interval 解析
// ---------------------------------------------------------------------------

namespace {
// (数字段的判定已折进 LooksLikeLoopInterval 的逐位扫;这里不留死函数。)
}  // namespace

bool LooksLikeLoopInterval(const std::string& token) {
    if (token.size() < 2) {
        return false;
    }
    // 数字段 + 单位字符,单位后必须到头("5migrate" 单位后还有字,不是)。
    std::size_t i = 0;
    while (i < token.size() && token[i] >= '0' && token[i] <= '9') {
        ++i;
    }
    if (i == 0 || i + 1 != token.size()) {
        return false;
    }
    const char unit = static_cast<char>(std::tolower(static_cast<unsigned char>(token[i])));
    return unit == 'm' || unit == 'h' || unit == 'd';
}

std::optional<std::chrono::seconds> ParseLoopInterval(const std::string& text) {
    if (!LooksLikeLoopInterval(text)) {
        return std::nullopt;
    }
    const std::string digits = text.substr(0, text.size() - 1);
    if (digits.size() > 9) {
        return std::nullopt;  // 溢出前先拒(9 位数字乘上限必超 int64 不了,但别赌)
    }
    const long long value = std::strtoll(digits.c_str(), nullptr, 10);
    if (value <= 0) {
        return std::nullopt;  // 0/负数(负号在 LooksLike 就过不去,这里是双保险)
    }
    const char unit = static_cast<char>(std::tolower(static_cast<unsigned char>(text.back())));
    long long seconds = value;
    long long multiplier = 1;
    if (unit == 'm') {
        multiplier = 60;
    } else if (unit == 'h') {
        multiplier = 3600;
    } else {  // 'd'
        multiplier = 86400;
    }
    // checked multiplication:溢出直接拒。
    if (value > (std::numeric_limits<long long>::max)() / multiplier) {
        return std::nullopt;
    }
    seconds = value * multiplier;
    // 绝对上限 7d(单子:计算统一转 seconds、先做 checked multiplication;
    // "绝对上限七天")。
    if (seconds > LoopDefaults::kMaximumInterval.count()) {
        return std::nullopt;
    }
    return std::chrono::seconds(seconds);
}

// ---------------------------------------------------------------------------
// 下一拍时间轴
// ---------------------------------------------------------------------------

DueComputation ComputeDueAdvance(WallTimeMs next_due_at_ms, WallTimeMs now_ms,
                                 std::chrono::seconds interval) {
    DueComputation out;
    if (now_ms < next_due_at_ms || interval.count() <= 0) {
        out.next_due_at_ms = next_due_at_ms;
        return out;
    }
    const std::int64_t interval_ms = MsFromSeconds(interval);
    const std::int64_t over_ms = now_ms - next_due_at_ms;
    // missed = floor(over / interval):跳过去的完整刻度数。
    const std::int64_t missed = over_ms / interval_ms;
    out.missed = static_cast<std::uint32_t>(missed);
    // 新 next_due = 原 next_due + (missed+1)*interval:原时间轴上第一个
    // future slot(now 恰在刻度上时,missed=0、新 slot = next_due+interval,
    // 即"到点那拍跑完后排的下一格")。
    out.next_due_at_ms = next_due_at_ms + (missed + 1) * interval_ms;
    // 防御:时钟向后跳得离谱时算出 still-past 的 slot,再推一 格。
    while (out.next_due_at_ms <= now_ms) {
        out.next_due_at_ms += interval_ms;
    }
    return out;
}

WallTimeMs ComputeResumeNextDue(WallTimeMs now_ms, std::chrono::seconds interval) {
    return now_ms + MsFromSeconds(interval);
}

bool LoopExpired(WallTimeMs expires_at_ms, WallTimeMs now_ms) {
    // 边界恰等于 now 也算过("expiry 边界恰等于 now"是测试矩阵钉的)。
    // 批五:时长尺走公共预算闸的 Headroom 口径(now >= deadline 即拦)。
    // deadline 是绝对墙钟点,当"从 epoch 起的 elapsed"用——机制同型,
    // 数值口径不变。
    return runtime::BudgetGate(runtime::BudgetScales{
                                   .elapsed_ms = expires_at_ms,
                               })
        .HeadroomElapsed(now_ms);
}

}  // namespace lubancode::runtime::loop
