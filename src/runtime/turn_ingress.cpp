// TurnIngress 实现(多渠道消息接入单阶段 3)。合同见 turn_ingress.hpp。
#include "runtime/turn_ingress.hpp"

namespace lubancode::runtime {

TurnIngress MakeChannelTurnIngress(const channel::ChannelInboundEvent& event,
                                   const channel::MessageProvenance& provenance,
                                   const std::string& session_key, bool allow_memory_retrieval) {
    TurnIngress ingress;
    ingress.source = TurnSource::Channel;
    ingress.provenance = provenance;
    ingress.session_key = session_key;
    // 回发路由:同一 conversation 的回复走同一条(阶段 4 ReplyAssembler
    // 用它定 reply_route_id;本批先把账带上)。
    ingress.reply_route = "channel:" + event.channel_id + ":" + event.account_id + ":" +
                          event.conversation.id;
    ingress.ingress_delivery_id = event.delivery_id;
    ingress.allow_memory_retrieval = allow_memory_retrieval;

    // 正文投影(§12.1):text 连拼;mention 带 @ 文本;媒体给一行稳定说明,
    // 不把二进制冒充文本。整条没有可投影内容时给占位行,不发空消息。
    std::string text;
    bool saw_anything = false;
    for (const channel::ChannelPart& part : event.parts) {
        switch (part.type) {
            case channel::ChannelPartType::Text:
                if (part.text.has_value() && !part.text->empty()) {
                    if (!text.empty()) text += "\n";
                    text += *part.text;
                    saw_anything = true;
                }
                break;
            case channel::ChannelPartType::Mention:
                if (part.text.has_value() && !part.text->empty()) {
                    if (!text.empty()) text += " ";
                    text += *part.text;
                }
                break;
            case channel::ChannelPartType::Image:
            case channel::ChannelPartType::Audio:
            case channel::ChannelPartType::Video:
            case channel::ChannelPartType::File: {
                if (!text.empty()) text += "\n";
                const char* kind = part.type == channel::ChannelPartType::Image    ? "图片"
                                   : part.type == channel::ChannelPartType::Audio  ? "音频"
                                   : part.type == channel::ChannelPartType::Video ? "视频"
                                                                                    : "文件";
                text += std::string("[收到") + kind;
                if (part.file_name.has_value() && !part.file_name->empty()) {
                    text += ":" + *part.file_name;
                }
                text += part.type == channel::ChannelPartType::Audio ? ",未转录]" : ",未解析]";
                saw_anything = true;
                break;
            }
            case channel::ChannelPartType::Link:
                if (part.url.has_value()) {
                    if (!text.empty()) text += "\n";
                    text += *part.url;
                    saw_anything = true;
                }
                break;
            case channel::ChannelPartType::Location:
                if (part.location_name.has_value()) {
                    if (!text.empty()) text += "\n";
                    text += "[位置:" + *part.location_name + "]";
                    saw_anything = true;
                }
                break;
            case channel::ChannelPartType::Unsupported:
                if (part.unsupported_reason.has_value()) {
                    if (!text.empty()) text += "\n";
                    text += *part.unsupported_reason;
                    saw_anything = true;
                }
                break;
        }
    }
    if (!saw_anything) {
        text = "[收到一条无文本内容的消息]";
    }
    ingress.message.role = api::Role::User;
    ingress.message.content.push_back(api::TextBlock{text});
    return ingress;
}

}  // namespace lubancode::runtime
