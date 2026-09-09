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
    // loop 单的生命周期四事件:创建可 deny(拒建/收紧
    // interval,不能扩大权限);拍起止只能给反馈不能拦;
    // Stop 在每拍 turn 末尾照跑(它的 continuation prompt 属于本拍内
    // 的 agent continuation,不是下一时钟拍)。
    LoopTaskCreate,
    LoopTickStart,
    LoopTickEnd,
    LoopTaskStop,
    // goal 单的生命周期事件(goal 单"Hooks"节的中立事件面):GoalCreated
    // 在 created 事件已落后发(可 deny = 不许再往下走?——单子边界:Hook
    // 不可直接写 Achieved,这里全部只给 additionalContext 与审计,没有
    // permission_decision);GoalIterationStart/End 钉 iteration 的起止;
    // GoalEvaluated 在判词落地后发;GoalPaused 是可恢复暂停的审计点;
    // GoalCompleted 在 terminal 事件已落后跑(它失败不把 Achieved 改回
    // Active,单子原文)。匹配字段:GoalCreated/GoalEvaluated/GoalPaused/
    // GoalCompleted 匹配稳定短码(state/decision/reason),Iteration 起止
    // 没有匹配对象。
    GoalCreated,
    GoalIterationStart,
    GoalIterationEnd,
    GoalEvaluated,
    GoalPaused,
    GoalCompleted,
    // 四层生命周期单 P2:Session/Turn/Step 三层的分层事件(§四矩阵)。口径
    // 与旧事件并存不串台:
    //   Pre/PostSession —— 会话实体创建后/真正关闭时(进程起收),resume/
    //     clear/compact 不触发;旧 SessionStart/End 保留原触发面(兼容)。
    //   Pre/PostTurn —— 用户输入被接受后、首个 Step 前 / 终局(最终
    //     assistant 落定且无续跑,恰好一次)。Stop 判续跑则 PostTurn 不触发。
    //   Pre/PostStep —— 每次 Step 请求构建前(steer 合批后)/ assistant 响应
    //     落账后、派生 Action 执行前。PreStep 否决=终止本 Turn。
    // Action 层不另立枚举:PreAction/PostAction 是 PreToolUse/PostToolUse 的
    // 升格别名——ParseHookEvent 认新名,枚举值与 wire 事件名共用旧名,既有
    // 配置与脚本零迁移(见 ParseHookEvent 的别名表)。
    PreSession,
    PostSession,
    PreTurn,
    PostTurn,
    PreStep,
    PostStep,
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
        case HookEvent::LoopTaskCreate:
            return "LoopTaskCreate";
        case HookEvent::LoopTickStart:
            return "LoopTickStart";
        case HookEvent::LoopTickEnd:
            return "LoopTickEnd";
        case HookEvent::LoopTaskStop:
            return "LoopTaskStop";
        case HookEvent::GoalCreated:
            return "GoalCreated";
        case HookEvent::GoalIterationStart:
            return "GoalIterationStart";
        case HookEvent::GoalIterationEnd:
            return "GoalIterationEnd";
        case HookEvent::GoalEvaluated:
            return "GoalEvaluated";
        case HookEvent::GoalPaused:
            return "GoalPaused";
        case HookEvent::GoalCompleted:
            return "GoalCompleted";
        case HookEvent::PreSession:
            return "PreSession";
        case HookEvent::PostSession:
            return "PostSession";
        case HookEvent::PreTurn:
            return "PreTurn";
        case HookEvent::PostTurn:
            return "PostTurn";
        case HookEvent::PreStep:
            return "PreStep";
        case HookEvent::PostStep:
            return "PostStep";
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
    } else if (name == "LoopTaskCreate") {
        out = HookEvent::LoopTaskCreate;
    } else if (name == "LoopTickStart") {
        out = HookEvent::LoopTickStart;
    } else if (name == "LoopTickEnd") {
        out = HookEvent::LoopTickEnd;
    } else if (name == "LoopTaskStop") {
        out = HookEvent::LoopTaskStop;
    } else if (name == "GoalCreated") {
        out = HookEvent::GoalCreated;
    } else if (name == "GoalIterationStart") {
        out = HookEvent::GoalIterationStart;
    } else if (name == "GoalIterationEnd") {
        out = HookEvent::GoalIterationEnd;
    } else if (name == "GoalEvaluated") {
        out = HookEvent::GoalEvaluated;
    } else if (name == "GoalPaused") {
        out = HookEvent::GoalPaused;
    } else if (name == "GoalCompleted") {
        out = HookEvent::GoalCompleted;
    } else if (name == "PreSession") {
        out = HookEvent::PreSession;
    } else if (name == "PostSession") {
        out = HookEvent::PostSession;
    } else if (name == "PreTurn") {
        out = HookEvent::PreTurn;
    } else if (name == "PostTurn") {
        out = HookEvent::PostTurn;
    } else if (name == "PreStep") {
        out = HookEvent::PreStep;
    } else if (name == "PostStep") {
        out = HookEvent::PostStep;
    } else if (name == "PreAction" || name == "PreActionHook") {
        // Action 层别名(四层单 §4.2):升格名指向既有枚举值,配置两写法
        // 同效,wire 事件名保持 PreToolUse 不动——旧脚本零迁移。
        out = HookEvent::PreToolUse;
    } else if (name == "PostAction" || name == "PostActionHook") {
        out = HookEvent::PostToolUse;
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
        // loop 单:LoopTaskCreate 匹配 prompt 源(source),LoopTickEnd
        // 匹配 outcome。
        case HookEvent::LoopTaskCreate:
        case HookEvent::LoopTickEnd:
        // goal 单:GoalEvaluated 匹配 decision(continue/achieved/blocked/
        // needs_user),GoalPaused 匹配 reason(no_progress/evaluator_failed/
        // provider_failures/user/...),GoalCreated/GoalCompleted 匹配 state
        // 稳定串。Iteration 起止没有匹配对象。
        case HookEvent::GoalCreated:
        case HookEvent::GoalEvaluated:
        case HookEvent::GoalPaused:
        case HookEvent::GoalCompleted:
            return true;
        case HookEvent::UserPromptSubmit:
        case HookEvent::SubagentStart:
        case HookEvent::SubagentStop:
        case HookEvent::Stop:
        // loop 单:拍起止/停任务没有匹配对象。
        case HookEvent::LoopTickStart:
        case HookEvent::LoopTaskStop:
        // goal 单:iteration 起止没有匹配对象。
        case HookEvent::GoalIterationStart:
        case HookEvent::GoalIterationEnd:
        // 四层单 P2:Session/Turn/Step 分层事件没有 matcher 字段(载荷里
        // 各带 id,不按值筛)——matcher 只能缺省或 *。
        case HookEvent::PreSession:
        case HookEvent::PostSession:
        case HookEvent::PreTurn:
        case HookEvent::PostTurn:
        case HookEvent::PreStep:
        case HookEvent::PostStep:
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
        case HookEvent::LoopTaskCreate:
            // deny 创建/收紧 interval:可拦。不能扩大权限
            // (permission_decision 恒 false)。
            return {false, false, true, true};
        case HookEvent::LoopTickStart:
        case HookEvent::LoopTickEnd:
        case HookEvent::LoopTaskStop:
            // 拍已起/止:事实已发生,只能给反馈与
            // 追加上下文,不能倒回去。
            return {false, false, true, false};
        case HookEvent::GoalCreated:
        case HookEvent::GoalIterationStart:
        case HookEvent::GoalIterationEnd:
        case HookEvent::GoalEvaluated:
        case HookEvent::GoalPaused:
        case HookEvent::GoalCompleted:
            // goal 单边界:Hook 可补 additional context(下一轮的
            // GoalContext 里带上)、留审计,不可拦(事件描述的是已落账的
            // 状态变更,拦也拦不回),更不可直接写 Achieved(单子:仍要过
            // evaluator 与硬门槛)。GoalCompleted 失败不把 Achieved 改回
            // Active——can_block 恒 false 正是这条边界的类型化表达。
            return {false, false, true, false};
        case HookEvent::PreSession:
            // 四层单 §4.1:可注入会话级上下文、可否决开张(continue=false/
            // exit 2 = 不进首输入)。
            return {false, false, true, true};
        case HookEvent::PostSession:
            // 会话已关闭:只观察。advisory——硬杀(taskkill/断电)时本事件
            // 不保证必达,协议文档如实写明。
            return {false, false, false, false};
        case HookEvent::PreTurn:
            // 可否决本 Turn(不发模型);additional_context 随本轮注入。
            // "改输入"的决策权仍在 UserPromptSubmit 既有口径(P0 落差清单
            // 记录:updatedInput 暂不扩到本事件)。
            return {false, false, true, true};
        case HookEvent::PostTurn:
            // 终局只观察(要干预走下一只 Turn 的 PreTurn/PreStep)。
            return {false, false, false, false};
        case HookEvent::PreStep:
            // 可注入上下文(随本 Step 请求)、可否决本 Step——否决语义 =
            // 终止本 Turn,不是跳过继续(§4.1)。
            return {false, false, true, true};
        case HookEvent::PostStep:
            // 响应已落账:只观察(§4.1 矩阵:要干预走下一层 PreAction)。
            // additional_context 不给——终局注入会造悬空 user 消息,反馈
            // 想进模型走 PreStep/PreTurn。
            return {false, false, false, false};
    }
    return {};
}

// 四层单 P2:这只事件是不是"Post 型观察事件"——durable outbox/ack 的
// 记账范围(可靠 Post,§4.3)。判定标准:副作用/事实已发生、hook 只观察
// 记账(外加给反馈),重放语义 at-least-once 有意义的事件。决策型事件
// (Pre*、PermissionRequest、Stop、SubagentStop 续跑裁决)不入 outbox:
// 它们的重放会再造决策,不是可靠投递的账。
constexpr bool IsPostObservationEvent(HookEvent event) {
    switch (event) {
        case HookEvent::PostToolUse:   // = PostAction(别名)
        case HookEvent::PostCompact:
        case HookEvent::SessionEnd:
        case HookEvent::SubagentStop:
        case HookEvent::LoopTickEnd:
        case HookEvent::LoopTaskStop:
        case HookEvent::GoalIterationEnd:
        case HookEvent::GoalEvaluated:
        case HookEvent::GoalCompleted:
        case HookEvent::PostSession:
        case HookEvent::PostTurn:
        case HookEvent::PostStep:
            return true;
        case HookEvent::SessionStart:
        case HookEvent::UserPromptSubmit:
        case HookEvent::PreToolUse:
        case HookEvent::PermissionRequest:
        case HookEvent::PreCompact:
        case HookEvent::SubagentStart:
        case HookEvent::Stop:
        case HookEvent::LoopTaskCreate:
        case HookEvent::LoopTickStart:
        case HookEvent::GoalCreated:
        case HookEvent::GoalIterationStart:
        case HookEvent::GoalPaused:
        case HookEvent::PreSession:
        case HookEvent::PreTurn:
        case HookEvent::PreStep:
            return false;
    }
    return false;
}

}  // namespace lubancode::hooks
