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
#include <string>

#include <nlohmann/json.hpp>

#include "config/config.hpp"
#include "runtime/goal_coordinator.hpp"
#include "runtime/goal_types.hpp"

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

}  // namespace lubancode::app
