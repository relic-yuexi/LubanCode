// agent 核心循环:user 消息入历史 -> 带工具发请求 -> 流式转发给上层(打字机
// 输出)同时喂给 assembler 攒消息 -> stop_reason 是 tool_use 就把模型要的
// 工具都执行一遍、结果攒成一条 user 消息喂回去 -> 再发请求 -> 如此往复,
// 直到 end_turn,或者达到步数上限。

#pragma once

#include <atomic>
#include <expected>
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "agent/context.hpp"
#include "agent/context_events.hpp"
#include "agent/prefix.hpp"
#include "api/backend.hpp"
#include "api/types.hpp"
#include "tools/registry.hpp"
#include "tools/tool.hpp"

namespace lubancode::agent {

struct Callbacks {
    // 流式文本增量,打字机效果打印用。
    std::function<void(const std::string& text)> on_text_delta;

    // 流式思考增量(thinking/reasoning):界面用来画"思考 Xs"折叠块。
    // 不设就静默跳过,不影响其余行为。
    std::function<void(const std::string& text)> on_thinking_delta;

    // 模型发起了一次工具调用,还没执行,给上层显示用(比如打印
    // `[工具] read_file {"path":...}`)。
    std::function<void(const std::string& name, const nlohmann::json& input)> on_tool_start;

    // 工具 needs_confirm() 为真时才会调用;返回 true 表示允许执行。
    // 没设这个回调、或者工具本来就不需要确认,都视为允许。
    std::function<bool(const std::string& name, const nlohmann::json& input)> on_tool_confirm;

    // 工具跑完了(不管成功、失败、被拒绝、还是压根没找到这个工具),都会调用一次。
    std::function<void(const std::string& name, const tools::Tool::Result& result)> on_tool_done;

    // 服务端内置工具只展示，不经本地 registry 执行。比如 Responses 的
    // web_search_call；两枚回调保证界面也有 running -> done 轨迹。
    std::function<void(const std::string& name, const nlohmann::json& input)> on_builtin_tool_start;
    std::function<void(const std::string& name, const nlohmann::json& input, const std::string& summary,
                       bool is_error)>
        on_builtin_tool_done;

    // 每一次到模型的独立请求结束时(MessageDone 到达那一刻)都会调用一次,
    // 把这一次的 usage 连同身份(步号/请求 id/模型)报出来。一次 Run() 内部
    // 可能因为工具调用来回好几趟,也就是好几次独立请求——这个回调按请求
    // 粒度触发,不是按 Run() 粒度,上层(turn_runner 的逐步流水账)拿它落
    // StepUsageRecord,整轮汇总从记录求和。可选;不设就跳过,不影响其余行为。
    std::function<void(const api::UsageReport& report)> on_usage;

    // M9:hooks.pre_tool。工具已经找到、还没问确认、更没执行的时候调用一次;
    // 返回非空表示被拦截——值就是要塞进 tool_result 里的 is_error 说明文本,
    // 该工具这次不会真的执行(needs_confirm 的确认也不会问)。返回
    // std::nullopt(或者压根没设这个回调)表示放行,跟没有 hooks 系统时
    // 的行为完全一样——main.cpp 不配 hooks 时就不设这个回调。
    // agent/ 本身不知道、也不关心 hooks 具体怎么解析、怎么执行(config::/
    // tools::hooks 那一层的事),只提供这一个挂接点,好保持依赖单向
    // (agent/ 不反过来牵扯 config/)。
    std::function<std::optional<std::string>(const std::string& name, const nlohmann::json& input)> on_pre_tool_hook;

    // M9:hooks.post_tool。工具真的执行完了(拿到 Result)才调用一次;不会
    // 影响返回给模型的结果,单纯给上层一个"跑一下 post_tool 命令"的机会。
    // 不设就跳过。
    std::function<void(const std::string& name, const nlohmann::json& input, const tools::Tool::Result& result)>
        on_post_tool_hook;
};

// Run() 的收场情况。cancelled=true 表示这一轮是被 ESC 打断收场的——打断
// 不是错误(std::expected 的 value 分支,不是 error 分支),半截 assistant
// 文本已经照常带着打断标注入了历史,history() 状态完整、下一轮能正常接着
// 聊,调用方(main.cpp)只需要照这个标志决定要不要额外打提示。
// hit_step_limit=true 表示步数预算用满(max_steps_per_turn>0 才可能):也不是
// 错误,history 里留着到限为止的全部来回——上层(子代理)据此按
// budget_exhausted 收账、带走部分结果,不许笼统当 failed。
// stop_reason/steps_used 把模型最后一次应答的原始 stop reason 与实际请求
// 次数交出去,失败语义由调用方分型。
struct RunOutcome {
    bool cancelled = false;
    bool hit_step_limit = false;
    std::string stop_reason;  // 模型最后一次应答的原始 stop_reason(空 = 一个字都没回来)
    int steps_used = 0;       // 本次 Run() 实际发出的模型请求数(turn 内的 step 数)
};

// 步数将尽提醒:剩三步时当步请求在 system 尾部附一句"收口"提示——停止
// 漫游、写检查点、交部分结论(规格"现场四")。只注入一次:提示落在"剩余
// 步数第一次降到阈值(含)以下"的那一步,此后各步不再重复(重复念叨只会
// 把剩余步数也烧掉)。阈值定死为 3。max_steps_per_turn <= 0(无上限)时压根
// 没有"将尽"这回事,见 ShouldNudgeStepLimit 实现——直接恒为 false。
constexpr int kStepLimitNudgeThreshold = 3;

// 纯函数,可单测:第 step_index 步(0-based,对应 Run() 里 for 循环的循环
// 变量)、总共 max_steps_per_turn 步,判断这一步该不该在请求里附加"步数
// 将尽"的提示。max_steps_per_turn <= 0 表示无上限,永远不触发(压根没有
// "将尽"这回事)。
bool ShouldNudgeStepLimit(int step_index, int max_steps_per_turn);

// ---------------------------------------------------------------------------
// mid-turn 上下文安全点(0.27.x 分层压缩第一期)
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

using OnContextPressure = std::function<void(const ContextPressure&)>;

// 跨会话传话(0.25.x)的安全收件点:Run() 的工具循环每次"下一次请求尚未
// 发出"的边界(循环顶)会调一次 inbox;有信就注进 history,再发请求——
// 工具跑着不打断,正文收口后才收。注入规则(纯函数,单测钉):
//   - history 末条是 user(比如刚攒完的 tool_result 消息):把来信的文本块
//     追加到那条消息的末尾(保持 user/assistant 交替,三种 wire 都安全);
//   - 否则(末条是 assistant 等罕见边界):新起一条 user 消息。
// 来信的"来历"由调用方在文本里带清来源标识(不装成用户手敲),这里只管
// 结构;来信绝不会被当成确认、权限或命令——这条路由里根本没有那些口子。
void InjectIncomingMessage(std::vector<api::Message>& history, api::Message incoming);

using InboxPoll = std::function<std::optional<api::Message>()>;

class AgentLoop {
public:
    // max_steps_per_turn:一次 Run()(一个 turn)里最多跟模型来回几步(每步
    // 一次模型请求;一步可含多枚工具调用)。
    // <= 0 表示无上限——现在的模型常态是跑十几个小时的长程任务,任何硬闸
    // 都是矮墙;默认不设上限,防跑飞靠用户 ESC/Ctrl+C 打断和成本可见性
    // (跟 Claude Code 一个待遇)。想设闸的人显式配一个正整数,超过这个数
    // 还没到 end_turn 就报错退出——闸只服务"我确实想要一个硬上限"这个场景
    // (比如管道模式没有 ESC 可打断,想兜底防真死循环)。main.cpp 里这个值
    // 改由 config.max_steps_per_turn 传入(可经配置文件/环境变量调整,旧名
    // max_turns 兼容读入),这里的默认参数只服务不经过 main.cpp 配置流程的
    // 调用方(单测、未来的其它入口)。
    // max_context_chars:发给模型前 history 裁剪的阈值(字符数),默认读
    // 环境变量 LUBANCODE_MAX_CONTEXT(没设置就是 kDefaultMaxContextChars)。
    AgentLoop(api::Backend& backend, tools::ToolRegistry& registry, std::string model,
              std::string system_prompt, int max_tokens = 4096, int max_steps_per_turn = 0,
              std::size_t max_context_chars = MaxContextCharsFromEnv());

    // 发一轮用户输入。内部可能会跑好几个来回(工具调用),直到模型给出
    // end_turn(或者别的非 tool_use 的 stop_reason)才返回。历史跨多次
    // Run() 调用保留,下一句问话会带着之前的上下文。
    // cancel 非空且流式/工具执行期间被外部(cli 层的 ESC 监听线程)置位:
    // 半截 assistant 文本(如果已经流出来了)照常攒进历史,末尾附一段打断
    // 标注;工具循环发现自己被打断,已经在执行的那个工具的结果照常入历史、
    // 还没轮到的补一条"未执行"的合成 tool_result(保住 tool_use/tool_result
    // 成对约束,不然下一轮重放历史会被 API 拒绝);两种情况都从 Run() 正常
    // 返回(RunOutcome::cancelled = true),不是 std::unexpected——打断不是错误。
    std::expected<RunOutcome, std::string> Run(const std::string& user_input, const Callbacks& callbacks,
                                                const std::atomic<bool>* cancel = nullptr);

    // 跟字符串入口同义，只是调用方已经把本地图片装进 user_message 了。图片
    // 也须原样入 history，下一轮、重发、会话恢复才能带得上。
    std::expected<RunOutcome, std::string> Run(api::Message user_message, const Callbacks& callbacks,
                                                const std::atomic<bool>* cancel = nullptr);

    const std::vector<api::Message>& history() const { return history_; }

    // tool_search(延迟挂载):工具过滤谓词。设了之后,每轮请求的 tools
    // 数组只拼"谓词放行"的工具;模型调用了"注册表里查得到、谓词却不放行"
    // 的工具(延迟且未挂载)时,不执行,回一条友好错误让模型先走
    // tool_search。谓词由 main.cpp 注入(按 loaded 集合过滤),AgentLoop
    // 自己不懂什么叫"延迟"——不设(默认)行为跟从前完全一样。每轮现查
    // 而不是构造时定死,是因为 tool_search 命中会在一次 Run() 中途改变
    // loaded 集合,下一轮请求就得看到新挂载的工具。
    void SetToolFilter(std::function<bool(const tools::Tool&)> filter) { tool_filter_ = std::move(filter); }

    // /worktree 切换目录后只换运行环境段，已有聊天史要照留。主循环在下一
    // 次请求前换掉系统提示，文件工具则由进程 CWD 即刻接管。
    void SetSystemPrompt(std::string system_prompt) { system_prompt_ = std::move(system_prompt); }

    // 跨会话传话的安全收件点(见上 InjectIncomingMessage 注释):每次调
    // 最多交出一封信;循环边界反复调到交空为止。回调只在主线程(Run 所在
    // 线程)的工具往返边界被调,绝不与流式回调、确认回调并发——"卡在权限
    // 确认时来信不能作答"由这一点天然保证。传空清除。
    void SetInbox(InboxPoll inbox) { inbox_ = std::move(inbox); }

    // 请求级上下文尾段。一次外层 Run 内的工具来回共用，下一条用户消息
    // 到来前由调用方重算。它不进 history，也不改稳定 system_prompt_；项目
    // 记忆这类按查询变化、又须经得住 /compact 的上下文走这里。
    void SetTurnSystemSuffix(std::string suffix) { turn_system_suffix_ = std::move(suffix); }

    // M6.6:/compact 用。跟 history() 是同一份数据,单独起个大写名字是为了
    // 跟任务规矩"只许新增两个方法,不许改现有的"对齐——不改名、不改签名、
    // 不复用 history(),原样再加一份。
    const std::vector<api::Message>& History() const { return history_; }

    // M6.6:/compact 压缩完之后,把 AgentLoop 内部存的完整历史换成压缩后的
    // 那份(archive 消息 + 最近一轮完整对话)。是本次任务里唯一允许写
    // history_ 的新入口,agent/compact.cpp 里的 Compact() 本身不碰
    // AgentLoop,只管算出新历史,真正替换由调用方(main.cpp)拿到新历史后
    // 调这个方法完成。
    // 前缀记账:这是有意改前缀,不装无事发生——显式开新 cache epoch
    // (history_compacted),清掉上一份请求的指纹;压缩后第一份请求就是新
    // epoch 的冷启动,后续再守追加律。
    void ReplaceHistory(std::vector<api::Message> new_history) {
        history_ = std::move(new_history);
        ++cache_epoch_;
        pending_epoch_break_reason_ = "history_compacted";
        last_prefix_.reset();
    }

    // 当前 cache epoch(前缀记账,agent/prefix.hpp):1 起,每次断前缀 +1。
    // /context 与调试展示用;epoch 断不是失败,是给"命中跌了"点名的那根梁。
    int cache_epoch() const { return cache_epoch_; }

    // mid-turn 上下文安全点:有效上下文窗口(token)。0(默认)= 未知,
    // Run() 不做 projected 评估,行为与从前完全一致。上层(交互会话)在
    // 构造/重建 loop、/context 或 /model 改窗口后同步进来。
    void SetContextWindowTokens(std::size_t window_tokens) { context_window_tokens_ = window_tokens; }

    // mid-turn 上下文安全点:压力通报回调。只在"工具结果已攒完、请求尚未
    // 发出"的轮次边界被调(PreRequest 阶段),以及 TrimHistory 这次真丢了
    // 东西之后(AfterHardTrim 阶段,纯通报)。回调在同一线程同步执行,里
    // 面可以安全地做一次语义压缩并 ReplaceHistory。不设 = 不通报,安全网
    // 照旧只是没人听见。
    void SetOnContextPressure(OnContextPressure hook) { on_context_pressure_ = std::move(hook); }

    // 无损结构压缩(0.27.x 第二期):默认开。每次请求前把"发给模型的
    // 视图"里的重复工具结果、被覆盖的旧版读取、超长结果换成引用与预览
    // (agent/context_events.hpp);活历史 history_ 与 session JSONL 一字
    // 不动,tool use/result 配对不破。关掉 = 视图与从前逐字节一致。
    void SetStructuralCompressionEnabled(bool enabled) { structural_compression_enabled_ = enabled; }
    void SetStructuralCompressionOptions(const StructuralCompressionOptions& options) {
        structural_options_ = options;
    }

private:
    api::Backend& backend_;
    tools::ToolRegistry& registry_;
    std::string model_;
    std::string system_prompt_;
    std::string turn_system_suffix_;
    int max_tokens_;
    int max_steps_per_turn_;  // 0 = 不限步(硬上限只管本 turn 内的 step)
    std::size_t max_context_chars_;
    std::size_t context_window_tokens_ = 0;  // mid-turn 评估用;0 = 未知,不评估
    bool structural_compression_enabled_ = true;  // 无损结构压缩(工作视图)
    StructuralCompressionOptions structural_options_{};
    StructuralCompressionStats structural_stats_{};  // 最近一次请求的结构压缩账(观测用)
    std::vector<api::Message> history_;
    // 前缀记账(agent/prefix.hpp):上一份实际发出的请求指纹(没有 = 本
    // turn 第一份请求,无从比较,天然算追加)、cache epoch 序号、loop 自己
    // 先知道的断因(compact/hard trim,报出后即清)。
    std::optional<PrefixFingerprint> last_prefix_;
    int cache_epoch_ = 1;
    std::string pending_epoch_break_reason_;
    OnContextPressure on_context_pressure_;
    std::function<bool(const tools::Tool&)> tool_filter_;  // tool_search:空 = 不过滤,全量直挂
    InboxPoll inbox_;  // 跨会话收件点:空 = 没有来信要收,行为跟从前一致

    std::vector<api::ToolDefinition> BuildToolDefinitions() const;
};

}  // namespace lubancode::agent
