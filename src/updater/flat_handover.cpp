// 实现与合同注释见 flat_handover.hpp(语义真源:scripts/updater.py 的
// full_backup / handover_flat_launcher / restore_flat_legacy /
// sync_updater_tree,与 activate 平铺分支的两步落位)。
#include "updater/flat_handover.hpp"

#include <algorithm>
#include <cstdlib>
#include <optional>
#include <system_error>
#include <utility>

#include "platform/file_in_use.hpp"
#include "platform/paths.hpp"
#include "updater/install_apply.hpp"
#include "updater/install_scan.hpp"  // IsLinkLike:备份/同步的"不跟进链接"守卫
#include "updater/paths.hpp"

#ifdef _WIN32
#include <process.h>  // _getpid
#else
#include <unistd.h>  // getpid
#endif

namespace lubancode::updater {
namespace {

namespace fs = std::filesystem;
using lubancode::platform::PathToUtf8;

// 目录条目按名排序(同 install_scan 的口径:遍历序不作约定,排了序
// 备份集才可复现、可对拍)。
std::vector<fs::path> SortedEntries(const fs::path& dir) {
    std::vector<fs::path> entries;
    std::error_code ec;
    for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
        entries.push_back(it->path());
    }
    std::sort(entries.begin(), entries.end(),
              [](const fs::path& a, const fs::path& b) {
                  return a.filename().u8string() < b.filename().u8string();
              });
    return entries;
}

// 树完整备份:常规文件与目录逐层原样拷;reparse 件不复制也不跟进
// (口径差见头注)。
std::expected<void, std::string> BackupTree(const fs::path& src_dir, const fs::path& dst_dir) {
    std::error_code ec;
    fs::create_directories(dst_dir, ec);
    if (ec) {
        return std::unexpected("备份目录建不成: " + PathToUtf8(dst_dir) + "(" + ec.message() + ")");
    }
    for (const fs::path& entry : SortedEntries(src_dir)) {
        if (IsLinkLike(entry)) {
            continue;
        }
        std::error_code entry_ec;
        if (fs::is_directory(entry, entry_ec) && !entry_ec) {
            if (auto sub = BackupTree(entry, dst_dir / entry.filename()); !sub.has_value()) {
                return sub;
            }
        } else if (fs::is_regular_file(entry, entry_ec) && !entry_ec) {
            const fs::path dst = dst_dir / entry.filename();
            if (auto copied = CopyFileDurable(entry, dst); !copied.has_value()) {
                return std::unexpected("备份失败(" + PathToUtf8(entry) + "): " + copied.error());
            }
        }
    }
    return {};
}

// 版本目录的 updater 树按文件覆盖同步到根(updater.py sync_updater_tree:
// 只覆盖不清理——更新器自己就住在那棵树里)。
std::expected<void, std::string> SyncUpdaterTree(const fs::path& src_tree, const fs::path& dst_tree) {
    std::error_code ec;
    if (!fs::is_directory(src_tree, ec) || ec) {
        return {};  // 版本目录没有 updater 树:无事可同步
    }
    if (IsLinkLike(src_tree)) {
        return {};  // 不跟进链接
    }
    std::vector<fs::path> pending{src_tree};
    while (!pending.empty()) {
        const fs::path dir = pending.back();
        pending.pop_back();
        for (const fs::path& entry : SortedEntries(dir)) {
            if (IsLinkLike(entry)) {
                continue;
            }
            std::error_code entry_ec;
            if (fs::is_directory(entry, entry_ec) && !entry_ec) {
                pending.push_back(entry);
                continue;
            }
            if (!fs::is_regular_file(entry, entry_ec) || entry_ec) {
                continue;
            }
            std::error_code rel_ec;
            const fs::path rel = fs::relative(entry, src_tree, rel_ec);
            if (rel_ec || rel.empty()) {
                continue;
            }
            const fs::path dst = dst_tree / rel;
            std::error_code dec;
            fs::create_directories(dst.parent_path(), dec);
            if (dec) {
                return std::unexpected("同步目录建不成: " + PathToUtf8(dst.parent_path()) +
                                       "(" + dec.message() + ")");
            }
            if (auto copied = CopyFileDurable(entry, dst); !copied.has_value()) {
                return std::unexpected("同步失败(" + PathToUtf8(entry) + "): " + copied.error());
            }
        }
    }
    return {};
}

#ifndef _WIN32
// POSIX 执行位(python make_executable:IXUSR|IXGRP|IXOTH)。
void MakeExecutable(const fs::path& file) {
    std::error_code ec;
    fs::permissions(file,
                    fs::perms::owner_exec | fs::perms::group_exec | fs::perms::others_exec,
                    fs::perm_options::add, ec);
}
#endif

std::string PidToken() {
#ifdef _WIN32
    return std::to_string(static_cast<unsigned long long>(_getpid()));
#else
    return std::to_string(static_cast<unsigned long long>(getpid()));
#endif
}

}  // namespace

std::expected<std::optional<fs::path>, FlatFailure> FlatFullBackup(
    const fs::path& install_root,
    const std::vector<std::string>& managed_trees,
    const std::vector<std::string>& record_files,
    const std::string& txn_id) {
    std::vector<std::string> folded_records;
    folded_records.reserve(record_files.size());
    for (const std::string& name : record_files) {
        folded_records.push_back(FoldKey(name));
    }

    std::error_code ec;
    if (!fs::is_directory(install_root, ec) || ec) {
        return std::nullopt;  // 安装根都不在:无事可备份
    }

    // 有没有可备的:任一棵活树或任一枚根级文件(python activate 的
    // any(isdir) 守卫,扩到根件)。
    bool has_any = false;
    for (const std::string& tree : managed_trees) {
        const fs::path tree_root = install_root / tree;
        std::error_code tree_ec;
        if (fs::is_directory(tree_root, tree_ec) && !tree_ec &&
            !IsLinkLike(tree_root)) {
            has_any = true;
            break;
        }
    }
    if (!has_any) {
        for (const fs::path& entry : SortedEntries(install_root)) {
            const std::string folded =
                FoldKey(lubancode::platform::PathToUtf8(entry.filename()));
            if (std::find(folded_records.begin(), folded_records.end(), folded) !=
                folded_records.end()) {
                continue;
            }
            if (IsLinkLike(entry)) {
                continue;
            }
            std::error_code entry_ec;
            if (fs::is_regular_file(entry, entry_ec) && !entry_ec) {
                has_any = true;
                break;
            }
        }
    }
    if (!has_any) {
        return std::nullopt;
    }

    const fs::path txn_root = install_root / "backups" / txn_id;
    const fs::path staging = txn_root / (".flat-staging-" + PidToken());
    const fs::path flat = txn_root / "flat";
    std::error_code mk_ec;
    fs::create_directories(staging, mk_ec);
    if (mk_ec) {
        return std::unexpected(FlatFailure{
            FlatHandoverError::Failed,
            "备份 staging 建不成: " + PathToUtf8(staging) + "(" + mk_ec.message() + ")"});
    }

    for (const std::string& tree : managed_trees) {
        const fs::path tree_root = install_root / tree;
        std::error_code tree_ec;
        if (!fs::is_directory(tree_root, tree_ec) || tree_ec) {
            continue;
        }
        if (IsLinkLike(tree_root)) {
            continue;
        }
        if (auto sub = BackupTree(tree_root, staging / tree); !sub.has_value()) {
            return std::unexpected(
                FlatFailure{FlatHandoverError::Failed, std::move(sub.error())});
        }
    }
    for (const fs::path& entry : SortedEntries(install_root)) {
        const std::string folded = FoldKey(lubancode::platform::PathToUtf8(entry.filename()));
        if (std::find(folded_records.begin(), folded_records.end(), folded) !=
            folded_records.end()) {
            continue;
        }
        if (IsLinkLike(entry)) {
            continue;
        }
        std::error_code entry_ec;
        if (!fs::is_regular_file(entry, entry_ec) || entry_ec) {
            continue;
        }
        if (auto copied = CopyFileDurable(entry, staging / entry.filename());
            !copied.has_value()) {
            return std::unexpected(FlatFailure{
                FlatHandoverError::Failed,
                "备份失败(" + PathToUtf8(entry) + "): " + copied.error()});
        }
    }

    // 两步落位:staging 改名成 flat(python activate 的 rename 步);挪不
    // 动就留原地,返回真实落点,账上照记。
    std::error_code ren_ec;
    fs::rename(staging, flat, ren_ec);
    if (ren_ec) {
        return staging;
    }
    return flat;
}

std::expected<void, FlatFailure> HandoverFlatLauncher(const LayoutPaths& paths,
                                                      const std::string& txn_id,
                                                      const fs::path& version_dir) {
    const fs::path exe_name = paths.exe.filename();
    const fs::path version_exe = version_dir / exe_name;

    // 重叠守卫(python 同款:安装根与版本目录重叠,拒绝交接)。
    std::error_code ec;
    const fs::path root_exe_full = fs::weakly_canonical(paths.exe, ec);
    const fs::path version_exe_full = fs::weakly_canonical(version_exe, ec);
    if (root_exe_full == version_exe_full) {
        return std::unexpected(
            FlatFailure{FlatHandoverError::Failed, "安装根与版本目录重叠,拒绝交接"});
    }

    const fs::path legacy_dir = paths.backups / txn_id / "legacy";
    std::error_code mk_ec;
    fs::create_directories(legacy_dir, mk_ec);
    if (mk_ec) {
        return std::unexpected(FlatFailure{
            FlatHandoverError::NeedsReview,
            "旧根 EXE 的备份目录建不成(" + PathToUtf8(legacy_dir) + ": " + mk_ec.message() +
                ")。处理完再重跑 lubancode update 续上(已下载核对的包不重下)。"
                "绝不强杀。"});
    }
    const fs::path legacy_target = legacy_dir / exe_name;

    // 旧根 EXE 让位:Windows 对运行中映像允许 rename;挪不进明报不强杀
    // (python NeedsReviewError 的文案口径)。
    std::error_code exe_ec;
    if (fs::is_regular_file(paths.exe, exe_ec) && !exe_ec) {
        std::error_code rm_ec;
        fs::remove(legacy_target, rm_ec);  // 停过一次的先清位(python 同款)
        std::error_code ren_ec;
        fs::rename(paths.exe, legacy_target, ren_ec);
        if (ren_ec) {
            return std::unexpected(FlatFailure{
                FlatHandoverError::NeedsReview,
                "旧根 EXE 挪不进备份(" + ren_ec.message() +
                    ")——多半仍被运行中的进程/杀软锁着。"
                    "退出所有 lubancode 进程后重跑 lubancode update 续上"
                    "(已下载核对的包不重下)。绝不强杀。"});
        }
    }

    if (!fs::is_regular_file(version_exe, exe_ec) || exe_ec) {
        return std::unexpected(FlatFailure{FlatHandoverError::Failed,
                                           "版本目录里没有启动器: " + PathToUtf8(version_exe)});
    }
    if (auto copied = CopyFileDurable(version_exe, paths.exe); !copied.has_value()) {
        return std::unexpected(FlatFailure{FlatHandoverError::Failed,
                                           "新版启动器落根位失败: " + copied.error()});
    }
#ifndef _WIN32
    MakeExecutable(paths.exe);
#endif
    if (auto synced = SyncUpdaterTree(version_dir / "updater", paths.updater);
        !synced.has_value()) {
        return std::unexpected(
            FlatFailure{FlatHandoverError::Failed, std::move(synced.error())});
    }
    return {};
}

std::expected<bool, FlatFailure> RestoreFlatLegacy(const LayoutPaths& paths,
                                                   const std::string& txn_id) {
    const fs::path exe_name = paths.exe.filename();
    const fs::path legacy = paths.backups / txn_id / "legacy" / exe_name;
    bool restored = false;
    // 恢复链失败不早退:记下失败,摘完 current 再返回——"恢复不成也要摘掉
    // 指针"是硬保证(python docstring 承诺、实现未兑现的那条,这里兑现),
    // 根位现场留给人工诊断。
    std::optional<FlatFailure> failure;
    std::error_code ec;
    if (fs::is_regular_file(legacy, ec) && !ec) {
        std::error_code root_ec;
        if (fs::is_regular_file(paths.exe, root_ec) && !root_ec) {
            const fs::path parked =
                paths.backups / txn_id / ("launcher-parked-" + PathToUtf8(exe_name));
            std::error_code ren_ec;
            fs::rename(paths.exe, parked, ren_ec);
            if (ren_ec) {
                failure = FlatFailure{FlatHandoverError::Failed,
                                      "根位启动器停不进备份(" + ren_ec.message() +
                                          "),旧 EXE 未恢复"};
            }
        }
        if (!failure.has_value()) {
            if (auto copied = CopyFileDurable(legacy, paths.exe); !copied.has_value()) {
                failure = FlatFailure{FlatHandoverError::Failed,
                                      "旧 EXE 恢复失败: " + copied.error()};
            } else {
#ifndef _WIN32
                MakeExecutable(paths.exe);
#endif
                restored = true;
            }
        }
    }
    // current.json 无条件摘(不在也 silently 过——python 的 except OSError:
    // pass;平铺旧 EXE 不认指针,留着会把后续启动当启动器空转)。
    std::error_code rm_ec;
    fs::remove(paths.current, rm_ec);
    if (failure.has_value()) {
        return std::unexpected(std::move(*failure));
    }
    return restored;
}

}  // namespace lubancode::updater
