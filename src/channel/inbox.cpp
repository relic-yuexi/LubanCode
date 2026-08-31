#include "channel/inbox.hpp"

#include <algorithm>

namespace lubancode::channel {

namespace {

// 轮转序里的下一个非空桶。bucket_order_ 里可能有已空桶(取空不除名),
// 跳着找;全空返回空串。cursor_ 停在选中桶的下一位。
std::string NextNonEmptyBucket(std::vector<std::string>& order, std::size_t& cursor,
                               const std::map<std::string, std::deque<ChannelInbox::Item>>& buckets) {
    if (order.empty()) return std::string();
    for (std::size_t step = 0; step < order.size(); ++step) {
        const std::size_t index = cursor % order.size();
        const std::string& name = order[index];
        const auto it = buckets.find(name);
        if (it != buckets.end() && !it->second.empty()) {
            cursor = (index + 1) % order.size();
            return name;
        }
        cursor = (index + 1) % order.size();
    }
    return std::string();
}

}  // namespace

ChannelInbox::ChannelInbox(InboxLimits limits) : limits_(limits) {}

ChannelInbox::EnqueueResult ChannelInbox::Enqueue(std::int64_t sid,
                                                  const std::string& conversation_id,
                                                  const std::string& sender_id,
                                                  const std::string& content_digest,
                                                  std::int64_t now_ms) {
    std::lock_guard<std::mutex> lock(mutex_);
    EnqueueResult result;

    // 同正文短窗:同 sender、同 conversation、同 digest 在窗内已收过。
    if (limits_.same_content_window_ms > 0 && !content_digest.empty()) {
        const std::string content_key = sender_id + "|" + conversation_id + "|" + content_digest;
        const auto hit = recent_content_.find(content_key);
        if (hit != recent_content_.end() && now_ms - hit->second < limits_.same_content_window_ms) {
            result.status = EnqueueResult::Status::DuplicateContent;
            result.reason = "duplicate_content";
            return result;
        }
    }

    // 每 sender 速率窗:窗内接受数过帽即拒。
    if (limits_.sender_rate_max > 0 && limits_.sender_rate_window_ms > 0) {
        auto& window = sender_windows_[sender_id];
        while (!window.empty() && now_ms - window.front() >= limits_.sender_rate_window_ms) {
            window.pop_front();
        }
        if (window.size() >= limits_.sender_rate_max) {
            result.status = EnqueueResult::Status::SenderRateLimited;
            result.reason = "sender_rate_limited";
            return result;
        }
    }

    // 水位:先看本会话,再看总量(细原因优先,doctor 好指路)。
    if (limits_.max_pending_per_conversation > 0) {
        const auto it = buckets_.find(conversation_id);
        if (it != buckets_.end() && it->second.size() >= limits_.max_pending_per_conversation) {
            result.status = EnqueueResult::Status::QueueFull;
            result.reason = "queue_full_conversation";
            return result;
        }
    }
    if (limits_.max_pending_total > 0) {
        std::size_t total = 0;
        for (const auto& [name, queue] : buckets_) {
            total += queue.size();
        }
        if (total >= limits_.max_pending_total) {
            result.status = EnqueueResult::Status::QueueFull;
            result.reason = "queue_full";
            return result;
        }
    }

    Item item;
    item.sid = sid;
    item.conversation_id = conversation_id;
    item.sender_id = sender_id;
    item.content_digest = content_digest;

    const bool new_bucket = buckets_.find(conversation_id) == buckets_.end();
    buckets_[conversation_id].push_back(std::move(item));
    if (new_bucket) {
        // 新桶入轮转序尾部(轮转指针不动,老桶不受打扰)。
        const auto it = std::find(bucket_order_.begin(), bucket_order_.end(), conversation_id);
        if (it == bucket_order_.end()) {
            bucket_order_.push_back(conversation_id);
        }
    }

    // 速率窗与短窗的记账只在真接受后落。
    if (limits_.sender_rate_max > 0 && limits_.sender_rate_window_ms > 0) {
        sender_windows_[sender_id].push_back(now_ms);
    }
    if (limits_.same_content_window_ms > 0 && !content_digest.empty()) {
        const std::string content_key = sender_id + "|" + conversation_id + "|" + content_digest;
        recent_content_[content_key] = now_ms;
        // 短窗账瘦身:条目数超过总帽两倍时扫一遍过期的。
        if (recent_content_.size() > limits_.max_pending_total * 2 + 16) {
            for (auto it = recent_content_.begin(); it != recent_content_.end();) {
                if (now_ms - it->second >= limits_.same_content_window_ms) {
                    it = recent_content_.erase(it);
                } else {
                    ++it;
                }
            }
        }
    }
    return result;
}

std::optional<ChannelInbox::Item> ChannelInbox::TakeNext() {
    std::lock_guard<std::mutex> lock(mutex_);
    const std::string bucket =
        NextNonEmptyBucket(bucket_order_, rotation_cursor_, buckets_);
    if (bucket.empty()) return std::nullopt;
    return PopBucket(bucket);
}

std::optional<ChannelInbox::Item> ChannelInbox::TakeNextFor(const std::string& conversation_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto it = buckets_.find(conversation_id);
    if (it == buckets_.end() || it->second.empty()) return std::nullopt;
    return PopBucket(conversation_id);
}

std::optional<ChannelInbox::Item> ChannelInbox::PopBucket(const std::string& conversation_id) {
    auto it = buckets_.find(conversation_id);
    if (it == buckets_.end() || it->second.empty()) return std::nullopt;
    Item item = std::move(it->second.front());
    it->second.pop_front();
    if (it->second.empty()) {
        buckets_.erase(it);
    }
    return item;
}

std::size_t ChannelInbox::pending_total() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::size_t total = 0;
    for (const auto& [name, queue] : buckets_) {
        total += queue.size();
    }
    return total;
}

std::size_t ChannelInbox::pending_for(const std::string& conversation_id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto it = buckets_.find(conversation_id);
    if (it == buckets_.end()) return 0;
    return it->second.size();
}

}  // namespace lubancode::channel
