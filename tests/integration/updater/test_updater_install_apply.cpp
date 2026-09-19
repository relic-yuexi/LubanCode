// 更新助手 C++ 化·批三第②单:盘面扫描 / 白名单 apply 落地 / 平铺交接册
// (真文件系统集成)。覆盖:
//   - scan:受管树+根件全量入账、树内目录入账、FoldKey 键、记录件与根级
//     普通目录不进账、abspath 绝对、sha256 惰性(nullopt);
//   - scan:reparse 探测(Windows 造 junction / POSIX 造 symlink)且不跟
//     进;孤儿 reparse 点名 conflict,清单内 reparse 不重复点名,用户
//     数据路径不入孤儿名单;
//   - HashDiskEntry:与 Sha256Hex 算法向量一致;目录/缺件返回 nullopt;
//   - apply:改过文件先备份再换新(备份内容对账)、内容相同零动作、
//     KeepUserData 不写不删、目录挡路 conflict(计划内+落地期)、父路径
//     被文件占整单报错、apply 后盘面逐文件 hash 对账;
//   - 账:install-state 往返(manifest 原件照抄、provenance=official-
//     package、version 空则照清单补、installed_at_utc 走 now seam);
//   - 交接:平铺备份完整性(树+根件,记录件排除;无可备返回空)、旧
//     EXE 挪 legacy + 新 EXE 落根位 + updater 树同步、重叠守卫、Windows
//     自持句柄锁死旧 EXE 明报"绝不强杀"松手重试成功、restore 回根位 +
//     摘 current.json(无 legacy 只摘指针)。
#include <doctest/doctest.h>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "platform/paths.hpp"
#include "platform/sha256.hpp"
#include "updater/flat_handover.hpp"
#include "updater/install_apply.hpp"
#include "updater/install_scan.hpp"
#include "updater/layout.hpp"
#include "updater/manifest.hpp"
#include "updater/paths.hpp"
#include "updater/whitelist_plan.hpp"

#ifdef _WIN32
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

namespace {

namespace fs = std::filesystem;
using namespace lubancode::updater;
using lubancode::platform::PathToUtf8;

const std::vector<std::string>& Trees() {
    static const std::vector<std::string> kTrees = {
        "skills", "docs", "web", "libexec", "licenses", "updater"};
    return kTrees;
}

const std::vector<std::string>& Records() {
    static const std::vector<std::string> kRecords = {"manifest.json", "install-state.json"};
    return kRecords;
}

fs::path TempRoot(const char* name) {
    const fs::path dir = fs::temp_directory_path() /
                         ("lubancode_updater_apply_" + std::string(name) + "_" +
                          std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir, ec);
    return dir;
}

void WriteBytes(const fs::path& file, const std::string& bytes) {
    std::error_code ec;
    fs::create_directories(file.parent_path(), ec);
    std::ofstream out(file, std::ios::binary | std::ios::trunc);
    out << bytes;
}

std::optional<std::string> ReadBytes(const fs::path& file) {
    std::error_code ec;
    if (!fs::is_regular_file(file, ec) || ec) return std::nullopt;
    std::ifstream in(file, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

std::string ShaOf(const std::string& bytes) {
    return lubancode::platform::Sha256Hex(bytes);
}

struct PkgFile {
    std::string path;
    std::string content;
};

// 造一枚包:文件落位 + manifest.json(schema 1,hash 现算)。返回清单文本。
std::string MakePkg(const fs::path& pkg_dir, const std::vector<PkgFile>& files) {
    nlohmann::json jf = nlohmann::json::array();
    for (const PkgFile& f : files) {
        WriteBytes(pkg_dir / lubancode::platform::Utf8ToPath(f.path), f.content);
        jf.push_back({{"path", f.path}, {"size", f.content.size()}, {"sha256", ShaOf(f.content)}});
    }
    nlohmann::json j = {{"schema", 1},
                        {"algo", "sha256"},
                        {"version", "9.9.9"},
                        {"platform", "test-os"},
                        {"channel", "stable"},
                        {"file_count", files.size()},
                        {"files", std::move(jf)}};
    const std::string text = j.dump(2) + "\n";
    WriteBytes(pkg_dir / "manifest.json", text);
    return text;
}

std::map<std::string, ManifestEntry> ManifestMapOf(const std::string& text) {
    std::vector<std::string> problems;
    auto manifest = ParseManifestText(text, &problems);
    REQUIRE_MESSAGE(manifest.has_value(), "测试清单不合格: ",
                    problems.empty() ? "?" : problems.front());
    return std::move(manifest->files);
}

// 扫盘 + 惰性 hash 补账(接线侧口诀:非链接非目录才算;算不出按链接归)。
std::map<std::string, DiskEntry> ScannedWithHashes(const fs::path& root) {
    auto disk = ScanInstallDisk(root, Trees(), Records());
    for (auto& [key, entry] : disk) {
        if (entry.is_dir || entry.reparse) {
            continue;
        }
        if (auto h = HashDiskEntry(entry.abspath)) {
            entry.sha256 = *h;
        } else {
            entry.reparse = true;  // 读不动:按非常规文件归(PS/python 口径)
        }
    }
    return disk;
}

const PlanEntry* FindEntry(const std::vector<PlanEntry>& plan, const std::string& path) {
    for (const PlanEntry& e : plan) {
        if (e.path == path) return &e;
    }
    return nullptr;
}

// Windows 造目录 junction(免特权);造不出返回 false(腿留 CI)。
bool MakeJunction(const fs::path& junction, const fs::path& target) {
#ifdef _WIN32
    const std::string command = "cmd /c mklink /J \"" + junction.string() + "\" \"" +
                                target.string() + "\" >NUL 2>&1";
    const int rc = std::system(command.c_str());
    return rc == 0 && fs::exists(junction);
#else
    (void)junction;
    (void)target;
    return false;
#endif
}

// POSIX 造目录 symlink;造不出(特权)返回 false。
bool MakeDirSymlink(const fs::path& link, const fs::path& target) {
#ifndef _WIN32
    std::error_code ec;
    fs::create_directory_symlink(target, link, ec);
    return !ec;
#else
    (void)link;
    (void)target;
    return false;
#endif
}

}  // namespace

// ---------------------------------------------------------------------------
// scan
// ---------------------------------------------------------------------------

TEST_CASE("scan: 受管树与根件全量入账,FoldKey 键,记录件与普通目录不进") {
    const fs::path root = TempRoot("scan_basic");
    WriteBytes(root / "skills" / "a.md", "A");
    WriteBytes(root / "skills" / "sub" / "b.md", "B");
    WriteBytes(root / "docs" / "c.md", "C");
    WriteBytes(root / "libexec" / "tool.sh", "T");
    WriteBytes(root / "lubancode.exe", "EXE");
    WriteBytes(root / "LICENSE", "L");
    WriteBytes(root / "Notes.TXT", "N");
    WriteBytes(root / "notes-user.txt", "u");
    WriteBytes(root / "manifest.json", "{}");
    WriteBytes(root / "install-state.json", "{}");
    fs::create_directories(root / "userdir");
    WriteBytes(root / "userdir" / "keep.txt", "K");

    const auto disk = ScanInstallDisk(root, Trees(), Records());

    REQUIRE(disk.count(FoldKey("skills/a.md")) == 1);
    REQUIRE(disk.count(FoldKey("skills/sub/b.md")) == 1);
    REQUIRE(disk.count(FoldKey("docs/c.md")) == 1);
    REQUIRE(disk.count(FoldKey("libexec/tool.sh")) == 1);
    REQUIRE(disk.count(FoldKey("lubancode.exe")) == 1);
    REQUIRE(disk.count(FoldKey("LICENSE")) == 1);
    REQUIRE(disk.count(FoldKey("Notes.TXT")) == 1);
    REQUIRE(disk.count(FoldKey("notes-user.txt")) == 1);
    // 记录件与根级普通目录:不在资产域
    CHECK(disk.count(FoldKey("manifest.json")) == 0);
    CHECK(disk.count(FoldKey("install-state.json")) == 0);
    CHECK(disk.count(FoldKey("userdir")) == 0);
    CHECK(disk.count(FoldKey("userdir/keep.txt")) == 0);

    const DiskEntry& tree_file = disk.at(FoldKey("skills/a.md"));
    CHECK(tree_file.abspath.is_absolute());
    CHECK_FALSE(tree_file.is_dir);
    CHECK_FALSE(tree_file.reparse);
    CHECK_FALSE(tree_file.sha256.has_value());  // 惰性:扫描不算 hash
    CHECK(disk.at(FoldKey("skills/sub")).is_dir);
    CHECK_FALSE(disk.at(FoldKey("skills/sub")).reparse);
    // 缺棵的树(web):不炸,没有条目
    CHECK(disk.count(FoldKey("web/anything")) == 0);
}

TEST_CASE("scan: 安装根不存在给空账") {
    const fs::path root = TempRoot("scan_missing") / "nope";
    const auto disk = ScanInstallDisk(root, Trees(), Records());
    CHECK(disk.empty());
}

TEST_CASE("scan: 链接件入账标 reparse 且不跟进;孤儿点名 conflict,清单内不点名") {
    const fs::path root = TempRoot("scan_reparse");
    fs::create_directories(root / "outside");
    WriteBytes(root / "outside" / "secret.txt", "S");
    WriteBytes(root / "skills" / "plain.md", "P");

    fs::path link = root / "skills" / "link";
    bool made = false;
#ifdef _WIN32
    made = MakeJunction(link, root / "outside");
#else
    made = MakeDirSymlink(link, root / "outside");
#endif
    if (!made) {
        MESSAGE("SKIP: 链接造不出(特权/平台),本机此腿留 CI");
        return;
    }

    const auto disk = ScanInstallDisk(root, Trees(), Records());
    REQUIRE(disk.count(FoldKey("skills/link")) == 1);
    const DiskEntry& entry = disk.at(FoldKey("skills/link"));
    CHECK(entry.reparse);
    // 不跟进:链接对面的内容绝不入账
    CHECK(disk.count(FoldKey("skills/link/secret.txt")) == 0);

    // 孤儿 reparse:清单没有它 -> 点名 ConflictReparse;用户数据路径不点
    const auto empty_map = ManifestMapOf(MakePkg(TempRoot("scan_reparse_pkg"), {}));
    const auto orphans = OrphanReparseConflicts(root, disk, empty_map);
    REQUIRE(orphans.size() == 1);
    CHECK(orphans[0].path == "skills/link");
    CHECK(orphans[0].action == Action::ConflictReparse);
    CHECK(orphans[0].reason == "盘面是链接/reparse,不写不删");

    // 清单内的 reparse:孤儿名单不重复点(决策表自己出 ConflictReparse)
    fs::path pkg = TempRoot("scan_reparse_pkg2");
    MakePkg(pkg, {{"skills/link", "whatever"}, {"skills/plain.md", "P"}});
    const auto new_map = ManifestMapOf(ReadBytes(pkg / "manifest.json").value());
    CHECK(OrphanReparseConflicts(root, disk, new_map).empty());
    const auto plan = BuildWhitelistPlan(new_map, disk);
    const PlanEntry* blocked = FindEntry(plan, "skills/link");
    REQUIRE(blocked != nullptr);
    CHECK(blocked->action == Action::ConflictReparse);
}

TEST_CASE("scan: 用户数据路径入账但决策标 KeepUserData,apply 不写不删") {
    const fs::path root = TempRoot("scan_userdata");
    WriteBytes(root / "config.toml", "USER-DATA");
    WriteBytes(root / ".env", "SECRET=1");
    fs::create_directories(root / ".lubancode" / "deep");
    WriteBytes(root / ".lubancode" / "deep" / "x.json", "{}");

    const auto disk = ScanInstallDisk(root, Trees(), Records());
    // 根级用户文件入账;.lubancode/ 是根级普通目录,不扫
    CHECK(disk.count(FoldKey("config.toml")) == 1);
    CHECK(disk.count(FoldKey(".env")) == 1);
    CHECK(disk.count(FoldKey(".lubancode/deep/x.json")) == 0);

    // 孤儿名单不点用户数据(用户数据闸在最前,PS/python 决策表同序)
    const auto empty_map = ManifestMapOf(MakePkg(TempRoot("scan_userdata_pkg"), {}));
    CHECK(OrphanReparseConflicts(root, disk, empty_map).empty());

    // 清单误收用户数据:决策 KeepUserData
    const fs::path pkg = TempRoot("scan_userdata_pkg2");
    MakePkg(pkg, {{"config.toml", "OFFICIAL-SHOULD-NOT-WRITE"}, {".env", "NOPE"}});
    const auto new_map = ManifestMapOf(ReadBytes(pkg / "manifest.json").value());
    const auto hashed = ScannedWithHashes(root);
    const auto plan = BuildWhitelistPlan(new_map, hashed);
    const PlanEntry* config = FindEntry(plan, "config.toml");
    REQUIRE(config != nullptr);
    CHECK(config->action == Action::KeepUserData);
    const PlanEntry* env = FindEntry(plan, ".env");
    REQUIRE(env != nullptr);
    CHECK(env->action == Action::KeepUserData);

    const auto applied = ApplyWhitelistPlan(plan, hashed, root, pkg, root / "backups" / "t-u");
    REQUIRE(applied.has_value());
    CHECK(ReadBytes(root / "config.toml").value() == "USER-DATA");
    CHECK(ReadBytes(root / ".env").value() == "SECRET=1");
    CHECK(applied->backed_up.empty());
    CHECK(applied->replaced.empty());
    CHECK(applied->installed.empty());
}

TEST_CASE("HashDiskEntry: 流式摘要与算法向量一致;目录/缺件返回空") {
    const fs::path root = TempRoot("hash");
    const std::string body = "The quick brown fox jumps over the lazy dog";
    WriteBytes(root / "f.bin", body);
    WriteBytes(root / "big.bin", std::string(200 * 1024, 'x'));  // 跨多个 64KB 块
    fs::create_directories(root / "adir");

    const auto h1 = HashDiskEntry(root / "f.bin");
    REQUIRE(h1.has_value());
    CHECK(*h1 == lubancode::platform::Sha256Hex(body));
    const auto h2 = HashDiskEntry(root / "big.bin");
    REQUIRE(h2.has_value());
    CHECK(*h2 == lubancode::platform::Sha256Hex(std::string(200 * 1024, 'x')));
    CHECK_FALSE(HashDiskEntry(root / "adir").has_value());
    CHECK_FALSE(HashDiskEntry(root / "missing.bin").has_value());
}

// ---------------------------------------------------------------------------
// apply
// ---------------------------------------------------------------------------

TEST_CASE("apply: 改过先备份再换新,内容相同零动作,新增直落,hash 逐文件对账") {
    const fs::path root = TempRoot("apply_basic");
    WriteBytes(root / "skills" / "a.md", "old-a");
    WriteBytes(root / "skills" / "same.md", "same");
    WriteBytes(root / "LICENSE", "old-license");

    const fs::path pkg = TempRoot("apply_basic_pkg");
    const std::string manifest_text = MakePkg(pkg, {{"skills/a.md", "new-a"},
                                                    {"skills/same.md", "same"},
                                                    {"LICENSE", "new-license"},
                                                    {"skills/new.md", "brand-new"},
                                                    {"skills/deep/dir/leaf.md", "leaf"}});
    const auto new_map = ManifestMapOf(manifest_text);
    const auto disk = ScannedWithHashes(root);
    const auto plan = BuildWhitelistPlan(new_map, disk);

    const PlanEntry* a = FindEntry(plan, "skills/a.md");
    const PlanEntry* same = FindEntry(plan, "skills/same.md");
    const PlanEntry* license = FindEntry(plan, "LICENSE");
    const PlanEntry* fresh = FindEntry(plan, "skills/new.md");
    REQUIRE(a != nullptr);
    REQUIRE(same != nullptr);
    REQUIRE(license != nullptr);
    REQUIRE(fresh != nullptr);
    REQUIRE(a->action == Action::Replace);
    REQUIRE(same->action == Action::SkipCurrent);
    REQUIRE(license->action == Action::Replace);
    REQUIRE(fresh->action == Action::InstallNew);

    const fs::path backup_root = root / "backups" / "txn-1";
    const auto applied = ApplyWhitelistPlan(plan, disk, root, pkg, backup_root);
    REQUIRE(applied.has_value());
    CHECK(applied->backup_root == backup_root);
    // 备份内容对账:备的是换新前的盘面内容
    CHECK(ReadBytes(backup_root / "skills" / "a.md").value() == "old-a");
    CHECK(ReadBytes(backup_root / "LICENSE").value() == "old-license");
    CHECK(std::find(applied->backed_up.begin(), applied->backed_up.end(), "skills/a.md") !=
          applied->backed_up.end());
    CHECK(std::find(applied->backed_up.begin(), applied->backed_up.end(), "LICENSE") !=
          applied->backed_up.end());
    CHECK(applied->conflicts.empty());

    // 换新与新增落地;内容相同件不动
    CHECK(ReadBytes(root / "skills" / "a.md").value() == "new-a");
    CHECK(ReadBytes(root / "LICENSE").value() == "new-license");
    CHECK(ReadBytes(root / "skills" / "new.md").value() == "brand-new");
    CHECK(ReadBytes(root / "skills" / "deep" / "dir" / "leaf.md").value() == "leaf");
    CHECK(ReadBytes(root / "skills" / "same.md").value() == "same");
    CHECK(std::find(applied->replaced.begin(), applied->replaced.end(), "skills/a.md") !=
          applied->replaced.end());
    CHECK(std::find(applied->installed.begin(), applied->installed.end(), "skills/new.md") !=
          applied->installed.end());

    // 逐文件 hash 对账:apply 后盘面即清单
    for (const auto& [key, entry] : new_map) {
        const auto h = HashDiskEntry(root / lubancode::platform::Utf8ToPath(entry.path));
        REQUIRE(h.has_value());
        CHECK_MESSAGE(*h == entry.sha256, "hash 对不上: ", entry.path);
    }
}

TEST_CASE("apply: 目录挡路 conflict(计划内与落地期),父路径被文件占整单报错") {
    SUBCASE("计划内:盘面目录挡住清单文件路径") {
        const fs::path root = TempRoot("apply_conflict_plan");
        fs::create_directories(root / "skills" / "blocked");
        const fs::path pkg = TempRoot("apply_conflict_pkg");
        const auto new_map = ManifestMapOf(MakePkg(pkg, {{"skills/blocked", "WANT-FILE"}}));
        const auto disk = ScannedWithHashes(root);
        const auto plan = BuildWhitelistPlan(new_map, disk);
        const PlanEntry* blocked = FindEntry(plan, "skills/blocked");
        REQUIRE(blocked != nullptr);
        REQUIRE(blocked->action == Action::ConflictKind);

        const auto applied = ApplyWhitelistPlan(plan, disk, root, pkg, root / "backups" / "t");
        REQUIRE(applied.has_value());
        REQUIRE(applied->conflicts.size() == 1);
        CHECK(applied->conflicts[0].path == "skills/blocked");
        CHECK(applied->conflicts[0].kind == "conflict-kind");
        CHECK(applied->conflicts[0].backup.find("backups") != std::string::npos);
        CHECK(applied->replaced.empty());
        CHECK(applied->installed.empty());
        // 目录原样在,没被写穿
        CHECK(fs::is_directory(root / "skills" / "blocked"));
    }

    SUBCASE("落地期:扫描后新出现的目录占位") {
        const fs::path root = TempRoot("apply_conflict_late");
        const auto disk = ScannedWithHashes(root);  // 先扫(空盘)
        const fs::path pkg = TempRoot("apply_conflict_late_pkg");
        const auto new_map = ManifestMapOf(MakePkg(pkg, {{"skills/late.md", "LATE"}}));
        const auto plan = BuildWhitelistPlan(new_map, disk);
        const PlanEntry* late = FindEntry(plan, "skills/late.md");
        REQUIRE(late != nullptr);
        REQUIRE(late->action == Action::InstallNew);

        fs::create_directories(root / "skills" / "late.md");  // 计划之后才冒出来的目录
        const auto applied = ApplyWhitelistPlan(plan, disk, root, pkg, root / "backups" / "t");
        REQUIRE(applied.has_value());
        REQUIRE(applied->conflicts.size() == 1);
        CHECK(applied->conflicts[0].path == "skills/late.md");
        CHECK(applied->conflicts[0].kind == "conflict-kind");
        CHECK(fs::is_directory(root / "skills" / "late.md"));
    }

    SUBCASE("父路径被文件占:整单报错不落地") {
        const fs::path root = TempRoot("apply_conflict_parent");
        WriteBytes(root / "docs", "I-AM-A-FILE");  // 树名被文件占了
        const auto disk = ScannedWithHashes(root);
        const fs::path pkg = TempRoot("apply_conflict_parent_pkg");
        const auto new_map = ManifestMapOf(MakePkg(pkg, {{"docs/x.md", "X"}}));
        const auto plan = BuildWhitelistPlan(new_map, disk);
        const PlanEntry* leaf = FindEntry(plan, "docs/x.md");
        REQUIRE(leaf != nullptr);
        REQUIRE(leaf->action == Action::InstallNew);

        const auto applied = ApplyWhitelistPlan(plan, disk, root, pkg, root / "backups" / "t");
        REQUIRE_FALSE(applied.has_value());
        CHECK(applied.error().find("拒绝写入") != std::string::npos);
        CHECK(ReadBytes(root / "docs").value() == "I-AM-A-FILE");
        CHECK_FALSE(fs::exists(root / "docs" / "x.md"));
    }
}

TEST_CASE("apply: 空计划开单只建空备份根;坏路径整单报错") {
    const fs::path root = TempRoot("apply_edge");
    const fs::path backup_root = root / "backups" / "t-empty";
    const auto applied = ApplyWhitelistPlan({}, {}, root, TempRoot("apply_edge_pkg"), backup_root);
    REQUIRE(applied.has_value());
    CHECK(fs::is_directory(backup_root));
    CHECK(applied->backed_up.empty());

    // 不合法路径(反斜杠/.. 等,ValidRelpath 拒):整单报错
    std::vector<PlanEntry> bad;
    bad.push_back({"..\\evil.txt", Action::InstallNew, false, "测试"});
    const auto refused =
        ApplyWhitelistPlan(bad, {}, root, TempRoot("apply_edge_pkg"), root / "backups" / "t-bad");
    REQUIRE_FALSE(refused.has_value());
    CHECK(refused.error().find("路径不合法") != std::string::npos);
}

TEST_CASE("apply+账: install-state 往返——manifest 原件照抄,provenance 记来源") {
    const fs::path root = TempRoot("backfill");
    const fs::path pkg = TempRoot("backfill_pkg");
    const std::string manifest_text =
        MakePkg(pkg, {{"skills/a.md", "A"}, {"LICENSE", "L"}});
    const auto paths = MakeLayoutPaths(root);

    InstallState state;
    state.channel = "stable";
    state.installer = "test-updater";
    state.transaction = "txn-9";
    state.source_asset_digest = "sha256:ab";
    state.version = "";  // 留空:照清单 version 补

    const auto filled =
        BackfillInstallState(paths, pkg, state, [] { return "2026-09-20T00:00:00Z"; });
    REQUIRE(filled.has_value());

    // manifest 原件逐字节照抄
    CHECK(ReadBytes(root / "manifest.json").value() == manifest_text);

    // install-state 往返
    const auto read = ReadInstallState(paths);
    REQUIRE(read.has_value());
    CHECK(read->version == "9.9.9");
    REQUIRE(read->platform.has_value());  // doctest 禁 CHECK 里写 &&,拆两条
    CHECK(*read->platform == "test-os");
    CHECK(read->channel == "stable");
    CHECK(read->installer == "test-updater");
    CHECK(read->transaction == "txn-9");
    CHECK(read->source_asset_digest == "sha256:ab");
    nlohmann::json expected_manifest = nlohmann::json::parse(manifest_text);
    CHECK(read->manifest == expected_manifest);

    const auto raw = ReadJsonFileTolerant(paths.state);
    REQUIRE(raw.has_value());
    CHECK((*raw)["manifest_provenance"] == "official-package");
    CHECK((*raw)["installed_at_utc"] == "2026-09-20T00:00:00Z");

    // 包里没有 manifest.json:拒绝回填(generated-from-source 是 install.ps1
    // 本地开发口子,C++ 更新器流程不走)
    const auto refused = BackfillInstallState(paths, TempRoot("backfill_empty_pkg"), state);
    REQUIRE_FALSE(refused.has_value());
}

// ---------------------------------------------------------------------------
// 交接(flat)
// ---------------------------------------------------------------------------

TEST_CASE("flat: 完整备份——树与根件原样进 flat,记录件排除;无可备返回空") {
    const fs::path root = TempRoot("flat_backup");
    WriteBytes(root / "skills" / "top.md", "TOP");
    WriteBytes(root / "skills" / "nested" / "deep.md", "DEEP");
    WriteBytes(root / "docs" / "only.md", "ONLY");
    WriteBytes(root / "LICENSE", "L");
    WriteBytes(root / "config.toml", "USER");
    WriteBytes(root / "manifest.json", "{}");
    WriteBytes(root / "install-state.json", "{}");
    fs::create_directories(root / "userdir");
    WriteBytes(root / "userdir" / "u.txt", "U");

    const auto backed = FlatFullBackup(root, Trees(), Records(), "txn-f");
    REQUIRE(backed.has_value());
    REQUIRE(backed->has_value());
    const fs::path flat = root / "backups" / "txn-f" / "flat";
    CHECK(**backed == flat);
    CHECK(ReadBytes(flat / "skills" / "top.md").value() == "TOP");
    CHECK(ReadBytes(flat / "skills" / "nested" / "deep.md").value() == "DEEP");
    CHECK(ReadBytes(flat / "docs" / "only.md").value() == "ONLY");
    CHECK(ReadBytes(flat / "LICENSE").value() == "L");
    CHECK(ReadBytes(flat / "config.toml").value() == "USER");  // 根级用户文件也备
    CHECK_FALSE(fs::exists(flat / "manifest.json"));
    CHECK_FALSE(fs::exists(flat / "install-state.json"));
    CHECK_FALSE(fs::exists(flat / "userdir"));

    // 空安装根:无事可备
    const auto nothing = FlatFullBackup(TempRoot("flat_backup_empty"), Trees(), Records(), "t");
    REQUIRE(nothing.has_value());
    CHECK_FALSE(nothing->has_value());
}

TEST_CASE("flat: 旧 EXE 挪 legacy,新 EXE 落根位,updater 树同步;重叠拒绝") {
    const fs::path root = TempRoot("flat_handover");
    const auto paths = MakeLayoutPaths(root);
    WriteBytes(paths.exe, "OLD-EXE");

    const fs::path version_dir = root / "versions" / "0.1.0";
    const fs::path version_exe = version_dir / paths.exe.filename();
    WriteBytes(version_exe, "NEW-EXE");
    WriteBytes(version_dir / "updater" / "u.txt", "UPDATER-TREE");
    WriteBytes(version_dir / "updater" / "sub" / "v.txt", "UPDATER-SUB");

    const auto done = HandoverFlatLauncher(paths, "txn-h", version_dir);
    REQUIRE(done.has_value());
    CHECK(ReadBytes(paths.backups / "txn-h" / "legacy" / paths.exe.filename()).value() == "OLD-EXE");
    CHECK(ReadBytes(paths.exe).value() == "NEW-EXE");
    CHECK(ReadBytes(paths.updater / "u.txt").value() == "UPDATER-TREE");
    CHECK(ReadBytes(paths.updater / "sub" / "v.txt").value() == "UPDATER-SUB");

    // 安装根与版本目录重叠:拒绝交接(python 同款守卫)
    const auto refused = HandoverFlatLauncher(paths, "txn-h", root);
    REQUIRE_FALSE(refused.has_value());
    CHECK(refused.error().find("重叠") != std::string::npos);
}

#ifdef _WIN32
TEST_CASE("flat: Windows 自持句柄锁死旧 EXE——明报不强杀,松手重试成功") {
    const fs::path root = TempRoot("flat_locked");
    const auto paths = MakeLayoutPaths(root);
    WriteBytes(paths.exe, "OLD-EXE");
    const fs::path version_dir = root / "versions" / "0.1.0";
    WriteBytes(version_dir / paths.exe.filename(), "NEW-EXE");

    // 无共享位打开(比运行中映像更狠的锁):rename 必须失败,且明报不强杀
    HANDLE held = CreateFileW(paths.exe.wstring().c_str(), GENERIC_READ, /*share=*/0,
                              nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    REQUIRE(held != INVALID_HANDLE_VALUE);
    const auto blocked = HandoverFlatLauncher(paths, "txn-l", version_dir);
    REQUIRE_FALSE(blocked.has_value());
    CHECK(blocked.error().find("绝不强杀") != std::string::npos);
    CHECK(ReadBytes(paths.exe).value() == "OLD-EXE");  // 没被强杀/强删
    CHECK_FALSE(fs::exists(paths.backups / "txn-l" / "legacy" / paths.exe.filename()));

    CloseHandle(held);
    const auto retried = HandoverFlatLauncher(paths, "txn-l", version_dir);
    REQUIRE(retried.has_value());
    CHECK(ReadBytes(paths.backups / "txn-l" / "legacy" / paths.exe.filename()).value() == "OLD-EXE");
    CHECK(ReadBytes(paths.exe).value() == "NEW-EXE");
}
#endif

TEST_CASE("flat: restore 回根位并摘 current.json;无 legacy 只摘指针") {
    const fs::path root = TempRoot("flat_restore");
    const auto paths = MakeLayoutPaths(root);
    WriteBytes(paths.exe, "OLD-EXE");
    const fs::path version_dir = root / "versions" / "0.1.0";
    WriteBytes(version_dir / paths.exe.filename(), "NEW-EXE");

    REQUIRE(HandoverFlatLauncher(paths, "txn-r", version_dir).has_value());
    WriteCurrent(paths, "0.1.0", std::nullopt, "txn-r");
    REQUIRE(fs::exists(paths.current));

    const auto restored = RestoreFlatLegacy(paths, "txn-r");
    REQUIRE(restored.has_value());
    CHECK(*restored);
    CHECK(ReadBytes(paths.exe).value() == "OLD-EXE");  // 旧 EXE 回根位
    CHECK(ReadBytes(paths.backups / "txn-r" /
                    ("launcher-parked-" + PathToUtf8(paths.exe.filename())))
              .value() == "NEW-EXE");
    CHECK_FALSE(fs::exists(paths.current));  // 指针摘掉

    // legacy 没了(restore 是 copy,手动清空模拟用尽):不动根位,只保证
    // 指针不在
    std::error_code rm_legacy;
    fs::remove(paths.backups / "txn-r" / "legacy" / paths.exe.filename(), rm_legacy);
    WriteCurrent(paths, "0.1.0", std::nullopt, "txn-r");
    const auto again = RestoreFlatLegacy(paths, "txn-r");
    REQUIRE(again.has_value());
    CHECK_FALSE(*again);
    CHECK(ReadBytes(paths.exe).value() == "OLD-EXE");
    CHECK_FALSE(fs::exists(paths.current));
}
