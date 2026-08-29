// 统一 Package 封装单阶段 4 的信任册:PackageTrustStore(键 = 包 id + 整包
// 内容哈希,文件一动哈希就变、旧信任即失效)、/package trust|untrust 的
// 审批材料与账务(五样回执)、AnalyzePackage 的门禁标注(MountPlan 里
// code 件全标待信任/已信任)、挂载连坐(未信任 → code 件零挂载,依赖它的
// Agent/Workflow unavailable)。测试账对齐单子 §十六"信任与安全"与阶段 4
// 验收线:项目 Package 未批时一行代码也不起;批准后只认账上那枚哈希。
#include <doctest/doctest.h>

#include <filesystem>
#include <fstream>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "agent/agent_catalog.hpp"
#include "app/commands/package_commands.hpp"  // ParsePackageCommand(纯函数)
#include "package/catalog.hpp"
#include "package/inventory.hpp"
#include "package/mounting.hpp"
#include "package/semver.hpp"
#include "package/trust.hpp"
#include "platform/dir_fingerprint.hpp"  // PluginDirFingerprintV1(共用底座)
#include "runtime/plugin_tool.hpp"        // ComputePluginContentHash(委托后的门面)
#include "workflow/catalog.hpp"

using namespace lubancode::package;
using lubancode::app::ParsePackageCommand;
namespace fs = std::filesystem;

namespace {

const fs::path kFixturesRoot = fs::path(LUBANCODE_SOURCE_DIR) / "tests" / "fixtures" / "packages";

class TempDir {
public:
    TempDir() {
        dir_ = fs::temp_directory_path() /
               ("lubancode_pkg_trust_" + std::to_string(reinterpret_cast<std::uintptr_t>(this)));
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

// 一只最小 code-bearing 包:agent 引 mcp(agent 的 mcp_servers 短名)。
void MakeCodePackage(const fs::path& parent, const std::string& dir, const std::string& id) {
    const fs::path root = parent / dir;
    WriteFile(root / "package.yaml",
              "schema: 1\nid: " + id + "\nversion: 0.1.0\nname: C\ndescription: 带码测试件。\n");
    WriteFile(root / "agents/probe.yaml", R"yaml(schema: 1
name: probe
description: 探针。
mcp_servers:
  - browser
)yaml");
    WriteFile(root / "mcp/browser/mcp.yaml",
              "schema: 1\nid: browser\ndescription: 假服务。\ntransport: stdio\nruntime:\n  command: "
              "node\n  args:\n    - \"${package_dir}/mcp/browser/server.js\"\npermissions:\n  network: "
              "false\n");
    WriteFile(root / "mcp/browser/server.js", "// placeholder\n");
}

PackageCandidate DevCandidate(const fs::path& root) {
    PackageCandidate candidate;
    candidate.scope = PackageScope::Dev;
    candidate.layer_root = root.parent_path();
    candidate.package_root = root;
    candidate.dir_name = root.filename().string();
    return candidate;
}

PackageRecord AnalyzeDev(const fs::path& root, const PackageTrustSnapshot* trust = nullptr) {
    return AnalyzePackage(DevCandidate(root), ScanOptions{}, PackageRefIndex{},
                          ExternalNamespaces{}, trust);
}

}  // namespace

// ---------------------------------------------------------------------------
// 命令拆解(纯函数)
// ---------------------------------------------------------------------------

TEST_CASE("ParsePackageCommand:trust/untrust 子命令,缺目标按 Invalid") {
    CHECK(ParsePackageCommand("trust moontide.full-stack").action ==
          lubancode::app::PackageCommandAction::Trust);
    CHECK(ParsePackageCommand("trust moontide.full-stack").target == "moontide.full-stack");
    CHECK(ParsePackageCommand("UNTRUST demo.pkg").action ==
          lubancode::app::PackageCommandAction::Untrust);
    CHECK(ParsePackageCommand("UNTRUST demo.pkg").target == "demo.pkg");
    CHECK(ParsePackageCommand("trust").action == lubancode::app::PackageCommandAction::Invalid);
    CHECK(ParsePackageCommand("trust").bad_word == "trust");
    CHECK(ParsePackageCommand("untrust").action == lubancode::app::PackageCommandAction::Invalid);
}

// ---------------------------------------------------------------------------
// PackageTrustStore
// ---------------------------------------------------------------------------

TEST_CASE("PackageTrustStore:纯内存可批可销;id+哈希是键,版本只是随账") {
    auto [store, err] = PackageTrustStore::Load(std::nullopt);
    REQUIRE(err == std::nullopt);
    CHECK_FALSE(store.IsTrusted("demo.pkg", "hash-a"));
    store.SetTrusted("demo.pkg", "1.0.0", "hash-a", "dev");
    CHECK(store.IsTrusted("demo.pkg", "hash-a"));
    CHECK_FALSE(store.IsTrusted("demo.pkg", "hash-b"));  // 哈希对不上不算批
    CHECK_FALSE(store.IsTrusted("other.pkg", "hash-a"));  // 包名对不上不算批
    const auto latest = store.Latest("demo.pkg");
    REQUIRE(latest.has_value());
    CHECK(latest->version == "1.0.0");
    CHECK(latest->scope == "dev");
    CHECK_FALSE(latest->trusted_at_unix.empty());
    CHECK(store.Untrust("demo.pkg").size() == 1);
    CHECK_FALSE(store.IsTrusted("demo.pkg", "hash-a"));
    CHECK(store.Latest("demo.pkg") == std::nullopt);
}

TEST_CASE("PackageTrustStore:落盘往返;坏 JSON 按空白重开") {
    TempDir temp;
    const fs::path file = temp.Get() / "package-trust.json";
    const std::string path_utf8 = file.string();
    {
        auto [store, err] = PackageTrustStore::Load(path_utf8);
        REQUIRE(err == std::nullopt);
        store.SetTrusted("demo.pkg", "1.2.0", "hash-x", "project");
        store.SetTrusted("demo.pkg", "1.3.0", "hash-y", "project");
        REQUIRE(fs::exists(file));
    }
    {
        auto [store, err] = PackageTrustStore::Load(path_utf8);
        REQUIRE(err == std::nullopt);
        CHECK(store.trusted_count() == 2);
        CHECK(store.IsTrusted("demo.pkg", "hash-x"));
        CHECK(store.IsTrusted("demo.pkg", "hash-y"));
        const auto latest = store.Latest("demo.pkg");
        REQUIRE(latest.has_value());
        CHECK(latest->content_hash == "hash-y");  // 后批的胜出
        CHECK(latest->version == "1.3.0");
        CHECK(store.Untrust("demo.pkg").size() == 2);
        CHECK(store.trusted_count() == 0);
    }
    // 坏账本:警告 + 空白。
    WriteFile(file, "{ not json");
    auto [broken, broken_err] = PackageTrustStore::Load(path_utf8);
    CHECK(broken_err.has_value());
    CHECK(broken.trusted_count() == 0);
}

// ---------------------------------------------------------------------------
// AnalyzePackage 的门禁标注
// ---------------------------------------------------------------------------

TEST_CASE("AnalyzePackage:dev 层 code-bearing 未批全标待信任;批了哈希即过门") {
    TempDir temp;
    MakeCodePackage(temp.Get(), "demo", "demo.pkg");
    const PackageRecord pending = AnalyzeDev(temp.Get() / "demo");
    REQUIRE(pending.valid);
    REQUIRE(pending.mount_plan.has_value());
    CHECK(pending.code_trust == CodeTrustStatus::PendingTrust);
    CHECK(pending.mount_plan->code_trust == CodeTrustStatus::PendingTrust);
    for (const auto& entry : pending.mount_plan->entries) {
        if (entry.code_bearing) {
            CHECK_FALSE(entry.trusted);  // code 件全标待信任
        }
    }

    const std::string hash = pending.inventory.content_hash;
    PackageTrustSnapshot snapshot;
    snapshot.keys.insert("demo.pkg\n" + hash);
    const PackageRecord trusted = AnalyzeDev(temp.Get() / "demo", &snapshot);
    CHECK(trusted.code_trust == CodeTrustStatus::Trusted);
    for (const auto& entry : trusted.mount_plan->entries) {
        if (entry.code_bearing) {
            CHECK(entry.trusted);
        }
    }
    // 喂一枚错的哈希:门照样关着——批的是那枚哈希,不是那串包名。
    PackageTrustSnapshot wrong;
    wrong.keys.insert("demo.pkg\n" + std::string(64, '0'));
    CHECK(AnalyzeDev(temp.Get() / "demo", &wrong).code_trust == CodeTrustStatus::PendingTrust);
}

TEST_CASE("AnalyzePackage:user/official 放置即信任,project 同 dev 要批") {
    TempDir temp;
    MakeCodePackage(temp.Get(), "demo", "demo.pkg");
    PackageCandidate user = DevCandidate(temp.Get() / "demo");
    user.scope = PackageScope::User;
    const PackageRecord by_placement =
        AnalyzePackage(user, ScanOptions{}, PackageRefIndex{}, ExternalNamespaces{});
    CHECK(by_placement.code_trust == CodeTrustStatus::Trusted);

    PackageCandidate official = DevCandidate(temp.Get() / "demo");
    official.scope = PackageScope::Official;
    CHECK(AnalyzePackage(official, ScanOptions{}, PackageRefIndex{}, ExternalNamespaces{})
              .code_trust == CodeTrustStatus::Trusted);

    PackageCandidate project = DevCandidate(temp.Get() / "demo");
    project.scope = PackageScope::Project;
    CHECK(AnalyzePackage(project, ScanOptions{}, PackageRefIndex{}, ExternalNamespaces{})
              .code_trust == CodeTrustStatus::PendingTrust);
}

TEST_CASE("AnalyzePackage:content-only 不过门(NoCode)") {
    const PackageRecord record =
        AnalyzeDev(kFixturesRoot / "minimal-content-only");
    REQUIRE(record.valid);
    CHECK(record.code_trust == CodeTrustStatus::NoCode);
}

// ---------------------------------------------------------------------------
// 挂载门禁与连坐
// ---------------------------------------------------------------------------

TEST_CASE("BuildPackageMount:未批时 code 件零挂载,依赖的 Agent 不可用;批后放行") {
    TempDir temp;
    const fs::path layer = temp.Get() / "dev-packages";
    MakeCodePackage(layer, "demo", "demo.pkg");

    PackageMountInput input;
    input.scan.dev_roots.push_back(layer);
    PackageMount mount = BuildPackageMount(input);
    REQUIRE(mount.entries.size() == 1);
    CHECK(mount.entries[0].code_trust == CodeTrustStatus::PendingTrust);
    // mounted 清账只有内容件;plugin/mcp 一件不在。
    for (const std::string& id : mount.entries[0].mounted_canonical_ids) {
        CHECK(id.find(":browser") == std::string::npos);
    }
    const std::vector<lubancode::agent::PackagedAgentEntry> blocked = MountAgentEntries(mount);
    REQUIRE(blocked.size() == 1);
    CHECK_FALSE(blocked[0].available);
    CHECK(blocked[0].unavailable_reason.find("未过信任门") != std::string::npos);

    // 批了那枚哈希:同一目录,门开了,Agent 可用——content 件两边都挂着。
    const std::string hash = mount.entries[0].content_hash;
    PackageTrustSnapshot snapshot;
    snapshot.keys.insert("demo.pkg\n" + hash);
    PackageMountInput trusted_input;
    trusted_input.scan.dev_roots.push_back(layer);
    trusted_input.trust = snapshot;
    PackageMount trusted_mount = BuildPackageMount(trusted_input);
    REQUIRE(trusted_mount.entries.size() == 1);
    CHECK(trusted_mount.entries[0].code_trust == CodeTrustStatus::Trusted);
    const std::vector<lubancode::agent::PackagedAgentEntry> released =
        MountAgentEntries(trusted_mount);
    REQUIRE(released.size() == 1);
    CHECK(released[0].available);
    CHECK(released[0].unavailable_reason.empty());
}

TEST_CASE("AgentCatalog:依赖未信任 MCP 的包内 Agent unavailable,FirstError 注缘由") {
    TempDir temp;
    const fs::path layer = temp.Get() / "dev-packages";
    MakeCodePackage(layer, "demo", "demo.pkg");
    PackageMountInput input;
    input.scan.dev_roots.push_back(layer);
    const PackageMount mount = BuildPackageMount(input);

    lubancode::agent::AgentCatalogScanRoots roots;
    roots.packaged = MountAgentEntries(mount);
    const lubancode::agent::AgentCatalog catalog = lubancode::agent::LoadAgentCatalog(roots);
    const auto* entry = catalog.Find("demo.pkg:probe");
    REQUIRE(entry != nullptr);
    CHECK_FALSE(entry->available);
    CHECK(entry->FirstError().find("未过信任门") != std::string::npos);
    CHECK(entry->FirstError().find("demo.pkg:browser") != std::string::npos);
}

TEST_CASE("WorkflowCatalog:依赖未信任 code 的包内 Workflow broken 并注缘由") {
    TempDir temp;
    const fs::path layer = temp.Get() / "dev-packages";
    // full-stack 夹具:smoke-test 直引 plugin 工具、又经 browser-tester 引 MCP。
    PackageMountInput input;
    input.scan.dev_roots.push_back(kFixturesRoot);
    input.scan.current_lubancode = lubancode::package::ParseSemVer("0.26.95").value_or(SemVer{});
    input.scan.current_platform = "windows";
    const PackageMount mount = BuildPackageMount(input);

    const lubancode::workflow::Catalog catalog =
        lubancode::workflow::LoadCatalog(std::nullopt, std::nullopt, MountWorkflowSources(mount));
    // Find 按既有规矩跳过 broken 条目(与 standalone 坏件同款:run 说找不到,
    // list 里见状态)。这里直接翻 entries 断言。
    const lubancode::workflow::CatalogEntry* entry = nullptr;
    for (const auto& candidate : catalog.entries) {
        if (candidate.package_id == "moontide.full-stack" &&
            candidate.definition.id == "moontide.full-stack:smoke-test") {
            entry = &candidate;
        }
    }
    REQUIRE(entry != nullptr);
    CHECK(entry->broken);
    REQUIRE_FALSE(entry->issues.empty());
    CHECK(entry->issues.front().message.find("未过信任门") != std::string::npos);
}

// ---------------------------------------------------------------------------
// 审批材料与账务(五样回执)
// ---------------------------------------------------------------------------

TEST_CASE("TrustPackage:full-stack 五样回执 + 落账;幂等;重启生效话在") {
    const PackageRecord record = AnalyzeDev(kFixturesRoot / "full-stack");
    REQUIRE(record.valid);
    auto [store, err] = PackageTrustStore::Load(std::nullopt);
    REQUIRE(err == std::nullopt);

    const PackageTrustActionResult result = TrustPackage(record, &store);
    REQUIRE(result.ok);
    REQUIRE(result.lines.size() >= 6);
    // 一样:身份(id、版本、层)。二样:包根。三样:逐件命令面。四样:文件数
    // + 完整指纹。五样:结论。
    CHECK(result.lines[0].find("moontide.full-stack") != std::string::npos);
    CHECK(result.lines[0].find("0.1.0") != std::string::npos);
    CHECK(result.lines[1].rfind("包根: ", 0) == 0);
    std::string joined;
    for (const std::string& line : result.lines) joined += line + "\n";
    CHECK(joined.find("插件 moontide.full-stack:dom-analyzer") != std::string::npos);
    CHECK(joined.find("MCP moontide.full-stack:browser") != std::string::npos);
    CHECK(joined.find("命令: node ${package_dir}/mcp/browser/server.js") != std::string::npos);
    CHECK(joined.find("BROWSER_TOKEN=${env:BROWSER_TOKEN}") != std::string::npos);
    CHECK(joined.find("网络: 不出网") != std::string::npos);
    CHECK(joined.find("plugin__moontide%2Efull-stack%2Edom-analyzer__inspect") !=
          std::string::npos);  // 契约:将新增的工具名逐枚亮出
    CHECK(joined.find("文件 16 个,完整内容指纹:") != std::string::npos);
    CHECK(joined.find(record.inventory.content_hash) != std::string::npos);
    CHECK(joined.find("已信任,重启后生效") != std::string::npos);
    CHECK(joined.find("改一个字节") != std::string::npos);
    CHECK(store.IsTrusted("moontide.full-stack", record.inventory.content_hash));

    // 幂等:同枚哈希重批只回执,不翻账。
    const PackageTrustActionResult again = TrustPackage(record, &store);
    REQUIRE(again.ok);
    CHECK(store.trusted_count() == 1);

    // 账面字段:哪个包、哪个版本、哪枚哈希、何时批的、批时哪层。
    const auto latest = store.Latest("moontide.full-stack");
    REQUIRE(latest.has_value());
    CHECK(latest->version == "0.1.0");
    CHECK(latest->content_hash == record.inventory.content_hash);
    CHECK(latest->scope == "dev");
    CHECK_FALSE(latest->trusted_at_unix.empty());
}

TEST_CASE("TrustPackage:content-only 不进门不记账;user/official 不记账;坏包明拒") {
    auto [store, err] = PackageTrustStore::Load(std::nullopt);
    REQUIRE(err == std::nullopt);

    const PackageRecord content_only = AnalyzeDev(kFixturesRoot / "minimal-content-only");
    REQUIRE(content_only.valid);
    const PackageTrustActionResult skip = TrustPackage(content_only, &store);
    REQUIRE(skip.ok);
    CHECK(skip.lines.back().find("不经信任门") != std::string::npos);
    CHECK(store.trusted_count() == 0);

    PackageCandidate user = DevCandidate(kFixturesRoot / "full-stack");
    user.scope = PackageScope::User;
    const PackageRecord placed =
        AnalyzePackage(user, ScanOptions{}, PackageRefIndex{}, ExternalNamespaces{});
    const PackageTrustActionResult exempt = TrustPackage(placed, &store);
    REQUIRE(exempt.ok);
    CHECK(exempt.lines.back().find("已安装来源") != std::string::npos);
    CHECK(store.trusted_count() == 0);

    // 坏包:审批材料出不来就批不了。
    const PackageRecord broken = AnalyzeDev(kFixturesRoot / "broken" / "bad-names");
    REQUIRE_FALSE(broken.valid);
    const PackageTrustActionResult refused = TrustPackage(broken, &store);
    CHECK_FALSE(refused.ok);
    CHECK(refused.error.find("批不了") != std::string::npos);

    // 没有账本(找不到主目录):明拒。
    const PackageTrustActionResult no_store = TrustPackage(content_only, nullptr);
    CHECK_FALSE(no_store.ok);
}

TEST_CASE("UntrustPackage:销账带指纹;没批过如实说") {
    const PackageRecord record = AnalyzeDev(kFixturesRoot / "full-stack");
    auto [store, err] = PackageTrustStore::Load(std::nullopt);
    REQUIRE(err == std::nullopt);
    REQUIRE(TrustPackage(record, &store).ok);

    const PackageTrustActionResult undone = UntrustPackage(record, &store);
    REQUIRE(undone.ok);
    std::string joined;
    for (const std::string& line : undone.lines) joined += line + "\n";
    CHECK(joined.find("已销信任 1 条") != std::string::npos);
    CHECK(joined.find(record.inventory.content_hash) != std::string::npos);
    CHECK(joined.find("/package trust moontide.full-stack") != std::string::npos);
    CHECK(store.trusted_count() == 0);

    const PackageTrustActionResult empty = UntrustPackage(record, &store);
    REQUIRE(empty.ok);
    CHECK(empty.lines.back().find("没有信任账") != std::string::npos);
}

// ---------------------------------------------------------------------------
// 文件变动让旧信任失效(验收线:改一行即失效,拦下并指路重批)
// ---------------------------------------------------------------------------

TEST_CASE("文件变动:改一个字节哈希即变,旧信任失效,挂载拦下指路重批") {
    TempDir temp;
    const fs::path root = temp.Get() / "demo";
    MakeCodePackage(temp.Get(), "demo", "demo.pkg");

    const PackageRecord before = AnalyzeDev(root);
    REQUIRE(before.valid);
    auto [store, err] = PackageTrustStore::Load(std::nullopt);
    REQUIRE(err == std::nullopt);
    REQUIRE(TrustPackage(before, &store).ok);
    REQUIRE(store.IsTrusted("demo.pkg", before.inventory.content_hash));

    // 动一个字节(README 加一行,不在任何组件目录里也照算——盘的是整包)。
    WriteFile(root / "README.md", "# demo\n");
    const PackageRecord after = AnalyzeDev(root);
    CHECK(after.inventory.content_hash != before.inventory.content_hash);
    CHECK_FALSE(store.IsTrusted("demo.pkg", after.inventory.content_hash));

    // 状态话:指路重批,不静默放行也不静默吞掉。
    const std::string status = DescribeTrustStatus(after.inventory, &store);
    CHECK(status.find("信任已失效") != std::string::npos);
    CHECK(status.find("/package trust demo.pkg") != std::string::npos);

    // 挂载门:快照里那枚旧哈希打不开新内容的门。
    PackageTrustSnapshot stale = store.Snapshot();
    CHECK(AnalyzeDev(root, &stale).code_trust == CodeTrustStatus::PendingTrust);

    // 改回去:哈希复原,旧账自然重新对上(批的是内容,不是时刻)。
    fs::remove(root / "README.md");
    CHECK(store.IsTrusted("demo.pkg", AnalyzeDev(root).inventory.content_hash));
}

TEST_CASE("DescribeTrustStatus:未信任/已信任/免审层各有一句") {
    const PackageRecord record = AnalyzeDev(kFixturesRoot / "full-stack");
    CHECK(DescribeTrustStatus(record.inventory, nullptr).find("未信任") != std::string::npos);
    CHECK(DescribeTrustStatus(record.inventory, nullptr).find("/package trust") != std::string::npos);

    auto [store, err] = PackageTrustStore::Load(std::nullopt);
    REQUIRE(err == std::nullopt);
    store.SetTrusted("moontide.full-stack", "0.1.0", record.inventory.content_hash, "dev");
    CHECK(DescribeTrustStatus(record.inventory, &store).find("已信任") != std::string::npos);

    PackageInventory content_only = AnalyzeDev(kFixturesRoot / "minimal-content-only").inventory;
    CHECK(DescribeTrustStatus(content_only, nullptr).find("不经信任门") != std::string::npos);
}

// ---------------------------------------------------------------------------
// 指纹共用底座(Plugin 与 Package 同一把尺)
// ---------------------------------------------------------------------------

TEST_CASE("指纹底座:Plugin v1 委托后行为照旧——稳定、敏感;Package v1 材料同源") {
    TempDir temp;
    const fs::path plugin_dir = temp.Get() / "probe-plugin";
    WriteFile(plugin_dir / "plugin.json", "{\"id\": \"probe\"}");
    WriteFile(plugin_dir / "runner.py", "print('hi')\n");

    const auto first = lubancode::runtime::ComputePluginContentHash(plugin_dir);
    REQUIRE(first.has_value());
    CHECK(lubancode::runtime::ComputePluginContentHash(plugin_dir).value() == *first);  // 稳定

    WriteFile(plugin_dir / "runner.py", "print('bye')\n");
    const auto changed = lubancode::runtime::ComputePluginContentHash(plugin_dir);
    REQUIRE(changed.has_value());
    CHECK(*changed != *first);  // 改一字节即变

    // 门面与底座是同一份:两枚函数同目录同果。
    CHECK(lubancode::platform::PluginDirFingerprintV1(plugin_dir).value() == *changed);

    // Package 材料的形状:头行 + 逐文件账(直接喂底座,出 64 位 hex)。
    std::vector<lubancode::platform::LedgerFile> ledger;
    ledger.push_back({"package.yaml", 10, std::string(64, 'a')});
    CHECK(lubancode::platform::PackageLedgerFingerprintV1(ledger).size() == 64);
}
