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
#include "runtime/worktree.hpp"
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
#include "tools/instruction_scope.hpp"  // 写前作用域闸(AGENTS.md 作用域单 P0)
#include "config/settings_local.hpp"
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
    // Package 会话钉快照(统一封装单阶段 3;阶段 6 起与交互会话共用
    // BuildSessionPackageMountInput 一只折算):单发一场即一会话,启动装配
    // 一次,跑完即弃。信任账同样在此钉住(读不动警告 + 空白续),启停账
    // 现读——停用的包单发也不挂,与交互会话同一道门。
    lubancode::package::PackageTrustSnapshot pinned_trust;
    if (const auto trust_path = lubancode::package::PackageTrustStore::DefaultStorePath();
        trust_path.has_value()) {
        auto [trust_store, trust_error] = lubancode::package::PackageTrustStore::Load(trust_path);
        if (trust_error.has_value()) {
            std::cerr << "[package] " << *trust_error << "\n";
        }
        pinned_trust = trust_store.Snapshot();
    }
    const std::shared_ptr<const lubancode::package::PackageSnapshot> package_snapshot =
        lubancode::package::BuildPackageSnapshot(
            lubancode::app::BuildSessionPackageMountInput(config, package_dirs, pinned_trust),
            /*generation=*/1);
    const std::vector<lubancode::tools::SkillMeta> skills = lubancode::tools::LoadSkills(
        CurrentDirUtf8(), home_dir, lubancode::platform::OfficialSkillsDir(),
        lubancode::package::MountSkillRoots(package_snapshot->mount()));
    const std::string skills_segment = lubancode::tools::BuildSkillsPromptSegment(skills);

    // 提示词运行时化:单发模式同走用户模块目录,拼出去的结构跟交互模式一致。
    const auto home_lubancode = lubancode::config::HomeLubancodeDir();
    const std::string prompts_dir =
        home_lubancode.has_value() ? (*home_lubancode + "/prompts") : std::string();
    // 作用域单 P1:单发与交互同一套闸与口径——Resolver 按会话口径装配
    //(全局层/fallback 名单),root->cwd 基线与逐 source 账一并出。
    const auto instruction_resolver =
        std::make_shared<const lubancode::config::ProjectInstructionResolver>(
            lubancode::config::SessionResolverOptions(config.project_doc_fallback_filenames));
    const lubancode::config::InstructionChain instruction_baseline =
        instruction_resolver->ResolveForPath(std::filesystem::current_path());
    const std::string project_instructions = instruction_baseline.content;
    std::vector<std::string> project_instruction_sources;
    for (const std::filesystem::path& source : instruction_baseline.sources) {
        project_instruction_sources.push_back(lubancode::tools::PathToUtf8(source));
    }
    const auto instruction_scope_state = std::make_shared<lubancode::tools::InstructionScopeState>();
    lubancode::tools::MarkBaselineSeen(*instruction_resolver, *instruction_scope_state,
                                       std::filesystem::current_path(), project_instructions);
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
    // Package 会话钉快照递进工具栈(阶段 3/6 换供应商口):单发一场一会
    // 话、没有 reload,供应商固定回这一折;local 变量 package_snapshot
    // 声明在前、活得比 tool_runtime 久。
    runtime_options.package_snapshot =
        [snapshot = package_snapshot]() { return snapshot; };
    // 动态工具 P3:有效模式装配期判(wire + 目录声明两道门,与交互路
    // BuildSessionStack 同一只纯函数);门不开而配置点名 native 时大声报
    // 并回落 legacy_expand,不悄悄换路。
    {
        const std::string bound_provider_for_mode =
            lubancode::config::BoundProviderName(config, config.active_provider);
        const lubancode::config::ModelCatalogEntry* model_entry =
            once_catalog.FindByProviderAndSlug(bound_provider_for_mode, config.model);
        if (model_entry == nullptr) {
            model_entry = once_catalog.FindBySlug(config.model);
        }
        const lubancode::config::DeferredToolsCapability native_capability =
            lubancode::config::ClassifyNativeToolSearch(model_entry);
        const lubancode::tools::DeferredToolModeResolution resolution = lubancode::tools::ResolveDeferredToolMode(
            config.deferred_tool_mode, config.wire == lubancode::config::Wire::Anthropic,
            native_capability.declared && native_capability.tool_reference,
            native_capability.server_tool_search);
        if (!resolution.native_denial.empty()) {
            std::cout << theme.error << "[tool_search] " << resolution.native_denial << theme.reset << "\n";
        }
        // 动态工具 P4:"auto" 档落 native 的生效说明(与交互路同款文案通道)。
        if (!resolution.mode_note.empty()) {
            std::cout << theme.stats << "[tool_search] " << resolution.mode_note << theme.reset << "\n";
        }
        runtime_options.deferred_mode = resolution.mode;
        runtime_options.native_server_tool_search = resolution.server_tool_search;
    }
    lubancode::app::ToolRuntime tool_runtime(config, theme, wrapped_backend, skills, skills_segment,
                                             CurrentDirUtf8(), std::move(runtime_options));
    auto& registry = tool_runtime.main_registry();
    auto& sub_registry = tool_runtime.sub_registry();
    const auto todo_state = tool_runtime.todo_state();
    const auto loaded_tools = tool_runtime.loaded_tools();
    const bool main_deferral = tool_runtime.main_deferral();
    const bool sub_deferral = tool_runtime.sub_deferral();
    const bool main_proxy = tool_runtime.main_proxy_enabled();
    const bool sub_proxy = tool_runtime.sub_proxy_enabled();
    const bool main_native = tool_runtime.main_native_enabled();
    const bool sub_native = tool_runtime.sub_native_enabled();
    // 动态工具 P4·§十三 P4-4:legacy 档明标 cache-hostile,文案与交互路
    //(session_stack.cpp)逐字一致。deferral 没启用不标。
    if (main_deferral && tool_runtime.main_tool_mode() == lubancode::tools::DeferredToolMode::LegacyExpand) {
        std::cout << theme.stats
                  << "[tool_search] legacy_expand 档:命中后 schema 扩写回顶层 tools 与延迟索引,断前缀缓存"
                     "(cache-hostile);迁移窗内可改 proxy_reference(前缀不断),见 docs/reference/tools.md。"
                  << theme.reset << "\n";
    }
    const auto sub_tool_filter = tool_runtime.sub_tool_filter();
    if (auto* agent_tool = tool_runtime.agent_tool(); agent_tool != nullptr) {
        agent_tool->SetPromptsDir(prompts_dir);  // 子代理系统提示同机制
        // Prompt Profile(阶段 2):项目层根,自定义 Agent 点名 Profile 时用。
        agent_tool->SetProjectPromptsRoot(lubancode::app::ComputeProjectPromptsRoot());
        // 统一 Package 封装单阶段 3:包层 Profile 根,canonical 名在包里解析。
        agent_tool->SetPackageProfileRoots(lubancode::package::MountProfileRoots(package_snapshot->mount()));
        agent_tool->SetProjectInstructions(project_instructions);
        // 作用域单 P0:单发的子代理与 main 共用同一只 Resolver。
        agent_tool->SetInstructionResolver(instruction_resolver);
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
            // 动态工具 P1:legacy 才注索引段;proxy 走 resolver(索引恒空)。
            // P3:native 也不注(发现走 provider 服务端搜索,system 恒定)。
            if (sub_proxy) {
                agent_tool->SetToolRefResolver(tool_runtime.sub_tool_ref_resolver());
                agent_tool->SetToolExecutionPolicy(tool_runtime.sub_execution_policy(),
                                                   tool_runtime.sub_execution_denial());
            } else if (!sub_native) {
                agent_tool->SetDeferredIndexProvider([&sub_registry, loaded_tools]() {
                    return lubancode::tools::BuildDeferredToolsIndexSegment(sub_registry, *loaded_tools);
                });
            }
        }
    }

    // 0.19.x 提示词模块化:跟 InteractiveLoop 同一套条件拼装(skills 有才注、
    // mcp/web/lsp 配了才注、平台段按 wire),发出去的结构两种模式一模一样。
    lubancode::agent::PromptOptions prompt_options;
    prompt_options.cwd = CurrentDirUtf8();
    prompt_options.persona = persona;
    prompt_options.skills_segment = skills_segment;
    prompt_options.project_instructions = project_instructions;
    prompt_options.project_instruction_sources = project_instruction_sources;
    prompt_options.mcp = !config.mcp_servers.empty();
    prompt_options.web = config.search.Configured();
    prompt_options.lsp = !config.lsp_servers.empty();
    prompt_options.wire = lubancode::config::ProviderWireName(config.wire);
    prompt_options.prompts_dir = prompts_dir;  // 运行时模块:构造时现读现拼
    // 统一 Package 封装单阶段 3:包层 Profile 根(canonical 名在包里解析)。
    prompt_options.package_profile_roots = lubancode::package::MountProfileRoots(package_snapshot->mount());

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
    // Agent 拼请求时现查;单发只跑一轮,行为与从前逐字节一致。动态工具 P1:
    // proxy 模式不注索引段(system 恒定,§8.1),改灌解引用器与执行资格。
    // P3:native 同理不注,改灌原生双字段(皮上 native_deferred_tools +
    // 请求档案 server_tool_search)。
    if (main_deferral) {
        once_agent_profile.tool_filter = tool_runtime.main_tool_filter();
        if (main_proxy) {
            once_agent_profile.tool_ref_resolver = tool_runtime.main_tool_ref_resolver();
            once_agent_profile.tool_execution_policy = tool_runtime.main_execution_policy();
            once_agent_profile.tool_execution_denial = tool_runtime.main_execution_denial();
        } else if (main_native) {
            once_agent_profile.native_deferred_tools = true;
            once_agent_profile.request.server_tool_search = tool_runtime.native_server_tool_search();
        } else {
            once_agent_profile.deferred_index_provider = [&registry, loaded_tools]() {
                return lubancode::tools::BuildDeferredToolsIndexSegment(registry, *loaded_tools);
            };
        }
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
    // 作用域单 P0:单发 main 的写前作用域闸(嵌套 AGENTS.md 首写拦下注入,
    // 重试放行)。
    turn.scope_gate = lubancode::tools::BuildScopeGateCallback(instruction_resolver, instruction_scope_state);
    const int status = RunTurn(std::move(turn)).status;
    std::filesystem::remove_all(oneshot_artifacts, artifacts_ec);
    return status;
}

}  // namespace lubancode::app
