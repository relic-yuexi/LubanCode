// one_shot.hpp 的实现:AskOnce 的全套装配。原样搬自原头文件,行为一字
// 未改;对 interactive_session.hpp 的依赖在后续 commit 里解。

#include "app/one_shot.hpp"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <deque>
#include <expected>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <iterator>
#include <memory>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>
#include <nlohmann/json.hpp>
#include "agent/compact.hpp"
#include "agent/loop.hpp"
#include "agent/peer_session.hpp"
#include "agent/prompts.hpp"
#include "agent/session_store.hpp"
#include "agent/workflow_recorder.hpp"
#include "api/anthropic/client.hpp"
#include "api/backend.hpp"
#include "api/chat/client.hpp"
#include "api/models.hpp"
#include "api/responses/client.hpp"
#include "app/backend_stack.hpp"
#include "app/tool_runtime.hpp"
#include "app/turn_runner.hpp"
#include "app/commands/session_commands.hpp"
#include "app/commands/prompt_commands.hpp"
#include "app/commands/settings_commands.hpp"
#include "app/commands/workspace_commands.hpp"
#include "app/version.hpp"
#include "cli/agent_status.hpp"
#include "cli/console_input.hpp"
#include "cli/context_tracker.hpp"
#include "cli/diff.hpp"
#include "cli/divider.hpp"
#include "cli/format_utils.hpp"
#include "cli/i18n.hpp"
#include "cli/image_input.hpp"
#include "cli/live_transcript.hpp"
#include "cli/worktree.hpp"
#include "cli/markdown.hpp"
#include "cli/provider_wizard.hpp"
#include "cli/record_command.hpp"
#include "cli/setup_wizard.hpp"
#include "cli/slash_commands.hpp"
#include "cli/spinner.hpp"
#include "cli/terminal_frame.hpp"
#include "cli/theme.hpp"
#include "cli/todo_render.hpp"
#include "cli/tool_display.hpp"
#include "cli/transcript.hpp"
#include "config/config.hpp"
#include "config/model_catalog.hpp"
#include "config/provider_catalog.hpp"
#include "config/prompt_files.hpp"
#include "config/project_instructions.hpp"
#include "config/skill_store.hpp"
#include "config/update_checker.hpp"
#include "lsp/manager.hpp"
#include "memory/memory_tool.hpp"
#include "memory/project_memory.hpp"
#include "mcp/client.hpp"
#include "mcp/mcp_tool.hpp"
#include "tools/agent_tool.hpp"
#include "tools/ask_user.hpp"
#include "tools/background_output.hpp"
#include "tools/background_tasks.hpp"
#include "tools/command_safety.hpp"
#include "tools/edit_file.hpp"
#include "tools/hooks.hpp"
#include "tools/lua_tool.hpp"
#include "tools/path_utils.hpp"
#include "tools/plugin_loader.hpp"
#include "tools/lsp_tool.hpp"
#include "tools/list_sessions_tool.hpp"
#include "tools/read_file.hpp"
#include "tools/registry.hpp"
#include "tools/run_command.hpp"
#include "tools/search.hpp"
#include "tools/send_session_message_tool.hpp"
#include "tools/skill_loader.hpp"
#include "tools/skill_tool.hpp"
#include "tools/todo_tool.hpp"
#include "tools/tool_search.hpp"
#include "tools/web_fetch.hpp"
#include "tools/web_search.hpp"
#include "tools/write_file.hpp"
#include "platform/console.hpp"
#include "platform/terminal_batch.hpp"
#include "platform/paths.hpp"

namespace lubancode::app {

using lubancode::platform::CurrentDirUtf8;
using lubancode::app::RunTurn;
using lubancode::app::MemoryOptionsFromConfig;
using lubancode::app::BuildBaseToolRegistry;
using lubancode::app::BuildBackend;
using lubancode::app::ThinkOverrideBackend;
using lubancode::app::DeferredIndexBackend;
using lubancode::app::SpinnerBackend;

// 单发模式(位置参数):也走 agent loop,同样支持工具,只是只问这一句。
// 管道/单发场景下 spinner_enabled 传进来的必然是 false(RunCli 里按
// DetectConsoleCapability().is_console 算好的),这里不用再判断一次。
// model_instructions:模型目录里当前模型的 base_instructions(RunCli 按
// 目录算好传进来,不在目录就是空串)。单发模式没有 /model,不用会话级
// 状态和包装层,构造 AgentLoop 时直接拼进系统提示,结构跟交互模式发出去
// 的一模一样;think/context_window 的目录应用同样由 RunCli 预先并进
// config,这里不重复判断——保持这个函数只管"按给定配置问一句"。
// soul_content(0.16.x 魂法分家):当前魂文件的原始内容(RunCli 按配置的
// soul 名读好传进来),单发模式没有 /soul,构造时直接叠加在系统提示最后
// (WithModelInstructions 之后,压轴),跟交互模式发出去的结构一模一样;
// 空串 = 不叠加。
int AskOnce(const lubancode::config::Config& config, const std::string& question, bool auto_confirm,
            const lubancode::cli::Theme& theme, const std::string& persona, bool spinner_enabled,
            const lubancode::config::SettingsLocal& settings_local,
            const std::string& model_instructions, const std::string& soul_content) {
    // M9:技能扫描,理由同 InteractiveLoop——单发模式也该能用技能。
    const std::optional<std::string> home_dir = lubancode::config::HomeDir();
    const std::vector<lubancode::tools::SkillMeta> skills =
        lubancode::tools::LoadSkills(CurrentDirUtf8(), home_dir, lubancode::platform::OfficialSkillsDir());
    const std::string skills_segment = lubancode::tools::BuildSkillsPromptSegment(skills);

    // 提示词运行时化:单发模式同走用户模块目录,拼出去的结构跟交互模式一致。
    const auto home_lubancode = lubancode::config::HomeLubancodeDir();
    const std::string prompts_dir =
        home_lubancode.has_value() ? (*home_lubancode + "/prompts") : std::string();
    const std::string project_instructions =
        lubancode::config::LoadProjectInstructions(std::filesystem::current_path()).content;
    std::shared_ptr<lubancode::memory::ProjectMemory> project_memory;
    if (home_lubancode.has_value() && config.memory.enabled) {
        auto identity = lubancode::memory::ResolveProjectIdentity(
            std::filesystem::current_path(), lubancode::tools::Utf8ToPath(*home_lubancode));
        if (identity.has_value()) {
            auto options = MemoryOptionsFromConfig(config.memory);
            // 单发只可召回不可写(规格授权节):学习档压到 off,写入闸关死。
            options.learn = lubancode::memory::LearnMode::Off;
            options.learn_ceiling = lubancode::memory::LearnMode::Off;
            project_memory = std::make_shared<lubancode::memory::ProjectMemory>(
                std::move(*identity), lubancode::tools::Utf8ToPath(*home_lubancode), options);
        }
    }

    std::unique_ptr<lubancode::api::Backend> backend = BuildBackend(config);
    // 单发模式没有 /think 命令,current_think 构造后不会再变,等价于直接
    // 按配置里的 think 发一次。
    auto current_think = std::make_shared<std::string>(config.think);
    const lubancode::config::ModelCatalog once_catalog = lubancode::config::LoadModelCatalog();
    auto once_model = std::make_shared<std::string>(config.model);
    ThinkOverrideBackend think_backend(*backend, current_think, once_model, &once_catalog);
    SpinnerBackend wrapped_backend(think_backend, theme, spinner_enabled);

    // 工具全栈与 InteractiveLoop 共用一套 ToolRuntime 装配(差异收在
    // options 里:单发无 explore、无 ask_user、不挂 memory_save)。单发
    // 没有横幅与 /mcp、/plugins、/lsp、/tools 命令,挂载行/机制照旧。
    lubancode::app::ToolRuntime::Options runtime_options;
    lubancode::app::ToolRuntime tool_runtime(config, theme, wrapped_backend, skills, skills_segment,
                                             CurrentDirUtf8(), std::move(runtime_options));
    auto& registry = tool_runtime.main_registry();
    auto& sub_registry = tool_runtime.sub_registry();
    const auto todo_state = tool_runtime.todo_state();
    const auto loaded_tools = tool_runtime.loaded_tools();
    const bool main_deferral = tool_runtime.main_deferral();
    const bool sub_deferral = tool_runtime.sub_deferral();
    const auto sub_tool_filter = tool_runtime.sub_tool_filter();
    if (auto* agent_tool = tool_runtime.agent_tool(); agent_tool != nullptr) {
        agent_tool->SetPromptsDir(prompts_dir);  // 子代理系统提示同机制
        agent_tool->SetProjectInstructions(project_instructions);
        if (sub_deferral) {
            agent_tool->SetToolFilter(sub_tool_filter);
            agent_tool->SetDeferredIndexProvider([&sub_registry, loaded_tools]() {
                return lubancode::tools::BuildDeferredToolsIndexSegment(sub_registry, *loaded_tools);
            });
        }
    }
    DeferredIndexBackend index_backend(
        wrapped_backend, [&registry, loaded_tools, main_deferral]() {
            return main_deferral ? lubancode::tools::BuildDeferredToolsIndexSegment(registry, *loaded_tools)
                                  : std::string();
        });

    // 0.19.x 提示词模块化:跟 InteractiveLoop 同一套条件拼装(skills 有才注、
    // mcp/web/lsp 配了才注、平台段按 wire),发出去的结构两种模式一模一样。
    lubancode::agent::PromptOptions prompt_options;
    prompt_options.cwd = CurrentDirUtf8();
    prompt_options.persona = persona;
    prompt_options.skills_segment = skills_segment;
    prompt_options.project_instructions = project_instructions;
    prompt_options.mcp = !config.mcp_servers.empty();
    prompt_options.web = config.search.Configured();
    prompt_options.lsp = !config.lsp_servers.empty();
    prompt_options.wire = lubancode::config::ProviderWireName(config.wire);
    prompt_options.prompts_dir = prompts_dir;  // 运行时模块:构造时现读现拼

    lubancode::agent::AgentLoop loop(
        index_backend, registry, config.model,
        lubancode::agent::WithSoul(
            lubancode::agent::WithModelInstructions(
                lubancode::agent::AssembleSystemPrompt(prompt_options), model_instructions),
            soul_content),
        // max_turns 同上,改用 config.max_turns(默认 0=无上限)。
        /*max_tokens=*/4096, config.max_turns, config.max_context_chars);
    if (main_deferral) {
        loop.SetToolFilter(tool_runtime.main_tool_filter());
    }
    if (project_memory != nullptr) {
        loop.SetTurnSystemSuffix(
            project_memory->BuildTurnContext(question, std::filesystem::current_path()));
    }
    std::set<std::string> always_allowed_tools;
    // settings.local.json 的 allow_tools:单发模式同样注入(免确认)。
    for (const std::string& tool_name : settings_local.allow_tools) {
        always_allowed_tools.insert(tool_name);
    }
    lubancode::cli::ContextTracker context_tracker(config.context_window_tokens);

    // 单发模式没有下一轮循环好把排队消息接着发出去——AskOnce 只问这一句就
    // 退出,ESC/排队这套机制天生只对交互循环有意义(spec 也只要求交互模式
    // 的手测清单),这里只取 status,忽略 cancelled。管道/重定向下监听线程
    // 压根不起,会话层队列天然为空。
    std::vector<lubancode::cli::TranscriptItem> transcript;
    return RunTurn(loop, question, auto_confirm, always_allowed_tools, theme, context_tracker, registry,
                    config.hooks, spinner_enabled, transcript, todo_state, /*transcript_expanded=*/nullptr,
                    settings_local.allow_commands, settings_local.deny_commands)
        .status;
}

}  // namespace lubancode::app