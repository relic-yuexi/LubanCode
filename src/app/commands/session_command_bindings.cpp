// HC-06(把Slash命令材料收窄到各自领域)第一小批:命令绑定单元的实现。
// 表本体自 command_registry.cpp 原样搬来(案序/注释随行),只换绑法:
// 已收窄域(Trace/Hook/Telemetry)的行捕获窄材料,其余域的行包装
// SlashDispatchContext 上的既有分派位——行为零变,换的只是材料怎么递。
#include "app/commands/session_command_bindings.hpp"

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
#include "app/commands/settings_commands.hpp"
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

std::vector<SlashCommandSpec> BuildSessionSlashCommandTable(const SessionCommandMaterials& materials) {
    // 案序照旧 switch(对账时按这序数):Image 进不来分派(ProcessLine 把
    // 图片路径截走)、NotSlash 在上一层已分流——两案留名为死案。行上只有
    // 枚举与执行器:命令词汇在 cli::SlashCommandDescriptors(),这里不
    // 重写名字(HC-05);材料在绑定期闭包捕获(HC-06)。
    std::vector<SlashCommandSpec> table;
    table.reserve(58);

    // 过渡域:尚未收窄的 handler 仍吃 SlashDispatchContext,这里包一层。
    SlashDispatchContext* const ctx = materials.dispatch;
    const auto via_dispatch = [ctx](CommandFlow (*handler)(SlashDispatchContext&,
                                                          const lubancode::cli::ParsedSlashCommand&)) {
        return SlashCommandHandler{
            [ctx, handler](const lubancode::cli::ParsedSlashCommand& parsed) { return handler(*ctx, parsed); }};
    };
    // 已收窄域(HC-06 第一小批:Trace/Hook/Telemetry;第二小批:
    // Model/Memory/Usage/Package):窄材料按值捕获(全是指针/回调,浅拷贝
    // 与旧路读同一批借用)。
    const TraceCommandContext trace = materials.trace;
    const HookCommandContext hook = materials.hook;
    const TelemetryCommandContext telemetry = materials.telemetry;
    const ModelCommandContext model = materials.model;
    const MemoryCommandContext memory = materials.memory;
    const UsageCommandContext usage = materials.usage;
    const PackageCommandContext package = materials.package;

    table.push_back({lubancode::cli::SlashCommand::Image, SlashCommandHandler{}, false, false});
    table.push_back({lubancode::cli::SlashCommand::Help, via_dispatch(HandleSlashHelp), false, false});
    // HC-06 已收窄(第二小批):/model 只吃窄材料。
    table.push_back({lubancode::cli::SlashCommand::Model,
                     SlashCommandHandler{[model](const lubancode::cli::ParsedSlashCommand& parsed) {
                         return HandleSlashModel(model, parsed);
                     }},
                     false, false});
    table.push_back({lubancode::cli::SlashCommand::Provider, via_dispatch(HandleSlashProvider), false, false});
    table.push_back({lubancode::cli::SlashCommand::Config, via_dispatch(HandleSlashConfig), false, false});
    table.push_back({lubancode::cli::SlashCommand::Update, via_dispatch(HandleSlashUpdate), false, false});
    table.push_back({lubancode::cli::SlashCommand::Init, via_dispatch(HandleSlashInit), false, false});
    table.push_back(
        {lubancode::cli::SlashCommand::Instructions, via_dispatch(HandleSlashInstructions), false, false});
    table.push_back({lubancode::cli::SlashCommand::Language, via_dispatch(HandleSlashLanguage), false, false});
    table.push_back({lubancode::cli::SlashCommand::Worktree, via_dispatch(HandleSlashWorktree), false, false});
    table.push_back({lubancode::cli::SlashCommand::Clear, via_dispatch(HandleSlashClear), false, false});
    table.push_back({lubancode::cli::SlashCommand::Context, via_dispatch(HandleSlashContext), false, false});
    // ContextWindow 交互面板单:同屏调当前模型窗口与思考强度(本会话)。
    table.push_back(
        {lubancode::cli::SlashCommand::ContextWindow, via_dispatch(HandleSlashContextWindow), false, false});
    // HC-06 已收窄(第二小批):/usage 只吃窄材料。
    table.push_back({lubancode::cli::SlashCommand::Usage,
                     SlashCommandHandler{[usage](const lubancode::cli::ParsedSlashCommand& parsed) {
                         return HandleSlashUsage(usage, parsed);
                     }},
                     false, false});
    table.push_back({lubancode::cli::SlashCommand::Insights, via_dispatch(HandleSlashInsights), false, false});
    table.push_back({lubancode::cli::SlashCommand::Compact, via_dispatch(HandleSlashCompact), false, false});
    table.push_back({lubancode::cli::SlashCommand::Think, via_dispatch(HandleSlashThink), false, false});
    table.push_back({lubancode::cli::SlashCommand::Skills, via_dispatch(HandleSlashSkills), false, false});
    table.push_back({lubancode::cli::SlashCommand::Skill, via_dispatch(HandleSlashSkill), false, false});
    table.push_back({lubancode::cli::SlashCommand::Mcp, via_dispatch(HandleSlashMcp), false, false});
    table.push_back({lubancode::cli::SlashCommand::Lsp, via_dispatch(HandleSlashLsp), false, false});
    table.push_back({lubancode::cli::SlashCommand::Todos, via_dispatch(HandleSlashTodos), false, false});
    table.push_back({lubancode::cli::SlashCommand::Plugins, via_dispatch(HandleSlashPlugins), false, false});
    table.push_back({lubancode::cli::SlashCommand::Plugin, via_dispatch(HandleSlashPlugin), false, false});
    table.push_back({lubancode::cli::SlashCommand::Agents, via_dispatch(HandleSlashAgents), false, false});
    table.push_back({lubancode::cli::SlashCommand::Agent, via_dispatch(HandleSlashAgent), false, false});
    table.push_back({lubancode::cli::SlashCommand::Tools, via_dispatch(HandleSlashTools), false, false});
    // HC-06 已收窄:/hooks 只吃窄材料。
    table.push_back({lubancode::cli::SlashCommand::Hooks,
                     SlashCommandHandler{[hook](const lubancode::cli::ParsedSlashCommand& parsed) {
                         return HandleSlashHooks(hook, parsed);
                     }},
                     false, false});
    table.push_back({lubancode::cli::SlashCommand::Background, via_dispatch(HandleSlashBackground), false, false});
    table.push_back({lubancode::cli::SlashCommand::Keymap, via_dispatch(HandleSlashKeymap), false, false});
    table.push_back({lubancode::cli::SlashCommand::Plan,
                     SlashCommandHandler{[ctx](const lubancode::cli::ParsedSlashCommand& parsed) {
                         return HandleSlashPlan(*ctx, parsed);
                     }},
                     false, true});
    // HC-06 已收窄(第二小批):/package 只吃窄材料。
    table.push_back({lubancode::cli::SlashCommand::Package,
                     SlashCommandHandler{[package](const lubancode::cli::ParsedSlashCommand& parsed) {
                         return HandleSlashPackage(package, parsed);
                     }},
                     false, false});
    // 多渠道消息接入单阶段 2:渠道账号面(只读 + 管理动作;普通交互
    // 进程没挂 ChannelManager 时 handler 只给 gateway 引导)。
    table.push_back({lubancode::cli::SlashCommand::Channels, via_dispatch(HandleSlashChannels), false, false});
    table.push_back({lubancode::cli::SlashCommand::Channel, via_dispatch(HandleSlashChannel), false, false});
    table.push_back({lubancode::cli::SlashCommand::Evolve, via_dispatch(HandleSlashEvolve), false, false});
    // HC-06 已收窄:/trace 只吃窄材料。
    table.push_back({lubancode::cli::SlashCommand::Trace,
                     SlashCommandHandler{[trace](const lubancode::cli::ParsedSlashCommand& parsed) {
                         return HandleSlashTrace(trace, parsed);
                     }},
                     false, false});
    table.push_back({lubancode::cli::SlashCommand::Doctor, via_dispatch(HandleSlashDoctor), false, false});
    // HC-06 已收窄:/telemetry 只吃窄材料。
    table.push_back({lubancode::cli::SlashCommand::Telemetry,
                     SlashCommandHandler{[telemetry](const lubancode::cli::ParsedSlashCommand& parsed) {
                         return HandleSlashTelemetry(telemetry, parsed);
                     }},
                     false, false});
    table.push_back({lubancode::cli::SlashCommand::Goal, via_dispatch(HandleSlashGoal), false, false});
    table.push_back({lubancode::cli::SlashCommand::Loop, via_dispatch(HandleSlashLoop), false, false});
    // HC-06 已收窄(第二小批):/memory 只吃窄材料。
    table.push_back({lubancode::cli::SlashCommand::Memory,
                     SlashCommandHandler{[memory](const lubancode::cli::ParsedSlashCommand& parsed) {
                         return HandleSlashMemory(memory, parsed);
                     }},
                     false, false});
    table.push_back({lubancode::cli::SlashCommand::Record, via_dispatch(HandleSlashRecord), true, false});
    table.push_back({lubancode::cli::SlashCommand::Sessions, via_dispatch(HandleSlashSessions), false, false});
    table.push_back({lubancode::cli::SlashCommand::Archive, via_dispatch(HandleSlashArchive), false, false});
    table.push_back({lubancode::cli::SlashCommand::Delete, via_dispatch(HandleSlashDelete), false, false});
    table.push_back({lubancode::cli::SlashCommand::Resume, via_dispatch(HandleSlashResume), false, false});
    table.push_back({lubancode::cli::SlashCommand::Export, via_dispatch(HandleSlashExport), false, false});
    table.push_back({lubancode::cli::SlashCommand::Copy, via_dispatch(HandleSlashCopy), false, false});
    table.push_back({lubancode::cli::SlashCommand::Title, via_dispatch(HandleSlashTitle), false, false});
    table.push_back({lubancode::cli::SlashCommand::Soul, via_dispatch(HandleSlashSoul), false, false});
    table.push_back({lubancode::cli::SlashCommand::Prompt, via_dispatch(HandleSlashPrompt), false, false});
    table.push_back({lubancode::cli::SlashCommand::Peers, via_dispatch(HandleSlashPeers), true, false});
    table.push_back({lubancode::cli::SlashCommand::Send, via_dispatch(HandleSlashSend), true, false});
    table.push_back({lubancode::cli::SlashCommand::Peerperm, via_dispatch(HandleSlashPeerperm), true, false});
    table.push_back({lubancode::cli::SlashCommand::Workflow, via_dispatch(HandleSlashWorkflow), false, false});
    table.push_back({lubancode::cli::SlashCommand::Exit, via_dispatch(HandleSlashExit), false, false});
    table.push_back({lubancode::cli::SlashCommand::Unknown, via_dispatch(HandleSlashUnknown), false, false});
    table.push_back({lubancode::cli::SlashCommand::NotSlash, SlashCommandHandler{}, false, false});
    return table;
}

}  // namespace lubancode::app
