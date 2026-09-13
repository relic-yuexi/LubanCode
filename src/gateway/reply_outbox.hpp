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
// 渠道发送(sending/retry_wait/delivery_unknown)归 V3 批的 adapter 面。
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
    std::string delivery_target;  // V1 恒 "local:file"
    std::uint64_t ordinal = 1;
    std::string reply_text;       // 固定正文(入箱即冻结,不随后续变化)
    std::string reply_sha256;     // 正文 hash(入箱时算定)
    std::string session_id;       // 来源 V3 场(诊断/反查)
    std::string turn_id;          // 来源轮
    std::int64_t enqueued_at_ms = 0;
    std::string state;            // pending | delivered | flagged
    std::string published_path;   // 发布文件相对 profile 的路径
    std::string flag_reason;      // flagged 时的人话
    std::int64_t delivered_at_ms = 0;
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

// 只读投影(status 分栏/测试用):从 outbox 账重放;文件不存在给空投影
//(零建目录零写盘)。与 DurableReplyOutbox::Open 同一份重放逻辑。
struct OutboxProjection {
    std::map<std::string, ReplyOutboxItem> items;  // deliveryId -> item
    std::size_t skipped_lines = 0;
};
OutboxProjection ReadOutboxProjection(const std::filesystem::path& log_file);

}  // namespace lubancode::gateway
