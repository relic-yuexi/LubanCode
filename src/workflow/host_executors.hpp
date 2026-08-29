// 宿主侧执行器(自然语言编排单第 4 批):tool/llm/approval/ask_user/skill
// 接现成设施。
//
// 分层:workflow 层(runtime)只认 NodeExecutor 抽象;这里把 LubanCode 的
// ToolRegistry、Backend、InteractionBroker 适配进去。执行器不反向 include
// cli/app(依赖只准 workflow runtime -> agent/tool/skill/interaction
// abstractions,单子"现有代码可接之处")。
//
// tool:经 agent::RunOneTool 正门调 ToolRegistry 已注册工具——PreToolUse/
// PostToolUse 钩子、确认档、Plan 闸、逐枚 trace 与主回合同一条链(批一
// 封暗道);旧 ConfirmGate 保留作确认缺省。llm:单次结构化模型调用,采样
// 走 agent::SampleModel 原语(批一·病四),不开完整 agent loop。agent:
// 与 main/subagent 共用 agent::Agent,turn 推进走 TurnHarness 的
// DriveTurn(批五乙·三外壳降策略:怎么跑 turn 只 harness 一份)。
// approval:经 InteractionBroker 悬起,等用户决定(accept/decline/cancel)。
// ask_user:经 InteractionBroker 问一句,答案写进 output。
// skill:把 Skill 的 SKILL.md 正文装进 llm 执行的上下文(同一只 llm 执行
// 器带 skill 前缀),不把 Skill 文本当代码跑。
//
// 事件流(骨架拆解批二):agent 节点装 event_sink 后,整段嵌套回合经
// TurnEventAdapter 翻成 ServerEvent 落会话 sink(与主回合同一只出水口)。
// tool 节点不另配 adapter——它的工具事件走 trace 分线(hub 的 UI 投影),
// 同一枚执行不记两本 item 账。

#pragma once

#include <expected>
#include <functional>
#include <memory>
#include <optional>
#include <string>

#include <nlohmann/json.hpp>

#include "agent/agent.hpp"  // Agent/AgentProfile:agent 节点与 main/sub 共用引擎
#include "agent/loop.hpp"
#include "agent/prompt_assembler.hpp"  // PackageProfileRoot:子代理系统提示的包层根
#include "api/backend.hpp"
#include "runtime/event_sink.hpp"
#include "runtime/id_authority.hpp"
#include "runtime/interaction_broker.hpp"
#include "tools/agent_tool.hpp"  // CustomAgentMaterial/BuildSubagentPromptOptions:自定义 Agent 的同源件
#include "tools/registry.hpp"
#include "workflow/runtime.hpp"

namespace lubancode::workflow {

// prompt 装配器:定义里的 prompt/task 是包内相对路径,宿主读文件拼系统段。
// 返回完整 prompt 文本;空串 = 读不到(报错)。
using PromptLoader = std::function<std::string(const std::string& package_relative_path)>;

// 终端从 Agent 面板递来的补充。执行器在一轮正常收口后取一次；若下一轮
// 没真正送成，restore 把原消息退回宿主队列。
struct NodeSteeringBatch {
    std::string input;
    std::function<void()> restore;
};
using NodeSteeringSource =
    std::function<std::optional<NodeSteeringBatch>(const NodeExecRequest& request)>;

// ---------------------------------------------------------------------------
// tool 节点
// ---------------------------------------------------------------------------

// 权限门:宿主把 TurnRuntime 的确认裁定包成这只函数(needs_confirm 时调,
// 返回是否放行)。空 = 没人管确认,needs_confirm 的工具直接拒(not_configured
// 不挂死,单子第 2 批的规矩)。
using ToolConfirmGate = std::function<bool(const std::string& tool_name, const nlohmann::json& input)>;

// tool 节点(骨架拆解单批一·病二):执行走 agent::RunOneTool 这条全仓正门
// ——PreToolUse 钩子(含 updatedInput 的 schema 复检)、确认档、Plan 闸、
// PostToolUse、逐枚 trace、编码清洗一个不少,与 JSON 后端/PTC 同一条代码
// 路,不再有第二条绕过 hooks 的暗道。旧路自带的 confirm gate 保留成
// Callbacks::on_tool_confirm 的缺省兜底;钩子/trace 由宿主经 callbacks 带入。
class ToolExecutor : public NodeExecutor {
public:
    struct Options {
        tools::ToolRegistry* registry = nullptr;  // RunOneTool 要可写引用
        ToolConfirmGate confirm;                  // 旧确认门(缺省兜底)
        // 宿主的钩子/权限/trace 链(与主回合同一份装配,全是控制口):
        // on_pre_tool_use_hook/on_post_tool_use_hook/on_mode_policy/
        // on_tool_phase/on_tool_trace/on_tool_trace_blocked/on_tool_confirm
        // (_async)。空装 = 没配,行为与旧路一致(needs_confirm 仍走旧 gate
        // 或明拒)。显示出水(events)不在这条链上——工具节点不上事件流
        // (trace 分线已够,同一枚执行不记两本 item 账)。
        agent::TurnWiring callbacks;
        // trace 发号(ToolTraceHub::NextExecutionId 那口)。空或 on_tool_trace
        // 缺席 = 不追踪,栅栏事件全空操作(旧行为)。
        std::function<std::string()> execution_id_issuer;
        std::string thread_id;  // trace 上下文的会话名(可空)
        std::string turn_id;    // trace 上下文的轮名(可空;缺省用 run_id)
    };

    explicit ToolExecutor(Options options);
    // 兼容门:只给 registry(+旧确认门),钩子链空装(单测/旧装配)。
    explicit ToolExecutor(tools::ToolRegistry* registry, ToolConfirmGate confirm = nullptr);

    NodeExecResult Execute(const NodeExecRequest& request) override;

private:
    Options options_;
};

// ---------------------------------------------------------------------------
// agent 节点：与 main/subagent 共用 agent::Agent
// ---------------------------------------------------------------------------

// 自定义 Agent 节点(`agent: <name>`,阶段 5)的解析产物:宿主查
// AgentCatalog(canonical/裸名;Package 挂载层已把包内短引用折成
// canonical)、走统一 AgentProfileResolver 之后递进来。workflow 层不摸
// Catalog——扫描与查名是宿主的活,这里只消费统一解析的结果;同一份定义
// 从 agent 工具与 Workflow 节点两路解析的账,钉在 AgentProfileResolver 的
// 对账册(test_agent_profile_resolver.cpp)。
struct CustomAgentNodeResolution {
    tools::CustomAgentMaterial material;   // 定义 + 预装技能正文 + builtin 记号
    agent::ResolvedAgentProfile resolved;  // 统一解析结果(profile/effective_tools/三笔决议)
    std::string resolved_name;             // 回执身份(Catalog 键名:canonical 或裸名)
    // 权限下限(比会话档严时的档位):Resolver 校验"不许放宽"在前,这枚
    // 是"收窄生效"——宿主按环境账的父档算好递进来,executor 接进
    // on_tool_confirm_floored。nullopt = 不比父严(或没递环境账)。
    std::optional<agent::AgentPermissionMode> permission_floor;
};

// 名字 -> 解析产物的口。error 出参带人话(查无此名/不可用的原因);
// 返回 nullopt 即失败。宿主装配,executor 首知即报。
using CustomAgentResolver =
    std::function<std::optional<CustomAgentNodeResolution>(const WorkflowNode& node, std::string& error)>;

// 子代理系统提示的会话材料(阶段 5):BuildSubagentPromptOptions 的入参
// 里,除节点/定义/解析结果外全是宿主会话材料——agent 工具路与 Workflow
// 节点路各持一份,宿主从同一来源(主回合 prompt_options / AgentTool 的
// setter 同源的值)灌同一份。"两路系统提示逐字节一致"的验收以此为前提。
struct SubagentPromptMaterial {
    std::string cwd;
    std::string prompts_dir;             // 用户模块目录;空 = 只用嵌入版
    std::string project_prompts_dir;     // 项目模块目录;空 = 没有项目层
    std::string project_instructions;    // AGENTS.md 分层内容;空 = 不注入
    std::string skills_segment;          // 技能清单段;空 = 不注 skills 模块
    std::vector<agent::PackageProfileRoot> package_profile_roots;  // 包层 Profile 根
};

class AgentExecutor : public NodeExecutor {
public:
    struct Binding {
        api::Backend* backend = nullptr;
        agent::AgentProfile profile;
    };
    using BindingResolver = std::function<std::optional<Binding>(const WorkflowNode& node)>;

    struct Options {
        Binding default_binding;
        BindingResolver resolve_binding;  // model_role/provider 路由；空则用 default
        tools::ToolRegistry* registry = nullptr;
        PromptLoader task_loader;
        agent::TurnWiring callbacks;  // 控制口(确认/钩子);显示走下面的 sink
        // 批二余款:装了 sink 的节点,嵌套回合经 TurnEventAdapter 上事件流
        //(ids/thread_id 用宿主会话的,与主回合同源,seq 才单调;ids 缺省落
        // ProcessIdAuthority,单测)。本执行器另从流里观察正文/usage 记账
        //(result 的 text/tokens),不另开旁路。不装 = 不上事件流(旧行为)。
        runtime::EventSink* event_sink = nullptr;
        runtime::IdAuthority* ids = nullptr;
        std::string thread_id;
        NodeSteeringSource steering;
        // ---- 自定义 Agent(阶段 5)----
        // `agent: <name>` 节点的解析口。空(旧装配/单测)= 没接:点名
        // agent 的节点报 not_configured,不静默退回 default binding。
        CustomAgentResolver custom_agent_resolver;
        SubagentPromptMaterial subagent_prompt_material;  // 系统提示的会话材料
    };

    explicit AgentExecutor(Options options);
    NodeExecResult Execute(const NodeExecRequest& request) override;

private:
    Options options_;
};

// ---------------------------------------------------------------------------
// llm 节点
// ---------------------------------------------------------------------------

class LlmExecutor : public NodeExecutor {
public:
    struct Binding {
        api::Backend* backend = nullptr;
        std::shared_ptr<api::Backend> owned_backend;
        std::string model;
        std::string reasoning_effort;
    };
    using BindingResolver = std::function<std::optional<Binding>(const WorkflowNode& node)>;

    struct Options {
        api::Backend* backend = nullptr;
        std::string model;
        std::string reasoning_effort;
        BindingResolver resolve_binding;  // model_role 路由；空则用上面缺省档
        PromptLoader prompt_loader;  // 包内路径 -> 正文
        std::int64_t max_output_tokens = 4096;
        runtime::EventSink* event_sink = nullptr;
        runtime::IdAuthority* ids = nullptr;
        std::string thread_id;
        NodeSteeringSource steering;
    };

    explicit LlmExecutor(Options options);
    NodeExecResult Execute(const NodeExecRequest& request) override;

    // 带 prompt 正文的直调(skill 节点把 SKILL.md 正文经这里注入,不走
    // 包内路径解析)。loader 之外的第二条装配口,单一职责不混。
    NodeExecResult ExecuteWithPrompt(const NodeExecRequest& request, const std::string& prompt_text);

private:
    Options options_;
};

// ---------------------------------------------------------------------------
// approval / ask_user 节点
// ---------------------------------------------------------------------------

class ApprovalExecutor : public NodeExecutor {
public:
    explicit ApprovalExecutor(runtime::InteractionBroker* broker);
    NodeExecResult Execute(const NodeExecRequest& request) override;

private:
    runtime::InteractionBroker* broker_;
};

class AskUserExecutor : public NodeExecutor {
public:
    explicit AskUserExecutor(runtime::InteractionBroker* broker);
    NodeExecResult Execute(const NodeExecRequest& request) override;

private:
    runtime::InteractionBroker* broker_;
};

// ---------------------------------------------------------------------------
// skill 节点:把 SKILL.md 装进 llm 上下文
// ---------------------------------------------------------------------------

// skill 目录表:skill 名 -> SKILL.md 正文。宿主从 SkillCatalog 投影。
class SkillExecutor : public NodeExecutor {
public:
    explicit SkillExecutor(std::shared_ptr<LlmExecutor> llm,
                           std::map<std::string, std::string> skill_bodies);
    NodeExecResult Execute(const NodeExecRequest& request) override;

private:
    std::shared_ptr<LlmExecutor> llm_;
    std::map<std::string, std::string> skill_bodies_;
};

// ---------------------------------------------------------------------------
// subflow 节点:调用另一份 Workflow
// ---------------------------------------------------------------------------

// 宿主装配:目标定义的解析 + runtime 引用。深度硬限 1(单子"Workflow 与
// Agent":现有子代理深度硬限 1,首版照守,不为嵌套 subflow 偷开)。
class SubflowExecutor : public NodeExecutor {
public:
    using DefinitionResolver = std::function<std::optional<WorkflowDefinition>(const std::string& id)>;
    using RuntimeRunner =
        std::function<WorkflowRunSummary(const WorkflowDefinition& def, const nlohmann::json& inputs)>;

    SubflowExecutor(DefinitionResolver resolver, RuntimeRunner runner);
    NodeExecResult Execute(const NodeExecRequest& request) override;

private:
    DefinitionResolver resolver_;
    RuntimeRunner runner_;
};

}  // namespace lubancode::workflow
