// TerminalTurnSink(骨架拆解批二余款:Callbacks 老路拔除)。
//
// 终端渲染改吃事件流的那只 sink:TurnEventAdapter 吐出的 ServerEvent 流在
// 这里翻成终端画面——打字机正文(StreamBodyTracker)、工具条目(ToolDisplay)、
// 视图账(TurnCollector)、usage 记账(TurnUsageStats + ContextTracker + 状态
// 行局部发布)、生成技能录制(WorkflowRecorder 旁听)。批二两轨并行时的
// BuildCallbacks 显示闭包,逐句搬进这里;水的来路从"回调直调"换成"事件
// 流",画的样子一个字节不变。
//
// 规矩:
//   - 从路事件(payload 带 subordinate 标:子代理/PTC stub)整枚跳过——
//     子代理只画外层卡(条目状态由 agent_tool 的控制钩子与台账 sink 各管
//     各的),主屏不刷嵌套回合的正文;账面侧(会话事件链)照收,与本 sink
//     无关。
//   - Cancelled 收口的工具条目(适配器 Finish 的兜底补账)不画——老路上
//     这类补账只进事件流,不碰显示闭包。
//   - 服务端内置工具(payload 带 builtin 标)只画一张卡,不进视图账与
//     录制——与老路 on_builtin_tool_* 闭包逐字节同画面。
//   - 线程约定(画面隔网先行批后):流内事件(SSE 回调产生的 delta/
//     内置工具起止)只投 UiEventPump 的队列,画在泵的消费线程上;控制路
//     事件(本地工具/usage/批次/收口)仍由产生事件的线程就地画,画前泵
//     先排干 pending。一切渲染被泵的画笔锁串着,画面次序与老路一致。
#pragma once

#include <atomic>
#include <map>
#include <string>
#include <utility>

#include <nlohmann/json.hpp>

#include "app/ui_event_pump.hpp"
#include "runtime/event.hpp"
#include "runtime/event_sink.hpp"

namespace lubancode::cli {
// ToolDisplay 定义是 struct(cli/tool_display.hpp);前置声明与定义统一,
// 防 MSVC C4099。
struct ToolDisplay;
class StreamBodyTracker;
class ContextTracker;
}  // namespace lubancode::cli

namespace lubancode::runtime {
class TurnCollector;
struct TurnUsageStats;
}  // namespace lubancode::runtime

namespace lubancode::skills {
class WorkflowRecorder;
}  // namespace lubancode::skills

namespace lubancode::app {

class TerminalTurnSink final : public runtime::EventSink {
public:
    // 装配材料(全部调用方持有,本类只借引用;可空项各自降级):
    //   recorder + trace_projection_installed:装了 trace hub 的轮次,录制
    //     吃 hub 投影(canonical),不走本 sink 的旁听——与老路同规矩;
    //   cancel_flag:ESC 后补的合成工具结果按 Interrupted 记视图账。
    struct Ingredients {
        cli::ToolDisplay* display = nullptr;
        cli::StreamBodyTracker* body_tracker = nullptr;
        runtime::TurnCollector* view_collector = nullptr;
        runtime::TurnUsageStats* usage_stats = nullptr;
        cli::ContextTracker* context_tracker = nullptr;
        skills::WorkflowRecorder* recorder = nullptr;
        bool trace_projection_installed = false;
        const std::atomic<bool>* cancel_flag = nullptr;
    };

    explicit TerminalTurnSink(Ingredients ingredients)
        : ingredients_(std::move(ingredients)),
          // 画面隔网先行批:渲染闭包挂上 UI 泵——流内事件(SSE 回调产生的
          // 那几种)只投队列、由泵的消费线程画;控制路事件就地画(画前排
          // 干)。泵成员声明在最后,析构最先收(StopAndDrain),渲染闭包
          // 引用的 this 在此期间仍完整。
          ui_pump_([this](const runtime::ServerEvent& event) { RenderEvent(event); }) {}

    // 条 1(画面隔网):网络路只投事件,不碰终端——SSE 流内回调可能产生
    // 的事件(正文/思考 delta、内置工具起止)从 Emit 进来后只进 UI 队列,
    // 产生事件的线程一个终端字节都不写;其余事件(本地工具起止/usage/
    // 批次边界/收口)仍由产生线程就地画——确认菜单等同步交互钉在那些
    // 路上,画前泵先把 pending 的流式事件排干,次序与老路一致(正文永远
    // 先于工具卡落笔)。从路(subordinate)照旧整枚跳过。
    void Emit(const runtime::ServerEvent& event) override;

    // 关账(turn 收尾,listener 停下之后调):停泵的消费线程、排干余量。
    // 此后的画面(收口 chrome、FinalizeRepaint、Stop 钩子续跑、统计行)
    // 全回到调用线程,与老路一字不差。幂等;析构兜底再收一次。
    void StopUiPump();

private:
    // 工具条目对账:item_id -> (tool_use_id, name)。ItemCompleted 的载荷不带
    // 工具名,从这里取;Cancelled 收口的条目从这里摘。
    struct OpenTool {
        std::string tool_use_id;
        std::string name;
    };

    // 既有直写渲染路(原 Emit 正文):由泵的消费线程(流内事件)或产生
    // 事件的线程(控制路,泵排干之后)调,两条路都被泵的画笔锁串着,
    // 状态账(open_tools_/display/body_tracker)不劈腿。
    void RenderEvent(const runtime::ServerEvent& event);
    void OnToolCompleted(const runtime::ServerEvent& event);

    // SSE 流内回调(send_stream 的流内 lambda)产生的事件就这几种:正文/
    // 思考的 ItemDelta、内置工具的 ItemStarted/ItemCompleted。全走投递路。
    static bool IsStreamOrigin(const runtime::ServerEvent& event);

    Ingredients ingredients_;
    std::map<std::string, OpenTool> open_tools_;
    UiEventPump ui_pump_;
};

}  // namespace lubancode::app
