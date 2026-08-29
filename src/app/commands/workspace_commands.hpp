// 工作区类 slash 命令的打印件:/tools 三态、/worktree 结果、/plugins、
// /mcp、/lsp。命令的交互接线(确认、取参数)仍在 InteractiveLoop。
// 路径杂务(PathToUtf8/SameFilesystemPath)两边都要用,一并住这,第八步
// 清债时再考虑挪 platform/paths。
//
// 搬家自 main.cpp,行为一字未改;依赖只认 cli/tools/lsp/mcp/app 装配层。


#pragma once

#include "app/commands/command_flow.hpp"  // CommandFlow(分派注册制)
#include "cli/slash_commands.hpp"          // ParsedSlashCommand(分派注册制)

#include <functional>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "app/tool_runtime.hpp"
#include "cli/i18n.hpp"
#include "cli/theme.hpp"
#include "cli/worktree.hpp"
#include "lsp/manager.hpp"
#include "tools/registry.hpp"

namespace lubancode::app {

using lubancode::cli::tr;
using lubancode::cli::trf;

// 各带计数。没启用延迟机制(总数没超阈值,或阈值是 0)时说明一句,不摆
// 三态的空架子。
void PrintToolsCommand(const lubancode::tools::ToolRegistry& registry, const std::set<std::string>& loaded,
                        bool deferral_enabled, int threshold);

std::string PathToUtf8(const std::filesystem::path& path);
bool SameFilesystemPath(const std::filesystem::path& left, const std::filesystem::path& right);

// /worktree 的显示层只拿 i18n 键说话。Git 调用和目录状态都在 cli/worktree
// 里，main 只管给交互会话报结果、在脏树删除前收一声确认。
void PrintWorktreeResult(const lubancode::cli::WorktreeResult& result);


// 当前工作目录,转成 UTF-8 字符串(拼进系统提示词里给模型看)。
std::string PathToUtf8(const std::filesystem::path& path);

bool SameFilesystemPath(const std::filesystem::path& left, const std::filesystem::path& right);


// 打不打,这个函数本身不做 is_console 判断。

// /plugins 命令:列已挂载的插件工具(完整工具名 + 类别)和启动时的加载
// 警告;一个都没有时打印目录约定,顺带说明两类插件各自怎么写。
void PrintPluginsCommand(const std::vector<PluginMountInfo>& mounted, const std::vector<std::string>& warnings);

// /plugin 子命令(plugins 单第 8 步):inspect <id> / doctor <id> /
// test <id>(P3-1:真跑 manifest 声明的 test_runner 自测,交 exit code、
// 耗时与 stdout/stderr 摘要;未声明的明说)/ reload <id> / enable|disable <id> /
// trust|untrust <id>(项目插件信任流)。args 是命令词后面的整段。manifests
// 给 process 插件的清单;mounted 给 native/Lua 的挂载账。
// reload/enable/disable 的运行时换装(v1)以"提示重启"为口径:Lua/process
// 可热重载的钩子另立批次,不在这硬造。trust/untrust 走 runtime 侧的账务
// 动作(概要 + 落账),project_root_utf8/project_trust 由会话的 ToolRuntime
// 递入(缺省空 = 信任流不可用,打一句人话,不硬造)。
void HandlePluginCommand(const std::string& args,
                         const std::vector<PluginMountInfo>& mounted,
                         const std::vector<std::shared_ptr<const lubancode::runtime::PluginManifest>>& manifests,
                         const std::string& project_root_utf8 = std::string(),
                         lubancode::config::PluginTrustStore* project_trust = nullptr);


// /mcp 命令:每个服务器一行状态(运行中/已退出)+ 工具数,底下缩进列出
// 完整工具名(mcp__服务器名__工具名,跟模型实际看到的名字一致)。
void PrintMcpCommand(const std::vector<McpServerRuntime>& mcp_servers);


// /lsp 命令:每个配置了的语言一行状态(未启动/运行中/已闲置关停/已退出)。
// StatusList() 要顺手收割闲置进程(改内部状态),所以入参是可变引用,
// 不装 const。
void PrintLspCommand(std::optional<lubancode::lsp::Manager>& lsp_manager);


// ---------------------------------------------------------------------------
// 工作区命令的窄状态
// ---------------------------------------------------------------------------

// /worktree 借用的会话侧状态:WorktreeSession 是会话与模型侧工具共用的
// 那一只,sync 在 enter/exit 搬了 cwd 之后由会话做提示词/子代理善后。
struct WorkspaceCommandState {
    lubancode::cli::WorktreeSession& worktree;
    std::function<void()> sync_worktree_directory;
};

// /worktree new|list|exit:两道硬确认(脏房强删、园子外的房)就地收。
CommandFlow HandleWorktreeCommand(WorkspaceCommandState& state, const std::string& args,
                                  const lubancode::cli::Theme& theme);

// /background:挪去 background_commands.hpp(管理面单:show/logs/stop)。

// ---------------------------------------------------------------------------
// 命令分派注册制(会话终章):工作面域的分派位(init/worktree/mcp/lsp/
// todos/plugins/plugin/tools)。case 体原样自 interactive_session
// 的大 switch 搬来,材料经 SlashDispatchContext 递入。
// ---------------------------------------------------------------------------
struct SlashDispatchContext;
CommandFlow HandleSlashInit(SlashDispatchContext& ctx, const lubancode::cli::ParsedSlashCommand& parsed);
CommandFlow HandleSlashWorktree(SlashDispatchContext& ctx, const lubancode::cli::ParsedSlashCommand& parsed);
CommandFlow HandleSlashMcp(SlashDispatchContext& ctx, const lubancode::cli::ParsedSlashCommand& parsed);
CommandFlow HandleSlashLsp(SlashDispatchContext& ctx, const lubancode::cli::ParsedSlashCommand& parsed);
CommandFlow HandleSlashTodos(SlashDispatchContext& ctx, const lubancode::cli::ParsedSlashCommand& parsed);
CommandFlow HandleSlashPlugins(SlashDispatchContext& ctx, const lubancode::cli::ParsedSlashCommand& parsed);
CommandFlow HandleSlashPlugin(SlashDispatchContext& ctx, const lubancode::cli::ParsedSlashCommand& parsed);
CommandFlow HandleSlashTools(SlashDispatchContext& ctx, const lubancode::cli::ParsedSlashCommand& parsed);

}  // namespace lubancode::app
