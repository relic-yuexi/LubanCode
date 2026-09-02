// 子代理审批五档的中立语义。五档不是线性 rank：有效权限由“自动能力集合
// 求交 + may_prompt 取 AND”得出。尤其 Yolo 的 may_prompt=true：它表示自身
// All 能力下通常无需询问，并不禁止更窄的后代把问题拉回主会话；只有
// DontAsk 明确禁止询问。
#pragma once

#include <cstdint>
#include <string>

namespace lubancode::agent {

enum class AgentPermissionMode { Default, AcceptEdits, Yolo, Auto, DontAsk };

enum class AutomaticCapability : std::uint8_t {
    None = 0,
    FileEdit = 1 << 0,
    SafeCommand = 1 << 1,
    All = 1 << 2,
};

struct AgentPermissionCapabilities {
    std::uint8_t automatic = 0;
    bool may_prompt = true;
};

inline constexpr AgentPermissionCapabilities PermissionCapabilities(AgentPermissionMode mode) {
    constexpr std::uint8_t kFileEdit = static_cast<std::uint8_t>(AutomaticCapability::FileEdit);
    constexpr std::uint8_t kSafeCommand = static_cast<std::uint8_t>(AutomaticCapability::SafeCommand);
    constexpr std::uint8_t kAll = static_cast<std::uint8_t>(AutomaticCapability::All);
    switch (mode) {
        case AgentPermissionMode::Default:
            return {0, true};
        case AgentPermissionMode::AcceptEdits:
            return {kFileEdit, true};
        case AgentPermissionMode::Yolo:
            return {static_cast<std::uint8_t>(kFileEdit | kSafeCommand | kAll), true};
        case AgentPermissionMode::Auto:
            return {static_cast<std::uint8_t>(kFileEdit | kSafeCommand), true};
        case AgentPermissionMode::DontAsk:
            return {0, false};
    }
    return {0, true};
}

inline constexpr AgentPermissionMode IntersectPermissionModes(AgentPermissionMode parent,
                                                               AgentPermissionMode child) {
    const AgentPermissionCapabilities lhs = PermissionCapabilities(parent);
    const AgentPermissionCapabilities rhs = PermissionCapabilities(child);
    const std::uint8_t automatic = static_cast<std::uint8_t>(lhs.automatic & rhs.automatic);
    if (!lhs.may_prompt || !rhs.may_prompt) return AgentPermissionMode::DontAsk;
    const auto yolo = PermissionCapabilities(AgentPermissionMode::Yolo).automatic;
    const auto auto_mode = PermissionCapabilities(AgentPermissionMode::Auto).automatic;
    const auto edits = PermissionCapabilities(AgentPermissionMode::AcceptEdits).automatic;
    if (automatic == yolo) return AgentPermissionMode::Yolo;
    if (automatic == auto_mode) return AgentPermissionMode::Auto;
    if (automatic == edits) return AgentPermissionMode::AcceptEdits;
    return AgentPermissionMode::Default;
}

inline constexpr const char* ToString(AgentPermissionMode mode) {
    switch (mode) {
        case AgentPermissionMode::Default:
            return "default";
        case AgentPermissionMode::AcceptEdits:
            return "accept_edits";
        case AgentPermissionMode::Yolo:
            return "yolo";
        case AgentPermissionMode::Auto:
            return "auto";
        case AgentPermissionMode::DontAsk:
            return "dont_ask";
    }
    return "default";
}

}  // namespace lubancode::agent
