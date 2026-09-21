// HC-06(把Slash命令材料收窄到各自领域)命令绑定单元的实现。表本体自
// command_registry.cpp 原样搬来(案序/注释随行),只换绑法:已收窄域的行
// 捕获窄材料,过渡域(/insights、/prompt——FD-06 占件)的行包装
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
#include "app/commands/telemetry_commands.hpp"
#include "app/commands/trace_commands.hpp"
#include "app/commands/usage_commands.hpp"
#include "app/commands/workflow_commands.hpp"
#include "app/commands/workspace_commands.hpp"

namespace lubancode::app {

std::vector<SlashCommandSpec> BuildSessionSlashCommandTable(const SessionCommandMaterials& materials) {
    // 案序照旧 switch(对账时按这序数):Image 进不来分派(ProcessLine 把
    // 图片路径截走)、NotSlash 在上一层已分流——两案留名为死案。行上只有
    // 枚举与执行器:命令词汇在 cli::SlashCommandDescriptors(),这里不
    // 重写名字(HC-05);材料在绑定期闭包捕获(HC-06)。
    std::vector<SlashCommandSpec> table;
    table.reserve(58);

    // 过渡域:/insights 与 /prompt(audit 委托在 FD-06 占件里)的 handler
    // 仍吃 SlashDispatchContext,这里包一层;FD-06 合入迁走后删。
    SlashDispatchContext* const ctx = materials.dispatch;
    const auto via_dispatch = [ctx](CommandFlow (*handler)(SlashDispatchContext&,
                                                          const lubancode::cli::ParsedSlashCommand&)) {
        return SlashCommandHandler{
            [ctx, handler](const lubancode::cli::ParsedSlashCommand& parsed) { return handler(*ctx, parsed); }};
    };
    // 已收窄域:窄材料按值捕获(全是指针/回调,浅拷贝与旧路读同一批借用)。
    const TraceCommandContext trace = materials.trace;
    const HookCommandContext hook = materials.hook;
    const TelemetryCommandContext telemetry = materials.telemetry;
    const ModelCommandContext model = materials.model;
    const MemoryCommandContext memory = materials.memory;
    const UsageCommandContext usage = materials.usage;
    const PackageCommandContext package = materials.package;
    const SessionQueryContext session_query = materials.session_query;
    const SessionRunContext session_run = materials.session_run;
    const SessionLifecycleContext session_lifecycle = materials.session_lifecycle;
    const SettingsCommandContext settings = materials.settings;
    const WorkspaceCommandContext workspace = materials.workspace;
    const DoctorCommandContext doctor = materials.doctor;
    const AgentCommandContext agent = materials.agent;
    const PeerCommandContext peer = materials.peer;
    const BackgroundCommandContext background = materials.background;
    const ChannelCommandContext channel = materials.channel;
    const EvolveCommandContext evolve = materials.evolve;
    const GoalCommandContext goal = materials.goal;
    const LoopCommandContext loop = materials.loop;
    const WorkflowDispatchContext workflow = materials.workflow;

    table.push_back({lubancode::cli::SlashCommand::Image, SlashCommandHandler{}, false, false});
    table.push_back({lubancode::cli::SlashCommand::Help,
                     SlashCommandHandler{[session_lifecycle](const lubancode::cli::ParsedSlashCommand& parsed) {
                         return HandleSlashHelp(session_lifecycle, parsed);
                     }},
                     false, false});
    // HC-06 已收窄(第二小批):/model 只吃窄材料。
    table.push_back({lubancode::cli::SlashCommand::Model,
                     SlashCommandHandler{[model](const lubancode::cli::ParsedSlashCommand& parsed) {
                         return HandleSlashModel(model, parsed);
                     }},
                     false, false});
    table.push_back({lubancode::cli::SlashCommand::Provider,
                     SlashCommandHandler{[settings](const lubancode::cli::ParsedSlashCommand& parsed) {
                         return HandleSlashProvider(settings, parsed);
                     }},
                     false, false});
    table.push_back({lubancode::cli::SlashCommand::Config,
                     SlashCommandHandler{[settings](const lubancode::cli::ParsedSlashCommand& parsed) {
                         return HandleSlashConfig(settings, parsed);
                     }},
                     false, false});
    table.push_back({lubancode::cli::SlashCommand::Update,
                     SlashCommandHandler{[settings](const lubancode::cli::ParsedSlashCommand& parsed) {
                         return HandleSlashUpdate(settings, parsed);
                     }},
                     false, false});
    table.push_back({lubancode::cli::SlashCommand::Init,
                     SlashCommandHandler{[workspace](const lubancode::cli::ParsedSlashCommand& parsed) {
                         return HandleSlashInit(workspace, parsed);
                     }},
                     false, false});
    table.push_back(
        {lubancode::cli::SlashCommand::Instructions,
         SlashCommandHandler{[workspace](const lubancode::cli::ParsedSlashCommand& parsed) {
             return HandleSlashInstructions(workspace, parsed);
         }},
         false, false});
    table.push_back({lubancode::cli::SlashCommand::Language,
                     SlashCommandHandler{[settings](const lubancode::cli::ParsedSlashCommand& parsed) {
                         return HandleSlashLanguage(settings, parsed);
                     }},
                     false, false});
    table.push_back({lubancode::cli::SlashCommand::Worktree,
                     SlashCommandHandler{[workspace](const lubancode::cli::ParsedSlashCommand& parsed) {
                         return HandleSlashWorktree(workspace, parsed);
                     }},
                     false, false});
    table.push_back({lubancode::cli::SlashCommand::Clear,
                     SlashCommandHandler{[session_lifecycle](const lubancode::cli::ParsedSlashCommand& parsed) {
                         return HandleSlashClear(session_lifecycle, parsed);
                     }},
                     false, false});
    table.push_back({lubancode::cli::SlashCommand::Context,
                     SlashCommandHandler{[session_query](const lubancode::cli::ParsedSlashCommand& parsed) {
                         return HandleSlashContext(session_query, parsed);
                     }},
                     false, false});
    // ContextWindow 交互面板单:同屏调当前模型窗口与思考强度(本会话)。
    table.push_back(
        {lubancode::cli::SlashCommand::ContextWindow,
         SlashCommandHandler{[session_query](const lubancode::cli::ParsedSlashCommand& parsed) {
             return HandleSlashContextWindow(session_query, parsed);
         }},
         false, false});
    // HC-06 已收窄(第二小批):/usage 只吃窄材料。
    table.push_back({lubancode::cli::SlashCommand::Usage,
                     SlashCommandHandler{[usage](const lubancode::cli::ParsedSlashCommand& parsed) {
                         return HandleSlashUsage(usage, parsed);
                     }},
                     false, false});
    // FD-06 占件:过渡域仍经 SlashDispatchContext。
    table.push_back({lubancode::cli::SlashCommand::Insights, via_dispatch(HandleSlashInsights), false, false});
    table.push_back({lubancode::cli::SlashCommand::Compact,
                     SlashCommandHandler{[session_run](const lubancode::cli::ParsedSlashCommand& parsed) {
                         return HandleSlashCompact(session_run, parsed);
                     }},
                     false, false});
    table.push_back({lubancode::cli::SlashCommand::Think,
                     SlashCommandHandler{[settings](const lubancode::cli::ParsedSlashCommand& parsed) {
                         return HandleSlashThink(settings, parsed);
                     }},
                     false, false});
    table.push_back({lubancode::cli::SlashCommand::Skills,
                     SlashCommandHandler{[settings](const lubancode::cli::ParsedSlashCommand& parsed) {
                         return HandleSlashSkills(settings, parsed);
                     }},
                     false, false});
    table.push_back({lubancode::cli::SlashCommand::Skill,
                     SlashCommandHandler{[settings](const lubancode::cli::ParsedSlashCommand& parsed) {
                         return HandleSlashSkill(settings, parsed);
                     }},
                     false, false});
    table.push_back({lubancode::cli::SlashCommand::Mcp,
                     SlashCommandHandler{[workspace](const lubancode::cli::ParsedSlashCommand& parsed) {
                         return HandleSlashMcp(workspace, parsed);
                     }},
                     false, false});
    table.push_back({lubancode::cli::SlashCommand::Lsp,
                     SlashCommandHandler{[workspace](const lubancode::cli::ParsedSlashCommand& parsed) {
                         return HandleSlashLsp(workspace, parsed);
                     }},
                     false, false});
    table.push_back({lubancode::cli::SlashCommand::Todos,
                     SlashCommandHandler{[workspace](const lubancode::cli::ParsedSlashCommand& parsed) {
                         return HandleSlashTodos(workspace, parsed);
                     }},
                     false, false});
    table.push_back({lubancode::cli::SlashCommand::Plugins,
                     SlashCommandHandler{[workspace](const lubancode::cli::ParsedSlashCommand& parsed) {
                         return HandleSlashPlugins(workspace, parsed);
                     }},
                     false, false});
    table.push_back({lubancode::cli::SlashCommand::Plugin,
                     SlashCommandHandler{[workspace](const lubancode::cli::ParsedSlashCommand& parsed) {
                         return HandleSlashPlugin(workspace, parsed);
                     }},
                     false, false});
    table.push_back({lubancode::cli::SlashCommand::Agents,
                     SlashCommandHandler{[agent](const lubancode::cli::ParsedSlashCommand& parsed) {
                         return HandleSlashAgents(agent, parsed);
                     }},
                     false, false});
    table.push_back({lubancode::cli::SlashCommand::Agent,
                     SlashCommandHandler{[agent](const lubancode::cli::ParsedSlashCommand& parsed) {
                         return HandleSlashAgent(agent, parsed);
                     }},
                     false, false});
    table.push_back({lubancode::cli::SlashCommand::Tools,
                     SlashCommandHandler{[workspace](const lubancode::cli::ParsedSlashCommand& parsed) {
                         return HandleSlashTools(workspace, parsed);
                     }},
                     false, false});
    // HC-06 已收窄:/hooks 只吃窄材料。
    table.push_back({lubancode::cli::SlashCommand::Hooks,
                     SlashCommandHandler{[hook](const lubancode::cli::ParsedSlashCommand& parsed) {
                         return HandleSlashHooks(hook, parsed);
                     }},
                     false, false});
    table.push_back({lubancode::cli::SlashCommand::Background,
                     SlashCommandHandler{[background](const lubancode::cli::ParsedSlashCommand& parsed) {
                         return HandleSlashBackground(background, parsed);
                     }},
                     false, false});
    table.push_back({lubancode::cli::SlashCommand::Keymap,
                     SlashCommandHandler{[settings](const lubancode::cli::ParsedSlashCommand& parsed) {
                         return HandleSlashKeymap(settings, parsed);
                     }},
                     false, false});
    // /plan 的分派位:正戏在 Plan 接线器(经 lifecycle.handle_plan_command
    // 活口,控制器/接线器递进来),这里只递参数。
    table.push_back({lubancode::cli::SlashCommand::Plan,
                     SlashCommandHandler{[session_lifecycle](const lubancode::cli::ParsedSlashCommand& parsed) {
                         return session_lifecycle.handle_plan_command(parsed.args);
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
    table.push_back({lubancode::cli::SlashCommand::Channels,
                     SlashCommandHandler{[channel](const lubancode::cli::ParsedSlashCommand& parsed) {
                         return HandleSlashChannels(channel, parsed);
                     }},
                     false, false});
    table.push_back({lubancode::cli::SlashCommand::Channel,
                     SlashCommandHandler{[channel](const lubancode::cli::ParsedSlashCommand& parsed) {
                         return HandleSlashChannel(channel, parsed);
                     }},
                     false, false});
    table.push_back({lubancode::cli::SlashCommand::Evolve,
                     SlashCommandHandler{[evolve](const lubancode::cli::ParsedSlashCommand& parsed) {
                         return HandleSlashEvolve(evolve, parsed);
                     }},
                     false, false});
    // HC-06 已收窄:/trace 只吃窄材料。
    table.push_back({lubancode::cli::SlashCommand::Trace,
                     SlashCommandHandler{[trace](const lubancode::cli::ParsedSlashCommand& parsed) {
                         return HandleSlashTrace(trace, parsed);
                     }},
                     false, false});
    table.push_back({lubancode::cli::SlashCommand::Doctor,
                     SlashCommandHandler{[doctor](const lubancode::cli::ParsedSlashCommand& parsed) {
                         return HandleSlashDoctor(doctor, parsed);
                     }},
                     false, false});
    // HC-06 已收窄:/telemetry 只吃窄材料。
    table.push_back({lubancode::cli::SlashCommand::Telemetry,
                     SlashCommandHandler{[telemetry](const lubancode::cli::ParsedSlashCommand& parsed) {
                         return HandleSlashTelemetry(telemetry, parsed);
                     }},
                     false, false});
    table.push_back({lubancode::cli::SlashCommand::Goal,
                     SlashCommandHandler{[goal](const lubancode::cli::ParsedSlashCommand& parsed) {
                         return HandleSlashGoal(goal, parsed);
                     }},
                     false, false});
    table.push_back({lubancode::cli::SlashCommand::Loop,
                     SlashCommandHandler{[loop](const lubancode::cli::ParsedSlashCommand& parsed) {
                         return HandleSlashLoop(loop, parsed);
                     }},
                     false, false});
    // HC-06 已收窄(第二小批):/memory 只吃窄材料。
    table.push_back({lubancode::cli::SlashCommand::Memory,
                     SlashCommandHandler{[memory](const lubancode::cli::ParsedSlashCommand& parsed) {
                         return HandleSlashMemory(memory, parsed);
                     }},
                     false, false});
    table.push_back({lubancode::cli::SlashCommand::Record,
                     SlashCommandHandler{[session_run](const lubancode::cli::ParsedSlashCommand& parsed) {
                         return HandleSlashRecord(session_run, parsed);
                     }},
                     true, false});
    table.push_back({lubancode::cli::SlashCommand::Sessions,
                     SlashCommandHandler{[session_lifecycle](const lubancode::cli::ParsedSlashCommand& parsed) {
                         return HandleSlashSessions(session_lifecycle, parsed);
                     }},
                     false, false});
    table.push_back({lubancode::cli::SlashCommand::Archive,
                     SlashCommandHandler{[session_lifecycle](const lubancode::cli::ParsedSlashCommand& parsed) {
                         return HandleSlashArchive(session_lifecycle, parsed);
                     }},
                     false, false});
    table.push_back({lubancode::cli::SlashCommand::Delete,
                     SlashCommandHandler{[session_lifecycle](const lubancode::cli::ParsedSlashCommand& parsed) {
                         return HandleSlashDelete(session_lifecycle, parsed);
                     }},
                     false, false});
    table.push_back({lubancode::cli::SlashCommand::Resume,
                     SlashCommandHandler{[session_lifecycle](const lubancode::cli::ParsedSlashCommand& parsed) {
                         return HandleSlashResume(session_lifecycle, parsed);
                     }},
                     false, false});
    table.push_back({lubancode::cli::SlashCommand::Export,
                     SlashCommandHandler{[session_lifecycle](const lubancode::cli::ParsedSlashCommand& parsed) {
                         return HandleSlashExport(session_lifecycle, parsed);
                     }},
                     false, false});
    table.push_back({lubancode::cli::SlashCommand::Copy,
                     SlashCommandHandler{[settings](const lubancode::cli::ParsedSlashCommand& parsed) {
                         return HandleSlashCopy(settings, parsed);
                     }},
                     false, false});
    table.push_back({lubancode::cli::SlashCommand::Title,
                     SlashCommandHandler{[session_lifecycle](const lubancode::cli::ParsedSlashCommand& parsed) {
                         return HandleSlashTitle(session_lifecycle, parsed);
                     }},
                     false, false});
    // FD-06 占件:/soul 与 /prompt 同文件(audit 委托),整域留在过渡袋。
    table.push_back({lubancode::cli::SlashCommand::Soul, via_dispatch(HandleSlashSoul), false, false});
    table.push_back({lubancode::cli::SlashCommand::Prompt, via_dispatch(HandleSlashPrompt), false, false});
    table.push_back({lubancode::cli::SlashCommand::Peers,
                     SlashCommandHandler{[peer](const lubancode::cli::ParsedSlashCommand& parsed) {
                         return HandleSlashPeers(peer, parsed);
                     }},
                     true, false});
    table.push_back({lubancode::cli::SlashCommand::Send,
                     SlashCommandHandler{[peer](const lubancode::cli::ParsedSlashCommand& parsed) {
                         return HandleSlashSend(peer, parsed);
                     }},
                     true, false});
    table.push_back({lubancode::cli::SlashCommand::Peerperm,
                     SlashCommandHandler{[peer](const lubancode::cli::ParsedSlashCommand& parsed) {
                         return HandleSlashPeerperm(peer, parsed);
                     }},
                     true, false});
    table.push_back({lubancode::cli::SlashCommand::Workflow,
                     SlashCommandHandler{[workflow](const lubancode::cli::ParsedSlashCommand& parsed) {
                         return HandleSlashWorkflow(workflow, parsed);
                     }},
                     false, false});
    table.push_back({lubancode::cli::SlashCommand::Exit,
                     SlashCommandHandler{[session_lifecycle](const lubancode::cli::ParsedSlashCommand& parsed) {
                         return HandleSlashExit(session_lifecycle, parsed);
                     }},
                     false, false});
    table.push_back({lubancode::cli::SlashCommand::Unknown,
                     SlashCommandHandler{[workflow](const lubancode::cli::ParsedSlashCommand& parsed) {
                         return HandleSlashUnknown(workflow, parsed);
                     }},
                     false, false});
    table.push_back({lubancode::cli::SlashCommand::NotSlash, SlashCommandHandler{}, false, false});
    return table;
}

}  // namespace lubancode::app
