// 渠道消息合同(多渠道消息接入单阶段 1):ChannelInboundEvent、
// MessageProvenance、ReplyAction 的结构体与 JSON 往返。
//
// 唯一真源是 docs/architecture/channels/message-contracts.md(阶段 0
// 冻结件);字段、枚举取值、parts 九类、去重键、preview/committed 分账
// 全照那份文档钉死,这里不另造语义。
//
// 编码风格照 trajectory/event.hpp 的样子("P0-1a 轨迹单的模式"):每个
// 顶层信封类型 schema 字段起于 1;ToJson()/FromJsonStrict() 一对,严格
// 解析——未知键、类型错、缺必填字段一律拒绝,不静默放宽。这里的"严格"
// 比 trajectory 的表驱动校验轻:字段集不大,逐字段手写查得清楚,不架
// PayloadField 表。
//
// 依赖铁律:channel 是纯合同库,不 include app/cli/runtime/agent;
// 只用 nlohmann::json 与标准库。
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

namespace lubancode::channel {

// ---------------------------------------------------------------------------
// 固定合同值
// ---------------------------------------------------------------------------

// ChannelInboundEvent 的信封 schema 版本(message-contracts.md §1)。
inline constexpr int kInboundEventSchemaVersion = 1;

// ---------------------------------------------------------------------------
// ConversationKind / ChannelConversation(message-contracts.md §1)
// ---------------------------------------------------------------------------

enum class ConversationKind { Direct, Group, Guild, Channel, Thread };

const char* ConversationKindName(ConversationKind kind);
std::optional<ConversationKind> ConversationKindFromName(std::string_view name);

struct ChannelConversation {
    ConversationKind kind = ConversationKind::Direct;
    std::string id;
    std::optional<std::string> parent_id;
    std::optional<std::string> thread_id;
    std::string title;

    nlohmann::json ToJson() const;
    // 严格解析:未知键、缺 kind/id、坏枚举值一律拒绝(error 落 *error)。
    static std::optional<ChannelConversation> FromJsonStrict(const nlohmann::json& json,
                                                              std::string* error);
};

// ---------------------------------------------------------------------------
// ChannelSender(message-contracts.md §1)
// ---------------------------------------------------------------------------

struct ChannelSender {
    std::string id;
    std::string display_name;
    bool is_bot = false;
    // is_owner 只能由宿主 allowlist 推导,sidecar 声称不算——这份真值
    // 归宿主。FromJsonStrict 允许该键存在(sidecar 上报時可以声称),但
    // 调用方(阶段 2 起的 ChannelRouter)必须自行按宿主 allowlist 复算,
    // 不得直接采信入站值。这条规矩落在调用点,不落在本结构体。
    bool is_owner = false;

    nlohmann::json ToJson() const;
    static std::optional<ChannelSender> FromJsonStrict(const nlohmann::json& json,
                                                        std::string* error);
};

// ---------------------------------------------------------------------------
// ChannelPart:入站消息分块,首版九类(message-contracts.md §1)
// ---------------------------------------------------------------------------

enum class ChannelPartType {
    Text,
    Image,
    Audio,
    Video,
    File,
    Link,
    Mention,
    Location,
    Unsupported
};

const char* ChannelPartTypeName(ChannelPartType type);
std::optional<ChannelPartType> ChannelPartTypeFromName(std::string_view name);

// 媒体 part 的公共字段(message-contracts.md §1 "每个媒体 part 只带")。
// text/mention/location 用不到的字段留空。
struct ChannelPart {
    ChannelPartType type = ChannelPartType::Text;

    // text:正文。mention:被提及者的展示文本(平台原文,不代替 sender 账)。
    std::optional<std::string> text;

    // 媒体(image/audio/video/file)字段。
    std::optional<std::string> media_id;      // 平台 media id
    std::optional<std::string> mime_type;
    std::optional<std::string> file_name;
    std::optional<std::int64_t> size_bytes;
    std::optional<std::string> local_path;    // sidecar 已下载的受控本地路径
    std::optional<std::string> remote_ref;    // 待下载引用(未下载时用这个,不用 local_path)
    std::optional<std::string> caption;
    std::optional<std::string> sha256;        // 已下载时才有

    // link:URL。mention:被提及者的平台 id。location:纬度/经度/地名。
    std::optional<std::string> url;
    std::optional<std::string> mentioned_id;
    std::optional<double> latitude;
    std::optional<double> longitude;
    std::optional<std::string> location_name;

    // unsupported:给模型的一行稳定说明(§1 "给模型一行稳定说明,不悄悄丢掉")。
    std::optional<std::string> unsupported_reason;

    nlohmann::json ToJson() const;
    static std::optional<ChannelPart> FromJsonStrict(const nlohmann::json& json, std::string* error);
};

// ---------------------------------------------------------------------------
// ChannelIngressHints(message-contracts.md §1:字段集阶段 1 定档)
// ---------------------------------------------------------------------------

struct ChannelIngressHints {
    // mention 命中:消息里是否 @ 了本 bot(群聊 require_mention 准入判断用)。
    bool mentions_bot = false;
    // 命令前缀识别:如 "/help" 解出的命令名(不含前缀),没识别到留空。
    std::optional<std::string> command;
    std::optional<std::string> command_args;
    // 平台侧标的"这是一条回复/引用消息"(与 reply_to_message_id 呼应,
    // 部分平台只给标志不给具体 id)。
    bool is_reply = false;

    nlohmann::json ToJson() const;
    static std::optional<ChannelIngressHints> FromJsonStrict(const nlohmann::json& json,
                                                              std::string* error);
};

// ---------------------------------------------------------------------------
// ChannelInboundEvent(message-contracts.md §1 主信封)
// ---------------------------------------------------------------------------

struct ChannelInboundEvent {
    int schema = kInboundEventSchemaVersion;
    std::string delivery_id;        // sidecar -> host 投递 id
    std::string provider_event_id;  // 平台原始去重 id
    std::string channel_id;
    std::string account_id;
    std::int64_t received_at_ms = 0;
    std::int64_t provider_at_ms = 0;

    ChannelConversation conversation;
    ChannelSender sender;
    std::string message_id;
    std::optional<std::string> reply_to_message_id;
    std::vector<ChannelPart> parts;
    ChannelIngressHints hints;

    nlohmann::json ToJson() const;
    // 严格解析:未知顶层键、缺必填字段(delivery_id/channel_id/account_id/
    // message_id/conversation/sender)、schema 不认得的版本一律拒绝。
    static std::optional<ChannelInboundEvent> FromJsonStrict(const nlohmann::json& json,
                                                              std::string* error);
};

// ---------------------------------------------------------------------------
// MessageProvenance(message-contracts.md §2)
// ---------------------------------------------------------------------------

enum class MessageOrigin {
    HumanTerminal,
    ExternalChannel,
    PeerSession,
    BackgroundCompletion,
    ToolResult,
    HostSynthetic,
};

const char* MessageOriginName(MessageOrigin origin);
std::optional<MessageOrigin> MessageOriginFromName(std::string_view name);

struct MessageProvenance {
    MessageOrigin origin = MessageOrigin::HostSynthetic;
    std::string channel_id;
    std::string account_id;
    std::string sender_id;
    std::string conversation_id;
    std::string provider_message_id;

    nlohmann::json ToJson() const;
    static std::optional<MessageProvenance> FromJsonStrict(const nlohmann::json& json,
                                                            std::string* error);
};

// ---------------------------------------------------------------------------
// ReplyAction(message-contracts.md §5)
// ---------------------------------------------------------------------------

enum class ReplyActionKind { Send, Edit, Typing, React, Upload };
enum class ReplyDurability { Preview, Committed };

const char* ReplyActionKindName(ReplyActionKind kind);
std::optional<ReplyActionKind> ReplyActionKindFromName(std::string_view name);
const char* ReplyDurabilityName(ReplyDurability durability);
std::optional<ReplyDurability> ReplyDurabilityFromName(std::string_view name);

// 出站媒体附件(channel.send/channel.edit 的 parts 里 file 一类的宿主侧
// 折算;bridge-protocol.md §4 channel.send 样例)。
struct MediaAttachment {
    std::string mime_type;
    std::string local_path;
    std::optional<std::int64_t> size_bytes;
    std::optional<std::string> file_name;
    std::optional<std::string> caption;

    nlohmann::json ToJson() const;
    static std::optional<MediaAttachment> FromJsonStrict(const nlohmann::json& json,
                                                          std::string* error);
};

struct ReplyAction {
    ReplyActionKind kind = ReplyActionKind::Send;
    ReplyDurability durability = ReplyDurability::Committed;
    std::string text;                  // Send/Edit 正文;edit 发累计全文
    std::vector<MediaAttachment> media;
    std::string reply_to_message_id;   // 回复关联
    std::string outbound_delivery_id;  // Committed 动作的账
    std::string client_id;             // 平台幂等 id

    nlohmann::json ToJson() const;
    static std::optional<ReplyAction> FromJsonStrict(const nlohmann::json& json, std::string* error);
};

}  // namespace lubancode::channel
