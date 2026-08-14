// 工具运行时的装配材料(第一刀:先收自由函数,ToolRuntime 类随后):
//   - BuildBaseToolRegistry / BuildExploreToolRegistry:子代理与主循环共用的
//     基础工具表,以及 Explore 子代理的只读硬边界表;
//   - McpServerRuntime / StartMcpServers / RegisterMcpTools:MCP 子进程的
//     起服、握手、注册(McpTool 抓 Client 引用,runtime 用 unique_ptr 存
//     Client 保地址稳定);
//   - PluginMountInfo / MountPlugins:DLL/Lua 插件扫目录挂进主表。
// 各函数注释里的寿命规矩(谁必须声明在谁之前)在收进 ToolRuntime 后由
// 成员声明顺序接手;眼下调用方(InteractiveLoop/AskOnce)仍须自己背好。

#pragma once

#include <filesystem>
#include <functional>
#include <iostream>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "api/backend.hpp"
#include "app/version.hpp"
#include "cli/i18n.hpp"
#include "cli/theme.hpp"
#include "config/config.hpp"
#include "lsp/manager.hpp"
#include "mcp/client.hpp"
#include "mcp/mcp_tool.hpp"
#include "memory/memory_tool.hpp"
#include "memory/project_memory.hpp"
#include "tools/agent_tool.hpp"
#include "tools/ask_user.hpp"
#include "tools/background_output.hpp"
#include "tools/edit_file.hpp"
#include "tools/lua_tool.hpp"
#include "tools/lsp_tool.hpp"
#include "tools/plugin_loader.hpp"
#include "tools/read_file.hpp"
#include "tools/registry.hpp"
#include "tools/run_command.hpp"
#include "tools/search.hpp"
#include "tools/skill_loader.hpp"
#include "tools/skill_tool.hpp"
#include "tools/todo_tool.hpp"
#include "tools/tool_search.hpp"
#include "tools/web_fetch.hpp"
#include "tools/web_search.hpp"
#include "tools/write_file.hpp"

namespace lubancode::app {

// i18n:装配函数里到处用 tr/trf,拉进来省得每处全限定。
using lubancode::cli::tr;
using lubancode::cli::trf;

// 基础工具集(不含 "agent" 自己)。子代理的工具表就是这一份原样一份——
// 防递归:子代理没法再委托一个孙代理,深度硬限 1。主循环的工具表在这份
// 基础上再多注册一个 "agent" 工具,两份各自独立构建(调用方各自新建一份,
// 每次调用都新建各工具实例,互不共享状态——这些工具本来就是无状态的,
// 多建几份不影响行为,只是各自持有自己的资源句柄)。
//
// 注:main_registry 里的 agent 工具会持有 sub_registry 的引用,调用方
// (InteractiveLoop / AskOnce)必须把两份都声明成同一层级的局部变量、
// sub_registry 声明在前 main_registry 声明在后——这样析构顺序自然反过来,
// 不会有悬垂引用;千万不能把它们塞进一个按值返回的结构体里再传出来,
// 那样 move/copy 一趟,agent 工具里存的引用就废了。
// skills:M9 新增,main.cpp 启动时(或 InteractiveLoop/AskOnce 入口)扫描一次
// 的技能清单,原样传进来注册成 "skill" 工具——子代理、主代理各自建的
// registry 都要有这个工具(技能对子代理同样有用),所以调用方每次调用都
// 传同一份清单进来。
// search_config:websearch 用。web_fetch 无条件注册;web_search 只在配置文件
// 写了 search 段(provider + api_key 齐活)时才注册——没配就不挂,模型的
// 工具表里压根没有这一项,不会瞎调。两个都进基础表,子代理也能用(子代理
// 干"搜了再读再总结"正合适)。
inline lubancode::tools::ToolRegistry BuildBaseToolRegistry(const std::vector<lubancode::tools::SkillMeta>& skills,
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

// Explore 的硬边界落在工具表，不只写在提示词里。只给文件读取、代码
// 搜索与网页查阅；命令、写入、技能和外挂工具一概不挂。
inline lubancode::tools::ToolRegistry BuildExploreToolRegistry(
    const lubancode::config::SearchConfig& search_config) {
    lubancode::tools::ToolRegistry registry;
    registry.Register(std::make_unique<lubancode::tools::ReadFileTool>());
    registry.Register(std::make_unique<lubancode::tools::SearchTool>());
    registry.Register(std::make_unique<lubancode::tools::WebFetchTool>("lubancode/" + std::string(kVersion)));
    if (search_config.Configured()) {
        registry.Register(std::make_unique<lubancode::tools::WebSearchTool>(search_config));
    }
    return registry;
}

// M8:一个已经跑起来的 MCP 服务器运行时状态——协议客户端本体,加上握手时
// 拿到的工具清单。/mcp 命令、注册进 registry 都要用这个。
// client 用 unique_ptr 而不是直接存 mcp::Client 对象:McpTool 持有
// mcp::Client& 引用,这份 runtime 要塞进 vector,vector 扩容/搬移只挪
// unique_ptr 本身(一个指针),Client 对象的地址不变,McpTool 里存的引用
// 不会失效。
struct McpServerRuntime {
    std::string name;
    std::unique_ptr<lubancode::mcp::Client> client;
    std::vector<lubancode::mcp::ToolInfo> tools;
};

// 按配置逐个起 MCP 服务器:起子进程 + initialize 握手 + tools/list。单个
// 服务器出岔子(起不来、握手超时、tools/list 失败……)只打一行警告就跳过,
// 不阻塞整个会话——只有真正跑通全流程的服务器才会进返回的 vector。
// mcpServers 没配(config.mcp_servers 是空 map)时,这个函数循环零次,
// 直接返回空 vector,天然满足"只有配了才起"这条要求,不用另外判断。
inline std::vector<McpServerRuntime> StartMcpServers(
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

// 把每个 MCP 服务器握手拿到的工具包成 McpTool,注册进 registry——主循环表、
// 子代理表都要各调一遍(MCP 工具对子代理同样有用),两份各自独立的
// McpTool 实例,但底下持的是同一个 mcp::Client&(工具背后是同一个子进程,
// 不会因为注册了两份就多起一个进程)。
inline void RegisterMcpTools(std::vector<McpServerRuntime>& mcp_servers, lubancode::tools::ToolRegistry& registry) {
    for (auto& runtime : mcp_servers) {
        for (const auto& tool_info : runtime.tools) {
            // tool_search:MCP 工具裹一层 DeferredTool 标成延迟挂载(mcp/
            // 目录不动,没法直接在 McpTool 上加 override)。阈值没超时延迟
            // 机制整个不启用,这层包装只是纯转发,行为不变。
            registry.Register(std::make_unique<lubancode::tools::DeferredTool>(
                std::make_unique<lubancode::mcp::McpTool>(*runtime.client, runtime.name, tool_info)));
        }
    }
}

// M7:一条插件工具的挂载记录,/plugins 命令展示用。
struct PluginMountInfo {
    std::string tool_name;  // 完整名(plugin__<名>__<工具>),跟模型看到的一致
    std::string kind;       // "DLL" 或 "lua"
};

// M7:扫两类插件(<主目录>/.lubancode/plugins 下的 *.dll 和 *.lua),挂进
// 主 registry——子代理表不挂,短命跑腿不用外挂。每个插件打一行
// "[plugin] 名: N 个工具";坏 DLL / 坏 lua 打警告跳过,不崩。
// plugin_host 由调用方持有,且必须声明在 registry 之前(PluginTool 手里的
// luban_tool_def* 指向 DLL 静态数据,模块要活得比 registry 久,析构反序那
// 一套,理由同 mcp_servers);LuaTool 连 lua_State 整个搬进 registry,没有
// 这层讲究。mounted/warnings 由调用方持有,交互模式给 /plugins 命令用。
inline void MountPlugins(lubancode::tools::PluginHost& plugin_host, lubancode::tools::ToolRegistry& registry,
                  const lubancode::cli::Theme& theme, std::vector<PluginMountInfo>& mounted,
                  std::vector<std::string>& warnings) {
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
    for (auto& warning : new_warnings) {
        std::cout << theme.error << warning << theme.reset << "\n";
        warnings.push_back(std::move(warning));
    }
    for (auto& wrapped : wrapped_plugins) {
        std::cout << trf("plugin.mounted_line", wrapped.stem, wrapped.tools.size()) << "\n";
        for (auto& tool : wrapped.tools) {
            mounted.push_back({tool->name(), "DLL"});
            registry.Register(std::move(tool));
        }
    }

    // Lua 插件
    auto lua_result = lubancode::tools::LoadLuaPlugins(plugins_dir);
    for (auto& warning : lua_result.warnings) {
        std::cout << theme.error << warning << theme.reset << "\n";
        warnings.push_back(std::move(warning));
    }
    for (auto& tool : lua_result.tools) {
        std::cout << trf("plugin.mounted_line", tool->stem(), 1) << "\n";
        mounted.push_back({tool->name(), "lua"});
        registry.Register(std::move(tool));
    }
}

// 一场会话的工具全栈:主循环表、子代理表、(交互模式的)Explore 只读表,
// 连同它们背后的拥有者——MCP 子进程(mcp_servers_)、插件宿主(plugin_host_)、
// LSP 管理器(lsp_manager_)——和 agent/todo/ask_user/memory/tool_search
// 的装配。InteractiveLoop 与 AskOnce 共用,差异全在 Options 里。
//
// 寿命规矩由成员声明顺序接手:拥有者先声明(后析构),用户表后声明
// (先析构)——McpTool 持 Client&、PluginTool 持 DLL 静态数据、LspTool 持
// Manager&、AgentTool 持 sub_registry&,全都先于所指物而亡。类不可复制、
// 不可移动:调用方在栈上原地构造,绝不塞进会搬家的容器。
class ToolRuntime {
public:
    struct Options {
        // 交互模式才有 Explore 只读表(硬边界:只读/搜索/网页,无命令无写入)。
        bool with_explore = false;
        // ask_user 只挂主交互会话(须真控制台),handler 由交互入口注入,
        // 工具层不碰终端。
        bool with_ask_user = false;
        lubancode::tools::AskUserHandler ask_user_handler;
        // 非空且 generate 启用时挂 memory_save 进主表;/memory on 的事后
        // 补挂走 AttachMemoryTool。
        std::shared_ptr<lubancode::memory::ProjectMemory> memory;
    };

    ToolRuntime(const lubancode::config::Config& config, const lubancode::cli::Theme& theme,
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
        // 三份表:sub_registry 只有基础工具(防递归,子代理没有 agent),
        // registry 在此之上多挂 agent 本身;explore 只读硬边界。两份基础表
        // 各自独立构建,工具实例互不共享(本就无状态)。
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
        // default_max_turns 从 15 提到 40:子代理干的是真活(委托它翻找文件、
        // 通读多份文件,实测一单能有 30~190 次工具调用),15 轮远远不够。
        main_registry_.Register(std::make_unique<lubancode::tools::AgentTool>(
            agent_backend, sub_registry_, cwd_utf8, config.model, /*default_max_turns=*/40, skills_segment));
        agent_tool_ = dynamic_cast<lubancode::tools::AgentTool*>(main_registry_.Find("agent"));
        if (agent_tool_ != nullptr && explore_registry_.has_value()) {
            agent_tool_->SetExploreRegistry(&*explore_registry_);
        }
        // todo_write 只挂主表:子代理是短命跑腿,不该有权限乱写主会话的
        // 待办清单。todo_state 由调用方取走共享(RunTurn 渲染、/todos 命令)。
        todo_state_ = std::make_shared<lubancode::tools::TodoListState>();
        main_registry_.Register(std::make_unique<lubancode::tools::TodoWriteTool>(todo_state_));
        if (options.with_ask_user) {
            main_registry_.Register(std::make_unique<lubancode::tools::AskUserTool>(options.ask_user_handler));
        }
        if (options.memory != nullptr && options.memory->generate_enabled()) {
            main_registry_.Register(std::make_unique<lubancode::memory::MemorySaveTool>(options.memory));
        }
        // 插件工具只挂主表(短命跑腿不用外挂),挂载行紧跟 [mcp] 那几行。
        MountPlugins(plugin_host_, main_registry_, theme, plugin_mounted_, plugin_warnings_);

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
    }

    ToolRuntime(const ToolRuntime&) = delete;
    ToolRuntime& operator=(const ToolRuntime&) = delete;

    lubancode::tools::ToolRegistry& main_registry() { return main_registry_; }
    lubancode::tools::ToolRegistry& sub_registry() { return sub_registry_; }
    lubancode::tools::ToolRegistry* explore_registry() {
        return explore_registry_.has_value() ? &*explore_registry_ : nullptr;
    }
    lubancode::tools::AgentTool* agent_tool() { return agent_tool_; }
    const std::shared_ptr<lubancode::tools::TodoListState>& todo_state() const { return todo_state_; }
    const std::shared_ptr<std::set<std::string>>& loaded_tools() const { return loaded_tools_; }
    bool main_deferral() const { return main_deferral_; }
    bool sub_deferral() const { return sub_deferral_; }
    const std::function<bool(const lubancode::tools::Tool&)>& main_tool_filter() const { return main_tool_filter_; }
    const std::function<bool(const lubancode::tools::Tool&)>& sub_tool_filter() const { return sub_tool_filter_; }
    const std::vector<PluginMountInfo>& plugin_mounted() const { return plugin_mounted_; }
    const std::vector<std::string>& plugin_warnings() const { return plugin_warnings_; }
    // /mcp、/lsp 命令展示用:只读巡检,不另开口子改状态。
    const std::vector<McpServerRuntime>& mcp_servers() const { return mcp_servers_; }
    std::optional<lubancode::lsp::Manager>& lsp_manager() { return lsp_manager_; }

    // /memory on 的事后补挂:主表还没有 memory_save 就挂上(用的还是构造时
    // 那份 ProjectMemory,会话期间不换对象)。已挂过(构造时就挂了)则空操作。
    void AttachMemoryTool(std::shared_ptr<lubancode::memory::ProjectMemory> memory) {
        if (memory != nullptr && main_registry_.Find("memory_save") == nullptr) {
            main_registry_.Register(std::make_unique<lubancode::memory::MemorySaveTool>(std::move(memory)));
        }
    }

private:
    // ---- 拥有者:先声明,后析构(用户表先亡,引用不悬垂) ----
    std::vector<McpServerRuntime> mcp_servers_;
    lubancode::tools::PluginHost plugin_host_;
    std::optional<lubancode::lsp::Manager> lsp_manager_;
    // ---- 用户表:后声明,先析构 ----
    std::optional<lubancode::tools::ToolRegistry> explore_registry_;
    lubancode::tools::ToolRegistry sub_registry_;
    lubancode::tools::ToolRegistry main_registry_;
    lubancode::tools::AgentTool* agent_tool_ = nullptr;  // 对象在 main_registry_ 里
    std::shared_ptr<lubancode::tools::TodoListState> todo_state_;
    std::shared_ptr<std::set<std::string>> loaded_tools_ = std::make_shared<std::set<std::string>>();
    bool main_deferral_ = false;
    bool sub_deferral_ = false;
    std::function<bool(const lubancode::tools::Tool&)> main_tool_filter_;
    std::function<bool(const lubancode::tools::Tool&)> sub_tool_filter_;
    std::vector<PluginMountInfo> plugin_mounted_;
    std::vector<std::string> plugin_warnings_;
};

}  // namespace lubancode::app

