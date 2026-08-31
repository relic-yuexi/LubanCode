// 条件工具的本轮能力段(动态工具 PromptCache 守恒单 P2·§8.2)。
//
// goal_checkpoint / loop_control 的定义自 P2 起常驻 tools 数组(暴露策略
// 会话内恒定,保 tools hash 与前缀缓存);"这一轮可不可用"两处另有交代:
//   - 给模型看的短状态:本文件拼的 [turn capabilities] 段,随 turn context
//     走(Agent::SetTurnContext)——追加在本轮 user 消息尾部,不进 system,
//     不追改旧前缀;
//   - 真正的硬拦:AgentProfile.tool_turn_gate 在 RunOneTool 调用当口验真实
//     生命周期,拒绝给 turn.tool_not_active 稳定码。
// 段本身明写"以执行门为准"——它不是安全边界(单子 §8.2:模型硬叫了,
// 执行门仍须拒绝)。
//
// 纯函数,零 IO,单测在 tests/unit/app/test_turn_capabilities.cpp。

#pragma once

#include <string>

namespace lubancode::app {

// 一枚条件工具在本轮的状态位。
struct TurnCapabilityLine {
    // 功能没开(features.goals/features.loop 关,或 env 总闸关):定义根本
    // 不进 tools,这行也不出——能力段不给看不见的工具报状态。
    bool shown = false;
    // 本轮可不可用(与执行门同款真值;假 = 等相应轮次)。
    bool available = false;
    // 可用时的一句注(哪只 goal / 哪只 loop 任务);空 = 不带注。
    std::string note;
};

// P2 已知的两枚 turn 级条件工具。往后添第三枚,这里加一行、装配层喂真值。
struct TurnCapabilities {
    TurnCapabilityLine goal_checkpoint;
    TurnCapabilityLine loop_control;
};

// 拼 [turn capabilities] 段(带前导空行)。两行都隐藏时返回空串——干净
// 会话不塞空脚手架(与记忆召回的零注入规矩同一条)。
std::string BuildTurnCapabilitiesSegment(const TurnCapabilities& caps);

}  // namespace lubancode::app
