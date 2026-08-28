// 组合根装配件的实现(会话终章):原 TerminalSessionController 构造函数的
// 装配段逐字搬来(初始化列表进了 SessionStack 的构造函数,函数体进了
// BuildSessionStack),行为一字未改——注释一并随行。
#include "app/session_stack.hpp"

#include <chrono>
#include <utility>

#include "agent/prompts.hpp"  // LoadSoulContentByName(魂的默认内容)
#include "app/commands/settings_commands.hpp"  // PrintBanner/PrintLubanIcon/ApplyModelCatalog
#include "app/commands/prompt_commands.hpp"    // LoadSoulContentByName(魂内容)
#include "app/commands/agent_commands.hpp"     // ComputeProjectPromptsRoot(阶段 2 Profile 项目层)
#include "app/turn_runner.hpp"                 // PromptAskUser(ask_user 工具的问话)
#include "cli/i18n.hpp"
#include "cli/terminal_port.hpp"
#include "config/model_catalog.hpp"
#include "config/project_instructions.hpp"
#include "memory/memory_tool.hpp"
#include "platform/console.hpp"
#include "platform/paths.hpp"
#include "tools/agent_tool.hpp"
#include "tools/ask_user.hpp"
#include "tools/path_utils.hpp"
#include "tools/registry.hpp"
#include "tools/tool_search.hpp"

namespace lubancode::app {

using lubancode::platform::CurrentDirUtf8;
using lubancode::cli::tr;
using lubancode::cli::trf;
using lubancode::cli::TermOut;

namespace {

// project memory 的装配:身份解析 + worker 起动,失败只打警告不拦会话。
// 保持原先"横幅之前完成"的次序。
std::shared_ptr<lubancode::memory::ProjectMemory> BuildProjectMemory(
    const lubancode::config::Config& config, const std::optional<std::string>& home_lubancode,
    const std::string& executable) {
    std::shared_ptr<lubancode::memory::ProjectMemory> project_memory;
    if (home_lubancode.has_value()) {
        auto identity = lubancode::memory::ResolveProjectIdentity(
            std::filesystem::current_path(), lubancode::tools::Utf8ToPath(*home_lubancode));
        if (identity.has_value()) {
            project_memory = std::make_shared<lubancode::memory::ProjectMemory>(
                std::move(*identity), lubancode::tools::Utf8ToPath(*home_lubancode),
                MemoryOptionsFromConfig(config.memory), executable);
            if (project_memory->generate_enabled()) {
                if (const auto launched = project_memory->LaunchWorker(); !launched.has_value()) {
                    TermOut() << trf("cmd.memory.worker_failed", launched.error()) << "\n";
                }
            }
        } else if (config.memory.enabled) {
            TermOut() << trf("cmd.memory.project_failed", identity.error()) << "\n";
        }
    }
    return project_memory;
}

}  // namespace

// ---- 窄口 ----
lubancode::tools::ToolRegistry& SessionStack::registry() { return tool_runtime->main_registry(); }
lubancode::tools::ToolRegistry& SessionStack::sub_registry() { return tool_runtime->sub_registry(); }
lubancode::tools::AgentTool* SessionStack::agent_tool() { return tool_runtime->agent_tool(); }
const std::shared_ptr<lubancode::tools::TodoListState>& SessionStack::todo_state() {
    return tool_runtime->todo_state();
}
const std::shared_ptr<std::set<std::string>>& SessionStack::loaded_tools() {
    return tool_runtime->loaded_tools();
}
const std::vector<McpServerRuntime>& SessionStack::mcp_servers() { return tool_runtime->mcp_servers(); }
std::optional<lubancode::lsp::Manager>& SessionStack::lsp_manager() { return tool_runtime->lsp_manager(); }
const std::vector<PluginMountInfo>& SessionStack::plugin_mounted() { return tool_runtime->plugin_mounted(); }
const std::vector<std::string>& SessionStack::plugin_warnings() { return tool_runtime->plugin_warnings(); }
const std::function<bool(const lubancode::tools::Tool&)>& SessionStack::main_tool_filter() {
    return tool_runtime->main_tool_filter();
}
const std::function<bool(const lubancode::tools::Tool&)>& SessionStack::sub_tool_filter() {
    return tool_runtime->sub_tool_filter();
}

lubancode::tools::DetachedAgentBackend SessionStack::BuildDetachedBackend() const {
    lubancode::tools::DetachedAgentBackend out;
    out.backend = BuildBackend(config_result.config);
    out.provider = active_provider;
    out.request_profile.model = *current_model;
    out.request_profile.reasoning_effort = *current_think;
    out.model_instructions = *current_model_instructions;
    out.soul = *current_soul;
    return out;
}

std::unique_ptr<lubancode::tools::ToolRegistry> SessionStack::BuildDetachedRegistry() const {
    return std::make_unique<lubancode::tools::ToolRegistry>(BuildBaseToolRegistry(detached_skills, detached_search));
}

// 构造 = 原控制器初始化列表的装配(成员声明序即装配序)。
SessionStack::SessionStack(const InteractiveSessionOptions& options)
    : config_result(options.config_result),
      home_dir(lubancode::config::HomeDir()),
      official_skills_dir(lubancode::platform::OfficialSkillsDir()),
      skills(lubancode::tools::LoadSkills(CurrentDirUtf8(), home_dir, official_skills_dir)),
      skills_segment(lubancode::tools::BuildSkillsPromptSegment(skills)),
      home_lubancode(lubancode::config::HomeLubancodeDir()),
      prompts_dir(home_lubancode.has_value() ? (*home_lubancode + "/prompts") : std::string()),
      project_memory(BuildProjectMemory(config_result.config, home_lubancode, options.executable)),
      project_instructions(
          lubancode::config::LoadProjectInstructions(std::filesystem::current_path()).content),
      global_skills_root(home_lubancode.has_value()
                             ? lubancode::tools::Utf8ToPath(*home_lubancode) / "skills"
                             : std::filesystem::path()),
      project_skills_root(lubancode::tools::Utf8ToPath(CurrentDirUtf8()) / ".lubancode" / "skills"),
      real_backend(config_result.config),
      current_model(std::make_shared<std::string>(config_result.config.model)),
      current_think(std::make_shared<std::string>(config_result.config.think)),
      artifact_store(std::make_shared<lubancode::agent::ContextArtifactStore>()),
      current_model_instructions(std::make_shared<std::string>()),
      current_soul_name(config_result.config.soul.empty() ? "default" : config_result.config.soul),
      current_soul(std::make_shared<std::string>(LoadSoulContentByName(current_soul_name, /*warn=*/true))),
      wrapped_backend(real_backend, options.theme, options.spinner_enabled),
      context_tracker(config_result.config.context_window_tokens),
      active_provider_write_path(
          config_result.sources.active_provider == lubancode::config::Source::ProjectConfigFile
              ? config_result.project_config_file_path
              : std::nullopt),
      detached_skills(skills),
      detached_search(config_result.config.search) {}

std::unique_ptr<SessionStack> BuildSessionStack(const InteractiveSessionOptions& options) {
    std::unique_ptr<SessionStack> stack = std::make_unique<SessionStack>(options);
    lubancode::config::Config& config = stack->config_result.config;
    const lubancode::cli::Theme& theme = options.theme;
    const bool spinner_enabled = options.spinner_enabled;

    // 旧单端字段和某条 provider 完全对上时,起手就把它认作当前端。这样
    // /provider list 的标记和"当前端不能删"都不留空档。
    const bool environment_unbound = lubancode::config::EnvironmentOverridesActiveProvider(
        config, stack->config_result.sources, config.active_provider);
    stack->active_provider = environment_unbound
                                 ? std::string()
                                 : lubancode::config::BoundProviderName(config, config.active_provider);
    if (stack->active_provider.empty() && !environment_unbound) {
        for (const auto& provider : config.providers) {
            if (provider.wire == config.wire && provider.base_url == config.base_url &&
                provider.model == config.model) {
                stack->active_provider = provider.name;
                break;
            }
        }
    }

    // 统一模型路由(模型分工第一期):后台小活(压缩/抽取/标题)按
    // TaskKind 取路由,usage 分角色记账。配置有歧义(compact_model 与
    // cheap_model 同写之类)时把 MergeConfig 记的提示打出来——路由看得见。
    stack->model_router = std::make_unique<lubancode::app::ModelRouterService>(
        stack->config_result, stack->real_backend, stack->current_model, stack->active_provider);
    for (const std::string& notice : stack->config_result.model_role_notices) {
        TermOut() << theme.stats << "[模型路由] " << notice << theme.reset << "\n";
    }

    // 图标只在真控制台打(管道/重定向不打装饰字符),横幅本身不受这条限制。
    if (spinner_enabled) {
        PrintLubanIcon(theme);
    }
    PrintBanner(config, theme);

    // 模型目录:启动时当前模型就在目录里,同样应用 default_think /
    // context_window / base_instructions——但用户显式配过的字段(Source
    // 不是内置默认值)不动。打印紧跟横幅,干了什么一眼看全。
    ApplyModelCatalog(options.model_catalog, *stack->current_model,
                      /*think_explicit=*/stack->config_result.sources.think != lubancode::config::Source::Default,
                      /*window_explicit=*/stack->config_result.sources.context_window_tokens !=
                          lubancode::config::Source::Default,
                      stack->current_think, stack->context_tracker, stack->current_model_instructions);

    // stream_usage 启动诊断提醒(缓存诊断单):chat wire 且没人声明过这个
    // 能力,token/缓存统计可能恒为 0。只提醒,不发请求。
    if (config.wire == lubancode::config::Wire::ChatCompletions && !config.stream_usage_declared) {
        TermOut() << theme.stats << tr("doctor.startup.stream_usage_hint") << theme.reset << "\n";
    }

    // 陈房清扫(0.27.x):只清 agent- 前缀、超过 3 天没动静的隔离子代理房;
    // 有活(改动/自有提交)的跳过,锁着的先放,用户手起的房永不碰。
    if (const auto stale_root = lubancode::cli::FindRepositoryRoot(std::filesystem::current_path())) {
        const auto cleanup = lubancode::cli::CleanStaleAgentWorktrees(*stale_root, std::chrono::hours(72));
        if (cleanup.removed > 0) {
            TermOut() << theme.stats << trf("cmd.worktree.cleaned", cleanup.removed) << theme.reset << "\n";
        }
    }

    // 工具全栈:三表 + MCP/插件/LSP/agent/todo/ask_user/memory/tool_search
    // 的装配全收进 ToolRuntime(引用寿命由成员声明顺序保住),交互与单发
    // 共用一套;会话可变的钩子在下面接着灌。
    lubancode::app::ToolRuntime::Options runtime_options;
    runtime_options.with_explore = true;
    runtime_options.with_ask_user = spinner_enabled;
    runtime_options.ask_user_handler = [theme](const lubancode::tools::AskUserQuestion& question) {
        return PromptAskUser(question, theme);
    };
    runtime_options.memory = stack->project_memory;
    runtime_options.worktree_session = &stack->worktree_session;
    // worktree 工具的两道硬确认(进园子外的房、脏房强删)走自己的问话通道,
    // 不经三档确认——确认档压不住这一问,管道模式没人可问就拒。
    runtime_options.worktree_confirm = [theme_ref = &theme](const std::string& question) -> std::optional<bool> {
        if (!lubancode::platform::StdinIsInteractive() ||
            !lubancode::platform::ProbeStdoutConsole().is_console) {
            return std::nullopt;
        }
        const lubancode::cli::StreamFooterSuspendScope footer_suspend;
        const lubancode::cli::Theme& ask_theme = *theme_ref;
        const auto answer = lubancode::cli::ReadLine(ask_theme.confirm + question + ask_theme.reset, ask_theme,
                                                     /*esc_rejects=*/true);
        return answer.has_value() && (*answer == "y" || *answer == "Y");
    };
    runtime_options.on_worktree_moved = [raw = stack.get()]() {
        if (raw->after_worktree_moved) {
            raw->after_worktree_moved();
        }
    };
    stack->tool_runtime.emplace(config, theme, stack->wrapped_backend, stack->skills, stack->skills_segment,
                                CurrentDirUtf8(), std::move(runtime_options));

    if (stack->agent_tool() != nullptr) {
        // execution_mode=auto 的缺省走向:交互会话里独立探索型任务默认后台
        // (结论稍后送达),模型非等结果不可时显式写 foreground。管道/单发
        // 不设这个,auto 等价前台。
        stack->agent_tool()->SetBackgroundByDefault(true);
        // 每个后台任务各造一份 HTTP client 与基础工具表。取配置/模型/魂时
        // 正在主线程的 agent 工具调用里,拷贝完才起线程,不跨线程读这些
        // 会话可变字段。
        stack->agent_tool()->SetDetachedBackendFactory([raw = stack.get()]() {
            return raw->BuildDetachedBackend();
        });
        stack->agent_tool()->SetDetachedRegistryFactory([raw = stack.get()]() {
            return raw->BuildDetachedRegistry();
        });
        // 墙钟兜底(规格三):整轮上限从 subagent.wall_clock_timeout_secs 来
        // (项目级压全局,都没写用公开默认 1800s;0 = 不限)。
        stack->agent_tool()->SetWallClockTimeout(
            config.subagent.wall_clock_timeout_secs.value_or(
                lubancode::config::kDefaultSubagentWallClockTimeoutSecs));
        // 提示词运行时化:子代理系统提示同机制(features 模块用户文件优先)。
        // Prompt Profile(阶段 2):项目层根一并递进去——自定义 Agent 点名
        // Profile 时,"项目选中覆盖"这层才用得上,default 上下文不受影响。
        stack->agent_tool()->SetPromptsDir(stack->prompts_dir);
        stack->agent_tool()->SetProjectPromptsRoot(lubancode::app::ComputeProjectPromptsRoot());
        stack->agent_tool()->SetProjectInstructions(stack->project_instructions);
        if (stack->sub_deferral) {
            stack->agent_tool()->SetToolFilter(stack->sub_tool_filter());
            stack->agent_tool()->SetDeferredIndexProvider([raw = stack.get()]() {
                return lubancode::tools::BuildDeferredToolsIndexSegment(raw->sub_registry(),
                                                                        *raw->loaded_tools());
            });
        }
    }
    stack->main_deferral = stack->tool_runtime->main_deferral();
    stack->sub_deferral = stack->tool_runtime->sub_deferral();
    stack->tool_search_threshold = config.tool_search_threshold;
    if (stack->main_deferral) {
        TermOut() << theme.stats << trf("tool_search.enabled", stack->tool_search_threshold) << theme.reset
                  << "\n";
    }
    return stack;
}

}  // namespace lubancode::app
