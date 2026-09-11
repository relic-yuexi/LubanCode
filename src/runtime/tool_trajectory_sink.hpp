// ToolTraceHub 的轨迹口(P0-2 轨迹接线,§15.2):
//   ToolTraceHub -> TrajectorySink(本口) -> Runtime EventSink(UI)
// 实现在 runtime/trajectory_session.hpp 的 TrajectoryTurnBridge;hub 只认
// 这只抽象口,不 include trajectory(依赖单向:hub -> 本口 <- 实现)。
#pragma once

#include <string>
#include <vector>

#include "agent/tool_trace.hpp"
#include "api/types.hpp"

namespace lubancode::runtime {

// 工具结果批次持久提交的回执(失败与恢复单 P1-A/FA-01):批次里每枚结果
// 各自走结果链(persisted → selected → tool 消息 → 接纳),回执把整批折成
// 三档——
//   Committed  全部落稳:模型上下文与溯源链都齐。
//   Degraded   约定降级:主账正文已保住(如 metadata 落盘失败 →
//              persist_failed 事件 + tool 消息照落),模型上下文完整,
//              selected/溯源链有缺口如实记在 degraded_codes——调用方按
//              降级合同放行,另查链。
//   Failed     硬失败:有任一枚的"模型可见 tool 消息"没写稳(结果仓开不
//              了、消息/接纳写失败)。结果已执行不可重做,主循环须停止后
//              续模型发送——不得拿内存里独有的结果当已提交输入再发请求。
struct ToolResultsCommitReceipt {
    enum class Status { Committed, Degraded, Failed };
    Status status = Status::Committed;
    // 首个硬失败的稳定码(Failed 时非空),如 tool.result.store_unavailable。
    std::string error_code;
    // 降级明细(稳定码逐枚列;Degraded 时至少一枚)。
    std::vector<std::string> degraded_codes;

    // 放行判定:Committed/Degraded 放行(降级是约定路径);Failed 拦。
    bool ok() const { return status != Status::Failed; }
};

class ToolTrajectorySink {
public:
    virtual ~ToolTrajectorySink() = default;
    // 一枚工具栅栏事件(Scheduled/ExecutionStarted/ExecutionFinished;
    // ResultCommitted 忽略——正文从 OnToolResultsCommitted 的消息翻)。
    virtual void OnToolTrace(const agent::ToolTraceEvent& event) = 0;
    // 批次尾:五枚结果收齐的 user 消息(tool.result.committed 的正文)。
    // 回执见 ToolResultsCommitReceipt;丢弃回执的旧调用方按"未设闸"走。
    virtual ToolResultsCommitReceipt OnToolResultsCommitted(const std::string& batch_id,
                                                            const api::Message& results) = 0;
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
