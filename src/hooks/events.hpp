// hooks 框架的事件面(Hooks 生命周期与信任协议,第一阶段对齐 Codex 的
// 共同骨架)。这里只定"有哪些事件、每个事件认哪些输出字段",不带任何
// 执行逻辑——解析、匹配、信任、并发、归并全在 dispatcher。
//
// 事件命名跟 Claude Code/Codex 的官方文档对齐(PascalCase),配置文件里
// 的键名就是这些名字。旧格式的小写下划线键(pre_tool/post_tool/
// session_start/session_end)由 loader 转 legacy adapter,不在这里出现。
#pragma once

#include <string_view>

namespace lubancode::hooks {

enum class HookEvent {
    SessionStart,
    SessionEnd,
    UserPromptSubmit,
    PreToolUse,
    PermissionRequest,
    PostToolUse,
    PreCompact,
    PostCompact,
    SubagentStart,
    SubagentStop,
    Stop,
};

// 配置键/协议字段里的规范名。
constexpr std::string_view ToString(HookEvent event) {
    switch (event) {
        case HookEvent::SessionStart:
            return "SessionStart";
        case HookEvent::SessionEnd:
            return "SessionEnd";
        case HookEvent::UserPromptSubmit:
            return "UserPromptSubmit";
        case HookEvent::PreToolUse:
            return "PreToolUse";
        case HookEvent::PermissionRequest:
            return "PermissionRequest";
        case HookEvent::PostToolUse:
            return "PostToolUse";
        case HookEvent::PreCompact:
            return "PreCompact";
        case HookEvent::PostCompact:
            return "PostCompact";
        case HookEvent::SubagentStart:
            return "SubagentStart";
        case HookEvent::SubagentStop:
            return "SubagentStop";
        case HookEvent::Stop:
            return "Stop";
    }
    return "Unknown";
}

// 配置键 -> 事件。不认得的键返回 false(调用方决定是报错还是忽略)。
constexpr bool ParseHookEvent(std::string_view name, HookEvent& out) {
    if (name == "SessionStart") {
        out = HookEvent::SessionStart;
    } else if (name == "SessionEnd") {
        out = HookEvent::SessionEnd;
    } else if (name == "UserPromptSubmit") {
        out = HookEvent::UserPromptSubmit;
    } else if (name == "PreToolUse") {
        out = HookEvent::PreToolUse;
    } else if (name == "PermissionRequest") {
        out = HookEvent::PermissionRequest;
    } else if (name == "PostToolUse") {
        out = HookEvent::PostToolUse;
    } else if (name == "PreCompact") {
        out = HookEvent::PreCompact;
    } else if (name == "PostCompact") {
        out = HookEvent::PostCompact;
    } else if (name == "SubagentStart") {
        out = HookEvent::SubagentStart;
    } else if (name == "SubagentStop") {
        out = HookEvent::SubagentStop;
    } else if (name == "Stop") {
        out = HookEvent::Stop;
    } else {
        return false;
    }
    return true;
}

// 每个事件"能匹配什么字段"——工具事件匹配 tool_name;SessionStart 匹配
// source(startup/resume/clear/compact);SessionEnd 匹配 reason;Pre/PostCompact
// 匹配 trigger(manual/auto);其余事件没有匹配对象,matcher 只能是 */缺省。
// 给 /hooks 与错误信息用人话说明,也给 dispatcher 判 matcher 是否适用。
constexpr bool EventMatchesOnToolName(HookEvent event) {
    return event == HookEvent::PreToolUse || event == HookEvent::PermissionRequest ||
           event == HookEvent::PostToolUse;
}

// 这个事件有没有"matcher 能匹配的字段"。工具事件匹配 tool_name;
// SessionStart 匹配 source(startup/resume/clear/compact);SessionEnd 匹配
// reason;Pre/PostCompact 匹配 trigger(manual/auto)。没有匹配字段的事件
// (UserPromptSubmit/Stop/Subagent*),matcher 只能缺省或 "*"——配置里写了
// 具体值在解析阶段就报错,不静默吞。
constexpr bool EventHasMatcherField(HookEvent event) {
    switch (event) {
        case HookEvent::PreToolUse:
        case HookEvent::PermissionRequest:
        case HookEvent::PostToolUse:
        case HookEvent::SessionStart:
        case HookEvent::SessionEnd:
        case HookEvent::PreCompact:
        case HookEvent::PostCompact:
            return true;
        case HookEvent::UserPromptSubmit:
        case HookEvent::SubagentStart:
        case HookEvent::SubagentStop:
        case HookEvent::Stop:
            return false;
    }
    return false;
}

// ---------------------------------------------------------------------------
// 每个事件认哪些 stdout 输出字段(逐事件 schema,协议层据此校验)。字段
// 用错报 hook failure,不悄悄吞(规格"统一 JSON 输出")。
// ---------------------------------------------------------------------------
struct EventOutputCapabilities {
    // permissionDecision(allow/deny/ask)与 permissionDecisionReason。
    bool permission_decision = false;
    // updatedInput(改写工具入参)。只许 PreToolUse,且只与 allow 同返。
    bool updated_input = false;
    // additionalContext(给模型的追加上下文/审查反馈)。
    bool additional_context = false;
    // continue=false 能不能拦住正在发生的事(阻断 prompt/compact;Stop 的
    // 语义反过来是"再续一轮")。
    bool can_block = false;
};

constexpr EventOutputCapabilities OutputCapabilities(HookEvent event) {
    switch (event) {
        case HookEvent::PreToolUse:
            return {true, true, true, true};
        case HookEvent::PermissionRequest:
            // PermissionRequest 只表态 allow/deny,不改写参数——改写走
            // PreToolUse 的 updatedInput,不许借道绕权限。
            return {true, false, true, false};
        case HookEvent::PostToolUse:
            // 副作用已发生:不能拦,不能撤销,只能给反馈。
            return {false, false, true, false};
        case HookEvent::UserPromptSubmit:
            return {false, false, true, true};
        case HookEvent::SessionStart:
            return {false, false, true, false};
        case HookEvent::SessionEnd:
            return {false, false, false, false};
        case HookEvent::PreCompact:
            return {false, false, true, true};
        case HookEvent::PostCompact:
            return {false, false, true, false};
        case HookEvent::SubagentStart:
            return {false, false, true, false};
        case HookEvent::SubagentStop:
            // continue=false = "还不能停,再收口一轮";stop_hook_active 防咬尾。
            return {false, false, true, true};
        case HookEvent::Stop:
            return {false, false, true, true};
    }
    return {};
}

}  // namespace lubancode::hooks
