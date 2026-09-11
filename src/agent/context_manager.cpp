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

void ContextManager::PopMessageBack() {
    if (!history_.empty()) {
        history_.pop_back();
    }
    if (!request_history_.empty()) {
        request_history_.pop_back();
    }
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
    // 新 epoch,压缩决策、截断钉子账与截断通报一并翻篇:compact/显式降档
    // 是唯一常规的全量重写点(正式 context 提交),重写后的视图按新档位
    // 从头定形(前缀缓存守恒单第六期;V3-REAL-01 换形状只走这条路);热区
    // 若仍带超线原文,下一请求的截断按新发生重新通报。
    result_view_memo_.decisions.clear();
    truncation_memo_.pinned.clear();
    announced_result_shapes_.clear();
}

ContextWorkingView ContextManager::BuildWorkingView(const ContextViewBudget& budget) {
    // 无损结构压缩(第六期"首次定形"):只改发给模型的视图——每枚
    // tool result 第一次进请求视图时定形(短则全文、超长首次即 artifact
    // 预览、重复自述指回、新版本自述替代),决策台账 epoch 内钉死,绝不
    // 追改已经发过的表示。活历史与 session JSONL 一字不动,tool use/
    // result 配对天然不破。压完的视图更小,后面保命索也更少真开刀。
    // 第二期:带仓时 Artifact 决策先落盘,视图带稳定 artifact_id。
    std::vector<api::Message> view_source;
    if (structural_compression_enabled_) {
        view_source = CompressWorkingView(request_history_, structural_options_, structural_stats_,
                                          result_view_memo_, artifact_store_);
    } else {
        view_source = request_history_;
    }

    // 保命索(单条巨肥工具结果的尾部截断,token 轴口径):截断按结果自身
    // 的 token 账算,且随首次采用的档位快照钉死(V3-REAL-01)——钉子账
    // 命中的直接重放定形时的形状,当次系数/窗口变化不重裁旧内容(估算器
    // 状态追改已发前缀,正是命中率 93%→57% 那场病理的暗裁来源);真换形状
    // 走 ReplaceHistory 正式提交。真"新发生"的截断(本 epoch 首次定形且真
    // 动了刀)才进 trim 报告,loop 拿去打 AfterHardTrim 通报并给前缀账
    // 点名。全量 JSONL 照旧保留。
    ContextWorkingView out;
    out.messages = ShrinkOversizedToolResults(std::move(view_source), budget.window_tokens,
                                              budget.token_calibration, &out.trim,
                                              &truncation_memo_);
    // 截断通报按结果身份+预览版本去重(V3-REAL-02):Shrink 只报新定形/硬闸
    // 降档的截断(带形状指纹),这里再滤掉"本 epoch 同身份同形状已通报过"
    // 的——同一枚重复采用不重报;另一枚巨型结果首次截断、已采用结果经
    // 硬闸换了形状,都必须报,不再被一枚全局布尔吞掉。
    bool newly_truncated = false;
    for (const auto& notice : out.trim.truncated_result_ids) {
        auto announced = announced_result_shapes_.find(notice.tool_use_id);
        if (announced == announced_result_shapes_.end()) {
            announced_result_shapes_.emplace(notice.tool_use_id, notice.shape_hash);
            newly_truncated = true;
        } else if (announced->second != notice.shape_hash) {
            announced->second = notice.shape_hash;  // 硬闸降档:新形状版本
            newly_truncated = true;
        }
    }
    out.trim.truncated_results = newly_truncated;
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
