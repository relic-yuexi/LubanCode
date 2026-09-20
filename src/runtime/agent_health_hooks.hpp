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
// 线程在跑回调)。析构有界收线:至多一个派发窗口内退,极端挂死(钩子里
// 死循环)才 detach 放行——与 AgentSupervisor 同一条纪律。
//
// 线程寿命(AR-02,2026-09-21 架构审查):派发线程绝不捕宿主 this,只捕
// 共享状态 State 的 shared_ptr。超时 detach 后宿主已析构,线程跑完手头
// 一批、只摸 State(shared_ptr 保命),退出路径也只碰 State 的原子与锁。
// 收线合同:停止后新 Publish 拒收并计入 dropped_events;批内剩余事件
// 照派发完。
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

    AgentHealthHookBus();
    ~AgentHealthHookBus();

    AgentHealthHookBus(const AgentHealthHookBus&) = delete;
    AgentHealthHookBus& operator=(const AgentHealthHookBus&) = delete;

    // 订阅(会话装配时一次性挂;不去重、不退订——总线与监督器同寿,会话
    // 级注册没有中途拔线的场景)。空回调忽略。
    void Subscribe(Callback callback);

    // 发布(监督线程/任务线程调):只入队,不跑钩子。队列有界,溢出丢最老
    // 并计数(dropped_events)——监督器的拍永远不被下游背压拖住。收线后
    // 拒收:RequestStop/析构之后再发的事件直接丢弃并计入 dropped_events。
    void Publish(const agent::AgentSupervisionEvent& event);

    // 收线:跑完手头一批就退(析构兜底也走这)。
    void RequestStop();

    // 诊断口:累计被丢的事件数(队列打满 = 下游跟不上,收线后拒收同计,
    // 账要看得见)。
    std::uint64_t dropped_events() const;
    std::uint64_t delivered_events() const;
    // 测试口:同步派发一轮(线程外直跑,不依赖派发线程的时序)。
    void DrainForTest();
    // 测试口:线程世界(共享状态+专职线程)同寿哨兵——weak 过期即专职
    // 线程已退出、共享状态已析构,此后进程里再无摸它的代码。
    std::weak_ptr<const void> lifetime_token_for_test() const;

private:
    // 线程世界:派发线程摸的全部状态都在这(宿主门面只转发)。shared_ptr
    // 由线程 lambda 持有一份,detach 放行后宿主析构也不悬垂。
    struct State;
    std::shared_ptr<State> state_;
};

}  // namespace lubancode::runtime
