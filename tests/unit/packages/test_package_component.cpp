// 统一 Package 封装单阶段 2 的组件册:ToolWireName 编码可逆、mcp.yaml 的
// 严格解析(含 ${package_dir} 越界)、六类 loader 走仓库夹具(full-stack 六
// 件全好;broken 各坏各的)、workflow parser 的 agent 字段。
#include <doctest/doctest.h>

#include <filesystem>
#include <fstream>
#include <string>

#include "package/component.hpp"
#include "runtime/plugin_contract.hpp"
#include "workflow/parser.hpp"

using namespace lubancode::package;
namespace fs = std::filesystem;

namespace {

const fs::path kFixturesRoot = fs::path(LUBANCODE_SOURCE_DIR) / "tests" / "fixtures" / "packages";

void WriteFile(const fs::path& path, const std::string& content) {
    fs::create_directories(path.parent_path());
    std::ofstream file(path, std::ios::binary);
    file << content;
}

ComponentSourceRoot MakeSource(const fs::path& package_root, ComponentKind kind,
                               const std::string& rel_path, const std::string& local_id,
                               const std::string& package_id) {
    ComponentSourceRoot source;
    source.package_root = package_root;
    source.component_path = package_root / rel_path;
    source.rel_path = rel_path;
    source.local_id = local_id;
    source.kind = kind;
    source.scope = PackageScope::Dev;
    source.package_id = package_id;
    return source;
}

}  // namespace

// ---------------------------------------------------------------------------
// ToolWireName(契约 §6.1:百分号编码,可逆,全仓一处)
// ---------------------------------------------------------------------------

TEST_CASE("wire-name:夹具钦定的两枚例子") {
    CHECK(lubancode::runtime::BuildPackagedToolWireName("plugin", "moontide.full-stack", "dom-analyzer",
                                             "inspect") ==
          "plugin__moontide%2Efull-stack%2Edom-analyzer__inspect");
    CHECK(lubancode::runtime::BuildPackagedToolWireName("mcp", "moontide.full-stack", "browser", "navigate") ==
          "mcp__moontide%2Efull-stack%2Ebrowser__navigate");
    CHECK(lubancode::runtime::BuildPackagedToolDisplayName("plugin", "moontide.full-stack", "dom-analyzer",
                                                "inspect") ==
          "plugin__moontide.full-stack.dom-analyzer__inspect");
}

TEST_CASE("wire-name:编码可逆,大写十六进制,合法字符不动") {
    const std::string id = "moontide.full-stack~凑";
    const std::string encoded = lubancode::runtime::EncodeToolWireId(id);
    CHECK(encoded.find("%2E") != std::string::npos);  // '.' -> %2E(大写)
    CHECK(encoded.find("%7E") != std::string::npos);  // '~' -> %7E
    const auto decoded = lubancode::runtime::DecodeToolWireId(encoded);
    REQUIRE(decoded.has_value());
    CHECK(*decoded == id);
    // 合法字符([A-Za-z0-9_-])原样过。
    CHECK(lubancode::runtime::EncodeToolWireId("abc-XYZ_9") == "abc-XYZ_9");
    // 坏编码不猜。
    CHECK_FALSE(lubancode::runtime::DecodeToolWireId("abc%2").has_value());
    CHECK_FALSE(lubancode::runtime::DecodeToolWireId("abc%zz").has_value());
}

// ---------------------------------------------------------------------------
// mcp.yaml
// ---------------------------------------------------------------------------

TEST_CASE("mcp-yaml:合法样例(仓库夹具原文)") {
    const std::string yaml = R"yaml(schema: 1
id: browser
description: 本地示例浏览器服务。
transport: stdio

runtime:
  command: node
  args:
    - "${package_dir}/mcp/browser/server.js"
  env:
    BROWSER_TOKEN: "${env:BROWSER_TOKEN}"
  timeout_ms: 30000

permissions:
  network: false
)yaml";
    const auto parsed = ParseMcpComponentYaml(yaml, fs::path("D:/pkg"));
    REQUIRE(parsed.has_value());
    CHECK(parsed->id == "browser");
    CHECK(parsed->transport == "stdio");
    CHECK(parsed->command == "node");
    CHECK(parsed->args.size() == 1);
    CHECK(parsed->env.size() == 1);
    CHECK(parsed->env[0].first == "BROWSER_TOKEN");
    CHECK(parsed->timeout_ms == 30000);
    CHECK_FALSE(parsed->network_allowed);
}

TEST_CASE("mcp-yaml:schema 非 1、transport 非 stdio、缺 command 都拒") {
    const auto base = std::string("description: d\ntransport: stdio\nruntime:\n  command: node\n");
    CHECK_FALSE(ParseMcpComponentYaml("schema: 2\n" + base, fs::path("D:/pkg")).has_value());
    CHECK_FALSE(ParseMcpComponentYaml("schema: 1\nid: a\ndescription: d\ntransport: http\nruntime:\n  command: node\n",
                                      fs::path("D:/pkg"))
                    .has_value());
    CHECK_FALSE(ParseMcpComponentYaml("schema: 1\nid: a\ndescription: d\ntransport: stdio\n",
                                      fs::path("D:/pkg"))
                    .has_value());
}

TEST_CASE("mcp-yaml:env 只许 ${env:NAME},明文与坏占位都报") {
    const auto errors_of = [](const std::string& yaml) {
        return ParseMcpComponentYaml(yaml, fs::path("D:/pkg"));
    };
    const std::string head = "schema: 1\nid: a\ndescription: d\ntransport: stdio\nruntime:\n  command: node\n";
    CHECK_FALSE(errors_of(head + "  env:\n    TOKEN: secret-value\n").has_value());
    CHECK_FALSE(errors_of(head + "  env:\n    TOKEN: \"${env:9bad}\"\n").has_value());
    CHECK_FALSE(errors_of(head + "  env:\n    TOKEN: \"${package_dir}\"\n").has_value());
    CHECK_FALSE(errors_of(head + "  args:\n    - \"${env:HOME}\"\n").has_value());  // env 占位不进 args
    CHECK_FALSE(errors_of(head + "  args:\n    - \"${project_dir}/x\"\n").has_value());
    CHECK(errors_of(head + "  env:\n    TOKEN: \"${env:GOOD_NAME}\"\n").has_value());
}

TEST_CASE("mcp-yaml:${package_dir} 展开后逃出包根即 path_escape") {
    const std::string yaml =
        "schema: 1\nid: leaky\ndescription: d\ntransport: stdio\nruntime:\n  command: node\n"
        "  args:\n    - \"${package_dir}/../../shared/server.js\"\n";
    const auto parsed = ParseMcpComponentYaml(yaml, fs::path("D:/pkg"));
    REQUIRE_FALSE(parsed.has_value());
    bool saw_escape = false;
    for (const auto& error : parsed.error()) {
        if (error.detail.find("path_escape") != std::string::npos) saw_escape = true;
    }
    CHECK(saw_escape);
    // 包内相对的 ${package_dir} 用法不报。
    const std::string ok_yaml =
        "schema: 1\nid: leaky\ndescription: d\ntransport: stdio\nruntime:\n  command: node\n"
        "  args:\n    - \"${package_dir}/mcp/leaky/server.js\"\n    - \"--flag=${package_data}/x\"\n";
    CHECK(ParseMcpComponentYaml(ok_yaml, fs::path("D:/pkg")).has_value());
}

// ---------------------------------------------------------------------------
// 六类 loader 走仓库夹具
// ---------------------------------------------------------------------------

TEST_CASE("component:full-stack 六件全好,原生产物都进了账") {
    const fs::path root = kFixturesRoot / "full-stack";
    const std::string pkg = "moontide.full-stack";

    const auto agent = ParsePackageComponent(
        MakeSource(root, ComponentKind::Agent, "agents/browser-tester.yaml", "browser-tester", pkg));
    REQUIRE(agent.agent.has_value());
    CHECK(agent.ok);
    CHECK(agent.issues.empty());
    CHECK(agent.agent->name == "browser-tester");
    REQUIRE(agent.agent->prompt.profile.has_value());
    CHECK(*agent.agent->prompt.profile == "browser-tester");
    CHECK(agent.agent->skills_preload == std::vector<std::string>{"browser-testing"});
    CHECK(agent.agent->mcp_servers == std::vector<std::string>{"browser"});

    const auto profile = ParsePackageComponent(
        MakeSource(root, ComponentKind::PromptProfile, "prompts/profiles/browser-tester",
                   "browser-tester", pkg));
    CHECK(profile.ok);
    CHECK(profile.profile_files.size() == 2);  // core/10-identity.md + features/web.md

    const auto skill = ParsePackageComponent(
        MakeSource(root, ComponentKind::Skill, "skills/browser-testing", "browser-testing", pkg));
    REQUIRE(skill.skill.has_value());
    CHECK(skill.ok);
    CHECK(*skill.skill->name == "browser-testing");

    const auto workflow = ParsePackageComponent(
        MakeSource(root, ComponentKind::Workflow, "workflows/smoke-test", "smoke-test", pkg));
    REQUIRE(workflow.workflow.has_value());
    CHECK(workflow.ok);
    CHECK(workflow.workflow->id == "smoke-test");

    const auto plugin = ParsePackageComponent(
        MakeSource(root, ComponentKind::Plugin, "plugins/dom-analyzer", "dom-analyzer", pkg));
    REQUIRE(plugin.plugin.has_value());
    CHECK(plugin.ok);
    REQUIRE(plugin.plugin->tools.size() == 1);
    CHECK(plugin.plugin->tools[0].name == "inspect");

    const auto mcp = ParsePackageComponent(
        MakeSource(root, ComponentKind::McpServer, "mcp/browser", "browser", pkg));
    REQUIRE(mcp.mcp.has_value());
    CHECK(mcp.ok);
    CHECK(mcp.mcp->id == "browser");
}

TEST_CASE("component:bad-names 六处坏各自落错,不互相吞") {
    const fs::path root = kFixturesRoot / "broken" / "bad-names";
    const std::string pkg = "moontide.bad-names";

    // agents/Browser-Tester.yaml:文件名大写 + name 带空格。
    const auto agent = ParsePackageComponent(
        MakeSource(root, ComponentKind::Agent, "agents/Browser-Tester.yaml", "Browser-Tester", pkg));
    CHECK_FALSE(agent.ok);
    REQUIRE(agent.issues.size() >= 2);
    bool saw_local_id = false, saw_bad_name = false;
    for (const auto& issue : agent.issues) {
        if (issue.message.find("id_invalid") != std::string::npos &&
            issue.message.find("Browser-Tester") != std::string::npos) {
            saw_local_id = true;
        }
        if (issue.message.find("browser tester") != std::string::npos) saw_bad_name = true;
    }
    CHECK(saw_local_id);
    CHECK(saw_bad_name);

    // skills/audit:frontmatter 名带下划线,且与目录不符。
    const auto skill = ParsePackageComponent(
        MakeSource(root, ComponentKind::Skill, "skills/audit", "audit", pkg));
    CHECK_FALSE(skill.ok);
    bool saw_invalid = false, saw_mismatch = false;
    for (const auto& issue : skill.issues) {
        if (issue.message.find("id_invalid") != std::string::npos) saw_invalid = true;
        if (issue.message.find("name_mismatch") != std::string::npos) saw_mismatch = true;
    }
    CHECK(saw_invalid);
    CHECK(saw_mismatch);

    // plugins/dom.analyzer:id 带点,原生 manifest 校验拒。
    const auto plugin = ParsePackageComponent(
        MakeSource(root, ComponentKind::Plugin, "plugins/dom.analyzer", "dom.analyzer", pkg));
    CHECK_FALSE(plugin.ok);
    CHECK_FALSE(plugin.plugin.has_value());

    // mcp/db:id 与目录不符(契约硬规矩:须一致)。
    const auto mcp = ParsePackageComponent(
        MakeSource(root, ComponentKind::McpServer, "mcp/db", "db", pkg));
    REQUIRE(mcp.mcp.has_value());  // yaml 本身合法,解析过了
    CHECK_FALSE(mcp.ok);           // 名不符按 error,组件不进 MountPlan
    bool saw_mcp_mismatch = false;
    for (const auto& issue : mcp.issues) {
        if (issue.message.find("name_mismatch") != std::string::npos) saw_mcp_mismatch = true;
    }
    CHECK(saw_mcp_mismatch);
}

TEST_CASE("component:path-escape 的 workflow 由原生 parser 报行号") {
    const fs::path root = kFixturesRoot / "broken" / "path-escape";
    const auto workflow = ParsePackageComponent(
        MakeSource(root, ComponentKind::Workflow, "workflows/checkout", "checkout",
                   "moontide.path-escape"));
    CHECK_FALSE(workflow.ok);
    CHECK_FALSE(workflow.issues.empty());
    // 诊断指到文件与字段(原生 parser 的位置透传)。
    const auto& issue = workflow.issues.front();
    CHECK(issue.path.find("workflows/checkout/workflow.yaml") != std::string::npos);
    CHECK(issue.message.find("越界") != std::string::npos);
}

// ---------------------------------------------------------------------------
// workflow parser 的 agent 字段(夹具钦定的包内短引用位)
// ---------------------------------------------------------------------------

TEST_CASE("workflow-parser:agent 节点的 agent 字段进 AST,round-trip 不丢") {
    const std::string yaml = R"yaml(schema_version: 1
id: mini
version: 1.0.0
name: mini
alias: mini
description: 测 agent 字段
enabled: true
entry: verify
nodes:
  verify:
    type: agent
    agent: browser-tester
    task: prompts/reporter.md
edges: []
result: {}
)yaml";
    const auto parsed = lubancode::workflow::ParseWorkflowYaml(yaml);
    REQUIRE(parsed.has_value());
    REQUIRE(parsed->nodes.size() == 1);
    CHECK(parsed->nodes[0].agent == "browser-tester");
    CHECK(parsed->nodes[0].task == "prompts/reporter.md");
    // 再解回(emit -> parse 保住字段)。
    const auto reparsed = lubancode::workflow::ParseWorkflowYaml(
        lubancode::workflow::EmitWorkflowYaml(*parsed));
    REQUIRE(reparsed.has_value());
    CHECK(reparsed->nodes[0].agent == "browser-tester");
}
