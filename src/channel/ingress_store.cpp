#include "channel/ingress_store.hpp"

#include <algorithm>
#include <array>
#include <fstream>

#include "channel/digest.hpp"
#include "platform/paths.hpp"

namespace lubancode::channel {

const char* IngressEventStateName(IngressEventState state) {
    switch (state) {
        case IngressEventState::Durable: return "durable";
        case IngressEventState::Authorized: return "authorized";
        case IngressEventState::Rejected: return "rejected";
        case IngressEventState::Routed: return "routed";
        case IngressEventState::Queued: return "queued";
        case IngressEventState::Running: return "running";
        case IngressEventState::Replied: return "replied";
        case IngressEventState::CompletedWithoutReply: return "completed_without_reply";
        case IngressEventState::Delivered: return "delivered";
        case IngressEventState::DeliveryFailed: return "delivery_failed";
        case IngressEventState::Archived: return "archived";
        case IngressEventState::Duplicate: return "duplicate";
        case IngressEventState::RateLimited: return "rate_limited";
        case IngressEventState::Unsupported: return "unsupported";
        case IngressEventState::DeadLettered: return "dead_letter";
        case IngressEventState::Cancelled: return "cancelled";
    }
    return "unknown";
}

std::optional<IngressEventState> IngressEventStateFromName(const std::string& name) {
    static const std::array<std::pair<const char*, IngressEventState>, 16> kNames = {{
        {"durable", IngressEventState::Durable},
        {"authorized", IngressEventState::Authorized},
        {"rejected", IngressEventState::Rejected},
        {"routed", IngressEventState::Routed},
        {"queued", IngressEventState::Queued},
        {"running", IngressEventState::Running},
        {"replied", IngressEventState::Replied},
        {"completed_without_reply", IngressEventState::CompletedWithoutReply},
        {"delivered", IngressEventState::Delivered},
        {"delivery_failed", IngressEventState::DeliveryFailed},
        {"archived", IngressEventState::Archived},
        {"duplicate", IngressEventState::Duplicate},
        {"rate_limited", IngressEventState::RateLimited},
        {"unsupported", IngressEventState::Unsupported},
        {"dead_letter", IngressEventState::DeadLettered},
        {"cancelled", IngressEventState::Cancelled},
    }};
    for (const auto& [key, value] : kNames) {
        if (name == key) return value;
    }
    return std::nullopt;
}

bool IsIngressTerminalState(IngressEventState state) {
    switch (state) {
        case IngressEventState::Archived:
        case IngressEventState::Duplicate:
        case IngressEventState::RateLimited:
        case IngressEventState::Unsupported:
        case IngressEventState::DeadLettered:
        case IngressEventState::Cancelled:
            return true;
        default:
            return false;
    }
}

bool CanIngressTransition(IngressEventState from, IngressEventState to) {
    if (from == to) return false;
    if (IsIngressTerminalState(from)) return false;
    // message-contracts.md §4 的主线:
    //   durable -> authorized | rejected -> routed -> queued -> running
    //   -> replied | completed_without_reply -> delivered | delivery_failed
    //   -> archived
    // durable 是唯一起点;任何非终态可走旁路(rate_limited/unsupported/
    // dead_letter/cancelled)——线上账只追加,旁路即终点。
    switch (from) {
        case IngressEventState::Durable:
            return to == IngressEventState::Authorized || to == IngressEventState::Rejected;
        case IngressEventState::Authorized:
            return to == IngressEventState::Routed;
        case IngressEventState::Rejected:
        case IngressEventState::Routed:
            return to == IngressEventState::Queued || to == IngressEventState::RateLimited;
        case IngressEventState::Queued:
            // queued -> rejected 是"执行前重验准入"的退场边(QQ 接入单 Q0):
            // 排队期间权限撤销(撤 allow_from/收窄 binding),取件时重跑
            // 路由不过的输入就地落 rejected,不进执行。
            return to == IngressEventState::Running || to == IngressEventState::RateLimited ||
                   to == IngressEventState::Rejected;
        case IngressEventState::Running:
            return to == IngressEventState::Replied ||
                   to == IngressEventState::CompletedWithoutReply ||
                   to == IngressEventState::Cancelled;
        case IngressEventState::Replied:
        case IngressEventState::CompletedWithoutReply:
            return to == IngressEventState::Delivered || to == IngressEventState::DeliveryFailed;
        case IngressEventState::Delivered:
        case IngressEventState::DeliveryFailed:
            return to == IngressEventState::Archived;
        default:
            return false;
    }
}

DedupeKey ComputeDedupeKey(const ChannelInboundEvent& event, std::string* parts_sha256_out) {
    // parts 指纹:事件 parts 的规范化 JSON 投影过 SHA-256。只服务第三级
    // 短窗去重,不做内容寻址。
    nlohmann::json parts_json = nlohmann::json::array();
    for (const auto& part : event.parts) {
        parts_json.push_back(part.ToJson());
    }
    const std::string digest = Sha256Hex(parts_json.dump());
    if (parts_sha256_out != nullptr) *parts_sha256_out = digest;

    DedupeKey key;
    if (!event.provider_event_id.empty()) {
        key.tier = 1;
        key.key = "p:" + event.channel_id + ":" + event.account_id + ":" + event.provider_event_id;
        return key;
    }
    if (!event.message_id.empty()) {
        key.tier = 2;
        key.key = "m:" + event.channel_id + ":" + event.account_id + ":" + event.conversation.id +
                  ":" + event.message_id;
        return key;
    }
    const std::int64_t bucket = event.provider_at_ms / kFingerprintBucketMs;
    key.tier = 3;
    key.key = "f:" + event.sender.id + ":" + digest + ":" + std::to_string(bucket);
    key.window_until_ms = event.received_at_ms + kFingerprintWindowMs;
    return key;
}

// ---------------------------------------------------------------------------
// ChannelIngressStore
// ---------------------------------------------------------------------------

namespace {

constexpr int kIngressJournalSchema = 1;

// evt 行:{"schema":1,"t":"evt","sid":N,"dedupe":"...","tier":N,
//         "parts_sha256":"...","event":{...}}
// tr 行:{"schema":1,"t":"tr","sid":N,"to":"...","reason":"..."}
nlohmann::json BuildEventLine(std::int64_t sid, const DedupeKey& key,
                              const std::string& parts_sha256, const ChannelInboundEvent& event) {
    nlohmann::json line = nlohmann::json::object();
    line["schema"] = kIngressJournalSchema;
    line["t"] = "evt";
    line["sid"] = sid;
    line["dedupe"] = key.key;
    line["tier"] = key.tier;
    line["parts_sha256"] = parts_sha256;
    line["event"] = event.ToJson();
    return line;
}

nlohmann::json BuildTransitionLine(std::int64_t sid, IngressEventState to,
                                    const std::string& reason) {
    nlohmann::json line = nlohmann::json::object();
    line["schema"] = kIngressJournalSchema;
    line["t"] = "tr";
    line["sid"] = sid;
    line["to"] = IngressEventStateName(to);
    line["reason"] = reason;
    return line;
}

// dup 行:重复投递的旁注。它不是状态迁移——原事件可能正走主线中途,
// 重放不该把它打成 duplicate 终态。replay 见 dup 行只计数,不重建状态。
nlohmann::json BuildDuplicateLine(std::int64_t sid, const std::string& reason) {
    nlohmann::json line = nlohmann::json::object();
    line["schema"] = kIngressJournalSchema;
    line["t"] = "dup";
    line["sid"] = sid;
    line["reason"] = reason;
    return line;
}

}  // namespace

std::unique_ptr<ChannelIngressStore> ChannelIngressStore::Open(
    const std::filesystem::path& account_dir, std::string channel_id, std::string account_id,
    OpenResult* result) {
    auto store = std::make_unique<ChannelIngressStore>();
    store->channel_id_ = std::move(channel_id);
    store->account_id_ = std::move(account_id);
    store->journal_path_ = account_dir / "ingress" / "journal.jsonl";
    store->dead_letter_path_ = account_dir / "ingress" / "dead-letter.jsonl";

    if (result != nullptr) {
        result->ok = true;
    }

    std::error_code ec;
    const std::filesystem::path ingress_dir = account_dir / "ingress";
    std::filesystem::create_directories(ingress_dir, ec);
    if (ec) {
        store->write_blocked_ = true;
        store->last_error_ = "建目录 " + platform::PathToUtf8(ingress_dir) + " 失败: " + ec.message();
        if (result != nullptr) {
            result->ok = false;
            result->error = store->last_error_;
        }
        return store;
    }

    store->ReplayLocked();
    if (result != nullptr) {
        result->skipped_lines = store->replayed_bad_lines_;
        if (!store->last_error_.empty()) {
            result->ok = false;
            result->error = store->last_error_;
        }
    }
    // 主账写柄(QQ 接入单 Q2:PowerLoss 档,Open 即持;打不开 = 只读拒绝写)。
    auto journal = trajectory::JournalWriter::Open(
        store->journal_path_, trajectory::JournalWriter::OpenMode::Append);
    if (!journal.has_value()) {
        store->write_blocked_ = true;
        store->last_error_ = journal.error();
        if (result != nullptr) {
            result->ok = false;
            result->error = store->last_error_;
        }
        return store;
    }
    store->journal_writer_ = std::move(*journal);
    return store;
}

void ChannelIngressStore::ReplayLocked() {
    std::error_code ec;
    if (!std::filesystem::exists(journal_path_, ec) || ec) {
        return;  // 新账
    }
    std::ifstream stream(journal_path_, std::ios::binary);
    if (!stream) {
        write_blocked_ = true;
        last_error_ = "journal 打不开: " + platform::PathToUtf8(journal_path_);
        return;
    }

    // 记录在账的 sid 集合:tr 行可能指到 evt 行之前(不可能——evt 先落);
    // 但半行 evt 之后的 tr 行仍要认(事件没落成,迁移行指空号,跳过)。
    std::string text_line;
    int skipped = 0;
    while (std::getline(stream, text_line)) {
        if (text_line.empty()) {
            continue;  // 尾部空行(最后崩溃前没写完换行)不算坏行
        }
        nlohmann::json parsed;
        try {
            parsed = nlohmann::json::parse(text_line);
        } catch (const nlohmann::json::exception&) {
            // 崩溃半写行:容错跳过,不崩宿主。文件还能继续追加(append 模式
            // 写在坏行之后;重建索引靠 evt/tr 行的 sid 对账,坏行等于没发生)。
            ++skipped;
            continue;
        }
        if (!parsed.is_object() || !parsed.contains("schema") || !parsed.contains("t") ||
            !parsed["schema"].is_number_integer() ||
            parsed["schema"].get<int>() != kIngressJournalSchema ||
            !parsed["t"].is_string()) {
            ++skipped;
            continue;
        }
        const std::string type = parsed["t"].get<std::string>();
        if (type == "dup") {
            // 重复投递旁注:不重建状态,直接跳过(账上可 grep,内存不建)。
            continue;
        }
        if (type == "evt") {
            if (!parsed.contains("sid") || !parsed.contains("dedupe") ||
                !parsed.contains("tier") || !parsed.contains("event") ||
                !parsed["sid"].is_number_integer()) {
                ++skipped;
                continue;
            }
            std::string event_error;
            auto event = ChannelInboundEvent::FromJsonStrict(parsed["event"], &event_error);
            if (!event.has_value()) {
                ++skipped;
                continue;
            }
            Record record;
            record.sid = parsed["sid"].get<std::int64_t>();
            record.key.key = parsed["dedupe"].get<std::string>();
            record.key.tier = parsed["tier"].get<int>();
            record.parts_sha256 =
                parsed.contains("parts_sha256") && parsed["parts_sha256"].is_string()
                    ? parsed["parts_sha256"].get<std::string>()
                    : std::string();
            record.event = std::move(*event);
            record.state = IngressEventState::Durable;
            if (record.key.tier == 1 || record.key.tier == 2) {
                permanent_keys_.emplace(record.key.key, record.sid);
            }
            delivery_ids_.emplace(record.event.delivery_id, record.sid);
            next_sid_ = std::max(next_sid_, record.sid + 1);
            records_.push_back(std::move(record));
        } else if (type == "tr") {
            if (!parsed.contains("sid") || !parsed.contains("to") || !parsed["sid"].is_number_integer() ||
                !parsed["to"].is_string()) {
                ++skipped;
                continue;
            }
            const std::int64_t sid = parsed["sid"].get<std::int64_t>();
            const auto to = IngressEventStateFromName(parsed["to"].get<std::string>());
            if (!to.has_value()) {
                ++skipped;
                continue;
            }
            // 找到账上对应记录(线性账 sid 升序,倒着找最快;replay 中段
            // 通常就是最后一条)。
            for (auto it = records_.rbegin(); it != records_.rend(); ++it) {
                if (it->sid == sid) {
                    it->state = *to;
                    it->last_transition_reason =
                        parsed.contains("reason") && parsed["reason"].is_string()
                            ? parsed["reason"].get<std::string>()
                            : std::string();
                    break;
                }
            }
        } else {
            ++skipped;
        }
    }
    replayed_bad_lines_ = skipped;
}

std::optional<std::string> ChannelIngressStore::AppendLine(const std::string& line) {
    if (write_blocked_ || !journal_writer_.has_value()) {
        return std::string("账本只读(journal 打不开或建目录失败): ") + last_error_;
    }
    // QQ 接入单 Q2(§六第一项):PowerLoss 档——规范化原文与去重键落稳
    // 才算 durable,调用方(OnInboundLocked)此后才 ack sidecar。写失败
    // 一次性进 write_blocked,后续追加全部拒绝(状态追加失败停止推进)。
    if (!journal_writer_->AppendLine(line, trajectory::Durability::PowerLoss)) {
        write_blocked_ = true;
        last_error_ = "journal 落盘失败: " + platform::PathToUtf8(journal_path_);
        return last_error_;
    }
    return std::nullopt;
}

std::optional<ChannelIngressStore::IngestOutcome> ChannelIngressStore::Ingest(
    const ChannelInboundEvent& event) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (write_blocked_) {
        return std::nullopt;  // last_error_ 有因;调用方经 last_error() 查
    }

    // delivery_id 对账:同一 delivery 重送(sidecar ack 前的退避重发)直接
    // 按 duplicate 收——事件早已 durable。这里只追加一笔重放账,不改写
    // 内存态:原事件可能正走主线中途,重复投递不该把它打成 duplicate 终态。
    const auto delivery_hit = delivery_ids_.find(event.delivery_id);
    if (delivery_hit != delivery_ids_.end()) {
        const std::string duplicate_line =
            BuildDuplicateLine(delivery_hit->second, "delivery_replayed").dump();
        if (AppendLine(duplicate_line).has_value()) {
            return std::nullopt;  // 账都写不进,不算收下;交给 sidecar 重发
        }
        IngestOutcome outcome;
        outcome.status = IngestOutcome::Status::Duplicate;
        outcome.sid = delivery_hit->second;
        outcome.ack = true;
        return outcome;
    }

    std::string parts_sha256;
    const DedupeKey key = ComputeDedupeKey(event, &parts_sha256);

    // 永久键(tier 1/2)。
    const auto permanent_hit = permanent_keys_.find(key.key);
    if (permanent_hit != permanent_keys_.end()) {
        const std::string duplicate_line =
            BuildDuplicateLine(permanent_hit->second, "key_replayed").dump();
        if (AppendLine(duplicate_line).has_value()) return std::nullopt;
        IngestOutcome outcome;
        outcome.status = IngestOutcome::Status::Duplicate;
        outcome.sid = permanent_hit->second;
        outcome.key = key;
        outcome.ack = true;
        return outcome;
    }

    // 短窗指纹(tier 3):窗口内同键即重复。
    if (key.tier == 3) {
        for (auto it = records_.rbegin(); it != records_.rend(); ++it) {
            if (it->key.tier != 3 || it->key.key != key.key) continue;
            const std::int64_t window_until =
                it->event.received_at_ms + kFingerprintWindowMs;
            if (event.received_at_ms <= window_until) {
                const std::string duplicate_line =
                    BuildDuplicateLine(it->sid, "fingerprint_replayed").dump();
                if (AppendLine(duplicate_line).has_value()) return std::nullopt;
                IngestOutcome outcome;
                outcome.status = IngestOutcome::Status::Duplicate;
                outcome.sid = it->sid;
                outcome.key = key;
                outcome.ack = true;
                return outcome;
            }
            break;  // records_ 按时间序,最近一条同键出了窗,更早的更出窗
        }
    }

    // 新事件:evt 行落盘(即 durable)。
    const std::int64_t sid = next_sid_;
    const std::string event_line = BuildEventLine(sid, key, parts_sha256, event).dump();
    if (AppendLine(event_line).has_value()) {
        return std::nullopt;
    }

    Record record;
    record.sid = sid;
    record.key = key;
    record.parts_sha256 = parts_sha256;
    record.event = event;
    record.state = IngressEventState::Durable;
    if (key.tier == 1 || key.tier == 2) {
        permanent_keys_.emplace(key.key, sid);
    }
    delivery_ids_.emplace(event.delivery_id, sid);
    records_.push_back(std::move(record));
    next_sid_ = sid + 1;

    IngestOutcome outcome;
    outcome.status = IngestOutcome::Status::Accepted;
    outcome.sid = sid;
    outcome.key = key;
    outcome.ack = true;
    return outcome;
}

std::optional<std::string> ChannelIngressStore::Transition(std::int64_t sid,
                                                           IngressEventState to,
                                                           const std::string& reason) {
    std::lock_guard<std::mutex> lock(mutex_);
    Record* record = nullptr;
    for (auto it = records_.rbegin(); it != records_.rend(); ++it) {
        if (it->sid == sid) {
            record = &(*it);
            break;
        }
    }
    if (record == nullptr) {
        return std::string("账上没有 sid ") + std::to_string(sid);
    }
    if (!CanIngressTransition(record->state, to)) {
        return std::string("非法迁移 ") + IngressEventStateName(record->state) + " -> " +
               IngressEventStateName(to);
    }
    const std::string line = BuildTransitionLine(sid, to, reason).dump();
    if (AppendLine(line).has_value()) {
        return last_error_;
    }
    record->state = to;
    record->last_transition_reason = reason;
    return std::nullopt;
}

std::optional<std::string> ChannelIngressStore::MoveToDeadLetter(std::int64_t sid,
                                                                 const std::string& reason,
                                                                 std::int64_t at_ms) {
    std::lock_guard<std::mutex> lock(mutex_);
    Record* record = nullptr;
    for (auto it = records_.rbegin(); it != records_.rend(); ++it) {
        if (it->sid == sid) {
            record = &*it;
            break;
        }
    }
    if (record == nullptr) {
        return std::string("账上没有 sid ") + std::to_string(sid);
    }
    if (IsIngressTerminalState(record->state)) {
        return std::string("终态事件不能进 dead letter: ") +
               IngressEventStateName(record->state);
    }
    const std::string line = BuildTransitionLine(sid, IngressEventState::DeadLettered, reason).dump();
    if (AppendLine(line).has_value()) {
        return last_error_;
    }
    record->last_transition_reason = reason;
    // dead-letter.jsonl 留完整档(replay 用;旁路账,写不进不阻塞迁移)。
    if (!write_blocked_) {
        nlohmann::json entry = nlohmann::json::object();
        entry["sid"] = sid;
        entry["channel_id"] = channel_id_;
        entry["account_id"] = account_id_;
        entry["delivery_id"] = record->event.delivery_id;
        entry["reason"] = reason;
        entry["at_ms"] = at_ms;
        entry["event"] = record->event.ToJson();
        (void)AppendDeadLetterLine(entry);
    }
    for (auto it = records_.rbegin(); it != records_.rend(); ++it) {
        if (it->sid == sid) {
            it->state = IngressEventState::DeadLettered;
            break;
        }
    }
    return std::nullopt;
}

std::optional<std::string> ChannelIngressStore::AppendDeadLetterLine(
    const nlohmann::json& entry) {
    if (!dead_letter_writer_.has_value()) {
        auto writer = trajectory::JournalWriter::Open(
            dead_letter_path_, trajectory::JournalWriter::OpenMode::Append);
        if (!writer.has_value()) {
            return writer.error();
        }
        dead_letter_writer_ = std::move(*writer);
    }
    if (!dead_letter_writer_->AppendLine(entry.dump(), trajectory::Durability::PowerLoss)) {
        dead_letter_writer_.reset();  // 下次重开;旁路账失败不阻塞主账
        return std::string("dead-letter 落盘失败: ") + platform::PathToUtf8(dead_letter_path_);
    }
    return std::nullopt;
}

std::vector<ChannelIngressStore::Record> ChannelIngressStore::Records() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return std::vector<Record>(records_.begin(), records_.end());
}

std::optional<ChannelIngressStore::Record> ChannelIngressStore::FindBySid(std::int64_t sid) const {
    std::lock_guard<std::mutex> lock(mutex_);
    for (const auto& record : records_) {
        if (record.sid == sid) return record;
    }
    return std::nullopt;
}

std::optional<std::int64_t> ChannelIngressStore::FindByDeliveryId(
    const std::string& delivery_id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto hit = delivery_ids_.find(delivery_id);
    if (hit == delivery_ids_.end()) return std::nullopt;
    return hit->second;
}

std::map<std::string, std::size_t> ChannelIngressStore::StateCounts() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::map<std::string, std::size_t> counts;
    for (const auto& record : records_) {
        counts[IngressEventStateName(record.state)] += 1;
    }
    return counts;
}

std::size_t ChannelIngressStore::dead_letter_count() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::size_t count = 0;
    for (const auto& record : records_) {
        if (record.state == IngressEventState::DeadLettered) ++count;
    }
    return count;
}

std::vector<ChannelIngressStore::DeadLetterEntry> ChannelIngressStore::DeadLetters() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<DeadLetterEntry> out;
    for (const auto& record : records_) {
        if (record.state != IngressEventState::DeadLettered) continue;
        DeadLetterEntry entry;
        entry.sid = record.sid;
        entry.channel_id = channel_id_;
        entry.account_id = account_id_;
        entry.delivery_id = record.event.delivery_id;
        entry.reason = record.last_transition_reason;
        entry.at_ms = record.event.received_at_ms;
        entry.event = record.event;
        out.push_back(std::move(entry));
    }
    return out;
}

std::int64_t ChannelIngressStore::next_sid() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return next_sid_;
}

// ---------------------------------------------------------------------------
// 只读投影(gateway status 渠道栏;零建目录零写盘)
// ---------------------------------------------------------------------------

ChannelIngressProjection ReadChannelIngressProjection(const std::filesystem::path& account_dir) {
    ChannelIngressProjection projection;
    const std::filesystem::path journal = account_dir / "ingress" / "journal.jsonl";
    std::error_code ec;
    if (!std::filesystem::is_regular_file(journal, ec) || ec) {
        return projection;
    }
    projection.ledger_present = true;
    // sid -> 当前状态(replay 语义同 ReplayLocked:evt 起 durable,tr 覆盖;
    // 坏行/半行跳过不猜)。
    std::map<std::int64_t, IngressEventState> states;
    std::ifstream stream(journal, std::ios::binary);
    std::string text_line;
    while (std::getline(stream, text_line)) {
        if (text_line.empty()) continue;
        nlohmann::json parsed;
        try {
            parsed = nlohmann::json::parse(text_line);
        } catch (const nlohmann::json::exception&) {
            continue;
        }
        if (!parsed.is_object() || !parsed.contains("t") || !parsed["t"].is_string() ||
            !parsed.contains("sid") || !parsed["sid"].is_number_integer()) {
            continue;
        }
        const std::string type = parsed["t"].get<std::string>();
        const std::int64_t sid = parsed["sid"].get<std::int64_t>();
        if (type == "evt") {
            states[sid] = IngressEventState::Durable;
            ++projection.events;
        } else if (type == "tr") {
            if (!parsed.contains("to") || !parsed["to"].is_string()) continue;
            const auto to = IngressEventStateFromName(parsed["to"].get<std::string>());
            if (to.has_value()) {
                states[sid] = *to;  // 指空号的 tr 等于没发生(evt 没落成)
            }
        }
    }
    for (const auto& [sid, state] : states) {
        ++projection.state_counts[IngressEventStateName(state)];
    }
    const std::filesystem::path dead_letter = account_dir / "ingress" / "dead-letter.jsonl";
    if (std::filesystem::is_regular_file(dead_letter, ec) && !ec) {
        std::ifstream dead_stream(dead_letter, std::ios::binary);
        std::string dead_line;
        while (std::getline(dead_stream, dead_line)) {
            if (!dead_line.empty()) {
                ++projection.dead_letter;
            }
        }
    }
    return projection;
}

// ---------------------------------------------------------------------------
// 最近来信链的只读投影(P1:channel status 链视图;零建目录零写盘)
// ---------------------------------------------------------------------------

ChannelIngressRecentChain ReadChannelIngressRecentChain(const std::filesystem::path& account_dir,
                                                        std::size_t limit) {
    ChannelIngressRecentChain chain;
    if (limit == 0) {
        return chain;
    }
    const std::filesystem::path journal = account_dir / "ingress" / "journal.jsonl";
    std::error_code ec;
    if (!std::filesystem::is_regular_file(journal, ec) || ec) {
        return chain;
    }
    chain.ledger_present = true;
    struct EntryState {
        ChannelIngressRecentEntry entry;
        bool seen = false;
    };
    // 全量重放(账可能很长,但状态机轻;只留最近 limit 枚的窗口裁剪在
    // 收尾做——tr 行可能落在 evt 行之后很久,窗口须按最终态裁)。
    std::vector<EntryState> records;
    std::map<std::int64_t, std::size_t> index_by_sid;
    std::ifstream stream(journal, std::ios::binary);
    std::string text_line;
    while (std::getline(stream, text_line)) {
        if (text_line.empty()) continue;
        nlohmann::json parsed;
        try {
            parsed = nlohmann::json::parse(text_line);
        } catch (const nlohmann::json::exception&) {
            continue;  // 半行/坏行:与 replay 同一容错
        }
        if (!parsed.is_object() || !parsed.contains("t") || !parsed["t"].is_string() ||
            !parsed.contains("sid") || !parsed["sid"].is_number_integer()) {
            continue;
        }
        const std::string type = parsed["t"].get<std::string>();
        const std::int64_t sid = parsed["sid"].get<std::int64_t>();
        if (type == "evt") {
            if (index_by_sid.count(sid) > 0) continue;  // 同 sid 重落:首笔为准
            EntryState state;
            state.seen = true;
            state.entry.sid = sid;
            state.entry.state = IngressEventStateName(IngressEventState::Durable);
            if (parsed.contains("event") && parsed.at("event").is_object()) {
                const nlohmann::json& event = parsed.at("event");
                if (event.contains("received_at_ms") && event.at("received_at_ms").is_number_integer()) {
                    state.entry.received_at_ms = event.at("received_at_ms").get<std::int64_t>();
                }
                if (event.contains("conversation") && event.at("conversation").is_object() &&
                    event.at("conversation").contains("id") &&
                    event.at("conversation").at("id").is_string()) {
                    state.entry.conversation_id =
                        event.at("conversation").at("id").get<std::string>();
                }
                if (event.contains("sender") && event.at("sender").is_object() &&
                    event.at("sender").contains("id") &&
                    event.at("sender").at("id").is_string()) {
                    state.entry.sender_id = event.at("sender").at("id").get<std::string>();
                }
            }
            index_by_sid[sid] = records.size();
            records.push_back(std::move(state));
        } else if (type == "tr") {
            const auto found = index_by_sid.find(sid);
            if (found == index_by_sid.end()) continue;
            if (!parsed.contains("to") || !parsed["to"].is_string()) continue;
            const auto to = IngressEventStateFromName(parsed["to"].get<std::string>());
            if (!to.has_value()) continue;
            records[found->second].entry.state = IngressEventStateName(*to);
            if (parsed.contains("reason") && parsed["reason"].is_string()) {
                records[found->second].entry.reason = parsed["reason"].get<std::string>();
            }
        }
    }
    // dead-letter 旁路账:补死信时间(它才有 at_ms;journal 行不带时间戳)。
    const std::filesystem::path dead_letter = account_dir / "ingress" / "dead-letter.jsonl";
    if (std::filesystem::is_regular_file(dead_letter, ec) && !ec) {
        std::ifstream dead_stream(dead_letter, std::ios::binary);
        std::string dead_line;
        while (std::getline(dead_stream, dead_line)) {
            if (dead_line.empty()) continue;
            ++chain.dead_letter_count;
            nlohmann::json parsed;
            try {
                parsed = nlohmann::json::parse(dead_line);
            } catch (const nlohmann::json::exception&) {
                continue;
            }
            if (!parsed.is_object() || !parsed.contains("sid") ||
                !parsed["sid"].is_number_integer()) {
                continue;
            }
            const std::int64_t sid = parsed["sid"].get<std::int64_t>();
            const auto found = index_by_sid.find(sid);
            if (found == index_by_sid.end()) continue;
            if (parsed.contains("at_ms") && parsed.at("at_ms").is_number_integer()) {
                records[found->second].entry.dead_letter_at_ms = parsed.at("at_ms").get<std::int64_t>();
            }
        }
    }
    // 最近 limit 枚(sid 降序)。
    const std::size_t begin =
        records.size() > limit ? records.size() - limit : 0;
    for (std::size_t i = records.size(); i-- > begin;) {
        if (records[i].seen) {
            chain.entries.push_back(std::move(records[i].entry));
        }
    }
    return chain;
}

}  // namespace lubancode::channel
