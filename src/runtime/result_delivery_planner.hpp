// ResultDeliveryPlanner(异步工具单 P2;单 §7 请求边界):
// 完成通知只入 mailbox;请求边界(拼请求前——冻结输入前的唯一时点)选
// 已提交结果,把选中的投递写成正式 tool 消息落账+接纳;prepared 落稳后
// 记 delivery.prepared;响应收口按证据写 acknowledged/uncertain。
//
// P2 投递面:native_deferred 的欠账配对——原调用的业务结果(persisted
// 事件 + ≤32 KiB 预览)在下一次请求边界配回原 provider call(单 §8
// native 轨迹)。job_handle 的完成通知入 mailbox 只记账(P2 由模型经
// job_get/job_wait 自取;notify-vs-continue 的调度归 P4)。
//
// 恢复(单 §6"原文已落仓、消息未提交→补投递不重跑"):RestoreFromLedger
// 把账上"终态已落、义务未配"的 native 欠账重建进 mailbox——同一套配对
// 路补消息,不重跑工作。
//
// 写账纪律:writer 是会话 v3 单写者(与主桥/协调器共享),写前持
// tool_results_mutex;配对链走 ToolActionSession::ReopenAligned(恢复侧
// 把手:只补 selected + tool 消息,不造新 pending,不改 attempt)。
#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "agent/async_tool_seam.hpp"
#include "api/types.hpp"
#include "trajectory/v3/reader.hpp"
#include "trajectory/v3/tool_action.hpp"
#include "trajectory/v3/writer.hpp"

namespace lubancode::runtime {

// 完成通知(mailbox 条目;协调器泵出的终态观测)。
struct CompletionNotice {
    std::string job_id;
    std::string action_id;
    std::string mode;  // job_handle|native_deferred
    // native_deferred:欠账的原 provider call id(wireCallRef.callId);
    // job_handle:接单调用的 provider call id(记账用)。
    std::string provider_call_id;
    std::string turn_id;
    std::string step_id;
    std::string result_ref;  // 业务 tool.result.persisted 事件 id
    std::uint64_t result_version = 0;
    std::string preview;  // ≤32 KiB 有界预览(配对正文)
    bool preview_truncated = false;
    bool failed = false;
    std::string failure;
    // 配对链对齐(ReopenAligned 用):工作 attempt 号与终态
    //(ToolActionSession::Terminal 的 int 值;0 = 未知)。
    std::uint64_t attempt = 0;
    int terminal_kind = 0;
    // 稳定提交次序:通知到达序(同序者按 job_id);恢复重建按账面 seq。
    std::uint64_t commit_ordinal = 0;
    // 目标分支(会话 id):不跨分支投递(单 §7)。
    std::string branch;
};

class ResultDeliveryPlannerImpl final : public agent::ResultDeliveryPlanner {
public:
    struct Hooks {
        trajectory::v3::V3Writer* writer = nullptr;
        std::shared_ptr<std::recursive_mutex> writer_mutex;
        std::function<std::string()> current_turn_id;
        // request_id -> 响应证据事件 id(acknowledged 的 evidenceRef;查不
        // 到 = 回执丢失,落 uncertain)。
        std::function<std::optional<std::string>(const std::string&)> response_evidence;
    };

    explicit ResultDeliveryPlannerImpl(Hooks hooks);
    ~ResultDeliveryPlannerImpl() override;

    // 完成通知只入 mailbox(单 §7):在途请求的结果自然留给下次——选取
    // 只发生在请求边界。同 job_id 重复通知按 result_version 去重。
    void NotifyCompletion(CompletionNotice notice);

    // ---- agent::ResultDeliveryPlanner(引擎在请求边界调) ----
    std::vector<api::Message> SelectForRequestBoundary() override;
    void NoteRequestPrepared(const std::string& request_id) override;
    void NoteResponseOutcome(const std::string& request_id, bool acknowledged) override;

    // 恢复(单 §6 表"原文已落仓、tool 消息未提交"):纯读账,把终态已落、
    // 义务未配的 native_deferred 欠账重建进 mailbox。返回重建条数。
    std::size_t RestoreFromLedger(const trajectory::v3::V3Ledger& ledger);

    // 诊断:mailbox 未投递条数(native 欠账 / 全部)。
    std::size_t pending_native_count() const;
    std::size_t mailbox_size() const;

private:
    struct PendingDelivery {
        std::string delivery_id;  // resultVersion+分支+用途 的稳定去重键
        std::string request_id;   // 已 prepared 的请求(空 = 选中未记账)
        CompletionNotice notice;
        bool message_written = false;
        bool prepared_written = false;
        bool settled = false;  // acknowledged/uncertain 已落
    };

    Hooks hooks_;
    mutable std::mutex mutex_;
    std::vector<CompletionNotice> mailbox_;
    std::vector<PendingDelivery> in_flight_;  // 本请求选中、待 prepared/收口
    std::uint64_t next_ordinal_ = 1;
};

}  // namespace lubancode::runtime
