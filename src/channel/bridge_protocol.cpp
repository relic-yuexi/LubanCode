#include "channel/bridge_protocol.hpp"

namespace lubancode::channel {

// ---------------------------------------------------------------------------
// BridgeMethod
// ---------------------------------------------------------------------------

const char* BridgeMethodName(BridgeMethod method) {
    switch (method) {
        case BridgeMethod::Initialize: return "channel.initialize";
        case BridgeMethod::Start: return "channel.start";
        case BridgeMethod::Stop: return "channel.stop";
        case BridgeMethod::Health: return "channel.health";
        case BridgeMethod::Send: return "channel.send";
        case BridgeMethod::Edit: return "channel.edit";
        case BridgeMethod::Typing: return "channel.typing";
        case BridgeMethod::React: return "channel.react";
        case BridgeMethod::LoginBegin: return "channel.login.begin";
        case BridgeMethod::LoginCancel: return "channel.login.cancel";
        case BridgeMethod::Logout: return "channel.logout";
        case BridgeMethod::InboundAck: return "channel.inbound.ack";
        case BridgeMethod::InboundNack: return "channel.inbound.nack";
        case BridgeMethod::Inbound: return "channel.inbound";
        case BridgeMethod::Status: return "channel.status";
        case BridgeMethod::DeliveryReceipt: return "channel.delivery.receipt";
        case BridgeMethod::LoginChallenge: return "channel.login.challenge";
        case BridgeMethod::LoginCompleted: return "channel.login.completed";
        case BridgeMethod::CapabilitiesChanged: return "channel.capabilities.changed";
        case BridgeMethod::Fatal: return "channel.fatal";
    }
    return "?";
}

std::optional<BridgeMethod> BridgeMethodFromName(std::string_view name) {
    if (name == "channel.initialize") return BridgeMethod::Initialize;
    if (name == "channel.start") return BridgeMethod::Start;
    if (name == "channel.stop") return BridgeMethod::Stop;
    if (name == "channel.health") return BridgeMethod::Health;
    if (name == "channel.send") return BridgeMethod::Send;
    if (name == "channel.edit") return BridgeMethod::Edit;
    if (name == "channel.typing") return BridgeMethod::Typing;
    if (name == "channel.react") return BridgeMethod::React;
    if (name == "channel.login.begin") return BridgeMethod::LoginBegin;
    if (name == "channel.login.cancel") return BridgeMethod::LoginCancel;
    if (name == "channel.logout") return BridgeMethod::Logout;
    if (name == "channel.inbound.ack") return BridgeMethod::InboundAck;
    if (name == "channel.inbound.nack") return BridgeMethod::InboundNack;
    if (name == "channel.inbound") return BridgeMethod::Inbound;
    if (name == "channel.status") return BridgeMethod::Status;
    if (name == "channel.delivery.receipt") return BridgeMethod::DeliveryReceipt;
    if (name == "channel.login.challenge") return BridgeMethod::LoginChallenge;
    if (name == "channel.login.completed") return BridgeMethod::LoginCompleted;
    if (name == "channel.capabilities.changed") return BridgeMethod::CapabilitiesChanged;
    if (name == "channel.fatal") return BridgeMethod::Fatal;
    return std::nullopt;
}

bool IsNotificationMethod(BridgeMethod method) {
    switch (method) {
        case BridgeMethod::Typing:
        case BridgeMethod::Inbound:
        case BridgeMethod::Status:
        case BridgeMethod::DeliveryReceipt:
        case BridgeMethod::LoginChallenge:
        case BridgeMethod::LoginCompleted:
        case BridgeMethod::CapabilitiesChanged:
        case BridgeMethod::Fatal:
            return true;
        default:
            return false;
    }
}

BridgeDirection MethodDirection(BridgeMethod method) {
    switch (method) {
        case BridgeMethod::Initialize:
        case BridgeMethod::Start:
        case BridgeMethod::Stop:
        case BridgeMethod::Health:
        case BridgeMethod::Send:
        case BridgeMethod::Edit:
        case BridgeMethod::Typing:
        case BridgeMethod::React:
        case BridgeMethod::LoginBegin:
        case BridgeMethod::LoginCancel:
        case BridgeMethod::Logout:
        case BridgeMethod::InboundAck:
        case BridgeMethod::InboundNack:
            return BridgeDirection::HostToSidecar;
        default:
            return BridgeDirection::SidecarToHost;
    }
}

// ---------------------------------------------------------------------------
// domain 错误稳定名
// ---------------------------------------------------------------------------

std::string_view DomainErrorStableName(DomainErrorName name) {
    switch (name) {
        case DomainErrorName::ProtocolIncompatible: return "protocol_incompatible";
        case DomainErrorName::FrameTooLarge: return "frame_too_large";
        case DomainErrorName::InvalidUtf8: return "invalid_utf8";
        case DomainErrorName::InvalidFrame: return "invalid_frame";
        case DomainErrorName::UnknownDeliveryId: return "unknown_delivery_id";
        case DomainErrorName::NotCapable: return "not_capable";
        case DomainErrorName::RateLimited: return "rate_limited";
        case DomainErrorName::PermanentReject: return "permanent_reject";
        case DomainErrorName::TransportFailed: return "transport_failed";
        case DomainErrorName::SpoolWriteFailed: return "spool_write_failed";
        case DomainErrorName::LoginRequired: return "login_required";
        case DomainErrorName::AccountRevoked: return "account_revoked";
        case DomainErrorName::SpawnFailed: return "spawn_failed";
        case DomainErrorName::ProcessCrashed: return "process_crashed";
        case DomainErrorName::ShutdownTimeout: return "shutdown_timeout";
    }
    return "?";
}

std::optional<DomainErrorName> DomainErrorNameFromStableName(std::string_view name) {
    if (name == "protocol_incompatible") return DomainErrorName::ProtocolIncompatible;
    if (name == "frame_too_large") return DomainErrorName::FrameTooLarge;
    if (name == "invalid_utf8") return DomainErrorName::InvalidUtf8;
    if (name == "invalid_frame") return DomainErrorName::InvalidFrame;
    if (name == "unknown_delivery_id") return DomainErrorName::UnknownDeliveryId;
    if (name == "not_capable") return DomainErrorName::NotCapable;
    if (name == "rate_limited") return DomainErrorName::RateLimited;
    if (name == "permanent_reject") return DomainErrorName::PermanentReject;
    if (name == "transport_failed") return DomainErrorName::TransportFailed;
    if (name == "spool_write_failed") return DomainErrorName::SpoolWriteFailed;
    if (name == "login_required") return DomainErrorName::LoginRequired;
    if (name == "account_revoked") return DomainErrorName::AccountRevoked;
    if (name == "spawn_failed") return DomainErrorName::SpawnFailed;
    if (name == "process_crashed") return DomainErrorName::ProcessCrashed;
    if (name == "shutdown_timeout") return DomainErrorName::ShutdownTimeout;
    return std::nullopt;
}

// ---------------------------------------------------------------------------
// 消息构造
// ---------------------------------------------------------------------------

nlohmann::json BuildRequestJson(std::int64_t id, BridgeMethod method, const nlohmann::json& params) {
    nlohmann::json out = nlohmann::json::object();
    out["jsonrpc"] = "2.0";
    out["id"] = id;
    out["method"] = BridgeMethodName(method);
    out["params"] = params.is_null() ? nlohmann::json::object() : params;
    return out;
}

nlohmann::json BuildNotificationJson(BridgeMethod method, const nlohmann::json& params) {
    nlohmann::json out = nlohmann::json::object();
    out["jsonrpc"] = "2.0";
    out["method"] = BridgeMethodName(method);
    out["params"] = params.is_null() ? nlohmann::json::object() : params;
    return out;
}

nlohmann::json BuildResultResponseJson(std::int64_t id, const nlohmann::json& result) {
    nlohmann::json out = nlohmann::json::object();
    out["jsonrpc"] = "2.0";
    out["id"] = id;
    out["result"] = result;
    return out;
}

nlohmann::json BuildStandardErrorResponseJson(std::int64_t id, JsonRpcStandardErrorCode code,
                                              std::string_view message) {
    nlohmann::json out = nlohmann::json::object();
    out["jsonrpc"] = "2.0";
    out["id"] = id;
    nlohmann::json error = nlohmann::json::object();
    error["code"] = static_cast<int>(code);
    error["message"] = std::string(message);
    out["error"] = std::move(error);
    return out;
}

nlohmann::json BuildDomainErrorResponseJson(std::int64_t id, DomainErrorName name,
                                            std::string_view detail) {
    nlohmann::json out = nlohmann::json::object();
    out["jsonrpc"] = "2.0";
    out["id"] = id;
    nlohmann::json error = nlohmann::json::object();
    error["code"] = kDomainErrorCode;
    error["message"] = std::string(DomainErrorStableName(name));
    nlohmann::json data = nlohmann::json::object();
    data["detail"] = std::string(detail);
    error["data"] = std::move(data);
    out["error"] = std::move(error);
    return out;
}

// ---------------------------------------------------------------------------
// 消息解析
// ---------------------------------------------------------------------------

IncomingMessage ParseIncomingMessage(const nlohmann::json& frame_json) {
    IncomingMessage out;
    if (!frame_json.is_object()) {
        out.malformed_reason = "frame body must be a JSON object";
        return out;
    }
    if (!frame_json.contains("jsonrpc") || !frame_json.at("jsonrpc").is_string() ||
        frame_json.at("jsonrpc").get<std::string>() != "2.0") {
        out.malformed_reason = "missing/invalid jsonrpc field (must be \"2.0\")";
        return out;
    }

    const bool has_method = frame_json.contains("method");
    const bool has_id = frame_json.contains("id") && !frame_json.at("id").is_null();
    const bool has_result = frame_json.contains("result");
    const bool has_error = frame_json.contains("error");

    if (has_method) {
        if (!frame_json.at("method").is_string()) {
            out.malformed_reason = "method must be a string";
            return out;
        }
        out.method_name = frame_json.at("method").get<std::string>();
        out.method = BridgeMethodFromName(out.method_name);
        if (frame_json.contains("params")) {
            if (!frame_json.at("params").is_object()) {
                out.malformed_reason = "params must be an object";
                return out;
            }
            out.params = frame_json.at("params");
        }
        if (has_id) {
            if (!frame_json.at("id").is_number_integer()) {
                out.malformed_reason = "id must be an integer";
                return out;
            }
            out.id = frame_json.at("id").get<std::int64_t>();
            out.kind = IncomingMessageKind::Request;
        } else {
            out.kind = IncomingMessageKind::Notification;
        }
        return out;
    }

    if (has_result || has_error) {
        if (!has_id) {
            out.malformed_reason = "response must have an id";
            return out;
        }
        if (!frame_json.at("id").is_number_integer()) {
            out.malformed_reason = "id must be an integer";
            return out;
        }
        out.id = frame_json.at("id").get<std::int64_t>();
        if (has_result && has_error) {
            out.malformed_reason = "response must not have both result and error";
            return out;
        }
        if (has_result) {
            out.kind = IncomingMessageKind::ResultResponse;
            out.result = frame_json.at("result");
            return out;
        }
        const nlohmann::json& error = frame_json.at("error");
        if (!error.is_object() || !error.contains("code") || !error.at("code").is_number_integer() ||
            !error.contains("message") || !error.at("message").is_string()) {
            out.malformed_reason = "error must be an object with integer code and string message";
            return out;
        }
        out.kind = IncomingMessageKind::ErrorResponse;
        out.error_code = error.at("code").get<int>();
        out.error_message = error.at("message").get<std::string>();
        if (error.contains("data")) out.error_data = error.at("data");
        return out;
    }

    out.malformed_reason = "message is neither request/notification (method) nor response (result/error)";
    return out;
}

// ---------------------------------------------------------------------------
// 逐 method 形状表(顶层字段级)
// ---------------------------------------------------------------------------

namespace {

struct FieldSpec {
    BridgeMethod method;
    bool is_result;  // false = params 表,true = result 表
    const char* name;
    const char* type;  // s/i/u/b/o/a/any,后缀 '|' 表可空
    bool required;
};

bool FieldMatchesType(const nlohmann::json& value, std::string_view type) {
    bool nullable = false;
    if (!type.empty() && type.back() == '|') {
        nullable = true;
        type.remove_suffix(1);
    }
    if (nullable && value.is_null()) return true;
    if (type == "s") return value.is_string();
    if (type == "i") return value.is_number_integer();
    if (type == "u") return value.is_number_unsigned();
    if (type == "b") return value.is_boolean();
    if (type == "o") return value.is_object();
    if (type == "a") return value.is_array();
    if (type == "any") return true;
    return false;
}

// 表内没登记的 method 视为"这个方向没有形状可校"(如 params 表里查
// result-only 的键)——ValidateMethodParamsShape/ResultShape 各自只挑
// is_result 匹配的条目,某 method 若在该方向确无字段(如 channel.stop 的
// params 是空 object),表里也不留条目,校验函数照准放行空 object。
constexpr FieldSpec kFieldSpecs[] = {
    // ---- channel.initialize(握手,§3) ----
    {BridgeMethod::Initialize, false, "protocol_version", "s", true},
    {BridgeMethod::Initialize, false, "channel_id", "s", true},
    {BridgeMethod::Initialize, false, "account_id", "s", true},
    {BridgeMethod::Initialize, false, "state_dir", "s", true},
    {BridgeMethod::Initialize, false, "locale", "s", false},
    {BridgeMethod::Initialize, false, "host", "o", true},
    {BridgeMethod::Initialize, false, "requested_capabilities", "o", false},
    {BridgeMethod::Initialize, true, "protocol_version", "s", true},
    {BridgeMethod::Initialize, true, "adapter", "o", true},
    {BridgeMethod::Initialize, true, "capabilities", "o", true},
    {BridgeMethod::Initialize, true, "account_state_version", "u", true},
    // ---- channel.start ----
    {BridgeMethod::Start, false, "transport", "s", true},
    {BridgeMethod::Start, true, "started", "b", true},
    {BridgeMethod::Start, true, "transport", "s", true},
    // ---- channel.stop(params 无字段,result 两枚) ----
    {BridgeMethod::Stop, true, "stopped", "b", true},
    {BridgeMethod::Stop, true, "flushed", "b", true},
    // ---- channel.health(params 无字段) ----
    {BridgeMethod::Health, true, "state", "s", true},
    {BridgeMethod::Health, true, "connected", "b", true},
    {BridgeMethod::Health, true, "cursor", "s|", true},
    {BridgeMethod::Health, true, "backlog", "u", true},
    {BridgeMethod::Health, true, "last_error", "any", true},
    // ---- channel.send ----
    {BridgeMethod::Send, false, "conversation", "o", true},
    {BridgeMethod::Send, false, "parts", "a", true},
    {BridgeMethod::Send, false, "reply_to_message_id", "s", false},
    {BridgeMethod::Send, false, "client_id", "s", false},
    {BridgeMethod::Send, true, "provider_message_id", "s", true},
    {BridgeMethod::Send, true, "accepted", "b", true},
    // ---- channel.edit ----
    {BridgeMethod::Edit, false, "provider_message_id", "s", true},
    {BridgeMethod::Edit, false, "text", "s", true},
    {BridgeMethod::Edit, false, "client_id", "s", false},
    {BridgeMethod::Edit, true, "edited", "b", true},
    // ---- channel.typing(notification,无 result) ----
    {BridgeMethod::Typing, false, "conversation", "o", true},
    {BridgeMethod::Typing, false, "on", "b", true},
    // ---- channel.react ----
    {BridgeMethod::React, false, "provider_message_id", "s", true},
    {BridgeMethod::React, false, "emoji", "s", true},
    {BridgeMethod::React, true, "applied", "b", true},
    // ---- channel.login.begin ----
    {BridgeMethod::LoginBegin, false, "mode", "s", true},
    {BridgeMethod::LoginBegin, true, "begun", "b", true},
    // ---- channel.login.cancel(params 无字段) ----
    {BridgeMethod::LoginCancel, true, "cancelled", "b", true},
    // ---- channel.logout(params 无字段) ----
    {BridgeMethod::Logout, true, "logged_out", "b", true},
    // ---- channel.inbound.ack ----
    {BridgeMethod::InboundAck, false, "delivery_id", "s", true},
    {BridgeMethod::InboundAck, true, "acked", "b", true},
    // ---- channel.inbound.nack ----
    {BridgeMethod::InboundNack, false, "delivery_id", "s", true},
    {BridgeMethod::InboundNack, false, "reason", "s", true},
    {BridgeMethod::InboundNack, false, "retry", "b", true},
    {BridgeMethod::InboundNack, false, "dead_letter", "b", true},
    {BridgeMethod::InboundNack, true, "nacked", "b", true},
    // ---- channel.inbound(notification,sidecar->host) ----
    // 完整形状由 ChannelInboundEvent::FromJsonStrict 校;这里只钉 params
    // 必带 delivery_id(bridge-protocol.md §5:"delivery_id 必带")。
    {BridgeMethod::Inbound, false, "delivery_id", "s", true},
    // ---- channel.status(notification) ----
    {BridgeMethod::Status, false, "state", "s", true},
    {BridgeMethod::Status, false, "reason", "s", false},
    {BridgeMethod::Status, false, "retry_at_ms", "i", false},
    {BridgeMethod::Status, false, "generation", "u", false},
    // ---- channel.delivery.receipt(notification) ----
    {BridgeMethod::DeliveryReceipt, false, "outbound_delivery_id", "s", true},
    {BridgeMethod::DeliveryReceipt, false, "provider_message_id", "s", false},
    {BridgeMethod::DeliveryReceipt, false, "outcome", "s", true},
    {BridgeMethod::DeliveryReceipt, false, "reason", "s", false},
    {BridgeMethod::DeliveryReceipt, false, "retry_after_ms", "i", false},
    // ---- channel.login.challenge(notification) ----
    {BridgeMethod::LoginChallenge, false, "kind", "s", true},
    {BridgeMethod::LoginChallenge, false, "payload", "s", true},
    {BridgeMethod::LoginChallenge, false, "expires_at_ms", "i", true},
    // ---- channel.login.completed(notification) ----
    {BridgeMethod::LoginCompleted, false, "account_summary", "o", true},
    // ---- channel.capabilities.changed(notification) ----
    {BridgeMethod::CapabilitiesChanged, false, "capabilities", "o", true},
    // ---- channel.fatal(notification) ----
    {BridgeMethod::Fatal, false, "reason", "s", true},
    {BridgeMethod::Fatal, false, "detail", "s", false},
};

std::optional<std::string> ValidateShape(BridgeMethod method, bool is_result,
                                         const nlohmann::json& value) {
    if (!value.is_object()) {
        return std::string("must be a JSON object");
    }
    for (const auto& field : kFieldSpecs) {
        if (field.method != method || field.is_result != is_result) continue;
        if (field.required && !value.contains(field.name)) {
            return std::string("missing required field: ") + field.name;
        }
        if (value.contains(field.name) && !FieldMatchesType(value.at(field.name), field.type)) {
            return std::string("field type mismatch: ") + field.name;
        }
    }
    for (auto it = value.begin(); it != value.end(); ++it) {
        bool known = false;
        for (const auto& field : kFieldSpecs) {
            if (field.method == method && field.is_result == is_result && it.key() == field.name) {
                known = true;
                break;
            }
        }
        if (!known) {
            return std::string("unknown field: ") + it.key();
        }
    }
    return std::nullopt;
}

}  // namespace

std::optional<std::string> ValidateMethodParamsShape(BridgeMethod method, const nlohmann::json& params) {
    return ValidateShape(method, false, params);
}

std::optional<std::string> ValidateMethodResultShape(BridgeMethod method, const nlohmann::json& result) {
    if (IsNotificationMethod(method)) return std::nullopt;  // notification 没有 result
    return ValidateShape(method, true, result);
}

}  // namespace lubancode::channel
