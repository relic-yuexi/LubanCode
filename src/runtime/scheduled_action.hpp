// ScheduledActionResolver 与 schedulable catalog(loop 单遗留,骨架):
// scheduled prompt(非 / 开头的定时正文)解析成安全动作的表。
//
// loop 单的边界(单子"ScheduledActionResolver 与 schedulable catalog"
// 留作后续):/loop 的 prompt 不许以 / 开头(定时执行 slash 命令首版不
// 支持,/exit /clear 这类定时执行会出事);但自然语言的定时正文里有
// 一类"结构性动作"(跑测试、看 CI、报告状态)可以解析成宿主侧的安全
// 动作枚举,不必每次都发模型。本骨架只做两件事:
//   1. Catalog:SchedulableAction 枚举与安全档(哪些动作允许无人值守
//      自动执行,哪些必须走模型/用户);
//   2. Resolver:prompt 正文 -> 动作候补(关键词对齐,中文/英文都收;
//      认不出给 nullopt,调用方回落"发模型"的老路)。
// 执行体不在骨架里——动作的真执行(跑测试命令、查 CI 状态)各自有
// 权限与审批链,后续按 catalog 逐枚接。纯函数,零 IO。

#pragma once

#include <optional>
#include <string>
#include <vector>

namespace lubancode::runtime::loop {

// 可调度的安全动作(catalog)。
enum class SchedulableAction {
    RunTests,       // 跑测试套件(宿主侧命令,过 run_command 权限链)
    ReportStatus,   // 汇报会话/worktree/loop 状态(纯本地,零副作用)
    CheckCi,        // 查 CI/PR 状态(只读远端查询)
    SummarizeDiff,  // 汇总当前分支 diff(只读 git)
};

// 动作的安全档:决定宿主能不能不发模型直接执行。
enum class ActionSafety {
    LocalOnly,    // 纯本地只读,无人值守安全(ReportStatus/SummarizeDiff)
    NeedsCommand, // 要起子进程(RunTests),过 run_command 的权限链
    NeedsRemote,  // 远端查询(CheckCi),只读但有费用/限流
};

// catalog 条目:动作 + 它的安全档 + 描述(给 /loop 帮助与诊断用)。
struct SchedulableActionInfo {
    SchedulableAction action;
    ActionSafety safety;
    const char* description;
};

// 全量 catalog(顺序稳定,help/诊断列同一份)。
std::vector<SchedulableActionInfo> SchedulableActionCatalog();

// 动作的安全档查表。
ActionSafety SafetyOf(SchedulableAction action);

// Resolver:prompt 正文 -> 动作候补。命中规则:按 catalog 序扫关键词表
// (中英都收,大小写不敏感),第一枚命中即返;认不出给 nullopt——调用方
// 回落"发模型"的老路,不硬猜。空串/全空白给 nullopt。
// 这是骨架的关键保守性:resolver 只把"明确说了要做什么"的正文折成动作,
// 模糊的都交模型,不许拿关键词匹配冒充理解。
std::optional<SchedulableAction> ResolveScheduledAction(const std::string& prompt);

}  // namespace lubancode::runtime::loop
