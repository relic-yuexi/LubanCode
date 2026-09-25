#include "cli/context_tracker.hpp"

#include <cstdint>

#include "agent/context.hpp"  // AutoCompactTriggerLine:触发线公共尺(§〇.1)

namespace lubancode::cli {

ContextTracker::ContextTracker(std::size_t window_tokens) : window_tokens_(window_tokens) {}

ContextTracker::CacheMissKind ContextTracker::ClassifyMiss(bool usage_reported, bool cache_read_reported,
                                                           std::int64_t cache_read,
                                                           const CacheDiagnostics& diag) {
    if (!usage_reported) {
        return CacheMissKind::Unreported;  // 缺测最优先:不冒充 0%,也不猜断因
    }
    // usage 在场而读取明细缺席(C2):读取量未知——既不是命中也不是上游
    // 没接住,先记"明细未报",不猜。读取数字非零时不算这类(非零本身
    // 就是证据,调用方已把它并进 cache_read_reported)。
    if (!cache_read_reported && cache_read <= 0) {
        return CacheMissKind::CacheDetailUnreported;
    }
    if (diag.epoch_first_request) {
        return CacheMissKind::FirstRequest;
    }
    if (!diag.prefix_append_only) {
        return CacheMissKind::EpochBreak;  // 本地断因先于上游结论
    }
    // 本地前缀稳定(追加律成立):报了命中是命中,报零是上游没接住。
    return cache_read > 0 ? CacheMissKind::Hit : CacheMissKind::UpstreamMiss;
}

void ContextTracker::ApplyContextEstimate(std::size_t tokens) {
    current_tokens_ = tokens;
    current_estimated_ = true;
    usage_stale_ = false;
}

void ContextTracker::Update(const api::Usage& usage) {
    current_estimated_ = false;
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
                                const CacheDiagnostics& diag, const UsageReportFlags& flags) {
    // §五:清场后、新场首个轮次登记前,旧场迟到的带号 usage 整笔丢弃
    // ——不带号的账没有身份可核,按老行为放行(单发/单测路径)。
    if (drop_usage_until_next_turn_ && !turn_id.empty()) {
        return;
    }
    // 四项全零 = provider 没在流末给 usage(见头文件注释):不清零、不
    // 覆盖,现有数字原样保住,只标旧值。C2 后"明报全零"另有 flags 说话
    // ——flags.known 时 usage_reported 才是权威,数字全零不再被当成没报。
    const bool measured = usage.input_tokens > 0 || usage.output_tokens > 0 ||
                          usage.cache_read_tokens > 0 || usage.cache_creation_tokens > 0 ||
                          usage.output_reasoning_tokens > 0;
    // 报告位合成:显式位是主路,但"数字非零"仍是明报的证据(provider 报了
    // 数字才非零)——flags 在场时明报与非零取或,没带(旧路径/单测)退回
    // 数字推断。明报全零(flags 真、数字零)不再被当成缺测。
    const bool usage_reported = flags.known ? (flags.usage_reported || measured) : measured;
    const bool cache_read_reported =
        (flags.known ? flags.cache_read_reported : measured) || usage.cache_read_tokens > 0;
    if (measured) {
        Update(usage);
        // 本场累计(命中率分子分母):只认实测到且自洽的这笔,跨轮不清零。
        // 异常样本(矛盾账)不进分子分母——比例只对自洽样本算。
        if (!flags.anomalous) {
            session_cache_read_total_ += usage.cache_read_tokens > 0 ? usage.cache_read_tokens : 0;
            session_input_total_ += api::TotalInputTokens(usage);
        }
        usage_stale_ = false;
    } else if (flags.known && flags.usage_reported) {
        if (current_estimated_) {
            Update(usage);
        }
        // provider 明报了 usage 而五项皆零:数字没变,但不是"没报"——
        // 不标旧值(那是给缺测的),照实当一次零实测。
        usage_stale_ = false;
    }
    // 逐请求历史:一次模型请求一笔,实测与缺测都记(缺测标 unreported,
    // 显示层写"未回报"),环形缓冲保留最近 kCacheHistorySize 次。总账
    // 不跟着环形挤,显示层拿它写"全会话共 N 次",12 不冒充总数。
    // 问题 9:同一笔把诊断账抄进去并分型——本地前缀稳不稳、断在哪层,
    // 面板不再让人猜。C2:读/写明报位与异常位一并抄入。
    CacheRequestRecord record;
    record.turn_id = turn_id;
    record.step_index = step_index;
    record.unreported = !usage_reported;
    record.cache_read_reported = cache_read_reported;
    record.cache_creation_reported =
        (flags.known ? flags.cache_creation_reported : false) || usage.cache_creation_tokens > 0;
    record.anomalous = flags.anomalous;
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
        record.miss_kind = ClassifyMiss(usage_reported, cache_read_reported, usage.cache_read_tokens, diag);
    }
    cache_history_.push_back(std::move(record));
    if (cache_history_.size() > kCacheHistorySize) {
        cache_history_.erase(cache_history_.begin());
    }
    ++total_model_requests_;
    // 陌生 turn_id(没走过 BeginUserTurn 的路径,如单发/续跑)自动补号,
    // 标签留空;显示层按"未登记"措辞,不猜内容。
    RegisterTurnIfMissing(turn_id);
    if (!usage_reported) {
        usage_stale_ = true;
    }
}

void ContextTracker::RegisterTurnIfMissing(const std::string& turn_id) {
    if (turn_id.empty()) {
        return;  // 空 id 不登记:显示层按"轮次不明"分组
    }
    BeginUserTurn(turn_id, std::string());
}

void ContextTracker::SetWindowBudget(std::size_t window_tokens, ContextWindowSource source,
                                      const std::string& provider, const std::string& model) {
    window_tokens_ = window_tokens;
    window_source_ = source;
    window_provider_ = provider;
    window_model_ = model;
}

void ContextTracker::ResetSession() {
    // §五:窗口设置与来路保留,观测值全清(合同清单逐项对齐):当前实测
    // 占用、最近 input/cache read、usage_stale、本场累计、逐请求历史、
    // 请求总数、轮次登记账与序号。server_prefix_caching 是端点属性观测
    // (归 provider),保留——新场首次 /doctor cache 前它仍是"上一次对
    // 这个端点的观测",不冒充新场实测。
    current_tokens_ = 0;
    current_estimated_ = false;
    last_cache_read_tokens_ = 0;
    last_input_tokens_ = 0;
    usage_stale_ = false;
    session_cache_read_total_ = 0;
    session_input_total_ = 0;
    cache_history_.clear();
    total_model_requests_ = 0;
    turn_labels_.clear();
    next_turn_ordinal_ = 0;
    // 旧场迟到闸:清场到新场首个 BeginUserTurn 之间的带号 usage 丢弃。
    drop_usage_until_next_turn_ = true;
}

RestoredWindowDecision ResolveRestoredContextWindow(const RestoredWindowInput& input) {
    RestoredWindowDecision out;
    // 1. 本次明确覆盖最优先:用户这进程里亲手设过,不拿旧档压回去。
    if (input.manual_override) {
        out.note = "resume_window.manual_kept";
        return out;
    }
    // 2. 旧档没有预算记录:沿用当前配置/目录,明说回落。
    if (!input.session_window_present || input.session_window_tokens == 0) {
        out.note = "resume_window.no_record";
        return out;
    }
    // 3. 身份核对:身份不全或对不上,都不能确认这份预算属于当前模型
    // ——不套用(不能拿模型 A 的预算套给模型 B),回落并说明。
    if (input.session_provider.empty() || input.session_model.empty() ||
        input.session_provider != input.now_provider || input.session_model != input.now_model) {
        out.note = "resume_window.identity_mismatch";
        return out;
    }
    // 4. 超限拒绝:目录已知上限被旧值超过,不静默截断、不按错误预算
    // 继续——回落当前配置,明报旧值与上限,待用户 /context 纠正。
    if (input.declared_limit.has_value() && *input.declared_limit > 0 &&
        input.session_window_tokens > *input.declared_limit) {
        out.note = "resume_window.over_limit_rejected";
        return out;
    }
    // 5. 匹配身份的会话预算:套用(值与当前不同才真动,调用方比对)。
    out.apply = true;
    out.tokens = input.session_window_tokens;
    out.note = "resume_window.restored";
    return out;
}

void ContextTracker::BeginUserTurn(const std::string& turn_id, const std::string& label) {
    // 新场的首个轮次登记:迟到的丢弃闸到此收口,之后的 usage 正常记账。
    if (drop_usage_until_next_turn_) {
        drop_usage_until_next_turn_ = false;
    }
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
    // 触发线(§〇.1 用户定案,2026-09-09):窗口×80% − 压缩提示词 4k −
    // 压缩结果预留 8k(200k 窗即 148k)。两笔算进账,摘要请求自己的指令与
    // 产出才有地方安放。与 loop 的 projected 双闸共用 agent::AutoCompact
    // TriggerLine 同一只,两条路口径不漂移;窗口小到扣不动时线夹到 0
    // ——但零占用不触发(空历史没有可压的东西)。
    return current_tokens_ > 0 && current_tokens_ >= agent::AutoCompactTriggerLine(window_tokens_);
}

}  // namespace lubancode::cli
