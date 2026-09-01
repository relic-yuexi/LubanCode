// /loop 子系统接线器(会话终章):loop 的"状态+装配+存档恢复"自
// TerminalSessionController 大类外迁,归这一只。控制器持句柄调;泵的公平
// 仲裁(session_work_scheduler)不动,留控制器——这里出 due 候选与单拍
// 执行。
//
// 骨架拆解反弹·问题 3:单拍执行的调度状态机(PumpDueTick/FinishTick 的
// 函数体)下沉 runtime::loop::LoopTickDriver(见 runtime/loop_tick_driver.
// hpp),这里只留"构造+注入":拼 Host、接依赖、转发调用。渲染不在这层
// ——状态机产 LoopTickNotice,经 Host.notify 递给装配层
// (interactive_session_assembly)画,wirings 文件里没有直接终端 IO。
//
// 状态归属:
//   - scheduler(内存真值)/due 唤醒 token/单拍驱动器/loop_control 工具
//     账——跟接线器走;
//   - 空闲唤醒总口(IdleWakeCoordinator)是会话级的(子代理与 loop 两路
//     并存),借来挂源,token 归接线器;
//   - 存档/ServerEvent 投影材料经 LoopWiring(命令材料包)递给
//     commands/loop_commands 的事件账路。
#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>

#include "app/commands/loop_commands.hpp"  // LoopWiring(命令材料包)
#include "cli/theme.hpp"
#include "config/config.hpp"
#include "runtime/idle_wake.hpp"
#include "runtime/loop_scheduler.hpp"
#include "runtime/loop_tick_driver.hpp"  // 单拍执行的调度状态机(问题 3 下沉)
#include "runtime/session_runtime.hpp"
#include "tools/loop_control_tool.hpp"

namespace lubancode::cli {
struct Theme;
}
namespace lubancode::tools {
class ToolRegistry;
}

namespace lubancode::app {

class LoopSessionWiring {
public:
    // 会话借给接线器的材料(全借用,接线器不拥有)。
    struct Host {
        const lubancode::cli::Theme* theme = nullptr;
        bool interactive = false;  // 真控制台(pipe/one-shot 的 create 明拒在命令层)
        lubancode::config::Config* config = nullptr;  // features_loop 等装配档
        lubancode::runtime::SessionRuntime* session_runtime = nullptr;  // ServerEvent 投影
        const std::optional<std::string>* home_lubancode = nullptr;     // 用户级 loop.md
        lubancode::runtime::IdleWakeCoordinator* idle_wakes = nullptr;  // due 唤醒多路总口
        // 开一枚 loop tick 轮(scheduled message + 失败出参;单飞,主线程调)。
        std::function<void(const std::string&, bool*)> start_turn;
        // 渲染事件出口(问题 3 第 2 条):状态机的 LoopTickNotice 从这递
        // 出,装配层决定怎么画——接线器自己不打终端。
        std::function<void(const lubancode::runtime::loop::LoopTickNotice&)> notify;
    };

    LoopSessionWiring() = default;
    explicit LoopSessionWiring(Host host);
    void AttachHost(Host host) { host_ = std::move(host); }

    // loop_control 窄工具的注册(装配期一次;tick turn 才放行)。
    void RegisterTools(lubancode::tools::ToolRegistry& registry);

    // 装配:scheduler 安家(features.loop + env 总闸)+ due 源挂进空闲唤醒
    // 多路总口 + timer 起动 + 单拍驱动器接线。幂等。
    void Ensure();

    // ---- 泵(主线程安全边界;状态机在 runtime::loop::LoopTickDriver) ----
    // 单拍执行:取一枚 due tick(先落事件再开 turn)、prompt 源每拍现读、
    // scheduled message 开轮、拍收口回写。返回 true = 消费了一拍。
    bool PumpDueTick(std::int64_t now_ms);
    // 拍收口(取消路:ESC 停全部/用户停时由外头调,正常路在 PumpDueTick
    // 尾上自调)。outcome 按 loop_control 声明/turn 失败/取消折算。
    void FinishTick(const std::string& tick_id, bool turn_failed, bool cancelled);
    // 审批旁听:WaitingPermission 的真接线(只在 loop 拍的 turn 里记账)。
    void NotePermissionWait(bool asked, bool allowed);
    // ESC 急停:活任务全停(空闲态 composer 的 stop_active_loops 钩)。
    int StopAllForEsc();

    // ---- 会话收尾 ----
    // 摘 due 唤醒源、停 timer、join 线程(shutdown 要 join,不能让 callback
    // 析构后摸 this)。
    void Shutdown();

    // ---- 查询口(控制器/状态栏用) ----
    lubancode::runtime::loop::LoopScheduler* scheduler();  // ensure 前空
    bool TickActive() const { return driver_.has_value() && driver_->TickActive(); }
    bool HasActiveTasks();
    // loop_control 的暴露位(动态工具 P2·§8.2):只认会话级条件
    //(features.loop 开且 env 总闸未关),与"本拍可不可用"(TickActive,
    // 执行门那半边)分家——暴露恒定,tools hash 不随 tick 的进出抖。
    bool ToolExposed() const;
    // 当前拍所属的 loop 任务 id(不在拍上给空串;本轮能力段的注脚用)。
    std::string ActiveLoopTaskId() const;
    // 到点账(泵的候选判定):SweepExpiry + HasDueWork。
    bool SweepAndCheckDue(std::int64_t now_ms);
    // 命令材料包(HandleLoopCommand/事件账落盘/存档恢复共用)。
    lubancode::app::LoopWiring MakeCommandWiring();

private:
    lubancode::app::LoopWiring BuildWiring();
    // 单拍驱动器的材料拼装(scheduler/每拍 prompt 现读/事件 flush/开轮/
    // 通知/工具账,全借本接线器)。
    lubancode::runtime::loop::LoopTickDriver::Host BuildDriverHost();

    Host host_;
    std::optional<lubancode::runtime::loop::LoopScheduler> scheduler_;
    std::optional<lubancode::runtime::loop::LoopTickDriver> driver_;  // 单拍状态机(Ensure 起)
    lubancode::runtime::IdleWakeCoordinator::Subscription wake_token_;
    std::shared_ptr<lubancode::tools::LoopControlState> control_state_;
};

}  // namespace lubancode::app
