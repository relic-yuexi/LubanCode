// Channel Bridge v1 协议合同(多渠道消息接入单阶段 1)。
//
// 唯一真源是 docs/architecture/channels/bridge-protocol.md。这里定:
//   - 协议常量(协议名、握手版本);
//   - 19 枚正文 method(§4 宿主发给 sidecar 12 项 + §5 sidecar 发给宿主
//     7 项)+ 握手 method channel.initialize(§3,文档单独记账,不计入
//     "19"的口径);
//   - JSON-RPC 2.0 五枚标准错误码 + 15 个 domain 错误稳定名(§6);
//   - 消息构造/解析(BuildXxxJson / ParseIncomingMessage);
//   - 逐 method 的 params/result 形状表驱动校验(顶层字段级,风格照
//     trajectory/schema.cpp 的 PayloadField,不递归校验嵌套对象内部——
//     嵌套形状(conversation、parts 等)已由 channel/types.hpp 的
//     ChannelInboundEvent::FromJsonStrict 等一条龙校验,这里不重复越界
//     去管"parts[].mime_type 该是什么类型"这种下一层的账)。
//
// 帧字节编解码(4B 大端长度前缀 + UTF-8 JSON)另在 channel/frame.hpp;
// 本文件只管"一份已解出的 JSON 正文长什么样、该配哪个 method"。
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include <nlohmann/json.hpp>

namespace lubancode::channel {

// ---------------------------------------------------------------------------
// 协议常量(bridge-protocol.md §1)
// ---------------------------------------------------------------------------

inline constexpr std::string_view kBridgeProtocolName = "lubancode-channel/1";
inline constexpr std::string_view kBridgeHandshakeProtocolVersion = "2026-08-29";

// ---------------------------------------------------------------------------
// BridgeMethod
// ---------------------------------------------------------------------------

enum class BridgeMethod {
    // 握手(§3):host -> sidecar,request。文档单独记账,不计入"19 枚
    // method"的口径(§4/§5 标题下合计 12+7=19)。
    Initialize,
    // 宿主发给 sidecar(§4,12 项)
    Start,
    Stop,
    Health,
    Send,
    Edit,
    Typing,  // notification
    React,
    LoginBegin,
    LoginCancel,
    Logout,
    InboundAck,
    InboundNack,
    // sidecar 发给宿主(§5,7 项,全部 notification)
    Inbound,
    Status,
    DeliveryReceipt,
    LoginChallenge,
    LoginCompleted,
    CapabilitiesChanged,
    Fatal,
};

// 线上方法名,如 "channel.initialize"/"channel.inbound.ack"。
const char* BridgeMethodName(BridgeMethod method);
std::optional<BridgeMethod> BridgeMethodFromName(std::string_view name);

// notification(无 id,不产生 response)还是 request(有 id,须回 response)。
bool IsNotificationMethod(BridgeMethod method);

enum class BridgeDirection { HostToSidecar, SidecarToHost };
BridgeDirection MethodDirection(BridgeMethod method);

// ---------------------------------------------------------------------------
// JSON-RPC 标准错误码(§6)
// ---------------------------------------------------------------------------

enum class JsonRpcStandardErrorCode : int {
    ParseError = -32700,
    InvalidRequest = -32600,
    MethodNotFound = -32601,
    InvalidParams = -32602,
    InternalError = -32603,
};

// ---------------------------------------------------------------------------
// domain 错误稳定名(§6:统一挂 -32000 到 -32099 段;message 放稳定名,
// data.detail 放脱敏细节。冻结文档没有给每个稳定名单独编号,这里用同一
// 个代表码 kDomainErrorCode,靠 message 里的稳定名区分——与文档字面一致)。
// ---------------------------------------------------------------------------

inline constexpr int kDomainErrorCode = -32000;

enum class DomainErrorName {
    ProtocolIncompatible,  // 握手版本不认得,明败不重试
    FrameTooLarge,         // 单帧超 8 MiB,立即停 adapter 进 backoff
    InvalidUtf8,           // 帧内坏 UTF-8,同上
    InvalidFrame,          // JSON 解不开或非 object,同上
    UnknownDeliveryId,     // ack/nack 对不上已上报 delivery,丢弃留诊断账
    NotCapable,            // 平台无此能力,调用方回落不重试
    RateLimited,           // 平台 429,尊重 Retry-After
    PermanentReject,       // 平台永久 4xx,dead letter
    TransportFailed,       // 发送层网络/5xx,退避重试
    SpoolWriteFailed,      // sidecar spool 落盘失败
    LoginRequired,         // 凭据缺失或失效,账号转 NeedsLogin
    AccountRevoked,        // 账号被平台吊销,不自动重试
    SpawnFailed,           // sidecar 起不来,账号 Fatal
    ProcessCrashed,        // sidecar 中途退出,重启计数
    ShutdownTimeout,       // 停机宽限用尽,杀进程树
};

std::string_view DomainErrorStableName(DomainErrorName name);
std::optional<DomainErrorName> DomainErrorNameFromStableName(std::string_view name);

// ---------------------------------------------------------------------------
// 消息构造(host/sidecar 双方都用同一套构造函数;方向由调用点决定用哪个
// method,函数本身不认"谁在发")。
// ---------------------------------------------------------------------------

nlohmann::json BuildRequestJson(std::int64_t id, BridgeMethod method, const nlohmann::json& params);
nlohmann::json BuildNotificationJson(BridgeMethod method, const nlohmann::json& params);
nlohmann::json BuildResultResponseJson(std::int64_t id, const nlohmann::json& result);
nlohmann::json BuildStandardErrorResponseJson(std::int64_t id, JsonRpcStandardErrorCode code,
                                              std::string_view message);
nlohmann::json BuildDomainErrorResponseJson(std::int64_t id, DomainErrorName name,
                                            std::string_view detail);

// ---------------------------------------------------------------------------
// 消息解析(framing 规矩,§2):jsonrpc 须是 "2.0";request 有 id+method,
// notification 有 method 无 id;response 有 id,恰好 result 或 error 之一。
// 陌生 method 仍归类为 Request/Notification,只是 method 字段留空——调用
// 方据此回 -32601,不静默吞(§2)。
// ---------------------------------------------------------------------------

enum class IncomingMessageKind { Request, Notification, ResultResponse, ErrorResponse, Malformed };

struct IncomingMessage {
    IncomingMessageKind kind = IncomingMessageKind::Malformed;
    std::optional<std::int64_t> id;
    std::string method_name;             // Request/Notification 原文方法名
    std::optional<BridgeMethod> method;  // nullopt = 陌生 method
    nlohmann::json params = nlohmann::json::object();
    nlohmann::json result;
    int error_code = 0;
    std::string error_message;
    nlohmann::json error_data = nlohmann::json::object();
    std::string malformed_reason;  // kind == Malformed 时的诊断
};

IncomingMessage ParseIncomingMessage(const nlohmann::json& frame_json);

// ---------------------------------------------------------------------------
// 逐 method 的 params/result 形状(顶层字段级,类型码同 trajectory/
// schema.cpp:s/i/u/b/o/a/any,后缀 '|' 表可空)。只钉顶层键与类型,不递归
// ——nested 形状交给各自的强类型 parser(ChannelInboundEvent 等)。
// ---------------------------------------------------------------------------

std::optional<std::string> ValidateMethodParamsShape(BridgeMethod method, const nlohmann::json& params);
// notification 没有 result;对 notification 调用永远返回 std::nullopt
// (无形状可校)。
std::optional<std::string> ValidateMethodResultShape(BridgeMethod method, const nlohmann::json& result);

}  // namespace lubancode::channel
