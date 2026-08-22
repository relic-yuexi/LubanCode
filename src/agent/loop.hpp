// agent 核心循环:user 消息入历史 -> 带工具发请求 -> 流式转发给上层(打字机
// 输出)同时喂给 assembler 攒消息 -> stop_reason 是 tool_use 就把模型要的
// 工具都执行一遍、结果攒成一条 user 消息喂回去 -> 再发请求 -> 如此往复,
// 直到 end_turn,或者达到步数上限。

#pragma once

#include <atomic>
#include <cstdint>
#include <expected>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "agent/context.hpp"
#include "agent/context_events.hpp"
#include "agent/microcompact.hpp"
#include "agent/prefix.hpp"
#include "agent/runtime_profile.hpp"
#include "agent/tool_trace.hpp"
#include "api/backend.hpp"
#include "api/types.hpp"
#include "tools/registry.hpp"
#include "tools/tool.hpp"

namespace lubancode::agent {

class ContextArtifactStore;

// ---- 异步审批(P2:显示系统剥离单)------------------------------------------
// 审批请求与悬挂未来的中立形状。完整合同在 runtime/interaction_broker.hpp
//(决定四态/悬空收口/迟到回答);agent/ 这边只认识这两枚类型,不 include
// runtime/ 头——InteractionBroker 的装配在会话层,内核只管发请求、等
// future、按四态收账。
struct ApprovalRequest {
    std::string tool_use_id;  // P4:这次审批钉在哪个条目上(ToolUseBlock.id)
    std::string tool_name;
    nlohmann::json input = nlohmann::json::object();
    std::string reason;  // 触发这次问话的线索(hook ask 的理由/权限档说明),可空
};

// 决定四态。与 runtime::InteractionDecision 语义一一对应,这里不 enum
// 别名(避免拖头),用独立枚举 + 会话层的映射。
enum class ApprovalDecision {
    Accept,             // 本次允许
    AcceptForSession,   // 本会话内该工具不再问(只写本场权限账,不落盘)
    Decline,            // 拒绝
    Cancel,             // 悬空收口(打断/断开/超时)——等价拒绝,但拒绝文案
                        // 须写"没人可答",不冒充用户拒绝
};

struct ApprovalResponse {
    ApprovalDecision decision = ApprovalDecision::Decline;
    std::string reason;  // decline 的理由(给模型的拒绝文案线索),可空
};

// 审批的未来。WaitApproval 阻塞到有结果或悬空收口;收口返回 nullopt。
class InteractionFuture {
public:
    virtual ~InteractionFuture() = default;
    virtual std::optional<ApprovalResponse> WaitApproval() = 0;
};

// hooks 框架第三步:工具生命周期相位(UI 状态机 requested -> checking_hook
// -> waiting_permission -> running -> done,被拦时停在 blocked,不冒充"运行
// 过又失败")。on_tool_start 算 requested;下面几个相位由 on_tool_phase 报。
enum class ToolPhase {
    CheckingHook,      // PreToolUse 钩子跑起来了(权限确认之前)
    WaitingPermission, // 即将问用户确认(只有真要弹确认才报)
    Running,           // 钩子与确认都过了,工具真开跑
    Blocked,           // 被钩子拦下,不会执行
};

// PreToolUse 钩子的归并表态(deny > ask > allow;updatedInput 只与 allow
// 同返,改写后须重过工具 schema、deny 规则与权限判断——不许借钩子越权)。
struct ToolHookDecision {
    enum class Decision { None, Allow, Ask, Deny };
    Decision decision = Decision::None;
    std::string reason;  // deny/ask 的理由,给用户与模型看
    std::optional<nlohmann::json> updated_input;
    std::vector<std::string> additional_context;  // 给模型的追加上下文
};

struct Callbacks {
    // 显示系统剥离单 P4(补稳定 id):工具生命周期的回调首参一律带
    // tool_use_id(模型给的 ToolUseBlock.id;PTC stub 调用是宿主合成的
    // "ptc-N")。ToolDisplay 与未来的 EventSink 都认它路由条目,不再靠
    // "当前主/子条目"的下标猜——多 thread、并行 turn 一来,隐式槽位会串账。
    // 旧回调仍以 name 为首参的,名字挪到第二位;签名变化由编译器盯住,
    // 不会有静默漏改。

    // 流式文本增量,打字机效果打印用。
    std::function<void(const std::string& text)> on_text_delta;

    // 流式思考增量(thinking/reasoning):界面用来画"思考 Xs"折叠块。
    // 不设就静默跳过,不影响其余行为。
    std::function<void(const std::string& text)> on_thinking_delta;

    // 模型发起了一次工具调用,还没执行,给上层显示用(比如打印
    // `[工具] read_file {"path":...}`)。
    std::function<void(const std::string& tool_use_id, const std::string& name,
                       const nlohmann::json& input)> on_tool_start;

    // 工具 needs_confirm() 为真时才会调用;返回 true 表示允许执行。
    // 没设这个回调、或者工具本来就不需要确认,都视为允许。
    // 显示系统剥离单 P2:这是同步问话通道。新代码优先用下面的
    // on_tool_confirm_async(异步审批,等 InteractionFuture);两个都设时
    // async 优先,只有 async 缺位才回落到这里——回落规则与 on_pre_tool_
    // hook 之于 on_pre_tool_use_hook 同款。既有消费方(子代理/PTC 转发、
    // 单测)不动,行为一字不变。
    std::function<bool(const std::string& tool_use_id, const std::string& name,
                       const nlohmann::json& input)> on_tool_confirm;

    // 异步审批通道(P2):与 on_tool_confirm 同一个触发点(工具
    // needs_confirm 且档位真要问用户),但把"问"变成"发请求、拿 future":
    // 回调立即返回 future,RunOneTool 在原地 Wait(工作线程阻塞等,事件
    // 泵/连接线程不跟着堵)。终端前端的实现还是当场问完(同步短路,行为
    // 与今日一字不差);远端前端(app-server/Web/Tauri)的实现登记
    // request_id 悬起,前端从任何线程 ResolveApproval 回答——审批从此
    // 不再钉死在 stdin 上。
    // 类型是 runtime/ 的中立合同(agent/ 不 include runtime/ 也行,但
    // approval 的形状就是 InteractionBroker 那套四态;这里用前置声明 +
    // shared_ptr,保持 agent 头不拖 runtime 头)。
    // P4:ApprovalRequest 带 tool_use_id,远端前端凭它把审批事件钉回条目。
    std::function<std::shared_ptr<InteractionFuture>(const ApprovalRequest& request)> on_tool_confirm_async;

    // on_tool_confirm 返回 false(拒绝)后,给模型的 tool_result 文案从这里
    // 取;不设用缺省"用户拒绝执行该工具"。给后台子代理用——它没人可问,
    // 拒绝的原因是"后台无法弹确认、未预放行",不是用户拒绝;子代理照缺省
    // 文案汇报,最终报告就会写成"均被用户拒绝",误导派工的主模型
    //(后台代理权限拒绝无告知单,2026-08-17)。与 on_tool_confirm 同线程
    // 先后调用,回调层可以拿同一份局部状态区分拒绝原因。
    std::function<std::string(const std::string& tool_use_id, const std::string& name)> on_tool_denial_text;

    // 工具跑完了(不管成功、失败、被拒绝、还是压根没找到这个工具),都会调用一次。
    std::function<void(const std::string& tool_use_id, const std::string& name,
                       const tools::Tool::Result& result)> on_tool_done;

    // 服务端内置工具只展示，不经本地 registry 执行。比如 Responses 的
    // web_search_call；两枚回调保证界面也有 running -> done 轨迹。
    // P4:首参带服务端给的 id(BuiltinToolStart/Done 的 e.id),可空——
    // 条目路由与 ToolUseBlock 同一套规矩。
    std::function<void(const std::string& tool_use_id, const std::string& name,
                       const nlohmann::json& input)> on_builtin_tool_start;
    std::function<void(const std::string& tool_use_id, const std::string& name, const nlohmann::json& input,
                       const std::string& summary, bool is_error)>
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
    //
    // hooks 框架第三步:新代码请用下面的 on_pre_tool_use_hook(带决策/
    // updatedInput/additionalContext 的完整表态);这个旧回调保留作兼容,
    // 两者都设时新回调优先,只有新回调缺位才回落到旧回调。
    std::function<std::optional<std::string>(const std::string& tool_use_id, const std::string& name, const nlohmann::json& input)> on_pre_tool_hook;

    // hooks 框架:PreToolUse 的完整表态。deny -> 工具不执行(确认也不问);
    // ask -> 即使确认档本来放行,也要问用户;allow -> 跳过用户确认,但
    // deny_commands/权限规则照走(在确认回调里,不许钩子越权);updatedInput
    // 只与 allow 同返,RunOneTool 会先过一遍工具 schema,改写打回即拦。
    std::function<ToolHookDecision(const std::string& tool_use_id, const std::string& name, const nlohmann::json& input)> on_pre_tool_use_hook;

    // hooks 框架:PermissionRequest。只有宿主本来要问用户确认时才触发
    // (RunOneTool 在调 on_tool_confirm 前把这个相位交给确认回调那一层,
    // 由它判断"真要弹确认"再发射)。deny -> 拒绝执行;allow -> 不弹确认;
    // 不表态 -> 正常问用户。
    std::function<ToolHookDecision(const std::string& tool_use_id, const std::string& name, const nlohmann::json& input)> on_permission_request;

    // hooks 框架:UI 工具状态机的相位通报。没配 hooks 的会话不设这个回调,
    // 展示行为与从前逐字节一致。
    std::function<void(const std::string& tool_use_id, const std::string& name, ToolPhase phase)> on_tool_phase;

    // M9:hooks.post_tool。工具真的执行完了(拿到 Result)才调用一次;不会
    // 影响返回给模型的结果,单纯给上层一个"跑一下 post_tool 命令"的机会。
    // 不设就跳过。
    std::function<void(const std::string& tool_use_id, const std::string& name, const nlohmann::json& input,
                       const tools::Tool::Result& result)>
        on_post_tool_hook;

    // hooks 框架:PostToolUse 的完整版。与旧回调的差别:在工具结果清洗成
    // 合法 UTF-8 之后触发(旧回调吃原始结果),返回的每段文本追加进模型
    // 所见的 tool_result(副作用已经发生,不能撤销,只许追加反馈;原始
    // 结果照旧进审计账)。不设就跳过。
    std::function<std::vector<std::string>(const std::string& tool_use_id, const std::string& name, const nlohmann::json& input,
                                           const tools::Tool::Result& result)>
        on_post_tool_use_hook;

    // ---- 逐枚追踪单:canonical 工具生命周期事件的唯一出水口 ---------------
    // RunOneTool 与工具循环只把栅栏事件(scheduled/execution_started/
    // execution_finished/result_committed,以及 verification/迟到响应)交给
    // 这一个回调;UI 投影(Runtime EventSink)、持久账(SessionTraceSink)、
    // Workflow projection、Hook correlation 各自从这条流里取自己那份——
    // 不许 AgentLoop 同时手调三份写口(单子"一份事件,两路消费")。
    // 不设 = 没装配 trace hub,行为与从前逐字节一致(单测/子代理旧路)。
    // 栅栏语义与持久规矩见 agent/tool_trace.hpp 文件头。同步回调,在工具
    // 执行线程里被调,sink 自己管线程安全;sink 失败各自报稳定错误,UI
    // 失败不拦工具,durable started 写失败由装配层按 effect class 决定
    // 是否拦执行。
    std::function<void(const ToolTraceEvent&)> on_tool_trace;
    // 拦截查询:RunOneTool 在 emit(execution_started) 之后、Tool::execute
    // 之前同步问一句。true = trace sink 判这枚 execution 该拦(started
    // 落不住且是副作用档),RunOneTool 立即以 result_store_failed 收尾,
    // 不 execute。不设 = 没有拦截源,照常执行。
    std::function<bool(const std::string& execution_id)> on_tool_trace_blocked;

    // ---- 逐枚追踪:消息落盘次序的三个关口(单子"消息落盘次序要改") ------
    // 1. assistant 消息组装完、刚入 history:装配层 append+flush 进 session。
    //    不设 = 老路(整轮收口后 PersistNewMessages),行为不变。
    std::function<void(const api::Message&)> on_assistant_message_ready;
    // 2. 本批五枚 tool result 全收齐、合并的 user 消息刚入 history:装配层
    //    append+flush user 消息,再为每枚写 result_committed 栅栏。
    std::function<void(const std::string& batch_id, const api::Message& tool_result_message)> on_tool_results_committed;
};

// 输出预算耗尽的明细账(规格根因四):max_tokens 从普通 end turn 里拆出来
// 的独立状态。exhausted=true 表示本次 Run() 以 finish_reason=length 收场、
// 自动续跑也用完(或未开)、最终一个正文字都没有——已收的 Text/Thinking/
// tool use/usage 全在 history 与 usage 回调里保住,这份账只管把"撞了哪堵
// 墙、续了几次、usage 报没报、思考留没留"交出去,失败页与子代理收场
// 分型共用,main 与子代理同一状态机。
struct OutputBudgetReport {
    bool exhausted = false;
    // 实际生效的输出上限;0 = unset(请求没带字段,交服务端/模型默认——
    // 这时候墙在服务端,数值客户端看不到)。
    int limit_tokens = 0;
    int continuations_used = 0;  // 自动续跑已用掉几次
    int continuation_limit = 0;  // 配置允许的续跑次数(profile.length_continuations)
    // 整场 Run() 里服务端有没有回过 usage(任一请求带回过非零 usage 即真)。
    // 未报告时 token 数不可知,展示层必须写"未报告",不许画 0。
    bool usage_reported = false;
    // 已收 thinking 的总字节与末段摘要(检查点证据:模型确实在思考,不是
    // 一个字没跑)。tail 截尾保留,不整段外带。
    std::size_t thinking_bytes = 0;
    std::string thinking_tail;
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
// output_budget:输出预算账(见上)。length_empty_output 是它的旧视图
// (exhausted && 无正文),保留给既有消费方。
struct RunOutcome {
    bool cancelled = false;
    bool hit_step_limit = false;
    // 输出预算耗尽(finish_reason=length→"max_tokens")且最后一条 assistant
    // 正文为空(reasoning 吃光了预算,一个正文字都没落)。不是错误,但调用方
    // 必须向用户明报,不许静默留一片空白(本地兼容端 Effort 诊断单)。
    bool length_empty_output = false;
    std::string stop_reason;  // 模型最后一次应答的原始 stop_reason(空 = 一个字都没回来)
    int steps_used = 0;       // 本次 Run() 实际发出的模型请求数(turn 内的 step 数)
    OutputBudgetReport output_budget;
};

// 步数将尽提醒:剩三步时在当步末条消息尾部附一句"收口"提示——停止
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

// 逐枚追踪:一枚工具调用的执行上下文(execution_id 宿主发号、批次与
// 序号、父执行/重试/补偿关系)。trace 缺席(单测/子代理旧路)时
// RunOneTool 的栅栏发射全部空操作,行为与从前逐字节一致。
struct ToolTraceContext {
    std::string execution_id;   // 审计主键(IdAuthority 的 item id 同源)
    std::string thread_id;      // 哪场会话
    std::string turn_id;        // 哪一轮
    std::string provider_request_id;  // MessageStart 的 request id;可空
    std::string batch_id;       // 同一 assistant message 的五枚共用
    int sequence_in_batch = -1; // 0..N-1
    std::string parent_execution_id;   // 子代理/PTC 内层归属;可空
    std::string retry_of;              // 显式重试关系;可空
    std::string blocked_by;            // 宿主因前置失败明确跳过;可空
    std::string compensates;           // 补偿哪枚调用;可空
};

// 执行一枚工具调用的完整链(公开导出;实现在 loop.cpp 顶部,注释在那头):
// 找工具/延迟挂载谓词 -> PreToolUse(含 updatedInput 的 schema 复检) ->
// 确认档(needs_confirm + PermissionRequest)-> 执行 -> PostToolUse -> 编码
// 清洗 -> on_tool_done。JSON 后端的工具循环与 PTC 的每一枚 stub 调用共用
// 这一条路,不许有第二条绕过 hooks/权限的暗门。
// filter_denial:过滤谓词不放行时的说明文案。默认空 = tool_search 的
// "尚未挂载"说法;Explore 这类角色限制的调用方另给一句写明"角色限制"
// 的文案——限制须来自角色并看得见,不许含糊成"子代理无权限"(规格)。
// trace:逐枚追踪的执行上下文;nullptr = 不追踪(旧行为)。
tools::Tool::Result RunOneTool(tools::ToolRegistry& registry, const api::ToolUseBlock& call, const Callbacks& callbacks,
                                const std::function<bool(const tools::Tool&)>& tool_filter,
                                const std::string& filter_denial = std::string(),
                                const ToolTraceContext* trace = nullptr);

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
    // 正门(规格根因一):吃一份不可变 AgentRuntimeProfile——输出预算、
    // 上下文预算、窗口、步数、length 续跑次数全从这一份来。main、
    // general-purpose 子代理、后台子代理、单发模式各自声明覆盖什么,其余
    // 继承,不再一串易漏的裸参数。system_prompt 单独给(它随 /clear 等重建)。
    AgentLoop(api::Backend& backend, tools::ToolRegistry& registry, AgentRuntimeProfile profile,
              std::string system_prompt);

    // 兼容门(单测与既有调用方):裸参数版,内部折成一份 profile。
    // max_tokens 不再有 4096 默认——nullopt = unset(chat/responses 不发
    // 字段交服务端默认;anthropic 由 client 落公开兜底),想给值显式传。
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
              std::string system_prompt, std::optional<int> max_tokens = std::nullopt,
              int max_steps_per_turn = 0, std::size_t max_context_chars = MaxContextCharsFromEnv());

    // 只读访问:运行期诊断(/context、agent 查看态)要展示"这份 loop 实际
    // 吃到的预算与来源",不再让各处自己猜。
    const AgentRuntimeProfile& runtime_profile() const { return profile_; }

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

    // 过滤谓词不放行时的说明文案(见 RunOneTool 的 filter_denial 注释)。
    // 不设 = "尚未挂载,请先 tool_search"的默认说法;Explore 这类显式
    // 只读角色设成"角色限制"的说法,模型与用户都看得出限制来自角色。
    void SetToolFilterDenial(std::string message) { tool_filter_denial_ = std::move(message); }

    // /worktree 切换目录后只换运行环境段，已有聊天史要照留。主循环在下一
    // 次请求前换掉系统提示，文件工具则由进程 CWD 即刻接管。
    void SetSystemPrompt(std::string system_prompt) { system_prompt_ = std::move(system_prompt); }

    // 跨会话传话的安全收件点(见上 InjectIncomingMessage 注释):每次调
    // 最多交出一封信;循环边界反复调到交空为止。回调只在主线程(Run 所在
    // 线程)的工具往返边界被调,绝不与流式回调、确认回调并发——"卡在权限
    // 确认时来信不能作答"由这一点天然保证。传空清除。
    void SetInbox(InboxPoll inbox) { inbox_ = std::move(inbox); }

    // 请求级动态上下文(项目记忆召回、运行中子代理名册)。前缀缓存守恒单
    // 第五期起不再塞 system 尾巴——那会让分叉点落在全部旧历史之前,每条
    // 外层用户消息都断一次前缀。现在它随本轮 user 消息进 request_history_
    // 的尾部 TextBlock,发过即钉住,后续请求原样重放;持久 history_ 不收
    // 这块,session/export/compact/记忆抽取都只见用户真输入。空串 = 不追加。
    void SetTurnContext(std::string context) { turn_context_ = std::move(context); }

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
        request_history_ = history_;
        // mid-turn compact 在 Run() 的请求安全点同步换史。本轮动态上下文
        // 不该进 compact/session,却仍须给压缩后的下一次请求看；新 epoch
        // 已经开了,补在最新消息尾部即可,不追改旧请求。
        if (run_active_ && !active_turn_context_.empty() && !request_history_.empty()) {
            request_history_.back().content.push_back(api::TextBlock{active_turn_context_});
        }
        ++cache_epoch_;
        pending_epoch_break_reason_ = "history_compacted";
        last_prefix_.reset();
        // 新 epoch,压缩决策与 sticky 视图一并翻篇:compact 是唯一常规的
        // 全量重写点,重写后的视图从头定形(前缀缓存守恒单第六期)。
        result_view_memo_.decisions.clear();
        sticky_view_.reset();
        sticky_base_history_size_ = 0;
    }

    // 当前 cache epoch(前缀记账,agent/prefix.hpp):1 起,每次断前缀 +1。
    // /context 与调试展示用;epoch 断不是失败,是给"命中跌了"点名的那根梁。
    int cache_epoch() const { return cache_epoch_; }

    // mid-turn 上下文安全点:有效上下文窗口(token)。0(默认)= 未知,
    // Run() 不做 projected 评估,行为与从前完全一致。上层(交互会话)在
    // 构造/重建 loop、/context 或 /model 改窗口后同步进来。
    void SetContextWindowTokens(std::size_t window_tokens) { profile_.context_window_tokens = window_tokens; }

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

    // 可追回 artifact(渐进式上下文仓第二期):设了仓之后,结构压缩判成
    // Artifact 的超长结果先原子落盘(blob -> chunks -> index),视图渲染带
    // 稳定 artifact_id,模型凭 id 走 context_search/context_read 追回全文;
    // 落盘失败保内存全文,行为退回没仓的样子。传空指针清除。仓的存活期
    // 由调用方(会话层)保证。
    void SetArtifactStore(ContextArtifactStore* store) { artifact_store_ = store; }

    // 最近一次请求的结构压缩账(/context 与诊断用)。
    const StructuralCompressionStats& structural_stats() const { return structural_stats_; }

    // L2 microcompact(第三期):把一趟局部摘要写进决策台账(冷区 artifact
    // 的视图从 L1 预览换成 cheap 摘要)。这是有意改已发前缀的收拾动作:
    // 给前缀记账点名(epoch 断因 microcompact)、清掉钉住的 sticky 视图按
    // 新决策重算,不装无事发生。原文与仓里的 blob 不动。返回换掉的枚数。
    int ApplyMicrocompactSummaries(const std::map<std::string, MicrocompactSummary>& summaries) {
        const int applied = agent::ApplyMicrocompactSummaries(result_view_memo_, summaries);
        if (applied > 0) {
            if (pending_epoch_break_reason_.empty()) {
                pending_epoch_break_reason_ = "microcompact";
            }
            sticky_view_.reset();
            sticky_base_history_size_ = 0;
        }
        return applied;
    }

    // 决策台账只读口(L2 挑候选用)。
    const ResultViewMemo& result_view_memo() const { return result_view_memo_; }

    // 逐枚追踪:execution_id 发号口。装配层(接了 Runtime 的会话)把它指
    // 到 IdAuthority::NextItemId 上——execution_id 与 Runtime item id 同源,
    // 不另开计数器。不设 = 旧路兜底 "exec-N"(仅单测/未接 Runtime 的会话)。
    void SetExecutionIdIssuer(std::function<std::string()> issuer) { execution_id_issuer_ = std::move(issuer); }

private:
    api::Backend& backend_;
    tools::ToolRegistry& registry_;
    std::string model_;
    std::string system_prompt_;
    std::string turn_context_;
    std::string active_turn_context_;  // 只在 Run() 活着时给 mid-turn compact 重注入
    bool run_active_ = false;
    // 运行策略(输出/上下文预算、窗口、步数、length 续跑):构造时定死,
    // 只有 context_window_tokens 有 setter(随 /context、/model 同步)。
    AgentRuntimeProfile profile_;
    bool structural_compression_enabled_ = true;  // 无损结构压缩(工作视图)
    StructuralCompressionOptions structural_options_{};
    StructuralCompressionStats structural_stats_{};  // 最近一次请求的结构压缩账(观测用)
    ContextArtifactStore* artifact_store_ = nullptr;  // 可追回 artifact 的仓(空 = 没仓,退回旧行为)
    std::vector<api::Message> history_;          // 可持久、可 compact 的真历史
    std::vector<api::Message> request_history_;  // 模型视图；另含每轮动态上下文
    // 前缀记账(agent/prefix.hpp):上一份实际发出的请求指纹(没有 = 本
    // turn 第一份请求,无从比较,天然算追加)、cache epoch 序号、loop 自己
    // 先知道的断因(compact/hard trim,报出后即清)。
    std::optional<PrefixFingerprint> last_prefix_;
    int cache_epoch_ = 1;
    std::string pending_epoch_break_reason_;
    // 结构压缩"首次定形"的决策台账(tool_use_id -> 决策),epoch 内跨请求
    // 钉死;ReplaceHistory(开新 epoch)时清空(agent/context_events.hpp)。
    ResultViewMemo result_view_memo_;
    // hard trim 的 sticky 工作视图:第一次真动手裁(丢轮/截结果)后把裁过
    // 的视图钉住,后续请求只往它尾部追加新消息——不再每请求拿全量 history
    // 重算"第一轮 + 最近 N 轮",裁剪窗口一路滑。sticky_base_history_size_
    // 记钉住那一刻全量视图的长度,追加时按它切尾。ReplaceHistory 时翻篇。
    std::optional<std::vector<api::Message>> sticky_view_;
    std::size_t sticky_base_history_size_ = 0;
    OnContextPressure on_context_pressure_;
    std::function<bool(const tools::Tool&)> tool_filter_;  // tool_search:空 = 不过滤,全量直挂
    std::string tool_filter_denial_;  // 过滤不放行的说明(空 = 默认"尚未挂载"文案)
    InboxPoll inbox_;  // 跨会话收件点:空 = 没有来信要收,行为跟从前一致

    // 逐枚追踪:批次序号(execution_id 的兜底发号)。装配层接了 Runtime
    // 的会话在 SetExecutionIdIssuer 里换成 IdAuthority 的号——单子明言
    // 不可再造第二只计数器,这里只是没接 Runtime 的旧路(单测)兜底。
    int batch_counter_ = 0;
    std::uint64_t execution_counter_ = 0;
    std::function<std::string()> execution_id_issuer_;
    std::string issue_execution_id() {
        if (execution_id_issuer_) {
            return execution_id_issuer_();
        }
        return "exec-" + std::to_string(++execution_counter_);
    }

    std::vector<api::ToolDefinition> BuildToolDefinitions() const;
};

}  // namespace lubancode::agent
