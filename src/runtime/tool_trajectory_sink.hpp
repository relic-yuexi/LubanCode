// ToolTraceHub 的轨迹口(P0-2 轨迹接线,§15.2):
//   ToolTraceHub -> TrajectorySink(本口) -> Runtime EventSink(UI)
// 实现在 runtime/trajectory_session.hpp 的 TrajectoryTurnBridge;hub 只认
// 这只抽象口,不 include trajectory(依赖单向:hub -> 本口 <- 实现)。
#pragma once

#include <string>

#include "agent/tool_trace.hpp"
#include "api/types.hpp"

namespace lubancode::runtime {

class ToolTrajectorySink {
public:
    virtual ~ToolTrajectorySink() = default;
    // 一枚工具栅栏事件(Scheduled/ExecutionStarted/ExecutionFinished;
    // ResultCommitted 忽略——正文从 OnToolResultsCommitted 的消息翻)。
    virtual void OnToolTrace(const agent::ToolTraceEvent& event) = 0;
    // 批次尾:五枚结果收齐的 user 消息(tool.result.committed 的正文)。
    virtual void OnToolResultsCommitted(const std::string& batch_id, const api::Message& results) = 0;
    // 模型历史预览钩子(V3-REAL-05,真实会话审计棒一):工具结果消息压进
    // 运行时历史之前调——上面那只批次尾口在入史之后,归仓来不及。带结果
    // 仓的实现(轨迹 v3 桥)把超帽全文换成固定预览并就地归仓原文
    //(结果链 persisted→selected 也就地落,批次尾只补 tool 消息)。默认不
    // 动:没有原文别处可去的实现(v2 桥/子桥)保持原文入史,正文不丢。
    virtual void RewriteToolResultsForHistory(api::Message& tool_result_message) {
        (void)tool_result_message;
    }
    // started 落不住时问一句要不要拦执行(true = 拦;只读/副作用档的
    // 区分由 hub 按既有 ShouldBlockOnFailedStart 表裁,这里只答轨迹侧
    // 写没写住)。
    virtual bool ShouldBlockExecution(const agent::ToolTraceEvent& started) = 0;
};

}  // namespace lubancode::runtime
