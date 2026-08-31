// agent 轮次推进器(骨架拆解批四:Agent 自立门户去了 agent/agent.hpp,
// 这里只剩 loop 的家当):user 消息入历史 -> 带工具发请求 -> 流式转发给
// 上层(打字机输出)同时喂给 assembler 攒消息 -> stop_reason 是 tool_use
// 就把模型要的工具都执行一遍、结果攒成一条 user 消息喂回去 -> 再发请求,
// 如此往复,直到 end_turn,或者达到步数上限。
//
// 住这头的:TurnWiring(一轮的控制接线 + 显示出水口)、RunOutcome/
// OutputBudgetReport(收口判据)、AgentLoop、RunOneTool(工具执行的完整
// 链)。审批四态与钩子表态在 runtime/interaction.hpp;ToolTraceContext 在
// agent/tool_trace.hpp;ContextPressure 在 agent/context.hpp;Agent/
// AgentProfile/AgentWiring 在 agent/agent.hpp。
//
// 骨架拆解批二余款:Callbacks 肥结构(25 枚 std::function)退役。显示出
// 水从此只有一只口——TurnWiring::events(TurnEventAdapter),正文/思考
// 增量、工具起止、usage、step/批次边界全翻成 ServerEvent 流,sink 侧多路
// 消费(终端画屏、会话事件链、app-server 各挂各的);控制口(审批/钩子/
// 权限/Plan 闸/逐枚追踪)有返回值、是"引擎问宿主"的问话,不是出水,收
// 在 TurnWiring 的控制字段里。

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

#include "accounting/purpose.hpp"  // RequestPurpose(Token 账本单 A1:model.request.prepared 的 purpose)
#include "agent/prompt_manifest.hpp"  // PromptManifest(Token 账本单 A1:request_snapshot_ref 的地基)
#include "agent/tool_trace.hpp"
#include "api/types.hpp"
#include "runtime/interaction.hpp"
#include "runtime/turn_event_adapter.hpp"
#include "tools/tool.hpp"
#include "tools/registry.hpp"

#include "agent/model_image_store.hpp"  // ModelImageLanding:on_model_image 的回执形状
#include "agent/permission_mode.hpp"  // AgentPermissionMode:on_tool_confirm_floored 的下限档

namespace lubancode::agent {

// OnRequestPrepared 随请求一并递的账(Token 账本单 A1)。purpose 是唯一
// 恒有效的字段(AgentProfile.purpose 总有默认值,§6.2"调用方不知道用途
// 便拒绝"在真实运行时路径上不会发生——没显式设时的默认就是 MainTurn,
// 不是"不知道");manifest/前缀四项是否可用各自看 has_prompt_manifest /
// has_prefix_account,没接 ResolvedPromptBuilder 或前缀账不可用时如实
// 置 false,不拿默认值冒充。
struct RequestPreparedContext {
    accounting::RequestPurpose purpose = accounting::RequestPurpose::MainTurn;
    bool has_prompt_manifest = false;
    PromptManifest prompt_manifest;  // has_prompt_manifest=false 时内容无意义
    bool has_prefix_account = false;
    std::string system_hash;   // agent/prefix.hpp 的指纹 hash(FNV-1a,诊断口径同源)
    std::string tools_hash;
    int cache_epoch = 0;
    bool prefix_append_only = true;
};

// 轮次边界的轨迹记录口(P0-2 轨迹接线:AgentLoop 接 input/request/output
// 边界)。AgentLoop 只在模型请求/输出这些边界上问宿主;落盘与状态机校验
// 全在实现侧(TrajectoryRecorder),loop 不碰文件。语义:
//   OnRequestPrepared —— 请求拼好、即将上 wire。返回 request_id;空串 =
//                        账写不住,loop 本步明败不发模型(§7.4"request
//                        prepared 记不住,不发模型")。ctx 带 purpose/
//                        manifest/前缀账(Token 账本单 A1)。
//   OnRequestSent     —— prepared 落稳后随发随记(prepared_event_id 由实现
//                        侧在 OnRequestPrepared 里落好,这里只报发出去)。
//   OnUsageRecorded   —— v2 usage owner(一 request attempt 一 owner;wire
//                        见没见过 usage 帧如实报)。cache_epoch/
//                        prefix_append_only 随本步前缀账一并交(0/true 起点
//                        表示当步没有前缀账可交,与"epoch=0"没有歧义——
//                        真实 epoch 从 1 起,recorder 侧把 0 当"未知不落")。
//   OnOutput*         —— 收口三态:completed(带规范消息与 stop_reason)、
//                        failed、cancelled。
// 不设(空指针)= 会话没接轨迹(flag 关的老路),一处不调,行为与从前
// 逐字节一致。
class LoopBoundaryRecorder {
public:
    virtual ~LoopBoundaryRecorder() = default;
    virtual std::string OnRequestPrepared(const api::Request& request, const RequestPreparedContext& ctx) = 0;
    virtual void OnRequestSent(const std::string& request_id) = 0;
    virtual void OnUsageRecorded(const std::string& request_id, const api::Usage& usage,
                                 bool reported_by_provider, const std::string& provider_response_id,
                                 int cache_epoch = 0, bool prefix_append_only = true) = 0;
    // 返回 false = 输出事实没写稳,loop 不执行工具(§7.4"model output
    // 记不住,不执行工具"),本步明败。
    virtual bool OnOutputCompleted(const std::string& request_id, const api::Message& assistant,
                                   const std::string& stop_reason,
                                   const std::string& provider_response_id) = 0;
    virtual void OnOutputFailed(const std::string& request_id, const std::string& reason) = 0;
    virtual void OnOutputCancelled(const std::string& request_id) = 0;
};

// 一轮的引擎接线(骨架拆解批二余款:Callbacks 肥结构退役)。控制半在这
// 里:每一枚都是"引擎问宿主、宿主答话"的关口——有返回值,或有落账副作
// 用;显示出水不在这些口上(events 一只口管完)。
struct TurnWiring {
    // 显示系统剥离单 P4(补稳定 id):工具生命周期的问话首参一律带
    // tool_use_id(模型给的 ToolUseBlock.id;PTC stub 调用是宿主合成的
    // "ptc-N")。装配层凭它路由条目,不再靠"当前主/子条目"的下标猜——
    // 多 thread、并行 turn 一来,隐式槽位会串账。

    // ---- 审批(合同形状在 runtime/interaction.hpp,与 InteractionBroker
    // 同源;宿主把"怎么问、怎么答"从那只口子递进来)------------------------

    // 异步审批通道(P2 主路):工具 needs_confirm 且档位真要问用户时,把
    // "问"变成"发请求、拿 future"——回调立即返回 future,RunOneTool 在
    // 原地 Wait(工作线程阻塞等,事件泵/连接线程不跟着堵)。终端前端的实
    // 现是当场问完(同步短路,行为与老同步路一字不差);远端前端
    // (app-server/Web/Tauri)的实现登记 request_id 悬起,前端从任何线程
    // ResolveApproval 回答——审批不再钉死在 stdin 上。设了它,下面的同步
    // 口不会被调。
    std::function<std::shared_ptr<runtime::InteractionFuture>(const runtime::ApprovalRequest& request)>
        on_tool_confirm_async;

    // 同步审批回落路:与 async 同一个触发点,当场问、当场答。子代理/PTC
    // 转发与单测走这条(任务线程不吃 future);两头都设时 async 优先。
    std::function<bool(const std::string& tool_use_id, const std::string& name,
                       const nlohmann::json& input)> on_tool_confirm;

    // 拒绝后给模型的 tool_result 文案从这里取;不设用缺省"用户拒绝执行
    // 该工具"。给后台子代理用——它没人可问,拒绝的原因是"后台无法弹确
    // 认、未预放行",不是用户拒绝;子代理照缺省文案汇报,最终报告就会写
    // 成"均被用户拒绝",误导派工的主模型(后台代理权限拒绝无告知单,
    // 2026-08-17)。与审批口同线程先后调用,回调层可以拿同一份局部状态区
    // 分拒绝原因。
    std::function<std::string(const std::string& tool_use_id, const std::string& name)> on_tool_denial_text;

    // 权限收窄执法(自定义 Agent 单·阶段 5,Workflow 侧接线):带档位下限
    // 的确认口。Workflow agent 节点派的自定义 Agent 定义比会话档严时
    //(父 yolo 子 confirm),AgentExecutor 用它把 on_tool_confirm 包一层
    // ——宿主在里头把会话档向下并到下限再裁定,该问就真把确认拉回。
    // 与 agent 工具路的 AgentTool::Hooks::on_tool_confirm_floored 同一
    // 先例(0.26.96)。空 = 宿主没接(旧装配),原样走 on_tool_confirm,
    // 行为不变。
    std::function<bool(const std::string& tool_use_id, const std::string& name,
                       const nlohmann::json& input, AgentPermissionMode floor)>
        on_tool_confirm_floored;

    // ---- 钩子表态(HookDispatcher 的归并决策,发射本体在 runtime 层)------

    // 旧口(hooks 框架第二步,兼容保留):返回非空 = deny,值是 tool_result
    // 里的说明文本。新代码用下面的 on_pre_tool_use_hook;两个都设时新口
    // 优先。
    std::function<std::optional<std::string>(const std::string& tool_use_id, const std::string& name,
                                             const nlohmann::json& input)> on_pre_tool_hook;

    // PreToolUse 的完整表态。deny -> 工具不执行(确认也不问);ask ->
    // 即使确认档本来放行,也要问用户;allow -> 跳过用户确认,但
    // deny_commands/权限规则照走(在确认回调里,不许钩子越权);updatedInput
    // 只与 allow 同返,RunOneTool 会先过一遍工具 schema,改写打回即拦。
    std::function<runtime::ToolHookDecision(const std::string& tool_use_id, const std::string& name,
                                            const nlohmann::json& input)>
        on_pre_tool_use_hook;

    // PermissionRequest。只有宿主本来要问用户确认时才触发(确认那一层判
    // "真要弹确认"再发射)。deny -> 拒绝执行;allow -> 不弹确认;不表态 ->
    // 正常问用户。
    std::function<runtime::ToolHookDecision(const std::string& tool_use_id, const std::string& name,
                                            const nlohmann::json& input)> on_permission_request;

    // 工具状态机的相位通报(hooks 框架:checking_hook/waiting_permission/
    // running/blocked)。控制面的状态机——装配层(终端)凭它给条目挂
    // "钩子检查中/被拦"的态,不是显示出水。没配 hooks 的会话不设,行为
    // 与从前逐字节一致。
    std::function<void(const std::string& tool_use_id, const std::string& name, runtime::ToolPhase phase)>
        on_tool_phase;

    // 旧口(hooks.post_tool):工具真的执行完了(拿到 Result)才调用一次;
    // 不影响返回给模型的结果,单纯给上层一个"跑一下 post_tool 命令"的机
    // 会(子代理终端用它回写子条目终态)。不设就跳过。
    std::function<void(const std::string& tool_use_id, const std::string& name, const nlohmann::json& input,
                       const tools::Tool::Result& result)>
        on_post_tool_hook;

    // PostToolUse 的完整版。在工具结果清洗成合法 UTF-8 之后触发(旧口吃
    // 原始结果),返回的每段文本追加进模型所见的 tool_result(副作用已经
    // 发生,不能撤销,只许追加反馈;原始结果照旧进审计账)。不设就跳过。
    std::function<std::vector<std::string>(const std::string& tool_use_id, const std::string& name,
                                           const nlohmann::json& input, const tools::Tool::Result& result)>
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

    // ---- 模型输出图片(ccmoon 真机巡检单 P0)--------------------------------
    // 引擎问宿主的落盘口:ImageOutput(base64)进来,宿主解码、验身、原子
    // 落进会话图片目录,还 ModelImageLanding(引用块 + 可打开的绝对路径)。
    // 失败回 error,引擎把回合明败——绝不吞图冒充成功。没设这枚(宿主没
    // 开会话/单发路)而图片真来了:同样明败,口径与落盘失败一致。
    // 同一 item id 只落一回(重复终帧去重),引擎侧管,宿主不用管。
    std::function<std::expected<ModelImageLanding, std::string>(const api::ImageOutput&)> on_model_image;

    // ---- MCP 富结果单 P0.5:工具二进制 artifact 的落盘目录 -----------------
    // 本轮工具调用共用的会话 artifact 目录(<会话目录>/mcp-artifacts)。
    // RunOneTool 把它递进 ToolExecutionContext::artifact_dir;MCP 工具返回
    // 的图片/音频/blob 字节先落这里,history 只留 ArtifactRef。空 = 本轮
    // 没开落盘地(单测/单发路),富二进制块按稳定错误收口,文本不受影响。
    std::string tool_artifact_dir;

    // ---- Plan 模式(只读研究硬闸单):ModePolicy 硬闸 ------------------------
    // RunOneTool 在 deferred/tool_search 可见性之后、PreToolUse Hook 之前
    // 调它(单子"调用次序":registry find -> deferred visibility -> ModePolicy
    // -> PreToolUse Hook -> schema -> permission -> execute)。返回空串 =
    // 放行(含 Default 模式);非空 = 稳定拒绝码,工具不执行、Hook 不跑、
    // 确认不问,tool_result 带"Plan 模式禁止"语义,终态 ModeDenied。
    // 拒绝不冒充"工具没加载"也不冒充"用户拒绝"——装配层给稳定
    // mode.denied 细码。不设 = 没装 Plan 闸(单测/子代理旧路),行为不变。
    // 入参是工具名 + 原始 input(装配层凭注册表查 capability;run_command
    // 的 shell 细判、agent 的角色细判都在装配层的这枚回调里做)。
    std::function<std::string(const std::string& tool_name, const nlohmann::json& input)> on_mode_policy;

    // ---- 写前作用域闸(AGENTS.md 作用域单 P0) ------------------------------
    // 紧跟 ModePolicy 之后、PreToolUse Hook 与用户确认之前调用——比打开
    // 写句柄、建目录、写临时文件都早(单子 §7.3)。返回空串/nullopt = 放行;
    // 非空 = 拦截文案(instructions_required):该目标的 instruction chain
    // 本 Agent 尚未确认,完整规则已拼进文案,随 tool_result 进下一份请求,
    // 模型读后原样重试即放行——第一次拦住是协议握手,不是错误。终态
    // ScopeGatePending,不冒充"用户拒绝"也不冒充工具失败。回调内部自持
    // 确认账(InstructionScopeState),装配层负责"谁在调"就绑谁的账。
    // 不设 = 没装闸(单测/旧装配),行为与从前一字不差。
    std::function<std::optional<std::string>(const std::string& tool_name, const nlohmann::json& input)>
        on_scope_gate;

    // ---- 逐枚追踪:消息落盘次序的三个关口(单子"消息落盘次序要改") ------
    // 1. assistant 消息组装完、刚入 history:装配层 append+flush 进 session。
    //    不设 = 老路(整轮收口后 PersistNewMessages),行为不变。
    std::function<void(const api::Message&)> on_assistant_message_ready;
    // 2. 本批五枚 tool result 全收齐、合并的 user 消息刚入 history:装配层
    //    append+flush user 消息,再为每枚写 result_committed 栅栏。
    std::function<void(const std::string& batch_id, const api::Message& tool_result_message)> on_tool_results_committed;

    // ---- 显示出水口(唯一):事件流适配器 -----------------------------------
    // 正文/思考增量、工具起止(含服务端内置)、usage、step/批次边界,全
    // 从这只出;终端画屏、会话事件链、app-server 都在 sink 侧吃同一份流。
    // 空 = 静默轮(后台子代理没接宿主流时、纯控制面单测),显示零出水,
    // 行为与老路"显示回调全不设"一致。
    runtime::TurnEventAdapter* events = nullptr;
    // 从路标记(PTC 的 stub 调用):事件照发,但 payload 带 subordinate
    // 标——画屏侧跳过(终端照旧只画一张外层卡),账面侧照收。子代理不走
    // 这枚(它有自己的从路适配器,见 agent_tool 的装配)。
    bool subordinate_stream = false;

    // ---- 轨迹边界口(P0-2)---------------------------------------------------
    // 模型请求/输出边界的轨迹记录(见 LoopBoundaryRecorder)。空 = 没接,
    // 行为与从前一字不差。不持有,调用方保证存活到本轮收口。
    LoopBoundaryRecorder* boundary_recorder = nullptr;
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
// hit_time_budget/hit_token_budget 同款(真机实测 P2-6 成本刹车):墙钟或
// 累计 token 硬线断的,分型时写明是哪根线,部分结果照常带走。
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
    // 成本硬线(步顶查,断线即收场):时间与 token 两根;步数那根是上面的
    // hit_step_limit。三根都只在对应上限 >0 时可能置位。
    bool hit_time_budget = false;
    bool hit_token_budget = false;
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

// 预算软线催办(真机实测 P2-1/P2-6)的正文:跨过软线那一步注入,要求模型
// 基于现有证据收尾——开新调查方向就别想了,把已查到的写下来。
std::string BuildBudgetSoftNudgeText();

// 纯函数,可单测:三根硬线(步数/累计 token/墙钟毫秒)各与已用量比对,
// 任一跨过软线(BudgetSoftLine 派生,percent 0 = 该线不催)即真。Run 循环
// 里只催一次:调用方自持 nudged 旗,本函数只管"此刻过没过线"。
bool CrossesBudgetSoftLine(int steps_used, int max_steps_per_turn, std::int64_t tokens_seen,
                           std::int64_t max_total_tokens, std::int64_t elapsed_ms, std::int64_t max_wall_ms,
                           int soft_percent);

class Agent;

// 无状态的轮次推进器。Agent 握身份、模型、提示、工具与历史；Loop 只把
// 一轮从“发请求”推到“工具收口”，不代表另一种代理。
class AgentLoop {
public:
    static std::expected<RunOutcome, std::string> Run(Agent& agent, api::Message user_message,
                                                       const TurnWiring& wiring,
                                                       const std::atomic<bool>* cancel = nullptr);
};

// 执行一枚工具调用的完整链(公开导出;实现在 loop.cpp 顶部,注释在那头):
// 找工具/延迟挂载谓词 -> PreToolUse(含 updatedInput 的 schema 复检) ->
// 确认档(needs_confirm + PermissionRequest)-> 执行 -> PostToolUse -> 编码
// 清洗 -> 工具终态上事件流。JSON 后端的工具循环与 PTC 的每一枚 stub 调用共用
// 这一条路,不许有第二条绕过 hooks/权限的暗门。
// filter_denial:过滤谓词不放行时的说明文案。默认空 = tool_search 的
// "尚未挂载"说法;Explore 这类角色限制的调用方另给一句写明"角色限制"
// 的文案——限制须来自角色并看得见,不许含糊成"子代理无权限"(规格)。
// trace:逐枚追踪的执行上下文;nullptr = 不追踪(旧行为)。
// cancel:本次调用的取消旗(子代理 x 停止失效单:贯通到工具进程)。经
// ToolExecutionContext 递进 execute;肯合作取消的工具(run_command 收进程
// 树、Lua 掐指令钩子)置位即收,不肯的照旧等它跑完——不硬杀线程。null =
// 没有取消源(旧调用方),行为与从前一字不差。
tools::Tool::Result RunOneTool(tools::ToolRegistry& registry, const api::ToolUseBlock& call, const TurnWiring& wiring,
                                const std::function<bool(const tools::Tool&)>& tool_filter,
                                const std::string& filter_denial = std::string(),
                                const ToolTraceContext* trace = nullptr,
                                const std::atomic<bool>* cancel = nullptr);

}  // namespace lubancode::agent
