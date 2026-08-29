// 统一 Package 封装单阶段 6 的快照与 reload 册:PackageSnapshot 的真身
//(显式快照对象:挂载账 + 钉住的信任账 + 启停账 + 技能正文查表)、原子
// reload(折好才换:对账行增/减/改、坏包回执诊断、code 组件须新会话)、
// 目录突变隔离(删包/改 SKILL——在跑引用读快照照旧,新装配查不着)、
// 半场 Workflow 不换 Skill(解析钉旧折)与 ToolRegistry 引用模块不被
// reload 卸掉。测试账对齐单子 §12.3"会话钉快照"与阶段 6 验收线。
#include <doctest/doctest.h>

#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "agent/agent_catalog.hpp"
#include "app/tool_runtime.hpp"  // ResolveCustomAgentMaterial(agent 节点解析钉快照)
#include "package/catalog.hpp"
#include "package/inventory.hpp"
#include "package/mounting.hpp"
#include "runtime/plugin_contract.hpp"
#include "tools/skill_loader.hpp"
#include "workflow/catalog.hpp"

using namespace lubancode::package;
namespace fs = std::filesystem;

namespace {

class TempDir {
public:
    TempDir() {
        dir_ = fs::temp_directory_path() /
               ("lubancode_pkg_reload_" + std::to_string(reinterpret_cast<std::uintptr_t>(this)));
        std::error_code ec;
        fs::remove_all(dir_, ec);
        fs::create_directories(dir_, ec);
    }
    ~TempDir() {
        std::error_code ec;
        fs::remove_all(dir_, ec);
    }
    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;
    const fs::path& Get() const { return dir_; }

private:
    fs::path dir_;
};

void WriteFile(const fs::path& path, const std::string& content) {
    fs::create_directories(path.parent_path());
    std::ofstream file(path, std::ios::binary);
    file << content;
}

const char* kAgentYaml = R"yaml(schema: 1
name: reviewer
description: 复核员。
skills:
  preload:
    - checklist
)yaml";

// 一只 content-only 包:Agent(预装技能)+ Skill,引用闭合。
fs::path MakeContentPackage(const fs::path& parent, const std::string& dir,
                            const std::string& id = "test.content-kit",
                            const std::string& skill_body = "照单核。") {
    const fs::path root = parent / dir;
    WriteFile(root / "package.yaml",
              "schema: 1\nid: " + id + "\nversion: 0.1.0\nname: Content Kit\n"
              "description: 内容包测试件。\n");
    WriteFile(root / "agents" / "reviewer.yaml", kAgentYaml);
    WriteFile(root / "skills" / "checklist" / "SKILL.md",
              "---\nname: checklist\ndescription: 核对章法。\n---\n" + skill_body);
    return root;
}

// 一只带 process 插件的 code-bearing 包(静态解析即可,插件不起不跑)。
const char* kPluginJson = R"json({
  "manifest_version": 1,
  "id": "count-words",
  "version": "1.0.0",
  "language": "python",
  "runtime": {
    "kind": "process",
    "command": "python",
    "args": ["${plugin_dir}/runner.py"],
    "timeout_ms": 15000
  },
  "tools": [
    {
      "name": "count",
      "description": "数词数。纯本地统计。",
      "input_schema": {
        "type": "object",
        "properties": {"text": {"type": "string"}},
        "required": ["text"]
      }
    }
  ],
  "permissions": {"network": false, "env": []}
})json";

fs::path MakeCodePackage(const fs::path& parent, const std::string& dir,
                         const std::string& id = "test.code-kit") {
    const fs::path root = parent / dir;
    WriteFile(root / "package.yaml",
              "schema: 1\nid: " + id + "\nversion: 0.1.0\nname: Code Kit\n"
              "description: 代码包测试件。\n");
    WriteFile(root / "plugins" / "count-words" / "plugin.json", kPluginJson);
    WriteFile(root / "plugins" / "count-words" / "runner.py", "print('{}')\n");
    return root;
}

PackageMountInput DevInput(const fs::path& layer_root) {
    PackageMountInput input;
    input.scan.dev_roots.push_back(layer_root);
    return input;
}

std::string JoinLines(const std::vector<std::string>& lines) {
    std::string out;
    for (const auto& line : lines) out += line + "\n";
    return out;
}

}  // namespace

// ---------------------------------------------------------------------------
// PackageSnapshot 的真身
// ---------------------------------------------------------------------------

TEST_CASE("BuildPackageSnapshot:挂载账 + 钉住的账 + 技能正文查表") {
    TempDir layers;
    const fs::path dev_layer = layers.Get() / "dev-packages";
    fs::create_directories(dev_layer);
    MakeContentPackage(dev_layer, "content-kit");

    const std::shared_ptr<const PackageSnapshot> snapshot = BuildPackageSnapshot(DevInput(dev_layer), 7);
    CHECK(snapshot->generation == 7);
    CHECK_FALSE(snapshot->built_at_unix.empty());
    REQUIRE(snapshot->mount().entries.size() == 1);
    CHECK(snapshot->mount().entries[0].package_id == "test.content-kit");
    CHECK(snapshot->Find("test.content-kit") != nullptr);  // Find 直通挂载账
    CHECK(snapshot->pinned_trust.keys.empty());            // 没批过谁
    CHECK(snapshot->state.IsEnabled("test.content-kit"));  // 没停过谁

    // 技能正文查表:parser 已读进 records,快照摊平成查表;canonical 名齐。
    const std::optional<std::string> body = snapshot->SkillBody("test.content-kit:checklist");
    REQUIRE(body.has_value());
    CHECK(body->find("照单核") != std::string::npos);
    CHECK(snapshot->SkillBody("test.content-kit:no-such") == std::nullopt);
    const std::vector<std::string> ids = snapshot->SkillCanonicalIds();
    REQUIRE(ids.size() == 1);
    CHECK(ids[0] == "test.content-kit:checklist");
}

// ---------------------------------------------------------------------------
// 原子 reload:折好才换
// ---------------------------------------------------------------------------

TEST_CASE("ReloadPackageSnapshot:对账行增/减/改;信任账钉 current 那份") {
    TempDir layers;
    const fs::path dev_layer = layers.Get() / "dev-packages";
    fs::create_directories(dev_layer);
    MakeContentPackage(dev_layer, "content-kit");

    const std::shared_ptr<const PackageSnapshot> first = BuildPackageSnapshot(DevInput(dev_layer), 1);

    SUBCASE("无变化:逐包一致") {
        const PackageReloadReport report = ReloadPackageSnapshot(first, DevInput(dev_layer));
        REQUIRE(report.ok);
        CHECK(report.snapshot != nullptr);
        CHECK(report.snapshot != first);
        CHECK(report.snapshot->generation == 2);
        const std::string joined = JoinLines(report.lines);
        CHECK(joined.find("第 2 折") != std::string::npos);
        CHECK(joined.find("与上一折逐包一致") != std::string::npos);
        CHECK(joined.find("code 组件") != std::string::npos);
        CHECK(joined.find("须新会话") != std::string::npos);
    }
    SUBCASE("新增 + 内容已变") {
        MakeContentPackage(dev_layer, "second-kit", "test.second-kit");
        WriteFile(dev_layer / "content-kit" / "skills" / "checklist" / "SKILL.md",
                  "---\nname: checklist\ndescription: 核对章法。\n---\n换了章法。");
        const PackageReloadReport report = ReloadPackageSnapshot(first, DevInput(dev_layer));
        REQUIRE(report.ok);
        const std::string joined = JoinLines(report.lines);
        CHECK(joined.find("新增 1 包: test.second-kit") != std::string::npos);
        CHECK(joined.find("内容已变 1 包: test.content-kit") != std::string::npos);
        REQUIRE(report.snapshot->mount().entries.size() == 2);
        CHECK(report.snapshot->SkillBody("test.content-kit:checklist").value_or("").find("换了章法") !=
              std::string::npos);
        // 旧折不动:在跑引用钉着它,正文还是原文。
        CHECK(first->SkillBody("test.content-kit:checklist").value_or("").find("照单核") !=
              std::string::npos);
    }
    SUBCASE("移除") {
        fs::remove_all(dev_layer / "content-kit");
        const PackageReloadReport report = ReloadPackageSnapshot(first, DevInput(dev_layer));
        REQUIRE(report.ok);
        const std::string joined = JoinLines(report.lines);
        CHECK(joined.find("移除 1 包: test.content-kit") != std::string::npos);
        CHECK(report.snapshot->empty());
        CHECK_FALSE(first->empty());  // 旧折照旧(在跑引用的证据)
    }
    SUBCASE("信任账钉 current 那份:reload 不放行运行中新批的 code") {
        // fresh input 带上一枚"已信任"的账(模拟运行中 /package trust 落了账);
        // reload 仍按 current 钉住的空账折——code 门禁会话启动定终身。
        PackageMountInput fresh = DevInput(dev_layer);
        PackageTrustSnapshot runtime_approved;
        runtime_approved.keys.insert("test.content-kit\n.deadbeef");
        fresh.trust = runtime_approved;
        const PackageReloadReport report = ReloadPackageSnapshot(first, fresh);
        REQUIRE(report.ok);
        CHECK(report.snapshot->pinned_trust.keys.empty());
    }
}

TEST_CASE("ReloadPackageSnapshot:坏包回执诊断,好包不连坐,旧快照不动") {
    TempDir layers;
    const fs::path dev_layer = layers.Get() / "dev-packages";
    fs::create_directories(dev_layer);
    MakeContentPackage(dev_layer, "content-kit");

    const std::shared_ptr<const PackageSnapshot> first = BuildPackageSnapshot(DevInput(dev_layer), 1);
    REQUIRE(first->mount().entries.size() == 1);

    // 盘中放一只坏包:清单四处错(schema 非 1 / id 大写 / version 非 SemVer /
    // 未知字段)。reload 折得成(ok),坏包一件不挂,回执指 doctor。
    const fs::path broken = dev_layer / "broken-kit";
    WriteFile(broken / "package.yaml",
              "schema: 9\nid: Test_Broken\nversion: not-semver\nname: Broken\n"
              "description: 坏包。\npermissions: {}\n");
    const PackageReloadReport report = ReloadPackageSnapshot(first, DevInput(dev_layer));
    REQUIRE(report.ok);
    const std::string joined = JoinLines(report.lines);
    CHECK(joined.find("invalid 包 1 件") != std::string::npos);
    CHECK(joined.find("broken-kit") != std::string::npos);
    CHECK(joined.find("/package doctor") != std::string::npos);
    // 好包不连坐:新折里照旧挂着。
    REQUIRE(report.snapshot->mount().entries.size() == 1);
    CHECK(report.snapshot->mount().entries[0].package_id == "test.content-kit");
    REQUIRE(report.snapshot->mount().rejected_ids.size() == 1);
    CHECK(report.snapshot->mount().rejected_ids[0] == "broken-kit");
    // 旧快照一字不动。
    CHECK(first->mount().rejected_ids.empty());
    CHECK(first->mount().entries.size() == 1);
}

// ---------------------------------------------------------------------------
// 目录突变不影响在跑会话(验收线:构造会话拿快照 -> 盘中途删包/改 SKILL
// -> 在跑引用照旧(读快照)、新装配查不着)
// ---------------------------------------------------------------------------

TEST_CASE("目录突变隔离:删包/改 SKILL 后旧快照照旧,新折查不着") {
    TempDir layers;
    const fs::path dev_layer = layers.Get() / "dev-packages";
    fs::create_directories(dev_layer);
    MakeContentPackage(dev_layer, "content-kit");

    const std::shared_ptr<const PackageSnapshot> pinned = BuildPackageSnapshot(DevInput(dev_layer), 1);
    const std::vector<lubancode::tools::SkillMeta> session_skills = lubancode::tools::LoadSkills(
        (layers.Get() / "project").string(), std::nullopt, std::nullopt,
        MountSkillRoots(pinned->mount()));

    // ---- 盘中途改 SKILL:旧快照正文是原文,新折是新文 ----
    WriteFile(dev_layer / "content-kit" / "skills" / "checklist" / "SKILL.md",
              "---\nname: checklist\ndescription: 核对章法。\n---\n夜里改的章法。");
    CHECK(pinned->SkillBody("test.content-kit:checklist").value_or("").find("照单核") != std::string::npos);
    const std::shared_ptr<const PackageSnapshot> after_edit =
        BuildPackageSnapshot(DevInput(dev_layer), 2);
    CHECK(after_edit->SkillBody("test.content-kit:checklist").value_or("").find("夜里改的章法") !=
          std::string::npos);

    // ---- 盘中途删包:旧快照四张表照旧,新折一件没有 ----
    fs::remove_all(dev_layer / "content-kit");
    const std::vector<lubancode::agent::PackagedAgentEntry> agents = MountAgentEntries(pinned->mount());
    REQUIRE(agents.size() == 1);
    CHECK(agents[0].canonical_name == "test.content-kit:reviewer");
    REQUIRE(agents[0].definition.skills_preload.size() == 1);
    CHECK(agents[0].definition.skills_preload[0] == "test.content-kit:checklist");
    CHECK_FALSE(MountSkillRoots(pinned->mount()).empty());

    const std::shared_ptr<const PackageSnapshot> after_delete =
        BuildPackageSnapshot(DevInput(dev_layer), 3);
    CHECK(after_delete->empty());
    CHECK(after_delete->SkillBody("test.content-kit:checklist") == std::nullopt);
    CHECK(MountAgentEntries(after_delete->mount()).empty());

    // ---- disable 同理:启停账只拦新折,旧快照不拆 ----
    MakeContentPackage(dev_layer, "content-kit");
    PackageMountInput disabled_input = DevInput(dev_layer);
    disabled_input.state.disabled.insert("test.content-kit");
    const std::shared_ptr<const PackageSnapshot> after_disable =
        BuildPackageSnapshot(disabled_input, 4);
    CHECK(after_disable->empty());
    CHECK_FALSE(pinned->empty());
}

// ---------------------------------------------------------------------------
// 半场 Workflow 不换 Skill(reload 后解析仍钉旧折)+ ToolRegistry 引用的
// code 模块不被 reload 卸掉(阶段 6 验收线,写成回归)
// ---------------------------------------------------------------------------

TEST_CASE("半场不换 Skill:reload 换档后,钉旧折的 agent 解析照旧") {
    TempDir layers;
    const fs::path dev_layer = layers.Get() / "dev-packages";
    fs::create_directories(dev_layer);
    MakeContentPackage(dev_layer, "content-kit");

    // 会话起步折第一份;workflow 跑一趟钉这一份(exec_ctx.package_snapshot)。
    const std::shared_ptr<const PackageSnapshot> startup = BuildPackageSnapshot(DevInput(dev_layer), 1);
    const std::vector<lubancode::tools::SkillMeta> skills = lubancode::tools::LoadSkills(
        (layers.Get() / "project").string(), std::nullopt, std::nullopt,
        MountSkillRoots(startup->mount()));
    REQUIRE(skills.size() == 1);

    const std::optional<lubancode::tools::CustomAgentMaterial> before =
        lubancode::app::ResolveCustomAgentMaterial(skills, startup.get(), "test.content-kit:reviewer");
    REQUIRE(before.has_value());
    REQUIRE(before->preloaded_skills.size() == 1);
    CHECK(before->preloaded_skills[0].find("照单核") != std::string::npos);

    // 半场:盘中改 SKILL + reload 换档(新折)。钉旧折的解析(在跑的那趟
    // Workflow)拿到的还是旧正文——reload 不会让半场 Workflow 换 Skill。
    WriteFile(dev_layer / "content-kit" / "skills" / "checklist" / "SKILL.md",
              "---\nname: checklist\ndescription: 核对章法。\n---\n中场换的章法。");
    const PackageReloadReport report = ReloadPackageSnapshot(startup, DevInput(dev_layer));
    REQUIRE(report.ok);
    const std::optional<lubancode::tools::CustomAgentMaterial> pinned_resolve =
        lubancode::app::ResolveCustomAgentMaterial(skills, startup.get(), "test.content-kit:reviewer");
    REQUIRE(pinned_resolve.has_value());
    CHECK(pinned_resolve->preloaded_skills[0].find("照单核") != std::string::npos);

    // 会话路(每派发现取现行快照):下一次装配见新账、新正文。
    const std::vector<lubancode::tools::SkillMeta> fresh_skills = lubancode::tools::LoadSkills(
        (layers.Get() / "project").string(), std::nullopt, std::nullopt,
        MountSkillRoots(report.snapshot->mount()));
    const std::optional<lubancode::tools::CustomAgentMaterial> fresh_resolve =
        lubancode::app::ResolveCustomAgentMaterial(fresh_skills, report.snapshot.get(),
                                                   "test.content-kit:reviewer");
    REQUIRE(fresh_resolve.has_value());
    CHECK(fresh_resolve->preloaded_skills[0].find("中场换的章法") != std::string::npos);

    // 盘中删包:钉旧折的解析照旧出材料(在跑引用读快照);现行快照查不着。
    fs::remove_all(dev_layer / "content-kit");
    const std::optional<lubancode::tools::CustomAgentMaterial> still_running =
        lubancode::app::ResolveCustomAgentMaterial(skills, startup.get(), "test.content-kit:reviewer");
    REQUIRE(still_running.has_value());
    CHECK(still_running->preloaded_skills[0].find("照单核") != std::string::npos);
    const PackageReloadReport after_removal = ReloadPackageSnapshot(report.snapshot, DevInput(dev_layer));
    REQUIRE(after_removal.ok);
    CHECK(after_removal.snapshot->empty());
    CHECK(lubancode::app::ResolveCustomAgentMaterial(fresh_skills, after_removal.snapshot.get(),
                                                     "test.content-kit:reviewer") == std::nullopt);
}

TEST_CASE("reload 不卸 ToolRegistry 引用的模块:code 件只在会话启动挂载") {
    TempDir layers;
    const fs::path dev_layer = layers.Get() / "dev-packages";
    fs::create_directories(dev_layer);
    MakeCodePackage(dev_layer, "code-kit");

    // 会话启动:code 件解析进快照(dev 层未批 → PendingTrust,一件不挂不
    // 执行;这里只取"挂载事务暂存材料的那份 manifest"——与 ToolRuntime
    // 从 MountPackageCode 拿到的 shared_ptr 同一所有权模型)。
    const std::shared_ptr<const PackageSnapshot> startup = BuildPackageSnapshot(DevInput(dev_layer), 1);
    REQUIRE(startup->mount().entries.size() == 1);
    CHECK(startup->mount().entries[0].code_trust == CodeTrustStatus::PendingTrust);
    std::shared_ptr<const lubancode::runtime::PluginManifest> held;
    for (const auto& record : startup->mount().records) {
        for (const auto& component : record.components) {
            if (component.kind == ComponentKind::Plugin && component.plugin.has_value()) {
                held = std::make_shared<const lubancode::runtime::PluginManifest>(*component.plugin);
            }
        }
    }
    REQUIRE(held != nullptr);
    REQUIRE(held->tools.size() == 1);

    // 盘中删包 + reload:新折里没有这只包,但 registry 侧 adapter 持有的
    // manifest(shared_ptr)原样活着——reload 是纯数据折算,不碰 ToolRegistry、
    // 不热插不热卸,回执明说 code 组件须新会话。
    fs::remove_all(dev_layer / "code-kit");
    const PackageReloadReport report = ReloadPackageSnapshot(startup, DevInput(dev_layer));
    REQUIRE(report.ok);
    CHECK(report.snapshot->empty());
    CHECK(held->id == "count-words");  // 在引用的模块照旧可读
    CHECK(held->tools.size() == 1);
    const std::string joined = JoinLines(report.lines);
    CHECK(joined.find("须新会话") != std::string::npos);
    CHECK(joined.find("不热插、不卸载") != std::string::npos);
}
