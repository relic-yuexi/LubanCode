// TaskLedger(骨架拆解批三·病十四:AgentTool 六职拆分之台账件)。
// 子代理任务的统一台账:TaskRecord(快照/inbox/消息账/活度账/墙钟三信号)
// 加全套查询与收账口(面板快照、查看态事件流、定向介入、取消、完成通知、
// 权限拒绝通知)。从前这些长在 AgentTool 身上,拆出后 AgentTool 只留工具
// 门面——execute 的入参校验与前后台分岔。
//
// 消息账(规格"现场三"):每只任务一份独立的、按时间追加的事件流,事件
// 类型与 main 的 transcript 对齐;查看态复用 main 的渲染组件,不手搓第二套。
// 写点全在任务线程(loop 回调 + RunTask 循环),读取(面板/查看态)在主
// 线程——都拿下面的 mutex,天然按发生次序。
#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <expected>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include <nlohmann/json.hpp>

#include "agent/agent_progress.hpp"  // AgentProgressClock/AgentHealthState:P0-0 进展合同
#include "agent/loop.hpp"  // ToolTraceEvent 等转发类型(与拆分前同一份引用)
#include "agent/task_spec.hpp"  // AgentTaskSpec v2 canonical task contract
#include "agent/turn_budget.hpp"  // TurnBudgetAccount:任务级 turn 预算唯一真账(turn 预算单)
#include "api/types.hpp"
#include "tools/subagent_scheduler.hpp"  // SubagentGovernance/AgentLedgerStats:P0-2 纯 admission

namespace lubancode::tools {

class AgentTool;

// 墙钟兜底的默认收杀宽限:看门狗发出停止信号后,任务线程这么久还没报终态
// (后端不理取消的绝境),就把台账强制翻成终态。上限本体的默认值
// (kDefaultSubagentWallClockTimeoutSecs)在 config.hpp,跟 subagent.* 那批
// 默认住一起;宽限是实现细节,住这边。
constexpr int kDefaultSubagentWallClockGraceSecs = 30;

// 运行态扩充(递归派工单 §八):WaitingChildren = 自己已写完、正等孩子;
// Completing = 活孩子清零、正在收口。两者都算"活"——占并发槽、可传话、
// 可取消;终态四枚一字不动。
enum class AgentTaskState { Running, WaitingChildren, Completing, Done, Failed, Cancelled, BudgetExhausted };

// 活态判定:所有"还占着资源、还可能翻终态"的状态。台账里凡是先前拿
// `state == Running` 当"还在跑"的口子,一律换这一枚——WaitingChildren 的
// 父任务不许被当成已收尾(否则 attached 孩子成孤儿)。
inline bool IsAliveTaskState(AgentTaskState state) {
    return state == AgentTaskState::Running || state == AgentTaskState::WaitingChildren ||
           state == AgentTaskState::Completing;
}

// 完成结果的送达去处(递归派工单 §7.1):前台结果走当前工具调用直接回
// 调用者(台账仍记 parent 便于对账);后台结果要么进 main 回合上下文,
// 要么进直接父任务的 mailbox——绝不跨级飞 main。
enum class TaskDeliveryTarget {
    ForegroundCaller,   // 前台:作为当前 agent 工具调用的 Tool::Result 回调用者
    MainTurnContext,    // 后台根任务:main 的下一轮请求里送达(DrainCompletionNotices)
    ParentTaskInbox,    // 后台嵌套:直接父任务的 mailbox(continuation 取件)
};

// ---------------------------------------------------------------------------
// 子代理的结构化结果(规格"现场三"):子代理不得只交 text + is_error。
// 预算耗尽(budget_exhausted)与失败(failed)分家;部分结果必须带回——
// 四十轮探索不许一笔勾销。
// ---------------------------------------------------------------------------

// 收场状态:completed = 正常交了结论;failed = 没交结论/出错;stopped =
// 用户中止;budget_exhausted = 步数预算用满(检查点/部分结果仍在)。
enum class TaskOutcomeStatus { Completed, Failed, Stopped, BudgetExhausted };

// 短因:面板与通知只放短因,完整错误进 transcript。
enum class TaskOutcomeReason {
    None,              // 无(还没收场/正常完成)
    ApiError,          // 接口报错(请求失败/流中断)
    StepLimitExhausted,  // 步数预算耗尽(旧名 MaxTurns,口径已改按 step 记)
    TimeBudgetExhausted,   // 时间预算耗尽(成本硬线,真机实测 P2-6)
    TokenBudgetExhausted,  // token 预算耗尽(成本硬线,真机实测 P2-6)
    OutputBudgetExhausted,  // 输出预算耗尽(max_tokens;续跑用完仍无正文,规格根因四)
    MaxContext,        // 上下文装不下
    NoFinalText,   // 最后一轮没有文本结论
    ToolError,     // 最后一次工具调用出错带崩了收尾
    UserStop,      // 用户中止
    WallClockTimeout,  // 整轮墙钟上限兜底(subagent.wall_clock_timeout_secs)
    ProtocolError, // 会话协议异常(连历史都没有等)
    // ---- 收场原因补齐(递归派工单 §8.3):注册后才会撞上的收场细分 ----
    InitializationFailed,  // 注册后的 backend/registry/worker 构造失败(Failed)
    ParentCancelled,       // 父任务取消引发的级联(子记 Cancelled,不冒充 UserStop)
    SessionClosing,        // 会话收场向下取消整树(Cancelled)
    HookDenied,            // hook 拒绝派工(Failed;拒绝发生在注册前则不造 TaskOutcome)
    ShutdownTimeoutUnknown,  // 停机超时未证实结束(映射 Failed,不冒充已收净)
    // ---- 任务级 turn 预算(turn 预算单 §8.1):新枚举,只服务新账 ----
    // 旧 StepLimitExhausted 留给 legacy per-run step 路,兼容窗内不混写。
    TurnLimitExhausted,    // 任务总 turn 预算用满(初始/续投/孩子回流/Stop 钩子共账)
    NoMeaningfulProgress,  // 连续完整轮指纹不变(监督器 §7.1 尺子;Failed,带部分结果)
};

// ---------------------------------------------------------------------------
// 运行中任务的实时活跃账(规格"子代理活跃度不可见"):思考/正文/工具/首字节
// 四个阶段,坞行与查看态据此换文案。计数只进字节数与阶段名,思考与正文
// 本身绝不进 dock 行。
// ---------------------------------------------------------------------------
struct AgentTaskActivity {
    enum class Stage { None, WaitingFirstByte, Thinking, Text, Tool };
    Stage stage = Stage::None;
    std::size_t reasoning_bytes = 0;
    std::size_t text_bytes = 0;
    // 显示用的码点数(汉字一眼读得出"多少字"):快照拷贝时从 pending 串现算,
    // 不在增量回调里逐片数(增量可能劈开多字节字符)。
    int reasoning_chars = 0;
    int text_chars = 0;
    // 当前请求发出时刻与首事件耗时(首字节):等首字节阶段显示"等首字节 · Ns",
    // 到了以后查看态统计行显示"首字节 Nms"。first_byte_ms < 0 = 还没来。
    std::chrono::steady_clock::time_point request_started{};
    int first_byte_ms = -1;
    // 工具执行中:工具名与起跑时刻(坞行显示"工具 read_file · 3s")。
    std::string tool_name;
    std::chrono::steady_clock::time_point tool_started{};
    // 最后一次工具调用的名字(真机实测 P2-1:Dock 要求"最后一次工具"常驻
    // 可见,不只工具跑着的那几秒)。工具收口不清,下一枚工具发起时覆盖;
    // 终态清空(整个 activity 一起换)。
    std::string last_tool_name;
};

struct TaskOutcome {
    TaskOutcomeStatus status = TaskOutcomeStatus::Failed;
    TaskOutcomeReason reason = TaskOutcomeReason::None;
    std::string message;         // 给人看的短说明(一两句)
    std::string partial_result;  // 已取得的事实或最后检查点(尽力保住)
    std::string stop_reason;     // 模型原始 stop reason(空 = 一个字都没回来)
    std::string last_tool;       // 最后一次工具名与结果摘要
    int steps_used = 0;
    int step_limit = 0;          // 0 = 不限步(一个 turn 内的模型请求数上限;legacy 投影)
    // ---- 任务级 turn 账(turn 预算单 §8.2):attempted/completed 分账 ----
    // turn_limit = 派出时冻结的任务总 turn 上限(0 = 不限);attempted 是
    // 宿主准入并发出过的逻辑模型请求(API 错/流断也保留),completed 只数
    // 完整收回 assistant 的那部分。展示一律拿 attempted 当消费数,不拿
    // completed 冒充。steps_used/step_limit 是兼容窗里的 legacy 投影,两套
    // 不混写成同一根"生效硬线"。
    int turn_limit = 0;
    int turns_reserved = 0;      // 正常终态必须归零(诊断口)
    int turns_attempted = 0;
    int turns_completed = 0;
    // 成本预算账(真机实测 P2-6):派出时的时间/token 硬线,0 = 不设。断线
    // 时 message 写明哪根线,这里留数好对账。
    int wall_limit_secs = 0;
    std::int64_t token_limit = 0;
    // 输出预算账(规格根因四,来自 agent::OutputBudgetReport):撞墙的
    // 上限(0 = unset,墙在服务端)、续跑次数、usage 是否报告、思考检查点。
    int output_limit_tokens = 0;       // 0 = unset(请求没带字段)
    int length_continuations_used = 0;
    bool usage_reported = false;       // 服务端一次 usage 都没回过时,token 数按"未报告"说
    std::string thinking_checkpoint;   // 已收思考的字节数 + 末段摘要
    // api::Usage 统一口径:input 是"非缓存输入",完整输入 = 三项相加
    // (见 TotalInputTokens)。展示一律用完整输入,别把缓存命中漏掉。
    std::int64_t input_tokens = 0;
    std::int64_t cache_read_tokens = 0;
    std::int64_t cache_creation_tokens = 0;
    std::int64_t output_tokens = 0;
    double elapsed_seconds = 0;

    std::int64_t total_input_tokens() const {
        return input_tokens + cache_read_tokens + cache_creation_tokens;
    }
};

struct AgentTaskToolCall {
    std::string name;
    std::string input_json;
    std::string result;
    bool done = false;
    bool is_error = false;
    std::string tool_use_id;  // P4:模型给的调用 id,收账按它精确对条目
};

struct AgentTaskSnapshot {
    int id = 0;
    std::string agent_type;
    // 真正短标题(人看的语义字段,与 prompt 分家)。新建任务必填;旧任务
    // 没有就空着,显示层退"未命名子代理 #N",绝不回退到 prompt 前若干字。
    // P0-1 起 title 是 canonical spec 的读投影——真值在 spec->title,这里
    // 拷一份给旧消费方(面板/通知),两边由注册口一次写齐。
    std::string title;
    std::string prompt;  // 原始派工说明，不含宿主运行信封
    std::string effective_cwd;  // 派出后冻结；Dock 与 system 运行环境段共用
    // ---- 显式 lineage(递归派工单 §7.1):parent 只认 child 记录上的
    // parent_task_id;root/depth 注册时从父边一次算出,之后不可改。----
    int parent_task_id = 0;  // 0 = main 派出
    int root_task_id = 0;    // 根子任务自己的 id(根任务上 == 自己的 id)
    int depth = 1;           // main=0,子=1,孙=2
    // 轨迹子账 run id(递归派工单 P1-2 嵌套轨迹边):这只任务自己的
    // subagents/<agent_run_id>.jsonl 开张后回填(RunTask 里,trajectory
    // 桥申请成功即写);空 = 没开轨迹账(flag 关/开张失败/旧调用方)。
    // IdentityOfSnapshot 把它投进 AgentRunIdentity::agent_run_id——这只
    // 任务派孩子时,子账的 relations.parent_run_id 认它,不冒充 main。
    std::string agent_run_id;
    TaskDeliveryTarget delivery_target = TaskDeliveryTarget::MainTurnContext;
    // canonical 活执行合同：title + instructions。空只供旧调用方直建快照。
    std::shared_ptr<const agent::AgentTaskSpec> spec;
    // 前台(阻塞父级调用)还是后台(独立线程)。详情可看,列表不必铺。
    bool foreground = false;
    AgentTaskState state = AgentTaskState::Running;
    // 停止信号已发、还没收口(快照拷出时从 TaskRecord::cancel 现读;运行中
    // 才有意义)。面板"停止中"回执用,与 AgentTaskSummary::stop_requested
    // 同一根旗。
    bool stop_requested = false;
    // 派出时写死的预算(0 = 不限步):面板可见,不等撞墙才揭晓(规格"现场四")。
    int step_limit = 0;
    int steps_used = 0;  // 已发生的模型请求数(RunOutcome 直接记账,不靠 usage 回调猜)
    // ---- 任务级 turn 账(turn 预算单 §6.1):唯一真账在 TaskRecord 的
    // turn_account,这几枚是快照拷出时的投影(运行中随时读活账)。----
    int turn_limit = 0;
    int turns_reserved = 0;
    int turns_attempted = 0;
    int turns_completed = 0;
    // 派出时的时间/token 成本预算(真机实测 P2-6;0 = 不设)。与 step_limit
    // 同一规矩:预算进快照,坞行与详情看得见,超了有短因。
    int wall_limit_secs = 0;
    std::int64_t token_limit = 0;
    // ---- 隔离房基线账(派工单 §三/§五):isolation=worktree 派工即冻结 ----
    // 基线 = 派工瞬间的调用者 HEAD(不再解析 origin 默认分支);恢复、重试、
    // 嵌套派工对账都认这几枚。空 isolation_branch = 无房任务。
    std::string isolation_branch;       // worktree/agent-xxxx
    std::string isolation_base_ref;     // 冻结时调用者所在分支;detached 记 "(detached)"
    std::string isolation_base_commit;  // 冻结的调用者 HEAD 全哈希
    // api::Usage 统一口径(input=非缓存输入),完整输入 = 三项相加。
    std::int64_t input_tokens = 0;
    std::int64_t cache_read_tokens = 0;
    std::int64_t cache_creation_tokens = 0;
    std::int64_t output_tokens = 0;
    // 服务端有没有回过 usage(规格根因三):没回过且已跑过步数时,面板
    // 写"tokens 未报告",不画 0。任何一次请求带回非零 usage 即置位。
    bool usage_reported = false;
    std::chrono::steady_clock::time_point start_time{};
    std::chrono::steady_clock::time_point end_time{};
    std::vector<AgentTaskToolCall> tool_calls;
    std::string live_output;
    std::string result;
    // 实时活跃账(运行中才有意义;终态清空)与内容修订号:查看态据此判断
    // "这只任务的消息账又长了"(思考/正文增量、事件追加、阶段翻页都 +1)。
    AgentTaskActivity activity;
    std::uint64_t content_revision = 0;
    // 结构化结果:终态时由任务收尾填。运行中 status 停在 Failed/None,
    // 消费方只看 state == Running 判"还在跑"。
    TaskOutcome outcome;
    bool delivered = false;

    std::int64_t total_input_tokens() const {
        return input_tokens + cache_read_tokens + cache_creation_tokens;
    }
};

// ---------------------------------------------------------------------------
// 子代理消息账(规格"现场三"):事件类型与 main 的 transcript 对齐,查看态
// 复用 main 的 TranscriptItem/折叠规则/工具卡 renderer 渲染这份账。
// ---------------------------------------------------------------------------
enum class AgentTaskEventKind {
    UserMessage,         // 任务说明(派出时的 prompt)或续投输入
    AssistantText,       // 助手正文一段(工具/思考边界切段)
    AssistantReasoning,  // 助手思考一段
    ToolStart,           // 发起一次工具调用
    ToolResult,          // 一次工具调用的结果
    SteeringMessage,     // main/用户中途介入(轮次边界注入 inbox 的那条)
    CompactCheckpoint,   // 上下文压缩边界(历史换轨,前情进存档)
    Completion,          // 正常完成(带最终结论全文)
    Failure,             // 失败/中止/耗尽(带短因与部分结果)
};

struct AgentTaskEvent {
    AgentTaskEventKind kind = AgentTaskEventKind::UserMessage;
    std::string text;        // 正文/结论/短因/检查点说明
    std::string tool_name;   // ToolStart/ToolResult:工具名
    std::string input_json;  // ToolStart:入参紧凑 JSON
    std::string result;      // ToolResult:结果全文(截 kFullOutputCapBytes)
    bool is_error = false;   // ToolResult:是否出错
    // 流式尾巴(追加需求"查看态实时思考流"):TaskEvents 拼在账尾的
    // "正在累积、尚未切段"的正文/思考带这面旗——查看态据此画"思考中 · N 字"
    // 的 Running 条目(与 main 流式思考同款折叠),封卷事件恒 false。
    bool streaming = false;
    // 事件号(监督器单 P1-0):从台账的 watch_generation 同一枚单调计数发
    // ——与 agent_watch 顶层的 revision 同一数轴,"after_revision 之后的事件"
    // 才对得齐(各任务自己的序列仍严格递增)。显示层不认它(照旧全量拼)。
    std::uint64_t revision = 0;
};

// 轻量列表条目(0.28.x 面板全量化):列表每 100ms 刷新一次,不再复制
// tool_calls/live_output 这些大块——面板列表行只要这几个字段;详情(完整
// 任务说明、工具流水、介入消息)另走 TaskDetail(task_id) 按需取。
struct AgentTaskSummary {
    int id = 0;
    std::string agent_type;
    std::string title;  // 真正短标题;空 = 旧任务,显示层退"未命名子代理 #N"
    std::string prompt;
    // lineage 投影(P0-2):Dock 画树、通知认父子都从这几枚读,不另养账。
    int parent_task_id = 0;
    int root_task_id = 0;
    int depth = 1;
    TaskDeliveryTarget delivery_target = TaskDeliveryTarget::MainTurnContext;
    bool foreground = false;
    AgentTaskState state = AgentTaskState::Running;
    // 停止信号已发、任务线程还没报终态(Running && stop_requested = 面板行
    // 显"停止中",不是死 Running;子代理 x 停止失效单的可验证回执半边)。
    bool stop_requested = false;
    int step_limit = 0;
    int steps_used = 0;
    // 任务级 turn 账的列表投影(turn 预算单:运行中行显示 turn attempted/
    // limit,真账在 TaskRecord::turn_account)。
    int turn_limit = 0;
    int turns_attempted = 0;
    int turns_completed = 0;
    int wall_limit_secs = 0;       // 派出时的时间预算(0 = 不设;P2-6)
    std::int64_t token_limit = 0;  // 派出时的 token 预算(0 = 不设;P2-6)
    TaskOutcomeReason outcome_reason = TaskOutcomeReason::None;  // 面板短因用
    std::int64_t input_tokens = 0;
    std::int64_t cache_read_tokens = 0;
    std::int64_t cache_creation_tokens = 0;
    std::int64_t output_tokens = 0;
    bool usage_reported = false;
    std::chrono::steady_clock::time_point start_time{};
    std::chrono::steady_clock::time_point end_time{};
    std::size_t tool_call_count = 0;
    std::size_t pending_message_count = 0;  // 已排队未送达的介入消息数
    AgentTaskActivity activity;
    std::uint64_t content_revision = 0;
    // 监督投影(监督器单 P1-1):Dock 行的"重连 N/3 · 静默 XmYs"与查看态的
    // 监督折叠行从这份账现算——真账仍在 TaskRecord.progress,这里只是
    // Summaries 拷出时的投影,与 activity 同一条路。
    agent::AgentProgressClock progress;
    // 结果是否已交回主会话(DrainCompletionNotices 置位)。
    bool delivered = false;

    std::int64_t total_input_tokens() const {
        return input_tokens + cache_read_tokens + cache_creation_tokens;
    }
};

// SendTaskMessage 的收信结果。
enum class TaskMessageStatus {
    Queued,    // 已排进该任务的 inbox,轮次边界注入
    NotFound,  // 没有这个任务号(已被清理/从未存在)
    Finished,  // 任务已进终态,明确拒收——绝不改投 main
};

// 定向消息的来路。三条来源不同(main steering 仍走会话层 SteeringQueue,
// 不进 inbox):查看态 composer 与用户排队转投是 User,主模型经
// agent_message 工具转交是 MainAgent。
enum class TaskMessageSource { User, MainAgent };

// mailbox 分型(递归派工单 §9.2):ChildCompletion 带结构化头(子任务结果
// 是外来资料,给模型的投影一律带来路声明),其余三型走原有文案。
enum class TaskMailboxKind {
    UserSteering,      // 用户/查看态介入(旧 InboxItem 缺省型)
    ParentSteering,    // 父代理经 agent_message 转交(MainAgent 的 lineage 化名)
    ChildCompletion,   // 直接子任务的结构化完成结果
    HostNotice,        // 宿主通知(收场提醒等)
};

// ---------------------------------------------------------------------------
// TaskRecord:一只任务的全套账。从前是 AgentTool 的私有嵌套,拆出随台账
// 走(TraceBackend/看门狗闭包要 shared_ptr 自保)。
// ---------------------------------------------------------------------------
struct TaskRecord {
    // 定向介入的收件口。推(SendTaskMessage,主线程)与取(子代理线程的
    // inbox 轮询)都拿 inbox_mutex;终态判定+投递在台账锁内成对完成,任务
    // 收尾后不可能再排进新信。delivered 标记:轮次边界取走的那条不再重发;
    // 未送达的留给详情展示与收场报告。
    // P0-4 起 mailbox 分型:ChildCompletion 项带 child_task_id——restore
    // (父续投失败退信)时要把那只子任务的 delivered 一并退回,"读出来了
    // 不等于送达了"。
    struct InboxItem {
        std::string text;
        bool delivered = false;
        TaskMessageSource source = TaskMessageSource::User;
        TaskMailboxKind kind = TaskMailboxKind::UserSteering;
        int child_task_id = 0;  // ChildCompletion 型:来源子任务的 id
    };
    AgentTaskSnapshot snapshot;
    std::atomic<bool> cancel{false};
    mutable std::mutex inbox_mutex;
    std::deque<InboxItem> inbox;
    // 消息账:按时间追加的事件流,写入全在任务线程(loop 回调 + RunTask
    // 循环),读取(TaskEvents)在主线程——都拿台账锁,天然按发生次序。
    // pending_* 是正在累积的助手正文/思考,在事件边界(工具发起/轮次收口)
    // 切成正式事件,不让中间文字被后续工具调用淹没。
    std::vector<AgentTaskEvent> events;
    std::string pending_text;
    std::string pending_reasoning;
    // 实时活跃账:阶段/字节数/首字节/工具起跑时刻,写点在任务线程
    //(TraceBackend 与 loop 回调,持台账锁),读点随快照拷出。
    // last_activity_touch 是增量路径的节流锚(1s 一拍 Touch;阶段翻页/
    // 事件边界不受节流,立即拍)。
    AgentTaskActivity activity;
    std::uint64_t content_revision = 0;
    std::chrono::steady_clock::time_point last_activity_touch{};
    // 进展合同(P0-0):四本时钟 + 恢复账 + 监督相位/健康,与显示用的
    // activity/content_revision 分账——progress_revision 只在 meaningful
    // progress 时前进,token 流量一概不抬它。写点:TraceBackend(请求/传输)、
    // ledger_sink(工具起止/轮次)、DeliverChildCompletion(孩子交付)、
    // 恢复钩子(重试);读点:监督拍、面板、agent_watch。
    agent::AgentProgressClock progress;
    // 墙钟兜底的三枚信号:wall_stop 看门狗到点置位(合并 cancel 线程收进
    // merged cancel,绝不动 task->cancel——那会被收成"用户中止");
    // wall_clock_fired 供收场分型(墙钟超时 ≠ 用户中止);finalized 是任务
    // 线程正式收尾的标志(看门狗宽限期内据此早退)。
    std::atomic<bool> wall_stop{false};
    std::atomic<bool> wall_clock_fired{false};
    // 监督器的空转停止信号(P0-2):与 wall_stop 同一条取消链(不是用户
    // 中止),收尾按 NoMeaningfulProgress 分型。
    std::atomic<bool> no_progress_fired{false};
    std::atomic<bool> finalized{false};
    // 看门狗强制收账后置位:任务线程晚到的收尾不得再把台账翻回去
    //(状态/结果/outcome 一律保持强制收账那份)。
    bool force_finalized = false;
    // 父任务取消引发的级联(P0-2 取消树):收尾分型据此把孩子记成
    // Cancelled/ParentCancelled,不冒充它自己收到了 UserStop。
    bool cancelled_by_parent = false;
    // 墙钟看门狗线程(RunTask 起,收尾块 join):闭包另握一份 record 的
    // shared_ptr 自保——任务线程卡死的绝境下 record 不悬垂。
    std::thread watchdog;
    // 封账闸(规格第五节"排到了却没送"):子代理准备从 Running 进终态时,
    // 在台账锁里查一遍 inbox——空则置 true,此后 SendTaskMessage 明确拒收
    //(Finished);非空则不封账,取走注入、再续跑一轮。置位与入队同锁成对,
    // 不存在"刚封账又收信"的缝。
    bool inbox_closed = false;
    // ---- 任务级 turn 预算唯一真账(turn 预算单 §6.1)-----------------------
    // 预算归任务记录,不归某次 Run/UI/hook/DriveReport。注册时按快照的
    // turn_limit 冻结;此后所有 Agent::Run()(初始任务、mailbox 续投、孩子
    // 回流、Stop 钩子续跑)都从这本账走原子准入。台账锁内驱动(Unlocked
    // 系列),与"验活态"同一锁域(设计单 §6.3)。
    agent::TurnBudgetAccount turn_account;
};

// 续投交接的取件批次:SealOrContinueInbox 取走未送项时记下标与原文;续跑
// 若失败(取消/provider 错误),按 indices 把这批退回未送,收场报告照列
// 原文——不能"取走了就当送到了"。P0-4:批次里的 ChildCompletion 项带
// child_task_id,restore 时连子任务的 delivered 一起退。
struct DrainedInbox {
    std::vector<std::size_t> indices;
    std::vector<std::string> texts;
    std::vector<TaskMessageSource> sources;
    std::vector<TaskMailboxKind> kinds;  // 与 indices 按位对齐(ChildCompletion 的投递格式不同)
    std::vector<int> child_task_ids;  // 与 indices 按位对齐;非子完成项为 0
};

// ---------------------------------------------------------------------------
// 台账本体。
// ---------------------------------------------------------------------------
class TaskLedger {
public:
    // 台账锁:带 Locked 后缀的写口(AppendEventLocked/FlushPendingTextLocked/
    // FinalizeLocked 等)要求调用方先持这把锁;其余公开口自己拿。任务线程
    //(写)与主线程(读/面板)共用这一把,次序天然按发生序。
    mutable std::mutex mutex;

    // 注册一只新任务:snapshot 的 id 在这里发,start_time/cancel 派生字段
    // 由调用方先填好。返回 TaskRecord 的 shared_ptr(看门狗/任务线程闭包
    // 自持自保)。P0-2 起这是不走 admission 的旧口(测试/兼容);真派工走
    // TryRegisterChild。
    std::shared_ptr<TaskRecord> Register(AgentTaskSnapshot snapshot);

    // ---- lineage 注册事务(P0-2:一笔写事务"验父->判限额->分 id->注册
    // child")。父终态与孩子派出可能在两条线程同时撞上,"先查父再注册"若
    // 松锁必留缝,故而全在同一锁域做完。proto 需先填好 parent_task_id 与
    // delivery_target;requested_depth 由调用方按 caller.depth+1 递进,这里
    // 对账父记录。拒绝返回 nullptr 并把模型可见文案写进 error_out——不注册
    // 半条任务,不发 provider 请求。
    std::shared_ptr<TaskRecord> TryRegisterChild(AgentTaskSnapshot proto, int requested_depth,
                                                 const SubagentGovernance& governance,
                                                 std::string* error_out);

    // ---- lineage 查询(children 只是派生索引,从 parent_task_id 重建)----
    std::vector<int> ChildTaskIds(int parent_task_id) const;
    std::size_t AliveChildCount(int parent_task_id) const;
    // 一棵根树的累计节点数(终态也算,max_tree_nodes 的口径)。
    std::size_t TreeNodesCount(int root_task_id) const;
    // admission 用的锁内快照(活跃数含等孩子的父节点,单子 §7.3)。
    AgentLedgerStats StatsForAdmission(int parent_task_id, int root_task_id) const;

    // 修订号 +1(面板/查看态按它决定重画)。原子,任何线程可调。
    void Touch() { revision_.fetch_add(1, std::memory_order_release); }
    std::uint64_t revision() const { return revision_.load(std::memory_order_acquire); }

    // ---- 任务级 turn 预算的准入口(turn 预算单 §6.2/§6.3)-------------------
    // task-scoped adapter(AgentTool::RunTask 装的 ModelTurnBudgetGate)从这
    // 几枚口子进。全部在台账锁内做完"验活态 -> 验额度 -> 占额/翻账",
    // 不给两线程同时占到最后一枚留缝;任务终态后再申请稳定拒绝。
    // 返回值与人话码见 agent/turn_budget.hpp(turn_budget.exhausted 一族)。
    std::expected<agent::ModelTurnPermit, std::string> TryReserveModelTurn(const std::shared_ptr<TaskRecord>& task);
    std::expected<int, std::string> CommitModelTurnSent(const std::shared_ptr<TaskRecord>& task,
                                                        const agent::ModelTurnPermit& permit);
    bool AbortModelTurnBeforeSend(const std::shared_ptr<TaskRecord>& task, const agent::ModelTurnPermit& permit);
    // 幂等:同一 permit 重复完成不加第二次。
    bool MarkModelTurnCompleted(const std::shared_ptr<TaskRecord>& task, const agent::ModelTurnPermit& permit);
    agent::ModelTurnBudgetSnapshot ModelTurnSnapshot(const std::shared_ptr<TaskRecord>& task) const;
    // 任务级"将尽提示"认领(§9.2):一任务恰一次。
    bool ClaimModelTurnNudge(const std::shared_ptr<TaskRecord>& task);

    // ---- 查询口(面板/查看态/测试)----
    // max_entries=0 取全量;给正数时保留全部运行中任务,再从新到旧补齐
    // 最近的终态任务。
    std::vector<AgentTaskSnapshot> Snapshots(std::size_t max_entries = 0) const;
    // 轻量全量列表(运行中、完成、失败、取消都在,不截 8 只)。
    std::vector<AgentTaskSummary> Summaries() const;
    // 某只任务的详情快照;认不出返回 nullopt。
    std::optional<AgentTaskSnapshot> Detail(int task_id) const;
    // 某只任务的消息账(按时间序,查看态的会话视口用):运行中也可调,
    // 读到的是已封口事件 + 正在累积的正文/思考尾巴(各切一段带出)。
    std::vector<AgentTaskEvent> Events(int task_id) const;
    // 事件号过滤口(监督器单 P1-0,agent_watch 的 include=events 档):回
    // revision > after_event_revision 的最多 max_events 枚(从旧到新)。
    // 只拷贝结构化字段(kind/tool/is_error/streaming/revision),text/result/
    // input_json 一律不带——模型侧的 watch 输出不泄正文与工具参数。
    std::vector<AgentTaskEvent> EventsSince(int task_id, std::uint64_t after_event_revision,
                                            std::size_t max_events) const;
    // 某只任务已排队未送达的介入消息原文(详情展示/测试用)。
    std::vector<std::string> PendingMessages(int task_id) const;
    // 某只任务的进展合同快照(测试与 agent_watch 的读口;认不出返回空钟)。
    agent::AgentProgressClock ProgressOf(int task_id) const;

    // ---- agent_watch 的等待口(监督器单 P1-0)--------------------------------
    // 监督可见修订:健康/相位/进展/重试/收发消息/生命周期任一动了 +1。token
    // 级传输流量不抬它——"流在动"与"任务有变化"分账(单子 §6.1)。
    std::uint64_t watch_generation() const;
    // condition variable 睡到:监督可见修订前进 / 到 deadline / extra_wake
    // 为真(取消旗那一类,置位方须另经台账路径 notify,见 NotifyExternalWake)。
    // 回 true = 修订前进了;false = 超时或被 extra_wake 叫醒。不忙轮询:
    // 没人动账时这条线程零 CPU 挂在 cv 上(单子 §9.2)。
    bool WaitForWatchChange(std::uint64_t after_generation, std::chrono::steady_clock::time_point deadline,
                            const std::function<bool()>& extra_wake = nullptr);
    // 外部事件唤醒(单子 §9.2"用户输入、父取消、session close 提前唤醒"):
    // ESC 打断/用户排入待发消息那类不经过台账写口的动作,由会话侧在这里
    // 叫醒所有等修订的 watcher。会推一代修订(醒来方拿到的是快照,不吃亏)。
    void NotifyExternalWake();

    // ---- 介入/取消/清理 ----
    // 定向介入:非活态(终态/封账)明确拒收(不改投 main)。
    TaskMessageStatus SendMessage(int task_id, const std::string& text,
                                  TaskMessageSource source = TaskMessageSource::User);
    // 正式取消(面板 x / agent 取消口):父停则向下取消整棵子树(单子不变量
    // 5)——后代各发停止信号并记 cancelled_by_parent,收尾按
    // Cancelled/ParentCancelled 分型。只发信号,等各任务线程自己报终态。
    bool CancelTask(int task_id);
    // 停全部活任务(含向下级联),返回发了停止信号的任务数。
    int CancelAllTasks();
    // 把一条终态任务从面板/台账清掉(顺带清它的介入消息)。运行中不给清。
    bool ClearFinishedTask(int task_id);
    // 会话收场(/clear、退出)用:所有还没送达的介入消息按任务列成人话
    // 报告并清掉。
    std::vector<std::string> TakeUndeliveredInboxReport();

    // ---- 投递口(主循环轮询)----
    bool HasRunningTasks() const;
    std::size_t RunningCount() const;  // 后台启动前的同步先手检查用(活态计数)
    // main 回合上下文里还有没有未送达的完成结果:只认 delivery_target ==
    // MainTurnContext 的根子任务——ParentTaskInbox 的账归直接父,main 不
    // 跨级提走(P0-4)。
    bool HasUndeliveredCompletions() const;
    // 旧线程收柄探测:这只任务(按 id)是否已进终态、它的线程可以收柄。
    // 认不出(没这只任务——比如已被 ClearFinishedTask 清掉)也按已收尾
    // 返回:能被清掉的任务必是终态,线程早退了,不收白漏一枚句柄。
    // 按任务号对账,不按注册序下标——台账里混着无线程的前台任务,下标
    // 对不齐会把旧任务的终态安到活线程头上(join 押死孵化,病灶一)。
    bool TaskSettled(int task_id) const;
    // 退出兜底:给所有任务广播取消(只置 task->cancel,不动状态)。
    void BroadcastCancel();
    std::vector<std::string> CompletionNoticeLines() const;
    std::vector<int> UndeliveredCompletionTaskIds() const;
    // 取走攒着的后台完成通知(结果全文,交回主模型)。P0-4 起按送达去处
    // 取:无参缺省只取 MainTurnContext(根子任务);嵌套子任务的完成走
    // 直接父 mailbox(DeliverChildCompletion),不在这里被 main 一把提走。
    std::string DrainCompletionNotices();

    // ---- 后台递归的结果归父(P0-4)----
    // 子任务线程收尾后调:delivery=ParentTaskInbox 且父仍活,则把结构化
    // ChildCompletion 项排进父 mailbox(带"外来资料"来路声明),子任务的
    // delivered 翻真,并唤醒等孩子的父。父已不活(看门狗强收的绝境)则
    // 保持未送达,收场报告照列——不 reparent,不悄悄改投 main。
    bool DeliverChildCompletion(const std::shared_ptr<TaskRecord>& child);
    // ChildCompletion 项的模型侧投影(来路声明 + 状态 + 结果),续投拼批
    // 与 mailbox 详情共用同一只 formatter(单子 §9.2)。
    static std::string FormatChildCompletion(const AgentTaskSnapshot& child);
    // 取走攒着的后台权限拒绝通知(每条一行,取走即清)。
    std::vector<std::string> TakePermissionDenialNotices();
    // 运行中子代理名册(给主模型/子代理的动态 context 用;递归派工单 §13.3):
    // caller_task_id=0(main)只列直接子任务(parent_task_id==0),不铺全局
    // 整棵树——Agent Dock 才展开孙辈;非 0 只列该任务自己的直接孩子
    //(parent_task_id==caller_task_id)。不把孙辈塞进每一轮 prompt。
    std::string RunningTasksRoster(int caller_task_id = 0) const;

    // ---- 消息账写口(Locked = 调用方已持 mutex)----
    void AppendEventLocked(const std::shared_ptr<TaskRecord>& task, AgentTaskEvent event);
    void FlushPendingTextLocked(const std::shared_ptr<TaskRecord>& task);

    // 活态内的状态翻页(WaitingChildren <-> Running,面板"等 N 只子任务"
    // 用):只许活态内翻,终态不动——迟到线程不许翻账。
    void SetLiveTaskState(const std::shared_ptr<TaskRecord>& task, AgentTaskState state);

    // ---- 进展合同写口(P0-0;监督器单)--------------------------------------
    // 口径(单子 §6.1):token 流量只刷 transport;工具开闭/完整消息提交/
    // inbox 送达刷 execution;指纹变化刷 meaningful progress。心跳(tick、
    // 状态查询)只在读侧,不进这些口——心跳不证明任务有进展。
    // 无 Locked 后缀的自己拿锁;带 Locked 的要求调用方已持 mutex(ledger_
    // sink 的逐事件路径在锁内,别双锁)。
    void RecordRequestStarted(const std::shared_ptr<TaskRecord>& task, int attempt,
                              const std::string& history_commit_hash);
    void RecordTransportActivity(const std::shared_ptr<TaskRecord>& task);  // 每枚 SSE 帧
    void RecordFirstStreamEvent(const std::shared_ptr<TaskRecord>& task, int ttfb_ms);
    void RecordStageLocked(const std::shared_ptr<TaskRecord>& task, agent::AgentSupervisionStage stage);
    // 相位翻页的自拿锁版(continuation 等台账锁外调用点用)。
    void RecordStage(const std::shared_ptr<TaskRecord>& task, agent::AgentSupervisionStage stage);
    void RecordExecutionActivityLocked(const std::shared_ptr<TaskRecord>& task);
    // 工具起跑:execution + tool_started_at + RunningTool 相位。
    void RecordToolStartedLocked(const std::shared_ptr<TaskRecord>& task);
    // meaningful progress + 轮次账:指纹与上一笔不同才算真进展(时间戳、
    // progress_revision、stale_rounds 归零);相同则 stale_rounds +1(空转
    // 计数)。返回真 = 本笔是新指纹。
    bool RecordMeaningfulProgressLocked(const std::shared_ptr<TaskRecord>& task, const std::string& fingerprint);
    // 完整 assistant 消息提交进 history(轮次边界):execution + meaningful。
    void RecordAssistantMessage(const std::shared_ptr<TaskRecord>& task, const std::string& content_fingerprint);
    // 工具收口:execution + meaningful(结果散列做指纹)。
    void RecordToolCompletedLocked(const std::shared_ptr<TaskRecord>& task, const std::string& result_fingerprint);
    // 孩子交付进父 mailbox / inbox 安全送达:前者 meaningful,后者 execution。
    void RecordChildDeliveredLocked(const std::shared_ptr<TaskRecord>& parent);
    void RecordInboxDeliveredLocked(const std::shared_ptr<TaskRecord>& task);

    // ---- 请求级恢复账(P0-1)------------------------------------------------
    // 重试决定:attempt 记账、健康翻 Recovering、相位翻 Recovering,并把
    // 上一尝试的半截显示账回滚到 live_output_mark(不拼两段正文,单子
    // §8.3)。终态任务不记(晚到线程不翻账)。
    void RecordRequestRetry(const std::shared_ptr<TaskRecord>& task, int attempt, const std::string& reason_code);
    // 尝试收场:成功回 Healthy(传输侧自愈),失败只留稳定错误码——终态
    // 由收尾分型定,这里不越权。
    void RecordRequestOutcome(const std::shared_ptr<TaskRecord>& task, bool succeeded,
                              const std::string& reason_code);

    // ---- 监督面(P0-2)------------------------------------------------------
    // 锁内对每只活任务跑一遍 visitor(记录指针 + 只读视景);visitor 里
    // 不许再拿台账锁。监督线程的取数口。
    void ForEachAliveVitals(
        const std::function<void(const std::shared_ptr<TaskRecord>&, const agent::TaskVitals&)>& visitor) const;
    // 翻健康(epoch 递增,通知按它去重);终态翻 Terminal 也走这。返回翻后
    // 的 epoch,0 = 没翻(同态/已终态)。
    std::uint64_t ApplyHealth(const std::shared_ptr<TaskRecord>& task, agent::AgentHealthState health,
                              const std::string& reason_code);
    // 空转提醒:host notice 进 inbox(轮次边界注入,给模型一轮自救上下文,
    // 单子 §七 SuspectAgent 流转),progress.host_notice_sent 置位只投一次。
    bool PushHostNotice(const std::shared_ptr<TaskRecord>& task, const std::string& text);
    // 空转收口信号:置 no_progress_fired + wall_stop(走取消链,不冒充用户
    // 中止),等任务线程自己收账;收不动由 ForceFinalizeNoProgress 兜底。
    void RequestNoProgressStop(const std::shared_ptr<TaskRecord>& task);
    // 看门狗式强收(Failed/NoMeaningfulProgress):与 ForceFinalizeWallClock
    // 同一骨架,部分结果照旧留在台账(CheckpointFallback 带得走)。
    void ForceFinalizeNoProgress(const std::shared_ptr<TaskRecord>& task, int stale_rounds);
    // 监督通知(去重由监督器按 task+epoch+reason 把):主会话空闲拍取走。
    void PushSupervisorNotice(std::string notice);
    // 去重版(单子 §十:同一 task_id + health_epoch + reason 只弹一次):
    // 监督器与台账侧(恢复成功/用尽/工具结果不明)共用这一只去重口,键
    // 账在台账里,谁发都走同一本。epoch=0 不弹。
    void PushSupervisorNoticeDeduped(const std::shared_ptr<TaskRecord>& task, std::uint64_t health_epoch,
                                     const std::string& reason_key, std::string notice);
    std::vector<std::string> TakeSupervisorNotices();

    // ---- 恢复账与工具不明账的台账侧事件(P1-1/P2)--------------------------
    // 重试链用尽(P1-1 通知"用尽"):AgentTool 的 on_request_attempt 在
    // Exhausted 相位调。记一笔 deduped 通知 + 监督事件投递。
    void RecordRecoveryExhausted(const std::shared_ptr<TaskRecord>& task, const std::string& reason_code);
    // 工具被取消后结果不明(P1-1 通知"工具结果不明",单子 §8.3
    // IndeterminateToolOutcome):已在执行边界后的工具被取消,副作用是否
    // 落地无从证实——消息账补一张明确的结果卡(不冒充成功),tool_calls
    // 对账收口,通知请用户/父代理核对。绝不自动重跑。调用方须已持 mutex。
    void RecordToolIndeterminateLocked(const std::shared_ptr<TaskRecord>& task, const std::string& tool_name,
                                       const std::string& tool_use_id);

    // ---- 监督事件投递口(P2:AgentHealthChanged 只读 hook 的进料)----------
    // 台账侧发生的恢复/不明/强收事件(recovery.started/succeeded/exhausted、
    // tool.indeterminate、force_finalize)经这枚 sink 递给监督器(它再投
    // 后台安全队列与指标计数)。sink 在台账锁内被调,只许入队,不许回头
    // 拿台账锁。空(默认)= 没人接,事件照常进通知,只是不出钩子。
    void SetSupervisionEventSink(std::function<void(const agent::AgentSupervisionEvent&)> sink) {
        supervision_event_sink_ = std::move(sink);
    }

    // ---- inbox 原子交接 ----
    // 一轮 Run 正常收口后调。有未送项 -> 取走标 delivered(sealed=false),
    // 调用方必须把它们注入再续跑一轮——"Queued 是交付承诺"。inbox 空且
    // 这只任务还有活孩子 -> 不封账(sealed=false,调用方去等孩子,
    // WaitingChildren);inbox 空且无活孩子 -> 置 inbox_closed 封账
    //(sealed=true,后续 SendMessage 拒收)。
    DrainedInbox SealOrContinueInbox(const std::shared_ptr<TaskRecord>& task, bool& sealed);
    // WaitingChildren 的等待口(P0-4):阻塞到"有未送 inbox 项 / 无活孩子 /
    // 取消信号 / 强制收账"任一成立。不占 provider 请求,不烧 token;取消
    // 与孩子终态都经同一枚条件变量唤醒,不忙轮询。醒来后调用方再查一遍
    // SealOrContinueInbox。
    void WaitForKeyChange(const std::shared_ptr<TaskRecord>& task);
    // 续跑失败时把取件批次退回未送(按下标回滚,不整箱回退)。批次里的
    // ChildCompletion 项连来源子任务的 delivered 一起退——"读出来了不等
    // 于送达了",退信后孩子重新挂未送达,收场报告照列。
    void RestoreDrainedInbox(const std::shared_ptr<TaskRecord>& task, const DrainedInbox& drained);
    // 收尾账注:还有未送介入消息时,给结果文本追加"N 条未送达 + 逐条
    // 首行原文"的附言。
    static std::string UndeliveredInboxNote(const std::shared_ptr<TaskRecord>& task);

    // ---- 收尾 ----
    // 前台/后台任务线程收尾的共用块(Locked 语义自己拿锁):写结果、定终态
    //(面板 x / 父轮 ESC 算用户中止)、清活度账、finalized 置位。看门狗已
    // 强制收账时只报收尾不翻账。watchdog 由调用方 join(线程句柄在
    // TaskRecord 上)。
    void FinalizeFromToolResult(const std::shared_ptr<TaskRecord>& task, const std::string& result_content,
                                bool cancelled_by_stop_signal);
    // 看门狗强制收账(墙钟绝境):状态翻 Failed/WallClockTimeout,force_
    // finalized 置位——任务线程晚到的收尾不得再翻回去。
    void ForceFinalizeWallClock(const std::shared_ptr<TaskRecord>& task, int timeout_secs);

    // 后台任务"需确认工具被拒"的当场通知:任务线程推一行,主会话空闲拍
    // 里取走(TakePermissionDenialNotices)。
    void PushPermissionDenialNotice(std::string notice);

private:
    // 台账锁内的派生查询(公开口都在锁外套这层):
    bool HasUndeliveredInboxLocked(const TaskRecord& task) const;
    std::size_t AliveChildCountLocked(int parent_task_id) const;
    void NotifyStateChangeLocked();
    // agent_watch 的唤醒源(P1-0):监督可见修订动一笔就 ++ 并 notify。
    // 调用方须已持 mutex(谓词同锁读,无丢醒)。
    void NotifyWatchChangeLocked();
    // 去重通知的锁内版(调用方已持 mutex)。
    void PushSupervisorNoticeDedupedLocked(const std::shared_ptr<TaskRecord>& task, std::uint64_t health_epoch,
                                           const std::string& reason_key, const std::string& notice);
    // 监督事件投递(锁内调,空 sink 零开销)。
    void EmitSupervisionEventLocked(const std::shared_ptr<TaskRecord>& task, agent::AgentSupervisionEventKind kind,
                                    agent::AgentHealthState old_health, agent::AgentHealthState new_health,
                                    const std::string& reason_code);

    std::vector<std::shared_ptr<TaskRecord>> tasks_;
    int next_task_id_ = 1;
    std::atomic<std::uint64_t> revision_{0};
    std::vector<std::string> permission_denial_notices_;
    std::vector<std::string> supervisor_notices_;
    // 监督通知去重键(P1-0 起与监督器共用这一本):task_id:epoch:reason。
    std::set<std::string> supervisor_notice_keys_;
    // 监督事件 sink(P2):台账侧事件 -> 监督器的钩子总线/指标。锁内调。
    std::function<void(const agent::AgentSupervisionEvent&)> supervision_event_sink_;
    // WaitingChildren 的唤醒源:inbox 入项/孩子终态/取消/强收都在台账锁内
    // 记账,松锁前 notify_all(单子 §6.3 第 7 条)。一把全局 cv 足够——
    // 谓词按各自 task 记录判,虚醒只是再查一遍。
    std::condition_variable state_cv_;
    // agent_watch 的等待口(P1-0):与 state_cv_ 分开——watcher 不必被
    // WaitingChildren 的每次虚醒搅醒,反之监督可见修订也不必动 state 谓词。
    std::condition_variable watch_cv_;
    std::uint64_t watch_generation_ = 0;
};

// ---- 台账侧的文本小件(自 agent_tool.cpp 搬来,行为一字不改)----

// 终态短标签(通知/面板共用)。
std::string StateShortLabel(AgentTaskState state);
// 终态映射:结构化 status -> 台账 state。
AgentTaskState StateFromOutcome(TaskOutcomeStatus status);
// 短因(规格"现场三"):面板与通知只放短因,完整错误进 transcript。
std::string ReasonShortLabel(TaskOutcomeReason reason);
// 单行化:取首行,截前 80 个码点(按 UTF-8 续字节截齐,不劈半个字)。
std::string FirstLineOf(const std::string& text);
// 结构化结果交回主模型的正文:短状态打头,再给检查点/部分结果与最后
// 工具、stop reason——几十步探索不许一笔勾销。
std::string ComposeOutcomeText(const TaskOutcome& outcome);
// 输出预算耗尽的失败页(规格根因四)。
std::string ComposeOutputBudgetOutcomeText(const TaskOutcome& outcome);
// 检查点兜底:最后一条 assistant 没有文本时,把台账里最后完成的工具
// 结果/实时输出尾巴当部分结果带回——绝不交白卷。
std::string CheckpointFallback(const AgentTaskSnapshot& snapshot);
// 定向消息注入 history 时的来源标签(User=用户直发,MainAgent=主模型
// 经 agent_message 工具转交);分栏写明"不是权限确认、不执行 slash"。
std::string FormatInboxDelivery(const std::string& text, TaskMessageSource source);

}  // namespace lubancode::tools
