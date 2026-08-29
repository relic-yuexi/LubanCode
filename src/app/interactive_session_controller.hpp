// TerminalSessionController 的私头(会话终章):类声明自 interactive_
// session.cpp 拆出——控制器的"装配半边"(构造/析构/接线/材料包,定义在
// interactive_session_wiring.cpp)与"运行半边"(主循环/泵仲裁/回合入口,
// 定义在 interactive_session.cpp)共用这一份声明。对外仍只有
// app/interactive_session.hpp 的 RunInteractiveSession;本头不进任何
// controller 之外的层。
#pragma once

#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "agent/agent.hpp"
#include "agent/artifact_store.hpp"
#include "agent/compact.hpp"
#include "agent/prompt_assembler.hpp"
#include "app/agent_panel_presenter.hpp"
#include "app/backend_stack.hpp"
#include "app/commands/command_flow.hpp"
#include "app/commands/command_registry.hpp"
#include "app/commands/goal_commands.hpp"
#include "app/commands/loop_commands.hpp"
#include "app/commands/memory_commands.hpp"  // SessionTailContext(会话尾款材料)
#include "app/commands/session_commands.hpp"
#include "app/interactive_session.hpp"
#include "app/memory_extract.hpp"
#include "app/mention_support.hpp"
#include "app/session_stack.hpp"
#include "app/turn_runner.hpp"
#include "app/wirings/goal_session_wiring.hpp"
#include "app/wirings/loop_session_wiring.hpp"
#include "app/wirings/peer_session_wiring.hpp"
#include "app/wirings/plan_session_wiring.hpp"
#include "app/wirings/record_session_wiring.hpp"
#include "cli/context_tracker.hpp"
#include "cli/session_picker_panel.hpp"
#include "cli/theme.hpp"
#include "cli/transcript_controller.hpp"
#include "cli/worktree.hpp"
#include "config/config.hpp"
#include "config/model_catalog.hpp"
#include "memory/project_memory.hpp"
#include "peers/peer_session.hpp"
#include "runtime/event_sinks.hpp"
#include "runtime/idle_wake.hpp"
#include "runtime/session_runtime.hpp"
#include "runtime/session_work_scheduler.hpp"
#include "sessions/session_store.hpp"
#include "tools/agent_tool.hpp"
#include "tools/registry.hpp"

namespace lubancode::app {

// 一场交互会话:整场可变状态按所有权收成成员。构造 = 原先
// InteractiveLoop 进 while 之前的全部装配;Run() = 主循环;析构 = 原先
// 函数尾的手工收尾(摘收件点、停 peer、摘 UI 回调),异常退场同路。
//
// P6(显示系统剥离单):会话的"不碰画面"那半账本已搬去
// runtime::SessionRuntime(存档账/权限账/thread 身份/事件接线),本类
// 持一份并按引用续用老名字;存档成员(session_store/session_meta/
// session_title/...)以引用别名指向 runtime 那份,寿命由 runtime 成员的
// 声明位保住。工具栈/backend 栈/peer/面板仍住本类,后续批次再搬。
class TerminalSessionController {
public:
    explicit TerminalSessionController(const InteractiveSessionOptions& options, SessionStack& stack);
    ~TerminalSessionController();

    TerminalSessionController(const TerminalSessionController&) = delete;
    TerminalSessionController& operator=(const TerminalSessionController&) = delete;

    // 主循环:读一行、分派一行,exit/quit 或 EOF 返回。
    void Run();

private:
    // ---- 工具全栈的别名口(ToolRuntime 在构造函数体内 emplace,
    // 引用成员绑不了,统一走这几个窄口) ----
    lubancode::tools::ToolRegistry& registry();
    lubancode::tools::ToolRegistry& sub_registry();
    lubancode::tools::AgentTool* session_agent_tool();
    const std::shared_ptr<lubancode::tools::TodoListState>& todo_state();
    const std::shared_ptr<std::set<std::string>>& loaded_tools();
    const std::vector<McpServerRuntime>& mcp_servers();
    std::optional<lubancode::lsp::Manager>& lsp_manager();
    const std::vector<PluginMountInfo>& plugin_mounted();
    const std::vector<std::string>& plugin_warnings();
    const std::function<bool(const lubancode::tools::Tool&)>& main_tool_filter();
    const std::function<bool(const lubancode::tools::Tool&)>& sub_tool_filter();

    // ---- 原先的大 lambda,逐只升成方法 ----
    void CleanupBackgroundAgents(bool dispose_queue);
    void RebuildLoop(bool preserve_history = false);
    // 会话级请求策略的同步口(批四·病十一其三:五层后端退役,/model、
    // /think、/soul 改完会话状态后把皮上的 request 档案与叠层刷新一遍,
    // 下一份请求即时生效——从前这活是传输层包装器在 send_stream 里干的)。
    void SyncAgentRequestPolicy();
    void RefreshSkills();
    void RefreshWorkflowCompletions();
    void RefreshProjectInstructions();
    // /package reload 的会话侧(统一封装单阶段 6):重折快照(折不动就
    // 一分不动)→ 原子换档 → 刷下游(技能清单/Profile 根/补全)。回执
    // 逐行带回,由命令层打印。
    std::vector<std::string> ReloadPackages();
    void PersistNewMessages();
    // 建档与开仓(第二期):建档提前到发轮前;仓跟着会话 id 开张。
    bool EnsureSessionBegun(const std::string& first_text);
    void OpenArtifactStore();
    // 外来消息轮:peer 来信是 user 语义(另一会话的用户正文);后台完成
    // 唤醒是宿主合成控制消息,传 BackgroundCompletion——检索整轮跳过,
    // 不在 trace 里留一串无意义词。
    // 双胞胎合一(会话终章):用户正文与外来消息共用 RunSessionTurn 一只
    // 回合入口,来源参数分档,原先的两只胞胎方法(RunUserTurn/RunPeerTurn)
    // 行为差异逐一保真(差异清单见实现处注释)。
    enum class TurnSource { User, Incoming };
    void RunSessionTurn(const std::string& content, TurnSource source,
                        bool* autosend_failed = nullptr, bool silent = false,
                        memory::QueryOrigin origin = memory::QueryOrigin::User);
    void PumpSteeringToSubagents();
    // 排队账落会话存档(取走即消费单路径二):queue 事件行,快照式,回放取
    // 最后一条。排队账一变(进队/送达/回还/清账)都追一份;存档没建档或
    // 已写坏就安静跳过——档是加层,不拦会话。
    void PersistSteeringQueue();
    // resume 重建队列:存档最后一条 queue 快照灌回 SessionSteeringQueue
    // (空档/没行 = 空队列,照旧)。恢复的条目保 id/次序/尝试次数。
    void RestoreSteeringQueueFrom(const std::vector<lubancode::sessions::ArchivedQueueItem>& items);
    // Ctrl+R 提问历史搜索的数据源(0.30.x 第二批):只读 session 事件账
    // (存档 JSONL 的用户提问行)拼整份 PromptHistoryDataset。
    lubancode::cli::PromptHistoryDataset CollectPromptHistory();
    // 终端标题模板:项目短名 · 分支 · 状态(0.30.x 第四批)。
    std::string BuildTerminalTitleText(const std::string& state_word) const;
    // 会话尾款 memory 接线的材料包(终端接线收尾单:三只函数在
    // commands/memory_commands,这里只递材料)。
    void SyncWorktreeDirectory();
    // 压缩接线的材料包(终端接线收尾单:/compact 正戏与自动压缩路都在
    // commands/session_commands,这里只递材料)。
    // 压缩参数的现场收集(窗口预算认压缩路由声明/目录条目;活动待办守恒)。
    lubancode::agent::CompactOptions BuildCompactOptions();
    lubancode::app::CompactSessionInputs MakeCompactInputs() {
        lubancode::app::CompactSessionInputs in;
        in.agent = &*main_agent;
        in.theme = &theme;
        in.spinner_enabled = spinner_enabled;
        in.session_compact_epoch = &session_compact_epoch;
        in.last_compact_line = &last_compact_line;
        in.persisted_count = &persisted_count;
        in.session_store = &session_store;
        in.session_store_broken = session_store_broken;
        in.hysteresis = &compact_hysteresis_;
        in.build_compact_options = [this]() { return BuildCompactOptions(); };
        in.attach_goal_snapshot = [this](lubancode::sessions::CompactV2Event& event) {
            goal_wiring_.AttachSnapshotToCompact(event);
        };
        in.attach_loop_snapshot = [this](lubancode::sessions::CompactV2Event& event) {
            lubancode::app::AttachLoopSnapshotToCompact(loop_wiring_.MakeCommandWiring(), event.metrics);
        };
        in.emit_session_hook =
            [this](lubancode::hooks::HookEvent event, nlohmann::json fields, const std::string& match_value) {
                EmitSessionHook(event, std::move(fields), match_value);
            };
        in.route_compact = [this]() { return model_router->Route(lubancode::agent::TaskKind::Compact); };
        in.route_repair = [this]() { return model_router->Route(lubancode::agent::TaskKind::CompactRepair); };
        in.normal_backend = &real_backend;
        in.current_model = current_model.get();
        in.record_usage = [this](lubancode::agent::ModelRole role, const lubancode::agent::ModelRoute& route,
                                 const lubancode::agent::BackgroundCallAccounting& accounting) {
            model_router->ledger().Record(role, route.model, accounting.usage, accounting.duration_ms,
                                          accounting.usage_reported);
        };
        in.record_fallback = [this](lubancode::agent::TaskKind kind, lubancode::agent::ModelRole from,
                                    lubancode::agent::ModelRole to, const std::string& reason) {
            model_router->ledger().RecordFallback(kind, from, to, reason);
        };
        in.persist_new_messages = [this]() { PersistNewMessages(); };
        return in;
    }
    lubancode::app::SessionTailContext MakeTailContext() {
        lubancode::app::SessionTailContext tail;
        tail.project_memory = project_memory.get();
        tail.agent = &*main_agent;
        tail.model_router = model_router.get();
        tail.prompts_dir = &prompts_dir;
        tail.artifact_store = artifact_store.get();
        tail.session_store = &session_store;
        tail.theme = &theme;
        tail.session_title = &session_title;
        tail.session_title_pending = &session_title_pending;
        tail.session_title_auto_attempted = &session_title_auto_attempted;
        return tail;
    }
    void EnsureMemoryTool();
    // 回合收尾的记忆抽取:learn 档位不在 off 才跑;失败只打一行,不影响
    // 主会话(用户基调 1/3:分型总结 + 检索扩展词)。
    // 会话起名(模型分工第一期,cheap 角色):新会话首轮收尾或 resume 进来
    // 一场没标题的旧档时,拿开头几条消息起一枚短标题,成功落 title 事件;
    // 失败安静降级(/sessions 继续用首句摘要)。一场只试一次,不追着重试。
    // context_read(summarize=true) 的按需摘要口:独占 cheap backend 读
    // artifact 真本,结果由工具回执追加在历史尾部,不追改旧消息。
    // autosend_failed(可空出参):这一行若是普通正文回合且以请求失败收场
    // (RunTurnResult.status != 0,含异常兜底),写给 true。会话泵的"排队
    // 消息自动发送失败退还"判定就吃这个——不空口猜,拿 RunTurn 真给的
    // 失败信号。
    CommandFlow ProcessLine(const std::string& content, bool* autosend_failed = nullptr);
    // slash 分派(命令注册制,会话终章):47 案 switch 已换成命令注册表
    // (commands/command_registry),这里只装材料(SlashDispatchContext,构造
    // 尾一次配齐)并递给查表路由;路由与门在 DispatchSessionSlashCommand。
    CommandFlow DispatchSlashCommand(const lubancode::cli::ParsedSlashCommand& parsed);
    void AssembleDispatchContext();
    // /workflow run 的 tool 执行器装配(骨架拆解批一·封暗道):工具节点走
    // agent::RunOneTool 正门,PreToolUse/PostToolUse 钩子、Plan 闸、逐枚
    // trace 与主回合同一条链,两处装配点(/workflow run 与 alias 直呼)共用。
    lubancode::workflow::ToolExecutor::Options BuildWorkflowToolOptions();
    // /workflow 的 agent 节点审批口:复用主回合的 ConfirmToolUse——档位
    // 映射、settings.local 前缀账、"总是允许"落会话账、三档菜单全同一套,
    // 不为 workflow 另造第二颗确认脑袋。tool 节点的确认门也借这一份。
    lubancode::agent::TurnWiring BuildWorkflowAgentCallbacks();
    // loop 单:compact 事件衡接 active loop 摘要(守恒面:task id/
    // prompt hash/间隔/状态/下一拍时间;不抄全 tick 日志进
    // summary,单子"tick 前走现有自动 compact 水位检查" + 摘要守恒)。
    // goal 生命周期进 hook 分发(goal 单"Hooks"节):全部只给审计与
    // additionalContext,没有 permission_decision(单子:Hook 不可直接写
    // Achieved)。fields 带 goal_id/revision 一类对账字段。
    // 状态栏的 goal/loop 段(goal 单合流):"goal <短码>·iter<N> · loop×<N>
    // next <差>"。两样都没有给空串(整段不挂)。短码不做状态机翻译的
    // 第二处——从 GoalState 现折,loop 用 scheduler 快照。
    // 子代理回流进 goal 的账(goal 单"预算与刹车/子代理"节):后台子代理
    // 完成时,它的结果折一枚二级证据(ToolResult,producer 标 subagent、
    // facts 带子任务 id——单子:仅"子代理说通过了"仍是二级证据,hard gate
    // 不凭它放行 achieved),usage 折进 goal 的 usage 账(子代理由该
    // iteration 派生,其消耗归 goal)。有 goal 在跑才记,没有零影响。
    // scheduler 攒的事件账落盘(append+flush);失败即 FailStore 熔断。
    // loop 事件 -> ServerEvent 投影(loop 单遗留:EventSink 面已立,这里
    // 灌)。sink 没挂零影响(终端老路);挂了(JsonEventSink/app-server)
    // 前端凭 payload 画状态栏与任务行。
    // /resume 后从存档 loop 事件账回放重建(默认 paused-on-resume 不自动
    // 烧 token,用户 /loop resume 显式续)。
    // ---- 上下文压缩的会话现场路(0.27.x 分层压缩第一期) ----
    // 压缩参数现场收集:窗口预算认压缩模型自己的目录条目,活动待办(未完
    // 成 todo 条目原文)进守恒校验——摘要漏一项 pending 就拒收,历史不动。
    // 自动(外层用户消息前)与 mid-turn(工具循环边界)压缩共用的一条路:
    // 压缩 → 校验 → 换历史 → 落盘事件 → 报数。midturn=true 时先补落盘再换
    // 史,这一轮攒下的消息先全量进 JSONL,真账一字不丢。
    // AgentLoop 每次模型请求前的压力通报:projected overflow 在安全点收一
    // 次历史;TrimHistory 真丢了东西就显式告警,不静默降级。
    SessionCommandState MakeSessionCommandState();
    // hooks 框架第四五步:会话级事件发射(SessionStart 各来源/SessionEnd
    // 各原因/Pre/PostCompact)。没配该事件的 hooks 就空操作。
    void EmitSessionHook(lubancode::hooks::HookEvent event, nlohmann::json fields, const std::string& match_value);


    // ---- 借用:调用方在 RunInteractiveSession 返回前保证存活 ----
    const InteractiveSessionOptions& opts_;

    // 分派材料包(命令注册制,会话终章):构造尾一次配齐,域 handler 全经
    // 它取料,不摸控制器本体。
    lubancode::app::SlashDispatchContext dispatch_ctx_;

    // ---- 组合根装配件(会话终章):材料/后端栈/工具全栈在组合根装好
    //(cli_app 调 BuildSessionStack),控制器只收——下列引用别名指进
    // stack_,名字沿用原局部变量,方法体原样。会话级真值(theme/config/
    // session_store)里 theme 与存档留本类,config 经别名指 stack_ 的唯一
    // 一份。 ----
    SessionStack& stack_;
    lubancode::config::ConfigResult& config_result;  // stack_ 那份(唯一真值)
    lubancode::config::Config& config;
    const lubancode::cli::Theme& theme;
    bool auto_confirm;
    std::string persona;
    bool spinner_enabled;
    const lubancode::config::ModelCatalog& model_catalog;
    const lubancode::config::SettingsLocal& settings_local;

    // ---- 提示词材料/后端栈/工具全栈(别名,本体在 stack_) ----
    const std::optional<std::string>& home_dir;
    const std::optional<std::string>& official_skills_dir;
    std::vector<lubancode::tools::SkillMeta>& skills;
    std::string& skills_segment;
    const std::optional<std::string>& home_lubancode;
    const std::string& prompts_dir;
    std::shared_ptr<lubancode::memory::ProjectMemory>& project_memory;
    std::string& project_instructions;
    const std::filesystem::path& global_skills_root;
    const std::filesystem::path& project_skills_root;
    RebuildableBackend& real_backend;
    const std::shared_ptr<std::string>& current_model;
    const std::shared_ptr<std::string>& current_think;
    std::string& active_provider;
    std::unique_ptr<lubancode::app::ModelRouterService>& model_router;
    std::shared_ptr<lubancode::agent::ContextArtifactStore>& artifact_store;
    const std::shared_ptr<std::string>& current_model_instructions;
    std::string& current_soul_name;
    const std::shared_ptr<std::string>& current_soul;
    lubancode::cli::SpinnerBackend& wrapped_backend;
    lubancode::cli::ContextTracker& context_tracker;
    lubancode::cli::WorktreeSession& worktree_session;
    std::optional<lubancode::app::ToolRuntime>& tool_runtime_;
    bool main_deferral;
    bool sub_deferral;
    int tool_search_threshold;

    // ---- UI 状态 ----
    // transcript 接线收尾单:导航/查看态/条目账/展开档全归这只控制器
    // (cli/transcript_controller),本类只装配钩子(查看态视口、横幅重画、
    // ESC 急停、活历史、轮视图存档)并转发按键。条目账本体在它那——
    // ToolDisplay/RunTurn/通知入账共用同一份(transcript_ui_.items())。
    lubancode::cli::TranscriptUiController transcript_ui_;
    // 子代理面板 presenter(终端接线收尾单自大类搬出):条目列表与查看态
    // 视口的数据行在这拼(台账缓存跟实例走),导航坞布局/条目状态机是
    // 已拆好的 cli/agent_panel 组件。
    lubancode::app::AgentPanelPresenter agent_panel_presenter_;
    // @ 提及支件(会话终章自大类搬出):索引缓存 + 提交前校验/账单。
    MentionSupport mention_support_;

    // ---- 主 AgentLoop 与轮次材料 ----
    lubancode::agent::PromptOptions prompt_options;
    // Package 快照镜像(阶段 6):命令面 ctx.package_mount 借用的那份账的
    // 拥有者——reload 换档时先换镜像再重指 ctx,借用在会话内永不悬垂。
    // 与 stack_.package_snapshot(原子槽)同折同换,由 ReloadPackages 维护。
    std::shared_ptr<const lubancode::package::PackageSnapshot> package_snapshot_view_;
    std::function<void()> reapply_peer_inbox;  // loop 重建后重灌收件点
    // loop 持 registry 引用,声明在后 = 先死,引用不悬垂。
    std::optional<lubancode::agent::Agent> main_agent;
    // P6:本体在 SessionRuntime.always_allowed(),这里引用别名(按 a 落
    // 进来的同一本账,远端审批 accept_for_session 也写它)。
    std::set<std::string>& always_allowed_tools;
    std::optional<std::string> config_file_path;  // /model、/language 可写回配置文件路径

    // ---- 会话存档与权限账(P6:本体在 runtime::SessionRuntime,这里引用) ----
    // runtime 声明在前(先析构引用别名,本体后析构),引用一律指它。
    lubancode::runtime::SessionRuntime session_runtime_;
    // 逐枚追踪单:canonical 工具事件的分线器(持久栅栏落 session、UI 投影
    // 待接 EventSink、录制投影由 RunTurn 挂)。与 session_runtime_ 同寿命。
    // 逐枚追踪单:hub 要抓 session_runtime_ 的 ids/store 引用,构造体里
    // 安家(初始化列表里绑引用不稳,成员序也保证不了先 runtime 后 hub)。
    std::optional<lubancode::runtime::ToolTraceHub> trace_hub_;
    // 事件流(骨架拆解批二:装配点配 sink 列表):会话级分线器。终端账本
    // (TerminalEventSink)先挂一只——事件照单全收逐条记账,渲染接线随
    // 显示剥离后续批次挂;app-server/脚本桥那路(JsonEventSink)以后往这
    // 串。SessionRuntime(TurnEventAdapter)与 TraceHub 的投影都落这里。
    // 声明序保证:fanout 先死、账本后死、再 hub、再 runtime(持裸指针的
    // 先退场)。
    lubancode::runtime::TerminalEventSink terminal_event_ledger_;
    lubancode::runtime::FanoutEventSink session_events_;
    // 每轮的 TurnView 存档(终端回合视觉收束单):Ctrl+L/resume 重放走
    // 同一颗渲染器,与实时画面同账。最近 N 轮,不无界攒。
    std::vector<lubancode::runtime::TurnView> turn_views_;
    static constexpr std::size_t kMaxArchivedTurnViews = 8;
    std::string wire_str;
    const std::string& sessions_dir;
    lubancode::sessions::SessionStore& session_store;
    lubancode::sessions::SessionMeta& session_meta;  // /export 用;Begin/resume 时填
    std::string session_start_ts;
    // session_meta 的构造绑定(引用成员):在初始化列表里接 runtime 那份。
    std::size_t& persisted_count;       // history 里前多少条已经落过盘
    int& session_compact_epoch;         // 本场第几次压缩(v2 事件记序;resume 接旧账)
    bool& session_store_broken;         // 建档失败过,别每轮都再撞一次
    std::string& session_title;         // /title 设的标题;resume 时取存档里最后一条
    bool& session_title_pending;        // 建档前设了标题,建档成功后补写事件行
    bool session_title_auto_attempted = false;  // cheap 起名只试一次,失败不追着重试
    // 最近一次 compact 的台账(第四期 /context"最近一次 compact 所用角色、
    // 模型、前后 token、耗时和校验结果"):一行人话,由压缩路径写。
    std::string last_compact_line;
    // 压缩滞回的会话活账(P1-1 连环压缩):上次压缩收口(成功/反涨拒收/
    // 失败)的压力口径估算记在这,自动触发先过滞回带,同一轮无进展不连压。
    lubancode::app::CompactHysteresis compact_hysteresis_;

    const std::filesystem::path recordings_root;

    // ---- 排队消息 ----
    // 0.28.x:排队消息住会话层 SteeringQueue(cli/queue_model.hpp 的
    // SessionSteeringQueue)——流式监听线程只提交编辑动作,投递由会话泵
    // (PumpSteeringQueue,循环顶/轮次边界)执行。

    // ---- 子系统接线器(会话终章) ----
    // goal/loop/plan/peer/录制各一只:状态+装配+泵+存档恢复归接线器,
    // 控制器持句柄调;会话级状态(theme/config/session_store)留本类。
    // idle_wakes 是会话级的(子代理与 loop 两路并存),loop 接线器借去挂源。
    lubancode::runtime::IdleWakeCoordinator idle_wakes_;
    lubancode::runtime::IdleWakeCoordinator::Subscription subagent_wake_token_;
    GoalSessionWiring goal_wiring_;
    LoopSessionWiring loop_wiring_;
    PlanSessionWiring plan_wiring_;
    PeerSessionWiring peer_wiring_;
    RecordSessionWiring record_wiring_;
    // 泵的公平仲裁(goal 分流合流):goal ready continuation 与 due loop
    // tick 共用一只泵(单飞),PumpNextWork 按优先级 + 公平账定谁走。
    bool PumpScheduledWork();

    // ---- 杂项 ----
    // 项目配置若显式钉了 active_provider,后续切换继续写回项目;没钉就
    // 记全局"上次使用",跨目录也能沿用。
    const std::optional<std::string> active_provider_write_path;
    // 后台任务 detached registry 的注册时点快照(原先 lambda 按值捕获,
    // 这里照抄成成员,后续 /skill 安装不追进来)。
};

}  // namespace lubancode::app
