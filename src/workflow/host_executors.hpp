// 宿主侧执行器(自然语言编排单第 4 批):tool/llm/approval/ask_user/skill
// 接现成设施。
//
// 分层:workflow 层(runtime)只认 NodeExecutor 抽象;这里把 LubanCode 的
// ToolRegistry、Backend、InteractionBroker 适配进去。执行器不反向 include
// cli/app(依赖只准 workflow runtime -> agent/tool/skill/interaction
// abstractions,单子"现有代码可接之处")。
//
// tool:调 ToolRegistry 已注册工具,输入输出验 schema 交给工具自身;
// needs_confirm 的工具走 TurnRuntime 同一套确认(经 ConfirmGate 注入)。
// llm:单次结构化模型调用(api::Backend),不开完整 agent loop。
// approval:经 InteractionBroker 悬起,等用户决定(accept/decline/cancel)。
// ask_user:经 InteractionBroker 问一句,答案写进 output。
// skill:把 Skill 的 SKILL.md 正文装进 llm 执行的上下文(同一只 llm 执行
// 器带 skill 前缀),不把 Skill 文本当代码跑。

#pragma once

#include <expected>
#include <functional>
#include <memory>
#include <optional>
#include <string>

#include <nlohmann/json.hpp>

#include "agent/loop.hpp"
#include "api/backend.hpp"
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

class ToolExecutor : public NodeExecutor {
public:
    explicit ToolExecutor(const tools::ToolRegistry* registry, ToolConfirmGate confirm = nullptr);

    NodeExecResult Execute(const NodeExecRequest& request) override;

private:
    const tools::ToolRegistry* registry_;
    ToolConfirmGate confirm_;
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
