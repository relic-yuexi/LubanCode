// ContextManager(骨架拆解批四·病六):上下文管理从 Agent 类里拆出的那
// 一只手。Agent 只留身份、皮、历史句柄;这里握着——
//   - 双历史:history_(可持久、可 compact 的真历史)与 request_history_
//     (模型视图,另含每轮动态上下文);
//   - 前缀指纹与 cache epoch(agent/prefix.hpp):追加律逐请求记账;
//   - 无损结构压缩的开关、选项、台账(agent/context_events.hpp);
//   - 可追回 artifact 仓(agent/artifact_store.hpp)。
//
// 账本规矩全在原主(前缀缓存守恒单、分层压缩单),这里只搬家不改判:
// ReplaceHistory(压缩/重建)显式开新 epoch、清台账;保命索真截了东西先记
// pending_epoch_break_reason,下一份请求的追加律判定用它点名(指纹 diff
// 只能报 old_message_changed,这里的因更准)。

#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <vector>

#include "agent/artifact_store.hpp"
#include "agent/context.hpp"
#include "agent/context_events.hpp"
#include "agent/prefix.hpp"
#include "api/types.hpp"

namespace lubancode::agent {

// 一次请求的工作视图预算(Agent 从运行档案里现折):token 轴口径。
// window_tokens 传有效窗口(0 = 未知,保命索内部落兜底);token_calibration
// 是会话级估算校准系数(loop 每步现取,1.0 = 默认尺)。
struct ContextViewBudget {
    std::size_t window_tokens = 0;
    double token_calibration = 1.0;
};

// BuildWorkingView 的产物:发上 wire 的那份消息视图 + 这次有没有真丢东西。
struct ContextWorkingView {
    std::vector<api::Message> messages;
    TrimReport trim;  // 有损硬裁的报告(丢轮/截结果),上层须向用户明报
};

class ContextManager {
public:
    // ---- 双历史 -----------------------------------------------------------

    // 可持久、可 compact 的真历史(session/export/compact/记忆抽取都看这本)。
    const std::vector<api::Message>& durable_history() const { return history_; }
    // 模型视图(每轮请求实际拼装的那本,另含动态上下文块)。
    const std::vector<api::Message>& request_history() const { return request_history_; }

    // 一轮用户输入落双账:durable 是真输入,request_view 是带动态上下文的
    // 请求视图版(两本可以差一个尾部 TextBlock)。
    void PushUserTurn(api::Message durable, api::Message request_view);
    // 一条消息原样落双账(assistant 回复、tool_result 批、续跑标记)。
    void PushMessage(api::Message message);
    // 轮次边界的来信注入(inbox):按 InjectIncomingMessage 的规矩进双账。
    void InjectIncoming(api::Message incoming);
    // 双账末条各追加一块(步数将尽提醒那类"随 history 留住"的尾部注入)。
    void AppendToLast(const api::ContentBlock& block);
    // mid-turn compact 后把本轮动态上下文补在请求视图末条尾部(新 epoch
    // 已开,不追改旧请求)。
    void AppendToLastRequest(const api::ContentBlock& block);

    // /compact 之后的整本换史:显式开新 cache epoch(history_compacted)、
    // 清上一份请求指纹与结构压缩台账、翻 sticky 视图。
    void ReplaceHistory(std::vector<api::Message> new_history);

    // ---- 工作视图(压缩 + 裁剪 + sticky)-----------------------------------

    void set_structural_compression_enabled(bool enabled) { structural_compression_enabled_ = enabled; }
    void set_structural_options(const StructuralCompressionOptions& options) { structural_options_ = options; }
    const StructuralCompressionOptions& structural_options() const { return structural_options_; }
    void set_artifact_store(ContextArtifactStore* store) { artifact_store_ = store; }

    // 最近一次请求的结构压缩账(/context 与诊断用)。
    const StructuralCompressionStats& structural_stats() const { return structural_stats_; }
    // 决策台账只读口(/context 诊断用)。摘要走 context_read 的新工具结果,
    // 不再回头改这本账。
    const ResultViewMemo& result_view_memo() const { return result_view_memo_; }

    // 拼下一份请求的工作视图:无损结构压缩(首次定形,epoch 内不追改)
    // -> 单条巨肥工具结果的保命索(token 轴口径)。真截了东西的因记进
    // pending_epoch_break_reason,随 AccountRequest 一并点名。
    ContextWorkingView BuildWorkingView(const ContextViewBudget& budget);

    // 压力/触发线专用的 dry-run 视图(P1-1 口径统一):与 BuildWorkingView
    // 走同一条无损结构压缩决策路,但 memo/stats/store 全用临时账——不落
    // 盘、不钉决策、不翻 sticky,纯回答"下一份请求真会发出去的 history
    // 长什么样"。/context 与压缩前后账拿这一本,不该拿未压缩的全量
    // history(真机 189k 的估账对 47k 的真实请求,就是两把尺分家的账)。
    // 注意:midturn 触发线自压缩触发失衡单起不再用这本——loop 直接拿
    // BuildWorkingView 的真视图估(连保命索的形状都一致),这本只剩
    // 显示/预算侧的用户。
    std::vector<api::Message> BuildPressureDryRunView() const;

    // ---- 前缀账(agent/prefix.hpp)-----------------------------------------

    // 当前 cache epoch:1 起,每次断前缀 +1。epoch 断不是失败,是给
    // "命中跌了"点名的那根梁。
    int cache_epoch() const { return cache_epoch_; }

    // loop 先知道的断因(hard trim 真丢了东西)先记着;下一份请求入账时
    // 由它点名——指纹 diff 只能报 old_message_changed,这里的因更准。已记
    // 未报期间不覆盖(第一个因优先)。
    void NotePendingEpochBreak(std::string reason) {
        if (pending_epoch_break_reason_.empty()) {
            pending_epoch_break_reason_ = std::move(reason);
        }
    }

    // 一份实际要发的请求入账:与上一份比追加律。断了开新 epoch 并点名
    // 断因——loop 自己知道的因(compact/hard trim)优先,没有就按指纹
    // diff 点名(model/system/tools/旧消息)。返回这份请求的判定,随
    // UsageReport 交出去。
    //
    // wire_dump(问题 9 诊断模式,LUBANCODE_DEBUG_PREFIX 打开时由 loop
    // 拿 backend 序列化好递进来):非空调用时与上一份序列化文本量公共
    // 前缀字节数,记进 account.wire_common_prefix_bytes 并留存这份文本供
    // 下一次比较;传 nullptr(默认,常态)完全不序列化、不存文本,字段
    // 留 -1(不可得),不冒充 0。
    struct PrefixAccount {
        bool append_only = true;
        std::string break_reason;  // 没断是空串
        // 诊断行用的细节(不带正文,只带位置与 hash;见 loop.cpp 的
        // LUBANCODE_DEBUG_PREFIX 输出)。had_previous=false 表示这是本
        // epoch 首份请求,无从比较(天然算追加,不打诊断行)。
        bool had_previous = false;
        std::size_t old_message_changed_at = 0;  // 违反追加律的第一条旧消息下标
        std::size_t appended_messages = 0;       // 尾部新添的消息条数
        std::string system_hash;                 // 指纹 hash 原样(loop 截前 8 位打日志)
        std::string tools_hash;
        // ---- 每请求缓存诊断账(问题 9):本地前缀视角,只留 hash 与长度 ----
        int cache_epoch = 1;  // 记账后的 epoch(断了就是新号)
        // 稳定消息前缀:与上一份请求逐条相等的那段开头消息。没有上一份
        // (首请求)或一条都不共享时 stable_prefix_messages=0、prefix_hash
        // 为空。total_messages 是本次请求的消息总数。
        std::string prefix_hash;             // 稳定前缀的合成指纹(空 = 无稳定前缀)
        std::size_t stable_prefix_messages = 0;
        std::size_t total_messages = 0;
        // wire 序列化公共前缀字节(-1 = 诊断模式没开/backend 不提供)。
        std::int64_t wire_common_prefix_bytes = -1;
    };
    PrefixAccount AccountRequest(const api::Request& request, const std::string* wire_dump = nullptr);

private:
    std::vector<api::Message> history_;          // 可持久、可 compact 的真历史
    std::vector<api::Message> request_history_;  // 模型视图;另含每轮动态上下文
    // 前缀记账:上一份实际发出的请求指纹(没有 = 本 turn 第一份请求,无从
    // 比较,天然算追加)、cache epoch 序号、loop 自己先知道的断因(compact/
    // hard trim,报出后即清)。
    std::optional<PrefixFingerprint> last_prefix_;
    int cache_epoch_ = 1;
    std::string pending_epoch_break_reason_;
    // 诊断模式(LUBANCODE_DEBUG_PREFIX)才留:上一份请求的 wire 序列化
    // 文本,只为量下一次的公共前缀字节。常态度(不开诊断)永远是空,
    // 不为对账常年序列化全份请求、揣着几 MB 文本跑。
    std::string last_wire_dump_;
    // 结构压缩"首次定形"的决策台账(tool_use_id -> 决策),epoch 内跨请求
    // 钉死;ReplaceHistory(开新 epoch)时清空。
    ResultViewMemo result_view_memo_;
    bool structural_compression_enabled_ = true;  // 无损结构压缩(工作视图)
    StructuralCompressionOptions structural_options_{};
    StructuralCompressionStats structural_stats_{};  // 最近一次请求的压缩账(观测用)
    ContextArtifactStore* artifact_store_ = nullptr;  // 空 = 没仓,退回旧行为
    // 保命索的截断是确定性的:同一份超线结果每个请求都会再截一次。真通报
    // 只打第一次(本 epoch 内),后续请求形状不变,不是新发生的有损动作——
    // 不去重会每请求刷一条告警。ReplaceHistory 开新 epoch 时复位,压缩后
    // 热区里若还留着超线原文,下一请求重新通报。
    bool truncation_announced_in_epoch_ = false;
};

}  // namespace lubancode::agent
