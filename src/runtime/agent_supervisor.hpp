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
// 锁序:supervisor 自有 mutex 只保护登记表;一切台账调用(ledger_)都在
// 松开自有 mutex 之后进行,FormAliveVitals 的 visitor 在台账锁内跑、不回拿
// supervisor mutex——两个方向不交叉,无环。
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
#include "tools/task_ledger.hpp"     // TaskLedger/TaskRecord:任务真账

namespace lubancode::runtime {

class AgentSupervisor {
public:
    using Clock = std::chrono::steady_clock;

    // ledger 由所有者(AgentTaskCoordinator)持有,寿命盖过本类。
    explicit AgentSupervisor(tools::TaskLedger& ledger);
    ~AgentSupervisor();  // 有界停线:监督线程最迟一个 tick 内退,挂死则 detach 放行

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
    const agent::SupervisionThresholds& thresholds() const { return thresholds_; }
    // 空转收口的宽限:停止信号发出后任务线程这么久没报终态,才强收账。
    void SetNoProgressGraceSecs(int secs) { no_progress_grace_secs_ = secs > 0 ? secs : 1; }

    // 会话收场:停监督线程(JoinAllBounded 之前调,跑完这拍就退)。
    void RequestStop();

    // 测试口:当前监督线程数(验收:100 只 fake task 不生 100 根线程)。
    std::size_t supervisor_thread_count_for_test() const;
    // 测试口:同步驱动一拍(线程外直跑健康拍;deadline 仍由线程/真时间驱动)。
    void TickHealthForTest(bool host_resume_suspected = false);

private:
    struct Deadline {
        enum class Kind { WallClock, WallGrace, NoProgressGrace };
        Kind kind = Kind::WallClock;
        Clock::time_point at{};
        std::shared_ptr<tools::TaskRecord> task;
        int wall_timeout_secs = 0;  // WallClock:强收文案用
        int grace_secs = 30;        // WallClock:停止信号后的宽限
    };

    void EnsureThreadStarted();
    void RunLoop();
    // 一拍健康判:取视景 -> 纯函数判 -> 执行动作。返回是否有人要叫醒下一拍。
    void HealthPass(bool host_resume_suspected);
    void ProcessDueDeadlines(const Clock::time_point now);
    void FireWallClock(const Deadline& deadline);
    void FireWallGrace(const Deadline& deadline);
    void FireNoProgressGrace(const Deadline& deadline);
    Clock::time_point NextDeadlineLocked() const;
    void PushNoticeDeduped(const std::shared_ptr<tools::TaskRecord>& task, std::uint64_t health_epoch,
                           const std::string& reason_code, const std::string& text);

    tools::TaskLedger& ledger_;
    agent::SupervisionThresholds thresholds_;
    int no_progress_grace_secs_ = 15;

    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::thread thread_;
    std::atomic<bool> thread_exited_{false};
    bool thread_started_ = false;
    bool stop_requested_ = false;
    std::vector<Deadline> deadlines_;
    std::vector<std::shared_ptr<tools::TaskRecord>> watches_;
    // 通知去重(单子 §十):同一 task_id + health_epoch + reason 只弹一次。
    std::set<std::string> noticed_keys_;
    Clock::time_point last_tick_{};
};

}  // namespace lubancode::runtime
