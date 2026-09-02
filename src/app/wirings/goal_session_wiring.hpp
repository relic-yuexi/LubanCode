// /goal 子系统接线器(会话终章):goal 的"状态+装配+泵+存档恢复"自
// TerminalSessionController 大类外迁,归这一只。控制器持句柄调;会话级
// 状态(theme/config)仍留控制器,两边不互相摸。
//
// 骨架拆解反弹·问题 3:Ensure 里"事件类型分族 + ledger sink 搭建"抽去
// runtime::goal::MakeSessionLedgerSink(纯函数);终端打印改产 notify 回调
// (装配层决定怎么画),本文件没有直接终端 IO。
//
// 状态归属(单子钉的):
//   - coordinator(状态机)/checkpoint 工具账/work source/fairness 账/
//     活跃 iteration 号——全跟接线器走;
//   - 模型路由/评估 backend——会话借来(Host 全借用);
//   - 开 turn 只经 start_turn 回调(单飞铁律:一场会话同时一枚主 turn)。
//
// 泵的公平仲裁(session_work_scheduler 的 PumpNextWork)不动,留控制器;
// 这里只出候选(ProbeWork)与消费(PumpContinuation/CloseIteration)。
#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>

#include <nlohmann/json.hpp>

#include "api/backend.hpp"
#include "app/commands/goal_commands.hpp"     // GoalWiring(命令材料包)
#include "app/model_router.hpp"
#include "cli/theme.hpp"
#include "config/config.hpp"
#include "runtime/goal_coordinator.hpp"
#include "runtime/session_work_scheduler.hpp"  // GoalWorkSource/FairnessCounter
#include "tools/goal_checkpoint_tool.hpp"

namespace lubancode::cli {
struct Theme;
}
namespace lubancode::runtime {
class ToolTraceHub;
}
namespace lubancode::tools {
class AgentTool;
class ToolRegistry;
}
namespace lubancode::runtime::loop {
class LoopScheduler;
}

namespace lubancode::app {

class GoalSessionWiring {
public:
    // 会话借给接线器的材料(全借用,接线器不拥有)。控制器先默认构造
    // 接线器,装配尾经 AttachHost 配齐(晚绑定槽捕获控制器 this)。
    struct Host {
        const lubancode::cli::Theme* theme = nullptr;
        const lubancode::config::Config* config = nullptr;      // Ensure 折 Options
        lubancode::runtime::ToolTraceHub* trace_hub = nullptr;   // 可空(采证)
        lubancode::app::ModelRouterService* model_router = nullptr;  // 可空
        lubancode::api::Backend* evaluation_backend = nullptr;   // 评估轮的独立请求
        std::shared_ptr<std::string> current_model;              // 评估兜底模型
        // 晚绑定槽(控制器在装配尾填):
        std::function<lubancode::tools::AgentTool*()> agent_tool;       // 命令材料
        std::function<lubancode::runtime::loop::LoopScheduler*()> loop_scheduler;
        // 开一枚 goal 执行轮(text + 失败出参;单飞,主线程调)。
        std::function<void(const std::string&, bool*)> start_turn;
        // 渲染事件出口(问题 3 第 2 条):is_error 定色,text 是纯文案
        // ——怎么画由装配层(interactive_session_assembly 填)决定。
        std::function<void(bool is_error, const std::string& text)> notify;
    };

    GoalSessionWiring() = default;
    explicit GoalSessionWiring(Host host);
    void AttachHost(Host host) { host_ = std::move(host); }

    // goal_checkpoint 窄工具的注册(装配期一次;靠 turn 级过滤放行,普通轮
    // 不露面)。状态跟接线器走,工具只持 shared_ptr。
    void RegisterTools(lubancode::tools::ToolRegistry& registry);

    // 装配:coordinator 从 config+env 折 Options 安家,ready continuation
    // 经 GoalWorkSource 出候选。幂等。(P0-6:旧存档的 LedgerSink 已删;
    // goal 事件的持久账接 trajectory 属 goal 单后续波次,如实记缺口。)
    void Ensure(const lubancode::config::Config& config);

    // ---- 泵(主线程安全边界) ----
    // ready continuation 的取件口:TakeReadyIteration 落 started 事件,
    // synthetic text 开 turn,收口走 CloseIteration。
    void PumpContinuation(std::int64_t now_ms);
    // goal 执行轮的收口路:采证(ToolTraceHub -> GoalEvidence)→ checkpoint
    // → 独立 evaluator → ApplyEvaluation → continue 则 ScheduleNextIteration。
    void CloseIteration(const std::string& turn_id, bool turn_failed);

    // ---- 恢复与守恒面 ----
    // (P0-6:RestoreFromArchive——旧存档 goal 事件账回放——已删;goal 的
    // 持久账接 trajectory 属 goal 单后续波次。)保留空位调用兼容。
    void RestoreFromArchive();
    // 后台子代理回流喂 goal 的证据/usage 账(没有活跃 goal 零影响)。
    void NoteSubagentCompletion();

    // ---- 查询口(控制器/状态栏用) ----
    lubancode::runtime::goal::GoalCoordinator* coordinator();  // ensure 前空
    bool HasActiveIteration() const { return !active_iteration_.empty(); }
    // goal_checkpoint 的暴露位(动态工具 P2·§8.2):只认会话级条件
    //(features.goals 开且 env 总闸未关),与"本轮可不可用"
    //(HasActiveIteration,执行门那半边)分家——暴露恒定,tools hash
    // 不随 goal 轮的进出抖。config 是会话启动定死的,此值会话内恒定。
    bool ToolExposed() const;
    // 当前活跃 iteration 所属的 goal id(不在 goal 轮给空串;本轮能力段
    // 的注脚用,不给模型当调用凭据)。
    std::string ActiveGoalId() const;
    // 公平账(泵的仲裁用;GoalWorkSource 的候选也在)。
    lubancode::runtime::GoalWorkSource& work_source() { return work_source_; }
    lubancode::runtime::FairnessCounter& fairness() { return fairness_; }
    // 命令材料包(HandleGoalCommand/EmitGoalHook/回流喂账共用)。
    lubancode::app::GoalWiring MakeCommandWiring(lubancode::tools::AgentTool* agent_tool,
                                                 lubancode::runtime::loop::LoopScheduler* loop_scheduler);

private:
    // 渲染事件出口的转发(notify 缺位时静默)。
    void Notify(bool is_error, const std::string& text);

    Host host_;
    std::optional<lubancode::runtime::goal::GoalCoordinator> coordinator_;
    std::shared_ptr<lubancode::tools::GoalCheckpointState> checkpoint_state_;
    lubancode::runtime::GoalWorkSource work_source_;
    lubancode::runtime::FairnessCounter fairness_;
    std::string active_iteration_;  // 当前在跑的 goal iteration(收口时清)
};

}  // namespace lubancode::app
