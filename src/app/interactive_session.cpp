// TerminalSessionController(原 InteractiveSession,显示系统剥离单第六步
// 更名):交互会话主循环的终端控制器。原先 main.cpp 的 InteractiveLoop
// 里那一把局部变量与大 lambda,全收成这里的成员与方法;头文件
// (interactive_session.hpp)只露 InteractiveSessionOptions 与
// RunInteractiveSession()。
//
// 寿命规矩写在成员声明旁:拥有者先声明(后析构),借用者后声明(先析
// 构)——AgentLoop 持 backend/registry 引用,必须先死;registry 背后的
// ToolRuntime 后死;peer 工具持 PeerRuntime 引用,PeerRuntime 又要活得比
// loop 久不了(见各成员注释)。回调注册(UI 面板、收件点)与 peer 起停
// 由构造函数开门、析构函数关门,异常退场也走同一条路。
//
// 依赖只认 agent/api/cli/config/memory/mcp/lsp/tools/platform 与 app
// 装配层;不反被任何层 include。

#include "app/interactive_session.hpp"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <deque>
#include <expected>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <iterator>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>
#include <nlohmann/json.hpp>
#include "agent/artifact_store.hpp"
#include "agent/compact.hpp"
#include "agent/context_budget.hpp"
#include "agent/loop.hpp"
#include "peers/peer_session.hpp"
#include "agent/prompts.hpp"
#include "sessions/session_store.hpp"
#include "skills/workflow_recorder.hpp"
#include "api/backend.hpp"
#include "api/models.hpp"
#include "app/backend_stack.hpp"
#include "app/commands/command_registry.hpp"  // 命令注册制:47 案分派的注册表与路由
// 子系统接线器(会话终章):goal/loop/plan/peer/录制各一只(状态+装配+
// 泵+存档恢复),控制器持句柄调;会话级状态(theme/config/session_store)
// 留控制器。
#include "app/wirings/goal_session_wiring.hpp"
#include "app/wirings/loop_session_wiring.hpp"
#include "app/wirings/peer_session_wiring.hpp"
#include "app/wirings/plan_session_wiring.hpp"
#include "app/wirings/record_session_wiring.hpp"
#include "app/runtime_profile.hpp"
#include "app/tool_runtime.hpp"
#include "app/agent_panel_presenter.hpp"
#include "app/commands/memory_commands.hpp"
#include "app/commands/model_commands.hpp"
#include "app/commands/trace_commands.hpp"
#include "app/hook_runtime.hpp"
#include "app/turn_runner.hpp"
#include "app/commands/session_commands.hpp"
#include "app/commands/prompt_commands.hpp"
#include "app/commands/settings_commands.hpp"
#include "app/commands/workspace_commands.hpp"
#include "app/commands/hook_commands.hpp"
#include "app/commands/peer_commands.hpp"
#include "app/commands/doctor_commands.hpp"
#include "app/commands/workflow_commands.hpp"
#include "workflow/host_executors.hpp"
#include "app/version.hpp"
#include "runtime/command_service.hpp"
#include "runtime/event_sinks.hpp"
#include "runtime/plan_mode.hpp"
#include "runtime/session_runtime.hpp"
#include "runtime/tool_trace_hub.hpp"
// 持久目标单:goal 状态机(coordinator)、GoalContext 注入、终端排版。
#include "app/commands/goal_commands.hpp"
#include "runtime/goal_context.hpp"
#include "runtime/goal_compact.hpp"
#include "runtime/goal_coordinator.hpp"
#include "runtime/goal_evaluator.hpp"
#include "runtime/goal_evidence.hpp"
#include "tools/goal_checkpoint_tool.hpp"
#include "tools/loop_control_tool.hpp"
// loop 单:会话定时循环(scheduler、空闲唤醒多路、终端排版)。
#include "app/commands/loop_commands.hpp"
#include "runtime/idle_wake.hpp"
#include "runtime/loop_scheduler.hpp"
#include "runtime/loop_types.hpp"
#include "runtime/session_work_scheduler.hpp"
#include "hooks/hash.hpp"  // Sha256Hex:PlanDocument 内容锚
#include "cli/agent_panel_host.hpp"
#include "cli/console_input.hpp"
#include "cli/context_tracker.hpp"
#include "cli/diff.hpp"
#include "cli/divider.hpp"
#include "cli/format_utils.hpp"
#include "cli/line_editor.hpp"  // DisplayWidthUtf8:查看帧折行记账
#include "cli/i18n.hpp"
#include "cli/live_transcript.hpp"
#include "cli/worktree.hpp"
#include "cli/markdown.hpp"
#include "cli/keymap.hpp"
#include "cli/mention_menu.hpp"
#include "cli/record_command.hpp"
#include "cli/slash_commands.hpp"
#include "cli/terminal_port.hpp"
#include "cli/spinner.hpp"
#include "cli/spinner_backend.hpp"
#include "cli/terminal_frame.hpp"
#include "cli/theme.hpp"
#include "cli/turn_renderer.hpp"
#include "cli/todo_render.hpp"
#include "cli/tool_display.hpp"
#include "cli/transcript.hpp"
#include "cli/transcript_controller.hpp"
#include "config/config.hpp"
#include "config/model_catalog.hpp"
#include "config/provider_catalog.hpp"
#include "config/prompt_files.hpp"
#include "config/project_instructions.hpp"
#include "config/skill_store.hpp"
#include "lsp/manager.hpp"
#include "memory/memory_tool.hpp"
#include "memory/project_memory.hpp"
#include "app/memory_extract.hpp"
#include "app/model_router.hpp"
#include "app/session_title.hpp"
#include "mcp/client.hpp"
#include "mcp/mcp_tool.hpp"
#include "tools/agent_tool.hpp"
#include "tools/ask_user.hpp"
#include "tools/background_output.hpp"
#include "tools/background_tasks.hpp"
#include "tools/command_safety.hpp"
#include "tools/context_tools.hpp"
#include "tools/edit_file.hpp"
#include "tools/hooks.hpp"
#include "tools/lua_tool.hpp"
#include "tools/path_utils.hpp"
#include "tools/plugin_loader.hpp"
#include "tools/lsp_tool.hpp"
#include "tools/list_sessions_tool.hpp"
#include "tools/read_file.hpp"
#include "tools/registry.hpp"
#include "tools/run_command.hpp"
#include "tools/search.hpp"
#include "tools/send_session_message_tool.hpp"
#include "tools/skill_loader.hpp"
#include "tools/skill_tool.hpp"
#include "tools/todo_tool.hpp"
#include "tools/tool_search.hpp"
#include "tools/web_fetch.hpp"
#include "tools/web_search.hpp"
#include "tools/write_file.hpp"
#include "platform/console.hpp"
#include "platform/terminal_batch.hpp"
#include "platform/paths.hpp"
#include "platform/clipboard.hpp"

namespace lubancode::app {

using lubancode::app::kVersion;
using lubancode::platform::CurrentDirUtf8;
using lubancode::cli::tr;
using lubancode::cli::trf;
// 终端接线收尾单:本文件(会话控制器)的 stdout/stderr 写全走输出端口
// (TermOut/TermErr),散打清零——改道与锁规矩见 cli/terminal_port.hpp。
using lubancode::cli::TermOut;
using lubancode::cli::TermErr;
// 会话层排队消息账本(0.28.x):流式监听线程落队、会话泵投递,共用这一只。
using lubancode::cli::SessionSteeringQueue;

namespace {

// project memory 的装配:身份解析 + worker 起动,失败只打警告不拦会话。
// 构造函数初始化列表里用,保持原先"横幅之前完成"的次序。
std::shared_ptr<lubancode::memory::ProjectMemory> BuildProjectMemory(
    const lubancode::config::Config& config, const std::optional<std::string>& home_lubancode,
    const std::string& executable) {
    std::shared_ptr<lubancode::memory::ProjectMemory> project_memory;
    if (home_lubancode.has_value()) {
        auto identity = lubancode::memory::ResolveProjectIdentity(
            std::filesystem::current_path(), lubancode::tools::Utf8ToPath(*home_lubancode));
        if (identity.has_value()) {
            project_memory = std::make_shared<lubancode::memory::ProjectMemory>(
                std::move(*identity), lubancode::tools::Utf8ToPath(*home_lubancode),
                MemoryOptionsFromConfig(config.memory), executable);
            if (project_memory->generate_enabled()) {
                if (const auto launched = project_memory->LaunchWorker(); !launched.has_value()) {
                    TermOut() << trf("cmd.memory.worker_failed", launched.error()) << "\n";
                }
            }
        } else if (config.memory.enabled) {
            TermOut() << trf("cmd.memory.project_failed", identity.error()) << "\n";
        }
    }
    return project_memory;
}

}  // namespace

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
    explicit TerminalSessionController(const InteractiveSessionOptions& options);
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
    lubancode::app::ToolRuntime::Options MakeRuntimeOptions();

    // ---- 原先的大 lambda,逐只升成方法 ----
    void CleanupBackgroundAgents(bool dispose_queue);
    void RebuildLoop(bool preserve_history = false);
    // 会话级请求策略的同步口(批四·病十一其三:五层后端退役,/model、
    // /think、/soul 改完会话状态后把皮上的 request 档案与叠层刷新一遍,
    // 下一份请求即时生效——从前这活是传输层包装器在 send_stream 里干的)。
    void SyncAgentRequestPolicy();
    void RefreshSkills();
    void RefreshProjectInstructions();
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
    // @ 提及(0.30.x 第三批):文件索引(按根缓存)与提交前校验/账单。
    std::vector<lubancode::cli::FileMentionEntry> FileMentionIndexSnapshot();
    // 终端标题模板:项目短名 · 分支 · 状态(0.30.x 第四批)。
    std::string BuildTerminalTitleText(const std::string& state_word) const;
    // 返回:第一段是错误(非空 = 拦下这一轮不发送),第二段是给模型的
    // 提及账(空 = 没有)。
    std::pair<std::string, std::string> BuildMentionLedger(const std::string& content);
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
    lubancode::tools::DetachedAgentBackend BuildDetachedBackend() const;
    std::unique_ptr<lubancode::tools::ToolRegistry> BuildDetachedRegistry() const;


    // ---- 借用:调用方在 RunInteractiveSession 返回前保证存活 ----
    const InteractiveSessionOptions& opts_;

    // 分派材料包(命令注册制,会话终章):构造尾一次配齐,域 handler 全经
    // 它取料,不摸控制器本体。
    lubancode::app::SlashDispatchContext dispatch_ctx_;

    // ---- 配置副本与会话标量(名字沿用原局部变量,方法体原样) ----
    lubancode::config::ConfigResult config_result_;
    lubancode::config::Config& config;
    const lubancode::cli::Theme& theme;
    bool auto_confirm;
    std::string persona;
    bool spinner_enabled;
    const lubancode::config::ModelCatalog& model_catalog;
    const lubancode::config::SettingsLocal& settings_local;

    // ---- 技能/提示词材料 ----
    const std::optional<std::string> home_dir;
    const std::optional<std::string> official_skills_dir;
    std::vector<lubancode::tools::SkillMeta> skills;
    std::string skills_segment;
    const std::optional<std::string> home_lubancode;
    const std::string prompts_dir;
    std::shared_ptr<lubancode::memory::ProjectMemory> project_memory;
    std::string project_instructions;
    const std::filesystem::path global_skills_root;
    const std::filesystem::path project_skills_root;

    // ---- 后端栈(骨架拆解批四:五层请求改写后端退役,归 RequestProfile
    // 管道——model/effort 走皮上的 request 档案,模型指令/魂/延迟索引是皮
    // 上的叠层字段,由 Agent 拼请求时就地生效;/model、/think、/soul 的
    // 会话级同步走 SyncAgentRequestPolicy。栈里只剩真实 client 的稳定壳与
    // spinner)----
    RebuildableBackend real_backend;
    std::shared_ptr<std::string> current_model;
    std::shared_ptr<std::string> current_think;
    std::string active_provider;
    // 统一模型路由(模型分工第一期):compact/记忆抽取/标题这类后台小活按
    // TaskKind 从这里取"模型+effort+backend",不再各自拼 compact_model 这类
    // 散装字符串。指针成员:构造函数体内 real_backend/current_model 落定后
    // 再建(引用成员绑不了构造顺序),会话生命周期内唯一。
    std::unique_ptr<lubancode::app::ModelRouterService> model_router;
    // 渐进式上下文仓(第二期):超长工具结果落 blobs/chunks/index,模型凭
    // artifact_id 用 context_search/context_read 追回全文。shared_ptr:两把
    // 工具持同一块内存,会话建档那一刻才 Open,没开的仓一切操作安全退化。
    std::shared_ptr<lubancode::agent::ContextArtifactStore> artifact_store;
    std::shared_ptr<std::string> current_model_instructions;
    std::string current_soul_name;
    std::shared_ptr<std::string> current_soul;
    // UI 件住显示层(批二自 backend_stack 挪来):装配次序不变,只是门牌
    // 从传输层换到 cli。
    lubancode::cli::SpinnerBackend wrapped_backend;
    lubancode::cli::ContextTracker context_tracker;

    // ---- 工具全栈(worktree_session 必须先于 ToolRuntime:worktree 工具
    // 持它的引用;ToolRuntime 在构造体内横幅之后 emplace,保住原先
    // "先横幅后 [mcp] 挂载行"的输出次序) ----
    lubancode::cli::WorktreeSession worktree_session;
    std::function<void()> after_worktree_moved;  // 晚绑定槽:loop 建好后由构造体填
    std::optional<lubancode::app::ToolRuntime> tool_runtime_;
    bool main_deferral = false;
    bool sub_deferral = false;
    int tool_search_threshold = 0;

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
    // @ 提及文件索引(第三批):按根缓存,根变了重扫(cwd/worktree 切换)。
    std::vector<lubancode::cli::FileMentionEntry> mention_index_;
    std::string mention_index_root_;

    // ---- 主 AgentLoop 与轮次材料 ----
    lubancode::agent::PromptOptions prompt_options;
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
    const std::vector<lubancode::tools::SkillMeta> detached_skills_;
    const lubancode::config::SearchConfig detached_search_;
};

lubancode::tools::ToolRegistry& TerminalSessionController::registry() { return tool_runtime_->main_registry(); }
lubancode::tools::ToolRegistry& TerminalSessionController::sub_registry() { return tool_runtime_->sub_registry(); }
lubancode::tools::AgentTool* TerminalSessionController::session_agent_tool() { return tool_runtime_->agent_tool(); }
const std::shared_ptr<lubancode::tools::TodoListState>& TerminalSessionController::todo_state() {
    return tool_runtime_->todo_state();
}
const std::shared_ptr<std::set<std::string>>& TerminalSessionController::loaded_tools() {
    return tool_runtime_->loaded_tools();
}
const std::vector<McpServerRuntime>& TerminalSessionController::mcp_servers() { return tool_runtime_->mcp_servers(); }
std::optional<lubancode::lsp::Manager>& TerminalSessionController::lsp_manager() { return tool_runtime_->lsp_manager(); }
const std::vector<PluginMountInfo>& TerminalSessionController::plugin_mounted() {
    return tool_runtime_->plugin_mounted();
}
const std::vector<std::string>& TerminalSessionController::plugin_warnings() {
    return tool_runtime_->plugin_warnings();
}
const std::function<bool(const lubancode::tools::Tool&)>& TerminalSessionController::main_tool_filter() {
    return tool_runtime_->main_tool_filter();
}
const std::function<bool(const lubancode::tools::Tool&)>& TerminalSessionController::sub_tool_filter() {
    return tool_runtime_->sub_tool_filter();
}

lubancode::workflow::ToolExecutor::Options TerminalSessionController::BuildWorkflowToolOptions() {
    // 封暗道(骨架拆解批一·病二):workflow 工具节点不再直捅
    // tool->execute(),改走 agent::RunOneTool 正门。这里把主回合那套横切
    // 链原样接上:hooks(PreToolUse/PostToolUse)、Plan 闸、逐枚 trace
    //(发号 + 分线 + 副作用闸)。确认门暂不接(会话层 /workflow 还没有
    // 审批宿主),ToolExecutor 自守"needs_confirm 无门明拒"的旧语义。
    lubancode::workflow::ToolExecutor::Options options;
    options.registry = &registry();
    options.thread_id = session_runtime_.thread_id();
    if (trace_hub_.has_value()) {
        lubancode::runtime::ToolTraceHub* hub = &*trace_hub_;
        options.execution_id_issuer = [hub] { return hub->NextExecutionId(); };
        options.callbacks.on_tool_trace = [hub](const lubancode::agent::ToolTraceEvent& event) {
            hub->OnTrace(event);
        };
        options.callbacks.on_tool_trace_blocked = [hub](const std::string& execution_id) {
            return hub->IsExecutionBlocked(execution_id);
        };
    }
    lubancode::hooks::HookDispatcher* dispatcher = lubancode::app::HookRuntime();
    if (dispatcher != nullptr && lubancode::runtime::HasToolHooks(dispatcher)) {
        options.callbacks.on_pre_tool_use_hook =
            [dispatcher](const std::string&, const std::string& name, const nlohmann::json& input) {
                return lubancode::runtime::EmitPreToolUse(dispatcher, name, input);
            };
        options.callbacks.on_post_tool_use_hook =
            [dispatcher](const std::string&, const std::string& name, const nlohmann::json& input,
                         const lubancode::tools::Tool::Result& result) {
                return lubancode::runtime::EmitPostToolUse(dispatcher, name, input, result);
            };
    }
    // Plan 闸:workflow 工具节点同过 ModePolicy(单子:不另开旁路)。
    options.callbacks.on_mode_policy = [this](const std::string& tool_name, const nlohmann::json& input) {
        return plan_wiring_.EvaluateGate(tool_name, input);
    };
    return options;
}

lubancode::app::ToolRuntime::Options TerminalSessionController::MakeRuntimeOptions() {
    lubancode::app::ToolRuntime::Options runtime_options;
    runtime_options.with_explore = true;
    runtime_options.with_ask_user = spinner_enabled;
    runtime_options.ask_user_handler = [this](const lubancode::tools::AskUserQuestion& question) {
        return PromptAskUser(question, theme);
    };
    runtime_options.memory = project_memory;
    runtime_options.worktree_session = &worktree_session;
    // worktree 工具的两道硬确认(进园子外的房、脏房强删)走自己的问话通道,
    // 不经三档确认——确认档压不住这一问,管道模式没人可问就拒。
    runtime_options.worktree_confirm = [this](const std::string& question) -> std::optional<bool> {
        if (!lubancode::platform::StdinIsInteractive() ||
            !lubancode::platform::ProbeStdoutConsole().is_console) {
            return std::nullopt;
        }
        const lubancode::cli::StreamFooterSuspendScope footer_suspend;
        const auto answer = lubancode::cli::ReadLine(theme.confirm + question + theme.reset, theme,
                                                     /*esc_rejects=*/true);
        return answer.has_value() && (*answer == "y" || *answer == "Y");
    };
    runtime_options.on_worktree_moved = [this]() {
        if (after_worktree_moved) {
            after_worktree_moved();
        }
    };
    return runtime_options;
}

TerminalSessionController::TerminalSessionController(const InteractiveSessionOptions& options)
    : opts_(options),
      config_result_(options.config_result),
      config(config_result_.config),
      theme(options.theme),
      transcript_ui_(theme),
      agent_panel_presenter_(theme),
      auto_confirm(options.auto_confirm),
      persona(options.persona),
      spinner_enabled(options.spinner_enabled),
      model_catalog(options.model_catalog),
      settings_local(options.settings_local),
      home_dir(lubancode::config::HomeDir()),
      official_skills_dir(lubancode::platform::OfficialSkillsDir()),
      skills(lubancode::tools::LoadSkills(CurrentDirUtf8(), home_dir, official_skills_dir)),
      skills_segment(lubancode::tools::BuildSkillsPromptSegment(skills)),
      home_lubancode(lubancode::config::HomeLubancodeDir()),
      prompts_dir(home_lubancode.has_value() ? (*home_lubancode + "/prompts") : std::string()),
      project_memory(BuildProjectMemory(config, home_lubancode, options.executable)),
      project_instructions(
          lubancode::config::LoadProjectInstructions(std::filesystem::current_path()).content),
      global_skills_root(home_lubancode.has_value()
                             ? lubancode::tools::Utf8ToPath(*home_lubancode) / "skills"
                             : std::filesystem::path()),
      project_skills_root(lubancode::tools::Utf8ToPath(CurrentDirUtf8()) / ".lubancode" / "skills"),
      real_backend(config),
      current_model(std::make_shared<std::string>(config.model)),
      current_think(std::make_shared<std::string>(config.think)),
      artifact_store(std::make_shared<lubancode::agent::ContextArtifactStore>()),
      current_model_instructions(std::make_shared<std::string>()),
      current_soul_name(config.soul.empty() ? "default" : config.soul),
      current_soul(std::make_shared<std::string>(LoadSoulContentByName(current_soul_name, /*warn=*/true))),
      wrapped_backend(real_backend, theme, spinner_enabled),
      context_tracker(config.context_window_tokens),
      config_file_path(config_result_.config_file_path),
      always_allowed_tools(session_runtime_.always_allowed()),
      // P6:存档账本体在 SessionRuntime;引用别名在此初始化列表里绑过去
      //(wire_str 先落值,runtime 的 Options 要吃它)。
      session_runtime_([&] {
          lubancode::runtime::SessionRuntime::Options runtime_options;
          runtime_options.sessions_dir =
              home_lubancode.has_value() ? (*home_lubancode + "/sessions") : std::string();
          runtime_options.wire_name = lubancode::config::ProviderWireName(config.wire);
          runtime_options.start_ts = lubancode::sessions::NowIdTimestamp();
          return runtime_options;
      }()),
      wire_str(lubancode::config::ProviderWireName(config.wire)),
      sessions_dir(session_runtime_.sessions_dir()),
      session_store(session_runtime_.store()),
      session_meta(session_runtime_.meta()),
      session_start_ts(session_runtime_.start_ts()),
      persisted_count(session_runtime_.persisted_count()),
      session_compact_epoch(session_runtime_.compact_epoch()),
      session_store_broken(session_runtime_.store_broken()),
      session_title(session_runtime_.title()),
      session_title_pending(session_runtime_.title_pending()),
      recordings_root(home_lubancode.has_value() ? lubancode::tools::Utf8ToPath(*home_lubancode) / "recordings"
                                                 : std::filesystem::path()),
      active_provider_write_path(
          config_result_.sources.active_provider == lubancode::config::Source::ProjectConfigFile
              ? config_result_.project_config_file_path
              : std::nullopt),
      detached_skills_(skills),
      detached_search_(config.search) {
    // 旧单端字段和某条 provider 完全对上时，起手就把它认作当前端。这样
    // /provider list 的标记和“当前端不能删”都不留空档。
    active_provider = config.active_provider;
    if (active_provider.empty()) {
        for (const auto& provider : config.providers) {
            if (provider.wire == config.wire && provider.base_url == config.base_url &&
                provider.model == config.model) {
                active_provider = provider.name;
                break;
            }
        }
    }

    // 逐枚追踪单:trace hub 安家(抓 session_runtime_ 的 ids/store 引用;
    // 分线 canonical 工具事件到 session 栅栏/录制投影/UI 投影)。
    trace_hub_.emplace(session_runtime_.ids(), &session_runtime_.store());
    // 事件流接线(骨架拆解批二,补显示剥离第六步停下的那段):sink 列表
    // 一处配齐——终端账本先挂;SessionRuntime 的每轮 adapter 与 hub 的
    // trace 投影都落到这串。往后加消费方(app-server 直出、脚本桥)只往
    // fanout 里 Add,装配点不再各挑回调。
    session_events_.Add(&terminal_event_ledger_);
    session_runtime_.AttachSink(&session_events_);
    trace_hub_->AttachSink(&session_events_);

    // 统一模型路由(模型分工第一期):后台小活(压缩/抽取/标题)按
    // TaskKind 取路由,usage 分角色记账。配置有歧义(compact_model 与
    // cheap_model 同写之类)时把 MergeConfig 记的提示打出来——路由看得见。
    model_router = std::make_unique<lubancode::app::ModelRouterService>(config_result_, real_backend,
                                                                        current_model, active_provider);
    for (const std::string& notice : config_result_.model_role_notices) {
        TermOut() << theme.stats << "[模型路由] " << notice << theme.reset << "\n";
    }

    // 图标只在真控制台打(管道/重定向不打装饰字符,理由同 ClearScreen 的
    // spinner_enabled 判断),横幅本身不受这条限制(重定向场景下横幅这类
    // 信息性文字原样保留,现状不动)。
    if (spinner_enabled) {
        PrintLubanIcon(theme);
    }
    PrintBanner(config, theme);

    // 模型目录:启动时当前模型就在目录里,同样应用 default_think /
    // context_window / base_instructions——但用户显式配过的字段(Source
    // 不是内置默认值)不动,目录只是"该模型的出厂默认",压不过用户自己
    // 的配置。打印紧跟横幅,干了什么一眼看全。
    ApplyModelCatalog(model_catalog, *current_model,
                      /*think_explicit=*/config_result_.sources.think != lubancode::config::Source::Default,
                      /*window_explicit=*/config_result_.sources.context_window_tokens !=
                          lubancode::config::Source::Default,
                      current_think, context_tracker, current_model_instructions);

    // stream_usage 启动诊断提醒(缓存诊断单):chat wire 且没人声明过这个
    // 能力(自定义端没写、也不在目录预设里),token/缓存统计可能恒为 0。
    // 只提醒,不发请求——能力探针(/doctor cache usage)由用户显式触发,
    // 结论写回 provider 配置,下次启动这一行就闭嘴。
    if (config.wire == lubancode::config::Wire::ChatCompletions && !config.stream_usage_declared) {
        TermOut() << theme.stats << tr("doctor.startup.stream_usage_hint") << theme.reset << "\n";
    }

    // 陈房清扫(0.27.x):只清 agent- 前缀、超过 3 天没动静的隔离子代理房;
    // 有活(改动/自有提交)的跳过,锁着的先放(被杀会话留下的),用户
    // 手起的房永不碰。
    if (const auto stale_root = lubancode::cli::FindRepositoryRoot(std::filesystem::current_path())) {
        const auto cleanup = lubancode::cli::CleanStaleAgentWorktrees(*stale_root, std::chrono::hours(72));
        if (cleanup.removed > 0) {
            TermOut() << theme.stats << trf("cmd.worktree.cleaned", cleanup.removed) << theme.reset << "\n";
        }
    }

    // 工具全栈:三表 + MCP/插件/LSP/agent/todo/ask_user/memory/tool_search
    // 的装配全收进 ToolRuntime(引用寿命由成员声明顺序保住),Interactive
    // 与单发共用一套;会话可变的钩子(detached factory、prompts、过滤)
    // 在下面接着灌。模型侧 worktree 工具与 /worktree 共这一个会话实例
    // (账只有一本,一边 active 另一边回 AlreadyActive)。
    tool_runtime_.emplace(config, theme, wrapped_backend, skills, skills_segment, CurrentDirUtf8(),
                          MakeRuntimeOptions());
    // 逐枚追踪单第四期:hub 已安家,挂 undo_file_edit(条件式撤销:凭
    // hub 的账本翻凭据,走与 write/edit 同一道确认门)。
    tool_runtime_->AttachUndoTool(&*trace_hub_);
    // 可追回 artifact 的两把只读钥匙(第二期):main 与子代理同级都有。
    // main 的 context_read 另接按需摘要:模型显式写 summarize=true 才花
    // cheap token,回执自然追加在尾部。子代理只给原文读取,免并发改路由账。
    registry().Register(std::make_unique<lubancode::tools::ContextSearchTool>(artifact_store));
    registry().Register(std::make_unique<lubancode::tools::ContextReadTool>(
        artifact_store, [this](const lubancode::agent::ArtifactRef& ref) {
            return lubancode::app::SummarizeArtifactOnDemand(MakeTailContext(), ref);
        }));
    sub_registry().Register(std::make_unique<lubancode::tools::ContextSearchTool>(artifact_store));
    sub_registry().Register(std::make_unique<lubancode::tools::ContextReadTool>(artifact_store));
    // 子系统接线器(会话终章):goal/loop/plan/peer/录制各配一只 Host(全
    // 借用 + 晚绑定槽),装配与状态归接线器,控制器持句柄调。会话级状态
    //(theme/config/session_store)留本类,两边不互相摸。
    {
        GoalSessionWiring::Host goal_host;
        goal_host.theme = &theme;
        goal_host.config = &config;
        goal_host.session_store = &session_store;
        goal_host.trace_hub = &*trace_hub_;
        goal_host.model_router = model_router.get();
        goal_host.evaluation_backend = &wrapped_backend;
        goal_host.current_model = current_model;
        goal_host.agent_tool = [this]() { return session_agent_tool(); };
        goal_host.loop_scheduler = [this]() { return loop_wiring_.scheduler(); };
        goal_host.start_turn = [this](const std::string& text, bool* turn_failed) {
            RunSessionTurn(text, TurnSource::User, turn_failed);
        };
        goal_wiring_.AttachHost(std::move(goal_host));

        LoopSessionWiring::Host loop_host;
        loop_host.theme = &theme;
        loop_host.interactive = spinner_enabled;
        loop_host.config = &config;
        loop_host.session_store = &session_store;
        loop_host.session_runtime = &session_runtime_;
        loop_host.home_lubancode = &home_lubancode;
        loop_host.idle_wakes = &idle_wakes_;
        loop_host.start_turn = [this](const std::string& text, bool* turn_failed) {
            RunSessionTurn(text, TurnSource::User, turn_failed);
        };
        loop_wiring_.AttachHost(std::move(loop_host));

        PlanSessionWiring::Host plan_host;
        plan_host.theme = &theme;
        plan_host.session_runtime = &session_runtime_;
        plan_host.prompt_options = &prompt_options;
        plan_host.artifact_store = artifact_store.get();
        plan_host.main_agent = [this]() { return main_agent.has_value() ? &*main_agent : nullptr; };
        plan_host.registry = [this]() { return &registry(); };
        plan_host.agent_tool = [this]() { return session_agent_tool(); };
        plan_host.rebuild_preserving = [this]() { RebuildLoop(/*preserve_history=*/true); };
        plan_host.start_turn = [this](const std::string& text, bool* turn_failed) {
            RunSessionTurn(text, TurnSource::User, turn_failed);
        };
        plan_wiring_.AttachHost(std::move(plan_host));

        PeerSessionWiring::Host peer_host;
        peer_host.theme = &theme;
        peer_host.interactive = spinner_enabled;
        peer_host.home_lubancode = &home_lubancode;
        peer_host.session_title = [this]() { return session_title; };
        peer_host.permission_mode = [] {
            return static_cast<int>(lubancode::cli::CurrentConfirmMode());
        };
        peer_wiring_.AttachHost(std::move(peer_host));

        RecordSessionWiring::Host record_host;
        record_host.recordings_root = &recordings_root;
        record_host.project_skills_root = &project_skills_root;
        record_host.global_skills_root = &global_skills_root;
        record_host.refresh_skills = [this]() { RefreshSkills(); };
        record_wiring_.AttachHost(std::move(record_host));
    }
    // goal/loop 的窄工具(goal 单第 2 期 + loop 单第 4 期的注册欠账):注册进
    // 主表,靠 turn 级动态过滤放行——goal_checkpoint 只在 goal iteration 的
    // turn 里露面,loop_control 只在 scheduled tick 的 turn 里露面,普通轮、
    // 子代理、MCP 一概看不见(单子:动态 tool set 的 scope 语义)。状态在
    // 接线器里先造(注册要用),id 由发 turn 的那两处泵灌。
    goal_wiring_.RegisterTools(registry());
    loop_wiring_.RegisterTools(registry());
    main_deferral = tool_runtime_->main_deferral();
    sub_deferral = tool_runtime_->sub_deferral();
    tool_search_threshold = config.tool_search_threshold;
    if (session_agent_tool() != nullptr) {
        // execution_mode=auto 的缺省走向:交互会话里独立探索型任务默认后台
        // (结论稍后送达),模型非等结果不可时显式写 foreground。管道/单发
        // 不设这个,auto 等价前台。
        session_agent_tool()->SetBackgroundByDefault(true);
        // 每个后台任务各造一份 HTTP client 与基础工具表。取配置/模型/魂时
        // 正在主线程的 agent 工具调用里，拷贝完才起线程，不跨线程读这些
        // 会话可变字段。
        session_agent_tool()->SetDetachedBackendFactory([this]() { return BuildDetachedBackend(); });
        session_agent_tool()->SetDetachedRegistryFactory([this]() { return BuildDetachedRegistry(); });
        // 墙钟兜底(规格三):整轮上限从 subagent.wall_clock_timeout_secs 来
        // (项目级压全局,都没写用公开默认 1800s;0 = 不限)。哪怕接口超时
        // 全失效,任务也不无限占着坞行。
        session_agent_tool()->SetWallClockTimeout(
            config.subagent.wall_clock_timeout_secs.value_or(
                lubancode::config::kDefaultSubagentWallClockTimeoutSecs));
        // 提示词运行时化:子代理系统提示同机制(features 模块用户文件优先)。
        session_agent_tool()->SetPromptsDir(prompts_dir);
        session_agent_tool()->SetProjectInstructions(project_instructions);
        if (sub_deferral) {
            session_agent_tool()->SetToolFilter(sub_tool_filter());
            session_agent_tool()->SetDeferredIndexProvider([this]() {
                return lubancode::tools::BuildDeferredToolsIndexSegment(sub_registry(), *loaded_tools());
            });
        }
    }
    if (main_deferral) {
        TermOut() << theme.stats << trf("tool_search.enabled", tool_search_threshold) << theme.reset << "\n";
    }

    // 后台子代理面板的数据源(缓存 + 修订号,面板每 100ms 拉一次)。列表走
    // 轻量全量(TaskSummaries,不截 8 只);查看态的长正文由视图切换钩子按
    // viewed_task_id 现取,整块换进上方会话视口——导航坞只放导航。
    lubancode::cli::SessionAgentPanelHost().SetProvider(
        [this]() { return agent_panel_presenter_.Entries(session_agent_tool()); });
    lubancode::cli::SetAgentViewSwitchHook(
        [this](int viewed_task_id, int tail_rows) {
            transcript_ui_.PrintViewedTranscript(viewed_task_id, tail_rows);
        });

    // 面板动作接线(x 停止/清除、Ctrl+X Ctrl+K 两段确认停全部):只发信号/
    // 清台账,面板等任务线程报终态的那一拍自己改灯。
    lubancode::cli::AgentPanelActions panel_actions;
    panel_actions.cancel_task = [this](int task_id) {
        return session_agent_tool() != nullptr && session_agent_tool()->CancelTask(task_id);
    };
    panel_actions.clear_task = [this](int task_id) {
        return session_agent_tool() != nullptr && session_agent_tool()->ClearFinishedTask(task_id);
    };
    panel_actions.cancel_all = [this]() {
        return session_agent_tool() != nullptr ? session_agent_tool()->CancelAllTasks() : 0;
    };
    lubancode::cli::SessionAgentPanelHost().SetActions(panel_actions);

    // 刮屏驱动器专用(tests/manual/agent_panel_driver.cpp,不进 ctest):设
    // LUBANCODE_AGENT_PANEL_DEMO=N 时面板显示 N 只假代理,便于真控制台断言
    // 导航坞贴底与残帧计数。正常启动不设这个变量,provider 还是真数据。
    // LUBANCODE_AGENT_PANEL_DEMO_IDLE=K 让前 K 只处于完成态,驱动闲置折叠
    // (完成行最多单列三只,更多折成一行汇总)。
    if (const auto demo = lubancode::platform::GetEnvVar("LUBANCODE_AGENT_PANEL_DEMO");
        demo.has_value() && !demo->empty()) {
        const int demo_count = std::max(1, std::atoi(demo->c_str()));
        int demo_idle = 0;
        if (const auto idle = lubancode::platform::GetEnvVar("LUBANCODE_AGENT_PANEL_DEMO_IDLE");
            idle.has_value() && !idle->empty()) {
            demo_idle = std::min(std::max(0, std::atoi(idle->c_str())), demo_count);
        }
        lubancode::cli::SessionAgentPanelHost().SetProvider([demo_count, demo_idle]() {
            std::vector<lubancode::cli::AgentPanelEntry> fake;
            for (int i = 1; i <= demo_count; ++i) {
                lubancode::cli::AgentPanelEntry entry;
                entry.task_id = i;
                entry.name = "general-purpose #" + std::to_string(i);
                entry.title = "演示任务 " + std::to_string(i);
                entry.running = i > demo_idle;
                entry.state = entry.running ? "运行中(2 次工具调用 · 1.2k tokens · 12s)"
                                            : "完成(2 次工具调用 · 1.2k tokens · 12s)";
                fake.push_back(std::move(entry));
            }
            return fake;
        });
        // 演示钩子同样接管视图切换:假代理不在 AgentTool 台账里,查看态的
        // 头行从假条目表出,正文给一行占位——刮屏驱动器照旧只认屏面。
        lubancode::cli::SetAgentViewSwitchHook([demo_count, demo_idle, this](int viewed_task_id, int tail_rows) {
            (void)tail_rows;  // 演示代理没有实时流,重铺拍与整铺同款
            std::lock_guard<std::mutex> stdout_lock(lubancode::cli::StdoutWriteMutex());
            TermOut() << "\n";
            if (viewed_task_id == 0) {
                TermOut() << theme.stats << tr("agent_panel.main_header") << theme.reset << "\n";
                TermOut() << theme.stats << tr("agent_panel.back_to_main") << theme.reset << "\n";
            } else if (viewed_task_id >= 1 && viewed_task_id <= demo_count) {
                TermOut() << trf("agent_panel.view_header", "general-purpose #" + std::to_string(viewed_task_id),
                                 "演示任务 " + std::to_string(viewed_task_id))
                          << "\n"
                          << "  [" << (viewed_task_id > demo_idle ? "后台" : "完成") << "] 演示条目,无 transcript 台账\n";
            }
            TermOut().flush();
        });
    }

    // 后台子代理结果回流(空闲唤醒):任务在会话空闲时跑完的,不能干等用户
    // 再敲一行才送达。ReadLine 等键的 100ms 面板刷新一拍里问这里,有未投递
    // 的完成结果就让位,主循环顶另起一轮把结果交回主代理。
    // loop 单起改多路:子代理与 loop 的 due 都挂进 IdleWakeCoordinator,
    // 单枚 SetIdleWakeHook 只装"问总口"的一枚总钩,谁也不覆盖谁。
    subagent_wake_token_ = idle_wakes_.AddSource("subagent", [this]() {
        return session_agent_tool() != nullptr && session_agent_tool()->HasUndeliveredCompletions();
    });
    lubancode::cli::SetIdleWakeHook([this]() { return idle_wakes_.AnyReady(); });

    // 后台代理权限拒绝的当场告知(后台代理权限拒绝无告知单,2026-08-17):
    // 后台任务的 needs_confirm 工具被拒那一刻,AgentTool 已把一行通知推进
    // 台账;空闲 composer 的 100ms 拍在这里取走——导航坞 toast 一枚(几秒
    // 自收)+ transcript 记一条有归属的事件,用户当拍看见,不攒到最终报告。
    // 只落 toast 与台账,不打裸行:不打断 composer,查看态零扰动。
    lubancode::cli::SetBackgroundNoticeHook([this]() {
        if (session_agent_tool() == nullptr) {
            return;
        }
        const std::vector<std::string> notices = session_agent_tool()->TakePermissionDenialNotices();
        if (notices.empty()) {
            return;
        }
        for (const std::string& notice : notices) {
            lubancode::cli::ShowPanelToast(notice);
            auto& transcript = transcript_ui_.items();
            transcript.push_back(lubancode::cli::MakeNoticeItem(
                static_cast<int>(transcript.size()) + 1, tr("agent_panel.denial_notice_title"),
                lubancode::cli::TranscriptStatus::Error, {notice}));
        }
    });

    // Ctrl+R 提问历史搜索的数据源(0.30.x 第二批):只读 session 事件账,
    // 打开搜索框时取一次(范围轮换在终端层本地过滤,不反复读盘)。
    lubancode::cli::SetPromptHistoryProvider([this]() { return CollectPromptHistory(); });

    // @ 文件提及菜单的数据源(0.30.x 第三批):按 Git 根(没有就 cwd)扫
    // 一份相对路径清单,根变了才重扫;排除 .git/构建产物与隐藏目录。
    lubancode::cli::SetFileMentionProvider([this]() { return FileMentionIndexSnapshot(); });

    // -----------------------------------------------------------------------
    // UI-D(0.16.0):Ctrl+O 紧凑/详细 + 焦点导航 + Ctrl+E 聚焦查看。
    // 按键语义翻译在 LineEditorCore(composer 空不空、键是什么),转发管道
    // 在 console_input 的 SetTranscriptUiHandler。导航/查看态/重画的本体在
    // cli::TranscriptUiController(终端接线收尾单自大类搬出),这里只装钩子
    // (查看态视口/横幅重画/ESC 急停/活历史/轮视图存档)并转发按键。只在等
    // 输入时会被调(流式期间监听线程天然吞不进这些键);管道模式走不到逐键
    // 路径,整套无感。
    // -----------------------------------------------------------------------
    {
        lubancode::cli::TranscriptUiController::Hooks transcript_hooks;
        transcript_hooks.build_task_transcript = [this](int task_id, int width) {
            return agent_panel_presenter_.TaskTranscriptLines(session_agent_tool(), task_id, width,
                                                              transcript_ui_.agent_view_expanded());
        };
        transcript_hooks.repaint_banner = [this]() { PrintBanner(config, theme); };
        // ESC 急停(空闲态、composer 空):活 loop 一键全停,定义保留,
        // 续跑 /loop resume <id>。停了的账落档(FlushLoopEvents)。
        transcript_hooks.stop_active_loops = [this]() -> int { return loop_wiring_.StopAllForEsc(); };
        transcript_hooks.history = [this]() -> const std::vector<lubancode::api::Message>* {
            return main_agent.has_value() ? &main_agent->History() : nullptr;
        };
        transcript_hooks.turn_views = [this]() -> const std::vector<lubancode::runtime::TurnView>* {
            return &turn_views_;
        };
        transcript_ui_.SetHooks(std::move(transcript_hooks));
    }
    lubancode::cli::SetTranscriptUiHandler(
        [this](lubancode::cli::UiKeyAction action) -> bool { return transcript_ui_.HandleKey(action); });

    // 0.19.x 提示词模块化:系统提示按会话实际启用的能力条件拼装——
    // skills 有技能才注、mcp/web/lsp 配了才注、平台段按 wire。法(persona)
    // 非空时 core 模块让位,环境/features 段照拼。
    prompt_options.cwd = CurrentDirUtf8();
    prompt_options.persona = persona;
    prompt_options.skills_segment = skills_segment;
    prompt_options.project_instructions = project_instructions;
    prompt_options.mcp = !config.mcp_servers.empty();
    prompt_options.web = config.search.Configured();
    prompt_options.lsp = !config.lsp_servers.empty();
    prompt_options.wire = lubancode::config::ProviderWireName(config.wire);
    prompt_options.prompts_dir = prompts_dir;  // 运行时模块:拼装时现读现拼

    // 跨会话传话(0.25.x 同机首版):loop 每次 rebuild(/clear、/model、
    // provider 切换)都会 emplace 重来,安全收件点(SetInbox)得跟着重灌。
    // 这里先挂一个可空的重灌钩子,PeerRuntime 起来之后再填实(见下)。
    RebuildLoop();

    std::set<std::string>& allowed = always_allowed_tools;
    // settings.local.json 的 allow_tools:启动即注入会话"总是允许"集合,这些
    // 工具本会话直接免确认(跟按 a 落进来的是同一个集合)。
    for (const std::string& tool_name : settings_local.allow_tools) {
        allowed.insert(tool_name);
    }

    // -----------------------------------------------------------------------
    // 会话存档(0.13.x):每轮结束把 history 里新增的消息逐条追加写
    // <主目录>/.lubancode/sessions/<会话id>.jsonl。文件在首条用户消息落地时
    // 才建(会话 id 的 slug 要用它),此前只记一个启动时间戳。找不到主目录
    // (sessions_dir 空)或建档失败,打一行警告后本场闭嘴,不拦着人聊。
    // -----------------------------------------------------------------------
    if (project_memory != nullptr) {
        project_memory->set_source_session(session_start_ts);
    }

    // --continue:等价开场自动 /resume 本目录最近一场;本目录没有存档就
    // 安静开新会话。resume 可能把会话搬回存档里的 worktree 房,搬没搬记
    // 一笔,后面 sync 定义好了再善后。
    bool resume_moved_into_worktree = false;
    if (opts_.continue_last) {
        const std::function<void(const std::vector<lubancode::sessions::ArchivedQueueItem>&)> queue_restorer =
            [this](const std::vector<lubancode::sessions::ArchivedQueueItem>& items) {
                RestoreSteeringQueueFrom(items);
            };
        // Plan 模式单:mode/plan/review 账的恢复口(resume 后档位/计划成品
        // /审阅悬稿都接得回来)。
        const std::function<void(const std::optional<lubancode::sessions::ModeEvent>&,
                                 const std::vector<lubancode::sessions::PlanEvent>&,
                                 const std::optional<lubancode::sessions::PlanReviewEvent>&)>
            mode_restorer = [this](const std::optional<lubancode::sessions::ModeEvent>& mode_event,
                                   const std::vector<lubancode::sessions::PlanEvent>& plans,
                                   const std::optional<lubancode::sessions::PlanReviewEvent>& review) {
                plan_wiring_.RestoreFromArchive(mode_event, plans, review);
            };
        if (ResumeSession("", sessions_dir, *main_agent, session_store, persisted_count, session_meta, session_title,
                          wire_str, *current_model, theme, /*quiet_if_none=*/true, &worktree_session,
                          &session_compact_epoch, &queue_restorer, &mode_restorer)) {
            resume_moved_into_worktree = worktree_session.active();
            // 仓按恢复的那场开张(旧档若落过盘,artifact 继续可追)。
            OpenArtifactStore();
            // 持久目标单:goal 事件账随档恢复(replay 重建 coordinator;默认
            // paused-on-resume,不自动续跑,用户 /goal status 看账、/goal
            // resume 显式续)。
            goal_wiring_.RestoreFromArchive();
            // loop 单:loop 事件账随档恢复(active 默认暂停,用户 /loop
            // resume 显式续;单子"恢复"节)。
            lubancode::app::RestoreLoopFromArchive(loop_wiring_.MakeCommandWiring());
            // resume 进来的旧档没标题:cheap 角色给续聊点个名(规格"会话标题
            // 与 resume 列表摘要"归 cheap)。
            lubancode::app::MaybeGenerateSessionTitle(MakeTailContext(),
                                             lubancode::agent::TaskKind::ResumeSummary);
        }
    }

    // -----------------------------------------------------------------------
    // 跨会话传话:登记名册、起 pipe/socket 服务与心跳。只在交互会话启用
    // (spinner_enabled = 真控制台;管道/单发没有可回话的人,也不该挂监听)。
    // Start 失败不拦着聊,只打一行提示——这场不在名册上,/peers 看不见
    // 别人,别人也递不进话。
    //
    // 收发规矩全在 agent/peer_session.* 与 agent/peer_mailbox.*:传输线程只
    // 把信放进 PeerMailbox(自带锁),不碰 history、不碰终端;主线程在轮次
    // 边界(loop 的安全收件点)与空闲(Run() 循环顶)取走。held 的信由主
    // 线程弹 [y/N] 确认,用户点头才交给模型;点头与否都不影响传输层已经
    // 回掉的 held。
    // -----------------------------------------------------------------------
    peer_wiring_.Start(registry());
    std::function<std::optional<lubancode::api::Message>()> peer_inbox_poll = peer_wiring_.BuildInboxPoll();
    // 0.28.x:轮次边界收件点改为常驻(不再依赖 peer 是否启动)。收件链两段,
    // 排队消息在前(用户自己的话优先)、peer 来信在后,来源文案分清:
    //   - 子代理目标:当场转投任务 inbox(PumpSteeringToSubagents,与面板
    //     定向介入同一条通道),不混进 main 的注入批次;
    //   - main 目标:一次边界攒下的多条合成一条 user 消息(一块一条
    //     TextBlock,顺序保留),InjectIncomingMessage 会追加在刚入 history 的
    //     tool_result 之后——tool result 在前、介入消息在后、下一 request 最后,
    //     钉死的正是这个次序。危险工具执行中途没有任何调用点,天然插不进话。
    reapply_peer_inbox = [this, peer_inbox_poll]() {
        // 批四·病十二:inbox 是接线,进 AgentWiring(其余接线照原样带上)。
        lubancode::agent::AgentWiring wiring = main_agent->wiring();
        wiring.inbox = [this, peer_inbox_poll]() -> std::optional<lubancode::api::Message> {
            PumpSteeringToSubagents();
            const auto queued = SessionSteeringQueue().TakeDeliverable(lubancode::cli::MessageTarget::Main());
            if (!queued.empty()) {
                // 取走即不再属于 footer 的活队列；若入队那一帧恰因终端隐藏/
                // 屏幕查询失败没画出来，后续快照也不会再有机会显示它。先在
                // 终端历史留下用户回显，再把同一批正文注入下一次模型请求。
                lubancode::cli::EchoDeliveredQueuedMessages(queued, theme);
                lubancode::api::Message inject;
                inject.role = lubancode::api::Role::User;
                for (const auto& item : queued) {
                    inject.content.push_back(lubancode::api::TextBlock{
                        "[用户排队消息] 用户在上一只工具执行期间补了话,按排队顺序接上,不另起新任务:\n" +
                        item.text});
                }
                // 送走的即出档(路径二):快照事件行记当前活队列,已注入的
                // 不在里头,resume 不复活已送出的消息。崩在这之后的半轮里,
                // 消息本体也已在 history 落盘路上(PersistNewMessages)。
                PersistSteeringQueue();
                return inject;
            }
            if (peer_inbox_poll) {
                return peer_inbox_poll();
            }
            return std::nullopt;
        };
        main_agent->SetWiring(std::move(wiring));
    };
    reapply_peer_inbox();
    // loop 已就位,把 worktree 工具 enter/exit 的善后接到这条 sync 上。
    after_worktree_moved = [this]() { SyncWorktreeDirectory(); };
    // --continue 若把会话搬回了存档里的房,提示词与子代理 cwd 跟着同步。
    if (resume_moved_into_worktree) {
        SyncWorktreeDirectory();
    }
    // Plan 模式单:起手档(--mode/env/settings 在 RunCli 算好)。--continue
    // 恢复出旧档的场合:档里有 mode 行的,旧账真值已灌进 runtime,起手档
    // 不再插手(单子:"/resume 从旧账恢复最后 mode");老档没 mode 行的,
    // 恢复按 Default,起手档照常生效——旧档没有 Plan 账,不存在两本真值。
    if (opts_.start_in_plan && !plan_wiring_.RestoredFromArchive() &&
        session_runtime_.collaboration_mode() != lubancode::runtime::CollaborationMode::Plan) {
        plan_wiring_.SwitchMode(lubancode::runtime::CollaborationMode::Plan, "slash");
    }

    // 命令注册制的分派材料:全部装配完了一次配齐(handler 从这取料)。
    AssembleDispatchContext();
}

TerminalSessionController::~TerminalSessionController() {
    // 定向介入收场(规格:退出/清场不能无声遗失):停全部、报未送达。任务
    // 线程由 AgentTool 析构统一 join(此刻 tool_runtime_ 还活着,先于成员析构)。
    // 析构走"退场"档:排队账不倒(已落档,resume 接得回),只提示去处。
    CleanupBackgroundAgents(/*dispose_queue=*/false);
    // 跨会话传话收尾:摘掉收件点(别让重建钩子再碰已停的 runtime),写
    // closing、摘名片、停 pipe——此后递来的信连不上,发送方拿 unavailable。
    reapply_peer_inbox = nullptr;
    if (main_agent.has_value()) {
        lubancode::agent::AgentWiring wiring = main_agent->wiring();
        wiring.inbox = nullptr;
        main_agent->SetWiring(std::move(wiring));
    }
    peer_wiring_.Stop();
    // UI 回调清挂(原先的 UiHandlerGuard):回调抓着 this,析构前必须摘掉,
    // 异常退场也走这条。
    lubancode::cli::SetTranscriptUiHandler(nullptr);
    // 面板接线宿主整体收清(provider/actions 双清;终端接线收尾单)。
    lubancode::cli::SessionAgentPanelHost().Reset();
    lubancode::cli::SetAgentViewSwitchHook(nullptr);
    lubancode::cli::SetIdleWakeHook(nullptr);
    lubancode::cli::SetBackgroundNoticeHook(nullptr);
    lubancode::cli::SetPromptHistoryProvider(nullptr);
    lubancode::cli::SetFileMentionProvider(nullptr);
    // 空闲唤醒源先摘;loop 随后停 timer/join(shutdown 要 join,不能让
    // callback 析构后摸 this)。
    subagent_wake_token_.reset();
    loop_wiring_.Shutdown();
}


lubancode::tools::DetachedAgentBackend TerminalSessionController::BuildDetachedBackend() const {
    lubancode::tools::DetachedAgentBackend out;
    out.backend = BuildBackend(config);
    out.provider = active_provider;
    out.request_profile.model = *current_model;
    out.request_profile.reasoning_effort = *current_think;
    out.model_instructions = *current_model_instructions;
    out.soul = *current_soul;
    if (const auto entry = model_catalog.FindByProviderAndSlug(active_provider, *current_model);
        entry != nullptr) {
        out.request_profile.reasoning = entry->reasoning;
    }
    return out;
}

std::unique_ptr<lubancode::tools::ToolRegistry> TerminalSessionController::BuildDetachedRegistry() const {
    return std::make_unique<lubancode::tools::ToolRegistry>(BuildBaseToolRegistry(detached_skills_, detached_search_));
}

void TerminalSessionController::RebuildLoop(bool preserve_history) {
    // 每次真正重建会话都重读项目指令。用户手改 AGENTS.md 后敲 /clear，
    // 不必退出进程；provider/技能触发的保历史重建也顺手吃到新内容。
    project_instructions = lubancode::config::LoadProjectInstructions(std::filesystem::current_path()).content;
    prompt_options.project_instructions = project_instructions;
    if (auto* agent_tool = dynamic_cast<lubancode::tools::AgentTool*>(registry().Find("agent"));
        agent_tool != nullptr) {
        agent_tool->SetProjectInstructions(project_instructions);
    }
    std::vector<lubancode::api::Message> old_history;
    if (preserve_history && main_agent.has_value()) {
        old_history = main_agent->History();
    }
    // 运行策略走统一 profile(规格根因一):输出上限三级解析(config >
    // provider > 模型目录),unset 交服务端默认,不再有写死的 4096;步数、
    // 上下文、窗口、续跑次数同一份。子代理 tool 也在此处同步拿到派生份
    // (subagent 段的显式覆盖在 BuildSubagentRuntimeProfile 里算),main 与
    // 子代理同级吃同一套有效值。
    // 批四·病十一其三:五层请求改写后端退役后,会话级请求策略(model/
    // effort/模型指令/魂/延迟索引)全在皮上;tool_search 的过滤谓词随重建
    // 重新灌——loaded 集合不清,已挂载的工具跨 /clear 仍然可用。
    const lubancode::agent::AgentRuntimeProfile main_profile =
        lubancode::app::BuildMainRuntimeProfile(config, &model_catalog, *current_model);
    lubancode::agent::AgentProfile main_agent_profile;
    main_agent_profile.provider = active_provider;
    main_agent_profile.request.model = *current_model;
    main_agent_profile.request.reasoning_effort = *current_think;
    if (const auto* entry = model_catalog.FindByProviderAndSlug(active_provider, *current_model);
        entry != nullptr) {
        main_agent_profile.request.reasoning = entry->reasoning;
    }
    main_agent_profile.runtime = main_profile;
    main_agent_profile.system_prompt = lubancode::agent::AssembleSystemPrompt(prompt_options);
    // 皮上的叠层(从前由传输层的 ModelInstructions/SoulOverlay/
    // DeferredIndex 三只包装后端现拼,现在 Agent 拼请求时就地生效)。
    main_agent_profile.model_instructions = *current_model_instructions;
    main_agent_profile.soul = *current_soul;
    main_agent_profile.deferred_index_provider = [this]() {
        // 发请求前现查现拼:tool_search 命中后的下一份请求,新挂载的工具
        // 自然从索引段里消失;未启用时恒给空串,等于不注。
        return main_deferral ? lubancode::tools::BuildDeferredToolsIndexSegment(registry(), *loaded_tools())
                             : std::string();
    };
    // 工具可见性(病十三的方向):goal/loop 窄工具的 turn 级放行(单子:
    // goal_checkpoint 只在 goal execution turn 动态露面,loop_control 只在
    // scheduled tick 的 turn 里;普通 turn 一概看不见)。goal_active_
    // iteration_/loop_active_tick_id_ 是会话泵发 turn 前置、turn 收口清的
    // 活跃账,恰是"这一轮是谁的轮"的真值。其余工具走 ToolRuntime 的主
    // 过滤(延迟挂载/memory gate 原样)。
    main_agent_profile.tool_filter = [this](const lubancode::tools::Tool& tool) {
        if (tool.name() == "goal_checkpoint") {
            return goal_wiring_.HasActiveIteration();
        }
        if (tool.name() == "loop_control") {
            return loop_wiring_.TickActive();
        }
        return main_tool_filter()(tool);
    };
    main_agent_profile.tool_filter_denial =
        "这只工具只在对应的 goal 执行轮/loop 定时拍里可用,当前轮不是。";
    // 病十(批三):四段开关写进皮——与 prompt_options 同源(配置),子代理
    // 派生时同段拷贝,不再有"主代理有、子代理无"的隐性分叉。
    main_agent_profile.prompt_sections.mcp = prompt_options.mcp;
    main_agent_profile.prompt_sections.web = prompt_options.web;
    main_agent_profile.prompt_sections.lsp = prompt_options.lsp;
    main_agent_profile.prompt_sections.wire = prompt_options.wire;
    main_agent.emplace(wrapped_backend, registry(), main_agent_profile);
    if (auto* agent_tool = dynamic_cast<lubancode::tools::AgentTool*>(registry().Find("agent"));
        agent_tool != nullptr) {
        lubancode::agent::AgentProfile subagent_profile = main_agent_profile;
        subagent_profile.runtime = lubancode::app::BuildSubagentRuntimeProfile(main_profile, config);
        agent_tool->SetAgentProfile(std::move(subagent_profile));
        // 子代理记忆召回(规格"同级能力审计"):按子任务 prompt 独立检索,
        // 同预算同安全声明;与 main 的召回同一只 ProjectMemory。关着
        // (use=false)就不注。
        if (project_memory != nullptr && config.memory.use) {
            agent_tool->SetTurnContextProvider(
                [memory = project_memory](const std::string& task_prompt) {
                    return memory->BuildTurnContext(task_prompt, std::filesystem::current_path(),
                                                    lubancode::memory::QueryOrigin::User);
                });
        }
    }
    // 可追回 artifact(第二期):重建的 loop 也要接上仓(空仓安全退化)。
    main_agent->context().set_artifact_store(artifact_store.get());
    // mid-turn 上下文安全点(0.27.x):窗口与压力通报随 loop 重建重灌;窗口
    // 的后续变化(/context、/model)由 RunUserTurn 发轮前再同步。
    main_agent->SetContextWindowTokens(context_tracker.window_tokens());
    // 接线(批四·病十二):压力钩进 AgentWiring;inbox 由 peer 钩在底下重灌。
    lubancode::agent::AgentWiring main_wiring;
    main_wiring.on_context_pressure = [this](const lubancode::agent::ContextPressure& pressure) {
        lubancode::app::HandleContextPressure(pressure, MakeCompactInputs());
    };
    main_agent->SetWiring(std::move(main_wiring));
    if (reapply_peer_inbox) {
        reapply_peer_inbox();  // 跨会话收件点:重建的 loop 也要能收信
    }
    if (preserve_history) {
        main_agent->ReplaceHistory(std::move(old_history));
    }
}

void TerminalSessionController::SyncAgentRequestPolicy() {
    // /model、/think、/soul 只改会话状态(current_model/current_think/
    // current_model_instructions/current_soul),loop 不重建——皮上的请求
    // 档案与叠层由这里整份刷新,下一份请求即时生效。reasoning 档位照
    // (provider, model) 从目录现查,与从前 ThinkOverrideBackend 在
    // send_stream 里干的是同一笔账,只是挪进了正门、进了前缀指纹的视野。
    if (!main_agent.has_value()) {
        return;
    }
    lubancode::api::RequestProfile request;
    request.model = *current_model;
    request.reasoning_effort = *current_think;
    if (const auto* entry = model_catalog.FindByProviderAndSlug(active_provider, *current_model);
        entry != nullptr) {
        request.reasoning = entry->reasoning;
    }
    main_agent->SetRequestProfile(std::move(request));
    main_agent->SetModelInstructions(*current_model_instructions);
    main_agent->SetSoul(*current_soul);
}

void TerminalSessionController::RefreshSkills() {
    skills = lubancode::tools::LoadSkills(CurrentDirUtf8(), home_dir, official_skills_dir);
    skills_segment = lubancode::tools::BuildSkillsPromptSegment(skills);
    if (auto* tool = dynamic_cast<lubancode::tools::SkillTool*>(registry().Find("skill")); tool != nullptr) {
        tool->SetSkills(skills);
    }
    if (auto* tool = dynamic_cast<lubancode::tools::SkillTool*>(sub_registry().Find("skill")); tool != nullptr) {
        tool->SetSkills(skills);
    }
    if (auto* tool = dynamic_cast<lubancode::tools::AgentTool*>(registry().Find("agent")); tool != nullptr) {
        tool->SetSkillsSegment(skills_segment);
    }
    prompt_options.skills_segment = skills_segment;
    RebuildLoop(/*preserve_history=*/true);
}

void TerminalSessionController::RefreshProjectInstructions() {
    RebuildLoop(/*preserve_history=*/true);
}

// 建档(渐进式上下文仓第二期起,第一轮用户输入**之前**就要建):仓要拿
// session id 开张,超长结果在第一轮请求里就得能落盘,不能等回合收尾。首条
// 文本做 slug;建档失败置 session_store_broken 照旧拦落盘,会话本身照跑。
// 建档成功顺手开仓(开不成只告警:超长结果退回内存全文,不产生假引用)。
bool TerminalSessionController::EnsureSessionBegun(const std::string& first_text) {
    // P6:建档本体在 SessionRuntime(错误不再自己打印,由这边按结果印)。
    const auto result =
        session_runtime_.EnsureBegun(first_text, *current_model, CurrentDirUtf8());
    if (result == lubancode::runtime::SessionBeginResult::Failed) {
        TermOut() << theme.error << trf("session.create_failed", sessions_dir) << theme.reset << "\n";
        return false;
    }
    if (result != lubancode::runtime::SessionBeginResult::Begun) {
        return session_store.active();  // Active/Disabled:照旧语义
    }
    // hooks 上下文补真 session id 与转录路径(建档这一刻才齐)。
    if (lubancode::app::HookRuntime() != nullptr) {
        lubancode::hooks::HookContext hook_context = lubancode::app::HookRuntime()->context();
        hook_context.session_id = session_store.session_id();
        hook_context.transcript_path = session_store.file_path();
        lubancode::app::UpdateHookRuntimeContext(hook_context);
    }
    OpenArtifactStore();
    return true;
}

// 开仓:<sessions_dir>/<session-id>/context(与 <session-id>.jsonl 并排,
// /sessions 只扫 *.jsonl,互不干扰)。开不成只告警——仓是加层,不是依赖。
void TerminalSessionController::OpenArtifactStore() {
    if (sessions_dir.empty() || !session_store.active()) {
        return;
    }
    const std::string root = sessions_dir + "/" + session_store.session_id() + "/context";
    if (!artifact_store->Open(root, session_store.session_id())) {
        TermOut() << theme.stats << trf("artifact.store_open_failed", root) << theme.reset << "\n";
    }
}

// 把 history 里 persisted_count 之后的消息逐条追加落盘(append+flush,
// 崩溃安全)。history 被 ReplaceHistory 换短(/compact)的场合由调用处
// 先把 persisted_count 收到新长度,这里只管"只增不减"的常态。
void TerminalSessionController::PersistNewMessages() {
    // P6:增量落盘本体在 SessionRuntime(只增不减、兜底建档同旧路)。
    // store 没开张时的兜底建档也在它那头(首条用户文本抽出来做 slug);
    // 这边只在"Begun 且还没 active"的窗口补一句给用户的话与 hooks 上下文。
    const auto result = session_runtime_.PersistNew(main_agent->History(), *current_model, CurrentDirUtf8());
    if (result == lubancode::runtime::SessionPersistResult::BrokenNow) {
        TermOut() << theme.error << tr("session.append_failed") << theme.reset << "\n";
        return;
    }
    if (result == lubancode::runtime::SessionPersistResult::Nothing && !session_store.active() &&
        !sessions_dir.empty() && !session_store_broken && !main_agent->History().empty()) {
        // 落盘账没动而 store 仍没开张:按旧兜底路走一遍建档(给 hooks 与
        // 仓一齐的机会)。PersistNew 里 EnsureBegun 只填账不碰 hooks,这里
        // 补上与 EnsureSessionBegun 相同的那段。
        std::string first_text;
        for (const auto& message : main_agent->History()) {
            if (message.role != lubancode::api::Role::User) {
                continue;
            }
            for (const auto& block : message.content) {
                if (const auto* tb = std::get_if<lubancode::api::TextBlock>(&block)) {
                    first_text = tb->text;
                    break;
                }
                if (const auto* image = std::get_if<lubancode::api::ImageBlock>(&block)) {
                    first_text = image->filename;
                    break;
                }
            }
            break;
        }
        if (!first_text.empty()) {
            EnsureSessionBegun(first_text);
        }
    }
}



// 子代理目标的排队消息转投任务 inbox(与面板定向介入同一条通道:
// AgentTool::SendTaskMessage——那只子代理自己的 AgentLoop 会在"当前工具
// 收尾、下一次请求未发"的边界收信)。终态明确拒收:标 TargetGone 留在
// 队列原位(屏上带"[目标已结束]"标记),等用户取回改目标或删掉,不改投
// main。只在主线程调(会话泵/轮次边界)。
void TerminalSessionController::PumpSteeringToSubagents() {
    bool queue_changed = false;
    for (const auto& item : SessionSteeringQueue().Snapshot()) {
        if (item.target.kind != lubancode::cli::MessageTarget::Kind::Subagent ||
            item.state != lubancode::cli::QueueItemState::Queued || item.edit_open) {
            continue;
        }
        lubancode::tools::AgentTool* agent_tool = session_agent_tool();
        if (agent_tool == nullptr) {
            SessionSteeringQueue().MarkFailed(item.id, "当前会话没有可用的子代理通道");
            queue_changed = true;
            continue;
        }
        const lubancode::tools::TaskMessageStatus status =
            agent_tool->SendTaskMessage(item.target.task_id, item.text);
        if (status == lubancode::tools::TaskMessageStatus::Queued) {
            // 已进任务 inbox,活队列退场(那边轮次边界自会送达)。
            SessionSteeringQueue().Remove(item.id);
            queue_changed = true;
        } else {
            // 终态(Finished)或任务号认不出(NotFound):一律按"目标已结束"
            // 标注,原文留队,绝不改投 main(规格"队列按目标分账")。
            SessionSteeringQueue().MarkTargetGone(item.id, "目标子代理已结束");
            queue_changed = true;
        }
    }
    if (queue_changed) {
        PersistSteeringQueue();  // 转投/标注也是排队账一变(路径二,快照对齐)
    }
}

// 排队账 -> 存档快照事件行(路径二)。落不了档(没建档/写坏)只安静退:
// 存档从来是加层,坏不到会话本体。TargetGone/Failed 的条目也一并进快照——
// 它们是"等用户处置"的活账,resume 后还该看得见。
void TerminalSessionController::PersistSteeringQueue() {
    if (sessions_dir.empty() || session_store_broken) {
        return;
    }
    if (!session_store.active()) {
        // 一条消息没发过就排了队、又直接 /exit:档还没建。拿队头那条当
        // 首句建档(slug 用得上),排队账才有处落——不然这类场子的队列
        // 依然落空。建不成档安静退,老规矩。
        std::string first_text;
        for (const auto& item : SessionSteeringQueue().Snapshot()) {
            if (!item.text.empty()) {
                first_text = item.text;
                break;
            }
        }
        if (first_text.empty() || !EnsureSessionBegun(first_text)) {
            return;
        }
    }
    const auto snapshot = SessionSteeringQueue().Snapshot();
    std::vector<lubancode::sessions::ArchivedQueueItem> items;
    items.reserve(snapshot.size());
    for (const auto& item : snapshot) {
        lubancode::sessions::ArchivedQueueItem archived;
        archived.id = item.id;
        archived.subagent = !item.target.is_main();
        archived.task_id = item.target.task_id;
        archived.text = item.text;
        archived.attempts = item.delivery_attempts;
        items.push_back(std::move(archived));
    }
    (void)session_store.AppendQueueEvent(items);  // 失败不告警:下一趟账变了再追
}

// 存档快照 -> 会话层队列(resume 路)。RestoreFromArchive 只在队列还空着时
// 收(本场自己还没排队),运行中的账不给旧档盖。
void TerminalSessionController::RestoreSteeringQueueFrom(
    const std::vector<lubancode::sessions::ArchivedQueueItem>& items) {
    if (items.empty()) {
        return;
    }
    std::vector<lubancode::cli::QueuedMessage> restored;
    restored.reserve(items.size());
    for (const auto& archived : items) {
        lubancode::cli::QueuedMessage item;
        item.id = archived.id;
        item.target = archived.subagent ? lubancode::cli::MessageTarget::Agent(archived.task_id)
                                        : lubancode::cli::MessageTarget::Main();
        item.text = archived.text;
        item.state = lubancode::cli::QueueItemState::Queued;
        item.delivery_attempts = archived.attempts;
        restored.push_back(std::move(item));
    }
    SessionSteeringQueue().RestoreFromArchive(std::move(restored));
}

// Ctrl+R 提问历史搜索的数据源:只读 session 事件账。ListSessions 按新→旧
// 给场次,这里倒序遍历(整体旧→新,BuildHistorySearchIndex 认这个序);
// 每场内部 ExtractPromptHistory 本就是旧→新。当前会话若还没建档(首条
// 消息未落地),活 history 里的用户提问也并进来——同一只读规则。
lubancode::cli::PromptHistoryDataset TerminalSessionController::CollectPromptHistory() {
    lubancode::cli::PromptHistoryDataset data;
    data.current_session_id = session_store.session_id();
    data.current_project_key = lubancode::sessions::NormalizePathForCompare(CurrentDirUtf8());
    if (!sessions_dir.empty()) {
        const std::vector<lubancode::sessions::SessionListEntry> listed =
            lubancode::sessions::ListSessions(sessions_dir, /*limit=*/150);
        for (auto it = listed.rbegin(); it != listed.rend(); ++it) {
            const auto bytes = lubancode::sessions::ReadSessionFileBytes(it->file_path);
            if (!bytes.has_value()) {
                continue;  // 读不动这场就跳过,不废整份
            }
            const std::string project_key = lubancode::sessions::NormalizePathForCompare(it->cwd);
            for (auto& record : lubancode::sessions::ExtractPromptHistory(*bytes)) {
                lubancode::cli::PromptHistoryEntry entry;
                entry.text = std::move(record.text);
                entry.ts = std::move(record.ts);
                entry.session_id = it->id;
                entry.title = it->title;
                entry.project_key = project_key;
                data.entries.push_back(std::move(entry));
            }
        }
    }
    // 活 history 兜底:建档前(或建不了档)的本场提问。
    const std::string current_id =
        data.current_session_id.empty() ? std::string("current") : data.current_session_id;
    for (const auto& message : main_agent->History()) {
        if (message.role != lubancode::api::Role::User || message.content.empty()) {
            continue;
        }
        const auto* text = std::get_if<lubancode::api::TextBlock>(&message.content.front());
        if (text == nullptr || text->text.empty() || text->text.front() == '/') {
            continue;  // 与 ExtractPromptHistory 同一只读规则
        }
        bool has_tool_result = false;
        for (const auto& block : message.content) {
            if (std::holds_alternative<lubancode::api::ToolResultBlock>(block)) {
                has_tool_result = true;
                break;
            }
        }
        if (has_tool_result) {
            continue;
        }
        lubancode::cli::PromptHistoryEntry entry;
        entry.text = text->text;
        entry.session_id = current_id;
        entry.project_key = data.current_project_key;
        entry.ts = session_start_ts;
        data.entries.push_back(std::move(entry));
    }
    return data;
}

// @ 提及的文件索引:Git 根优先(提"项目文件"按项目走),没有根就 cwd。
// 深度限 6、条目限 3000,排除 .git/构建产物/依赖目录/点目录。相对路径
// 一律正斜杠。根没变就返回缓存(cwd/worktree 切换由 SyncWorktreeDirectory
// 清缓存)。
std::vector<lubancode::cli::FileMentionEntry> TerminalSessionController::FileMentionIndexSnapshot() {
    const std::filesystem::path cwd = std::filesystem::current_path();
    const auto root = lubancode::cli::FindRepositoryRoot(cwd);
    const std::filesystem::path base = root.value_or(cwd);
    const std::string root_key = lubancode::tools::PathToUtf8(base);
    if (root_key == mention_index_root_ && !mention_index_.empty()) {
        return mention_index_;
    }
    mention_index_root_ = root_key;
    mention_index_.clear();
    static const std::set<std::string> kExcluded = {
        ".git", "build", "out", "dist", "node_modules", "target", "_deps", "_build",
        ".lubancode", ".cache", "__pycache__", ".venv", "venv", "cmake-build-debug"};
    std::error_code ec;
    std::filesystem::recursive_directory_iterator it(base, ec), end;
    while (it != end && mention_index_.size() < 3000) {
        const std::filesystem::path current = it->path();
        const std::string name = lubancode::tools::PathToUtf8(current.filename());
        if (it->is_symlink(ec)) {
            it.disable_recursion_pending();
            ++it;
            continue;  // 符号链接不进清单也不下钻
        }
        const bool is_dir = it->is_directory(ec);
        if (is_dir && (kExcluded.contains(name) || (!name.empty() && name.front() == '.'))) {
            it.disable_recursion_pending();
            ++it;
            continue;
        }
        if (it.depth() > 6) {
            it.disable_recursion_pending();
            ++it;
            continue;
        }
        if (current != base) {
            std::string rel = lubancode::tools::PathToUtf8(current.lexically_relative(base));
            for (char& c : rel) {
                if (c == '\\') {
                    c = '/';
                }
            }
            mention_index_.push_back(lubancode::cli::FileMentionEntry{rel, is_dir});
        }
        ++it;
    }
    // 目录排前、路径短排前——@src/cli 选目录一击即中。
    std::sort(mention_index_.begin(), mention_index_.end(),
              [](const auto& a, const auto& b) {
                  if (a.is_dir != b.is_dir) {
                      return a.is_dir;
                  }
                  return a.relative_path < b.relative_path;
              });
    return mention_index_;
}

// 提交前提及校验(规格第二批第 2 条验收):目标消失或跑出工作区要明报错,
// 这轮不发。活着的提及附一份"相对 → 绝对"账给模型(turn context,不进
// 永久 history),不叫模型猜裸路径。图片路径不进账——它们走
// PrepareImageInput 的视觉附件路。
std::pair<std::string, std::string> TerminalSessionController::BuildMentionLedger(const std::string& content) {
    const std::vector<std::string> tokens = lubancode::cli::ExtractTextMentions(content);
    if (tokens.empty()) {
        return {};
    }
    const std::filesystem::path cwd = std::filesystem::current_path();
    const auto root = lubancode::cli::FindRepositoryRoot(cwd);
    const std::filesystem::path base = root.value_or(cwd);
    const std::string base_key = lubancode::sessions::NormalizePathForCompare(lubancode::tools::PathToUtf8(base));
    std::string ledger;
    for (const std::string& token : tokens) {
        if (lubancode::cli::MediaTypeForPath(token).has_value()) {
            continue;  // 图片:视觉附件路自己管
        }
        // 相对根解析;根内没有再按 cwd 相对试一次(临时文件那类提及)。
        std::filesystem::path resolved;
        bool found = false;
        for (const std::filesystem::path& candidate : {base / lubancode::tools::Utf8ToPath(token),
                                                       cwd / lubancode::tools::Utf8ToPath(token)}) {
            std::error_code ec;
            if (std::filesystem::exists(candidate, ec)) {
                resolved = candidate;
                found = true;
                break;
            }
        }
        if (!found) {
            return {trf("mention.missing", token), {}};
        }
        // 项目根校验:解析后的绝对路径必须仍在根内(或等于根),不许 @..
        // 越狱到园子外。
        std::error_code ec;
        const std::filesystem::path canon = std::filesystem::weakly_canonical(resolved, ec);
        const std::string canon_key =
            lubancode::sessions::NormalizePathForCompare(lubancode::tools::PathToUtf8(canon));
        if (!canon_key.empty() && canon_key.rfind(base_key + "/", 0) != 0 && canon_key != base_key) {
            return {trf("mention.outside_root", token), {}};
        }
        const bool is_dir = std::filesystem::is_directory(canon, ec);
        ledger += "\n- " + token + " -> " + lubancode::tools::PathToUtf8(canon) +
                  (is_dir ? "(目录)" : "(文件)");
    }
    if (ledger.empty()) {
        return {};
    }
    return {{}, tr("mention.ledger_header") + ledger + "\n"};
}

// 终端标题模板:项目短名 · 分支 · 状态词。纯拼串,可单测可不测(肉眼可核)。
std::string TerminalSessionController::BuildTerminalTitleText(const std::string& state_word) const {
    std::string project;
    if (const auto root = lubancode::cli::FindRepositoryRoot(std::filesystem::current_path())) {
        project = lubancode::tools::PathToUtf8(root->filename());
    }
    if (project.empty()) {
        project = "lubancode";
    }
    const std::string branch = lubancode::cli::CurrentGitBranch(std::filesystem::current_path());
    std::string out = "lubancode · " + project;
    if (!branch.empty()) {
        out += " · " + branch;
    }
    out += " · " + state_word;
    return out;
}

void TerminalSessionController::EnsureMemoryTool() {
    // capability gate:全局未授权时 memory_save 不注册(双保险之一,另一道
    // 在 MemorySaveTool::execute 的运行时判定)。
    if (project_memory != nullptr && project_memory->generate_enabled() &&
        registry().Find("memory_save") == nullptr) {
        registry().Register(std::make_unique<lubancode::memory::MemorySaveTool>(project_memory));
    }
}

void TerminalSessionController::SyncWorktreeDirectory() {
    // 切 worktree 收面板:查看态目标跟着旧房的任务走,别把消息投去旧目标。
    lubancode::cli::ResetAgentPanelSession();
    // @ 提及索引跟着根走:根变了重扫(下一拍 FileMentionIndexSnapshot 自办)。
    mention_index_root_.clear();
    mention_index_.clear();
    prompt_options.cwd = CurrentDirUtf8();
    if (project_memory != nullptr) {
        if (const auto updated = project_memory->SetWorkingDirectory(std::filesystem::current_path());
            !updated.has_value()) {
            TermOut() << trf("cmd.memory.switch_failed", updated.error()) << "\n";
        }
    }
    project_instructions = lubancode::config::LoadProjectInstructions(std::filesystem::current_path()).content;
    prompt_options.project_instructions = project_instructions;
    main_agent->SetSystemPrompt(lubancode::agent::AssembleSystemPrompt(prompt_options));
    if (auto* agent_tool = dynamic_cast<lubancode::tools::AgentTool*>(registry().Find("agent"));
        agent_tool != nullptr) {
        agent_tool->SetWorkingDirectory(prompt_options.cwd);
        agent_tool->SetProjectInstructions(project_instructions);
    }
    // 会话档跟 cwd 走(0.27.x):目录动了就追加一条 cwd 事件,
    // /resume 靠它把会话送回原房。
    if (session_store.active()) {
        session_store.AppendCwdEvent(prompt_options.cwd);
    }
}

// 处理"确定不是空行、不是裸词 exit/quit"的一行输入,不管这行是刚
// ReadLine() 读到的、还是从 pending_queue 里取出来的自动发送的——两条
// 路径共用这一份 slash 分支 + 自动 compact 检查 + RunTurn 调用,行为
// 完全一致(spec 要求"队列里是 slash 命令也认")。返回 false 表示这一行
// 触发了 /exit,外层循环该退出了。
CommandFlow TerminalSessionController::ProcessLine(const std::string& content, bool* autosend_failed) {
    const lubancode::cli::ParsedSlashCommand parsed = lubancode::cli::ParseSlashCommand(content);
    // 会话级兜底(宽窄转换异常单):slash 命令、普通回合、回合收尾的起名/
    // 记忆抽取,任何 std::exception 都不再穿透顶层把整场掀了——错误上屏、
    // history 里已有的落盘、循环继续。这不是"每层包一遍":回合执行的最内
    // 环已有 RunTurn 那道收口,这里是会话边界唯一的一道;再往外只剩启动
    // 期(cli_app 顶层 catch)才许退进程。
    try {
        if (parsed.command != lubancode::cli::SlashCommand::NotSlash &&
            parsed.command != lubancode::cli::SlashCommand::Image) {
            return DispatchSlashCommand(parsed);
        }
        // 普通正文(含 peer 来信组包后的文字):自动压缩检查 + 发一轮。
        RunSessionTurn(content, TurnSource::User, autosend_failed);
        return CommandFlow::Continue;
    } catch (const std::exception& e) {
        if (autosend_failed != nullptr) {
            *autosend_failed = true;  // 回合异常收场:排队消息按失败退还(路径一的兜底判定)
        }
        {
            std::lock_guard<std::mutex> lock(lubancode::cli::StdoutWriteMutex());
            TermErr() << "\n"
                      << theme.error << tr("error.prefix") << trf("error.unexpected", e.what()) << theme.reset
                      << "\n";
            TermErr().flush();
        }
        try {
            PersistNewMessages();  // 已入 history 的部分照常落盘,/resume 接得回来
        } catch (...) {
            // 落盘自己都失败了:报不出更多信息,会话仍续命
        }
        return CommandFlow::Continue;
    }
}

// slash 分派(命令注册制,会话终章):47 案 switch 换成命令注册表——各案
// handler 归各域文件(commands/*.cpp 的 HandleSlashXxx),注册表与查表路由
// 在 commands/command_registry;这里只把会话材料装进 SlashDispatchContext
// (构造尾一次配齐)递过去。
CommandFlow TerminalSessionController::DispatchSlashCommand(const lubancode::cli::ParsedSlashCommand& parsed) {
    return lubancode::app::DispatchSessionSlashCommand(dispatch_ctx_, parsed);
}

// 分派材料的装配:全借用/回调,handler 不拥有会话资源。回调一律窄口
// (控制器方法一跳),域文件不反向 include 会话层。
void TerminalSessionController::AssembleDispatchContext() {
    lubancode::app::SlashDispatchContext& ctx = dispatch_ctx_;
    ctx.opts = &opts_;
    ctx.config_result = &config_result_;
    ctx.config = &config;
    ctx.theme = &theme;
    ctx.model_catalog = &model_catalog;
    ctx.settings_local = &settings_local;
    ctx.spinner_enabled = spinner_enabled;
    ctx.wire_str = &wire_str;
    ctx.active_provider = &active_provider;
    ctx.active_provider_write_path = &active_provider_write_path;
    ctx.config_file_path = &config_file_path;
    ctx.home_dir = &home_dir;
    ctx.home_lubancode = &home_lubancode;
    ctx.prompts_dir = &prompts_dir;
    ctx.persona = &persona;
    ctx.global_skills_root = &global_skills_root;
    ctx.project_skills_root = &project_skills_root;
    ctx.recordings_root = &recordings_root;
    ctx.skills = &skills;
    ctx.real_backend = &real_backend;
    ctx.current_model = current_model;
    ctx.current_think = current_think;
    ctx.current_model_instructions = current_model_instructions;
    ctx.current_soul = current_soul;
    ctx.current_soul_name = &current_soul_name;
    ctx.context_tracker = &context_tracker;
    ctx.model_router = model_router.get();
    ctx.artifact_store = artifact_store;
    ctx.registry = &registry();
    ctx.sub_registry = &sub_registry();
    ctx.agent_tool = session_agent_tool();
    ctx.todo_state = &todo_state();
    ctx.loaded_tools = &loaded_tools();
    ctx.mcp_servers = &mcp_servers();
    ctx.lsp_manager = &lsp_manager();
    ctx.plugin_mounted = &plugin_mounted();
    ctx.plugin_warnings = &plugin_warnings();
    ctx.main_tool_filter = &main_tool_filter();
    ctx.main_deferral = main_deferral;
    ctx.tool_search_threshold = tool_search_threshold;
    ctx.tool_runtime = tool_runtime_.has_value() ? &*tool_runtime_ : nullptr;
    ctx.worktree_session = &worktree_session;
    ctx.main_agent = main_agent.has_value() ? &*main_agent : nullptr;
    ctx.session_runtime = &session_runtime_;
    ctx.trace_hub = trace_hub_.has_value() ? &*trace_hub_ : nullptr;
    ctx.session_events = &session_events_;
    ctx.session_store = &session_store;
    ctx.sessions_dir = &sessions_dir;
    ctx.session_meta = &session_meta;
    ctx.session_title = &session_title;
    ctx.last_compact_line = &last_compact_line;
    ctx.prompt_options = &prompt_options;
    ctx.project_memory = project_memory.get();
    ctx.peer_wiring = &peer_wiring_;
    ctx.record_wiring = &record_wiring_;
    ctx.rebuild_loop = [this](bool preserve_history) { RebuildLoop(preserve_history); };
    ctx.sync_request_policy = [this]() { SyncAgentRequestPolicy(); };
    ctx.refresh_skills = [this]() { RefreshSkills(); };
    ctx.refresh_project_instructions = [this]() { RefreshProjectInstructions(); };
    ctx.sync_worktree_directory = [this]() { SyncWorktreeDirectory(); };
    ctx.ensure_memory_tool = [this]() { EnsureMemoryTool(); };
    ctx.ensure_goal_coordinator = [this]() { goal_wiring_.Ensure(config); };
    ctx.ensure_loop_scheduler = [this]() { loop_wiring_.Ensure(); };
    ctx.make_goal_wiring = [this]() {
        return goal_wiring_.MakeCommandWiring(session_agent_tool(), loop_wiring_.scheduler());
    };
    ctx.make_loop_wiring = [this]() { return loop_wiring_.MakeCommandWiring(); };
    ctx.make_compact_inputs = [this]() { return MakeCompactInputs(); };
    ctx.make_session_command_state = [this]() { return MakeSessionCommandState(); };
    ctx.handle_plan_command = [this](const std::string& args) { return plan_wiring_.HandleCommand(args); };
    ctx.switch_collaboration_mode = [this](lubancode::runtime::CollaborationMode mode, const std::string& reason) {
        plan_wiring_.SwitchMode(mode, reason);
    };
    ctx.reset_plan_review = [this]() { plan_wiring_.DiscardReview(); };
    ctx.build_workflow_tool_options = [this]() { return BuildWorkflowToolOptions(); };
}

// 主循环泵(公平仲裁,goal 分流合流):goal ready continuation 与 due
// loop tick 不共用 trigger(evaluator 判终点 vs 时钟到点),共用这只泵
//(单飞:一场 session 同时只跑一枚主 turn)。先各问一句,凑候选表,
// PumpNextWork 按优先级 + 公平账(goal 连跑三轮让一枚 due loop tick)定
// 谁走;user queue / pending interaction 已在泵前面的主循环各分支消费过,
// 这里只收自动工作两类。每圈只消费一拍,回主循环顶重新检查高优先级来源
//(不一次把八枚 due 全倒进队列,用户按 stop 时还来得及)。
bool TerminalSessionController::PumpScheduledWork() {
    const auto now_ms = [] {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
                   std::chrono::system_clock::now().time_since_epoch())
            .count();
    }();
    std::vector<lubancode::runtime::SessionWork> candidates;
    // 有 ready continuation:候选里放一枚占位,真取件(TakeReadyIteration
    // 落 started 事件)等选中后再做——没选中就不动 goal 的账。
    if (goal_wiring_.coordinator() != nullptr && goal_wiring_.work_source().ProbeWork().has_value()) {
        lubancode::runtime::SessionWork work;
        work.kind = lubancode::runtime::WorkKind::GoalContinuation;
        work.id = "goal-continuation";
        candidates.push_back(work);
    }
    if (loop_wiring_.SweepAndCheckDue(now_ms)) {
        lubancode::runtime::SessionWork work;
        work.kind = lubancode::runtime::WorkKind::LoopTick;
        work.id = "loop-tick";
        candidates.push_back(work);
    }
    const auto picked = lubancode::runtime::PumpNextWork(candidates, goal_wiring_.fairness());
    if (!picked.has_value()) {
        lubancode::app::FlushLoopEvents(loop_wiring_.MakeCommandWiring());
        return false;
    }
    if (picked->kind == lubancode::runtime::WorkKind::GoalContinuation) {
        goal_wiring_.PumpContinuation(now_ms);
        return true;
    }
    if (!loop_wiring_.HasActiveTasks()) {
        return false;
    }
    return loop_wiring_.PumpDueTick(now_ms);
}

lubancode::agent::CompactOptions TerminalSessionController::BuildCompactOptions() {
    lubancode::agent::CompactOptions options;
    // 窗口预算认压缩路由自己的声明:高级段 model_roles 声明了 context_
    // window 就用它;没有再查模型目录条目;目录里也查不到(自定义模型、
    // 中转起名)就留空——Compact() 不做窗口拦截,但输出会明说"窗口未知,
    // 未校验",不假装核过。cheap 与 normal 窗口不同,分块按 cheap 的窗口
    // 算(规格"预算来源")。
    const auto compact_route = model_router->RouteInfo(lubancode::agent::TaskKind::Compact);
    if (compact_route.context_window.has_value()) {
        options.budget.window_tokens = compact_route.context_window;
    } else if (const auto* entry = model_catalog.FindBySlug(compact_route.model); entry != nullptr) {
        options.budget.window_tokens = entry->context_window_tokens;
    }
    // 预算总账(第四期):协议余量按统一公式算(协议头 + 估算误差边),
    // /context 展示的与 compact 拦截用的是同一本账,不再各写各的魔数。
    {
        lubancode::agent::ContextBudgetInputs budget_inputs;
        budget_inputs.window_tokens = options.budget.window_tokens;
        budget_inputs.requested_output_reserve_tokens = options.budget.output_reserve_tokens;
        budget_inputs.compact_prompt_overhead_tokens = 512;  // 压缩指令的公开估算档
        const auto plan = lubancode::agent::BuildContextBudgetPlan(budget_inputs);
        if (plan.compact_call_input_budget.has_value()) {
            options.budget.protocol_headroom_tokens =
                plan.protocol_headroom + plan.tokenizer_error_margin + plan.compact_prompt_overhead;
        }
    }
    // 活动待办守恒:pending/in_progress 条目原文逐条钉进 manifest 校验,
    // 摘要漏一项就拒收,旧历史不动。
    if (const auto& state = todo_state(); state != nullptr) {
        for (const auto& item : state->items) {
            if (item.status != lubancode::tools::TodoStatus::Completed && !item.content.empty()) {
                options.required_open_items.push_back(item.content);
            }
        }
    }
    return options;
}





// 双胞胎合一(会话终章):RunUserTurn/RunPeerTurn 收成这一只带来源参数的
// 回合入口。User 走全套(配置门/建档/窗口同步/自动压缩/提及账/标题铃/
// trace 与 usage 记账/计划与记忆收尾);Incoming(peer 来信、后台子代理
// 回流)走精简路:peer 名册亮忙,silent 档可静默(查看态回流不上屏),
// 不挂录制、不追 usage、不做提及与收尾抽取。两路差异逐一保真:
//   - User 拦空配置(欢迎页逻辑),Incoming 不拦(能起 peer 必已配好);
//   - User 的 turn_id 复 trace 那枚(两本账对得上),Incoming 由适配器现发;
//   - 排队账快照(PersistSteeringQueue)两路都收(轮内可能进队/送走过)。
void TerminalSessionController::RunSessionTurn(const std::string& content, TurnSource source,
                                               bool* autosend_failed, bool silent,
                                               memory::QueryOrigin origin) {
    const bool is_user_turn = source == TurnSource::User;
    if (!is_user_turn) {
        peer_wiring_.SetStatus("busy");
    }
    if (is_user_turn) {
        // 欢迎页允许空配置进主界面;slash 命令在 ProcessLine 上一层已先分流。
        // 普通正文到这里才拦,免得拿空 base_url 真发请求、落下一场假会话。
        if (!lubancode::config::RequireConfigured(config_result_).has_value()) {
            TermOut() << theme.error << tr("setup.turn.blocked") << theme.reset << "\n";
            if (autosend_failed != nullptr) {
                *autosend_failed = true;
            }
            return;
        }
        // 建档提前到发轮之前(第二期):仓要拿 session id 开张,第一轮请求里
        // 的超长结果才有地方落盘。失败不拦会话,只是没有 artifact 可追。
        EnsureSessionBegun(content);
        // 窗口同步(0.27.x):/context、/model 改的是 tracker 的窗口,loop 的
        // mid-turn 评估用同一份,发轮前对齐一次。
        main_agent->SetContextWindowTokens(context_tracker.window_tokens());
        // 自动压缩:发真正的用户输入前,占用超过阈值(80%)就先压一压。失败只
        // 警告不拦——字符数硬安全网(TrimHistory)还在,不会真的爆掉;工具循环
        // 中途的溢出由 loop 的压力通报(HandleContextPressure)另走 mid-turn 路。
        if (context_tracker.ShouldAutoCompact()) {
            lubancode::app::TryRunCompact(/*midturn=*/false, MakeCompactInputs());
        }
    }
    // 人在聚焦查看画面里直接敲了正文发送:视为离开聚焦态(新一轮输出
    // 马上往下铺,聚焦画面已经不是"当前画面"了),下次 Ctrl+E 是重新
    // 聚焦,不是"返回"。
    transcript_ui_.ExitFocusView();
    std::string turn_suffix;
    if (is_user_turn) {
        // @ 提及校验(0.30.x 第三批):目标消失/越出项目根,明报错拦下这轮;
        // 活着的提及附账进 turn context(不进永久 history)。
        const auto [mention_error, mention_ledger] = BuildMentionLedger(content);
        if (!mention_error.empty()) {
            TermOut() << theme.error << mention_error << theme.reset << "\n";
            if (autosend_failed != nullptr) {
                *autosend_failed = true;  // 这轮没发出去:自动发送的消息按"没送达"回队
            }
            return;
        }
        turn_suffix = mention_ledger;
    }
    turn_suffix += project_memory != nullptr
                       ? project_memory->BuildTurnContext(content, std::filesystem::current_path(), origin)
                       : std::string();
    // 运行中子代理名册(规格第二节):每条外层用户消息/外来消息到来时给
    // main 一份动态重算的名册——task id + 真 title + 类型 + 待送数,不塞
    // prompt 与日志。走请求级 turn_context:不永久复制进 history,任务状态
    // 变了下轮重算,compact 后照常从台账重注入。主模型认得 task id,才知道
    // agent_message 该投给谁。
    if (session_agent_tool() != nullptr) {
        turn_suffix += session_agent_tool()->RunningTasksRoster();
    }
    // PTC 指南(PTC 单):当前已挂载 stub 的签名索引,随轮次请求视图走
    //(不进稳定的 system——前缀缓存守恒)。tool_search 中途挂载新工具,
    // 下一轮这里自动带上新签名。
    if (tool_runtime_->ptc_tool() != nullptr) {
        turn_suffix += tool_runtime_->ptc_tool()->GuideSegment();
    }
    main_agent->SetTurnContext(std::move(turn_suffix));
    std::size_t history_before = 0;
    std::string trace_turn_id;
    lubancode::runtime::TurnUsageStats turn_usage;
    const auto turn_started = std::chrono::steady_clock::now();
    if (is_user_turn) {
        // 查看帧的 app 侧擦账已拆(见 PrintViewedTranscript 注释):新回合铺
        // 正文不再需要在这里复位什么行账,终端层那本 view_body_top 按读取段
        // 自生灭。终端标题(0.30.x 第四批):跑着/等输入两态,项目·分支跟
        // 着;拿不到焦点状态,不做"未聚焦才通知"的假判断,只在长轮收口时
        // 叫一声铃。
        history_before = main_agent->History().size();
        if (spinner_enabled) {
            lubancode::cli::SetTerminalTitle(BuildTerminalTitleText(tr("notify.state_busy")));
        }
        // usage 出账(模型分工第一期):整轮逐步 usage 带出来记进分角色台账
        // (普通 turn = normal 档);compact/抽取的后台采样在各自路径另记,
        // 不混进这里。
        trace_turn_id = session_runtime_.ids().NextTurnId();
        turn_views_.emplace_back();
    }
    // 批二:这轮的事件适配器(sink 已在 SessionRuntime 上配好;User 的
    // turn_id 复 trace 那枚,两本账对得上;Incoming 由适配器现发——这轮
    // 没有 trace 口径的现成号)。
    lubancode::runtime::TurnEventAdapter turn_events = session_runtime_.MakeTurnAdapter();
    // 批三:RunTurn 二十四参收成一只 TurnContext。
    lubancode::app::TurnContext turn;
    turn.loop = &*main_agent;
    turn.user_input = content;
    turn.auto_confirm = auto_confirm;
    turn.always_allowed_tools = &always_allowed_tools;
    turn.theme = theme;
    turn.context_tracker = &context_tracker;
    turn.registry = &registry();
    turn.hook_dispatcher = lubancode::app::HookRuntime();
    turn.is_console = spinner_enabled;
    turn.transcript = &transcript_ui_.items();
    turn.todo_state = todo_state();
    turn.transcript_expanded = transcript_ui_.expanded_flag();
    turn.allow_commands = settings_local.allow_commands;
    turn.deny_commands = settings_local.deny_commands;
    turn.completion_agent = session_agent_tool();
    turn.recorder = is_user_turn ? record_wiring_.recorder() : nullptr;
    turn.silent = silent;
    turn.turn_events = &turn_events;
    if (is_user_turn) {
        turn.usage_out = &turn_usage;
        turn.trace_hub = &*trace_hub_;
        turn.thread_id_for_trace = session_runtime_.thread_id();
        turn.turn_id_for_trace = trace_turn_id;
        turn.turn_view_out = &turn_views_.back();
        turn.mode_gate = [this](const std::string& tool_name, const nlohmann::json& input) {
            return plan_wiring_.EvaluateGate(tool_name, input);
        };
        turn.approval_observer = [this](bool asked, bool allowed) {
            loop_wiring_.NotePermissionWait(asked, allowed);
        };
    }
    const lubancode::app::RunTurnResult turn_result = RunTurn(std::move(turn));
    if (is_user_turn) {
        // 轮次视图存档封顶(最近 N 轮,重铺够用;不无界攒)。
        if (turn_views_.size() > kMaxArchivedTurnViews) {
            turn_views_.erase(turn_views_.begin());
        }
        if (autosend_failed != nullptr) {
            *autosend_failed = turn_result.status != 0;  // 取走即消费单:失败信号原样递给会话泵
        }
        for (const auto& step : turn_usage.steps) {
            api::Usage step_usage;
            step_usage.input_tokens = step.input_tokens;
            step_usage.output_tokens = step.output_tokens;
            step_usage.cache_read_tokens = step.cache_read_tokens;
            step_usage.cache_creation_tokens = step.cache_creation_tokens;
            step_usage.output_reasoning_tokens = step.reasoning_tokens;
            model_router->ledger().Record(lubancode::agent::ModelRole::Normal,
                                          step.model.empty() ? *current_model : step.model, step_usage,
                                          /*duration_ms=*/0, step.reported);
        }
        if (spinner_enabled) {
            lubancode::cli::SetTerminalTitle(BuildTerminalTitleText(tr("notify.state_idle")));
            const auto elapsed = std::chrono::steady_clock::now() - turn_started;
            if (elapsed > std::chrono::seconds(30)) {
                lubancode::cli::NotifyUserAttention();  // 长轮跑完叫一声,每轮至多一次
            }
        }
    }
    // 每轮结束(成功/出错/ESC 打断都算)把新增消息逐条追加落盘。
    PersistNewMessages();
    if (is_user_turn) {
        // Plan 模式(只读研究硬闸单):turn 正常收口后扫本轮 assistant 正文,
        // <proposed_plan> 完整则记 PlanDocument 并弹审阅框(单子:不在解析到
        // </proposed_plan> 的同一次 Provider response 内直接执行——工具表、
        // 提示词、mode event 与 UI 都在半新半旧状态时不动手)。
        if (turn_result.status == 0 && !turn_result.cancelled) {
            plan_wiring_.CollectProposal(history_before, trace_turn_id);
        }
        // 会话起名(cheap 角色):建档后第一轮回合收尾、还没有标题时起一枚,
        // 成功落 title 事件;失败安静降级(/sessions 继续用首句摘要,不拦人)。
        lubancode::app::MaybeGenerateSessionTitle(MakeTailContext(), lubancode::agent::TaskKind::SessionTitle);
        // 回合收尾总结与候选抽取(learn off 时是空操作)。
        lubancode::app::ExtractTurnMemory(MakeTailContext(), content, history_before);
    }
    // 排队账快照落档(路径二):轮内可能进过队/边界注入送走过,趁收尾把
    // 最新一份快照追进存档,/exit 或崩掉后 resume 接得回来。
    PersistSteeringQueue();
    if (!is_user_turn) {
        peer_wiring_.SetStatus("idle");
    }
}
// ---------------------------------------------------------------------------
// 上下文压缩的会话现场路(0.27.x 分层压缩第一期)
// ---------------------------------------------------------------------------

SessionCommandState TerminalSessionController::MakeSessionCommandState() {
    return SessionCommandState{
        [this](bool preserve_history) { RebuildLoop(preserve_history); },
        *main_agent,
        session_store,
        persisted_count,
        session_compact_epoch,
        session_meta,
        session_title,
        session_title_pending,
        session_store_broken,
        session_start_ts,
        [this]() {
            // /clear:旧上下文就此终局——SessionEnd(reason=clear) 先发,新的
            // 空会话用 SessionStart(source=clear) 开账。仓也关掉:工具们持
            // 同一只仓,scope 只跟当前会话,旧场子的 artifact 查不到。
            artifact_store->Close();
            EmitSessionHook(lubancode::hooks::HookEvent::SessionEnd, nlohmann::json{{"reason", "clear"}}, "clear");
            EmitSessionHook(lubancode::hooks::HookEvent::SessionStart, nlohmann::json{{"source", "clear"}},
                            "clear");
            if (project_memory != nullptr) {
                project_memory->set_source_session(session_start_ts);
            }
        },
        [this](const std::string& title) { peer_wiring_.SetName(title); },
        [this]() { SyncWorktreeDirectory(); },
        [this]() { CleanupBackgroundAgents(/*dispose_queue=*/true); },
        &worktree_session,
        sessions_dir,
        wire_str,
        current_model,
        // /resume 成功:恢复的历史开新账(SessionStart source=resume),
        // 仓也按恢复的那场开张(旧档若落过盘,artifact 继续可追)。
        [this]() {
            EmitSessionHook(lubancode::hooks::HookEvent::SessionStart, nlohmann::json{{"source", "resume"}},
                            "resume");
            OpenArtifactStore();
            // 持久目标单:goal 事件账随档恢复(默认 paused-on-resume)。
            goal_wiring_.RestoreFromArchive();
        },
        // /resume 的排队账重建(路径二):存档快照灌回会话层队列。
        [this](const std::vector<lubancode::sessions::ArchivedQueueItem>& items) {
            RestoreSteeringQueueFrom(items);
        },
        // Plan 模式单:/resume 的 mode/plan/review 账恢复。
        [this](const std::optional<lubancode::sessions::ModeEvent>& mode_event,
               const std::vector<lubancode::sessions::PlanEvent>& plans,
               const std::optional<lubancode::sessions::PlanReviewEvent>& review) {
            plan_wiring_.RestoreFromArchive(mode_event, plans, review);
        }};
}

void TerminalSessionController::EmitSessionHook(lubancode::hooks::HookEvent event, nlohmann::json fields,
                                         const std::string& match_value) {
    lubancode::hooks::HookDispatcher* dispatcher = lubancode::app::HookRuntime();
    if (dispatcher == nullptr || dispatcher->Empty() || !dispatcher->HasHandlersFor(event)) {
        return;
    }
    // SessionStart 的两个来源(startup 已在 cli_app 发过、这里管 resume/clear/
    // compact)都要把转录路径与 session id 对齐——存档文件名就是会话 id
    // (MakeSessionId 落盘时定的),resume 后这份才是真的。
    if (event == lubancode::hooks::HookEvent::SessionStart) {
        lubancode::hooks::HookContext ctx = dispatcher->context();
        if (session_store.active()) {
            ctx.transcript_path = session_store.file_path();
            // 会话存档名就是会话 id(MakeSessionId:时间戳 + 首句 slug,slug
            // 原样保留多字节字符)——GBK 机器上 .string() 遇 emoji 就是
            // 1113 异常,一律走 u8 通道。
            ctx.session_id = lubancode::tools::PathToUtf8(
                lubancode::tools::Utf8ToPath(session_store.file_path()).stem());
        }
        lubancode::app::UpdateHookRuntimeContext(ctx);
    }
    lubancode::hooks::HookPayload payload;
    payload.event = event;
    payload.fields = std::move(fields);
    payload.match_value = match_value;
    dispatcher->Emit(event, payload);
}

// /clear 与退出共用的清场:停全部子代理、收面板。排队消息分两档
// (取走即消费单):
//   - dispose_queue=true(/clear):用户明说清场——全数倒掉,落空快照,
//     醒目告知(条数 + 首条预览),"明确丢弃、不静默消失"。
//   - dispose_queue=false(退出/析构):队列**不倒**。账在 Run() 退场前已
//     落档(resume 接得回,验收"排队→/exit→resume 队列还在"),这里只
//     提示一句"已存档几条";倒掉反而把刚落的档废了。
void TerminalSessionController::CleanupBackgroundAgents(bool dispose_queue) {
    // 面板收场:查看态/收件目标整份收干净,不给已收场的任务留悬空目标。
    lubancode::cli::ResetAgentPanelSession();
    if (dispose_queue) {
        // 会话层排队消息:先倒账、再把"空快照"追进存档(resume 不复活已
        // 倒掉的账),醒目告知(路径三:条数 + 首条预览,淡字换醒目色)。
        const auto discarded = SessionSteeringQueue().TakeAllForDisposal();
        if (!discarded.empty()) {
            PersistSteeringQueue();
            for (const std::string& row : lubancode::cli::BuildQueueDisposalRows(discarded)) {
                TermOut() << theme.error << row << theme.reset << "\n";
            }
        }
    } else if (!SessionSteeringQueue().empty()) {
        // 退场:排队账已在 Run() 落档,这里只说一句去处,别让人以为丢了。
        const auto snapshot = SessionSteeringQueue().Snapshot();
        for (const std::string& row : lubancode::cli::BuildQueueArchiveRows(snapshot)) {
            TermOut() << theme.stats << row << theme.reset << "\n";
        }
    }
    if (session_agent_tool() == nullptr) {
        return;
    }
    session_agent_tool()->CancelAllTasks();
    for (const auto& line : session_agent_tool()->TakeUndeliveredInboxReport()) {
        TermOut() << theme.stats << line << theme.reset << "\n";
    }
}







void TerminalSessionController::Run() {
    while (true) {
        // status panel 每圈都重取 cwd 与 Git 分支。/worktree、run_command
        // 切目录/分支，或队列紧接着发下一条时，都不会挂着上一帧的旧值。
        lubancode::cli::StatusPanelData status_data;
        status_data.model = *current_model;
        status_data.cwd = CurrentDirUtf8();
        status_data.git_branch = lubancode::cli::CurrentGitBranch(std::filesystem::current_path());
        status_data.worktree = worktree_session.active_name();
        status_data.provider = active_provider;
        status_data.effort = *current_think;
        status_data.context_percent = context_tracker.UsagePercent();
        status_data.used_tokens = static_cast<long long>(context_tracker.current_tokens());
        status_data.window_tokens = static_cast<long long>(context_tracker.window_tokens());
        // 缓存注记(缓存诊断单):与回合内局部更新同一只 helper、同一只
        // tracker,空闲重建的第一帧不会先新后旧。
        status_data.cache_note =
            lubancode::cli::BuildCacheNote(context_tracker, !context_tracker.usage_stale());
        // 旧值标记同样出自 tracker:回合内 on_usage 局部发布的快照与这里整份
        // 重建读同一只 ContextTracker,数字与 ~ 标记完全一致,收口后的第一只
        // composer 不会先新后旧。
        status_data.context_stale = context_tracker.usage_stale();
        // REC 标记:录制中恒挂状态行第一段(见 StatusPanelData::rec)。
        status_data.rec = lubancode::cli::RecorderStatusMarker(record_wiring_.recorder_optional());
        // 工具调用后端档(PTC 单):json 默认时留空(状态行零变化),
        // programmatic/auto 时恒亮一段,回落原因写全(规格 UI 节)。
        if (config.tool_calling != lubancode::config::ToolCallingMode::Json) {
            status_data.tools = tool_runtime_->ptc_resolution();
        }
        // Plan 模式标记(只读研究硬闸单):与 confirm/auto/yolo 并列(规格
        // "plan · confirm"),不重置确认档。
        if (session_runtime_.collaboration_mode() == lubancode::runtime::CollaborationMode::Plan) {
            status_data.plan_mode = tr("plan.mode_label");
        }
        // goal/loop 会话状态段(goal 单合流):有常驻自动工作在跑才挂。
        status_data.goal_loop =
            lubancode::app::BuildGoalLoopStatusSegment(goal_wiring_.coordinator(), loop_wiring_.scheduler());
        lubancode::cli::SetStatusLineData(status_data, config.status_panel.items, config.status_panel.separator);

        // 后台命令完成通知:每圈开头取一次"新进入终态"的任务,有就打一行淡色
        // 通知给用户。不插进对话流(不发给模型、不消耗 token)——只让人看见
        // "后台那条命令跑完了";模型要是需要细节,自己调 background_output 工具查。
        // 跟 pending_queue 那条路分开:排队消息是用户自己键入的正文,要发给模型;
        // 后台通知是系统侧的状态播报,只给人看。
        if (const auto finished = lubancode::tools::BackgroundTaskRegistry::Instance().DrainCompleted();
            !finished.empty()) {
            std::lock_guard<std::mutex> stdout_lock(lubancode::cli::StdoutWriteMutex());
            for (const auto& t : finished) {
                const char* label = "已结束";
                switch (t.status) {
                    case lubancode::tools::BackgroundTaskStatus::Completed: label = "完成(退出码 0)"; break;
                    case lubancode::tools::BackgroundTaskStatus::Failed: label = "失败"; break;
                    case lubancode::tools::BackgroundTaskStatus::Stopped: label = "已停止"; break;
                    case lubancode::tools::BackgroundTaskStatus::StopFailed: label = "停止失败"; break;
                    default: break;
                }
                TermOut() << theme.stats << "[后台任务 #" << t.task_id << " " << label << "]";
                if (t.status != lubancode::tools::BackgroundTaskStatus::Completed) {
                    TermOut() << " (exit "
                              << (t.exit.exit_code.has_value() ? std::to_string(*t.exit.exit_code) : "unknown")
                              << ")";
                }
                TermOut() << " " << t.command << theme.reset << "\n";
            }
        }

        // 0.28.x 会话泵:把流式期间排下的消息送上路。子代理目标先转投任务
        // inbox(SendTaskMessage 那套,共用面板定向介入的通道);main 目标取
        // 队头自动发送——本轮没再调工具自然收尾的场合,队列紧接着成为下一
        // 次请求的用户消息,不等用户再敲一下(规格)。用户自己的排队消息
        // 优先于 peer 来信与子代理完成回流,所以泵挂在它们前头。
        //
        // 取走即消费单(路径一):拿去自动发送的那条,若这一轮以请求失败
        // 收场,原样还回队首并带"已试过一次"的账——同一条最多自动重试
        // 一次,再失败留队列等用户手动(Shift+← 取回改写再排、或删掉),
        // 错误文案旁明写一句"没送达,已回队",不再无声吞掉。
        PumpSteeringToSubagents();
        if (auto head = SessionSteeringQueue().TakeFirstAutoSendable(lubancode::cli::MessageTarget::Main())) {
            TermOut() << theme.prompt << "> " << theme.reset << head->text << "\n";
            peer_wiring_.SetStatus("busy");
            bool autosend_failed = false;
            const CommandFlow flow = ProcessLine(head->text, &autosend_failed);
            peer_wiring_.SetStatus("idle");
            if (autosend_failed) {
                // 失败退还:回队首(带 attempts+1),文案旁明说。还会再自动
                // 试一次(attempts < 2);到顶的那次退还后队列里留着,泵的
                // 防死循环闸跳过它,等用户处置。
                std::string preview = head->text;  // 先留底,ReturnToFront 会 move 走
                SessionSteeringQueue().ReturnToFront(std::move(*head));
                preview.erase(0, preview.find_first_not_of("\r\n\t "));
                const std::size_t preview_cut = preview.find('\n');
                if (preview_cut != std::string::npos) {
                    preview.resize(preview_cut);
                }
                TermOut() << theme.error << trf("queue.autosend_returned", preview) << theme.reset << "\n";
            }
            PersistSteeringQueue();
            if (flow == CommandFlow::Exit) {
                break;
            }
            continue;
        }
        // main 目标都送空了:"打断并立即送"的状态旗收掉(队列区标题复位;
        // TargetGone/失败条目留在原位等用户处置,不算"没送完")。
        if (!SessionSteeringQueue().HasDeliverable(lubancode::cli::MessageTarget::Main())) {
            SessionSteeringQueue().ClearImmediateDelivery();
        }

        // loop 单:定时循环的泵。优先级在用户排队消息之后、peer 来信与
        // 子代理回流之前——用户输入最高,审批/peer/子代理完成次之,loop
        // 最后;每圈只消费一拍,回主循环顶重新检查高优先级来源(不一次把
        // 八枚 due 全倒进队列,用户按 stop 时还来得及)。泵只在会话空闲
        // (当前没有 loop 拍在跑)时取件;single-flight 由 scheduler 保证。
        if (loop_wiring_.scheduler() != nullptr && !loop_wiring_.TickActive() &&
            loop_wiring_.HasActiveTasks()) {
            if (PumpScheduledWork()) {
                continue;
            }
        }

        // 跨会话来信:空闲当口(不在 Run 里)收进来的信,经确认后直接
        // 另起一轮外来消息,不等用户再敲一行。用户自己的排队消息优先。
        peer_wiring_.CollectHeldMessages();
        const auto incoming_peer = peer_wiring_.TakeReadyMessage();
        if (incoming_peer.has_value() &&
            !SessionSteeringQueue().HasDeliverable(lubancode::cli::MessageTarget::Main())) {
            const lubancode::peers::PeerEnvelope envelope = std::move(*incoming_peer);
            TermOut() << theme.stats
                      << trf("cmd.peers.incoming_notice", envelope.sender_name, envelope.sender_id) << theme.reset
                      << "\n";
            RunSessionTurn(lubancode::app::FormatPeerText(envelope), TurnSource::Incoming);
            continue;
        }

        // 后台子代理结果回流:任务在会话空闲时跑完的,结果不能干等用户再敲
        // 一行才送达——面板只画"完成",真正让主循环动起来的是这里。检测到
        // 未投递的完成结果就另起一轮(同外来消息那条路,不落 slash),RunTurn
        // 开头会把 DrainCompletionNotices 拿到的结果原文附带进消息。用户自己
        // 排队的消息优先:队列非空时先让队头那条走,它起 RunTurn 一样能把
        // 结果捎上。
        // 完成通知(规格"现场二"):短进度行 + 归 main 的 transcript 事件,
        // 有且只有一条——旧底栏已在 wake 路正式退场(RetireIdleChrome),通知
        // 不再夹在两副 chrome 中间当一行来路不明的永久字;Ctrl+O/Ctrl+E 重铺
        // transcript 时它跟着回来。
        // 查看态(回流单规格第一节):用户正看某只子代理时,回流照常发生但
        // 一切终端影响收进后台——通知不打裸 cout(事件照进 main 台账),主轮
        // 走静默档(输出进台账、usage 照记、查看帧零扰动),收口后坞里那行
        // 退场,导航坞提示行给一枚短 toast。回 main 时新内容都在。
        if (session_agent_tool() != nullptr &&
            !SessionSteeringQueue().HasDeliverable(lubancode::cli::MessageTarget::Main()) &&
            session_agent_tool()->HasUndeliveredCompletions()) {
            const bool viewing = lubancode::cli::CurrentAgentViewedTaskId() != 0;
            std::string reflow_ids;
            if (viewing) {
                for (const int id : session_agent_tool()->UndeliveredCompletionTaskIds()) {
                    if (!reflow_ids.empty()) {
                        reflow_ids += " ";
                    }
                    reflow_ids += "#" + std::to_string(id);
                }
            }
            const std::vector<std::string> notices = session_agent_tool()->CompletionNoticeLines();
            // goal 合流:子代理完成喂 goal 的证据/usage 账(没有活跃 goal
            // 零影响);在 RunPeerTurn 之前记,证据落在"消化回流"那轮的
            // 采证之前,checkpoint 引用得着。
            goal_wiring_.NoteSubagentCompletion();
            {
                auto& transcript = transcript_ui_.items();
                lubancode::cli::TranscriptItem item = lubancode::cli::MakeNoticeItem(
                    static_cast<int>(transcript.size()) + 1, tr("agent_panel.completion_notice"),
                    lubancode::cli::TranscriptStatus::Ok, notices);
                if (!viewing) {
                    std::lock_guard<std::mutex> stdout_lock(lubancode::cli::StdoutWriteMutex());
                    TermOut() << theme.tool_line << item.title << theme.reset << "\n";
                    for (const auto& note : notices) {
                        TermOut() << theme.stats << "  ⎿ " << note << theme.reset << "\n";
                    }
                    TermOut().flush();
                }
                transcript.push_back(std::move(item));
            }
            RunSessionTurn("后台子代理有新结果送达(资料附在本条消息里)。请阅读后继续推进手头任务;"
                           "若结论已够用,向用户简要汇报要点,不要重新摸排。",
                           TurnSource::Incoming, /*autosend_failed=*/nullptr,
                           /*silent=*/viewing, memory::QueryOrigin::BackgroundCompletion);
            if (viewing) {
                // 坞行退场由 DrainCompletionNotices 的 TouchTasks + 下一帧带出;
                // toast 替那行留一句人话,几秒自收,不抢屏。
                lubancode::cli::ShowPanelToast(trf("agent_panel.reflow_toast", reflow_ids));
            }
            continue;
        }

        std::string content;
        std::optional<int> composer_target;  // 这条话若出自查看态 composer,收件人是那只子代理
        // UI-A:主提示符是唯一开 composer 的读取点——Alt/Shift+Enter 插
        // 换行、Enter 全发、全空白不发送。别的 ReadLine 调用点(确认提示、
        // /model 编号选择、向导)保持单行语义。查看态里提交的话,收件
        // 目标由面板控制器记着(输入框上横线右端的短标题就是它)。
        const std::optional<std::string> line =
            lubancode::cli::ReadLine(theme.prompt + "> " + theme.reset, theme,
                                      /*esc_rejects=*/false, /*composer=*/true);
        if (!line.has_value()) {
            if (lubancode::cli::ComposerStashHasContent()) {
                TermOut() << theme.stats << tr("stash.still_there") << theme.reset << "\n";
            }
            PersistSteeringQueue();  // EOF 退场同路(路径二,"先留后清")
            break;  // EOF:Ctrl+Z 或管道读尽
        }
        if (line->empty()) {
            continue;  // 空行不退出,重新给提示符
        }
        content = *line;
        composer_target = lubancode::cli::CurrentComposerAgentTarget();

        if (content == "exit" || content == "quit") {
            if (lubancode::cli::ComposerStashHasContent()) {
                TermOut() << theme.stats << tr("stash.still_there") << theme.reset << "\n";
            }
            PersistSteeringQueue();  // 裸退场同样先留账(路径二,"先留后清")
            break;
        }
        // 定向介入(规格第七节):查看态 composer 提交的话直接进那只子代理
        // 自己的 inbox,在"当前工具收尾、下一次请求未发"的边界注入它的
        // history——不经 main,不串台。slash 命令仍走会话主路(/exit 这类
        // 会话级动作不该被子代理视角扣下)。终态明确拒收,不改投 main。
        if (composer_target.has_value() && !content.empty() && content.front() != '/' &&
            session_agent_tool() != nullptr) {
            const lubancode::tools::TaskMessageStatus status =
                session_agent_tool()->SendTaskMessage(*composer_target, content);
            TermOut() << theme.stats
                      << (status == lubancode::tools::TaskMessageStatus::Queued
                              ? trf("agent_panel.target_queued", *composer_target)
                              : trf("agent_panel.target_rejected", *composer_target))
                      << theme.reset << "\n";
            continue;
        }
        peer_wiring_.SetStatus("busy");  // 名册上亮"忙",对端知道别指望立刻回话
        const CommandFlow flow = ProcessLine(content);
        peer_wiring_.SetStatus("idle");
        if (flow == CommandFlow::Exit) {
            // 退出前把排队账最后一眼落档(路径二):/exit 这轮里可能还排着
            // 没送走的话,resume 要接得回来。CleanupBackgroundAgents 里那趟
            // 落的是清账后的空快照,先后次序就是"先留后清"。
            PersistSteeringQueue();
            break;
        }
    }
}

int RunInteractiveSession(const InteractiveSessionOptions& options) {
    TerminalSessionController session(options);
    session.Run();
    return 0;
}

}  // namespace lubancode::app
