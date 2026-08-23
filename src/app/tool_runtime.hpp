// 工具运行时的装配材料:
//   - BuildBaseToolRegistry / BuildExploreToolRegistry:子代理与主循环共用的
//     基础工具表,以及 Explore 子代理的只读硬边界表;
//   - McpServerRuntime / StartMcpServers / RegisterMcpTools:MCP 子进程的
//     起服、握手、注册(McpTool 抓 Client 引用,runtime 用 unique_ptr 存
//     Client 保地址稳定);
//   - PluginMountInfo / MountPlugins:DLL/Lua 插件扫目录挂进主表。
// 各函数注释里的寿命规矩(谁必须声明在谁之前)由 ToolRuntime 的成员声明
// 顺序接手,调用方(InteractiveSession/AskOnce)不用再背。
//
// 实现在 tool_runtime.cpp(编译边界):具体工具的构造、i18n 输出、目录扫描
// 的依赖都留在 .cpp 一侧,公开头只露类型与函数声明。

#pragma once

#include <atomic>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "api/backend.hpp"
#include "cli/theme.hpp"
#include "cli/worktree.hpp"
#include "config/config.hpp"
#include "config/plugin_trust.hpp"
#include "lsp/manager.hpp"
#include "mcp/client.hpp"
#include "memory/project_memory.hpp"
#include "ptc/ptc_tool.hpp"
#include "runtime/plugin_lua.hpp"
#include "runtime/tool_trace_hub.hpp"
#include "runtime/plugin_tool.hpp"
#include "tools/agent_tool.hpp"
#include "tools/ask_user.hpp"
#include "tools/plugin_loader.hpp"
#include "tools/registry.hpp"
#include "tools/skill_loader.hpp"
#include "tools/todo_tool.hpp"
#include "tools/worktree_tool.hpp"

namespace lubancode::app {

// MountPlugins 的 process 插件出参缺省值(不想收 manifest 的调用方给空
// 静态容器,行为上只是不回填)。
namespace detail {
inline std::vector<std::shared_ptr<const lubancode::runtime::PluginManifest>> kEmptyPluginManifests;
inline std::vector<std::string> kEmptyPluginWarnings;
}  // namespace detail

lubancode::memory::Options MemoryOptionsFromConfig(const lubancode::config::MemoryConfig& config);

// 基础工具集(不含 "agent" 自己)。子代理的工具表就是这一份原样一份——
// 防递归:子代理没法再委托一个孙代理,深度硬限 1。主循环的工具表在这份
// 基础上再多注册一个 "agent" 工具,两份各自独立构建(调用方各自新建一份,
// 每次调用都新建各工具实例,互不共享状态——这些工具本来就是无状态的,
// 多建几份不影响行为,只是各自持有自己的资源句柄)。
//
// 注:main_registry 里的 agent 工具会持有 sub_registry 的引用,调用方
// (InteractiveSession / AskOnce)必须把两份都声明成同一层级的局部变量、
// sub_registry 声明在前 main_registry 声明在后——这样析构顺序自然反过来,
// 不会有悬垂引用;千万不能把它们塞进一个按值返回的结构体里再传出来,
// 那样 move/copy 一趟,agent 工具里存的引用就废了。
// skills:M9 新增,启动时扫描一次的技能清单,原样传进来注册成 "skill"
// 工具——子代理、主代理各自建的 registry 都要有这个工具(技能对子代理
// 同样有用),所以调用方每次调用都传同一份清单进来。
// search_config:websearch 用。web_fetch 无条件注册;web_search 只在配置文件
// 写了 search 段(provider + api_key 齐活)时才注册——没配就不挂,模型的
// 工具表里压根没有这一项,不会瞎调。两个都进基础表,子代理也能用(子代理
// 干"搜了再读再总结"正合适)。
lubancode::tools::ToolRegistry BuildBaseToolRegistry(const std::vector<lubancode::tools::SkillMeta>& skills,
                                                     const lubancode::config::SearchConfig& search_config);

// Explore 的硬边界落在工具表，不只写在提示词里。只给文件读取、代码
// 搜索与网页查阅；命令、写入、技能和外挂工具一概不挂。
lubancode::tools::ToolRegistry BuildExploreToolRegistry(const lubancode::config::SearchConfig& search_config);

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
std::vector<McpServerRuntime> StartMcpServers(
    const std::map<std::string, lubancode::config::McpServerConfig>& configs, const lubancode::cli::Theme& theme);

// 把每个 MCP 服务器握手拿到的工具包成 McpTool,注册进 registry——主循环表、
// 子代理表都要各调一遍(MCP 工具对子代理同样有用),两份各自独立的
// McpTool 实例,但底下持的是同一个 mcp::Client&(工具背后是同一个子进程,
// 不会因为注册了两份就多起一个进程)。
void RegisterMcpTools(std::vector<McpServerRuntime>& mcp_servers, lubancode::tools::ToolRegistry& registry);

// M7:一条插件工具的挂载记录,/plugins 命令展示用。
struct PluginMountInfo {
    std::string tool_name;  // 完整名(plugin__<名>__<工具>),跟模型看到的一致
    std::string kind;       // "DLL" 或 "lua"
};

// M7:扫两类插件(<主目录>/.lubancode/plugins 下的 *.dll 和 *.lua),挂进
// 目标 registry——主表与子代理表都挂(子代理与 main 同能力,独立任务
// agent 默认完成后退出,不是低配跑腿);Explore 只读表不挂。每个插件打
// 一行 "[plugin] 名: N 个工具";坏 DLL / 坏 lua 打警告跳过,不崩。report
// 为 false 时只给另一张 registry 装独立 wrapper/state,不重复打印与记账。
// plugin_host 由调用方持有,且必须声明在 registry 之前(PluginTool 手中的
// luban_tool_def* 指向 DLL 静态数据,模块要活得比 registry 久,析构反序那
// 一套,理由同 mcp_servers);Lua 侧第 4 步起走 EmbeddedLuaRuntime(与
// LegacyLuaTool 同一份 state 引擎,profile/预算/帽/取消链见 plugin_lua.hpp)。
// mounted/warnings 由调用方持有,交互模式给 /plugins 命令用。
void MountPlugins(lubancode::tools::PluginHost& plugin_host, lubancode::runtime::EmbeddedLuaRuntime& lua_runtime,
                  lubancode::tools::ToolRegistry& registry, const lubancode::cli::Theme& theme,
                  std::vector<PluginMountInfo>& mounted, std::vector<std::string>& warnings, bool report = true,
                  std::vector<std::shared_ptr<const lubancode::runtime::PluginManifest>>& process_manifests =
                      detail::kEmptyPluginManifests,
                  std::vector<std::string>& process_warnings = detail::kEmptyPluginWarnings,
                  const std::string& project_root_utf8 = std::string(),
                  const lubancode::config::PluginTrustStore* project_trust = nullptr);

// 一场会话的工具全栈:主循环表、子代理表、(交互模式的)Explore 只读表,
// 连同它们背后的拥有者——MCP 子进程(mcp_servers_)、插件宿主(plugin_host_)、
// LSP 管理器(lsp_manager_)——和 agent/todo/ask_user/memory/tool_search
// 的装配。InteractiveSession 与 AskOnce 共用,差异全在 Options 里。
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
        // 模型侧 worktree 工具(0.27.x):跟用户 /worktree 共用的会话实例。
        // 非空才注册 "worktree" 工具(交互入口传;单发/管道模式没人可问
        // 硬确认,不挂)。confirm 是工具自己的问话通道(进园子外的房、脏房
        // 强删两道硬安全线,确认档压不住);on_session_moved 在 enter/exit
        // 搬了 cwd 之后回调,交互入口用它重拼系统提示、同步子代理 cwd。
        lubancode::cli::WorktreeSession* worktree_session = nullptr;
        lubancode::tools::WorktreeTool::ConfirmHandler worktree_confirm;
        std::function<void()> on_worktree_moved;
    };

    ToolRuntime(const lubancode::config::Config& config, const lubancode::cli::Theme& theme,
                lubancode::api::Backend& agent_backend, const std::vector<lubancode::tools::SkillMeta>& skills,
                const std::string& skills_segment, const std::string& cwd_utf8, Options options);

    ToolRuntime(const ToolRuntime&) = delete;
    ToolRuntime& operator=(const ToolRuntime&) = delete;

    lubancode::tools::ToolRegistry& main_registry() { return main_registry_; }
    lubancode::tools::ToolRegistry& sub_registry() { return sub_registry_; }
    lubancode::tools::ToolRegistry* explore_registry() {
        return explore_registry_.has_value() ? &*explore_registry_ : nullptr;
    }
    lubancode::tools::AgentTool* agent_tool() { return agent_tool_; }
    // PTC(programmatic_tool_calling 工具):tool_calling=json 或装配不成时
    // 为 nullptr。装配成败与 auto 档的落点看 ptc_resolution 文案。
    lubancode::ptc::PtcTool* ptc_tool() { return ptc_tool_; }
    // 本场工具调用后端的落点文案(状态栏用):"ptc" / "json" / "auto→json"
    // / "auto→ptc" / "ptc→json(回落原因)"。
    const std::string& ptc_resolution() const { return ptc_resolution_; }
    const std::shared_ptr<lubancode::tools::TodoListState>& todo_state() const { return todo_state_; }
    // 子表的 todo 板(占位:AgentTool::RunTask 会给每只任务换独占实例,
    // 这块板只是"子代理有 todo 能力"的装配落点,面板不读它)。
    const std::shared_ptr<lubancode::tools::TodoListState>& sub_todo_state() const { return sub_todo_state_; }
    const std::shared_ptr<std::set<std::string>>& loaded_tools() const { return loaded_tools_; }
    bool main_deferral() const { return main_deferral_; }
    bool sub_deferral() const { return sub_deferral_; }
    const std::function<bool(const lubancode::tools::Tool&)>& main_tool_filter() const { return main_tool_filter_; }
    const std::function<bool(const lubancode::tools::Tool&)>& sub_tool_filter() const { return sub_tool_filter_; }
    const std::vector<PluginMountInfo>& plugin_mounted() const { return plugin_mounted_; }
    const std::vector<std::string>& plugin_warnings() const { return plugin_warnings_; }
    // process 插件(plugin.json)的已解析清单,/plugin inspect/doctor 用。
    const std::vector<std::shared_ptr<const lubancode::runtime::PluginManifest>>& process_manifests() const {
        return process_manifests_;
    }
    // ESC 取消链与项目根:每轮由 turn_runner 灌(plugins 单第 7 步)。
    void SetPluginCancel(const std::atomic<bool>* cancel);
    void SetPluginCwd(std::string cwd_utf8);
    // 插件日志去处(LogSink:stderr 分流,不进模型结果)。
    void SetPluginLogSink(lubancode::runtime::PluginLogSink sink);
    // /mcp、/lsp 命令展示用:只读巡检,不另开口子改状态。
    const std::vector<McpServerRuntime>& mcp_servers() const { return mcp_servers_; }
    std::optional<lubancode::lsp::Manager>& lsp_manager() { return lsp_manager_; }

    // /memory on 的事后补挂:主表还没有 memory_save 就挂上(用的还是构造时
    // 那份 ProjectMemory,会话期间不换对象)。已挂过(构造时就挂了)则空操作。
    void AttachMemoryTool(std::shared_ptr<lubancode::memory::ProjectMemory> memory);

    // 逐枚追踪单第四期:挂 undo_file_edit(条件式撤销的执行侧)。hub 由
    // 会话层持有(与 ToolRuntime 同寿命或更久),工具凭它按 execution_id
    // 翻撤销凭据。不挂 = 该会话没有撤销工具(旧路,行为不变)。
    void AttachUndoTool(lubancode::runtime::ToolTraceHub* trace_hub);
    // 上次某枚 undo 补偿了谁(execution_id -> owner);装配层喂给 AgentLoop
    // 的 on_tool_compensates。没有 undo 工具时恒空。
    std::string LastCompensatesOf(const std::string& tool_use_id) const;

private:
    // ---- 拥有者:先声明,后析构(用户表先亡,引用不悬垂) ----
    std::vector<McpServerRuntime> mcp_servers_;
    lubancode::tools::PluginHost plugin_host_;
    lubancode::runtime::EmbeddedLuaRuntime lua_runtime_;
    // process 插件(plugin.json)的清单与 adapter(plugins 单第 7 步挂进
    // MountPlugins):manifest 由 shared_ptr 钉住,adapter 进各张 registry。
    std::vector<std::shared_ptr<const lubancode::runtime::PluginManifest>> process_manifests_;
    std::vector<std::unique_ptr<lubancode::runtime::PluginToolAdapter>> process_adapters_;
    std::vector<std::string> process_plugin_warnings_;
    // 项目插件的信任账(plugins 单第 8 步):启动装载一次,挂在拥有者区。
    std::optional<lubancode::config::PluginTrustStore> project_plugin_trust_;
    std::optional<lubancode::lsp::Manager> lsp_manager_;
    // ---- 用户表:后声明,先析构 ----
    std::optional<lubancode::tools::ToolRegistry> explore_registry_;
    lubancode::tools::ToolRegistry sub_registry_;
    lubancode::tools::ToolRegistry main_registry_;
    // PTC 工具对象在 main_registry_ 里(与 agent_tool_ 同款:注册表持有,
    // 这里只留裸指针)。
    lubancode::ptc::PtcTool* ptc_tool_ = nullptr;
    std::string ptc_resolution_;
    lubancode::tools::AgentTool* agent_tool_ = nullptr;  // 对象在 main_registry_ 里
    std::shared_ptr<lubancode::tools::TodoListState> todo_state_;
    std::shared_ptr<lubancode::tools::TodoListState> sub_todo_state_;
    std::shared_ptr<std::set<std::string>> loaded_tools_ = std::make_shared<std::set<std::string>>();
    bool main_deferral_ = false;
    bool sub_deferral_ = false;
    std::function<bool(const lubancode::tools::Tool&)> main_tool_filter_;
    std::function<bool(const lubancode::tools::Tool&)> sub_tool_filter_;
    std::vector<PluginMountInfo> plugin_mounted_;
    std::vector<std::string> plugin_warnings_;
};

}  // namespace lubancode::app
