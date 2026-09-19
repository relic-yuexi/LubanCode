// TerminalTurnSink(骨架拆解批二余款:Callbacks 老路拔除)。
//
// 终端渲染改吃事件流的那只 sink:TurnEventAdapter 吐出的 ServerEvent 流在
// 这里翻成终端画面——打字机正文(StreamBodyTracker)、工具条目(ToolDisplay)、
// 视图账(TurnCollector)、usage 记账(TurnUsageStats + ContextTracker + 状态
// 行局部发布)、生成技能录制(WorkflowRecorder 旁听)。批二两轨并行时的
// BuildCallbacks 显示闭包,逐句搬进这里;水的来路从"回调直调"换成"事件
// 流",画的样子一个字节不变。
//
// 按代理状态投影单 P1(收账与绘制分离):HandleEvent 拆成两半——
//   - ApplyEvent(收账,恒跑):视图账(TurnCollector,经登记簿
//     ApplyToMainTurn 进锁、修订号 +1)、工具配对(open_tools_)、usage/
//     统计、录制。main 不是当前页也一分不少——账是事实,画是投影。
//   - DrawEvent(绘制,按当前页投影):登记簿闸门(MainShouldDraw)开了
//     才置位 ToolDisplay/StreamBodyTracker 的动态绘制闸,画面随后让路。
//     闸门按修订号对打印水位判定:切回 main 重铺钉过水位后,新事件接续
//     落笔,旧事件(已在重铺快照里)不重放。
// 不许在 Emit 顶部按页 return——那会把视图账、工具配对和统计一并截掉
// (单子 §二.2 的原话)。
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
//   - 证据基建(P0):事件接收、状态应用、帧提交三个关卡各记一笔
//     ui_trace(带 owner/世代/修订号),默认关,不刷屏。
#pragma once

#include <atomic>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <utility>

#include <nlohmann/json.hpp>

#include "app/ui_event_pump.hpp"
#include "app/session_ui_dispatcher.hpp"
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

class AgentViewRegistry;
class SessionUiDispatcher;

class TerminalTurnSink final : public runtime::EventSink {
public:
    // 装配材料(全部调用方持有,本类只借引用;可空项各自降级):
    //   recorder + trace_projection_installed:装了 trace hub 的轮次,录制
    //     吃 hub 投影(canonical),不走本 sink 的旁听——与老路同规矩;
    //   cancel_flag:ESC 后补的合成工具结果按 Interrupted 记视图账;
    //   view_registry(可空):会话级视图登记簿——收账经它进锁、修订号
    //     由它发号、绘制闸按它的当前页/打印水位判定。空(单发/单测/旧
    //     装配)= 不分账,收账直接走、绘制恒开,行为与从前一字不差。
    //   ui_dispatcher(可空,P2 收拢写者):会话级 UI 调度。给了它,流内
    //     与控制路事件一律提交为调度命令、在统一提交锁内落笔——产生
    //     事件的线程(SSE 回调/工具执行/收口)一个终端字节不写,旧路
    //     DispatchInline 的"产生线程就地画"退役;StopUiPump 之后迟到
    //     的 Emit 退化回就地(与旧泵停表同款)。空 = 旧路(本地泵),
    //     行为与从前一字不差。
    struct Ingredients {
        cli::ToolDisplay* display = nullptr;
        cli::StreamBodyTracker* body_tracker = nullptr;
        runtime::TurnCollector* view_collector = nullptr;
        runtime::TurnUsageStats* usage_stats = nullptr;
        cli::ContextTracker* context_tracker = nullptr;
        skills::WorkflowRecorder* recorder = nullptr;
        bool trace_projection_installed = false;
        const std::atomic<bool>* cancel_flag = nullptr;
        AgentViewRegistry* view_registry = nullptr;
        SessionUiDispatcher* ui_dispatcher = nullptr;
    };

    // P2:调度器路径下本地泵不构造(optional 按需起),事件全走调度;
    // 旧路照旧——泵声明在最后,析构最先收(StopAndDrain),渲染闭包
    // 引用的 this 在此期间仍完整。
    ~TerminalTurnSink() override;

    explicit TerminalTurnSink(Ingredients ingredients)
        : ingredients_(std::move(ingredients)), dispatcher_(ingredients_.ui_dispatcher) {
        if (dispatcher_ == nullptr) {
            ui_pump_.emplace([this](const runtime::ServerEvent& event) { HandleEvent(event); },
                             UiEventPump::FrameIntervalFromEnv(), &own_commit_mutex_);
            renderer_id_ = 0;
        } else {
            renderer_id_ = dispatcher_->AttachRenderer(
                [this](const runtime::ServerEvent& event) { HandleEvent(event); });
        }
    }

    // 条 1(画面隔网)+ P2(收拢写者):网络路只投事件,不碰终端——SSE
    // 流内回调可能产生的事件(正文/思考 delta、内置工具起止)从 Emit 进
    // 来后只进 UI 队列,产生事件的线程一个终端字节都不写;控制路事件
    // (本地工具起止/usage/批次边界/收口)有调度器时同样只提交命令
    // (FIFO 保住"正文先于工具卡"),没调度器(单发/单测)照旧由产生线程
    // 就地画(画前泵排干)。StopUiPump 之后迟到的 Emit(Stop 钩子续跑)
    // 一律退化成就地画。从路(subordinate)照旧整枚跳过。
    void Emit(const runtime::ServerEvent& event) override;

    // 关账(turn 收尾,listener 停下之后调):停泵/停提交、排干余量。
    // 此后的画面(收口 chrome、FinalizeRepaint、Stop 钩子续跑、统计行)
    // 全回到调用线程,与老路一字不差。幂等;析构兜底再收一次。
    void StopUiPump();

    // 统一提交锁(P2):调度器路径给调度器那把(会话级);旧路给泵共享
    // 的 sink 自持那把。一切落笔(换页事务、收口 chrome、确认菜单的显示
    // 半边)在它内核对页身份——RunTurn 起回合时把它登记进
    // AgentViewRegistry::BeginMainTurn,WithMainRenderLock 的护栏由此
    // 与在飞的调度渲染互斥。
    std::recursive_mutex& CommitMutex() {
        return dispatcher_ != nullptr ? dispatcher_->commit_mutex() : own_commit_mutex_;
    }

private:
    // 工具条目对账:item_id -> (tool_use_id, name)。ItemCompleted 的载荷不带
    // 工具名,从这里取;Cancelled 收口的条目从这里摘。
    struct OpenTool {
        std::string tool_use_id;
        std::string name;
    };

    // 泵消费侧/就地路的总口:先收账(恒跑),再按当前页决定画不画。
    void HandleEvent(const runtime::ServerEvent& event);

    // 收账半边(P1):视图账(经登记簿进锁,出参 revision 带回本笔修订号)、
    // 工具配对、usage/统计、录制。不碰 ToolDisplay/StreamBodyTracker。
    void ApplyEvent(const runtime::ServerEvent& event, std::uint64_t& revision);

    // 绘制半边(P1):ToolDisplay/StreamBodyTracker 的原有调用,一个不少;
    // 画不画由事先置好的动态绘制闸(display/body_tracker 的 SetPageVisible)
    // 决定。由泵的消费线程(流内事件)或产生事件的线程(控制路,泵排干
    // 之后)调,两条路都被泵的画笔锁串着。completed_tool 是 HandleEvent
    // 在收账前取好的工具配对(open_tools_ 已被收账摘掉)。
    void DrawEvent(const runtime::ServerEvent& event, const std::optional<OpenTool>& completed_tool);
    void OnToolCompleted(const runtime::ServerEvent& event, const std::optional<OpenTool>& completed_tool);

    // 事件种类短标签(ui_trace 用)。
    static std::string TraceKindOf(const runtime::ServerEvent& event);

    // SSE 流内回调(send_stream 的流内 lambda)产生的事件就这几种:正文/
    // 思考的 ItemDelta、内置工具的 ItemStarted/ItemCompleted。全走投递路。
    static bool IsStreamOrigin(const runtime::ServerEvent& event);

    Ingredients ingredients_;
    std::map<std::string, OpenTool> open_tools_;
    // P2 收拢写者:调度器(可空)与旧路本地泵(可空 optional)二选一。
    // own_commit_mutex_ 是旧路泵的共享提交锁(声明先于 pump_,泵构造时
    // 借指针);调度器路径用调度器那把,这把闲着。
    SessionUiDispatcher* dispatcher_ = nullptr;
    std::uint64_t renderer_id_ = 0;         // 调度器渲染世代(旧路恒 0)
    bool stopped_ = false;                  // StopUiPump 已收(此后 Emit 就地画)
    std::recursive_mutex own_commit_mutex_;
    std::optional<UiEventPump> ui_pump_;
};

}  // namespace lubancode::app
