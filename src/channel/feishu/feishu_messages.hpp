// 飞书回话发送(飞书/企微设计单 F1,§5.8):POST
// /open-apis/im/v1/messages/{message_id}/reply,锚 message_id(同 QQ 的
// msg_id 锚纪律——被动回复挂来信锚点,超长拆段归宿主 outbox)。
//
// 首版只发 msg_type=text(范围纪律:媒体/卡片/post 全不做)。Unauthorized
// 刷 token 重试一次(同 QqMessageSender 的受控刷新路,不连环刷)。
#pragma once

#include <string>

#include "channel/feishu/feishu_auth.hpp"
#include "channel/feishu/feishu_http.hpp"
#include "channel/feishu/feishu_proto.hpp"

namespace lubancode::channel::feishu {

struct FeishuReplyRequest {
    std::string message_id;  // 被动回复锚(来信 message_id);空 = 主动消息
                             //(首版不宣称,装配层拒绝)
    std::string text;        // 纯文本(msg_type=text)
    std::string outbound_delivery_id;  // 宿主 delivery 账(不入平台载荷)
};

class FeishuMessageSender {
public:
    struct Options {
        FeishuHttpFunc http;
        FeishuTokenManager* tokens = nullptr;
        std::string open_base = "https://open.feishu.cn";  // 首版仅国内域
        int io_timeout_ms = 15'000;
    };

    struct Outcome {
        enum class Status {
            Sent,           // 平台已接受(provider_message_id 有值)
            DeferredRetry,  // 限频/5xx/网络:调用方退避后重试(同载荷)
            PermanentFail,  // 权限拒绝/未知业务码/形状错:不再自动重试
        };
        Status status = Status::PermanentFail;
        std::string provider_message_id;
        FeishuApiError error;  // 非 Sent 时的分型账
    };

    explicit FeishuMessageSender(Options options) : options_(std::move(options)) {}

    // 发送(或重试)一条被动回复。回话窗口/超长拆段归宿主 outbox;这里
    // 只管一次 HTTP 的成败分型。
    Outcome SendReply(const FeishuReplyRequest& request);

private:
    Options options_;
};

}  // namespace lubancode::channel::feishu
