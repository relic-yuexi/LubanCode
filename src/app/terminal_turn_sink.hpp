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
//   - 线程约定与老回调一致:Run() 所在线程被调,本类不加锁。
#pragma once

#include <atomic>
#include <map>
#include <string>
#include <utility>

#include <nlohmann/json.hpp>

#include "runtime/event.hpp"
#include "runtime/event_sink.hpp"

namespace lubancode::cli {
class ToolDisplay;
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

    explicit TerminalTurnSink(Ingredients ingredients) : ingredients_(std::move(ingredients)) {}

    void Emit(const runtime::ServerEvent& event) override;

private:
    // 工具条目对账:item_id -> (tool_use_id, name)。ItemCompleted 的载荷不带
    // 工具名,从这里取;Cancelled 收口的条目从这里摘。
    struct OpenTool {
        std::string tool_use_id;
        std::string name;
    };

    void OnToolCompleted(const runtime::ServerEvent& event);

    Ingredients ingredients_;
    std::map<std::string, OpenTool> open_tools_;
};

}  // namespace lubancode::app
