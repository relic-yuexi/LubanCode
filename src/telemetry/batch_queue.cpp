// 有界优先级 BatchQueue 的实现。合同见 batch_queue.hpp 文件头。
//
// 跨档取件的实现:Pop 扫一遍队列取最高优先级最老者。队列常态近空
//(消费者周期排空),O(n) 扫不出事;不为四档各养一条 deque 添复杂度。
#include "telemetry/batch_queue.hpp"

#include <algorithm>
#include <utility>

namespace lubancode::telemetry {

const char* PriorityName(Priority value) {
    switch (value) {
        case Priority::P0:
            return "P0";
        case Priority::P1:
            return "P1";
        case Priority::P2:
            return "P2";
        case Priority::P3:
            return "P3";
    }
    return "P?";
}

namespace {

// 一只批里合同对象的 JSON 体积(帽的判据)。attributes 与 span 字段是
// 已脱敏的 D1 形态,尺寸有界,dump 便宜。
std::uint64_t DumpBytes(const BatchItem& item) {
    std::uint64_t bytes = 512;  // 批壳:身份/窗口/哈希等固定字段
    for (const TraceSpan& span : item.spans) {
        bytes += span.ToJson().dump().size();
    }
    for (const MetricSample& metric : item.metrics) {
        bytes += metric.ToJson().dump().size();
    }
    bytes += item.resource_attributes.dump().size();
    return bytes;
}

}  // namespace

std::uint64_t BatchItem::EstimatedBytes() const { return DumpBytes(*this); }

BatchQueue::BatchQueue(BatchQueueOptions options) : options_(options) {
    if (options_.capacity_items == 0) {
        options_.capacity_items = 1;
    }
    if (options_.capacity_bytes == 0) {
        options_.capacity_bytes = 1;
    }
}

void BatchQueue::AccountDrop(Priority priority, bool preempted) {
    const auto index = static_cast<std::size_t>(priority);
    stats_.dropped_by_priority[index] += 1;
    if (preempted) {
        stats_.dropped_preempted_total += 1;
    } else {
        stats_.dropped_overflow_total += 1;
    }
}

bool BatchQueue::MakeRoom(std::uint64_t incoming_bytes, Priority incoming) {
    // §17.2 次序:P3 → P2(旧者先丢)→ 仅当来者是 P0 时挤 P1。
    const auto evict_lowest = [&](Priority floor) {
        for (auto it = items_.begin(); it != items_.end(); ++it) {
            if (static_cast<int>(it->priority) >= static_cast<int>(floor)) {
                bytes_ -= std::min(bytes_, it->EstimatedBytes());
                AccountDrop(it->priority, true);
                items_.erase(it);
                return true;
            }
        }
        return false;
    };
    while (items_.size() >= options_.capacity_items || bytes_ + incoming_bytes > options_.capacity_bytes) {
        const bool evicted = evict_lowest(Priority::P3) || evict_lowest(Priority::P2) ||
                             (incoming == Priority::P0 && evict_lowest(Priority::P1));
        if (!evicted) {
            return false;
        }
    }
    return true;
}

bool BatchQueue::TryPush(BatchItem item) {
    const std::lock_guard<std::mutex> lock(mutex_);
    const std::uint64_t incoming_bytes = item.EstimatedBytes();

    // §17.2 第 1 步:同 stream 的 metrics 批并系合并(累积快照,新值胜)。
    // 合并后老批退场,来者带合并结果入队——队列里每 stream 至多一只
    // metrics 批,token delta 一类高频快照不再堆队。
    if (item.IsMetrics()) {
        for (auto it = items_.begin(); it != items_.end();) {
            if (it->IsMetrics() && it->workspace_key == item.workspace_key &&
                it->session_id == item.session_id && it->stream_id == item.stream_id) {
                for (const MetricSample& sample : it->metrics) {
                    const auto same_series = std::find_if(
                        item.metrics.begin(), item.metrics.end(),
                        [&sample](const MetricSample& candidate) {
                            return candidate.name == sample.name &&
                                   candidate.labels.dump() == sample.labels.dump();
                        });
                    if (same_series == item.metrics.end()) {
                        item.metrics.push_back(sample);
                    } else {
                        // 同系:来者是更新窗口的快照,新值胜;老系计并系账。
                        stats_.coalesced_series_total += 1;
                    }
                }
                // 窗口身份随来者(更新的窗口),首事件留老批的(合并批
                // 覆盖 [老首事件, 来者末事件])。老批退场不是丢数据,不计
                // drop 账——只有同系被新值顶掉才计并系账(上面已计)。
                item.first_event_id =
                    it->first_event_id.empty() ? item.first_event_id : it->first_event_id;
                item.batch_id = item.batch_id.empty() ? it->batch_id : item.batch_id;
                bytes_ -= std::min(bytes_, it->EstimatedBytes());
                it = items_.erase(it);
                continue;
            }
            ++it;
        }
    }

    if (!MakeRoom(incoming_bytes, item.priority)) {
        // §17.2 第 6 步:挤不出位。P0 记 emergency(报红);其余按 overflow 拒。
        AccountDrop(item.priority, false);
        if (item.priority == Priority::P0) {
            stats_.emergency_reject_total += 1;
        }
        return false;
    }
    bytes_ += incoming_bytes;
    items_.push_back(std::move(item));
    return true;
}

std::optional<BatchItem> BatchQueue::Pop() {
    const std::lock_guard<std::mutex> lock(mutex_);
    if (items_.empty()) {
        return std::nullopt;
    }
    // 最高优先级;同档最老(FIFO)。
    auto best = items_.begin();
    for (auto it = items_.begin(); it != items_.end(); ++it) {
        if (static_cast<int>(it->priority) < static_cast<int>(best->priority)) {
            best = it;
        }
    }
    BatchItem out = std::move(*best);
    bytes_ -= std::min(bytes_, out.EstimatedBytes());
    items_.erase(best);
    return out;
}

std::size_t BatchQueue::DrainDiscard() {
    const std::lock_guard<std::mutex> lock(mutex_);
    const std::size_t discarded = items_.size();
    for (const BatchItem& item : items_) {
        AccountDrop(item.priority, true);
    }
    items_.clear();
    bytes_ = 0;
    return discarded;
}

BatchQueueStats BatchQueue::Stats() const {
    const std::lock_guard<std::mutex> lock(mutex_);
    BatchQueueStats stats = stats_;
    stats.size_items = items_.size();
    stats.capacity_items = options_.capacity_items;
    stats.size_bytes = bytes_;
    stats.capacity_bytes = options_.capacity_bytes;
    return stats;
}

std::size_t BatchQueue::size() const {
    const std::lock_guard<std::mutex> lock(mutex_);
    return items_.size();
}

nlohmann::json BatchQueueStats::ToJson() const {
    nlohmann::json dropped = nlohmann::json::object();
    for (int i = 0; i < 4; ++i) {
        dropped[PriorityName(static_cast<Priority>(i))] =
            dropped_by_priority[static_cast<std::size_t>(i)];
    }
    nlohmann::json out = nlohmann::json::object();
    out["size_items"] = size_items;
    out["capacity_items"] = capacity_items;
    out["size_bytes"] = size_bytes;
    out["capacity_bytes"] = capacity_bytes;
    out["dropped_overflow_total"] = dropped_overflow_total;
    out["dropped_preempted_total"] = dropped_preempted_total;
    out["dropped_by_priority"] = std::move(dropped);
    out["coalesced_series_total"] = coalesced_series_total;
    out["emergency_reject_total"] = emergency_reject_total;
    return out;
}

}  // namespace lubancode::telemetry
