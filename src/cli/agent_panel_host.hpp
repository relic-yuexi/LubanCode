// 终端接线收尾单:子代理面板的接线宿主。
//
// 病灶五(用户查账原文):provider/actions 挂 console_input.cpp 静态槽,
// 能用但不是实例注入。收进这一只实例:数据源(AgentPanelProvider)与
// 动作(AgentPanelActions)是实例成员,生命周期跟实例走——会话收场
// Reset 一并双清,不再靠两枚裸 std::function 各自记得清。导航坞布局与
// 按键状态机仍在 cli/agent_panel(纯逻辑组件,不动形状)。
//
// 进程级实例:console_input 的三处消费点(空闲 composer、流式 footer
// 重画、监听线程)经 SessionAgentPanelHost() 取同一只——与
// SharedEditor/PanelSessionSlot 同一款存法,单终端进程只有一本。

#pragma once

#include "cli/console_input.hpp"  // AgentPanelProvider/AgentPanelActions(接线类型)

namespace lubancode::cli {

class AgentPanelHost {
public:
    void SetProvider(AgentPanelProvider provider) { provider_ = std::move(provider); }
    void SetActions(AgentPanelActions actions) { actions_ = std::move(actions); }

    const AgentPanelProvider& provider() const { return provider_; }
    const AgentPanelActions& actions() const { return actions_; }

    // 会话收场(/clear、退出、切 worktree):provider/actions 双清。
    void Reset();

private:
    AgentPanelProvider provider_;
    AgentPanelActions actions_;
};

// 进程级面板接线实例。
AgentPanelHost& SessionAgentPanelHost();

}  // namespace lubancode::cli
