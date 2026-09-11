// 上下文保护(token 轴唯一裁剪,字节轴已拆):语义压缩(compact)是主
// 防线;这里只留两条纯函数支撑——统一 token 估算尺,和一条"单条巨肥工具
// 结果"的保命索。旧的按字节整轮删裁(TrimHistory,600k 字符线)已删:
// 两把尺松紧随语言漂移(英文裁早、中文失守),互相打架,保护收敛到 token
// 轴一户。

#pragma once

#include <cstddef>
#include <map>
#include <string>
#include <vector>

#include "api/types.hpp"

namespace lubancode::agent {

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
// 改准了名字。字节轴裁剪拆除后它只剩计量用途:/context 显示、校准样本的
// 请求字节数——不做任何裁剪决策。
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
//
// 两把尺的分工(compact 切分劈开工具原子组单 §2.1):这一把是公共尺,
// 热区(HotZoneStartIndex)、事件账、episode 切分共用户,语义钉死不动;
// 它管"谁开了一轮话",不管"工具原子组完不完整"。带正文的 steer 消息
// 在它眼里是新轮头——工具循环中途插话会把一枚工具原子组劈过轮界。compact
// 规划器另有一把尺:BuildTurnPartitionPlan 调 HealTurnBoundariesOverTool
// Groups 后处理,把劈组的界点并回进行中的轮(steer 归当前 turn),两把
// 尺各司其职,互不改写。
bool IsUserTurnStart(const api::Message& message);

// 按上式把整份 history 切成连续 turn 区间:turns[i] = [from, to),首条
// 消息下标是 turn 头,到下一枚 turn 头之前收尾,末段到 history.size()。
// 真正用户输入之前若有零散消息(旧档外壳、异常形状),不属于任何 turn,
// 由调用方自行处置;一条用户输入都没有时返回空表。
std::vector<std::pair<std::size_t, std::size_t>> SplitIntoTurns(const std::vector<api::Message>& history);

// ---------------------------------------------------------------------------
// 保命索:单条巨肥工具结果的尾部截断(挂 token 轴)
//
// 防的是另一种死法:单条工具结果巨肥(read_file 吞大文件一类)时,compact
// 的摘要请求本身就会超窗——历史压多少遍,腾出来的空间都不够装下这条结果
// 自己所在的分块,压缩与请求一起死循环。旧的字节轴整轮删裁(TrimHistory)
// 已拆,这条保险改挂 token 轴口径:按真实 token 估算(含校准系数)判,
// 不按固定字节数。
// ---------------------------------------------------------------------------

// 单条工具结果的窗口占比线:一条 ToolResultBlock 的估算 token 超过估算
// 窗口的 25% 即动手截尾,截到落回线内。25% 的依据:compact 的 map 输入
// 按 turn 分块,单条结果就要吃掉窗口四分之一时,它所在的 turn 分块加上
// 压缩指令与输出预留,大概率已超压缩模型的单次输入预算——摘要请求装不下
// 历史,也装不下自己;四条这样的结果就能塞满整窗,系统提示与工具表无处
// 安放。25% 在"单条结果还留得下足够原文(128k 窗口下约 32k token)"与
// "给其余历史留足四分之三"之间取的保守线。
constexpr int kOversizedToolResultWindowPercent = 25;

// 硬裁剪报告:保命索这一次有没有真动手。上层(UI)拿到报告须向用户明说
// 发生了有损截断——静默降级会让用户以为语义压缩已成功,模型其实已经看
// 不到那段原文了。截断形状随首次采用的档位快照固定(V3-REAL-01,带
// TruncationMemo 时):同一份超线结果每个请求重放同一副形状,不算新动作;
// ContextManager 再按结果身份去重(V3-REAL-02):同一枚通报过的不重报,
// 同 epoch 新来的另一枚首次截断必须报。
struct TrimReport {
    bool truncated_results = false;  // 有超大工具结果被截尾(首次定形的新截断)
    // 本次新定形的截断落在哪些结果身上(结果身份 = tool_use_id)。带
    // memo 调用时只有首次定形且真动了刀的进列;memo 命中的重复采用不进。
    // 上层按它做"按身份去重"的通报,不再拿一枚全局布尔吞掉后来者。
    std::vector<std::string> truncated_result_ids;
};

// 保命索的钉子账(V3-REAL-01):tool_use_id -> 首次采用的截断快照。裁剪
// 结果随档位快照固定——一枚工具结果第一次进工作视图时按当次预算
//(窗口 25% × 当次校准系数)定形(线内也是定形:全文即形状),此后
// epoch 内的普通追加请求不再重裁:估算器系数更新、窗口读数变化都不得
// 追改已发前缀里那副旧形状。真要换形状,走正式 context 提交
//(ContextManager::ReplaceHistory,compact/显式降档)清账重新定形,
// 由前缀账点名一次可解释断点。原文指纹防串:同 id 的结果原文变了
//(本不该发生,重试改写一类的坏账)按新结果重新定形,不拿旧快照顶。
struct TruncationMemo {
    struct Pinned {
        std::string source_hash;       // 定形时原文(裁剪前)的指纹
        bool reduced = false;          // 定形时真动了刀(线内定形 = false)
        api::ToolResultBlock result;   // 定形后的整块形状(reduced 才有意义)
    };
    std::map<std::string, Pinned> pinned;
};

// 截断保命索(纯函数):
//   - 逐条扫 ToolResultBlock:估算 token(EstimateMessageTokens 同一把尺,
//     含校准系数 calibration)不超过 window_tokens * 25% 的原样放行;
//   - 超线的从尾部截短,截到结果自身落回 25% 线内,尾部打
//     "[内容过长已截断]" 标注;至少保留 kMinKeepBytes 字节,免得截成
//     空壳,模型连是什么工具的结果都看不出;
//   - 富块结果(MCP blocks)只裁 TextContent 的 text(从最后一块起倒着
//     裁),图片/音频/资源引用与 structuredContent 一概不动,裁完按真账
//     重算投影;
//   - 只动 tool_result 的 content/blocks,消息条数、tool_use/tool_result
//     配对关系一概不碰;
//   - 截断按内容自身算,确定性的:同一份历史每请求截出同一副形状,追加律
//     不受牵连(旧的按全量 overage 截会随历史增长滑窗,已随之退场);
//   - report 非空时把本次实际发生的截断填进去(没截就保持全默认假)。
// memo 非空时(V3-REAL-01,ContextManager 的正式路径):每枚结果的裁剪
// 形状首次采用即定形——memo 命中的(原文指纹一致)直接重放快照,当次
// 系数/窗口一概不看;线内首次定形(reduced=false)后来越线也不回头裁
//(首次已把全文发给 provider,后请求裁它必断缓存前缀)。新定形且真动了
// 刀的记进 report->truncated_result_ids。memo 为空指针 = 无记忆的一次性
// 纯函数调用(单测、dry-run),按当次预算现裁,行为与从前一字不差。
// window_tokens 传 0(窗口未知)时按 kFallbackContextWindowTokens 兜底,
// 不裸奔。返回处理后的消息(没动就是原样拷贝)。
std::vector<api::Message> ShrinkOversizedToolResults(std::vector<api::Message> messages,
                                                     std::size_t window_tokens,
                                                     double calibration = 1.0,
                                                     TrimReport* report = nullptr,
                                                     TruncationMemo* memo = nullptr);

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

// ---------------------------------------------------------------------------
// 自动压缩触发线的两笔预留(compact 切分劈开工具原子组单 §〇.1,用户定案
// 2026-09-09):触发线 = 窗口×80% − 压缩提示词 4k − 压缩结果预留 8k。例:
// 200k 窗即 148k 触发。两笔的账要算在触发线里——不扣这两笔,压到 80% 才
// 动手,摘要请求自己的指令与产出就没地方安放。与 BuildContextBudgetPlan
// 的口径分工核实过:那份算的是"压缩请求自身的输入预算"(窗口 − 输出预留
// − 协议余量 − 压缩指令,按会话现场配置取数),这里的两笔是"触发时机"
// 的固定档,各管各的账,不重复扣。
// ---------------------------------------------------------------------------
constexpr std::size_t kAutoCompactPromptReserveTokens = 4096;   // 压缩提示词
constexpr std::size_t kAutoCompactSummaryReserveTokens = 8192;  // 压缩结果预留

// 触发线(纯函数):窗口×kProjectedOverflowPercent% − 上述两笔。窗口小到
// 扣不动(不足 15360 token 一类)时夹到 0——那种窗口里任何占用都该压,
// 触发线为 0 即"始终该压"。ContextTracker 的 turn 间触发与 loop 的
// projected 双闸共用这一只,两条路口径不漂移。
inline std::size_t AutoCompactTriggerLine(std::size_t window_tokens) {
    const std::size_t percent_line =
        window_tokens * static_cast<std::size_t>(kProjectedOverflowPercent) / 100;
    const std::size_t reserves = kAutoCompactPromptReserveTokens + kAutoCompactSummaryReserveTokens;
    return percent_line > reserves ? percent_line - reserves : std::size_t{0};
}

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
//   AfterHardTrim —— 保命索这次真动了手(单条巨肥工具结果被截尾)。纯通
//                     报:上层必须向用户显式告警"发生了有损截断",不许静
//                     默降级;此时再压缩也救不回这一次的请求。
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
