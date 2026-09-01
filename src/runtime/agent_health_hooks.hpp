// AgentHealthChanged 只读钩子总线(《子代理监督器、agent_watch 与停滞恢复
// 设计》§11.2,P2):监督事件经后台安全队列异步送达外部订阅者,发布方
//(AgentSupervisor/台账)从不直接跑钩子。
//
// 红线(单子 §11.2):
//   - P0/P2 都不让外部 hook 改恢复决定——这里只有 notify,没有投票口;
//   - hook 慢、坏(抛异常)、超时,不得反过来卡 Supervisor:Publish 只在
//     小锁里入队(有界,溢出丢最老并计数),钩子全在专职线程上跑;
//   - 事件只带 id/枚举/计数/时长/稳定码,不带 thinking、正文、Secret 与
//     完整工具参数(单子 §五·11)。
//
// 订阅方自备线程安全(回调在总线自己的派发线程上跑,同一时刻只有一条
// 线程在跑回调)。析构有界收线:最迟一个派发窗口内退,挂死则 detach
// 放行——与 AgentSupervisor 同一条纪律。
#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "agent/agent_progress.hpp"  // AgentSupervisionEvent:事件形状

namespace lubancode::runtime {

class AgentHealthHookBus {
public:
    using Callback = std::function<void(const agent::AgentSupervisionEvent&)>;

    AgentHealthHookBus() = default;
    ~AgentHealthHookBus();

    AgentHealthHookBus(const AgentHealthHookBus&) = delete;
    AgentHealthHookBus& operator=(const AgentHealthHookBus&) = delete;

    // 订阅(会话装配时一次性挂;不去重、不退订——总线与监督器同寿,会话
    // 级注册没有中途拔线的场景)。空回调忽略。
    void Subscribe(Callback callback);

    // 发布(监督线程/任务线程调):只入队,不跑钩子。队列有界,溢出丢最老
    // 并计数(dropped_events)——监督器的拍永远不被下游背压拖住。
    void Publish(const agent::AgentSupervisionEvent& event);

    // 收线:跑完手头一批就退(析构兜底也走这)。
    void RequestStop();

    // 诊断口:累计被丢的事件数(队列打满 = 下游跟不上,账要看得见)。
    std::uint64_t dropped_events() const { return dropped_events_.load(std::memory_order_acquire); }
    std::uint64_t delivered_events() const { return delivered_events_.load(std::memory_order_acquire); }
    // 测试口:同步派发一轮(线程外直跑,不依赖派发线程的时序)。
    void DrainForTest();

private:
    void EnsureThreadStarted();
    void DispatchLoop();
    void DrainBatch();

    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::vector<agent::AgentSupervisionEvent> queue_;
    std::vector<Callback> callbacks_;
    std::thread thread_;
    std::atomic<bool> thread_exited_{false};
    bool thread_started_ = false;
    bool stop_requested_ = false;
    std::atomic<std::uint64_t> dropped_events_{0};
    std::atomic<std::uint64_t> delivered_events_{0};
};

}  // namespace lubancode::runtime
