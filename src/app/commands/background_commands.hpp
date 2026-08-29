// /background 命令(background 管理面单):后台任务的用户可见、可查、
// 可停的管理面。四个子命令全走本地台账(BackgroundTaskRegistry),不
// 经模型、不发请求——模型代办的 background_output/stop_background 与
// 这里读的是同一份账,状态不各算各的。
//
//   /background                列清单(照旧,带最近三行非空输出)
//   /background show <id>      详情:状态/PID/命令/cwd/启动/已跑/退出码/日志路径
//   /background logs <id> [--tail N]   查看日志尾部(默认 100 行;空/删/坏各说各的)
//   /background stop <id>      停一只(running→stopping→stopped,树死透才报已停)
//   /background stop all       列出将停的并确认;已终态不重复杀
//
// 参数拆解是纯函数(ParseBackgroundCommand,单测钉),这一头只留台账
// 读写与打印。文案走字面量不进 i18n——与既有 /background 清单同路数,
// 这是给开发者的运维视图。边界钉死:日志查看就是读文件,不是"进入终端"
// (可附着 PTY 另立契约,这里不碰)。
#pragma once

#include <string>
#include <vector>

#include "app/commands/command_flow.hpp"  // CommandFlow(分派注册制)
#include "cli/slash_commands.hpp"         // ParsedSlashCommand(分派注册制)
#include "tools/background_tasks.hpp"     // BackgroundTaskInfo(状态段折数)

namespace lubancode::app {

// 定义在 command_registry.hpp(struct;前置声明与定义统一,防 MSVC C4099)。
struct SlashDispatchContext;

enum class BackgroundCommandAction {
    Invalid,
    List,  // /background 或 /background list
    Show,  // /background show <id>
    Logs,  // /background logs <id> [--tail N]
    Stop,  // /background stop <id> | stop all
};

struct ParsedBackgroundCommand {
    BackgroundCommandAction action = BackgroundCommandAction::List;  // 裸敲 = list
    std::string target;    // Show/Logs/Stop 的 task_id(Stop 时可为空,见 stop_all)
    bool stop_all = false; // Stop 专用:stop all
    int tail_lines = 100;  // Logs 的 --tail N,缺省 100;<=0 查全文(与 background_output 同语义)
    std::string bad_word;  // Invalid 时第一词的原始拼写(提示用)
};

// 纯解析:拆出动作、目标、tail 行数。认不得的子命令、show/logs/stop 缺
// 目标、--tail 不带整数一律 Invalid,由 handler 统一打用法。
ParsedBackgroundCommand ParseBackgroundCommand(const std::string& args);

// 底栏状态行段的文本(纯函数,单测钉):tasks 折成 "后台 N 运行 / M 完成"。
// Running 与 Stopping 都算"运行"(停止中也是还没停完);其余终态
// (完成/失败/已停止/停止失败)算"完成"。一只任务都没有返回空串(段收起)。
std::string BuildBackgroundStatusSegment(const std::vector<lubancode::tools::BackgroundTaskInfo>& tasks);

// /background 的分派位(命令注册表登册用)。
CommandFlow HandleSlashBackground(SlashDispatchContext& ctx,
                                  const lubancode::cli::ParsedSlashCommand& parsed);

}  // namespace lubancode::app
