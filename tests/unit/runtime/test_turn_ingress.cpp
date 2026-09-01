// TurnIngress 投影单测(多渠道消息接入单阶段 3):渠道事件折回合入账
//(§12.1 投影规矩:text 连拼、媒体给稳定说明、空消息给占位行)。

#include <doctest/doctest.h>

#include <string>

#include "channel/types.hpp"
#include "runtime/turn_ingress.hpp"

using namespace lubancode;
using namespace lubancode::runtime;

namespace {

channel::ChannelPart Text(std::string value) {
    channel::ChannelPart part;
    part.type = channel::ChannelPartType::Text;
    part.text = std::move(value);
    return part;
}

channel::ChannelPart Media(channel::ChannelPartType type, std::string name) {
    channel::ChannelPart part;
    part.type = type;
    part.file_name = std::move(name);
    part.mime_type = "application/octet-stream";
    return part;
}

channel::ChannelInboundEvent MakeEvent() {
    channel::ChannelInboundEvent event;
    event.delivery_id = "d-7";
    event.channel_id = "qqbot";
    event.account_id = "main";
    event.conversation.kind = channel::ConversationKind::Direct;
    event.conversation.id = "dm-1";
    event.sender.id = "owner";
    event.message_id = "m-7";
    return event;
}

std::string OnlyText(const TurnIngress& ingress) {
    REQUIRE(ingress.message.content.size() == 1);
    const auto* block = std::get_if<api::TextBlock>(&ingress.message.content[0]);
    REQUIRE(block != nullptr);
    return block->text;
}

}  // namespace

TEST_CASE("MakeChannelTurnIngress:合同字段全带") {
    const auto event = MakeEvent();
    channel::MessageProvenance provenance;
    provenance.origin = channel::MessageOrigin::ExternalChannel;
    provenance.channel_id = "qqbot";
    provenance.sender_id = "owner";

    const auto ingress =
        MakeChannelTurnIngress(event, provenance, "channel:qqbot:main:direct:dm-1", false);
    CHECK(ingress.source == TurnSource::Channel);
    CHECK(ingress.session_key == "channel:qqbot:main:direct:dm-1");
    CHECK(ingress.reply_route == "channel:qqbot:main:dm-1");
    CHECK(ingress.ingress_delivery_id == "d-7");
    CHECK_FALSE(ingress.allow_memory_retrieval);
    CHECK(ingress.provenance.channel_id == "qqbot");
    CHECK(ingress.message.role == api::Role::User);
}

TEST_CASE("投影:text 多块连拼,mention 附带") {
    auto event = MakeEvent();
    event.parts.push_back(Text("第一行"));
    event.parts.push_back(Text("第二行"));
    channel::ChannelPart mention;
    mention.type = channel::ChannelPartType::Mention;
    mention.text = "@bot";
    event.parts.push_back(mention);

    const auto ingress =
        MakeChannelTurnIngress(event, channel::MessageProvenance{}, "k", true);
    CHECK(OnlyText(ingress) == "第一行\n第二行 @bot");
}

TEST_CASE("投影:媒体给稳定说明,不冒充文本") {
    auto event = MakeEvent();
    event.parts.push_back(Media(channel::ChannelPartType::Image, "photo.png"));
    event.parts.push_back(Media(channel::ChannelPartType::Audio, "voice.amr"));
    event.parts.push_back(Media(channel::ChannelPartType::File, "doc.pdf"));

    const auto text = OnlyText(MakeChannelTurnIngress(event, channel::MessageProvenance{}, "k", true));
    CHECK(text.find("图片:photo.png") != std::string::npos);
    CHECK(text.find("未解析") != std::string::npos);
    CHECK(text.find("音频:voice.amr") != std::string::npos);
    CHECK(text.find("未转录") != std::string::npos);
    CHECK(text.find("文件:doc.pdf") != std::string::npos);
}

TEST_CASE("投影:unsupported 给一行说明,空消息给占位") {
    auto event = MakeEvent();
    channel::ChannelPart unsupported;
    unsupported.type = channel::ChannelPartType::Unsupported;
    unsupported.unsupported_reason = "[平台贴纸,暂不支持]";
    event.parts.push_back(unsupported);
    CHECK(OnlyText(MakeChannelTurnIngress(event, channel::MessageProvenance{}, "k", true)) ==
          "[平台贴纸,暂不支持]");

    auto empty = MakeEvent();
    CHECK(OnlyText(MakeChannelTurnIngress(empty, channel::MessageProvenance{}, "k", true)) ==
          "[收到一条无文本内容的消息]");
}
