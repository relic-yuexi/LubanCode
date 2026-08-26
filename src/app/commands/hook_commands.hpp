// /hooks 管理面(Hooks 生命周期单):只读浏览为主——事件、matcher、类型、
// 命令、来源文件、managed/trusted/pending、definition hash 短码、sync/
// async、timeout、最近一次结果;另有四只动作(trust/untrust/disable/
// enable)与最近运行记录(runs)。不在 TUI 里编辑整份 JSON——看得见、审
//得过、关得掉,先把账立住。
#pragma once

#include "app/commands/command_flow.hpp"  // CommandFlow(分派注册制)
#include "cli/slash_commands.hpp"          // ParsedSlashCommand(分派注册制)

#include <string>

#include "cli/theme.hpp"
#include "hooks/dispatcher.hpp"

namespace lubancode::app {

// args 为 /hooks 后面的整段(已剥空白):
//   空                  列出全部定义(含跳过状态)
//   runs [N]            最近 N 条运行记录(缺省 20)
//   trust <id>          信任该定义的当前 definition hash(仅项目来源需要)
//   untrust <id>        撤销信任(下次即跳过)
//   disable <id>        禁用(managed 不可禁)
//   enable <id>         重新启用
// dispatcher 为空(没 Setup 过,理论只有异常路径)打一行说明。
void HandleHooksCommand(const std::string& args, lubancode::hooks::HookDispatcher* dispatcher,
                         const lubancode::cli::Theme& theme);

// 命令分派注册制(会话终章):/hooks 的分派位(case 体原样搬自大 switch)。
struct SlashDispatchContext;
CommandFlow HandleSlashHooks(SlashDispatchContext& ctx, const lubancode::cli::ParsedSlashCommand& parsed);

}  // namespace lubancode::app
