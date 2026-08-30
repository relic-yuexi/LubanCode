// LoopTickDriver 的实现(骨架拆解反弹·问题 3):函数体自
// app/wirings/loop_session_wiring.cpp 的 PumpDueTick/FinishTick 原样搬来
//(那边又是自 interactive_session 大类搬来的),行为一字未改——注释一并
// 随行。改动只有两处形状:TermOut 打印换成 LoopTickNotice 回调;墙钟走
// Host.now 注入口(缺省真 system_clock,与搬家前同口径)。
#include "runtime/loop_tick_driver.hpp"

#include <chrono>
#include <utility>

namespace lubancode::runtime::loop {

namespace {

// 墙钟:Host.now 注入了用注入的(单测喂 fake),没注入走真 system_clock
// ——与搬家前(interactive_session/wiring 里现取)同一口径。
WallTimeMs DriverNow(const std::function<WallTimeMs()>& injected) {
    if (injected) {
        return injected();
    }
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

}  // namespace

bool LoopTickDriver::PumpDueTick(WallTimeMs now_ms) {
    const auto tick = host_.scheduler->PumpDueTick(now_ms, "loop-turn");
    if (!tick.has_value()) {
        host_.flush_events();
        return false;
    }
    // 事件落盘先于 synthetic message(due/started 必须在 turn 前落,写盘
    // 栅栏 2)。
    host_.flush_events();
    // prompt 源每拍现读(用户改文件,下一拍生效;读失败本拍 Broken,不拿
    // 上一版暗跑)。文件源才重读;inline/builtin 直接用建任务的正文。
    std::string prompt_text = tick->text;
    if (tick->task.prompt_source == LoopPromptSource::ProjectFile ||
        tick->task.prompt_source == LoopPromptSource::UserFile) {
        const PromptSourceRead resolved = host_.read_prompt ? host_.read_prompt() : PromptSourceRead{};
        if (!resolved.error.empty() ||
            (tick->task.prompt_source == LoopPromptSource::ProjectFile &&
             resolved.source != LoopPromptSource::ProjectFile)) {
            // 源没了:本拍 prompt_source_missing,任务停终态(不每十分钟
            // 刷同一错)。落账序:FinishTick 记 outcome、Stop 收终态
            //(Cancelled)——旧注释说"落 Broken"与实收状态不符,搬来时
            // 按实改正。
            host_.scheduler->FinishTick(tick->tick.tick_id, LoopTickOutcome::PromptSourceMissing, now_ms,
                                        resolved.error.empty() ? "loop.md 没了" : resolved.error);
            host_.scheduler->Stop(tick->task.task_id, now_ms, "prompt_source_missing");
            host_.flush_events();
            LoopTickNotice notice;
            notice.kind = LoopTickNotice::Kind::Error;
            notice.text = "loop " + tick->task.task_id + " 的 prompt 源读失败,任务已停: " +
                          (resolved.error.empty() ? std::string("loop.md 没了") : resolved.error);
            if (host_.notify) {
                host_.notify(notice);
            }
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
    if (host_.control_state != nullptr) {
        host_.control_state->task_id = tick->task.task_id;
        host_.control_state->complete_requested = false;
        host_.control_state->pause_requested = false;
    }
    if (host_.notify) {
        LoopTickNotice notice;
        notice.kind = LoopTickNotice::Kind::Info;
        notice.text =
            "[loop " + tick->task.task_id + " 第 " + std::to_string(tick->tick.tick_no) + " 拍]";
        host_.notify(notice);
    }
    bool turn_failed = false;
    host_.start_turn(message, &turn_failed);
    FinishTick(tick->tick.tick_id, turn_failed, /*cancelled=*/false);
    return true;
}

void LoopTickDriver::FinishTick(const std::string& tick_id, bool turn_failed, bool cancelled) {
    if (active_tick_id_ != tick_id) {
        return;  // 迟到收口,留账不动(scheduler 自己会拒)
    }
    active_tick_id_.clear();
    const WallTimeMs now_ms = DriverNow(host_.now);
    // loop_control 窄工具的声明消费(单子第 4 期:complete 是正常终态,
    // pause 用于需要用户处理的情况;两者都在拍收口时落到 scheduler 账,
    // 不在工具 execute 里直改——工具只立旗)。scope 清零先做:迟到的工具
    // 调用立即明拒。
    const std::string control_task_id =
        host_.control_state != nullptr ? host_.control_state->task_id : std::string();
    const bool control_complete =
        host_.control_state != nullptr && host_.control_state->complete_requested;
    const bool control_pause =
        host_.control_state != nullptr && host_.control_state->pause_requested;
    if (host_.control_state != nullptr) {
        host_.control_state->task_id.clear();
        host_.control_state->complete_requested = false;
        host_.control_state->pause_requested = false;
    }
    if (control_complete && !control_task_id.empty()) {
        // complete 先落(Running -> Completed 的合法边),tick 收口在后
        //(terminal 同态回写补账,转换表明收)。
        host_.scheduler->Complete(control_task_id, now_ms, "model_declared_complete");
        host_.scheduler->FinishTick(tick_id, LoopTickOutcome::Succeeded, now_ms, "loop_control_complete");
        host_.flush_events();
        if (host_.notify) {
            LoopTickNotice notice;
            notice.kind = LoopTickNotice::Kind::Info;
            notice.text = "loop " + control_task_id + ":模型声明完成,任务落终态(下一拍不再排)。";
            host_.notify(notice);
        }
        return;
    }
    if (control_pause && !control_task_id.empty()) {
        host_.scheduler->Pause(control_task_id, now_ms, "model_requested_pause");
        host_.scheduler->FinishTick(tick_id, LoopTickOutcome::Succeeded, now_ms, "loop_control_pause");
        host_.flush_events();
        if (host_.notify) {
            LoopTickNotice notice;
            notice.kind = LoopTickNotice::Kind::Info;
            notice.text = "loop " + control_task_id + ":模型请求暂停(需要用户处理);续跑 /loop resume。";
            host_.notify(notice);
        }
        return;
    }
    if (cancelled) {
        host_.scheduler->FinishTick(tick_id, LoopTickOutcome::Cancelled, now_ms, "user_stop");
    } else if (turn_failed) {
        host_.scheduler->FinishTick(tick_id, LoopTickOutcome::ProviderError, now_ms, "provider_error");
    } else {
        host_.scheduler->FinishTick(tick_id, LoopTickOutcome::Succeeded, now_ms);
    }
    host_.flush_events();
}

void LoopTickDriver::NotePermissionWait(bool asked, bool allowed) {
    // WaitingPermission 真接线:只在 loop 拍的 turn 里记账(普通轮的审批
    // 与 scheduler 无关)。审批旁听口从回合进来,这里拿 active_tick_id_
    // 认"这一轮是不是 loop 的轮"。
    if (active_tick_id_.empty()) {
        return;
    }
    const WallTimeMs now_ms = DriverNow(host_.now);
    if (asked) {
        host_.scheduler->NotePermissionWait(active_tick_id_, now_ms);
        host_.flush_events();
        return;
    }
    // 答完:allowed 走 resolved(本拍继续),拒走 declined(连三拍自动
    // Pause 的账在 FinishTick 的 Declined 分支;这里只记事件)。
    if (allowed) {
        host_.scheduler->NotePermissionResolved(active_tick_id_, now_ms);
    } else {
        host_.scheduler->NotePermissionDeclined(active_tick_id_, now_ms);
    }
    host_.flush_events();
}

}  // namespace lubancode::runtime::loop
