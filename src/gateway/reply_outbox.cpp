// DurableReplyOutbox 实现(常驻总装 V1)。合同见头文件与 contracts.md §3/§4.4。
#include "gateway/reply_outbox.hpp"

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
        if (type == kTypeEnqueued) {
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
            if (item.selection_id.empty() || item.reply_sha256.empty()) {
                ++projection.skipped_lines;
                continue;
            }
            projection.items[delivery_id] = std::move(item);
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

DurableReplyOutbox::~DurableReplyOutbox() = default;

DurableReplyOutbox::DurableReplyOutbox(DurableReplyOutbox&&) noexcept = default;

DurableReplyOutbox& DurableReplyOutbox::operator=(DurableReplyOutbox&&) noexcept = default;

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
        std::filesystem::path("delivery") / "out" / (delivery_id + ".txt").generic_string();
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

DurableReplyOutbox::DeliverResult DurableReplyOutbox::DeliverPending(std::int64_t now_ms) {
    DeliverResult result;
    for (auto& [id, item] : items_) {
        if (item.state != "pending") continue;
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
    std::vector<ReplyOutboxItem> items;
    items.reserve(items_.size());
    for (const auto& [id, item] : items_) {
        items.push_back(item);
    }
    return items;
}

std::optional<ReplyOutboxItem> DurableReplyOutbox::Find(const std::string& delivery_id) const {
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
