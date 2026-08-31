// 多渠道消息接入单阶段 1:src/channel/types.* 的结构体与 JSON 往返册。
// 照 message-contracts.md 冻结件逐项核:ChannelInboundEvent 九类 parts、
// MessageProvenance 的 origin 分档必填、ReplyAction 的 kind/durability。
// 严格解析规矩:未知键拒绝、缺必填拒绝、坏枚举值拒绝、坏 schema 版本拒绝。
#include <doctest/doctest.h>

#include "channel/types.hpp"

using namespace lubancode::channel;

namespace {

ChannelInboundEvent MakeSampleEvent() {
    ChannelInboundEvent event;
    event.delivery_id = "in-7";
    event.provider_event_id = "prov-evt-1";
    event.channel_id = "qqbot";
    event.account_id = "main";
    event.received_at_ms = 1724700000000;
    event.provider_at_ms = 1724699999000;
    event.conversation.kind = ConversationKind::Group;
    event.conversation.id = "group_openid";
    event.conversation.title = "测试群";
    event.sender.id = "sender-1";
    event.sender.display_name = "张三";
    event.sender.is_bot = false;
    event.sender.is_owner = false;
    event.message_id = "msg-1";
    event.reply_to_message_id = "msg-0";
    ChannelPart text_part;
    text_part.type = ChannelPartType::Text;
    text_part.text = "你好";
    event.parts.push_back(text_part);
    ChannelPart image_part;
    image_part.type = ChannelPartType::Image;
    image_part.media_id = "media-1";
    image_part.mime_type = "image/png";
    image_part.file_name = "pic.png";
    image_part.size_bytes = 1024;
    image_part.local_path = "/state/qqbot/main/media/pic.png";
    image_part.sha256 = std::string(64, 'a');
    event.parts.push_back(image_part);
    event.hints.mentions_bot = true;
    event.hints.command = "help";
    return event;
}

}  // namespace

TEST_CASE("ChannelInboundEvent:ToJson/FromJsonStrict 往返") {
    const ChannelInboundEvent original = MakeSampleEvent();
    const nlohmann::json json = original.ToJson();
    CHECK(json.at("schema") == kInboundEventSchemaVersion);
    CHECK(json.at("delivery_id") == "in-7");
    CHECK(json.at("conversation").at("kind") == "group");
    CHECK(json.at("parts").size() == 2);

    std::string error;
    const auto parsed = ChannelInboundEvent::FromJsonStrict(json, &error);
    REQUIRE_MESSAGE(parsed.has_value(), error);
    CHECK(parsed->delivery_id == original.delivery_id);
    CHECK(parsed->provider_event_id == original.provider_event_id);
    CHECK(parsed->channel_id == original.channel_id);
    CHECK(parsed->account_id == original.account_id);
    CHECK(parsed->received_at_ms == original.received_at_ms);
    CHECK(parsed->conversation.kind == ConversationKind::Group);
    CHECK(parsed->conversation.id == "group_openid");
    CHECK(parsed->sender.id == "sender-1");
    REQUIRE(parsed->parts.size() == 2);
    CHECK(parsed->parts[0].type == ChannelPartType::Text);
    CHECK(parsed->parts[0].text == "你好");
    CHECK(parsed->parts[1].type == ChannelPartType::Image);
    CHECK(parsed->parts[1].sha256 == std::string(64, 'a'));
    CHECK(parsed->hints.mentions_bot == true);
    CHECK(parsed->hints.command == "help");
    CHECK(parsed->reply_to_message_id == "msg-0");

    // 往返第二轮:parsed 再 ToJson 应与原 json 逐键相等(canonical 往返)。
    CHECK(parsed->ToJson() == json);
}

TEST_CASE("ChannelInboundEvent:未知顶层键拒绝") {
    nlohmann::json json = MakeSampleEvent().ToJson();
    json["bogus_field"] = "x";
    std::string error;
    const auto parsed = ChannelInboundEvent::FromJsonStrict(json, &error);
    CHECK_FALSE(parsed.has_value());
    CHECK(error.find("unknown field") != std::string::npos);
}

TEST_CASE("ChannelInboundEvent:坏 schema 版本拒绝") {
    nlohmann::json json = MakeSampleEvent().ToJson();
    json["schema"] = 2;
    std::string error;
    const auto parsed = ChannelInboundEvent::FromJsonStrict(json, &error);
    CHECK_FALSE(parsed.has_value());
    CHECK(error.find("unsupported schema version") != std::string::npos);
}

TEST_CASE("ChannelInboundEvent:缺必填字段拒绝") {
    nlohmann::json json = MakeSampleEvent().ToJson();
    json.erase("channel_id");
    std::string error;
    const auto parsed = ChannelInboundEvent::FromJsonStrict(json, &error);
    CHECK_FALSE(parsed.has_value());
    CHECK(error.find("channel_id") != std::string::npos);
}

TEST_CASE("ChannelInboundEvent:conversation 坏枚举值拒绝") {
    nlohmann::json json = MakeSampleEvent().ToJson();
    json["conversation"]["kind"] = "not-a-kind";
    std::string error;
    const auto parsed = ChannelInboundEvent::FromJsonStrict(json, &error);
    CHECK_FALSE(parsed.has_value());
}

TEST_CASE("ChannelPart:九类 type 名互转") {
    const ChannelPartType kinds[] = {ChannelPartType::Text,     ChannelPartType::Image,
                                     ChannelPartType::Audio,    ChannelPartType::Video,
                                     ChannelPartType::File,     ChannelPartType::Link,
                                     ChannelPartType::Mention,  ChannelPartType::Location,
                                     ChannelPartType::Unsupported};
    for (const auto kind : kinds) {
        const char* name = ChannelPartTypeName(kind);
        const auto roundtrip = ChannelPartTypeFromName(name);
        REQUIRE(roundtrip.has_value());
        CHECK(*roundtrip == kind);
    }
}

TEST_CASE("ChannelPart:link 缺 url 拒绝") {
    ChannelPart part;
    part.type = ChannelPartType::Link;
    nlohmann::json json = part.ToJson();
    std::string error;
    const auto parsed = ChannelPart::FromJsonStrict(json, &error);
    CHECK_FALSE(parsed.has_value());
    CHECK(error.find("url") != std::string::npos);
}

TEST_CASE("MessageProvenance:external_channel 缺字段拒绝,其余 origin 允许留空") {
    MessageProvenance prov;
    prov.origin = MessageOrigin::ExternalChannel;
    prov.channel_id = "qqbot";
    // sender_id/conversation_id/account_id 留空:应拒绝。
    std::string error;
    auto parsed = MessageProvenance::FromJsonStrict(prov.ToJson(), &error);
    CHECK_FALSE(parsed.has_value());

    prov.account_id = "main";
    prov.sender_id = "sender-1";
    prov.conversation_id = "group_openid";
    parsed = MessageProvenance::FromJsonStrict(prov.ToJson(), &error);
    REQUIRE_MESSAGE(parsed.has_value(), error);
    CHECK(parsed->origin == MessageOrigin::ExternalChannel);

    MessageProvenance terminal;
    terminal.origin = MessageOrigin::HumanTerminal;
    auto terminal_parsed = MessageProvenance::FromJsonStrict(terminal.ToJson(), &error);
    REQUIRE_MESSAGE(terminal_parsed.has_value(), error);
    CHECK(terminal_parsed->channel_id.empty());
}

TEST_CASE("ReplyAction:Send 带媒体往返") {
    ReplyAction action;
    action.kind = ReplyActionKind::Send;
    action.durability = ReplyDurability::Committed;
    action.text = "累计全文";
    action.reply_to_message_id = "om_123";
    action.outbound_delivery_id = "out-42";
    action.client_id = "out-42";
    MediaAttachment media;
    media.mime_type = "image/png";
    media.local_path = "/tmp/x.png";
    media.size_bytes = 2048;
    action.media.push_back(media);

    const nlohmann::json json = action.ToJson();
    std::string error;
    const auto parsed = ReplyAction::FromJsonStrict(json, &error);
    REQUIRE_MESSAGE(parsed.has_value(), error);
    CHECK(parsed->kind == ReplyActionKind::Send);
    CHECK(parsed->durability == ReplyDurability::Committed);
    CHECK(parsed->text == "累计全文");
    REQUIRE(parsed->media.size() == 1);
    CHECK(parsed->media[0].mime_type == "image/png");
    CHECK(parsed->client_id == "out-42");
}

TEST_CASE("ReplyAction:kind/durability 坏值拒绝") {
    nlohmann::json json = nlohmann::json::object();
    json["kind"] = "not-a-kind";
    std::string error;
    CHECK_FALSE(ReplyAction::FromJsonStrict(json, &error).has_value());

    json["kind"] = "send";
    json["durability"] = "not-a-durability";
    CHECK_FALSE(ReplyAction::FromJsonStrict(json, &error).has_value());
}
