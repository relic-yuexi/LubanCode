// 命令分派注册制(会话终章):注册表本体与路由入口。各案 handler 在
// 各域文件(commands/*.cpp),这里只登册与查表——旧 47 案 switch 的
// 路由职责收于此,案序照旧 switch 登册(枚举可对)。
#include "app/commands/command_registry.hpp"

#include <set>

#include "app/commands/agent_commands.hpp"
#include "app/commands/background_commands.hpp"
#include "app/commands/channel_commands.hpp"
#include "app/commands/doctor_commands.hpp"
#include "app/commands/evolve_commands.hpp"
#include "app/commands/goal_commands.hpp"
#include "app/commands/hook_commands.hpp"
#include "app/commands/insights_commands.hpp"
#include "app/commands/loop_commands.hpp"
#include "app/commands/memory_commands.hpp"
#include "app/commands/model_commands.hpp"
#include "app/commands/package_commands.hpp"
#include "app/commands/peer_commands.hpp"
#include "app/commands/prompt_commands.hpp"
#include "app/commands/session_commands.hpp"
#include "app/commands/telemetry_commands.hpp"  // /telemetry(端云协同可观测单 T1)
#include "app/commands/settings_commands.hpp"
#include "app/commands/trace_commands.hpp"
#include "app/commands/usage_commands.hpp"
#include "app/commands/workflow_commands.hpp"
#include "app/commands/workspace_commands.hpp"

namespace lubancode::app {

namespace {

// /plan 的分派位:正戏在 Plan 接线器(经 ctx.handle_plan_command 活口,
// 控制器/接线器递进来),这里只递参数。
CommandFlow HandleSlashPlan(SlashDispatchContext& ctx, const lubancode::cli::ParsedSlashCommand& parsed) {
    return ctx.handle_plan_command(parsed.args);
}

}  // namespace

const std::vector<SlashCommandSpec>& SlashCommandTable() {
    // 案序照旧 switch(对账时按这序数):Image 进不来分派(ProcessLine 把
    // 图片路径截走)、NotSlash 在上一层已分流——两案留名为死案。行上只有
    // 枚举与 handler:命令词汇在 cli::SlashCommandDescriptors(),这里不
    // 重写名字(HC-05)。
    static const std::vector<SlashCommandSpec> table = {
        {lubancode::cli::SlashCommand::Image, nullptr, false, false},
        {lubancode::cli::SlashCommand::Help, HandleSlashHelp, false, false},
        {lubancode::cli::SlashCommand::Model, HandleSlashModel, false, false},
        {lubancode::cli::SlashCommand::Provider, HandleSlashProvider, false, false},
        {lubancode::cli::SlashCommand::Config, HandleSlashConfig, false, false},
        {lubancode::cli::SlashCommand::Update, HandleSlashUpdate, false, false},
        {lubancode::cli::SlashCommand::Init, HandleSlashInit, false, false},
        {lubancode::cli::SlashCommand::Instructions, HandleSlashInstructions, false, false},
        {lubancode::cli::SlashCommand::Language, HandleSlashLanguage, false, false},
        {lubancode::cli::SlashCommand::Worktree, HandleSlashWorktree, false, false},
        {lubancode::cli::SlashCommand::Clear, HandleSlashClear, false, false},
        {lubancode::cli::SlashCommand::Context, HandleSlashContext, false, false},
        // ContextWindow 交互面板单:同屏调当前模型窗口与思考强度(本会话)。
        {lubancode::cli::SlashCommand::ContextWindow, HandleSlashContextWindow, false, false},
        {lubancode::cli::SlashCommand::Usage, HandleSlashUsage, false, false},
        {lubancode::cli::SlashCommand::Insights, HandleSlashInsights, false, false},
        {lubancode::cli::SlashCommand::Compact, HandleSlashCompact, false, false},
        {lubancode::cli::SlashCommand::Think, HandleSlashThink, false, false},
        {lubancode::cli::SlashCommand::Skills, HandleSlashSkills, false, false},
        {lubancode::cli::SlashCommand::Skill, HandleSlashSkill, false, false},
        {lubancode::cli::SlashCommand::Mcp, HandleSlashMcp, false, false},
        {lubancode::cli::SlashCommand::Lsp, HandleSlashLsp, false, false},
        {lubancode::cli::SlashCommand::Todos, HandleSlashTodos, false, false},
        {lubancode::cli::SlashCommand::Plugins, HandleSlashPlugins, false, false},
        {lubancode::cli::SlashCommand::Plugin, HandleSlashPlugin, false, false},
        {lubancode::cli::SlashCommand::Agents, HandleSlashAgents, false, false},
        {lubancode::cli::SlashCommand::Agent, HandleSlashAgent, false, false},
        {lubancode::cli::SlashCommand::Tools, HandleSlashTools, false, false},
        {lubancode::cli::SlashCommand::Hooks, HandleSlashHooks, false, false},
        {lubancode::cli::SlashCommand::Background, HandleSlashBackground, false, false},
        {lubancode::cli::SlashCommand::Keymap, HandleSlashKeymap, false, false},
        {lubancode::cli::SlashCommand::Plan, HandleSlashPlan, false, true},
        {lubancode::cli::SlashCommand::Package, HandleSlashPackage, false, false},
        // 多渠道消息接入单阶段 2:渠道账号面(只读 + 管理动作;普通交互
        // 进程没挂 ChannelManager 时 handler 只给 gateway 引导)。
        {lubancode::cli::SlashCommand::Channels, HandleSlashChannels, false, false},
        {lubancode::cli::SlashCommand::Channel, HandleSlashChannel, false, false},
        {lubancode::cli::SlashCommand::Evolve, HandleSlashEvolve, false, false},
        {lubancode::cli::SlashCommand::Trace, HandleSlashTrace, false, false},
        {lubancode::cli::SlashCommand::Doctor, HandleSlashDoctor, false, false},
        {lubancode::cli::SlashCommand::Telemetry, HandleSlashTelemetry, false, false},
        {lubancode::cli::SlashCommand::Goal, HandleSlashGoal, false, false},
        {lubancode::cli::SlashCommand::Loop, HandleSlashLoop, false, false},
        {lubancode::cli::SlashCommand::Memory, HandleSlashMemory, false, false},
        {lubancode::cli::SlashCommand::Record, HandleSlashRecord, true, false},
        {lubancode::cli::SlashCommand::Sessions, HandleSlashSessions, false, false},
        {lubancode::cli::SlashCommand::Archive, HandleSlashArchive, false, false},
        {lubancode::cli::SlashCommand::Delete, HandleSlashDelete, false, false},
        {lubancode::cli::SlashCommand::Resume, HandleSlashResume, false, false},
        {lubancode::cli::SlashCommand::Export, HandleSlashExport, false, false},
        {lubancode::cli::SlashCommand::Copy, HandleSlashCopy, false, false},
        {lubancode::cli::SlashCommand::Title, HandleSlashTitle, false, false},
        {lubancode::cli::SlashCommand::Soul, HandleSlashSoul, false, false},
        {lubancode::cli::SlashCommand::Prompt, HandleSlashPrompt, false, false},
        {lubancode::cli::SlashCommand::Peers, HandleSlashPeers, true, false},
        {lubancode::cli::SlashCommand::Send, HandleSlashSend, true, false},
        {lubancode::cli::SlashCommand::Peerperm, HandleSlashPeerperm, true, false},
        {lubancode::cli::SlashCommand::Workflow, HandleSlashWorkflow, false, false},
        {lubancode::cli::SlashCommand::Exit, HandleSlashExit, false, false},
        {lubancode::cli::SlashCommand::Unknown, HandleSlashUnknown, false, false},
        {lubancode::cli::SlashCommand::NotSlash, nullptr, false, false},
    };
    return table;
}

CommandFlow DispatchSessionSlashCommand(SlashDispatchContext& ctx,
                                        const lubancode::cli::ParsedSlashCommand& parsed) {
    for (const SlashCommandSpec& spec : SlashCommandTable()) {
        if (spec.command != parsed.command) {
            continue;
        }
        if (spec.handler == nullptr) {
            break;  // 死案(Image/NotSlash):旧 switch 的 break 同语义
        }
        return spec.handler(ctx, parsed);
    }
    return CommandFlow::Continue;  // switch 完备性兜底同款
}

// ---------------------------------------------------------------------------
// P0-2 TrajectoryCommandExecutor(§14.1/§15.7)
// ---------------------------------------------------------------------------

namespace {

// effect class 的粗分表(§14.2 的类别列;动作级细分——/context 裸敲与
// /context 256k 之别——随 P0-4 的注册表元数据落,P0-2 按命令名粗分)。
const char* CoarseEffectClass(const std::string& name) {
    static const std::set<std::string> kSessionState = {
        "model",     "provider", "think",  "context", "plan",       "soul",     "prompt",
        "language",  "title",    "keymap", "init",    "worktree",   "config",   "hooks",
        "compact",   "record",   "memory", "todos",   "instructions",
        "context-window"};
    static const std::set<std::string> kExternalWrite = {"skill", "plugin", "package", "send", "peerperm",
                                                         "evolve"};
    static const std::set<std::string> kSpawnRun = {"agent", "workflow", "goal", "loop", "background",
                                                    "doctor"};
    static const std::set<std::string> kSessionBoundary = {"clear", "resume", "exit", "archive", "delete"};
    // Token 账本单 A5:/insights 写派生摘要与报告(§11.2 effect=derived_write);
    // status 是只读面,粗分按命令名给执行档,报告路径的细分留给后续注册表
    // 元数据。
    if (name == "insights") {
        return "derived_write";
    }
    if (kSessionBoundary.count(name) != 0) {
        return name == "delete" ? "destructive" : "session_boundary";
    }
    if (kSpawnRun.count(name) != 0) {
        return "spawn_run";
    }
    if (kExternalWrite.count(name) != 0) {
        return "external_write";
    }
    if (kSessionState.count(name) != 0) {
        return "session_state";
    }
    return "read_only";  // help/skills/mcp/lsp/agents/tools/peers/sessions/trace/export/copy/unknown...
}

}  // namespace

CommandFlow ExecuteSessionCommand(SlashDispatchContext& ctx,
                                  const lubancode::cli::ParsedSlashCommand& parsed) {
    lubancode::runtime::TrajectorySessionLedger* ledger = ctx.trajectory;
    if (ledger == nullptr) {
        return DispatchSessionSlashCommand(ctx, parsed);  // flag 关:零变透传
    }
    // 轨迹账的命令名从词汇表主名取(HC-05:app 不再自带一份名字)。Unknown/
    // NotSlash 不是用户词汇,表上没有,落回这两个审计名。
    std::string spec_name =
        parsed.command == lubancode::cli::SlashCommand::NotSlash ? "notslash" : "unknown";
    if (const lubancode::cli::SlashCommandDescriptor* descriptor =
            lubancode::cli::FindSlashCommandDescriptor(parsed.command);
        descriptor != nullptr) {
        spec_name = descriptor->word;
    }
    // clear/resume 是跨 session 例外(§14.1):requested 落旧 main、terminal
    // 由新 main 的换账事务写(P0-3 的 SessionManager 八步/七步掌管),不走
    // 这只"同一 main stream 内 requested/terminal"的通用环。
    if (spec_name == "clear" || spec_name == "resume") {
        return DispatchSessionSlashCommand(ctx, parsed);
    }
    // requested 先 durable(§14.1:有外部写入/派生执行/session 切换时
    // 须先落账再动手),handler 跑完落 terminal。P0-2 的 handler 还没翻成
    // CommandOutcome,status 只按流转给("ok"——failed 分型随 P0-4)。
    const std::string command_id = ledger->BeginCommand(spec_name, spec_name,
                                                        CoarseEffectClass(spec_name));
    const CommandFlow flow = DispatchSessionSlashCommand(ctx, parsed);
    ledger->EndCommand(command_id, /*ok=*/true, std::string());
    return flow;
}

}  // namespace lubancode::app
