// 轨迹子代理桥(AR-12 机械拆分:自 trajectory_session.hpp 按桥类边界拆出,
// 类体与合同一字未动)。AgentTool 派工时申请,子 loop 的边界与工具事件落
// 子账;SubagentSpawnFailure 是 SpawnSubagent 的结构化失败(装配层吞
// error() 是子代理空轨迹单第一因查不出的根,失败必须带阶段与稳定码过境)。
// 实现桥与开账装配在 trajectory_session.cpp(TrajectorySessionLedger 的
// 窄工厂),不在此件。
#pragma once

#include <string>

#include "runtime/trajectory_turn_bridge.hpp"

namespace lubancode::runtime {

// ---------------------------------------------------------------------------
// 子代理轨迹桥:AgentTool 派工时申请,子 loop 的边界与工具事件落子账
// ---------------------------------------------------------------------------

class TrajectorySubagentBridge {
public:
    virtual ~TrajectorySubagentBridge() = default;
    virtual const std::string& run_id() const = 0;
    virtual TrajectoryTurnBridge& turn_bridge() = 0;
    // 收口:run terminal + 关柄(§8.3 journal_sha256)。返回子账终态事件
    // 的 event_hash(父账 finished 边界引用它);账已坏给空串,父账如实
    // 标注。
    virtual std::string Finish(bool ok, const std::string& reason) = 0;
};

// ---------------------------------------------------------------------------
// 子代理空轨迹单 P0-A/P0-B:SpawnSubagent 的结构化失败(替代裸字符串——
// 装配层吞 error() 是这次第一因查不出的根,失败必须带阶段与稳定码过境)。
// ---------------------------------------------------------------------------
struct SubagentSpawnFailure {
    // 失败阶段:reserve_stream | recorder_start | run_started。
    std::string stage;
    // 稳定码(trajectory.subagent_stream / trajectory.subagent_recorder /
    // trajectory.subagent_run_started 前缀 + 底层码)。
    std::string error_code;
    // 字段级人话(schema 缺哪个字段、io 细节);不含子 prompt 正文与
    // 敏感绝对路径。
    std::string detail;
    std::string reserved_run_id;  // 已铸出的子 run id(失败前铸了就带上)
    bool retryable = false;      // I/O 类失败可重试;schema/状态机类不可
};

}  // namespace lubancode::runtime
