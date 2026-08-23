// LoopScheduler(loop 单第 1 期):session 内定时任务的内存真值。
//
// 单子的铁律(这里只管内存账;持久化在第 3 期走 LoopLedgerSink):
//   - timer thread 只算 due、发 wake signal,不碰 history、不调
//     ProcessLine、不画 TUI、不问权限。
//   - 一场 session 同时只跑一枚主 turn(single-flight):Due 队列是唯一
//     取件口,消费发生在主循环安全边界(PumpDueTicks)。
//   - 所有状态变更先持 scheduler mutex,再 append 事件(锁内回调只交
//     出已拷贝的事件,磁盘 IO 不在锁内——事件先攒 pending_events,泵外
//     由装配层 flush)。
//   - 时钟可注入(Clock 注入口):单测喂 fake clock,不开 /loop 1s 后门。
//     等待用 steady 语义(由实现层的轮询间隔承载),账面时间全走 wall ms。
//   - 析构 stop/join,无 use-after-free;timer 回调不摸已析构的 this。
//
// 依赖铁律:只认标准库 + nlohmann/json + loop_types,不 include cli/app/
// agent。装配层(interactive_session)把它接进会话泵。

#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "runtime/loop_types.hpp"

namespace lubancode::runtime::loop {

// 可注入时钟(单测喂 fake):NowWallMs 供账面,WaitUntil 由实现层轮询/
// 条件变量承载。真实现持稳轮询间隔,不忙等。
class LoopClock {
public:
    virtual ~LoopClock() = default;
    virtual WallTimeMs NowWallMs() const;
};

// scheduler 向外发的事件(装配层拿去落 session JSONL / 发 Runtime 事件)。
// 形状对齐 goal 单的 GoalCoordinatorEvent:先攒后 flush,写盘不在锁内。
struct LoopSchedulerEvent {
    std::string family;      // "loop_task_v1" / "loop_tick_v1"
    std::string event;       // created/paused/.../due/started/finished/...
    std::string task_id;
    std::string tick_id;     // tick 级事件带
    nlohmann::json payload = nlohmann::json::object();
    WallTimeMs timestamp_ms = 0;
};

// 泵取走的一枚 due tick:装配层拿它拼 scheduled message、开 turn。
struct DispatchedTick {
    LoopTask task;    // 取走那一刻的 task 快照(state=Running)
    LoopTick tick;    // 已记 due+started 的 tick 账
    std::string text; // 本拍的 prompt 正文(prompt 源由装配层先解析喂进;
                      // 这里只透传 Create 时收下的 inline/占位)
};

class LoopScheduler {
public:
    // Options 的口都留默认;装配层从集中配置灌。
    struct Options {
        int max_active_per_session = LoopDefaults::kMaxActivePerSession;
        bool enabled = true;  // feature gate(features.loop,装配层读)
    };

    // 时钟不传则用真墙钟。id_issuer 可注入(测试定死 id)。
    explicit LoopScheduler(Options options, std::shared_ptr<LoopClock> clock = {});
    LoopScheduler() : LoopScheduler(Options{}) {}
    ~LoopScheduler();

    LoopScheduler(const LoopScheduler&) = delete;
    LoopScheduler& operator=(const LoopScheduler&) = delete;

    // ---- 任务生命周期 --------------------------------------------------------
    // /loop create:建一只 Active task。已过 max_active 报
    // kErrLoopTooManyActive;feature 关报 disabled。事件先攒账,由
    // FlushEvents 交装配层落盘;落盘失败由装配层调 FailStore 断新 tick。
    struct CommandResult {
        bool ok = false;
        std::string error_code;
        std::string error_message;
        nlohmann::json payload = nlohmann::json::object();
    };
    CommandResult Create(std::string prompt, std::chrono::seconds interval, WallTimeMs now_ms,
                         std::string cwd_identity, std::string session_id,
                         LoopPromptSource source = LoopPromptSource::Inline,
                         std::string prompt_file = std::string(),
                         std::function<std::string()> id_issuer = nullptr);

    CommandResult Pause(const std::string& task_id, WallTimeMs now_ms, const std::string& reason);
    // all:task_id == "all" 时逐只 pause(只动非终态),payload 带只数。
    CommandResult Resume(const std::string& task_id, WallTimeMs now_ms);
    CommandResult Stop(const std::string& task_id, WallTimeMs now_ms, const std::string& reason);
    // 立即补一拍,不改原 cadence:把 next_due 拉到 now,下一拍泵走。
    CommandResult RunNow(const std::string& task_id, WallTimeMs now_ms);

    // 模型经 loop_control 声明 complete(窄工具的口):Due/Running/Waiting
    // 收,落 Completed 终态;下一拍不再排。
    CommandResult Complete(const std::string& task_id, WallTimeMs now_ms, const std::string& reason);

    // ---- 泵(主线程安全边界调) ----------------------------------------------
    // 有没有活 task(状态栏/空闲唤醒问)。
    bool HasActiveTasks() const;
    // 到点没:空闲唤醒问(composer 100ms 拍)。真到点的判定在泵里现算,
    // 这里只报"有到点的活"。
    bool HasDueWork(WallTimeMs now_ms) const;
    // 取一只 due(按 next_due_at,再按 creation_seq 稳定排序;单飞:
    // Running/Waiting/BackingOff 的 task 不取)。没有 due 给 nullopt。
    // 取走即记 due+started 事件(state -> Running),tick 账同建。
    std::optional<DispatchedTick> PumpDueTick(WallTimeMs now_ms, const std::string& turn_id);

    // ---- 拍子收口 ------------------------------------------------------------
    // 一枚 dispatched tick 跑完:记 finished 事件、算下一拍(succeeded 时
    // next_due = 原 cadence 推进;provider_error/rate_limited 走退避账)。
    // consecutive provider 失败撞 5 拍自动 Pause;kErrLoop* 由 error_code 带。
    void FinishTick(const std::string& tick_id, LoopTickOutcome outcome, WallTimeMs now_ms,
                    std::string error_code = std::string());

    // 同 tick 的退避重试(provider 瞬时错,attempt+1 再试):
    // attempts 没到 3 且未过 expiry 时返回下次该等的毫秒数;到顶给 nullopt
    //(调用方按 provider_error 收口)。
    std::optional<std::chrono::milliseconds> RetryBackoffFor(const std::string& tick_id,
                                                             WallTimeMs now_ms);

    // 权限悬起/答回:Waiting 时后续拍全 coalesce(不另开 tick);答回继续。
    void NotePermissionWait(const std::string& tick_id, WallTimeMs now_ms);
    void NotePermissionResolved(const std::string& tick_id, WallTimeMs now_ms);
    // 拒了一拍:declined 连拍账 +1,撞 3 拍自动 Pause(reason=denials)。
    void NotePermissionDeclined(const std::string& tick_id, WallTimeMs now_ms);

    // rate limit 带 retry-after:task 进 BackingOff,backoff_until 记账,
    // 到点回 Active(不双发:cadence 从 backoff 结束点重排)。
    void NoteRateLimited(const std::string& tick_id, std::chrono::seconds retry_after,
                         WallTimeMs now_ms);

    // ---- 维护 ----------------------------------------------------------------
    // expiry 扫(泵每圈调):过期的落 expired 事件,不默默消失。
    void SweepExpiry(WallTimeMs now_ms);
    // cwd/worktree 移房:绑不上的 task Pause(单子:不能在新目录照旧跑)。
    void NoteCwdChanged(const std::string& cwd_identity, WallTimeMs now_ms);
    // 存档写盘失败:断新 tick(单子:失去恢复账后继续跑,风险大过便利)。
    // 已跑的拍收口照旧;之后 PumpDueTick 恒空、Create 拒。
    void FailStore(const std::string& reason);
    // 一次性审批不跨 tick:装配层在 tick 边界调,只做账面清零的口。
    // (审批真值在 interaction broker,这里不复制。)

    // ---- 查询 ----------------------------------------------------------------
    struct TaskView {
        LoopTask task;
        bool has_current_tick = false;
        LoopTick current_tick;      // Running/Waiting 时那枚
        WallTimeMs backoff_until_ms = 0;  // BackingOff 时有效
        bool delayed = false;  // 超 interval*2 未消费(状态栏提示用)
    };
    std::vector<TaskView> Snapshot(WallTimeMs now_ms) const;
    std::optional<TaskView> Find(const std::string& task_id, WallTimeMs now_ms) const;
    // 裸数字别名("3" -> loop-3):宿主 id 规约,parser 与查询共用。
    std::string ResolveTaskId(const std::string& input) const;

    // ---- 事件账 ------------------------------------------------------------
    // 取走攒着的事件(交装配层落盘);取走即清。锁外调用。
    std::vector<LoopSchedulerEvent> TakeEvents();

    // 恢复口(第 3 期 LoopLedger 喂):回放一条事件行,重建内存账。
    // 坏事件跳过(返回 false),不废整场。
    bool ReplayEvent(const LoopSchedulerEvent& event);

    // timer 线程控制(真实现:轮询 due 并唤醒;测试不启)。
    void StartTimer();
    void StopTimer();
    // 空闲唤醒的口:有 due 活就该醒(IdleWakeCoordinator 的 source)。
    bool ShouldWakeNow() const;

private:
    struct TaskEntry {
        LoopTask task;
        std::optional<LoopTick> current_tick;  // Running/Waiting/BackingOff 的那枚
        WallTimeMs backoff_until_ms = 0;
        bool run_now_requested = false;  // /loop run 拉早的旗:补拍不改 cadence
    };
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace lubancode::runtime::loop
