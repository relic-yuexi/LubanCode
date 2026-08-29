// 权限三档的中立枚举(自定义 Agent 与 Prompt Profile 单·阶段 5 从
// agent_profile_resolver.hpp 拆出):runtime::PermissionMode 的中立镜像——
// agent 层不 include turn_runtime(那头牵 config),枚举在此重申,映射由
// app 装配层做,与 runtime::PermissionMode 镜像 cli::ConfirmMode 同一规矩。
// 拆成独立轻头是因为 loop.hpp(TurnWiring 的家)也要用它(阶段 5 的
// on_tool_confirm_floored 口),而 resolver.hpp include agent.hpp、
// agent.hpp 又 include loop.hpp——不拆就循环。
#pragma once

#include <string>

namespace lubancode::agent {

enum class AgentPermissionMode { Confirm, Auto, Yolo };

// 宽窄序:confirm(0) < auto(1) < yolo(2)。子代理的档 rank 只许 <= 父。
inline constexpr int AgentPermissionModeRank(AgentPermissionMode mode) {
    return static_cast<int>(mode);
}

inline constexpr const char* ToString(AgentPermissionMode mode) {
    switch (mode) {
        case AgentPermissionMode::Confirm:
            return "confirm";
        case AgentPermissionMode::Auto:
            return "auto";
        case AgentPermissionMode::Yolo:
            return "yolo";
    }
    return "confirm";
}

}  // namespace lubancode::agent
