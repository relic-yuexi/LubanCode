// 企业微信智能机器人长连接协议纯函数(飞书与企业微信接入设计单 W1,
//§六协议细账)。一切帧皆 JSON 文本:{"cmd":...,"headers":{"req_id":...},
//"body":{...}};平台回执透传 headers.req_id、带 errcode(0 成功)。
//
// 唯一事实依据:企微官方长连接文档 developer.work.weixin.qq.com/document/
// path/101463(连接/订阅/心跳/回调/respond/频率),接收消息 path/100719,
// 主动回复 path/101138。2026-09-17 核读;错误码表官方未在长连接页逐字
// 列全——订阅/回复的分型表按企微通用错误码族钉初版,真机联调核验归 W2,
// 未知码一律按"事实照报、不猜语义"处置。
//
// 本文件零 IO:吃 JSON 吐 JSON/结构体。secret 只经参数传递,错误文案不带值。
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "channel/types.hpp"

namespace lubancode::channel::wecombot {

// 官方长连接端点(§六 6.1;测试注入 ws://127.0.0.1:...)。
inline constexpr char kWecomDefaultEndpoint[] = "wss://openws.work.weixin.qq.com";

// aibot_respond_msg 的 markdown content 上限(官方:UTF-8 字节 ≤20480)。
inline constexpr std::size_t kWecomMarkdownMaxBytes = 20'480;

// ---------------------------------------------------------------------------
// 入站帧分类与解析
// ---------------------------------------------------------------------------

enum class WecomFrameKind {
    Ack,           // 平台回执:无 cmd、有 errcode(订阅/ping/respond 的应答)
    MsgCallback,   // cmd=aibot_msg_callback(用户消息)
    EventCallback, // cmd=aibot_event_callback(enter_chat/卡片/反馈/被踢)
    Unknown,       // 认不得的 cmd / 无 cmd 无 errcode 的杂帧
};

struct WecomInboundFrame {
    WecomFrameKind kind = WecomFrameKind::Unknown;
    std::string cmd;     // callback 帧的 cmd 原值(Unknown 时也保留)
    std::string req_id;  // headers.req_id(缺省空)
    std::optional<std::int64_t> errcode;  // Ack 帧必有
    std::string errmsg;
    nlohmann::json body = nlohmann::json::object();
};

// 严格解析:非 object / cmd 非 string / Ack 帧 errcode 非整数一律拒绝
//(错误落 *error)。body 非 object 时留空 object(回调体在映射层再验)。
std::optional<WecomInboundFrame> ParseWecomInboundFrame(const nlohmann::json& payload,
                                                        std::string* error);

// ---------------------------------------------------------------------------
// 出站帧构造(订阅/心跳/回复)
// ---------------------------------------------------------------------------

// aibot_subscribe:WS 连上即发,bot_id + secret;勿重发(平台有频率保护)。
std::string BuildSubscribeFrame(const std::string& bot_id, const std::string& secret,
                                const std::string& req_id);

// 每 30s 心跳(§六 6.2;间隔归网关配置)。
std::string BuildPingFrame(const std::string& req_id);

// aibot_respond_msg:透传回调 req_id(同一次回调的分段回复共用同一
// req_id,官方口径),msgtype=markdown。content 长度由调用方先过
// SplitUtf8Chunks 分段(这里不截断——静默截正文是丢信)。
std::string BuildRespondFrame(const std::string& req_id, const std::string& content);

// ---------------------------------------------------------------------------
// aibot_msg_callback -> ChannelInboundEvent 映射
// ---------------------------------------------------------------------------

struct WecomMsgMapping {
    ChannelInboundEvent event;
    std::string warnings;  // 非致命说明(voice 转写/mixed 降级),进诊断不进正文
};

// 映射规则(W1 首版,§六 6.3):
//   - provider_event_id = message_id = body.msgid(排重键,宿主 ingress 按
//     此去重——平台可能重复推送);
//   - chattype=single → conversation=Direct,id 取 from.userid;group →
//     Group,id 取 chatid(缺失即拒);其余 chattype 拒;
//   - sender.id = from.userid 原样作身份键(创建者为超管则明文,否则平台
//     加密串——不解密,配对/allowlist 对 opaque 串一视同仁);display_name
//     留空(官方字段表未列,不猜);is_bot 恒 false;
//   - msgtype=text(text.content)/voice(voice.content 已转写,白捡)落
//     text part;mixed(msg_item 抽 msgtype=text 项,其余项落占位 part);
//     image/file/video 落标注 unsupported 的占位 part(媒体下载归 W2);
//     其余 msgtype 拒;
//   - quote 存在(object)→ hints.is_reply=true(引用正文不展开);
//   - aibotid 非空且与连接 bot_id 不符 → 拒(对账,照 QQ 互动事件规矩);
//   - hints.mentions_bot:single 恒 true;group 恒 true(企微群回调只投
//     给被 @ 的机器人——真机核验归 W2,文档位先订上)。
std::optional<WecomMsgMapping> MapMsgCallback(const nlohmann::json& body,
                                              const std::string& channel_id,
                                              const std::string& account_id,
                                              const std::string& bot_id,
                                              const std::string& delivery_id,
                                              std::int64_t received_at_ms,
                                              std::string* error);

// ---------------------------------------------------------------------------
// aibot_event_callback(§六 6.4:只记日志不入模型;disconnected_event 归
// 网关处置——新连踢旧,收到即断不再抢线)
// ---------------------------------------------------------------------------

inline constexpr char kWecomEventEnterChat[] = "enter_chat";
inline constexpr char kWecomEventTemplateCard[] = "template_card_event";
inline constexpr char kWecomEventFeedback[] = "feedback_event";
inline constexpr char kWecomEventDisconnected[] = "disconnected_event";

struct WecomEventCallbackInfo {
    std::string eventtype;  // event.eventtype
    std::string msgid;      // 排重键(事件也带 msgid)
};

// 严格映射:非 object / 缺 event.eventtype(string)拒绝。
std::optional<WecomEventCallbackInfo> ParseEventCallback(const nlohmann::json& body,
                                                         std::string* error);

// ---------------------------------------------------------------------------
// 错误分型(初版表:官方长连接页未列全错误码,按企微通用错误码族钉;
// 未知码照报事实不猜语义,真机核验归 W2)
// ---------------------------------------------------------------------------

// 订阅回执 errcode 分型:凭据错即止(换密钥前重试无意义,退避也不发)。
enum class WecomSubscribeStatus {
    Ok,
    CredentialRejected,  // 40013/40014/40029/41001/41004(不合法凭据族)
    Transient,           // 其余(限流保护/服务端错):退避重连
};
WecomSubscribeStatus ClassifySubscribeErrcode(std::int64_t errcode);

// respond 回执 errcode 分型(发送线程的重试裁决)。
enum class WecomRespondStatus {
    Ok,
    RateLimited,  // 限流族:延后重试(同 req_id 重发合法)
    Rejected,     // 窗口过期/内容拒绝/权限等:永久拒绝,带码如实报
};
WecomRespondStatus ClassifyRespondErrcode(std::int64_t errcode);

// ---------------------------------------------------------------------------
// markdown 分段(content ≤20480 字节 UTF-8;在码点边界切,不截半个字)
// ---------------------------------------------------------------------------

std::vector<std::string> SplitUtf8Chunks(const std::string& text, std::size_t max_bytes);

}  // namespace lubancode::channel::wecombot
