// agent_supervisor.hpp 的实现:单线程定时驱动、期限表、健康拍与通知去重。
// AR-02(2026-09-21 架构审查):监督循环整包住进共享状态 Loop,线程只捕
// shared_ptr<Loop>;宿主门面只转发,析构断开台账访问后请求停止。
#include "runtime/agent_supervisor.hpp"

#include <algorithm>
#include <utility>

namespace lubancode::runtime {

namespace {
// 监督拍间隔。500ms 足够看 20s/30s 级软线,又不会跟 100ms 的面板刷新抢锁。
constexpr auto kTickInterval = std::chrono::milliseconds(500);
// 睡眠甄别线(单子 §7.2):一次 tick 跳过 max(2*interval, 30s) 即疑宿主
// 睡眠/挂起,该拍不判 SuspectAgent。
constexpr auto kHostResumeMinGap = std::chrono::seconds(30);
// 收线截止(与旧每任务看门狗同款规矩):监督线程至多一个 tick 内退;
// 极端挂死(台账锁被长期占死等)才 detach 放行——脱离线程只摸 Loop,
// 不再发起任何台账访问。
constexpr auto kShutdownDeadline = std::chrono::milliseconds(1500);
}  // namespace

// 线程世界(AR-02):监督线程摸的全部状态与方法都在这。宿主析构置
// host_detached 断连标志后,线程在循环检查点处不再发起台账访问;跨
// 截止时间存活的脱离线程跑完在途一拍,回环见标志即退——全程只摸
// Loop(shared_ptr 保命),退出路径只碰原子与锁。
struct AgentSupervisor::Loop : std::enable_shared_from_this<Loop> {
    struct Deadline {
        enum class Kind { WallClock, WallGrace, NoProgressGrace };
        Kind kind = Kind::WallClock;
        Clock::time_point at{};
        std::shared_ptr<tools::TaskRecord> task;
        int wall_timeout_secs = 0;  // WallClock:强收文案用
        int grace_secs = 30;        // WallClock:停止信号后的宽限
    };

    tools::TaskLedger& ledger;
    agent::SupervisionThresholds thresholds;
    int no_progress_grace_secs = 15;
    mutable std::mutex mutex;
    std::condition_variable cv;
    std::thread thread;
    std::atomic<bool> thread_exited{false};
    std::atomic<bool> stop_requested{false};   // 收线标志:锁外也要查,原子
    std::atomic<bool> host_detached{false};    // 宿主断连:此后不发起新台账访问
    bool thread_started = false;               // 锁内
    std::vector<Deadline> deadlines;
    std::vector<std::shared_ptr<tools::TaskRecord>> watches;
    Clock::time_point last_tick{};

    explicit Loop(tools::TaskLedger& ledger_ref) : ledger(ledger_ref) {}

    void EnsureThreadStartedLocked() {
        // 调用方已持 mutex。
        if (thread_started) {
            return;
        }
        thread_started = true;
        thread = std::thread([self = shared_from_this()] { self->RunLoop(); });
    }

    void RunLoop() {
        std::unique_lock<std::mutex> lock(mutex);
        last_tick = Clock::now();
        while (!stop_requested.load(std::memory_order_acquire) &&
               !host_detached.load(std::memory_order_acquire)) {
            const Clock::time_point next_deadline = NextDeadlineLocked();
            const Clock::time_point wake_at =
                next_deadline.time_since_epoch().count() != 0
                    ? std::min(next_deadline, Clock::now() + kTickInterval)
                    : Clock::now() + kTickInterval;
            cv.wait_until(lock, wake_at);
            if (stop_requested.load(std::memory_order_acquire) ||
                host_detached.load(std::memory_order_acquire)) {
                break;
            }
            const Clock::time_point now = Clock::now();
            const auto gap = now - last_tick;
            last_tick = now;
            // 睡眠甄别(单子 §7.2):跳过的跨度超过 max(2*interval, 30s)(30s
            // 恒大于 1s,直接比 30s)即疑宿主刚醒。
            const bool host_resume_suspected = gap > kHostResumeMinGap;
            lock.unlock();
            // 断连检查点:宿主已析构(或正在析构)后不再发起台账访问;
            // 在途的一拍由收线窗兜住,跑完回环即退。
            if (host_detached.load(std::memory_order_acquire)) {
                break;
            }
            ProcessDueDeadlines(now);
            HealthPass(host_resume_suspected);
            lock.lock();
        }
        if (!lock.owns_lock()) {
            lock.lock();
        }
        // 退出只碰 Loop:宿主可能已析构,这里是脱离线程唯一合法落点。
        thread_exited.store(true, std::memory_order_release);
        cv.notify_all();  // 叫醒等收线的析构
    }

    Clock::time_point NextDeadlineLocked() const {
        Clock::time_point best{};
        for (const auto& deadline : deadlines) {
            if (best.time_since_epoch().count() == 0 || deadline.at < best) {
                best = deadline.at;
            }
        }
        return best;
    }

    void ProcessDueDeadlines(const Clock::time_point now) {
        std::vector<Deadline> due;
        {
            std::lock_guard<std::mutex> lock(mutex);
            for (auto it = deadlines.begin(); it != deadlines.end();) {
                if (it->at <= now) {
                    due.push_back(std::move(*it));
                    it = deadlines.erase(it);
                } else {
                    ++it;
                }
            }
        }
        for (const auto& deadline : due) {
            switch (deadline.kind) {
                case Deadline::Kind::WallClock:
                    FireWallClock(deadline);
                    break;
                case Deadline::Kind::WallGrace:
                    FireWallGrace(deadline);
                    break;
                case Deadline::Kind::NoProgressGrace:
                    FireNoProgressGrace(deadline);
                    break;
            }
        }
    }

    void FireWallClock(const Deadline& deadline) {
        const auto& task = deadline.task;
        if (task == nullptr || task->finalized.load(std::memory_order_acquire)) {
            return;  // 正常收尾在限内办完/已被强收:无事发生(finalized=终态)
        }
        task->wall_clock_fired.store(true, std::memory_order_release);
        task->wall_stop.store(true, std::memory_order_release);
        // 宽限期限:停止信号发出后任务线程仍没报终态才强收。
        Deadline grace;
        grace.kind = Deadline::Kind::WallGrace;
        grace.at = Clock::now() + std::chrono::seconds(deadline.grace_secs);
        grace.task = task;
        grace.wall_timeout_secs = deadline.wall_timeout_secs;
        grace.grace_secs = deadline.grace_secs;
        {
            std::lock_guard<std::mutex> lock(mutex);
            deadlines.push_back(std::move(grace));
        }
        cv.notify_all();
    }

    void FireWallGrace(const Deadline& deadline) {
        const auto& task = deadline.task;
        if (task == nullptr || task->finalized.load(std::memory_order_acquire)) {
            return;  // 停止信号起了作用,任务线程自己收的账更准
        }
        ledger.ForceFinalizeWallClock(task, deadline.wall_timeout_secs);
        // 强收是终局一次性通知:固定代际 1,去重键由 task+reason 指认。
        PushNoticeDeduped(task, 1, "wall_clock.force_finalized",
                          "[监督] #" + std::to_string(task->snapshot.id) + " " +
                              (task->snapshot.title.empty() ? "(未命名)" : task->snapshot.title) +
                              " 墙钟到点后宽限期内仍未收口,已强制收账(" +
                              std::to_string(deadline.wall_timeout_secs) + "s)。");
    }

    void FireNoProgressGrace(const Deadline& deadline) {
        const auto& task = deadline.task;
        if (task == nullptr || task->finalized.load(std::memory_order_acquire)) {
            return;
        }
        // AR-06(锁外读修正):stale_rounds 不再锁外取——强收口在台账锁内
        // 现读,读值与终态提交同锁同刻,任务线程的轮次提交插不进缝。
        ledger.ForceFinalizeNoProgress(task);
        PushNoticeDeduped(task, 1, "agent.no_meaningful_progress.finalized",
                          "[监督] #" + std::to_string(task->snapshot.id) + " " +
                              (task->snapshot.title.empty() ? "(未命名)" : task->snapshot.title) +
                              " 空转收口信号后宽限期内未收口,已按无进展强制收账(部分结果保留)。");
    }

    void HealthPass(bool host_resume_suspected) {
        struct Finding {
            std::shared_ptr<tools::TaskRecord> task;
            agent::SupervisionVerdict verdict;
            agent::TaskVitals vitals;
        };
        std::vector<Finding> findings;
        std::vector<std::shared_ptr<tools::TaskRecord>> still_watching;
        agent::SupervisionThresholds thresholds_snapshot;
        int no_progress_grace_secs_snapshot = 15;
        {
            std::lock_guard<std::mutex> lock(mutex);
            thresholds_snapshot = thresholds;
            no_progress_grace_secs_snapshot = no_progress_grace_secs;
            watches.swap(still_watching);
        }
        // 取视景:visitor 在台账锁内跑,只攒事实,不回拿监督锁。
        ledger.ForEachAliveVitals([&](const std::shared_ptr<tools::TaskRecord>& task,
                                      const agent::TaskVitals& vitals) {
            agent::TaskVitals copy = vitals;
            const agent::SupervisionVerdict verdict = agent::EvaluateSupervision(copy, thresholds_snapshot,
                                                                                 host_resume_suspected);
            if (verdict.action != agent::SupervisionAction::None) {
                findings.push_back(Finding{task, verdict, copy});
            }
        });
        // 执行动作(台账锁外):翻健康、投通知、发停止信号。ApplyHealth 返回翻后
        // 的 epoch(0 = 没翻),通知按它去重。
        for (const auto& finding : findings) {
            const auto& task = finding.task;
            const int task_id = task->snapshot.id;
            const std::string title = task->snapshot.title.empty() ? "(未命名)" : task->snapshot.title;
            switch (finding.verdict.action) {
                case agent::SupervisionAction::MarkQuiet:
                case agent::SupervisionAction::MarkSuspectTransport:
                case agent::SupervisionAction::MarkSuspectTool:
                case agent::SupervisionAction::MarkSuspectAgent: {
                    const std::uint64_t epoch =
                        ledger.ApplyHealth(task, finding.verdict.new_health, finding.verdict.reason_code);
                    if (epoch != 0) {
                        const std::string label = agent::HealthLabel(finding.verdict.new_health);
                        PushNoticeDeduped(task, epoch, finding.verdict.reason_code,
                                          "[监督] #" + std::to_string(task_id) + " " + title + " " + label + "(" +
                                              finding.verdict.reason_code +
                                              ")。硬超时与总墙钟照旧兜底,这里只提醒,不自动杀。");
                    }
                    break;
                }
                case agent::SupervisionAction::Recovered: {
                    const std::uint64_t epoch =
                        ledger.ApplyHealth(task, finding.verdict.new_health, finding.verdict.reason_code);
                    if (epoch != 0) {
                        PushNoticeDeduped(task, epoch, "progress.resumed",
                                          "[监督] #" + std::to_string(task_id) + " " + title +
                                              " 恢复正常:又见新的实质进展。");
                    }
                    break;
                }
                case agent::SupervisionAction::HostNotice: {
                    // 投一轮自救上下文(单子 §七 SuspectAgent 流转):轮次边界注入,
                    // 不打断正在跑的工具。通知只投一次。
                    ledger.PushHostNotice(
                        task, "[宿主监督提醒] 已连续 " + std::to_string(finding.vitals.stale_rounds) +
                                  " 个完整轮次没有产生任何可验证的新进展(工具结果与输出指纹不变)。"
                                  "请基于已有证据收敛:要么换一个能产生新事实的做法,要么写下当前结论"
                                  "并收口。仍无进展则本任务将按空转收口(部分结果会保留)。");
                    const std::uint64_t epoch =
                        ledger.ApplyHealth(task, finding.verdict.new_health, finding.verdict.reason_code);
                    PushNoticeDeduped(task, epoch != 0 ? epoch : 1, "agent.stale_fingerprint",
                                      "[监督] #" + std::to_string(task_id) + " " + title +
                                          " 疑似空转:已向它投递一次宿主提醒,给一轮自救上下文。");
                    break;
                }
                case agent::SupervisionAction::StopNoProgress: {
                    ledger.RequestNoProgressStop(task);
                    const std::uint64_t epoch =
                        ledger.ApplyHealth(task, finding.verdict.new_health, finding.verdict.reason_code);
                    PushNoticeDeduped(task, epoch != 0 ? epoch : 1, "agent.no_meaningful_progress",
                                      "[监督] #" + std::to_string(task_id) + " " + title +
                                          " 提醒后仍无实质进展,已发空转收口信号(部分结果与现场保留)。");
                    Deadline grace;
                    grace.kind = Deadline::Kind::NoProgressGrace;
                    grace.at = Clock::now() + std::chrono::seconds(no_progress_grace_secs_snapshot);
                    grace.task = task;
                    {
                        std::lock_guard<std::mutex> lock(mutex);
                        deadlines.push_back(std::move(grace));
                    }
                    cv.notify_all();
                    break;
                }
                case agent::SupervisionAction::None:
                    break;
            }
        }
        // 退场:终态任务不再看。活着与否用台账的收柄口径(锁内判,不锁外读)。
        std::vector<std::shared_ptr<tools::TaskRecord>> alive;
        for (auto& task : still_watching) {
            if (task != nullptr && !ledger.TaskSettled(task->snapshot.id)) {
                alive.push_back(std::move(task));
            }
        }
        {
            std::lock_guard<std::mutex> lock(mutex);
            for (auto& task : alive) {
                if (std::find(watches.begin(), watches.end(), task) == watches.end()) {
                    watches.push_back(std::move(task));
                }
            }
        }
    }

    void PushNoticeDeduped(const std::shared_ptr<tools::TaskRecord>& task, std::uint64_t health_epoch,
                           const std::string& reason_code, const std::string& text) {
        // 去重账归台账(单一去重口,P1-1 起台账侧通知与监督器共用同一本键账)。
        ledger.PushSupervisorNoticeDeduped(task, health_epoch, reason_code, text);
    }

    void BeginShutdown() {
        {
            std::lock_guard<std::mutex> lock(mutex);
            // 先断连再停线:检查点之后线程不再发起台账访问,宿主与台账的
            // 寿命账就此两清(在途一拍由收线窗兜住)。
            host_detached.store(true, std::memory_order_release);
            stop_requested.store(true, std::memory_order_release);
        }
        cv.notify_all();
        if (!thread.joinable()) {
            return;
        }
        // 有界收线:窗内退了就 join;极端挂死才 detach——脱离线程此后
        // 只摸 Loop(shared_ptr 保命)。
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

AgentSupervisor::AgentSupervisor(tools::TaskLedger& ledger)
    : ledger_(ledger),
      metrics_(&ledger),
      health_hooks_(std::make_shared<AgentHealthHookBus>()),
      loop_(std::make_shared<Loop>(ledger)) {
    // P2:台账侧事件(恢复起讫/工具结果不明/强收)经 sink 递进来——投钩子
    // 总线、记指标。sink 在台账锁内被调,OnLedgerSupervisionEvent 只入队,
    // 不回拿台账锁,无锁序环。析构头部先拔 sink(台账活得比本件长),晚
    // 到的台账事件摸不到悬垂的 this。
    ledger_.SetSupervisionEventSink([this](const agent::AgentSupervisionEvent& event) {
        OnLedgerSupervisionEvent(event);
    });
}

AgentSupervisor::~AgentSupervisor() {
    // 先拔台账 sink(无锁赋值,不与台账锁交叉):拔掉之后台账侧再无入口
    // 摸宿主。监督线程不在 sink 调用路径上(事件全走台账线程),拔除无
    // 并发调用之忧——台账事件发射与 sink 赋值都发生在台账使用方之间的
    // 会话收线序里。
    ledger_.SetSupervisionEventSink(nullptr);
    // 断开台账访问并请求停止:监督线程世界(Loop)由 shared_ptr 保命,
    // 跨截止时间存活也只摸 Loop。
    loop_->BeginShutdown();
}

void AgentSupervisor::ArmWallClock(const std::shared_ptr<tools::TaskRecord>& task, int timeout_secs,
                                   int grace_secs) {
    if (task == nullptr || timeout_secs <= 0) {
        return;
    }
    Loop::Deadline deadline;
    deadline.kind = Loop::Deadline::Kind::WallClock;
    deadline.at = task->snapshot.start_time + std::chrono::seconds(timeout_secs);
    deadline.task = task;
    deadline.wall_timeout_secs = timeout_secs;
    deadline.grace_secs = grace_secs > 0 ? grace_secs : 1;
    {
        std::lock_guard<std::mutex> lock(loop_->mutex);
        loop_->EnsureThreadStartedLocked();
        loop_->deadlines.push_back(std::move(deadline));
    }
    loop_->cv.notify_all();
}

void AgentSupervisor::WatchTask(const std::shared_ptr<tools::TaskRecord>& task) {
    if (task == nullptr) {
        return;
    }
    {
        std::lock_guard<std::mutex> lock(loop_->mutex);
        loop_->EnsureThreadStartedLocked();
        if (std::find(loop_->watches.begin(), loop_->watches.end(), task) == loop_->watches.end()) {
            loop_->watches.push_back(task);
        }
    }
    loop_->cv.notify_all();
}

void AgentSupervisor::SetThresholds(agent::SupervisionThresholds thresholds) {
    std::lock_guard<std::mutex> lock(loop_->mutex);
    loop_->thresholds = thresholds;
}

const agent::SupervisionThresholds& AgentSupervisor::thresholds() const { return loop_->thresholds; }

void AgentSupervisor::SetNoProgressGraceSecs(int secs) {
    std::lock_guard<std::mutex> lock(loop_->mutex);
    loop_->no_progress_grace_secs = secs > 0 ? secs : 1;
}

void AgentSupervisor::RequestStop() {
    {
        std::lock_guard<std::mutex> lock(loop_->mutex);
        loop_->stop_requested.store(true, std::memory_order_release);
    }
    loop_->cv.notify_all();
}

std::size_t AgentSupervisor::supervisor_thread_count_for_test() const {
    std::lock_guard<std::mutex> lock(loop_->mutex);
    return loop_->thread_started && loop_->thread.joinable() ? std::size_t{1} : std::size_t{0};
}

void AgentSupervisor::TickHealthForTest(bool host_resume_suspected) {
    loop_->HealthPass(host_resume_suspected);
}

void AgentSupervisor::OnLedgerSupervisionEvent(const agent::AgentSupervisionEvent& event) {
    // 只入队/计数:钩子在总线的专职线程上跑,慢与坏都不占监督拍(单子
    // §11.2 红线)。
    health_hooks_->Publish(event);
    metrics_.Count(event);
}

}  // namespace lubancode::runtime
