// one_shot.hpp 的实现:AskOnce 的全套装配。原样搬自原头文件,行为一字
// 未改;对 interactive_session.hpp 的依赖在后续 commit 里解。

#include "app/one_shot.hpp"
#include "app/session_stack.hpp"  // AddEvolutionStoreSelections(store 选中版本并轨)

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
#include "peers/peer_session.hpp"
#include "agent/prompts.hpp"
#include "sessions/session_store.hpp"
#include "skills/workflow_recorder.hpp"
#include "api/anthropic/client.hpp"
#include "api/backend.hpp"
#include "api/chat/client.hpp"
#include "api/models.hpp"
#include "api/responses/client.hpp"
#include "app/backend_stack.hpp"
#include "runtime/session_runtime.hpp"
#include "app/runtime_profile.hpp"
#include "app/tool_runtime.hpp"
#include "app/hook_runtime.hpp"
#include "app/turn_runner.hpp"
#include "app/commands/session_commands.hpp"
#include "app/commands/prompt_commands.hpp"
#include "app/commands/settings_commands.hpp"
#include "app/commands/workspace_commands.hpp"
#include "app/commands/agent_commands.hpp"  // ComputeProjectPromptsRoot(阶段 2 Profile 项目层)
#include "app/version.hpp"
#include "package/mounting.hpp"  // PackageMount:会话钉快照(阶段 3 挂载)
#include "package/semver.hpp"    // ParseSemVer:包兼容性检查的当前版本
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
#include "cli/spinner_backend.hpp"
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
#include "platform/process.hpp"  // CurrentProcessId:单发 artifact 临时目录名

namespace lubancode::app {

using lubancode::platform::CurrentDirUtf8;
using lubancode::app::RunTurn;
using lubancode::app::MemoryOptionsFromConfig;
using lubancode::app::BuildBaseToolRegistry;
using lubancode::app::BuildBackend;
using lubancode::cli::SpinnerBackend;

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
            const std::string& model_instructions, const std::string& soul_content,
            const std::vector<std::string>& package_dirs) {
    // app-server 防守(app-server 单:绝不能把子命令当单发问题送进 AskOnce
    // ——那会在协议管线上打出一坨终端文案,搅坏 stdout 分帧)。旗标在
    // ParseCliArgs/RunCli 两层都拦了,这里守最后一道:问题正文恰是裸
    // "app-server" 就明拒,退 2。
    if (question == "app-server") {
        std::cerr << "app-server 是子命令,不是问题;该走协议主循环,不进单发\n";
        return 2;
    }    // M9:技能扫描,理由同 InteractiveLoop——单发模式也该能用技能。
    const std::optional<std::string> home_dir = lubancode::config::HomeDir();
    // Package 会话钉快照(统一封装单阶段 3):单发一场即一会话,启动装配
    // 一次,跑完即弃。四层根与交互会话同一套口径(home 的 packages、项目
    // .lubancode/packages、官方目录、--package-dir);包外短引用的兜底账喂
    // config 的 mcpServers 键、builtin Agent 两名与第一趟裸扫的技能名单。
    lubancode::package::PackageMountInput package_input;
    if (const auto home_lubancode_for_packages = lubancode::config::HomeLubancodeDir();
        home_lubancode_for_packages.has_value()) {
        package_input.scan.user_root =
            lubancode::tools::Utf8ToPath(*home_lubancode_for_packages) / "packages";
    }
    package_input.scan.project_root = std::filesystem::current_path() / ".lubancode" / "packages";
    if (const auto official_packages = lubancode::platform::OfficialPackagesDir();
        official_packages.has_value()) {
        package_input.scan.official_root = lubancode::tools::Utf8ToPath(*official_packages);
    }
    for (const std::string& dev_dir : package_dirs) {
        if (!dev_dir.empty()) {
            package_input.scan.dev_roots.push_back(lubancode::tools::Utf8ToPath(dev_dir));
        }
    }
    if (const auto version = lubancode::package::ParseSemVer(lubancode::app::kVersion);
        version.has_value()) {
        package_input.scan.current_lubancode = *version;
    }
#if defined(_WIN32)
    package_input.scan.current_platform = "windows";
#elif defined(__APPLE__)
    package_input.scan.current_platform = "macos";
#else
    package_input.scan.current_platform = "linux";
#endif
    for (const auto& [mcp_name, mcp_server] : config.mcp_servers) {
        (void)mcp_server;
        package_input.external.mcp_servers.insert(mcp_name);
    }
    package_input.external.agents.insert("general-purpose");
    package_input.external.agents.insert("Explore");
    for (const auto& meta : lubancode::tools::LoadSkills(CurrentDirUtf8(), home_dir,
                                                         lubancode::platform::OfficialSkillsDir())) {
        package_input.external.skills.insert(meta.name);
    }
    // 信任账的只读快照(阶段 4),与交互会话同一出入口;单发一场即一会话,
    // 同样钉住。
    if (const auto trust_path = lubancode::package::PackageTrustStore::DefaultStorePath();
        trust_path.has_value()) {
        auto [trust_store, trust_error] = lubancode::package::PackageTrustStore::Load(trust_path);
        if (trust_error.has_value()) {
            std::cerr << "[package] " << *trust_error << "\n";
        }
        package_input.trust = trust_store.Snapshot();
    }
    // evolution store 的选中版本并轨(阶段 4):单发一场即一会话,与交互
    // 会话同一枚并轨逻辑;store 后续指针变化不影响本场。
    lubancode::app::AddEvolutionStoreSelections(package_input);
    const lubancode::package::PackageMount package_mount =
        lubancode::package::BuildPackageMount(package_input);
    const std::vector<lubancode::tools::SkillMeta> skills = lubancode::tools::LoadSkills(
        CurrentDirUtf8(), home_dir, lubancode::platform::OfficialSkillsDir(),
        lubancode::package::MountSkillRoots(package_mount));
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
    // 批四·病十一其三:五层请求改写后端退役。单发没有 /think,推理档位
    // 由皮上的 request 档案直接带(与配置一字不差),不再经传输层包装。
    const lubancode::config::ModelCatalog once_catalog = lubancode::config::LoadModelCatalog();
    SpinnerBackend wrapped_backend(*backend, theme, spinner_enabled);

    // 工具全栈与 InteractiveLoop 共用一套 ToolRuntime 装配(差异收在
    // options 里:单发无 explore、无 ask_user、不挂 memory_save)。单发
    // 没有横幅与 /mcp、/plugins、/lsp、/tools 命令,挂载行/机制照旧。
    lubancode::app::ToolRuntime::Options runtime_options;
    // Package 会话钉快照递进工具栈(阶段 3):agent 工具派发时按 canonical
    // 名解析 packaged Agent。借用指针,local 变量 package_mount 声明在前、
    // 活得比 tool_runtime 久。
    runtime_options.package_mount = &package_mount;
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
        // Prompt Profile(阶段 2):项目层根,自定义 Agent 点名 Profile 时用。
        agent_tool->SetProjectPromptsRoot(lubancode::app::ComputeProjectPromptsRoot());
        // 统一 Package 封装单阶段 3:包层 Profile 根,canonical 名在包里解析。
        agent_tool->SetPackageProfileRoots(lubancode::package::MountProfileRoots(package_mount));
        agent_tool->SetProjectInstructions(project_instructions);
        // 病十(批三):四段开关随皮走——单发的子代理与 main 同段(mcp/web/
        // lsp 按配置、platforms 按 wire),不再走"四段不传"的薄壳。
        lubancode::agent::AgentProfile subagent_profile;
        subagent_profile.prompt_sections.mcp = !config.mcp_servers.empty();
        subagent_profile.prompt_sections.web = config.search.Configured();
        subagent_profile.prompt_sections.lsp = !config.lsp_servers.empty();
        subagent_profile.prompt_sections.wire = lubancode::config::ProviderWireName(config.wire);
        // 运行策略同级(规格根因一):单发模式的子代理也吃 main 的有效
        // profile 派生份——输出上限/字符安全网/续跑次数同一份,不落回
        // 环境默认。main 的 profile 在下面算,这里先建一份同源的。
        subagent_profile.runtime = lubancode::app::BuildSubagentRuntimeProfile(
            lubancode::app::BuildMainRuntimeProfile(config, &once_catalog, config.model), config);
        agent_tool->SetAgentProfile(std::move(subagent_profile));
        // 单发模式的子代理记忆召回:按任务 prompt 独立检索(与 main 同一只
        // ProjectMemory;关着就不注)。
        if (project_memory != nullptr && config.memory.use) {
            agent_tool->SetTurnContextProvider([memory = project_memory](const std::string& task_prompt) {
                return memory->BuildTurnContext(task_prompt, std::filesystem::current_path(),
                                                lubancode::memory::QueryOrigin::User);
            });
        }
        if (sub_deferral) {
            agent_tool->SetToolFilter(sub_tool_filter);
            agent_tool->SetDeferredIndexProvider([&sub_registry, loaded_tools]() {
                return lubancode::tools::BuildDeferredToolsIndexSegment(sub_registry, *loaded_tools);
            });
        }
    }

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
    // 统一 Package 封装单阶段 3:包层 Profile 根(canonical 名在包里解析)。
    prompt_options.package_profile_roots = lubancode::package::MountProfileRoots(package_mount);

    // 运行策略走统一 profile(规格根因一):输出上限三级解析(config >
    // provider > 模型目录),unset 交服务端默认——与交互会话同一只装配
    // 函数,单发不再单写一枚 4096。
    const lubancode::agent::AgentRuntimeProfile main_profile =
        lubancode::app::BuildMainRuntimeProfile(config, &once_catalog, config.model);
    const std::string bound_provider =
        lubancode::config::BoundProviderName(config, config.active_provider);
    lubancode::agent::AgentProfile once_agent_profile;
    once_agent_profile.provider = bound_provider;
    once_agent_profile.request.model = config.model;
    once_agent_profile.request.reasoning_effort = config.think;
    if (const auto* entry = once_catalog.FindByProviderAndSlug(bound_provider, config.model);
        entry != nullptr) {
        once_agent_profile.request.reasoning = entry->reasoning;
    }
    // 病十(批三):四段开关写进皮——单发的 main 皮与拼 prompt_options 的
    // 条件同源,子代理派生时同段拷贝(上面 SetAgentProfile 那笔)。
    once_agent_profile.prompt_sections.mcp = prompt_options.mcp;
    once_agent_profile.prompt_sections.web = prompt_options.web;
    once_agent_profile.prompt_sections.lsp = prompt_options.lsp;
    once_agent_profile.prompt_sections.wire = prompt_options.wire;
    once_agent_profile.runtime = main_profile;
    once_agent_profile.system_prompt = lubancode::agent::WithSoul(
        lubancode::agent::WithModelInstructions(
            lubancode::agent::AssembleSystemPrompt(prompt_options), model_instructions),
        soul_content);
    // tool_search 的索引段(从前由 DeferredIndexBackend 现拼):皮上的活口,
    // Agent 拼请求时现查;单发只跑一轮,行为与从前逐字节一致。
    if (main_deferral) {
        once_agent_profile.deferred_index_provider = [&registry, loaded_tools]() {
            return lubancode::tools::BuildDeferredToolsIndexSegment(registry, *loaded_tools);
        };
        once_agent_profile.tool_filter = tool_runtime.main_tool_filter();
    }
    lubancode::agent::Agent loop(wrapped_backend, registry, std::move(once_agent_profile));
    std::string turn_context;
    if (project_memory != nullptr) {
        // 单发模式的问题就是用户提问,query_origin=user 才跑检索。
        turn_context = project_memory->BuildTurnContext(question, std::filesystem::current_path(),
                                                         memory::QueryOrigin::User);
    }
    // PTC 指南:与交互会话同一份(当前挂载集的签名索引)。
    if (tool_runtime.ptc_tool() != nullptr) {
        turn_context += tool_runtime.ptc_tool()->GuideSegment();
    }
    if (!turn_context.empty()) {
        loop.SetTurnContext(std::move(turn_context));
    }
    lubancode::cli::ContextTracker context_tracker(config.context_window_tokens);
    // P6/P10(显示系统剥离单):权限账归 SessionRuntime——单发模式与交互
    // 会话同一颗内核(不再各装一遍大栈)。单发不落盘,sessions_dir 给空;
    // settings 的 allow_tools 灌进 runtime 那本,RunTurn 引用同一份。
    lubancode::runtime::SessionRuntime session_runtime({"", std::string(), std::string()});
    for (const std::string& tool_name : settings_local.allow_tools) {
        session_runtime.always_allowed().insert(tool_name);
    }
    std::set<std::string>& always_allowed_tools = session_runtime.always_allowed();

    // 单发模式没有下一轮循环好把排队消息接着发出去——AskOnce 只问这一句就
    // 退出,ESC/排队这套机制天生只对交互循环有意义(spec 也只要求交互模式
    // 的手测清单),这里只取 status,忽略 cancelled。管道/重定向下监听线程
    // 压根不起,会话层队列天然为空。
    std::vector<lubancode::cli::TranscriptItem> transcript;
    // 批三:RunTurn 二十四参收成一只 TurnContext。
    lubancode::app::TurnContext turn;
    turn.loop = &loop;
    turn.user_input = question;
    turn.auto_confirm = auto_confirm;
    turn.always_allowed_tools = &always_allowed_tools;
    turn.theme = theme;
    turn.context_tracker = &context_tracker;
    turn.registry = &registry;
    turn.hook_dispatcher = lubancode::app::HookRuntime();
    turn.is_console = spinner_enabled;
    turn.transcript = &transcript;
    turn.todo_state = todo_state;
    turn.allow_commands = settings_local.allow_commands;
    turn.deny_commands = settings_local.deny_commands;
    // 输入图片前置拦截(MiniCPM5 真机巡检单 P2):与交互会话同一道闸。
    turn.model_catalog = &once_catalog;
    turn.model_id = config.model;
    turn.active_provider = bound_provider;
    // 工具结果图片回喂单:单发不落会话档,但工具(MCP/插件 v2)返回的图片
    // 仍要有落账地——不落账,截图类工具在管道模式下整次被拒(与交互模式
    // 的 <会话>/mcp-artifacts 同一待遇,这里给一只临时目录,进程收尾尽力
    // 清掉;中途崩溃残留的由系统临时目录自理)。字节落了账,wire 才有得
    // 重灌;会话不持久,所以不留长期档案。
    const std::filesystem::path oneshot_artifacts =
        std::filesystem::temp_directory_path() /
        ("lubancode-oneshot-artifacts-" + std::to_string(platform::CurrentProcessId()));
    std::error_code artifacts_ec;
    std::filesystem::create_directories(oneshot_artifacts, artifacts_ec);
    turn.tool_artifact_dir = oneshot_artifacts.generic_string();
    const int status = RunTurn(std::move(turn)).status;
    std::filesystem::remove_all(oneshot_artifacts, artifacts_ec);
    return status;
}

}  // namespace lubancode::app
