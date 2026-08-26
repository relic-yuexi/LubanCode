// 预算闸实现(骨架拆解批五先行半批)。纯函数,无状态。

#include "runtime/budget_gate.hpp"

namespace lubancode::runtime {

namespace {

// Headroom:used >= limit 即拦("下一轮装不下")。
bool HeadroomTripped(const std::optional<std::int64_t>& limit, std::optional<std::int64_t> used) {
    if (!limit.has_value() || !used.has_value()) {
        return false;  // 尺没设,或这把尺没账可对
    }
    return *used >= *limit;
}

// Overrun:used > limit 才拦("已经越帽")。
bool OverrunTripped(const std::optional<std::int64_t>& limit, std::optional<std::int64_t> used) {
    if (!limit.has_value() || !used.has_value()) {
        return false;
    }
    return *used > *limit;
}

}  // namespace

BudgetStopReason BudgetGate::CheckHeadroom(std::optional<std::int64_t> count_used,
                                           std::optional<std::int64_t> elapsed_used_ms,
                                           std::optional<std::int64_t> tokens_used) const {
    if (HeadroomTripped(scales_.count, count_used)) return BudgetStopReason::kCount;
    if (HeadroomTripped(scales_.elapsed_ms, elapsed_used_ms)) return BudgetStopReason::kElapsed;
    if (HeadroomTripped(scales_.tokens, tokens_used)) return BudgetStopReason::kTokens;
    return BudgetStopReason::kNone;
}

BudgetStopReason BudgetGate::CheckOverrun(std::optional<std::int64_t> count_used,
                                          std::optional<std::int64_t> elapsed_used_ms,
                                          std::optional<std::int64_t> tokens_used) const {
    if (OverrunTripped(scales_.count, count_used)) return BudgetStopReason::kCount;
    if (OverrunTripped(scales_.elapsed_ms, elapsed_used_ms)) return BudgetStopReason::kElapsed;
    if (OverrunTripped(scales_.tokens, tokens_used)) return BudgetStopReason::kTokens;
    return BudgetStopReason::kNone;
}

bool BudgetGate::HeadroomCount(std::int64_t used) const {
    return HeadroomTripped(scales_.count, used);
}

bool BudgetGate::HeadroomElapsed(std::int64_t used_ms) const {
    return HeadroomTripped(scales_.elapsed_ms, used_ms);
}

bool BudgetGate::HeadroomTokens(std::int64_t used) const {
    return HeadroomTripped(scales_.tokens, used);
}

bool BudgetGate::OverrunCount(std::int64_t used) const {
    return OverrunTripped(scales_.count, used);
}

bool BudgetGate::OverrunElapsed(std::int64_t used_ms) const {
    return OverrunTripped(scales_.elapsed_ms, used_ms);
}

bool BudgetGate::OverrunTokens(std::int64_t used) const {
    return OverrunTripped(scales_.tokens, used);
}

}  // namespace lubancode::runtime
