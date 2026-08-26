// /loop 子系统接线器的实现(会话终章):函数体原样自 interactive_session
// 大类搬来(EnsureLoopScheduler/NoteLoopPermissionWait/PumpLoopTicks 的单拍
// 执行半/FinishLoopTick/ESC 急停/MakeLoopWiring),材料换经 Host 递入,
// 行为一字未改——注释一并随行。泵的公平仲裁(PumpNextWork)留控制器。
#include "app/wirings/loop_session_wiring.hpp"

#include <chrono>
#include <utility>

#include "cli/terminal_port.hpp"
#include "tools/registry.hpp"

namespace lubancode::app {

using lubancode::cli::TermOut;

LoopSessionWiring::LoopSessionWiring(Host host) : host_(std::move(host)) {}

lubancode::runtime::loop::LoopScheduler* LoopSessionWiring::scheduler() {
    return scheduler_.has_value() ? &*scheduler_ : nullptr;
}

void LoopSessionWiring::RegisterTools(lubancode::tools::ToolRegistry& registry) {
    if (!control_state_) {
        control_state_ = std::make_shared<lubancode::tools::LoopControlState>();
    }
    registry.Register(std::make_unique<lubancode::tools::LoopControlTool>(control_state_));
}

void LoopSessionWiring::Ensure() {
    if (scheduler_.has_value()) {
        return;
    }
    lubancode::runtime::loop::LoopScheduler::Options options;
    options.enabled = host_.config->features_loop && !lubancode::app::LoopDisabledByEnv();
    scheduler_.emplace(options);
    // 空闲唤醒多路化:loop 的 due 源挂进 coordinator,不再覆盖子代理那枚
    // 单钩(SetIdleWakeHook 的总钩由会话装配统一装)。
    if (host_.idle_wakes != nullptr) {
        wake_token_ = host_.idle_wakes->AddSource("loop", [this]() {
            return scheduler_.has_value() && scheduler_->ShouldWakeNow();
        });
    }
    scheduler_->StartTimer();
}

bool LoopSessionWiring::HasActiveTasks() {
    return scheduler_.has_value() && scheduler_->HasActiveTasks();
}

bool LoopSessionWiring::SweepAndCheckDue(std::int64_t now_ms) {
    if (!scheduler_.has_value() || !scheduler_->HasActiveTasks()) {
        return false;
    }
    scheduler_->SweepExpiry(now_ms);
    return scheduler_->HasDueWork(now_ms);
}

lubancode::app::LoopWiring LoopSessionWiring::BuildWiring() {
    lubancode::app::LoopWiring wiring;
    wiring.interactive = host_.interactive;
    wiring.feature_enabled = host_.config->features_loop && !lubancode::app::LoopDisabledByEnv();
    wiring.theme = host_.theme;
    wiring.scheduler = scheduler_.has_value() ? &*scheduler_ : nullptr;
    wiring.session_store = host_.session_store;
    wiring.home_lubancode = host_.home_lubancode;
    wiring.session_runtime = host_.session_runtime;
    wiring.flush_events = [this]() { lubancode::app::FlushLoopEvents(BuildWiring()); };
    return wiring;
}

lubancode::app::LoopWiring LoopSessionWiring::MakeCommandWiring() { return BuildWiring(); }

void LoopSessionWiring::NotePermissionWait(bool asked, bool allowed) {
    // WaitingPermission 真接线:只在 loop 拍的 turn 里记账(普通轮的审批
    // 与 scheduler 无关)。审批旁听口从回合进来,这里拿 active_tick_id_
    // 认"这一轮是不是 loop 的轮"。
    if (active_tick_id_.empty() || !scheduler_.has_value()) {
        return;
    }
    const auto now_ms = [] {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
                   std::chrono::system_clock::now().time_since_epoch())
            .count();
    }();
    if (asked) {
        scheduler_->NotePermissionWait(active_tick_id_, now_ms);
        lubancode::app::FlushLoopEvents(BuildWiring());
        return;
    }
    // 答完:allowed 走 resolved(本拍继续),拒走 declined(连三拍自动
    // Pause 的账在 FinishTick 的 Declined 分支;这里只记事件)。
    if (allowed) {
        scheduler_->NotePermissionResolved(active_tick_id_, now_ms);
    } else {
        scheduler_->NotePermissionDeclined(active_tick_id_, now_ms);
    }
    lubancode::app::FlushLoopEvents(BuildWiring());
}

int LoopSessionWiring::StopAllForEsc() {
    if (!scheduler_.has_value()) {
        return 0;
    }
    const auto now_ms = [] {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
                   std::chrono::system_clock::now().time_since_epoch())
            .count();
    }();
    int stopped = 0;
    for (const auto& view : scheduler_->Snapshot(now_ms)) {
        if (lubancode::runtime::loop::IsLoopTerminal(view.task.state) ||
            view.task.state == lubancode::runtime::loop::LoopTaskState::Paused) {
            continue;
        }
        if (scheduler_->Stop(view.task.task_id, now_ms, "user_esc").ok) {
            ++stopped;
        }
    }
    if (stopped > 0) {
        lubancode::app::FlushLoopEvents(BuildWiring());
    }
    return stopped;
}

void LoopSessionWiring::Shutdown() {
    // due 唤醒源先摘;随后停 timer/join(shutdown 要 join,不能让 callback
    // 析构后摸 this)。
    wake_token_.reset();
    if (scheduler_.has_value()) {
        scheduler_->StopTimer();
    }
}

bool LoopSessionWiring::PumpDueTick(std::int64_t now_ms) {
    const auto tick = scheduler_->PumpDueTick(now_ms, "loop-turn");
    if (!tick.has_value()) {
        lubancode::app::FlushLoopEvents(BuildWiring());
        return false;
    }
    // 事件落盘先于 synthetic message(due/started 必须在 turn 前落,写盘
    // 栅栏 2)。
    lubancode::app::FlushLoopEvents(BuildWiring());
    // prompt 源每拍现读(用户改文件,下一拍生效;读失败本拍 Broken,不拿
    // 上一版暗跑)。文件源才重读;inline/builtin 直接用建任务的正文。
    std::string prompt_text = tick->text;
    if (tick->task.prompt_source == lubancode::runtime::loop::LoopPromptSource::ProjectFile ||
        tick->task.prompt_source == lubancode::runtime::loop::LoopPromptSource::UserFile) {
        const auto resolved = lubancode::app::ResolveLoopPrompt(BuildWiring(), std::string());
        if (!resolved.error.empty() ||
            (tick->task.prompt_source == lubancode::runtime::loop::LoopPromptSource::ProjectFile &&
             resolved.source != lubancode::runtime::loop::LoopPromptSource::ProjectFile)) {
            // 源没了:本拍 prompt_source_missing,task 落 Broken(不每十分钟
            // 刷同一错)。
            scheduler_->FinishTick(tick->tick.tick_id,
                                    lubancode::runtime::loop::LoopTickOutcome::PromptSourceMissing,
                                    now_ms, resolved.error.empty() ? "loop.md 没了" : resolved.error);
            scheduler_->Stop(tick->task.task_id, now_ms, "prompt_source_missing");
            lubancode::app::FlushLoopEvents(BuildWiring());
            TermOut() << host_.theme->error << "loop " << tick->task.task_id
                      << " 的 prompt 源读失败,任务已停: "
                      << (resolved.error.empty() ? std::string("loop.md 没了") : resolved.error)
                      << host_.theme->reset << "\n";
            return true;
        }
        prompt_text = resolved.text;
    }
    // scheduled message:模型须知道来源与时间,不伪装成用户刚敲的正文。
    const std::string message =
        "[定时循环 tick]\ntask_id: " + tick->task.task_id + "\ntick: " +
        std::to_string(tick->tick.tick_no) + "\nscheduled_at_ms: " +
        std::to_string(tick->tick.scheduled_at_ms) + "\nmissed_since_last: " +
        std::to_string(tick->tick.missed_count) +
        "\n\n原始任务:\n" + prompt_text;
    active_tick_id_ = tick->tick.tick_id;
    // loop_control 工具的会话级状态:本拍 scope 灌好(空 task_id = 工具
    // 明拒),上一拍的声明清零(单子:tick turn 前灌 task_id,收口后清)。
    if (control_state_ != nullptr) {
        control_state_->task_id = tick->task.task_id;
        control_state_->complete_requested = false;
        control_state_->pause_requested = false;
    }
    TermOut() << host_.theme->stats << "[loop " << tick->task.task_id << " 第 " << tick->tick.tick_no
              << " 拍]" << host_.theme->reset << "\n";
    bool turn_failed = false;
    host_.start_turn(message, &turn_failed);
    FinishTick(tick->tick.tick_id, turn_failed, /*cancelled=*/false);
    return true;
}

void LoopSessionWiring::FinishTick(const std::string& tick_id, bool turn_failed, bool cancelled) {
    if (active_tick_id_ != tick_id) {
        return;  // 迟到收口,留账不动(scheduler 自己会拒)
    }
    active_tick_id_.clear();
    if (!scheduler_.has_value()) {
        return;
    }
    const auto now_ms = [] {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
                   std::chrono::system_clock::now().time_since_epoch())
            .count();
    }();
    // loop_control 窄工具的声明消费(单子第 4 期:complete 是正常终态,
    // pause 用于需要用户处理的情况;两者都在拍收口时落到 scheduler 账,
    // 不在工具 execute 里直改——工具只立旗)。scope 清零先做:迟到的工具
    // 调用立即明拒。
    const std::string control_task_id =
        control_state_ != nullptr ? control_state_->task_id : std::string();
    const bool control_complete =
        control_state_ != nullptr && control_state_->complete_requested;
    const bool control_pause =
        control_state_ != nullptr && control_state_->pause_requested;
    if (control_state_ != nullptr) {
        control_state_->task_id.clear();
        control_state_->complete_requested = false;
        control_state_->pause_requested = false;
    }
    using lubancode::runtime::loop::LoopTickOutcome;
    if (control_complete && !control_task_id.empty()) {
        // complete 先落(Running -> Completed 的合法边),tick 收口在后
        //(terminal 同态回写补账,转换表明收)。
        scheduler_->Complete(control_task_id, now_ms, "model_declared_complete");
        scheduler_->FinishTick(tick_id, LoopTickOutcome::Succeeded, now_ms, "loop_control_complete");
        lubancode::app::FlushLoopEvents(BuildWiring());
        TermOut() << host_.theme->stats << "loop " << control_task_id
                  << ":模型声明完成,任务落终态(下一拍不再排)。" << host_.theme->reset << "\n";
        return;
    }
    if (control_pause && !control_task_id.empty()) {
        scheduler_->Pause(control_task_id, now_ms, "model_requested_pause");
        scheduler_->FinishTick(tick_id, LoopTickOutcome::Succeeded, now_ms, "loop_control_pause");
        lubancode::app::FlushLoopEvents(BuildWiring());
        TermOut() << host_.theme->stats << "loop " << control_task_id
                  << ":模型请求暂停(需要用户处理);续跑 /loop resume。" << host_.theme->reset << "\n";
        return;
    }
    if (cancelled) {
        scheduler_->FinishTick(tick_id, LoopTickOutcome::Cancelled, now_ms, "user_stop");
    } else if (turn_failed) {
        scheduler_->FinishTick(tick_id, LoopTickOutcome::ProviderError, now_ms, "provider_error");
    } else {
        scheduler_->FinishTick(tick_id, LoopTickOutcome::Succeeded, now_ms);
    }
    lubancode::app::FlushLoopEvents(BuildWiring());
}

}  // namespace lubancode::app
