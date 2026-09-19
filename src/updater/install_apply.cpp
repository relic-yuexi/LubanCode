// 实现与合同注释见 install_apply.hpp(行为权威:install.ps1 终稿 PR #140
// 的 Invoke-ResourceApply / Write-InstallRecords,同源 install_plan.py 的
// apply_plan / write_records)。
#include "updater/install_apply.hpp"

#include <cstdio>
#include <memory>
#include <string_view>
#include <system_error>
#include <utility>

#include <nlohmann/json.hpp>

#include "platform/file_in_use.hpp"
#include "platform/paths.hpp"
#include "updater/install_scan.hpp"
#include "updater/manifest.hpp"
#include "updater/paths.hpp"

#ifdef _WIN32
#include <io.h>  // _commit/_fileno
#else
#include <unistd.h>  // fsync/fileno
#endif

namespace lubancode::updater {
namespace {

namespace fs = std::filesystem;
using lubancode::platform::PathToUtf8;
using lubancode::platform::Utf8ToPath;

// 文件数据落盘:fflush + fsync/_commit(与 platform/atomic_write 的
// ProcessCrashDurability 档同一手法,目录条目落盘归事务账的原子写)。
std::expected<void, std::string> CommitFileData(std::FILE* file, const fs::path& display) {
    if (std::fflush(file) != 0) {
        return std::unexpected("fflush 失败: " + PathToUtf8(display));
    }
#ifdef _WIN32
    if (_commit(_fileno(file)) != 0) {
        return std::unexpected("数据落盘失败(_commit): " + PathToUtf8(display));
    }
#else
    if (::fsync(::fileno(file)) != 0) {
        return std::unexpected("数据落盘失败(fsync): " + PathToUtf8(display));
    }
#endif
    return {};
}

std::unique_ptr<std::FILE, decltype(&std::fclose)> OpenRead(const fs::path& file) {
#ifdef _WIN32
    std::FILE* raw = nullptr;
    if (_wfopen_s(&raw, file.c_str(), L"rb") != 0 || raw == nullptr) {
        return {nullptr, &std::fclose};
    }
#else
    std::FILE* raw = std::fopen(file.c_str(), "rb");
#endif
    return {raw, &std::fclose};
}

std::unique_ptr<std::FILE, decltype(&std::fclose)> OpenWrite(const fs::path& file) {
#ifdef _WIN32
    std::FILE* raw = nullptr;
    if (_wfopen_s(&raw, file.c_str(), L"wb") != 0 || raw == nullptr) {
        return {nullptr, &std::fclose};
    }
#else
    std::FILE* raw = std::fopen(file.c_str(), "wb");
#endif
    return {raw, &std::fclose};
}

// 两枚现存文件是否同一枚(std::filesystem::equivalent 的错误码口;任一
// 不在/打不开都算不同——samefile 守卫只在两边都在时才有意义)。
bool SameFile(const fs::path& a, const fs::path& b) {
    std::error_code ec;
    if (!std::filesystem::exists(a, ec) || ec) return false;
    if (!std::filesystem::exists(b, ec) || ec) return false;
    const bool same = std::filesystem::equivalent(a, b, ec);
    return !ec && same;
}

// 动作名串(PS conflicts 条目的 kind 字段口径)。
std::string_view KindText(Action action) {
    switch (action) {
        case Action::ConflictKind:
            return "conflict-kind";
        case Action::ConflictReparse:
            return "conflict-reparse";
        default:
            return "";
    }
}

// 备份根的账面显示:能折成安装根下相对路径就折(PS 'backups/<名>' 形状),
// 折不成(不同盘/无亲缘)给绝对路径。
std::string BackupDisplay(const fs::path& backup_root, const fs::path& install_root) {
    std::error_code ec;
    const fs::path rel = std::filesystem::relative(backup_root, install_root, ec);
    if (!ec && !rel.empty() && rel.native().find(fs::path("..").native()) != 0) {
        return rel.generic_string();
    }
    return PathToUtf8(backup_root);
}

// 目标的祖先链体检(PS Get-DestPathFor 的父路径巡检):父辈里有一枚不是
// 普通目录(被文件占/是 reparse)就拒绝写入,不许借道链接写出安装根。
// 巡到安装根为止(安装根本身不查——调用方给的就是它)。
std::expected<void, std::string> CheckParentChain(const fs::path& dst,
                                                  const fs::path& install_root) {
    fs::path cur = dst.parent_path();
    while (!cur.empty() && cur != install_root && cur != cur.parent_path()) {
        std::error_code ec;
        if (std::filesystem::exists(cur, ec) && !ec) {
            const bool is_dir = std::filesystem::is_directory(cur, ec) && !ec;
            if (!is_dir || IsLinkLike(cur)) {
                return std::unexpected("目标父路径不是普通目录,拒绝写入: " + PathToUtf8(cur));
            }
        }
        cur = cur.parent_path();
    }
    return {};
}

}  // namespace

std::expected<void, std::string> CopyFileDurable(const fs::path& src, const fs::path& dst) {
    std::error_code ec;
    if (!std::filesystem::is_regular_file(src, ec) || ec) {
        return std::unexpected("源不是常规文件: " + PathToUtf8(src));
    }
    auto in = OpenRead(src);
    if (in == nullptr) {
        return std::unexpected("源打不开: " + PathToUtf8(src));
    }
    auto out = OpenWrite(dst);
    if (out == nullptr) {
        return std::unexpected("目标打不开(被占/权限): " + PathToUtf8(dst));
    }
    std::string chunk(64 * 1024, '\0');
    while (true) {
        const std::size_t got = std::fread(chunk.data(), 1, chunk.size(), in.get());
        if (got > 0) {
            const std::size_t put = std::fwrite(chunk.data(), 1, got, out.get());
            if (put != got) {
                return std::unexpected("写入失败(盘满/IO 错): " + PathToUtf8(dst));
            }
        }
        if (got < chunk.size()) {
            if (std::ferror(in.get()) != 0) {
                return std::unexpected("源读取失败: " + PathToUtf8(src));
            }
            break;  // EOF
        }
    }
    if (auto synced = CommitFileData(out.get(), dst); !synced.has_value()) {
        return synced;
    }
    if (std::fclose(out.release()) != 0) {
        return std::unexpected("close 检查失败: " + PathToUtf8(dst));
    }
    return {};
}

std::expected<ApplyReport, std::string> ApplyWhitelistPlan(
    const std::vector<PlanEntry>& plan,
    const std::map<std::string, DiskEntry>& disk,
    const fs::path& install_root,
    const fs::path& pkg_dir,
    const fs::path& backup_root) {
    // 备份根开单即建(PS New-BackupRoot):零备份的空单也留个空根。
    std::error_code ec;
    std::filesystem::create_directories(backup_root, ec);
    if (ec) {
        return std::unexpected("备份根建不成: " + PathToUtf8(backup_root) + "(" + ec.message() + ")");
    }

    ApplyReport report;
    report.backup_root = backup_root;
    const std::string backup_display = BackupDisplay(backup_root, install_root);
    (void)disk;  // 签名留位:接线侧把扫描账一并递进来好对账;备份与占位
                 // 的判定以落地时实况为准(PS/python apply 同口径)

    for (const PlanEntry& entry : plan) {
        if (entry.action == Action::ConflictKind || entry.action == Action::ConflictReparse) {
            report.conflicts.push_back(
                {entry.path, std::string(KindText(entry.action)), backup_display});
            continue;
        }
        if (entry.action == Action::SkipCurrent || entry.action == Action::KeepUserData) {
            continue;  // 零动作
        }

        // Replace / InstallNew:路径先过契约(PS Get-DestPathFor 的 throw 侧)。
        if (!ValidRelpath(entry.path)) {
            return std::unexpected("路径不合法,拒绝落地: " + entry.path);
        }
        const fs::path rel = Utf8ToPath(entry.path);
        const fs::path dst = install_root / rel;
        const fs::path src = pkg_dir / rel;

        if (auto chain = CheckParentChain(dst, install_root); !chain.has_value()) {
            return chain;
        }

        // 落地期占位复查(计划之后盘面又变了):目标被目录/reparse 占住,
        // 点名 conflict 不硬写,与计划层同归。
        std::error_code lec;
        const bool dst_exists = std::filesystem::exists(dst, lec) && !lec;
        if (dst_exists) {
            const bool dst_dir = std::filesystem::is_directory(dst, lec) && !lec;
            const bool dst_reparse = IsLinkLike(dst);
            if (dst_dir || dst_reparse) {
                report.conflicts.push_back(
                    {entry.path, dst_reparse ? "conflict-reparse" : "conflict-kind",
                     backup_display});
                continue;
            }
        }

        // Replace 先备份:落地时目标真是普通文件就备(PS/python 的实时
        // 口径:Test-Path Leaf 且非 reparse——盘面账是计划侧的旧照,这里
        // 以实况为准;reparse/目录位已被上面的占位复查挡掉)。disk 参数
        // 只作接线侧对账,不参与此判定。
        if (entry.action == Action::Replace && entry.backup && dst_exists) {
            const fs::path backup_dst = backup_root / rel;
            std::error_code bec;
            std::filesystem::create_directories(backup_dst.parent_path(), bec);
            if (bec) {
                return std::unexpected("备份目录建不成: " + PathToUtf8(backup_dst.parent_path()) +
                                       "(" + bec.message() + ")");
            }
            if (auto copied = CopyFileDurable(dst, backup_dst); !copied.has_value()) {
                return std::unexpected("备份失败(" + entry.path + "): " + copied.error());
            }
            report.backed_up.push_back(entry.path);
        }

        if (SameFile(src, dst)) {
            // 同目录安装,不搬自己(PS/python samefile 守卫);账照记。
            if (entry.action == Action::Replace) {
                report.replaced.push_back(entry.path);
            } else {
                report.installed.push_back(entry.path);
            }
            continue;
        }

        std::error_code dec;
        std::filesystem::create_directories(dst.parent_path(), dec);
        if (dec) {
            return std::unexpected("目标目录建不成: " + PathToUtf8(dst.parent_path()) +
                                   "(" + dec.message() + ")");
        }
        if (auto copied = CopyFileDurable(src, dst); !copied.has_value()) {
            return std::unexpected("落地失败(" + entry.path + "): " + copied.error() +
                                   "(若程序占用,请关闭后重试)");
        }
        if (entry.action == Action::Replace) {
            report.replaced.push_back(entry.path);
        } else {
            report.installed.push_back(entry.path);
        }
    }
    return report;
}

std::expected<void, std::string> BackfillInstallState(const LayoutPaths& paths,
                                                      const fs::path& pkg_dir,
                                                      InstallState state,
                                                      const UtcNowFn& now) {
    const fs::path pkg_manifest = pkg_dir / "manifest.json";
    std::error_code ec;
    if (!std::filesystem::is_regular_file(pkg_manifest, ec) || ec) {
        return std::unexpected("包内没有官方 manifest.json,拒绝回填安装账: " +
                               PathToUtf8(pkg_manifest));
    }

    // manifest 原件逐字节照抄进安装根(同文件不搬自己)。
    const fs::path dst_manifest = paths.root / "manifest.json";
    if (!SameFile(pkg_manifest, dst_manifest)) {
        if (auto copied = CopyFileDurable(pkg_manifest, dst_manifest); !copied.has_value()) {
            return std::unexpected("manifest 原件照抄失败: " + copied.error());
        }
    }

    // 清单 json 入账(不抛的解析口:坏 JSON 给 discarded,这里明报)。
    auto in = OpenRead(pkg_manifest);
    if (in == nullptr) {
        return std::unexpected("manifest 读不动: " + PathToUtf8(pkg_manifest));
    }
    std::string bytes;
    std::string chunk(64 * 1024, '\0');
    while (true) {
        const std::size_t got = std::fread(chunk.data(), 1, chunk.size(), in.get());
        bytes.append(chunk.data(), got);
        if (got < chunk.size()) {
            if (std::ferror(in.get()) != 0) {
                return std::unexpected("manifest 读取失败: " + PathToUtf8(pkg_manifest));
            }
            break;
        }
    }
    const nlohmann::json manifest = nlohmann::json::parse(bytes, nullptr, /*allow_exceptions=*/false);
    if (manifest.is_discarded() || !manifest.is_object()) {
        return std::unexpected("包内 manifest.json 不是合法 JSON 对象: " + PathToUtf8(pkg_manifest));
    }
    state.manifest = manifest;
    if (state.version.empty() && manifest.contains("version") && manifest["version"].is_string()) {
        state.version = manifest["version"].get<std::string>();
    }
    if (!state.platform.has_value() && manifest.contains("platform") && manifest["platform"].is_string()) {
        state.platform = manifest["platform"].get<std::string>();
    }
    if (state.channel.empty() && manifest.contains("channel") && manifest["channel"].is_string()) {
        state.channel = manifest["channel"].get<std::string>();
    }
    WriteInstallState(paths, state, now);
    return {};
}

}  // namespace lubancode::updater
