// 统一 Package 封装单阶段 2 的 catalog 册:AnalyzePackage 走仓库夹具——
// full-stack 出静态 MountPlan;broken 四只各自逐件指错且整包 invalid;短名/
// 全名引用解析规矩(本包优先、全名跨包须指向已存在包、悬空报结构化错);
// wire 名长度帽。测试账对齐单子 §十六"组件闭合"与"发现与覆盖"两节。
#include <doctest/doctest.h>

#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "package/catalog.hpp"

using namespace lubancode::package;
namespace fs = std::filesystem;

namespace {

const fs::path kFixturesRoot = fs::path(LUBANCODE_SOURCE_DIR) / "tests" / "fixtures" / "packages";

class TempDir {
public:
    TempDir() {
        dir_ = fs::temp_directory_path() /
               ("lubancode_pkg_catalog_" + std::to_string(reinterpret_cast<std::uintptr_t>(this)));
        std::error_code ec;
        fs::remove_all(dir_, ec);
        fs::create_directories(dir_, ec);
    }
    ~TempDir() {
        std::error_code ec;
        fs::remove_all(dir_, ec);
    }
    const fs::path& Get() const { return dir_; }

private:
    fs::path dir_;
};

void WriteFile(const fs::path& path, const std::string& content) {
    fs::create_directories(path.parent_path());
    std::ofstream file(path, std::ios::binary);
    file << content;
}

PackageCandidate DirectCandidate(const fs::path& root, const std::string& dir_name) {
    PackageCandidate candidate;
    candidate.scope = PackageScope::Dev;
    candidate.layer_root = root.parent_path();
    candidate.package_root = root;
    candidate.dir_name = dir_name;
    // manifest 让 AnalyzePackage 自己补读(BuildPackageInventory 的直诊口径)。
    return candidate;
}

const std::string kManifest = "schema: 1\nid: test.sample\nversion: 0.1.0\nname: S\ndescription: d.\n";

const std::string kSkillMd = "---\nname: helper\ndescription: 帮手。\n---\nbody";

const std::string kAgentYaml = R"yaml(schema: 1
name: tester
description: 试引用。
prompt:
  profile: tester-profile
skills:
  preload:
    - helper
mcp_servers:
    - local-server
)yaml";

const std::string kProfileMd = "# identity";

const std::string kMcpYaml =
    "schema: 1\nid: local-server\ndescription: 本地服务。\ntransport: stdio\nruntime:\n  command: node\n";

const std::string kPluginJson = R"json({
  "manifest_version": 1,
  "id": "toolkit",
  "version": "1.0.0",
  "language": "python",
  "runtime": {"kind": "process", "command": "python", "args": ["${plugin_dir}/runner.py"]},
  "tools": [{"name": "poke", "description": "戳一下", "input_schema": {"type": "object"}}],
  "permissions": {"network": false, "env": []}
})json";

// 一只五脏俱全、引用全闭合的样例包(id 可改)。
fs::path MakeClosedPackage(const fs::path& parent, const std::string& dir,
                           const std::string& id = "test.sample") {
    const fs::path root = parent / dir;
    WriteFile(root / "package.yaml",
              "schema: 1\nid: " + id + "\nversion: 0.1.0\nname: S\ndescription: d.\n");
    WriteFile(root / "agents" / "tester.yaml", kAgentYaml);
    WriteFile(root / "prompts" / "profiles" / "tester-profile" / "core" / "10-identity.md",
              kProfileMd);
    WriteFile(root / "skills" / "helper" / "SKILL.md", kSkillMd);
    WriteFile(root / "mcp" / "local-server" / "mcp.yaml", kMcpYaml);
    WriteFile(root / "plugins" / "toolkit" / "plugin.json", kPluginJson);
    WriteFile(root / "plugins" / "toolkit" / "runner.py", "print(1)");
    return root;
}

// 引用与组件的查找小助手。
const ComponentRef* FindRef(const PackageRecord& record, const std::string& field) {
    for (const auto& ref : record.references) {
        if (ref.field == field) return &ref;
    }
    return nullptr;
}

const ParsedComponent* FindComponent(const PackageRecord& record, ComponentKind kind) {
    for (const auto& component : record.components) {
        if (component.kind == kind) return &component;
    }
    return nullptr;
}

}  // namespace

// ---------------------------------------------------------------------------
// full-stack:完整包出静态 MountPlan
// ---------------------------------------------------------------------------

TEST_CASE("catalog:full-stack valid,六件进 MountPlan,wire 名对得上") {
    const PackageRecord record = AnalyzePackage(
        DirectCandidate(kFixturesRoot / "full-stack", "full-stack"), ScanOptions{}, PackageRefIndex{});
    CHECK(record.inventory.manifest_ok);
    CHECK(record.valid);
    REQUIRE(record.mount_plan.has_value());
    const auto& plan = *record.mount_plan;
    CHECK(plan.package_id == "moontide.full-stack");
    CHECK(plan.entries.size() == 6);
    CHECK(plan.CountKind(ComponentKind::Agent) == 1);
    CHECK(plan.CountKind(ComponentKind::PromptProfile) == 1);
    CHECK(plan.CountKind(ComponentKind::Skill) == 1);
    CHECK(plan.CountKind(ComponentKind::Workflow) == 1);
    CHECK(plan.CountKind(ComponentKind::Plugin) == 1);
    CHECK(plan.CountKind(ComponentKind::McpServer) == 1);
    CHECK(plan.HasCodeBearing());

    // plugin 工具的 wire 名与展示名(契约 §6.1 的正例)。
    const MountPlanEntry* plugin_entry = nullptr;
    for (const auto& entry : plan.entries) {
        if (entry.kind == ComponentKind::Plugin) plugin_entry = &entry;
    }
    REQUIRE(plugin_entry != nullptr);
    REQUIRE(plugin_entry->tools.size() == 1);
    CHECK(plugin_entry->tools[0].wire_name == "plugin__moontide%2Efull-stack%2Edom-analyzer__inspect");
    CHECK(plugin_entry->tools[0].display_name == "plugin__moontide.full-stack.dom-analyzer__inspect");
    CHECK(plugin_entry->wire_component_id == "moontide%2Efull-stack%2Edom-analyzer");

    // 引用账:agent 的 profile/skill/mcp 与 workflow 的 agent/task/tool 都在。
    const auto ref_count = [&](const std::string& field) {
        std::size_t count = 0;
        for (const auto& ref : record.references) {
            if (ref.field == field && ref.resolved) ++count;
        }
        return count;
    };
    CHECK(ref_count("prompt.profile") == 1);
    CHECK(ref_count("skills.preload[0]") == 1);
    CHECK(ref_count("mcp_servers[0]") == 1);
    CHECK(ref_count("nodes.verify.agent") == 1);
    CHECK(ref_count("nodes.verify.task") == 1);
    CHECK(ref_count("nodes.collect.tool") == 1);
    CHECK(ref_count("requires.tools[0]") == 1);
    CHECK(ref_count("requires.tools[1]") == 1);
    for (const auto& ref : record.references) {
        CHECK_MESSAGE(ref.resolved, "悬空引用: ", ref.Format());
    }

    // agent 依赖账:profile、skill、mcp 三件本包组件。
    const MountPlanEntry* agent_entry = nullptr;
    for (const auto& entry : plan.entries) {
        if (entry.kind == ComponentKind::Agent) agent_entry = &entry;
    }
    REQUIRE(agent_entry != nullptr);
    CHECK(agent_entry->depends_on.size() == 4);  // profile + skill + mcp + plugin(工具闭合)
}

// ---------------------------------------------------------------------------
// broken 四只:逐件指错,整包 invalid,无 MountPlan
// ---------------------------------------------------------------------------

TEST_CASE("catalog:bad-names 整包 invalid,六处坏逐件报") {
    const PackageRecord record =
        AnalyzePackage(DirectCandidate(kFixturesRoot / "broken" / "bad-names", "bad-names"),
                       ScanOptions{}, PackageRefIndex{});
    CHECK_FALSE(record.valid);
    CHECK_FALSE(record.mount_plan.has_value());
    // 组件逐件都在账上(不因第一个错停摆)。
    CHECK(record.components.size() == 4);  // agent + skill + plugin + mcp
    std::size_t broken = 0;
    for (const auto& component : record.components) {
        if (component.HasError()) ++broken;
    }
    CHECK(broken == 4);  // 四件全有错:agent 文件名、skill 名、plugin id、mcp 名不符
    CHECK(record.inventory.diagnostics.size() >= 1);  // 近似目录 skill/ 的 warning
}

TEST_CASE("catalog:path-escape 整包 invalid,MCP 越界与 workflow 越界都指到") {
    const PackageRecord record =
        AnalyzePackage(DirectCandidate(kFixturesRoot / "broken" / "path-escape", "path-escape"),
                       ScanOptions{}, PackageRefIndex{});
    CHECK_FALSE(record.valid);
    CHECK_FALSE(record.mount_plan.has_value());
    bool saw_mcp_escape = false, saw_workflow_escape = false;
    for (const auto& component : record.components) {
        for (const auto& issue : component.issues) {
            if (issue.message.find("path_escape") != std::string::npos &&
                component.kind == ComponentKind::McpServer) {
                saw_mcp_escape = true;
            }
            if (component.kind == ComponentKind::Workflow &&
                issue.message.find("越界") != std::string::npos) {
                saw_workflow_escape = true;
                CHECK(issue.path.find("workflows/checkout/workflow.yaml") != std::string::npos);
            }
        }
    }
    CHECK(saw_mcp_escape);
    CHECK(saw_workflow_escape);
}

TEST_CASE("catalog:missing-manifest 组件无辜,但整包 invalid") {
    const PackageRecord record =
        AnalyzePackage(DirectCandidate(kFixturesRoot / "broken" / "missing-manifest", "missing-manifest"),
                       ScanOptions{}, PackageRefIndex{});
    CHECK_FALSE(record.inventory.manifest_ok);
    CHECK_FALSE(record.valid);
    CHECK_FALSE(record.mount_plan.has_value());
    // 组件照诊:packer skill 在账上且是好的。
    const ParsedComponent* skill = FindComponent(record, ComponentKind::Skill);
    REQUIRE(skill != nullptr);
    CHECK(skill->ok);
}

TEST_CASE("catalog:invalid-manifest 清单有罪,组件照诊") {
    const PackageRecord record = AnalyzePackage(
        DirectCandidate(kFixturesRoot / "broken" / "invalid-manifest", "invalid-manifest"),
        ScanOptions{}, PackageRefIndex{});
    CHECK_FALSE(record.inventory.manifest_ok);
    CHECK_FALSE(record.valid);
    const ParsedComponent* skill = FindComponent(record, ComponentKind::Skill);
    REQUIRE(skill != nullptr);
    CHECK(skill->ok);  // 清单的罪不连累组件诊断
}

// ---------------------------------------------------------------------------
// 引用解析规矩
// ---------------------------------------------------------------------------

TEST_CASE("refs:包内短名先解到本包;外部命名空间兜底;都没有就悬空") {
    TempDir tmp;
    const fs::path root = MakeClosedPackage(tmp.Get(), "sample");
    const PackageCandidate candidate = DirectCandidate(root, "sample");

    SUBCASE("外部命名空间不给,mcp 短名解到本包") {
        const PackageRecord record = AnalyzePackage(candidate, ScanOptions{}, PackageRefIndex{});
        CHECK(record.valid);
        const auto* mcp_ref = FindRef(record, "mcp_servers[0]");
        REQUIRE(mcp_ref != nullptr);
        CHECK(mcp_ref->resolved);
        CHECK(mcp_ref->in_package);
        CHECK(mcp_ref->target == "test.sample:local-server");
    }

    SUBCASE("把 mcp 组件挪走,短名悬空且指明写全名") {
        std::error_code ec;
        fs::remove_all(root / "mcp", ec);
        const PackageRecord record = AnalyzePackage(candidate, ScanOptions{}, PackageRefIndex{});
        CHECK_FALSE(record.valid);
        const auto* mcp_ref = FindRef(record, "mcp_servers[0]");
        REQUIRE(mcp_ref != nullptr);
        CHECK_FALSE(mcp_ref->resolved);
        CHECK(mcp_ref->message.find("须写全名") != std::string::npos);
        // 悬空落回组件诊断,指到字段。
        const ParsedComponent* agent = FindComponent(record, ComponentKind::Agent);
        REQUIRE(agent != nullptr);
        bool saw_dangling = false;
        for (const auto& issue : agent->issues) {
            if (issue.field == "mcp_servers[0]" && issue.message.find("悬空") != std::string::npos) {
                saw_dangling = true;
            }
        }
        CHECK(saw_dangling);
    }

    SUBCASE("外部命名空间给了 config 服务名,短名解到包外") {
        std::error_code ec;
        fs::remove_all(root / "mcp", ec);
        ExternalNamespaces external;
        external.mcp_servers.insert("local-server");
        const PackageRecord record = AnalyzePackage(candidate, ScanOptions{}, PackageRefIndex{}, external);
        const auto* mcp_ref = FindRef(record, "mcp_servers[0]");
        REQUIRE(mcp_ref != nullptr);
        CHECK(mcp_ref->resolved);
        CHECK_FALSE(mcp_ref->in_package);
    }
}

TEST_CASE("refs:全名跨包引用须指向已存在包;指回本包也要真有") {
    TempDir tmp;
    const fs::path root = MakeClosedPackage(tmp.Get(), "sample");
    const PackageCandidate candidate = DirectCandidate(root, "sample");

    PackageRefIndex index;
    PackageComponentSet other;
    other.package_id = "other.lib";
    other.package_root = tmp.Get() / "other";
    other.skills.insert("helper");
    index.packages["other.lib"] = other;

    SUBCASE("全名指到已存在包的组件,解析成功") {
        WriteFile(root / "agents" / "tester.yaml",
                  "schema: 1\nname: tester\ndescription: d。\nskills:\n  preload:\n    - other.lib:helper\n");
        const PackageRecord record = AnalyzePackage(candidate, ScanOptions{}, index);
        CHECK(record.valid);
        const auto* ref = FindRef(record, "skills.preload[0]");
        REQUIRE(ref != nullptr);
        CHECK(ref->resolved);
        CHECK(ref->is_canonical);
        CHECK(ref->target == "other.lib:helper");
    }

    SUBCASE("全名指到不存在的包,悬空") {
        WriteFile(root / "agents" / "tester.yaml",
                  "schema: 1\nname: tester\ndescription: d。\nskills:\n  preload:\n    - ghost.pkg:helper\n");
        const PackageRecord record = AnalyzePackage(candidate, ScanOptions{}, index);
        CHECK_FALSE(record.valid);
        const auto* ref = FindRef(record, "skills.preload[0]");
        REQUIRE(ref != nullptr);
        CHECK_FALSE(ref->resolved);
        CHECK(ref->message.find("已存在包") != std::string::npos);
    }

    SUBCASE("全名指回本包但组件不在,悬空") {
        WriteFile(root / "agents" / "tester.yaml",
                  "schema: 1\nname: tester\ndescription: d。\nskills:\n  preload:\n    - test.sample:nosuch\n");
        const PackageRecord record = AnalyzePackage(candidate, ScanOptions{}, index);
        CHECK_FALSE(record.valid);
        const auto* ref = FindRef(record, "skills.preload[0]");
        REQUIRE(ref != nullptr);
        CHECK_FALSE(ref->resolved);
    }

    SUBCASE("全名指到存在包但没有那件组件,悬空") {
        WriteFile(root / "agents" / "tester.yaml",
                  "schema: 1\nname: tester\ndescription: d。\nskills:\n  preload:\n    - other.lib:nosuch\n");
        const PackageRecord record = AnalyzePackage(candidate, ScanOptions{}, index);
        CHECK_FALSE(record.valid);
        const auto* ref = FindRef(record, "skills.preload[0]");
        REQUIRE(ref != nullptr);
        CHECK_FALSE(ref->resolved);
    }
}

TEST_CASE("refs:workflow 的 task 文件引用缺文件即悬空") {
    TempDir tmp;
    const fs::path root = MakeClosedPackage(tmp.Get(), "sample");
    WriteFile(root / "workflows" / "run" / "workflow.yaml",
              "schema_version: 1\nid: run\nversion: 1.0.0\nname: run\nalias: run\n"
              "description: d\nenabled: true\nentry: verify\nnodes:\n  verify:\n    type: agent\n"
              "    agent: tester\n    task: prompts/gone.md\nedges: []\nresult: {}\n");
    const PackageCandidate candidate = DirectCandidate(root, "sample");
    const PackageRecord record = AnalyzePackage(candidate, ScanOptions{}, PackageRefIndex{});
    CHECK_FALSE(record.valid);
    const auto* ref = FindRef(record, "nodes.verify.task");
    REQUIRE(ref != nullptr);
    CHECK_FALSE(ref->resolved);
    CHECK(ref->is_file_ref);
}

TEST_CASE("refs:agent 的 plugin 工具 wire 名闭合与撞空") {
    TempDir tmp;
    const fs::path root = MakeClosedPackage(tmp.Get(), "sample");

    SUBCASE("工具在 manifest 里,闭合") {
        WriteFile(root / "agents" / "tester.yaml",
                  "schema: 1\nname: tester\ndescription: d。\ntools:\n  allow:\n    - "
                  "plugin__test%2Esample%2Etoolkit__poke\n");
        const PackageRecord record =
            AnalyzePackage(DirectCandidate(root, "sample"), ScanOptions{}, PackageRefIndex{});
        CHECK(record.valid);
        const auto* ref = FindRef(record, "tools.allow[0]");
        REQUIRE(ref != nullptr);
        CHECK(ref->resolved);
        CHECK(ref->in_package);
    }

    SUBCASE("工具不在 manifest 里,悬空点名") {
        WriteFile(root / "agents" / "tester.yaml",
                  "schema: 1\nname: tester\ndescription: d。\ntools:\n  allow:\n    - "
                  "plugin__test%2Esample%2Etoolkit__nope\n");
        const PackageRecord record =
            AnalyzePackage(DirectCandidate(root, "sample"), ScanOptions{}, PackageRefIndex{});
        CHECK_FALSE(record.valid);
        const auto* ref = FindRef(record, "tools.allow[0]");
        REQUIRE(ref != nullptr);
        CHECK_FALSE(ref->resolved);
        CHECK(ref->message.find("没有工具") != std::string::npos);
    }

    SUBCASE("编码坏的 wire 名报结构化错") {
        WriteFile(root / "agents" / "tester.yaml",
                  "schema: 1\nname: tester\ndescription: d。\ntools:\n  allow:\n    - "
                  "plugin__test%zz%2Esample__poke\n");
        const PackageRecord record =
            AnalyzePackage(DirectCandidate(root, "sample"), ScanOptions{}, PackageRefIndex{});
        CHECK_FALSE(record.valid);
        const auto* ref = FindRef(record, "tools.allow[0]");
        REQUIRE(ref != nullptr);
        CHECK_FALSE(ref->resolved);
    }
}

TEST_CASE("wire-budget:package id 太长,工具名超 64 字符帽即 invalid") {
    TempDir tmp;
    // 60 字符的包 id:%2E 三枚,编码后必超帽。
    const std::string long_id = "a." + std::string(56, 'b') + "-c";
    const fs::path root = MakeClosedPackage(tmp.Get(), "sample", long_id);
    const PackageRecord record =
        AnalyzePackage(DirectCandidate(root, "sample"), ScanOptions{}, PackageRefIndex{});
    CHECK_FALSE(record.valid);
    CHECK_FALSE(record.mount_plan.has_value());
    const ParsedComponent* plugin = FindComponent(record, ComponentKind::Plugin);
    REQUIRE(plugin != nullptr);
    bool saw_budget = false;
    for (const auto& issue : plugin->issues) {
        if (issue.message.find("64 字符帽") != std::string::npos) saw_budget = true;
    }
    CHECK(saw_budget);
}

TEST_CASE("profile:modes 不可换,core/features/platforms 可换") {
    TempDir tmp;
    const fs::path root = MakeClosedPackage(tmp.Get(), "sample");
    WriteFile(root / "prompts" / "profiles" / "tester-profile" / "modes" / "default.md", "mode");
    const PackageRecord record =
        AnalyzePackage(DirectCandidate(root, "sample"), ScanOptions{}, PackageRefIndex{});
    CHECK_FALSE(record.valid);
    const ParsedComponent* profile = FindComponent(record, ComponentKind::PromptProfile);
    REQUIRE(profile != nullptr);
    bool saw_modes = false;
    for (const auto& issue : profile->issues) {
        if (issue.message.find("modes") != std::string::npos) saw_modes = true;
    }
    CHECK(saw_modes);
}

// ---------------------------------------------------------------------------
// BuildPackageRefIndex:轻扫候选,清单坏的包不进账
// ---------------------------------------------------------------------------

TEST_CASE("ref-index:从候选建,遮蔽取高优先级,坏清单不进") {
    TempDir tmp;
    ScanOptions options;
    options.dev_roots.push_back(tmp.Get() / "layer-dev");
    options.user_root = tmp.Get() / "layer-user";
    WriteFile(tmp.Get() / "layer-dev" / "sample" / "package.yaml",
              "schema: 1\nid: test.sample\nversion: 0.3.0\nname: D\ndescription: d.\n");
    WriteFile(tmp.Get() / "layer-dev" / "sample" / "skills" / "helper" / "SKILL.md", kSkillMd);
    WriteFile(tmp.Get() / "layer-user" / "sample" / "package.yaml",
              "schema: 1\nid: test.sample\nversion: 0.2.0\nname: U\ndescription: u.\n");
    WriteFile(tmp.Get() / "layer-user" / "sample" / "skills" / "helper" / "SKILL.md", kSkillMd);
    WriteFile(tmp.Get() / "layer-user" / "broken" / "package.yaml", "schema: 9\n");

    const auto candidates = ScanPackages(options);
    const PackageRefIndex index = BuildPackageRefIndex(candidates);
    REQUIRE(index.packages.count("test.sample") == 1);
    CHECK(index.packages.at("test.sample").package_root == tmp.Get() / "layer-dev" / "sample");
    CHECK(index.packages.count("broken") == 0);  // 清单解析失败,不进引用账
}
