// AgentSupervisorMetrics(《子代理监督器、agent_watch 与停滞恢复设计》
// §11.3,P2"能本地落的"):监督事件的低基数 OTel 指标聚合器。吃
// AgentSupervisionEvent(经 AgentHealthHookBus 或直喂),维护单子列的六枚
// 指标,输出 telemetry::MetricSample(可直接走 telemetry::
// EncodeMetricsRequest 编成 OTLP/HTTP JSON)。
//
// 六枚(单子 §11.3原名,按仓库遥测惯例加 lubancode. 前缀):
//   lubancode.agent.active{stage,health}            活任务按相位/健康计数
//   lubancode.agent.transport_idle_seconds          传输静默(观测窗内最大值)
//   lubancode.agent.progress_idle_seconds           实质进展静默(同上)
//   lubancode.agent.recovery_attempts_total{reason,outcome}
//   lubancode.agent.stall_total{layer,reason}
//   lubancode.agent.force_finalize_total{reason}
//
// 基数闸(单子 §11.3 红线):label 只许 stage/health/layer/reason/outcome
// 这批有界枚举与稳定码——task_id/title/tool_input/error_message 一律不做
// label。idle 两枚是"自聚合器起的观测最大值"(单调非降),与编码器的
// CUMULATIVE+isMonotonic 语义一致,不是瞬时 gauge。
//
// 线程纪律:Count() 在监督线程/任务线程被调,只拿自家小锁,不回拿台账锁;
// Snapshot() 会扫台账折 active 两枚(锁序:台账 -> 指标,与 Count 的方向
// 不交叉)。导出到 spool/OTLP 端点归遥测服务的 journal 投影路——本件只把
// 账记准、把形状对齐 OTLP,不自己起网络。
#pragma once

#include <cstdint>
#include <map>
#include <mutex>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "agent/agent_progress.hpp"      // AgentSupervisionEvent
#include "telemetry/contract.hpp"        // MetricSample:OTLP 侧的统一形状

namespace lubancode::tools {
class TaskLedger;
}

namespace lubancode::runtime {

class AgentSupervisorMetrics {
public:
    // ledger 用来折 active/idle 的现值(可为空 = 只记事件计数,不产 gauge)。
    explicit AgentSupervisorMetrics(const tools::TaskLedger* ledger) : ledger_(ledger) {}

    // 喂一枚监督事件(计数器面)。锁内只动自家账,不碰台账。
    void Count(const agent::AgentSupervisionEvent& event);

    // 稳定序快照:计数器 + 现值 gauge,按 (name, labels 字典序) 排序,
    // 同输入同输出(单测钉形状)。
    std::vector<telemetry::MetricSample> Snapshot() const;

    // 诊断口:累计喂入事件数。
    std::uint64_t events_counted() const;

private:
    const tools::TaskLedger* ledger_ = nullptr;
    mutable std::mutex mutex_;
    // 计数器账:键 = "name|label_k=v;..."(稳定拼接),值为计数。
    std::map<std::string, telemetry::MetricSample> counters_;
    // idle 观测最大值(Snapshot 是 const 读口,mutable 只在自家锁内动)。
    mutable std::uint64_t transport_idle_max_ms_ = 0;
    mutable std::uint64_t progress_idle_max_ms_ = 0;
    std::uint64_t events_counted_ = 0;

    void BumpCounter(const std::string& name, const nlohmann::json& labels);
};

}  // namespace lubancode::runtime
