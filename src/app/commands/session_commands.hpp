// 会话类 slash 命令:/sessions 列档、/resume 恢复、/export 导出 Markdown。
// 底层读写在 sessions/session_store,这里只管选择交互与输出拼装。
//
// 搬家自 main.cpp,行为一字未改;依赖只认 agent/cli/platform。


#pragma once

#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <string_view>

#include "agent/compact.hpp"
#include "agent/context_budget.hpp"
#include "agent/agent.hpp"  // Agent/AgentProfile(批四自立门户)
#include "agent/context.hpp"  // EstimateHistory*:token 估算
#include "agent/loop.hpp"
#include "agent/prompt_assembler.hpp"  // PromptOptions(/context 的系统提示估算)
#include "sessions/session_store.hpp"
#include "sessions/session_lifecycle.hpp"
#include "api/backend.hpp"
#include "app/commands/command_flow.hpp"
#include "cli/slash_commands.hpp"  // ParsedSlashCommand(分派注册制)
#include "app/hook_runtime.hpp"
#include "app/model_router.hpp"
#include "config/config.hpp"
#include "cli/console_input.hpp"
#include "cli/context_tracker.hpp"
#include "cli/i18n.hpp"
#include "cli/theme.hpp"
#include "cli/worktree.hpp"
#include "hooks/dispatcher.hpp"
#include "platform/paths.hpp"
#include "tools/tool.hpp"

namespace lubancode::app {

using lubancode::cli::tr;
using lubancode::cli::trf;

// 粗略估算一段历史占用了多少"token"——统一口径在 agent/context.hpp
// (ASCII 4 字符 1 token,非 ASCII 1.5 token/字);这里只是转发,老调用点
// (/context 分类明细)不必各认一遍。数字带 ~ 提醒是估算值,真实用量以
// provider usage 为准。
std::size_t EstimateHistoryChars(const std::vector<lubancode::api::Message>& history);

std::size_t EstimateHistoryTokens(const std::vector<lubancode::api::Message>& history);

// 压力口径估算(P1-1,手工/自动压缩共用):任意一份 history 过一遍 L1 无损
// 结构压缩(临时 memo/stats,不落盘不定形)后按统一 token 口径估。触发线、
// /context 对话历史、压缩前后账与反涨断言都拿这一把尺。
std::size_t PressureEstimateTokens(lubancode::agent::Agent& loop,
                                   const std::vector<lubancode::api::Message>& history);

// 反涨闸(0.26.84 治三,阶段 0 收口):压缩后的新史(压力口径)不比旧史小
// 时拒收——手工 /compact 与自动 TryRunCompact 共用同一只。返回 true = 拒收
// (回执已打,调用方就地收场,旧 history 一字不动)。
bool RejectGrownCompactHistory(const lubancode::cli::Theme& theme, std::size_t before_tokens,
                               std::size_t new_tokens);


// /context 命令:不带参数打分类占用分析(系统提示/工具定义/对话历史三类
// 统一口径 token 估算 + 条形图,拼装规则全在 FormatContextBreakdown,这里
// 只管收集与打印);带参数(256k/512k/1m/裸数字)临时改窗口大小,只本会话
// 生效,不改配置文件。sys_tokens/tools_tokens/history_tokens 由调用方在会话
// 现场按统一口径(agent/context.hpp)算好(裸敲才用得上,带参数分支忽略),
// 缓存命中/窗口/实测占用都从 context_tracker 拿。cache_epoch 是 loop 的
// 前缀记账序号(agent/prefix.hpp),有实测 usage 时多打一行前缀缓存账。
// main_profile(可空):当前 loop 实际吃到的运行策略,非空时多打一行输出
// 上限与来源(规格根因一:"本轮上限"看得见,unset 也说破)。
// /context 的分层占用与预算报告(第四期,规格"/context"节):由会话现场
// 收集——视图各层的枚数与字节、ContextBudgetPlan 总账、最近一次 compact
// 的台账。字段全可选/零值安全,没有就不打那一行。
struct ContextLayersReport {
    std::size_t inline_full_results = 0;      // 视图里全文的结果枚数(L0)
    std::size_t artifact_previews = 0;        // L1 预览枚数
    std::size_t reclaimable_bytes = 0;        // 结构压缩最近一次请求省下的字节
    std::optional<lubancode::agent::ContextBudgetPlan> budget;
    // 最近一次 compact:"cheap:m · 62k→18k · 3.2s · 校验通过";空 = 没压过。
    std::string last_compact_line;
};

// usage_ledger(可空,模型分工第一期):分角色 usage 台账,非空时列一节
// "模型调用分角色账"——普通 turn 归 normal,压缩/抽取/标题归 cheap,
// 回退另有留痕(规格"路由看得见")。roles_table(可空,问题 6):三角色
// 有效路由,非空时账目固定列全 cheap/normal/lao,零调用角色写明"本场
// 未触发 + 默认职责",回落关系与 /model roles 同一份 source 口径。
void HandleContextCommand(const std::string& args, lubancode::cli::ContextTracker& context_tracker,
                           std::size_t sys_tokens, std::size_t tools_tokens, std::size_t history_tokens,
                           const lubancode::cli::Theme& theme, int cache_epoch = 1,
                           const lubancode::agent::AgentRuntimeProfile* main_profile = nullptr,
                           const lubancode::agent::ModelUsageLedger* usage_ledger = nullptr,
                           const lubancode::agent::ContextArtifactStore* artifact_store = nullptr,
                           const ContextLayersReport* layers = nullptr,
                           const lubancode::agent::ModelRouteTable* roles_table = nullptr,
                           int compact_partition_count = 0);

// ---- /context 的会话现场收集(终端接线收尾单自大类搬出) ------------------
//
// 裸敲才现算三类 token(带参数走切窗口分支,收了也白收)与分层预算账,
// 收完递给上面那只 HandleContextCommand 打印。口径对齐"实际发出的请求",
// token 全按统一口径(agent/context.hpp)折算:系统提示 = 拼装结果 +
// 目录 base_instructions + 魂;工具定义 = 会真进 tools 数组的工具 +
// 延迟索引段;对话历史 = loop.History() 全量。
struct ContextEstimateInputs {
    const lubancode::agent::PromptOptions* prompt_options = nullptr;
    const std::string* model_instructions = nullptr;  // 目录 base_instructions
    const std::string* soul = nullptr;
    lubancode::tools::ToolRegistry* registry = nullptr;
    const std::function<bool(const lubancode::tools::Tool&)>* tool_filter = nullptr;  // 延迟挂载谓词
    bool tool_deferral = false;
    const std::set<std::string>* loaded_tools = nullptr;
    lubancode::agent::Agent* agent = nullptr;  // 活 loop(要能会话路由)
    lubancode::cli::ContextTracker* context_tracker = nullptr;
    const lubancode::agent::ModelUsageLedger* usage_ledger = nullptr;  // 可空
    // 三角色有效路由(问题 6:零调用角色列全 + 回落口径)。可空(单测/
    // 无路由路径,分角色账退回无模型名的裸账)。指向调用方栈上现算的
    // ModelRouterService::Table() 副本,本结构不拥有。
    const lubancode::agent::ModelRouteTable* roles_table = nullptr;
    const lubancode::agent::ContextArtifactStore* artifact_store = nullptr;  // 可空
    const std::string* last_compact_line = nullptr;
    // compact_partition_count(§八,Compact 四分区单·阶段 1):/context 展示
    // "compact turn 策略"一行用;0 = 现场没带到(不打那一行)。
    int compact_partition_count = 0;
};
void RunContextCommand(const std::string& args, const ContextEstimateInputs& in,
                       const lubancode::cli::Theme& theme);

// ---- /compact 的会话接线(终端接线收尾单自大类搬出) ----------------------
//
// PreCompact 钩子闸 → cheap 路由 → HandleCompactCommand 正戏 → 分角色
// 记账 + 状态栏短闪 → 落盘基线收缩 + compact_v2 事件追加(goal/loop
// snapshot 由会话补)→ PostCompact/SessionStart 审计钩子。

// 压缩滞回的会话活账(P1-1 连环压缩):由会话控制器持有,随材料包递进
// TryRunCompact。上次压缩收口(成功换账、反涨拒收或失败收场都算)时的
// 压力口径估算记在 last_post_tokens;下次触发先问 ShouldSkipCompact-
// ForHysteresis——新增不足滞回带就不压,同一 turn 无进展不得连压。
struct CompactHysteresis {
    bool armed = false;                 // 本场是否已有一次压缩收口
    std::size_t last_post_tokens = 0;   // 上次收口时的压力口径估算
};
struct CompactSessionInputs {
    lubancode::agent::Agent* agent = nullptr;
    const lubancode::cli::Theme* theme = nullptr;
    bool spinner_enabled = false;
    int* session_compact_epoch = nullptr;          // 本场第几次压缩(递给正戏)
    std::string* last_compact_line = nullptr;      // /context 的台账行(写出)
    std::size_t* persisted_count = nullptr;        // 落盘基线(压缩后收缩)
    lubancode::sessions::SessionStore* session_store = nullptr;
    bool session_store_broken = false;
    // 压缩滞回活账(可空 = 单测/无状态场景,不设防)。
    CompactHysteresis* hysteresis = nullptr;
    // 压缩参数现场收集(compact 的窗口预算/守恒校验材料)。BuildCompactOptions
    // 的现算(路由声明/目录条目/活动待办守恒)全在里面。
    std::function<lubancode::agent::CompactOptions()> build_compact_options;
    // compact_v2 事件落盘前补 goal/loop snapshot(有才带;manifest 守恒面)。
    std::function<void(lubancode::sessions::CompactV2Event&)> attach_goal_snapshot;
    std::function<void(lubancode::sessions::CompactV2Event&)> attach_loop_snapshot;
    // PostCompact/SessionStart 审计钩子的会话级发射口。
    std::function<void(lubancode::hooks::HookEvent, nlohmann::json, const std::string&)> emit_session_hook;
    // 压缩路由:cheap 角色的 backend + route(会话侧的 ModelRouterService)。
    std::function<lubancode::app::ModelRouterService::Routed()> route_compact;
    // 修理路由(cheap 失败回退 normal 的那只)。
    std::function<lubancode::app::ModelRouterService::Routed()> route_repair;
    // normal 回退的落点(cheap 跨 provider 拿不到时的主 backend)。
    lubancode::api::Backend* normal_backend = nullptr;
    // 会话当前模型(判"配了独立 cheap 路由"用)。
    const std::string* current_model = nullptr;
    // 分角色记账(压缩用了谁)。
    std::function<void(lubancode::agent::ModelRole, const lubancode::agent::ModelRoute&,
                       const lubancode::agent::BackgroundCallAccounting&)>
        record_usage;
    // 回退留痕(cheap -> normal 的台账)。
    std::function<void(lubancode::agent::TaskKind kind, lubancode::agent::ModelRole from,
                       lubancode::agent::ModelRole to, const std::string& reason)>
        record_fallback;
    // mid-turn 压缩前补全量落盘(JSONL 一字不丢)。
    std::function<void()> persist_new_messages;
};
void RunCompactCommand(const std::string& args, const CompactSessionInputs& in);

// 自动(外层用户消息前)与 mid-turn(工具循环边界)压缩共用的一条路
//(终端接线收尾单自大类搬出):压缩 → 校验 → 换历史 → 落盘事件 → 报数。
// 返回 true = 压缩成功。正戏(HandleCompactCommand)之外还有 PreCompact
// auto 闸、cheap 失败回退 normal 修一次、分层压缩与 v2 事件。
bool TryRunCompact(bool midturn, const CompactSessionInputs& in);

// AgentLoop 每次模型请求前的压力通报接线:PreRequest 安全点撞线就
// mid-turn 收一次历史;HardTrim 真丢了东西就显式告警,不静默降级。
void HandleContextPressure(const lubancode::agent::ContextPressure& pressure, const CompactSessionInputs& in);


// /compact 命令的结果:event 是 compact_v2 压缩事件(archive + kept_from +
// manifest + metrics),成功时调用方追加写进存档流水,/resume 才能回放出
// 压缩后的活状态;失败/没得压给 nullopt。before/after tokens 用统一估算
// 口径;manifest_* 是摘要 manifest 里守住的目标数,供成功提示带一句
// "保留了几条约束/待办"。
struct CompactCommandResult {
    std::optional<lubancode::sessions::CompactV2Event> event;
    std::size_t before_tokens = 0;
    std::size_t after_tokens = 0;
    std::size_t manifest_constraints = 0;
    std::size_t manifest_open_items = 0;
};

// /compact 命令:分层压缩(装得下单次摘要;装不下按 episode 分块 map、
// 归并 reduce),顶替掉中间那段老对话,热区按 token 预算保留。backend 由
// 调用方按 ModelRouterService 的 Compact 路由取(可能是会话裸 backend,也
// 可能是跨 provider 的另一只 client)——CompactHierarchical() 自己把
// route.model 写进 request.model、route.effort 带上。args 是 /compact 的
// 重点文本(或 --dry-run);compact_epoch 进出两头用:进 = 本场已压过几次,
// 出 = 本次压完的序号(写进 v2 事件);options 携带窗口预算与必须守恒的
// 待办;accounting 非空时收回本次压缩的 usage/时长(分角色记账)。
// 压缩模型窗口装不下(分块也救不了)时明确拒绝、不静默截史;manifest
// 校验不过同样旧 history 不动。
CompactCommandResult HandleCompactCommand(const std::string& args, lubancode::agent::Agent& loop,
                                          lubancode::api::Backend& raw_backend,
                                          const lubancode::agent::ModelRoute& compact_route,
                                          const lubancode::cli::Theme& theme, bool spinner_enabled,
                                          const lubancode::agent::CompactOptions& options, int& compact_epoch,
                                          lubancode::agent::BackgroundCallAccounting* accounting = nullptr);

void PrintSessionsCommand(const std::string& sessions_dir, const std::string& args);

// ---------------------------------------------------------------------------
// 归档与永久删除(会话管理器单第四、五步)。搬与删全经
// sessions::SessionLifecycle——这里是接活的人:解析引用、列重名、走确认屏、
// 报结果,不直接碰 filesystem。
// ---------------------------------------------------------------------------

// 引用解析 + 消歧的共用壳:candidates 归 lifecycle 出,标题由 SessionCatalog
// 补;重名列短 id 叫用户点明,绝不猜一场。命中唯一时填 out_id/out_title。
// 返回 false = 没解出(人话已打好,调用方直接印)。include_archived 版
// 连归档场一起列候选(unarchive 的目标恰是归档场)。
bool ResolveSessionReference(const std::string& sessions_dir, const std::string& ref,
                             const std::function<std::string()>& stdin_line, std::string& out_id,
                             std::string& out_title, std::string& out_message, bool& ambiguous);
bool ResolveSessionReference(const std::string& sessions_dir, const std::string& ref,
                             const std::function<std::string()>& stdin_line, bool include_archived,
                             std::string& out_id, std::string& out_title, std::string& out_message,
                             bool& ambiguous);

// 顶层 `lubancode archive <SESSION>` / `unarchive <SESSION>` /
// `delete <SESSION> [--force]` 的执行体。stdin_line:读一行输入的回调
// (确认屏用;null = 非交互,delete 一律按缺确认拒绝)。force 只 delete 认。
// 返回进程退出码(0 = 成功)。
int HandleSessionManagementCommand(const std::string& sessions_dir, int kind, const std::string& ref,
                                   bool force, const lubancode::cli::Theme& theme,
                                   const std::function<std::string()>& stdin_line);

// 会话内 /archive:只归档当前会话。先经 lifecycle 刷盘收柄再搬,成功后
// 调用方退场(返回 true = 该退出交互会话)。
bool ArchiveCurrentSession(const std::string& sessions_dir, lubancode::sessions::SessionStore& store,
                           const lubancode::cli::Theme& theme);

// 会话内 /delete:只删当前会话。回合在跑/工具在飞/审批悬着时拒绝
// (调用方先验);闲下来后走确认屏,再关柄、删文件。返回 true = 该退出。
bool DeleteCurrentSession(const std::string& sessions_dir, lubancode::sessions::SessionStore& store,
                          const lubancode::sessions::SessionMeta& meta, const std::string& title,
                          const lubancode::cli::Theme& theme);


// /resume 裸敲:本目录最近 20 场直接做成方向键菜单。显式编号/id 仍由
// ResumeSession 解析，脚本和熟手用法不变。
std::optional<std::string> PromptResumeTarget(const std::string& sessions_dir,
                                              const lubancode::cli::Theme& theme);


// /resume <编号或id> 和 --continue 共用的执行逻辑。target 是编号(按
// ListSessions 的倒序编号)、会话 id、或空串(--continue:最近一场)。
// 编号和"最近一场"都只在**本目录**(meta.cwd == 当前 cwd)的场子里数;
// 直接给 id 的仍然全局能找(拼路径兜底),跨目录恢复留了这条明路。
// 成功:回放事件 + 成对修补 + ReplaceHistory + 接管文件继续追加,返回 true;
// session_title 同步成存档里最后一条 title 事件(没有就清空)。
// quiet_if_none:--continue 本目录找不到任何存档时不报错、安静开新会话。
// worktree_session(0.27.x,可空):会话档 meta.cwd(含 cwd 事件回放)若指向
// 一间还在的 worktree 房,验明正身后把会话搬回去;房没了回落启动目录并
// 说一声;马甲房(.git 指回主仓那类)拒进并报原因。非空时成功恢复后由
// 调用方做一次目录善后同步(重拼系统提示那条路)。
// on_queue_restored(可空,取走即消费单路径二):恢复成功后收一份存档里
// 最后一条 queue 事件快照(没排过队就是空表),会话层拿它重建
// SessionSteeringQueue。不接就照旧丢弃(单测/旧调用点)。
// on_mode_restored(可空,Plan 模式单):恢复成功后收存档的 mode/plan/review
// 账(老档没 mode 行给 Default 与空表),会话层拿它恢复协作模式档位与
// 最近计划成品。不接照旧丢弃。
// on_think_history_restored(可空,Kimi 保留式思考单 P1):恢复成功后收存档
// 最后一条 think_history 事件(老档没这行给 nullopt),会话层拿它恢复
// /think history 的选择并按当前模型重新校验。不接照旧丢弃(按 default)。
bool ResumeSession(const std::string& target, const std::string& sessions_dir,
                    lubancode::agent::Agent& loop, lubancode::sessions::SessionStore& store,
                    std::size_t& persisted_count, lubancode::sessions::SessionMeta& session_meta,
                    std::string& session_title, const std::string& wire_str, const std::string& current_model,
                    const lubancode::cli::Theme& theme, bool quiet_if_none,
                    lubancode::cli::WorktreeSession* worktree_session = nullptr,
                    int* compact_epoch_out = nullptr,
                    const std::function<void(const std::vector<lubancode::sessions::ArchivedQueueItem>&)>*
                        on_queue_restored = nullptr,
                    const std::function<void(const std::optional<lubancode::sessions::ModeEvent>&,
                                             const std::vector<lubancode::sessions::PlanEvent>&,
                                             const std::optional<lubancode::sessions::PlanReviewEvent>&)>*
                        on_mode_restored = nullptr,
                    const std::function<void(const std::optional<lubancode::sessions::ThinkHistoryEvent>&)>*
                        on_think_history_restored = nullptr);


// /export [路径]:当前会话导出 Markdown,默认写 sessions/<id>.md。
// 有存档文件就从文件读**全量流水**导出(压缩不丢内容,发生点插一行标注);
// 没有存档文件(没落过盘)退回导内存里这份历史。/title 设过的标题当大标题。
// artifact_store(可空,第二期):非空且有落盘时追加"可追回 artifact"附录
// ——id/工具/字节/sha 指纹/真本路径,导出内容里被折叠的结果按 id 对得上
// 真本(规格"/export 必须说明哪些内容来自 artifact")。
void HandleExportCommand(const std::string& args, const lubancode::agent::Agent& loop,
                          const lubancode::sessions::SessionStore& store, const std::string& sessions_dir,
                          const lubancode::sessions::SessionMeta& session_meta, const std::string& session_title,
                          const lubancode::agent::ContextArtifactStore* artifact_store = nullptr);


// ---------------------------------------------------------------------------
// 会话命令的窄状态:handler 只借引用干活,不拥有会话资源。
// ---------------------------------------------------------------------------

// /clear /title /resume 共用的会话侧状态。全部借用:会话
// (InteractiveSession)在命令执行期间保证存活。
struct SessionCommandState {
    std::function<void(bool)> rebuild_loop;  // /clear 传 false = 丢历史重建
    lubancode::agent::Agent& loop;        // /compact /resume /export 用
    lubancode::sessions::SessionStore& store;
    std::size_t& persisted_count;             // 落盘基线
    int& compact_epoch;                       // 压缩序号(/resume 接旧账,/compact 进出两头用)
    lubancode::sessions::SessionMeta& meta;
    std::string& title;
    bool& title_pending;
    bool& store_broken;
    std::string& start_ts;                   // /clear 翻新会话 id 时间戳
    std::function<void()> on_session_restarted;  // /clear 善后(project memory 源)
    std::function<void(const std::string&)> on_title_changed;  // peer 名册改名,可空
    std::function<void()> sync_worktree_directory;  // resume 搬房后的善后
    // /clear 的子代理清场(0.28.x):停掉全部后台任务、把未送达的介入消息
    // 报给人看——面板规格"清场不能无声遗失"的一条。可空(单测不接)。
    std::function<void()> on_agents_cleanup;
    lubancode::cli::WorktreeSession* worktree_session = nullptr;  // 可空
    const std::string& sessions_dir;
    const std::string& wire_str;
    const std::shared_ptr<std::string>& current_model;
    // hooks 框架:/resume 成功后回调(SessionStart source=resume 的发射口,
    // 会话层接)。可空(单测不接)。
    std::function<void()> on_resumed;
    // /resume 成功后的排队账重建(取走即消费单路径二):收存档最后一条
    // queue 快照,会话层灌回 SessionSteeringQueue。可空(单测不接)。
    std::function<void(const std::vector<lubancode::sessions::ArchivedQueueItem>&)> on_queue_restored;
    // Plan 模式单:/resume 成功后的 mode/plan/review 账恢复。可空(单测
    // 不接;不接就丢弃,老档行为)。
    std::function<void(const std::optional<lubancode::sessions::ModeEvent>&,
                       const std::vector<lubancode::sessions::PlanEvent>&,
                       const std::optional<lubancode::sessions::PlanReviewEvent>&)>
        on_mode_restored;
    // Kimi 保留式思考单 P1:/resume 成功后的跨轮保留选择恢复。可空(单测
    // 不接;不接就按 ProviderDefault)。
    std::function<void(const std::optional<lubancode::sessions::ThinkHistoryEvent>&)> on_think_history_restored;
};

// /clear:丢历史重建、存档翻篇、标题翻篇。
CommandFlow HandleClearCommand(SessionCommandState& state, const lubancode::config::Config& config,
                               const lubancode::cli::Theme& theme, bool spinner_enabled);

// /title [标题]:看/设标题;建档前设的先挂起,建档成功由落盘路径补写事件行。
CommandFlow HandleTitleCommand(SessionCommandState& state, const std::string& args,
                               const lubancode::cli::Theme& theme);

// /resume [编号或id]:裸敲弹最近 20 场菜单;成功后接管存档继续追加。
CommandFlow HandleResumeCommand(SessionCommandState& state, const std::string& args,
                                const lubancode::cli::Theme& theme);

// ---------------------------------------------------------------------------
// 命令分派注册制(会话终章):会话域的分派位。旧 interactive_session 大
// switch 的会话类 case 原样搬来,材料经 SlashDispatchContext(定义在
// command_registry.hpp)递入;行为一字未改。
// ---------------------------------------------------------------------------
struct SlashDispatchContext;

CommandFlow HandleSlashHelp(SlashDispatchContext& ctx, const lubancode::cli::ParsedSlashCommand& parsed);
CommandFlow HandleSlashClear(SlashDispatchContext& ctx, const lubancode::cli::ParsedSlashCommand& parsed);
CommandFlow HandleSlashContext(SlashDispatchContext& ctx, const lubancode::cli::ParsedSlashCommand& parsed);
CommandFlow HandleSlashCompact(SlashDispatchContext& ctx, const lubancode::cli::ParsedSlashCommand& parsed);
CommandFlow HandleSlashRecord(SlashDispatchContext& ctx, const lubancode::cli::ParsedSlashCommand& parsed);
CommandFlow HandleSlashSessions(SlashDispatchContext& ctx, const lubancode::cli::ParsedSlashCommand& parsed);
CommandFlow HandleSlashArchive(SlashDispatchContext& ctx, const lubancode::cli::ParsedSlashCommand& parsed);
CommandFlow HandleSlashDelete(SlashDispatchContext& ctx, const lubancode::cli::ParsedSlashCommand& parsed);
CommandFlow HandleSlashResume(SlashDispatchContext& ctx, const lubancode::cli::ParsedSlashCommand& parsed);
CommandFlow HandleSlashExport(SlashDispatchContext& ctx, const lubancode::cli::ParsedSlashCommand& parsed);
CommandFlow HandleSlashTitle(SlashDispatchContext& ctx, const lubancode::cli::ParsedSlashCommand& parsed);
CommandFlow HandleSlashExit(SlashDispatchContext& ctx, const lubancode::cli::ParsedSlashCommand& parsed);

}  // namespace lubancode::app
