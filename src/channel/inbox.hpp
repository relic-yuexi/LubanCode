// ChannelInbox:每账号内存队列与背压(多渠道消息接入单阶段 2)。
//
// 唯一真源 TODO"多渠道消息接入与常驻 ChannelPlugin 设计"§13.6(队列与
// 背压)与 message-contracts.md §4(rate_limited 旁路终态)。规矩:
//   - 已 durable 的事件不因内存队列满而消失——队列满给 QueueFull 决策,
//     调用方在 ingress 账上记 rate_limited 并 nack sidecar(retry=true),
//     事件留在 journal 账上可查,sidecar 按退避重发。不默丢。
//   - 每 conversation FIFO:同会话串行(同 conversation 只跑一枚 main
//     turn 的内存侧前置);不同 conversation 轮转取件,谁也不饿谁。
//   - 每 sender 速率窗 + 同正文短窗(灌水与重发风暴的粗闸)。
//
// 纯内存件:线程安全(mutex),不落盘——耐久归 ChannelIngressStore。
#pragma once

#include <cstdint>
#include <deque>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace lubancode::channel {

struct InboxLimits {
    // 每账号总 pending 上限。
    std::size_t max_pending_total = 256;
    // 每 conversation pending 上限。
    std::size_t max_pending_per_conversation = 32;
    // 每 sender 速率窗。
    std::int64_t sender_rate_window_ms = 60 * 1000;
    std::size_t sender_rate_max = 30;
    // 同正文(sender+conversation+digest)短窗。
    std::int64_t same_content_window_ms = 5 * 1000;
};

class ChannelInbox {
public:
    struct Item {
        std::int64_t sid = 0;              // ingress 账序号
        std::string conversation_id;
        std::string sender_id;
        std::string content_digest;        // 正文指纹(短窗去重与诊断)
    };

    struct EnqueueResult {
        enum class Status {
            Accepted,
            QueueFull,        // 总量或本会话水位到帽:不默丢,nack retry
            SenderRateLimited,
            DuplicateContent,  // 同 sender 同会话同正文短窗内已收过
        };
        Status status = Status::Accepted;
        std::string reason;  // 稳定名:queue_full / queue_full_conversation /
                             // sender_rate_limited / duplicate_content;Accepted 空
    };

    explicit ChannelInbox(InboxLimits limits = InboxLimits{});

    EnqueueResult Enqueue(std::int64_t sid, const std::string& conversation_id,
                          const std::string& sender_id, const std::string& content_digest,
                          std::int64_t now_ms);

    // 公平取件:conversation 桶间轮转(round-robin),桶内 FIFO。
    std::optional<Item> TakeNext();
    // 指定 conversation 的下一件(同会话串行的直取口)。
    std::optional<Item> TakeNextFor(const std::string& conversation_id);

    std::size_t pending_total() const;
    std::size_t pending_for(const std::string& conversation_id) const;
    const InboxLimits& limits() const { return limits_; }

private:
    std::optional<Item> PopBucket(const std::string& conversation_id);

    InboxLimits limits_;
    mutable std::mutex mutex_;
    std::map<std::string, std::deque<Item>> buckets_;  // conversation -> FIFO
    std::vector<std::string> bucket_order_;            // 轮转序(map 保序,轮转指针另记)
    std::size_t rotation_cursor_ = 0;
    // sender 速率窗:sender -> 窗内时刻账。
    std::map<std::string, std::deque<std::int64_t>> sender_windows_;
    // 同正文短窗:sender|conversation|digest -> 上次接受时刻。
    std::map<std::string, std::int64_t> recent_content_;
};

}  // namespace lubancode::channel
