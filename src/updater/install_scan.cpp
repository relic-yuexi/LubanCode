// 实现与合同注释见 install_scan.hpp(行为权威:install.ps1 终稿 PR #140
// 的 Get-InstallDiskState / Get-DiskHash,同源 install_plan.py 的
// scan_disk / disk_hash)。
#include "updater/install_scan.hpp"

#include <algorithm>
#include <cstdio>
#include <memory>
#include <string_view>
#include <system_error>
#include <utility>

#include "platform/file_in_use.hpp"
#include "platform/paths.hpp"
#include "platform/sha256.hpp"
#include "updater/paths.hpp"

namespace lubancode::updater {
namespace {

// 目录条目按名排序后入账:std::filesystem::directory_iterator 的遍历序
// 不作约定,排序后同盘面出同账,测试与黄金对拍才可复现。
std::vector<std::filesystem::path> SortedEntries(const std::filesystem::path& dir) {
    std::vector<std::filesystem::path> entries;
    std::error_code ec;
    for (std::filesystem::directory_iterator it(dir, ec), end; !ec && it != end;
         it.increment(ec)) {
        entries.push_back(it->path());
    }
    std::sort(entries.begin(), entries.end(),
              [](const std::filesystem::path& a, const std::filesystem::path& b) {
                  return a.filename().u8string() < b.filename().u8string();
              });
    return entries;
}

// 纯词法的路径后缀截取:full 去掉 base 前缀的余段,拼成 '/' 分隔相对
// 路径。绝不用 std::filesystem::relative——那走 weakly_canonical,会把
// symlink/junction 条目解析到链接对面(macOS 腿实测翻车:键成了
// "skills/../../outside");扫描键只要词法后缀。
std::string LexicalSuffix(const std::filesystem::path& full,
                          const std::filesystem::path& base) {
    auto fi = full.begin();
    auto bi = base.begin();
    for (; bi != base.end() && fi != full.end() && *bi == *fi; ++bi, ++fi) {
    }
    std::filesystem::path rest;
    for (; fi != full.end(); ++fi) {
        rest /= *fi;
    }
    if (rest.empty()) {
        return lubancode::platform::PathToUtf8(full.filename());
    }
    return rest.generic_string();
}

}  // namespace

bool IsLinkLike(const std::filesystem::path& path) {
    if (lubancode::platform::IsReparsePoint(path)) {
        return true;  // Windows:reparse 点(junction/symlink)一网打尽
    }
    std::error_code ec;
    return std::filesystem::is_symlink(path, ec);  // POSIX:symlink 独眼
}

std::map<std::string, DiskEntry> ScanInstallDisk(
    const std::filesystem::path& install_root,
    const std::vector<std::string>& managed_trees,
    const std::vector<std::string>& root_files) {
    std::map<std::string, DiskEntry> disk;

    std::error_code ec;
    if (!std::filesystem::is_directory(install_root, ec) || ec) {
        return disk;  // 安装根不在:空账(PS 提前 return $disk)
    }

    // 记录件排除表折好待查(整名比对,PS -contains / python in 的同款)。
    std::vector<std::string> folded_records;
    folded_records.reserve(root_files.size());
    for (const std::string& name : root_files) {
        folded_records.push_back(FoldKey(name));
    }

    // 受管树:逐棵全递归,不跟进 reparse(树根本身是 reparse 整棵跳过,
    // 口径差见头注)。树内目录也入账。
    for (const std::string& tree : managed_trees) {
        const std::filesystem::path tree_root = install_root / tree;
        std::error_code tree_ec;
        if (!std::filesystem::is_directory(tree_root, tree_ec) || tree_ec) {
            continue;
        }
        if (IsLinkLike(tree_root)) {
            continue;
        }
        // 显式栈代替递归:深树不吃栈,条目序与递归序一致(先本层后子层)。
        std::vector<std::filesystem::path> pending{tree_root};
        while (!pending.empty()) {
            const std::filesystem::path dir = pending.back();
            pending.pop_back();
            for (const std::filesystem::path& entry : SortedEntries(dir)) {
                std::error_code entry_ec;
                const bool is_dir = std::filesystem::is_directory(entry, entry_ec) && !entry_ec;
                const bool reparse = IsLinkLike(entry);
                const std::string rel = tree + "/" + LexicalSuffix(entry, tree_root);
                DiskEntry item;
                item.abspath = entry;
                item.is_dir = is_dir;
                item.reparse = reparse;
                item.sha256 = std::nullopt;  // 惰性:要用时 HashDiskEntry 补
                disk[FoldKey(rel)] = std::move(item);
                if (is_dir && !reparse) {
                    pending.push_back(entry);
                }
            }
        }
    }

    // 安装根顶层:文件 + reparse 件入账(记录件除外),普通目录不入账。
    for (const std::filesystem::path& entry : SortedEntries(install_root)) {
        const std::string folded = FoldKey(lubancode::platform::PathToUtf8(entry.filename()));
        if (std::find(folded_records.begin(), folded_records.end(), folded) !=
            folded_records.end()) {
            continue;
        }
        const bool reparse = IsLinkLike(entry);
        std::error_code entry_ec;
        const bool is_dir = std::filesystem::is_directory(entry, entry_ec) && !entry_ec;
        if (!reparse) {
            const bool is_file =
                std::filesystem::is_regular_file(entry, entry_ec) && !entry_ec;
            if (!is_file) {
                continue;  // 根级普通目录:不在资产域
            }
        }
        DiskEntry item;
        item.abspath = entry;
        item.is_dir = is_dir;
        item.reparse = reparse;
        item.sha256 = std::nullopt;
        disk[folded] = std::move(item);
    }
    return disk;
}

std::optional<std::string> HashDiskEntry(const std::filesystem::path& abspath) {
    std::error_code ec;
    if (!std::filesystem::is_regular_file(abspath, ec) || ec) {
        return std::nullopt;
    }
#ifdef _WIN32
    FILE* raw = nullptr;
    if (_wfopen_s(&raw, abspath.c_str(), L"rb") != 0 || raw == nullptr) {
        return std::nullopt;
    }
#else
    std::FILE* raw = std::fopen(abspath.c_str(), "rb");
    if (raw == nullptr) {
        return std::nullopt;
    }
#endif
    std::unique_ptr<std::FILE, decltype(&std::fclose)> file(raw, &std::fclose);

    lubancode::platform::Sha256Stream hasher;
    std::string chunk(64 * 1024, '\0');
    while (true) {
        const std::size_t got = std::fread(chunk.data(), 1, chunk.size(), file.get());
        if (got > 0) {
            hasher.Update(std::string_view(chunk.data(), got));
        }
        if (got < chunk.size()) {
            if (std::ferror(file.get()) != 0) {
                return std::nullopt;  // 读失败:算不出,调用方按口径处置
            }
            break;  // EOF
        }
    }
    return hasher.FinalHex();
}

std::vector<PlanEntry> OrphanReparseConflicts(
    const std::filesystem::path& install_root,
    const std::map<std::string, DiskEntry>& disk,
    const std::map<std::string, ManifestEntry>& new_map) {
    std::vector<PlanEntry> conflicts;
    for (const auto& [key, entry] : disk) {
        if (!entry.reparse) {
            continue;
        }
        if (new_map.find(key) != new_map.end()) {
            continue;  // 清单内:BuildWhitelistPlan 自己出 ConflictReparse
        }
        if (IsUserDataPath(key)) {
            continue;  // 用户数据闸在最前(PS/python 决策表同序)
        }
        PlanEntry item;
        // 点名路径也走纯词法截尾:relative() 会穿链接把 symlink/junction
        // 条目解析到对面(见 LexicalSuffix 注)。
        std::string rel = LexicalSuffix(entry.abspath, install_root);
        item.path = !rel.empty() ? std::move(rel) : key;
        item.action = Action::ConflictReparse;
        item.backup = false;
        item.reason = "盘面是链接/reparse,不写不删";
        conflicts.push_back(std::move(item));
    }
    return conflicts;
}

}  // namespace lubancode::updater
