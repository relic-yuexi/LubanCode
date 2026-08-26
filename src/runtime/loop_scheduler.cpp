// LoopScheduler(loop 单第 1 期)实现。内存真值 + 事件攒账;timer 线程
// 只算 due 发 wake,不碰 history/ProcessLine/TUI/权限(单子铁规矩)。

#include "runtime/loop_scheduler.hpp"

#include <algorithm>
#include <exception>
#include <thread>

#include "platform/wall_clock.hpp"
#include "runtime/budget_gate.hpp"
#include "runtime/id_authority.hpp"
#include "runtime/retry_backoff.hpp"

namespace lubancode::runtime::loop {

WallTimeMs LoopClock::NowWallMs() const {
    // 墙钟(system_clock):账面、存档、resume 全用它。批五起五套台账的
    // 真钟同读 platform 这一枚(口径不变,只收源);进程内等待由
    // scheduler 的轮询间隔承载(steady 语义),这里不掺和。
    return platform::WallClockNowMs();
}

struct LoopScheduler::Impl {
    mutable std::mutex mutex;
    Options options;
    std::shared_ptr<LoopClock> clock;
    std::map<std::string, TaskEntry> tasks;   // task_id -> entry
    std::vector<LoopSchedulerEvent> pending;  // 攒账,TakeEvents 交出去
    std::uint64_t next_seq = 1;               // creation_seq 的源(稳定排序键)
    IdAuthority ids;  // loop-N 发号(批五:台账 id 收编同一发号器;session
                      // 域号,一场 scheduler 一只实例,回放 SeedPrefixedId 续)
    bool store_failed = false;
    bool stopping = false;
    std::thread timer;
    std::atomic<bool> timer_running{false};


    void EmitLocked(const std::string& family, const std::string& event, const LoopTask& task,
                    const LoopTick* tick, nlohmann::json payload) {
        LoopSchedulerEvent e;
        e.family = family;
        e.event = event;
        e.task_id = task.task_id;
        if (tick != nullptr) {
            e.tick_id = tick->tick_id;
        }
        e.timestamp_ms = clock ? clock->NowWallMs() : 0;
        e.payload = std::move(payload);
        pending.push_back(std::move(e));
    }

    // 转换表用动词(event="pause"),存档行用过去式("paused",单子 JSONL
    // 样本钉的形状)。落账名走这张映射;tick 级转换原样落。
    static const char* LedgerEventName(const std::string& event) {
        if (event == "pause") return "paused";
        if (event == "resume") return "resumed";
        if (event == "stop") return "stopped";
        if (event == "complete") return "completed";
        if (event == "expire") return "expired";
        if (event == "break") return "broken";
        if (event == "tick_due") return "due";
        if (event == "tick_started") return "started";
        if (event == "tick_finished") return "finished";
        if (event == "permission_wait") return "permission_wait";
        if (event == "permission_resolved") return "permission_resolved";
        if (event == "backoff") return "backoff";
        if (event == "backoff_done") return "backoff_done";
        return event.c_str();
    }

    // 转换 + 落账。非法转换不走(返回 false,调用方自定),合法即写。
    // suppress_event:转换只换态不落账(due 事件由 PumpDueTick 手发,带
    // missed 明细;这里的 tick_due 转换是同一件事的状态面,不重复记)。
    bool ApplyLocked(LoopTask& task, const std::string& event, LoopTaskState to,
                     const LoopTick* tick, nlohmann::json payload = nlohmann::json::object(),
                     bool suppress_event = false) {
        if (!IsLoopTransitionAllowed(task.state, event, to)) {
            return false;
        }
        task.state = to;
        if (!suppress_event) {
            const char* ledger_event = LedgerEventName(event);
            EmitLocked(to == LoopTaskState::Due || to == LoopTaskState::Running ? "loop_tick_v1"
                                                                               : "loop_task_v1",
                       ledger_event, task, tick, std::move(payload));
        }
        return true;
    }

};

namespace {
// 活跃态判定(Impl 与外层共用):非终态且非 Paused。
bool IsActiveState(LoopTaskState s) {
    return !IsLoopTerminal(s) && s != LoopTaskState::Paused;
}
}  // namespace

LoopScheduler::LoopScheduler(Options options, std::shared_ptr<LoopClock> clock)
    : impl_(std::make_unique<Impl>()) {
    impl_->options = std::move(options);
    impl_->clock = clock ? std::move(clock) : std::make_shared<LoopClock>();
}

LoopScheduler::~LoopScheduler() {
    StopTimer();
}

// ---------------------------------------------------------------------------
// 任务生命周期
// ---------------------------------------------------------------------------

auto LoopScheduler::Create(std::string prompt, std::chrono::seconds interval, WallTimeMs now_ms,
                           std::string cwd_identity, std::string session_id, LoopPromptSource source,
                           std::string prompt_file, std::function<std::string()> id_issuer)
    -> CommandResult {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    CommandResult result;
    if (!impl_->options.enabled) {
        result.error_code = "loop.disabled";
        result.error_message = "features.loop 未开启";
        return result;
    }
    if (impl_->store_failed) {
        result.error_code = kErrLoopStoreUnavailable;
        result.error_message = "存档写盘失败,loop 已熔断";
        return result;
    }
    if (interval < LoopDefaults::kMinimumInterval) {
        result.error_code = kErrLoopIntervalTooSmall;
        result.error_message = "间隔小于最小值 1m";
        return result;
    }
    if (interval > LoopDefaults::kMaximumInterval) {
        result.error_code = kErrLoopIntervalTooLarge;
        result.error_message = "间隔大于最大值 7d";
        return result;
    }
    if (source == LoopPromptSource::Inline && prompt.empty()) {
        result.error_code = kErrLoopPromptEmpty;
        result.error_message = "inline prompt 不能为空";
        return result;
    }
    int active = 0;
    for (const auto& [id, entry] : impl_->tasks) {
        (void)id;
        if (IsActiveState(entry.task.state)) {
            ++active;
        }
    }
    if (active >= impl_->options.max_active_per_session) {
        result.error_code = kErrLoopTooManyActive;
        result.error_message = "active 任务数已到上限";
        return result;
    }
    LoopTask task;
    // 发号(批五):默认 id 走 IdAuthority;注入口照旧(测试定死 id)。
    // next_seq 只当 creation_seq 的源(稳定排序键),不再拼 id。
    task.task_id = id_issuer ? id_issuer() : impl_->ids.NextPrefixedId("loop");
    while (impl_->tasks.count(task.task_id) > 0) {
        task.task_id = id_issuer ? id_issuer() : impl_->ids.NextPrefixedId("loop");
    }
    ++impl_->next_seq;
    task.session_id = std::move(session_id);
    task.prompt = std::move(prompt);
    task.prompt_source = source;
    task.prompt_file = std::move(prompt_file);
    task.interval = interval;
    task.created_at_ms = now_ms;
    task.expires_at_ms = now_ms +
                         std::chrono::duration_cast<std::chrono::milliseconds>(
                             LoopDefaults::kExpiryAge)
                             .count();
    task.next_due_at_ms = ComputeResumeNextDue(now_ms, interval);
    task.state = LoopTaskState::Active;
    task.cwd_identity = std::move(cwd_identity);
    task.creation_seq = impl_->next_seq;

    // created 的 payload 就是整套 LoopTask json(回放 from_json 直读,不
    // 另立一份平行 shape)。
    nlohmann::json payload = task.to_json();
    impl_->EmitLocked("loop_task_v1", "created", task, nullptr, std::move(payload));

    impl_->tasks[task.task_id] = TaskEntry{task, std::nullopt, 0};
    result.ok = true;
    result.payload["task_id"] = task.task_id;
    result.payload["next_due_at_ms"] = task.next_due_at_ms;
    return result;
}

auto LoopScheduler::Pause(const std::string& task_id, WallTimeMs now_ms, const std::string& reason)
    -> CommandResult {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    CommandResult result;
    // all:逐只 pause(只动非终态)。
    if (task_id == "all") {
        int paused = 0;
        for (auto& [id, entry] : impl_->tasks) {
            if (IsLoopTerminal(entry.task.state) || entry.task.state == LoopTaskState::Paused) {
                continue;
            }
            if (impl_->ApplyLocked(entry.task, "pause", LoopTaskState::Paused, nullptr,
                                   {{"reason", reason}, {"at_ms", now_ms}})) {
                ++paused;
            }
        }
        result.ok = true;
        result.payload["paused"] = paused;
        return result;
    }
    auto it = impl_->tasks.find(task_id);
    if (it == impl_->tasks.end()) {
        result.error_code = kErrLoopNotFound;
        result.error_message = "任务不存在: " + task_id;
        return result;
    }
    LoopTask& task = it->second.task;
    if (IsLoopTerminal(task.state)) {
        result.error_code = kErrLoopTerminal;
        result.error_message = "任务已终态";
        return result;
    }
    if (!impl_->ApplyLocked(task, "pause", LoopTaskState::Paused, nullptr,
                            {{"reason", reason}, {"at_ms", now_ms}})) {
        result.error_code = kErrLoopInvalidTransition;
        result.error_message = "pause 转换不合法";
        return result;
    }
    result.ok = true;
    result.payload["task_id"] = task.task_id;
    return result;
}

auto LoopScheduler::Resume(const std::string& task_id, WallTimeMs now_ms) -> CommandResult {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    CommandResult result;
    if (task_id == "all") {
        int resumed = 0;
        for (auto& [id, entry] : impl_->tasks) {
            if (entry.task.state != LoopTaskState::Paused) {
                continue;
            }
            if (impl_->ApplyLocked(entry.task, "resume", LoopTaskState::Active, nullptr,
                                   {{"at_ms", now_ms}})) {
                entry.task.next_due_at_ms = ComputeResumeNextDue(now_ms, entry.task.interval);
                ++resumed;
            }
        }
        result.ok = true;
        result.payload["resumed"] = resumed;
        return result;
    }
    auto it = impl_->tasks.find(task_id);
    if (it == impl_->tasks.end()) {
        result.error_code = kErrLoopNotFound;
        result.error_message = "任务不存在: " + task_id;
        return result;
    }
    LoopTask& task = it->second.task;
    if (IsLoopTerminal(task.state)) {
        result.error_code = kErrLoopTerminal;
        result.error_message = "任务已终态";
        return result;
    }
    if (!impl_->ApplyLocked(task, "resume", LoopTaskState::Active, nullptr, {{"at_ms", now_ms}})) {
        result.error_code = kErrLoopBusy;
        result.error_message = "resume 只收 Paused";
        return result;
    }
    // pause/resume 从 now + interval 起,不补旧账。
    task.next_due_at_ms = ComputeResumeNextDue(now_ms, task.interval);
    result.ok = true;
    result.payload["task_id"] = task.task_id;
    result.payload["next_due_at_ms"] = task.next_due_at_ms;
    return result;
}

auto LoopScheduler::Stop(const std::string& task_id, WallTimeMs now_ms, const std::string& reason)
    -> CommandResult {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    CommandResult result;
    if (task_id == "all") {
        int stopped = 0;
        for (auto& [id, entry] : impl_->tasks) {
            if (IsLoopTerminal(entry.task.state)) {
                continue;
            }
            if (impl_->ApplyLocked(entry.task, "stop", LoopTaskState::Cancelled, nullptr,
                                   {{"reason", reason}, {"at_ms", now_ms}})) {
                entry.task.active_turn_id.reset();
                ++stopped;
            }
        }
        result.ok = true;
        result.payload["stopped"] = stopped;
        return result;
    }
    auto it = impl_->tasks.find(task_id);
    if (it == impl_->tasks.end()) {
        result.error_code = kErrLoopNotFound;
        result.error_message = "任务不存在: " + task_id;
        return result;
    }
    LoopTask& task = it->second.task;
    if (!impl_->ApplyLocked(task, "stop", LoopTaskState::Cancelled, nullptr,
                            {{"reason", reason}, {"at_ms", now_ms}})) {
        result.error_code = kErrLoopTerminal;
        result.error_message = "任务已终态";
        return result;
    }
    task.active_turn_id.reset();
    result.ok = true;
    result.payload["task_id"] = task.task_id;
    return result;
}

auto LoopScheduler::RunNow(const std::string& task_id, WallTimeMs now_ms) -> CommandResult {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    CommandResult result;
    auto it = impl_->tasks.find(task_id);
    if (it == impl_->tasks.end()) {
        result.error_code = kErrLoopNotFound;
        result.error_message = "任务不存在: " + task_id;
        return result;
    }
    LoopTask& task = it->second.task;
    if (IsLoopTerminal(task.state)) {
        result.error_code = kErrLoopTerminal;
        result.error_message = "任务已终态";
        return result;
    }
    if (task.state != LoopTaskState::Active && task.state != LoopTaskState::Paused &&
        task.state != LoopTaskState::BackingOff && task.state != LoopTaskState::Due) {
        result.error_code = kErrLoopBusy;
        result.error_message = "当前态不能立即补拍";
        return result;
    }
    // 立即补一拍,不改原 cadence:next_due 拉到 now,下一圈泵走。原时间轴
    // 不动(cadence 由 tick finished 后的 ComputeDueAdvance 从原 slot 推进;
    // 这里只记 requested 事件与拉早 next_due)。
    if (task.state == LoopTaskState::Paused) {
        if (!impl_->ApplyLocked(task, "resume", LoopTaskState::Active, nullptr, {{"reason", "run"}})) {
            result.error_code = kErrLoopInvalidTransition;
            return result;
        }
    } else if (task.state == LoopTaskState::BackingOff) {
        if (!impl_->ApplyLocked(task, "backoff_done", LoopTaskState::Active, nullptr)) {
            result.error_code = kErrLoopInvalidTransition;
            return result;
        }
        it->second.backoff_until_ms = 0;
    }
    it->second.run_now_requested = true;
    impl_->EmitLocked("loop_task_v1", "run_requested", task, nullptr, {{"at_ms", now_ms}});
    result.ok = true;
    result.payload["task_id"] = task.task_id;
    return result;
}

auto LoopScheduler::Complete(const std::string& task_id, WallTimeMs now_ms, const std::string& reason)
    -> CommandResult {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    CommandResult result;
    auto it = impl_->tasks.find(task_id);
    if (it == impl_->tasks.end()) {
        result.error_code = kErrLoopNotFound;
        result.error_message = "任务不存在: " + task_id;
        return result;
    }
    LoopTask& task = it->second.task;
    if (!impl_->ApplyLocked(task, "complete", LoopTaskState::Completed, nullptr,
                            {{"reason", reason}, {"at_ms", now_ms}})) {
        result.error_code = kErrLoopInvalidTransition;
        result.error_message = "complete 只从运行态来";
        return result;
    }
    task.active_turn_id.reset();
    result.ok = true;
    result.payload["task_id"] = task.task_id;
    return result;
}

// ---------------------------------------------------------------------------
// 泵
// ---------------------------------------------------------------------------

bool LoopScheduler::HasActiveTasks() const {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    for (const auto& [id, entry] : impl_->tasks) {
        (void)id;
        if (IsActiveState(entry.task.state)) {
            return true;
        }
    }
    return false;
}

bool LoopScheduler::HasDueWork(WallTimeMs now_ms) const {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (impl_->store_failed || !impl_->options.enabled) {
        return false;
    }
    for (const auto& [id, entry] : impl_->tasks) {
        (void)id;
        const bool run_now = entry.run_now_requested;
        if (entry.task.state == LoopTaskState::Active && !LoopExpired(entry.task.expires_at_ms, now_ms) &&
            (run_now || now_ms >= entry.task.next_due_at_ms)) {
            return true;
        }
    }
    return false;
}

std::optional<DispatchedTick> LoopScheduler::PumpDueTick(WallTimeMs now_ms, const std::string& turn_id) {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (impl_->store_failed || !impl_->options.enabled) {
        return std::nullopt;
    }
    // 到点的候选:按消费点(next_due 或 run_now 的 now),再按 creation_seq
    // 稳定排序。
    const TaskEntry* chosen = nullptr;
    std::int64_t chosen_consume_at = 0;
    for (const auto& [id, entry] : impl_->tasks) {
        (void)id;
        if (entry.task.state != LoopTaskState::Active) {
            continue;  // single-flight:Running/Waiting/BackingOff/Paused 不取
        }
        if (LoopExpired(entry.task.expires_at_ms, now_ms)) {
            continue;  // 过期的走 SweepExpiry 的 expired 事件,不当 due 消费
        }
        const bool run_now = entry.run_now_requested;
        if (!run_now && now_ms < entry.task.next_due_at_ms) {
            continue;
        }
        const std::int64_t consume_at =
            run_now ? now_ms : entry.task.next_due_at_ms;
        if (chosen == nullptr || consume_at < chosen_consume_at ||
            (consume_at == chosen_consume_at &&
             entry.task.creation_seq < chosen->task.creation_seq)) {
            chosen = &entry;
            chosen_consume_at = consume_at;
        }
    }
    if (chosen == nullptr) {
        return std::nullopt;
    }
    TaskEntry& entry = const_cast<TaskEntry&>(*chosen);
    LoopTask& task = entry.task;

    // due 事件 + 错过合并账。run_now 的补拍:missed=0、scheduled_at=now、
    // cadence 不动(下一拍仍在原 slot);常规拍按原时间轴推进。
    const bool run_now = entry.run_now_requested;
    entry.run_now_requested = false;
    const auto advance =
        run_now ? DueComputation{0, task.next_due_at_ms}
                : ComputeDueAdvance(task.next_due_at_ms, now_ms, task.interval);
    ++task.tick_seq;
    LoopTick tick;
    tick.task_id = task.task_id;
    tick.tick_no = task.tick_seq;
    tick.tick_id = task.task_id + "#" + std::to_string(tick.tick_no);
    tick.scheduled_at_ms = run_now ? now_ms : task.next_due_at_ms;
    tick.prompt_sha256 = task.prompt_sha256;
    tick.source = task.prompt_source;
    tick.missed_count = advance.missed;
    task.skipped_count += advance.missed;
    tick.outcome = LoopTickOutcome::Pending;

    nlohmann::json due_payload;
    due_payload["tick"] = tick.tick_no;
    due_payload["scheduled_at_ms"] = tick.scheduled_at_ms;
    due_payload["missed"] = advance.missed;
    impl_->EmitLocked("loop_tick_v1", "due", task, &tick, std::move(due_payload));

    // Active -> Due -> Running(due 事件的账已手发,这里只换态)。
    if (!impl_->ApplyLocked(task, "tick_due", LoopTaskState::Due, &tick, {}, /*suppress_event=*/true)) {
        return std::nullopt;  // 防御:状态表钉死过,走不到
    }
    tick.dispatched_at_ms = now_ms;
    tick.turn_id = turn_id;
    if (!impl_->ApplyLocked(task, "tick_started", LoopTaskState::Running, &tick,
                            {{"turn_id", turn_id}, {"tick", tick.tick_no}})) {
        return std::nullopt;
    }
    task.active_turn_id = turn_id;
    ++task.run_count;
    entry.current_tick = tick;

    // cadence 先按取件时刻的 future slot 预推;收口时 FinishTick 再按
    // finish 时刻补推(执行拖过了 slot 的场合,下一拍顺延,不倒贴着 now
    // 立刻又开一轮)。
    task.next_due_at_ms = advance.next_due_at_ms;

    DispatchedTick out;
    out.task = task;
    out.tick = tick;
    out.text = task.prompt;
    return out;
}

// ---------------------------------------------------------------------------
// 拍子收口
// ---------------------------------------------------------------------------

void LoopScheduler::FinishTick(const std::string& tick_id, LoopTickOutcome outcome, WallTimeMs now_ms,
                               std::string error_code) {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    // tick_id = "<task_id>#<n>":拆回 task。
    const std::size_t hash = tick_id.rfind('#');
    if (hash == std::string::npos) {
        return;
    }
    auto it = impl_->tasks.find(tick_id.substr(0, hash));
    if (it == impl_->tasks.end()) {
        return;
    }
    TaskEntry& entry = it->second;
    LoopTask& task = entry.task;
    if (!entry.current_tick.has_value() || entry.current_tick->tick_id != tick_id) {
        return;  // 不是当前拍:迟到收口,留账不动状态
    }
    entry.current_tick->outcome = outcome;
    entry.current_tick->error_code = error_code;
    entry.current_tick->finished_at_ms = now_ms;
    entry.current_tick->next_due_at_ms = task.next_due_at_ms;

    // 连败/连拒账(批五:连撞计数的机制走 StreakMeter,计数仍落在
    // task 域字段上——存档要)。
    StreakMeter provider_streak{LoopDefaults::kProviderFailPauseThreshold,
                                static_cast<std::int64_t>(task.consecutive_failures)};
    if (outcome == LoopTickOutcome::ProviderError || outcome == LoopTickOutcome::RateLimited) {
        provider_streak.NoteBad();
    } else {
        provider_streak.NoteGood();
    }
    task.consecutive_failures = static_cast<std::uint64_t>(provider_streak.count);
    StreakMeter denial_streak{LoopDefaults::kDenialPauseThreshold,
                              static_cast<std::int64_t>(task.consecutive_denials)};
    if (outcome == LoopTickOutcome::Declined) {
        denial_streak.NoteBad();
    } else {
        denial_streak.NoteGood();
    }
    task.consecutive_denials = static_cast<std::uint64_t>(denial_streak.count);

    nlohmann::json payload;
    payload["tick"] = entry.current_tick->tick_no;
    payload["outcome"] = ToString(outcome);
    if (!error_code.empty()) {
        payload["error_code"] = error_code;
    }
    payload["next_due_at_ms"] = task.next_due_at_ms;
    // 熔断门:存档写盘失败时不再启动新 tick(单子:失去恢复账后继续跑,
    // 风险大过便利)。已跑的这拍照常收口。
    if (impl_->store_failed) {
        impl_->EmitLocked("loop_tick_v1", "finished", task, &*entry.current_tick, std::move(payload));
        entry.current_tick.reset();
        return;
    }
    // provider 连败五拍自动 Pause;denial 连三拍自动 Pause。
    const bool provider_tripped = provider_streak.tripped();
    const bool denial_tripped = denial_streak.tripped();
    if (LoopExpired(task.expires_at_ms, now_ms)) {
        impl_->EmitLocked("loop_tick_v1", "finished", task, &*entry.current_tick, std::move(payload));
        entry.current_tick.reset();
        impl_->ApplyLocked(task, "expire", LoopTaskState::Expired, nullptr, {{"at_ms", now_ms}});
        return;
    }
    if (provider_tripped || denial_tripped) {
        impl_->EmitLocked("loop_tick_v1", "finished", task, &*entry.current_tick, std::move(payload));
        entry.current_tick.reset();
        const std::string why = provider_tripped ? "provider_failures" : "denials";
        impl_->ApplyLocked(task, "pause", LoopTaskState::Paused, nullptr,
                           {{"reason", why}, {"auto", true}, {"at_ms", now_ms}});
        return;
    }
    // 正常收口:cadence 补推到 finish 时刻的 future slot,再 tick_finished
    // -> Active。
    const auto readvance =
        ComputeDueAdvance(task.next_due_at_ms, now_ms, task.interval);
    task.next_due_at_ms = readvance.next_due_at_ms;
    entry.current_tick->next_due_at_ms = task.next_due_at_ms;
    impl_->EmitLocked("loop_tick_v1", "finished", task, &*entry.current_tick, std::move(payload));
    entry.current_tick.reset();
    task.active_turn_id.reset();
    impl_->ApplyLocked(task, "tick_finished", LoopTaskState::Active, nullptr);
}

std::optional<std::chrono::milliseconds> LoopScheduler::RetryBackoffFor(const std::string& tick_id,
                                                                        WallTimeMs now_ms) {
    (void)now_ms;
    std::lock_guard<std::mutex> lock(impl_->mutex);
    const std::size_t hash = tick_id.rfind('#');
    if (hash == std::string::npos) {
        return std::nullopt;
    }
    auto it = impl_->tasks.find(tick_id.substr(0, hash));
    if (it == impl_->tasks.end() || !it->second.current_tick.has_value() ||
        it->second.current_tick->tick_id != tick_id) {
        return std::nullopt;
    }
    LoopTick& tick = *it->second.current_tick;
    if (tick.attempts >= static_cast<std::uint32_t>(LoopDefaults::kMaxAttemptsPerTick)) {
        return std::nullopt;  // 首发+两次重试用完
    }
    // 退避阶梯(批五):5s/15s 两级走公共退避件的 Ladder 档——越表即
    // 没得再等,与 attempts 上限同一道闸(这里 attempts 顶在先,阶梯
    // 越表是同一件事的双保险)。
    BackoffPolicy ladder;
    ladder.kind = BackoffPolicy::Kind::Ladder;
    ladder.ladder_ms = {
        std::chrono::duration_cast<std::chrono::milliseconds>(LoopDefaults::kRetryBackoffFirst).count(),
        std::chrono::duration_cast<std::chrono::milliseconds>(LoopDefaults::kRetryBackoffSecond).count(),
    };
    auto wait = BackoffWaitMs(ladder, tick.attempts);
    if (!wait.has_value()) {
        return std::nullopt;
    }
    ++tick.attempts;
    return wait;
}

void LoopScheduler::NotePermissionWait(const std::string& tick_id, WallTimeMs now_ms) {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    const std::size_t hash = tick_id.rfind('#');
    if (hash == std::string::npos) {
        return;
    }
    auto it = impl_->tasks.find(tick_id.substr(0, hash));
    if (it == impl_->tasks.end()) {
        return;
    }
    LoopTask& task = it->second.task;
    impl_->ApplyLocked(task, "permission_wait", LoopTaskState::WaitingPermission, nullptr,
                       {{"tick_id", tick_id}, {"at_ms", now_ms}});
}

void LoopScheduler::NotePermissionResolved(const std::string& tick_id, WallTimeMs now_ms) {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    const std::size_t hash = tick_id.rfind('#');
    if (hash == std::string::npos) {
        return;
    }
    auto it = impl_->tasks.find(tick_id.substr(0, hash));
    if (it == impl_->tasks.end()) {
        return;
    }
    LoopTask& task = it->second.task;
    impl_->ApplyLocked(task, "permission_resolved", LoopTaskState::Running, nullptr,
                       {{"tick_id", tick_id}, {"at_ms", now_ms}});
}

void LoopScheduler::NotePermissionDeclined(const std::string& tick_id, WallTimeMs now_ms) {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    const std::size_t hash = tick_id.rfind('#');
    if (hash == std::string::npos) {
        return;
    }
    auto it = impl_->tasks.find(tick_id.substr(0, hash));
    if (it == impl_->tasks.end()) {
        return;
    }
    LoopTask& task = it->second.task;
    // 连拍账在 FinishTick(Declined) 里 +1;这里只留审批被拒的事件账
    //(装配层要在 UI 上说一句"本拍被拒,下一拍照常")。
    impl_->EmitLocked("loop_task_v1", "permission_declined", task, nullptr,
                      {{"tick_id", tick_id}, {"at_ms", now_ms}});
}

void LoopScheduler::NoteRateLimited(const std::string& tick_id, std::chrono::seconds retry_after,
                                    WallTimeMs now_ms) {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    const std::size_t hash = tick_id.rfind('#');
    if (hash == std::string::npos) {
        return;
    }
    auto it = impl_->tasks.find(tick_id.substr(0, hash));
    if (it == impl_->tasks.end()) {
        return;
    }
    TaskEntry& entry = it->second;
    LoopTask& task = entry.task;
    entry.backoff_until_ms =
        now_ms + std::chrono::duration_cast<std::chrono::milliseconds>(retry_after).count();
    if (impl_->ApplyLocked(task, "backoff", LoopTaskState::BackingOff, nullptr,
                           {{"retry_after_ms",
                             std::chrono::duration_cast<std::chrono::milliseconds>(retry_after).count()},
                            {"until_ms", entry.backoff_until_ms}})) {
        // 退避结束点重排 cadence,不双发。
        task.next_due_at_ms = std::max(task.next_due_at_ms, entry.backoff_until_ms);
    }
}

// ---------------------------------------------------------------------------
// 维护
// ---------------------------------------------------------------------------

void LoopScheduler::SweepExpiry(WallTimeMs now_ms) {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    for (auto& [id, entry] : impl_->tasks) {
        (void)id;
        LoopTask& task = entry.task;
        if (IsLoopTerminal(task.state)) {
            continue;
        }
        if (!LoopExpired(task.expires_at_ms, now_ms)) {
            continue;
        }
        // Running 的拍让 FinishTick 收口时落 expired;这里只收非运行态。
        if (task.state == LoopTaskState::Running || task.state == LoopTaskState::WaitingPermission) {
            continue;
        }
        if (task.state == LoopTaskState::BackingOff) {
            impl_->ApplyLocked(task, "backoff_done", LoopTaskState::Active, nullptr);
        }
        if (task.state == LoopTaskState::Due) {
            impl_->ApplyLocked(task, "tick_finished", LoopTaskState::Active, nullptr);
        }
        impl_->ApplyLocked(task, "expire", LoopTaskState::Expired, nullptr, {{"at_ms", now_ms}});
    }
}

void LoopScheduler::NoteCwdChanged(const std::string& cwd_identity, WallTimeMs now_ms) {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    for (auto& [id, entry] : impl_->tasks) {
        (void)id;
        LoopTask& task = entry.task;
        if (IsLoopTerminal(task.state) || task.state == LoopTaskState::Paused) {
            continue;
        }
        if (task.cwd_identity != cwd_identity) {
            impl_->ApplyLocked(task, "pause", LoopTaskState::Paused, nullptr,
                               {{"reason", "cwd_moved"}, {"at_ms", now_ms}});
        }
    }
}

void LoopScheduler::FailStore(const std::string& reason) {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (impl_->store_failed) {
        return;
    }
    impl_->store_failed = true;
    LoopSchedulerEvent e;
    e.family = "loop_task_v1";
    e.event = "store_failed";
    e.task_id = "*";
    e.timestamp_ms = impl_->clock->NowWallMs();
    e.payload["reason"] = reason;
    impl_->pending.push_back(std::move(e));
}

// ---------------------------------------------------------------------------
// 查询
// ---------------------------------------------------------------------------

std::vector<LoopScheduler::TaskView> LoopScheduler::Snapshot(WallTimeMs now_ms) const {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    std::vector<TaskView> out;
    out.reserve(impl_->tasks.size());
    for (const auto& [id, entry] : impl_->tasks) {
        (void)id;
        TaskView view;
        view.task = entry.task;
        view.has_current_tick = entry.current_tick.has_value();
        if (view.has_current_tick) {
            view.current_tick = *entry.current_tick;
        }
        view.backoff_until_ms = entry.backoff_until_ms;
        // delayed:Active 且超 interval*2 未消费(连续用户输入可能让 loop
        // 饿死;只提示,不抢)。
        const std::int64_t two_intervals =
            2 * static_cast<std::int64_t>(entry.task.interval.count()) * 1000;
        view.delayed = entry.task.state == LoopTaskState::Active &&
                       now_ms > entry.task.next_due_at_ms + two_intervals;
        out.push_back(std::move(view));
    }
    return out;
}

std::optional<LoopScheduler::TaskView> LoopScheduler::Find(const std::string& task_id,
                                                           WallTimeMs now_ms) const {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    auto it = impl_->tasks.find(task_id);
    if (it == impl_->tasks.end()) {
        return std::nullopt;
    }
    TaskView view;
    view.task = it->second.task;
    view.has_current_tick = it->second.current_tick.has_value();
    if (view.has_current_tick) {
        view.current_tick = *it->second.current_tick;
    }
    view.backoff_until_ms = it->second.backoff_until_ms;
    const std::int64_t two_intervals =
        2 * static_cast<std::int64_t>(it->second.task.interval.count()) * 1000;
    view.delayed = it->second.task.state == LoopTaskState::Active &&
                   now_ms > it->second.task.next_due_at_ms + two_intervals;
    return view;
}

std::string LoopScheduler::ResolveTaskId(const std::string& input) const {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (impl_->tasks.count(input) > 0) {
        return input;
    }
    // 裸数字别名:"3" -> "loop-3"。
    if (!input.empty() &&
        input.find_first_not_of("0123456789") == std::string::npos) {
        const std::string candidate = "loop-" + input;
        if (impl_->tasks.count(candidate) > 0) {
            return candidate;
        }
    }
    return input;  // 原样(调用方报 not_found)
}

std::vector<LoopSchedulerEvent> LoopScheduler::TakeEvents() {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    std::vector<LoopSchedulerEvent> out = std::move(impl_->pending);
    impl_->pending.clear();
    return out;
}

// ---------------------------------------------------------------------------
// 恢复(第 3 期喂):按事件行重建
// ---------------------------------------------------------------------------

std::optional<replay::Envelope> LoopScheduler::ParseLoopLedgerLine(const std::string& line) {
    // 顶层粗筛省 JSON 解析(与收编前的手抄路同款快路径);真验在解析后。
    if (line.find("\"loop_task_v1\"") == std::string::npos &&
        line.find("\"loop_tick_v1\"") == std::string::npos) {
        return std::nullopt;
    }
    nlohmann::json j;
    try {
        j = nlohmann::json::parse(line);
    } catch (...) {
        return std::nullopt;  // 坏行跳过,不废整场
    }
    if (!j.is_object()) {
        return std::nullopt;
    }
    const std::string family = j.value("type", std::string());
    if (family != "loop_task_v1" && family != "loop_tick_v1") {
        return std::nullopt;
    }
    replay::Envelope envelope;
    envelope.family = family;
    envelope.event = j.value("event", std::string());
    envelope.timestamp_ms = j.value("timestamp_ms", static_cast<std::int64_t>(0));
    // 域字段进 payload 原样过境:task_id/tick_id 是 loop 的主体 id,域载荷
    // 挂 "payload" 键下(与落盘行同名,信封侧不自造第二套键名)。
    nlohmann::json payload = nlohmann::json::object();
    payload["task_id"] = j.value("task_id", std::string());
    payload["tick_id"] = j.value("tick_id", std::string());
    payload["payload"] = j.value("payload", nlohmann::json::object());
    envelope.payload = std::move(payload);
    return envelope;
}

std::optional<LoopSchedulerEvent> LoopScheduler::EventFromEnvelope(const replay::Envelope& envelope) {
    const auto str_field = [](const nlohmann::json& payload, const char* key) {
        const auto it = payload.find(key);
        return it != payload.end() && it->is_string() ? it->get<std::string>() : std::string();
    };
    if (!envelope.payload.is_object()) {
        return std::nullopt;  // 信封侧自家写的,不是 object 属装配 bug
    }
    LoopSchedulerEvent event;
    event.family = envelope.family;
    event.event = envelope.event;
    event.task_id = str_field(envelope.payload, "task_id");
    event.tick_id = str_field(envelope.payload, "tick_id");
    // 域载荷原样过境,不问形状——与收编前的 j.value("payload", object())
    // 同口径:键在就照搬,不在落空对象。
    if (const auto domain_payload = envelope.payload.find("payload");
        domain_payload != envelope.payload.end()) {
        event.payload = *domain_payload;
    }
    event.timestamp_ms = envelope.timestamp_ms;
    return event;
}

bool LoopScheduler::ReplayEvent(const LoopSchedulerEvent& event) {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (event.family == "loop_task_v1" && event.event == "created") {
        LoopTask task;
        try {
            task = LoopTask::from_json(event.payload);
        } catch (const std::exception&) {
            return false;  // 坏行跳过,不废整场
        }
        if (task.task_id.empty()) {
            task.task_id = event.task_id;  // 行级 task_id 兜底
        }
        if (task.task_id.empty()) {
            return false;
        }
        impl_->next_seq = std::max(impl_->next_seq, task.creation_seq + 1);
        // 发号器抬底(批五):恢复后新 task 号接在存档已发的号之后,
        // 与旧 next_seq 口径一致(creation_seq+1 起发,不重号)。
        impl_->ids.SeedPrefixedId("loop", task.creation_seq);
        TaskEntry entry;
        entry.task = task;
        entry.task.state = LoopTaskState::Active;  // created 那一刻的态
        impl_->tasks[task.task_id] = entry;
        return true;
    }
    // 其余事件:状态推进按转换表;payload 里带 next_due/expires 的更新账。
    auto it = impl_->tasks.find(event.task_id);
    if (it == impl_->tasks.end()) {
        return false;  // 没 task 先行,跳过
    }
    LoopTask& task = it->second.task;
    if (event.payload.contains("next_due_at_ms")) {
        task.next_due_at_ms = event.payload.at("next_due_at_ms").get<WallTimeMs>();
    }
    if (event.payload.contains("expires_at_ms")) {
        task.expires_at_ms = event.payload.at("expires_at_ms").get<WallTimeMs>();
    }
    // 注意:Replay 不再 emit(账已在档里,回放只重建内存)。
    const std::string& ev = event.event;
    if (ev == "paused") {
        task.state = IsLoopTerminal(task.state) ? task.state : LoopTaskState::Paused;
    } else if (ev == "resumed") {
        task.state = task.state == LoopTaskState::Paused ? LoopTaskState::Active : task.state;
    } else if (ev == "stopped" || ev == "cancelled") {
        task.state = LoopTaskState::Cancelled;
    } else if (ev == "completed") {
        task.state = LoopTaskState::Completed;
    } else if (ev == "expired") {
        task.state = LoopTaskState::Expired;
    } else if (ev == "broken") {
        task.state = LoopTaskState::Broken;
    } else if (ev == "tick_due" || ev == "due") {
        task.state = LoopTaskState::Due;
        if (event.payload.contains("tick")) {
            task.tick_seq = std::max< std::uint64_t>(
                task.tick_seq, event.payload.at("tick").get<std::uint64_t>());
        }
    } else if (ev == "tick_started" || ev == "started") {
        task.state = LoopTaskState::Running;
        task.active_turn_id = event.tick_id;
        ++task.run_count;
        it->second.current_tick = LoopTick{};
        it->second.current_tick->task_id = task.task_id;
        it->second.current_tick->tick_id = event.tick_id;
        it->second.current_tick->outcome = LoopTickOutcome::Pending;
    } else if (ev == "tick_finished" || ev == "finished") {
        task.state = LoopTaskState::Active;
        task.active_turn_id.reset();
        it->second.current_tick.reset();
        if (event.payload.contains("outcome")) {
            const std::string outcome = event.payload.at("outcome").get<std::string>();
            if (outcome == "provider_error" || outcome == "rate_limited") {
                ++task.consecutive_failures;
            } else {
                task.consecutive_failures = 0;
            }
            if (outcome == "declined") {
                ++task.consecutive_denials;
            } else {
                task.consecutive_denials = 0;
            }
        }
    } else if (ev == "run_requested") {
        // 账面事件,不改态。
    }
    return true;
}

// ---------------------------------------------------------------------------
// timer(只发 wake;不碰 history/ProcessLine/TUI/权限)
// ---------------------------------------------------------------------------

void LoopScheduler::StartTimer() {
    if (impl_->timer_running.exchange(true)) {
        return;
    }
    impl_->stopping = false;
    impl_->timer = std::thread([this]() {
        // 轮询 due:每秒看一眼账(间隔不小于 1m,秒级轮询绰绰有余)。
        // 到点不自己消费——只把 due 标记留在账上,主泵在安全边界取。
        while (!impl_->stopping) {
            std::this_thread::sleep_for(std::chrono::milliseconds(250));
            if (impl_->stopping) {
                break;
            }
            // HasDueWork 只读账,不 mutate;唤醒消费归主泵。
            (void)HasDueWork(impl_->clock->NowWallMs());
        }
    });
}

void LoopScheduler::StopTimer() {
    if (!impl_->timer_running.exchange(false)) {
        return;
    }
    impl_->stopping = true;
    if (impl_->timer.joinable()) {
        impl_->timer.join();
    }
}

bool LoopScheduler::ShouldWakeNow() const {
    return HasDueWork(impl_->clock->NowWallMs());
}

}  // namespace lubancode::runtime::loop
