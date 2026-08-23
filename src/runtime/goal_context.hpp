// GoalContext(持久目标单第 2 期):每轮喂给执行模型的动态目标上下文。
//
// 单子的规矩:
//   - 放在普通 system prompt 之后、当前 user message 之前;由宿主拼,
//     走 AgentLoop 的 SetTurnContext(请求级,不进永久 history)。
//   - objective 与 contract 每轮原样从 GoalTask 账注入,不靠 history 回忆。
//   - 只带最近 checkpoint 与仍有效 evidence 摘要,不带八轮旧正文。
//   - system prompt 明说执行模型无权切 goal state。
//   - 本轮将到 step 上限时,要求先写 checkpoint。
//
// 纯函数,零 IO。

#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "runtime/goal_types.hpp"

namespace lubancode::runtime::goal {

struct GoalContextOptions {
    // 下轮 iteration 的建议 next_action 出不出(单子:next_action 是建议,
    // 不准盖过用户刚下的新指令——装配层在用户 turn 里可以不带)。
    bool include_next_action = true;
    // evidence 摘要最多带几枚(旧的不带,防上下文膨胀)。
    std::size_t max_evidence_items = 8;
};

// 拼一轮的 GoalContext 文本。task 与 evidence 由装配层给(evidence 只传
// 仍有效的;stale 的标记带)。
std::string BuildGoalContext(const GoalTask& task, const std::vector<GoalEvidence>& evidence,
                             const GoalContextOptions& options = {});

// synthetic continuation 消息的正文(宿主文字;metadata 由装配层另塞)。
// 与 GoalCoordinator::TakeReadyIteration 的 synthetic_text 分工:那边是
// coordinator 自己的短版;这里带完整 GoalContext 的版本由装配层在发轮时
// 组(SetTurnContext 吃 GoalContext,RunTurn 的 user_input 吃短版文字)。
std::string BuildGoalContinuationMessage(const GoalTask& task, int iteration_index);

}  // namespace lubancode::runtime::goal
