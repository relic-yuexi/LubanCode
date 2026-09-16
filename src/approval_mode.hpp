#pragma once

#include <optional>
#include <string>
#include <string_view>

namespace lubancode {

// 宿主之间共享的审批档位。枚举值不参与任何持久化协议；跨边界只写
// ApprovalModeMachineName() 返回的稳定名字。
enum class ApprovalMode { Default, AcceptEdits, Yolo, Auto, DontAsk };

// T11-B / V3-GAP-06(Session v3 旧设计清理单):审批档宽严序与策略版本。
// 宽严序按"自动放行面"排:default 全问(0) < accept_edits 只放文件编辑
// (1) < dont_ask 不问但非白名单即拒(2) < auto 放文件编辑+安全命令(3)
// < yolo 显式全放(4)。dont_ask 排 auto 之下:它对任意命令直接拒,拿到
// 的授权面窄于 auto(auto 还能问了再放)。恢复重算用:有效档 = 按此序
// 取源场档与当前策略的较严者,不静默提权。
inline constexpr int ApprovalModePermissiveness(ApprovalMode mode) {
    switch (mode) {
        case ApprovalMode::Default: return 0;
        case ApprovalMode::AcceptEdits: return 1;
        case ApprovalMode::DontAsk: return 2;
        case ApprovalMode::Auto: return 3;
        case ApprovalMode::Yolo: return 4;
    }
    return 0;
}

// 取较严档(T11-B 恢复重算的 clamp 口)。
inline constexpr ApprovalMode StricterApprovalMode(ApprovalMode a, ApprovalMode b) {
    return ApprovalModePermissiveness(a) <= ApprovalModePermissiveness(b) ? a : b;
}

// 审批档策略版本(v1 冻结):五档枚举 + 机器名 + 上述宽严序。v3 账
// approval.mode.applied 的 policyVersion 用它——序或枚举要变,先升版本,
// 不在同一版本下改旧含义。
inline constexpr std::string_view kApprovalPolicyVersion = "approval-policy-v1";

constexpr const char* ApprovalModeMachineName(ApprovalMode mode) {
    switch (mode) {
        case ApprovalMode::Default: return "default";
        case ApprovalMode::AcceptEdits: return "accept_edits";
        case ApprovalMode::Yolo: return "yolo";
        case ApprovalMode::Auto: return "auto";
        case ApprovalMode::DontAsk: return "dont_ask";
    }
    return "default";
}

// 严格解析供配置诊断使用。confirm 是旧协议兼容别名。
constexpr std::optional<ApprovalMode> ParseApprovalMode(std::string_view value) {
    if (value == "default" || value == "confirm") return ApprovalMode::Default;
    if (value == "accept_edits") return ApprovalMode::AcceptEdits;
    if (value == "yolo") return ApprovalMode::Yolo;
    if (value == "auto") return ApprovalMode::Auto;
    if (value == "dont_ask") return ApprovalMode::DontAsk;
    return std::nullopt;
}

// 不可信持久化边界使用：旧 confirm 收正，未知值保守退 Default。
constexpr ApprovalMode ParseApprovalModeOrDefault(std::string_view value) {
    const auto parsed = ParseApprovalMode(value);
    return parsed.value_or(ApprovalMode::Default);
}

constexpr const char* CanonicalApprovalModeName(std::string_view value) {
    return ApprovalModeMachineName(ParseApprovalModeOrDefault(value));
}

}  // namespace lubancode
