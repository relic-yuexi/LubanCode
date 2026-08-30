// agent_task_coordinator.hpp 的实现。
#include "tools/agent_task_coordinator.hpp"

#include <chrono>
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

void AgentTaskCoordinator::TrackThread(int task_id, std::thread thread) {
    std::lock_guard<std::mutex> lock(threads_mutex_);
    threads_.push_back(TaskThreadEntry{task_id, std::move(thread)});
}

void AgentTaskCoordinator::ReapSettledThreads() {
    // 已收尾的 std::thread 收柄对账按自家任务号查(原 LaunchBackground 的
    // 规矩原样迁来):台账里混着无线程的前台任务,按下标对齐会把早终态的
    // 旧任务误当这只线程已收尾,join 押死孵化(病灶一)。
    std::lock_guard<std::mutex> lock(threads_mutex_);
    for (std::size_t i = 0; i < threads_.size();) {
        if (ledger_.TaskSettled(threads_[i].task_id) && threads_[i].thread.joinable()) {
            threads_[i].thread.join();
            threads_.erase(threads_.begin() + static_cast<std::ptrdiff_t>(i));
            continue;
        }
        ++i;
    }
}

void AgentTaskCoordinator::JoinAllBounded() {
    // 退出兜底(原 ~AgentTool):先广播取消,再给每只后台线程一枚有界 join
    // 窗口;join 等不到的 detach 放它走——台账已是终态(或由看门狗强制收
    // 账),detach 不丢账;线程闭包自持 TaskRecord 的 shared_ptr,晚归不悬垂。
    ledger_.BroadcastCancel();
    std::vector<TaskThreadEntry> entries;
    {
        std::lock_guard<std::mutex> lock(threads_mutex_);
        entries = std::move(threads_);
        threads_.clear();
    }
    for (auto& entry : entries) {
        auto& thread = entry.thread;
        if (!thread.joinable()) {
            continue;
        }
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        bool settled = false;
        while (std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
            settled = !ledger_.HasRunningTasks();
            if (settled) {
                break;
            }
        }
        if (settled) {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));  // 给收尾尾格一点余量
            thread.join();
        } else {
            thread.detach();  // 挂死绝境:放线程走,不冻退出
        }
    }
}

}  // namespace lubancode::tools
