// Plan 模式接线器(会话终章):plan 的"状态+装配+泵+存档恢复"自
// TerminalSessionController 大类外迁,归这一只。只读研究硬闸的档位切换、
// 计划采集与审阅、执行交接全在这;控制器持句柄调。
//
// 状态归属:
//   - plan_id 发号器/审阅悬稿/"档位恢复自旧档"旗——跟接线器走;
//   - 协作档位真值(SessionRuntime)与提示段(PromptOptions)是会话级
//     账,接线器借来写(SetCollaborationMode/plan_mode 段),不搬;
//   - 切档后的重建(RebuildLoop)与会话泵走控制器回调。
#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <string>

#include <nlohmann/json.hpp>

#include "app/commands/command_flow.hpp"
#include "agent/artifact_store.hpp"
#include "agent/prompt_assembler.hpp"
#include "cli/slash_commands.hpp"
#include "cli/theme.hpp"
#include "runtime/plan_mode.hpp"
#include "runtime/session_runtime.hpp"
#include "sessions/session_store.hpp"  // ModeEvent/PlanEvent/PlanReviewEvent
#include "tools/registry.hpp"

namespace lubancode::agent {
class Agent;
}
namespace lubancode::tools {
class AgentTool;
}

namespace lubancode::app {

class PlanSessionWiring {
public:
    // 会话借给接线器的材料(全借用,接线器不拥有)。
    struct Host {
        const lubancode::cli::Theme* theme = nullptr;
        lubancode::runtime::SessionRuntime* session_runtime = nullptr;  // 档位真值
        lubancode::agent::PromptOptions* prompt_options = nullptr;      // plan 段
        lubancode::agent::ContextArtifactStore* artifact_store = nullptr;  // 超限落仓
        // 晚绑定槽(控制器在装配尾填):
        std::function<lubancode::agent::Agent*()> main_agent;        // 历史与运行档案
        std::function<lubancode::tools::ToolRegistry*()> registry;   // Plan 闸的注册表元数据
        std::function<lubancode::tools::AgentTool*()> agent_tool;    // busy 判定
        std::function<void()> rebuild_preserving;                    // 切档后保历史重建
        std::function<void(const std::string&, bool*)> start_turn;   // 规划/执行轮
    };

    PlanSessionWiring() = default;
    explicit PlanSessionWiring(Host host);
    void AttachHost(Host host) { host_ = std::move(host); }

    // /plan 命令组:切入/带任务切入/status/off/review。只在空闲 composer
    // 生效;EnterWithTask 切档后把正文当规划请求发一轮。
    lubancode::app::CommandFlow HandleCommand(const std::string& args);

    // ModePolicy 装配:按注册表元数据 + 模式判一枚工具放不放行。返回空串
    // = 放行;"code|reason" = 拒绝。
    std::string EvaluateGate(const std::string& tool_name, const nlohmann::json& input);

    // 切档的正路:落 mode_v1、重拼系统提示(mode 段)、刷状态栏(重建由
    // Host 回调做)。
    void SwitchMode(lubancode::runtime::CollaborationMode mode, const std::string& reason);

    // Plan turn 收口后扫 assistant 正文:<proposed_plan> 完整则记 PlanDocument
    // 并弹审阅框;多份/半截只打一行提示。
    void CollectProposal(std::size_t history_before, const std::string& turn_id);

    // resume 恢复 mode/plan/review 账(老档没 mode 行按 Default;Approved 已
    // 落而 Default 未落时按"已批准待执行"提示,不自动重跑)。
    void RestoreFromArchive(const std::optional<lubancode::sessions::ModeEvent>& mode_event,
                            const std::vector<lubancode::sessions::PlanEvent>& plans,
                            const std::optional<lubancode::sessions::PlanReviewEvent>& review);

    // ---- 查询口 ----
    bool RestoredFromArchive() const { return restored_from_archive_; }
    void ResetReviewPending() { review_pending_.reset(); }
    // 悬稿翻篇的回调口(/clear、/plan off 用)。
    void DiscardReview() { review_pending_.reset(); }

private:
    // 审阅框(四选项)。esc 只关框仍留 Plan;/plan review 重开。
    void RunReviewPrompt();
    // 批准后的执行交接:先落 review、再切 Default、ImplementationBrief
    // 另起 synthetic turn。
    void LaunchApprovedExecution(lubancode::runtime::PlanDocument plan, bool auto_mode);

    Host host_;
    // plan_id 发号("plan-<n>",会话内单调;与 IdAuthority 分开——计划不是
    // 事件条目,不走 item 计数器)。
    std::uint64_t plan_counter_ = 0;
    // 审阅框 ESC 后想重开(/plan review):留最近一份候选。
    std::optional<lubancode::runtime::PlanDocument> review_pending_;
    // resume 恢复出"显式 mode 真值"(档里有 mode 行):起手档不再插手。
    bool restored_from_archive_ = false;
};

}  // namespace lubancode::app
