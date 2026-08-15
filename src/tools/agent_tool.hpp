// 内置 "agent" 工具:把一个独立子任务委托给子代理执行。子代理是一个全新
// 的、空历史的 agent::AgentLoop,只有它自己知道任务细节——主对话历史里
// 只留一次工具调用的入参(prompt)和一段 Result.content(子代理的最终
// 结论),中间的搜索/试错/来回工具调用过程都不会挤占主对话的上下文,
// 这是这个工具存在的全部意义。
//
// 防递归:子代理拿到的工具注册表(sub_registry)不含 "agent" 自己,深度
// 硬限 1——子代理没法再委托一个孙代理。main.cpp 负责建两份 ToolRegistry
// (主表含 agent,子表不含),这里只管拿着子表的引用用,自己不做任何排除
// 逻辑。
//
// 回调贯通:子代理执行期间的确认请求(needs_confirm 的工具)、usage 记账、
// "子代理调了个工具"这件事本身,都要能让上层(main.cpp)看到、按同一套
// 确认模式处理——但子代理的碎碎念文本(on_text_delta)不逐字外放,免得
// 刷屏。做法是 Hooks 结构体:main.cpp 在每一轮 RunTurn 开始时,把这一轮
// 的 on_tool_confirm/on_usage 直接转发过来、外加一个"子代理调了工具"的
// 打印回调,通过 SetHooks 灌进来;execute() 每次跑子代理时都用当前这份
// Hooks 转发,不设(默认构造的空 Hooks)也不会崩,只是不转发/不打印。
#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include <nlohmann/json.hpp>

#include "api/backend.hpp"
#include "api/types.hpp"
#include "cli/worktree.hpp"
#include "tools/isolation.hpp"
#include "tools/registry.hpp"
#include "tools/tool.hpp"

namespace lubancode::tools {

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
    MaxContext,        // 上下文装不下
    NoFinalText,   // 最后一轮没有文本结论
    ToolError,     // 最后一次工具调用出错带崩了收尾
    UserStop,      // 用户中止
    ProtocolError, // 会话协议异常(连历史都没有等)
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
    std::int64_t input_tokens = 0;
    std::int64_t output_tokens = 0;
    double elapsed_seconds = 0;
};

struct AgentTaskToolCall {
    std::string name;
    std::string input_json;
    std::string result;
    bool done = false;
    bool is_error = false;
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
    std::int64_t input_tokens = 0;
    std::int64_t output_tokens = 0;
    std::chrono::steady_clock::time_point start_time{};
    std::chrono::steady_clock::time_point end_time{};
    std::vector<AgentTaskToolCall> tool_calls;
    std::string live_output;
    std::string result;
    // 结构化结果:终态时由 RunTask 填。运行中 status 停在 Failed/None,
    // 消费方只看 state == Running 判"还在跑"。
    TaskOutcome outcome;
    bool delivered = false;
};

// ---------------------------------------------------------------------------
// 子代理消息账(规格"现场三"):每只任务一份独立的、按时间追加的事件流,
// 事件类型与 main 的 transcript 对齐。查看态复用 main 的 TranscriptItem/
// 折叠规则/工具卡 renderer 渲染这份账——两边各有独立消息账,共用一套显示
// 组件,绝不手搓第二套长文本。
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
    std::int64_t output_tokens = 0;
    std::chrono::steady_clock::time_point start_time{};
    std::chrono::steady_clock::time_point end_time{};
    std::size_t tool_call_count = 0;
    std::size_t pending_message_count = 0;  // 已排队未送达的介入消息数
    // 结果是否已交回主会话(DrainCompletionNotices 置位)。面板的"退场"账
    // 用:done+delivered 的任务从活动导航坞退场,台账照查(规格"现场一")。
    bool delivered = false;
};

// SendTaskMessage 的收信结果。
enum class TaskMessageStatus {
    Queued,    // 已排进该任务的 inbox,轮次边界注入
    NotFound,  // 没有这个任务号(已被清理/从未存在)
    Finished,  // 任务已进终态,明确拒收——绝不改投 main
};

// 定向消息的来路。三条来源不同(main steering 仍走会话层 SteeringQueue,
// 不进 inbox):查看态 composer 与用户排队转投是 User,主模型经
// agent_message 工具转交是 MainAgent——history 里的来源标签按这个分,
// 不混(规格"不要混淆三种 queue"节)。
enum class TaskMessageSource { User, MainAgent };

// 后台子代理不能借主回合那条 Backend：主回合会重画 spinner，切 provider
// 时还会替换内部 client。工厂在 launch 当口造一份独立快照，线程随后只
// 握自己的 client、模型和提示词叠加层。
struct DetachedAgentBackend {
    std::unique_ptr<api::Backend> backend;
    std::string model;
    std::string reasoning_effort;
    std::string model_instructions;
    std::string soul;
    nlohmann::json request_extra_body = nlohmann::json::object();
};

class AgentTool : public Tool {
public:
    struct Hooks {
        // 子代理内部工具 needs_confirm() 为真时,原样转发给父级
        // on_tool_confirm——三档确认模式(yolo/auto/confirm)在父级那份
        // 回调里已经处理好了,这里不用重复实现。
        std::function<bool(const std::string& name, const nlohmann::json& input)> on_tool_confirm;

        // 子代理发起了一次工具调用(还没执行),给上层打一行提示用,比如
        // "  [子代理·工具] read_file {...}"。跟 on_tool_confirm 分开是因为
        // 这个纯粹用于展示,没有返回值、不影响子代理是否真的执行。
        std::function<void(const std::string& name, const nlohmann::json& input)> on_sub_tool_start;

        // 子代理每次独立请求结束的 usage,原样转发给父级 on_usage——累计
        // 进本轮 token 统计,请求次数也算进去。
        std::function<void(const api::Usage& usage)> on_usage;

        // M9:子代理内部的工具调用也要受 pre_tool/post_tool 钩子管——原样
        // 转发给父级的同名回调,子代理这边不重复实现匹配/执行逻辑。
        std::function<std::optional<std::string>(const std::string& name, const nlohmann::json& input)>
            on_pre_tool_hook;
        std::function<void(const std::string& name, const nlohmann::json& input, const Tool::Result& result)>
            on_post_tool_hook;

        // ESC/Ctrl+C 打断信号(main.cpp 那份 cancel_flag 的地址)——子代理
        // 内部的 AgentLoop::Run() 原样收这根指针,工具循环里就能跟顶层同一套
        // "cancel != nullptr && cancel->load()"判断打断,不用重新实现打断
        // 语义。不设(默认 nullptr)= 子代理收不到外部打断,行为跟从前一样。
        const std::atomic<bool>* cancel = nullptr;
    };

    // backend:子代理发请求用的后端。main.cpp 传进来的通常是跟主循环共用
    // 的那条包装链(模型覆盖/推理强度覆盖/转轮),子代理天然继承同一套。
    // sub_registry:子代理能用的工具注册表,调用方保证其中不含 "agent" 自己
    // (防递归,深度硬限 1)。
    // cwd:子代理系统提示词里的工作目录段,跟主代理一致。
    // model:子代理发请求用的 model 字段;如果 backend 链里有会覆盖 model
    // 的包装层(比如 main.cpp 的 ModelOverrideBackend),这里填什么都会被
    // 覆盖掉,留空也没关系。
    // default_max_steps_per_turn:入参没给 max_steps_per_turn 时用的默认值。
    // 调用方必须从配置传入(首选 subagent.max_steps_per_turn,未设继承
    // config.max_steps_per_turn;默认 0 = 不限步)——这里不再暗藏魔数(旧版
    // 先后藏过 15 和 40,规格"现场四"点名拆掉);仅测试直调时用参数默认 0。
    // skills_segment:M9 新增,系统提示词里"可用技能"那一段(见
    // agent::BuildSkillsPromptSegment),子代理跟主代理共用同一份扫描结果,
    // 空串表示没有技能(不注入)。main.cpp 扫描一次,主代理、子代理都传
    // 同一份。
    AgentTool(api::Backend& backend, ToolRegistry& sub_registry, std::string cwd, std::string model = std::string(),
              int default_max_steps_per_turn = 0, std::string skills_segment = std::string());

    ~AgentTool() override;

    void SetHooks(Hooks hooks) { hooks_ = std::move(hooks); }

    // Explore 是内置只读代理。调用方另建一张只读工具表塞进来；不设时
    // Explore 仍能启动，只是沿用普通子表再由过滤器挡掉写入工具。
    void SetExploreRegistry(ToolRegistry* registry) { explore_registry_ = registry; }

    // 交互会话开、单发/单测关。入参显式给 run_in_background 时压过它。
    void SetBackgroundByDefault(bool enabled) { background_by_default_ = enabled; }
    void SetDetachedBackendFactory(std::function<DetachedAgentBackend()> factory) {
        detached_backend_factory_ = std::move(factory);
    }
    void SetDetachedRegistryFactory(std::function<std::unique_ptr<ToolRegistry>()> factory) {
        detached_registry_factory_ = std::move(factory);
    }

    // max_entries=0 取全量；给正数时保留全部运行中任务，再从新到旧补齐
    // 最近的终态任务。面板用限量快照，长会话不会越画越长。
    std::vector<AgentTaskSnapshot> TaskSnapshots(std::size_t max_entries = 0) const;
    std::uint64_t TaskRevision() const { return task_revision_.load(std::memory_order_acquire); }
    std::string DrainCompletionNotices();
    bool HasRunningTasks() const;

    // ---- 0.28.x 面板全量 + 定向介入 ----

    // 轻量全量列表(运行中、完成、失败、取消都在,不截 8 只)。面板 provider
    // 每 100ms 拉这个;开销只有 id/类型/短述/计数,不复制工具流水。
    std::vector<AgentTaskSummary> TaskSummaries() const;
    // 某只任务的详情快照(查看态按需取):完整 prompt、全部工具调用流水、
    // live_output/result。task_id 认不出返回 nullopt。
    std::optional<AgentTaskSnapshot> TaskDetail(int task_id) const;
    // 某只任务的消息账(按时间序,查看态的会话视口用):事件流复用 main 的
    // 渲染组件,不在这里拼显示文本。task_id 认不出返回空表。运行中也可调,
    // 读到的是已封口事件 + 正在累积的正文/思考尾巴(各切一段带出)。
    std::vector<AgentTaskEvent> TaskEvents(int task_id) const;
    // 某只任务已排队未送达的介入消息原文(详情展示/测试用)。
    std::vector<std::string> PendingTaskMessages(int task_id) const;

    // 定向介入:把用户在查看态提交的话排进该任务自己的 inbox,在"当前
    // 工具调用收尾、下一次模型请求发出之前"注进那只子代理的 history——
    // 不经 main history,不串到别只代理。终态明确拒收(不改投 main)。
    // source 只决定注入 history 时的来源标签(User=用户直发,MainAgent=
    // 主模型经 agent_message 工具转交),落点同一条 inbox。
    TaskMessageStatus SendTaskMessage(int task_id, const std::string& text,
                                      TaskMessageSource source = TaskMessageSource::User);

    // 运行中子代理名册(给主模型的动态 context 用):每条外层用户消息到
    // 来时现算一份,只列 task id/真标题/类型/待送数,不塞 prompt 与日志。
    // 没有运行中任务时返回空串(不注入)。调用方把它拼进请求级 system 尾
    // 段(AgentLoop::SetTurnSystemSuffix)——不进 history,compact 后照常
    // 从台账重注入,不依赖摘要记任务号。
    std::string RunningTasksRoster() const;

    // 正式取消接口(面板 x / Ctrl+X Ctrl+K 接这里):只发停止信号,等任务
    // 线程报出终态,快照里的灯才会变——不从面板侧抹行。
    bool CancelTask(int task_id);
    // 停全部运行中任务,返回发了停止信号的任务数。
    int CancelAllTasks();
    // 把一条终态任务从面板/台账清掉(顺带清它的介入消息)。运行中不给清。
    bool ClearFinishedTask(int task_id);

    // 会话收场(/clear、退出)用:把所有还没送达的介入消息按任务列成人话
    // 报告并清掉——"不能无声遗失"的最低限;调用方负责打给人看。
    std::vector<std::string> TakeUndeliveredInboxReport();

    // 有没有"已经进终态、结果还没投递给主会话"的后台任务。面板轮询靠
    // TaskRevision 画状态,主循环靠这个知道"该把结果送回主代理了"——
    // DrainCompletionNotices 投递完之后这里就翻回 false。
    bool HasUndeliveredCompletions() const;
    // 未投递完成结果的短行(每个任务一行,只 peek 不置 delivered):完成
    // 通知给人看的短进度行用——"#2 标题 · 完成 · 12 次工具 · 3.4k tokens"。
    // 完整结果照旧走 DrainCompletionNotices 进模型消息。
    std::vector<std::string> CompletionNoticeLines() const;

    // 主会话切进 /worktree 后，子代理也得看见同一处工作目录。
    void SetWorkingDirectory(std::string cwd) { cwd_ = std::move(cwd); }

    // 子代理的上下文窗口(token):mid-turn 压力评估与自动 compact 用
    // (规格"长任务还缺 compact")。0 = 未知,不评估——行为与从前一致。
    // 启动时(main.cpp)从 config.context_window_tokens 传入。
    void SetContextWindowTokens(std::size_t tokens) { context_window_tokens_ = tokens; }

    // tool_search(延迟挂载):子代理注册表同机制。filter 原样灌给每次
    // execute() 新建的 sub_loop(loaded 集合与主会话共享,挂载一次两边
    // 可用);index_provider 每次 execute() 现算"延迟未加载"索引段,拼进
    // 子代理系统提示末尾。两个都不设(默认)= 子代理不启用延迟,行为跟
    // 从前完全一样。启动时(main.cpp)设一次,不随 RunTurn 重灌——它们
    // 跟 Hooks(每轮现算的转发回调)生命周期不同。
    void SetToolFilter(std::function<bool(const Tool&)> filter) { tool_filter_ = std::move(filter); }
    void SetDeferredIndexProvider(std::function<std::string()> provider) {
        deferred_index_provider_ = std::move(provider);
    }

    // 提示词运行时化(0.21.x):用户模块目录(~/.lubancode/prompts)。设了
    // 之后每次 execute() 新建子代理时,系统提示的 features 模块同走"用户
    // 文件优先、嵌入回退"——跟主循环同机制。不设(默认空)= 只用嵌入版,
    // 行为跟从前完全一样。
    void SetPromptsDir(std::string prompts_dir) { prompts_dir_ = std::move(prompts_dir); }

    // isolation=worktree 的房务 Git 调用可替身(测试注入假 runner);
    // 不设走真 git。
    void SetGitRunner(lubancode::cli::GitRunner runner) { git_runner_ = std::move(runner); }

    void SetSkillsSegment(std::string skills_segment) { skills_segment_ = std::move(skills_segment); }
    void SetProjectInstructions(std::string instructions) { project_instructions_ = std::move(instructions); }
    std::string name() const override;
    std::string description() const override;
    nlohmann::json input_schema() const override;
    bool needs_confirm() const override { return false; }  // 子代理内部的危险工具各自有确认关
    Result execute(const nlohmann::json& input) override;

private:
    struct TaskRecord {
        // 定向介入的收件口。推(SendTaskMessage,主线程)与取(子代理线程的
        // inbox 轮询)都拿 inbox_mutex;终态判定+投递在 tasks_mutex_ 内成对
        // 完成,任务收尾后不可能再排进新信。delivered 标记:轮次边界取走的
        // 那条不再重发;未送达的留给详情展示与收场报告。
        struct InboxItem {
            std::string text;
            bool delivered = false;
            TaskMessageSource source = TaskMessageSource::User;
        };
        AgentTaskSnapshot snapshot;
        std::atomic<bool> cancel{false};
        mutable std::mutex inbox_mutex;
        std::deque<InboxItem> inbox;
        // 消息账(规格"现场三"):按时间追加的事件流,写入全在任务线程
        // (loop 回调 + RunTask 循环),读取(TaskEvents)在主线程——都拿
        // tasks_mutex_,天然按发生次序。pending_* 是正在累积的助手正文/思考,
        // 在事件边界(工具发起/轮次收口)切成正式事件,不让中间文字被后
        // 续工具调用淹没。
        std::vector<AgentTaskEvent> events;
        std::string pending_text;
        std::string pending_reasoning;
        // 封账闸(规格第五节"排到了却没送"):子代理准备从 Running 进终态
        // 时,在 tasks_mutex_ 里查一遍 inbox——空则置 true,此后
        // SendTaskMessage 明确拒收(Finished);非空则不封账,取走注入、
        // 再续跑一轮。置位与入队同锁成对,不存在"刚封账又收信"的缝。
        bool inbox_closed = false;
    };

    // 续投交接的取件批次:SealOrContinueInbox 取走未送项时记下标与原文;
    // 续跑若失败(取消/provider 错误),按 indices 把这批退回未送,收场
    // 报告照列原文——不能"取走了就当送到了"。
    struct DrainedInbox {
        std::vector<std::size_t> indices;
        std::vector<std::string> texts;
        std::vector<TaskMessageSource> sources;
    };

    // isolation=worktree 的一站式准备:从 cwd_ 找仓库根、建房(agent- 前缀,
    // fresh 基准)、上锁。失败给 Result 错误,成功返回房信息。
    std::optional<lubancode::cli::AgentWorktree> SetupIsolationRoom(Result& error_out);
    // 收工房务:解锁;干净删房,有活留房并返回给结果文本的附言。
    static std::string FinishIsolationRoom(const lubancode::cli::AgentWorktree& room,
                                           const lubancode::cli::GitRunner& runner);

    Result ExecuteForeground(const nlohmann::json& input, const std::string& title, const std::string& agent_type,
                             ToolRegistry& task_registry, int max_steps_per_turn, bool isolate);
    Result LaunchBackground(const nlohmann::json& input, const std::string& title, const std::string& agent_type,
                            ToolRegistry& task_registry, int max_steps_per_turn, bool isolate);
    Result RunTask(api::Backend& backend, ToolRegistry& task_registry, const std::string& prompt,
                   const std::string& agent_type, int max_steps_per_turn, const Hooks* foreground_hooks,
                   const std::shared_ptr<TaskRecord>& task,
                   const DetachedAgentBackend* detached = nullptr,
                   const std::string* prepared_system_prompt = nullptr,
                   const IsolationScope* isolation_scope = nullptr);
    void TouchTasks() { task_revision_.fetch_add(1, std::memory_order_release); }

    // 消息账的写入辅助(都假定调用方已持 tasks_mutex_ 或独占任务线程收口):
    //   AppendTaskEventLocked —— 追加一枚事件(正文按 kLiveOutputCap 截尾);
    //   FlushPendingTaskTextLocked —— 把正在累积的正文/思考切成正式事件
    //   (工具发起/轮次收口等边界调用,保住"助手文字 -> 工具卡"的时序)。
    void AppendTaskEventLocked(const std::shared_ptr<TaskRecord>& task, AgentTaskEvent event);
    void FlushPendingTaskTextLocked(const std::shared_ptr<TaskRecord>& task);

    // 原子交接(规格第五节):子代理一轮 Run 正常收口后调。inbox 空 ->
    // 置 inbox_closed 封账(sealed=true,后续 SendTaskMessage 拒收);
    // 有未送项 -> 取走标 delivered(sealed=false),调用方必须把它们注入
    // 再续跑一轮——"Queued 是交付承诺",纯文本收尾也续开一轮处理增量。
    DrainedInbox SealOrContinueInbox(const std::shared_ptr<TaskRecord>& task, bool& sealed);
    // 续跑失败时把取件批次退回未送(按下标回滚,不整箱回退)。
    void RestoreDrainedInbox(const std::shared_ptr<TaskRecord>& task, const DrainedInbox& drained);
    // 收尾账注:还有未送介入消息时,给结果文本追加"N 条未送达 + 逐条
    // 首行原文"的附言(取消/步数耗尽/provider 错误都不能无声遗失)。
    static std::string UndeliveredInboxNote(const std::shared_ptr<TaskRecord>& task);

    api::Backend& backend_;
    ToolRegistry& sub_registry_;
    ToolRegistry* explore_registry_ = nullptr;
    std::string cwd_;
    std::string model_;
    int default_max_steps_per_turn_;
    std::string skills_segment_;
    std::string prompts_dir_;  // 提示词运行时化:空 = 只用嵌入版
    std::string project_instructions_;  // 当前工作目录的 AGENTS.md 分层内容
    Hooks hooks_;
    bool background_by_default_ = false;
    std::function<DetachedAgentBackend()> detached_backend_factory_;
    std::function<std::unique_ptr<ToolRegistry>()> detached_registry_factory_;
    mutable std::mutex tasks_mutex_;
    std::vector<std::shared_ptr<TaskRecord>> tasks_;
    std::vector<std::thread> task_threads_;
    int next_task_id_ = 1;
    std::atomic<std::uint64_t> task_revision_{0};
    std::function<bool(const Tool&)> tool_filter_;            // tool_search:空 = 不过滤
    std::function<std::string()> deferred_index_provider_;    // tool_search:空 = 不注索引段
    lubancode::cli::GitRunner git_runner_;                    // isolation 房务;空 = 真 git
    std::size_t context_window_tokens_ = 0;                   // 子代理 mid-turn 压缩评估;0 = 未知
};

}  // namespace lubancode::tools
