#include "channel/qq/qq_proto.hpp"

#include <cstdlib>
#include <cstring>
#include <sstream>

namespace lubancode::channel::qq {

namespace {

bool JsonIsNumber(const nlohmann::json& value) { return value.is_number(); }

// nlohmann 纪律:const json 上 operator[] 查缺键是 UB——取字段一律先 contains。
std::optional<std::string> GetStringField(const nlohmann::json& object, const char* key) {
    if (!object.is_object() || !object.contains(key)) {
        return std::nullopt;
    }
    const nlohmann::json& value = object.at(key);
    if (!value.is_string()) {
        return std::nullopt;
    }
    return value.get<std::string>();
}

}  // namespace

std::optional<GatewayOp> GatewayOpFromInt(int value) {
    switch (value) {
        case 0:
            return GatewayOp::Dispatch;
        case 1:
            return GatewayOp::Heartbeat;
        case 2:
            return GatewayOp::Identify;
        case 6:
            return GatewayOp::Resume;
        case 7:
            return GatewayOp::Reconnect;
        case 9:
            return GatewayOp::InvalidSession;
        case 10:
            return GatewayOp::Hello;
        case 11:
            return GatewayOp::HeartbeatAck;
        default:
            return std::nullopt;
    }
}

std::optional<GatewayPayload> ParseGatewayPayload(const nlohmann::json& payload,
                                                  std::string* error) {
    if (!payload.is_object()) {
        if (error != nullptr) {
            *error = "gateway payload not an object";
        }
        return std::nullopt;
    }
    GatewayPayload out;
    if (!payload.contains("op") || !payload.at("op").is_number_integer()) {
        if (error != nullptr) {
            *error = "gateway payload missing integer op";
        }
        return std::nullopt;
    }
    out.op_raw = payload.at("op").get<int>();
    out.op = GatewayOpFromInt(out.op_raw);
    if (payload.contains("d")) {
        const nlohmann::json& d = payload.at("d");
        if (!d.is_object() && !d.is_null()) {
            if (error != nullptr) {
                *error = "gateway payload d must be object or null";
            }
            return std::nullopt;
        }
        if (!d.is_null()) {
            out.d = d;
        }
    }
    if (payload.contains("s") && payload.at("s").is_number_integer()) {
        out.s = payload.at("s").get<std::int64_t>();
    }
    if (const auto id = GetStringField(payload, "id")) {
        out.id = *id;
    }
    if (const auto t = GetStringField(payload, "t")) {
        out.t = *t;
    }
    return out;
}

std::optional<std::int64_t> ParseHelloInterval(const nlohmann::json& d) {
    if (!d.is_object() || !d.contains("heartbeat_interval_ms") ||
        !d.at("heartbeat_interval_ms").is_number()) {
        return std::nullopt;
    }
    const std::int64_t ms = d.at("heartbeat_interval_ms").get<std::int64_t>();
    if (ms <= 0) {
        return std::nullopt;
    }
    return ms;
}

nlohmann::json BuildIdentify(const std::string& access_token, std::uint32_t intents) {
    nlohmann::json d = nlohmann::json::object();
    d["token"] = "QQBot " + access_token;
    d["intents"] = intents;
    nlohmann::json payload = nlohmann::json::object();
    payload["op"] = static_cast<int>(GatewayOp::Identify);
    payload["d"] = std::move(d);
    return payload;
}

nlohmann::json BuildHeartbeat(std::optional<std::int64_t> last_seq) {
    nlohmann::json payload = nlohmann::json::object();
    payload["op"] = static_cast<int>(GatewayOp::Heartbeat);
    payload["d"] = last_seq.has_value() ? nlohmann::json(*last_seq) : nlohmann::json();
    return payload;
}

nlohmann::json BuildResume(const std::string& access_token, const std::string& session_id,
                           std::int64_t seq) {
    nlohmann::json d = nlohmann::json::object();
    d["token"] = "QQBot " + access_token;
    d["session_id"] = session_id;
    d["seq"] = seq;
    nlohmann::json payload = nlohmann::json::object();
    payload["op"] = static_cast<int>(GatewayOp::Resume);
    payload["d"] = std::move(d);
    return payload;
}

std::optional<GatewayReadyInfo> ParseReady(const nlohmann::json& d, std::string* error) {
    const auto session_id = GetStringField(d, "session_id");
    std::string user_id;
    if (d.is_object() && d.contains("user") && d.at("user").is_object()) {
        if (const auto uid = GetStringField(d.at("user"), "id")) {
            user_id = *uid;
        }
    }
    if (!session_id.has_value() || session_id->empty()) {
        if (error != nullptr) {
            *error = "READY missing session_id";
        }
        return std::nullopt;
    }
    return GatewayReadyInfo{*session_id, user_id};
}

std::optional<bool> ParseInvalidSessionResumable(const nlohmann::json& payload) {
    if (!payload.is_object() || !payload.contains("d") || !payload.at("d").is_boolean()) {
        return std::nullopt;
    }
    return payload.at("d").get<bool>();
}

std::optional<std::int64_t> ParseRfc3339Ms(const std::string& text) {
    // 形状:YYYY-MM-DDTHH:MM:SS(.frac)?(Z|±HH:MM)。手写窄解析,不赌
    // 各平台 chrono 流解析器的实现质量。
    if (text.size() < 20) {
        return std::nullopt;
    }
    auto digits = [&](std::size_t pos, std::size_t count) -> std::optional<long long> {
        long long value = 0;
        for (std::size_t i = 0; i < count; ++i) {
            const char c = text[pos + i];
            if (c < '0' || c > '9') {
                return std::nullopt;
            }
            value = value * 10 + (c - '0');
        }
        return value;
    };
    const auto year = digits(0, 4);
    const auto month = digits(5, 2);
    const auto day = digits(8, 2);
    const auto hour = digits(11, 2);
    const auto minute = digits(14, 2);
    const auto second = digits(17, 2);
    if (!year.has_value() || !month.has_value() || !day.has_value() || !hour.has_value() ||
        !minute.has_value() || !second.has_value()) {
        return std::nullopt;
    }
    if (text[4] != '-' || text[7] != '-' || text[10] != 'T' || text[13] != ':' ||
        text[16] != ':') {
        return std::nullopt;
    }
    if (*month < 1 || *month > 12 || *day < 1 || *day > 31 || *hour > 23 || *minute > 59 ||
        *second > 60) {
        return std::nullopt;
    }
    std::size_t pos = 19;
    double frac_seconds = 0.0;
    if (pos < text.size() && text[pos] == '.') {
        std::size_t dot_end = pos + 1;
        while (dot_end < text.size() && text[dot_end] >= '0' && text[dot_end] <= '9') {
            ++dot_end;
        }
        if (dot_end == pos + 1) {
            return std::nullopt;
        }
        frac_seconds = std::strtod(text.substr(pos, dot_end - pos).c_str(), nullptr);
        pos = dot_end;
    }
    std::int64_t offset_seconds = 0;
    if (pos >= text.size()) {
        return std::nullopt;  // 无时区后缀:拒绝(事件必带 +08:00/Z)
    }
    if (text[pos] == 'Z' || text[pos] == 'z') {
        if (pos + 1 != text.size()) {
            return std::nullopt;
        }
    } else if (text[pos] == '+' || text[pos] == '-') {
        if (text.size() != pos + 6 || text[pos + 3] != ':') {
            return std::nullopt;
        }
        const auto off_hour = digits(pos + 1, 2);
        const auto off_minute = digits(pos + 4, 2);
        if (!off_hour.has_value() || !off_minute.has_value()) {
            return std::nullopt;
        }
        offset_seconds = (*off_hour * 3600 + *off_minute * 60);
        if (text[pos] == '-') {
            offset_seconds = -offset_seconds;
        }
    } else {
        return std::nullopt;
    }

    // days from civil(Howard Hinnant 算法)——不引入时区库。
    long long y = *year;
    const long long m = *month;
    const long long dd = *day;
    y -= m <= 2;
    const long long era = (y >= 0 ? y : y - 399) / 400;
    const long long yoe = y - era * 400;
    const long long doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + dd - 1;
    const long long doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    const long long days = era * 146097LL + doe - 719468LL;
    const std::int64_t epoch_seconds =
        days * 86400LL + *hour * 3600 + *minute * 60 + *second - offset_seconds;
    const std::int64_t epoch_ms =
        epoch_seconds * 1000 + static_cast<std::int64_t>(frac_seconds * 1000.0);
    return epoch_ms;
}

std::optional<C2cEventMapping> MapC2cMessageCreate(const nlohmann::json& d,
                                                   const std::string& envelope_event_id,
                                                   const std::string& channel_id,
                                                   const std::string& account_id,
                                                   const std::string& delivery_id,
                                                   std::int64_t received_at_ms,
                                                   std::string* error) {
    if (!d.is_object()) {
        if (error != nullptr) {
            *error = "c2c event body not an object";
        }
        return std::nullopt;
    }
    const auto message_id = GetStringField(d, "id");
    if (!message_id.has_value() || message_id->empty()) {
        if (error != nullptr) {
            *error = "c2c event missing id";
        }
        return std::nullopt;
    }
    if (!d.contains("author") || !d.at("author").is_object()) {
        if (error != nullptr) {
            *error = "c2c event missing author";
        }
        return std::nullopt;
    }
    const nlohmann::json& author = d.at("author");
    const auto user_openid = GetStringField(author, "user_openid");
    if (!user_openid.has_value() || user_openid->empty()) {
        if (error != nullptr) {
            *error = "c2c author missing user_openid";
        }
        return std::nullopt;
    }
    const auto content = GetStringField(d, "content");
    if (!content.has_value()) {
        if (error != nullptr) {
            *error = "c2c event missing content";
        }
        return std::nullopt;
    }
    int message_type = 0;
    if (d.contains("message_type") && d.at("message_type").is_number_integer()) {
        message_type = d.at("message_type").get<int>();
    }
    switch (message_type) {
        case 0:  // 纯文本
        case 3:  // ARK 卡片(content 是平台摘要文本)
        case 103:  // 引用消息(Q4 展开 msg_elements;正文仍落 content)
            break;
        default:
            if (error != nullptr) {
                *error = "c2c unsupported message_type " + std::to_string(message_type);
            }
            return std::nullopt;
    }

    C2cEventMapping mapping;
    ChannelInboundEvent& event = mapping.event;
    event.schema = kInboundEventSchemaVersion;
    event.delivery_id = delivery_id;
    // 去重键:官方"相同 msg_id 可能重复推送,结合 msg_seq 去重"——
    // msg_idx 在 message_scene.ext("msg_idx=..."),有则拼进 provider_event_id。
    std::string msg_idx;
    if (d.contains("message_scene") && d.at("message_scene").is_object() &&
        d.at("message_scene").contains("ext") && d.at("message_scene").at("ext").is_array()) {
        for (const auto& item : d.at("message_scene").at("ext")) {
            if (!item.is_string()) {
                continue;
            }
            const std::string& entry = item.get_ref<const std::string&>();
            if (entry.rfind("msg_idx=", 0) == 0) {
                msg_idx = entry.substr(std::strlen("msg_idx="));
                break;
            }
        }
    }
    event.provider_event_id =
        msg_idx.empty() ? *message_id : (*message_id + "|" + msg_idx);
    event.channel_id = channel_id;
    event.account_id = account_id;
    event.received_at_ms = received_at_ms;
    event.provider_at_ms = 0;
    if (const auto timestamp = GetStringField(d, "timestamp")) {
        if (const auto parsed = ParseRfc3339Ms(*timestamp)) {
            event.provider_at_ms = *parsed;
        }
    }
    event.conversation.kind = ConversationKind::Direct;
    event.conversation.id = *user_openid;
    event.sender.id = *user_openid;
    if (const auto username = GetStringField(author, "username")) {
        event.sender.display_name = *username;
    }
    if (author.contains("bot") && author.at("bot").is_boolean()) {
        event.sender.is_bot = author.at("bot").get<bool>();
    }
    event.sender.is_owner = false;  // 宿主 allowlist 推导,sidecar 不自称
    event.message_id = *message_id;
    event.hints.mentions_bot = true;  // 单聊消息天然发给本 bot

    ChannelPart text_part;
    text_part.type = ChannelPartType::Text;
    text_part.text = *content;
    event.parts.push_back(std::move(text_part));

    std::ostringstream warnings;
    if (message_type == 3) {
        warnings << "ark_card_not_expanded;";  // 卡片按摘要文本入账,结构不进模型
    }
    if (message_type == 103) {
        event.hints.is_reply = true;
        warnings << "quoted_elements_deferred_to_q4;";
    }
    if (d.contains("attachments") && d.at("attachments").is_array() &&
        !d.at("attachments").empty()) {
        for (const auto& attachment : d.at("attachments")) {
            if (!attachment.is_object()) {
                continue;
            }
            ChannelPart part;
            part.type = ChannelPartType::Unsupported;
            part.unsupported_reason = "qq attachment download lands in Q4; url kept as remote_ref";
            if (const auto filename = GetStringField(attachment, "filename")) {
                part.file_name = *filename;
            }
            if (const auto url = GetStringField(attachment, "url")) {
                part.remote_ref = *url;
            }
            if (attachment.contains("size") && attachment.at("size").is_number_integer()) {
                part.size_bytes = attachment.at("size").get<std::int64_t>();
            }
            if (const auto mime = GetStringField(attachment, "content_type")) {
                part.mime_type = *mime;
            }
            event.parts.push_back(std::move(part));
        }
        warnings << "attachments_downgraded;";
    }
    mapping.warnings = warnings.str();
    (void)envelope_event_id;  // 事件 id 已含在 payload.id;映射不重复存两份
    return mapping;
}

nlohmann::json BuildAccessTokenRequest(const std::string& app_id,
                                       const std::string& client_secret) {
    nlohmann::json body = nlohmann::json::object();
    body["appId"] = app_id;
    body["clientSecret"] = client_secret;
    return body;
}

std::optional<AccessTokenResponse> ParseAccessTokenResponse(const nlohmann::json& body,
                                                            std::string* error) {
    if (!body.is_object()) {
        if (error != nullptr) {
            *error = "token response not an object";
        }
        return std::nullopt;
    }
    const auto token = GetStringField(body, "access_token");
    if (!token.has_value() || token->empty()) {
        if (error != nullptr) {
            *error = "token response missing access_token";
        }
        return std::nullopt;
    }
    AccessTokenResponse out;
    out.access_token = *token;
    if (body.contains("expires_in") && body.at("expires_in").is_number()) {
        out.expires_in_secs = body.at("expires_in").get<std::int64_t>();
    }
    if (out.expires_in_secs <= 0) {
        if (error != nullptr) {
            *error = "token response invalid expires_in";
        }
        return std::nullopt;
    }
    return out;
}

nlohmann::json BuildC2cSendPayload(const C2cSendRequest& request) {
    nlohmann::json body = nlohmann::json::object();
    body["msg_type"] = 0;
    body["content"] = request.content;
    if (!request.msg_id.empty()) {
        body["msg_id"] = request.msg_id;
        body["msg_seq"] = request.msg_seq;
    }
    return body;
}

std::string C2cSendPath(const std::string& openid) {
    return "/v2/users/" + openid + "/messages";
}

std::optional<C2cSendResponse> ParseC2cSendResponse(const nlohmann::json& body,
                                                    std::string* error) {
    if (!body.is_object()) {
        if (error != nullptr) {
            *error = "send response not an object";
        }
        return std::nullopt;
    }
    const auto id = GetStringField(body, "id");
    if (!id.has_value() || id->empty()) {
        if (error != nullptr) {
            *error = "send response missing id";
        }
        return std::nullopt;
    }
    C2cSendResponse out;
    out.provider_message_id = *id;
    if (const auto timestamp = GetStringField(body, "timestamp")) {
        out.timestamp = *timestamp;
    }
    if (body.contains("ext_info") && body.at("ext_info").is_object()) {
        if (const auto ref = GetStringField(body.at("ext_info"), "ref_idx")) {
            out.ref_idx = *ref;
        }
    }
    return out;
}

QqApiError ClassifyQqSendFailure(int http_status, const std::string& body) {
    QqApiError out;
    out.http_status = http_status;
    // 平台错误体 {"code":...,"message":...};body 非合法 JSON 时 code 留 0。
    nlohmann::json parsed = nlohmann::json::parse(body, nullptr, /*allow_exceptions=*/false);
    if (parsed.is_object() && parsed.contains("code") && parsed.at("code").is_number_integer()) {
        out.platform_code = parsed.at("code").get<std::int64_t>();
    }
    if (parsed.is_object() && parsed.contains("message") && parsed.at("message").is_string()) {
        out.detail = parsed.at("message").get<std::string>();
    }

    const auto code = out.platform_code;
    auto kind_for_code = [&]() -> std::optional<QqApiErrorKind> {
        switch (code) {
            case 40034100:
                return QqApiErrorKind::RateLimited;
            case 304103:
            case 40034005:
            case 40034128:
            case 40034026:
                return QqApiErrorKind::MsgIdExpired;
            case 40054005:
                return QqApiErrorKind::Deduped;
            case 40054004:
            case 40054006:
                return QqApiErrorKind::NoFriend;
            case 40054013:
                return QqApiErrorKind::UserRejected;
            case 40034006:
            case 304061:
            case 40054007:
            case 40054018:
            case 22006:
            case 304080:
                return QqApiErrorKind::ContentRejected;
            case 50055002:
                return QqApiErrorKind::ServerError;
            default:
                return std::nullopt;
        }
    };
    if (const auto by_code = kind_for_code()) {
        out.kind = *by_code;
        return out;
    }
    if (http_status == 429) {
        out.kind = QqApiErrorKind::RateLimited;
        return out;
    }
    if (http_status == 401 || http_status == 403) {
        out.kind = QqApiErrorKind::Unauthorized;
        return out;
    }
    if (http_status >= 500) {
        out.kind = QqApiErrorKind::ServerError;
        return out;
    }
    if (http_status >= 400 && http_status < 500) {
        out.kind = QqApiErrorKind::UnknownError;
        return out;
    }
    out.kind = QqApiErrorKind::InvalidResponse;
    return out;
}

}  // namespace lubancode::channel::qq
