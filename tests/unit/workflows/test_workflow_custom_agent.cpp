// 自定义 Agent 与 Prompt Profile 单·阶段 5(Workflow 接入):`agent: <name>`
// 节点。五笔账:
//   - 解析链:名字经宿主解析口走统一 AgentProfileResolver;查无此名、
//     解析不过、宿主没接解析口各报各的稳定码,不静默退回 default binding。
//   - 编译期校验:CapabilityTable.agent_names 查引用,unknown_agent 落
//     nodes.<id>.agent;定义 JSON roundtrip 收 `agent` 字段(0.26.92 的欠账)。
//   - 两路同源(验收线):同一份 YAML 定义,agent 工具路与 Workflow 节点路
//     各跑一轮(假后端),系统提示与工具表逐字节一致。
//   - 四件语义:前台失败/预算尽/取消/async 后台回收——自定义路与 role
//     路同一条收口链,逐一照旧。
//   - 权限下限:Resolver 算出的 permission_floor 经 on_tool_confirm_floored
//     真收紧(比会话档严时);没接 floored 口或档不严时原样走,行为不变。

#include <doctest/doctest.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include <nlohmann/json.hpp>

#include "agent/agent.hpp"
#include "agent/agent_definition.hpp"
#include "agent/agent_profile_resolver.hpp"
#include "api/backend.hpp"
#include "runtime/event.hpp"
#include "runtime/event_sink.hpp"
#include "tools/agent_tool.hpp"
#include "tools/registry.hpp"
#include "tools/tool.hpp"
#include "workflow/host_executors.hpp"
#include "workflow/parser.hpp"
#include "workflow/runtime.hpp"
#include "workflow/validator.hpp"

using namespace lubancode;

namespace {

// 假工具:计数调用,可声明 needs_confirm。
class FakeTool : public tools::Tool {
public:
    FakeTool(std::string name, bool confirm = false)
        : name_(std::move(name)), confirm_(confirm) {}

    std::string name() const override { return name_; }
    std::string description() const override { return "fake tool for test"; }
    nlohmann::json input_schema() const override { return nlohmann::json::object(); }
    bool needs_confirm() const override { return confirm_; }
    tools::Tool::Result execute(const nlohmann::json&) override {
        ++calls;
        return {"干了", false};
    }

    int calls = 0;

private:
    std::string name_;
    bool confirm_;
};

// 脚本假后端:首轮吐 tool_use,次轮吐文本结论(与 test_workflow_agents 的
// AgentBackend 同款手艺)。
class ToolLoopBackend : public api::Backend {
public:
    std::string first_tool = "read_file";
    std::vector<api::Request> requests;

    std::expected<void, api::Error> send_stream(
        const api::Request& request, const std::function<void(const api::StreamEvent&)>& on_event,
        const std::atomic<bool>* cancel) override {
        (void)cancel;
        requests.push_back(request);
        api::Usage usage;
        usage.input_tokens = 10;
        usage.output_tokens = 5;
        on_event(api::MessageStart{"msg", request.model});
        if (requests.size() == 1) {
            on_event(api::ToolUseStart{0, "tool-1", first_tool});
            on_event(api::ToolUseInputDelta{0, "{}"});
            on_event(api::ContentBlockDone{0});
            on_event(api::MessageDone{"tool_use", usage});
        } else {
            on_event(api::TextDelta{R"({"answer":"ok"})"});
            on_event(api::ContentBlockDone{0});
            on_event(api::MessageDone{"end_turn", usage});
        }
        return {};
    }
};

// 恒 tool_use 的后端(预算闸的钉用:步数耗尽前永远在调工具)。
class ForeverToolBackend : public api::Backend {
public:
    std::vector<api::Request> requests;

    std::expected<void, api::Error> send_stream(
        const api::Request& request, const std::function<void(const api::StreamEvent&)>& on_event,
        const std::atomic<bool>* cancel) override {
        (void)cancel;
        requests.push_back(request);
        api::Usage usage;
        on_event(api::MessageStart{"msg", request.model});
        on_event(api::ToolUseStart{0, "tool-1", "read_file"});
        on_event(api::ToolUseInputDelta{0, "{}"});
        on_event(api::ContentBlockDone{0});
        on_event(api::MessageDone{"tool_use", usage});
        return {};
    }
};

// 报错后端(前台失败的分型)。
class FailingBackend : public api::Backend {
public:
    std::expected<void, api::Error> send_stream(
        const api::Request& request, const std::function<void(const api::StreamEvent&)>& on_event,
        const std::atomic<bool>* cancel) override {
        (void)request;
        (void)on_event;
        (void)cancel;
        return std::unexpected(api::Error{api::ErrorKind::Api, "接口崩了"});
    }
};

// 挂住等闸的后端(取消的分型:放闸或取消二选一)。
class GateBackend : public api::Backend {
public:
    std::mutex mutex;
    std::condition_variable cv;
    bool started = false;
    std::vector<api::Request> requests;

    std::expected<void, api::Error> send_stream(
        const api::Request& request, const std::function<void(const api::StreamEvent&)>& on_event,
        const std::atomic<bool>* cancel) override {
        {
            std::unique_lock<std::mutex> lock(mutex);
            requests.push_back(request);
            started = true;
            cv.notify_all();
            (void)cv.wait_for(lock, std::chrono::seconds(5), [&]() {
                return cancel != nullptr && cancel->load(std::memory_order_acquire);
            });
        }
        if (cancel != nullptr && cancel->load(std::memory_order_acquire)) {
            return std::unexpected(api::Error{api::ErrorKind::Cancelled, "cancelled"});
        }
        on_event(api::MessageStart{"msg", request.model});
        on_event(api::TextDelta{R"({"answer":"ok"})"});
        on_event(api::ContentBlockDone{0});
        on_event(api::MessageDone{"end_turn", api::Usage{}});
        return {};
    }
};

// 端到端对账用的同一份定义 YAML(两路各解析一遍)。allow 只点两枚:todo_write
// 两路有私有台账机制差异(agent 工具路给每任务换独占实例,schema 随之不同),
// 不进对账面——对账钉的是"定义驱动的系统提示与工具表同源"。
const char* kProbeYaml = R"YAML(schema: 1
name: probe-reviewer
description: 审查探针,只读摸排。
tools:
  allow:
    - read_file
    - search
runtime:
  max_steps_per_turn: 6
  max_output_tokens: 2048
prompt:
  project_instructions: omit
  soul: off
)YAML";

agent::AgentDefinition ParseDefinition(const char* yaml) {
    const agent::AgentDefinitionParseResult parsed = agent::ParseAgentDefinitionYaml(yaml, "(test)");
    REQUIRE(parsed.definition.has_value());
    return *parsed.definition;
}

// 两路共用的父皮与工具面。
agent::AgentProfile MakeParent() {
    agent::AgentProfile parent;
    parent.provider = "prov-x";
    parent.request.model = "model-x";
    parent.request.reasoning_effort = "medium";
    parent.runtime.max_steps_per_turn = 12;
    parent.runtime.max_output_tokens = 4096;
    parent.prompt_sections.web = false;
    parent.prompt_sections.lsp = true;
    return parent;
}

agent::AgentProfileResolveEnvironment MakeEnv(agent::AgentPermissionMode parent_mode =
                                                  agent::AgentPermissionMode::Auto) {
    agent::AgentProfileResolveEnvironment env;
    env.parent_permission = parent_mode;
    return env;
}

void RegisterProbeTools(tools::ToolRegistry& registry) {
    registry.Register(std::make_unique<FakeTool>("read_file"));
    registry.Register(std::make_unique<FakeTool>("search"));
    registry.Register(std::make_unique<FakeTool>("todo_write"));
    registry.Register(std::make_unique<FakeTool>("run_command", /*confirm=*/true));  // 确认口的钉子
}

std::vector<std::string> ParentToolNames(const tools::ToolRegistry& registry) {
    std::vector<std::string> names;
    for (const auto& tool : registry.All()) names.push_back(tool->name());
    return names;
}

// Workflow 路的解析口(与生产装配 workflow_commands.cpp 同构):查名 ->
// BuildWorkflowAgentResolveRequest -> permission_floor。测试注入材料,
// 生产是 AgentCatalog;形状与次序一字不差。
workflow::CustomAgentNodeResolution ResolveForWorkflow(
    const agent::AgentDefinition& definition, const std::string& name, const agent::AgentProfile& parent,
    const std::vector<std::string>& parent_tools, int default_steps,
    const agent::AgentProfileResolveEnvironment& env, int step_limit) {
    agent::AgentDispatchOverrides overrides;
    if (step_limit > 0) overrides.max_steps_per_turn = step_limit;
    workflow::CustomAgentNodeResolution out;
    out.resolved = agent::ResolveAgentProfile(agent::BuildWorkflowAgentResolveRequest(
        definition, parent, parent_tools, default_steps, env, overrides));
    out.material.definition = definition;
    out.resolved_name = name;
    if (agent::AgentPermissionModeRank(out.resolved.permission) <
        agent::AgentPermissionModeRank(env.parent_permission)) {
        out.permission_floor = out.resolved.permission;
    }
    return out;
}

// 请求的工具表名字序列(对账用;顺序也一并比)。
std::vector<std::string> RequestToolNames(const api::Request& request) {
    std::vector<std::string> names;
    for (const auto& tool : request.tools) names.push_back(tool.name);
    return names;
}

workflow::WorkflowDefinition ParseOrDie(const char* yaml) {
    auto parsed = workflow::ParseWorkflowYaml(yaml);
    REQUIRE(parsed.has_value());
    return *parsed;
}

// 事件录音机(取消路的终态分型用)。
class RecordingSink final : public runtime::EventSink {
public:
    void Emit(const runtime::ServerEvent& event) override { events.push_back(event); }
    std::vector<runtime::ServerEvent> events;
};

bool SawOutcome(const RecordingSink& sink, runtime::Outcome outcome) {
    for (const auto& event : sink.events) {
        if (event.outcome.has_value() && *event.outcome == outcome) return true;
    }
    return false;
}

}  // namespace

TEST_SUITE("workflow-custom-agent") {

// ---------------------------------------------------------------------------
// 解析链:稳定码,不静默退回
// ---------------------------------------------------------------------------

TEST_CASE("agent: 名字经解析口走统一 Resolver,回执身份是 resolved 名") {
    ToolLoopBackend backend;
    tools::ToolRegistry registry;
    RegisterProbeTools(registry);
    const agent::AgentDefinition definition = ParseDefinition(kProbeYaml);
    const agent::AgentProfile parent = MakeParent();

    workflow::AgentExecutor::Options options;
    options.default_binding.backend = &backend;
    options.default_binding.profile = parent;
    options.registry = &registry;
    options.task_loader = [](const std::string&) { return std::string("任务正文"); };
    options.custom_agent_resolver = [&](const workflow::WorkflowNode& node,
                                         std::string& error) -> std::optional<workflow::CustomAgentNodeResolution> {
        if (node.agent != "probe-reviewer") {
            error = "没有名叫 \"" + node.agent + "\" 的 Agent";
            return std::nullopt;
        }
        return ResolveForWorkflow(definition, node.agent, parent, ParentToolNames(registry), 9, MakeEnv(), 0);
    };
    workflow::AgentExecutor executor(std::move(options));

    const workflow::WorkflowDefinition def = ParseOrDie(R"YAML(
schema_version: 1
id: probe-flow
version: 1.0.0
entry: probe
nodes:
  probe:
    type: agent
    agent: probe-reviewer
    task: prompts/probe.md
)YAML");
    workflow::NodeExecRequest request;
    request.definition = &def;
    request.node = &def.node_map.at("probe");
    request.resolved_input = nlohmann::json{{"topic", std::string("账目")}};

    const workflow::NodeExecResult result = executor.Execute(request);
    REQUIRE(result.ok);
    CHECK(result.agent_name == "probe-reviewer");
    REQUIRE(backend.requests.size() == 2);
    // 皮来自 Resolver:模型/effort 照父,预算按 YAML 收窄。
    CHECK(backend.requests[0].model == "model-x");
    CHECK(backend.requests[0].reasoning_effort == "medium");
    // 工具面按 YAML allow 收窄(read_file/search;todo_write/run_command 不见)。
    const std::vector<std::string> names = RequestToolNames(backend.requests[0]);
    CHECK(names == std::vector<std::string>({"read_file", "search"}));
    // 系统提示同源拼装:自定义 persona + 能力推导按有效工具。
    CHECK(backend.requests[0].system.find("你是 probe-reviewer 子代理") != std::string::npos);
    // user message:task 正文在前、节点 input 在后。
    bool user_seen = false;
    for (const auto& message : backend.requests[0].messages) {
        for (const auto& block : message.content) {
            if (const auto* text = std::get_if<api::TextBlock>(&block)) {
                if (text->text.find("任务正文") != std::string::npos &&
                    text->text.find("账目") != std::string::npos) {
                    user_seen = true;
                }
            }
        }
    }
    CHECK(user_seen);
}

TEST_CASE("agent: 查无此名报 agent_unresolved,不发请求不换人") {
    ToolLoopBackend backend;
    tools::ToolRegistry registry;
    RegisterProbeTools(registry);

    workflow::AgentExecutor::Options options;
    options.default_binding.backend = &backend;
    options.default_binding.profile = MakeParent();
    options.registry = &registry;
    options.task_loader = [](const std::string&) { return std::string("任务正文"); };
    options.custom_agent_resolver = [](const workflow::WorkflowNode& node,
                                        std::string& error) -> std::optional<workflow::CustomAgentNodeResolution> {
        error = "没有名叫 \"" + node.agent + "\" 的 Agent(可用清单看 /agents)";
        return std::nullopt;
    };
    workflow::AgentExecutor executor(std::move(options));

    const workflow::WorkflowDefinition def = ParseOrDie(R"YAML(
schema_version: 1
id: ghost-flow
version: 1.0.0
entry: ghost
nodes:
  ghost:
    type: agent
    agent: no-such-agent
    task: prompts/ghost.md
)YAML");
    workflow::NodeExecRequest request;
    request.definition = &def;
    request.node = &def.node_map.at("ghost");
    request.resolved_input = nlohmann::json::object();

    const workflow::NodeExecResult result = executor.Execute(request);
    CHECK_FALSE(result.ok);
    CHECK(result.error_code == "agent_unresolved");
    CHECK(result.error_message.find("no-such-agent") != std::string::npos);
    CHECK(result.error_message.find("/agents") != std::string::npos);
    CHECK(backend.requests.empty());
}

TEST_CASE("agent: 定义解析不过报 agent_unresolved 带结构化错误;没接解析口报 not_configured") {
    tools::ToolRegistry registry;
    RegisterProbeTools(registry);
    const agent::AgentProfile parent = MakeParent();

    // 定义比父宽(父 Auto 下声明 Yolo):Resolver 报 agent.permission_widening。
    const agent::AgentDefinition widening = ParseDefinition(
        "schema: 1\nname: widening-probe\ndescription: 越权探针。\npermissions:\n  mode: yolo\n");
    workflow::AgentExecutor::Options options;
    options.default_binding.backend = nullptr;  // 不会走到
    options.registry = &registry;
    options.task_loader = [](const std::string&) { return std::string("任务正文"); };
    options.custom_agent_resolver = [&](const workflow::WorkflowNode& node,
                                         std::string&) -> std::optional<workflow::CustomAgentNodeResolution> {
        return ResolveForWorkflow(widening, node.agent, parent, ParentToolNames(registry), 9,
                                  MakeEnv(agent::AgentPermissionMode::Auto), 0);
    };
    workflow::AgentExecutor executor(std::move(options));

    const workflow::WorkflowDefinition def = ParseOrDie(R"YAML(
schema_version: 1
id: wide-flow
version: 1.0.0
entry: wide
nodes:
  wide:
    type: agent
    agent: widening-probe
    task: prompts/wide.md
)YAML");
    workflow::NodeExecRequest request;
    request.definition = &def;
    request.node = &def.node_map.at("wide");
    request.resolved_input = nlohmann::json::object();

    const workflow::NodeExecResult result = executor.Execute(request);
    CHECK_FALSE(result.ok);
    CHECK(result.error_code == "agent_unresolved");
    CHECK(result.error_message.find("agent.permission_widening") != std::string::npos);

    // 宿主没接解析口(旧装配/单测):not_configured,不静默走 default binding。
    workflow::AgentExecutor::Options bare_options;
    bare_options.default_binding.backend = nullptr;
    bare_options.registry = &registry;
    bare_options.task_loader = [](const std::string&) { return std::string("任务正文"); };
    workflow::AgentExecutor bare(std::move(bare_options));
    const workflow::NodeExecResult bare_result = bare.Execute(request);
    CHECK_FALSE(bare_result.ok);
    CHECK(bare_result.error_code == "not_configured");
    CHECK(bare_result.error_message.find("解析口") != std::string::npos);
}

// ---------------------------------------------------------------------------
// 编译期:能力表查引用 + 定义 roundtrip 收 agent 字段
// ---------------------------------------------------------------------------

TEST_CASE("validator:unknown_agent 编译期报,可用名放行;空表不校验") {
    const workflow::WorkflowDefinition def = ParseOrDie(R"YAML(
schema_version: 1
id: refs-flow
version: 1.0.0
entry: known
nodes:
  known:
    type: agent
    agent: probe-reviewer
    task: prompts/a.md
  unknown:
    type: agent
    agent: ghost-agent
    task: prompts/b.md
edges:
  - { from: known, on: success, to: unknown }
)YAML");

    workflow::CapabilityTable caps;
    caps.agent_names = {"probe-reviewer", "general-purpose", "Explore"};
    const workflow::ValidationResult result = workflow::ValidateDefinition(def, caps);
    REQUIRE_FALSE(result.ok());
    bool seen = false;
    for (const auto& issue : result.issues) {
        if (issue.code != "unknown_agent") continue;
        CHECK(issue.path == "nodes.unknown.agent");
        CHECK(issue.message.find("ghost-agent") != std::string::npos);
        seen = true;
    }
    CHECK(seen);

    // 空表(宿主没递)= 不校验,行为与从前一致。
    caps.agent_names.clear();
    const workflow::ValidationResult lax = workflow::ValidateDefinition(def, caps);
    bool any_agent_issue = false;
    for (const auto& issue : lax.issues) {
        if (issue.code == "unknown_agent") any_agent_issue = true;
    }
    CHECK_FALSE(any_agent_issue);
}

TEST_CASE("定义 JSON roundtrip 收 agent 字段(0.26.92 收 YAML 字段的欠账)") {
    const workflow::WorkflowDefinition def = ParseOrDie(R"YAML(
schema_version: 1
id: roundtrip-flow
version: 1.0.0
entry: probe
nodes:
  probe:
    type: agent
    agent: moontide.suite:browser-tester
    task: prompts/probe.md
)YAML");
    REQUIRE(def.node_map.at("probe").agent == "moontide.suite:browser-tester");

    const nlohmann::json json = def.ToJson();
    const workflow::WorkflowDefinition back = workflow::WorkflowDefinition::FromJson(json);
    CHECK(back.node_map.at("probe").agent == "moontide.suite:browser-tester");
    // 归一化 JSON 也收(内容 hash 才不漏字段)。
    const nlohmann::json normalized = workflow::BuildNormalizedJson(def);
    CHECK(normalized.dump().find("moontide.suite:browser-tester") != std::string::npos);
}

// ---------------------------------------------------------------------------
// 两路同源(验收线):同一 YAML,agent 工具路 == Workflow 节点路
// ---------------------------------------------------------------------------

TEST_CASE("对账:同一 YAML 定义两路各跑一轮,系统提示与工具表逐字节一致") {
    // ---- agent 工具路 ----
    ToolLoopBackend tool_backend;
    tools::ToolRegistry sub_registry;
    RegisterProbeTools(sub_registry);
    const agent::AgentDefinition definition_a = ParseDefinition(kProbeYaml);
    const agent::AgentProfile parent = MakeParent();
    const agent::AgentProfileResolveEnvironment env = MakeEnv();

    tools::AgentTool agent_tool(tool_backend, sub_registry, "/work/probe", "model-x", 9);
    agent_tool.SetAgentProfile(parent);
    agent_tool.SetResolveEnvironment([env]() { return env; });
    agent_tool.SetCustomAgentResolver(
        [definition_a](const std::string& name) -> std::optional<tools::CustomAgentMaterial> {
            if (name != definition_a.name) return std::nullopt;
            tools::CustomAgentMaterial material;
            material.definition = definition_a;
            return material;
        });
    nlohmann::json input;
    input["title"] = "审查";
    input["prompt"] = "把账目过一遍";
    input["agent_type"] = "probe-reviewer";
    const tools::Tool::Result tool_result = agent_tool.execute(input);
    REQUIRE_FALSE(tool_result.is_error);
    REQUIRE(tool_backend.requests.size() == 2);

    // ---- Workflow 节点路 ----
    ToolLoopBackend wf_backend;
    tools::ToolRegistry registry;
    RegisterProbeTools(registry);
    const agent::AgentDefinition definition_b = ParseDefinition(kProbeYaml);

    workflow::AgentExecutor::Options options;
    options.default_binding.backend = &wf_backend;
    options.default_binding.profile = parent;
    options.registry = &registry;
    options.task_loader = [](const std::string&) { return std::string("把账目过一遍"); };
    options.custom_agent_resolver = [&](const workflow::WorkflowNode& node,
                                         std::string& error) -> std::optional<workflow::CustomAgentNodeResolution> {
        if (node.agent != definition_b.name) {
            error = "没有这名";
            return std::nullopt;
        }
        return ResolveForWorkflow(definition_b, node.agent, parent, ParentToolNames(registry), 9, env, 0);
    };
    // 会话材料与 agent 工具路的构造参数同值(cwd 一致才有同一段运行环境)。
    options.subagent_prompt_material.cwd = "/work/probe";
    workflow::AgentExecutor executor(std::move(options));

    const workflow::WorkflowDefinition def = ParseOrDie(R"YAML(
schema_version: 1
id: same-source-flow
version: 1.0.0
entry: probe
nodes:
  probe:
    type: agent
    agent: probe-reviewer
    task: prompts/probe.md
)YAML");
    workflow::NodeExecRequest request;
    request.definition = &def;
    request.node = &def.node_map.at("probe");
    request.resolved_input = nlohmann::json{{"topic", std::string("账目")}};
    const workflow::NodeExecResult wf_result = executor.Execute(request);
    REQUIRE(wf_result.ok);
    REQUIRE(wf_backend.requests.size() == 2);

    // 系统提示逐字节一致。
    CHECK(tool_backend.requests[0].system == wf_backend.requests[0].system);
    // 工具表逐枚逐字节(名字与 schema 的 JSON 序列化都相同,顺序也相同)。
    REQUIRE(tool_backend.requests[0].tools.size() == wf_backend.requests[0].tools.size());
    for (std::size_t i = 0; i < tool_backend.requests[0].tools.size(); ++i) {
        CHECK(tool_backend.requests[0].tools[i].name == wf_backend.requests[0].tools[i].name);
        nlohmann::json a = tool_backend.requests[0].tools[i].input_schema;
        nlohmann::json b = wf_backend.requests[0].tools[i].input_schema;
        CHECK(a.dump() == b.dump());
    }
    // 抽几笔决议值:两路同一份(YAML 收窄的预算、关掉的魂)。
    CHECK(tool_backend.requests[0].max_tokens.value_or(0) == wf_backend.requests[0].max_tokens.value_or(0));
}

// ---------------------------------------------------------------------------
// 四件语义:与 role 路同一条收口链,逐一照旧
// ---------------------------------------------------------------------------

TEST_CASE("语义对账:前台失败与预算尽,自定义路与 role 路同码") {
    tools::ToolRegistry registry;
    RegisterProbeTools(registry);
    const agent::AgentProfile parent = MakeParent();

    const workflow::WorkflowDefinition def = ParseOrDie(R"YAML(
schema_version: 1
id: semantics-flow
version: 1.0.0
entry: custom
nodes:
  role:
    type: agent
    task: prompts/a.md
    step_limit: 1
  custom:
    type: agent
    agent: probe-reviewer
    task: prompts/b.md
edges:
  - { from: custom, on: success, to: role }
)YAML");

    const agent::AgentDefinition definition = ParseDefinition(kProbeYaml);

    // 前台失败:backend 报错 -> agent_error(role 路既有册同码)。
    {
        FailingBackend backend;
        workflow::AgentExecutor::Options options;
        options.default_binding.backend = &backend;
        options.default_binding.profile = parent;
        options.registry = &registry;
        options.task_loader = [](const std::string&) { return std::string("任务正文"); };
        options.custom_agent_resolver = [&](const workflow::WorkflowNode& node,
                                             std::string&) -> std::optional<workflow::CustomAgentNodeResolution> {
            return ResolveForWorkflow(definition, node.agent, parent, ParentToolNames(registry), 9, MakeEnv(), 0);
        };
        workflow::AgentExecutor executor(std::move(options));

        workflow::NodeExecRequest custom_request;
        custom_request.definition = &def;
        custom_request.node = &def.node_map.at("custom");
        custom_request.resolved_input = nlohmann::json::object();
        const workflow::NodeExecResult custom_result = executor.Execute(custom_request);
        CHECK_FALSE(custom_result.ok);
        CHECK(custom_result.error_code == "agent_error");

        workflow::NodeExecRequest role_request;
        role_request.definition = &def;
        role_request.node = &def.node_map.at("role");
        role_request.resolved_input = nlohmann::json::object();
        const workflow::NodeExecResult role_result = executor.Execute(role_request);
        CHECK_FALSE(role_result.ok);
        CHECK(role_result.error_code == "agent_error");
        CHECK(custom_result.error_code == role_result.error_code);
    }

    // 预算尽:步数闸 -> budget_exhausted(自定义路的 YAML 6 步与 role 路的
    // step_limit 1 同一收口)。
    {
        ForeverToolBackend backend;
        workflow::AgentExecutor::Options options;
        options.default_binding.backend = &backend;
        options.default_binding.profile = parent;
        options.registry = &registry;
        options.task_loader = [](const std::string&) { return std::string("任务正文"); };
        options.custom_agent_resolver = [&](const workflow::WorkflowNode& node,
                                             std::string&) -> std::optional<workflow::CustomAgentNodeResolution> {
            return ResolveForWorkflow(definition, node.agent, parent, ParentToolNames(registry), 9, MakeEnv(), 0);
        };
        workflow::AgentExecutor executor(std::move(options));

        workflow::NodeExecRequest custom_request;
        custom_request.definition = &def;
        custom_request.node = &def.node_map.at("custom");
        custom_request.resolved_input = nlohmann::json::object();
        const workflow::NodeExecResult custom_result = executor.Execute(custom_request);
        CHECK_FALSE(custom_result.ok);
        CHECK(custom_result.error_code == "budget_exhausted");

        workflow::NodeExecRequest role_request;
        role_request.definition = &def;
        role_request.node = &def.node_map.at("role");
        role_request.resolved_input = nlohmann::json::object();
        const workflow::NodeExecResult role_result = executor.Execute(role_request);
        CHECK(role_result.error_code == "budget_exhausted");
    }
}

TEST_CASE("语义对账:取消与 async 后台回收,自定义路照旧") {
    tools::ToolRegistry registry;
    RegisterProbeTools(registry);
    const agent::AgentProfile parent = MakeParent();
    const agent::AgentDefinition definition = ParseDefinition(kProbeYaml);

    // 取消:cancel 旗打断在途请求,事件流收 Cancelled;两路同款。
    {
        GateBackend custom_backend;
        RecordingSink custom_sink;
        workflow::AgentExecutor::Options custom_options;
        custom_options.event_sink = &custom_sink;
        custom_options.default_binding.backend = &custom_backend;
        custom_options.default_binding.profile = parent;
        custom_options.registry = &registry;
        custom_options.task_loader = [](const std::string&) { return std::string("任务正文"); };
        custom_options.custom_agent_resolver = [&](const workflow::WorkflowNode& node,
                                                    std::string&) -> std::optional<workflow::CustomAgentNodeResolution> {
            return ResolveForWorkflow(definition, node.agent, parent, ParentToolNames(registry), 9, MakeEnv(), 0);
        };
        workflow::AgentExecutor custom_executor(std::move(custom_options));

        GateBackend role_backend;
        workflow::AgentExecutor::Options role_options;
        role_options.default_binding.backend = &role_backend;
        role_options.default_binding.profile = parent;
        role_options.registry = &registry;
        role_options.task_loader = [](const std::string&) { return std::string("任务正文"); };
        workflow::AgentExecutor role_executor(std::move(role_options));

        const workflow::WorkflowDefinition def = ParseOrDie(R"YAML(
schema_version: 1
id: cancel-flow
version: 1.0.0
entry: custom
nodes:
  custom:
    type: agent
    agent: probe-reviewer
    task: prompts/b.md
  role:
    type: agent
    task: prompts/a.md
edges:
  - { from: custom, on: success, to: role }
)YAML");

        std::atomic<bool> custom_cancel{false};
        workflow::NodeExecRequest custom_request;
        custom_request.definition = &def;
        custom_request.node = &def.node_map.at("custom");
        custom_request.resolved_input = nlohmann::json::object();
        custom_request.cancel = &custom_cancel;
        std::thread canceller([&custom_backend, &custom_cancel]() {
            std::unique_lock<std::mutex> lock(custom_backend.mutex);
            custom_backend.cv.wait_for(lock, std::chrono::seconds(2),
                                       [&]() { return custom_backend.started; });
            custom_cancel.store(true);
            // GateBackend 的等待谓词只看 cancel,置位即自醒。
        });
        const workflow::NodeExecResult custom_result = custom_executor.Execute(custom_request);
        canceller.join();
        // 打断不是错误(既有语义):请求被掐、只发过一发、产出为空,节点
        // 半截收场;与 role 路同形。
        CHECK(custom_result.ok);
        CHECK(custom_result.error_code.empty());
        REQUIRE(custom_backend.requests.size() == 1);
        // 事件流的终态分型:嵌套轮收 Cancelled(既有语义,自定义路同款)。
        CHECK(SawOutcome(custom_sink, runtime::Outcome::Cancelled));

        std::atomic<bool> role_cancel{false};
        workflow::NodeExecRequest role_request;
        role_request.definition = &def;
        role_request.node = &def.node_map.at("role");
        role_request.resolved_input = nlohmann::json::object();
        role_request.cancel = &role_cancel;
        std::thread role_canceller([&role_backend, &role_cancel]() {
            std::unique_lock<std::mutex> lock(role_backend.mutex);
            role_backend.cv.wait_for(lock, std::chrono::seconds(2), [&]() { return role_backend.started; });
            role_cancel.store(true);
        });
        const workflow::NodeExecResult role_result = role_executor.Execute(role_request);
        role_canceller.join();
        CHECK(role_result.ok);
        CHECK(role_result.error_code == custom_result.error_code);
        REQUIRE(role_backend.requests.size() == 1);
    }

    // async 后台回收:async 外壳换工作线程跑自定义 agent body,产物挂回
    // 外壳节点,整场 Succeeded。
    {
        ToolLoopBackend backend;
        workflow::AgentExecutor::Options options;
        options.default_binding.backend = &backend;
        options.default_binding.profile = parent;
        options.registry = &registry;
        options.task_loader = [](const std::string&) { return std::string("任务正文"); };
        options.custom_agent_resolver = [&](const workflow::WorkflowNode& node,
                                             std::string&) -> std::optional<workflow::CustomAgentNodeResolution> {
            return ResolveForWorkflow(definition, node.agent, parent, ParentToolNames(registry), 9, MakeEnv(), 0);
        };
        auto agent_executor = std::make_shared<workflow::AgentExecutor>(std::move(options));

        const workflow::WorkflowDefinition def = ParseOrDie(R"YAML(
schema_version: 1
id: async-custom-flow
version: 1.0.0
entry: wait
nodes:
  wait:
    type: async
    body: probe
  probe:
    type: agent
    agent: probe-reviewer
    task: prompts/probe.md
  fin: { type: end }
edges:
  - { from: wait, on: success, to: fin }
result:
  out: "${nodes.probe.output}"
)YAML");
        workflow::RuntimeOptions runtime_options;
        runtime_options.executors[workflow::NodeKind::Agent] = agent_executor;
        workflow::WorkflowRuntime runtime(std::move(runtime_options));
        const workflow::WorkflowRunSummary summary = runtime.Run(def, workflow::RunInputs{});
        REQUIRE(summary.state == workflow::RunState::Succeeded);
        CHECK(summary.nodes.at("wait").state == workflow::NodeState::Succeeded);
        CHECK(summary.nodes.at("probe").state == workflow::NodeState::Succeeded);
        CHECK(summary.result["out"]["answer"] == "ok");
    }
}

// ---------------------------------------------------------------------------
// 权限下限:Resolver 的 permission_floor 接进 on_tool_confirm_floored
// ---------------------------------------------------------------------------

TEST_CASE("权限下限:定义比会话档严时确认真拉回,档不严或没接口时原样走") {
    tools::ToolRegistry registry;
    RegisterProbeTools(registry);
    const agent::AgentProfile parent = MakeParent();

    // 父 Yolo 子 confirm:floor 递进来,floored 口被调,needs_confirm 工具被拒。
    const agent::AgentDefinition strict = ParseDefinition(
        "schema: 1\nname: strict-probe\ndescription: 严档探针。\ntools:\n  allow:\n    - read_file\n    - "
        "run_command\npermissions:\n  mode: confirm\n");

    ToolLoopBackend backend;
    backend.first_tool = "run_command";
    workflow::AgentExecutor::Options options;
    options.default_binding.backend = &backend;
    options.default_binding.profile = parent;
    options.registry = &registry;
    options.task_loader = [](const std::string&) { return std::string("任务正文"); };
    options.custom_agent_resolver = [&](const workflow::WorkflowNode& node,
                                         std::string&) -> std::optional<workflow::CustomAgentNodeResolution> {
        return ResolveForWorkflow(strict, node.agent, parent, ParentToolNames(registry), 9,
                                  MakeEnv(agent::AgentPermissionMode::Yolo), 0);
    };
    agent::AgentPermissionMode seen_floor = agent::AgentPermissionMode::Yolo;
    int floored_calls = 0;
    int plain_calls = 0;
    options.callbacks.on_tool_confirm_floored =
        [&seen_floor, &floored_calls](const std::string&, const std::string&, const nlohmann::json&,
                                      agent::AgentPermissionMode floor) {
            seen_floor = floor;
            ++floored_calls;
            return false;  // 宿主按 min(会话档 Yolo, 下限 Confirm) 裁定:真问,测试里答"拒"
        };
    options.callbacks.on_tool_confirm = [&plain_calls](const std::string&, const std::string&,
                                                       const nlohmann::json&) {
        ++plain_calls;
        return true;
    };
    workflow::AgentExecutor executor(std::move(options));

    const workflow::WorkflowDefinition def = ParseOrDie(R"YAML(
schema_version: 1
id: strict-flow
version: 1.0.0
entry: strict
nodes:
  strict:
    type: agent
    agent: strict-probe
    task: prompts/strict.md
)YAML");
    workflow::NodeExecRequest request;
    request.definition = &def;
    request.node = &def.node_map.at("strict");
    request.resolved_input = nlohmann::json::object();

    const workflow::NodeExecResult result = executor.Execute(request);
    REQUIRE(result.ok);
    CHECK(floored_calls == 1);
    CHECK(seen_floor == agent::AgentPermissionMode::Confirm);
    CHECK(plain_calls == 0);  // floored 口在,原口不跑

    // 档不严(inherit 同父):没有 floor,原样走 on_tool_confirm。
    const agent::AgentDefinition inherit = ParseDefinition(
        "schema: 1\nname: inherit-probe\ndescription: 同档探针。\ntools:\n  allow:\n    - read_file\n    - "
        "run_command\n");
    ToolLoopBackend plain_backend;
    plain_backend.first_tool = "run_command";
    workflow::AgentExecutor::Options plain_options;
    plain_options.default_binding.backend = &plain_backend;
    plain_options.default_binding.profile = parent;
    plain_options.registry = &registry;
    plain_options.task_loader = [](const std::string&) { return std::string("任务正文"); };
    plain_options.custom_agent_resolver = [&](const workflow::WorkflowNode& node,
                                               std::string&) -> std::optional<workflow::CustomAgentNodeResolution> {
        return ResolveForWorkflow(inherit, node.agent, parent, ParentToolNames(registry), 9,
                                  MakeEnv(agent::AgentPermissionMode::Yolo), 0);
    };
    int plain_floored = 0;
    int plain_confirm = 0;
    plain_options.callbacks.on_tool_confirm_floored =
        [&plain_floored](const std::string&, const std::string&, const nlohmann::json&,
                         agent::AgentPermissionMode) {
            ++plain_floored;
            return false;
        };
    plain_options.callbacks.on_tool_confirm = [&plain_confirm](const std::string&, const std::string&,
                                                               const nlohmann::json&) {
        ++plain_confirm;
        return true;
    };
    workflow::AgentExecutor plain_executor(std::move(plain_options));

    const workflow::WorkflowDefinition plain_def = ParseOrDie(R"YAML(
schema_version: 1
id: inherit-flow
version: 1.0.0
entry: inherit
nodes:
  inherit:
    type: agent
    agent: inherit-probe
    task: prompts/inherit.md
)YAML");
    workflow::NodeExecRequest plain_request;
    plain_request.definition = &plain_def;
    plain_request.node = &plain_def.node_map.at("inherit");
    plain_request.resolved_input = nlohmann::json::object();
    const workflow::NodeExecResult plain_result = plain_executor.Execute(plain_request);
    REQUIRE(plain_result.ok);
    CHECK(plain_floored == 0);
    CHECK(plain_confirm == 1);  // 没有 floor,原口照走
}

// ---------------------------------------------------------------------------
// 白名单交集:节点 allowed_tools 压过 YAML allow
// ---------------------------------------------------------------------------

TEST_CASE("节点 allowed_tools 压过 YAML allow:谓词与能力推导按交集") {
    ToolLoopBackend backend;
    tools::ToolRegistry registry;
    RegisterProbeTools(registry);
    const agent::AgentProfile parent = MakeParent();
    const agent::AgentDefinition definition = ParseDefinition(kProbeYaml);

    workflow::AgentExecutor::Options options;
    options.default_binding.backend = &backend;
    options.default_binding.profile = parent;
    options.registry = &registry;
    options.task_loader = [](const std::string&) { return std::string("任务正文"); };
    options.custom_agent_resolver = [&](const workflow::WorkflowNode& node,
                                         std::string&) -> std::optional<workflow::CustomAgentNodeResolution> {
        return ResolveForWorkflow(definition, node.agent, parent, ParentToolNames(registry), 9, MakeEnv(), 0);
    };
    workflow::AgentExecutor executor(std::move(options));

    const workflow::WorkflowDefinition def = ParseOrDie(R"YAML(
schema_version: 1
id: whitelist-flow
version: 1.0.0
entry: probe
nodes:
  probe:
    type: agent
    agent: probe-reviewer
    task: prompts/probe.md
    allowed_tools: [read_file]
)YAML");
    workflow::NodeExecRequest request;
    request.definition = &def;
    request.node = &def.node_map.at("probe");
    request.resolved_input = nlohmann::json::object();

    const workflow::NodeExecResult result = executor.Execute(request);
    REQUIRE(result.ok);
    REQUIRE(backend.requests.size() == 2);
    const std::vector<std::string> names = RequestToolNames(backend.requests[0]);
    CHECK(names == std::vector<std::string>({"read_file"}));  // 交集后只剩 read_file
}

}  // TEST_SUITE(workflow-custom-agent)
