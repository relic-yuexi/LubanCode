// DurableReplyOutbox(常驻总装 V1 第四件事的投递半场):本地投递的耐久
// 出箱账。注意身份:src/app_server/outbox.hpp(前端有界事件队列)与
// src/hooks/outbox.hpp(hook pending/ack 账)都不是这个 outbox——
// contracts.md §2 明令,名字相似也不能顶替。
//
// 账规矩(contracts.md §3/§4.4/§11.5):
//   - 只记 committed reply 的固定正文 artifact 与引用,不重拼 Agent
//     history(§4.4 ReplyAssembler 纪律:同一 committed response 重建出
//     相同 delivery_id 与相同 payload hash)。
//   - deliveryId 定式(§11.1):replySelectionId + target + ordinal 散列,
//     恢复后仍指原已提交选择——resume 一次不多送一次。
//   - 投影幂等:同 deliveryId 只入一账(item.enqueued 不重复落);崩在
//     selection 已提交与 outbox 投影之间,RecoveryProjector 按稳定
//     deliveryId 从已提交选择补出同一条 item——确定性投影,不重跑 Agent、
//     不调模型。
//   - 本地投递幂等:发布文件名固定为 out/<deliveryId>.txt;已发布(hash
//     核对相符)而回执未落,重启后补回执不出第二份;hash 不符不动文件,
//     记 flagged(dead letter 一类,V1 最小面)。
//   - 账行走 JournalWriter::AppendLine(PowerLoss);原件走
//     AtomicWriteFile(ProcessCrashDurability)。
//
// 发送态(§4.4 拆 V1 本地面):pending -> delivered;hash 不符 -> flagged。
// 渠道发送(QQ 接入单 Q2 §七,contracts.md §4.4 冻结表):
//   pending -> sending -> sent;sending 超时 -> delivery_unknown(停自动重发,
//   不虚 exactly-once);平台明确拒绝/限频耗尽 -> failed_*。
//   发出前记尝试(item.attempt),返回后记 provider_message_id(item.sent)。
#pragma once

#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "trajectory/journal.hpp"

namespace lubancode::gateway {

// 一枚出箱项(投影与投递的事实)。
struct ReplyOutboxItem {
    std::string delivery_id;      // 定式散列(dl-<hash16>)
    std::string selection_id;     // 来源 reply selection(恢复后不变)
    std::string delivery_target;  // "local:file" | "channel:<ch>:<acct>:<conv>"
    std::uint64_t ordinal = 1;    // 同 selection 的段序(拆段后 1..k;本地恒 1)
    std::string reply_text;       // 固定正文(入箱即冻结,不随后续变化)
    std::string reply_sha256;     // 正文 hash(入箱时算定)
    std::string session_id;       // 来源 V3 场(诊断/反查)
    std::string turn_id;          // 来源轮
    std::int64_t enqueued_at_ms = 0;
    std::string state;            // pending|sending|sent|delivery_unknown|failed
                                  // |delivered|flagged(本地族)
    std::string published_path;   // 发布文件相对 profile 的路径(本地族)
    std::string flag_reason;      // flagged 时的人话
    std::int64_t delivered_at_ms = 0;
    // ---- QQ 渠道 target(Q2 §七第一项;本地族全空) ----
    std::string target_channel_id;             // "qqbot"
    std::string target_account_id;             // "main"
    std::string target_conversation_id;        // direct openid
    std::string target_reply_to_message_id;    // 被动回复锚(来信 msg_id)
    std::uint32_t target_msg_seq = 0;          // 稳定 msg_seq(= ordinal;0=未记)
    std::string provider_message_id;           // QQ 回执(send 响应带的 om_*)
    std::string delivery_error;                // failed/delivery_unknown 时的稳定码
    std::int64_t attempts = 0;                 // 发送尝试次数(item.attempt 计数)
    std::string source_ref;                    // 来源审计 "ingress:<ch>:<acct>:<sid>"
    // ---- 出站附件(Q4 §十;只有带附件的段填,其余全空) ----
    std::string attachment_local_path;   // 冻结的产物原件(UTF-8 路径;投递
                                         // 时适配器读它上传,不拷贝进账)
    std::string attachment_file_name;    // 展示名(入箱时净化)
    std::string attachment_mime_type;
    std::int64_t attachment_size_bytes = 0;
    std::string attachment_sha256;       // 入箱时算定(冻结校验)
};

class DurableReplyOutbox {
public:
    struct OpenResult {
        bool ok = false;
        std::string error;
        std::size_t skipped_lines = 0;
    };

    struct Paths {
        std::filesystem::path log_file;     // outbox.jsonl
        std::filesystem::path replies_dir;  // replies/(正文原件)
        std::filesystem::path published_dir;  // out/(本地发布)
    };

    static OpenResult Open(DurableReplyOutbox* out, const Paths& paths);

    DurableReplyOutbox() = default;
    ~DurableReplyOutbox();
    DurableReplyOutbox(DurableReplyOutbox&&) noexcept;
    DurableReplyOutbox& operator=(DurableReplyOutbox&&) noexcept;
    DurableReplyOutbox(const DurableReplyOutbox&) = delete;
    DurableReplyOutbox& operator=(const DurableReplyOutbox&) = delete;

    // 入箱(投影):正文原件先落稳(replies/<selectionId>.txt),账行
    // (item.enqueued)后提交。同 delivery_id 重复入箱 = 幂等 no-op 回
    // duplicate(true),正文不重写(入箱后不随最新 prompt/路由改变,§4.4)。
    struct EnqueueReceipt {
        bool accepted = false;
        bool duplicate = false;
        std::string error_code;  // outbox.append_failed | outbox.artifact_failed
        std::string delivery_id;
    };
    EnqueueReceipt Enqueue(const std::string& selection_id, const std::string& reply_text,
                           const std::string& session_id, const std::string& turn_id,
                           std::int64_t now_ms);

    // ---- QQ 渠道 target(Q2 §七) -------------------------------------------
    struct ChannelTarget {
        std::string channel_id;
        std::string account_id;
        std::string conversation_id;
        std::string reply_to_message_id;  // 被动回复锚(可空)
        std::string source_ref;           // "ingress:<ch>:<acct>:<sid>"(结算反查)
    };
    // 渠道入箱:冻结正文按段限拆段(UTF-8 边界),每段一枚 item
    //(deliveryId = MakeDeliveryId(selection, target 串, ordinal))。
    // 返回本 selection 的全部段 id(含此前已入箱的段;幂等重入同款)。
    // attachment(Q4)非空时挂到末段:正文为空也照拆出一枚空段承载附件
    //(§十 10.2"没有文字、只有一个文件也算有效回复");附件本体不拷贝,
    // 账行冻结引用 + sha256。
    struct ChannelAttachment {
        std::string local_path;
        std::string file_name;
        std::string mime_type;
        std::int64_t size_bytes = 0;
    };
    struct ChannelEnqueueReceipt {
        bool accepted = false;   // 本次至少新入一段
        bool duplicate = false;  // 全部段已在(幂等重入)
        std::string error_code;
        std::vector<std::string> delivery_ids;  // 全部段(ordinal 序)
    };
    ChannelEnqueueReceipt EnqueueChannel(const std::string& selection_id,
                                         const std::string& reply_text,
                                         const std::string& session_id,
                                         const std::string& turn_id, const ChannelTarget& target,
                                         std::int64_t now_ms,
                                         const ChannelAttachment* attachment = nullptr);

    // 渠道投递驱动(泵侧逐段调;账行为先,状态推进幂等):
    bool RecordAttempt(const std::string& delivery_id, std::int64_t now_ms);  // 发出前记尝试
    bool MarkSent(const std::string& delivery_id, const std::string& provider_message_id,
                  std::int64_t now_ms);  // QQ 已接受
    bool MarkOutcomeUnknown(const std::string& delivery_id,
                            std::int64_t now_ms);  // 超时:停自动重发,不虚 exactly-once
    bool MarkChannelFailed(const std::string& delivery_id, const std::string& error_code,
                           std::int64_t now_ms);  // 平台明确拒绝/限频耗尽/令牌失效
    // 只读:待发送/在途的渠道项(pending 与 sending;ordinal 不保证全局序,
    // 同 selection 的段序由 delivery_id 的 ordinal 编码,泵按入箱序取)。
    std::vector<ReplyOutboxItem> PendingChannelItems() const;
    // 取渠道段的冻结正文(内存没有(重开后)从段原件读回并核 hash;
    // 原件丢失/损坏 = false——已提交原件丢失即隔离,不猜正文)。
    bool LoadChannelItemText(const std::string& delivery_id, std::string* text) const;

    // 本地投递:逐枚 pending 发布到 out/<deliveryId>.txt(原子写),
    // 成功落 item.delivered。已发布文件在且 hash 相符(上次崩在文件后
    // 回执前)→ 补回执不重写;hash 不符 → flagged,不覆盖不删。
    // 返回 (本轮发布数, 仍 pending 数)。broken 时返回 pending 数并在
    // error 出人话(泵停)。
    struct DeliverResult {
        std::size_t delivered = 0;
        std::size_t flagged = 0;
        std::size_t pending = 0;
        std::string error;
    };
    DeliverResult DeliverPending(std::int64_t now_ms);

    // ---- 只读投影(status/测试) ------------------------------------------
    std::vector<ReplyOutboxItem> ListItems() const;
    std::optional<ReplyOutboxItem> Find(const std::string& delivery_id) const;
    std::size_t PendingCount() const;
    // 回复原件目录(渠道路的 selection 原件与段原件同落此处;装配层
    // 递给执行器/恢复器,单一来源)。
    const std::filesystem::path& replies_dir() const { return paths_.replies_dir; }

    bool broken() const { return broken_; }

private:
    bool AppendLinePowerLoss(const nlohmann::json& line);
    std::filesystem::path ReplyArtifactPath(const std::string& selection_id) const;
    std::filesystem::path PublishedPath(const std::string& delivery_id) const;

    Paths paths_;
    std::optional<trajectory::JournalWriter> writer_;
    bool broken_ = false;
    std::map<std::string, ReplyOutboxItem> items_;
};

// deliveryId 定式散列(§11.1):replySelectionId + target + ordinal。
// 同一选择恒同一 deliveryId——resume/重扫不另发一份。
std::string MakeDeliveryId(const std::string& selection_id, const std::string& target,
                           std::uint64_t ordinal);

// 渠道 target 的稳定串(§七第一项):"channel:<ch>:<acct>:<conv>"——进
// MakeDeliveryId 与 reply.selection.committed 的 deliveryTarget,同源同值。
std::string MakeChannelDeliveryTarget(const std::string& channel_id,
                                      const std::string& account_id,
                                      const std::string& conversation_id);

// 冻结正文拆段(§七第一项"拆段结果"):按字节帽在 UTF-8 边界切,尽量
// 落在换行处(帽内最后一条换行);0/超帽参数非法回空(调用方明败)。
// 首版纯文本不做 markdown 感知;真平台长度上限归 Q3 实测校准。
std::vector<std::string> SplitReplySegments(const std::string& text, std::size_t max_bytes);

// 渠道段帽(§七:2000 字节保守值)。泵侧判"拆段 > 1 要不要给产物附件"
// 用同一常量,不另养第二份帽。
inline constexpr std::size_t kChannelSegmentBytes = 2000;

// 只读投影(status 分栏/测试用):从 outbox 账重放;文件不存在给空投影
//(零建目录零写盘)。与 DurableReplyOutbox::Open 同一份重放逻辑。
struct OutboxProjection {
    std::map<std::string, ReplyOutboxItem> items;  // deliveryId -> item
    std::size_t skipped_lines = 0;
};
OutboxProjection ReadOutboxProjection(const std::filesystem::path& log_file);

}  // namespace lubancode::gateway
