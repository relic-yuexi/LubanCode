#include "channel/qq/qq_proto.hpp"

#include <charconv>
#include <cstdlib>
#include <cstring>
#include <sstream>
#include <string_view>

namespace lubancode::channel::qq {

namespace {

bool JsonIsNumber(const nlohmann::json& value) { return value.is_number(); }

bool IsAsciiSpace(char c) {
    return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

}  // namespace

std::optional<std::int64_t> ParseLooseInt64(const nlohmann::json& value) {
    if (value.is_number_integer()) {
        return value.get<std::int64_t>();
    }
    if (!value.is_string()) {
        // 浮点/布尔/null/缺失不收——平台没发过小数,出现即按异常明拒。
        return std::nullopt;
    }
    std::string_view text = value.get_ref<const std::string&>();
    while (!text.empty() && IsAsciiSpace(text.front())) {
        text.remove_prefix(1);
    }
    while (!text.empty() && IsAsciiSpace(text.back())) {
        text.remove_suffix(1);
    }
    if (text.empty()) {
        return std::nullopt;
    }
    std::int64_t parsed = 0;
    const char* first = text.data();
    const char* last = text.data() + text.size();
    const auto result = std::from_chars(first, last, parsed);
    if (result.ec != std::errc{} || result.ptr != last) {
        return std::nullopt;
    }
    return parsed;
}

namespace {

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
        // d 的形状随 op 而异(Hello/READY 是 object,Heartbeat 是数字,
        // Invalid Session 是 bool,null 合法)——不在此处限定形状,由各
        // op 的解析函数(ParseHelloInterval 等)自行校验。
        out.d = payload.at("d");
    }
    if (payload.contains("s")) {
        if (const auto seq = ParseLooseInt64(payload.at("s"))) {
            out.s = *seq;
        }
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
    if (!d.is_object() || !d.contains("heartbeat_interval_ms")) {
        return std::nullopt;
    }
    const std::optional<std::int64_t> ms = ParseLooseInt64(d.at("heartbeat_interval_ms"));
    if (!ms.has_value() || *ms <= 0) {
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
            // Q4 附件映射:按官方 content_type 枚举落真类型 part(不再降级
            // Unsupported)。url 存 remote_ref——下载是宿主接纳服务的账
            // (准入通过才拉原件),这里只冻结平台给的入口引用。
            ChannelPart part;
            std::string content_type;
            if (const auto mime = GetStringField(attachment, "content_type")) {
                content_type = *mime;
                part.mime_type = *mime;
            }
            if (content_type.rfind("image/", 0) == 0) {
                part.type = ChannelPartType::Image;
            } else if (content_type.rfind("video/", 0) == 0) {
                part.type = ChannelPartType::Video;
            } else if (content_type == "voice" || content_type.rfind("audio/", 0) == 0) {
                part.type = ChannelPartType::Audio;
            } else {
                // "file"(平台枚举)与其余未认出的类型:普通文件档——白名单
                // 收纳与否是宿主接纳服务的账,映射层不虚报能解析。
                part.type = ChannelPartType::File;
            }
            if (const auto filename = GetStringField(attachment, "filename")) {
                part.file_name = *filename;
            }
            if (const auto url = GetStringField(attachment, "url")) {
                part.remote_ref = *url;
            }
            // size 宽松解析:平台数值字段可能以字符串回传(真机教训)。
            if (attachment.contains("size")) {
                if (const auto size = ParseLooseInt64(attachment.at("size"))) {
                    part.size_bytes = *size;
                }
            }
            event.parts.push_back(std::move(part));
        }
        warnings << "attachments_pending_download;";
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
    if (body.contains("expires_in")) {
        if (const auto secs = ParseLooseInt64(body.at("expires_in"))) {
            out.expires_in_secs = *secs;
        }
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
    if (!request.media_file_info.empty()) {
        // Q4 富媒体:msg_type=7 + media.file_info(官方示例不带 content——
        // 文本与附件由 outbox 拆段分开发送,不混在一条消息里)。
        body["msg_type"] = 7;
        body["media"] = nlohmann::json{{"file_info", request.media_file_info}};
    } else {
        body["msg_type"] = 0;
        body["content"] = request.content;
    }
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

// ---- v2 富媒体上传纯函数(Q4) --------------------------------------------

nlohmann::json BuildUploadPrepareRequest(int file_type, std::int64_t file_size,
                                         const std::string& file_name, const std::string& md5,
                                         const std::string& sha1, const std::string& md5_10m) {
    nlohmann::json body = nlohmann::json::object();
    body["file_type"] = file_type;
    // 官方字段表:file_size 是字符串(SDK 1.0.4 发 number 属漂移,不采信)。
    body["file_size"] = std::to_string(file_size);
    body["file_name"] = file_name;
    body["md5"] = md5;
    body["sha1"] = sha1;
    body["md5_10m"] = md5_10m;
    return body;
}

std::optional<UploadPrepareResponse> ParseUploadPrepareResponse(const nlohmann::json& body,
                                                                std::string* error) {
    if (!body.is_object()) {
        if (error != nullptr) {
            *error = "upload_prepare response not an object";
        }
        return std::nullopt;
    }
    const auto upload_id = GetStringField(body, "upload_id");
    if (!upload_id.has_value() || upload_id->empty()) {
        if (error != nullptr) {
            *error = "upload_prepare response missing upload_id";
        }
        return std::nullopt;
    }
    UploadPrepareResponse out;
    out.upload_id = *upload_id;
    // block_size/parts[].block_size:官方为字符串(宽松解析——真机教训,
    // 平台数值字段可能数字/字符串两态)。
    if (body.contains("block_size")) {
        if (const auto size = ParseLooseInt64(body.at("block_size"))) {
            out.block_size = *size;
        }
    }
    if (out.block_size <= 0) {
        if (error != nullptr) {
            *error = "upload_prepare response invalid block_size";
        }
        return std::nullopt;
    }
    if (!body.contains("parts") || !body.at("parts").is_array() ||
        body.at("parts").empty()) {
        if (error != nullptr) {
            *error = "upload_prepare response missing parts";
        }
        return std::nullopt;
    }
    for (const auto& part : body.at("parts")) {
        if (!part.is_object()) {
            if (error != nullptr) {
                *error = "upload_prepare part not an object";
            }
            return std::nullopt;
        }
        UploadPreparePart parsed;
        if (part.contains("index")) {
            // 官方"从 0 开始"、SDK 漂移从 1 起:原值保留回显,偏移不依赖它。
            if (const auto index = ParseLooseInt64(part.at("index"))) {
                parsed.index = *index;
            }
        }
        const auto url = GetStringField(part, "presigned_url");
        if (!url.has_value() || url->empty()) {
            if (error != nullptr) {
                *error = "upload_prepare part missing presigned_url";
            }
            return std::nullopt;
        }
        parsed.presigned_url = *url;
        if (part.contains("block_size")) {
            if (const auto size = ParseLooseInt64(part.at("block_size"))) {
                parsed.block_size = *size;
            }
        }
        if (parsed.block_size <= 0) {
            if (error != nullptr) {
                *error = "upload_prepare part invalid block_size";
            }
            return std::nullopt;
        }
        out.parts.push_back(std::move(parsed));
    }
    // upload_config:官方在嵌套对象下(SDK 1.0.4 误读顶层,不采信)。只记
    // 账不消费——上传恒串行,防错误并发进循环(§十 10.1)。
    if (body.contains("upload_config") && body.at("upload_config").is_object()) {
        const nlohmann::json& config = body.at("upload_config");
        if (config.contains("concurrency")) {
            if (const auto value = ParseLooseInt64(config.at("concurrency"))) {
                out.concurrency = *value;
            }
        }
        if (config.contains("retry_timeout")) {
            if (const auto value = ParseLooseInt64(config.at("retry_timeout"))) {
                out.retry_timeout_secs = *value;
            }
        }
        if (config.contains("retry_delay")) {
            if (const auto value = ParseLooseInt64(config.at("retry_delay"))) {
                out.retry_delay_secs = *value;
            }
        }
    }
    return out;
}

nlohmann::json BuildUploadPartFinishRequest(const std::string& upload_id,
                                            std::int64_t part_index, std::int64_t block_size,
                                            const std::string& part_md5) {
    nlohmann::json body = nlohmann::json::object();
    body["upload_id"] = upload_id;
    body["part_index"] = part_index;
    body["block_size"] = std::to_string(block_size);  // 官方:字符串
    body["md5"] = part_md5;
    return body;
}

std::string UploadPreparePath(const std::string& openid) {
    return "/v2/users/" + openid + "/upload_prepare";
}

std::string UploadPartFinishPath(const std::string& openid) {
    return "/v2/users/" + openid + "/upload_part_finish";
}

nlohmann::json BuildFileUploadBody(int file_type, const std::string& file_name,
                                   const std::string& upload_id) {
    nlohmann::json body = nlohmann::json::object();
    body["file_type"] = file_type;
    body["file_name"] = file_name;
    body["upload_id"] = upload_id;
    // 可靠 outbox 不走平台直发捷径(§十 10.1: srv_send_msg=true 占主动
    // 消息频次,发送另走 messages 接口拿全量回执)。
    body["srv_send_msg"] = false;
    return body;
}

std::string FileUploadPath(const std::string& openid) {
    return "/v2/users/" + openid + "/files";
}

std::optional<FileUploadResponse> ParseFileUploadResponse(const nlohmann::json& body,
                                                          std::string* error) {
    if (!body.is_object()) {
        if (error != nullptr) {
            *error = "file upload response not an object";
        }
        return std::nullopt;
    }
    const auto file_info = GetStringField(body, "file_info");
    if (!file_info.has_value() || file_info->empty()) {
        if (error != nullptr) {
            *error = "file upload response missing file_info";
        }
        return std::nullopt;
    }
    FileUploadResponse out;
    out.file_info = *file_info;
    if (const auto uuid = GetStringField(body, "file_uuid")) {
        out.file_uuid = *uuid;
    }
    // ttl 官方为 integer(秒);宽松解析防字符串态。缺失 = -1(调用方按
    // 已过期处理,不虚报长期有效)。
    if (body.contains("ttl")) {
        if (const auto ttl = ParseLooseInt64(body.at("ttl"))) {
            out.ttl_secs = *ttl;
        }
    }
    return out;
}

QqApiError ClassifyQqSendFailure(int http_status, const std::string& body) {
    QqApiError out;
    out.http_status = http_status;
    // 平台错误体 {"code":...,"message":...};body 非合法 JSON 时 code 留 0。
    nlohmann::json parsed = nlohmann::json::parse(body, nullptr, /*allow_exceptions=*/false);
    if (parsed.is_object() && parsed.contains("code")) {
        if (const auto code = ParseLooseInt64(parsed.at("code"))) {
            out.platform_code = *code;
        }
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
            case 850019:  // Q4 媒体:不支持的文件格式
            case 850031:  // Q4 媒体:上传文件超过大小限制
                return QqApiErrorKind::ContentRejected;
            case 50055002:
            case 850026:  // Q4 媒体:平台转存原始文件失败(可重试)
            case 850027:  // Q4 媒体:发送数据超时(可重试)
            case 40093001:  // Q4 媒体:分片上传 BDH 通道异常(官方建议重试)
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
