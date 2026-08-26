// agent 轮次推进器(骨架拆解批四:Agent 自立门户去了 agent/agent.hpp,
// 这里只剩 loop 的家当):user 消息入历史 -> 带工具发请求 -> 流式转发给
// 上层(打字机输出)同时喂给 assembler 攒消息 -> stop_reason 是 tool_use
// 就把模型要的工具都执行一遍、结果攒成一条 user 消息喂回去 -> 再发请求,
// 如此往复,直到 end_turn,或者达到步数上限。
//
// 住这头的:Callbacks(装配并行的回调笔)、RunOutcome/OutputBudgetReport
//(收口判据)、AgentLoop、RunOneTool(工具执行的完整链)。审批四态与钩子
// 表态在 runtime/interaction.hpp;ToolTraceContext 在 agent/tool_trace.hpp;
// ContextPressure 在 agent/context.hpp;Agent/AgentProfile/AgentWiring 在
// agent/agent.hpp。

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

#include "agent/tool_trace.hpp"
#include "api/types.hpp"
#include "runtime/interaction.hpp"
#include "tools/tool.hpp"
#include "tools/registry.hpp"

namespace lubancode::agent {

class ContextArtifactStore;

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
    // on_tool_confirm_async(异步审批,等 runtime::InteractionFuture);两个都设时
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
    // 合同在 runtime/interaction.hpp(骨架拆解批四:审批四态从 loop.hpp
    // 归位 runtime,与 InteractionBroker 的那套合成一份,镜像层拆掉)。
    // P4:ApprovalRequest 带 tool_use_id,远端前端凭它把审批事件钉回条目。
    std::function<std::shared_ptr<runtime::InteractionFuture>(const runtime::ApprovalRequest& request)>
        on_tool_confirm_async;

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
    std::function<runtime::ToolHookDecision(const std::string& tool_use_id, const std::string& name,
                                            const nlohmann::json& input)>
        on_pre_tool_use_hook;

    // hooks 框架:PermissionRequest。只有宿主本来要问用户确认时才触发
    // (RunOneTool 在调 on_tool_confirm 前把这个相位交给确认回调那一层,
    // 由它判断"真要弹确认"再发射)。deny -> 拒绝执行;allow -> 不弹确认;
    // 不表态 -> 正常问用户。
    std::function<runtime::ToolHookDecision(const std::string& tool_use_id, const std::string& name,
                                            const nlohmann::json& input)>
        on_permission_request;

    // hooks 框架:UI 工具状态机的相位通报。没配 hooks 的会话不设这个回调,
    // 展示行为与从前逐字节一致。
    std::function<void(const std::string& tool_use_id, const std::string& name, runtime::ToolPhase phase)>
        on_tool_phase;

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

    // 补偿关系查询(逐枚追踪单第四期):工具 execute 之后、finished 栅栏
    // 发射之前问一句"这枚补偿哪枚 execution"。undo/补偿类工具由装配层
    // 把 last_compensates 报上来,关系边随 finished 落账(单子:"补偿
    // 失败不得回头覆盖原调用",原调用的账不动,只在这里加边)。不设 =
    // 没有补偿类工具,行为不变。
    std::function<std::string(const std::string& execution_id, const std::string& tool_name)> on_tool_compensates;

    // ---- Plan 模式（只读研究硬闸单）:ModePolicy 硬闸 ------------------------
    // RunOneTool 在 deferred/tool_search 可见性之后、PreToolUse Hook 之前
    // 调它（单子"调用次序":registry find -> deferred visibility -> ModePolicy
    // -> PreToolUse Hook -> schema -> permission -> execute）。返回空串 =
    // 放行（含 Default 模式）;非空 = 稳定拒绝码，工具不执行、Hook 不跑、
    // 确认不问，tool_result 带"Plan 模式禁止"语义，终态 ModeDenied。
    // 拒绝不冒充"工具没加载"也不冒充"用户拒绝"——装配层给稳定
    // mode.denied 细码。不设 = 没装 Plan 闸（单测/子代理旧路），行为不变。
    // 入参是工具名 + 原始 input（装配层凭注册表查 capability;run_command
    // 的 shell 细判、agent 的角色细判都在装配层的这枚回调里做）。
    std::function<std::string(const std::string& tool_name, const nlohmann::json& input)> on_mode_policy;

    // ---- 逐枚追踪:消息落盘次序的三个关口(单子"消息落盘次序要改") ------
    // 1. assistant 消息组装完、刚入 history:装配层 append+flush 进 session。
    //    不设 = 老路(整轮收口后 PersistNewMessages),行为不变。
    std::function<void(const api::Message&)> on_assistant_message_ready;
    // 2. 本批五枚 tool result 全收齐、合并的 user 消息刚入 history:装配层
    //    append+flush user 消息,再为每枚写 result_committed 栅栏。
    std::function<void(const std::string& batch_id, const api::Message& tool_result_message)> on_tool_results_committed;

    // ---- 回合视觉收束(终端回合视觉收束单):step/batch 边界 --------------
    // 眼下回调只报单枚工具 start/done,批次边界走丢了——同一条 assistant
    // message 吐三枚 tool_use,界面看不出它们同属一拍。这三枚回调补边界:
    //   - on_model_step_started:每次模型请求发出之前(循环顶)触发,
    //     step_index 是 0 起的步号;整轮 Run() 第一次发请求必发一枚;
    //   - on_tool_batch_started:拿到完整 assistant message、遍历
    //     ToolUseBlock 之前触发。batch_index 是本 Run() 内的批次序号(0 起,
    //     每个含工具的 step 一枚),ordered_tool_use_ids 是模型给的顺序;
    //   - on_tool_batch_finished:遍历完(含 ESC 中断时"未执行"的补账)后
    //     触发,interrupted 为真表示这批没收完就被打断。
    // 没工具的 step 不发空 batch(单子:没有工具不发空 batch)。不设这些
    // 回调行为与从前逐字节一致;工具执行语义(串行、确认、hooks)不动。
    std::function<void(int step_index)> on_model_step_started;
    std::function<void(int step_index, int batch_index, const std::vector<std::string>& ordered_tool_use_ids)>
        on_tool_batch_started;
    std::function<void(int batch_index, bool interrupted)> on_tool_batch_finished;
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

class Agent;

// 无状态的轮次推进器。Agent 握身份、模型、提示、工具与历史；Loop 只把
// 一轮从“发请求”推到“工具收口”，不代表另一种代理。
class AgentLoop {
public:
    static std::expected<RunOutcome, std::string> Run(Agent& agent, api::Message user_message,
                                                       const Callbacks& callbacks,
                                                       const std::atomic<bool>* cancel = nullptr);
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

// 装配并行的回调笔(骨架拆解批三,原语自 runtime 下沉 engine):把 events
// 的显示回调并进 target——每个出水口先走 events(事件流,canonical 账)再
// 走 target,次序稳定;任一侧缺省就只走另一侧。只并 void 出水口(正文/
// 思考增量、工具起止、服务端内置工具、usage);确认/钩子/权限/拦截查询
// 这些控制口有返回值、不是出水口,不并——它们仍归各装配点自己配。
// runtime/turn_event_adapter.hpp 的同名函数是这枚的转发(老消费方不断链)。
inline void ComposeDisplayCallbacks(Callbacks& target, const Callbacks& events) {
    const auto compose = [](auto& target_fn, const auto& events_fn) {
        if (!events_fn) {
            return;
        }
        if (!target_fn) {
            target_fn = events_fn;
            return;
        }
        target_fn = [front = events_fn, back = std::move(target_fn)](auto&&... args) -> void {
            front(args...);
            back(std::forward<decltype(args)>(args)...);
        };
    };
    compose(target.on_text_delta, events.on_text_delta);
    compose(target.on_thinking_delta, events.on_thinking_delta);
    compose(target.on_tool_start, events.on_tool_start);
    compose(target.on_tool_done, events.on_tool_done);
    compose(target.on_builtin_tool_start, events.on_builtin_tool_start);
    compose(target.on_builtin_tool_done, events.on_builtin_tool_done);
    compose(target.on_usage, events.on_usage);
}

}  // namespace lubancode::agent
