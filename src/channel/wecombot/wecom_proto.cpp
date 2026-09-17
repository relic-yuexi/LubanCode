// wecom_proto.hpp 的实现(纯函数,零 IO)。
#include "channel/wecombot/wecom_proto.hpp"

#include <algorithm>
#include <cstddef>

namespace lubancode::channel::wecombot {

namespace {

// 占位 part:W1 只做文本进出,image/file/video 落一行稳定说明给模型
//(§六 6.3;媒体下载归 W2),不虚报能解析。
ChannelPart PlaceholderPart(const std::string& msgtype) {
    ChannelPart part;
    part.type = ChannelPartType::Unsupported;
    part.unsupported_reason =
        "[wecombot] 收到 " + msgtype + " 消息,媒体接收首版未支持(归 W2)";
    return part;
}

// 码点字节长(UTF-8 首字节定长;坏序列按 1 字节吞——分段只求不切半个
// 合法字,坏输入原样透传给平台/映射层)。
std::size_t Utf8CodepointBytes(unsigned char first) {
    if (first < 0x80) {
        return 1;
    }
    if ((first & 0xE0) == 0xC0) {
        return 2;
    }
    if ((first & 0xF0) == 0xE0) {
        return 3;
    }
    if ((first & 0xF8) == 0xF0) {
        return 4;
    }
    return 1;
}

}  // namespace

std::optional<WecomInboundFrame> ParseWecomInboundFrame(const nlohmann::json& payload,
                                                        std::string* error) {
    if (error != nullptr) {
        error->clear();
    }
    if (!payload.is_object()) {
        if (error != nullptr) {
            *error = "wecom frame is not a json object";
        }
        return std::nullopt;
    }
    WecomInboundFrame frame;
    if (payload.contains("headers") && payload.at("headers").is_object() &&
        payload.at("headers").contains("req_id") &&
        payload.at("headers").at("req_id").is_string()) {
        frame.req_id = payload.at("headers").at("req_id").get<std::string>();
    }
    if (payload.contains("body") && payload.at("body").is_object()) {
        frame.body = payload.at("body");
    }
    if (payload.contains("cmd")) {
        if (!payload.at("cmd").is_string() || payload.at("cmd").get<std::string>().empty()) {
            if (error != nullptr) {
                *error = "wecom frame cmd must be a non-empty string";
            }
            return std::nullopt;
        }
        frame.cmd = payload.at("cmd").get<std::string>();
        if (frame.cmd == "aibot_msg_callback") {
            frame.kind = WecomFrameKind::MsgCallback;
        } else if (frame.cmd == "aibot_event_callback") {
            frame.kind = WecomFrameKind::EventCallback;
        } else {
            frame.kind = WecomFrameKind::Unknown;
        }
        return frame;
    }
    // 无 cmd:回执帧须带整数 errcode(官方回执口径:errcode 0 成功)。
    if (payload.contains("errcode")) {
        if (!payload.at("errcode").is_number_integer()) {
            if (error != nullptr) {
                *error = "wecom ack frame errcode must be an integer";
            }
            return std::nullopt;
        }
        frame.kind = WecomFrameKind::Ack;
        frame.errcode = payload.at("errcode").get<std::int64_t>();
        if (payload.contains("errmsg") && payload.at("errmsg").is_string()) {
            frame.errmsg = payload.at("errmsg").get<std::string>();
        }
        return frame;
    }
    if (error != nullptr) {
        *error = "wecom frame has neither cmd nor errcode";
    }
    return std::nullopt;
}

std::string BuildSubscribeFrame(const std::string& bot_id, const std::string& secret,
                                const std::string& req_id) {
    nlohmann::json frame = nlohmann::json::object();
    frame["cmd"] = "aibot_subscribe";
    frame["headers"] = nlohmann::json{{"req_id", req_id}};
    frame["body"] = nlohmann::json{{"bot_id", bot_id}, {"secret", secret}};
    return frame.dump();
}

std::string BuildPingFrame(const std::string& req_id) {
    nlohmann::json frame = nlohmann::json::object();
    frame["cmd"] = "ping";
    frame["headers"] = nlohmann::json{{"req_id", req_id}};
    return frame.dump();
}

std::string BuildRespondFrame(const std::string& req_id, const std::string& content) {
    nlohmann::json frame = nlohmann::json::object();
    frame["cmd"] = "aibot_respond_msg";
    frame["headers"] = nlohmann::json{{"req_id", req_id}};
    frame["body"] = nlohmann::json{
        {"msgtype", "markdown"},
        {"markdown", nlohmann::json{{"content", content}}},
    };
    return frame.dump();
}

std::optional<WecomMsgMapping> MapMsgCallback(const nlohmann::json& body,
                                              const std::string& channel_id,
                                              const std::string& account_id,
                                              const std::string& bot_id,
                                              const std::string& delivery_id,
                                              std::int64_t received_at_ms,
                                              std::string* error) {
    if (error != nullptr) {
        error->clear();
    }
    if (!body.is_object()) {
        if (error != nullptr) {
            *error = "msg callback body is not an object";
        }
        return std::nullopt;
    }
    // msgid:排重键/回话锚,必填。
    if (!body.contains("msgid") || !body.at("msgid").is_string() ||
        body.at("msgid").get<std::string>().empty()) {
        if (error != nullptr) {
            *error = "msg callback body missing msgid";
        }
        return std::nullopt;
    }
    // aibotid 对账(非空且不符即拒——别家 bot 的事件不进本账号的模型)。
    if (body.contains("aibotid") && body.at("aibotid").is_string() &&
        !body.at("aibotid").get<std::string>().empty() &&
        body.at("aibotid").get<std::string>() != bot_id) {
        if (error != nullptr) {
            *error = "msg callback aibotid mismatch";
        }
        return std::nullopt;
    }
    // from.userid:身份键(明文或平台加密串,原样用)。
    if (!body.contains("from") || !body.at("from").is_object() ||
        !body.at("from").contains("userid") ||
        !body.at("from").at("userid").is_string() ||
        body.at("from").at("userid").get<std::string>().empty()) {
        if (error != nullptr) {
            *error = "msg callback body missing from.userid";
        }
        return std::nullopt;
    }
    const std::string userid = body.at("from").at("userid").get<std::string>();
    // chattype → conversation。
    if (!body.contains("chattype") || !body.at("chattype").is_string()) {
        if (error != nullptr) {
            *error = "msg callback body missing chattype";
        }
        return std::nullopt;
    }
    const std::string chattype = body.at("chattype").get<std::string>();
    std::string conversation_id;
    ConversationKind conversation_kind;
    if (chattype == "single") {
        conversation_kind = ConversationKind::Direct;
        conversation_id = userid;
    } else if (chattype == "group") {
        conversation_kind = ConversationKind::Group;
        if (!body.contains("chatid") || !body.at("chatid").is_string() ||
            body.at("chatid").get<std::string>().empty()) {
            if (error != nullptr) {
                *error = "group msg callback body missing chatid";
            }
            return std::nullopt;
        }
        conversation_id = body.at("chatid").get<std::string>();
    } else {
        if (error != nullptr) {
            *error = "msg callback chattype unsupported: " + chattype;
        }
        return std::nullopt;
    }
    // msgtype → parts。
    if (!body.contains("msgtype") || !body.at("msgtype").is_string()) {
        if (error != nullptr) {
            *error = "msg callback body missing msgtype";
        }
        return std::nullopt;
    }
    const std::string msgtype = body.at("msgtype").get<std::string>();
    WecomMsgMapping mapping;
    std::vector<ChannelPart>& parts = mapping.event.parts;
    if (msgtype == "text") {
        if (!body.contains("text") || !body.at("text").is_object() ||
            !body.at("text").contains("content") ||
            !body.at("text").at("content").is_string()) {
            if (error != nullptr) {
                *error = "text msg callback missing text.content";
            }
            return std::nullopt;
        }
        ChannelPart part;
        part.type = ChannelPartType::Text;
        part.text = body.at("text").at("content").get<std::string>();
        parts.push_back(std::move(part));
    } else if (msgtype == "voice") {
        // voice(仅单聊):voice.content 是平台转写文本——白捡(§一 5)。
        if (!body.contains("voice") || !body.at("voice").is_object() ||
            !body.at("voice").contains("content") ||
            !body.at("voice").at("content").is_string()) {
            if (error != nullptr) {
                *error = "voice msg callback missing voice.content";
            }
            return std::nullopt;
        }
        ChannelPart part;
        part.type = ChannelPartType::Text;
        part.text = body.at("voice").at("content").get<std::string>();
        parts.push_back(std::move(part));
        mapping.warnings = "voice message mapped from platform transcript";
    } else if (msgtype == "mixed") {
        if (!body.contains("msg_item") || !body.at("msg_item").is_array()) {
            if (error != nullptr) {
                *error = "mixed msg callback missing msg_item array";
            }
            return std::nullopt;
        }
        bool saw_text = false;
        for (const auto& item : body.at("msg_item")) {
            if (!item.is_object()) {
                continue;
            }
            const std::string item_type = item.value("msgtype", std::string());
            if (item_type == "text" && item.contains("text") && item.at("text").is_object() &&
                item.at("text").contains("content") &&
                item.at("text").at("content").is_string()) {
                ChannelPart part;
                part.type = ChannelPartType::Text;
                part.text = item.at("text").at("content").get<std::string>();
                parts.push_back(std::move(part));
                saw_text = true;
            } else {
                parts.push_back(PlaceholderPart(item_type.empty() ? "unknown" : item_type));
            }
        }
        if (!saw_text) {
            mapping.warnings = "mixed message had no text item";
        }
    } else if (msgtype == "image" || msgtype == "file" || msgtype == "video") {
        parts.push_back(PlaceholderPart(msgtype));
    } else {
        if (error != nullptr) {
            *error = "msg callback msgtype unsupported: " + msgtype;
        }
        return std::nullopt;
    }

    mapping.event.schema = kInboundEventSchemaVersion;
    mapping.event.delivery_id = delivery_id;
    mapping.event.provider_event_id = body.at("msgid").get<std::string>();
    mapping.event.message_id = body.at("msgid").get<std::string>();
    mapping.event.channel_id = channel_id;
    mapping.event.account_id = account_id;
    mapping.event.received_at_ms = received_at_ms;
    mapping.event.provider_at_ms = 0;  // 官方回调体未带可解析时间戳,不猜
    mapping.event.conversation.kind = conversation_kind;
    mapping.event.conversation.id = conversation_id;
    mapping.event.sender.id = userid;
    mapping.event.sender.is_bot = false;
    mapping.event.hints.mentions_bot = true;
    if (body.contains("quote") && body.at("quote").is_object()) {
        mapping.event.hints.is_reply = true;  // 引用消息:只立标志不展开
    }
    return mapping;
}

std::optional<WecomEventCallbackInfo> ParseEventCallback(const nlohmann::json& body,
                                                         std::string* error) {
    if (error != nullptr) {
        error->clear();
    }
    if (!body.is_object()) {
        if (error != nullptr) {
            *error = "event callback body is not an object";
        }
        return std::nullopt;
    }
    if (!body.contains("event") || !body.at("event").is_object() ||
        !body.at("event").contains("eventtype") ||
        !body.at("event").at("eventtype").is_string() ||
        body.at("event").at("eventtype").get<std::string>().empty()) {
        if (error != nullptr) {
            *error = "event callback body missing event.eventtype";
        }
        return std::nullopt;
    }
    WecomEventCallbackInfo info;
    info.eventtype = body.at("event").at("eventtype").get<std::string>();
    if (body.contains("msgid") && body.at("msgid").is_string()) {
        info.msgid = body.at("msgid").get<std::string>();
    }
    return info;
}

WecomSubscribeStatus ClassifySubscribeErrcode(std::int64_t errcode) {
    if (errcode == 0) {
        return WecomSubscribeStatus::Ok;
    }
    // 不合法凭据族(企微通用错误码:40013 不合法 ID/40014 不合法 secret/
    // 40029 无效 oauth/41001 缺 secret/41004 缺授权):换密钥前重试无意义。
    switch (errcode) {
        case 40013:
        case 40014:
        case 40029:
        case 41001:
        case 41004:
            return WecomSubscribeStatus::CredentialRejected;
        default:
            return WecomSubscribeStatus::Transient;
    }
}

WecomRespondStatus ClassifyRespondErrcode(std::int64_t errcode) {
    if (errcode == 0) {
        return WecomRespondStatus::Ok;
    }
    // 限流族(api 配额超限的通用码):延后重试;同 req_id 重发官方容忍
    //(分段回复本就共用 req_id)。其余码——窗口过期/内容拒绝/权限——按
    // 永久拒绝报,稳定码 + errcode 数值如实透传,不猜语义。
    switch (errcode) {
        case 45009:
        case 45011:
            return WecomRespondStatus::RateLimited;
        default:
            return WecomRespondStatus::Rejected;
    }
}

std::vector<std::string> SplitUtf8Chunks(const std::string& text, std::size_t max_bytes) {
    std::vector<std::string> chunks;
    if (max_bytes == 0) {
        return chunks;
    }
    std::string current;
    std::size_t i = 0;
    while (i < text.size()) {
        const std::size_t codepoint =
            Utf8CodepointBytes(static_cast<unsigned char>(text[i]));
        const std::size_t take = std::min(codepoint, text.size() - i);
        if (current.size() + take > max_bytes && !current.empty()) {
            chunks.push_back(current);
            current.clear();
        }
        current.append(text, i, take);
        i += take;
    }
    if (!current.empty() || chunks.empty()) {
        chunks.push_back(std::move(current));
    }
    return chunks;
}

}  // namespace lubancode::channel::wecombot
