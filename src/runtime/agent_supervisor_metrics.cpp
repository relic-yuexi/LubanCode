// agent_supervisor_metrics.hpp 的实现:事件 -> 六枚低基数指标。
#include "runtime/agent_supervisor_metrics.hpp"

#include <algorithm>
#include <chrono>
#include <utility>

#include "tools/task_ledger.hpp"  // ForEachAliveVitals:active/idle 的现值

namespace lubancode::runtime {

namespace {

// label 键的封闭表(基数闸):只许这几枚,task_id/title/tool_input/
// error_message 一律不许进(单子 §11.3)。Count/Snapshot 只用表内键。
constexpr const char* kLabelStage = "stage";
constexpr const char* kLabelHealth = "health";
constexpr const char* kLabelLayer = "layer";
constexpr const char* kLabelReason = "reason";
constexpr const char* kLabelOutcome = "outcome";

// 计数器账键:名字 + 排序后的 label 串,同一样本恒同键。
std::string CounterKey(const std::string& name, const nlohmann::json& labels) {
    std::string key = name;
    for (auto it = labels.begin(); it != labels.end(); ++it) {
        key += ";";
        key += it.key();
        key += "=";
        key += it->get<std::string>();
    }
    return key;
}

// 疑滞分layer(单子 §七的分型):健康 -> transport/tool/agent/quiet。
const char* StallLayerOf(agent::AgentHealthState health) {
    switch (health) {
        case agent::AgentHealthState::SuspectTransport:
            return "transport";
        case agent::AgentHealthState::SuspectTool:
            return "tool";
        case agent::AgentHealthState::SuspectAgent:
            return "agent";
        case agent::AgentHealthState::Quiet:
            return "quiet";
        case agent::AgentHealthState::Degraded:
            return "degraded";
        default:
            return "other";
    }
}

}  // namespace

void AgentSupervisorMetrics::BumpCounter(const std::string& name, const nlohmann::json& labels) {
    // 调用方已持 mutex_。
    const std::string key = CounterKey(name, labels);
    auto it = counters_.find(key);
    if (it == counters_.end()) {
        telemetry::MetricSample sample;
        sample.name = name;
        sample.labels = labels;
        sample.value = 1;
        counters_.emplace(key, std::move(sample));
        return;
    }
    it->second.value += 1;
}

void AgentSupervisorMetrics::Count(const agent::AgentSupervisionEvent& event) {
    std::lock_guard<std::mutex> lock(mutex_);
    ++events_counted_;
    switch (event.kind) {
        case agent::AgentSupervisionEventKind::RecoveryStarted:
            BumpCounter("lubancode.agent.recovery_attempts_total",
                        nlohmann::json{{kLabelOutcome, "started"},
                                       {kLabelReason, event.reason_code.empty() ? "unknown" : event.reason_code}});
            break;
        case agent::AgentSupervisionEventKind::RecoverySucceeded:
            BumpCounter("lubancode.agent.recovery_attempts_total",
                        nlohmann::json{{kLabelOutcome, "succeeded"},
                                       {kLabelReason, event.reason_code.empty() ? "unknown" : event.reason_code}});
            break;
        case agent::AgentSupervisionEventKind::RecoveryExhausted:
            BumpCounter("lubancode.agent.recovery_attempts_total",
                        nlohmann::json{{kLabelOutcome, "exhausted"},
                                       {kLabelReason, event.reason_code.empty() ? "unknown" : event.reason_code}});
            break;
        case agent::AgentSupervisionEventKind::HealthChanged: {
            // 疑滞计数(单子 §11.3 agent_stall_total):只数"降级进疑滞态"
            // 的翻页——恢复(回 Healthy/Terminal)不算新一次疑滞。
            const agent::AgentHealthState h = event.new_health;
            if (h == agent::AgentHealthState::Quiet || h == agent::AgentHealthState::SuspectTransport ||
                h == agent::AgentHealthState::SuspectTool || h == agent::AgentHealthState::SuspectAgent ||
                h == agent::AgentHealthState::Degraded) {
                BumpCounter("lubancode.agent.stall_total",
                            nlohmann::json{{kLabelLayer, StallLayerOf(h)},
                                           {kLabelReason,
                                            event.reason_code.empty() ? "unknown" : event.reason_code}});
            }
            break;
        }
        case agent::AgentSupervisionEventKind::ToolIndeterminate:
            BumpCounter("lubancode.agent.stall_total",
                        nlohmann::json{{kLabelLayer, "tool"}, {kLabelReason, "tool.outcome_indeterminate"}});
            break;
        case agent::AgentSupervisionEventKind::ForceFinalized:
            BumpCounter("lubancode.agent.force_finalize_total",
                        nlohmann::json{
                            {kLabelReason, event.reason_code.empty() ? "unknown" : event.reason_code}});
            break;
    }
}

std::vector<telemetry::MetricSample> AgentSupervisorMetrics::Snapshot() const {
    // 现值 gauge 从台账折(锁序:台账 -> 指标;Count() 只拿指标锁,方向不交叉)。
    std::map<std::string, std::uint64_t> active;
    std::uint64_t transport_idle_max_ms = 0;
    std::uint64_t progress_idle_max_ms = 0;
    if (ledger_ != nullptr) {
        ledger_->ForEachAliveVitals([&](const std::shared_ptr<tools::TaskRecord>&,
                                        const agent::TaskVitals& vitals) {
            const std::string key = std::string(agent::SupervisionStageTag(vitals.stage)) + "|" +
                                    agent::HealthTag(vitals.health);
            ++active[key];
            if (vitals.has_transport) {
                const auto idle = std::chrono::duration_cast<std::chrono::milliseconds>(
                                      vitals.now - vitals.last_transport_at)
                                      .count();
                if (idle > static_cast<std::int64_t>(transport_idle_max_ms)) {
                    transport_idle_max_ms = static_cast<std::uint64_t>(idle < 0 ? 0 : idle);
                }
            }
            if (vitals.has_progress) {
                const auto idle = std::chrono::duration_cast<std::chrono::milliseconds>(
                                      vitals.now - vitals.last_meaningful_progress_at)
                                      .count();
                if (idle > static_cast<std::int64_t>(progress_idle_max_ms)) {
                    progress_idle_max_ms = static_cast<std::uint64_t>(idle < 0 ? 0 : idle);
                }
            }
        });
    }
    std::lock_guard<std::mutex> lock(mutex_);
    // idle 两枚是观测最大值(单调非降),与编码器的 CUMULATIVE 语义一致。
    if (transport_idle_max_ms > transport_idle_max_ms_) {
        transport_idle_max_ms_ = transport_idle_max_ms;
    }
    if (progress_idle_max_ms > progress_idle_max_ms_) {
        progress_idle_max_ms_ = progress_idle_max_ms;
    }
    std::vector<telemetry::MetricSample> out;
    out.reserve(counters_.size() + active.size() + 2);
    for (const auto& [key, sample] : counters_) {
        (void)key;
        out.push_back(sample);
    }
    for (const auto& [key, count] : active) {
        const std::size_t cut = key.find('|');
        telemetry::MetricSample sample;
        sample.name = "lubancode.agent.active";
        sample.labels = nlohmann::json{{kLabelStage, key.substr(0, cut)}, {kLabelHealth, key.substr(cut + 1)}};
        sample.value = count;
        out.push_back(std::move(sample));
    }
    {
        telemetry::MetricSample sample;
        sample.name = "lubancode.agent.transport_idle_seconds";
        sample.labels = nlohmann::json::object();
        sample.value = transport_idle_max_ms_ / 1000;
        out.push_back(std::move(sample));
    }
    {
        telemetry::MetricSample sample;
        sample.name = "lubancode.agent.progress_idle_seconds";
        sample.labels = nlohmann::json::object();
        sample.value = progress_idle_max_ms_ / 1000;
        out.push_back(std::move(sample));
    }
    // 稳定序:(name, labels dump) 字典序——同输入同输出。
    std::sort(out.begin(), out.end(), [](const telemetry::MetricSample& a, const telemetry::MetricSample& b) {
        if (a.name != b.name) {
            return a.name < b.name;
        }
        return a.labels.dump() < b.labels.dump();
    });
    return out;
}

std::uint64_t AgentSupervisorMetrics::events_counted() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return events_counted_;
}

}  // namespace lubancode::runtime
