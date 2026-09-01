// agent_health_hooks.hpp 的实现:有界队列 + 专职派发线程。
#include "runtime/agent_health_hooks.hpp"

#include <chrono>
#include <utility>

namespace lubancode::runtime {

namespace {
// 队列帽:监督事件是低频的(健康翻页/重试/强收),256 深度足够吸收任何
// 派发抖动;打满说明订阅方挂死,丢最老保监督器不被背压卡住。
constexpr std::size_t kQueueCap = 256;
constexpr auto kDispatchIdleWait = std::chrono::milliseconds(250);
}  // namespace

AgentHealthHookBus::~AgentHealthHookBus() {
    RequestStop();
    if (!thread_.joinable()) {
        return;
    }
    // 有界收线(与 AgentSupervisor 同款纪律):派发线程至多一个空闲窗内
    // 退;极端挂死(钩子里死循环)才 detach 放行——监督器不受牵连。
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(1000);
    while (std::chrono::steady_clock::now() < deadline && !thread_exited_.load(std::memory_order_acquire)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    if (thread_exited_.load(std::memory_order_acquire)) {
        thread_.join();
    } else {
        thread_.detach();
    }
}

void AgentHealthHookBus::Subscribe(Callback callback) {
    if (callback == nullptr) {
        return;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    callbacks_.push_back(std::move(callback));
}

void AgentHealthHookBus::Publish(const agent::AgentSupervisionEvent& event) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        EnsureThreadStarted();
        if (queue_.size() >= kQueueCap) {
            queue_.erase(queue_.begin());  // 丢最老,保发布方永不被背压卡住
            dropped_events_.fetch_add(1, std::memory_order_release);
        }
        queue_.push_back(event);
    }
    cv_.notify_all();
}

void AgentHealthHookBus::RequestStop() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stop_requested_ = true;
    }
    cv_.notify_all();
}

void AgentHealthHookBus::DrainForTest() {
    DrainBatch();
}

void AgentHealthHookBus::EnsureThreadStarted() {
    // 调用方已持 mutex_。
    if (thread_started_) {
        return;
    }
    thread_started_ = true;
    thread_ = std::thread([this] { DispatchLoop(); });
}

void AgentHealthHookBus::DispatchLoop() {
    std::unique_lock<std::mutex> lock(mutex_);
    while (!stop_requested_) {
        if (queue_.empty()) {
            cv_.wait_for(lock, kDispatchIdleWait);
            if (stop_requested_ && queue_.empty()) {
                break;
            }
            continue;
        }
        // 批量取出后在锁外跑:钩子再慢也不占发布方的锁。
        std::vector<agent::AgentSupervisionEvent> batch = std::move(queue_);
        queue_.clear();
        std::vector<Callback> callbacks = callbacks_;
        lock.unlock();
        for (const auto& event : batch) {
            for (const auto& callback : callbacks) {
                try {
                    callback(event);  // 坏钩子只坑自己:异常吞掉,下一枚照送
                } catch (...) {
                }
            }
        }
        delivered_events_.fetch_add(batch.size(), std::memory_order_release);
        lock.lock();
    }
    thread_exited_.store(true, std::memory_order_release);
}

void AgentHealthHookBus::DrainBatch() {
    std::vector<agent::AgentSupervisionEvent> batch;
    std::vector<Callback> callbacks;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        batch = std::move(queue_);
        queue_.clear();
        callbacks = callbacks_;
    }
    for (const auto& event : batch) {
        for (const auto& callback : callbacks) {
            try {
                callback(event);
            } catch (...) {
            }
        }
    }
    delivered_events_.fetch_add(batch.size(), std::memory_order_release);
}

}  // namespace lubancode::runtime
