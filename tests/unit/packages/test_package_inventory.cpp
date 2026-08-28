// 统一 Package 封装单阶段 1:四层扫描、稳定盘点、内容哈希、路径越界、
// 近似目录名与 /package 参数解析的单测。TempDir 现造目录树,全离线;
// symlink 不真建(Windows 要特权),越界路径直接喂校验函数——与仓库现有
// 测试的处理路数一致。
//
// 测试账对齐单子 §十六:四层优先级、同 id 遮蔽、文件枚举稳定、哈希跨
// 重复扫描不漂、内容改动必改哈希、空 Package/近似目录名/未知顶层目录、
// symlink/junction/../绝对路径逃逸(检测逻辑)。

#include <doctest/doctest.h>

#include <filesystem>
#include <fstream>
#include <string>

#include "app/commands/package_commands.hpp"
#include "package/inventory.hpp"
#include "package/manifest.hpp"

using namespace lubancode::package;

namespace {

namespace fs = std::filesystem;

class TempDir {
public:
    TempDir() {
        dir_ = fs::temp_directory_path() /
               ("lubancode_package_test_" + std::to_string(reinterpret_cast<std::uintptr_t>(this)));
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

// 造一只五脏俱全的样例包(id 可指定,版本可指定)。
fs::path MakeFullPackage(const fs::path& parent, const std::string& dir,
                         const std::string& id, const std::string& version = "0.1.0") {
    const fs::path root = parent / dir;
    WriteFile(root / "package.yaml",
              "schema: 1\nid: " + id + "\nversion: " + version +
                  "\nname: Sample\ndescription: sample package.\n");
    WriteFile(root / "agents" / "tester.yaml", "id: tester\n");
    WriteFile(root / "prompts" / "profiles" / "tester" / "core" / "10-identity.md", "identity");
    WriteFile(root / "skills" / "browser-testing" / "SKILL.md", "---\nname: browser-testing\n---\nbody");
    WriteFile(root / "workflows" / "smoke-test" / "workflow.yaml", "id: smoke-test\n");
    WriteFile(root / "plugins" / "dom-analyzer" / "plugin.json", "{}");
    WriteFile(root / "plugins" / "dom-analyzer" / "runner.py", "print('hi')");
    WriteFile(root / "mcp" / "browser" / "mcp.yaml", "schema: 1\nid: browser\n");
    WriteFile(root / "mcp" / "browser" / "server.js", "console.log(1)");
    WriteFile(root / "assets" / "logo.svg", "<svg/>");
    WriteFile(root / "docs" / "usage.md", "usage");
    WriteFile(root / "README.md", "readme");
    return root;
}

PackageCandidate DirectCandidate(const fs::path& root, const std::string& dir_name) {
    PackageCandidate candidate;
    candidate.scope = PackageScope::Dev;
    candidate.layer_root = root.parent_path();
    candidate.package_root = root;
    candidate.dir_name = dir_name;
    return candidate;
}

}  // namespace

// ---------------------------------------------------------------------------
// 四层扫描与优先级
// ---------------------------------------------------------------------------

TEST_CASE("扫描.四层优先级与遮蔽账") {
    TempDir tmp;
    // 同一只 id 摆四层:dev/project/user/official 各一份,版本不同。
    MakeFullPackage(tmp.Get() / "dev", "pkg", "demo.pkg", "9.0.0");
    MakeFullPackage(tmp.Get() / "project", "pkg", "demo.pkg", "2.0.0");
    MakeFullPackage(tmp.Get() / "user", "pkg", "demo.pkg", "1.0.0");
    MakeFullPackage(tmp.Get() / "official", "pkg", "demo.pkg", "0.1.0");

    ScanOptions options;
    options.dev_roots.push_back(tmp.Get() / "dev");
    options.project_root = tmp.Get() / "project";
    options.user_root = tmp.Get() / "user";
    options.official_root = tmp.Get() / "official";

    const std::vector<PackageCandidate> candidates = ScanPackages(options);
    REQUIRE(candidates.size() == 4);
    // 优先级高的排前:dev > project > user > official。
    CHECK(candidates[0].scope == PackageScope::Dev);
    CHECK(candidates[1].scope == PackageScope::Project);
    CHECK(candidates[2].scope == PackageScope::User);
    CHECK(candidates[3].scope == PackageScope::Official);
    CHECK(candidates[0].manifest->version.text == "9.0.0");
}

TEST_CASE("扫描.缺层与空层安静跳过") {
    ScanOptions options;  // 全空
    CHECK(ScanPackages(options).empty());

    TempDir tmp;
    fs::create_directories(tmp.Get() / "packages");
    options.user_root = tmp.Get() / "packages";  // 空目录
    CHECK(ScanPackages(options).empty());
}

TEST_CASE("扫描.缺清单的子目录进账") {
    TempDir tmp;
    fs::create_directories(tmp.Get() / "packages" / "no-manifest");
    WriteFile(tmp.Get() / "packages" / "has-manifest" / "package.yaml",
              "schema: 1\nid: a.b\nversion: 0.1.0\nname: n\ndescription: d\n");

    ScanOptions options;
    options.user_root = tmp.Get() / "packages";
    const std::vector<PackageCandidate> candidates = ScanPackages(options);
    REQUIRE(candidates.size() == 2);
    bool saw_broken = false;
    for (const auto& candidate : candidates) {
        if (candidate.dir_name == "no-manifest") {
            saw_broken = true;
            REQUIRE(candidate.manifest_error.has_value());
            CHECK(candidate.manifest_error->detail.find("缺 package.yaml") != std::string::npos);
        }
    }
    CHECK(saw_broken);
}

// ---------------------------------------------------------------------------
// 盘点
// ---------------------------------------------------------------------------

TEST_CASE("盘点.六类组件与canonical_id") {
    TempDir tmp;
    const fs::path root = MakeFullPackage(tmp.Get(), "pkg", "moontide.demo");

    const PackageInventory inventory = BuildPackageInventory(DirectCandidate(root, "pkg"));
    CHECK(inventory.valid);
    CHECK(inventory.manifest_ok);
    REQUIRE(inventory.agents.size() == 1);
    CHECK(inventory.agents[0].local_id == "tester");
    CHECK(inventory.agents[0].canonical_id == "moontide.demo:tester");
    REQUIRE(inventory.prompt_profiles.size() == 1);
    CHECK(inventory.prompt_profiles[0].canonical_id == "moontide.demo:tester");
    CHECK(inventory.prompt_profiles[0].rel_path == "prompts/profiles/tester");
    REQUIRE(inventory.skills.size() == 1);
    CHECK(inventory.skills[0].canonical_id == "moontide.demo:browser-testing");
    REQUIRE(inventory.workflows.size() == 1);
    CHECK(inventory.workflows[0].canonical_id == "moontide.demo:smoke-test");
    REQUIRE(inventory.plugins.size() == 1);
    CHECK(inventory.plugins[0].canonical_id == "moontide.demo:dom-analyzer");
    REQUIRE(inventory.mcp_servers.size() == 1);
    CHECK(inventory.mcp_servers[0].canonical_id == "moontide.demo:browser");
}

TEST_CASE("盘点.缺入口文件只警告不吞") {
    TempDir tmp;
    const fs::path root = tmp.Get() / "pkg";
    WriteFile(root / "package.yaml",
              "schema: 1\nid: a.b\nversion: 0.1.0\nname: n\ndescription: d\n");
    WriteFile(root / "skills" / "no-entry" / "notes.md", "x");       // 缺 SKILL.md
    WriteFile(root / "workflows" / "no-entry" / "readme.txt", "x");  // 缺 workflow.yaml
    WriteFile(root / "plugins" / "no-entry" / "runner.py", "x");     // 缺 plugin.json
    WriteFile(root / "mcp" / "no-entry" / "server.js", "x");         // 缺 mcp.yaml

    const PackageInventory inventory = BuildPackageInventory(DirectCandidate(root, "pkg"));
    CHECK(inventory.skills.empty());
    CHECK(inventory.workflows.empty());
    CHECK(inventory.plugins.empty());
    CHECK(inventory.mcp_servers.empty());
    // 缺入口是警告:整包仍 valid(不是静态错)。
    CHECK(inventory.valid);
    int entry_warnings = 0;
    for (const auto& diagnostic : inventory.diagnostics) {
        if (diagnostic.message.find("缺入口文件") != std::string::npos) {
            ++entry_warnings;
            CHECK(diagnostic.kind == PackageDiagnostic::Kind::Warning);
        }
    }
    CHECK(entry_warnings == 4);
}

TEST_CASE("盘点.排序稳定且哈希跨扫描不漂") {
    TempDir tmp;
    const fs::path root = MakeFullPackage(tmp.Get(), "pkg", "a.b");

    const PackageInventory first = BuildPackageInventory(DirectCandidate(root, "pkg"));
    // 同一只包盘两回:枚举次序变了(不可能,但保险),哈希必须一字不差。
    const PackageInventory second = BuildPackageInventory(DirectCandidate(root, "pkg"));
    CHECK(first.content_hash == second.content_hash);
    CHECK(first.content_hash.size() == 64);  // sha256 hex

    // 组件账也是稳定序(UTF-8 排序)。
    WriteFile(root / "agents" / "zzz.yaml", "1");
    WriteFile(root / "agents" / "aaa.yaml", "1");
    WriteFile(root / "agents" / "mmm.yaml", "1");
    const PackageInventory third = BuildPackageInventory(DirectCandidate(root, "pkg"));
    REQUIRE(third.agents.size() == 4);
    CHECK(third.agents[0].local_id == "aaa");
    CHECK(third.agents[1].local_id == "mmm");
    CHECK(third.agents[2].local_id == "tester");
    CHECK(third.agents[3].local_id == "zzz");
}

TEST_CASE("盘点.内容改动必改哈希") {
    TempDir tmp;
    const fs::path root = MakeFullPackage(tmp.Get(), "pkg", "a.b");
    const std::string before = BuildPackageInventory(DirectCandidate(root, "pkg")).content_hash;

    // 同名同长,只差一个字节。
    WriteFile(root / "skills" / "browser-testing" / "SKILL.md",
              "---\nname: browser-testing\n---\nbodX");
    const std::string after = BuildPackageInventory(DirectCandidate(root, "pkg")).content_hash;
    CHECK(before != after);

    // 只改文件名(内容不动)也得变:文件账进哈希。
    const std::string renamed = [&] {
        fs::rename(root / "docs" / "usage.md", root / "docs" / "manual.md");
        return BuildPackageInventory(DirectCandidate(root, "pkg")).content_hash;
    }();
    CHECK(after != renamed);
}

TEST_CASE("盘点.近似目录名与未知顶层") {
    TempDir tmp;
    const fs::path root = tmp.Get() / "pkg";
    WriteFile(root / "package.yaml",
              "schema: 1\nid: a.b\nversion: 0.1.0\nname: n\ndescription: d\n");
    WriteFile(root / "skill" / "x" / "SKILL.md", "x");        // 少个 s
    WriteFile(root / "workflow" / "y" / "workflow.yaml", "x");  // 少个 s
    WriteFile(root / "agent" / "z.yaml", "x");                 // 少个 s
    WriteFile(root / "random-stuff" / "keep.txt", "x");        // 真未知

    const PackageInventory inventory = BuildPackageInventory(DirectCandidate(root, "pkg"));
    int near_miss = 0;
    int unknown = 0;
    for (const auto& diagnostic : inventory.diagnostics) {
        if (diagnostic.message.find("疑似拼错") != std::string::npos) {
            ++near_miss;
            CHECK(diagnostic.kind == PackageDiagnostic::Kind::Warning);
        }
        if (diagnostic.message.find("未知顶层") != std::string::npos) {
            ++unknown;
            CHECK(diagnostic.kind == PackageDiagnostic::Kind::Info);
        }
    }
    CHECK(near_miss == 3);
    CHECK(unknown == 1);  // random-stuff 只是未知,不是近似
    // 近似名不吞组件:skill/ 不是 skills/,一件也不认。
    CHECK(inventory.skills.empty());
}

TEST_CASE("盘点.目录名与id对不上警告") {
    TempDir tmp;
    const fs::path root = tmp.Get() / "elsewhere";
    WriteFile(root / "package.yaml",
              "schema: 1\nid: moontide.demo\nversion: 0.1.0\nname: n\ndescription: d\n");

    const PackageInventory inventory = BuildPackageInventory(DirectCandidate(root, "elsewhere"));
    bool warned = false;
    for (const auto& diagnostic : inventory.diagnostics) {
        if (diagnostic.message.find("目录名与 id 对不上") != std::string::npos) {
            warned = true;
        }
    }
    CHECK(warned);

    // 目录名 = id 尾段(moontide.demo -> demo):不警告。
    const PackageInventory aligned = BuildPackageInventory(DirectCandidate(root, "demo"));
    for (const auto& diagnostic : aligned.diagnostics) {
        CHECK(diagnostic.message.find("目录名与 id 对不上") == std::string::npos);
    }
}

TEST_CASE("盘点.code_bearing分类") {
    TempDir tmp;
    // 纯内容包:六类目录但 plugins/mcp 为空,无脚本扩展名。
    const fs::path content_only = tmp.Get() / "content";
    WriteFile(content_only / "package.yaml",
              "schema: 1\nid: a.b\nversion: 0.1.0\nname: n\ndescription: d\n");
    WriteFile(content_only / "skills" / "s" / "SKILL.md", "x");
    WriteFile(content_only / "assets" / "data.txt", "x");
    const PackageInventory content = BuildPackageInventory(DirectCandidate(content_only, "content"));
    CHECK_FALSE(content.code_bearing());

    // 代码包:plugin 脚本与 mcp server 脚本都在。
    const fs::path code_pkg = MakeFullPackage(tmp.Get(), "code", "c.d");
    const PackageInventory code = BuildPackageInventory(DirectCandidate(code_pkg, "code"));
    CHECK(code.code_bearing());
    // plugins/、mcp/ 下的 4 个文件 + 无散落脚本扩展 → 计数 4。
    CHECK(code.code_bearing_file_count == 4);

    // 散落脚本也算:skills 里塞个 .py。
    WriteFile(content_only / "skills" / "s" / "helper.py", "print(1)");
    const PackageInventory mixed = BuildPackageInventory(DirectCandidate(content_only, "content"));
    CHECK(mixed.code_bearing());
    CHECK(mixed.code_bearing_file_count == 1);
}

TEST_CASE("盘点.清单解析失败仍盘全账") {
    TempDir tmp;
    const fs::path root = tmp.Get() / "broken";
    WriteFile(root / "package.yaml", "schema: 1\nid: A.B\nversion: 0.1.0\nname: n\ndescription: d\n");
    WriteFile(root / "skills" / "s" / "SKILL.md", "x");

    const PackageInventory inventory = BuildPackageInventory(DirectCandidate(root, "broken"));
    CHECK_FALSE(inventory.valid);
    CHECK_FALSE(inventory.manifest_ok);
    CHECK(inventory.package_id == "broken");  // 目录名兜底
    REQUIRE(inventory.skills.size() == 1);    // 组件账照盘
    CHECK(inventory.skills[0].canonical_id == "broken:s");
    bool has_error = false;
    for (const auto& diagnostic : inventory.diagnostics) {
        if (diagnostic.kind == PackageDiagnostic::Kind::Error) has_error = true;
    }
    CHECK(has_error);
}

TEST_CASE("盘点.标准目录位置上是文件则invalid") {
    TempDir tmp;
    const fs::path root = tmp.Get() / "pkg";
    WriteFile(root / "package.yaml",
              "schema: 1\nid: a.b\nversion: 0.1.0\nname: n\ndescription: d\n");
    WriteFile(root / "skills", "not a directory");  // 标准目录名被文件占了

    const PackageInventory inventory = BuildPackageInventory(DirectCandidate(root, "pkg"));
    CHECK_FALSE(inventory.valid);
}

TEST_CASE("盘点.compatibility诊断") {
    TempDir tmp;
    const fs::path root = tmp.Get() / "pkg";
    WriteFile(root / "package.yaml",
              "schema: 1\nid: a.b\nversion: 0.1.0\nname: n\ndescription: d\n"
              "compatibility:\n  lubancode: \">=0.27.0 <0.28.0\"\n  platforms:\n    - linux\n");

    ScanOptions options;
    options.current_lubancode = ParseSemVer("0.26.76");
    options.current_platform = "windows";
    REQUIRE(options.current_lubancode.has_value());

    const PackageInventory inventory =
        BuildPackageInventory(DirectCandidate(root, "pkg"), options);
    CHECK(inventory.valid);  // 兼容性是警告,不是静态错
    bool version_warned = false;
    bool platform_warned = false;
    for (const auto& diagnostic : inventory.diagnostics) {
        if (diagnostic.path == "compatibility.lubancode") version_warned = true;
        if (diagnostic.path == "compatibility.platforms") platform_warned = true;
    }
    CHECK(version_warned);
    CHECK(platform_warned);

    // 满足时不警告。
    ScanOptions ok_options;
    ok_options.current_lubancode = ParseSemVer("0.27.3");
    ok_options.current_platform = "linux";
    const PackageInventory ok = BuildPackageInventory(DirectCandidate(root, "pkg"), ok_options);
    for (const auto& diagnostic : ok.diagnostics) {
        CHECK(diagnostic.path != "compatibility.lubancode");
        CHECK(diagnostic.path != "compatibility.platforms");
    }
}

// ---------------------------------------------------------------------------
// 路径越界(检测逻辑直接喂构造串;真 symlink 要特权,不在单测里建)
// ---------------------------------------------------------------------------

TEST_CASE("越界.相对路径校验") {
    CHECK(CheckPackageRelativePath("agents/a.yaml") == PathIssue::None);
    CHECK(CheckPackageRelativePath("skills/s/SKILL.md") == PathIssue::None);
    CHECK(CheckPackageRelativePath("./agents/a.yaml") == PathIssue::None);
    CHECK(CheckPackageRelativePath("agents\\a.yaml") == PathIssue::None);  // Windows 写法放行

    CHECK(CheckPackageRelativePath("") == PathIssue::Empty);
    CHECK(CheckPackageRelativePath("../outside.yaml") == PathIssue::ParentEscape);
    CHECK(CheckPackageRelativePath("agents/../../outside.yaml") == PathIssue::ParentEscape);
    CHECK(CheckPackageRelativePath("..\\outside.yaml") == PathIssue::ParentEscape);
    CHECK(CheckPackageRelativePath("agents\\..\\..\\x") == PathIssue::ParentEscape);
    CHECK(CheckPackageRelativePath("/etc/passwd") == PathIssue::Absolute);
    CHECK(CheckPackageRelativePath("\\\\server\\share\\x") == PathIssue::Absolute);  // UNC
    CHECK(CheckPackageRelativePath("C:\\Windows\\x") == PathIssue::Absolute);        // 盘符
    CHECK(CheckPackageRelativePath("c:/x") == PathIssue::Absolute);
}

TEST_CASE("越界.真实路径解析后仍在包内") {
    TempDir tmp;
    const fs::path root = MakeFullPackage(tmp.Get(), "pkg", "a.b");
    // 盘点不跟随 symlink 目录:包内正常路径全在账上,包外路径根本进不来
    // (枚举自包根)。这里钉的是"规范路径都不含 .. 段"这条静态门。
    const PackageInventory inventory = BuildPackageInventory(DirectCandidate(root, "pkg"));
    CHECK(inventory.total_file_count >= 10);
    for (const char* probe : {"agents/tester.yaml", "skills/browser-testing/SKILL.md"}) {
        CHECK(CheckPackageRelativePath(probe) == PathIssue::None);
    }
}

// ---------------------------------------------------------------------------
// /package 参数解析(纯函数)
// ---------------------------------------------------------------------------

TEST_CASE("命令.参数解析") {
    {  // 裸敲 = list all
        const auto parsed = lubancode::app::ParsePackageCommand("");
        CHECK(parsed.action == lubancode::app::PackageCommandAction::List);
        CHECK_FALSE(parsed.scope_filter.has_value());
    }
    {
        const auto parsed = lubancode::app::ParsePackageCommand("list");
        CHECK(parsed.action == lubancode::app::PackageCommandAction::List);
        CHECK_FALSE(parsed.scope_filter.has_value());
    }
    {
        const auto parsed = lubancode::app::ParsePackageCommand("  list   user ");
        CHECK(parsed.action == lubancode::app::PackageCommandAction::List);
        REQUIRE(parsed.scope_filter.has_value());
        CHECK(*parsed.scope_filter == "user");
    }
    {
        const auto parsed = lubancode::app::ParsePackageCommand("list bogus");
        CHECK(parsed.action == lubancode::app::PackageCommandAction::Invalid);
    }
    {
        const auto parsed = lubancode::app::ParsePackageCommand("show moontide.browser-suite");
        CHECK(parsed.action == lubancode::app::PackageCommandAction::Show);
        CHECK(parsed.target == "moontide.browser-suite");
    }
    {
        const auto parsed = lubancode::app::ParsePackageCommand("doctor C:\\tmp\\pkg");
        CHECK(parsed.action == lubancode::app::PackageCommandAction::Doctor);
        CHECK(parsed.target == "C:\\tmp\\pkg");
    }
    {
        const auto parsed = lubancode::app::ParsePackageCommand("show");
        CHECK(parsed.action == lubancode::app::PackageCommandAction::Invalid);
    }
    {
        const auto parsed = lubancode::app::ParsePackageCommand("trust a.b");
        CHECK(parsed.action == lubancode::app::PackageCommandAction::Invalid);
        CHECK(parsed.bad_word == "trust");  // 阶段 1 没有 trust,明说不认
    }
}
