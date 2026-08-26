// 跨会话传话(同机首版):信封与收件信箱。
//
// 线上的信封是版本化 JSON(规格"信封"一节),正文只认纯文本。传输线程
// (platform/peer_transport_*)收到一封信,先过 PeerMailbox 这道防线——
// 同一 message_id 只收一次、同一对会话限速、相同正文短窗去重、待读队列
// 硬上限——过了才进线程安全的待读队列;主线程在轮次边界取走
// (agent/loop 的安全收件点 + main.cpp 的空闲收件)。
//
// 全部是纯逻辑(时钟由调用方注入),单测钉在 tests/unit/peer/test_peer_mailbox.cpp。
#pragma once

#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <nlohmann/json.hpp>

namespace lubancode::peers {

constexpr int kPeerEnvelopeVersion = 1;

// 线上信封。字段与规格逐条对齐;新增字段一律可选,老发送方写的新信封
// 照样能解析。
struct PeerEnvelope {
    int version = kPeerEnvelopeVersion;
    std::string message_id;    // 去重键(发送方生成的唯一 id)
    std::string sender_id;     // 发送方 peer_id
    std::string sender_name;   // 发送方显示名
    std::string target_id;     // 接收方 peer_id
    long long sent_at = 0;     // unix 秒
    std::optional<std::string> reply_to;  // 回的是哪条消息的 id,可为空
    std::string text;          // 正文,纯文本
};

nlohmann::json PeerEnvelopeToJson(const PeerEnvelope& envelope);
// 解析并校验:JSON 对象、version 认得、必填字段齐全且是字符串。不合法
// 返回 std::nullopt。
std::optional<PeerEnvelope> PeerEnvelopeFromJson(const std::string& raw);

// 传输层要回的结果(规格:发送方看得见送没送到)。
enum class PeerDelivery {
    Delivered,   // 已入收件方待读队列
    Held,        // 收件方扣住了,等它的用户点头
    Refused,     // 收件方回绝(权限档 refuse / 信封不合法)
    Expired,     // 限速撞上限 / 队列满 / 重复正文,这封不收
    Unavailable, // 对方不在(没监听、已退出)
};
const char* PeerDeliveryName(PeerDelivery status);

// 一封信投递进信箱的结果。
enum class PeerOfferStatus {
    Accepted,       // 入队
    Duplicate,      // 同一 message_id 收过了,不重复入队(不算错)
    RateLimited,    // 同一发送方短窗内信太多
    DuplicateText,  // 同一发送方相同正文短窗内已经投过
    QueueFull,      // 待读队列到了硬上限
};

class PeerMailbox {
public:
    // capacity:待读队列硬上限;rate_limit/rate_window:同一发送方在窗口
    // 秒数内最多收几封;dup_text_window:相同正文在这个秒数内视为重复。
    explicit PeerMailbox(std::size_t capacity = 64, std::size_t rate_limit = 10, int rate_window_seconds = 30,
                         int dup_text_window_seconds = 10);

    // 收件防线 + 入队。now_unix 由调用方注入(可测)。线程安全:传输线程
    // Offer、主线程 Drain,一把互斥锁隔开。
    PeerOfferStatus Offer(PeerEnvelope envelope, long long now_unix);

    // 取走全部待读信(原顺序),内部清空。
    std::vector<PeerEnvelope> Drain();

    std::size_t pending() const;

private:
    std::size_t capacity_;
    std::size_t rate_limit_;
    long long rate_window_seconds_;
    long long dup_text_window_seconds_;

    mutable std::mutex mutex_;
    std::deque<PeerEnvelope> queue_;
    std::unordered_set<std::string> seen_ids_;                 // message_id 去重(带上限,防无限长)
    std::deque<std::pair<std::string, long long>> seen_order_; // seen_ids_ 的淘汰账
    std::unordered_map<std::string, std::deque<long long>> send_times_;       // 限速窗
    std::unordered_map<std::string, std::deque<std::pair<long long, std::size_t>>> send_texts_;  // 正文去重窗
};

// 权限三档 + 默认档计算(规格"权限"一节)。mode 用 0=confirm、1=auto、
// 2=yolo 的 int,跟 cli::ConfirmMode 的取值序对齐,免得 agent 层反向依赖
// cli 层的头。
enum class PeerPermissionTier { Auto, Accept, Hold, Refuse };

// 默认收件档:
//   - 两边都要确认工具(confirm)——可直接收;
//   - 任一边处在 auto/yolo(免确认一类)——默认 hold;
//   - 两边 cwd 相隔甚远(项目不同)——默认 hold。
// 纯函数,单测钉住。
PeerPermissionTier DefaultReceiveTier(int local_mode, int remote_mode, bool cwd_far_apart);

// cwd 距离的粗判:规范化分隔符后拆段(Windows [D:, work, ...],POSIX
// [home, user, ...]),头两段不同就算"相隔甚远"——同盘/同用户下的相邻
// 项目算近。空 cwd(信息不全)按远算,保守。
bool PeerCwdFarApart(const std::string& a, const std::string& b);

}  // namespace lubancode::peers
