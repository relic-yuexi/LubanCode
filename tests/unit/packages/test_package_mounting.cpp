// 统一 Package 封装单阶段 3 的挂载册:会话钉快照(BuildPackageMount)与四张
// 表的可见/消失端到端——临时目录放 content-only 包 -> 四张表都查得着
// canonical 名 -> 删包 -> 新快照空、四张表查不着。另钉:包内短引用折
// canonical、包层不抢裸 alias、同 id 四层定胜者、坏包一件不挂、无包时与
// main 基线一致(legacy 零变化)。测试账对齐单子 §十六"发现与覆盖""兼容"
// 与阶段 3 验收线。
#include <doctest/doctest.h>

#include <filesystem>
#include <fstream>
#include <set>
#include <string>
#include <vector>

#include "agent/agent_catalog.hpp"
#include "agent/prompt_assembler.hpp"
#include "package/mounting.hpp"
#include "package/semver.hpp"
#include "tools/skill_loader.hpp"
#include "workflow/catalog.hpp"
#include "workflow/validator.hpp"

using namespace lubancode::package;
namespace fs = std::filesystem;

namespace {

class TempDir {
public:
    TempDir() {
        dir_ = fs::temp_directory_path() /
               ("lubancode_pkg_mount_" + std::to_string(reinterpret_cast<std::uintptr_t>(this)));
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

const char* kManifest = "schema: 1\nid: test.content-kit\nversion: 0.1.0\nname: Content Kit\n"
                        "description: 内容包测试件。\n";

const char* kAgentYaml = R"yaml(schema: 1
name: reviewer
description: 复核员。
prompt:
  profile: reviewer-profile
skills:
  preload:
    - checklist
)yaml";

const char* kSkillMd = "---\nname: checklist\ndescription: 核对章法。\n---\n照单核。";

const char* kWorkflowYaml = R"yaml(schema_version: 1
id: review-pass
version: 1.0.0
name: 复核一趟
alias: review-pass
description: 采摘要,交 reviewer 复核
enabled: true
inputs:
  type: object
  required:
    - text
  properties:
    text:
      type: string
      description: 待复核正文
entry: verify
nodes:
  verify:
    type: agent
    label: 复核
    agent: reviewer
    task: prompts/verify.md
    input:
      text: "${inputs.text}"
  done:
    type: end
    label: 缴单
edges:
  - from: verify
    on: success
    to: done
)yaml";

// 一只 content-only 包(Agent + Profile + Skill + Workflow 四类各一,引用全
// 闭合且全在本包内)。
fs::path MakeContentPackage(const fs::path& parent, const std::string& dir,
                            const std::string& id = "test.content-kit") {
    const fs::path root = parent / dir;
    WriteFile(root / "package.yaml",
              "schema: 1\nid: " + id + "\nversion: 0.1.0\nname: Content Kit\n"
              "description: 内容包测试件。\n");
    WriteFile(root / "agents" / "reviewer.yaml", kAgentYaml);
    WriteFile(root / "prompts" / "profiles" / "reviewer-profile" / "core" / "10-identity.md",
              "你是复核员,只认证据。");
    WriteFile(root / "skills" / "checklist" / "SKILL.md", kSkillMd);
    WriteFile(root / "workflows" / "review-pass" / "prompts" / "verify.md", "复核:{{text}}");
    WriteFile(root / "workflows" / "review-pass" / "workflow.yaml", kWorkflowYaml);
    return root;
}

PackageMount MountDevDir(const fs::path& layer_root) {
    PackageMountInput input;
    input.scan.dev_roots.push_back(layer_root);
    return BuildPackageMount(input);
}

}  // namespace

// ---------------------------------------------------------------------------
// 端到端验收线:放包 -> 四张表可见;删包 -> 新 runtime 查不着。
// ---------------------------------------------------------------------------
TEST_CASE("content-only 包端到端:四张表可见,删包后消失") {
    TempDir layers;
    const fs::path dev_layer = layers.Get() / "dev-packages";
    fs::create_directories(dev_layer);
    MakeContentPackage(dev_layer, "content-kit");

    PackageMount mount = MountDevDir(dev_layer);
    REQUIRE_FALSE(mount.empty());
    REQUIRE(mount.entries.size() == 1);
    CHECK(mount.entries[0].package_id == "test.content-kit");
    CHECK(mount.entries[0].code_trust == CodeTrustStatus::NoCode);  // 纯内容包不过门
    CHECK(mount.entries[0].mounted_canonical_ids.size() == 4);

    // ---- Skill 表:canonical 名,来源标签带包 ----
    const std::vector<lubancode::tools::SkillMeta> skills =
        lubancode::tools::LoadSkills((layers.Get() / "empty-project").string(), std::nullopt, std::nullopt,
                                     MountSkillRoots(mount));
    REQUIRE(skills.size() == 1);
    CHECK(skills[0].name == "test.content-kit:checklist");
    CHECK(skills[0].package_id == "test.content-kit");
    CHECK(skills[0].source_level.find("包") != std::string::npos);

    // ---- Agent 表:canonical 名,包层登册,引用已折 canonical ----
    lubancode::agent::AgentCatalogScanRoots agent_roots;
    agent_roots.packaged = MountAgentEntries(mount);
    const lubancode::agent::AgentCatalog agents = lubancode::agent::LoadAgentCatalog(agent_roots);
    const lubancode::agent::AgentCatalogEntry* agent =
        agents.Find("test.content-kit:reviewer");
    REQUIRE(agent != nullptr);
    CHECK(agent->layer == lubancode::agent::AgentSourceLayer::Package);
    CHECK(agent->package_id == "test.content-kit");
    CHECK(agent->available);
    REQUIRE(agent->definition.has_value());
    CHECK(agent->definition->prompt.profile.value_or("") == "test.content-kit:reviewer-profile");
    REQUIRE(agent->definition->skills_preload.size() == 1);
    CHECK(agent->definition->skills_preload[0] == "test.content-kit:checklist");

    // ---- Workflow 表:canonical id,包内 agent 短引用已折 canonical ----
    const std::vector<lubancode::workflow::PackagedWorkflowSource> workflows = MountWorkflowSources(mount);
    REQUIRE(workflows.size() == 1);
    CHECK(workflows[0].canonical_id == "test.content-kit:review-pass");
    CHECK(workflows[0].definition.id == "test.content-kit:review-pass");
    REQUIRE(workflows[0].definition.nodes.size() == 2);
    CHECK(workflows[0].definition.nodes[0].agent == "test.content-kit:reviewer");
    const lubancode::workflow::Catalog wf_catalog =
        lubancode::workflow::LoadCatalog(std::nullopt, std::nullopt, workflows);
    const lubancode::workflow::CatalogEntry* wf = wf_catalog.Find("test.content-kit:review-pass");
    REQUIRE(wf != nullptr);
    CHECK(wf->scope == lubancode::workflow::WorkflowScope::Package);
    CHECK(wf->package_id == "test.content-kit");
    // canonical id 过校验(裸 id 规矩只管 standalone)。
    const lubancode::workflow::ValidationResult validated =
        lubancode::workflow::ValidateDefinition(wf->definition, std::nullopt);
    for (const auto& issue : validated.issues) {
        CHECK(issue.code != "bad_id");
    }

    // ---- Prompt Profile:canonical 名在包根解析,来源账本记 package 层 ----
    lubancode::agent::PromptOptions options;
    options.cwd = "D:/nowhere";
    options.profile = "test.content-kit:reviewer-profile";
    options.package_profile_roots = MountProfileRoots(mount);
    lubancode::agent::PromptSourceLedger ledger;
    const std::string prompt = lubancode::agent::AssembleSystemPrompt(options, &ledger);
    CHECK(prompt.find("你是复核员,只认证据。") != std::string::npos);
    const auto* identity = ledger.Find("core/10-identity.md");
    REQUIRE(identity != nullptr);
    CHECK(identity->origin == lubancode::agent::PromptModuleOrigin::PackageProfile);
    CHECK(identity->file.find("reviewer-profile") != std::string::npos);

    // ---- 拿走包:新快照空,四张表全查不着 ----
    std::error_code ec;
    fs::remove_all(dev_layer / "content-kit", ec);
    PackageMount mount_after = MountDevDir(dev_layer);
    CHECK(mount_after.empty());
    CHECK(MountSkillRoots(mount_after).empty());
    CHECK(MountAgentEntries(mount_after).empty());
    CHECK(MountWorkflowSources(mount_after).empty());
    CHECK(MountProfileRoots(mount_after).empty());

    agent_roots.packaged.clear();
    const lubancode::agent::AgentCatalog agents_after =
        lubancode::agent::LoadAgentCatalog(agent_roots);
    CHECK(agents_after.Find("test.content-kit:reviewer") == nullptr);

    const std::vector<lubancode::tools::SkillMeta> skills_after = lubancode::tools::LoadSkills(
        (layers.Get() / "empty-project").string(), std::nullopt, std::nullopt, MountSkillRoots(mount_after));
    CHECK(skills_after.empty());

    const lubancode::workflow::Catalog wf_after =
        lubancode::workflow::LoadCatalog(std::nullopt, std::nullopt, MountWorkflowSources(mount_after));
    CHECK(wf_after.Find("test.content-kit:review-pass") == nullptr);

    options.package_profile_roots.clear();
    const std::string prompt_after = lubancode::agent::AssembleSystemPrompt(options);
    CHECK(prompt_after.find("你是复核员,只认证据。") == std::string::npos);
}

// ---------------------------------------------------------------------------
// 会话钉快照:启动后目录突变,快照不变(首版语义:下回启动才见)。
// ---------------------------------------------------------------------------
TEST_CASE("会话钉快照:启动后放包/删包,本会话材料不变") {
    TempDir layers;
    const fs::path dev_layer = layers.Get() / "dev-packages";
    fs::create_directories(dev_layer);
    MakeContentPackage(dev_layer, "content-kit");

    PackageMount mount = MountDevDir(dev_layer);
    REQUIRE(mount.entries.size() == 1);

    // 会话中删盘上的包:快照的账照旧(材料已折进内存);新快照才空。
    std::error_code ec;
    fs::remove_all(dev_layer / "content-kit", ec);
    CHECK_FALSE(mount.empty());
    CHECK(MountAgentEntries(mount).size() == 1);

    // 会话中放一只新包:同一份快照看不见。
    MakeContentPackage(dev_layer, "late-kit", "test.late-kit");
    CHECK(mount.Find("test.late-kit") == nullptr);
    PackageMount next = MountDevDir(dev_layer);
    CHECK(next.Find("test.late-kit") != nullptr);
}

// ---------------------------------------------------------------------------
// 发现与覆盖:同 id 四层定胜者;被遮的不挂。坏包一件不挂。
// ---------------------------------------------------------------------------
TEST_CASE("同 id 定胜者:dev 层盖 user 层,只挂胜者") {
    TempDir layers;
    const fs::path dev_layer = layers.Get() / "dev-packages";
    const fs::path user_layer = layers.Get() / "packages";
    fs::create_directories(dev_layer);
    fs::create_directories(user_layer);
    MakeContentPackage(dev_layer, "kit", "test.content-kit");
    MakeContentPackage(user_layer, "kit", "test.content-kit");

    PackageMountInput input;
    input.scan.dev_roots.push_back(dev_layer);
    input.scan.user_root = user_layer;
    PackageMount mount = BuildPackageMount(input);
    REQUIRE(mount.entries.size() == 1);
    CHECK(mount.entries[0].scope == PackageScope::Dev);
    CHECK(mount.entries[0].package_root == dev_layer / "kit");
}

TEST_CASE("坏包一件不挂:悬空引用让整包 invalid") {
    TempDir layers;
    const fs::path dev_layer = layers.Get() / "dev-packages";
    fs::create_directories(dev_layer);
    const fs::path root = MakeContentPackage(dev_layer, "broken-kit", "test.broken-kit");
    // 抽走 skill:agent 的 skills.preload 悬空 -> 整包 invalid,一件不挂。
    std::error_code ec;
    fs::remove_all(root / "skills", ec);

    PackageMount mount = MountDevDir(dev_layer);
    CHECK(mount.empty());
    CHECK(MountSkillRoots(mount).empty());
    CHECK(MountAgentEntries(mount).empty());
    CHECK(MountWorkflowSources(mount).empty());
    CHECK(MountProfileRoots(mount).empty());
}

// ---------------------------------------------------------------------------
// 命名空间:canonical 不撞裸名;包层 workflow 不抢裸 alias。
// ---------------------------------------------------------------------------
TEST_CASE("canonical 名与裸名并存,包层不抢裸 alias") {
    TempDir layers;
    const fs::path dev_layer = layers.Get() / "dev-packages";
    fs::create_directories(dev_layer);
    MakeContentPackage(dev_layer, "content-kit");

    // 同目录里再造一只 standalone 同名 alias 的 workflow,直呼归它。
    WriteFile(layers.Get() / "proj" / ".lubancode" / "workflows" / "review-pass" / "workflow.yaml",
              kWorkflowYaml);
    WriteFile(layers.Get() / "proj" / ".lubancode" / "workflows" / "review-pass" / "prompts" /
                  "verify.md",
              "复核:{{text}}");

    PackageMount mount = MountDevDir(dev_layer);
    const lubancode::workflow::Catalog catalog = lubancode::workflow::LoadCatalog(
        layers.Get() / "proj", std::nullopt, MountWorkflowSources(mount));
    // 两份并存:canonical 与裸 id 各归各。
    CHECK(catalog.Find("test.content-kit:review-pass") != nullptr);
    CHECK(catalog.Find("review-pass") != nullptr);
    // 裸 alias 直呼归 standalone;packaged 那份不抢。
    const lubancode::workflow::CatalogEntry* by_alias = catalog.FindByAlias("review-pass");
    REQUIRE(by_alias != nullptr);
    CHECK(by_alias->scope == lubancode::workflow::WorkflowScope::Project);
}

// ---------------------------------------------------------------------------
// 兼容(legacy 零变化):无包时,四条加载路与 main 基线一致。
// ---------------------------------------------------------------------------
TEST_CASE("无包时与旧路逐字节一致") {
    TempDir layers;
    const std::string project = (layers.Get() / "proj").string();
    WriteFile(layers.Get() / "proj" / ".lubancode" / "skills" / "solo" / "SKILL.md", kSkillMd);
    WriteFile(layers.Get() / "proj" / ".lubancode" / "agents" / "solo.yaml",
              "schema: 1\nname: solo\ndescription: 散装代理。\n");
    WriteFile(layers.Get() / "proj" / ".lubancode" / "workflows" / "solo-run" / "workflow.yaml",
              kWorkflowYaml);
    WriteFile(layers.Get() / "proj" / ".lubancode" / "workflows" / "solo-run" / "prompts" /
                  "verify.md",
              "复核:{{text}}");

    // Skill:空包根 = 旧三参签名同果。
    const std::vector<lubancode::tools::SkillMeta> with_roots =
        lubancode::tools::LoadSkills(project, std::nullopt, std::nullopt, {});
    const std::vector<lubancode::tools::SkillMeta> without_roots =
        lubancode::tools::LoadSkills(project, std::nullopt, std::nullopt);
    REQUIRE(with_roots.size() == 1);
    REQUIRE(without_roots.size() == 1);
    CHECK(with_roots[0].name == without_roots[0].name);
    CHECK(with_roots[0].dir_path == without_roots[0].dir_path);
    CHECK(with_roots[0].source_level == without_roots[0].source_level);
    CHECK(with_roots[0].package_id.empty());

    // Agent:空包层 = 旧三层同果(名字、层、来源文件)。
    lubancode::agent::AgentCatalogScanRoots roots;
    roots.project_dir = layers.Get() / "proj" / ".lubancode" / "agents";
    const lubancode::agent::AgentCatalog with_packaged = lubancode::agent::LoadAgentCatalog(roots);
    roots.packaged.clear();  // 本就空;旧签名等价
    const lubancode::agent::AgentCatalog without_packaged = lubancode::agent::LoadAgentCatalog(roots);
    REQUIRE(with_packaged.entries.size() == without_packaged.entries.size());
    CHECK(with_packaged.entries.size() >= 2);  // builtin 两枚 + solo
    for (std::size_t i = 0; i < with_packaged.entries.size(); ++i) {
        CHECK(with_packaged.entries[i].name == without_packaged.entries[i].name);
        CHECK(with_packaged.entries[i].layer == without_packaged.entries[i].layer);
        CHECK(with_packaged.entries[i].file == without_packaged.entries[i].file);
        CHECK(with_packaged.entries[i].package_id.empty());
    }

    // Workflow:空包层 = 旧两级同果。
    const lubancode::workflow::Catalog wf_with =
        lubancode::workflow::LoadCatalog(layers.Get() / "proj", std::nullopt, {});
    const lubancode::workflow::Catalog wf_without =
        lubancode::workflow::LoadCatalog(layers.Get() / "proj", std::nullopt);
    REQUIRE(wf_with.entries.size() == wf_without.entries.size());
    for (std::size_t i = 0; i < wf_with.entries.size(); ++i) {
        CHECK(wf_with.entries[i].definition.id == wf_without.entries[i].definition.id);
        CHECK(wf_with.entries[i].scope == wf_without.entries[i].scope);
        CHECK(wf_with.entries[i].package_id.empty());
    }

    // Prompt:空包根时,default 上下文与裸名 Profile 逐字节不变。
    lubancode::agent::PromptOptions base;
    base.cwd = "D:/nowhere";
    lubancode::agent::PromptOptions with_package_roots = base;
    with_package_roots.package_profile_roots = MountProfileRoots(PackageMount{});
    CHECK(lubancode::agent::AssembleSystemPrompt(base) ==
          lubancode::agent::AssembleSystemPrompt(with_package_roots));
    base.profile = "some-bare-profile";
    with_package_roots.profile = "some-bare-profile";
    CHECK(lubancode::agent::AssembleSystemPrompt(base) ==
          lubancode::agent::AssembleSystemPrompt(with_package_roots));
}

// ---------------------------------------------------------------------------
// canonical id 形状(validator 的放宽只认这个形状)。
// ---------------------------------------------------------------------------
TEST_CASE("canonical workflow id 形状") {
    CHECK(lubancode::workflow::IsCanonicalPackagedWorkflowId("moontide.browser-suite:smoke-test"));
    CHECK(lubancode::workflow::IsCanonicalPackagedWorkflowId("a.b:c"));
    CHECK_FALSE(lubancode::workflow::IsCanonicalPackagedWorkflowId("smoke-test"));
    CHECK_FALSE(lubancode::workflow::IsCanonicalPackagedWorkflowId("moontide.browser-suite:x:y"));
    CHECK_FALSE(lubancode::workflow::IsCanonicalPackagedWorkflowId("nodot:name"));
    CHECK_FALSE(lubancode::workflow::IsCanonicalPackagedWorkflowId("moontide.Browser:name"));
    CHECK_FALSE(lubancode::workflow::IsCanonicalPackagedWorkflowId("moontide.browser-suite:Bad"));
}

// ---------------------------------------------------------------------------
// full-stack 夹具:code-bearing 包的内容组件照挂,代码组件账上待信任门。
// ---------------------------------------------------------------------------
TEST_CASE("full-stack 夹具:内容组件挂载,Plugin/MCP 待信任门") {
    const fs::path fixtures = fs::path(LUBANCODE_SOURCE_DIR) / "tests" / "fixtures" / "packages";
    PackageMountInput input;
    input.scan.dev_roots.push_back(fixtures);
    // doctor/挂载同一口径:兼容性版本警告不挡(阶段 1 定的 Warning),但
    // 引用账照查——full-stack 的引用全闭合。
    input.scan.current_lubancode = lubancode::package::ParseSemVer("0.26.95").value_or(SemVer{});
    input.scan.current_platform = "windows";
    PackageMount mount = BuildPackageMount(input);

    REQUIRE(mount.entries.size() >= 2);  // full-stack + minimal-content-only
    const PackageMountEntry* full = mount.Find("moontide.full-stack");
    REQUIRE(full != nullptr);
    CHECK(full->code_trust == CodeTrustStatus::PendingTrust);  // 有 plugin + mcp,未批
    // 内容组件四类各一,canonical 齐全;plugin/mcp 不进 mounted 清账。
    // agent 与 profile 同名各归各表(契约 §6:canonical 不带 kind 段)。
    std::set<std::string> ids(full->mounted_canonical_ids.begin(), full->mounted_canonical_ids.end());
    CHECK(ids.count("moontide.full-stack:browser-tester") == 1);   // agent + profile
    CHECK(ids.count("moontide.full-stack:browser-testing") == 1);  // skill
    CHECK(ids.count("moontide.full-stack:smoke-test") == 1);       // workflow
    CHECK(ids.count("moontide.full-stack:dom-analyzer") == 0);     // plugin 不挂
    CHECK(ids.count("moontide.full-stack:browser") == 0);          // mcp 不挂
    CHECK(full->mounted_canonical_ids.size() == 4);

    const std::vector<lubancode::agent::PackagedAgentEntry> agents = MountAgentEntries(mount);
    REQUIRE(agents.size() == 1);
    CHECK(agents[0].canonical_name == "moontide.full-stack:browser-tester");
    CHECK(agents[0].definition.prompt.profile.value_or("") ==
          "moontide.full-stack:browser-tester");  // 短名已折 canonical
    // 阶段 4 连坐:包未信任,引 MCP browser 的 Agent 不可用并注明缘由;
    // 依赖该 Agent(又直引 plugin 工具)的 Workflow 同样不可用。
    CHECK_FALSE(agents[0].available);
    CHECK(agents[0].unavailable_reason.find("未过信任门") != std::string::npos);
    CHECK(agents[0].unavailable_reason.find("moontide.full-stack:browser") != std::string::npos);
    const std::vector<lubancode::workflow::PackagedWorkflowSource> workflows =
        MountWorkflowSources(mount);
    REQUIRE(workflows.size() == 1);
    CHECK(workflows[0].canonical_id == "moontide.full-stack:smoke-test");
    CHECK_FALSE(workflows[0].available);
    CHECK(workflows[0].unavailable_reason.find("未过信任门") != std::string::npos);
}
