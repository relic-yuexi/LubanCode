// agent_task_coordinator.hpp 的实现。
#include "tools/agent_task_coordinator.hpp"

#include <algorithm>
#include <chrono>
#include <stdexcept>
#include <utility>

namespace lubancode::tools {

namespace {
// 线程局部执行身份:见头文件注释。默认空指针 = 这条线程不在任何任务里
//(main 或装配线程),派工身份回落到 handle 自带的那份。
thread_local const AgentRunIdentity* t_dispatch_identity = nullptr;
}  // namespace

const AgentRunIdentity* CurrentDispatchIdentity() {
    return t_dispatch_identity;
}

ScopedDispatchIdentity::ScopedDispatchIdentity(const AgentRunIdentity& identity)
    : saved_(t_dispatch_identity), storage_(identity) {
    t_dispatch_identity = &storage_;
}

ScopedDispatchIdentity::~ScopedDispatchIdentity() {
    t_dispatch_identity = saved_;
}

AgentRunIdentity IdentityOfSnapshot(const AgentTaskSnapshot& snapshot) {
    AgentRunIdentity identity;
    identity.task_id = snapshot.id;
    identity.root_task_id = snapshot.root_task_id;
    identity.depth = snapshot.depth;
    // 轨迹嵌套边(P1-2):这只任务自己的 run id 投进身份,它派孩子时
    // (TLS CurrentDispatchIdentity 走这份投影)子账的 parent_run_id 认它。
    identity.agent_run_id = snapshot.agent_run_id;
    return identity;
}

AgentDispatchHandle::AgentDispatchHandle(std::weak_ptr<AgentTaskCoordinator> coordinator,
                                         AgentRunIdentity identity,
                                         std::shared_ptr<const SubagentDispatchEnv> env)
    : coordinator_(std::move(coordinator)), identity_(std::move(identity)), env_(std::move(env)) {}

Tool* AgentDispatchHandle::facade_tool() const {
    std::shared_ptr<AgentTaskCoordinator> coordinator = coordinator_.lock();
    return coordinator == nullptr ? nullptr : coordinator->facade_tool();
}

Tool::Result AgentDispatchHandle::Dispatch(const nlohmann::json& input) {
    std::shared_ptr<AgentTaskCoordinator> coordinator = coordinator_.lock();
    if (coordinator == nullptr) {
        // 协调器(随引擎)已退场:后台任务的尾巴派工稳定收口,不悬垂调用。
        return {"会话的子代理派工口已收场,本次调用不再执行。请在新的会话里重新派工。", true};
    }
    AgentDispatchRequest request;
    request.input = input;
    // 身份以线程执行链为准:正在一只任务里跑(前台嵌套/后台线程),就按
    // 那只任务算派工者——旧转发壳与直捕的表骗不了 lineage。
    if (const AgentRunIdentity* current = CurrentDispatchIdentity(); current != nullptr) {
        request.caller = *current;
    } else {
        request.caller = identity_;
    }
    request.env = env_;
    request.fail_account = this;
    return coordinator->Dispatch(request);
}

Tool::Result AgentTaskCoordinator::Dispatch(const AgentDispatchRequest& request) {
    if (closing_.load(std::memory_order_acquire)) {
        return {"会话正在收场,不再接受新的子代理派工。请直接在当前对话里收尾。", true};
    }
    if (!engine_) {
        return {"派工引擎未接线,本次调用无法执行。", true};
    }
    return engine_(request);
}

void AgentTaskCoordinator::RequestClose() {
    std::lock_guard<std::mutex> lock(threads_mutex_);
    closing_.store(true, std::memory_order_release);
}

void AgentTaskCoordinator::SetThreadFactoryForTesting(ThreadFactory factory) {
    auto replacement = factory ? std::make_shared<const ThreadFactory>(std::move(factory)) : nullptr;
    {
        std::lock_guard<std::mutex> lock(threads_mutex_);
        replacement.swap(thread_factory_);
    }
    // Retire arbitrary factory captures outside the ownership mutex.
}

bool AgentTaskCoordinator::StartThread(int task_id, ThreadBody body,
                                       std::shared_ptr<std::atomic<bool>> exit_receipt) {
    if (!body) throw std::invalid_argument("agent thread body missing");
    auto entry = std::make_shared<TaskThreadEntry>();
    entry->task_id = task_id;
    entry->exit_receipt = std::move(exit_receipt);
    std::shared_ptr<const ThreadFactory> factory;
    {
        std::lock_guard<std::mutex> lock(threads_mutex_);
        if (closing_.load(std::memory_order_acquire)) return false;
        // All owner-table allocations happen before a factory can create a
        // thread. Close may take this entry, but waits for its handoff below.
        threads_.push_back(entry);
        factory = thread_factory_;
    }
    try {
        entry->thread = factory ? (*factory)(std::move(body)) : std::thread(std::move(body));
        if (!entry->thread.joinable()) throw std::runtime_error("agent thread factory returned no thread");
    } catch (...) {
        {
            std::lock_guard<std::mutex> lock(threads_mutex_);
            entry->starting = false;
            auto found = std::find(threads_.begin(), threads_.end(), entry);
            if (found != threads_.end()) threads_.erase(found);
        }
        threads_ready_.notify_all();
        throw;
    }
    {
        std::lock_guard<std::mutex> lock(threads_mutex_);
        entry->starting = false;
    }
    threads_ready_.notify_all();
    return true;
}

void AgentTaskCoordinator::ReapExitedThreads() {
    // 已收尾的 std::thread 收柄(原 LaunchBackground 的规矩)。AR-01 起对账
    // 只认线程退出回执:worker 闭包最后一笔才置位。它证明业务尾声走完,
    // 不证明 capture 析构已退;锁外 join 仍须等实际线程退出。旧账按 TaskSettled(业务终态)判——监督器强收
    // 或父收树把台账翻成 Failed 时线程可能还在跑,那一路 join 会无期限押死
    // 派工线程;业务终态从此只用于展示,不用于证明线程结束。
    std::vector<std::shared_ptr<TaskThreadEntry>> finished;
    {
        std::lock_guard<std::mutex> lock(threads_mutex_);
        finished.reserve(threads_.size());
        for (std::size_t i = 0; i < threads_.size();) {
            const auto& entry = threads_[i];
            if (!entry->starting && !entry->reaping && entry->exit_receipt != nullptr &&
                entry->exit_receipt->load(std::memory_order_acquire) && entry->thread.joinable() &&
                entry->thread.get_id() != std::this_thread::get_id()) {
                finished.push_back(entry);
                entry->reaping = true;
            }
            ++i;
        }
    }
    try {
        for (const auto& entry : finished) {
            entry->thread.join();
            {
                std::lock_guard<std::mutex> lock(threads_mutex_);
                entry->reaping = false;
                auto found = std::find(threads_.begin(), threads_.end(), entry);
                if (found != threads_.end()) threads_.erase(found);
            }
            threads_ready_.notify_all();
        }
    } catch (...) {
        {
            std::lock_guard<std::mutex> lock(threads_mutex_);
            for (const auto& entry : finished) entry->reaping = false;
        }
        threads_ready_.notify_all();
        throw;
    }
}

bool AgentTaskCoordinator::HasReapingThreadForTesting() {
    std::lock_guard<std::mutex> lock(threads_mutex_);
    return std::any_of(threads_.begin(), threads_.end(), [](const auto& entry) { return entry->reaping; });
}

void AgentTaskCoordinator::JoinAllBounded() {
    // 退出兜底(原 ~AgentTool):先广播取消,再给每只后台线程一枚有界等窗
    // ——等的是线程退出回执,不是台账终态。先等 startup/reaping 所有权
    // 交接,回执在手后 join 仍等 capture 析构;两段均无硬截止。回执窗口
    // 尽了还没等到就 detach 放它走——worker 闭包自持冻结
    // run_state(钉住协调器与台账)与 TaskRecord 的 shared_ptr,晚归不悬垂、
    // 不丢账,也不冻退出。
    ledger_.BroadcastCancel();
    std::vector<std::shared_ptr<TaskThreadEntry>> entries;
    {
        std::lock_guard<std::mutex> lock(threads_mutex_);
        entries = std::move(threads_);
        threads_.clear();
    }
    for (auto& entry : entries) {
        {
            std::unique_lock<std::mutex> lock(threads_mutex_);
            threads_ready_.wait(lock, [&] { return !entry->starting && !entry->reaping; });
        }
        auto& thread = entry->thread;
        if (!thread.joinable()) {
            continue;
        }
        const auto deadline = std::chrono::steady_clock::now() + shutdown_join_window_;
        bool exited = false;
        while (std::chrono::steady_clock::now() < deadline) {
            if (entry->exit_receipt != nullptr && entry->exit_receipt->load(std::memory_order_acquire)) {
                exited = true;
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        if (exited) {
            thread.join();
        } else {
            thread.detach();  // 挂死绝境:放线程走,不冻退出
        }
    }
    // 任务线程收完,监督线也停(单子 P0-2:session close 时 watcher 都要收净;
    // 析构里的 detach 只是兜底)。
    supervisor_.RequestStop();
}

void AgentTaskCoordinator::ClearFacadeTool() {
    facade_tool_.store(nullptr, std::memory_order_release);
}

}  // namespace lubancode::tools
