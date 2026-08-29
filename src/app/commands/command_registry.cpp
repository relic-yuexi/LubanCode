// 命令分派注册制(会话终章):注册表本体与路由入口。各案 handler 在
// 各域文件(commands/*.cpp),这里只登册与查表——旧 47 案 switch 的
// 路由职责收于此,案序照旧 switch 登册(枚举可对)。
#include "app/commands/command_registry.hpp"

#include "app/commands/agent_commands.hpp"
#include "app/commands/background_commands.hpp"
#include "app/commands/doctor_commands.hpp"
#include "app/commands/evolve_commands.hpp"
#include "app/commands/goal_commands.hpp"
#include "app/commands/hook_commands.hpp"
#include "app/commands/loop_commands.hpp"
#include "app/commands/memory_commands.hpp"
#include "app/commands/model_commands.hpp"
#include "app/commands/package_commands.hpp"
#include "app/commands/peer_commands.hpp"
#include "app/commands/prompt_commands.hpp"
#include "app/commands/session_commands.hpp"
#include "app/commands/settings_commands.hpp"
#include "app/commands/trace_commands.hpp"
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
    // 图片路径截走)、NotSlash 在上一层已分流——两案留名为死案。
    static const std::vector<SlashCommandSpec> table = {
        {lubancode::cli::SlashCommand::Image, "image", nullptr, false, false},
        {lubancode::cli::SlashCommand::Help, "help", HandleSlashHelp, false, false},
        {lubancode::cli::SlashCommand::Model, "model", HandleSlashModel, false, false},
        {lubancode::cli::SlashCommand::Provider, "provider", HandleSlashProvider, false, false},
        {lubancode::cli::SlashCommand::Config, "config", HandleSlashConfig, false, false},
        {lubancode::cli::SlashCommand::Update, "update", HandleSlashUpdate, false, false},
        {lubancode::cli::SlashCommand::Init, "init", HandleSlashInit, false, false},
        {lubancode::cli::SlashCommand::Language, "language", HandleSlashLanguage, false, false},
        {lubancode::cli::SlashCommand::Worktree, "worktree", HandleSlashWorktree, false, false},
        {lubancode::cli::SlashCommand::Clear, "clear", HandleSlashClear, false, false},
        {lubancode::cli::SlashCommand::Context, "context", HandleSlashContext, false, false},
        {lubancode::cli::SlashCommand::Compact, "compact", HandleSlashCompact, false, false},
        {lubancode::cli::SlashCommand::Think, "think", HandleSlashThink, false, false},
        {lubancode::cli::SlashCommand::Skills, "skills", HandleSlashSkills, false, false},
        {lubancode::cli::SlashCommand::Skill, "skill", HandleSlashSkill, false, false},
        {lubancode::cli::SlashCommand::Mcp, "mcp", HandleSlashMcp, false, false},
        {lubancode::cli::SlashCommand::Lsp, "lsp", HandleSlashLsp, false, false},
        {lubancode::cli::SlashCommand::Todos, "todos", HandleSlashTodos, false, false},
        {lubancode::cli::SlashCommand::Plugins, "plugins", HandleSlashPlugins, false, false},
        {lubancode::cli::SlashCommand::Plugin, "plugin", HandleSlashPlugin, false, false},
        {lubancode::cli::SlashCommand::Agents, "agents", HandleSlashAgents, false, false},
        {lubancode::cli::SlashCommand::Agent, "agent", HandleSlashAgent, false, false},
        {lubancode::cli::SlashCommand::Tools, "tools", HandleSlashTools, false, false},
        {lubancode::cli::SlashCommand::Hooks, "hooks", HandleSlashHooks, false, false},
        {lubancode::cli::SlashCommand::Background, "background", HandleSlashBackground, false, false},
        {lubancode::cli::SlashCommand::Keymap, "keymap", HandleSlashKeymap, false, false},
        {lubancode::cli::SlashCommand::Plan, "plan", HandleSlashPlan, false, true},
        {lubancode::cli::SlashCommand::Package, "package", HandleSlashPackage, false, false},
        {lubancode::cli::SlashCommand::Evolve, "evolve", HandleSlashEvolve, false, false},
        {lubancode::cli::SlashCommand::Trace, "trace", HandleSlashTrace, false, false},
        {lubancode::cli::SlashCommand::Doctor, "doctor", HandleSlashDoctor, false, false},
        {lubancode::cli::SlashCommand::Goal, "goal", HandleSlashGoal, false, false},
        {lubancode::cli::SlashCommand::Loop, "loop", HandleSlashLoop, false, false},
        {lubancode::cli::SlashCommand::Memory, "memory", HandleSlashMemory, false, false},
        {lubancode::cli::SlashCommand::Record, "record", HandleSlashRecord, true, false},
        {lubancode::cli::SlashCommand::Sessions, "sessions", HandleSlashSessions, false, false},
        {lubancode::cli::SlashCommand::Archive, "archive", HandleSlashArchive, false, false},
        {lubancode::cli::SlashCommand::Delete, "delete", HandleSlashDelete, false, false},
        {lubancode::cli::SlashCommand::Resume, "resume", HandleSlashResume, false, false},
        {lubancode::cli::SlashCommand::Export, "export", HandleSlashExport, false, false},
        {lubancode::cli::SlashCommand::Copy, "copy", HandleSlashCopy, false, false},
        {lubancode::cli::SlashCommand::Title, "title", HandleSlashTitle, false, false},
        {lubancode::cli::SlashCommand::Soul, "soul", HandleSlashSoul, false, false},
        {lubancode::cli::SlashCommand::Prompt, "prompt", HandleSlashPrompt, false, false},
        {lubancode::cli::SlashCommand::Peers, "peers", HandleSlashPeers, true, false},
        {lubancode::cli::SlashCommand::Send, "send", HandleSlashSend, true, false},
        {lubancode::cli::SlashCommand::Peerperm, "peerperm", HandleSlashPeerperm, true, false},
        {lubancode::cli::SlashCommand::Workflow, "workflow", HandleSlashWorkflow, false, false},
        {lubancode::cli::SlashCommand::Exit, "exit", HandleSlashExit, false, false},
        {lubancode::cli::SlashCommand::Unknown, "unknown", HandleSlashUnknown, false, false},
        {lubancode::cli::SlashCommand::NotSlash, "notslash", nullptr, false, false},
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

}  // namespace lubancode::app
