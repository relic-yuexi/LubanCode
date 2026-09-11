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
#include <map>
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
#include "runtime/goal_service.hpp"  // GoalService(轨迹 v3 §4.67 G1)
#include "runtime/session_work_scheduler.hpp"  // GoalWorkSource/FairnessCounter
#include "tools/goal_checkpoint_tool.hpp"

namespace lubancode::cli {
struct Theme;
}
namespace lubancode::runtime {
class ToolTraceHub;
class TrajectorySessionLedger;
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
        std::string evaluation_provider;  // 评估端点身份(§4.67 G2 进账:
        std::string evaluation_wire;      // assistant 必带 provider/wire/model)
        // 晚绑定槽(控制器在装配尾填):
        std::function<lubancode::tools::AgentTool*()> agent_tool;       // 命令材料
        std::function<lubancode::runtime::loop::LoopScheduler*()> loop_scheduler;
        // 开一枚 goal 执行轮(text + 失败出参;单飞,主线程调)。
        std::function<void(const std::string&, bool*)> start_turn;
        // 最近一轮收口后的 turnId(§4.67 G2 验收 parentTurnId 回指工作轮;
        // 可空 = 拿不到,评估账如实落 null,不伪造)。
        std::function<std::string()> last_turn_id;
        // 渲染事件出口(问题 3 第 2 条):is_error 定色,text 是纯文案
        // ——怎么画由装配层(interactive_session_assembly 填)决定。
        std::function<void(bool is_error, const std::string& text)> notify;
        // 会话轨迹账(轨迹 v3 §4.67 G1):v3_main_writer()/session_dir()/
        // 来源链从这取。可空 = v2 场,goal 走 v1 coordinator 旧路。
        lubancode::runtime::TrajectorySessionLedger* trajectory = nullptr;
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
    // v3 路线(轨迹 v3 §4.67 G1):沿 resume 来源链投影 goal head →
    // AdoptFromProjection 接管(单写者);缺口如实报不猜,不接管。
    // pendingIntent 未认领 → 按 workItemId 补泵候选(重复 resume 只补同一
    // 项);他 run 已认领 → 报"恢复核验待 G2",不重放。v2 场保持 P0-6 的
    // 幂等空位(旧存档 goal 事件账回放路已删)。
    void RestoreFromArchive();
    // /clear 换账善后(G1):v3 goal 的内存接管态清空——clear 开的新场不
    // 沿链带旧 goal(§4.67.2 clear 才撤 goal;v1 coordinator 照旧)。
    void HandleSessionCleared();
    // 后台子代理回流喂 goal 的证据/usage 账(没有活跃 goal 零影响)。
    void NoteSubagentCompletion();

    // ---- 查询口(控制器/状态栏用) ----
    lubancode::runtime::goal::GoalCoordinator* coordinator();  // ensure 前空
    // v3 goal 服务(§4.67 G1):v3 卷 + Ensure 后非空;v2 场空(命令照旧走
    // coordinator)。
    lubancode::runtime::goal::GoalService* goal_service();
    // v3 goal 的单一读面(命令 status 与恢复共这一口);v2 场给 nullopt。
    std::optional<lubancode::runtime::goal::GoalLineageProjection> ProjectGoalForCommands();
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
    // v3 服务接线(G1):建/换绑(writer 指针变了 = clear/resume 换场)。
    // 只在有 v3 主写者的会话安家;顺带做一次接管(幂等)。
    void BindGoalService();
    // v3 泵路(G1/G2):认领 → 开轮 → synthetic turn → 收口(采证/内部
    // 请求验收/判词采用与续排,逻辑在 runtime 的
    // CloseGoalIterationWithEvaluation)。true = 这一拍 v3 吃了(v1 路跳过);
    // false = v3 没活。
    bool PumpV3Continuation(std::int64_t now_ms);
    // 当前写者 epoch(认领/沿用判据):v3 主写者的 run id。
    std::string V3WriterEpoch() const;

    Host host_;
    std::optional<lubancode::runtime::goal::GoalCoordinator> coordinator_;
    std::optional<lubancode::runtime::goal::GoalService> goal_service_;
    std::string goal_service_bound_session_;  // 服务绑定的卷(换场重绑判据)
    bool goal_v3_restored_ = false;  // RestoreFromArchive 的 v3 接管只做一次
    std::shared_ptr<lubancode::tools::GoalCheckpointState> checkpoint_state_;
    lubancode::runtime::GoalWorkSource work_source_;
    lubancode::runtime::FairnessCounter fairness_;
    std::string active_iteration_;  // 当前在跑的 goal iteration(收口时清)
    // v3 证据内存(§4.67 G2):id -> 判材料全量(facts 在这,快照只存引用)。
    // 会话级账——resume 后旧证据材料从账投影补齐归 G3;期间旧证据缺材料
    // 只会让验收更保守(不 achieved),不会放过缺口。
    std::map<std::string, lubancode::runtime::goal::GoalEvidence> v3_evidence_memory_;
};

}  // namespace lubancode::app
