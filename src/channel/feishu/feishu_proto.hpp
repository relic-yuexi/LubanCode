// 飞书协议纯函数层(飞书/企微设计单 F1,§5):事件体映射、发送体构造、
// 引导/令牌响应解析、错误分型。本文件零 IO:吃 JSON 吐结构体;AppSecret/
// token 只经参数与请求体传递,错误文案不带值。
//
// 真源:设计单 §5(逐条钉死)+ 飞书官方文档(open.feishu.cn,事件订阅
// im.message.receive_v1 / im/v1/messages reply / auth/v3 tenant_access_token)。
// 官方错误码表设计单未逐枚钉死的(发送侧业务码),分型保守:HTTP 状态为
// 主、业务码记账进 detail,不猜不枚举未核实的码义。
#pragma once

#include <cstdint>
#include <optional>
#include <string>

#include <nlohmann/json.hpp>

#include "channel/types.hpp"

namespace lubancode::channel::feishu {

// ---------------------------------------------------------------------------
// 引导(§5.1):POST /callback/ws/endpoint → WSS 地址 + ClientConfig
// ---------------------------------------------------------------------------

nlohmann::json BuildBootstrapRequest(const std::string& app_id,
                                     const std::string& app_secret);

// 引导业务码(设计单 §5.1 钉死):0 OK;1 SystemBusy;1000040343
// InternalError。其余码未钉死——按"被平台拒绝"归类(非重试),码值进账。
struct FeishuBootstrapEndpoint {
    std::string url;                        // data.URL(wss://…,query 已拼好)
    std::int64_t ping_interval_secs = 120;  // ClientConfig.PingInterval(缺省)
};
std::optional<FeishuBootstrapEndpoint> ParseBootstrapResponse(const nlohmann::json& body,
                                                              std::string* error);

// 引导失败的稳定分型(适配器折算连接错误用)。
enum class FeishuBootstrapErrorKind {
    ServerError,        // 业务码 1/1000040343、HTTP 5xx:可重试
    RateLimited,        // HTTP 429:可重试
    InvalidCredentials, // HTTP 401/403、未知业务码:配置侧问题,不重试
    BadResponse,        // 形状解不开:不重试(重试同形状也无解)
};
FeishuBootstrapErrorKind ClassifyBootstrapFailure(int http_status,
                                                  const nlohmann::json& body);

// ---------------------------------------------------------------------------
// tenant_access_token(§5.8)
// ---------------------------------------------------------------------------

nlohmann::json BuildTenantTokenRequest(const std::string& app_id,
                                       const std::string& app_secret);

struct FeishuTenantToken {
    std::string tenant_access_token;
    std::int64_t expire_secs = 0;  // 官方字段 expire(秒);expires_in 兼容兜底
};
// 响应 {"code":0,"tenant_access_token":…,"expire":…}:code 非 0、缺字段、
// 空 token、非正 expire 一律拒绝。
std::optional<FeishuTenantToken> ParseTenantTokenResponse(const nlohmann::json& body,
                                                          std::string* error);

// ---------------------------------------------------------------------------
// im.message.receive_v1 → ChannelInboundEvent(§5.7)
// ---------------------------------------------------------------------------

struct FeishuEventMapping {
    enum class Kind {
        Mapped,               // event 即映射产物
        UnsupportedEventType, // 事件类型认得协议但不建模(如 card 回调事件):
                             // 明确终结(计数留痕),不是错误
        Invalid,              // 事件体解不开(缺必填字段/形状坏):明确终结留痕
    };
    Kind kind = Kind::Invalid;
    ChannelInboundEvent event;  // Kind::Mapped 时有值
    std::string detail;         // Unsupported/Invalid 的人话(不带敏感值)
};

// 映射规则(设计单 §5.7 钉死):
//   - provider_event_id = header.event_id;schema 须 "2.0";
//   - header.app_id 非空时须等于 expected_app_id(与连接账号对账,对不上
//     按无效丢弃——QQ 互动事件的同款纪律);
//   - conversation:chat_type=p2p → Direct、group → Group,id = chat_id;
//   - sender.id = sender.sender_id.open_id;is_bot = sender_type=="app";
//     is_owner 恒 false(宿主 allowlist 推导,适配器不得自称);
//   - message_type=text → text part(content 是嵌 JSON 字符串
//     {"text":"…"});其余 message_type 映成标注 unsupported 的占位 part
//     (§范围纪律:首版不做媒体,占位不丢弃);
//   - hints.mentions_bot:p2p 恒 true(单聊天然发给 bot);群聊首版不解析
//     @(§十后置),恒 false——群聊默认 disabled,真开群须关 require_mention。
// expected_app_id 为空 = 不对账(观测型装配)。
FeishuEventMapping MapFeishuEventPayload(const nlohmann::json& payload,
                                         const std::string& channel_id,
                                         const std::string& account_id,
                                         const std::string& expected_app_id,
                                         const std::string& delivery_id,
                                         std::int64_t received_at_ms);

// ---------------------------------------------------------------------------
// 回话(§5.8):POST /open-apis/im/v1/messages/{message_id}/reply
// ---------------------------------------------------------------------------

// 请求体:{"msg_type":"text","content":"{\"text\":…}"}——content 是嵌 JSON
// 字符串(官方合同),非二次转义会让平台按坏体拒。
nlohmann::json BuildReplyPayload(const std::string& text);
std::string ReplyPath(const std::string& message_id);

struct FeishuReplyResponse {
    std::string provider_message_id;  // data.message_id
};
std::optional<FeishuReplyResponse> ParseReplyResponse(const nlohmann::json& body,
                                                      std::string* error);

// ---------------------------------------------------------------------------
// 发送/令牌侧错误分型(HTTP 状态为主;业务码进 detail 不猜义)
// ---------------------------------------------------------------------------

enum class FeishuApiErrorKind {
    RateLimited,      // HTTP 429:延后重试
    Unauthorized,     // HTTP 401:刷 token 后可重试一次
    PermissionDenied, // HTTP 403:权限未开,永久拒绝
    ServerError,      // HTTP 5xx:可重试
    InvalidResponse,  // 2xx 但成功合同无法核对(body 解不出/缺 message_id)
    NetworkError,     // 传输失败(调用方折算进来)
    UnknownError,     // 其余 4xx / 业务码非 0:永久拒绝,码值进账
};

struct FeishuApiError {
    FeishuApiErrorKind kind = FeishuApiErrorKind::UnknownError;
    int http_status = 0;
    std::int64_t platform_code = 0;  // 业务码(有则;0 = 无)
    std::string detail;              // 脱敏;不带 token/正文
};

FeishuApiError ClassifyFeishuApiFailure(int http_status, const std::string& body);

}  // namespace lubancode::channel::feishu
