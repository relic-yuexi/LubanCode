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
#include <iostream>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "app/version.hpp"
#include "cli/i18n.hpp"
#include "cli/theme.hpp"
#include "config/config.hpp"
#include "mcp/client.hpp"
#include "mcp/mcp_tool.hpp"
#include "tools/background_output.hpp"
#include "tools/edit_file.hpp"
#include "tools/lua_tool.hpp"
#include "tools/plugin_loader.hpp"
#include "tools/read_file.hpp"
#include "tools/registry.hpp"
#include "tools/run_command.hpp"
#include "tools/search.hpp"
#include "tools/skill_loader.hpp"
#include "tools/skill_tool.hpp"
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

}  // namespace lubancode::app

