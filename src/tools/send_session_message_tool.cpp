// send_session_message 工具实现。

#include "tools/send_session_message_tool.hpp"

#include <optional>
#include <sstream>

#include "tools/tool_text.hpp"  // 模型可见文案(描述/参数说明)查表,源头 prompts/tools/

namespace lubancode::tools {

SendSessionMessageTool::SendSessionMessageTool(
    std::function<std::vector<peers::PeerCard>()> peers_provider,
    std::function<peers::PeerDelivery(const peers::PeerCard&, const std::string&)> send)
    : peers_provider_(std::move(peers_provider)), send_(std::move(send)) {}

std::string SendSessionMessageTool::description() const {
    // 文案在 src/prompts/tools/<语言>/send_session_message.md,兜底是迁移前的原文。
    return ToolText("send_session_message", "description",
                    "给同一台机器上另一场 Lubancode 会话递一条纯文本消息(不传文件、不传聊天记录,只递一张字条)。"
                    "target 填对方的名字或 peer_id(list_sessions 可查)。对端正忙时消息会在两次工具调用之间送达,"
                    "不打断它手头的工具;对端空闲则另起一轮。只有在手头结论会影响另一场活会话时才发送;"
                    "不许闲聊,不许催问成环。");
}

nlohmann::json SendSessionMessageTool::input_schema() const {
    return nlohmann::json{
        {"type", "object"},
        {"properties",
         {
             {"target", {{"type", "string"},
                         {"description", ToolText("send_session_message", "param.target",
                                                  "对方会话的名字或 peer_id")}}},
             {"text", {{"type", "string"},
                       {"description", ToolText("send_session_message", "param.text", "纯文本正文")}}},
         }},
        {"required", nlohmann::json::array({"target", "text"})},
    };
}

Tool::Result SendSessionMessageTool::execute(const nlohmann::json& input) {
    if (!input.is_object() || !input.contains("target") || !input.contains("text") ||
        !input["target"].is_string() || !input["text"].is_string()) {
        return Tool::Result{"参数不对:需要字符串字段 target 与 text。", true};
    }
    const std::string target = input["target"].get<std::string>();
    const std::string text = input["text"].get<std::string>();
    if (target.empty() || text.empty()) {
        return Tool::Result{"target 与 text 都不能为空。", true};
    }

    std::vector<peers::PeerCard> peers;
    if (peers_provider_) {
        peers = peers_provider_();
    }
    std::optional<peers::PeerCard> found;
    for (const auto& card : peers) {
        if (card.peer_id == target || card.name == target) {
            found = card;
            break;
        }
    }
    if (!found.has_value()) {
        return Tool::Result{"找不到会话 \"" + target +
                                "\"。用 list_sessions 先查当前可见的会话(名字或 peer_id)。",
                            true};
    }
    if (!send_) {
        return Tool::Result{"跨会话传话在本场会话没有启用。", true};
    }

    const peers::PeerDelivery status = send_(*found, text);
    switch (status) {
        case peers::PeerDelivery::Delivered:
            return Tool::Result{"已送达 " + found->name + "(" + found->peer_id + ")。", false};
        case peers::PeerDelivery::Held:
            return Tool::Result{"对方已收到但先扣住了,等它的用户点头。", false};
        case peers::PeerDelivery::Refused:
            return Tool::Result{"对方回绝了这条消息(权限档为 refuse)。", true};
        case peers::PeerDelivery::Expired:
            return Tool::Result{"对方限速或队列已满,这条没有收下;稍等再试,别连续重发。", true};
        case peers::PeerDelivery::Unavailable:
            return Tool::Result{"对方会话不在(已退出或不可达),这条没有送达。", true};
    }
    return Tool::Result{"未知发送结果。", true};
}

}  // namespace lubancode::tools
