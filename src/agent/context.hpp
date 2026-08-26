// 历史裁剪:上下文超限时,把中间的老消息整轮扔掉,只留最早一轮(含最早
// 那条 user 消息)和最近几轮完整对话。system 提示词不在 history 里
// (Request::system 单独传),这里不用管。裁剪只影响"发给模型看"的那份
// messages,AgentLoop 自己存的完整历史(history())不受影响。

#pragma once

#include <cstddef>
#include <vector>

#include "api/types.hpp"

namespace lubancode::agent {

// 默认的裁剪阈值(字节账:消息里所有文本/工具入参/工具结果的 UTF-8 字节
// 数之和;对中文按字节算,不是按字数算)。环境变量 LUBANCODE_MAX_CONTEXT
// 可以覆盖这个数。
constexpr std::size_t kDefaultMaxContextChars = 600000;

// 默认保留最近几轮完整对话不裁。
constexpr std::size_t kDefaultKeepRecentTurns = 3;

// ---------------------------------------------------------------------------
// 统一 token 估算口径(全库唯一一把尺)
//
// 旧账有两把尺:context.cpp 的"字节/3"对中文严重低估(一个汉字 3 字节,
// 实际约 1.5~2 token),session_commands.cpp 的"字节/2"对英文高估(4 个
// ASCII 字符约 1 token)。两把尺并存,做分块预算时必然切歪。这里统一成:
//   ASCII 字符:4 个算 1 token;
//   非 ASCII 码点(中日韩等):每字 1.5 token(2 字 3 token)。
// 不引分词依赖,够做预算与触发判定用;真实用量仍以 provider usage 为准。
// ---------------------------------------------------------------------------

// 一段 UTF-8 文本估多少 token(统一口径,见上)。
std::size_t EstimateUtf8Tokens(const std::string& text);

// 一条消息估多少 token(各内容块文本按统一口径累加;图片按 base64 体积
// 粗折,工具入参按 JSON dump 文本算)。
std::size_t EstimateMessageTokens(const api::Message& message);

// 一段历史估多少 token(逐条消息按统一口径累加)。
std::size_t EstimateHistoryTokens(const std::vector<api::Message>& history);

// 粗略估算一段历史的字节账(所有文本/工具入参/工具结果的 UTF-8 字节数之
// 和)。名字里的 Bytes 是明话:这是字节,不是字符数,更不是 token 数。
// 旧名 EstimateChars 的实际行为就是它——名字里叫 chars 算的是字节,这账
// 改准了名字。
std::size_t EstimateHistoryBytes(const std::vector<api::Message>& history);

// 从环境变量 LUBANCODE_MAX_CONTEXT 读裁剪阈值,没设置、或者设置的不是合法
// 正整数,就用默认值 kDefaultMaxContextChars。
std::size_t MaxContextCharsFromEnv();

// 硬裁剪报告:TrimHistory 这一次有没有丢东西、丢了多少。上层(UI)拿到
// 报告须向用户明说发生了有损裁剪——静默降级会让用户以为语义压缩已成功,
// 模型其实已经看不到那段原文了。
struct TrimReport {
    bool trimmed_turns = false;        // 丢了中间整轮(换成了占位说明)
    std::size_t dropped_messages = 0;  // 丢掉的消息条数(不含占位合并的那条)
    bool truncated_results = false;    // 有超大工具结果被截尾
};

// 裁剪历史:
//   - EstimateHistoryBytes(history) 不超过 max_chars 时,原样返回,一条不动。
//   - 超过时,把历史按"轮"切开(一轮 = 一条真正的用户输入消息,加上后面
//     跟着的所有 assistant/tool_result 消息,直到下一条用户输入消息之前),
//     保留最早一轮(含最早那条 user 消息)和最近 keep_recent_turns 轮,
//     中间的轮次整轮扔掉,换成一条 "[早前对话已裁剪]" 的占位 user 消息。
//   - 因为永远按"轮"的边界切,一轮内部的 tool_use 和它对应的 tool_result
//     永远同进同退,不会出现只有 tool_use 没有 tool_result(或反过来)的
//     情况——那样喂给 API 会直接报错。
//   - 轮数本来就不够裁(第一轮和最近 N 轮已经覆盖/重叠了全部历史)时,
//     原样返回(超大工具结果的截尾兜底仍会做)。
//   - report 非空时,把本次实际发生的丢失填进去(没丢就保持全默认假)。
std::vector<api::Message> TrimHistory(const std::vector<api::Message>& history,
                                       std::size_t max_chars = kDefaultMaxContextChars,
                                       std::size_t keep_recent_turns = kDefaultKeepRecentTurns,
                                       TrimReport* report = nullptr);

// ---------------------------------------------------------------------------
// mid-turn 上下文安全点(0.27.x 分层压缩第一期;骨架拆解批四从 loop.hpp
// 归位 context——压力是上下文的账)
//
// 自动压缩旧账只看"上一回请求的 usage",且只在下一条外层用户消息发送前
// 触发——工具循环中途回填了大结果后,下一次模型请求可能先撞墙。现在每次
// 模型请求前(工具结果已攒完、请求尚未发出,正是不打断工具的那个缝)
// 都先估一次 projected overflow,快撞窗口就把历史收一收。
// ---------------------------------------------------------------------------

// projected 判定的默认参考线:估占窗口的百分比。80 与 ContextTracker 的
// kAutoCompactThresholdPercent 同档——这是参考线,不是写死的唯一口径。
constexpr int kProjectedOverflowPercent = 80;

// 每次模型请求前的上下文压力通报。phase 区分两种调用:
//   PreRequest    —— 请求拼装前。projected_overflow 为真时,上层可在这个
//                     安全点同步做语义压缩(ReplaceHistory);回调返回后
//                     Run() 用(可能已换短的)history 重新拼请求。
//   AfterHardTrim —— TrimHistory 字符安全网这次真丢了东西(丢轮/截结果)。
//                     纯通报:上层必须向用户显式告警"发生了有损硬裁",
//                     不许静默降级;此时再压缩也救不回这一次的请求。
struct ContextPressure {
    enum class Phase { PreRequest, AfterHardTrim };
    Phase phase = Phase::PreRequest;
    bool projected_overflow = false;   // 预计(含输出预留)放不下
    std::size_t projected_tokens = 0;  // 估算的下一请求 prompt + 输出预留
    std::size_t window_tokens = 0;     // 有效窗口;0 = 未知
    bool hard_trimmed_turns = false;   // 丢了中间整轮
    std::size_t hard_dropped_messages = 0;
    bool hard_truncated_results = false;  // 截了超大工具结果
};

// 跨会话传话(0.25.x)的来信注入规则(纯函数,单测钉):把一封来信按
// user/assistant 交替的协议安全注进 history——
//   - history 末条是 user(比如刚攒完的 tool_result 消息):把来信的文本块
//     追加到那条消息的末尾(保持 user/assistant 交替,三种 wire 都安全);
//   - 否则(末条是 assistant 等罕见边界):新起一条 user 消息。
// 来信的"来历"由调用方在文本里带清来源标识(不装成用户手敲),这里只管
// 结构;来信绝不会被当成确认、权限或命令——这条路由里根本没有那些口子。
void InjectIncomingMessage(std::vector<api::Message>& history, api::Message incoming);

}  // namespace lubancode::agent
