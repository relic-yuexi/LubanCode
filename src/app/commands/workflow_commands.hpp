// /workflow 命令的终端薄壳(自然语言编排单第 1 批起)。
//
// 分层规矩:业务在 src/workflow/(纯逻辑,单测钉);这里只做"拆子命令、
// 打印"。第 1 批覆盖 list/show/graph/validate/doctor;run/resume/cancel
// 随第 2/3 批接线时扩充,非法子命令打用法。
//
// 与另一工人的接缝:slash 分派在 InteractiveSession::DispatchSlashCommand,
// 这里只经 WorkflowCommandContext 拿材料(catalog 现扫,不占会话状态)。

#pragma once

#include "app/commands/command_flow.hpp"  // CommandFlow(分派注册制)
#include "cli/slash_commands.hpp"          // ParsedSlashCommand(分派注册制)
#include "cli/line_editor.hpp"             // CompletionCandidate(alias 补全)

#include <atomic>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "agent/agent.hpp"  // AgentProfile(批四自立门户)
#include "api/backend.hpp"
#include "cli/theme.hpp"
#include "config/model_catalog.hpp"
#include "package/mounting.hpp"  // PackageSnapshot(阶段 6:跑一趟钉一份)
#include "runtime/event_sink.hpp"
#include "runtime/id_authority.hpp"
#include "runtime/interaction_broker.hpp"
#include "runtime/trajectory_session.hpp"  // TrajectorySessionLedger(编排账接线)
#include "tools/registry.hpp"
#include "tools/skill_loader.hpp"
#include "workflow/catalog.hpp"
#include "workflow/host_executors.hpp"  // ToolExecutor::Options(执行器装配)
#include "workflow/runtime.hpp"
#include "workflow/validator.hpp"

namespace lubancode::app {

// /workflow 命令的装配材料。project_root/user_root 是两级 workflows 目录
// 的锚点(catalog 自己拼 .lubancode/workflows);registry 可空(空则
// capability check 只数 core 工具,给 unknown_tool 警告)。packaged_workflows
// (统一 Package 封装单阶段 3):包层成品件,会话钉快照折来——canonical
// id 登册,不参与裸 alias;空 = 没有包,行为与从前一致。
struct WorkflowCommandContext {
    std::optional<std::filesystem::path> project_root;  // 通常 cwd
    std::optional<std::filesystem::path> user_root;     // 通常 home
    std::optional<std::filesystem::path> home_lubancode;  // workflow-runs 落点
    std::vector<lubancode::workflow::PackagedWorkflowSource> packaged_workflows;
    const lubancode::tools::ToolRegistry* registry = nullptr;  // capability 快照
    std::vector<std::string> skill_names;               // 撞名检查用
    // AgentCatalog 可用条目名(阶段 5):`agent: <name>` 节点的编译期引用
    // 校验查这张表(canonical 与裸名两套键)。空 = 没扫(旧装配),不校验。
    std::vector<std::string> agent_names;
    const lubancode::cli::Theme* theme = nullptr;       // 必填(指针免默认构造被删)
    // 直呼 workflow 却没带必填输入时，终端可据字段 schema 当场问一句。
    // headless/app-server 不装这只口，仍由 runtime 返回 invalid_inputs。
    std::function<std::optional<std::string>(const std::string& field,
                                             const nlohmann::json& schema)> request_input;
    // 真正开跑前才调：终端拿它在必填输入收齐后起忙碌钟，不能把用户
    // 停在输入框里的工夫算进执行耗时。
    std::function<void()> on_run_start;
    // workflow 自身的 run/node 事件出口。终端、app-server 与 headless
    // 各自装 sink；空指针仍可安静运行。
    lubancode::runtime::EventSink* event_sink = nullptr;
    std::string thread_id;
    lubancode::runtime::IdAuthority* id_authority = nullptr;
    // 轨迹会话账(workflow 会话归属统一单):非空 = run 起动开编排 Journal、
    // 逐 attempt 开 node stream(fail closed);空 = headless/旧装配,不落
    // 轨迹账(旧 workflow-runs/ 路照旧)。
    lubancode::runtime::TrajectorySessionLedger* trajectory = nullptr;
};

// 拆好的 /workflow 子命令。Invalid 时 usage 打印兜底。
enum class WorkflowCommandAction {
    Invalid,
    List,   // list [project|home|all]
    Show,   // show <id>
    Graph,  // graph <id> [ascii|mermaid|json]
    Validate,  // validate <id>
    Doctor,  // doctor:撞名/坏定义/缺失能力巡检
    Run,    // run <id> [参数...]
    Resume, // resume <run_id>
    Cancel, // cancel <run_id>(首版:标记请求,跑动中的取消经 ESC 通道)
    History,  // history <id> | history delete <run_id>
    Enable,   // enable|disable <id>
    Remove,   // remove <id>(要确认)
    Create,   // create <描述...>(第 5 批:自然语言向导)
    Alias,    // alias:列出直呼名与撞名账
};

struct ParsedWorkflowCommand {
    WorkflowCommandAction action = WorkflowCommandAction::Invalid;
    std::string id;      // show/graph/validate/run 的目标
    std::string format;  // graph 的格式(空 = ascii)
    std::string scope;   // list 的范围(空 = all)
    std::string rest;    // run 的剩余参数/create 的描述原文
    bool confirm = false;  // remove/delete 的确认词(remove yes / delete yes)
};

// Workflow Agent 面板把结构化节点产物投成人话。运行时仍收原 JSON，
// 这里只改终端读法；summary 给导航行，markdown 给查看页。
struct WorkflowPanelOutput {
    std::string summary;
    std::string markdown;
    bool structured = false;
};

WorkflowPanelOutput FormatWorkflowPanelOutput(const std::string& raw);
std::string FormatWorkflowPanelInput(const nlohmann::json& input);
std::string FormatWorkflowRunResult(const nlohmann::json& result);

// 纯解析(单测钉)。
ParsedWorkflowCommand ParseWorkflowCommand(const std::string& args);

// 命令入口。返回 true = 交给会话层继续(恒 true;失败只打提示)。
bool HandleWorkflowCommand(const std::string& args, const WorkflowCommandContext& context);

// /workflow run 的执行装配:宿主(InteractiveSession)填执行器表;这里
// 只做编排。返回 run 摘要文本(给人看的一屏)。
std::string RunWorkflowById(const WorkflowCommandContext& context, const std::string& id,
                            const std::string& raw_args,
                            const std::map<lubancode::workflow::NodeKind,
                                           std::shared_ptr<lubancode::workflow::NodeExecutor>>& executors,
                            const std::atomic<bool>* cancel_token = nullptr);

// 会话层给 alias 直呼用的查询:catalog 里有没有这个 alias;返回 workflow
// id(撞名禁用/不存在给空串)。第 5 批把这里换成 autocomplete 同源。
std::string ResolveWorkflowAlias(const WorkflowCommandContext& context, const std::string& alias);

// 当前 catalog 里可直呼、未撞名、已启用的 alias，供两只 composer 补全。
std::vector<lubancode::cli::CompletionCandidate> BuildWorkflowSlashCompletionCandidates(
    const WorkflowCommandContext& context);

// ---- 执行器装配(终端接线收尾单自大类两段重复装配收口) ------------------
//
// /workflow run 与 alias 直呼原先在 DispatchSlashCommand 里各拼一份一模
// 一样的执行器表(第 4 批宿主执行器);收进这一只函数,两路共用。材料由
// 会话侧递进来;prompt 目录按 workflow id 现查 catalog。
struct WorkflowExecutorContext {
    lubancode::tools::ToolRegistry* registry = nullptr;
    lubancode::api::Backend* backend = nullptr;  // RebuildableBackend 那只
    std::function<lubancode::workflow::ToolExecutor::Options()> build_tool_options;
    // agent 节点的审批口(确认回调装配);空 = 没接审批宿主。tool 节点
    // 的确认门也借这一份(只在 build_tool_options 没填确认口时补)。
    std::function<lubancode::agent::TurnWiring()> build_agent_callbacks;
    std::string provider;                       // active_provider
    std::string model;                          // *current_model
    std::string effort;                         // *current_think
    const lubancode::config::ModelCatalog* model_catalog = nullptr;  // reasoning 档;可空
    lubancode::agent::AgentRuntimeProfile agent_profile;  // main_agent 的运行档案副本
    lubancode::runtime::EventSink* event_sink = nullptr;  // 会话 fanout;可空
    std::string thread_id;
    lubancode::runtime::IdAuthority* id_authority = nullptr;  // 空 = 进程级
    std::shared_ptr<lubancode::runtime::InteractionBroker> interaction_broker;
    const std::vector<lubancode::tools::SkillMeta>* skills = nullptr;
    lubancode::workflow::NodeSteeringSource steering;
    lubancode::workflow::LlmExecutor::BindingResolver resolve_llm_binding;
    int subflow_depth = 0;  // 终端首版只准一层 nesting,防交叉递归没帽
    // ---- 自定义 Agent(阶段 5)----
    // `agent: <name>` 节点的解析口来源:会话级 agent 工具(它的
    // custom_agent_resolver 与 resolve_environment 是 agent 工具路的同一
    // 只——两路解析喂同一份查名与环境账,才不各养一本)。空 = 没接
    //(headless/旧装配),点名 agent 的节点报 not_configured。
    lubancode::tools::AgentTool* agent_tool = nullptr;
    // Package 会话钉快照(统一封装单阶段 6):跑一趟钉一份——agent 节点
    // 的解析与预装技能正文全从这份折,半场 /package reload 不换这趟的账
    //(验收线:reload 不会让半场 Workflow 换 Skill)。空 = 没接包快照,
    // 解析口退回会话级 resolver(行为与从前一致)。
    std::shared_ptr<const lubancode::package::PackageSnapshot> package_snapshot;
    // 子代理系统提示的会话材料(与 agent 工具路同源):从主回合的
    // PromptOptions 折来,宿主保证两路值一致。
    lubancode::workflow::SubagentPromptMaterial subagent_prompt_material;
};

// BuildWorkflowExecutors(拼执行器表)已搬去 app/wirings/workflow_wiring
// (骨架拆解反弹·问题 3:装配根归位)——声明与实现在那边,这里只留
// 材料包。调用方(workflow_commands.cpp 与单测)include 那只头。

// ---------------------------------------------------------------------------
// 命令分派注册制(会话终章):workflow 域的分派位(/workflow 正门与
// /<alias> 直呼的 Unknown 兜底)。case 体原样自 interactive_session 的大
// switch 搬来,材料经 SlashDispatchContext 递入。
// ---------------------------------------------------------------------------
struct SlashDispatchContext;
CommandFlow HandleSlashWorkflow(SlashDispatchContext& ctx, const lubancode::cli::ParsedSlashCommand& parsed);
CommandFlow HandleSlashUnknown(SlashDispatchContext& ctx, const lubancode::cli::ParsedSlashCommand& parsed);

}  // namespace lubancode::app
