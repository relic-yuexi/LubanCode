// /goal 命令处理器(持久目标单第 1/5 期):终端这条 slash 的业务面。
//
// 分层:GoalCoordinator(runtime)是状态机唯一写口;这里只做三件事——
//   1. 把 ParsedGoalCommand 翻成 coordinator 调用;
//   2. 把结构化 Status()/错误码翻成终端能印的几行字(人话由前端拼,
//      稳定码原样带出);
//   3. feature gate(env 总闸 LUBANCODE_DISABLE_GOALS 在这层读,只关
//      功能不改存档)。
//
// 首版落的命令面(单子"首版命令面"):
//   /goal <objective>   创建(Preparing,合同 preflight 由第 0 iteration 拟)
//   /goal | /goal status 查账,不发模型
//   /goal edit <objective>
//   /goal pause | resume | clear
// clear 先问一句(二次确认显示 objective preview/iteration/已耗预算)。

#pragma once

#include <cstdint>
#include <functional>
#include <string>

#include <nlohmann/json.hpp>

#include "app/commands/command_flow.hpp"
#include "cli/slash_commands.hpp"  // ParsedGoalCommand
#include "cli/theme.hpp"
#include "config/config.hpp"
#include "runtime/goal_coordinator.hpp"
#include "runtime/goal_types.hpp"
#include "runtime/loop_scheduler.hpp"

#include "tools/goal_checkpoint_tool.hpp"  // GoalCheckpointState(白名单补账)

namespace lubancode::cli {
struct Theme;
}
namespace lubancode::sessions {
class SessionStore;
}
namespace lubancode::tools {
class AgentTool;
}

namespace lubancode::app {

// env 总闸(只关功能不改存档;装配层也读同一枚)。
bool GoalsDisabledByEnv();

// 从会话配置折 GoalCoordinator::Options(features.goals + goals 段 + env 闸)。
lubancode::runtime::goal::GoalCoordinator::Options GoalOptionsFromConfig(bool features_goals,
                                                                         const lubancode::config::GoalsConfig& goals);

// /goal 的执行结果:ok 由 coordinator 判;lines 是终端直接印的行(已含
// 错误人话);payload 带结构化账(status 的原始 Status() JSON)。
struct GoalCommandOutcome {
    bool ok = false;
    std::string error_code;
    std::vector<std::string> lines;
    nlohmann::json payload = nlohmann::json::object();
};

// 错误码 → 一句人话(稳定码原样带出;排版用,机器判断仍认码)。
std::string DescribeGoalErrorCode(const std::string& code, const std::string& message);

// /goal status|裸敲:全账排版(id/state/revision/iteration/目标/当前/
// 判定/进展/预算/下一步),纯本地输出,不发模型。now_ms 由调用方给。
GoalCommandOutcome FormatGoalStatus(const lubancode::runtime::goal::GoalCoordinator& coordinator,
                                    std::int64_t now_ms);

// clear 的二次确认文案(objective preview + iteration + 已耗预算 + 提醒
// clear 不是 rollback)。
std::vector<std::string> BuildGoalClearConfirmLines(const lubancode::runtime::goal::GoalTask& task);

// ---- /goal 会话接线(终端接线收尾单自大类搬出) ----------------------------
//
// 命令分派、goal 钩子发射、状态栏短段、子代理回流喂账原先住在大类里,搬
// 到这里;材料经 GoalWiring 递入(装配与状态留会话)。
struct GoalWiring {
    const lubancode::cli::Theme* theme = nullptr;
    lubancode::runtime::goal::GoalCoordinator* coordinator = nullptr;  // ensure 后非空
    lubancode::tools::AgentTool* agent_tool = nullptr;      // 子代理台账(可空)
    lubancode::tools::GoalCheckpointState* checkpoint_state = nullptr;  // 可空
    lubancode::runtime::loop::LoopScheduler* loop_scheduler = nullptr;  // 状态栏短段(可空)
};

// /goal 七动作的接线(view/status/create/edit/pause/resume/clear;clear 走
// 二次确认)。coordinator 由调用方先 ensure。
lubancode::app::CommandFlow HandleGoalCommand(const lubancode::cli::ParsedGoalCommand& goal,
                                               const GoalWiring& wiring);

// goal 生命周期进 hook 分发:全部只给审计与 additionalContext,没有
// permission_decision(Hook 不可直接写 Achieved)。
void EmitGoalHook(const GoalWiring& wiring, lubancode::hooks::HookEvent event, nlohmann::json fields,
                  const std::string& match_value);

// 状态栏的 goal/loop 段:"goal <短码>·iter<N> · loop×<N> next <差>"。两样
// 都没有给空串(整段不挂)。goal 从 GoalState 现折,loop 用 scheduler 快照。
std::string BuildGoalLoopStatusSegment(lubancode::runtime::goal::GoalCoordinator* goal,
                                       lubancode::runtime::loop::LoopScheduler* loop);

// 子代理回流进 goal 的账:后台子代理完成时,结果折一枚二级证据(producer
// 标 subagent),usage 折进 goal 的 usage 账。有 goal 在跑才记,没有零影响。
void NoteSubagentCompletionForGoal(const GoalWiring& wiring);

// 命令分派注册制(会话终章):/goal 的分派位(case 体原样搬自大 switch;
// 装配 ensure 与材料包走 SlashDispatchContext 的回调)。
struct SlashDispatchContext;
CommandFlow HandleSlashGoal(SlashDispatchContext& ctx, const lubancode::cli::ParsedSlashCommand& parsed);

}  // namespace lubancode::app
