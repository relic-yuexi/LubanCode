// 历史裁剪:上下文超限时,把中间的老消息整轮扔掉,只留最早一轮(含最早
// 那条 user 消息)和最近几轮完整对话。system 提示词不在 history 里
// (Request::system 单独传),这里不用管。裁剪只影响"发给模型看"的那份
// messages,AgentLoop 自己存的完整历史(history())不受影响。

#pragma once

#include <cstddef>
#include <vector>

#include "api/types.hpp"

namespace lubancode::agent {

// 默认的裁剪阈值(字符数,即消息里所有文本/工具入参/工具结果的字节数之和)。
// 环境变量 LUBANCODE_MAX_CONTEXT 可以覆盖这个数。
constexpr std::size_t kDefaultMaxContextChars = 600000;

// 默认保留最近几轮完整对话不裁。
constexpr std::size_t kDefaultKeepRecentTurns = 3;

// 粗略估算一段历史的字符数(所有文本/工具入参/工具结果的字节数之和)。
std::size_t EstimateChars(const std::vector<api::Message>& history);

// 粗略估算 token 数:字节数 / 3,够用就行,不引分词依赖。
std::size_t EstimateTokens(const std::vector<api::Message>& history);

// 从环境变量 LUBANCODE_MAX_CONTEXT 读裁剪阈值,没设置、或者设置的不是合法
// 正整数,就用默认值 kDefaultMaxContextChars。
std::size_t MaxContextCharsFromEnv();

// 裁剪历史:
//   - EstimateChars(history) 不超过 max_chars 时,原样返回,一条不动。
//   - 超过时,把历史按"轮"切开(一轮 = 一条真正的用户输入消息,加上后面
//     跟着的所有 assistant/tool_result 消息,直到下一条用户输入消息之前),
//     保留最早一轮(含最早那条 user 消息)和最近 keep_recent_turns 轮,
//     中间的轮次整轮扔掉,换成一条 "[早前对话已裁剪]" 的占位 user 消息。
//   - 因为永远按"轮"的边界切,一轮内部的 tool_use 和它对应的 tool_result
//     永远同进同退,不会出现只有 tool_use 没有 tool_result(或反过来)的
//     情况——那样喂给 API 会直接报错。
//   - 轮数本来就不够裁(第一轮和最近 N 轮已经覆盖/重叠了全部历史)时,
//     原样返回。
std::vector<api::Message> TrimHistory(const std::vector<api::Message>& history,
                                       std::size_t max_chars = kDefaultMaxContextChars,
                                       std::size_t keep_recent_turns = kDefaultKeepRecentTurns);

}  // namespace lubancode::agent
