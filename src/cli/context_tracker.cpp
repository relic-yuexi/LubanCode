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
    // 缓存命中量同样覆盖式记一份,/context 分类明细用;负数(不该出现)按 0。
    last_cache_read_tokens_ = usage.cache_read_tokens > 0 ? usage.cache_read_tokens : 0;
}

void ContextTracker::ApplyUsage(const api::Usage& usage) {
    // 四项全零 = provider 没在流末给 usage(见头文件注释):不清零、不
    // 覆盖,现有数字原样保住,只标旧值。
    const bool measured = usage.input_tokens > 0 || usage.output_tokens > 0 ||
                          usage.cache_read_tokens > 0 || usage.cache_creation_tokens > 0;
    if (measured) {
        Update(usage);
        usage_stale_ = false;
        return;
    }
    usage_stale_ = true;
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
