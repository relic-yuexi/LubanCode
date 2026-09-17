#include "channel/feishu/feishu_proto.hpp"

#include <utility>

namespace lubancode::channel::feishu {

namespace {

// 宽松整数:飞书事件/配置体里数值字段偶有数字字符串形态(同 QQ 的真机
// 教训 ParseLooseInt64 口径)。浮点/带杂质字符串/其它类型 nullopt。
std::optional<std::int64_t> LooseInt(const nlohmann::json& value) {
    if (value.is_number_integer()) {
        return value.get<std::int64_t>();
    }
    if (value.is_string()) {
        const std::string text = value.get<std::string>();
        if (text.empty()) {
            return std::nullopt;
        }
        std::size_t start = 0;
        while (start < text.size() &&
               (text[start] == ' ' || text[start] == '\t')) {
            ++start;
        }
        std::size_t end = text.size();
        while (end > start && (text[end - 1] == ' ' || text[end - 1] == '\t')) {
            --end;
        }
        if (start >= end) {
            return std::nullopt;
        }
        std::int64_t result = 0;
        for (std::size_t i = start; i < end; ++i) {
            const char c = text[i];
            if (c < '0' || c > '9') {
                return std::nullopt;
            }
            result = result * 10 + (c - '0');
        }
        return result;
    }
    return std::nullopt;
}

// 平台业务码(body 是 JSON object 时读 code 字段;缺/非法 = nullopt)。
std::optional<std::int64_t> PlatformCodeOf(const nlohmann::json& body) {
    if (body.is_object() && body.contains("code")) {
        return LooseInt(body.at("code"));
    }
    return std::nullopt;
}

}  // namespace

// ---------------------------------------------------------------------------
// 引导
// ---------------------------------------------------------------------------

nlohmann::json BuildBootstrapRequest(const std::string& app_id,
                                     const std::string& app_secret) {
    // 官方字段名大写开头(设计单 §5.1 的原文形状);ClientAssertion 首版
    // 恒空串(长连接不需要 client assertion)。
    return nlohmann::json{
        {"AppID", app_id}, {"AppSecret", app_secret}, {"ClientAssertion", ""}};
}

std::optional<FeishuBootstrapEndpoint> ParseBootstrapResponse(const nlohmann::json& body,
                                                              std::string* error) {
    if (error != nullptr) error->clear();
    if (!body.is_object()) {
        if (error != nullptr) *error = "bootstrap response not an object";
        return std::nullopt;
    }
    const auto code = PlatformCodeOf(body);
    if (!code.has_value()) {
        if (error != nullptr) *error = "bootstrap response missing code";
        return std::nullopt;
    }
    if (*code != 0) {
        // 设计单 §5.1:1=SystemBusy、1000040343=InternalError(服务端错,
        // 调用方可重试);其余码人话里只报数值,不猜义。
        if (error != nullptr) {
            *error = *code == 1 || *code == 1000040343
                         ? "bootstrap server error code " + std::to_string(*code)
                         : "bootstrap rejected code " + std::to_string(*code);
        }
        return std::nullopt;
    }
    if (!body.contains("data") || !body.at("data").is_object()) {
        if (error != nullptr) *error = "bootstrap response missing data";
        return std::nullopt;
    }
    const nlohmann::json& data = body.at("data");
    if (!data.contains("URL") || !data.at("URL").is_string() ||
        data.at("URL").get<std::string>().empty()) {
        if (error != nullptr) *error = "bootstrap response missing data.URL";
        return std::nullopt;
    }
    FeishuBootstrapEndpoint endpoint;
    endpoint.url = data.at("URL").get<std::string>();
    // ClientConfig 可缺(缺省 PingInterval=120s,设计单 §5.1/§5.4)。
    if (data.contains("ClientConfig") && data.at("ClientConfig").is_object()) {
        const nlohmann::json& config = data.at("ClientConfig");
        if (config.contains("PingInterval")) {
            const auto interval = LooseInt(config.at("PingInterval"));
            if (!interval.has_value() || *interval <= 0) {
                if (error != nullptr) *error = "bootstrap ClientConfig.PingInterval invalid";
                return std::nullopt;
            }
            endpoint.ping_interval_secs = *interval;
        }
    }
    return endpoint;
}

FeishuBootstrapErrorKind ClassifyBootstrapFailure(int http_status,
                                                  const nlohmann::json& body) {
    const auto code = PlatformCodeOf(body);
    if (code.has_value() && (*code == 1 || *code == 1000040343)) {
        return FeishuBootstrapErrorKind::ServerError;
    }
    if (http_status == 429) {
        return FeishuBootstrapErrorKind::RateLimited;
    }
    if (http_status >= 500) {
        return FeishuBootstrapErrorKind::ServerError;
    }
    if (http_status >= 200 && http_status < 300 && code.has_value() && *code != 0) {
        // 2xx + 未知业务码:配置侧被平台拒(AppID/Secret 不对一类),重试
        // 无解——非重试。
        return FeishuBootstrapErrorKind::InvalidCredentials;
    }
    if (http_status == 401 || http_status == 403) {
        return FeishuBootstrapErrorKind::InvalidCredentials;
    }
    if (http_status >= 200 && http_status < 300) {
        return FeishuBootstrapErrorKind::BadResponse;  // 2xx 但形状解不开
    }
    return FeishuBootstrapErrorKind::InvalidCredentials;
}

// ---------------------------------------------------------------------------
// tenant_access_token
// ---------------------------------------------------------------------------

nlohmann::json BuildTenantTokenRequest(const std::string& app_id,
                                       const std::string& app_secret) {
    return nlohmann::json{{"app_id", app_id}, {"app_secret", app_secret}};
}

std::optional<FeishuTenantToken> ParseTenantTokenResponse(const nlohmann::json& body,
                                                          std::string* error) {
    if (error != nullptr) error->clear();
    if (!body.is_object()) {
        if (error != nullptr) *error = "token response not an object";
        return std::nullopt;
    }
    const auto code = PlatformCodeOf(body);
    if (!code.has_value()) {
        if (error != nullptr) *error = "token response missing code";
        return std::nullopt;
    }
    if (*code != 0) {
        if (error != nullptr) *error = "token rejected code " + std::to_string(*code);
        return std::nullopt;
    }
    if (!body.contains("tenant_access_token") ||
        !body.at("tenant_access_token").is_string() ||
        body.at("tenant_access_token").get<std::string>().empty()) {
        if (error != nullptr) *error = "token response missing tenant_access_token";
        return std::nullopt;
    }
    // 官方字段 expire(秒);SDK 漂移形态 expires_in 兜底,两字段都在且
    // 不等时报冲突不猜。
    std::optional<std::int64_t> expire;
    if (body.contains("expire") && body.at("expire").is_number()) {
        expire = LooseInt(body.at("expire"));
    }
    if (body.contains("expires_in")) {
        const auto fallback = LooseInt(body.at("expires_in"));
        if (expire.has_value() && fallback.has_value() && *expire != *fallback) {
            if (error != nullptr) *error = "token expire/expires_in conflict";
            return std::nullopt;
        }
        if (!expire.has_value()) {
            expire = fallback;
        }
    }
    if (!expire.has_value() || *expire <= 0) {
        if (error != nullptr) *error = "token response missing expire";
        return std::nullopt;
    }
    FeishuTenantToken token;
    token.tenant_access_token = body.at("tenant_access_token").get<std::string>();
    token.expire_secs = *expire;
    return token;
}

// ---------------------------------------------------------------------------
// im.message.receive_v1 → ChannelInboundEvent
// ---------------------------------------------------------------------------

FeishuEventMapping MapFeishuEventPayload(const nlohmann::json& payload,
                                         const std::string& channel_id,
                                         const std::string& account_id,
                                         const std::string& expected_app_id,
                                         const std::string& delivery_id,
                                         std::int64_t received_at_ms) {
    FeishuEventMapping mapping;
    const auto invalid = [&](const std::string& detail) {
        mapping.kind = FeishuEventMapping::Kind::Invalid;
        mapping.detail = detail;
        return mapping;
    };
    if (!payload.is_object()) {
        return invalid("event payload not an object");
    }
    if (!payload.contains("schema") || !payload.at("schema").is_string() ||
        payload.at("schema").get<std::string>() != "2.0") {
        return invalid("event payload schema is not 2.0");
    }
    if (!payload.contains("header") || !payload.at("header").is_object()) {
        return invalid("event payload missing header");
    }
    const nlohmann::json& header = payload.at("header");
    if (!header.contains("event_id") || !header.at("event_id").is_string() ||
        header.at("event_id").get<std::string>().empty()) {
        return invalid("event header missing event_id");
    }
    if (!header.contains("event_type") || !header.at("event_type").is_string()) {
        return invalid("event header missing event_type");
    }
    // 账号对账:事件所属 app 与连接账号对不上 = 无效丢弃(QQ 互动事件的
    // 同款纪律),不进宿主裁决。
    if (!expected_app_id.empty() && header.contains("app_id") &&
        header.at("app_id").is_string() &&
        header.at("app_id").get<std::string>() != expected_app_id) {
        return invalid("event app_id mismatch");
    }
    const std::string event_type = header.at("event_type").get<std::string>();
    if (event_type != "im.message.receive_v1") {
        mapping.kind = FeishuEventMapping::Kind::UnsupportedEventType;
        mapping.detail = event_type;
        return mapping;
    }
    if (!payload.contains("event") || !payload.at("event").is_object()) {
        return invalid("im.message.receive_v1 missing event body");
    }
    const nlohmann::json& event = payload.at("event");
    if (!event.contains("message") || !event.at("message").is_object()) {
        return invalid("im.message.receive_v1 missing message");
    }
    const nlohmann::json& message = event.at("message");
    if (!message.contains("message_id") || !message.at("message_id").is_string() ||
        message.at("message_id").get<std::string>().empty()) {
        return invalid("message missing message_id");
    }
    if (!message.contains("chat_id") || !message.at("chat_id").is_string() ||
        message.at("chat_id").get<std::string>().empty()) {
        return invalid("message missing chat_id");
    }
    if (!message.contains("chat_type") || !message.at("chat_type").is_string()) {
        return invalid("message missing chat_type");
    }
    const std::string chat_type = message.at("chat_type").get<std::string>();
    const bool is_p2p = chat_type == "p2p";
    if (!is_p2p && chat_type != "group") {
        return invalid("message chat_type unknown: " + chat_type);
    }
    if (!message.contains("message_type") || !message.at("message_type").is_string()) {
        return invalid("message missing message_type");
    }
    const std::string message_type = message.at("message_type").get<std::string>();
    if (!event.contains("sender") || !event.at("sender").is_object()) {
        return invalid("im.message.receive_v1 missing sender");
    }
    const nlohmann::json& sender = event.at("sender");
    if (!sender.contains("sender_id") || !sender.at("sender_id").is_object() ||
        !sender.at("sender_id").contains("open_id") ||
        !sender.at("sender_id").at("open_id").is_string() ||
        sender.at("sender_id").at("open_id").get<std::string>().empty()) {
        return invalid("sender missing open_id");
    }

    mapping.kind = FeishuEventMapping::Kind::Mapped;
    mapping.event.schema = kInboundEventSchemaVersion;
    mapping.event.delivery_id = delivery_id;
    mapping.event.provider_event_id = header.at("event_id").get<std::string>();
    mapping.event.channel_id = channel_id;
    mapping.event.account_id = account_id;
    mapping.event.received_at_ms = received_at_ms;
    // header.create_time:飞书事件 v2 是毫秒时间戳字符串;解不出留 0
    //(宿主按 received_at 记账,不拿 0 冒充平台时刻)。
    if (header.contains("create_time")) {
        if (const auto at_ms = LooseInt(header.at("create_time")); at_ms.has_value()) {
            mapping.event.provider_at_ms = *at_ms;
        }
    }
    mapping.event.conversation.kind = is_p2p ? ConversationKind::Direct
                                             : ConversationKind::Group;
    mapping.event.conversation.id = message.at("chat_id").get<std::string>();
    mapping.event.sender.id =
        sender.at("sender_id").at("open_id").get<std::string>();
    mapping.event.sender.is_bot =
        sender.contains("sender_type") && sender.at("sender_type").is_string() &&
        sender.at("sender_type").get<std::string>() == "app";
    mapping.event.sender.is_owner = false;  // 宿主 allowlist 推导,适配器不得自称
    mapping.event.message_id = message.at("message_id").get<std::string>();
    mapping.event.hints.mentions_bot = is_p2p;  // 群 @ 解析后置(§十)

    ChannelPart part;
    if (message_type == "text") {
        // content 是嵌 JSON 字符串:text 时 {"text":"…"}。解不开/缺 text
        // 字段 = 事件体坏(Invalid),不拿原文冒充正文。
        if (!message.contains("content") || !message.at("content").is_string()) {
            return invalid("text message missing content");
        }
        const auto content = nlohmann::json::parse(message.at("content").get<std::string>(),
                                                   nullptr, /*allow_exceptions=*/false);
        if (content.is_discarded() || !content.is_object() || !content.contains("text") ||
            !content.at("text").is_string()) {
            return invalid("text message content is not {\"text\":…}");
        }
        part.type = ChannelPartType::Text;
        part.text = content.at("text").get<std::string>();
    } else {
        // 范围纪律(首版不做媒体/富文本/post/语音):非文本映成标注
        // unsupported 的占位 part——给模型一行稳定说明,不悄悄丢掉。
        part.type = ChannelPartType::Unsupported;
        part.unsupported_reason =
            "飞书消息类型 " + message_type + " 首版不支持(仅收发文本)";
    }
    mapping.event.parts.push_back(std::move(part));
    return mapping;
}

// ---------------------------------------------------------------------------
// 回话
// ---------------------------------------------------------------------------

nlohmann::json BuildReplyPayload(const std::string& text) {
    // content 是"嵌 JSON 字符串"(官方合同):先 dump 内层 {"text":…} 再
    // 作为字符串放进 content——不是嵌套对象。
    nlohmann::json payload;
    payload["msg_type"] = "text";
    payload["content"] = nlohmann::json{{"text", text}}.dump();
    return payload;
}

std::string ReplyPath(const std::string& message_id) {
    return "/open-apis/im/v1/messages/" + message_id + "/reply";
}

std::optional<FeishuReplyResponse> ParseReplyResponse(const nlohmann::json& body,
                                                      std::string* error) {
    if (error != nullptr) error->clear();
    if (!body.is_object()) {
        if (error != nullptr) *error = "reply response not an object";
        return std::nullopt;
    }
    const auto code = PlatformCodeOf(body);
    if (!code.has_value()) {
        if (error != nullptr) *error = "reply response missing code";
        return std::nullopt;
    }
    if (*code != 0) {
        if (error != nullptr) *error = "reply rejected code " + std::to_string(*code);
        return std::nullopt;
    }
    if (!body.contains("data") || !body.at("data").is_object() ||
        !body.at("data").contains("message_id") ||
        !body.at("data").at("message_id").is_string() ||
        body.at("data").at("message_id").get<std::string>().empty()) {
        if (error != nullptr) *error = "reply response missing data.message_id";
        return std::nullopt;
    }
    FeishuReplyResponse response;
    response.provider_message_id = body.at("data").at("message_id").get<std::string>();
    return response;
}

FeishuApiError ClassifyFeishuApiFailure(int http_status, const std::string& body) {
    const auto parsed =
        nlohmann::json::parse(body, nullptr, /*allow_exceptions=*/false);
    const bool is_json = !parsed.is_discarded() && parsed.is_object();
    const auto code = is_json ? PlatformCodeOf(parsed) : std::nullopt;

    FeishuApiError error;
    error.http_status = http_status;
    error.platform_code = code.value_or(0);
    // 受控诊断:状态 + 业务码数值 + body 是否 JSON。平台 message 不透传
    //(可能回显请求内容);token/secret/正文一概不进文案。
    error.detail = "HTTP " + std::to_string(http_status) +
                   (is_json ? " JSON" : " non-JSON") +
                   (code.has_value() ? " code=" + std::to_string(*code) : std::string());

    if (http_status == 429) {
        error.kind = FeishuApiErrorKind::RateLimited;
        return error;
    }
    if (http_status == 401) {
        error.kind = FeishuApiErrorKind::Unauthorized;
        return error;
    }
    if (http_status == 403) {
        error.kind = FeishuApiErrorKind::PermissionDenied;
        return error;
    }
    if (http_status >= 500) {
        error.kind = FeishuApiErrorKind::ServerError;
        return error;
    }
    if (http_status >= 200 && http_status < 300) {
        // 2xx 不等于成功:业务码非 0/码字段非法/形状解不开都按永久拒绝
        // 收账(平台状态未知或明确拒绝,不冒充送达)。
        error.kind = FeishuApiErrorKind::UnknownError;
        return error;
    }
    // 其余 4xx:未逐枚核实的业务码不猜义,按永久拒绝记码。
    error.kind = FeishuApiErrorKind::UnknownError;
    return error;
}

}  // namespace lubancode::channel::feishu
