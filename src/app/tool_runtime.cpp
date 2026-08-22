// tool_runtime.hpp 的实现:工具表装配、MCP 起服注册、插件挂载与
// ToolRuntime 构造的全套函数体,具体工具与 i18n 的依赖都在这只
// translation unit 里,不往公开头漏。

#include "app/tool_runtime.hpp"

#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <utility>

#include "app/version.hpp"
#include "cli/i18n.hpp"
#include "memory/memory_tool.hpp"
#include "mcp/mcp_tool.hpp"
#include "ptc/profile.hpp"
#include "tools/agent_message_tool.hpp"
#include "tools/background_output.hpp"
#include "tools/edit_file.hpp"
#include "tools/lua_tool.hpp"
#include "tools/lsp_tool.hpp"
#include "tools/read_file.hpp"
#include "tools/run_command.hpp"
#include "tools/search.hpp"
#include "tools/skill_tool.hpp"
#include "tools/tool_search.hpp"
#include "tools/web_fetch.hpp"
#include "tools/web_search.hpp"
#include "tools/write_file.hpp"

namespace lubancode::app {

namespace {

// 沙箱豁免开关:POSIX 只有 rlimit(资源上限,无文件系统/网络隔离),按
// 规格"没有可靠 sandbox 的平台默认禁 PTC"执行;开发者/测试要用环境
// 变量 LUBANCODE_PTC_ALLOW_NO_SANDBOX 显式豁免,风险自担。
bool PosixSandboxExempted() {
#ifndef _WIN32
    const char* raw = std::getenv("LUBANCODE_PTC_ALLOW_NO_SANDBOX");
    return raw != nullptr && raw[0] != '\0';
#else
    return true;  // Windows 有 Job Object + 受限 token,天然豁免
#endif
}

}  // namespace

// i18n:装配函数里到处用 tr/trf,拉进来省得每处全限定。
using lubancode::cli::tr;
using lubancode::cli::trf;

lubancode::memory::Options MemoryOptionsFromConfig(const lubancode::config::MemoryConfig& config) {
    lubancode::memory::Options options;
    // config.memory.enabled 只能由用户全局配置打开(config merge 层守过),
    // 到了运行对象就当 global_allowed 用;本场总开关起手与全局授权同步。
    options.global_allowed = config.enabled;
    options.enabled = config.enabled;
    options.use = config.use;
    options.user_enabled = config.user_enabled;
    // learn 档位与上限都来自合并后的配置;本场 set_learn 只能降到上限以内。
    options.learn = lubancode::memory::ParseLearnMode(config.learn).value_or(lubancode::memory::LearnMode::Off);
    options.learn_ceiling = options.learn;
    options.max_index_bytes = config.max_index_bytes;
    options.max_retrieval_bytes = config.max_retrieval_bytes;
    options.max_results = config.max_results;
    return options;
}

lubancode::tools::ToolRegistry BuildBaseToolRegistry(const std::vector<lubancode::tools::SkillMeta>& skills,
                                                     const lubancode::config::SearchConfig& search_config) {
    lubancode::tools::ToolRegistry registry;
    registry.Register(std::make_unique<lubancode::tools::ReadFileTool>());
    registry.Register(std::make_unique<lubancode::tools::RunCommandTool>());
    // 后台命令三件套:run_command 起后台(background_tasks 登记 task_id + watcher
    // 探活),background_output 查状态/读输出,stop_background 收尾。两个新工具
    // 是纯进程内单例查询/控制,无外部依赖,直接进基础表(子代理也能用)。
    registry.Register(std::make_unique<lubancode::tools::BackgroundOutputTool>());
    registry.Register(std::make_unique<lubancode::tools::StopBackgroundTool>());
    registry.Register(std::make_unique<lubancode::tools::WriteFileTool>());
    registry.Register(std::make_unique<lubancode::tools::EditFileTool>());
    registry.Register(std::make_unique<lubancode::tools::SearchTool>());
    registry.Register(std::make_unique<lubancode::tools::SkillTool>(skills));
    registry.Register(std::make_unique<lubancode::tools::WebFetchTool>("lubancode/" + std::string(kVersion)));
    if (search_config.Configured()) {
        registry.Register(std::make_unique<lubancode::tools::WebSearchTool>(search_config));
    }
    return registry;
}

lubancode::tools::ToolRegistry BuildExploreToolRegistry(const lubancode::config::SearchConfig& search_config) {
    lubancode::tools::ToolRegistry registry;
    registry.Register(std::make_unique<lubancode::tools::ReadFileTool>());
    registry.Register(std::make_unique<lubancode::tools::SearchTool>());
    registry.Register(std::make_unique<lubancode::tools::WebFetchTool>("lubancode/" + std::string(kVersion)));
    if (search_config.Configured()) {
        registry.Register(std::make_unique<lubancode::tools::WebSearchTool>(search_config));
    }
    return registry;
}

std::vector<McpServerRuntime> StartMcpServers(
    const std::map<std::string, lubancode::config::McpServerConfig>& configs, const lubancode::cli::Theme& theme) {
    std::vector<McpServerRuntime> out;
    for (const auto& [name, server_config] : configs) {
        auto client = std::make_unique<lubancode::mcp::Client>(name);
        const auto start_result = client->StartProcess(server_config.command, server_config.args, server_config.env);
        if (!start_result.success) {
            std::cout << theme.error << trf("mcp.start_failed", name, start_result.error) << theme.reset << "\n";
            continue;
        }
        const auto init_result = client->Initialize();
        if (!init_result.has_value()) {
            std::cout << theme.error << trf("mcp.init_failed", name, init_result.error()) << theme.reset << "\n";
            continue;
        }
        auto tools_result = client->ListTools();
        if (!tools_result.has_value()) {
            std::cout << theme.error << trf("mcp.list_failed", name, tools_result.error()) << theme.reset << "\n";
            continue;
        }

        McpServerRuntime runtime;
        runtime.name = name;
        runtime.tools = std::move(*tools_result);
        std::cout << trf("mcp.mounted", name, runtime.tools.size()) << "\n";
        runtime.client = std::move(client);
        out.push_back(std::move(runtime));
    }
    return out;
}

void RegisterMcpTools(std::vector<McpServerRuntime>& mcp_servers, lubancode::tools::ToolRegistry& registry) {
    for (auto& runtime : mcp_servers) {
        // 每只 MCP 的 tools 按 qualified name 排序后注册:服务端 ListTools
        // 若乱序,LubanCode 也跟着乱——跨进程 /resume 时 schema 内容虽一样,
        // tools 数组次序换了,请求前缀照样对不上(前缀缓存守恒单第七期)。
        std::sort(runtime.tools.begin(), runtime.tools.end(),
                  [](const auto& left, const auto& right) { return left.name < right.name; });
        for (const auto& tool_info : runtime.tools) {
            // tool_search:MCP 工具裹一层 DeferredTool 标成延迟挂载(mcp/
            // 目录不动,没法直接在 McpTool 上加 override)。阈值没超时延迟
            // 机制整个不启用,这层包装只是纯转发,行为不变。
            registry.Register(std::make_unique<lubancode::tools::DeferredTool>(
                std::make_unique<lubancode::mcp::McpTool>(*runtime.client, runtime.name, tool_info)));
        }
    }
}

void MountPlugins(lubancode::tools::PluginHost& plugin_host, lubancode::tools::ToolRegistry& registry,
                  const lubancode::cli::Theme& theme, std::vector<PluginMountInfo>& mounted,
                  std::vector<std::string>& warnings, bool report) {
    const auto home_dir = lubancode::config::HomeLubancodeDir();
    if (!home_dir.has_value()) {
        return;  // 找不到主目录,也就没有插件目录可扫
    }
    const std::filesystem::path plugins_dir =
        std::filesystem::path(
            std::u8string(reinterpret_cast<const char8_t*>(home_dir->data()), home_dir->size())) /
        "plugins";

    // C ABI DLL 插件
    std::vector<std::string> new_warnings = plugin_host.LoadDirectory(plugins_dir);
    auto wrapped_plugins = plugin_host.WrapTools(new_warnings);
    if (report) {
        for (auto& warning : new_warnings) {
            std::cout << theme.error << warning << theme.reset << "\n";
            warnings.push_back(std::move(warning));
        }
    }
    for (auto& wrapped : wrapped_plugins) {
        if (report) {
            std::cout << trf("plugin.mounted_line", wrapped.stem, wrapped.tools.size()) << "\n";
        }
        for (auto& tool : wrapped.tools) {
            if (report) {
                mounted.push_back({tool->name(), "DLL"});
            }
            registry.Register(std::move(tool));
        }
    }

    // Lua 插件
    auto lua_result = lubancode::tools::LoadLuaPlugins(plugins_dir);
    if (report) {
        for (auto& warning : lua_result.warnings) {
            std::cout << theme.error << warning << theme.reset << "\n";
            warnings.push_back(std::move(warning));
        }
    }
    for (auto& tool : lua_result.tools) {
        if (report) {
            std::cout << trf("plugin.mounted_line", tool->stem(), 1) << "\n";
            mounted.push_back({tool->name(), "lua"});
        }
        registry.Register(std::move(tool));
    }
}

ToolRuntime::ToolRuntime(const lubancode::config::Config& config, const lubancode::cli::Theme& theme,
                         lubancode::api::Backend& agent_backend, const std::vector<lubancode::tools::SkillMeta>& skills,
                         const std::string& skills_segment, const std::string& cwd_utf8, Options options) {
    // M8:起服务器打出 "[mcp] xxx: N 个工具已挂载" 行,紧跟着调用方
    // 刚打完的横幅;单个服务器出岔子只打警告跳过,不阻塞会话。
    mcp_servers_ = StartMcpServers(config.mcp_servers, theme);
    // LSP:配了 lsp 段才构造;构造本身不起进程(懒启动,首次用到某语言
    // 才拉),析构时把还活着的服务器按 shutdown/exit + 2s 兜底全关。
    if (!config.lsp_servers.empty()) {
        lsp_manager_.emplace(config.lsp_servers, cwd_utf8);
    }
    // 三份表:sub_registry 与 main_registry 同为"基础 + MCP + LSP"的全能力
    // 表(子代理与 main 同级,规格"产品不变量");agent 委托工具两边都有
    // ——子表挂的是转发壳(AgentDispatchTool),递归不再靠拿掉工具防,改由
    // AgentTool 的全局并发槽 + 显式深度上限治理(SetDispatchGovernance)。
    // explore 只读硬边界:独立一张只读表,不含 agent/todo/写入类。
    if (options.with_explore) {
        explore_registry_.emplace(BuildExploreToolRegistry(config.search));
    }
    sub_registry_ = BuildBaseToolRegistry(skills, config.search);
    main_registry_ = BuildBaseToolRegistry(skills, config.search);
    // MCP 工具进主表 + 子代理表(不进 explore);两份独立 McpTool 实例,
    // 底下同一个 mcp::Client&。
    RegisterMcpTools(mcp_servers_, sub_registry_);
    RegisterMcpTools(mcp_servers_, main_registry_);
    // lsp 工具同样进主表 + 子代理表,有 explore 也一并;两份(三份)
    // LspTool 共享同一个 Manager。
    if (lsp_manager_.has_value()) {
        if (explore_registry_.has_value()) {
            explore_registry_->Register(std::make_unique<lubancode::tools::LspTool>(*lsp_manager_));
        }
        sub_registry_.Register(std::make_unique<lubancode::tools::LspTool>(*lsp_manager_));
        main_registry_.Register(std::make_unique<lubancode::tools::LspTool>(*lsp_manager_));
    }
    // 子代理步数预算从配置来(规格"现场四"):首选 subagent 段的预算,未设
    // 继承主代理的;0 的语义全路一致(不限步)。旧版这里先后写死
    // 过 40、构造器默认 15——两处暗闸都拆掉,不再有魔数。
    const int subagent_default_steps_per_turn = config.subagent.max_steps_per_turn.value_or(config.max_steps_per_turn);
    main_registry_.Register(std::make_unique<lubancode::tools::AgentTool>(
        agent_backend, sub_registry_, cwd_utf8, config.model, subagent_default_steps_per_turn,
        skills_segment));
    agent_tool_ = dynamic_cast<lubancode::tools::AgentTool*>(main_registry_.Find("agent"));
    if (agent_tool_ != nullptr) {
        // 长任务 compact:子代理复用主 compact,窗口从配置来(0 = 未知不评估)。
        agent_tool_->SetContextWindowTokens(config.context_window_tokens);
        // 派工治理(规格"递归派工不能再靠拿掉工具解决"):并发槽与深度上限
        // 都从配置来,没写用公开默认值(config.hpp)。
        agent_tool_->SetDispatchGovernance(config.subagent.max_active.value_or(lubancode::config::kDefaultSubagentMaxActive),
                                           config.subagent.max_depth.value_or(lubancode::config::kDefaultSubagentMaxDepth));
    }
    // 同级派工:子表也挂 agent(转发壳,目标是上面那只 AgentTool)。后台
    // 的独立注册表(BuildDetachedRegistry)不挂——后台线程不能同步跑前台
    // 任务,那条路要另立单子接(见 AgentDispatchTool 注释)。
    if (agent_tool_ != nullptr) {
        sub_registry_.Register(std::make_unique<lubancode::tools::AgentDispatchTool>(*agent_tool_));
    }
    // agent_message:主模型给运行中子代理传增量的窄工具(只挂主表——深度
    // 超限的孙代理不该再往下传话;主表那枚是 main 用的)。execute 只调
    // AgentTool::SendTaskMessage,与查看态传话、排队转投共用同一本
    // TaskRecord::inbox。
    if (agent_tool_ != nullptr) {
        main_registry_.Register(std::make_unique<lubancode::tools::AgentMessageTool>(agent_tool_));
    }
    if (agent_tool_ != nullptr && explore_registry_.has_value()) {
        agent_tool_->SetExploreRegistry(&*explore_registry_);
    }
    // todo_write:主表挂会话级待办(/todos、RunTurn 渲染读同一份 state);
    // 子表也挂(同级能力),但 AgentTool::RunTask 会把每只任务的 todo_write
    // 换成该任务独占的实例——子代理有自己的私有 todo,不乱写 main 的
    // 待办,也不与别只子代理共用一块板。
    todo_state_ = std::make_shared<lubancode::tools::TodoListState>();
    main_registry_.Register(std::make_unique<lubancode::tools::TodoWriteTool>(todo_state_));
    sub_todo_state_ = std::make_shared<lubancode::tools::TodoListState>();
    sub_registry_.Register(std::make_unique<lubancode::tools::TodoWriteTool>(sub_todo_state_));
    if (options.with_ask_user) {
        main_registry_.Register(std::make_unique<lubancode::tools::AskUserTool>(options.ask_user_handler));
    }
    // memory_save 同级(规格"同级能力审计"memory 写入行):同授权、默认写
    // 候选——子表也挂,与主表同一个 ProjectMemory 引擎。
    if (options.memory != nullptr && options.memory->generate_enabled()) {
        main_registry_.Register(std::make_unique<lubancode::memory::MemorySaveTool>(options.memory));
        sub_registry_.Register(std::make_unique<lubancode::memory::MemorySaveTool>(options.memory));
    }
    // 模型侧 worktree 工具:只挂主表(子代理不起房,带 isolation 的
    // 子代理由 agent_tool 另走 base_dir 包装那条路)。
    if (options.worktree_session != nullptr) {
        main_registry_.Register(std::make_unique<lubancode::tools::WorktreeTool>(
            *options.worktree_session, options.worktree_confirm, options.on_worktree_moved));
    }
    // 插件工具主表 + 子表都挂(子代理与 main 同能力,独立任务 agent 默认
    // 完成后退出,不是低配跑腿),挂载行紧跟 [mcp] 那几行。
    MountPlugins(plugin_host_, main_registry_, theme, plugin_mounted_, plugin_warnings_);
    MountPlugins(plugin_host_, sub_registry_, theme, plugin_mounted_, plugin_warnings_, /*report=*/false);

    // tool_search(延迟挂载):全部工具(MCP/插件/LSP/agent/todo)都注册
    // 完了才数总数、定启停。loaded 集合是会话级的(/clear 不清),主会话
    // 与子代理共享同一份;主表/子表各自按各自的总数判定,同一阈值。
    const int tool_search_threshold = config.tool_search_threshold;
    main_deferral_ = lubancode::tools::DeferralEnabled(main_registry_.All().size(), tool_search_threshold);
    sub_deferral_ = lubancode::tools::DeferralEnabled(sub_registry_.All().size(), tool_search_threshold);
    if (main_deferral_) {
        main_registry_.Register(
            std::make_unique<lubancode::tools::ToolSearchTool>(main_registry_, loaded_tools_));
    }
    if (sub_deferral_) {
        sub_registry_.Register(
            std::make_unique<lubancode::tools::ToolSearchTool>(sub_registry_, loaded_tools_));
    }
    main_tool_filter_ = [loaded = loaded_tools_, deferral = main_deferral_, memory = options.memory](
                            const lubancode::tools::Tool& tool) {
        if (tool.name() == "memory_save") {
            return memory != nullptr && memory->generate_enabled();
        }
        return !deferral || !tool.deferred() || loaded->count(tool.name()) != 0;
    };
    sub_tool_filter_ = [loaded = loaded_tools_, deferral = sub_deferral_](const lubancode::tools::Tool& tool) {
        return !deferral || !tool.deferred() || loaded->count(tool.name()) != 0;
    };

    // ---- PTC 装配(tool_calling 配置档 + 五条硬条件 + auto 门槛)----
    // json(默认):什么都不挂,行为与从前逐字节一致。
    // programmatic:五条硬条件齐(Python 探测在 PtcTool 构造里做)才挂;
    // 不齐明报回落 json。auto:画像 verified + 门槛过才选 ptc——首版没有
    // verified 画像,恒落 json(ResolveToolCalling 照规格实现,不放宽)。
    ptc_resolution_ = "json";
    if (config.tool_calling != lubancode::config::ToolCallingMode::Json) {
        lubancode::ptc::PtcHardConditions hard;
#ifdef _WIN32
        hard.sandbox_reliable = true;  // Job Object + 受限 token
#else
        hard.sandbox_reliable = PosixSandboxExempted();  // rlimit 不算可靠沙箱
#endif
        // 构造 PtcTool(顺带探测 Python)。条件齐才注册进主表。
        auto tool = std::make_unique<lubancode::ptc::PtcTool>(main_registry_, main_tool_filter_,
                                                              [&] {
                                                                  lubancode::ptc::PtcTool::Config ptc_config;
                                                                  ptc_config.python_cmd = config.ptc.python;
                                                                  ptc_config.limits.wall_clock_ms =
                                                                      config.ptc.wall_clock_ms;
                                                                  ptc_config.limits.cpu_ms = config.ptc.cpu_ms;
                                                                  ptc_config.limits.memory_bytes =
                                                                      config.ptc.memory_bytes;
                                                                  ptc_config.limits.output_bytes =
                                                                      config.ptc.output_bytes;
                                                                  ptc_config.limits.max_calls =
                                                                      config.ptc.max_calls;
                                                                  ptc_config.limits.max_concurrency =
                                                                      config.ptc.max_concurrency;
                                                                  ptc_config.restricted_token =
                                                                      config.ptc.restricted_token;
                                                                  ptc_config.eligible_tools = config.ptc.tools;
                                                                  return ptc_config;
                                                              }());
        const bool python_ok = tool->available();
        hard.python_version_ok = python_ok;
        hard.tools_wired = python_ok;  // 入选集(默认 read_file/search)都接 RPC/权限/hooks/取消链
        hard.context_fits_stubs = true;
        hard.model_free_code = true;

        // 画像指纹:provider + endpoint + model + wire + python + harness。
        // 存档里 harness 版本对不上 = 画像过期,按 unknown 算。
        const std::string store_path = lubancode::ptc::DefaultProfileStorePath();
        lubancode::ptc::PtcStatus profile_status = lubancode::ptc::PtcStatus::Unknown;
        if (!store_path.empty()) {
            const std::string fingerprint = lubancode::ptc::BuildPtcFingerprint(
                config.active_provider, config.base_url, config.model,
                lubancode::config::ProviderWireName(config.wire), tool->available() ? "py" : "",
                lubancode::ptc::kPtcHarnessRevision);
            lubancode::ptc::PtcProfileStore store(store_path);
            const auto profile = store.Find(fingerprint);
            if (profile.has_value() && profile->harness_revision == lubancode::ptc::kPtcHarnessRevision) {
                profile_status = profile->status;
            }
        }

        lubancode::ptc::PtcTool* registered = nullptr;
        if (config.tool_calling == lubancode::config::ToolCallingMode::Programmatic) {
            // 强制档:硬条件不齐(POSIX 无沙箱未豁免/没有 Python)就明报
            // 回落,不留一只空壳工具让模型白调。
            if (hard.AllMet()) {
                registered = tool.get();
                main_registry_.Register(std::move(tool));
                ptc_resolution_ = "ptc";
            } else {
                const auto failures = hard.FailureTexts();
                ptc_resolution_ = "ptc→json(" + (failures.empty() ? std::string("条件不齐") : failures.front()) + ")";
                std::cout << theme.error << trf("ptc.fallback_line", ptc_resolution_) << theme.reset << "\n";
            }
        } else {
            // auto:门槛判定(首版无 verified 画像,恒 json)。
            lubancode::ptc::PtcAutoGates gates;
            gates.profile_status = profile_status;
            gates.hard_conditions_met = hard.AllMet();
            gates.estimated_chain_depth = 0;  // 预估器未建:保守零
            gates.estimated_fanout = 0;
            const auto decision = lubancode::ptc::ResolveToolCalling(gates);
            if (decision == lubancode::ptc::ToolCallingDecision::Programmatic) {
                registered = tool.get();
                main_registry_.Register(std::move(tool));
                ptc_resolution_ = "auto→ptc";
            } else {
                ptc_resolution_ = "auto→json";
                if (!python_ok && !config.ptc.python.empty()) {
                    std::cout << theme.error << trf("ptc.probe_failed", tool->unavailability_reason())
                              << theme.reset << "\n";
                }
            }
        }
        ptc_tool_ = registered;
    }
}

void ToolRuntime::AttachMemoryTool(std::shared_ptr<lubancode::memory::ProjectMemory> memory) {
    // capability gate:全局未授权时连工具都不注册(规格"授权边界"节)。
    if (memory != nullptr && memory->generate_enabled() &&
        main_registry_.Find("memory_save") == nullptr) {
        main_registry_.Register(std::make_unique<lubancode::memory::MemorySaveTool>(std::move(memory)));
    }
}

}  // namespace lubancode::app
