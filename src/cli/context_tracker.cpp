#include "cli/context_tracker.hpp"

#include <cstdint>

namespace lubancode::cli {

ContextTracker::ContextTracker(std::size_t window_tokens) : window_tokens_(window_tokens) {}

void ContextTracker::Update(const api::Usage& usage) {
    // Anthropic 语义:usage.input_tokens 不含缓存命中/缓存写入那部分——
    // 真实发出去的提示词体积得把 cache_read/cache_creation 一起加回来,
    // 不然压缩过一轮、后续请求大量命中缓存时,input_tokens 会很小,
    // 算出来的占用远低于真实值(实测出现过 input=144、cache_read=1472,
    // 按旧公式只算 144,报出 context 0%)。
    const std::int64_t total =
        usage.input_tokens + usage.cache_read_tokens + usage.cache_creation_tokens + usage.output_tokens;
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
