// types.hpp 的实现:轻合同配套的名字/解析/稳定码函数。SV-09 拆分前住
// project_memory.cpp,搬来一字未动。

#include "memory/types.hpp"

#include "memory/internal.hpp"  // LowerAscii

namespace lubancode::memory {

namespace {

bool LooksLikeDateOrIsoTime(const std::string& raw) {
    // 宽松校验:YYYY-MM-DD 起头,可带 THH:MM:SSZ。字典序即时间序,召回
    // 侧只做字符串比较。
    if (raw.size() < 10) return false;
    for (std::size_t i = 0; i < 10; ++i) {
        const char c = raw[i];
        const bool digit = c >= '0' && c <= '9';
        const bool dash = c == '-' && (i == 4 || i == 7);
        if (!digit && !dash) return false;
    }
    for (std::size_t i = 10; i < raw.size(); ++i) {
        const char c = raw[i];
        if (!((c >= '0' && c <= '9') || c == ':' || c == 'T' || c == 'Z' || c == '+' || c == '-')) {
            return false;
        }
    }
    return true;
}

}  // namespace

bool LooksLikeMemoryDate(const std::string& raw) { return LooksLikeDateOrIsoTime(raw); }

std::string MemoryKindName(MemoryKind kind) {
    switch (kind) {
        case MemoryKind::Fact: return "fact";
        case MemoryKind::Preference: return "preference";
        case MemoryKind::Feedback: return "feedback";
    }
    return "fact";
}

std::expected<MemoryKind, std::string> ParseMemoryKind(const std::string& raw) {
    const std::string lower = LowerAscii(raw);
    if (lower == "fact") return MemoryKind::Fact;
    if (lower == "preference") return MemoryKind::Preference;
    if (lower == "feedback") return MemoryKind::Feedback;
    return std::unexpected("memory kind 只认 fact、preference 或 feedback");
}

std::string QueryOriginName(QueryOrigin origin) {
    switch (origin) {
        case QueryOrigin::User: return "user";
        case QueryOrigin::BackgroundCompletion: return "background_completion";
        case QueryOrigin::Hook: return "hook";
        case QueryOrigin::Compact: return "compact";
        case QueryOrigin::System: return "system";
    }
    return "user";
}

std::string LearnModeName(LearnMode mode) {
    switch (mode) {
        case LearnMode::Off: return "off";
        case LearnMode::Review: return "review";
        case LearnMode::Auto: return "auto";
    }
    return "off";
}

std::expected<LearnMode, std::string> ParseLearnMode(const std::string& raw) {
    const std::string lower = LowerAscii(raw);
    if (lower == "off") return LearnMode::Off;
    if (lower == "review") return LearnMode::Review;
    if (lower == "auto") return LearnMode::Auto;
    return std::unexpected("learn 档位只认 off、review 或 auto");
}

std::string MemoryWriteSourceName(MemoryWriteSource source) {
    switch (source) {
        case MemoryWriteSource::ExplicitCommandSave: return "explicit_command_save";
        case MemoryWriteSource::ModelToolSave: return "model_tool_save";
        case MemoryWriteSource::ExplicitForget: return "explicit_forget";
        case MemoryWriteSource::CandidateAccept: return "candidate_accept";
        case MemoryWriteSource::AutoExtraction: return "auto_extraction";
    }
    return "model_tool_save";
}

std::string MemoryWriteReceiptOutcomeName(MemoryWriteReceiptOutcome outcome) {
    switch (outcome) {
        case MemoryWriteReceiptOutcome::Queued: return "queued";
        case MemoryWriteReceiptOutcome::Rejected: return "rejected";
    }
    return "rejected";
}

std::string MemoryWorkerLaunchStateName(MemoryWorkerLaunchState state) {
    switch (state) {
        case MemoryWorkerLaunchState::Started: return "started";
        case MemoryWorkerLaunchState::AlreadyRunning: return "already_running";
        case MemoryWorkerLaunchState::StartFailed: return "start_failed";
        case MemoryWorkerLaunchState::Unavailable: return "unavailable";
        case MemoryWorkerLaunchState::Idle: return "idle";
    }
    return "unknown";
}

std::string StableWriteErrorCode(const std::string& error) {
    // 前缀映射到 EnqueueXxx 系的固定文案(编译期字面量,跨平台一致);
    // 文案改动只会降级成 other,不会错归因。
    if (error.starts_with("本场记忆写入未开启")) return "write_disabled";
    if (error.starts_with("本场记忆学习未开启")) return "write_disabled";
    if (error.starts_with("本场记忆未开启")) return "memory_disabled";
    if (error.starts_with("memory.global_unauthorized")) return "global_unauthorized";
    if (error.starts_with("用户级记忆未在全局配置授权")) return "user_layer_unauthorized";
    if (error.starts_with("找不到候选")) return "candidate_not_found";
    if (error.starts_with("候选置信度是 inferred")) return "candidate_inferred";
    if (error.starts_with("feedback 候选只收")) return "candidate_feedback_not_user_stated";
    if (error.starts_with("fact 候选缺证据路径")) return "candidate_fact_no_evidence";
    if (error.starts_with("记忆 id 不合法")) return "invalid_id";
    if (error.starts_with("记忆标题") || error.starts_with("记忆正文") || error.starts_with("记忆摘要") ||
        error.starts_with("记忆 paths") || error.starts_with("记忆元数据") ||
        error.starts_with("keywords") || error.starts_with("evidence") ||
        error.starts_with("confidence ") || error.starts_with("confidence 只认") ||
        error.starts_with("scope.") || error.starts_with("scope=") || error.starts_with("用户级记忆")) {
        return "invalid_request";
    }
    if (error.starts_with("feedback 只收用户明说的纠正")) return "feedback_requires_user_stated";
    return "other";
}

}  // namespace lubancode::memory
