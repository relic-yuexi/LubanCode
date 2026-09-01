// P2 监督遥测的单元测试(监督器单 §11.2/§11.3):
//   - AgentHealthHookBus:发布只入队、订阅者在派发线程收到、坏钩子(抛
//     异常)不杀总线也不漏下一枚、有界队列在下游挂住时丢最老并计数;
//   - AgentSupervisorMetrics:单子六枚指标的形状与计数(名字/label/值),
//     基数闸——task_id/title/tool_input/error_message 一律不进 label;
//   - 监督器接线:健康翻页(经台账 ApplyHealth 的 sink)既进钩子总线也进
//     指标;台账侧事件(恢复链)同一条路;慢钩子不卡监督拍。

#include <doctest/doctest.h>

#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <nlohmann/json.hpp>

#include "agent/agent_progress.hpp"
#include "runtime/agent_health_hooks.hpp"
#include "runtime/agent_supervisor.hpp"
#include "runtime/agent_supervisor_metrics.hpp"
#include "tools/task_ledger.hpp"

using namespace lubancode;

namespace {

using Clock = std::chrono::steady_clock;

std::shared_ptr<tools::TaskRecord> MakeTask(tools::TaskLedger& ledger, const std::string& title) {
    tools::AgentTaskSnapshot snapshot;
    snapshot.title = title;
    snapshot.prompt = "test";
    snapshot.start_time = Clock::now();
    return ledger.Register(std::move(snapshot));
}

agent::AgentSupervisionEvent MakeEvent(agent::AgentSupervisionEventKind kind, const std::string& reason) {
    agent::AgentSupervisionEvent event;
    event.kind = kind;
    event.task_id = 7;
    event.reason_code = reason;
    return event;
}

const telemetry::MetricSample* FindMetric(const std::vector<telemetry::MetricSample>& metrics,
                                          const std::string& name, const nlohmann::json& labels) {
    for (const auto& metric : metrics) {
        if (metric.name != name) {
            continue;
        }
        if (labels.is_null() ? metric.labels.empty() : metric.labels.dump() == labels.dump()) {
            return &metric;
        }
    }
    return nullptr;
}

// 六枚指标的名字(单子 §11.3 原名,按仓库遥测惯例加 lubancode. 前缀)。
constexpr const char* kActive = "lubancode.agent.active";
constexpr const char* kTransportIdle = "lubancode.agent.transport_idle_seconds";
constexpr const char* kProgressIdle = "lubancode.agent.progress_idle_seconds";
constexpr const char* kRecoveryAttempts = "lubancode.agent.recovery_attempts_total";
constexpr const char* kStall = "lubancode.agent.stall_total";
constexpr const char* kForceFinalize = "lubancode.agent.force_finalize_total";

agent::SupervisionThresholds FastThresholds() {
    agent::SupervisionThresholds thresholds;
    thresholds.first_byte_soft_secs = 1;
    thresholds.streaming_soft_secs = 1;
    thresholds.tool_soft_secs = 1;
    thresholds.exec_idle_soft_secs = 1;
    thresholds.stale_notice_rounds = 2;
    thresholds.stale_fail_rounds = 3;
    return thresholds;
}

}  // namespace

TEST_CASE("AgentHealthHookBus:只读投递,坏钩子不杀总线不漏下一枚") {
    runtime::AgentHealthHookBus bus;
    std::mutex received_mutex;
    std::vector<agent::AgentSupervisionEvent> received;
    std::atomic<int> thrower_hits{0};

    bus.Subscribe([&](const agent::AgentSupervisionEvent& event) {
        thrower_hits.fetch_add(1);
        throw std::runtime_error("bad hook");
    });
    bus.Subscribe([&](const agent::AgentSupervisionEvent& event) {
        std::lock_guard<std::mutex> lock(received_mutex);
        received.push_back(event);
    });

    // 发布方只入队:先同步派发一批钉行为,不赌派发线程的时序(派发线程与
    // DrainForTest 可能分食批次,断言只认成员不认次序)。
    bus.Publish(MakeEvent(agent::AgentSupervisionEventKind::RecoveryStarted, "network.error"));
    bus.Publish(MakeEvent(agent::AgentSupervisionEventKind::RecoverySucceeded, "transport.recovered"));
    bus.DrainForTest();
    {
        std::lock_guard<std::mutex> lock(received_mutex);
        REQUIRE(received.size() == 2);
        bool saw_started = false;
        bool saw_succeeded = false;
        for (const auto& event : received) {
            saw_started = saw_started ||
                          (event.kind == agent::AgentSupervisionEventKind::RecoveryStarted &&
                           event.reason_code == "network.error");
            saw_succeeded = saw_succeeded || event.kind == agent::AgentSupervisionEventKind::RecoverySucceeded;
        }
        CHECK(saw_started);
        CHECK(saw_succeeded);
    }
    CHECK(thrower_hits.load() == 2);  // 坏钩子被叫过,但没杀总线
}

TEST_CASE("AgentHealthHookBus:下游挂住时队列有界,丢最老并计数") {
    runtime::AgentHealthHookBus bus;
    std::atomic<bool> release{false};
    std::atomic<int> hits{0};
    bus.Subscribe([&](const agent::AgentSupervisionEvent&) {
        hits.fetch_add(1);
        while (!release.load(std::memory_order_acquire)) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    });
    // 先发一枚,等派发线程吃进第一批并堵在钩子里——之后的入队没人消化,
    // 队列帽 256 即触顶,丢最老。
    bus.Publish(MakeEvent(agent::AgentSupervisionEventKind::HealthChanged, "seed"));
    bool blocked = false;
    for (int i = 0; i < 300; ++i) {
        if (hits.load(std::memory_order_acquire) >= 1) {
            blocked = true;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    REQUIRE(blocked);
    for (int i = 0; i < 300; ++i) {
        bus.Publish(MakeEvent(agent::AgentSupervisionEventKind::HealthChanged, "x" + std::to_string(i)));
    }
    CHECK(bus.dropped_events() == 300 - 256);  // 恰好丢最老 44 枚
    release.store(true, std::memory_order_release);
}

TEST_CASE("AgentSupervisorMetrics:六枚指标形状与计数,基数闸不认任务内容") {
    tools::TaskLedger ledger;
    runtime::AgentSupervisorMetrics metrics(&ledger);

    metrics.Count(MakeEvent(agent::AgentSupervisionEventKind::RecoveryStarted, "network.error"));
    metrics.Count(MakeEvent(agent::AgentSupervisionEventKind::RecoveryStarted, "network.error"));
    metrics.Count(MakeEvent(agent::AgentSupervisionEventKind::RecoverySucceeded, "transport.recovered"));
    metrics.Count(MakeEvent(agent::AgentSupervisionEventKind::RecoveryExhausted, "http.503"));
    {
        // 疑滞只数"降级进疑滞态"的翻页:new_health 得真是 Suspect*。
        auto stall = MakeEvent(agent::AgentSupervisionEventKind::HealthChanged, "transport.stream_quiet");
        stall.old_health = agent::AgentHealthState::Healthy;
        stall.new_health = agent::AgentHealthState::SuspectTransport;
        metrics.Count(stall);
        // 回 Healthy 的翻页(恢复)不算新一次疑滞。
        auto recovered = MakeEvent(agent::AgentSupervisionEventKind::HealthChanged, "progress.resumed");
        recovered.old_health = agent::AgentHealthState::SuspectTransport;
        recovered.new_health = agent::AgentHealthState::Healthy;
        metrics.Count(recovered);
    }
    metrics.Count(MakeEvent(agent::AgentSupervisionEventKind::ForceFinalized, "wall_clock.force_finalized"));
    metrics.Count(MakeEvent(agent::AgentSupervisionEventKind::ToolIndeterminate, "tool.outcome_indeterminate"));

    const auto snapshot = metrics.Snapshot();
    // 恢复计数:started×2 / succeeded / exhausted 各一枚,label 只有 reason+outcome。
    REQUIRE(FindMetric(snapshot, kRecoveryAttempts,
                       nlohmann::json{{"outcome", "started"}, {"reason", "network.error"}}) != nullptr);
    CHECK(FindMetric(snapshot, kRecoveryAttempts,
                     nlohmann::json{{"outcome", "started"}, {"reason", "network.error"}})->value == 2);
    REQUIRE(FindMetric(snapshot, kRecoveryAttempts,
                       nlohmann::json{{"outcome", "succeeded"}, {"reason", "transport.recovered"}}) != nullptr);
    CHECK(FindMetric(snapshot, kRecoveryAttempts,
                     nlohmann::json{{"outcome", "succeeded"}, {"reason", "transport.recovered"}})->value == 1);
    REQUIRE(FindMetric(snapshot, kRecoveryAttempts,
                       nlohmann::json{{"outcome", "exhausted"}, {"reason", "http.503"}}) != nullptr);
    CHECK(FindMetric(snapshot, kRecoveryAttempts,
                     nlohmann::json{{"outcome", "exhausted"}, {"reason", "http.503"}})->value == 1);
    // 疑滞:transport 层一条 + 工具不明一条(归 tool 层)。
    REQUIRE(FindMetric(snapshot, kStall,
                       nlohmann::json{{"layer", "transport"}, {"reason", "transport.stream_quiet"}}) != nullptr);
    CHECK(FindMetric(snapshot, kStall,
                     nlohmann::json{{"layer", "transport"}, {"reason", "transport.stream_quiet"}})->value == 1);
    REQUIRE(FindMetric(snapshot, kStall,
                       nlohmann::json{{"layer", "tool"}, {"reason", "tool.outcome_indeterminate"}}) != nullptr);
    CHECK(FindMetric(snapshot, kStall,
                     nlohmann::json{{"layer", "tool"}, {"reason", "tool.outcome_indeterminate"}})->value == 1);
    // 强收:按因计数。
    REQUIRE(FindMetric(snapshot, kForceFinalize,
                       nlohmann::json{{"reason", "wall_clock.force_finalized"}}) != nullptr);
    CHECK(FindMetric(snapshot, kForceFinalize,
                     nlohmann::json{{"reason", "wall_clock.force_finalized"}})->value == 1);

    // 基数闸(单子 §11.3 红线):高基数字段一律不进 label。
    for (const auto& metric : snapshot) {
        for (auto it = metric.labels.begin(); it != metric.labels.end(); ++it) {
            CHECK(it.key().find("task_id") == std::string::npos);
            CHECK(it.key().find("title") == std::string::npos);
            CHECK(it.key().find("tool_input") == std::string::npos);
            CHECK(it.key().find("error_message") == std::string::npos);
        }
    }

    // idle 两枚:无活任务时是 0;活任务有过传输与进展后再静默,取观测最大值
    //(单调非降;从未收过传输/进展的任务不算静默)。
    CHECK(FindMetric(snapshot, kTransportIdle, nlohmann::json())->value == 0);
    CHECK(FindMetric(snapshot, kProgressIdle, nlohmann::json())->value == 0);
    const auto task = MakeTask(ledger, "有静默的任务");
    ledger.RecordRequestStarted(task, 1, "hash");  // 置 AwaitingFirstByte 相位
    ledger.RecordTransportActivity(task);          // 有过传输
    ledger.RecordAssistantMessage(task, agent::FingerprintOfParts("msg", "first"));  // 有过实质进展
    std::this_thread::sleep_for(std::chrono::milliseconds(1200));
    const auto snapshot2 = metrics.Snapshot();
    // 相位随最后一笔消息提交翻到 awaiting_tool_input_complete(RecordAssistantMessage)。
    REQUIRE(FindMetric(snapshot2, kActive,
                       nlohmann::json{{"stage", "awaiting_tool_input_complete"}, {"health", "healthy"}}) != nullptr);
    CHECK(FindMetric(snapshot2, kActive,
                     nlohmann::json{{"stage", "awaiting_tool_input_complete"}, {"health", "healthy"}})->value == 1);
    CHECK(FindMetric(snapshot2, kTransportIdle, nlohmann::json())->value >= 1);
    CHECK(FindMetric(snapshot2, kProgressIdle, nlohmann::json())->value >= 1);
    // 快照稳定序:同输入同输出。
    const auto snapshot3 = metrics.Snapshot();
    REQUIRE(snapshot2.size() == snapshot3.size());
    for (std::size_t i = 0; i < snapshot2.size(); ++i) {
        CHECK(snapshot2[i].name == snapshot3[i].name);
        CHECK(snapshot2[i].labels.dump() == snapshot3[i].labels.dump());
    }
}

TEST_CASE("监督器接线:健康翻页与台账侧事件既进钩子总线也进指标") {
    tools::TaskLedger ledger;
    runtime::AgentSupervisor supervisor(ledger);
    supervisor.SetThresholds(FastThresholds());

    std::mutex received_mutex;
    std::vector<agent::AgentSupervisionEvent> received;
    supervisor.health_hooks().Subscribe([&](const agent::AgentSupervisionEvent& event) {
        std::lock_guard<std::mutex> lock(received_mutex);
        received.push_back(event);
    });
    const auto wait_for_events = [&](std::size_t at_least) {
        for (int i = 0; i < 300; ++i) {
            {
                std::lock_guard<std::mutex> lock(received_mutex);
                if (received.size() >= at_least) {
                    return true;
                }
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        return false;
    };

    const auto task = MakeTask(ledger, "监督接线");
    supervisor.WatchTask(task);
    ledger.RecordRequestStarted(task, 1, "hash");
    std::this_thread::sleep_for(std::chrono::milliseconds(1100));
    supervisor.TickHealthForTest();
    // 健康翻页(疑似断流)经台账 sink 进了总线。
    REQUIRE(wait_for_events(1));
    {
        std::lock_guard<std::mutex> lock(received_mutex);
        REQUIRE_FALSE(received.empty());
        CHECK(received.front().kind == agent::AgentSupervisionEventKind::HealthChanged);
        CHECK(received.front().new_health == agent::AgentHealthState::SuspectTransport);
        CHECK(received.front().task_id == task->snapshot.id);
    }
    // 也进了指标(stall_total 的 transport 层)。
    REQUIRE(FindMetric(supervisor.MetricsSnapshot(), kStall,
                       nlohmann::json{{"layer", "transport"}, {"reason", "transport.first_byte_quiet"}}) != nullptr);

    // 台账侧事件(恢复链)同一条路:重试 -> started,成功 -> succeeded。
    ledger.RecordRequestRetry(task, 2, "network.error");
    ledger.RecordRequestOutcome(task, true, std::string());
    REQUIRE(wait_for_events(3));
    {
        std::lock_guard<std::mutex> lock(received_mutex);
        bool saw_started = false;
        bool saw_succeeded = false;
        for (const auto& event : received) {
            saw_started = saw_started || event.kind == agent::AgentSupervisionEventKind::RecoveryStarted;
            saw_succeeded = saw_succeeded || event.kind == agent::AgentSupervisionEventKind::RecoverySucceeded;
        }
        CHECK(saw_started);
        CHECK(saw_succeeded);
    }
    REQUIRE(FindMetric(supervisor.MetricsSnapshot(), kRecoveryAttempts,
                       nlohmann::json{{"outcome", "started"}, {"reason", "network.error"}}) != nullptr);
    supervisor.RequestStop();
}

TEST_CASE("钩子慢不卡监督拍:慢订阅者在派发线程上睡,监督器照常翻账") {
    tools::TaskLedger ledger;
    runtime::AgentSupervisor supervisor(ledger);
    std::atomic<int> slow_hits{0};
    supervisor.health_hooks().Subscribe([&](const agent::AgentSupervisionEvent&) {
        slow_hits.fetch_add(1);
        std::this_thread::sleep_for(std::chrono::milliseconds(300));  // 慢钩子
    });
    const auto task = MakeTask(ledger, "慢钩子下的任务");
    supervisor.WatchTask(task);
    // 台账侧事件先灌一枚,让总线线程开始消化慢钩子。
    ledger.RecordRequestRetry(task, 2, "network.error");
    bool hooked = false;
    for (int i = 0; i < 200; ++i) {
        if (slow_hits.load(std::memory_order_acquire) >= 1) {
            hooked = true;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    REQUIRE(hooked);  // 慢钩子正睡在派发线程上
    // 此刻监督拍照常跑(另一条线程),台账照常翻健康,不被背压卡住。
    ledger.ApplyHealth(task, agent::AgentHealthState::Quiet, "smoke.test");
    supervisor.TickHealthForTest();
    CHECK(ledger.ProgressOf(task->snapshot.id).health == agent::AgentHealthState::Quiet);
    supervisor.RequestStop();
}

// LocalVariables:
// fill-column: 100
// End:
