// Plan 模式(只读研究硬闸单):CollaborationMode / ModeState / ModePolicy /
// PlanDocument 的合同与纯函数。
//
// 单子定案在这里落地:
//   - 两根轴不揉成一根:CollaborationMode(Default/Plan)决定"这轮准做
//     哪类事";PermissionMode(Confirm/Auto/Yolo)决定"准做的事还要不要
//     问"。先过 Plan capability gate,再过 Hook/schema,再过 permission。
//   - 模式由宿主/UI 切,模型话术、工具结果、MCP annotation、Skill 一概
//     无权改档。Plan 拒绝的动作,Yolo 也压不过去。
//   - ModePolicy 拒绝用稳定 `mode_denied`,不冒充"工具没加载"或"用户拒绝"。
//   - 计划成品(PlanDocument)有独立 id/revision/hash 与审阅状态,与
//     todo_write 的施工清单是两码事。
//
// 依赖铁律:本头只认标准库与 nlohmann/json,不 include cli/app/agent——
// runtime 合同层最底,谁都可以拿去用,它谁都不认。模式判定是纯函数,
// RunOneTool 在 PreToolUse Hook 之前调它(单子"调用次序")。

#pragma once

#include <cstdint>
#include <optional>
#include <string>

#include <nlohmann/json.hpp>

namespace lubancode::runtime {

// ---------------------------------------------------------------------------
// 两根轴
// ---------------------------------------------------------------------------

// 协作模式:研究与设计(Plan)还是实施(Default)。与 PermissionMode 正交。
enum class CollaborationMode { Default, Plan };

// ModePolicy 拒绝的稳定错误码(分层错误码风格,不拿中文正文作机器判断)。
inline constexpr const char* kErrModeDenied = "mode.denied";
// MCP/插件等外挂工具在 Plan 里默认拒绝的细码(区分"未知来源"与"已知写
// 盘"):细码只是诊断,主码仍 mode_denied,不另立终态。
inline constexpr const char* kErrModeDeniedUnknownSource = "mode.denied.unknown_source";
inline constexpr const char* kErrModeDeniedWrite = "mode.denied.write";
inline constexpr const char* kErrModeDeniedShell = "mode.denied.shell";
inline constexpr const char* kErrModeDeniedAgentRole = "mode.denied.agent_role";

// ---------------------------------------------------------------------------
// 工具能力(Plan 硬闸的判定输入)
// ---------------------------------------------------------------------------

// 工具来源。诊断/判定用;tools::ToolSourceKind 的中立镜像(不 include
// tools/*,映射由装配层做,两侧语义一一对应)。
enum class PlanToolOrigin { Builtin, Mcp, Lsp, PluginLua, PluginNative, Agent, Ptc, Unknown };

// 一枚工具的"Plan 能力"三件套:来源、副作用档、是否宿主声明过 Plan 安全。
// plan_safe_by_default 是宿主(装配层)显式的声明,不是工具自报——工具的
// description 谁都能写,宿主的注册表才是信任根。
struct PlanToolCapability {
    PlanToolOrigin origin = PlanToolOrigin::Builtin;
    // tools::EffectClass 的镜像档(只用判断"是否写盘级副作用"那一档,
    // 远端细档这里不重复枚举)。
    bool mutating = false;  // write/edit/undo/todo/memory/workflow/record/install 这类
    bool plan_safe_by_default = false;  // 宿主声明:read/search/ask_user/web 等
    std::string name;                    // 工具名(拒绝文案用)
};

// ModePolicy 的裁定。
struct ModeVerdict {
    bool allowed = true;
    std::string code;    // 拒绝时是 kErrModeDenied* 之一;放行为空串
    std::string reason;  // 拒绝时的人话(模型与用户都看)
};

// 纯函数,可单测:Default 一概放行(Plan 闸只在 Plan 模式收紧);Plan 按
// 单子首版工具表判。input 只给 run_command(shell 子集)与 agent(角色/
// 工具面)两处用——细判放调用侧的 ClassifyPlanShell / agent 工具面检查,
// 这里只吃它们的结果。判定次序:
//   1. Default -> 放行;
//   2. 宿主声明 plan_safe -> 放行(声明不能来自工具自报,见上);
//   3. 未知来源(MCP/插件/Deferred 透传不出可信声明)-> 拒;
//   4. 写盘级(mutating)-> 拒(agent 是派发容器,装配层不按注册档的
//      InProcessUnknown 记 mutating,由 5' 的工具面判);
//   5. run_command 交调用侧细判(ClassifyPlanShell),这里按
//      capability.shell_safe 传入结论,拒绝文案带 shell_rule(命中规则);
//   5'. agent 按参数判(P2-3):内置 Explore,或 agent_tools_readonly
//      (tools.allow 全为只读工具的自定义 Agent)放行,其余拒;
//   6. 其余 builtin 只读类按白名单放行,认不得的拒(保守为纲)。
struct PlanToolInput {
    bool shell_safe = false;  // run_command 的命令串已过 Plan shell 分类器
    std::string shell_rule;   // 命中的拒绝规则(ClassifyPlanShellDetailed 给),拒绝文案用
    std::string agent_role;   // agent 工具的 agent_type(general-purpose/Explore/自定义名)
    // agent 工具面只读(P2-3):agent_type 解析出的 tools.allow 全是只读
    // 工具(或内置 Explore)。装配层算好递进来,这里不摸注册表。
    bool agent_tools_readonly = false;
};
ModeVerdict EvaluateModePolicy(CollaborationMode mode, const PlanToolCapability& capability,
                               const PlanToolInput& input = PlanToolInput{});

// 工具名白名单(Plan 放行的内置只读件)。放表里不藏 if 链,测试逐条钉。
// skill 在列(P2-3):加载技能只往模型上下文装 SKILL.md 说明,不改状态。
bool IsPlanAllowedBuiltinTool(const std::string& name);

// ---------------------------------------------------------------------------
// Plan shell 分类(独立于 Auto 档的 Safe)
// ---------------------------------------------------------------------------

// ClassifyCommand 的 Safe 只表示"auto 档可少问一句"(允许 cd/echo/一批
// 探版命令,未证明不会写工作树)。Plan 须另开一张窄表(单子"shell 不能
// 沿用 Auto 的 Safe"):PlanReadOnly / PlanUnknown / PlanMutating。
enum class PlanShellVerdict { ReadOnly, Unknown, Mutating };

// 分类结论 + 命中的拒绝规则(P2-3:拦截回执要把命中的规则打印出来)。
// verdict 为 ReadOnly 时 rule 为空;Unknown 时 rule 说明撞了哪条
// (重定向/子表达式/脚本块/环境赋值/git 子命令表外/首词表外/空命令)。
struct PlanShellClassification {
    PlanShellVerdict verdict = PlanShellVerdict::Unknown;
    std::string rule;
};

// 纯函数,可单测:command 原文 + shell 语义("powershell"/"cmd")。
// 只放:rg/findstr/Get-Content/Get-ChildItem/Select-String 等只读件、
// 只读管道件(Select-Object/Where-Object 无脚本块写法/Sort-Object/
// Format-Table 等,与 command_safety 既有 Safe 分档同源)、git 只读子命令
// (status/log/diff/show/ls-files/ls-tree/rev-parse/blame 等)、pwd/
// Get-Location、Get-Item/Test-Path、无重定向无子表达式无环境赋值的版本
// 探针。段内有重定向/子表达式/PowerShell 脚本块 { }/环境赋值一律不下
// ReadOnly;真有副作用的命令照拒(真机实测 P2-3)。
PlanShellVerdict ClassifyPlanShell(const std::string& command, const std::string& shell);

// 同上,带拒绝规则(拒绝回执打印用;测试逐条钉规则文案)。
PlanShellClassification ClassifyPlanShellDetailed(const std::string& command, const std::string& shell);

// ---------------------------------------------------------------------------
// ModeState(会话侧的两轴真值)
// ---------------------------------------------------------------------------

// 归 SessionRuntime 所有(单子:"不塞进 LineEditor 的 ConfirmMode 原子")。
// execution_mode_before_plan 记"进 Plan 前用户原本的确认档",离开 Plan
// 不重置用户原有档;批准框选的新档只改本 session。
struct ModeState {
    CollaborationMode active = CollaborationMode::Default;
    // 进 Plan 那一刻的确认档快照(restore 用;Default 档下无意义)。
    // 用字符串存("confirm"/"auto"/"yolo")——本头不引 runtime::PermissionMode
    // 之外的第二套枚举,装配层翻译。
    std::string permission_before_plan;
    std::optional<std::string> latest_plan_id;  // 最近一份 PlanDocument 的 id
    std::uint64_t revision = 0;                 // 模式切换计数(session 事件行用)
};

// ---------------------------------------------------------------------------
// PlanDocument(计划成品)
// ---------------------------------------------------------------------------

enum class PlanReviewState { Draft, Presented, Approved, Rejected, Superseded };

std::string ToString(CollaborationMode mode);
bool ParseCollaborationMode(const std::string& s, CollaborationMode& out);
std::string ToString(PlanReviewState state);
bool ParsePlanReviewState(const std::string& s, PlanReviewState& out);

// 一份计划成品。markdown 有字节上限(超限走 artifact,item 留 hash/引用,
// 这里只存引用路径);content_sha256 是"用户审的是哪一稿"的锚,批准必须
// 同时匹配 id/revision/hash。
struct PlanDocument {
    std::string plan_id;      // "plan-<n>",会话内单调
    std::uint64_t revision = 1;  // 同一 plan_id 的第几稿(新稿 supersede 旧稿)
    std::string markdown;
    std::string source_turn_id;
    PlanReviewState state = PlanReviewState::Draft;
    std::string content_sha256;  // markdown 的 SHA-256 十六进制
    // 超限落仓时的 artifact 引用;空 = 正文内联在 session 事件行里。
    std::string artifact_ref;

    bool valid() const { return !plan_id.empty() && !content_sha256.empty(); }
};

// ---------------------------------------------------------------------------
// <proposed_plan> 流式解析
// ---------------------------------------------------------------------------

// 一轮 assistant 正文里扫出的计划候选:恰好一对完整标签才算数;半截、
// 嵌套、代码块里的同名文本、一轮两份都按普通 text(单子"计划成品与审阅")。
struct ProposedPlanScan {
    bool found = false;         // 恰好一份完整计划
    std::string markdown;       // 标签内的 Markdown(tag 外正文不算)
    bool ambiguous = false;     // 开标签出现两次及以上(嵌套/两份):不弹审批
    bool truncated = false;     // 有开标签没有完整闭合(流式中途/半截)
};

// 纯函数,可单测:扫一段 assistant 正文(text 是本轮累积的全部正文,不是
// 增量)。判定规矩:
//   - 代码块围栏(``` 或 ~~~ 行)内的 <proposed_plan> 不触发——围栏状态机
//     走一遍,围栏里的标签当普通文本;
//   - 恰好一对完整 <proposed_plan>...</proposed_plan> 才 found;
//   - 开标签出现多次(嵌套/两份)置 ambiguous,不算 found;
//   - 只有开标签没有闭合置 truncated(流式中途的正常态;收口时仍 truncated
//     就当普通 text,不弹审批)。
ProposedPlanScan ScanProposedPlan(const std::string& text);

// PlanDocument markdown 的内联字节上限:超限不塞 session 事件行,先落
// artifact,事件行留 artifact_ref 与 hash(单子"大稿接 artifact")。
inline constexpr std::uint64_t kPlanMarkdownInlineCap = 64 * 1024;

}  // namespace lubancode::runtime
