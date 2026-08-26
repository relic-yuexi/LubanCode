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
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "cli/slash_commands.hpp"
#include "cli/theme.hpp"
#include "runtime/loop_scheduler.hpp"
#include "runtime/loop_types.hpp"
#include "sessions/session_store.hpp"

namespace lubancode::runtime {
class EventSink;
}
namespace lubancode::runtime {
class SessionRuntime;
}

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

// ---- /loop 会话接线(终端接线收尾单自大类搬出) ----------------------------
//
// 命令分派、prompt 源解析、存档恢复、ServerEvent 投影原先住在大类里,搬
// 到这里;材料经 LoopWiring 递入(装配与状态留会话)。
struct LoopWiring {
    bool interactive = false;          // 真控制台(pipe/one-shot 明拒)
    bool feature_enabled = false;      // features.loop && !env 总闸
    const lubancode::cli::Theme* theme = nullptr;
    lubancode::runtime::loop::LoopScheduler* scheduler = nullptr;  // ensure 后非空
    lubancode::sessions::SessionStore* session_store = nullptr;
    const std::optional<std::string>* home_lubancode = nullptr;    // 用户级 loop.md
    lubancode::runtime::SessionRuntime* session_runtime = nullptr; // ServerEvent 投影(thread/seq/sink)
    std::function<void()> flush_events;                             // 事件账落盘(EnsureLoopScheduler 后)
};

// 每拍现读的 prompt 源解析:inline 压 loop.md(项目 trust)压用户压内置;
// 文件超 25k 拒。失败给 {空串, 源, 路径} 与 error。
struct LoopPromptResolution {
    std::string text;
    lubancode::runtime::loop::LoopPromptSource source = lubancode::runtime::loop::LoopPromptSource::Builtin;
    std::string file;
    std::string error;
};
LoopPromptResolution ResolveLoopPrompt(const LoopWiring& wiring, const std::string& inline_prompt);

// /loop 命令组(Invalid 用法/Create/管理动作)。ensure_scheduler 由调用方
// 先跑(scheduler 已在 wiring 里)。
int HandleLoopCommand(const lubancode::cli::ParsedLoopCommand& command, const LoopWiring& wiring);

// /resume 后从存档 loop 事件账回放重建(默认 paused-on-resume)。
void RestoreLoopFromArchive(const LoopWiring& wiring);

// compact_v2 事件落盘前补 active loop 摘要(守恒面;没活任务不带)。
void AttachLoopSnapshotToCompact(const LoopWiring& wiring, nlohmann::json& metrics_out);

// scheduler 攒的事件账落盘(append+flush);失败即 FailStore 熔断。投影
// (EmitLoopServerEvents)同在里头。没建档的会话照常跑,事件只进内存。
void FlushLoopEvents(const LoopWiring& wiring);

// loop 事件 -> ServerEvent 投影(sink 没挂零影响)。
void EmitLoopServerEvents(const LoopWiring& wiring,
                          const std::vector<lubancode::runtime::loop::LoopSchedulerEvent>& events);

// 把毫秒间隔折成人话("5 分钟"/"2 小时"/"1 天")。
std::string FormatLoopInterval(std::chrono::seconds interval);

// 把"距今多久"折成人话("4 分钟后"/"已到点"/"2 小时前")。
std::string FormatLoopDelta(std::int64_t now_ms, std::int64_t at_ms);

}  // namespace lubancode::app
