#include "cli/context_tracker.hpp"

#include <cstdint>

namespace lubancode::cli {

ContextTracker::ContextTracker(std::size_t window_tokens) : window_tokens_(window_tokens) {}

void ContextTracker::Update(const api::Usage& usage) {
    const std::int64_t total = usage.input_tokens + usage.output_tokens;
    current_tokens_ = total > 0 ? static_cast<std::size_t>(total) : 0;
}

int ContextTracker::UsagePercent() const {
    if (window_tokens_ == 0) {
        return 0;
    }
    const double ratio = static_cast<double>(current_tokens_) / static_cast<double>(window_tokens_) * 100.0;
    return static_cast<int>(ratio + 0.5);
}

bool ContextTracker::ShouldAutoCompact() const {
    if (window_tokens_ == 0) {
        return false;
    }
    return static_cast<double>(current_tokens_) >=
           static_cast<double>(window_tokens_) * (static_cast<double>(kAutoCompactThresholdPercent) / 100.0);
}

}  // namespace lubancode::cli
