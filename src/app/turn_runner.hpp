// 一轮问答的执行器:RunTurn(发一轮、起监听线程与状态画板、收排队消息、
// 打统计行),连带 ask_user 的控制台问询(PromptAskUser)与确认前的参数
// 摘要(PrintConfirmDetails/PrintFirstLines)。分界线(PrintDivider)只有
// RunTurn 用,一并住在这。
//
// 骨架拆解批二余款:BuildCallbacks(显示回调装配)随 Callbacks 老路退役。
// 显示出水只剩一只口——轮内起(或宿主给)一只 TurnEventAdapter,终端画
// 屏的 TerminalTurnSink 从 sink 侧吃事件流;控制面(确认/钩子/Plan 闸/
// 逐枚追踪)在 RunTurn 里装配 agent::TurnWiring,递给引擎。
//
// 这一层只认 agent/cli/config/tools/platform 的既有抽象,不 include 会话
// 层的东西;Interactive 与 OneShot 共用。行为、文案、回调次序与搬家前
// 一致——实现住在 turn_runner.cpp(编译边界),公开头只放签名与结果类型。

#pragma once

#include <atomic>
#include <cstdint>
#include <expected>
#include <set>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "agent/loop.hpp"
#include "skills/workflow_recorder.hpp"
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
#include "runtime/turn_event_adapter.hpp"
#include "runtime/turn_runtime.hpp"
#include "runtime/tool_trace_hub.hpp"
#include "tools/agent_tool.hpp"
#include "tools/ask_user.hpp"
#include "tools/registry.hpp"
#include "tools/todo_tool.hpp"

namespace lubancode::config {
struct ModelCatalog;
}

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

std::expected<lubancode::tools::AskUserResponse, std::string> PromptAskUser(
    const lubancode::tools::AskUserQuestion& question, const lubancode::cli::Theme& theme);

// ---------------------------------------------------------------------------
// TurnContext(骨架拆解批三:harness 合流):RunTurn 二十四参 +
// 回调装配十五参收成一只。三个装配点(交互会话、peer 轮、单发)从前
// 各抱一串位置参数,漏一位就是静默错位;现在填字段,名字自带说明。
// 渲染档位也在这只上表达:终端画(缺省,吃事件流的 TerminalTurnSink)、
// 静默(silent,查看态回流)。事件流适配器(turn_events)批二余款起是唯一
// 出水口:给了用宿主的,没给 RunTurn 自起一只。
// ---------------------------------------------------------------------------
struct TurnContext {
    // ---- 回合本体 ----
    lubancode::agent::Agent* loop = nullptr;      // 谁的轮
    std::string user_input;                       // 本轮用户输入(含图像附件语法)
    bool auto_confirm = false;                    // --yes/管道档:确认回调自动放行
    std::set<std::string>* always_allowed_tools = nullptr;  // 会话级"总是允许"账(调用方持有)
    lubancode::cli::Theme theme{};                // 配色(plain 主题下着色为空串)
    lubancode::cli::ContextTracker* context_tracker = nullptr;
    lubancode::tools::ToolRegistry* registry = nullptr;     // 本轮实际在用的工具表
    lubancode::hooks::HookDispatcher* hook_dispatcher = nullptr;

    // ---- 终端档 ----
    bool is_console = false;                      // 真控制台(管道/重定向恒假)
    std::vector<lubancode::cli::TranscriptItem>* transcript = nullptr;  // 会话级条目存档
    std::shared_ptr<lubancode::tools::TodoListState> todo_state;        // 本轮的 todo 板
    std::atomic<bool>* transcript_expanded = nullptr;  // 紧凑/详细档(跨线程)

    // ---- 确认叠加 ----
    std::vector<std::string> allow_commands;      // run_command 前缀白名单
    std::vector<std::string> deny_commands;       // run_command 前缀黑名单

    // ---- 周边接线 ----
    lubancode::tools::AgentTool* completion_agent = nullptr;  // 后台子代理结果回流口
    lubancode::skills::WorkflowRecorder* recorder = nullptr;   // 生成技能录制(旁听)
    // Plan 模式(只读研究硬闸):ModePolicy 闸,空 = 没装。
    std::function<std::string(const std::string&, const nlohmann::json&)> mode_gate;
    // 审批悬起旁听(loop 单遗留):真要问用户前 asked(true),答完 answered
    //(allowed)——装配层拿它推 loop scheduler 的 WaitingPermission 账。
    std::function<void(bool asked, bool allowed)> approval_observer;

    // ---- 台账/追踪 ----
    bool silent = false;                          // 静默档(查看态回流):装饰性输出不上屏
    lubancode::runtime::TurnUsageStats* usage_out = nullptr;   // 整轮 usage 出账
    lubancode::runtime::ToolTraceHub* trace_hub = nullptr;     // 逐枚追踪 hub
    std::string thread_id_for_trace;              // trace 口径的会话号
    std::string turn_id_for_trace;                // trace 口径的轮号(空 = 现发)
    lubancode::runtime::TurnView* turn_view_out = nullptr;     // 轮视图存档(Ctrl+L/resume)

    // ---- 模型输出图片(ccmoon 巡检单 P0)----
    // 会话图片目录(<sessions_dir>/<session-id>/images)。非空 = 挂上
    // on_model_image 落盘口,模型出的图解码落盘、引用入史;空(单发/没
    // 开会话)= 不挂,图片真来了由引擎明败("未接线图片落盘"),不吞图。
    std::string model_images_dir;

    // ---- MCP 富结果单 P0.5:工具二进制 artifact 目录 ----
    // 会话 artifact 目录(<sessions_dir>/<session-id>/mcp-artifacts)。非空
    // = 经 TurnWiring.tool_artifact_dir 递给每次工具调用,MCP 返回的图片/
    // 音频/blob 字节先落这里再入史;空(单发/没开会话)= 富二进制块按稳定
    // 错误收口,文本结果不受影响。
    std::string tool_artifact_dir;

    // ---- 输入图片前置拦截(MiniCPM5 真机巡检单 P2)----
    // 模型目录与当前模型身份:目录声明纯文本(capabilities.image=false 或
    // input_modalities 只列 text)的模型,带附件(/image、@路径)的输入在
    // 发送前拦住,不等服务端回 500。目录没声明 = 允许试探。catalog 可空
    //(调用方没接,行为回退现状);model_id 空 = 不查。
    const lubancode::config::ModelCatalog* model_catalog = nullptr;
    std::string model_id;
    std::string active_provider;

    // ---- 事件流(骨架拆解批二;批二余款升唯一出水口)----
    // SessionRuntime::MakeTurnAdapter() 造的那只(落点已挂会话事件链)。
    // 给了就复用:Start(复用 trace 口径的 turn_id)→ 终端画屏的 sink 从
    // AttachAlongside 补挂 → 收口 Finish(三档 tone 映射终态),Stop 钩子
    // 续跑轮并入同一 turn 的账;缺省 nullptr(单发/单测)= RunTurn 就地起
    // 一只本地适配器,同一套接线。终端渲染照旧逐字节——改的是水的来路。
    lubancode::runtime::TurnEventAdapter* turn_events = nullptr;
};

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

// 发一轮用户输入(TurnContext 收参,批三):走 agent loop(可能会有若干次
// 工具调用来回),流式打字机打印回复,结束后按档打统计行。监听线程
//(TurnInputListener)、流式 footer、心跳、分界线的存废与从前同一套
//(见 TurnContext 字段注释与 turn_runner.cpp 内注)。循环本体、Stop 钩子
// 续跑环、取消链、收场分型在 agent::TurnHarness(与子代理同一份)。
RunTurnResult RunTurn(TurnContext ctx);

}  // namespace lubancode::app
