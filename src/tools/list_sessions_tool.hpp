// 跨会话传话:模型侧的窄工具之一——列同机可见的其它会话。
//
// 名字刻意不叫 list_agents(规格:不要复用/混淆 subagent 的 agent 工具,
// 跨会话与父子代理不绑一张表)。只读,不需要确认。数据由 main.cpp 注入
// (PeerRuntime::ListPeers),工具本身不碰文件、不碰线程。

#pragma once

#include <functional>
#include <string>
#include <vector>

#include "peers/peer_registry.hpp"
#include "tools/tool.hpp"

namespace lubancode::tools {

class ListSessionsTool : public Tool {
public:
    // self_peer_id:本场的 peer_id,列表里不出现自己。
    explicit ListSessionsTool(std::function<std::vector<agent::PeerCard>()> peers_provider,
                              std::string self_peer_id);

    std::string name() const override { return "list_sessions"; }
    std::string description() const override;
    nlohmann::json input_schema() const override;
    Tool::Result execute(const nlohmann::json& input) override;

private:
    std::function<std::vector<agent::PeerCard>()> peers_provider_;
    std::string self_peer_id_;
};

}  // namespace lubancode::tools
