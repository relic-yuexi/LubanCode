// QQ v2 消息发送(QQ 机器人接入单 Q1):POST /v2/users/{openid}/messages、
// 稳定 msg_seq、重试复用同一载荷(§十五 messages 模块)。
//
// 稳定载荷合同(单 §七"相同 delivery_id 的网络重试复用同一发送载荷"):
// 同一 outbound delivery 的每次尝试发同一 msg_seq——QQ 按 msg_id+msg_seq
// 去重(官方 40054005),重试换序号会让"重试"变成"多一条新消息"。同一
// msg_id 的不同回复各占一个序号(官方:每条消息最多回复 4 次,序号区分)。
#pragma once

#include <cstdint>
#include <map>
#include <mutex>
#include <optional>
#include <string>

#include "channel/qq/qq_auth.hpp"
#include "channel/qq/qq_http.hpp"
#include "channel/qq/qq_proto.hpp"

namespace lubancode::channel::qq {

class QqMessageSender {
public:
    struct Options {
        QqHttpFunc http;
        QqTokenManager* tokens = nullptr;
        std::string api_base = "https://api.sgroup.qq.com";
        int io_timeout_ms = 15'000;
    };

    struct Outcome {
        enum class Status {
            Sent,           // 平台已接受(provider_message_id 有值)
            Deduped,        // 40054005:平台此前已收同一载荷——按已送达收账
            DeferredRetry,  // 限频/5xx/网络:调用方退避后重试(同载荷)
            PermanentFail,  // 窗口过期/内容拒绝/无好友/拒收/未知 4xx:不再自动重试
        };
        Status status = Status::PermanentFail;
        std::string provider_message_id;
        QqApiError error;  // 非 Sent 时的分型账
    };

    explicit QqMessageSender(Options options) : options_(std::move(options)) {}

    // 发送(或按冻结载荷重试)一条被动回复。request.msg_id 为空 = 主动消息
    // (首版配置不开,载荷不带 msg_id/msg_seq)。
    Outcome SendC2c(const C2cSendRequest& request);

    // 诊断:当前 msg_seq 记账规模。
    std::size_t frozen_delivery_count() const;

private:
    // 给 delivery 分配(并冻结)msg_seq。
    std::uint32_t AssignSeq(const C2cSendRequest& request);

    Options options_;
    std::mutex mutex_;                          // seq 记账串行
    std::map<std::string, std::uint32_t> next_seq_by_msg_id_;
    std::map<std::string, std::uint32_t> frozen_seq_by_delivery_;
    std::size_t frozen_done_ = 0;  // 已终结 delivery 的清理计数(防 map 无界涨)
};

}  // namespace lubancode::channel::qq
