// LoopTickDriver(骨架拆解反弹·问题 3):/loop 单拍执行的调度状态机。
//
// 这段逻辑原先住在 app/wirings/loop_session_wiring.cpp 的 PumpDueTick/
// FinishTick(函数体自 interactive_session 大类搬来后一直没分家)——
// "取 due tick、prompt 源每拍现读与失源停账、scheduled message 拼装、
// loop_control 声明消费、拍收口回写"是调度业务,不是装配,下沉 runtime:
//   - 取件/状态变更全走 LoopScheduler(同目录的内存真值);
//   - 要不要打一句话,产出 LoopTickNotice 交回调,本层零终端;
//   - 开 turn 经 start_turn 回调递出(单飞铁律:一场会话同时一枚主 turn)。
//
// 依赖铁律:只认 loop_types/loop_scheduler/tools 的 loop_control 状态与回
// 调;不 include cli/app/sessions。单测喂真 LoopScheduler + 注入 id 发号
// 即可起,不必拉整个会话(见 tests/unit/runtime/test_loop_tick_driver.cpp)。
#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>

#include "runtime/loop_scheduler.hpp"
#include "tools/loop_control_tool.hpp"  // LoopControlState(loop_control 声明的会话账)

namespace lubancode::runtime::loop {

// 状态机产出的通知(渲染事件,整改单问题 3 第 2 条):kind 定色,text 是
// 纯文案(不带主题色、不带换行)——怎么画由装配层的回调决定,wirings
// 里不再有直接终端 IO。
struct LoopTickNotice {
    enum class Kind { Info, Error };
    Kind kind = Kind::Info;
    std::string text;
};

class LoopTickDriver {
public:
    // prompt 源的每拍现读结果(装配层递:读 loop.md/用户文件)。
    struct PromptSourceRead {
        std::string text;    // 读到的正文
        LoopPromptSource source = LoopPromptSource::Builtin;  // 实际解析到的源
        std::string error;   // 读失败的原因(空 = 没错)
    };

    // 借来的材料与递出的口(全借用,不拥有):
    struct Host {
        LoopScheduler* scheduler = nullptr;
        // 每拍现读 prompt 源:文件源才调(用户改文件,下一拍生效;读失败
        // 本拍 Broken,不拿上一版暗跑)。
        std::function<PromptSourceRead()> read_prompt;
        // 事件落盘(pending 事件交装配层 flush;写盘栅栏:先于 synthetic
        // message)。
        std::function<void()> flush_events;
        // 开一枚 loop tick 轮(scheduled message + 失败出参;单飞,主线程调)。
        std::function<void(const std::string& message, bool* turn_failed)> start_turn;
        // 渲染事件出口(通知怎么画由装配层定)。
        std::function<void(const LoopTickNotice&)> notify;
        // loop_control 窄工具的会话级状态(可空:没注册工具的装配)。
        std::shared_ptr<lubancode::tools::LoopControlState> control_state;
        // 墙钟注入口(收口/审批旁听用):空 = 真 system_clock(生产行为)。
        // 单测喂 fake,与 scheduler 的 LoopClock 同一只钟,免得收口时刻
        // 比建账时刻"晚七天"把任务误判 Expired。
        std::function<WallTimeMs()> now;
    };

    LoopTickDriver() = default;
    explicit LoopTickDriver(Host host) : host_(std::move(host)) {}
    void AttachHost(Host host) { host_ = std::move(host); }

    // 单拍执行:取一枚 due tick(先落事件再开 turn)、prompt 源每拍现读、
    // scheduled message 开轮、拍收口回写。返回 true = 消费了一拍。
    bool PumpDueTick(WallTimeMs now_ms);
    // 拍收口(取消路:ESC 停全部/用户停时由外头调,正常路在 PumpDueTick
    // 尾上自调)。outcome 按 loop_control 声明/turn 失败/取消折算。
    void FinishTick(const std::string& tick_id, bool turn_failed, bool cancelled);
    // 审批旁听:WaitingPermission 的真接线(只在 loop 拍的 turn 里记账)。
    void NotePermissionWait(bool asked, bool allowed);

    bool TickActive() const { return !active_tick_id_.empty(); }

private:
    Host host_;
    std::string active_tick_id_;  // 当前在跑的 tick(收口时清)
};

}  // namespace lubancode::runtime::loop
