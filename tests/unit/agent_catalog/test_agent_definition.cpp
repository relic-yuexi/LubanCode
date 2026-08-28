// AgentDefinition 严格解析的单测(自定义 Agent 单阶段 1)。覆盖单子测试账
// "解析与加载"的前半:合法(完整/最小)、缺必填、未知字段、错类型、错
// schema、枚举认不得、名字不合词法、数组去重保序、YAML 语法坏。夹具样本
// (tests/fixtures/agent_catalog/)在 test_agent_catalog.cpp 那册走真文件,
// 这里全用内联文本,错误定位好钉。

#include <doctest/doctest.h>

#include <fstream>
#include <sstream>
#include <string>

#include "agent/agent_definition.hpp"

#ifndef LUBANCODE_TEST_FIXTURES_DIR
#define LUBANCODE_TEST_FIXTURES_DIR "tests/fixtures"
#endif

using namespace lubancode;

namespace {

// 数一数 issues 里的 error(非 warning)。
int ErrorCount(const agent::AgentDefinitionParseResult& result) {
    int count = 0;
    for (const auto& issue : result.issues) {
        if (!issue.warning) {
            ++count;
        }
    }
    return count;
}

// 有没有哪条 error/warning 落在指定字段上。
bool HasIssueOn(const agent::AgentDefinitionParseResult& result, const std::string& field) {
    for (const auto& issue : result.issues) {
        if (issue.field == field) {
            return true;
        }
    }
    return false;
}

}  // namespace

TEST_CASE("完整合法 YAML:逐字段解析对账(todo 4.1 的样例)") {
    const std::string yaml = R"yaml(schema: 1
name: browser-tester
description: Inspect a web application, reproduce UI failures, and report evidence.

prompt:
  profile: browser-tester
  project_instructions: inherit
  soul: inherit

model:
  role: inherit
  effort: inherit

skills:
  preload:
    - browser-testing

tools:
  allow:
    - context_read
    - todo_read
  deny:
    - shell

mcp_servers:
  - browser

requires:
  tools:
    - mcp__browser__navigate

runtime:
  max_output_tokens: 8192
  max_steps_per_turn: 24
  max_context_chars: 600000
  context_window_tokens: 0
  length_continuations: 1
  execution_mode: auto
  isolation: none

permissions:
  mode: confirm
)yaml";
    const auto result = agent::ParseAgentDefinitionYaml(yaml, "browser-tester.yaml");
    REQUIRE(result.definition.has_value());
    CHECK(result.issues.empty());
    const auto& def = *result.definition;
    CHECK(def.schema == 1);
    CHECK(def.name == "browser-tester");
    CHECK(def.description.rfind("Inspect a web application", 0) == 0);
    REQUIRE(def.prompt.profile.has_value());
    CHECK(*def.prompt.profile == "browser-tester");
    CHECK(def.prompt.project_instructions == agent::AgentPromptSpec::ProjectInstructions::Inherit);
    CHECK(def.prompt.soul == agent::AgentPromptSpec::Soul::Inherit);
    CHECK(def.model.role == "inherit");
    CHECK(def.model.effort == "inherit");
    CHECK(def.skills_preload == std::vector<std::string>{"browser-testing"});
    CHECK(def.tools.allow == std::vector<std::string>{"context_read", "todo_read"});
    CHECK(def.tools.deny == std::vector<std::string>{"shell"});
    CHECK(def.mcp_servers == std::vector<std::string>{"browser"});
    CHECK(def.requires_tools == std::vector<std::string>{"mcp__browser__navigate"});
    // 契约 4.8:五个预算字段与 AgentRuntimeProfile 同名同义,逐一对账。
    REQUIRE(def.max_output_tokens.has_value());
    CHECK(*def.max_output_tokens == 8192);
    REQUIRE(def.max_steps_per_turn.has_value());
    CHECK(*def.max_steps_per_turn == 24);
    REQUIRE(def.max_context_chars.has_value());
    CHECK(*def.max_context_chars == 600000);
    REQUIRE(def.context_window_tokens.has_value());
    CHECK(*def.context_window_tokens == 0);  // 0 = 未知,合法值
    REQUIRE(def.length_continuations.has_value());
    CHECK(*def.length_continuations == 1);
    CHECK(def.execution_mode == "auto");
    CHECK(def.isolation == "none");
    CHECK(def.permissions_mode == "confirm");
}

TEST_CASE("最小合法 YAML:三样必填之外全走默认(继承)") {
    const std::string yaml = "schema: 1\nname: minimal-agent\ndescription: 干杂活。\n";
    const auto result = agent::ParseAgentDefinitionYaml(yaml, "minimal.yaml");
    REQUIRE(result.definition.has_value());
    CHECK(result.issues.empty());
    const auto& def = *result.definition;
    CHECK_FALSE(def.prompt.profile.has_value());
    CHECK(def.prompt.project_instructions == agent::AgentPromptSpec::ProjectInstructions::Inherit);
    CHECK(def.prompt.soul == agent::AgentPromptSpec::Soul::Inherit);
    CHECK(def.model.role.empty());
    CHECK(def.model.effort.empty());
    CHECK(def.skills_preload.empty());
    CHECK(def.tools.allow.empty());
    CHECK(def.tools.deny.empty());
    CHECK(def.mcp_servers.empty());
    CHECK(def.requires_tools.empty());
    CHECK_FALSE(def.max_output_tokens.has_value());
    CHECK_FALSE(def.max_steps_per_turn.has_value());
    CHECK_FALSE(def.max_context_chars.has_value());
    CHECK_FALSE(def.context_window_tokens.has_value());
    CHECK_FALSE(def.length_continuations.has_value());
    CHECK(def.execution_mode.empty());  // 空 = auto,由 resolver 落默认
    CHECK(def.isolation.empty());
    CHECK(def.permissions_mode.empty());
}

TEST_CASE("缺必填:schema/name/description 各缺一回,都报 error 且不交定义") {
    const auto missing = [](const std::string& yaml) {
        const auto result = agent::ParseAgentDefinitionYaml(yaml, "x.yaml");
        CHECK_FALSE(result.definition.has_value());
        CHECK(ErrorCount(result) >= 1);
        return result;
    };
    CHECK(HasIssueOn(missing("name: a\ndescription: d\n"), "schema"));
    CHECK(HasIssueOn(missing("schema: 1\ndescription: d\n"), "name"));
    CHECK(HasIssueOn(missing("schema: 1\nname: a\n"), "description"));
}

TEST_CASE("未知字段:顶层与嵌套各报一处,错误指到字段与行列") {
    const std::string yaml = R"yaml(schema: 1
name: probe
description: d
persona: 你是一枚子代理
prompt:
  soul: off
  temperature: 0.2
)yaml";
    const auto result = agent::ParseAgentDefinitionYaml(yaml, "probe.yaml");
    CHECK_FALSE(result.definition.has_value());
    CHECK(HasIssueOn(result, "persona"));
    CHECK(HasIssueOn(result, "prompt.temperature"));
    bool has_line = false;
    for (const auto& issue : result.issues) {
        if (issue.field == "persona") {
            // "persona" 在第 4 行;yaml-cpp 的 mark 0 起,解析器 +1。
            CHECK(issue.line == 4);
            CHECK(issue.column > 0);
            CHECK(issue.message.find("未知字段") != std::string::npos);
            has_line = true;
        }
    }
    CHECK(has_line);
}

TEST_CASE("类型错:runtime.max_steps_per_turn 给字符串,报行列") {
    const std::string yaml =
        "schema: 1\nname: probe\ndescription: d\nruntime:\n  max_steps_per_turn: 很多步\n";
    const auto result = agent::ParseAgentDefinitionYaml(yaml, "probe.yaml");
    CHECK_FALSE(result.definition.has_value());
    REQUIRE(HasIssueOn(result, "runtime.max_steps_per_turn"));
    for (const auto& issue : result.issues) {
        if (issue.field == "runtime.max_steps_per_turn") {
            CHECK(issue.line == 5);
            CHECK(issue.column > 0);
        }
    }
}

TEST_CASE("runtime 五预算键(契约 4.8):正例与 tests/fixtures/agents/complete.yaml 同款") {
    // 与夹具同款的 runtime 段全键过一遍——名字与 AgentRuntimeProfile 一字不差。
    const std::string yaml = R"yaml(schema: 1
name: budget-probe
description: d
runtime:
  max_output_tokens: 4096
  max_steps_per_turn: 0
  max_context_chars: 200000
  context_window_tokens: 256000
  length_continuations: 0
)yaml";
    const auto result = agent::ParseAgentDefinitionYaml(yaml, "budget-probe.yaml");
    REQUIRE(result.definition.has_value());
    CHECK(result.issues.empty());
    const auto& def = *result.definition;
    REQUIRE(def.max_output_tokens.has_value());
    CHECK(*def.max_output_tokens == 4096);
    REQUIRE(def.max_steps_per_turn.has_value());
    CHECK(*def.max_steps_per_turn == 0);  // 0 = 不限步,合法
    REQUIRE(def.max_context_chars.has_value());
    CHECK(*def.max_context_chars == 200000);
    REQUIRE(def.context_window_tokens.has_value());
    CHECK(*def.context_window_tokens == 256000);
    REQUIRE(def.length_continuations.has_value());
    CHECK(*def.length_continuations == 0);  // 0 = 不续跑,合法

    // tests/fixtures/agents/complete.yaml 本尊也过一遍(夹具即正例)。
    std::ifstream fixture(std::string(LUBANCODE_TEST_FIXTURES_DIR) + "/agents/complete.yaml",
                          std::ios::binary);
    REQUIRE_MESSAGE(fixture.is_open(), "夹具 tests/fixtures/agents/complete.yaml 打不开");
    std::stringstream buffer;
    buffer << fixture.rdbuf();
    const auto from_file = agent::ParseAgentDefinitionYaml(buffer.str(), "complete.yaml");
    REQUIRE(from_file.definition.has_value());
    CHECK(from_file.issues.empty());
    REQUIRE(from_file.definition->max_output_tokens.has_value());
    CHECK(*from_file.definition->max_output_tokens == 8192);
    REQUIRE(from_file.definition->max_context_chars.has_value());
    CHECK(*from_file.definition->max_context_chars == 600000);
    REQUIRE(from_file.definition->context_window_tokens.has_value());
    CHECK(*from_file.definition->context_window_tokens == 0);
    REQUIRE(from_file.definition->length_continuations.has_value());
    CHECK(*from_file.definition->length_continuations == 1);
    REQUIRE(from_file.definition->max_steps_per_turn.has_value());
    CHECK(*from_file.definition->max_steps_per_turn == 24);
}

TEST_CASE("runtime 五预算键:下界与类型各报一处,行列指得到") {
    // max_output_tokens 要正整数:0 报错(契约 4.8"正整数")。
    CHECK(HasIssueOn(agent::ParseAgentDefinitionYaml(
                         "schema: 1\nname: a\ndescription: d\nruntime:\n  max_output_tokens: 0\n", "a.yaml"),
                     "runtime.max_output_tokens"));
    // max_context_chars 要正整数:0 报错。
    CHECK(HasIssueOn(agent::ParseAgentDefinitionYaml(
                         "schema: 1\nname: a\ndescription: d\nruntime:\n  max_context_chars: 0\n", "a.yaml"),
                     "runtime.max_context_chars"));
    // 负数:stoull 不收负号,按类型错报。
    CHECK(HasIssueOn(agent::ParseAgentDefinitionYaml(
                         "schema: 1\nname: a\ndescription: d\nruntime:\n  length_continuations: -1\n", "a.yaml"),
                     "runtime.length_continuations"));
    // 字符串类型错,行列指到那一行(第 5 行)。
    const auto typed = agent::ParseAgentDefinitionYaml(
        "schema: 1\nname: a\ndescription: d\nruntime:\n  context_window_tokens: 很大\n", "a.yaml");
    CHECK_FALSE(typed.definition.has_value());
    REQUIRE(HasIssueOn(typed, "runtime.context_window_tokens"));
    for (const auto& issue : typed.issues) {
        if (issue.field == "runtime.context_window_tokens") {
            CHECK(issue.line == 5);
            CHECK(issue.column > 0);
        }
    }
    // 超上限(size_t 键的 1 TiB 帽):写一串离谱的数,报"超上限"不截断装小。
    const auto overflow = agent::ParseAgentDefinitionYaml(
        "schema: 1\nname: a\ndescription: d\nruntime:\n  max_context_chars: 9999999999999999\n", "a.yaml");
    CHECK_FALSE(overflow.definition.has_value());
    REQUIRE(HasIssueOn(overflow, "runtime.max_context_chars"));
    for (const auto& issue : overflow.issues) {
        if (issue.field == "runtime.max_context_chars") {
            CHECK(issue.message.find("超上限") != std::string::npos);
        }
    }
}

TEST_CASE("类型错:prompt 不是映射、mcp_servers 项是映射(内联 MCP 拒收)") {
    SUBCASE("prompt 是标量") {
        const auto result = agent::ParseAgentDefinitionYaml(
            "schema: 1\nname: a\ndescription: d\nprompt: browser\n", "a.yaml");
        CHECK_FALSE(result.definition.has_value());
        CHECK(HasIssueOn(result, "prompt"));
    }
    SUBCASE("mcp_servers 项内联 command") {
        const std::string yaml = R"yaml(schema: 1
name: a
description: d
mcp_servers:
  - command: npx something
)yaml";
        const auto result = agent::ParseAgentDefinitionYaml(yaml, "a.yaml");
        CHECK_FALSE(result.definition.has_value());
        CHECK(HasIssueOn(result, "mcp_servers"));
    }
}

TEST_CASE("错 schema:写 2 拒载并提示升级") {
    const auto result = agent::ParseAgentDefinitionYaml("schema: 2\nname: a\ndescription: d\n", "a.yaml");
    CHECK_FALSE(result.definition.has_value());
    REQUIRE(HasIssueOn(result, "schema"));
    for (const auto& issue : result.issues) {
        if (issue.field == "schema") {
            CHECK(issue.message.find("升级") != std::string::npos);
        }
    }
}

TEST_CASE("枚举认不得:prompt.soul=on 报错并列出只收的值") {
    const auto result =
        agent::ParseAgentDefinitionYaml("schema: 1\nname: a\ndescription: d\nprompt:\n  soul: on\n", "a.yaml");
    CHECK_FALSE(result.definition.has_value());
    REQUIRE(HasIssueOn(result, "prompt.soul"));
    for (const auto& issue : result.issues) {
        if (issue.field == "prompt.soul") {
            CHECK(issue.message.find("inherit") != std::string::npos);
            CHECK(issue.message.find("off") != std::string::npos);
        }
    }
}

TEST_CASE("permissions.mode:契约四值各过,read_only 报错并指路 tools.allow") {
    // 契约 4.9:mode 只认 inherit/confirm/auto/yolo,四值各解析成自己。
    for (const char* mode : {"inherit", "confirm", "auto", "yolo"}) {
        const std::string yaml =
            std::string("schema: 1\nname: a\ndescription: d\npermissions:\n  mode: ") + mode + "\n";
        const auto result = agent::ParseAgentDefinitionYaml(yaml, "a.yaml");
        REQUIRE(result.definition.has_value());
        CHECK(result.issues.empty());
        CHECK(result.definition->permissions_mode == mode);
    }

    // read_only 不进首版:报 agent.bad_enum 一路的错,文案点名 tools.allow。
    const auto rejected = agent::ParseAgentDefinitionYaml(
        "schema: 1\nname: a\ndescription: d\npermissions:\n  mode: read_only\n", "a.yaml");
    CHECK_FALSE(rejected.definition.has_value());
    REQUIRE(HasIssueOn(rejected, "permissions.mode"));
    for (const auto& issue : rejected.issues) {
        if (issue.field == "permissions.mode") {
            CHECK(issue.line == 5);
            CHECK(issue.message.find("read_only") != std::string::npos);
            CHECK(issue.message.find("tools.allow") != std::string::npos);
        }
    }
}

TEST_CASE("名字词法:大写/下划线/横线连写都拒,kebab-case 过") {
    CHECK(agent::IsValidAgentName("browser-tester"));
    CHECK(agent::IsValidAgentName("explore"));
    CHECK(agent::IsValidAgentName("a1"));
    CHECK_FALSE(agent::IsValidAgentName("BrowserTester"));
    CHECK_FALSE(agent::IsValidAgentName("browser_tester"));
    CHECK_FALSE(agent::IsValidAgentName("-browser"));
    CHECK_FALSE(agent::IsValidAgentName("browser-"));
    CHECK_FALSE(agent::IsValidAgentName("browser--tester"));
    CHECK_FALSE(agent::IsValidAgentName(""));
    const auto result = agent::ParseAgentDefinitionYaml("schema: 1\nname: Browser_Tester\ndescription: d\n",
                                                        "a.yaml");
    CHECK_FALSE(result.definition.has_value());
    CHECK(HasIssueOn(result, "name"));
}

TEST_CASE("数组去重保序:allow/preload/mcp_servers 的重复项只留首见") {
    const std::string yaml = R"yaml(schema: 1
name: a
description: d
tools:
  allow:
    - read_file
    - search
    - read_file
mcp_servers:
  - browser
  - browser
)yaml";
    const auto result = agent::ParseAgentDefinitionYaml(yaml, "a.yaml");
    REQUIRE(result.definition.has_value());
    CHECK(result.definition->tools.allow == std::vector<std::string>{"read_file", "search"});
    CHECK(result.definition->mcp_servers == std::vector<std::string>{"browser"});
    // 纯函数也钉一遍:次序是"首见次序",不是字典序。
    CHECK(agent::DedupePreserveOrder({"c", "a", "c", "b", "a"}) ==
          std::vector<std::string>{"c", "a", "b"});
}

TEST_CASE("YAML 语法坏与空文件:各折成一条 error,不交定义") {
    const auto broken = agent::ParseAgentDefinitionYaml("schema: [1\nname: a\n", "a.yaml");
    CHECK_FALSE(broken.definition.has_value());
    REQUIRE(broken.issues.size() == 1);
    CHECK(broken.issues[0].message.find("YAML 语法解析失败") != std::string::npos);

    const auto empty = agent::ParseAgentDefinitionYaml("", "a.yaml");
    CHECK_FALSE(empty.definition.has_value());
    REQUIRE_FALSE(empty.issues.empty());
}

TEST_CASE("诊断行格式:file:line:col `field`: message") {
    const auto result = agent::ParseAgentDefinitionYaml(
        "schema: 1\nname: a\ndescription: d\nruntime:\n  execution_mode: someday\n", "a.yaml");
    REQUIRE_FALSE(result.issues.empty());
    const std::string line = result.issues[0].Format("agents/a.yaml");
    CHECK(line.find("agents/a.yaml:5:") == 0);
    CHECK(line.find("`runtime.execution_mode`") != std::string::npos);
}
