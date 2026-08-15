#include "cli/context_tracker.hpp"

#include <cstdint>

namespace lubancode::cli {

ContextTracker::ContextTracker(std::size_t window_tokens) : window_tokens_(window_tokens) {}

void ContextTracker::Update(const api::Usage& usage) {
    // 统一口径(api::Usage 文件头):input_tokens 已是"非缓存输入",
    // 完整提示词体积 = TotalInputTokens(input + cache_read + cache_creation),
    // 再加输出。三家 wire 摊成同一副语义后,这一只公式对所有家都对——
    // 旧实现里 Chat/Responses 把含 cached 的总数塞进 input_tokens,这里
    // 再加一遍 cache_read,会把占用算重(前缀缓存守恒单第一期修掉)。
    const std::int64_t total = api::TotalInputTokens(usage) + usage.output_tokens;
    current_tokens_ = total > 0 ? static_cast<std::size_t>(total) : 0;
    // 缓存命中量与完整输入同样覆盖式记一份,/context 分类明细与命中率用;
    // 负数(不该出现)按 0。
    last_cache_read_tokens_ = usage.cache_read_tokens > 0 ? usage.cache_read_tokens : 0;
    last_input_tokens_ = api::TotalInputTokens(usage);
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
