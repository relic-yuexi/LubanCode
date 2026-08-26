// 面板接线宿主实现(合同见 agent_panel_host.hpp)。

#include "cli/agent_panel_host.hpp"

namespace lubancode::cli {

void AgentPanelHost::Reset() {
    provider_ = AgentPanelProvider{};
    actions_ = AgentPanelActions{};
}

AgentPanelHost& SessionAgentPanelHost() {
    static AgentPanelHost host;
    return host;
}

}  // namespace lubancode::cli
