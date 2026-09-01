// agent_supervisor.hpp 的实现:单线程定时驱动、期限表、健康拍与通知去重。
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
}  // namespace

AgentSupervisor::AgentSupervisor(tools::TaskLedger& ledger) : ledger_(ledger) {}

AgentSupervisor::~AgentSupervisor() {
    RequestStop();
    if (!thread_.joinable()) {
        return;
    }
    // 有界收线(与旧每任务看门狗同款规矩):监督线程至多一个 tick 内退;
    // 给它一小扇窗,窗内退了就 join,极端挂死(系统调度丢拍)才 detach
    // 放行——那之后它不再碰台账(退出路径只剩原子置位)。
    const auto deadline = Clock::now() + std::chrono::milliseconds(1500);
    while (Clock::now() < deadline && !thread_exited_.load(std::memory_order_acquire)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    if (thread_exited_.load(std::memory_order_acquire)) {
        thread_.join();
    } else {
        thread_.detach();
    }
}

void AgentSupervisor::RequestStop() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stop_requested_ = true;
    }
    cv_.notify_all();
}

void AgentSupervisor::SetThresholds(agent::SupervisionThresholds thresholds) {
    std::lock_guard<std::mutex> lock(mutex_);
    thresholds_ = thresholds;
}

void AgentSupervisor::ArmWallClock(const std::shared_ptr<tools::TaskRecord>& task, int timeout_secs,
                                   int grace_secs) {
    if (task == nullptr || timeout_secs <= 0) {
        return;
    }
    Deadline deadline;
    deadline.kind = Deadline::Kind::WallClock;
    deadline.at = task->snapshot.start_time + std::chrono::seconds(timeout_secs);
    deadline.task = task;
    deadline.wall_timeout_secs = timeout_secs;
    deadline.grace_secs = grace_secs > 0 ? grace_secs : 1;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        EnsureThreadStarted();
        deadlines_.push_back(std::move(deadline));
    }
    cv_.notify_all();
}

void AgentSupervisor::WatchTask(const std::shared_ptr<tools::TaskRecord>& task) {
    if (task == nullptr) {
        return;
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        EnsureThreadStarted();
        if (std::find(watches_.begin(), watches_.end(), task) == watches_.end()) {
            watches_.push_back(task);
        }
    }
    cv_.notify_all();
}

std::size_t AgentSupervisor::supervisor_thread_count_for_test() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return thread_started_ && thread_.joinable() ? std::size_t{1} : std::size_t{0};
}

void AgentSupervisor::TickHealthForTest(bool host_resume_suspected) { HealthPass(host_resume_suspected); }

void AgentSupervisor::EnsureThreadStarted() {
    // 调用方已持 mutex_。
    if (thread_started_) {
        return;
    }
    thread_started_ = true;
    thread_ = std::thread([this] { RunLoop(); });
}

void AgentSupervisor::RunLoop() {
    std::unique_lock<std::mutex> lock(mutex_);
    last_tick_ = Clock::now();
    while (!stop_requested_) {
        const Clock::time_point next_deadline = NextDeadlineLocked();
        const Clock::time_point wake_at =
            next_deadline.time_since_epoch().count() != 0
                ? std::min(next_deadline, Clock::now() + kTickInterval)
                : Clock::now() + kTickInterval;
        cv_.wait_until(lock, wake_at);
        if (stop_requested_) {
            break;
        }
        const Clock::time_point now = Clock::now();
        const auto gap = now - last_tick_;
        last_tick_ = now;
        // 睡眠甄别(单子 §7.2):跳过的跨度超过 max(2*interval, 30s)(30s
        // 恒大于 1s,直接比 30s)即疑宿主刚醒。
        const bool host_resume_suspected = gap > kHostResumeMinGap;
        lock.unlock();
        ProcessDueDeadlines(now);
        HealthPass(host_resume_suspected);
        lock.lock();
    }
    thread_exited_.store(true, std::memory_order_release);
}

AgentSupervisor::Clock::time_point AgentSupervisor::NextDeadlineLocked() const {
    Clock::time_point best{};
    for (const auto& deadline : deadlines_) {
        if (best.time_since_epoch().count() == 0 || deadline.at < best) {
            best = deadline.at;
        }
    }
    return best;
}

void AgentSupervisor::ProcessDueDeadlines(const Clock::time_point now) {
    std::vector<Deadline> due;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (auto it = deadlines_.begin(); it != deadlines_.end();) {
            if (it->at <= now) {
                due.push_back(std::move(*it));
                it = deadlines_.erase(it);
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

void AgentSupervisor::FireWallClock(const Deadline& deadline) {
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
        std::lock_guard<std::mutex> lock(mutex_);
        deadlines_.push_back(std::move(grace));
    }
    cv_.notify_all();
}

void AgentSupervisor::FireWallGrace(const Deadline& deadline) {
    const auto& task = deadline.task;
    if (task == nullptr || task->finalized.load(std::memory_order_acquire)) {
        return;  // 停止信号起了作用,任务线程自己收的账更准
    }
    ledger_.ForceFinalizeWallClock(task, deadline.wall_timeout_secs);
    // 强收是终局一次性通知:固定代际 1,去重键由 task+reason 指认。
    PushNoticeDeduped(task, 1, "wall_clock.force_finalized",
                      "[监督] #" + std::to_string(task->snapshot.id) + " " +
                          (task->snapshot.title.empty() ? "(未命名)" : task->snapshot.title) +
                          " 墙钟到点后宽限期内仍未收口,已强制收账(" +
                          std::to_string(deadline.wall_timeout_secs) + "s)。");
}

void AgentSupervisor::FireNoProgressGrace(const Deadline& deadline) {
    const auto& task = deadline.task;
    if (task == nullptr || task->finalized.load(std::memory_order_acquire)) {
        return;
    }
    ledger_.ForceFinalizeNoProgress(task, task->progress.stale_rounds);
    PushNoticeDeduped(task, 1, "agent.no_meaningful_progress.finalized",
                      "[监督] #" + std::to_string(task->snapshot.id) + " " +
                          (task->snapshot.title.empty() ? "(未命名)" : task->snapshot.title) +
                          " 空转收口信号后宽限期内未收口,已按无进展强制收账(部分结果保留)。");
}

void AgentSupervisor::HealthPass(bool host_resume_suspected) {
    struct Finding {
        std::shared_ptr<tools::TaskRecord> task;
        agent::SupervisionVerdict verdict;
        agent::TaskVitals vitals;
    };
    std::vector<Finding> findings;
    std::vector<std::shared_ptr<tools::TaskRecord>> still_watching;
    agent::SupervisionThresholds thresholds_snapshot;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        thresholds_snapshot = thresholds_;
        watches_.swap(still_watching);
    }
    // 取视景:visitor 在台账锁内跑,只攒事实,不回拿监督锁。
    ledger_.ForEachAliveVitals([&](const std::shared_ptr<tools::TaskRecord>& task,
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
                    ledger_.ApplyHealth(task, finding.verdict.new_health, finding.verdict.reason_code);
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
                    ledger_.ApplyHealth(task, finding.verdict.new_health, finding.verdict.reason_code);
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
                ledger_.PushHostNotice(
                    task, "[宿主监督提醒] 已连续 " + std::to_string(finding.vitals.stale_rounds) +
                              " 个完整轮次没有产生任何可验证的新进展(工具结果与输出指纹不变)。"
                              "请基于已有证据收敛:要么换一个能产生新事实的做法,要么写下当前结论"
                              "并收口。仍无进展则本任务将按空转收口(部分结果会保留)。");
                const std::uint64_t epoch =
                    ledger_.ApplyHealth(task, finding.verdict.new_health, finding.verdict.reason_code);
                PushNoticeDeduped(task, epoch != 0 ? epoch : 1, "agent.stale_fingerprint",
                                  "[监督] #" + std::to_string(task_id) + " " + title +
                                      " 疑似空转:已向它投递一次宿主提醒,给一轮自救上下文。");
                break;
            }
            case agent::SupervisionAction::StopNoProgress: {
                ledger_.RequestNoProgressStop(task);
                const std::uint64_t epoch =
                    ledger_.ApplyHealth(task, finding.verdict.new_health, finding.verdict.reason_code);
                PushNoticeDeduped(task, epoch != 0 ? epoch : 1, "agent.no_meaningful_progress",
                                  "[监督] #" + std::to_string(task_id) + " " + title +
                                      " 提醒后仍无实质进展,已发空转收口信号(部分结果与现场保留)。");
                Deadline grace;
                grace.kind = Deadline::Kind::NoProgressGrace;
                grace.at = Clock::now() + std::chrono::seconds(no_progress_grace_secs_);
                grace.task = task;
                {
                    std::lock_guard<std::mutex> lock(mutex_);
                    deadlines_.push_back(std::move(grace));
                }
                cv_.notify_all();
                break;
            }
            case agent::SupervisionAction::None:
                break;
        }
    }
    // 退场:终态任务不再看。活着与否用台账的收柄口径(锁内判,不锁外读)。
    std::vector<std::shared_ptr<tools::TaskRecord>> alive;
    for (auto& task : still_watching) {
        if (task != nullptr && !ledger_.TaskSettled(task->snapshot.id)) {
            alive.push_back(std::move(task));
        }
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (auto& task : alive) {
            if (std::find(watches_.begin(), watches_.end(), task) == watches_.end()) {
                watches_.push_back(std::move(task));
            }
        }
    }
}

void AgentSupervisor::PushNoticeDeduped(const std::shared_ptr<tools::TaskRecord>& task,
                                        std::uint64_t health_epoch, const std::string& reason_code,
                                        const std::string& text) {
    if (task == nullptr || health_epoch == 0) {
        return;
    }
    const std::string key =
        std::to_string(task->snapshot.id) + ":" + std::to_string(health_epoch) + ":" + reason_code;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!noticed_keys_.insert(key).second) {
            return;  // 同一健康代际同一因,只弹一次(单子 §十)
        }
        // 防账面无限涨:去重键保留最近若干即可(旧代际不会再来)。
        if (noticed_keys_.size() > 512) {
            noticed_keys_.erase(noticed_keys_.begin());
        }
    }
    ledger_.PushSupervisorNotice(text);
}

}  // namespace lubancode::runtime
