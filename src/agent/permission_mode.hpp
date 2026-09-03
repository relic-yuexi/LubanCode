// 子代理审批档的中立语义(收口审计单 P1:四套审批枚举收口)。值域不再
// 另造:直接吃公共 lubancode::ApprovalMode,本件只持能力投影。
// 五档不是线性 rank:有效权限由“自动能力集合求交 + may_prompt 取 AND”
// 得出。尤其 Yolo 的 may_prompt=true:它表示自身 All 能力下通常无需询问,
// 并不禁止更窄的后代把问题拉回主会话;只有 DontAsk 明确禁止询问。
// 档位比较一律按能力集合(PermissionCapabilities/IntersectPermissionModes),
// 不拿枚举大小或声明序排强弱。
#pragma once

#include <cstdint>

#include "approval_mode.hpp"

namespace lubancode::agent {

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

inline constexpr AgentPermissionCapabilities PermissionCapabilities(ApprovalMode mode) {
    constexpr std::uint8_t kFileEdit = static_cast<std::uint8_t>(AutomaticCapability::FileEdit);
    constexpr std::uint8_t kSafeCommand = static_cast<std::uint8_t>(AutomaticCapability::SafeCommand);
    constexpr std::uint8_t kAll = static_cast<std::uint8_t>(AutomaticCapability::All);
    switch (mode) {
        case ApprovalMode::Default:
            return {0, true};
        case ApprovalMode::AcceptEdits:
            return {kFileEdit, true};
        case ApprovalMode::Yolo:
            return {static_cast<std::uint8_t>(kFileEdit | kSafeCommand | kAll), true};
        case ApprovalMode::Auto:
            return {static_cast<std::uint8_t>(kFileEdit | kSafeCommand), true};
        case ApprovalMode::DontAsk:
            return {0, false};
    }
    return {0, true};
}

inline constexpr ApprovalMode IntersectPermissionModes(ApprovalMode parent, ApprovalMode child) {
    const AgentPermissionCapabilities lhs = PermissionCapabilities(parent);
    const AgentPermissionCapabilities rhs = PermissionCapabilities(child);
    const std::uint8_t automatic = static_cast<std::uint8_t>(lhs.automatic & rhs.automatic);
    if (!lhs.may_prompt || !rhs.may_prompt) return ApprovalMode::DontAsk;
    const auto yolo = PermissionCapabilities(ApprovalMode::Yolo).automatic;
    const auto auto_mode = PermissionCapabilities(ApprovalMode::Auto).automatic;
    const auto edits = PermissionCapabilities(ApprovalMode::AcceptEdits).automatic;
    if (automatic == yolo) return ApprovalMode::Yolo;
    if (automatic == auto_mode) return ApprovalMode::Auto;
    if (automatic == edits) return ApprovalMode::AcceptEdits;
    return ApprovalMode::Default;
}

// 机读名直接用公共值域的 ApprovalModeMachineName(approval_mode.hpp);
// 解析持久化/YAML 一律按 machine name(ParseApprovalMode*),不走整数。

}  // namespace lubancode::agent
