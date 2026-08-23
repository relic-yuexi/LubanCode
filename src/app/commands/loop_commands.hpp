// /loop 命令处理器(loop 单第 2 期):终端这条 slash 的业务面。
//
// 分层:LoopScheduler(runtime)是状态机唯一写口;这里只做三件事——
//   1. 把 ParsedLoopCommand 翻成 scheduler 调用;
//   2. 把结构化 Snapshot()/错误码翻成终端能印的几行字;
//   3. feature gate(features.loop + env 总闸在这层折)。
//
// 事件账(flush/恢复)在 interactive_session 装配层做,这里不碰磁盘。

#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "cli/slash_commands.hpp"
#include "runtime/loop_scheduler.hpp"
#include "runtime/loop_types.hpp"

namespace lubancode::app {

// env 总闸(LUBANCODE_DISABLE_LOOP=1 只关功能不改存档;装配层读同一枚)。
bool LoopDisabledByEnv();

// /loop 的执行结果:ok 由 scheduler 判;lines 是终端直接印的行。
struct LoopCommandOutcome {
    bool ok = false;
    std::vector<std::string> lines;
};

// create 之外的所有动作(list/status/pause/resume/stop/run)。
LoopCommandOutcome HandleLoopManageCommand(lubancode::runtime::loop::LoopScheduler& scheduler,
                                           const lubancode::cli::ParsedLoopCommand& command,
                                           std::int64_t now_ms);

// create:interval 已在会话层解成 seconds;prompt 为空表示走 loop.md/内置
// 源(源解析在会话层做,这里只收成品)。
LoopCommandOutcome HandleLoopCreateCommand(lubancode::runtime::loop::LoopScheduler& scheduler,
                                           const std::string& prompt,
                                           std::chrono::seconds interval,
                                           const std::string& cwd_identity,
                                           const std::string& session_id,
                                           std::int64_t now_ms,
                                           lubancode::runtime::loop::LoopPromptSource source,
                                           const std::string& prompt_file);

// 内置 maintenance prompt(裸 /loop 且没有 loop.md 时的兜底;单子"内置
// maintenance prompt"节原文收窄到三件事)。
std::string BuiltinLoopMaintenancePrompt();

// 把毫秒间隔折成人话("5 分钟"/"2 小时"/"1 天")。
std::string FormatLoopInterval(std::chrono::seconds interval);

// 把"距今多久"折成人话("4 分钟后"/"已到点"/"2 小时前")。
std::string FormatLoopDelta(std::int64_t now_ms, std::int64_t at_ms);

}  // namespace lubancode::app
