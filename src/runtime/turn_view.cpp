// TurnView 枚举 <-> 稳定字符串(终端回合视觉收束单)。
//
// 线上表示是字符串(与 event.hpp/turn_item.hpp 同一约定):数字重排就是
// 协议破坏。Parse 认不得就返回 false,不静默映射默认值——把协议错误留给
// 调用方决断,不藏成数据错误。

#include "runtime/turn_view.hpp"

namespace lubancode::runtime {

std::string ToString(TurnItemViewKind kind) {
    switch (kind) {
        case TurnItemViewKind::User: return "user";
        case TurnItemViewKind::Thinking: return "thinking";
        case TurnItemViewKind::Text: return "text";
        case TurnItemViewKind::Tool: return "tool";
        case TurnItemViewKind::Warning: return "warning";
        case TurnItemViewKind::Error: return "error";
    }
    return "tool";
}

std::string ToString(TurnItemViewState status) {
    switch (status) {
        case TurnItemViewState::Pending: return "pending";
        case TurnItemViewState::Running: return "running";
        case TurnItemViewState::Succeeded: return "succeeded";
        case TurnItemViewState::Failed: return "failed";
        case TurnItemViewState::Declined: return "declined";
        case TurnItemViewState::Cancelled: return "cancelled";
        case TurnItemViewState::Interrupted: return "interrupted";
        case TurnItemViewState::Skipped: return "skipped";
    }
    return "running";
}

std::string ToString(TurnActivityPhase phase) {
    switch (phase) {
        case TurnActivityPhase::Idle: return "idle";
        case TurnActivityPhase::WaitingModel: return "waiting_model";
        case TurnActivityPhase::Thinking: return "thinking";
        case TurnActivityPhase::RunningTool: return "running_tool";
        case TurnActivityPhase::WaitingApproval: return "waiting_approval";
        case TurnActivityPhase::Stopping: return "stopping";
    }
    return "idle";
}

bool ParseTurnItemViewKind(const std::string& s, TurnItemViewKind& out) {
    if (s == "user") { out = TurnItemViewKind::User; return true; }
    if (s == "thinking") { out = TurnItemViewKind::Thinking; return true; }
    if (s == "text") { out = TurnItemViewKind::Text; return true; }
    if (s == "tool") { out = TurnItemViewKind::Tool; return true; }
    if (s == "warning") { out = TurnItemViewKind::Warning; return true; }
    if (s == "error") { out = TurnItemViewKind::Error; return true; }
    return false;
}

bool ParseTurnItemViewState(const std::string& s, TurnItemViewState& out) {
    if (s == "pending") { out = TurnItemViewState::Pending; return true; }
    if (s == "running") { out = TurnItemViewState::Running; return true; }
    if (s == "succeeded") { out = TurnItemViewState::Succeeded; return true; }
    if (s == "failed") { out = TurnItemViewState::Failed; return true; }
    if (s == "declined") { out = TurnItemViewState::Declined; return true; }
    if (s == "cancelled") { out = TurnItemViewState::Cancelled; return true; }
    if (s == "interrupted") { out = TurnItemViewState::Interrupted; return true; }
    if (s == "skipped") { out = TurnItemViewState::Skipped; return true; }
    return false;
}

bool ParseTurnActivityPhase(const std::string& s, TurnActivityPhase& out) {
    if (s == "idle") { out = TurnActivityPhase::Idle; return true; }
    if (s == "waiting_model") { out = TurnActivityPhase::WaitingModel; return true; }
    if (s == "thinking") { out = TurnActivityPhase::Thinking; return true; }
    if (s == "running_tool") { out = TurnActivityPhase::RunningTool; return true; }
    if (s == "waiting_approval") { out = TurnActivityPhase::WaitingApproval; return true; }
    if (s == "stopping") { out = TurnActivityPhase::Stopping; return true; }
    return false;
}

}  // namespace lubancode::runtime
