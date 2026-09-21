// 轨迹 workflow 编排账桥(AR-12 机械拆分:自 trajectory_session.hpp 按桥类
// 边界拆出,类体与合同一字未动)。workflow 会话归属统一单:ReserveWorkflow
// Run/ReserveWorkflowNodeStream 的生产消费方。编排 Journal 一份(run_kind=
// workflow,只记编排事实);每个 node attempt 一份 node stream(run_kind=
// workflow_node,模型请求/工具事件经轮桥落账)。实现桥在 trajectory_
// workflow_bridge.cpp,开账装配是 TrajectorySessionLedger::SpawnWorkflowRun。
#pragma once

#include <cstdint>
#include <expected>
#include <memory>
#include <string>
#include <vector>

#include "runtime/trajectory_turn_bridge.hpp"

namespace lubancode::runtime {

// ---------------------------------------------------------------------------
// workflow 编排账(workflow 会话归属统一单):ReserveWorkflowRun/
// ReserveWorkflowNodeStream 的生产消费方。编排 Journal 一份(run_kind=
// workflow,只记编排事实);每个 node attempt 一份 node stream(run_kind=
// workflow_node,模型请求/工具事件经轮桥落账,ownership 与子代理同款:
// 声明才进、无主拒)。P0-C 同款合同:延迟开卷、独占创建、开张失败 fail
// closed 且不留 0 字节残留。
// ---------------------------------------------------------------------------

// 开张失败的结构化账(与 SubagentSpawnFailure 同款:阶段 + 稳定码 + 人话)。
struct WorkflowSpawnFailure {
    // 失败阶段:reserve_run | recorder_start | run_started(编排账);
    // reserve_stream | recorder_start | run_started(node 账)。
    std::string stage;
    std::string error_code;
    std::string detail;
    std::string reserved_run_id;  // workflow_run_id 或 node_run_id
    bool retryable = false;       // I/O 类可重试;schema/状态机类不可
};

// 一次 node attempt 的账:执行器经 turn_bridge 落模型/工具事件;Finish
// 收口回终态 hash(编排账的 node 终态事实引用它)。宿主合成调用(tool
// 节点不经模型声明)不进 tool.* 事件——ownership 门挡在桥里。
class TrajectoryWorkflowNodeBridge {
public:
    virtual ~TrajectoryWorkflowNodeBridge() = default;
    virtual const std::string& run_id() const = 0;  // node_run_id
    virtual TrajectoryTurnBridge& turn_bridge() = 0;
    // cancelled = run.cancelled 终态(取消不是失败);ok = run.completed。
    virtual std::string Finish(bool ok, bool cancelled, const std::string& reason) = 0;
};

// 一场 workflow run 的编排账。workflow runtime 只经这只口写编排事实;
// 执行器无权写 workflow.jsonl(§15.6)。
class TrajectoryWorkflowRunBridge {
public:
    struct DefinitionInfo {
        std::string workflow_id;
        std::string workflow_version;
        std::string content_hash;
        std::string cwd;
        std::string definition_json;  // 归一化快照(definition.json 原文)
    };

    virtual ~TrajectoryWorkflowRunBridge() = default;
    virtual const std::string& run_id() const = 0;  // workflow_run_id
    // 开张已落的定义快照(definition.json + workflow.definition.loaded)
    // 的 workflow_id——node 账首行引用它。
    virtual const std::string& workflow_id() const = 0;

    // ---- 编排事实(非关键投影:写不住记诊断,不拦运行)----
    virtual void RecordNodeWaiting(const std::string& node_id, const std::string& wait_kind,
                                   const std::string& body) = 0;
    virtual void RecordBranchStarted(const std::string& node_id,
                                     const std::vector<std::string>& branches, int concurrency) = 0;
    virtual void RecordJoinCompleted(const std::string& node_id, const std::string& join,
                                     int succeeded, int failed,
                                     const std::vector<std::string>& unavailable) = 0;
    virtual void RecordLoopIterationStarted(const std::string& node_id, int iteration) = 0;
    virtual void RecordLoopIterationCompleted(const std::string& node_id, int iteration,
                                              bool condition_met) = 0;

    // ---- 关键编排事实(写不住返回 false,调用方须停在明确失败态)----
    // node 派发:relations.child_run_id 引 node stream。
    virtual bool RecordNodeDispatched(const std::string& node_id, const std::string& node_run_id,
                                      int attempt, const std::string& node_kind, int map_item_index,
                                      const std::string& input_sha256) = 0;
    // node 终态(completed 带 outcome=success|empty;failed 带 code/error)。
    // child_terminal_event_hash 非空时 verifier 与 node 文件逐位对账。
    virtual bool RecordNodeCompleted(const std::string& node_id, const std::string& node_run_id,
                                     int attempt, const std::string& outcome, std::int64_t duration_ms,
                                     std::int64_t tokens, const std::string& agent_name,
                                     const std::string& output_sha256,
                                     const std::string& child_terminal_event_hash) = 0;
    virtual bool RecordNodeFailed(const std::string& node_id, const std::string& node_run_id,
                                  int attempt, const std::string& code, const std::string& error,
                                  std::int64_t duration_ms, std::int64_t tokens,
                                  const std::string& child_terminal_event_hash) = 0;
    virtual void RecordNodeSkipped(const std::string& node_id, const std::string& node_run_id,
                                   const std::string& reason) = 0;
    // 重试是编排事实但不是终态:attempt 收口在 node stream 自己的 run.failed。
    virtual void RecordNodeRetrying(const std::string& node_id, const std::string& node_run_id,
                                    int attempt, int max_attempts, const std::string& code) = 0;
    // at_seq 由账内取(已落事件数);store_sha256 是 Store 快照的内容寻址。
    virtual bool RecordCheckpointSaved(const std::string& store_sha256) = 0;

    // ---- node attempt 账开张(fail closed:失败即该节点不得执行)----
    virtual std::expected<std::unique_ptr<TrajectoryWorkflowNodeBridge>, WorkflowSpawnFailure>
    SpawnNodeStream(const std::string& node_id, const std::string& node_run_id, int attempt,
                    const std::string& node_kind, int map_item_index) = 0;

    // 编排账健康:关键事实写不住即 true(调度器据此停在明确失败态)。
    virtual bool broken() const = 0;

    // run 收口:run terminal + 关柄;回终态 hash(空 = 账坏,如实标注)。
    virtual std::string Finish(bool ok, bool cancelled, const std::string& reason) = 0;
};

}  // namespace lubancode::runtime
