// /loop 子系统接线器的实现(会话终章):EnsureLoopScheduler/NoteLoop
// PermissionWait/PumpLoopTicks 的装配与转发,材料换经 Host 递入,行为一字
// 未改——注释一并随行。泵的公平仲裁(PumpNextWork)留控制器。
//
// 骨架拆解反弹·问题 3:PumpDueTick/FinishTick 的调度状态机下沉
// runtime::loop::LoopTickDriver(函数体在那边,原文随行),这里只剩拼
// Host、接依赖、转发;本文件不再有 TermOut——状态机的通知经 Host.notify
// 递给装配层(interactive_session_assembly)画。
#include "app/wirings/loop_session_wiring.hpp"

#include <chrono>
#include <utility>

#include "tools/registry.hpp"

namespace lubancode::app {

LoopSessionWiring::LoopSessionWiring(Host host) : host_(std::move(host)) {}

lubancode::runtime::loop::LoopScheduler* LoopSessionWiring::scheduler() {
    return scheduler_.has_value() ? &*scheduler_ : nullptr;
}

bool LoopSessionWiring::ToolExposed() const {
    // 会话级条件(features.loop 正门 + env 总闸),与 Ensure 里给 scheduler
    // 的 enabled 同一条判式(动态工具 P2 的 ToolExposurePolicy):暴露位
    // 与调度器同源,会话内恒定,tools hash 不随 tick 进出抖。
    return host_.config != nullptr && host_.config->features_loop && !lubancode::app::LoopDisabledByEnv();
}

std::string LoopSessionWiring::ActiveLoopTaskId() const {
    // loop_control 工具账里的 task_id 就是"当前这一拍属于谁"的真值:驱动器
    // 开轮前灌、FinishTick 后清,与 TickActive 的窗口一致。
    return control_state_ != nullptr ? control_state_->task_id : std::string();
}

void LoopSessionWiring::RegisterTools(lubancode::tools::ToolRegistry& registry) {
    if (!control_state_) {
        control_state_ = std::make_shared<lubancode::tools::LoopControlState>();
        // 驱动器已先起(Ensure 早于 RegisterTools 的装配次序)时补喂这份
        // 工具账;常规次序(注册在前)Ensure 时自然带进去。
        if (driver_.has_value()) {
            driver_->AttachHost(BuildDriverHost());
        }
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
    // 单拍驱动器(问题 3):状态机在 runtime,材料全借本接线器。
    driver_.emplace(BuildDriverHost());
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

lubancode::runtime::loop::LoopTickDriver::Host LoopSessionWiring::BuildDriverHost() {
    lubancode::runtime::loop::LoopTickDriver::Host driver_host;
    driver_host.scheduler = &*scheduler_;
    // prompt 源每拍现读:复用命令层的解析口(用户改文件,下一拍生效)。
    driver_host.read_prompt = [this]() {
        const auto resolved = lubancode::app::ResolveLoopPrompt(BuildWiring(), std::string());
        lubancode::runtime::loop::LoopTickDriver::PromptSourceRead read;
        read.text = resolved.text;
        read.source = resolved.source;
        read.error = resolved.error;
        return read;
    };
    // 事件落盘先于 synthetic message(写盘栅栏 2)。
    driver_host.flush_events = [this]() { lubancode::app::FlushLoopEvents(BuildWiring()); };
    driver_host.start_turn = host_.start_turn;
    driver_host.notify = host_.notify;
    driver_host.control_state = control_state_;
    return driver_host;
}

void LoopSessionWiring::NotePermissionWait(bool asked, bool allowed) {
    if (!driver_.has_value()) {
        return;
    }
    driver_->NotePermissionWait(asked, allowed);
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
    if (!driver_.has_value()) {
        return false;
    }
    return driver_->PumpDueTick(now_ms);
}

void LoopSessionWiring::FinishTick(const std::string& tick_id, bool turn_failed, bool cancelled) {
    if (!driver_.has_value()) {
        return;
    }
    driver_->FinishTick(tick_id, turn_failed, cancelled);
}

}  // namespace lubancode::app
