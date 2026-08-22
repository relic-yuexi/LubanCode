// list_sessions 工具实现。说明文字(给模型看的)不走 i18n——那是界面
// 文案的事,跟 agent/loop、其余工具一个规矩。

#include "tools/list_sessions_tool.hpp"

#include <sstream>

#include "tools/tool_text.hpp"  // 模型可见文案(描述)查表,源头 prompts/tools/

namespace lubancode::tools {

namespace {

std::string StatusLabel(const std::string& status) {
    if (status == "busy") {
        return "忙(正在跑一轮)";
    }
    if (status == "waiting") {
        return "等待(等用户确认)";
    }
    if (status == "closing") {
        return "退出中";
    }
    return "空闲";
}

}  // namespace

ListSessionsTool::ListSessionsTool(std::function<std::vector<agent::PeerCard>()> peers_provider,
                                   std::string self_peer_id)
    : peers_provider_(std::move(peers_provider)), self_peer_id_(std::move(self_peer_id)) {}

std::string ListSessionsTool::description() const {
    // 文案在 src/prompts/tools/<语言>/list_sessions.md,兜底是迁移前的原文。
    return ToolText("list_sessions", "description",
                    "列出同一台机器上当前用户开启的其它 Lubancode 会话(不跨机器、不跨用户)。"
                    "每场会话给出 peer_id(短 id,发送消息时用来定人)、名字、状态(空闲/忙/等待)、"
                    "工作目录。只有在手头的结论会影响另一场活会话时才需要查它;不要闲聊、不要催问成环。");
}

nlohmann::json ListSessionsTool::input_schema() const {
    return nlohmann::json::object();  // 无参数
}

Tool::Result ListSessionsTool::execute(const nlohmann::json& /*input*/) {
    std::vector<agent::PeerCard> peers;
    if (peers_provider_) {
        peers = peers_provider_();
    }
    if (peers.empty()) {
        return Tool::Result{"当前没有其它可见的会话(只有本机同一用户的交互会话可见)。", false};
    }
    std::ostringstream out;
    out << "共 " << peers.size() << " 场会话:\n";
    for (const auto& card : peers) {
        if (card.peer_id == self_peer_id_) {
            continue;
        }
        out << "- " << card.name << " (peer_id=" << card.peer_id << ") · " << StatusLabel(card.status)
            << " · cwd=" << card.cwd << "\n";
    }
    return Tool::Result{out.str(), false};
}

}  // namespace lubancode::tools
