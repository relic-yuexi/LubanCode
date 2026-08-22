// outbox.hpp 的实现。
#include "app_server/outbox.hpp"

#include <optional>
#include <utility>

#include <nlohmann/json.hpp>

#include "platform/json_safe.hpp"

namespace lubancode::app_server {

namespace {

// 两条已序列化的 item/delta 行能不能合并:method 都是 item/delta、itemId
// 相同。能就把新 delta 拼到旧行屁股上,seq 取新行的(seq 单调,后到的
// 大),其余字段(旧行的)不动。返回 nullopt = 合不了(不是 delta /
// itemId 不同 / 行解析失败——解析失败按合不了走丢事件的路,绝不因
// 合并把出站行搞坏)。
//
// 只并队尾最后一条:delta 的顺序语义是追加,并到中间会与后续事件交错;
// 队尾那条正是"还没发走的最后一段增量",并它不乱序。
std::optional<std::string> CoalesceDeltaPair(const std::string& queued, const std::string& incoming) {
    nlohmann::json old_json;
    nlohmann::json new_json;
    try {
        old_json = nlohmann::json::parse(queued);
        new_json = nlohmann::json::parse(incoming);
    } catch (const nlohmann::json::exception&) {
        return std::nullopt;
    }
    if (!old_json.is_object() || !new_json.is_object()) {
        return std::nullopt;
    }
    const auto old_method = old_json.find("method");
    const auto new_method = new_json.find("method");
    if (old_method == old_json.end() || new_method == new_json.end() || !old_method->is_string() ||
        !new_method->is_string() || *old_method != std::string(kEventItemDelta) ||
        *new_method != std::string(kEventItemDelta)) {
        return std::nullopt;
    }
    const auto old_params = old_json.find("params");
    const auto new_params = new_json.find("params");
    if (old_params == old_json.end() || new_params == new_json.end() || !old_params->is_object() ||
        !new_params->is_object()) {
        return std::nullopt;
    }
    const auto old_item = old_params->find("itemId");
    const auto new_item = new_params->find("itemId");
    if (old_item == old_params->end() || new_item == new_params->end() || !old_item->is_string() ||
        !new_item->is_string() || *old_item != *new_item) {
        return std::nullopt;
    }
    const auto old_delta = old_params->find("delta");
    const auto new_delta = new_params->find("delta");
    if (old_delta == old_params->end() || new_delta == new_params->end() || !old_delta->is_string() ||
        !new_delta->is_string()) {
        return std::nullopt;
    }
    nlohmann::json merged = std::move(old_json);
    // 两个 delta 都是字符串(上面验过),取值拼接。
    merged["params"]["delta"] = old_delta->get<std::string>() + new_delta->get<std::string>();
    // seq 盖新行的(合并后的增量以最后一段的序号计,前端按 seq 查漏时
    // 这枚 seq 对应的增量内容是完整的)。
    const auto new_seq = new_params->find(kSeqField);
    if (new_seq != new_params->end()) {
        merged["params"][kSeqField] = *new_seq;
    }
    // 出站行的底线:永远可解析(DumpJsonSanitized 洗坏 UTF-8,绝不抛)。
    return platform::DumpJsonSanitized(merged);
}

}  // namespace

bool BoundedOutbox::Push(std::string line, bool must_keep) {
    bool pushed = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (queue_.size() >= capacity_ && !must_keep) {
            // 撞满的可丢事件:先试并进队尾同 item 的 delta(不丢内容、
            // 不动队列长度)。救不下再丢。
            if (!queue_.empty() && !queue_.back().must_keep) {
                if (std::optional<std::string> merged =
                        CoalesceDeltaPair(queue_.back().line, line)) {
                    queue_.back().line = std::move(*merged);
                    ++coalesced_;
                    cv_.notify_all();
                    return true;
                }
            }
            ++dropped_;
            cv_.notify_all();
            return false;
        }
        if (queue_.size() >= capacity_ && must_keep) {
            // 必保事件撞满:先从队头丢可丢的,腾一个位置。丢哪个都是丢,
            // 溢出账 +1(丢的也是事件,通报里要算数)。
            bool evicted = false;
            for (auto it = queue_.begin(); it != queue_.end(); ++it) {
                if (!it->must_keep) {
                    queue_.erase(it);
                    ++dropped_;
                    evicted = true;
                    break;
                }
            }
            if (!evicted) {
                // 全满必保(理论上只有容量极小时才会发生):必保事件自己
                // 也不能无限堆,丢弃并记账——绝不阻塞调用线程是死规矩。
                ++dropped_;
                cv_.notify_all();
                return false;
            }
        }
        queue_.push_back(OutboundEntry{std::move(line), must_keep});
        pushed = true;
    }
    // 锁外唤醒:睡着的写线程该干活了(丢了也唤——它好重新算队况)。
    cv_.notify_all();
    return pushed;
}

std::optional<std::string> BoundedOutbox::Pop() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (queue_.empty()) {
        return std::nullopt;
    }
    std::string line = std::move(queue_.front().line);
    queue_.pop_front();
    return line;
}

std::optional<std::string> BoundedOutbox::PopWait(std::chrono::milliseconds timeout) {
    std::unique_lock<std::mutex> lock(mutex_);
    cv_.wait_for(lock, timeout, [this] { return !queue_.empty(); });
    if (queue_.empty()) {
        return std::nullopt;
    }
    std::string line = std::move(queue_.front().line);
    queue_.pop_front();
    return line;
}

void BoundedOutbox::Notify() {
    std::lock_guard<std::mutex> lock(mutex_);
    cv_.notify_all();
}

std::vector<std::string> BoundedOutbox::PopAll() {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<std::string> lines;
    lines.reserve(queue_.size());
    for (auto& entry : queue_) {
        lines.push_back(std::move(entry.line));
    }
    queue_.clear();
    return lines;
}

std::uint64_t BoundedOutbox::dropped() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return dropped_;
}

std::uint64_t BoundedOutbox::coalesced() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return coalesced_;
}

bool BoundedOutbox::full() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return queue_.size() >= capacity_;
}

std::size_t BoundedOutbox::size() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return queue_.size();
}

}  // namespace lubancode::app_server
