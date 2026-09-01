// AgentProfileResolver 的单测(自定义 Agent 单阶段 3)。四笔账:
//   - 预算并流:runtime 四字段(max_output_tokens/max_context_chars/
//     context_window_tokens/length_continuations)按"YAML 显式 > 父值"合并;
//     max_steps_per_turn 按"入参显式 > YAML > 配置默认";max_output_tokens
//     的来源枚举四级(YAML=ConfigFile / provider / 目录 / unset)各钉一档。
//   - 权限只可收窄:inherit 同父、收窄放行、放宽报 agent.permission_widening。
//   - 缺依赖结构化错误:requires.tools / mcp_servers / skills.preload 缺项报
//     agent.missing_dependency;allow 越权报 agent.tool_not_granted;effort
//     越档报 agent.effort_not_supported——一律不悄悄放宽。
//   - 两路对账(验收线):同一份 YAML(夹具 complete.yaml)从 AgentTool 路
//     (BuildSubagentResolveRequest)与 Workflow 路
//     (BuildWorkflowAgentResolveRequest)各解析一遍,逐字段比,必须全等。

#include <doctest/doctest.h>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "agent/agent_definition.hpp"
#include "agent/agent_profile_resolver.hpp"
#include "agent/runtime_profile.hpp"
#include "tools/tool.hpp"

#ifndef LUBANCODE_TEST_FIXTURES_DIR
#define LUBANCODE_TEST_FIXTURES_DIR "tests/fixtures"
#endif

using namespace lubancode;

namespace {

// 夹具 complete.yaml 的全文(全字段样例);不依赖盘上的文件也解析得出,
// 但对账那条真读夹具,两不吃亏。
std::string ReadFixture(const std::string& relative) {
    const std::filesystem::path path = std::filesystem::path(LUBANCODE_TEST_FIXTURES_DIR) / relative;
    std::ifstream in(path, std::ios::binary);
    if (!in.is_open()) {
        return std::string();
    }
    std::ostringstream buffer;
    buffer << in.rdbuf();
    return buffer.str();
}

agent::AgentDefinition ParseOrThrow(const std::string& yaml) {
    const agent::AgentDefinitionParseResult parsed = agent::ParseAgentDefinitionYaml(yaml, "(test)");
    REQUIRE(parsed.definition.has_value());
    return *parsed.definition;
}

// 造一份带四枚预算字段的定义。
std::string BudgetYaml(const std::string& runtime_block) {
    return "schema: 1\n"
           "name: budget-probe\n"
           "description: 预算并流探针。\n"
           "runtime:\n" +
           runtime_block;
}

// 有没有哪条 error 落在指定码上。
bool HasCode(const std::vector<agent::AgentDefinitionIssue>& issues, const std::string& code) {
    for (const auto& issue : issues) {
        if (issue.code == code && !issue.warning) {
            return true;
        }
    }
    return false;
}

// 对账用的父上下文(两条路共用同一份——验收线钉的就是"给定同一父上下文
// 与同一份定义,两路解析必须得出同一份 Profile")。
agent::AgentProfile MakeParentProfile() {
    agent::AgentProfile parent;
    parent.provider = "prov-a";
    parent.request.model = "model-main";
    parent.request.reasoning_effort = "medium";
    parent.runtime.max_output_tokens = 4096;
    parent.runtime.max_output_tokens_source = agent::OutputBudgetSource::ProviderDeclared;
    parent.runtime.max_steps_per_turn = 12;
    parent.runtime.max_context_chars = 300000;
    parent.runtime.context_window_tokens = 128000;
    parent.runtime.length_continuations = 2;
    parent.prompt_sections.mcp = true;
    parent.prompt_sections.web = false;
    parent.prompt_sections.lsp = true;
    parent.prompt_sections.wire = "anthropic";
    return parent;
}

agent::AgentProfileResolveEnvironment MakeEnvironment() {
    agent::AgentProfileResolveEnvironment env;
    env.parent_permission = agent::AgentPermissionMode::Auto;
    env.skill_names = {"browser-testing"};
    env.mcp_server_names = {"browser"};
    env.role_normal = {"", "model-normal"};
    env.role_cheap = {"", "model-cheap"};
    env.role_lao = {"prov-lao", "model-lao"};
    env.supported_efforts = {"low", "medium", "high"};
    return env;
}

std::vector<std::string> MakeParentTools() {
    return {"read_file", "search", "run_command", "todo_write", "mcp__browser__navigate"};
}

// 逐字段比两份 resolved(验收线:完全一致)。工具谓词是 std::function,
// 没法直接比——用同一份工具名单喂两份谓词,行为等价才算过。
void CheckProfilesIdentical(const agent::ResolvedAgentProfile& a, const agent::ResolvedAgentProfile& b) {
    CHECK(a.profile.provider == b.profile.provider);
    CHECK(a.profile.request.model == b.profile.request.model);
    CHECK(a.profile.request.reasoning_effort == b.profile.request.reasoning_effort);
    CHECK(a.profile.runtime.max_output_tokens == b.profile.runtime.max_output_tokens);
    CHECK(a.profile.runtime.max_output_tokens_source == b.profile.runtime.max_output_tokens_source);
    CHECK(a.profile.runtime.max_steps_per_turn == b.profile.runtime.max_steps_per_turn);
    CHECK(a.profile.runtime.max_context_chars == b.profile.runtime.max_context_chars);
    CHECK(a.profile.runtime.context_window_tokens == b.profile.runtime.context_window_tokens);
    CHECK(a.profile.runtime.length_continuations == b.profile.runtime.length_continuations);
    CHECK(a.profile.runtime.max_wall_secs == b.profile.runtime.max_wall_secs);
    CHECK(a.profile.runtime.max_total_tokens == b.profile.runtime.max_total_tokens);
    CHECK(a.profile.runtime.budget_soft_percent == b.profile.runtime.budget_soft_percent);
    CHECK(a.profile.prompt_sections.mcp == b.profile.prompt_sections.mcp);
    CHECK(a.profile.prompt_sections.web == b.profile.prompt_sections.web);
    CHECK(a.profile.prompt_sections.lsp == b.profile.prompt_sections.lsp);
    CHECK(a.profile.prompt_sections.wire == b.profile.prompt_sections.wire);
    CHECK(a.profile.system_prompt == b.profile.system_prompt);
    CHECK(a.profile.model_instructions == b.profile.model_instructions);
    CHECK(a.profile.soul == b.profile.soul);
    CHECK(a.profile.tool_filter_denial == b.profile.tool_filter_denial);
    CHECK(a.effective_tools == b.effective_tools);
    CHECK(a.prompt_profile == b.prompt_profile);
    CHECK(a.project_instructions == b.project_instructions);
    CHECK(a.soul == b.soul);
    CHECK(a.execution_mode == b.execution_mode);
    CHECK(a.isolation == b.isolation);
    CHECK(a.permission == b.permission);
    // 任务 turn 预算(turn 预算单 P0-3 对账线):数值、来源与 legacy 影子账
    // 三笔都全等——同一 Agent 定义两路跑,预算逐字段一致。
    CHECK(a.turn_budget.max_turns == b.turn_budget.max_turns);
    CHECK(a.turn_budget.source == b.turn_budget.source);
    CHECK(a.turn_budget.legacy_max_steps_per_input == b.turn_budget.legacy_max_steps_per_input);
    CHECK(a.issues.size() == b.issues.size());
    // 谓词行为等价:同一份候选名单,两份谓词逐名同断。
    const bool a_has_filter = a.profile.tool_filter != nullptr;
    CHECK(a_has_filter == (b.profile.tool_filter != nullptr));
    if (a_has_filter) {
        struct NamedTool : tools::Tool {
            std::string label;
            explicit NamedTool(std::string name) : label(std::move(name)) {}
            std::string name() const override { return label; }
            std::string description() const override { return {}; }
            nlohmann::json input_schema() const override { return nlohmann::json::object(); }
            Result execute(const nlohmann::json&) override { return {"", false}; }
        };
        for (const std::string& candidate :
             {"read_file", "search", "run_command", "todo_write", "mcp__browser__navigate", "write_file"}) {
            NamedTool tool(candidate);
            CHECK(a.profile.tool_filter(tool) == b.profile.tool_filter(tool));
        }
    }
}

}  // namespace

// ---------------------------------------------------------------------------
// 预算并流:四字段 YAML 显式 > 父值;steps 入参 > YAML > 配置默认
// ---------------------------------------------------------------------------

TEST_CASE("预算并流:YAML 四字段显式给就按给的上,没给落父值") {
    const agent::AgentDefinition with_yaml = ParseOrThrow(BudgetYaml(
        "  max_output_tokens: 8192\n"
        "  max_steps_per_turn: 24\n"
        "  max_context_chars: 600000\n"
        "  context_window_tokens: 200000\n"
        "  length_continuations: 3\n"));
    const agent::AgentDefinition without_yaml = ParseOrThrow(BudgetYaml("  max_steps_per_turn: 24\n"));
    agent::AgentProfileResolveRequest request;
    request.parent_profile = MakeParentProfile();
    request.parent_tool_names = MakeParentTools();
    request.default_max_steps_per_turn = 9;

    agent::ResolvedAgentProfile resolved = agent::ResolveAgentProfile(request);
    CHECK(resolved.ok());
    // 父值原样继承(父的来源也照抄,不重算三级)。
    CHECK(resolved.profile.runtime.max_output_tokens == 4096);
    CHECK(resolved.profile.runtime.max_output_tokens_source == agent::OutputBudgetSource::ProviderDeclared);
    CHECK(resolved.profile.runtime.max_context_chars == 300000);
    CHECK(resolved.profile.runtime.context_window_tokens == 128000);
    CHECK(resolved.profile.runtime.length_continuations == 2);
    CHECK(resolved.profile.runtime.max_steps_per_turn == 9);  // YAML 没给,落配置默认

    request.definition = with_yaml;
    resolved = agent::ResolveAgentProfile(request);
    CHECK(resolved.ok());
    CHECK(resolved.profile.runtime.max_output_tokens == 8192);
    CHECK(resolved.profile.runtime.max_output_tokens_source == agent::OutputBudgetSource::ConfigFile);
    CHECK(resolved.profile.runtime.max_context_chars == 600000);
    CHECK(resolved.profile.runtime.context_window_tokens == 200000);
    CHECK(resolved.profile.runtime.length_continuations == 3);
    CHECK(resolved.profile.runtime.max_steps_per_turn == 24);  // YAML 压过配置默认

    // 入参显式压过 YAML(steps 三级的顶格)。
    request.overrides.max_steps_per_turn = 5;
    resolved = agent::ResolveAgentProfile(request);
    CHECK(resolved.profile.runtime.max_steps_per_turn == 5);  // YAML 24 被入参 5 压住
}

TEST_CASE("max_output_tokens 四级来源各一档:YAML=ConfigFile > provider > 目录 > unset") {
    const std::string yaml = BudgetYaml("  max_output_tokens: 777\n");
    const struct {
        std::optional<int> tokens;
        agent::OutputBudgetSource source;
        int expect;  // -1 = unset
    } parents[] = {
        {4096, agent::OutputBudgetSource::ConfigFile, 4096},
        {2048, agent::OutputBudgetSource::ProviderDeclared, 2048},
        {1024, agent::OutputBudgetSource::ModelCatalog, 1024},
        {std::nullopt, agent::OutputBudgetSource::Unset, -1},
    };
    for (const auto& parent_case : parents) {
        agent::AgentProfileResolveRequest request;
        request.parent_profile = MakeParentProfile();
        request.parent_profile.runtime.max_output_tokens = parent_case.tokens;
        request.parent_profile.runtime.max_output_tokens_source = parent_case.source;
        request.parent_tool_names = MakeParentTools();
        request.default_max_steps_per_turn = 0;

        // YAML 缺席:父值与父来源原样继承(四级里的后三级由此钉住)。
        agent::ResolvedAgentProfile resolved = agent::ResolveAgentProfile(request);
        CHECK(resolved.ok());
        CHECK(resolved.profile.runtime.max_output_tokens == parent_case.tokens);
        CHECK(resolved.profile.runtime.max_output_tokens_source == parent_case.source);

        // YAML 在场:视同 config 级显式(第一级),来源标 ConfigFile。
        request.definition = ParseOrThrow(yaml);
        resolved = agent::ResolveAgentProfile(request);
        CHECK(resolved.profile.runtime.max_output_tokens == 777);
        CHECK(resolved.profile.runtime.max_output_tokens_source == agent::OutputBudgetSource::ConfigFile);
    }
}

TEST_CASE("context_window:YAML > 会话同步值 > 父皮值(三级钉一档)") {
    const agent::AgentDefinition yaml_window = ParseOrThrow(BudgetYaml("  context_window_tokens: 900000\n"));
    agent::AgentProfileResolveRequest request;
    request.parent_profile = MakeParentProfile();  // 父皮 128000
    request.parent_tool_names = MakeParentTools();

    request.context_window_tokens = 0;
    CHECK(agent::ResolveAgentProfile(request).profile.runtime.context_window_tokens == 128000);  // 落父皮

    request.context_window_tokens = 256000;
    CHECK(agent::ResolveAgentProfile(request).profile.runtime.context_window_tokens == 256000);  // 同步值压父皮

    request.definition = yaml_window;
    request.context_window_tokens = 256000;
    CHECK(agent::ResolveAgentProfile(request).profile.runtime.context_window_tokens == 900000);  // YAML 压一切
}

// ---------------------------------------------------------------------------
// 权限只可收窄(契约 §4.9)
// ---------------------------------------------------------------------------

TEST_CASE("权限:inherit 同父、收窄放行、放宽报 agent.permission_widening") {
    auto resolve_with = [](const std::string& mode, agent::AgentPermissionMode parent) {
        const std::string yaml = "schema: 1\nname: perm-probe\ndescription: 权限探针。\npermissions:\n  mode: " +
                                 mode + "\n";
        agent::AgentProfileResolveRequest request;
        request.definition = ParseOrThrow(yaml);
        request.parent_permission = parent;
        request.parent_tool_names = MakeParentTools();
        return agent::ResolveAgentProfile(request);
    };

    // inherit / 空缺 = 同父,不报错。
    agent::ResolvedAgentProfile resolved = resolve_with("inherit", agent::AgentPermissionMode::Auto);
    CHECK(resolved.ok());
    CHECK(resolved.permission == agent::AgentPermissionMode::Auto);

    // 收窄:auto 父下声明 confirm,放行。
    resolved = resolve_with("confirm", agent::AgentPermissionMode::Auto);
    CHECK(resolved.ok());
    CHECK(resolved.permission == agent::AgentPermissionMode::Confirm);

    // 同档:不算放宽。
    resolved = resolve_with("auto", agent::AgentPermissionMode::Auto);
    CHECK(resolved.ok());

    // 放宽一档:confirm 父下声明 auto,结构化报错。
    resolved = resolve_with("auto", agent::AgentPermissionMode::Confirm);
    CHECK_FALSE(resolved.ok());
    CHECK(HasCode(resolved.issues, "agent.permission_widening"));
    CHECK(agent::FormatResolutionIssues(resolved.issues).find("agent.permission_widening") != std::string::npos);

    // 放宽两档:confirm 父下声明 yolo,同样报。
    resolved = resolve_with("yolo", agent::AgentPermissionMode::Confirm);
    CHECK_FALSE(resolved.ok());
    CHECK(HasCode(resolved.issues, "agent.permission_widening"));
}

// ---------------------------------------------------------------------------
// 缺依赖:结构化错误,不悄悄放宽
// ---------------------------------------------------------------------------

TEST_CASE("缺依赖:requires/MCP/Skill 缺项与 allow 越权各报各的码") {
    const std::string yaml =
        "schema: 1\n"
        "name: dep-probe\n"
        "description: 依赖探针。\n"
        "skills:\n"
        "  preload:\n"
        "    - browser-testing\n"
        "    - no-such-skill\n"
        "tools:\n"
        "  allow:\n"
        "    - read_file\n"
        "    - mcp__browser__navigate\n"
        "mcp_servers:\n"
        "  - browser\n"
        "  - ghost-server\n"
        "requires:\n"
        "  tools:\n"
        "    - read_file\n"
        "    - run_command\n";  // allow 没放行 run_command -> requires 断言不过
    agent::AgentProfileResolveRequest request;
    request.definition = ParseOrThrow(yaml);
    request.parent_profile = MakeParentProfile();
    request.parent_tool_names = MakeParentTools();
    request.environment = MakeEnvironment();

    const agent::ResolvedAgentProfile resolved = agent::ResolveAgentProfile(request);
    CHECK_FALSE(resolved.ok());
    CHECK(HasCode(resolved.issues, "agent.missing_dependency"));  // run_command 不在有效面
    CHECK(HasCode(resolved.issues, "agent.missing_dependency"));  // ghost-server 未挂载
    CHECK(HasCode(resolved.issues, "agent.missing_dependency"));  // no-such-skill 不在清单
    // 有效面:allow 命中且未被 deny(read_file 与 mcp__browser__navigate)。
    REQUIRE(resolved.effective_tools.size() == 2);
    CHECK(resolved.effective_tools[0] == "read_file");
    CHECK(resolved.effective_tools[1] == "mcp__browser__navigate");

    // 依赖齐整的份:一条错都没有,工具谓词按 allow 收窄。
    agent::AgentProfileResolveRequest clean = request;
    clean.definition.requires_tools = {"read_file"};
    clean.definition.mcp_servers = {"browser"};
    clean.definition.skills_preload = {"browser-testing"};
    const agent::ResolvedAgentProfile ok_resolved = agent::ResolveAgentProfile(clean);
    CHECK(ok_resolved.ok());
    CHECK(ok_resolved.effective_tools.size() == 2);

    // 宿主没接环境账(旧调用方):技能/MCP 校验跳过,requires 仍查(工具
    // 面是 AgentTool 自持注册表折的,不靠环境)。
    agent::AgentProfileResolveRequest no_env = clean;
    no_env.environment.reset();
    const agent::ResolvedAgentProfile legacy = agent::ResolveAgentProfile(no_env);
    CHECK(legacy.ok());

    // allow 点到父面之外:不算授权,报 agent.tool_not_granted。
    agent::AgentProfileResolveRequest overreach = clean;
    overreach.definition.tools.allow = {"read_file", "mcp__ghost__fly"};
    const agent::ResolvedAgentProfile denied = agent::ResolveAgentProfile(overreach);
    CHECK_FALSE(denied.ok());
    CHECK(HasCode(denied.issues, "agent.tool_not_granted"));
}

TEST_CASE("effort:越档报 agent.effort_not_supported,档内在场则落到请求档案") {
    const std::string yaml =
        "schema: 1\n"
        "name: effort-probe\n"
        "description: 思考档探针。\n"
        "model:\n"
        "  effort: xhigh\n";
    agent::AgentProfileResolveRequest request;
    request.definition = ParseOrThrow(yaml);
    request.parent_profile = MakeParentProfile();
    request.parent_tool_names = MakeParentTools();
    request.environment = MakeEnvironment();  // 声明 low/medium/high

    agent::ResolvedAgentProfile resolved = agent::ResolveAgentProfile(request);
    CHECK_FALSE(resolved.ok());
    CHECK(HasCode(resolved.issues, "agent.effort_not_supported"));
    CHECK(resolved.profile.request.reasoning_effort == "medium");  // 父值不被污染

    request.environment->supported_efforts = {"low", "medium", "high", "xhigh"};
    resolved = agent::ResolveAgentProfile(request);
    CHECK(resolved.ok());
    CHECK(resolved.profile.request.reasoning_effort == "xhigh");

    // provider 未声明思考档(空表):不查,原样落。
    request.environment->supported_efforts = {};
    resolved = agent::ResolveAgentProfile(request);
    CHECK(resolved.ok());
    CHECK(resolved.profile.request.reasoning_effort == "xhigh");
}

// ---------------------------------------------------------------------------
// 模型角色:回落链照现有路由链
// ---------------------------------------------------------------------------

TEST_CASE("模型角色:inherit 照抄父;cheap/lao 按回落链;normal 未配置落父模型") {
    const auto resolve_role = [](const std::string& role, const agent::AgentProfileResolveEnvironment& env) {
        const std::string yaml =
            "schema: 1\nname: role-probe\ndescription: 角色探针。\nmodel:\n  role: " + role + "\n";
        agent::AgentProfileResolveRequest request;
        request.definition = ParseOrThrow(yaml);
        request.parent_profile = MakeParentProfile();  // 父 model-main @ prov-a
        request.parent_tool_names = MakeParentTools();
        request.environment = env;
        return agent::ResolveAgentProfile(request);
    };
    const agent::AgentProfileResolveEnvironment env = MakeEnvironment();

    agent::ResolvedAgentProfile resolved = resolve_role("inherit", env);
    CHECK(resolved.profile.provider == "prov-a");
    CHECK(resolved.profile.request.model == "model-main");

    resolved = resolve_role("cheap", env);
    CHECK(resolved.profile.provider == "prov-a");  // cheap 路由没写 provider,沿用父
    CHECK(resolved.profile.request.model == "model-cheap");

    resolved = resolve_role("lao", env);
    CHECK(resolved.profile.provider == "prov-lao");  // 高级段自带 provider
    CHECK(resolved.profile.request.model == "model-lao");

    // cheap 未配置 -> 回落 normal;normal 也未配置 -> 回落父模型。
    agent::AgentProfileResolveEnvironment bare;
    bare.role_lao = {"", "model-lao"};
    resolved = resolve_role("cheap", bare);
    CHECK(resolved.profile.request.model == "model-main");
    resolved = resolve_role("lao", bare);
    CHECK(resolved.profile.request.model == "model-lao");
    resolved = resolve_role("normal", bare);
    CHECK(resolved.profile.request.model == "model-main");
}

// ---------------------------------------------------------------------------
// 两路对账(验收线):同一份 YAML,AgentTool 路 == Workflow 路
// ---------------------------------------------------------------------------

TEST_CASE("对账:同一 Definition 从 AgentTool 与 Workflow 两条路解析,逐字段全等") {
    const std::string yaml = ReadFixture("agents/complete.yaml");
    REQUIRE_FALSE(yaml.empty());
    const agent::AgentDefinition definition = ParseOrThrow(yaml);
    REQUIRE(definition.name == "browser-tester");

    const agent::AgentProfile parent = MakeParentProfile();
    const agent::AgentProfileResolveEnvironment environment = MakeEnvironment();
    const std::vector<std::string> parent_tools = MakeParentTools();
    const int default_steps = 15;

    // AgentTool 路:execute() 用的同一口(BuildSubagentResolveRequest),
    // 连会话同步的窗口也一并给(0 = 用父皮值,与 Workflow 路同基线)。
    const agent::ResolvedAgentProfile from_agent_tool = agent::ResolveAgentProfile(
        agent::BuildSubagentResolveRequest(definition, parent, parent_tools, default_steps,
                                           /*default_max_turns=*/0,
                                           /*context_window_tokens=*/0, environment,
                                           agent::AgentDispatchOverrides{}));
    // Workflow 路:workflow_commands 装配 default binding 用的同一口
    //(BuildWorkflowAgentResolveRequest)。
    const agent::ResolvedAgentProfile from_workflow = agent::ResolveAgentProfile(
        agent::BuildWorkflowAgentResolveRequest(definition, parent, parent_tools, default_steps,
                                                /*default_max_turns=*/0, environment,
                                                agent::AgentDispatchOverrides{}));

    // 夹具的工具名(mcp__browser__screenshot 等)不在父面里,两条路都该
    // 报 agent.tool_not_granted——错也要错得一样。
    CHECK_FALSE(from_agent_tool.ok());
    CHECK_FALSE(from_workflow.ok());
    CheckProfilesIdentical(from_agent_tool, from_workflow);

    // 换一份依赖齐整的定义(父面放宽到夹具点名的工具),两条路再对一次,
    // 这次必须双双 ok 且全等。
    const std::vector<std::string> wide_parent_tools = {
        "read_file", "search", "context_read", "todo_write", "run_command",
        "mcp__browser__navigate", "mcp__browser__screenshot",
    };
    const agent::ResolvedAgentProfile clean_agent_tool = agent::ResolveAgentProfile(
        agent::BuildSubagentResolveRequest(definition, parent, wide_parent_tools, default_steps,
                                           /*default_max_turns=*/0,
                                           /*context_window_tokens=*/0, environment,
                                           agent::AgentDispatchOverrides{}));
    const agent::ResolvedAgentProfile clean_workflow = agent::ResolveAgentProfile(
        agent::BuildWorkflowAgentResolveRequest(definition, parent, wide_parent_tools, default_steps,
                                                /*default_max_turns=*/0, environment,
                                                agent::AgentDispatchOverrides{}));
    CHECK(clean_agent_tool.ok());
    CHECK(clean_workflow.ok());
    CheckProfilesIdentical(clean_agent_tool, clean_workflow);
    // 完整夹具的决议值抽几笔钉死(两路共用的那份)。
    CHECK(clean_agent_tool.profile.runtime.max_output_tokens == 8192);
    CHECK(clean_agent_tool.profile.runtime.max_steps_per_turn == 24);
    CHECK(clean_agent_tool.profile.runtime.length_continuations == 1);
    CHECK(clean_agent_tool.prompt_profile == "browser-tester");
    CHECK(clean_agent_tool.execution_mode == "auto");
    CHECK(clean_agent_tool.isolation == "none");
    CHECK(clean_agent_tool.permission == agent::AgentPermissionMode::Auto);  // inherit = 同父
    CHECK(clean_agent_tool.profile.request.model == "model-main");           // role: inherit
    // allow 六名全在宽父面里,deny 只点 run_command(不在 allow):六枚全放行。
    REQUIRE(clean_agent_tool.effective_tools.size() == 6);
}

TEST_CASE("对账:入参显式只影响步数一笔,两路同口径") {
    const agent::AgentDefinition definition = ParseOrThrow(BudgetYaml("  max_steps_per_turn: 24\n"));
    const agent::AgentProfile parent = MakeParentProfile();
    const std::vector<std::string> parent_tools = MakeParentTools();
    agent::AgentDispatchOverrides overrides;
    overrides.max_steps_per_turn = 7;

    const agent::ResolvedAgentProfile from_agent_tool = agent::ResolveAgentProfile(
        agent::BuildSubagentResolveRequest(definition, parent, parent_tools, 15, /*default_max_turns=*/0, 0,
                                           agent::AgentProfileResolveEnvironment{}, overrides));
    const agent::ResolvedAgentProfile from_workflow = agent::ResolveAgentProfile(
        agent::BuildWorkflowAgentResolveRequest(definition, parent, parent_tools, 15,
                                                /*default_max_turns=*/0,
                                                agent::AgentProfileResolveEnvironment{}, overrides));
    CHECK(from_agent_tool.profile.runtime.max_steps_per_turn == 7);  // 入参 > YAML 24 > 默认 15
    CheckProfilesIdentical(from_agent_tool, from_workflow);
}

// ---------------------------------------------------------------------------
// 任务总 turn 预算(turn 预算单 P0-3):三级合并、来源、只可收窄。
// ---------------------------------------------------------------------------

TEST_CASE("任务 turn 预算:override(收窄)> 定义 runtime.max_turns > 配置默认 > 0") {
    const agent::AgentProfile parent = MakeParentProfile();
    const std::vector<std::string> parent_tools = MakeParentTools();

    // 定义缺席 + 默认缺席 = 0(不限),来源 Default。
    const agent::AgentDefinition bare = ParseOrThrow(BudgetYaml("  max_output_tokens: 4096\n"));
    const agent::ResolvedAgentProfile unset = agent::ResolveAgentProfile(agent::BuildSubagentResolveRequest(
        bare, parent, parent_tools, 0, /*default_max_turns=*/0, 0, std::nullopt, {}));
    CHECK(unset.turn_budget.max_turns == 0);
    CHECK(unset.turn_budget.source == agent::TurnBudgetSource::Default);

    // 配置默认:subagent.default_max_turns 那一级。
    const agent::ResolvedAgentProfile config_level = agent::ResolveAgentProfile(
        agent::BuildSubagentResolveRequest(bare, parent, parent_tools, 0, /*default_max_turns=*/20, 0,
                                           std::nullopt, {}));
    CHECK(config_level.turn_budget.max_turns == 20);
    CHECK(config_level.turn_budget.source == agent::TurnBudgetSource::Config);

    // 定义 runtime.max_turns 压过配置默认。
    const agent::AgentDefinition with_turns = ParseOrThrow(BudgetYaml("  max_turns: 12\n"));
    const agent::ResolvedAgentProfile definition_level = agent::ResolveAgentProfile(
        agent::BuildSubagentResolveRequest(with_turns, parent, parent_tools, 0, /*default_max_turns=*/20, 0,
                                           std::nullopt, {}));
    CHECK(definition_level.turn_budget.max_turns == 12);
    CHECK(definition_level.turn_budget.source == agent::TurnBudgetSource::Definition);

    // 宿主 override 收窄:压过定义。
    agent::AgentDispatchOverrides narrowing;
    narrowing.max_turns = 5;
    const agent::ResolvedAgentProfile narrowed = agent::ResolveAgentProfile(
        agent::BuildSubagentResolveRequest(with_turns, parent, parent_tools, 0, 0, 0, std::nullopt, narrowing));
    CHECK(narrowed.turn_budget.max_turns == 5);
    CHECK(narrowed.turn_budget.source == agent::TurnBudgetSource::HostOverride);

    // 定义 0(不限)时宿主可给正数。
    agent::AgentDispatchOverrides set_on_unlimited;
    set_on_unlimited.max_turns = 8;
    const agent::ResolvedAgentProfile set_fresh = agent::ResolveAgentProfile(
        agent::BuildSubagentResolveRequest(bare, parent, parent_tools, 0, 0, 0, std::nullopt, set_on_unlimited));
    CHECK(set_fresh.turn_budget.max_turns == 8);
    CHECK(set_fresh.turn_budget.source == agent::TurnBudgetSource::HostOverride);
}

TEST_CASE("任务 turn 预算:override 放宽(变大或 0)报 agent.turn_budget_widening") {
    const agent::AgentProfile parent = MakeParentProfile();
    const std::vector<std::string> parent_tools = MakeParentTools();
    const agent::AgentDefinition with_turns = ParseOrThrow(BudgetYaml("  max_turns: 12\n"));

    // 改大:拒。
    agent::AgentDispatchOverrides bigger;
    bigger.max_turns = 30;
    const agent::ResolvedAgentProfile widened = agent::ResolveAgentProfile(
        agent::BuildSubagentResolveRequest(with_turns, parent, parent_tools, 0, 0, 0, std::nullopt, bigger));
    CHECK_FALSE(widened.ok());
    CHECK(HasCode(widened.issues, "agent.turn_budget_widening"));
    CHECK(widened.turn_budget.max_turns == 12);  // 诊断份仍带定义值

    // override=0(想抹成不限):同样算放宽,拒。
    agent::AgentDispatchOverrides unlimit;
    unlimit.max_turns = 0;
    const agent::ResolvedAgentProfile erased = agent::ResolveAgentProfile(
        agent::BuildSubagentResolveRequest(with_turns, parent, parent_tools, 0, 0, 0, std::nullopt, unlimit));
    CHECK_FALSE(erased.ok());
    CHECK(HasCode(erased.issues, "agent.turn_budget_widening"));
}

TEST_CASE("任务 turn 预算:legacy per-input step 的影子账(§10.2)随行,不混写") {
    const agent::AgentDefinition with_legacy = ParseOrThrow(BudgetYaml("  max_steps_per_turn: 24\n"));
    const agent::AgentProfile parent = MakeParentProfile();
    const std::vector<std::string> parent_tools = MakeParentTools();
    const agent::ResolvedAgentProfile resolved = agent::ResolveAgentProfile(
        agent::BuildSubagentResolveRequest(with_legacy, parent, parent_tools, 0, 0, 0, std::nullopt, {}));
    // legacy 字段照旧喂 per-run 步数(兼容窗旧义),新账是另一笔。
    REQUIRE(resolved.turn_budget.legacy_max_steps_per_input.has_value());
    CHECK(*resolved.turn_budget.legacy_max_steps_per_input == 24);
    CHECK(resolved.profile.runtime.max_steps_per_turn == 24);
    CHECK(resolved.turn_budget.max_turns == 0);
    CHECK(resolved.turn_budget.source == agent::TurnBudgetSource::Default);
}

TEST_CASE("对账:同一 Definition 的任务 turn 预算从 AgentTool 与 Workflow 两路解析全等") {
    const agent::AgentDefinition with_turns = ParseOrThrow(BudgetYaml("  max_turns: 12\n"));
    const agent::AgentProfile parent = MakeParentProfile();
    const std::vector<std::string> parent_tools = MakeParentTools();

    // 同一份宿主 override(Workflow 的节点 turn_limit 走的就是这只口)。
    agent::AgentDispatchOverrides overrides;
    overrides.max_turns = 6;
    const agent::ResolvedAgentProfile from_agent_tool = agent::ResolveAgentProfile(
        agent::BuildSubagentResolveRequest(with_turns, parent, parent_tools, 15, /*default_max_turns=*/20, 0,
                                           agent::AgentProfileResolveEnvironment{}, overrides));
    const agent::ResolvedAgentProfile from_workflow = agent::ResolveAgentProfile(
        agent::BuildWorkflowAgentResolveRequest(with_turns, parent, parent_tools, 15,
                                                /*default_max_turns=*/20,
                                                agent::AgentProfileResolveEnvironment{}, overrides));
    CHECK(from_agent_tool.turn_budget.max_turns == 6);
    CHECK(from_agent_tool.turn_budget.source == agent::TurnBudgetSource::HostOverride);
    CheckProfilesIdentical(from_agent_tool, from_workflow);
}
