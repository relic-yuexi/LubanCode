// DurableReplyOutbox 实现(常驻总装 V1)。合同见头文件与 contracts.md §3/§4.4。
#include "gateway/reply_outbox.hpp"

#include <algorithm>
#include <fstream>
#include <iterator>
#include <utility>

#include "platform/atomic_write.hpp"
#include "platform/paths.hpp"
#include "platform/sha256.hpp"

namespace lubancode::gateway {

namespace {

constexpr const char* kTypeEnqueued = "item.enqueued";
constexpr const char* kTypeDelivered = "item.delivered";
constexpr const char* kTypeFlagged = "item.flagged";
// QQ 渠道族(Q2 §七):尝试/已接受/超时未知/最终失败。
constexpr const char* kTypeAttempt = "item.attempt";
constexpr const char* kTypeSent = "item.sent";
constexpr const char* kTypeOutcomeUnknown = "item.outcome_unknown";
constexpr const char* kTypeChannelFailed = "item.channel_failed";
// QQ 发送身份(A05/A06):分配/裁决。分配行是发号的唯一事实源——重开
// (重启)后 next_seq 从这里重放,不归零。
constexpr const char* kTypeIdentityAssigned = "msgseq.assigned";
constexpr const char* kTypeIdentityRetired = "msgseq.retired";

std::string GetJsonString(const nlohmann::json& json, const char* key) {
    if (!json.is_object() || !json.contains(key) || !json[key].is_string()) {
        return std::string();
    }
    return json[key].get<std::string>();
}

std::int64_t GetJsonInt(const nlohmann::json& json, const char* key) {
    if (!json.is_object() || !json.contains(key) || !json[key].is_number_integer()) {
        return 0;
    }
    return json[key].get<std::int64_t>();
}

std::uint64_t GetJsonUint(const nlohmann::json& json, const char* key) {
    return static_cast<std::uint64_t>(GetJsonInt(json, key));
}

// 读小文件全文;读不动给空(调用方按"文件不在/读不了"分支处理)。
std::optional<std::string> ReadFileText(const std::filesystem::path& path) {
    std::error_code ec;
    if (!std::filesystem::exists(path, ec) || ec) {
        return std::nullopt;
    }
    std::ifstream stream(path, std::ios::binary);
    if (!stream) {
        return std::nullopt;
    }
    return std::string((std::istreambuf_iterator<char>(stream)), std::istreambuf_iterator<char>());
}

}  // namespace

OutboxProjection ReadOutboxProjection(const std::filesystem::path& log_file) {
    OutboxProjection projection;
    std::error_code ec;
    if (!std::filesystem::is_regular_file(log_file, ec) || ec) {
        return projection;  // 没有账(或路径被占)= 空投影(只读面零副作用)
    }
    std::ifstream stream(log_file, std::ios::binary);
    std::string line_text;
    while (std::getline(stream, line_text)) {
        if (line_text.empty()) continue;
        nlohmann::json line;
        try {
            line = nlohmann::json::parse(line_text);
        } catch (const nlohmann::json::exception&) {
            ++projection.skipped_lines;
            continue;
        }
        const std::string type = GetJsonString(line, "type");
        const std::string delivery_id = GetJsonString(line, "deliveryId");
        if (delivery_id.empty()) {
            ++projection.skipped_lines;
            continue;
        }
        if (type == kTypeIdentityAssigned) {
            // 发送身份分配(A05):live 身份 + 发号计数两本账。
            ChannelSendIdentity identity;
            identity.anchor_msg_id = GetJsonString(line, "msgId");
            identity.payload_sha256 = GetJsonString(line, "payloadSha256");
            identity.msg_seq = static_cast<std::uint32_t>(GetJsonUint(line, "msgSeq"));
            const std::string account = GetJsonString(line, "accountId");
            const std::int64_t assigned_at = GetJsonInt(line, "atMs");
            projection.live_identities[delivery_id] =
                ChannelIdentityBookEntry{identity, assigned_at};
            const std::string key = account + "\n" + identity.anchor_msg_id;
            const auto max_seq = projection.next_seq_by_identity.find(key);
            if (max_seq == projection.next_seq_by_identity.end() ||
                identity.msg_seq > max_seq->second) {
                projection.next_seq_by_identity[key] = identity.msg_seq;
            }
        } else if (type == kTypeIdentityRetired) {
            projection.live_identities.erase(delivery_id);
        } else if (type == kTypeEnqueued) {
            ReplyOutboxItem item;
            item.delivery_id = delivery_id;
            item.selection_id = GetJsonString(line, "selectionId");
            item.delivery_target = GetJsonString(line, "deliveryTarget");
            item.ordinal = GetJsonUint(line, "ordinal");
            item.reply_sha256 = GetJsonString(line, "replySha256");
            item.session_id = GetJsonString(line, "sessionId");
            item.turn_id = GetJsonString(line, "turnId");
            item.enqueued_at_ms = GetJsonInt(line, "enqueuedAtMs");
            item.published_path = GetJsonString(line, "publishedPath");
            item.state = "pending";
            // QQ 渠道 target 字段(Q2;本地族缺省空)。
            item.target_channel_id = GetJsonString(line, "targetChannelId");
            item.target_account_id = GetJsonString(line, "targetAccountId");
            item.target_conversation_id = GetJsonString(line, "targetConversationId");
            item.target_reply_to_message_id = GetJsonString(line, "targetReplyToMessageId");
            item.target_msg_seq = static_cast<std::uint32_t>(GetJsonUint(line, "targetMsgSeq"));
            item.source_ref = GetJsonString(line, "sourceRef");
            // Q4 出站附件(旧账行无这些键,缺省空 = 无附件)。
            item.attachment_local_path = GetJsonString(line, "attachmentLocalPath");
            item.attachment_file_name = GetJsonString(line, "attachmentFileName");
            item.attachment_mime_type = GetJsonString(line, "attachmentMimeType");
            item.attachment_size_bytes = GetJsonInt(line, "attachmentSizeBytes");
            item.attachment_sha256 = GetJsonString(line, "attachmentSha256");
            if (item.selection_id.empty() || item.reply_sha256.empty()) {
                ++projection.skipped_lines;
                continue;
            }
            projection.items[delivery_id] = std::move(item);
        } else if (type == kTypeAttempt) {
            const auto found = projection.items.find(delivery_id);
            if (found == projection.items.end()) {
                ++projection.skipped_lines;
                continue;
            }
            found->second.state = "sending";
            ++found->second.attempts;
        } else if (type == kTypeSent) {
            const auto found = projection.items.find(delivery_id);
            if (found == projection.items.end()) {
                ++projection.skipped_lines;
                continue;
            }
            found->second.state = "sent";
            found->second.provider_message_id = GetJsonString(line, "providerMessageId");
            found->second.delivered_at_ms = GetJsonInt(line, "sentAtMs");
        } else if (type == kTypeOutcomeUnknown) {
            const auto found = projection.items.find(delivery_id);
            if (found == projection.items.end()) {
                ++projection.skipped_lines;
                continue;
            }
            found->second.state = "delivery_unknown";
            found->second.delivery_error = "channel.delivery_unknown";
        } else if (type == kTypeChannelFailed) {
            const auto found = projection.items.find(delivery_id);
            if (found == projection.items.end()) {
                ++projection.skipped_lines;
                continue;
            }
            found->second.state = "failed";
            found->second.delivery_error = GetJsonString(line, "code");
        } else if (type == kTypeDelivered) {
            const auto found = projection.items.find(delivery_id);
            if (found == projection.items.end()) {
                ++projection.skipped_lines;
                continue;
            }
            found->second.state = "delivered";
            found->second.delivered_at_ms = GetJsonInt(line, "deliveredAtMs");
        } else if (type == kTypeFlagged) {
            const auto found = projection.items.find(delivery_id);
            if (found == projection.items.end()) {
                ++projection.skipped_lines;
                continue;
            }
            found->second.state = "flagged";
            found->second.flag_reason = GetJsonString(line, "reason");
        } else {
            ++projection.skipped_lines;
        }
    }
    return projection;
}

std::string MakeDeliveryId(const std::string& selection_id, const std::string& target,
                           std::uint64_t ordinal) {
    const std::string canonical = selection_id + "\n" + target + "\n" + std::to_string(ordinal);
    return "dl-" + platform::Sha256Hex(canonical).substr(0, 16);
}

std::string MakeChannelDeliveryTarget(const std::string& channel_id,
                                      const std::string& account_id,
                                      const std::string& conversation_id) {
    return "channel:" + channel_id + ":" + account_id + ":" + conversation_id;
}

std::vector<std::string> SplitReplySegments(const std::string& text, std::size_t max_bytes) {
    std::vector<std::string> segments;
    if (max_bytes == 0 || text.empty()) {
        return segments;  // 非法帽/空正文:无段(Q4 纯附件回复的空文本
                          // 段由 EnqueueChannel 的附件参数造,不发空消息)
    }
    if (text.size() <= max_bytes) {
        segments.push_back(text);
        return segments;
    }
    std::size_t cursor = 0;
    while (cursor < text.size()) {
        std::size_t end = std::min(cursor + max_bytes, text.size());
        if (end < text.size()) {
            // 不切断 UTF-8 多字节序列(§七"冻结正文"不能拼出半个字符):
            // 回退到边界字节(连续 10xx xxxx 的开头)。
            while (end > cursor && end < text.size() &&
                   (static_cast<unsigned char>(text[end]) & 0xC0) == 0x80) {
                --end;
            }
            if (end == cursor) {
                end = std::min(cursor + max_bytes, text.size());  // 防御:不无限回退
            } else {
                // 帽内尽量落换行(段界自然,不撕破行中词)。
                const std::size_t newline = text.rfind('\n', end - 1);
                if (newline != std::string::npos && newline > cursor &&
                    end - newline <= max_bytes / 2) {
                    end = newline + 1;
                }
            }
        }
        segments.push_back(text.substr(cursor, end - cursor));
        cursor = end;
    }
    return segments;
}

DurableReplyOutbox::~DurableReplyOutbox() = default;

// Q6:类内加了 mutex(渠道 turn 工作线程与泵 tick 并发),default move 会被
// 删除——手写,锁不搬(移动是装配期独占操作,无并发)。
DurableReplyOutbox::DurableReplyOutbox(DurableReplyOutbox&& other) noexcept
    : paths_(std::move(other.paths_)),
      writer_(std::move(other.writer_)),
      broken_(other.broken_),
      items_(std::move(other.items_)),
      live_identities_(std::move(other.live_identities_)),
      next_seq_by_identity_(std::move(other.next_seq_by_identity_)) {}

DurableReplyOutbox& DurableReplyOutbox::operator=(DurableReplyOutbox&& other) noexcept {
    if (this != &other) {
        paths_ = std::move(other.paths_);
        writer_ = std::move(other.writer_);
        broken_ = other.broken_;
        items_ = std::move(other.items_);
        live_identities_ = std::move(other.live_identities_);
        next_seq_by_identity_ = std::move(other.next_seq_by_identity_);
    }
    return *this;
}

DurableReplyOutbox::OpenResult DurableReplyOutbox::Open(DurableReplyOutbox* out,
                                                        const Paths& paths) {
    OpenResult result;
    if (out == nullptr) {
        result.error = "channel.outbox_full: out 为空(装配错)";
        return result;
    }
    std::error_code ec;
    std::filesystem::create_directories(paths.log_file.parent_path(), ec);
    if (ec && !paths.log_file.parent_path().empty()) {
        result.error = "建 outbox 目录失败: " + ec.message();
        return result;
    }
    std::filesystem::create_directories(paths.replies_dir, ec);
    std::filesystem::create_directories(paths.published_dir, ec);
    std::size_t skipped = 0;
    OutboxProjection projection = ReadOutboxProjection(paths.log_file);
    out->items_ = std::move(projection.items);
    out->live_identities_ = std::move(projection.live_identities);
    out->next_seq_by_identity_ = std::move(projection.next_seq_by_identity);
    // 升级兼容(A05):旧账行的 targetMsgSeq 是 ordinal 顶替的。没有显式
    // 身份分配行的项,把账面值折成 live 身份——重试沿用旧值不换号;发号
    // 计数同步取 max,新分配永不与在途旧号相撞。
    for (auto& [id, item] : out->items_) {
        if (item.delivery_target == "local:file" || item.target_msg_seq == 0) {
            continue;
        }
        if (out->live_identities_.count(id) > 0) {
            continue;  // 显式身份行优先
        }
        ChannelIdentityBookEntry entry;
        entry.identity.anchor_msg_id = item.target_reply_to_message_id;
        entry.identity.msg_seq = item.target_msg_seq;
        entry.identity.payload_sha256 = item.reply_sha256;
        entry.assigned_at_ms = item.enqueued_at_ms;
        out->live_identities_[id] = entry;
        const std::string key =
            DurableReplyOutbox::IdentityKey(item.target_account_id, entry.identity.anchor_msg_id);
        const auto max_seq = out->next_seq_by_identity_.find(key);
        if (max_seq == out->next_seq_by_identity_.end() ||
            entry.identity.msg_seq > max_seq->second) {
            out->next_seq_by_identity_[key] = entry.identity.msg_seq;
        }
    }
    skipped = projection.skipped_lines;
    out->paths_ = paths;
    out->writer_.reset();  // lazy:首笔提交才开写者(占位/只读故障首笔暴露)
    out->broken_ = false;
    result.ok = true;
    result.skipped_lines = skipped;
    return result;
}

std::filesystem::path DurableReplyOutbox::ReplyArtifactPath(const std::string& selection_id) const {
    return paths_.replies_dir / (selection_id + ".txt");
}

std::filesystem::path DurableReplyOutbox::PublishedPath(const std::string& delivery_id) const {
    return paths_.published_dir / (delivery_id + ".txt");
}

bool DurableReplyOutbox::AppendLinePowerLoss(const nlohmann::json& line) {
    if (broken_) {
        return false;
    }
    if (!writer_.has_value()) {
        auto writer = trajectory::JournalWriter::Open(paths_.log_file,
                                                      trajectory::JournalWriter::OpenMode::Append);
        if (!writer.has_value()) {
            broken_ = true;  // 账开不了:与首笔写失败同款处置(停投递)
            return false;
        }
        writer_ = std::move(*writer);
    }
    if (!writer_->AppendLine(line.dump(), trajectory::Durability::PowerLoss)) {
        broken_ = true;
        return false;
    }
    return true;
}

DurableReplyOutbox::EnqueueReceipt DurableReplyOutbox::Enqueue(const std::string& selection_id,
                                                               const std::string& reply_text,
                                                               const std::string& session_id,
                                                               const std::string& turn_id,
                                                               std::int64_t now_ms) {
    const std::lock_guard<std::mutex> outbox_lock(mutex_);
    EnqueueReceipt receipt;
    const std::string delivery_id = MakeDeliveryId(selection_id, "local:file", 1);
    receipt.delivery_id = delivery_id;
    const auto existing = items_.find(delivery_id);
    if (existing != items_.end()) {
        // 幂等投影:同 deliveryId 已入箱不重复入(§4.4 同一 committed
        // response 重建出相同 delivery_id)。正文不重写——入箱即冻结。
        receipt.duplicate = true;
        return receipt;
    }
    // 原件先落稳(§3 回复次序):replies/<selectionId>.txt。正文已在
    // (崩溃窗口:原件落了账行没落)且 hash 相符 = 可直接补账行;不符则
    // 拒(已提交原件不许被覆盖,§11.5)。
    const std::filesystem::path artifact = ReplyArtifactPath(selection_id);
    std::string reply_sha = platform::Sha256Hex(reply_text);
    if (const auto existing_text = ReadFileText(artifact); existing_text.has_value()) {
        if (platform::Sha256Hex(*existing_text) != reply_sha) {
            receipt.error_code = "outbox.artifact_failed";
            return receipt;  // 不覆盖原件,隔离给人工
        }
    } else {
        const auto write = platform::AtomicWriteFile(artifact, reply_text,
                                                     platform::WriteDurability::ProcessCrashDurability);
        if (!write.has_value()) {
            receipt.error_code = "outbox.artifact_failed";
            return receipt;
        }
    }
    const std::string published_rel =
        (std::filesystem::path("delivery") / "out" / (delivery_id + ".txt")).generic_string();
    nlohmann::json line = nlohmann::json::object();
    line["type"] = kTypeEnqueued;
    line["schemaVersion"] = 1;
    line["deliveryId"] = delivery_id;
    line["selectionId"] = selection_id;
    line["deliveryTarget"] = "local:file";
    line["ordinal"] = 1;
    line["replySha256"] = reply_sha;
    line["sessionId"] = session_id;
    line["turnId"] = turn_id;
    line["enqueuedAtMs"] = now_ms;
    line["publishedPath"] = published_rel;
    if (!AppendLinePowerLoss(line)) {
        receipt.error_code = "outbox.append_failed";
        return receipt;
    }
    ReplyOutboxItem item;
    item.delivery_id = delivery_id;
    item.selection_id = selection_id;
    item.delivery_target = "local:file";
    item.ordinal = 1;
    item.reply_text = reply_text;
    item.reply_sha256 = reply_sha;
    item.session_id = session_id;
    item.turn_id = turn_id;
    item.enqueued_at_ms = now_ms;
    item.published_path = published_rel;
    item.state = "pending";
    items_[delivery_id] = std::move(item);
    receipt.accepted = true;
    return receipt;
}

// ---- QQ 渠道族(Q2 §七) ----------------------------------------------------

DurableReplyOutbox::ChannelEnqueueReceipt DurableReplyOutbox::EnqueueChannel(
    const std::string& selection_id, const std::string& reply_text,
    const std::string& session_id, const std::string& turn_id, const ChannelTarget& target,
    std::int64_t now_ms, const ChannelAttachment* attachment) {
    const std::lock_guard<std::mutex> outbox_lock(mutex_);
    // 段帽:2000 字节(UTF-8 边界 + 换行偏好)。真平台的长度上限与
    // 计数口径(每条消息回复次数)归 Q3 实测校准,这里只做保守拆段。
    ChannelEnqueueReceipt receipt;
    std::vector<std::string> segments = SplitReplySegments(reply_text, kChannelSegmentBytes);
    if (attachment != nullptr) {
        // 附件独占一枚空文本末段(ordinal = 文本段数 + 1):QQ 富媒体消息
        // (msg_type=7)不带 content(官方示例口径)——正文全在前面自己的
        // 段里发,末段纯附件,谁也不吃掉谁。空正文 + 附件 = 只有这枚段。
        segments.emplace_back();
    }
    if (segments.empty()) {
        receipt.error_code = "outbox.segment_invalid";
        return receipt;
    }
    // 附件冻结校验(Q4):读原件算 sha256(入箱即冻结;原件丢失/读不了
    // = 明败——已提交产物不能猜)。空 attachment 不走这道。
    std::string attachment_sha;
    if (attachment != nullptr) {
        const auto bytes = ReadFileText(platform::Utf8ToPath(attachment->local_path));
        if (!bytes.has_value()) {
            receipt.error_code = "outbox.attachment_unreadable";
            return receipt;
        }
        attachment_sha = platform::Sha256Hex(*bytes);
    }
    const std::string target_str =
        MakeChannelDeliveryTarget(target.channel_id, target.account_id, target.conversation_id);
    for (std::size_t index = 0; index < segments.size(); ++index) {
        const std::uint64_t ordinal = index + 1;
        const std::string delivery_id = MakeDeliveryId(selection_id, target_str, ordinal);
        receipt.delivery_ids.push_back(delivery_id);
        const auto existing = items_.find(delivery_id);
        if (existing != items_.end()) {
            // 幂等投影:同段已入箱不重复入,正文不重写(入箱即冻结)。
            receipt.duplicate = true;
            continue;
        }
        // 段原件先落稳:replies/<deliveryId>.txt(段键;与本地族的
        // replies/<selectionId>.txt 全文原件并存,互不覆盖)。
        const std::filesystem::path artifact = paths_.replies_dir / (delivery_id + ".txt");
        const std::string segment_text = segments[index];
        const std::string segment_sha = platform::Sha256Hex(segment_text);
        if (const auto existing_text = ReadFileText(artifact); existing_text.has_value()) {
            if (platform::Sha256Hex(*existing_text) != segment_sha) {
                receipt.error_code = "outbox.artifact_failed";
                return receipt;  // 不覆盖已提交原件
            }
        } else {
            std::error_code ec;
            std::filesystem::create_directories(paths_.replies_dir, ec);
            const auto write =
                platform::AtomicWriteFile(artifact, segment_text,
                                          platform::WriteDurability::ProcessCrashDurability);
            if (!write.has_value()) {
                receipt.error_code = "outbox.artifact_failed";
                return receipt;
            }
        }
        nlohmann::json line = nlohmann::json::object();
        line["type"] = kTypeEnqueued;
        line["schemaVersion"] = 1;
        line["deliveryId"] = delivery_id;
        line["selectionId"] = selection_id;
        line["deliveryTarget"] = target_str;
        line["ordinal"] = ordinal;
        line["replySha256"] = segment_sha;
        line["sessionId"] = session_id;
        line["turnId"] = turn_id;
        line["enqueuedAtMs"] = now_ms;
        line["targetChannelId"] = target.channel_id;
        line["targetAccountId"] = target.account_id;
        line["targetConversationId"] = target.conversation_id;
        if (!target.reply_to_message_id.empty()) {
            line["targetReplyToMessageId"] = target.reply_to_message_id;
        }
        // A05:msg_seq 不再由 ordinal 顶替——首次网络发送前经
        // AssignChannelSendIdentity 按 (账号,锚) 持久发号(跨回复不撞号)。
        if (!target.source_ref.empty()) {
            line["sourceRef"] = target.source_ref;
        }
        // Q4:附件只挂末段(纯附件空文本段,正文在前面各段)。
        if (attachment != nullptr && index + 1 == segments.size()) {
            line["attachmentLocalPath"] = attachment->local_path;
            line["attachmentFileName"] = attachment->file_name;
            line["attachmentMimeType"] = attachment->mime_type;
            line["attachmentSizeBytes"] = attachment->size_bytes;
            line["attachmentSha256"] = attachment_sha;
        }
        if (!AppendLinePowerLoss(line)) {
            receipt.error_code = "outbox.append_failed";
            return receipt;
        }
        ReplyOutboxItem item;
        item.delivery_id = delivery_id;
        item.selection_id = selection_id;
        item.delivery_target = target_str;
        item.ordinal = ordinal;
        item.reply_text = segment_text;
        item.reply_sha256 = segment_sha;
        item.session_id = session_id;
        item.turn_id = turn_id;
        item.enqueued_at_ms = now_ms;
        item.state = "pending";
        item.target_channel_id = target.channel_id;
        item.target_account_id = target.account_id;
        item.target_conversation_id = target.conversation_id;
        item.target_reply_to_message_id = target.reply_to_message_id;
        item.target_msg_seq = 0;  // 未分配(A05:首次发送前 AssignChannelSendIdentity)
        item.source_ref = target.source_ref;
        if (attachment != nullptr && index + 1 == segments.size()) {
            item.attachment_local_path = attachment->local_path;
            item.attachment_file_name = attachment->file_name;
            item.attachment_mime_type = attachment->mime_type;
            item.attachment_size_bytes = attachment->size_bytes;
            item.attachment_sha256 = attachment_sha;
        }
        items_[delivery_id] = std::move(item);
        receipt.accepted = true;
    }
    if (!receipt.error_code.empty()) {
        // 中途失败(段原件/账行写不进):不冒充部分成功——accepted 收回,
        // 已入箱的段靠同 selection 幂等重入续齐(调用方停泵后恢复路接管)。
        receipt.accepted = false;
        receipt.delivery_ids.clear();
    }
    return receipt;
}

bool DurableReplyOutbox::RecordAttempt(const std::string& delivery_id, std::int64_t now_ms) {
    const std::lock_guard<std::mutex> outbox_lock(mutex_);
    const auto found = items_.find(delivery_id);
    if (found == items_.end() || broken_) {
        return false;
    }
    ReplyOutboxItem& item = found->second;
    if (item.state != "pending" && item.state != "sending") {
        return false;  // 终态不再发
    }
    // 发出前记尝试(§七:先账后网络——崩在发出后,恢复路看见 sending 无
    // 回执,按超时/unknown 处置,不假称没发过)。
    nlohmann::json line = nlohmann::json::object();
    line["type"] = kTypeAttempt;
    line["schemaVersion"] = 1;
    line["deliveryId"] = delivery_id;
    line["attempt"] = item.attempts + 1;
    line["atMs"] = now_ms;
    if (!AppendLinePowerLoss(line)) {
        return false;
    }
    ++item.attempts;
    item.state = "sending";
    return true;
}

bool DurableReplyOutbox::MarkSent(const std::string& delivery_id,
                                  const std::string& provider_message_id, std::int64_t now_ms) {
    const std::lock_guard<std::mutex> outbox_lock(mutex_);
    const auto found = items_.find(delivery_id);
    if (found == items_.end() || broken_) {
        return false;
    }
    ReplyOutboxItem& item = found->second;
    if (item.state == "sent") {
        return true;  // 幂等:重复回执只收一次
    }
    if (item.state != "sending") {
        return false;
    }
    nlohmann::json line = nlohmann::json::object();
    line["type"] = kTypeSent;
    line["schemaVersion"] = 1;
    line["deliveryId"] = delivery_id;
    line["providerMessageId"] = provider_message_id;
    line["sentAtMs"] = now_ms;
    if (!AppendLinePowerLoss(line)) {
        return false;
    }
    item.state = "sent";
    item.provider_message_id = provider_message_id;
    item.delivered_at_ms = now_ms;
    return true;
}

bool DurableReplyOutbox::MarkOutcomeUnknown(const std::string& delivery_id, std::int64_t now_ms) {
    const std::lock_guard<std::mutex> outbox_lock(mutex_);
    const auto found = items_.find(delivery_id);
    if (found == items_.end() || broken_) {
        return false;
    }
    ReplyOutboxItem& item = found->second;
    if (item.state != "sending") {
        return false;
    }
    nlohmann::json line = nlohmann::json::object();
    line["type"] = kTypeOutcomeUnknown;
    line["schemaVersion"] = 1;
    line["deliveryId"] = delivery_id;
    line["atMs"] = now_ms;
    if (!AppendLinePowerLoss(line)) {
        return false;
    }
    item.state = "delivery_unknown";
    item.delivery_error = "channel.delivery_unknown";
    return true;
}

bool DurableReplyOutbox::MarkChannelFailed(const std::string& delivery_id,
                                           const std::string& error_code, std::int64_t now_ms) {
    const std::lock_guard<std::mutex> outbox_lock(mutex_);
    const auto found = items_.find(delivery_id);
    if (found == items_.end() || broken_) {
        return false;
    }
    ReplyOutboxItem& item = found->second;
    if (item.state == "failed" || item.state == "sent" || item.state == "delivery_unknown") {
        return false;  // 终态不翻转
    }
    nlohmann::json line = nlohmann::json::object();
    line["type"] = kTypeChannelFailed;
    line["schemaVersion"] = 1;
    line["deliveryId"] = delivery_id;
    line["code"] = error_code;
    line["atMs"] = now_ms;
    if (!AppendLinePowerLoss(line)) {
        return false;
    }
    item.state = "failed";
    item.delivery_error = error_code;
    return true;
}

std::string DurableReplyOutbox::IdentityKey(const std::string& account_id,
                                             const std::string& anchor) {
    return account_id + "\n" + anchor;
}

DurableReplyOutbox::IdentityReceipt DurableReplyOutbox::AssignChannelSendIdentity(
    const std::string& delivery_id, const std::string& account_id,
    const std::string& anchor_msg_id, const std::string& payload_sha256, std::int64_t now_ms) {
    const std::lock_guard<std::mutex> outbox_lock(mutex_);
    IdentityReceipt receipt;
    if (delivery_id.empty() || account_id.empty()) {
        receipt.error_code = "identity_invalid_args";
        return receipt;
    }
    const auto live = live_identities_.find(delivery_id);
    if (live != live_identities_.end()) {
        // 幂等/裁决合同(A06):live 身份在身——锚一致复用(结果未知的
        // 重试恒同 (msg_id,msg_seq),平台去重兜底);锚变了必须先裁决。
        if (live->second.identity.anchor_msg_id != anchor_msg_id) {
            receipt.error_code = "identity_anchor_conflict";
            return receipt;
        }
        if (!live->second.identity.payload_sha256.empty() &&
            live->second.identity.payload_sha256 != payload_sha256) {
            receipt.error_code = "identity_payload_mismatch";
            return receipt;
        }
        receipt.ok = true;
        receipt.identity = live->second.identity;
        return receipt;
    }
    // 新发号:主动消息(空锚)不带 seq(0);被动消息按 (账号,锚) 单调 +1。
    ChannelSendIdentity identity;
    identity.anchor_msg_id = anchor_msg_id;
    identity.payload_sha256 = payload_sha256;
    std::string counter_key;
    std::uint32_t counter_next = 0;
    if (!anchor_msg_id.empty()) {
        counter_key = IdentityKey(account_id, anchor_msg_id);
        counter_next =
            (next_seq_by_identity_.count(counter_key) > 0
                 ? next_seq_by_identity_.at(counter_key)
                 : 0) +
            1;
        identity.msg_seq = counter_next;
    }
    nlohmann::json line = nlohmann::json::object();
    line["type"] = kTypeIdentityAssigned;
    line["schemaVersion"] = 1;
    line["deliveryId"] = delivery_id;
    line["accountId"] = account_id;
    line["msgId"] = anchor_msg_id;
    line["msgSeq"] = identity.msg_seq;
    line["payloadSha256"] = payload_sha256;
    line["atMs"] = now_ms;
    if (!AppendLinePowerLoss(line)) {
        receipt.error_code = "outbox.append_failed";
        return receipt;  // 账写不进:号未确认,内存计数不动(重试同号)
    }
    if (!counter_key.empty()) {
        next_seq_by_identity_[counter_key] = counter_next;
    }
    live_identities_[delivery_id] = ChannelIdentityBookEntry{identity, now_ms};
    // 项上的投影同步(item 字段是泵消费的视图;账行不可改,身份行为准)。
    const auto item = items_.find(delivery_id);
    if (item != items_.end()) {
        item->second.target_reply_to_message_id = identity.anchor_msg_id;
        item->second.target_msg_seq = identity.msg_seq;
    }
    receipt.ok = true;
    receipt.identity = identity;
    return receipt;
}

bool DurableReplyOutbox::RetireChannelSendIdentity(const std::string& delivery_id,
                                                   const std::string& reason,
                                                   std::int64_t now_ms) {
    const std::lock_guard<std::mutex> outbox_lock(mutex_);
    if (live_identities_.count(delivery_id) == 0) {
        return true;  // 幂等:无 live 身份即已裁决
    }
    nlohmann::json line = nlohmann::json::object();
    line["type"] = kTypeIdentityRetired;
    line["schemaVersion"] = 1;
    line["deliveryId"] = delivery_id;
    line["reason"] = reason;
    line["atMs"] = now_ms;
    if (!AppendLinePowerLoss(line)) {
        return false;
    }
    live_identities_.erase(delivery_id);
    const auto item = items_.find(delivery_id);
    if (item != items_.end()) {
        item->second.target_msg_seq = 0;  // 下一轮分配换新身份(账里留旧迹)
    }
    return true;
}

std::optional<ChannelSendIdentity>
DurableReplyOutbox::FindLiveChannelSendIdentity(const std::string& delivery_id) const {
    const std::lock_guard<std::mutex> outbox_lock(mutex_);
    const auto found = live_identities_.find(delivery_id);
    if (found == live_identities_.end()) {
        return std::nullopt;
    }
    return found->second.identity;
}

std::vector<ReplyOutboxItem> DurableReplyOutbox::PendingChannelItems() const {
    const std::lock_guard<std::mutex> outbox_lock(mutex_);
    std::vector<ReplyOutboxItem> out;
    for (const auto& [id, item] : items_) {
        if (item.delivery_target == "local:file") {
            continue;
        }
        if (item.state == "pending" || item.state == "sending") {
            ReplyOutboxItem copy = item;
            // live 身份覆盖视图(A06:重试读到的恒是冻结身份,不因内存
            // item 字段旧值漂移)。
            const auto live = live_identities_.find(id);
            if (live != live_identities_.end()) {
                copy.target_reply_to_message_id = live->second.identity.anchor_msg_id;
                copy.target_msg_seq = live->second.identity.msg_seq;
            }
            out.push_back(std::move(copy));
        }
    }
    return out;
}

bool DurableReplyOutbox::LoadChannelItemText(const std::string& delivery_id,
                                             std::string* text) const {
    const std::lock_guard<std::mutex> outbox_lock(mutex_);
    if (text == nullptr) {
        return false;
    }
    const auto found = items_.find(delivery_id);
    if (found == items_.end()) {
        return false;
    }
    if (!found->second.reply_text.empty()) {
        *text = found->second.reply_text;
        return true;
    }
    const auto artifact_text = ReadFileText(paths_.replies_dir / (delivery_id + ".txt"));
    if (!artifact_text.has_value() ||
        platform::Sha256Hex(*artifact_text) != found->second.reply_sha256) {
        return false;  // 原件丢失/损坏:隔离给人工,不猜正文
    }
    *text = *artifact_text;
    return true;
}

DurableReplyOutbox::DeliverResult DurableReplyOutbox::DeliverPending(std::int64_t now_ms) {
    const std::lock_guard<std::mutex> outbox_lock(mutex_);
    DeliverResult result;
    for (auto& [id, item] : items_) {
        if (item.state != "pending") continue;
        if (item.delivery_target != "local:file") {
            continue;  // QQ 渠道族:归 ChannelWorkPump 的桥投递路,不发本地文件
        }
        if (broken_) {
            result.error = "channel.outbox_full: outbox 账 broken(写盘失败停止投递)";
            break;
        }
        // 正文来源:入箱冻结的正文;内存没有(重开后)从原件读回并核 hash。
        std::string text = item.reply_text;
        if (text.empty()) {
            const auto artifact_text = ReadFileText(ReplyArtifactPath(item.selection_id));
            if (!artifact_text.has_value() ||
                platform::Sha256Hex(*artifact_text) != item.reply_sha256) {
                // 原件丢失/损坏:已提交原件丢失即隔离,不跳过继续(§11.5)。
                nlohmann::json flag = nlohmann::json::object();
                flag["type"] = kTypeFlagged;
                flag["schemaVersion"] = 1;
                flag["deliveryId"] = id;
                flag["reason"] = "reply_artifact_missing_or_corrupt";
                flag["flaggedAtMs"] = now_ms;
                if (AppendLinePowerLoss(flag)) {
                    item.state = "flagged";
                    item.flag_reason = "reply_artifact_missing_or_corrupt";
                    ++result.flagged;
                } else {
                    result.error = "channel.outbox_full: flagged 行落不了盘";
                }
                continue;
            }
            text = *artifact_text;
        }
        const std::filesystem::path published = PublishedPath(id);
        const auto existing = ReadFileText(published);
        if (existing.has_value()) {
            if (platform::Sha256Hex(*existing) != item.reply_sha256) {
                // 文件在但内容对不上:不动它,flagged 留人工(不删用户的文件)。
                nlohmann::json flag = nlohmann::json::object();
                flag["type"] = kTypeFlagged;
                flag["schemaVersion"] = 1;
                flag["deliveryId"] = id;
                flag["reason"] = "published_file_hash_mismatch";
                flag["flaggedAtMs"] = now_ms;
                if (AppendLinePowerLoss(flag)) {
                    item.state = "flagged";
                    item.flag_reason = "published_file_hash_mismatch";
                    ++result.flagged;
                } else {
                    result.error = "channel.outbox_full: flagged 行落不了盘";
                }
                continue;
            }
            // 窗口:文件已发布、回执未落——补回执,不出第二份(§11 故障
            // 清单"本地文件已发布,receipt 未落")。
        } else {
            const auto write = platform::AtomicWriteFile(published, text,
                                                          platform::WriteDurability::ProcessCrashDurability);
            if (!write.has_value()) {
                result.error = "outbox.artifact_failed: 发布文件写不进(" + write.error().code +
                               ")";
                continue;  // 留 pending,下轮重试;不冒充已发布
            }
        }
        nlohmann::json line = nlohmann::json::object();
        line["type"] = kTypeDelivered;
        line["schemaVersion"] = 1;
        line["deliveryId"] = id;
        line["deliveredAtMs"] = now_ms;
        if (!AppendLinePowerLoss(line)) {
            result.error = "channel.outbox_full: delivered 行落不了盘(写盘失败停止投递)";
            break;
        }
        item.state = "delivered";
        item.delivered_at_ms = now_ms;
        ++result.delivered;
    }
    for (const auto& [id, item] : items_) {
        if (item.state == "pending") {
            ++result.pending;
        }
    }
    return result;
}

std::vector<ReplyOutboxItem> DurableReplyOutbox::ListItems() const {
    const std::lock_guard<std::mutex> outbox_lock(mutex_);
    std::vector<ReplyOutboxItem> items;
    items.reserve(items_.size());
    for (const auto& [id, item] : items_) {
        items.push_back(item);
    }
    return items;
}

std::optional<ReplyOutboxItem> DurableReplyOutbox::Find(const std::string& delivery_id) const {
    const std::lock_guard<std::mutex> outbox_lock(mutex_);
    const auto found = items_.find(delivery_id);
    if (found == items_.end()) {
        return std::nullopt;
    }
    return found->second;
}

std::size_t DurableReplyOutbox::PendingCount() const {
    std::size_t count = 0;
    for (const auto& [id, item] : items_) {
        if (item.state == "pending") {
            ++count;
        }
    }
    return count;
}

}  // namespace lubancode::gateway
