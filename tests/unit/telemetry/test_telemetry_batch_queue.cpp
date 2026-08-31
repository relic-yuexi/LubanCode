// 有界优先级 BatchQueue 测试(端云协同可观测单 §17.1/§17.2/§12.3,
// 实施分期 T1"bounded queue、优先级与 drop 账"):
//   - 帽与优先级:满队按 P3 → P2 → (来者 P0 时)P1 让位;
//   - P0 挤不出位 = 回绝 + emergency 账(§17.2 第 6 步);
//   - metrics 并系合并:同 stream 的旧快照被新快照吸收,coalesced 记账;
//   - drop 账:overflow/preempted 与分档各有数;
//   - Pop 顺序:P0 先走,同档 FIFO。
#include <doctest/doctest.h>

#include <string>
#include <vector>

#include "telemetry/batch_queue.hpp"

using namespace lubancode::telemetry;

namespace {

BatchItem MakeItem(const std::string& id, Priority priority) {
    BatchItem item;
    item.batch_id = id;
    item.priority = priority;
    item.workspace_key = "ws-000000000000";
    item.session_id = "s1";
    item.stream_id = "main.jsonl";
    item.first_event_id = "main-1:evt-00000001";
    item.last_event_id = "main-1:evt-0000000" + id.substr(id.size() - 1, 1);
    item.last_event_hash = "hash-" + id;
    TraceSpan span;
    span.trace_id = "0123456789abcdef0123456789abcdef";
    span.span_id = "0123456789abcdef";
    span.name = "lubancode.tool.execute";
    span.source_event_id = item.first_event_id;
    item.spans.push_back(std::move(span));
    return item;
}

BatchItem MakeMetrics(const std::string& id, const std::string& series,
                      std::uint64_t value) {
    BatchItem item;
    item.batch_id = id;
    item.priority = Priority::P2;
    item.workspace_key = "ws-000000000000";
    item.session_id = "s1";
    item.stream_id = "main.jsonl";
    item.last_event_id = "main-1:evt-0000000" + id.substr(id.size() - 1, 1);
    item.last_event_hash = "hash-" + id;
    MetricSample sample;
    sample.name = series;
    sample.value = value;
    item.metrics.push_back(std::move(sample));
    return item;
}

}  // namespace

TEST_CASE("帽与优先级:满队按 §17.2 次序让位") {
    BatchQueueOptions options;
    options.capacity_items = 2;
    options.capacity_bytes = 1024 * 1024;
    BatchQueue queue(options);

    CHECK(queue.TryPush(MakeItem("a1", Priority::P3)));
    CHECK(queue.TryPush(MakeItem("a2", Priority::P2)));
    // 再进一只 P1:挤掉 P3(最老的低档),账记 preempted。
    CHECK(queue.TryPush(MakeItem("a3", Priority::P1)));
    auto stats = queue.Stats();
    CHECK(stats.size_items == 2);
    CHECK(stats.dropped_preempted_total == 1);
    CHECK(stats.dropped_by_priority[static_cast<std::size_t>(Priority::P3)] == 1);
    // P0 进来,连 P2 也让位(§17.2 第 4 步:P1 只在来者是 P0 时被挤,
    // P2 属"普通样本"档,按老者先丢)。
    CHECK(queue.TryPush(MakeItem("a4", Priority::P0)));
    stats = queue.Stats();
    CHECK(stats.dropped_preempted_total == 2);
    CHECK(stats.size_items == 2);
}

TEST_CASE("P0 挤不出位 = 回绝 + emergency 账(§17.2 第 6 步)") {
    BatchQueueOptions options;
    options.capacity_items = 1;
    options.capacity_bytes = 1024 * 1024;
    BatchQueue queue(options);
    CHECK(queue.TryPush(MakeItem("b1", Priority::P0)));
    // 队里是 P0,来者也是 P0:谁也不让谁,回绝。
    CHECK_FALSE(queue.TryPush(MakeItem("b2", Priority::P0)));
    const auto stats = queue.Stats();
    CHECK(stats.emergency_reject_total == 1);
    CHECK(stats.dropped_overflow_total == 1);
    CHECK(stats.dropped_by_priority[static_cast<std::size_t>(Priority::P0)] == 1);
    // 队没被塞坏:原批还在。
    const auto item = queue.Pop();
    REQUIRE(item.has_value());
    CHECK(item->batch_id == "b1");
}

TEST_CASE("metrics 并系合并:同 stream 旧快照被吸收") {
    BatchQueue queue(BatchQueueOptions{});
    CHECK(queue.TryPush(MakeMetrics("m1", "lubancode.turn.completed_total", 3)));
    CHECK(queue.TryPush(MakeMetrics("m2", "lubancode.turn.completed_total", 7)));
    auto stats = queue.Stats();
    // 同系被新值顶掉 = 并系一次;老批退场不计 drop(数据没丢)。
    CHECK(stats.coalesced_series_total == 1);
    CHECK(stats.dropped_preempted_total == 0);
    CHECK(stats.size_items == 1);
    const auto item = queue.Pop();
    REQUIRE(item.has_value());
    REQUIRE(item->metrics.size() == 1);
    CHECK(item->metrics[0].value == 7);  // 累计快照,新值胜
    // 异系不并、同 stream 合批:每 stream 至多一只 metrics 批,异系被
    // 吸收进新批一起走(§17.2 第 1 步的完整语义)。
    CHECK(queue.TryPush(MakeMetrics("m3", "lubancode.model.tokens", 100)));
    CHECK(queue.TryPush(MakeMetrics("m4", "lubancode.turn.completed_total", 9)));
    stats = queue.Stats();
    CHECK(stats.coalesced_series_total == 1);  // 异系吸收不算并系账
    CHECK(stats.size_items == 1);
    const auto merged = queue.Pop();
    REQUIRE(merged.has_value());
    CHECK(merged->metrics.size() == 2);
}

TEST_CASE("Pop 顺序:P0 先走,同档 FIFO;字节帽同样触发收缩") {
    BatchQueue queue(BatchQueueOptions{});
    CHECK(queue.TryPush(MakeItem("c1", Priority::P2)));
    CHECK(queue.TryPush(MakeItem("c2", Priority::P0)));
    CHECK(queue.TryPush(MakeItem("c3", Priority::P2)));
    CHECK(queue.Pop()->batch_id == "c2");
    CHECK(queue.Pop()->batch_id == "c1");
    CHECK(queue.Pop()->batch_id == "c3");
    CHECK_FALSE(queue.Pop().has_value());

    BatchQueueOptions tiny;
    tiny.capacity_items = 64;
    const BatchItem probe = MakeItem("d0", Priority::P2);
    tiny.capacity_bytes = probe.EstimatedBytes() + probe.EstimatedBytes() / 2;
    BatchQueue bytes_queue(tiny);
    CHECK(bytes_queue.TryPush(MakeItem("d1", Priority::P2)));
    CHECK(bytes_queue.TryPush(MakeItem("d2", Priority::P1)));
    const auto stats = bytes_queue.Stats();
    CHECK(stats.size_items == 1);
    CHECK(stats.dropped_preempted_total == 1);
}
