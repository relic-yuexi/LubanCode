// AgentSupervisor(《子代理监督器、agent_watch 与停滞恢复设计》P0-2):会话级
// 单线程监督器。它吃 TaskLedger 里的进展合同(四本时钟),按阶段软线判
// Healthy/Quiet/Suspect*,把散落信号合成诊断,再接上有限恢复——不另养一份
// 任务状态(TaskLedger 仍是真账,单子不变量 1),不为每只任务起 watchdog
// 线程(单子 §十五:任务多用 deadline 表,莫每只任务一根轮询线程)。
//
// 它管三件事:
//   1. 墙钟迁移:原 RunTask 里每任务一根的墙钟看门狗线程收进来,统一登记
//      期限,同一根监督线程落锤(置 wall_stop -> 宽限 -> ForceFinalize)。
//   2. 健康拍:每 500ms 对活任务跑一遍 EvaluateSupervision(纯函数,假钟可
//      单测),翻健康、投去重通知、按空转尺子给 host notice / 收口信号。
//   3. 睡眠甄别:一次 tick 跳过 max(2*interval, 30s) 记 host_resume_suspected,
//      该拍不判 SuspectAgent——合盖醒来的跨度不许记到 Agent 头上(§7.2)。
//
// 恢复的另一半(请求级重试)不在 supervisor:P0-1 的 ModelRequestRecovery
// 在发送线程内闭环,supervisor 只把健康翻成 Recovering 供显示与账面。
// 自动重派新 Agent 永远不做(单子 §8.4)。
//
// 线程寿命(AR-02,2026-09-21 架构审查):监督线程绝不捕宿主 this,监督
// 循环连同它的锁、期限表、观察表整包住进共享状态 Loop(shared_ptr 保
// 命)。宿主析构先断开台账访问(置断连标志:线程不再发起新的台账访问,
// 在途访问由有界收线窗兜住)再请求停止;跨截止时间存活的脱离线程只摸
// Loop,退出路径也只碰 Loop 的原子与锁。台账仍由所有者持有、寿命盖过
// 本类(声明序在前,析构在后)。
//
// 锁序:Loop 自有 mutex 只保护登记表;一切台账调用(ledger)都在松开
// 自有 mutex 之后进行,FormAliveVitals 的 visitor 在台账锁内跑、不回拿
// Loop mutex——两个方向不交叉,无环。
#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "agent/agent_progress.hpp"  // SupervisionThresholds/TaskVitals/EvaluateSupervision
#include "runtime/agent_health_hooks.hpp"     // AgentHealthHookBus:P2 只读钩子总线
#include "runtime/agent_supervisor_metrics.hpp"  // AgentSupervisorMetrics:P2 六枚指标
#include "tools/task_ledger.hpp"     // TaskLedger/TaskRecord:任务真账

namespace lubancode::runtime {

class AgentSupervisor {
public:
    using Clock = std::chrono::steady_clock;

    // ledger 由所有者(AgentTaskCoordinator)持有,寿命盖过本类。
    explicit AgentSupervisor(tools::TaskLedger& ledger);
    // 有界停线:监督线程最迟一个 tick 内退,挂死则 detach 放行(脱离
    // 线程只摸共享状态 Loop,见上)。
    ~AgentSupervisor();

    AgentSupervisor(const AgentSupervisor&) = delete;
    AgentSupervisor& operator=(const AgentSupervisor&) = delete;

    // ---- 墙钟期限(P0-2 自每任务 watchdog 线程迁入)-------------------------
    // 到点置 wall_clock_fired + wall_stop(走取消链),宽限后仍无终态则
    // ForceFinalizeWallClock。timeout_secs<=0 不登记。
    void ArmWallClock(const std::shared_ptr<tools::TaskRecord>& task, int timeout_secs, int grace_secs);

    // ---- 健康拍登记 --------------------------------------------------------
    // 每只进台账的活任务都登(没有墙钟也要看健康);终态任务由健康拍自动退场。
    void WatchTask(const std::shared_ptr<tools::TaskRecord>& task);

    // 尺子(测试注入用小阈值;默认见 SupervisionThresholds)。
    void SetThresholds(agent::SupervisionThresholds thresholds);
    const agent::SupervisionThresholds& thresholds() const;
    // 空转收口的宽限:停止信号发出后任务线程这么久没报终态,才强收账。
    void SetNoProgressGraceSecs(int secs);

    // 会话收场:停监督线程(JoinAllBounded 之前调,跑完这拍就退)。
    void RequestStop();

    // 测试口:当前监督线程数(验收:100 只 fake task 不生 100 根线程)。
    std::size_t supervisor_thread_count_for_test() const;
    // 测试口:同步驱动一拍(线程外直跑健康拍;deadline 仍由线程/真时间驱动)。
    void TickHealthForTest(bool host_resume_suspected = false);

    // ---- P2:只读钩子与指标 --------------------------------------------------
    // AgentHealthChanged 钩子总线(后台安全队列):会话装配层 Subscribe,
    // 慢/坏钩子不卡监督拍。监督器与台账侧事件(恢复/不明/强收)都进这里。
    AgentHealthHookBus& health_hooks() { return *health_hooks_; }
    // 六枚低基数指标(单子 §11.3):事件计数 + 台账现值,Snapshot 出
    // telemetry::MetricSample(可走 OTLP 编码器)。
    std::vector<telemetry::MetricSample> MetricsSnapshot() const { return metrics_.Snapshot(); }
    // 测试口:线程世界(共享状态 Loop+监督线程)同寿哨兵——weak 过期即
    // 监督线程已退出、共享状态已析构,此后进程里再无摸它的代码。
    std::weak_ptr<const void> lifetime_token_for_test() const { return loop_; }

private:
    // 线程世界(定义在 .cpp):监督循环、锁、期限表与观察表整包。监督
    // 线程只捕 shared_ptr<Loop>,宿主门面只转发。
    struct Loop;

    // 台账侧监督事件(恢复起讫/工具不明/强收)的进料口:投钩子总线 + 记
    // 指标。在台账锁内被调,只入队/拿自家小锁,不回拿台账锁。
    void OnLedgerSupervisionEvent(const agent::AgentSupervisionEvent& event);

    tools::TaskLedger& ledger_;
    // P2 指标:只有台账 sink(析构头部即拔)与外部快照摸,监督线程不碰。
    AgentSupervisorMetrics metrics_;
    // P2 钩子总线:shared_ptr 持有(事件只经台账 sink 进总线,sink 拔掉
    // 后无人再摸;总线自带专职派发线程,慢/坏钩子不占监督拍)。
    std::shared_ptr<AgentHealthHookBus> health_hooks_;
    // 线程世界:声明在最后,析构先于以上——其实次序无关紧要(各自
    // shared_ptr 独立保命),排在这只为读起来顺。
    std::shared_ptr<Loop> loop_;
};

}  // namespace lubancode::runtime
