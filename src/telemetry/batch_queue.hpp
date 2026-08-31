// 有界优先级 BatchQueue(端云协同可观测架构与 Telemetry 插件设计单
// §17.1/§17.2,实施分期 T1"bounded queue、优先级与 drop 账")。
//
// P0 critical(error/终态/治理回执/spool 损坏)→ P3 verbose 四档(§17.1)。
// TryPush 永不阻塞、绝不反压业务线程(§17.2 末段):满了按 §17.2 次序
// 收缩——
//   1. 同 stream 的 metrics 批并系合并(累积值快照,新值胜旧值);
//   2. 丢 P3(旧者先丢);
//   3. 按"普通样本"待遇丢 P2(旧者先丢;采样权重档首版未落,旧先丢是
//      收窄的实现,账里如实记);
//   4. P1 只在来者是 P0 时让位(§17.2"P1 尽量挤掉低档"收窄面);
//   5. P0 仍进不去 → 回 false,调用方记 emergency counter 并让
//      /doctor telemetry 报红(§17.2 第 6 步)。
//
// 账目照 §12.3:size/capacity/dropped_total{reason,priority}/coalesced_total
// 全有数,/telemetry 与 /doctor telemetry 从本地读。
#pragma once

#include <array>
#include <cstdint>
#include <deque>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "telemetry/contract.hpp"

namespace lubancode::telemetry {

// §17.1 四档。数值即优先级序,P0 最高。
enum class Priority { P0 = 0, P1 = 1, P2 = 2, P3 = 3 };
const char* PriorityName(Priority value);

// 一只待发批:一个 stream 窗口的投影产物。traces 批带 spans,metrics 批
// 带累计快照(可并系合并);payload 保持合同对象形态,编码(OTLP JSON)
// 在出队落 spool 时做——生产者不做 IO,也不在热路反复编码。
struct BatchItem {
    std::string batch_id;  // generation 内确定(同窗口重放同 id,去重靠它)
    Priority priority = Priority::P2;
    std::string workspace_key;
    std::string session_id;
    std::string stream_id;
    std::string first_event_id;    // 本窗口首事件(账目用)
    std::string last_event_id;     // 本窗口末事件(cursor 推进对账)
    std::string last_event_hash;   // 末事件 hash(cursor 推进对账)
    bool final_window = false;     // 流收口窗(允许 terminal=missing 的 span)
    nlohmann::json resource_attributes = nlohmann::json::object();
    std::vector<TraceSpan> spans;      // traces 批
    std::vector<MetricSample> metrics;  // metrics 批(可并系)

    bool IsMetrics() const { return !metrics.empty(); }

    // 字节估计(帽的判据):按合同 JSON 的 dump 长度估,不预编码。
    std::uint64_t EstimatedBytes() const;
};

struct BatchQueueOptions {
    std::size_t capacity_items = 8192;     // §24.1 草案起点
    std::uint64_t capacity_bytes = 16 * 1024 * 1024;  // 16 MiB
};

// 每档的丢弃账:reason = overflow(来者被拒)/ preempted(在队者被挤)。
struct BatchQueueStats {
    std::size_t size_items = 0;
    std::size_t capacity_items = 0;
    std::uint64_t size_bytes = 0;
    std::uint64_t capacity_bytes = 0;
    std::uint64_t dropped_overflow_total = 0;   // 按 (reason=overflow) 计
    std::uint64_t dropped_preempted_total = 0;  // 按 (reason=preempted) 计
    std::array<std::uint64_t, 4> dropped_by_priority = {};  // 下标 = Priority
    std::uint64_t coalesced_series_total = 0;   // 并系合并掉的旧系次数
    std::uint64_t emergency_reject_total = 0;   // P0 进不去的回绝数(报红)

    nlohmann::json ToJson() const;
};

// 有界 MPSC:生产者(TelemetryService 投影侧)与消费者(spool 落盘侧)
// 各持一把小锁过队;TryPush/Pop 均 O(队列长) 最坏,常态近 O(1)。
class BatchQueue {
public:
    explicit BatchQueue(BatchQueueOptions options = BatchQueueOptions{});

    // 非阻塞投递。false = §17.2 走到第 6 步(P0 也进不去)或来者被拒,
    // 调用方按 emergency/overflow 记账——本类已记 emergency_reject_total。
    bool TryPush(BatchItem item);

    // 弹一只待落盘批(FIFO:同优先级先来先走,跨档 P0 先走)。
    std::optional<BatchItem> Pop();

    // 清空(关停收场用);回被清掉的批数。
    std::size_t DrainDiscard();

    BatchQueueStats Stats() const;
    std::size_t size() const;

private:
    // §17.2 收缩:给来者腾位。true = 腾出来了。
    bool MakeRoom(std::uint64_t incoming_bytes, Priority incoming);
    void AccountDrop(Priority priority, bool preempted);

    mutable std::mutex mutex_;
    BatchQueueOptions options_;
    std::deque<BatchItem> items_;
    std::uint64_t bytes_ = 0;
    BatchQueueStats stats_;
};

}  // namespace lubancode::telemetry
