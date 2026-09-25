// Workflow 节点独立 v3 场(V3-GAP-05 棒二):llm/agent/skill 节点的每次
// nodeExecution attempt 开自己的 v3 session,不再共用一个混杂场。
//
// 设计合同(todos/Workflow接入SessionV3_编排账与节点恢复设计.todo):
//   §一  只有实际向模型发消息的节点才开 agent session——template/transform/
//        switch/tool/approval 一类宿主计算节点不伪造 system 与 assistant。
//   §三  节点场落 workflow-runs/<runId>/nodes/<nodeExecutionId>/sessions/
//        <sessionId>/<sessionId>.jsonl;父主线只持引用(§一"父主线只接收
//        明确选用的收据与报告")。
//   §六  agent = 一个 nodeExecution 的独立 session;llm = 独立轻量模型
//        session(不开工具循环,仍走完整请求/流式/usage 合同);skill 沿
//        llm 执行器,同一场。
//
// 场间关系按 v3 schema 合同落(trajectory-v3-schema.md §四):父 session
// 账写 subagent.spawn.requested + subagent.linked(childSessionRef.journalPath
// 指进 run 目录),节点场首行 system 的 systemMeta 带 cause=workflow_node 与
// nodeExecutionRef 五件(workflowRunId/nodeId/nodeExecutionId/attemptId/
// spawnEventRef)。/usage 的两代并账沿会话树递归,节点场的 usage 由此进
// 主账口径(V3-GAP-01 余项)。
//
// 与 v2 桥的关系:TrajectoryWorkflowNodeBridge 的执行器面(turn_bridge/
// Finish)原样复用——host_executors 零改动;本件是 v3 编排账模式下的节点
// 账提供者,旧 v2 SpawnNodeStream 路不再启(单事实源)。
#pragma once

#include <expected>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "runtime/trajectory_workflow_bridge.hpp"  // TrajectoryWorkflowNodeBridge/WorkflowSpawnFailure
#include "trajectory/v3/writer.hpp"
#include "workflow/account.hpp"  // NodeExecutionIdentity

namespace lubancode::runtime {
class TrajectorySessionLedger;
}

namespace lubancode::workflow {

// 节点场引用定义在 account.hpp(编排账 terminal 事件的投影材料;此处复用)。

// 开场材料:从宿主会话账本折出(v3 场才有;v2 场/无账给 nullopt,节点
// 照跑只是没有独立场——编排事实仍在账上)。
struct NodeSessionMaterial {
    trajectory::v3::V3Writer* parent_writer = nullptr;
    std::string parent_session_id;
    std::string parent_run_id;
    std::string workspace_key;
    std::filesystem::path parent_session_dir;

    static std::optional<NodeSessionMaterial> FromLedger(runtime::TrajectorySessionLedger* ledger);
};

// 一次开场的产品:桥(递给执行器)+ 引用(编排账收口带)。
struct NodeSessionSpawn {
    std::unique_ptr<runtime::TrajectoryWorkflowNodeBridge> bridge;
    NodeSessionRef ref;
};

// 一场 run 的节点场发号局:铸 session id(id 掺 nodeExecutionId+attempt
// 的内容哈希,跨进程恢复不撞号)、算 journalPath、开卷建桥。线程安全
// (map/parallel 的 worker 并发各开各的场;父账写者自带锁)。
class WorkflowNodeSessions {
public:
    WorkflowNodeSessions(NodeSessionMaterial material, std::filesystem::path run_dir,
                         std::string workflow_run_id);

    // 开一场节点 attempt 的独立 v3 场。失败给结构化账(调用方 fail
    // closed:场开不出,节点不执行)。
    std::expected<NodeSessionSpawn, runtime::WorkflowSpawnFailure> Open(
        const NodeExecutionIdentity& identity, const WorkflowNode& node, int attempt);

    // 诊断投影(测试与 /doctor 用;空 = 没有落账错误)。
    std::vector<std::string> recent_errors() const;

private:
    NodeSessionMaterial material_;
    std::filesystem::path run_dir_;
    std::string workflow_run_id_;
    mutable std::mutex errors_mutex_;
    std::vector<std::string> io_errors_;
    // 各节点场的错误汇(桥在 worker 线程写自己那份;run 收口后聚合读)。
    std::vector<std::shared_ptr<const std::vector<std::string>>> session_errors_;
};

}  // namespace lubancode::workflow
