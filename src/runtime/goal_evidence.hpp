// GoalEvidence 适配器(goal 单"合流接线"项):从 ToolTraceHub 的 canonical
// 事件里采证,喂 GoalCoordinator 的证据账。
//
// 单子的定案(goal 单"GoalEvidence"节):
//   - 证据由宿主从 tool trace、git 状态、测试结果、用户回答里摘;
//     assistant 正文不是证据。
//   - run_command 记 argv/command、cwd、exit code、duration、stdout/stderr
//     digest 与截断标记;write/edit 记路径、前后 hash、diff 摘要,不把密钥
//     或整份大文件抄进 goal 事件。
//   - MCP 记 server/tool/call id、结构化 result hash、error code。
//   - 证据涉及改动后,旧 validation 按 MarkEvidenceStale 翻 stale(调用方
//     在写盘级工具 finished 后把更早的 CommandExit/TestReport 证据标旧)。
//
// 本文件只做"一枚 ToolTraceEvent -> 一枚 GoalEvidence"的纯翻译,零 IO、
// 零锁;落账(coordinator.RecordEvidence)与事件行(goal_evidence_v1)由
// 装配层接。ev id 的发号也归装配层(这里收 next_id 参数,纯函数好测)。

#pragma once

#include <cstdint>
#include <optional>
#include <string>

#include "agent/tool_trace.hpp"
#include "runtime/goal_types.hpp"

namespace lubancode::runtime::goal {

// 采证的上下文:这枚 trace 事件钉在哪个 goal/iteration 上(装配层从
// 当前活跃的 goal iteration 拿;不在 goal turn 时不采)。
struct GoalEvidenceContext {
    std::string goal_id;
    std::string iteration_id;
    std::string turn_id;  // 填 GoalEvidence.tool_use_id 之外的对账字段用
};

// 从一枚 finished 的工具事件采证。
//   - 只认 ExecutionFinished:started/scheduled 阶段没有结果事实,采了
//     也是空壳(单子:"证据由宿主从 tool trace 里摘",摘的是结果)。
//   - 子代理内层事件(parent_execution_id 非空)也采,producer 标
//     "subagent"——单子:"子代理结果须带 child task id;仅'子代理说通过
//     了'仍是二级证据"。
//   - id_issuer:ev-<n> 的发号口(装配层给单调计数;测试给定值)。
// 返回 nullopt = 这枚事件不产证据(非 finished / 无 goal 上下文)。
std::optional<GoalEvidence> EvidenceFromToolTrace(const agent::ToolTraceEvent& event,
                                                  const GoalEvidenceContext& context,
                                                  const std::string& evidence_id);

// 证据种类按工具名分档(单子 EvidenceKind 表):
//   run_command          -> CommandExit(argv/cwd/exit/duration/stdout digest)
//   write_file/edit_file -> FileDigest(路径/前后 hash/行数)
//   其它 builtin/MCP/LSP -> ToolResult(工具名/结果 hash/字节数)
EvidenceKind EvidenceKindForTool(const std::string& tool_name, agent::ToolSourceKind source);

// 写盘级工具落完之后,哪些旧证据该翻 stale(单子:"证据涉及改动后,旧
// validation 要按影响范围翻 stale")。首版按证据种类收:FileDigest 的
// fresh 不动(它自己就是改动事实),CommandExit/TestReport 全标旧——
// 改动之后的旧验证不再可信,须重验。纯函数,装配层拿着 id 列表调
// coordinator.MarkEvidenceStale。
bool EvidenceStalesOnWrite(EvidenceKind kind);

}  // namespace lubancode::runtime::goal
