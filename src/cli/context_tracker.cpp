#include "cli/context_tracker.hpp"

#include <cstdint>

namespace lubancode::cli {

ContextTracker::ContextTracker(std::size_t window_tokens) : window_tokens_(window_tokens) {}

ContextTracker::CacheMissKind ContextTracker::ClassifyMiss(bool reported, std::int64_t cache_read,
                                                           const CacheDiagnostics& diag) {
    if (!reported) {
        return CacheMissKind::Unreported;  // 缺测最优先:不冒充 0%,也不猜断因
    }
    if (diag.epoch_first_request) {
        return CacheMissKind::FirstRequest;
    }
    if (!diag.prefix_append_only) {
        return CacheMissKind::EpochBreak;  // 本地断因先于上游结论
    }
    // 本地前缀稳定(追加律成立):报了命中就是命中,报零就是上游没接住。
    return cache_read > 0 ? CacheMissKind::Hit : CacheMissKind::UpstreamMiss;
}

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

void ContextTracker::ApplyUsage(const api::Usage& usage, const std::string& turn_id, int step_index,
                                const CacheDiagnostics& diag) {
    // 四项全零 = provider 没在流末给 usage(见头文件注释):不清零、不
    // 覆盖,现有数字原样保住,只标旧值。
    const bool measured = usage.input_tokens > 0 || usage.output_tokens > 0 ||
                          usage.cache_read_tokens > 0 || usage.cache_creation_tokens > 0 ||
                          usage.output_reasoning_tokens > 0;
    if (measured) {
        Update(usage);
        // 本场累计(命中率分子分母):只认实测到的这笔,跨轮不清零。
        session_cache_read_total_ += usage.cache_read_tokens > 0 ? usage.cache_read_tokens : 0;
        session_input_total_ += api::TotalInputTokens(usage);
        usage_stale_ = false;
    }
    // 逐请求历史:一次模型请求一笔,实测与缺测都记(缺测标 unreported,
    // 显示层写"未回报"),环形缓冲保留最近 kCacheHistorySize 次。总账
    // 不跟着环形挤,显示层拿它写"全会话共 N 次",12 不冒充总数。
    // 问题 9:同一笔把诊断账抄进去并分型——本地前缀稳不稳、断在哪层,
    // 面板不再让人猜。
    CacheRequestRecord record;
    record.turn_id = turn_id;
    record.step_index = step_index;
    record.unreported = !measured;
    if (measured) {
        record.input_tokens = api::TotalInputTokens(usage);
        record.cache_read_tokens = usage.cache_read_tokens > 0 ? usage.cache_read_tokens : 0;
    }
    if (diag.present) {
        record.diagnostics_present = true;
        record.cache_epoch = diag.cache_epoch;
        record.epoch_break_reason = diag.epoch_break_reason;
        record.prefix_append_only = diag.prefix_append_only;
        record.epoch_first_request = diag.epoch_first_request;
        record.system_hash = diag.system_hash;
        record.tools_hash = diag.tools_hash;
        record.prefix_hash = diag.prefix_hash;
        record.stable_prefix_messages = diag.stable_prefix_messages;
        record.total_messages = diag.total_messages;
        record.wire_common_prefix_bytes = diag.wire_common_prefix_bytes;
        record.miss_kind = ClassifyMiss(measured, usage.cache_read_tokens, diag);
    }
    cache_history_.push_back(std::move(record));
    if (cache_history_.size() > kCacheHistorySize) {
        cache_history_.erase(cache_history_.begin());
    }
    ++total_model_requests_;
    // 陌生 turn_id(没走过 BeginUserTurn 的路径,如单发/续跑)自动补号,
    // 标签留空;显示层按"未登记"措辞,不猜内容。
    RegisterTurnIfMissing(turn_id);
    if (!measured) {
        usage_stale_ = true;
    }
}

void ContextTracker::RegisterTurnIfMissing(const std::string& turn_id) {
    if (turn_id.empty()) {
        return;  // 空 id 不登记:显示层按"轮次不明"分组
    }
    BeginUserTurn(turn_id, std::string());
}

void ContextTracker::BeginUserTurn(const std::string& turn_id, const std::string& label) {
    for (auto& entry : turn_labels_) {
        if (entry.turn_id == turn_id) {
            if (!label.empty()) {
                entry.label = label;  // 已登记就只补标签,序号不动
            }
            return;
        }
    }
    UserTurnLabel entry;
    entry.turn_id = turn_id;
    entry.label = label;
    entry.ordinal = ++next_turn_ordinal_;
    turn_labels_.push_back(std::move(entry));
    if (turn_labels_.size() > kMaxTurnLabels) {
        turn_labels_.erase(turn_labels_.begin());
    }
}

const ContextTracker::UserTurnLabel* ContextTracker::FindTurnLabel(const std::string& turn_id) const {
    for (const auto& entry : turn_labels_) {
        if (entry.turn_id == turn_id) {
            return &entry;
        }
    }
    return nullptr;
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
