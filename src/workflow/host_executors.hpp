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
// 走 agent::SampleModel 原语(批一·病四),不开完整 agent loop。
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
#include "api/backend.hpp"
#include "runtime/event_sink.hpp"
#include "runtime/id_authority.hpp"
#include "runtime/interaction_broker.hpp"
#include "tools/registry.hpp"
#include "workflow/runtime.hpp"

namespace lubancode::workflow {

// prompt 装配器:定义里的 prompt/task 是包内相对路径,宿主读文件拼系统段。
// 返回完整 prompt 文本;空串 = 读不到(报错)。
using PromptLoader = std::function<std::string(const std::string& package_relative_path)>;

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
        // 宿主的钩子/权限/trace 链(与主回合同一份装配):on_pre_tool_use_hook/
        // on_post_tool_use_hook/on_mode_policy/on_tool_phase/on_tool_trace/
        // on_tool_trace_blocked/on_tool_confirm(_async)。空装 = 没配,行为
        // 与旧路一致(needs_confirm 仍走旧 gate 或明拒)。
        agent::Callbacks callbacks;
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
        agent::Callbacks callbacks;
        // 批二(事件流升正房):装了 sink 的节点,显示回调经 TurnEventAdapter
        // 上事件流。ids/thread_id 用宿主会话的(与主回合同源,seq 才单调);
        // ids 缺省落 ProcessIdAuthority(单测)。不装 = 不上事件流(旧行为)。
        runtime::EventSink* event_sink = nullptr;
        runtime::IdAuthority* ids = nullptr;
        std::string thread_id;
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
    struct Options {
        api::Backend* backend = nullptr;
        std::string model;
        PromptLoader prompt_loader;  // 包内路径 -> 正文
        std::int64_t max_output_tokens = 4096;
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
