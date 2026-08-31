#include "channel/types.hpp"

namespace lubancode::channel {

namespace {

// 严格解析的公共小工具:取字符串/整数/布尔/浮点必填字段;缺失或类型错
// 落 *error 并返回 false。这里不架 trajectory/schema.cpp 那套表驱动
// (字段集小,手写更看得清),但"未知键拒绝"的规矩一样照办。

bool RequireObject(const nlohmann::json& json, std::string* error) {
    if (!json.is_object()) {
        if (error != nullptr) *error = "must be a JSON object";
        return false;
    }
    return true;
}

bool GetRequiredString(const nlohmann::json& json, const char* key, std::string* out,
                       std::string* error) {
    if (!json.contains(key) || !json.at(key).is_string()) {
        if (error != nullptr) *error = std::string("missing/invalid required string field: ") + key;
        return false;
    }
    *out = json.at(key).get<std::string>();
    return true;
}

void PutOptionalString(nlohmann::json& json, const char* key, const std::optional<std::string>& value) {
    if (value.has_value()) json[key] = *value;
}

bool GetOptionalString(const nlohmann::json& json, const char* key, std::optional<std::string>* out,
                       std::string* error) {
    if (!json.contains(key)) return true;
    if (!json.at(key).is_string()) {
        if (error != nullptr) *error = std::string("invalid type for optional string field: ") + key;
        return false;
    }
    *out = json.at(key).get<std::string>();
    return true;
}

bool GetOptionalInt64(const nlohmann::json& json, const char* key, std::optional<std::int64_t>* out,
                      std::string* error) {
    if (!json.contains(key)) return true;
    if (!json.at(key).is_number_integer()) {
        if (error != nullptr) *error = std::string("invalid type for optional integer field: ") + key;
        return false;
    }
    *out = json.at(key).get<std::int64_t>();
    return true;
}

bool GetOptionalDouble(const nlohmann::json& json, const char* key, std::optional<double>* out,
                       std::string* error) {
    if (!json.contains(key)) return true;
    if (!json.at(key).is_number()) {
        if (error != nullptr) *error = std::string("invalid type for optional number field: ") + key;
        return false;
    }
    *out = json.at(key).get<double>();
    return true;
}

// 未知键检查:allowed 之外的键一律拒绝。
bool RejectUnknownKeys(const nlohmann::json& json, std::initializer_list<const char*> allowed,
                       std::string* error) {
    for (auto it = json.begin(); it != json.end(); ++it) {
        bool known = false;
        for (const char* key : allowed) {
            if (it.key() == key) {
                known = true;
                break;
            }
        }
        if (!known) {
            if (error != nullptr) *error = "unknown field: " + it.key();
            return false;
        }
    }
    return true;
}

}  // namespace

// ---------------------------------------------------------------------------
// ConversationKind
// ---------------------------------------------------------------------------

const char* ConversationKindName(ConversationKind kind) {
    switch (kind) {
        case ConversationKind::Direct: return "direct";
        case ConversationKind::Group: return "group";
        case ConversationKind::Guild: return "guild";
        case ConversationKind::Channel: return "channel";
        case ConversationKind::Thread: return "thread";
    }
    return "?";
}

std::optional<ConversationKind> ConversationKindFromName(std::string_view name) {
    if (name == "direct") return ConversationKind::Direct;
    if (name == "group") return ConversationKind::Group;
    if (name == "guild") return ConversationKind::Guild;
    if (name == "channel") return ConversationKind::Channel;
    if (name == "thread") return ConversationKind::Thread;
    return std::nullopt;
}

// ---------------------------------------------------------------------------
// ChannelConversation
// ---------------------------------------------------------------------------

nlohmann::json ChannelConversation::ToJson() const {
    nlohmann::json out = nlohmann::json::object();
    out["kind"] = ConversationKindName(kind);
    out["id"] = id;
    PutOptionalString(out, "parent_id", parent_id);
    PutOptionalString(out, "thread_id", thread_id);
    if (!title.empty()) out["title"] = title;
    return out;
}

std::optional<ChannelConversation> ChannelConversation::FromJsonStrict(const nlohmann::json& json,
                                                                       std::string* error) {
    if (!RequireObject(json, error)) return std::nullopt;
    if (!RejectUnknownKeys(json, {"kind", "id", "parent_id", "thread_id", "title"}, error)) {
        return std::nullopt;
    }
    std::string kind_name;
    if (!GetRequiredString(json, "kind", &kind_name, error)) return std::nullopt;
    const auto kind = ConversationKindFromName(kind_name);
    if (!kind.has_value()) {
        if (error != nullptr) *error = "unknown conversation kind: " + kind_name;
        return std::nullopt;
    }
    ChannelConversation out;
    out.kind = *kind;
    if (!GetRequiredString(json, "id", &out.id, error)) return std::nullopt;
    if (!GetOptionalString(json, "parent_id", &out.parent_id, error)) return std::nullopt;
    if (!GetOptionalString(json, "thread_id", &out.thread_id, error)) return std::nullopt;
    std::optional<std::string> title;
    if (!GetOptionalString(json, "title", &title, error)) return std::nullopt;
    if (title.has_value()) out.title = *title;
    return out;
}

// ---------------------------------------------------------------------------
// ChannelSender
// ---------------------------------------------------------------------------

nlohmann::json ChannelSender::ToJson() const {
    nlohmann::json out = nlohmann::json::object();
    out["id"] = id;
    out["display_name"] = display_name;
    out["is_bot"] = is_bot;
    out["is_owner"] = is_owner;
    return out;
}

std::optional<ChannelSender> ChannelSender::FromJsonStrict(const nlohmann::json& json,
                                                            std::string* error) {
    if (!RequireObject(json, error)) return std::nullopt;
    if (!RejectUnknownKeys(json, {"id", "display_name", "is_bot", "is_owner"}, error)) {
        return std::nullopt;
    }
    ChannelSender out;
    if (!GetRequiredString(json, "id", &out.id, error)) return std::nullopt;
    if (json.contains("display_name")) {
        if (!json.at("display_name").is_string()) {
            if (error != nullptr) *error = "invalid type for display_name";
            return std::nullopt;
        }
        out.display_name = json.at("display_name").get<std::string>();
    }
    if (json.contains("is_bot")) {
        if (!json.at("is_bot").is_boolean()) {
            if (error != nullptr) *error = "invalid type for is_bot";
            return std::nullopt;
        }
        out.is_bot = json.at("is_bot").get<bool>();
    }
    if (json.contains("is_owner")) {
        if (!json.at("is_owner").is_boolean()) {
            if (error != nullptr) *error = "invalid type for is_owner";
            return std::nullopt;
        }
        out.is_owner = json.at("is_owner").get<bool>();
    }
    return out;
}

// ---------------------------------------------------------------------------
// ChannelPartType / ChannelPart
// ---------------------------------------------------------------------------

const char* ChannelPartTypeName(ChannelPartType type) {
    switch (type) {
        case ChannelPartType::Text: return "text";
        case ChannelPartType::Image: return "image";
        case ChannelPartType::Audio: return "audio";
        case ChannelPartType::Video: return "video";
        case ChannelPartType::File: return "file";
        case ChannelPartType::Link: return "link";
        case ChannelPartType::Mention: return "mention";
        case ChannelPartType::Location: return "location";
        case ChannelPartType::Unsupported: return "unsupported";
    }
    return "?";
}

std::optional<ChannelPartType> ChannelPartTypeFromName(std::string_view name) {
    if (name == "text") return ChannelPartType::Text;
    if (name == "image") return ChannelPartType::Image;
    if (name == "audio") return ChannelPartType::Audio;
    if (name == "video") return ChannelPartType::Video;
    if (name == "file") return ChannelPartType::File;
    if (name == "link") return ChannelPartType::Link;
    if (name == "mention") return ChannelPartType::Mention;
    if (name == "location") return ChannelPartType::Location;
    if (name == "unsupported") return ChannelPartType::Unsupported;
    return std::nullopt;
}

nlohmann::json ChannelPart::ToJson() const {
    nlohmann::json out = nlohmann::json::object();
    out["type"] = ChannelPartTypeName(type);
    PutOptionalString(out, "text", text);
    PutOptionalString(out, "media_id", media_id);
    PutOptionalString(out, "mime_type", mime_type);
    PutOptionalString(out, "file_name", file_name);
    if (size_bytes.has_value()) out["size_bytes"] = *size_bytes;
    PutOptionalString(out, "local_path", local_path);
    PutOptionalString(out, "remote_ref", remote_ref);
    PutOptionalString(out, "caption", caption);
    PutOptionalString(out, "sha256", sha256);
    PutOptionalString(out, "url", url);
    PutOptionalString(out, "mentioned_id", mentioned_id);
    if (latitude.has_value()) out["latitude"] = *latitude;
    if (longitude.has_value()) out["longitude"] = *longitude;
    PutOptionalString(out, "location_name", location_name);
    PutOptionalString(out, "unsupported_reason", unsupported_reason);
    return out;
}

std::optional<ChannelPart> ChannelPart::FromJsonStrict(const nlohmann::json& json, std::string* error) {
    if (!RequireObject(json, error)) return std::nullopt;
    if (!RejectUnknownKeys(json,
                           {"type", "text", "media_id", "mime_type", "file_name", "size_bytes",
                            "local_path", "remote_ref", "caption", "sha256", "url", "mentioned_id",
                            "latitude", "longitude", "location_name", "unsupported_reason"},
                           error)) {
        return std::nullopt;
    }
    std::string type_name;
    if (!GetRequiredString(json, "type", &type_name, error)) return std::nullopt;
    const auto type = ChannelPartTypeFromName(type_name);
    if (!type.has_value()) {
        if (error != nullptr) *error = "unknown part type: " + type_name;
        return std::nullopt;
    }
    ChannelPart out;
    out.type = *type;
    if (!GetOptionalString(json, "text", &out.text, error)) return std::nullopt;
    if (!GetOptionalString(json, "media_id", &out.media_id, error)) return std::nullopt;
    if (!GetOptionalString(json, "mime_type", &out.mime_type, error)) return std::nullopt;
    if (!GetOptionalString(json, "file_name", &out.file_name, error)) return std::nullopt;
    if (!GetOptionalInt64(json, "size_bytes", &out.size_bytes, error)) return std::nullopt;
    if (!GetOptionalString(json, "local_path", &out.local_path, error)) return std::nullopt;
    if (!GetOptionalString(json, "remote_ref", &out.remote_ref, error)) return std::nullopt;
    if (!GetOptionalString(json, "caption", &out.caption, error)) return std::nullopt;
    if (!GetOptionalString(json, "sha256", &out.sha256, error)) return std::nullopt;
    if (!GetOptionalString(json, "url", &out.url, error)) return std::nullopt;
    if (!GetOptionalString(json, "mentioned_id", &out.mentioned_id, error)) return std::nullopt;
    if (!GetOptionalDouble(json, "latitude", &out.latitude, error)) return std::nullopt;
    if (!GetOptionalDouble(json, "longitude", &out.longitude, error)) return std::nullopt;
    if (!GetOptionalString(json, "location_name", &out.location_name, error)) return std::nullopt;
    if (!GetOptionalString(json, "unsupported_reason", &out.unsupported_reason, error)) return std::nullopt;

    // 类型内部一致性(message-contracts.md §1 每类只带自家字段;这里只
    // 拦最容易错传的必填项,不做全字段矩阵——多余字段留给上层业务判断)。
    if (out.type == ChannelPartType::Text && !out.text.has_value()) {
        if (error != nullptr) *error = "text part requires text field";
        return std::nullopt;
    }
    if (out.type == ChannelPartType::Link && !out.url.has_value()) {
        if (error != nullptr) *error = "link part requires url field";
        return std::nullopt;
    }
    return out;
}

// ---------------------------------------------------------------------------
// ChannelIngressHints
// ---------------------------------------------------------------------------

nlohmann::json ChannelIngressHints::ToJson() const {
    nlohmann::json out = nlohmann::json::object();
    out["mentions_bot"] = mentions_bot;
    PutOptionalString(out, "command", command);
    PutOptionalString(out, "command_args", command_args);
    out["is_reply"] = is_reply;
    return out;
}

std::optional<ChannelIngressHints> ChannelIngressHints::FromJsonStrict(const nlohmann::json& json,
                                                                       std::string* error) {
    if (!RequireObject(json, error)) return std::nullopt;
    if (!RejectUnknownKeys(json, {"mentions_bot", "command", "command_args", "is_reply"}, error)) {
        return std::nullopt;
    }
    ChannelIngressHints out;
    if (json.contains("mentions_bot")) {
        if (!json.at("mentions_bot").is_boolean()) {
            if (error != nullptr) *error = "invalid type for mentions_bot";
            return std::nullopt;
        }
        out.mentions_bot = json.at("mentions_bot").get<bool>();
    }
    if (!GetOptionalString(json, "command", &out.command, error)) return std::nullopt;
    if (!GetOptionalString(json, "command_args", &out.command_args, error)) return std::nullopt;
    if (json.contains("is_reply")) {
        if (!json.at("is_reply").is_boolean()) {
            if (error != nullptr) *error = "invalid type for is_reply";
            return std::nullopt;
        }
        out.is_reply = json.at("is_reply").get<bool>();
    }
    return out;
}

// ---------------------------------------------------------------------------
// ChannelInboundEvent
// ---------------------------------------------------------------------------

nlohmann::json ChannelInboundEvent::ToJson() const {
    nlohmann::json out = nlohmann::json::object();
    out["schema"] = schema;
    out["delivery_id"] = delivery_id;
    out["provider_event_id"] = provider_event_id;
    out["channel_id"] = channel_id;
    out["account_id"] = account_id;
    out["received_at_ms"] = received_at_ms;
    out["provider_at_ms"] = provider_at_ms;
    out["conversation"] = conversation.ToJson();
    out["sender"] = sender.ToJson();
    out["message_id"] = message_id;
    PutOptionalString(out, "reply_to_message_id", reply_to_message_id);
    nlohmann::json parts_json = nlohmann::json::array();
    for (const auto& part : parts) parts_json.push_back(part.ToJson());
    out["parts"] = std::move(parts_json);
    out["hints"] = hints.ToJson();
    return out;
}

std::optional<ChannelInboundEvent> ChannelInboundEvent::FromJsonStrict(const nlohmann::json& json,
                                                                       std::string* error) {
    if (!RequireObject(json, error)) return std::nullopt;
    if (!RejectUnknownKeys(json,
                           {"schema", "delivery_id", "provider_event_id", "channel_id", "account_id",
                            "received_at_ms", "provider_at_ms", "conversation", "sender", "message_id",
                            "reply_to_message_id", "parts", "hints"},
                           error)) {
        return std::nullopt;
    }
    if (!json.contains("schema") || !json.at("schema").is_number_integer()) {
        if (error != nullptr) *error = "missing/invalid required integer field: schema";
        return std::nullopt;
    }
    ChannelInboundEvent out;
    out.schema = json.at("schema").get<int>();
    if (out.schema != kInboundEventSchemaVersion) {
        if (error != nullptr) {
            *error = "unsupported schema version: " + std::to_string(out.schema) + " (only " +
                    std::to_string(kInboundEventSchemaVersion) + " implemented)";
        }
        return std::nullopt;
    }
    if (!GetRequiredString(json, "delivery_id", &out.delivery_id, error)) return std::nullopt;
    if (!GetRequiredString(json, "provider_event_id", &out.provider_event_id, error)) return std::nullopt;
    if (!GetRequiredString(json, "channel_id", &out.channel_id, error)) return std::nullopt;
    if (!GetRequiredString(json, "account_id", &out.account_id, error)) return std::nullopt;
    if (json.contains("received_at_ms")) {
        if (!json.at("received_at_ms").is_number_integer()) {
            if (error != nullptr) *error = "invalid type for received_at_ms";
            return std::nullopt;
        }
        out.received_at_ms = json.at("received_at_ms").get<std::int64_t>();
    }
    if (json.contains("provider_at_ms")) {
        if (!json.at("provider_at_ms").is_number_integer()) {
            if (error != nullptr) *error = "invalid type for provider_at_ms";
            return std::nullopt;
        }
        out.provider_at_ms = json.at("provider_at_ms").get<std::int64_t>();
    }
    if (!json.contains("conversation")) {
        if (error != nullptr) *error = "missing required field: conversation";
        return std::nullopt;
    }
    std::string sub_error;
    auto conversation = ChannelConversation::FromJsonStrict(json.at("conversation"), &sub_error);
    if (!conversation.has_value()) {
        if (error != nullptr) *error = "conversation: " + sub_error;
        return std::nullopt;
    }
    out.conversation = std::move(*conversation);
    if (!json.contains("sender")) {
        if (error != nullptr) *error = "missing required field: sender";
        return std::nullopt;
    }
    auto sender = ChannelSender::FromJsonStrict(json.at("sender"), &sub_error);
    if (!sender.has_value()) {
        if (error != nullptr) *error = "sender: " + sub_error;
        return std::nullopt;
    }
    out.sender = std::move(*sender);
    if (!GetRequiredString(json, "message_id", &out.message_id, error)) return std::nullopt;
    if (!GetOptionalString(json, "reply_to_message_id", &out.reply_to_message_id, error)) {
        return std::nullopt;
    }
    if (json.contains("parts")) {
        if (!json.at("parts").is_array()) {
            if (error != nullptr) *error = "invalid type for parts (must be array)";
            return std::nullopt;
        }
        for (const auto& item : json.at("parts")) {
            auto part = ChannelPart::FromJsonStrict(item, &sub_error);
            if (!part.has_value()) {
                if (error != nullptr) *error = "parts[]: " + sub_error;
                return std::nullopt;
            }
            out.parts.push_back(std::move(*part));
        }
    }
    if (json.contains("hints")) {
        auto hints = ChannelIngressHints::FromJsonStrict(json.at("hints"), &sub_error);
        if (!hints.has_value()) {
            if (error != nullptr) *error = "hints: " + sub_error;
            return std::nullopt;
        }
        out.hints = std::move(*hints);
    }
    return out;
}

// ---------------------------------------------------------------------------
// MessageOrigin / MessageProvenance
// ---------------------------------------------------------------------------

const char* MessageOriginName(MessageOrigin origin) {
    switch (origin) {
        case MessageOrigin::HumanTerminal: return "human_terminal";
        case MessageOrigin::ExternalChannel: return "external_channel";
        case MessageOrigin::PeerSession: return "peer_session";
        case MessageOrigin::BackgroundCompletion: return "background_completion";
        case MessageOrigin::ToolResult: return "tool_result";
        case MessageOrigin::HostSynthetic: return "host_synthetic";
    }
    return "?";
}

std::optional<MessageOrigin> MessageOriginFromName(std::string_view name) {
    if (name == "human_terminal") return MessageOrigin::HumanTerminal;
    if (name == "external_channel") return MessageOrigin::ExternalChannel;
    if (name == "peer_session") return MessageOrigin::PeerSession;
    if (name == "background_completion") return MessageOrigin::BackgroundCompletion;
    if (name == "tool_result") return MessageOrigin::ToolResult;
    if (name == "host_synthetic") return MessageOrigin::HostSynthetic;
    return std::nullopt;
}

nlohmann::json MessageProvenance::ToJson() const {
    nlohmann::json out = nlohmann::json::object();
    out["origin"] = MessageOriginName(origin);
    out["channel_id"] = channel_id;
    out["account_id"] = account_id;
    out["sender_id"] = sender_id;
    out["conversation_id"] = conversation_id;
    out["provider_message_id"] = provider_message_id;
    return out;
}

std::optional<MessageProvenance> MessageProvenance::FromJsonStrict(const nlohmann::json& json,
                                                                    std::string* error) {
    if (!RequireObject(json, error)) return std::nullopt;
    if (!RejectUnknownKeys(json,
                           {"origin", "channel_id", "account_id", "sender_id", "conversation_id",
                            "provider_message_id"},
                           error)) {
        return std::nullopt;
    }
    std::string origin_name;
    if (!GetRequiredString(json, "origin", &origin_name, error)) return std::nullopt;
    const auto origin = MessageOriginFromName(origin_name);
    if (!origin.has_value()) {
        if (error != nullptr) *error = "unknown origin: " + origin_name;
        return std::nullopt;
    }
    MessageProvenance out;
    out.origin = *origin;
    // channel/account/sender/conversation/provider_message_id 只在
    // ExternalChannel 起源必填;其余起源(终端、peer 等)允许留空——由
    // FromJsonStrict 的调用方(session 事件行)按 origin 分支判必填。
    std::optional<std::string> value;
    if (!GetOptionalString(json, "channel_id", &value, error)) return std::nullopt;
    if (value.has_value()) out.channel_id = *value;
    value.reset();
    if (!GetOptionalString(json, "account_id", &value, error)) return std::nullopt;
    if (value.has_value()) out.account_id = *value;
    value.reset();
    if (!GetOptionalString(json, "sender_id", &value, error)) return std::nullopt;
    if (value.has_value()) out.sender_id = *value;
    value.reset();
    if (!GetOptionalString(json, "conversation_id", &value, error)) return std::nullopt;
    if (value.has_value()) out.conversation_id = *value;
    value.reset();
    if (!GetOptionalString(json, "provider_message_id", &value, error)) return std::nullopt;
    if (value.has_value()) out.provider_message_id = *value;
    if (out.origin == MessageOrigin::ExternalChannel &&
        (out.channel_id.empty() || out.account_id.empty() || out.sender_id.empty() ||
         out.conversation_id.empty())) {
        if (error != nullptr) {
            *error = "external_channel provenance requires channel_id/account_id/sender_id/"
                    "conversation_id";
        }
        return std::nullopt;
    }
    return out;
}

// ---------------------------------------------------------------------------
// ReplyActionKind / ReplyDurability
// ---------------------------------------------------------------------------

const char* ReplyActionKindName(ReplyActionKind kind) {
    switch (kind) {
        case ReplyActionKind::Send: return "send";
        case ReplyActionKind::Edit: return "edit";
        case ReplyActionKind::Typing: return "typing";
        case ReplyActionKind::React: return "react";
        case ReplyActionKind::Upload: return "upload";
    }
    return "?";
}

std::optional<ReplyActionKind> ReplyActionKindFromName(std::string_view name) {
    if (name == "send") return ReplyActionKind::Send;
    if (name == "edit") return ReplyActionKind::Edit;
    if (name == "typing") return ReplyActionKind::Typing;
    if (name == "react") return ReplyActionKind::React;
    if (name == "upload") return ReplyActionKind::Upload;
    return std::nullopt;
}

const char* ReplyDurabilityName(ReplyDurability durability) {
    switch (durability) {
        case ReplyDurability::Preview: return "preview";
        case ReplyDurability::Committed: return "committed";
    }
    return "?";
}

std::optional<ReplyDurability> ReplyDurabilityFromName(std::string_view name) {
    if (name == "preview") return ReplyDurability::Preview;
    if (name == "committed") return ReplyDurability::Committed;
    return std::nullopt;
}

// ---------------------------------------------------------------------------
// MediaAttachment
// ---------------------------------------------------------------------------

nlohmann::json MediaAttachment::ToJson() const {
    nlohmann::json out = nlohmann::json::object();
    out["mime_type"] = mime_type;
    out["local_path"] = local_path;
    if (size_bytes.has_value()) out["size_bytes"] = *size_bytes;
    PutOptionalString(out, "file_name", file_name);
    PutOptionalString(out, "caption", caption);
    return out;
}

std::optional<MediaAttachment> MediaAttachment::FromJsonStrict(const nlohmann::json& json,
                                                                std::string* error) {
    if (!RequireObject(json, error)) return std::nullopt;
    if (!RejectUnknownKeys(json, {"mime_type", "local_path", "size_bytes", "file_name", "caption"},
                           error)) {
        return std::nullopt;
    }
    MediaAttachment out;
    if (!GetRequiredString(json, "mime_type", &out.mime_type, error)) return std::nullopt;
    if (!GetRequiredString(json, "local_path", &out.local_path, error)) return std::nullopt;
    if (!GetOptionalInt64(json, "size_bytes", &out.size_bytes, error)) return std::nullopt;
    if (!GetOptionalString(json, "file_name", &out.file_name, error)) return std::nullopt;
    if (!GetOptionalString(json, "caption", &out.caption, error)) return std::nullopt;
    return out;
}

// ---------------------------------------------------------------------------
// ReplyAction
// ---------------------------------------------------------------------------

nlohmann::json ReplyAction::ToJson() const {
    nlohmann::json out = nlohmann::json::object();
    out["kind"] = ReplyActionKindName(kind);
    out["durability"] = ReplyDurabilityName(durability);
    if (!text.empty()) out["text"] = text;
    if (!media.empty()) {
        nlohmann::json media_json = nlohmann::json::array();
        for (const auto& item : media) media_json.push_back(item.ToJson());
        out["media"] = std::move(media_json);
    }
    if (!reply_to_message_id.empty()) out["reply_to_message_id"] = reply_to_message_id;
    if (!outbound_delivery_id.empty()) out["outbound_delivery_id"] = outbound_delivery_id;
    if (!client_id.empty()) out["client_id"] = client_id;
    return out;
}

std::optional<ReplyAction> ReplyAction::FromJsonStrict(const nlohmann::json& json, std::string* error) {
    if (!RequireObject(json, error)) return std::nullopt;
    if (!RejectUnknownKeys(json,
                           {"kind", "durability", "text", "media", "reply_to_message_id",
                            "outbound_delivery_id", "client_id"},
                           error)) {
        return std::nullopt;
    }
    std::string kind_name;
    if (!GetRequiredString(json, "kind", &kind_name, error)) return std::nullopt;
    const auto kind = ReplyActionKindFromName(kind_name);
    if (!kind.has_value()) {
        if (error != nullptr) *error = "unknown reply action kind: " + kind_name;
        return std::nullopt;
    }
    ReplyAction out;
    out.kind = *kind;
    out.durability = ReplyDurability::Committed;
    if (json.contains("durability")) {
        std::string durability_name;
        if (!GetRequiredString(json, "durability", &durability_name, error)) return std::nullopt;
        const auto durability = ReplyDurabilityFromName(durability_name);
        if (!durability.has_value()) {
            if (error != nullptr) *error = "unknown reply durability: " + durability_name;
            return std::nullopt;
        }
        out.durability = *durability;
    }
    std::optional<std::string> value;
    if (!GetOptionalString(json, "text", &value, error)) return std::nullopt;
    if (value.has_value()) out.text = *value;
    if (json.contains("media")) {
        if (!json.at("media").is_array()) {
            if (error != nullptr) *error = "invalid type for media (must be array)";
            return std::nullopt;
        }
        for (const auto& item : json.at("media")) {
            std::string sub_error;
            auto attachment = MediaAttachment::FromJsonStrict(item, &sub_error);
            if (!attachment.has_value()) {
                if (error != nullptr) *error = "media[]: " + sub_error;
                return std::nullopt;
            }
            out.media.push_back(std::move(*attachment));
        }
    }
    value.reset();
    if (!GetOptionalString(json, "reply_to_message_id", &value, error)) return std::nullopt;
    if (value.has_value()) out.reply_to_message_id = *value;
    value.reset();
    if (!GetOptionalString(json, "outbound_delivery_id", &value, error)) return std::nullopt;
    if (value.has_value()) out.outbound_delivery_id = *value;
    value.reset();
    if (!GetOptionalString(json, "client_id", &value, error)) return std::nullopt;
    if (value.has_value()) out.client_id = *value;
    return out;
}

}  // namespace lubancode::channel
