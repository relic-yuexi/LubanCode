// 跨会话传话:模型侧的窄工具之二——给另一场会话递一张字条。
//
// 正文只认纯文本;对端收不收、什么时候读,由对端的安全收件点与权限档
// 决定(见 peers/peer_session.hpp)。发送方拿回 delivered/held/refused/
// expired/unavailable,如实转告模型。需要确认(给出去的是一次跨会话
// 副作用,不该静默发送)。

#pragma once

#include <functional>
#include <string>
#include <vector>

#include "peers/peer_mailbox.hpp"  // PeerDelivery
#include "peers/peer_registry.hpp"
#include "tools/tool.hpp"

namespace lubancode::tools {

class SendSessionMessageTool : public Tool {
public:
    // peers_provider:列会话(找目标);send:PeerRuntime::Send 的转发。
    SendSessionMessageTool(std::function<std::vector<agent::PeerCard>()> peers_provider,
                           std::function<agent::PeerDelivery(const agent::PeerCard&, const std::string&)> send);

    std::string name() const override { return "send_session_message"; }
    std::string description() const override;
    nlohmann::json input_schema() const override;
    bool needs_confirm() const override { return true; }
    Tool::Result execute(const nlohmann::json& input) override;

private:
    std::function<std::vector<agent::PeerCard>()> peers_provider_;
    std::function<agent::PeerDelivery(const agent::PeerCard&, const std::string&)> send_;
};

}  // namespace lubancode::tools
