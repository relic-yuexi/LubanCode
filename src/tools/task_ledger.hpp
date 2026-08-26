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
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include <nlohmann/json.hpp>

#include "agent/loop.hpp"  // ToolTraceEvent 等转发类型(与拆分前同一份引用)
#include "api/types.hpp"

namespace lubancode::tools {

class AgentTool;

// 墙钟兜底的默认收杀宽限:看门狗发出停止信号后,任务线程这么久还没报终态
// (后端不理取消的绝境),就把台账强制翻成终态。上限本体的默认值
// (kDefaultSubagentWallClockTimeoutSecs)在 config.hpp,跟 subagent.* 那批
// 默认住一起;宽限是实现细节,住这边。
constexpr int kDefaultSubagentWallClockGraceSecs = 30;

enum class AgentTaskState { Running, Done, Failed, Cancelled, BudgetExhausted };

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
    OutputBudgetExhausted,  // 输出预算耗尽(max_tokens;续跑用完仍无正文,规格根因四)
    MaxContext,        // 上下文装不下
    NoFinalText,   // 最后一轮没有文本结论
    ToolError,     // 最后一次工具调用出错带崩了收尾
    UserStop,      // 用户中止
    WallClockTimeout,  // 整轮墙钟上限兜底(subagent.wall_clock_timeout_secs)
    ProtocolError, // 会话协议异常(连历史都没有等)
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
};

struct TaskOutcome {
    TaskOutcomeStatus status = TaskOutcomeStatus::Failed;
    TaskOutcomeReason reason = TaskOutcomeReason::None;
    std::string message;         // 给人看的短说明(一两句)
    std::string partial_result;  // 已取得的事实或最后检查点(尽力保住)
    std::string stop_reason;     // 模型原始 stop reason(空 = 一个字都没回来)
    std::string last_tool;       // 最后一次工具名与结果摘要
    int steps_used = 0;
    int step_limit = 0;          // 0 = 不限步(一个 turn 内的模型请求数上限)
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
    std::string title;
    std::string prompt;
    // 前台(阻塞父级调用)还是后台(独立线程)。详情可看,列表不必铺。
    bool foreground = false;
    AgentTaskState state = AgentTaskState::Running;
    // 派出时写死的预算(0 = 不限步):面板可见,不等撞墙才揭晓(规格"现场四")。
    int step_limit = 0;
    int steps_used = 0;  // 已发生的模型请求数(RunOutcome 直接记账,不靠 usage 回调猜)
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
};

// 轻量列表条目(0.28.x 面板全量化):列表每 100ms 刷新一次,不再复制
// tool_calls/live_output 这些大块——面板列表行只要这几个字段;详情(完整
// 任务说明、工具流水、介入消息)另走 TaskDetail(task_id) 按需取。
struct AgentTaskSummary {
    int id = 0;
    std::string agent_type;
    std::string title;  // 真正短标题;空 = 旧任务,显示层退"未命名子代理 #N"
    std::string prompt;
    bool foreground = false;
    AgentTaskState state = AgentTaskState::Running;
    int step_limit = 0;
    int steps_used = 0;
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

// ---------------------------------------------------------------------------
// TaskRecord:一只任务的全套账。从前是 AgentTool 的私有嵌套,拆出随台账
// 走(TraceBackend/看门狗闭包要 shared_ptr 自保)。
// ---------------------------------------------------------------------------
struct TaskRecord {
    // 定向介入的收件口。推(SendTaskMessage,主线程)与取(子代理线程的
    // inbox 轮询)都拿 inbox_mutex;终态判定+投递在台账锁内成对完成,任务
    // 收尾后不可能再排进新信。delivered 标记:轮次边界取走的那条不再重发;
    // 未送达的留给详情展示与收场报告。
    struct InboxItem {
        std::string text;
        bool delivered = false;
        TaskMessageSource source = TaskMessageSource::User;
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
    // 墙钟兜底的三枚信号:wall_stop 看门狗到点置位(合并 cancel 线程收进
    // merged cancel,绝不动 task->cancel——那会被收成"用户中止");
    // wall_clock_fired 供收场分型(墙钟超时 ≠ 用户中止);finalized 是任务
    // 线程正式收尾的标志(看门狗宽限期内据此早退)。
    std::atomic<bool> wall_stop{false};
    std::atomic<bool> wall_clock_fired{false};
    std::atomic<bool> finalized{false};
    // 看门狗强制收账后置位:任务线程晚到的收尾不得再把台账翻回去
    //(状态/结果/outcome 一律保持强制收账那份)。
    bool force_finalized = false;
    // 墙钟看门狗线程(RunTask 起,收尾块 join):闭包另握一份 record 的
    // shared_ptr 自保——任务线程卡死的绝境下 record 不悬垂。
    std::thread watchdog;
    // 封账闸(规格第五节"排到了却没送"):子代理准备从 Running 进终态时,
    // 在台账锁里查一遍 inbox——空则置 true,此后 SendTaskMessage 明确拒收
    //(Finished);非空则不封账,取走注入、再续跑一轮。置位与入队同锁成对,
    // 不存在"刚封账又收信"的缝。
    bool inbox_closed = false;
};

// 续投交接的取件批次:SealOrContinueInbox 取走未送项时记下标与原文;续跑
// 若失败(取消/provider 错误),按 indices 把这批退回未送,收场报告照列
// 原文——不能"取走了就当送到了"。
struct DrainedInbox {
    std::vector<std::size_t> indices;
    std::vector<std::string> texts;
    std::vector<TaskMessageSource> sources;
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
    // 自持自保)。
    std::shared_ptr<TaskRecord> Register(AgentTaskSnapshot snapshot);

    // 修订号 +1(面板/查看态按它决定重画)。原子,任何线程可调。
    void Touch() { revision_.fetch_add(1, std::memory_order_release); }
    std::uint64_t revision() const { return revision_.load(std::memory_order_acquire); }

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
    // 某只任务已排队未送达的介入消息原文(详情展示/测试用)。
    std::vector<std::string> PendingMessages(int task_id) const;

    // ---- 介入/取消/清理 ----
    // 定向介入:终态明确拒收(不改投 main)。
    TaskMessageStatus SendMessage(int task_id, const std::string& text,
                                  TaskMessageSource source = TaskMessageSource::User);
    // 正式取消(面板 x):只发停止信号,等任务线程报出终态。
    bool CancelTask(int task_id);
    // 停全部运行中任务,返回发了停止信号的任务数。
    int CancelAllTasks();
    // 把一条终态任务从面板/台账清掉(顺带清它的介入消息)。运行中不给清。
    bool ClearFinishedTask(int task_id);
    // 会话收场(/clear、退出)用:所有还没送达的介入消息按任务列成人话
    // 报告并清掉。
    std::vector<std::string> TakeUndeliveredInboxReport();

    // ---- 投递口(主循环轮询)----
    bool HasRunningTasks() const;
    std::size_t RunningCount() const;  // 后台启动前的同步先手检查用
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
    // 取走攒着的后台完成通知(结果全文,交回主模型)。
    std::string DrainCompletionNotices();
    // 取走攒着的后台权限拒绝通知(每条一行,取走即清)。
    std::vector<std::string> TakePermissionDenialNotices();
    // 运行中子代理名册(给主模型的动态 context 用)。
    std::string RunningTasksRoster() const;

    // ---- 消息账写口(Locked = 调用方已持 mutex)----
    void AppendEventLocked(const std::shared_ptr<TaskRecord>& task, AgentTaskEvent event);
    void FlushPendingTextLocked(const std::shared_ptr<TaskRecord>& task);

    // ---- inbox 原子交接 ----
    // 一轮 Run 正常收口后调。inbox 空 -> 置 inbox_closed 封账(sealed=true,
    // 后续 SendMessage 拒收);有未送项 -> 取走标 delivered(sealed=false),
    // 调用方必须把它们注入再续跑一轮——"Queued 是交付承诺"。
    DrainedInbox SealOrContinueInbox(const std::shared_ptr<TaskRecord>& task, bool& sealed);
    // 续跑失败时把取件批次退回未送(按下标回滚,不整箱回退)。
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
    std::vector<std::shared_ptr<TaskRecord>> tasks_;
    int next_task_id_ = 1;
    std::atomic<std::uint64_t> revision_{0};
    std::vector<std::string> permission_denial_notices_;
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
