// ContextManager 的实现(骨架拆解批四·病六):双历史/前缀账/sticky 视图/
// 结构压缩台账从 AgentLoop::Run 的散装局部账搬进一只类。判定与次序一字
// 不动——前缀缓存守恒单与分层压缩单钉着的行为,搬家不改判。

#include "agent/context_manager.hpp"

#include "agent/context.hpp"

namespace lubancode::agent {

namespace {

// 两段 wire 文本的最长公共前缀字节数(与 doctor 探针的 CommonPrefixBytes
// 同一只算法;那头导出在 app 层纯函数,这里不跨层引用,自留一份三行)。
std::size_t CommonWirePrefixBytes(const std::string& a, const std::string& b) {
    const std::size_t n = a.size() < b.size() ? a.size() : b.size();
    std::size_t i = 0;
    while (i < n && a[i] == b[i]) {
        ++i;
    }
    return i;
}

}  // namespace

void ContextManager::PushUserTurn(api::Message durable, api::Message request_view) {
    history_.push_back(std::move(durable));
    request_history_.push_back(std::move(request_view));
}

void ContextManager::PushMessage(api::Message message) {
    history_.push_back(message);
    request_history_.push_back(std::move(message));
}

void ContextManager::InjectIncoming(api::Message incoming) {
    InjectIncomingMessage(history_, incoming);
    InjectIncomingMessage(request_history_, std::move(incoming));
}

void ContextManager::AppendToLast(const api::ContentBlock& block) {
    if (!history_.empty()) {
        history_.back().content.push_back(block);
    }
    if (!request_history_.empty()) {
        request_history_.back().content.push_back(block);
    }
}

void ContextManager::AppendToLastRequest(const api::ContentBlock& block) {
    if (!request_history_.empty()) {
        request_history_.back().content.push_back(block);
    }
}

void ContextManager::ReplaceHistory(std::vector<api::Message> new_history) {
    history_ = std::move(new_history);
    request_history_ = history_;
    ++cache_epoch_;
    pending_epoch_break_reason_ = "history_compacted";
    last_prefix_.reset();
    last_wire_dump_.clear();  // 新 epoch:上一份 wire 文本不再可比,清掉
    // 新 epoch,压缩决策与 sticky 视图一并翻篇:compact 是唯一常规的全量
    // 重写点,重写后的视图从头定形(前缀缓存守恒单第六期)。
    result_view_memo_.decisions.clear();
    sticky_view_.reset();
    sticky_base_history_size_ = 0;
}

ContextWorkingView ContextManager::BuildWorkingView(const ContextViewBudget& budget) {
    // 无损结构压缩(第六期"首次定形"):只改发给模型的视图——每枚
    // tool result 第一次进请求视图时定形(短则全文、超长首次即 artifact
    // 预览、重复自述指回、新版本自述替代),决策台账 epoch 内钉死,绝不
    // 追改已经发过的表示。活历史与 session JSONL 一字不动,tool use/
    // result 配对天然不破。压完的视图更小,后面字符安全网也更少真开刀。
    // 第二期:带仓时 Artifact 决策先落盘,视图带稳定 artifact_id。
    std::vector<api::Message> view_source;
    if (structural_compression_enabled_) {
        view_source = CompressWorkingView(request_history_, structural_options_, structural_stats_,
                                          result_view_memo_, artifact_store_);
    } else {
        view_source = request_history_;
    }

    // 字符安全网 + sticky 视图:还没动手裁过,就按老规矩裁;真动手裁了
    // (丢轮/截结果),把裁过的视图钉住,后续只往尾部追加新消息——不再
    // 每请求重算"第一轮 + 最近 N 轮"让窗口一路滑(窗口滑就是追改已发
    // 前缀)。全量 JSONL 照旧保留,sticky 只是模型眼下那本账。
    ContextWorkingView out;
    if (sticky_view_.has_value() && view_source.size() >= sticky_base_history_size_) {
        std::vector<api::Message> pinned = *sticky_view_;
        pinned.insert(pinned.end(), view_source.begin() + static_cast<std::ptrdiff_t>(sticky_base_history_size_),
                      view_source.end());
        if (EstimateHistoryBytes(pinned) > budget.max_context_chars) {
            // 钉住的视图也装不下了(长会话总会到这一步):重裁一次,换一副
            // 新形状并重新钉住——这是一次明确的 epoch break,下面的
            // trim 报告会把它记上(hard_trim)。
            pinned = TrimHistory(std::move(pinned), budget.max_context_chars, kDefaultKeepRecentTurns,
                                 &out.trim);
            sticky_view_ = pinned;
            sticky_base_history_size_ = view_source.size();
        }
        out.messages = std::move(pinned);
        return out;
    }
    std::vector<api::Message> trimmed =
        TrimHistory(view_source, budget.max_context_chars, kDefaultKeepRecentTurns, &out.trim);
    if (out.trim.trimmed_turns || out.trim.truncated_results) {
        // 第一次真动手裁:钉住,开新 epoch 的账由 hard_trim 记。
        sticky_view_ = trimmed;
        sticky_base_history_size_ = view_source.size();
    }
    out.messages = std::move(trimmed);
    return out;
}

std::vector<api::Message> ContextManager::BuildPressureDryRunView() const {
    if (!structural_compression_enabled_) {
        return request_history_;
    }
    ResultViewMemo scratch_memo;
    StructuralCompressionStats scratch_stats;
    // store 传空:dry-run 不落盘 artifact,也不改本对象的决策台账——估
    // 算归估算,正式 BuildWorkingView 时该落的照落。
    return CompressWorkingView(request_history_, structural_options_, scratch_stats, scratch_memo,
                               /*store=*/nullptr);
}

ContextManager::PrefixAccount ContextManager::AccountRequest(const api::Request& request, const std::string* wire_dump) {
    const PrefixFingerprint fingerprint = FingerprintRequest(request);
    PrefixAccount account;
    account.system_hash = fingerprint.system_hash;
    account.tools_hash = fingerprint.tools_hash;
    account.total_messages = fingerprint.message_hashes.size();
    if (last_prefix_.has_value()) {
        const PrefixDiff diff = DiffFingerprints(*last_prefix_, fingerprint);
        account.had_previous = true;
        account.append_only = diff.append_only();
        account.old_message_changed_at = diff.old_message_changed_at;
        account.appended_messages = diff.appended_messages;
        // 稳定消息前缀(问题 9 诊断账):只看消息层,与追加律判定分开
        // ——tools/system 换了梁,消息前缀照样可以逐条稳定。
        const StablePrefixView stable = StablePrefixOf(*last_prefix_, fingerprint);
        account.stable_prefix_messages = stable.messages;
        account.prefix_hash = stable.hash;
        if (!account.append_only) {
            account.break_reason =
                pending_epoch_break_reason_.empty() ? diff.break_reason() : pending_epoch_break_reason_;
            ++cache_epoch_;
        }
    }
    account.cache_epoch = cache_epoch_;
    // wire 公共前缀(诊断模式才有):量完即换新的一份,留待下一次比较。
    if (wire_dump != nullptr) {
        account.wire_common_prefix_bytes =
            last_wire_dump_.empty() ? -1 : static_cast<std::int64_t>(CommonWirePrefixBytes(last_wire_dump_, *wire_dump));
        last_wire_dump_ = *wire_dump;
    }
    pending_epoch_break_reason_.clear();
    last_prefix_ = std::move(fingerprint);
    return account;
}

}  // namespace lubancode::agent
