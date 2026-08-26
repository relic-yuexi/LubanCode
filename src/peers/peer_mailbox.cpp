// 信封与收件信箱实现。设计见 peer_mailbox.hpp 文件头。

#include "peers/peer_mailbox.hpp"

#include <algorithm>
#include <mutex>
#include <optional>
#include <utility>

namespace lubancode::peers {

nlohmann::json PeerEnvelopeToJson(const PeerEnvelope& envelope) {
    nlohmann::json json;
    json["version"] = envelope.version;
    json["message_id"] = envelope.message_id;
    json["sender_id"] = envelope.sender_id;
    json["sender_name"] = envelope.sender_name;
    json["target_id"] = envelope.target_id;
    json["sent_at"] = envelope.sent_at;
    if (envelope.reply_to.has_value()) {
        json["reply_to"] = *envelope.reply_to;
    } else {
        json["reply_to"] = nullptr;
    }
    json["text"] = envelope.text;
    return json;
}

std::optional<PeerEnvelope> PeerEnvelopeFromJson(const std::string& raw) {
    nlohmann::json json;
    try {
        json = nlohmann::json::parse(raw);
    } catch (const nlohmann::json::exception&) {
        return std::nullopt;
    }
    if (!json.is_object()) {
        return std::nullopt;
    }
    const auto version = json.find("version");
    if (version == json.end() || !version->is_number() || version->get<int>() > kPeerEnvelopeVersion) {
        return std::nullopt;  // 认不得的协议版本
    }
    PeerEnvelope envelope;
    envelope.version = version->get<int>();
    const auto read_required = [&](const char* key, std::string& out) {
        const auto it = json.find(key);
        if (it == json.end() || !it->is_string()) {
            return false;
        }
        out = it->get<std::string>();
        return true;
    };
    if (!read_required("message_id", envelope.message_id) || envelope.message_id.empty() ||
        !read_required("sender_id", envelope.sender_id) || !read_required("target_id", envelope.target_id) ||
        !read_required("text", envelope.text)) {
        return std::nullopt;
    }
    const auto read_optional = [&](const char* key, std::string& out) {
        const auto it = json.find(key);
        if (it != json.end() && it->is_string()) {
            out = it->get<std::string>();
        }
    };
    read_optional("sender_name", envelope.sender_name);
    if (const auto it = json.find("sent_at"); it != json.end() && it->is_number()) {
        envelope.sent_at = it->get<long long>();
    }
    if (const auto it = json.find("reply_to"); it != json.end() && it->is_string()) {
        envelope.reply_to = it->get<std::string>();
    }
    return envelope;
}

const char* PeerDeliveryName(PeerDelivery status) {
    switch (status) {
        case PeerDelivery::Delivered:
            return "delivered";
        case PeerDelivery::Held:
            return "held";
        case PeerDelivery::Refused:
            return "refused";
        case PeerDelivery::Expired:
            return "expired";
        case PeerDelivery::Unavailable:
            return "unavailable";
    }
    return "unavailable";
}

PeerMailbox::PeerMailbox(std::size_t capacity, std::size_t rate_limit, int rate_window_seconds,
                         int dup_text_window_seconds)
    : capacity_(capacity),
      rate_limit_(rate_limit),
      rate_window_seconds_(rate_window_seconds),
      dup_text_window_seconds_(dup_text_window_seconds) {}

PeerOfferStatus PeerMailbox::Offer(PeerEnvelope envelope, long long now_unix) {
    std::lock_guard<std::mutex> lock(mutex_);

    // 1) message_id 去重:同一封信只收一次。
    if (seen_ids_.count(envelope.message_id) != 0) {
        return PeerOfferStatus::Duplicate;
    }

    // 2) 同一发送方限速:窗口内超过 rate_limit_ 封就拦。
    auto& times = send_times_[envelope.sender_id];
    while (!times.empty() && now_unix - times.front() > rate_window_seconds_) {
        times.pop_front();
    }
    if (times.size() >= rate_limit_) {
        return PeerOfferStatus::RateLimited;
    }

    // 3) 相同正文短窗去重:来回重发同一句话,只当一封。
    const std::size_t text_hash = std::hash<std::string>{}(envelope.text);
    auto& texts = send_texts_[envelope.sender_id];
    while (!texts.empty() && now_unix - texts.front().first > dup_text_window_seconds_) {
        texts.pop_front();
    }
    for (const auto& [at, hash] : texts) {
        if (hash == text_hash) {
            return PeerOfferStatus::DuplicateText;
        }
    }

    // 4) 队列硬上限:到了就不再收,发件方会拿到 expired。
    if (queue_.size() >= capacity_) {
        return PeerOfferStatus::QueueFull;
    }

    queue_.push_back(std::move(envelope));
    const PeerEnvelope& stored = queue_.back();
    seen_ids_.insert(stored.message_id);
    seen_order_.emplace_back(stored.message_id, now_unix);
    // 去重账上限 256 条(远大于限速窗口能产生的量),防无限长。
    while (seen_order_.size() > 256) {
        seen_ids_.erase(seen_order_.front().first);
        seen_order_.pop_front();
    }
    times.push_back(now_unix);
    texts.emplace_back(now_unix, text_hash);
    return PeerOfferStatus::Accepted;
}

std::vector<PeerEnvelope> PeerMailbox::Drain() {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<PeerEnvelope> out(queue_.begin(), queue_.end());
    queue_.clear();
    return out;
}

std::size_t PeerMailbox::pending() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return queue_.size();
}

PeerPermissionTier DefaultReceiveTier(int local_mode, int remote_mode, bool cwd_far_apart) {
    // 任一边免确认(auto/yolo),默认扣住等用户点头;项目相隔甚远同理。
    if (local_mode != 0 || remote_mode != 0 || cwd_far_apart) {
        return PeerPermissionTier::Hold;
    }
    return PeerPermissionTier::Accept;
}

bool PeerCwdFarApart(const std::string& a, const std::string& b) {
    if (a.empty() || b.empty()) {
        return true;  // 信息不全,保守按远算
    }
    const auto normalize = [](std::string s) {
        for (char& c : s) {
            if (c == '\\') {
                c = '/';
            }
        }
        return s;
    };
    // 拆成路径段(剥掉根部的空段):Windows 是 [D:, work, ...],POSIX 是
    // [home, alice, ...]。比头两段:同用户/同盘同第一级目录算近,否则远。
    const auto components = [](const std::string& path) {
        std::vector<std::string> out;
        std::size_t start = 0;
        if (!path.empty() && path[0] == '/') {
            start = 1;  // POSIX 根斜杠不算段
        }
        std::size_t i = start;
        while (i <= path.size()) {
            if (i == path.size() || path[i] == '/') {
                if (i > start) {
                    out.push_back(path.substr(start, i - start));
                }
                start = i + 1;
            }
            ++i;
        }
        return out;
    };
    const std::vector<std::string> ca = components(normalize(a));
    const std::vector<std::string> cb = components(normalize(b));
    const std::size_t compare = (std::min)(std::size_t{2}, (std::min)(ca.size(), cb.size()));
    for (std::size_t i = 0; i < compare; ++i) {
        if (ca[i] != cb[i]) {
            return true;
        }
    }
    return false;
}

}  // namespace lubancode::peers
