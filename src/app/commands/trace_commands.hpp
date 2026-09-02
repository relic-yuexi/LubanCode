// 终端接线收尾单:/trace 命令 presenter。原先 166 行的 Trace case 住在
// interactive_session 的 DispatchSlashCommand 里,按病灶二拆出:命令产出
// 数据行(hub 的进程内账/存档真本),presenter 负责怎么画(输出全走
// TerminalPort);大类只留分派。

#pragma once

#include "app/commands/command_flow.hpp"  // CommandFlow(分派注册制)
#include "cli/slash_commands.hpp"          // ParsedSlashCommand(分派注册制)

#include <string>

namespace lubancode::runtime {
class ToolTraceHub;
}
namespace lubancode::sessions {
class SessionStore;
}
namespace lubancode::cli {
struct Theme;
}

namespace lubancode::app {

struct TraceCommandContext {
    lubancode::runtime::ToolTraceHub* trace_hub = nullptr;  // 空 = 没装 hub
    const lubancode::cli::Theme* theme = nullptr;
};

// /trace 的四档:export <路径>(脱敏诊断包)/ errors(明确失败账)/
// toolu|turn|<execution_id>(详细档,翻存档真本)/ 裸敲(最近一批摘要)。
void HandleTraceCommand(const TraceCommandContext& ctx, const std::string& args);

// 命令分派注册制(会话终章):/trace 的分派位(case 体原样搬自大 switch)。
struct SlashDispatchContext;
CommandFlow HandleSlashTrace(SlashDispatchContext& ctx, const lubancode::cli::ParsedSlashCommand& parsed);

}  // namespace lubancode::app
