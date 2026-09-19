// 更新助手 C++ 化·批一第②单:白名单覆盖决策表册。覆盖:
//   - 基本六动作表驱动逐枚一例(skip-current/replace/install-new/
//     keep-user-data/conflict-kind/conflict-reparse),reason 文案按合同锁死;
//   - 盘面内容未知(sha256 nullopt/空串)按需 Replace 先备份;
//   - 对拍 install.legacy.tests.ps1(PR #140):改过官方件备份后换新、自建
//     技能与普通文件保留、清单误收 config.toml/.env 不覆盖、新版删掉的
//     文件仍保留(无 retire)、重复装零动作;
//   - FoldKey 碰撞:Windows/macOS 折叠口径下大小写异写同名归一路径,
//     Linux 口径下是两条路径;
//   - 包根文件表与受管树边界:表内/树内收,表外/树外/裸树名不收。
// 纯函数零 IO 的反证:DiskEntry.abspath 全册故意留空,决策照样出全——
// 它若敢碰盘,册子先红。
#include <doctest/doctest.h>

#include <algorithm>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "updater/manifest.hpp"
#include "updater/paths.hpp"
#include "updater/whitelist_plan.hpp"

namespace {

using lubancode::updater::Action;
using lubancode::updater::DiskEntry;
using lubancode::updater::ManifestEntry;
using lubancode::updater::PlanEntry;

std::string Sha64(char fill) {
    return std::string(64, fill);
}

ManifestEntry Entry(std::string_view path, const std::string& sha) {
    ManifestEntry e;
    e.path = std::string(path);
    e.size = sha.size();   // 决策表不读 size,随手给个数
    e.sha256 = sha;
    return e;
}

// manifest 侧:键 = FoldKey(path)(ParseManifestText 同款契约)。
std::map<std::string, ManifestEntry> NewMap(
    std::initializer_list<ManifestEntry> entries) {
    std::map<std::string, ManifestEntry> m;
    for (const ManifestEntry& e : entries) {
        m.emplace(lubancode::updater::FoldKey(e.path), e);
    }
    return m;
}

// 盘面侧三件套。abspath 一律不填(零 IO 反证,见文件头)。
DiskEntry DiskFile(const std::optional<std::string>& sha) {
    DiskEntry d;
    d.sha256 = sha;
    return d;
}

DiskEntry DiskDir() {
    DiskEntry d;
    d.is_dir = true;
    return d;
}

DiskEntry DiskReparseDir() {
    DiskEntry d;
    d.is_dir = true;
    d.reparse = true;
    return d;
}

const PlanEntry* Find(const std::vector<PlanEntry>& plan, std::string_view path) {
    const auto it = std::find_if(plan.begin(), plan.end(),
                                 [&](const PlanEntry& e) { return e.path == path; });
    return it == plan.end() ? nullptr : &*it;
}

std::size_t Count(const std::vector<PlanEntry>& plan, Action action) {
    return static_cast<std::size_t>(std::count_if(
        plan.begin(), plan.end(),
        [&](const PlanEntry& e) { return e.action == action; }));
}

}  // namespace

// ------------------------------------------------------------ 基本六动作 ---

TEST_CASE("基本六动作:表驱动逐枚一例,reason 按合同锁死") {
    const auto new_map = NewMap({
        Entry("docs/same.md", Sha64('1')),
        Entry("docs/diff.md", Sha64('2')),
        Entry("docs/new.md", Sha64('3')),
        Entry("config.toml", Sha64('4')),   // 清单误收用户数据
        Entry("docs/kind.md", Sha64('5')),
        Entry("docs/link.md", Sha64('6')),
    });
    std::map<std::string, DiskEntry> disk;
    disk.emplace(lubancode::updater::FoldKey("docs/same.md"), DiskFile(Sha64('1')));
    disk.emplace(lubancode::updater::FoldKey("docs/diff.md"), DiskFile(Sha64('e')));
    // config.toml 盘面也在:闸在白名单与盘面判定之前,照样 KeepUserData。
    disk.emplace(lubancode::updater::FoldKey("config.toml"), DiskFile(Sha64('u')));
    disk.emplace(lubancode::updater::FoldKey("docs/kind.md"), DiskDir());
    disk.emplace(lubancode::updater::FoldKey("docs/link.md"), DiskReparseDir());

    const auto plan = lubancode::updater::BuildWhitelistPlan(new_map, disk);

    const PlanEntry* same = Find(plan, "docs/same.md");
    REQUIRE(same != nullptr);
    CHECK(same->action == Action::SkipCurrent);
    CHECK_FALSE(same->backup);
    CHECK(same->reason == "已是新版内容");

    const PlanEntry* diff = Find(plan, "docs/diff.md");
    REQUIRE(diff != nullptr);
    CHECK(diff->action == Action::Replace);
    CHECK(diff->backup);
    CHECK(diff->reason == "同名文件内容有异,先备份再换新");

    const PlanEntry* fresh = Find(plan, "docs/new.md");
    REQUIRE(fresh != nullptr);
    CHECK(fresh->action == Action::InstallNew);
    CHECK_FALSE(fresh->backup);
    CHECK(fresh->reason == "新版新增");

    const PlanEntry* user = Find(plan, "config.toml");
    REQUIRE(user != nullptr);
    CHECK(user->action == Action::KeepUserData);
    CHECK_FALSE(user->backup);
    CHECK(user->reason == "用户配置或数据,不写不删");

    const PlanEntry* kind = Find(plan, "docs/kind.md");
    REQUIRE(kind != nullptr);
    CHECK(kind->action == Action::ConflictKind);
    CHECK_FALSE(kind->backup);
    CHECK(kind->reason == "盘面是目录,不写不删");

    const PlanEntry* link = Find(plan, "docs/link.md");
    REQUIRE(link != nullptr);
    CHECK(link->action == Action::ConflictReparse);
    CHECK_FALSE(link->backup);
    CHECK(link->reason == "盘面是链接/reparse,不写不删");

    CHECK(lubancode::updater::PlanBlocks(plan));
}

TEST_CASE("盘面内容未知:sha256 缺席或空串按需 Replace 先备份") {
    const auto new_map = NewMap({
        Entry("docs/n1.md", Sha64('1')),
        Entry("docs/n2.md", Sha64('2')),
    });
    std::map<std::string, DiskEntry> disk;
    disk.emplace(lubancode::updater::FoldKey("docs/n1.md"), DiskFile(std::nullopt));
    disk.emplace(lubancode::updater::FoldKey("docs/n2.md"), DiskFile(""));

    const auto plan = lubancode::updater::BuildWhitelistPlan(new_map, disk);
    REQUIRE(plan.size() == 2);
    for (const PlanEntry& e : plan) {
        CHECK(e.action == Action::Replace);
        CHECK(e.backup);
        CHECK(e.reason == "盘面内容未知,先备份再换新");
    }
    CHECK_FALSE(lubancode::updater::PlanBlocks(plan));
}

TEST_CASE("reparse 目录双标志:reparse 优先归 ConflictReparse") {
    // Build-FilePlan 的归类:is_dir 且非 reparse 才是 conflict-kind;链接目录
    // (is_dir+reparse 同真)走 else 分支归 conflict-reparse。
    const auto new_map = NewMap({Entry("skills/linked", Sha64('a'))});
    std::map<std::string, DiskEntry> disk;
    disk.emplace(lubancode::updater::FoldKey("skills/linked"), DiskReparseDir());

    const auto plan = lubancode::updater::BuildWhitelistPlan(new_map, disk);
    REQUIRE(plan.size() == 1);
    CHECK(plan[0].action == Action::ConflictReparse);
    CHECK(lubancode::updater::PlanBlocks(plan));
}

// ------------------------------------- 对拍 install.legacy.tests.ps1 场景 ---

TEST_CASE("对拍 legacy 册:旧安装白名单迁移(install.legacy.tests.ps1)") {
    // 场景复刻 PR #140 scripts/tests/install.legacy.tests.ps1:
    //   old 包(lubancode.exe + skills 三件 + config.toml)拷成 installed,
    //   装后 skills/edited.md 被改写、添自建 skills/mine.md 与 notes.txt、
    //   添 .env;new 包改 exe/official/edited、误收 config.toml 与
    //   notes.txt、添 .env、删 skills/retired.md。
    const auto new_map = NewMap({
        Entry("lubancode.exe", Sha64('n') + "-exe"),
        Entry("skills/official.md", Sha64('n') + "-off"),
        Entry("skills/edited.md", Sha64('n') + "-edit"),
        Entry("config.toml", Sha64('n') + "-conf"),   // 误收
        Entry("notes.txt", Sha64('n') + "-note"),     // 根级但不在包根文件表
        Entry(".env", Sha64('n') + "-env"),           // 误收
    });
    std::map<std::string, DiskEntry> disk;
    disk.emplace(lubancode::updater::FoldKey("lubancode.exe"), DiskFile(Sha64('o') + "-exe"));
    disk.emplace(lubancode::updater::FoldKey("skills/official.md"), DiskFile(Sha64('o') + "-off"));
    disk.emplace(lubancode::updater::FoldKey("skills/edited.md"), DiskFile("m" + Sha64('y') + "-edit"));
    disk.emplace(lubancode::updater::FoldKey("skills/retired.md"), DiskFile(Sha64('o') + "-ret"));
    disk.emplace(lubancode::updater::FoldKey("skills/mine.md"), DiskFile(Sha64('m') + "-mine"));
    disk.emplace(lubancode::updater::FoldKey("notes.txt"), DiskFile(Sha64('m') + "-note"));
    disk.emplace(lubancode::updater::FoldKey("config.toml"), DiskFile("user-conf"));
    disk.emplace(lubancode::updater::FoldKey(".env"), DiskFile("user-env"));

    const auto plan = lubancode::updater::BuildWhitelistPlan(new_map, disk);

    // 旧安装迁移更新程序/官方技能:hash 异,先备份再换新。
    const PlanEntry* exe = Find(plan, "lubancode.exe");
    REQUIRE(exe != nullptr);
    CHECK(exe->action == Action::Replace);
    CHECK(exe->backup);
    const PlanEntry* official = Find(plan, "skills/official.md");
    REQUIRE(official != nullptr);
    CHECK(official->action == Action::Replace);
    CHECK(official->backup);

    // 白名单同名文件即使改过也更新,且覆盖前备份(edited.md 盘面是'my edit')。
    const PlanEntry* edited = Find(plan, "skills/edited.md");
    REQUIRE(edited != nullptr);
    CHECK(edited->action == Action::Replace);
    CHECK(edited->backup);

    // 新版不含的旧文件仍然保留:无 retire,不出条目。
    CHECK(Find(plan, "skills/retired.md") == nullptr);
    // 自建技能保留:盘面孤儿件不进计划。
    CHECK(Find(plan, "skills/mine.md") == nullptr);
    // 普通用户文件保留:清单里有 notes.txt,但根文件表不收,不出条目。
    CHECK(Find(plan, "notes.txt") == nullptr);

    // 清单误收 config.toml/.env 也不能覆盖。
    const PlanEntry* conf = Find(plan, "config.toml");
    REQUIRE(conf != nullptr);
    CHECK(conf->action == Action::KeepUserData);
    const PlanEntry* env = Find(plan, ".env");
    REQUIRE(env != nullptr);
    CHECK(env->action == Action::KeepUserData);

    // 条目按 FoldKey 键序:".env" < "config.toml" < "lubancode.exe" <
    // "skills/edited.md" < "skills/official.md"(合同:map 键序产出)。
    REQUIRE(plan.size() == 5);
    CHECK(plan[0].path == ".env");
    CHECK(plan[1].path == "config.toml");
    CHECK(plan[2].path == "lubancode.exe");
    CHECK(plan[3].path == "skills/edited.md");
    CHECK(plan[4].path == "skills/official.md");

    CHECK(Count(plan, Action::Replace) == 3);
    CHECK(Count(plan, Action::KeepUserData) == 2);
    CHECK_FALSE(lubancode::updater::PlanBlocks(plan));
}

TEST_CASE("对拍 legacy 册:重复装同包零动作") {
    // 第一遍 apply 后的盘面:三件官方件已是新版内容,用户件原样。
    const auto new_map = NewMap({
        Entry("lubancode.exe", Sha64('n') + "-exe"),
        Entry("skills/official.md", Sha64('n') + "-off"),
        Entry("skills/edited.md", Sha64('n') + "-edit"),
        Entry("config.toml", Sha64('n') + "-conf"),
        Entry("notes.txt", Sha64('n') + "-note"),
        Entry(".env", Sha64('n') + "-env"),
    });
    std::map<std::string, DiskEntry> disk;
    disk.emplace(lubancode::updater::FoldKey("lubancode.exe"), DiskFile(Sha64('n') + "-exe"));
    disk.emplace(lubancode::updater::FoldKey("skills/official.md"), DiskFile(Sha64('n') + "-off"));
    disk.emplace(lubancode::updater::FoldKey("skills/edited.md"), DiskFile(Sha64('n') + "-edit"));
    disk.emplace(lubancode::updater::FoldKey("skills/mine.md"), DiskFile(Sha64('m') + "-mine"));
    disk.emplace(lubancode::updater::FoldKey("skills/retired.md"), DiskFile(Sha64('o') + "-ret"));
    disk.emplace(lubancode::updater::FoldKey("notes.txt"), DiskFile(Sha64('m') + "-note"));
    disk.emplace(lubancode::updater::FoldKey("config.toml"), DiskFile("user-conf"));
    disk.emplace(lubancode::updater::FoldKey(".env"), DiskFile("user-env"));

    const auto plan = lubancode::updater::BuildWhitelistPlan(new_map, disk);

    // PS 断言:再次安装相同包,replace/install-new/retire 计数为 0(本表
    // 无 retire 动作,前两枚为零即同款)。
    CHECK(Count(plan, Action::Replace) == 0);
    CHECK(Count(plan, Action::InstallNew) == 0);
    CHECK(Count(plan, Action::SkipCurrent) == 3);
    CHECK(Count(plan, Action::KeepUserData) == 2);
    CHECK(plan.size() == 5);
    CHECK_FALSE(lubancode::updater::PlanBlocks(plan));
}

// ------------------------------------------------------- FoldKey 碰撞案 ---

TEST_CASE("FoldKey 碰撞:大小写异写同名按平台口径归一或分家") {
    // manifest 写 docs/README.md,盘面扫出 docs/readme.md——异写同名。
    const auto new_map = NewMap({Entry("docs/README.md", Sha64('r'))});
    std::map<std::string, DiskEntry> disk;
    disk.emplace(lubancode::updater::FoldKey("docs/readme.md"), DiskFile(Sha64('r')));

    const auto plan = lubancode::updater::BuildWhitelistPlan(new_map, disk);
    REQUIRE(plan.size() == 1);
    if constexpr (lubancode::updater::kCaseFoldingActive) {
        // Windows/macOS:折叠键撞上,同一路径,内容相同 → SkipCurrent。
        CHECK(plan[0].action == Action::SkipCurrent);
    } else {
        // Linux:大小写敏感,两条路径互不相认 → 新版新增。
        CHECK(plan[0].action == Action::InstallNew);
    }
}

TEST_CASE("FoldKey 碰撞:包根文件表命中也按平台口径") {
    // manifest 写 README.MD(异写):折叠口径下折成 readme.md 命中包根文件
    // 表;Linux 口径下逐字节比对不命中,连计划都不进。
    const auto new_map = NewMap({Entry("README.MD", Sha64('m'))});
    const auto plan = lubancode::updater::BuildWhitelistPlan(new_map, {});

    if constexpr (lubancode::updater::kCaseFoldingActive) {
        REQUIRE(plan.size() == 1);
        CHECK(plan[0].action == Action::InstallNew);
    } else {
        CHECK(plan.empty());
    }
}

TEST_CASE("FoldKey 碰撞:受管树首段异写按平台口径") {
    const auto new_map = NewMap({Entry("Docs/guide.md", Sha64('g'))});
    const auto plan = lubancode::updater::BuildWhitelistPlan(new_map, {});

    if constexpr (lubancode::updater::kCaseFoldingActive) {
        // Windows/macOS:Docs 折成 docs,受管树收。
        REQUIRE(plan.size() == 1);
        CHECK(plan[0].action == Action::InstallNew);
    } else {
        // Linux:Docs 不是受管树,白名单外。
        CHECK(plan.empty());
    }
}

// ------------------------------------------------- 包根文件表与受管树边界 ---

TEST_CASE("包根文件表:七枚逐名命中(install.ps1 终稿 $rootFiles)") {
    // 逐名验证常量表提取无笔误:七枚根级官方件全收,盘面缺席 → InstallNew。
    const auto new_map = NewMap({
        Entry("lubancode.exe", Sha64('1')),
        Entry("LICENSE", Sha64('2')),
        Entry("THIRD_PARTY_NOTICES.md", Sha64('3')),
        Entry("README.md", Sha64('4')),
        Entry("README.en.md", Sha64('5')),
        Entry("install.ps1", Sha64('6')),
        Entry("uninstall.ps1", Sha64('7')),
    });
    const auto plan = lubancode::updater::BuildWhitelistPlan(new_map, {});
    REQUIRE(plan.size() == 7);
    CHECK(Count(plan, Action::InstallNew) == 7);
}

TEST_CASE("包根文件表外:根级杂件不收,连计划都不进") {
    // notes.txt 正是 legacy 册的普通用户文件;manifest.json 是记录件,表里
    // 没有;裸名 lubancode 终稿表也不收(只有 lubancode.exe)。
    const auto new_map = NewMap({
        Entry("notes.txt", Sha64('1')),
        Entry("manifest.json", Sha64('2')),
        Entry("install-state.json", Sha64('3')),
        Entry("uninstall.sh", Sha64('4')),
        Entry("lubancode", Sha64('5')),
        Entry("CHANGELOG.md", Sha64('6')),
    });
    const auto plan = lubancode::updater::BuildWhitelistPlan(new_map, {});
    CHECK(plan.empty());
}

TEST_CASE("受管树边界:六树内收,树外/裸树名不收") {
    const auto inside = NewMap({
        Entry("skills/lubancode-config/SKILL.md", Sha64('1')),
        Entry("docs/dev/manual.md", Sha64('2')),
        Entry("web/assistant/app.js", Sha64('3')),
        Entry("libexec/rg.exe", Sha64('4')),
        Entry("licenses/mbedtls.txt", Sha64('5')),
        Entry("updater/updater.py", Sha64('6')),
    });
    const auto plan_in = lubancode::updater::BuildWhitelistPlan(inside, {});
    REQUIRE(plan_in.size() == 6);
    CHECK(Count(plan_in, Action::InstallNew) == 6);

    const auto outside = NewMap({
        Entry("eval/run.py", Sha64('a')),
        Entry("scripts/tool.py", Sha64('b')),
        Entry("tests/unit/x.cpp", Sha64('c')),
        Entry("myskills/a.md", Sha64('d')),
        Entry("documentation/guide.md", Sha64('e')),
        Entry("skills2/a.md", Sha64('f')),
    });
    const auto plan_out = lubancode::updater::BuildWhitelistPlan(outside, {});
    CHECK(plan_out.empty());

    // 裸树名(无 '/'):Build-PackageFilePlan 要 $path.Contains('/'),不收。
    const auto bare = NewMap({
        Entry("skills", Sha64('a')),
        Entry("docs", Sha64('b')),
        Entry("updater", Sha64('c')),
    });
    const auto plan_bare = lubancode::updater::BuildWhitelistPlan(bare, {});
    CHECK(plan_bare.empty());
}

TEST_CASE("用户数据边界:精确名与两目录前缀,树内同名件不算") {
    const auto new_map = NewMap({
        Entry(".lubancode", Sha64('1')),           // 目录名本身也算
        Entry(".lubancode/state/ledger.json", Sha64('2')),
        Entry(".agents", Sha64('3')),
        Entry(".agents/tools/probe.md", Sha64('4')),
    });
    const auto plan = lubancode::updater::BuildWhitelistPlan(new_map, {});
    REQUIRE(plan.size() == 4);
    CHECK(Count(plan, Action::KeepUserData) == 4);

    // skills/config.toml 是树内官方件,不是用户数据(paths.hpp 的口径),
    // 走正常白名单决策。
    const auto tree_conf = NewMap({Entry("skills/config.toml", Sha64('9'))});
    const auto plan_tree = lubancode::updater::BuildWhitelistPlan(tree_conf, {});
    REQUIRE(plan_tree.size() == 1);
    CHECK(plan_tree[0].action == Action::InstallNew);

    // config.toml.bak 不是用户数据,也不是包根文件,白名单外。
    const auto near = NewMap({Entry("config.toml.bak", Sha64('8'))});
    CHECK(lubancode::updater::BuildWhitelistPlan(near, {}).empty());
}

// ------------------------------------------------------------- 阻断判定 ---

TEST_CASE("PlanBlocks:conflict 才阻断,其余不阻断") {
    CHECK_FALSE(lubancode::updater::PlanBlocks({}));

    std::vector<PlanEntry> calm;
    calm.push_back({"docs/a.md", Action::SkipCurrent, false, ""});
    calm.push_back({"docs/b.md", Action::Replace, true, ""});
    calm.push_back({"docs/c.md", Action::InstallNew, false, ""});
    calm.push_back({"config.toml", Action::KeepUserData, false, ""});
    CHECK_FALSE(lubancode::updater::PlanBlocks(calm));

    std::vector<PlanEntry> kind = {{"docs/d.md", Action::ConflictKind, false, ""}};
    CHECK(lubancode::updater::PlanBlocks(kind));

    std::vector<PlanEntry> reparse = {{"docs/e.md", Action::ConflictReparse, false, ""}};
    CHECK(lubancode::updater::PlanBlocks(reparse));

    // 混在末尾的 conflict 也得揪出来(install.ps1 退出码 3 不看先后)。
    calm.push_back({"docs/f.md", Action::ConflictReparse, false, ""});
    CHECK(lubancode::updater::PlanBlocks(calm));
}

TEST_CASE("空清单与空盘面:包根白名单全量 InstallNew,空清单出空计划") {
    const auto new_map = NewMap({Entry("LICENSE", Sha64('1'))});
    CHECK(lubancode::updater::BuildWhitelistPlan(new_map, {}).size() == 1);
    CHECK(lubancode::updater::BuildWhitelistPlan({}, {}).empty());

    // 盘面有、清单无:盘面孤儿件不进计划(对拍 keep-unknown 不设动作)。
    std::map<std::string, DiskEntry> disk;
    disk.emplace(lubancode::updater::FoldKey("skills/stray.md"), DiskFile(Sha64('s')));
    disk.emplace(lubancode::updater::FoldKey("skills/linked"), DiskReparseDir());
    CHECK(lubancode::updater::BuildWhitelistPlan({}, disk).empty());
}
