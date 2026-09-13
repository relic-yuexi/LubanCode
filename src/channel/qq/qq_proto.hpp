// QQ 开放平台 v2 协议纯函数(QQ 机器人接入单 Q1)。
//
// 唯一事实依据:官方 bot.q.qq.com v2 文档(定案记入 todo §十五):
//   - 通用 payload/opcode/intents:dev-prepare/event-emit/payload.html
//   - C2C 事件:autogen/event/c2c_message_create.html(事件体三例固化为测试 fixture)
//   - 发送单聊消息:autogen/api/v2_users_user_openid_messages.post.html
//     (msg_type/msg_id/msg_seq 合同与全量错误码表)
// 官方文档与 SDK 冲突处(如富媒体上传分片起点)保留为测试案例,不静默采信。
// Ready/Resume 的 d 字段形状官方 payload 页未逐字列全,按 opcode 表语义与
// 官方 SDK 交叉核;真平台联调核验归 Q3,todo 记"未验"。
//
// 本文件零 IO:吃 JSON 吐 JSON/结构体。token 值只经参数传递,错误文案不带值。
#pragma once

#include <cstdint>
#include <optional>
#include <string>

#include <nlohmann/json.hpp>

#include "channel/types.hpp"

namespace lubancode::channel::qq {

// ---------------------------------------------------------------------------
// 网关 payload(官方通用数据结构页)
// ---------------------------------------------------------------------------

// intents 位(官方事件订阅表)。Q1 只订单聊事件;群聊/互动后批再开。
inline constexpr std::uint32_t kIntentGroupAndC2cEvent = 1u << 25;

enum class GatewayOp {
    Dispatch = 0,       // 服务端推送
    Heartbeat = 1,      // 客户端发,携带最新 s
    Identify = 2,       // 客户端鉴权
    Resume = 6,         // 客户端恢复连接
    Reconnect = 7,      // 服务端通知重连
    InvalidSession = 9, // identify/resume 参数错
    Hello = 10,         // 连接后首条,带心跳间隔
    HeartbeatAck = 11,  // 心跳回执
};

std::optional<GatewayOp> GatewayOpFromInt(int value);

struct GatewayPayload {
    int op_raw = -1;              // 原文 op(陌生值保留,-1 = 字段缺失)
    std::optional<GatewayOp> op;  // 认得的 op
    std::int64_t s = -1;          // 下行序号;缺省 -1(心跳未收到过任何事件时发 null)
    std::string id;               // 事件 id(外层)
    std::string t;                // 事件类型(op=0 Dispatch)
    nlohmann::json d = nlohmann::json::object();
};

// 严格解析:op 缺失或非整数、d 存在但非 object 一律拒绝(错误落 *error)。
std::optional<GatewayPayload> ParseGatewayPayload(const nlohmann::json& payload,
                                                  std::string* error);

// Hello(op=10)的 d.heartbeat_interval_ms。缺失/非正数返回 nullopt。
std::optional<std::int64_t> ParseHelloInterval(const nlohmann::json& d);

// Identify(op=2):token 前缀 "QQBot "(官方鉴权口径,AccessToken 鉴权)。
nlohmann::json BuildIdentify(const std::string& access_token, std::uint32_t intents);

// Heartbeat(op=1):d = 最新 s(没收到过事件为 null——官方 opcode 表"携带客户端
// 收到的最新的 s")。
nlohmann::json BuildHeartbeat(std::optional<std::int64_t> last_seq);

// Resume(op=6):d = {token, session_id, seq}。
nlohmann::json BuildResume(const std::string& access_token, const std::string& session_id,
                           std::int64_t seq);

// Ready(t=READY)的 d:{session_id, user{id}}。
struct GatewayReadyInfo {
    std::string session_id;
    std::string user_id;
};
std::optional<GatewayReadyInfo> ParseReady(const nlohmann::json& d, std::string* error);

// Invalid Session(op=9)的 d 是 bool:可恢复(true,重连后 Resume)/不可(false,
// 清 session 重新 Identify)。
std::optional<bool> ParseInvalidSessionResumable(const nlohmann::json& payload);

// ---------------------------------------------------------------------------
// RFC3339 时间戳(事件 timestamp 字段)
// ---------------------------------------------------------------------------

// "2026-07-21T10:00:00+08:00" / 带 Z / 带小数秒 -> epoch 毫秒。解析不动原文。
std::optional<std::int64_t> ParseRfc3339Ms(const std::string& text);

// ---------------------------------------------------------------------------
// C2C_MESSAGE_CREATE -> ChannelInboundEvent 映射
// ---------------------------------------------------------------------------

struct C2cEventMapping {
    ChannelInboundEvent event;
    std::string warnings;  // 非致命说明(卡片未展开/附件降级),进诊断不进模型正文
};

// 映射规则(Q1 首版):
//   - conversation = Direct,id 取 author.user_openid(单聊会话键);
//   - sender.id = user_openid,display_name = username,is_bot = author.bot;
//     is_owner 恒 false(宿主 allowlist 推导,sidecar 不得自称);
//   - provider_event_id = d.id + "|" + msg_idx(message_scene.ext 有则拼,无则
//     单独 d.id)——官方"相同 msg_id 可能重复推送,结合 msg_seq 去重";
//   - message_type=0/3 落 text part(content 即平台摘要);103 落 text 并置
//     hints.is_reply(引用正文 Q4 展开);其余未知类型拒绝;
//   - attachments 首版一律 Unsupported part(媒体下载 Q4),不虚报已接通;
//   - hints.mentions_bot = true(单聊消息天然发给 bot)。
std::optional<C2cEventMapping> MapC2cMessageCreate(const nlohmann::json& d,
                                                   const std::string& envelope_event_id,
                                                   const std::string& channel_id,
                                                   const std::string& account_id,
                                                   const std::string& delivery_id,
                                                   std::int64_t received_at_ms,
                                                   std::string* error);

// ---------------------------------------------------------------------------
// Access Token(官方鉴权:POST /app/getAppAccessToken @ bots.qq.com)
// ---------------------------------------------------------------------------

nlohmann::json BuildAccessTokenRequest(const std::string& app_id,
                                       const std::string& client_secret);

struct AccessTokenResponse {
    std::string access_token;
    std::int64_t expires_in_secs = 0;
};

// 响应 {access_token, expires_in};缺字段/空 token 拒绝。
std::optional<AccessTokenResponse> ParseAccessTokenResponse(const nlohmann::json& body,
                                                            std::string* error);

// ---------------------------------------------------------------------------
// v2 发送单聊消息
// ---------------------------------------------------------------------------

struct C2cSendRequest {
    std::string openid;              // 接收方 user_openid
    std::string content;             // 纯文本(msg_type=0)
    std::string msg_id;              // 被动回复锚(来信 d.id);主动消息留空
    std::uint32_t msg_seq = 1;       // 与 msg_id 联合防重;同回复重试复用同一值
    std::string outbound_delivery_id;  // 宿主 delivery 账(不入平台载荷)
};

// POST https://api.sgroup.qq.com/v2/users/{openid}/messages 的请求体。
nlohmann::json BuildC2cSendPayload(const C2cSendRequest& request);
std::string C2cSendPath(const std::string& openid);

struct C2cSendResponse {
    std::string provider_message_id;  // 响应 id
    std::string timestamp;
    std::string ref_idx;  // ext_info.ref_idx(可空)
};
std::optional<C2cSendResponse> ParseC2cSendResponse(const nlohmann::json& body,
                                                    std::string* error);

// ---------------------------------------------------------------------------
// 平台错误分型(官方发送接口错误码表)
// ---------------------------------------------------------------------------

enum class QqApiErrorKind {
    RateLimited,        // 40034100 / HTTP 429:延后重试
    MsgIdExpired,       // 304103/40034005/40034128/40034026:回复窗口过期,不重试
    Deduped,            // 40054005:同 msg_id+msg_seq 平台已收——按已送达收账
    NoFriend,           // 40054004:无好友关系,永久拒绝
    UserRejected,       // 40054013:用户拒收,永久拒绝
    ContentRejected,    // 40034006/304061/40054007/40054018/22006/304080:内容/形状永久拒绝
    Unauthorized,       // HTTP 401/403:token 失效,强制刷新后可重试
    ServerError,        // HTTP 5xx / 50055002:可重试
    InvalidResponse,    // 2xx 但 body 解不出
    NetworkError,       // 传输失败(调用方折算进来)
    UnknownError,       // 其余 4xx
};

struct QqApiError {
    QqApiErrorKind kind = QqApiErrorKind::UnknownError;
    int http_status = 0;
    std::int64_t platform_code = 0;  // body 里的 code 字段(有则)
    std::string detail;              // 脱敏;不带 token/正文
};

// 按 HTTP 状态 + body(腾讯错误体 {"code":..,"message":..} 或 HTML)分型。
QqApiError ClassifyQqSendFailure(int http_status, const std::string& body);

}  // namespace lubancode::channel::qq
