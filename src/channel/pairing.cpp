#include "channel/pairing.hpp"

#include <array>
#include <chrono>
#include <fstream>
#include <random>

#include <nlohmann/json.hpp>

#include "channel/digest.hpp"
#include "platform/atomic_write.hpp"  // 统一原子写(审计 P1:旧写法先删正式账再换名)
#include "platform/paths.hpp"

namespace lubancode::channel {

namespace {

const char* PairingStatusName(PairingStore::Record::Status status) {
    switch (status) {
        case PairingStore::Record::Status::Pending: return "pending";
        case PairingStore::Record::Status::Approved: return "approved";
        case PairingStore::Record::Status::Rejected: return "rejected";
        case PairingStore::Record::Status::Expired: return "expired";
    }
    return "unknown";
}

std::optional<PairingStore::Record::Status> PairingStatusFromName(const std::string& name) {
    if (name == "pending") return PairingStore::Record::Status::Pending;
    if (name == "approved") return PairingStore::Record::Status::Approved;
    if (name == "rejected") return PairingStore::Record::Status::Rejected;
    if (name == "expired") return PairingStore::Record::Status::Expired;
    return std::nullopt;
}

nlohmann::json RecordToJson(const PairingStore::Record& record) {
    nlohmann::json json = nlohmann::json::object();
    json["channel_id"] = record.channel_id;
    json["account_id"] = record.account_id;
    json["sender_id"] = record.sender_id;
    json["code_hash"] = record.code_hash;
    json["created_at_ms"] = record.created_at_ms;
    json["expires_at_ms"] = record.expires_at_ms;
    json["status"] = PairingStatusName(record.status);
    return json;
}

std::optional<PairingStore::Record> RecordFromJson(const nlohmann::json& json) {
    const std::array<const char*, 7> kRequired = {"channel_id", "account_id", "sender_id",
                                                  "code_hash",  "created_at_ms",
                                                  "expires_at_ms", "status"};
    for (const char* key : kRequired) {
        if (!json.contains(key)) return std::nullopt;
    }
    if (!json["channel_id"].is_string() || !json["account_id"].is_string() ||
        !json["sender_id"].is_string() || !json["code_hash"].is_string() ||
        !json["status"].is_string() || !json["created_at_ms"].is_number_integer() ||
        !json["expires_at_ms"].is_number_integer()) {
        return std::nullopt;
    }
    const auto status = PairingStatusFromName(json["status"].get<std::string>());
    if (!status.has_value()) return std::nullopt;
    PairingStore::Record record;
    record.channel_id = json["channel_id"].get<std::string>();
    record.account_id = json["account_id"].get<std::string>();
    record.sender_id = json["sender_id"].get<std::string>();
    record.code_hash = json["code_hash"].get<std::string>();
    record.created_at_ms = json["created_at_ms"].get<std::int64_t>();
    record.expires_at_ms = json["expires_at_ms"].get<std::int64_t>();
    record.status = *status;
    return record;
}

nlohmann::json NoticeLogToJson(const PairingStore::NoticeLogEntry& entry) {
    nlohmann::json json = nlohmann::json::object();
    json["sender_id"] = entry.sender_id;
    json["conversation_id"] = entry.conversation_id;
    json["notified_at_ms"] = entry.notified_at_ms;
    return json;
}

std::optional<PairingStore::NoticeLogEntry> NoticeLogFromJson(const nlohmann::json& json) {
    if (!json.is_object() || !json.contains("sender_id") || !json.contains("notified_at_ms")) {
        return std::nullopt;
    }
    if (!json["sender_id"].is_string() || !json["notified_at_ms"].is_number_integer()) {
        return std::nullopt;
    }
    PairingStore::NoticeLogEntry entry;
    entry.sender_id = json["sender_id"].get<std::string>();
    entry.notified_at_ms = json["notified_at_ms"].get<std::int64_t>();
    // 会话字段旧账可缺(Q1b 之前无此账)。
    if (json.contains("conversation_id") && json["conversation_id"].is_string()) {
        entry.conversation_id = json["conversation_id"].get<std::string>();
    }
    return entry;
}

// 快照正文:v2 对象(records + notices);读侧兼容旧裸数组(Q1b 之前的
// 账只有 records,notices 读作空——旧进程从没发过提示,空账即事实)。
nlohmann::json SnapshotToJson(const std::vector<PairingStore::Record>& records,
                              const std::vector<PairingStore::NoticeLogEntry>& notices) {
    nlohmann::json json = nlohmann::json::object();
    json["schema_version"] = 2;
    nlohmann::json record_array = nlohmann::json::array();
    for (const auto& record : records) {
        record_array.push_back(RecordToJson(record));
    }
    json["records"] = std::move(record_array);
    nlohmann::json notice_array = nlohmann::json::array();
    for (const auto& entry : notices) {
        notice_array.push_back(NoticeLogToJson(entry));
    }
    json["notices"] = std::move(notice_array);
    return json;
}

}  // namespace

std::string PairingStore::DefaultCodeGenerator() {
    static const char kAlphabet[] = "ABCDEFGHJKLMNPQRSTUVWXYZ23456789";
    static thread_local std::mt19937 generator(std::random_device{}());
    std::uniform_int_distribution<std::size_t> pick(0, sizeof(kAlphabet) - 2);
    std::string code;
    code.reserve(kPairingCodeLength);
    for (std::size_t i = 0; i < kPairingCodeLength; ++i) {
        code.push_back(kAlphabet[pick(generator)]);
    }
    return code;
}

std::unique_ptr<PairingStore> PairingStore::Open(const std::filesystem::path& account_dir,
                                                  std::string channel_id,
                                                  std::string account_id) {
    auto store = std::make_unique<PairingStore>();
    store->channel_id_ = std::move(channel_id);
    store->account_id_ = std::move(account_id);
    store->pairing_path_ = account_dir / "pairing.json";

    std::error_code ec;
    std::filesystem::create_directories(account_dir, ec);
    if (ec) {
        store->write_blocked_ = true;
        store->last_error_ = "建目录 " + platform::PathToUtf8(account_dir) + " 失败: " + ec.message();
        return store;
    }
    if (!std::filesystem::exists(store->pairing_path_, ec) || ec) {
        return store;  // 新账
    }
    std::ifstream stream(store->pairing_path_, std::ios::binary);
    if (!stream) {
        store->write_blocked_ = true;
        store->last_error_ = "pairing 账打不开: " + platform::PathToUtf8(store->pairing_path_);
        return store;
    }
    try {
        const nlohmann::json parsed = nlohmann::json::parse(stream);
        if (parsed.is_array()) {
            // 旧格式(Q1b 前):裸数组只有 records。
            for (const auto& item : parsed) {
                auto record = RecordFromJson(item);
                if (record.has_value()) {
                    store->records_.push_back(std::move(*record));
                }
            }
        } else if (parsed.is_object() && parsed.contains("records") &&
                   parsed["records"].is_array()) {
            for (const auto& item : parsed["records"]) {
                auto record = RecordFromJson(item);
                if (record.has_value()) {
                    store->records_.push_back(std::move(*record));
                }
            }
            if (parsed.contains("notices") && parsed["notices"].is_array()) {
                for (const auto& item : parsed["notices"]) {
                    auto entry = NoticeLogFromJson(item);
                    if (entry.has_value()) {
                        store->notice_log_.push_back(std::move(*entry));
                    }
                }
            }
        }
    } catch (const nlohmann::json::exception&) {
        // 快照坏了:不装死,清账重来——pairing 是短命旁账,重新申请的代价
        // 是一条新 code,远小于拒收所有远端用户。
        store->write_blocked_ = false;
        store->last_error_ = "pairing 账读不懂,已按空账重开(旧待审作废)";
        store->records_.clear();
    }
    return store;
}

bool PairingStore::SaveLocked() {
    if (write_blocked_) return false;
    const std::string text = SnapshotToJson(records_, notice_log_).dump();
    // 原子写,统一走 platform::AtomicWriteFile(旧写法先 remove 正式账再
    // rename,每次保存都留出账本不存在的窗口;新写法平台原子替换,失败
    // 不动正式账)。
    const auto written = platform::AtomicWriteFile(pairing_path_, text);
    if (!written.has_value()) {
        write_blocked_ = true;
        last_error_ = "pairing 账落盘失败: " + written.error().message;
        return false;
    }
    return true;
}

std::optional<std::string> PairingStore::RequestPairing(const std::string& sender_id,
                                                        std::int64_t now_ms,
                                                        const CodeGenerator& generator) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (write_blocked_) return std::nullopt;

    // 已批准的 sender 不再发 code(直接放行是路由层的事)。
    for (const auto& record : records_) {
        if (record.sender_id == sender_id && record.status == Record::Status::Approved) {
            last_error_ = "already_approved";
            return std::nullopt;
        }
    }
    // 被拒过的 sender 不再发 code(Q1b:被拒/黑名单不提示不返码——
    // 否则拒收一个人却让他每 30 秒领一枚新码,账越滚越长)。
    for (const auto& record : records_) {
        if (record.sender_id == sender_id && record.status == Record::Status::Rejected) {
            last_error_ = "sender_rejected";
            return std::nullopt;
        }
    }
    // 重复申请限速:同 sender 的 pending 记录还在冷却期内即拒。
    for (const auto& record : records_) {
        if (record.sender_id == sender_id && record.status == Record::Status::Pending &&
            now_ms - record.created_at_ms < kPairingRequestCooldownMs) {
            last_error_ = "rate_limited";
            return std::nullopt;
        }
    }
    // 同 sender 旧 pending 作废(一枚 sender 同时至多一枚活 code)。
    for (auto& record : records_) {
        if (record.sender_id == sender_id && record.status == Record::Status::Pending) {
            record.status = Record::Status::Expired;
        }
    }

    const std::string code = generator ? generator() : DefaultCodeGenerator();
    Record record;
    record.channel_id = channel_id_;
    record.account_id = account_id_;
    record.sender_id = sender_id;
    record.code_hash = Sha256Hex(code);
    record.created_at_ms = now_ms;
    record.expires_at_ms = now_ms + kPairingCodeTtlMs;
    record.status = Record::Status::Pending;
    records_.push_back(record);

    if (!SaveLocked()) return std::nullopt;
    return code;
}

// 结算一枚 pending 记录(批准/拒绝共用;调用方已持 mutex_)。
std::optional<std::string> PairingStore::FinalizeLatestPending(Record& record,
                                                               std::int64_t now_ms,
                                                               Record::Status target,
                                                               std::string* error) {
    if (record.status != Record::Status::Pending) {
        if (error != nullptr) *error = "already_finalized";
        return std::nullopt;
    }
    if (now_ms >= record.expires_at_ms) {
        record.status = Record::Status::Expired;
        SaveLocked();
        if (error != nullptr) *error = "expired";
        return std::nullopt;
    }
    record.status = target;
    if (!SaveLocked()) {
        // 快照没写进去:内存也不算数,回滚,免得"批准了但重启就丢"。
        record.status = Record::Status::Pending;
        if (error != nullptr) *error = last_error_;
        return std::nullopt;
    }
    return record.sender_id;
}

std::optional<std::string> PairingStore::FinalizeByCode(const std::string& code,
                                                        std::int64_t now_ms,
                                                        Record::Status target,
                                                        std::string* sender_out,
                                                        std::string* error) {
    const std::string hash = Sha256Hex(code);
    for (auto& record : records_) {
        if (record.code_hash != hash) continue;
        const auto sender = FinalizeLatestPending(record, now_ms, target, error);
        if (sender.has_value() && sender_out != nullptr) {
            *sender_out = *sender;
        }
        return sender;
    }
    if (error != nullptr) *error = "not_found";
    return std::nullopt;
}

std::optional<std::string> PairingStore::ApproveSender(const std::string& sender_id,
                                                       std::int64_t now_ms,
                                                       std::string* error) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (write_blocked_) return std::nullopt;
    // 最新一枚 pending(records 追加序,倒找第一枚)。
    for (auto it = records_.rbegin(); it != records_.rend(); ++it) {
        if (it->sender_id != sender_id || it->status != Record::Status::Pending) continue;
        return FinalizeLatestPending(*it, now_ms, Record::Status::Approved, error);
    }
    // 没有 pending:按既成事实分型(已批准过/被拒过 = already_finalized,
    // 根本没有这个人 = not_found)。
    bool saw_finalized = false;
    for (const auto& record : records_) {
        if (record.sender_id != sender_id) continue;
        if (record.status == Record::Status::Approved ||
            record.status == Record::Status::Rejected) {
            saw_finalized = true;
        }
    }
    if (error != nullptr) {
        *error = saw_finalized ? std::string("already_finalized") : std::string("not_found");
    }
    return std::nullopt;
}

std::optional<std::string> PairingStore::RejectSender(const std::string& sender_id,
                                                      std::int64_t now_ms, std::string* error) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (write_blocked_) return std::nullopt;
    for (auto it = records_.rbegin(); it != records_.rend(); ++it) {
        if (it->sender_id != sender_id || it->status != Record::Status::Pending) continue;
        return FinalizeLatestPending(*it, now_ms, Record::Status::Rejected, error);
    }
    // 没有 pending:与批准路同款分型。
    bool saw_finalized = false;
    for (const auto& record : records_) {
        if (record.sender_id != sender_id) continue;
        if (record.status == Record::Status::Approved ||
            record.status == Record::Status::Rejected) {
            saw_finalized = true;
        }
    }
    if (error != nullptr) {
        *error = saw_finalized ? std::string("already_finalized") : std::string("not_found");
    }
    return std::nullopt;
}

bool PairingStore::IsSenderRejected(const std::string& sender_id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    for (const auto& record : records_) {
        if (record.sender_id == sender_id && record.status == Record::Status::Rejected) {
            return true;
        }
    }
    return false;
}

bool PairingStore::MarkNoticeSent(const std::string& sender_id,
                                  const std::string& conversation_id, std::int64_t now_ms) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (write_blocked_) return false;
    for (const auto& entry : notice_log_) {
        if (entry.sender_id == sender_id &&
            now_ms - entry.notified_at_ms < kPairingNoticeCooldownMs) {
            return false;  // 冷却窗内:不再提示(账不动)
        }
    }
    bool updated = false;
    for (auto& entry : notice_log_) {
        if (entry.sender_id == sender_id) {
            entry.conversation_id = conversation_id;
            entry.notified_at_ms = now_ms;
            updated = true;
            break;
        }
    }
    if (!updated) {
        NoticeLogEntry entry;
        entry.sender_id = sender_id;
        entry.conversation_id = conversation_id;
        entry.notified_at_ms = now_ms;
        notice_log_.push_back(std::move(entry));
    }
    return SaveLocked();
}

std::optional<std::string> PairingStore::Approve(const std::string& code, std::int64_t now_ms) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (write_blocked_) return std::nullopt;
    std::string sender;
    return FinalizeByCode(code, now_ms, Record::Status::Approved, &sender, &last_error_);
}

std::optional<std::string> PairingStore::Reject(const std::string& code, std::int64_t now_ms,
                                                std::string* error) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (write_blocked_) return std::nullopt;
    std::string sender;
    return FinalizeByCode(code, now_ms, Record::Status::Rejected, &sender, error);
}

bool PairingStore::IsSenderApproved(const std::string& sender_id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    for (const auto& record : records_) {
        if (record.sender_id == sender_id && record.status == Record::Status::Approved) {
            return true;
        }
    }
    return false;
}

std::vector<PairingStore::PendingView> PairingStore::PendingList(std::int64_t now_ms) const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<PendingView> out;
    for (const auto& record : records_) {
        if (record.status != Record::Status::Pending) continue;
        if (now_ms >= record.expires_at_ms) continue;
        PendingView view;
        view.sender_id = record.sender_id;
        view.expires_at_ms = record.expires_at_ms;
        out.push_back(std::move(view));
    }
    return out;
}

std::size_t PairingStore::approved_count() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::size_t count = 0;
    for (const auto& record : records_) {
        if (record.status == Record::Status::Approved) ++count;
    }
    return count;
}

std::vector<PairingStore::Record> PairingStore::Records() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return records_;
}

std::vector<PairingStore::NoticeLogEntry> PairingStore::NoticeLog() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return notice_log_;
}

PairingStore::Projection PairingStore::ReadProjection(const std::filesystem::path& account_dir) {
    // 只读观测面(channel status 四态展示):零建目录零写盘,坏账如实报
    // parse_ok=false,不冒充"0 个已批准"。
    Projection projection;
    const std::filesystem::path pairing_file = account_dir / "pairing.json";
    std::error_code ec;
    if (!std::filesystem::is_regular_file(pairing_file, ec) || ec) {
        return projection;  // present=false:账还没立(没人申请过配对)
    }
    projection.present = true;
    std::ifstream stream(pairing_file, std::ios::binary);
    if (!stream) {
        projection.parse_ok = false;
        return projection;
    }
    try {
        const nlohmann::json parsed = nlohmann::json::parse(stream);
        const nlohmann::json* records = nullptr;
        if (parsed.is_array()) {
            records = &parsed;  // 旧格式
        } else if (parsed.is_object() && parsed.contains("records") &&
                   parsed["records"].is_array()) {
            records = &parsed["records"];
        } else {
            projection.parse_ok = false;
            return projection;
        }
        const std::int64_t now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                        std::chrono::system_clock::now().time_since_epoch())
                                        .count();
        for (const auto& item : *records) {
            auto record = RecordFromJson(item);
            if (!record.has_value()) continue;
            if (record->status == Record::Status::Approved) {
                ++projection.approved;
            } else if (record->status == Record::Status::Pending &&
                       now_ms < record->expires_at_ms) {
                ++projection.pending;
            }
        }
    } catch (const nlohmann::json::exception&) {
        projection.parse_ok = false;
    }
    return projection;
}

std::string MakePairingNoticeText(const std::string& code, const std::string& channel_id,
                                  const std::string& account_id) {
    // 文案要素:为什么没被处理 / 配对码与有效期 / 管理员怎么批 / 批准后
    // 怎么继续。批准后原消息不自动执行——用户须重发,单内原文写明。
    std::string text;
    text += "这条消息没有交给助理处理:你还没有完成身份配对。\n";
    text += "配对码: " + code + "(5 分钟内有效,一次性)。\n";
    text += "请把这串码告诉机器人管理员,由管理员在本机执行:\n";
    text += "lubancode channel pairing approve " + channel_id + " " + account_id + " " + code;
    text += "\n批准后重新发送你的消息即可(已发的这条不会自动补跑)。";
    return text;
}

}  // namespace lubancode::channel
