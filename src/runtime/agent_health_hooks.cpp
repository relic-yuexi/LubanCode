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
// 收线截止(与 AgentSupervisor 同款纪律):派发线程至多一个空闲窗内退;
// 极端挂死(钩子里死循环)才 detach 放行——脱离线程只摸 State。
constexpr auto kShutdownDeadline = std::chrono::milliseconds(1000);
}  // namespace

// 线程世界(AR-02):队列、回调表、计数与线程句柄全在此。派发线程只捕
// shared_ptr<State>(enable_shared_from_this 自持),宿主析构后 State 由
// 线程保命,跑完手头一批、退出置 thread_exited——全程不摸宿主。
struct AgentHealthHookBus::State : std::enable_shared_from_this<State> {
    mutable std::mutex mutex;
    std::condition_variable cv;
    std::vector<agent::AgentSupervisionEvent> queue;
    std::vector<Callback> callbacks;
    std::thread thread;
    std::atomic<bool> thread_exited{false};
    std::atomic<bool> stop_requested{false};  // 收线标志:锁外也要查,原子
    bool thread_started = false;              // 锁内
    std::atomic<std::uint64_t> dropped{0};
    std::atomic<std::uint64_t> delivered{0};

    void EnsureThreadStartedLocked() {
        // 调用方已持 mutex。
        if (thread_started) {
            return;
        }
        thread_started = true;
        thread = std::thread([self = shared_from_this()] { self->DispatchLoop(); });
    }

    void DispatchLoop() {
        std::unique_lock<std::mutex> lock(mutex);
        while (!stop_requested.load(std::memory_order_acquire)) {
            if (queue.empty()) {
                cv.wait_for(lock, kDispatchIdleWait);
                if (stop_requested.load(std::memory_order_acquire) && queue.empty()) {
                    break;
                }
                continue;
            }
            // 批量取出后在锁外跑:钩子再慢也不占发布方的锁。
            std::vector<agent::AgentSupervisionEvent> batch = std::move(queue);
            queue.clear();
            std::vector<Callback> snapshot = callbacks;
            lock.unlock();
            // 手头这批照跑完(收线合同:跑完手头一批就退):跨过宿主析构
            // 截止时间的回调返回后,这里摸的只有 State 的原子与局部批。
            for (const auto& event : batch) {
                for (const auto& callback : snapshot) {
                    try {
                        callback(event);  // 坏钩子只坑自己:异常吞掉,下一枚照送
                    } catch (...) {
                    }
                }
            }
            delivered.fetch_add(batch.size(), std::memory_order_release);
            if (stop_requested.load(std::memory_order_acquire)) {
                break;  // 收线:不再回去拿新批(锁都省了)
            }
            lock.lock();
        }
        if (!lock.owns_lock()) {
            lock.lock();
        }
        // 退出只碰 State:宿主可能已析构,这里是脱离线程唯一合法落点。
        thread_exited.store(true, std::memory_order_release);
        cv.notify_all();  // 叫醒等收线的析构
    }

    void BeginShutdown() {
        {
            std::lock_guard<std::mutex> lock(mutex);
            stop_requested.store(true, std::memory_order_release);
        }
        cv.notify_all();
        if (!thread.joinable()) {
            return;
        }
        // 有界收线:窗内退了就 join;极端挂死才 detach——脱离线程此后
        // 只摸 State(shared_ptr 保命),摸不着也不需要宿主。
        {
            std::unique_lock<std::mutex> lock(mutex);
            cv.wait_for(lock, kShutdownDeadline,
                        [this] { return thread_exited.load(std::memory_order_acquire); });
        }
        if (thread_exited.load(std::memory_order_acquire)) {
            thread.join();
        } else {
            thread.detach();
        }
    }
};

AgentHealthHookBus::AgentHealthHookBus() : state_(std::make_shared<State>()) {}

AgentHealthHookBus::~AgentHealthHookBus() { state_->BeginShutdown(); }

void AgentHealthHookBus::Subscribe(Callback callback) {
    if (callback == nullptr) {
        return;
    }
    std::lock_guard<std::mutex> lock(state_->mutex);
    state_->callbacks.push_back(std::move(callback));
}

void AgentHealthHookBus::Publish(const agent::AgentSupervisionEvent& event) {
    {
        std::lock_guard<std::mutex> lock(state_->mutex);
        if (state_->stop_requested.load(std::memory_order_acquire)) {
            // 收线后拒收:不起线程、不入队,账计入 dropped(看得见)。
            state_->dropped.fetch_add(1, std::memory_order_release);
            return;
        }
        state_->EnsureThreadStartedLocked();
        if (state_->queue.size() >= kQueueCap) {
            state_->queue.erase(state_->queue.begin());  // 丢最老,保发布方永不被背压卡住
            state_->dropped.fetch_add(1, std::memory_order_release);
        }
        state_->queue.push_back(event);
    }
    state_->cv.notify_all();
}

void AgentHealthHookBus::RequestStop() {
    {
        std::lock_guard<std::mutex> lock(state_->mutex);
        state_->stop_requested.store(true, std::memory_order_release);
    }
    state_->cv.notify_all();
}

void AgentHealthHookBus::DrainForTest() {
    std::vector<agent::AgentSupervisionEvent> batch;
    std::vector<Callback> snapshot;
    {
        std::lock_guard<std::mutex> lock(state_->mutex);
        batch = std::move(state_->queue);
        state_->queue.clear();
        snapshot = state_->callbacks;
    }
    for (const auto& event : batch) {
        for (const auto& callback : snapshot) {
            try {
                callback(event);
            } catch (...) {
            }
        }
    }
    state_->delivered.fetch_add(batch.size(), std::memory_order_release);
}

std::uint64_t AgentHealthHookBus::dropped_events() const {
    return state_->dropped.load(std::memory_order_acquire);
}

std::uint64_t AgentHealthHookBus::delivered_events() const {
    return state_->delivered.load(std::memory_order_acquire);
}

std::weak_ptr<const void> AgentHealthHookBus::lifetime_token_for_test() const {
    return std::weak_ptr<const void>(state_);
}

}  // namespace lubancode::runtime
