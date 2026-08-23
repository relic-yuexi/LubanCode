// 一轮问答的执行器:BuildCallbacks(流式正文/工具条目/确认/usage 记账的
// 回调装配)、RunTurn(发一轮、起监听线程与状态画板、收排队消息、打统计
// 行),连带 ask_user 的控制台问询(PromptAskUser)与确认前的参数摘要
// (PrintConfirmDetails/PrintFirstLines)。分界线(PrintDivider)只有
// RunTurn 用,一并住在这。
//
// 这一层只认 agent/cli/config/tools/platform 的既有抽象,不 include 会话
// 层的东西;Interactive 与 OneShot 共用。行为、文案、回调次序与搬家前
// 一致——实现住在 turn_runner.cpp(编译边界),公开头只放签名与结果类型。

#pragma once

#include <atomic>
#include <cstdint>
#include <expected>
#include <functional>
#include <set>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "agent/loop.hpp"
#include "agent/workflow_recorder.hpp"
#include "api/types.hpp"
#include "cli/context_tracker.hpp"
#include "cli/format_utils.hpp"  // TurnFooterTone(Worked/Stopped/Failed)
#include "cli/image_input.hpp"
#include "cli/live_transcript.hpp"
#include "cli/theme.hpp"
#include "cli/tool_display.hpp"
#include "cli/transcript.hpp"
#include "config/config.hpp"
#include "hooks/dispatcher.hpp"
#include "platform/paths.hpp"
#include "runtime/turn_collector.hpp"
#include "runtime/turn_runtime.hpp"
#include "runtime/tool_trace_hub.hpp"
#include "tools/agent_tool.hpp"
#include "tools/ask_user.hpp"
#include "tools/registry.hpp"
#include "tools/todo_tool.hpp"

namespace lubancode::app {

// main.cpp 原文里这些名字是不限定引用的;搬进 app 命名空间后对齐一下。
using lubancode::cli::StreamBodyTracker;
using lubancode::cli::ToolDisplay;
using lubancode::cli::tr;
using lubancode::cli::trf;
using lubancode::platform::CurrentDirUtf8;

// M11(0.10.0):输入/输出分界线。用户回车提交、模型真要开始作答那一刻打
// 一条,回合结束的统计行之后再打一条,把一问一答从视觉上框出来——纯粹
// 是一条线,不带文字、不带花边。is_console 为假(管道/重定向)时直接
// 什么都不打,不污染被重定向的输出。
void PrintDivider(const lubancode::cli::Theme& theme, bool is_console);

// turn 尾分界线(终端回合视觉收束单):"──── Worked for 6m 41s ────"。
// 每个用户 turn 恰一枚;tone 三档(Worked/Stopped/Failed)对应完成/打断/
// 失败;wall_ms 是整轮墙钟(steady_clock 差,毫秒)。窄屏退化成纯文案。
void PrintTurnFooter(const lubancode::cli::Theme& theme, bool is_console, std::int64_t wall_ms,
                     lubancode::cli::TurnFooterTone tone);

// 打印一段文本的前几行,超过就注明省略了多少行。给确认前的改动摘要用。
void PrintFirstLines(const std::string& text, int max_lines);

// 确认前把工具的入参打印清楚,好让人一眼看明白将要发生什么:
// write_file/edit_file 显示路径和内容/改动的前几行摘要,run_command 显示
// 完整命令,别的按通用 JSON 打印兜底。
void PrintConfirmDetails(const std::string& name, const nlohmann::json& input);

std::string TrimAscii(std::string value);

std::expected<std::vector<std::string>, std::string> PromptAskUser(
    const lubancode::tools::AskUserQuestion& question, const lubancode::cli::Theme& theme);

// 交互循环、单发模式共用的回调:文本打字机打印(正文保持原色,不着色),
// 工具调用打一行提示,needs_confirm 的工具按 auto_confirm 决定是自动放行
// 还是问用户一句(三选:y 本次允许 / a 本会话总是允许该工具 / N 拒绝)。
// always_allowed_tools 由调用方持有,跨多轮 Run() 保留,选过 a 的工具本
// 会话内不会再问。usage_stats 由调用方持有,只在这一次 Run() 范围内累计
// (RunTurn() 每次都会传一份新的进来)。registry 是这一轮实际在用的工具表——
// 如果里面注册了 "agent" 工具,这里顺带把这一轮现算好的确认/记账/打印
// 逻辑通过 SetHooks 灌给它,子代理被调用时就能用上同一套(详见
// tools/agent_tool.hpp 顶部注释)。
// display:UI-B(0.12.0)新增,这一轮的工具条目展示总管(建条目、原地
// 改写状态、管道模式的 [工具]/[工具完成] 稳定纯文本),todo_state 也归它
// 持有。回调层只管把事件原样转进去。
lubancode::agent::Callbacks BuildCallbacks(bool auto_confirm, std::set<std::string>& always_allowed_tools,
                                            const lubancode::cli::Theme& theme, lubancode::runtime::TurnUsageStats& usage_stats,
                                            lubancode::cli::ContextTracker& context_tracker,
                                            lubancode::tools::ToolRegistry& registry,
                                            lubancode::hooks::HookDispatcher* hook_dispatcher,
                                            ToolDisplay& display, StreamBodyTracker& body_tracker,
                                            const std::vector<std::string>& allow_commands,
                                            const std::vector<std::string>& deny_commands,
                                            const std::atomic<bool>* cancel_flag = nullptr,
                                            lubancode::agent::WorkflowRecorder* recorder = nullptr,
                                            lubancode::runtime::ToolTraceHub* trace_hub = nullptr,
                                            lubancode::runtime::TurnCollector* view_collector = nullptr,
                                            std::function<std::string(const std::string&, const nlohmann::json&)>
                                                mode_gate = {});

// RunTurn() 的结果:status 沿用老语义(0 成功、非 0 出错);cancelled 标记
// 这一轮是不是被 ESC 打断的(打断不算错误,status 照样是 0)。
// 0.28.x 起,流式期间排下的消息不再经这里事后搬运——监听线程直接落进会话层
// SteeringQueue(cli/queue_model.hpp),由会话泵在安全点(工具边界/收场)
// 投递,queued_lines 字段随之废除。
struct RunTurnResult {
    int status = 0;
    bool cancelled = false;
};

std::string ImageInputErrorText(const lubancode::cli::ImageInputError& error);

// 发一轮用户输入,走 agent loop(可能会有若干次工具调用来回),流式打字机
// 打印回复,结束后打一行 token 用量统计(暗色/淡色,plain 主题下就是空
// 前后缀)。always_allowed_tools 由调用方持有,记录本会话内选过"总是允许"
// 的工具。registry 是这一轮实际在用的工具表,传给 BuildCallbacks 好给里头
// 的 agent 工具(如果有)灌这一轮的转发钩子。
//
// M10:这里起一条 TurnInputListener,存活区间正好是"发出请求到本轮 Run()
// 结束"——ESC 打断、消息排队都靠它。真控制台之外(管道/重定向)监听器
// 构造函数自己判断不起线程,行为跟 0.7.0 完全一致。
// is_console:M11(0.10.0)新增,决定要不要打输入/输出分界线(管道/重定向
// 模式恒为假,分界线完全不出现,不污染被重定向的输出)。todo_state 同样
// M11 新增,转发给 BuildCallbacks 给 on_tool_done 用;留空指针表示这一轮
// 的 registry 没注册 todo_write(目前两个调用点都注册了,这个默认值只是
// 留个口子)。
// transcript:UI-B(0.12.0)新增,会话级工具条目存档(交互循环/AskOnce
// 各持有一份,跨多轮累积),UI-C/D 的 Ctrl+E 全文查看要用。
// transcript_expanded:UI-D(0.16.0)紧凑/详细会话级开关(Ctrl+O 翻转,
// 交互循环持有),详细态下新条目直接按展开版画;回合中切档还会
// 从线程安全快照把现有条目整组重打。AskOnce 不传(nullptr),恒紧凑。
// atomic<bool>:回合执行
// 期间监听线程(另一个线程)会写、Run() 所在的这个线程会读,真机驱动器
// 实测踩到过普通 bool 在这条跨线程路径上的可见性问题(写了但读的那一刻
// 还没看见),换成 atomic<bool> 用 load/store 的 acquire/release 语义堵上。
// allow_commands/deny_commands:settings.local.json 的 run_command 前缀白/黑
// 名单,原样递给 BuildCallbacks 的确认回调叠加判定(缺省空表 = 无叠加)。
// silent(查看态回流单):静默收货档——给"用户正查看别的子代理、main 在
// 后台消化后台结果"的那一轮用。轮子照常跑(模型请求、工具执行、usage/
// context 记账、Hooks、确认交互一个不少),但一切装饰性输出不上屏:分界
// 线/统计行不打、流式 footer 不起、心跳不跳、正文与工具卡只进 transcript
// 台账(StreamBodyTracker 攒正文,收口时归档成一条 assistant 条目),回
// main 时重铺可见。错误路径的 std::cerr 照打——错要让人看见,不静默吞。
RunTurnResult RunTurn(lubancode::agent::AgentLoop& loop, const std::string& user_input, bool auto_confirm,
                       std::set<std::string>& always_allowed_tools, const lubancode::cli::Theme& theme,
                       lubancode::cli::ContextTracker& context_tracker, lubancode::tools::ToolRegistry& registry,
                       lubancode::hooks::HookDispatcher* hook_dispatcher, bool is_console,
                       std::vector<lubancode::cli::TranscriptItem>& transcript,
                       std::shared_ptr<lubancode::tools::TodoListState> todo_state = nullptr,
                       std::atomic<bool>* transcript_expanded = nullptr,
                       const std::vector<std::string>& allow_commands = {},
                       const std::vector<std::string>& deny_commands = {},
                       lubancode::tools::AgentTool* completion_agent = nullptr,
                       lubancode::agent::WorkflowRecorder* recorder = nullptr,
                       bool silent = false,
                       lubancode::runtime::TurnUsageStats* usage_out = nullptr,
                       lubancode::runtime::ToolTraceHub* trace_hub = nullptr,
                       std::string thread_id_for_trace = std::string(),
                       std::string turn_id_for_trace = std::string(),
                       lubancode::runtime::TurnView* turn_view_out = nullptr,
                       std::function<std::string(const std::string&, const nlohmann::json&)>
                           mode_gate = {});

}  // namespace lubancode::app
