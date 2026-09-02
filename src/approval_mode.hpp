#pragma once

#include <optional>
#include <string_view>

namespace lubancode {

// 宿主之间共享的审批档位。枚举值不参与任何持久化协议；跨边界只写
// ApprovalModeMachineName() 返回的稳定名字。
enum class ApprovalMode { Default, AcceptEdits, Yolo, Auto, DontAsk };

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
