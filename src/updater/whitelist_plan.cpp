// 实现与合同注释见 whitelist_plan.hpp(行为权威:install.ps1 终稿 PR #140
// 的 Build-PackageFilePlan / Build-FilePlan)。
#include "updater/whitelist_plan.hpp"

#include <string_view>

#include "updater/paths.hpp"

namespace lubancode::updater {
namespace {

// 包根文件表——出处:install.ps1 终稿(PR #140,codex/legacy-install-
// migration)Build-PackageFilePlan 的 $rootFiles。根级官方件全集,整名
// 比对(PS -contains 大小写不敏感,此处 FoldKey 口径)。注意只有
// lubancode.exe,没有裸名 lubancode——终稿如此,POSIX 侧名表归后续批次。
constexpr std::string_view kPackageRootFiles[] = {
    "lubancode.exe", "LICENSE",      "THIRD_PARTY_NOTICES.md", "README.md",
    "README.zh-CN.md", "install.ps1", "uninstall.ps1",
};

// 受管树表——出处:同上 install.ps1 的 $script:ManagedTrees(L57)。
// 整棵由清单说了算;树内路径须带 '/'(PS: $path.Contains('/') -and
// $ManagedTrees -contains $top),裸树名不算。
constexpr std::string_view kManagedTrees[] = {
    "skills", "docs", "web", "libexec", "licenses", "updater",
};

// manifest 路径是否落在覆盖白名单内(整名命中包根文件表,或首段命中受管
// 树且确有子层)。比对走 FoldKey:Windows/macOS 折叠口径与 PS -contains
// 一致,Linux 原样(异写即不同路径,install_plan.py 读侧同口径)。
bool IsWhitelistedPath(std::string_view path) {
    const std::string folded = FoldKey(path);
    for (const std::string_view root_file : kPackageRootFiles) {
        if (folded == FoldKey(root_file)) {
            return true;
        }
    }
    const auto slash = path.find('/');
    if (slash == std::string_view::npos || slash == 0) {
        return false;  // 根级路径只有整名命中一条路;首段空不是合法相对路径
    }
    const std::string folded_top = FoldKey(path.substr(0, slash));
    for (const std::string_view tree : kManagedTrees) {
        if (folded_top == FoldKey(tree)) {
            return true;
        }
    }
    return false;
}

}  // namespace

std::vector<PlanEntry> BuildWhitelistPlan(
    const std::map<std::string, ManifestEntry>& new_map,
    const std::map<std::string, DiskEntry>& disk) {
    std::vector<PlanEntry> plan;
    plan.reserve(new_map.size());

    for (const auto& [key, entry] : new_map) {
        // 头一道闸:用户配置/数据从不参与安装,清单误收也不写不删
        // (Build-FilePlan L541-546 的声明语义,放在白名单判定之前)。
        if (IsUserDataPath(entry.path)) {
            plan.push_back(
                {entry.path, Action::KeepUserData, false, "用户配置或数据,不写不删"});
            continue;
        }

        // 白名单外:不出条目(Build-PackageFilePlan 只把 allowed 递给决策,
        // 包外路径连计划都不进,盘面件全留)。
        if (!IsWhitelistedPath(entry.path)) {
            continue;
        }

        const auto disk_it = disk.find(key);
        if (disk_it == disk.end()) {
            plan.push_back({entry.path, Action::InstallNew, false, "新版新增"});
            continue;
        }
        const DiskEntry& on_disk = disk_it->second;

        // 盘面目录/reparse 挡住白名单路径:不写不删,阻断。reparse 优先于
        // is_dir 归类(Build-FilePlan L559-563 的 else 分支,reparse 目录也
        //归 conflict-reparse)。
        if (on_disk.is_dir && !on_disk.reparse) {
            plan.push_back(
                {entry.path, Action::ConflictKind, false, "盘面是目录,不写不删"});
            continue;
        }
        if (on_disk.reparse) {
            plan.push_back({entry.path, Action::ConflictReparse, false,
                            "盘面是链接/reparse,不写不删"});
            continue;
        }

        // 盘面是普通文件:白名单语境没有旧清单,同名即换;内容相同才跳过。
        // sha256 没给(nullopt/空串)= 内容未知,按需 Replace 先备份。
        const bool hash_known =
            on_disk.sha256.has_value() && !on_disk.sha256->empty();
        if (hash_known && *on_disk.sha256 == entry.sha256) {
            plan.push_back({entry.path, Action::SkipCurrent, false, "已是新版内容"});
            continue;
        }
        plan.push_back({entry.path, Action::Replace, true,
                        hash_known ? "同名文件内容有异,先备份再换新"
                                   : "盘面内容未知,先备份再换新"});
    }
    return plan;
}

bool PlanBlocks(const std::vector<PlanEntry>& plan) {
    for (const PlanEntry& entry : plan) {
        if (entry.action == Action::ConflictKind ||
            entry.action == Action::ConflictReparse) {
            return true;
        }
    }
    return false;
}

}  // namespace lubancode::updater
