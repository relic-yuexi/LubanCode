// outbox.hpp 的实现。
#include "app_server/outbox.hpp"

#include <utility>

namespace lubancode::app_server {

namespace {

// 锁内入队:溢出分型与溢出账都在这。
bool PushLocked(std::deque<OutboundEntry>& queue, std::size_t capacity, std::uint64_t& dropped,
                std::string line, bool must_keep) {
    if (queue.size() >= capacity) {
        if (!must_keep) {
            ++dropped;
            return false;
        }
        // 必保事件撞满:先从队头丢可丢的,腾一个位置。丢哪个都是丢,
        // 溢出账 +1(丢的也是事件,通报里要算数)。
        bool evicted = false;
        for (auto it = queue.begin(); it != queue.end(); ++it) {
            if (!it->must_keep) {
                queue.erase(it);
                ++dropped;
                evicted = true;
                break;
            }
        }
        if (!evicted) {
            // 全满必保(理论上只有容量极小时才会发生):必保事件自己也不能
            // 无限堆,丢弃并记账——绝不阻塞调用线程是死规矩。
            ++dropped;
            return false;
        }
    }
    queue.push_back(OutboundEntry{std::move(line), must_keep});
    return true;
}

}  // namespace

bool BoundedOutbox::Push(std::string line, bool must_keep) {
    bool pushed = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        pushed = PushLocked(queue_, capacity_, dropped_, std::move(line), must_keep);
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

bool BoundedOutbox::full() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return queue_.size() >= capacity_;
}

std::size_t BoundedOutbox::size() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return queue_.size();
}

}  // namespace lubancode::app_server
