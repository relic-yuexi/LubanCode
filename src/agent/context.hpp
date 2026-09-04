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

// 校准系数落进整数估算:四舍五入;空文本(0)不凭空造 token,非零估算
// 校准后至少保 1——系数 <1 不把小文本抹成零。calibration == 1.0 原样
// 返回,零开销。
std::size_t ApplyTokenCalibration(std::size_t tokens, double calibration);

// 一段 UTF-8 文本估多少 token(统一口径,见上)。calibration 是会话级
// 校准系数(TokenCalibrator 按 (provider,model) 桶给,真实 usage 反推),
// 缺省 1.0 = 默认尺,行为与从前一字不差。
std::size_t EstimateUtf8Tokens(const std::string& text, double calibration = 1.0);

// 一条消息估多少 token(各内容块文本按统一口径累加;图片按像素折,
// 工具入参按 JSON dump 文本算)。calibration 同上。
std::size_t EstimateMessageTokens(const api::Message& message, double calibration = 1.0);

// 一段历史估多少 token(逐条消息按统一口径累加)。calibration 同上。
std::size_t EstimateHistoryTokens(const std::vector<api::Message>& history, double calibration = 1.0);

// 粗略估算一段历史的字节账(所有文本/工具入参/工具结果的 UTF-8 字节数之
// 和)。名字里的 Bytes 是明话:这是字节,不是字符数,更不是 token 数。
// 旧名 EstimateChars 的实际行为就是它——名字里叫 chars 算的是字节,这账
// 改准了名字。
std::size_t EstimateHistoryBytes(const std::vector<api::Message>& history);

// ---------------------------------------------------------------------------
// 公共 turn 切分(Compact 四分区单阶段 0)
//
// §二《Turn 怎样算》的唯一定义,全库一份:一枚 turn 从真正的外层用户输入
// 开始(user 角色 + 至少一枚 TextBlock 或 ImageBlock),到下一枚外层用户
// 输入之前结束。只带 ToolResultBlock 的 user 消息(工具结果回填)不开新
// turn;assistant text/thinking/tool_use 归当前 turn。原先 compact.cpp、
// context.cpp、context_events.cpp 各揣一份私有拷贝,语义靠注释互相押韵,
// 日后必漂移——现在收拢到这里,磁盘账与内存路共用同一只。
// (api/chat/request.cpp 那份在 api 层,依赖只许单向,留在原地。)
// ---------------------------------------------------------------------------

// 判定一条消息是不是"真正的外层用户输入"(一枚 turn 的开头)。
// user 角色且内容里至少有一枚 TextBlock 或 ImageBlock;空内容不算——
// 没有 text/image 就没有"用户说了话"的证据,不凭空开 turn(空壳 user
// 消息若插在 tool_use 与 tool_result 之间,当成轮头会把工具原子组劈开)。
bool IsUserTurnStart(const api::Message& message);

// 按上式把整份 history 切成连续 turn 区间:turns[i] = [from, to),首条
// 消息下标是 turn 头,到下一枚 turn 头之前收尾,末段到 history.size()。
// 真正用户输入之前若有零散消息(旧档外壳、异常形状),不属于任何 turn,
// 由调用方自行处置;一条用户输入都没有时返回空表。
std::vector<std::pair<std::size_t, std::size_t>> SplitIntoTurns(const std::vector<api::Message>& history);

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

// 真实水位闸(压缩触发失衡单 §二.B):projected 用的是"临出门"的保守
// 托底尺(空白逐词计数),短词密集的工具输出能虚出日常尺的两倍——单看
// 它判溢出,触发线实际落在真实水位的 ~25-27%,用户真机 24% 就被喊溢出、
// 压完"没有冷区榨不出收益"空跑。B 闸要求工作视图按日常尺
// (EstimateHistoryTokens,与 /context 同一把)的真实水位同时过这条线,
// 虚算单独不触发;真实水位真到线上(配合 80% 参考线),该压的仍压。
// 60 的依据:它必须显著低于 kProjectedOverflowPercent 才当得起"第二道
// 保险"(A 的估算偏低时仍能拦住撞墙),又不能低到形同虚设——与
// ShouldAutoCompact 的 80%(turn 间,按真实 usage)之间留出 midturn 提前
// 收口的一档,不抢它的活。
constexpr int kRealOverflowPercent = 60;

// 每次模型请求前的上下文压力通报。phase 区分三种调用:
//   PreRequest    —— 请求拼装前。projected_overflow 为真时,上层可在这个
//                     安全点同步做语义压缩(ReplaceHistory);回调返回后
//                     Run() 用(可能已换短的)history 重新拼请求。
//                     (压缩触发失衡单后是双闸:projected 过参考线之外,
//                     真实水位也须过 kRealOverflowPercent——见下。)
//   AfterHardTrim —— TrimHistory 字符安全网这次真丢了东西(丢轮/截结果)。
//                     纯通报:上层必须向用户显式告警"发生了有损硬裁",
//                     不许静默降级;此时再压缩也救不回这一次的请求。
//   PreflightExceeded —— token 预检的最终闸判定(派工单 §4.4)。三项账
//                     (estimated_input + reserved_output + protocol_margin)
//                     从这里进可观测事件;reserve_clamped = 常规预留装不下、
//                     已按应急小预留收窄放行(本请求 max_tokens 随之改小)。
struct ContextPressure {
    enum class Phase { PreRequest, AfterHardTrim, PreflightExceeded };
    Phase phase = Phase::PreRequest;
    // 双闸同时过线才为真(压缩触发失衡单 §二):projected(保守托底尺 +
    // 输出预留)过 kProjectedOverflowPercent,且 working_view_tokens(日常
    // 尺的真实水位)过 kRealOverflowPercent。虚算单独不触发——单看托底
    // 尺,真实水位四分之一就会被喊溢出。
    bool projected_overflow = false;
    std::size_t projected_tokens = 0;  // 估算的下一请求 prompt + 输出预留(工作视图口径 + 托底尺)
    std::size_t window_tokens = 0;     // 有效窗口;0 = 未知
    // ---- B 闸自账(PreRequest 时填;轨迹/前端对账用)----------------------
    // working_view_tokens:与真请求同一副工作视图按日常尺(EstimateHistory
    // Tokens)估的 token,不含 system/工具表/输出预留——与 /context 显示同
    // 一把尺。working_view_overflow:它过没过 kRealOverflowPercent 线。
    std::size_t working_view_tokens = 0;
    bool working_view_overflow = false;
    bool hard_trimmed_turns = false;   // 丢了中间整轮
    std::size_t hard_dropped_messages = 0;
    bool hard_truncated_results = false;  // 截了超大工具结果
    // ---- 预检三项账(派工单 §4.4;phase == PreflightExceeded 时填)--------
    std::size_t estimated_input_tokens = 0;
    std::size_t reserved_output_tokens = 0;
    std::size_t protocol_headroom_tokens = 0;
    bool reserve_clamped = false;  // 应急收窄放行(没拒,降级继续)
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
