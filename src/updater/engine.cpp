// engine.hpp 的实现。语义对齐 scripts/updater.py 的 cmd_update/cmd_rollback/
// cmd_gc/cmd_status/cmd_plan 主链(文件头注释见 engine.hpp)。
#include "updater/engine.hpp"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <fstream>
#include <iterator>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "platform/atomic_write.hpp"
#include "platform/file_in_use.hpp"
#include "platform/paths.hpp"
#include "platform/sha256.hpp"
#include "updater/archive.hpp"
#include "updater/download.hpp"
#include "updater/layout.hpp"
#include "updater/lock.hpp"
#include "updater/manifest.hpp"
#include "updater/paths.hpp"
#include "updater/probe.hpp"
#include "updater/txn.hpp"

#ifndef _WIN32
#include <fcntl.h>
#include <unistd.h>
#endif

namespace lubancode::updater {

namespace {

#ifdef _WIN32
constexpr const char* kExeName = "lubancode.exe";
constexpr const char* kRgName = "rg.exe";
#else
constexpr const char* kExeName = "lubancode";
constexpr const char* kRgName = "rg";
#endif

// 护栏常量(python updater.py L88-94 同款)。
constexpr std::uint64_t kDiskHeadroomBytes = 256ull << 20;      // DISK_HEADROOM_BYTES
constexpr std::uint64_t kEstimateFallbackBytes = 512ull << 20;  // estimate_need 的缺省基数

// 平铺迁移完整备份的受管树(python install_plan.ROLE_TOP_DIRS)。
constexpr const char* kRoleTopDirs[] = {"skills", "docs", "web", "libexec", "licenses", "updater"};

std::vector<std::string> GcVersions(const LayoutPaths& paths, const ProgressSink& sink);

// ---------------------------------------------------------------------------
// 异常三分(python UpdaterError/NeedsReviewError/RolledBackError 的退出码)
// ---------------------------------------------------------------------------

class UpdaterFailure : public std::runtime_error {
public:
    explicit UpdaterFailure(std::string message) : std::runtime_error(std::move(message)) {}
};

class NeedsReviewFailure : public UpdaterFailure {
public:
    explicit NeedsReviewFailure(std::string message) : UpdaterFailure(std::move(message)) {}
};

class RolledBackFailure : public UpdaterFailure {
public:
    explicit RolledBackFailure(std::string message) : UpdaterFailure(std::move(message)) {}
};

// ---------------------------------------------------------------------------
// 小件
// ---------------------------------------------------------------------------

void Say(const ProgressSink& sink, const std::string& line) {
    if (sink) sink(line);
}

std::string Utf8(const std::filesystem::path& path) { return platform::PathToUtf8(path); }

// 字段是字符串给串,否则/缺键给空串(日志/文本行用)。
std::string JsonStringField(const nlohmann::json& obj, const char* key) {
    if (obj.contains(key) && obj[key].is_string()) return obj[key].get<std::string>();
    return std::string();
}

// 字段是字符串原样给,否则/缺键给 null(status --json 的 .get() 口径)。
nlohmann::json StringFieldOrJson(const nlohmann::json& obj, const char* key) {
    if (obj.contains(key) && obj[key].is_string()) return obj[key];
    return nlohmann::json();
}

std::optional<std::string> ReadTextFile(const std::filesystem::path& file) {
    std::error_code ec;
    if (!std::filesystem::is_regular_file(file, ec) || ec) return std::nullopt;
    std::ifstream in(file, std::ios::binary);
    if (!in.is_open()) return std::nullopt;
    std::string bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (in.bad()) return std::nullopt;
    return bytes;
}

// 流式整文件摘要(python sha256_file:1MiB 块,绝不整读进内存)。
std::string Sha256File(const std::filesystem::path& file) {
    std::ifstream in(file, std::ios::binary);
    if (!in.is_open()) throw UpdaterFailure("读不了: " + Utf8(file));
    platform::Sha256Stream stream;
    std::string buffer(1u << 20, '\0');
    while (in.good()) {
        in.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
        if (in.gcount() > 0) {
            stream.Update(std::string_view(buffer.data(), static_cast<std::size_t>(in.gcount())));
        }
    }
    if (in.bad()) throw UpdaterFailure("读不了: " + Utf8(file));
    return stream.FinalHex();
}

void AtomicWriteOrThrow(const std::filesystem::path& target, const std::string& bytes) {
    const auto result =
        platform::AtomicWriteFile(target, bytes, platform::WriteDurability::ProcessCrashDurability);
    if (!result.has_value()) {
        throw UpdaterFailure("账落盘失败 " + Utf8(target) + ": " + result.error().code + " " +
                             result.error().message);
    }
}

std::string LayoutName(LayoutKind layout) {
    switch (layout) {
        case LayoutKind::Versioned:
            return "versioned";
        case LayoutKind::Flat:
            return "flat";
        case LayoutKind::Empty:
        default:
            return "empty";
    }
}

std::string DetectPlatform() {
#if defined(_WIN32)
    return "windows-x64";
#elif defined(__linux__)
    return "linux-x64";
#else
    return "macos-arm64";
#endif
}

#ifndef _WIN32
void FsyncDir(const std::filesystem::path& dir) {
    // python fsync_dir:Windows 目录不可 fsync,原子性靠原子替换——本函数
    // 只在 POSIX 侧编入。
    const int fd = ::open(Utf8(dir).c_str(), O_RDONLY | O_DIRECTORY);
    if (fd < 0) return;
    ::fsync(fd);
    ::close(fd);
}

void MakeExecutable(const std::filesystem::path& file) {
    std::error_code ec;
    std::filesystem::permissions(file,
                                 std::filesystem::perms::owner_exec |
                                     std::filesystem::perms::group_exec |
                                     std::filesystem::perms::others_exec,
                                 std::filesystem::perm_options::add, ec);
}
#else
void FsyncDir(const std::filesystem::path&) {}
void MakeExecutable(const std::filesystem::path&) {}
#endif

// 'sha256:<hex>' 或裸 hex -> 小写 hex(python normalize_digest;别的算法拒)。
std::string NormalizeDigest(const std::string& digest) {
    if (digest.empty()) {
        throw UpdaterFailure("缺少资产摘要(sha256);没有可信摘要不做自动安装");
    }
    std::string value = digest;
    const auto is_space = [](char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; };
    value.erase(std::remove_if(value.begin(), value.end(), is_space), value.end());
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (value.rfind("sha256:", 0) == 0) {
        value.erase(0, 7);
    } else if (value.find(':') != std::string::npos) {
        throw UpdaterFailure("不认的摘要格式: " + digest + "(只认 sha256)");
    }
    if (value.size() != 64 || value.find_first_not_of("0123456789abcdef") != std::string::npos) {
        throw UpdaterFailure("摘要不是 64 位十六进制: " + digest);
    }
    return value;
}

// 已落定的事务状态(failed/needs-review/rolled-back/committed):失败收尾与
// pending 统计都跳过(python TERMINAL_BAD ∪ {committed})。
bool TxnSettled(std::string_view state) {
    return state == kTxnFailed || state == kTxnNeedsReview || state == kTxnRolledBack ||
           state == kTxnCommitted;
}

// ---------------------------------------------------------------------------
// 目标与包核对
// ---------------------------------------------------------------------------

TxnTarget CollectTarget(const EngineArgs& args, const ProgressSink& sink) {
    TxnTarget target;
    if (!args.digest.empty()) {
        target.digest_hex = NormalizeDigest(args.digest);
    } else if (args.from_archive.has_value()) {
        std::error_code ec;
        if (!std::filesystem::is_regular_file(*args.from_archive, ec) || ec) {
            throw UpdaterFailure("本地包不存在: " + Utf8(*args.from_archive));
        }
        target.digest_hex = Sha256File(*args.from_archive);
        Say(sink, "[target] 本地包摘要 sha256:" + target.digest_hex);
    } else {
        throw UpdaterFailure(
            "缺少资产摘要(sha256)。GitHub 资产未带 digest 时须从可信更新元数据取,"
            "否则不做自动安装;本地包用 --archive 会自算摘要。");
    }
    if (args.version.empty()) throw UpdaterFailure("缺少 --version");
    target.version = args.version;
    target.tag = args.tag.empty() ? "v" + args.version : args.tag;
    target.exe_version = args.exe_version.empty() ? args.version : args.exe_version;
    target.platform = DetectPlatform();
    target.dirname = target.version + "-" + target.digest_hex.substr(0, 8);
    if (!args.repo.empty()) target.repo = args.repo;
    if (args.release_id != 0) target.release_id = static_cast<std::int64_t>(args.release_id);
    if (args.asset_id != 0) target.asset_id = static_cast<std::int64_t>(args.asset_id);
    if (!args.asset_name.empty()) target.asset_name = args.asset_name;
    if (args.asset_size.has_value()) target.asset_size = *args.asset_size;
    return target;
}

// verify_package(L601-650):逐文件核对 manifest(路径/大小/SHA-256)、包内
// 无清单外文件、EXE 在、平台资源(rg)在、EXE 探针过。任何一项不合即失败
// ——坏包不激活。返回清单原样 json(install-state 落账用)。
nlohmann::json VerifyPackage(const std::filesystem::path& pkg_dir, const TxnTarget& target) {
    std::error_code ec;
    const std::filesystem::path manifest_path = pkg_dir / "manifest.json";
    if (!std::filesystem::is_regular_file(manifest_path, ec) || ec) {
        throw UpdaterFailure("包里缺 manifest.json");
    }
    ec.clear();
    const auto raw = ReadJsonFileTolerant(manifest_path);
    if (!raw.has_value() || !raw->is_object()) {
        throw UpdaterFailure("包内清单 读不了: " + Utf8(manifest_path));
    }
    const auto text = ReadTextFile(manifest_path);
    if (!text.has_value()) {
        throw UpdaterFailure("包内清单 读不了: " + Utf8(manifest_path));
    }
    std::vector<std::string> problems;
    const auto parsed = ParseManifestText(*text, &problems);
    if (!parsed.has_value()) {
        std::string joined;
        for (const std::string& problem : problems) {
            if (!joined.empty()) joined += "; ";
            joined += problem;
        }
        throw UpdaterFailure("包内清单不干净: " + joined);
    }
    const std::string manifest_version = JsonStringField(*raw, "version");
    if (manifest_version != target.version) {
        throw UpdaterFailure("清单版本('" + manifest_version + "')与目标版本(" + target.version +
                             ")不合");
    }

    for (const auto& entry : parsed->files) {
        const std::filesystem::path full = pkg_dir / std::filesystem::path(entry.second.path);
        if (!std::filesystem::is_regular_file(full, ec) || ec) {
            throw UpdaterFailure("包内缺清单文件: " + entry.second.path);
        }
        ec.clear();
        const std::uintmax_t size = std::filesystem::file_size(full, ec);
        if (ec) throw UpdaterFailure("读不了: " + entry.second.path + ": " + ec.message());
        if (size != entry.second.size) {
            throw UpdaterFailure("大小不合: " + entry.second.path + "(清单 " +
                                 std::to_string(entry.second.size) + ",实得 " +
                                 std::to_string(size) + ")");
        }
        const std::string actual = Sha256File(full);
        if (!entry.second.sha256.empty() && actual != entry.second.sha256) {
            throw UpdaterFailure("摘要不合: " + entry.second.path + "(期望 " + entry.second.sha256 +
                                 ",实得 " + actual + ")");
        }
    }

    // 清单外文件(拼套检测):manifest.json 自己除外。
    std::set<std::string> allowed;
    for (const auto& entry : parsed->files) {
        allowed.insert(entry.first);
    }
    allowed.insert(FoldKey("manifest.json"));
    for (std::filesystem::recursive_directory_iterator it(pkg_dir, ec), end; !ec && it != end;
         it.increment(ec)) {
        if (!it->is_regular_file()) continue;
        const std::string rel =
            std::filesystem::relative(it->path(), pkg_dir, ec).generic_string();
        if (ec) {
            throw UpdaterFailure("包目录扫不动: " + Utf8(pkg_dir) + ": " + ec.message());
        }
        if (!allowed.count(FoldKey(rel))) {
            throw UpdaterFailure("包内有清单外文件: " + rel);
        }
    }
    if (ec) {
        throw UpdaterFailure("包目录扫不动: " + Utf8(pkg_dir) + ": " + ec.message());
    }

    const std::filesystem::path exe = pkg_dir / kExeName;
    if (!std::filesystem::is_regular_file(exe, ec) || ec) {
        throw UpdaterFailure(std::string("包里缺 ") + kExeName);
    }
#ifndef _WIN32
    MakeExecutable(exe);
    const std::filesystem::path bundled_rg = pkg_dir / "libexec" / kRgName;
    ec.clear();
    if (std::filesystem::is_regular_file(bundled_rg, ec) && !ec) MakeExecutable(bundled_rg);
#endif
    const ProbeOutcome probe = ProbeExe(exe, target.exe_version, kProbeTimeoutSecs);
    if (!probe.ok) throw UpdaterFailure("EXE 探针不过: " + probe.detail);
    ec.clear();
    if (!std::filesystem::is_regular_file(pkg_dir / "libexec" / kRgName, ec) || ec) {
        throw UpdaterFailure(std::string("包里缺 libexec/") + kRgName);
    }
    return *raw;
}

// ---------------------------------------------------------------------------
// 磁盘预检 / 本地包 / 下载
// ---------------------------------------------------------------------------

std::uint64_t EstimateNeed(std::optional<std::uint64_t> asset_size) {
    const std::uint64_t base = asset_size.has_value() ? *asset_size : kEstimateFallbackBytes;
    return base * 3;  // 包 + 解包 + 备份余量
}

void DiskPreflight(const std::filesystem::path& root, std::uint64_t need_bytes) {
    std::error_code ec;
    const std::filesystem::space_info info = std::filesystem::space(root, ec);
    if (ec) throw UpdaterFailure("磁盘余量查不了: " + Utf8(root) + ": " + ec.message());
    if (info.available < need_bytes + kDiskHeadroomBytes) {
        throw UpdaterFailure("磁盘空间不够:需要约 " +
                             std::to_string((need_bytes + kDiskHeadroomBytes) >> 20) +
                             " MiB(含余量)," + Utf8(root) + " 只剩 " +
                             std::to_string(info.available >> 20) + " MiB");
    }
}

void CopyLocalArchive(const std::filesystem::path& src, Transaction& txn, const TxnTarget& target) {
    std::error_code ec;
    if (!std::filesystem::is_regular_file(src, ec) || ec) {
        throw UpdaterFailure("本地包不存在: " + Utf8(src));
    }
    const std::filesystem::path dest = txn.ArchivePath();
    std::filesystem::create_directories(dest.parent_path(), ec);
    const std::string digest = Sha256File(src);
    if (!target.digest_hex.empty() && digest != target.digest_hex) {
        throw UpdaterFailure("本地包摘要不符:期望 sha256:" + target.digest_hex + ",实得 sha256:" +
                             digest);
    }
    ec.clear();
    std::filesystem::copy_file(src, dest, std::filesystem::copy_options::overwrite_existing, ec);
    if (ec) throw UpdaterFailure("本地包拷贝失败: " + Utf8(dest) + ": " + ec.message());
    nlohmann::json fields = nlohmann::json::object();
    fields["archive_sha256"] = "sha256:" + digest;
    fields["source_archive"] = Utf8(std::filesystem::absolute(src).lexically_normal());
    txn.Transition(kTxnVerified, std::move(fields));
}

std::string GithubAssetUrl(const std::string& repo, std::int64_t asset_id) {
    return "https://api.github.com/repos/" + repo + "/releases/assets/" + std::to_string(asset_id);
}

// ---------------------------------------------------------------------------
// 平铺完整备份与 gc
// ---------------------------------------------------------------------------

std::filesystem::path FullBackup(const std::filesystem::path& root) {
    // 无基线时的完整备份(install_plan.full_backup):受管树全量 + 记录目录
    // 顶层文件(记录件除外),全部原样进备份。
    std::error_code ec;
    const std::filesystem::path backup_root = root / "backups" / MakeTxnId();
    std::filesystem::create_directories(backup_root, ec);
    if (ec) throw UpdaterFailure("建备份目录失败: " + Utf8(backup_root) + ": " + ec.message());
    for (const char* tree : kRoleTopDirs) {
        const std::filesystem::path src = root / tree;
        if (!std::filesystem::is_directory(src, ec) || ec || std::filesystem::is_symlink(src, ec)) {
            ec.clear();
            continue;
        }
        std::error_code copy_ec;
        std::filesystem::copy(src, backup_root / tree,
                              std::filesystem::copy_options::recursive |
                                  std::filesystem::copy_options::copy_symlinks,
                              copy_ec);
        if (copy_ec) {
            throw UpdaterFailure("平铺备份拷贝失败(" + std::string(tree) + "): " + copy_ec.message());
        }
    }
    std::vector<std::string> names;
    for (std::filesystem::directory_iterator it(root, ec), end; !ec && it != end; it.increment(ec)) {
        if (!it->is_regular_file()) continue;
        names.push_back(it->path().filename().string());
    }
    std::sort(names.begin(), names.end());
    for (const std::string& name : names) {
        if (name == "manifest.json" || name == "install-state.json") continue;  // RECORD_FILES
        std::error_code copy_ec;
        std::filesystem::copy_file(root / name, backup_root / name,
                                   std::filesystem::copy_options::overwrite_existing, copy_ec);
        if (copy_ec) {
            throw UpdaterFailure("平铺备份拷贝失败(" + name + "): " + copy_ec.message());
        }
    }
    return backup_root;
}

// 版本目录在用吗(python version_in_use):Windows 主 EXE 独占写探测;POSIX
// 扫 /proc,任何进程的 exe 是这棵目录里的主 EXE 或随包 rg 即算(macOS 无
// /proc 恒 false,清理侧靠 keep 集合兜底)。
bool VersionInUse(const std::filesystem::path& version_dir) {
    std::error_code ec;
    const std::filesystem::path exe = version_dir / kExeName;
    if (std::filesystem::is_regular_file(exe, ec) && !ec && platform::IsFileLockedForWrite(exe)) {
        return true;
    }
#ifndef _WIN32
    const std::filesystem::path bundled_rg = version_dir / "libexec" / kRgName;
    std::error_code proc_ec;
    for (std::filesystem::directory_iterator it("/proc", proc_ec), end; !proc_ec && it != end;
         it.increment(proc_ec)) {
        const std::string pid_name = it->path().filename().string();
        if (pid_name.empty() || pid_name.find_first_not_of("0123456789") != std::string::npos) {
            continue;
        }
        const unsigned long pid = std::strtoul(pid_name.c_str(), nullptr, 10);
        if (platform::ProcessHoldsExe(pid, exe)) return true;
        if (platform::ProcessHoldsExe(pid, bundled_rg)) return true;
    }
#endif
    return false;
}

std::optional<Manifest> LoadVersionManifest(const std::filesystem::path& version_dir) {
    const auto text = ReadTextFile(version_dir / "manifest.json");
    if (!text.has_value()) return std::nullopt;
    return ParseManifestText(*text);
}

std::vector<std::string> GcVersions(const LayoutPaths& paths, const ProgressSink& sink) {
    // 至少保留一份已知可用整包;跳过当前/上次可用/在途/运行中版本。删版本
    // 目录前,清单外文件(用户塞进去的)先抢救进 backups/stranded-<名>/。
    std::vector<std::string> removed;
    const auto pointer = ReadCurrent(paths);
    std::set<std::string> keep;
    if (pointer.has_value()) {
        keep.insert(pointer->current);
        if (pointer->previous.has_value()) keep.insert(*pointer->previous);
    }
    std::set<std::string> pending;
    for (const Transaction& txn : ListTransactions(paths)) {
        if (TxnSettled(txn.state())) continue;
        const std::string dirname = JsonStringField(txn.data(), "target_dirname");
        if (!dirname.empty()) pending.insert(dirname);
    }
    std::error_code ec;
    if (!std::filesystem::is_directory(paths.versions, ec) || ec) return removed;
    std::vector<std::string> names;
    for (std::filesystem::directory_iterator it(paths.versions, ec), end; !ec && it != end;
         it.increment(ec)) {
        if (!it->is_directory()) continue;
        names.push_back(it->path().filename().string());
    }
    std::sort(names.begin(), names.end());
    for (const std::string& name : names) {
        const std::filesystem::path full = paths.versions / name;
        if (!std::filesystem::is_directory(full, ec) || ec) {
            ec.clear();
            continue;
        }
        if (keep.count(name) || pending.count(name)) continue;
        if (VersionInUse(full)) {
            Say(sink, "[gc] " + name + " 有进程在用,跳过");
            continue;
        }
        const auto manifest = LoadVersionManifest(full);
        std::set<std::string> listed;
        if (manifest.has_value()) {
            for (const auto& entry : manifest->files) listed.insert(entry.first);
        }
        int stranded = 0;
        for (std::filesystem::recursive_directory_iterator it(full, ec), end; !ec && it != end;
             it.increment(ec)) {
            if (ec || !it->is_regular_file()) continue;
            std::error_code rel_ec;
            const std::filesystem::path rel = std::filesystem::relative(it->path(), full, rel_ec);
            if (rel_ec || rel.empty()) {
                ec = rel_ec;
                break;
            }
            if (listed.count(FoldKey(rel.generic_string()))) continue;
            ++stranded;
            const std::filesystem::path rescue = paths.backups / ("stranded-" + name) / rel;
            std::error_code make_ec;
            std::filesystem::create_directories(rescue.parent_path(), make_ec);
            if (make_ec) throw UpdaterFailure("抢救目录建不成: " + Utf8(rescue.parent_path()));
            std::error_code copy_ec;
            std::filesystem::copy_file(it->path(), rescue,
                                       std::filesystem::copy_options::overwrite_existing, copy_ec);
            if (copy_ec) {
                throw UpdaterFailure("清单外文件抢救失败: " + Utf8(rescue) + ": " + copy_ec.message());
            }
        }
        if (ec) throw UpdaterFailure("版本目录扫不动: " + Utf8(full) + ": " + ec.message());
        std::error_code remove_ec;
        std::filesystem::remove_all(full, remove_ec);
        if (remove_ec) {
            throw UpdaterFailure("旧版本删不掉: " + Utf8(full) + ": " + remove_ec.message());
        }
        if (stranded > 0) {
            Say(sink, "[gc] 已移除旧版本 " + name + ";清单外文件 " + std::to_string(stranded) +
                          " 个保存在 backups/stranded-" + name + "/");
        } else {
            Say(sink, "[gc] 已移除旧版本 " + name);
        }
        removed.push_back(name);
    }
    return removed;
}

// ---------------------------------------------------------------------------
// 激活 / 交接 / 回滚 / 提交
// ---------------------------------------------------------------------------

void SyncUpdaterTree(const LayoutPaths& paths, const std::filesystem::path& version_path) {
    // 把版本目录里的 updater 树按清单文件覆盖同步到根(python sync_updater_tree:
    // 不清不删——更新助手自己就住在那棵树里)。
    std::error_code ec;
    const std::filesystem::path src_tree = version_path / "updater";
    if (!std::filesystem::is_directory(src_tree, ec) || ec) return;
    for (std::filesystem::recursive_directory_iterator it(src_tree, ec), end; !ec && it != end;
         it.increment(ec)) {
        if (ec || !it->is_regular_file()) continue;
        const std::filesystem::path rel = std::filesystem::relative(it->path(), src_tree, ec);
        if (ec || rel.empty()) continue;
        const std::filesystem::path dest = paths.updater / rel;
        std::error_code make_ec;
        std::filesystem::create_directories(dest.parent_path(), make_ec);
        if (make_ec) throw UpdaterFailure("updater 树目录建不成: " + Utf8(dest.parent_path()));
        std::error_code copy_ec;
        std::filesystem::copy_file(it->path(), dest, std::filesystem::copy_options::overwrite_existing,
                                   copy_ec);
        if (copy_ec) {
            throw UpdaterFailure("updater 树同步失败: " + Utf8(dest) + ": " + copy_ec.message());
        }
    }
}

void HandoverFlatLauncher(const LayoutPaths& paths, Transaction& txn, const TxnTarget& target) {
    // 平铺 -> 版本化交接(§六):不替换运行中的 EXE,不强杀——旧根 EXE 改名
    // 挪进 backups/<txn>/legacy/(Windows 对运行中映像允许改名,运行中的进程
    // 不受影响);新版 EXE 落根位当固定启动器;updater 树同步到根。挪不动
    // (被锁)停 needs-review,等用户退出后重跑续上。
    std::error_code ec;
    const std::filesystem::path version_exe = paths.versions / target.dirname / kExeName;
    if (std::filesystem::absolute(paths.exe).lexically_normal() ==
        std::filesystem::absolute(version_exe).lexically_normal()) {
        throw UpdaterFailure("安装根与版本目录重叠,拒绝交接");
    }
    const std::filesystem::path legacy_dir = paths.backups / txn.id() / "legacy";
    std::filesystem::create_directories(legacy_dir, ec);
    if (ec) {
        throw NeedsReviewFailure("旧根 EXE 的备份目录建不成(" + Utf8(legacy_dir) + ": " + ec.message() +
                                 ")。处理完再重跑 lubancode update 续上"
                                 "(已下载核对的包不重下)。绝不强杀。");
    }
    const std::filesystem::path legacy_target = legacy_dir / kExeName;
    if (std::filesystem::is_regular_file(paths.exe, ec) && !ec) {
        std::error_code remove_ec;
        std::filesystem::remove(legacy_target, remove_ec);
        std::error_code rename_ec;
        std::filesystem::rename(paths.exe, legacy_target, rename_ec);
        if (rename_ec) {
            throw NeedsReviewFailure(
                "旧根 EXE 挪不进备份(" + rename_ec.message() +
                ")——多半仍被运行中的进程/杀软锁着。"
                "退出所有 lubancode 进程后重跑 lubancode update 续上(已下载核对的包不重下)。"
                "绝不强杀。");
        }
    }
    ec.clear();
    std::filesystem::copy_file(version_exe, paths.exe,
                               std::filesystem::copy_options::overwrite_existing, ec);
    if (ec) throw UpdaterFailure("新版 EXE 落根位失败: " + Utf8(paths.exe) + ": " + ec.message());
    MakeExecutable(paths.exe);
    SyncUpdaterTree(paths, paths.versions / target.dirname);
}

bool RestoreFlatLegacy(const LayoutPaths& paths, Transaction& txn) {
    // 从平铺交接的备份恢复旧根 EXE。恢复不成也要摘掉 current 指针——平铺
    // 旧 EXE 不认指针,留着会把后续启动当启动器空转(必摘)。
    std::error_code ec;
    const std::filesystem::path legacy = paths.backups / txn.id() / "legacy" / kExeName;
    bool restored = false;
    if (std::filesystem::is_regular_file(legacy, ec) && !ec) {
        if (std::filesystem::is_regular_file(paths.exe, ec) && !ec) {
            // launcher-parked 挪移尽力而为(python 裸 rename,失败即炸恢复链;
            // 这里吞错继续——必摘指针那条硬保证优先)。
            std::error_code park_ec;
            std::filesystem::rename(paths.exe,
                                    paths.backups / txn.id() /
                                        ("launcher-parked-" + std::string(kExeName)),
                                    park_ec);
        }
        ec.clear();
        std::filesystem::copy_file(legacy, paths.exe,
                                   std::filesystem::copy_options::overwrite_existing, ec);
        if (ec) {
            throw UpdaterFailure("平铺旧 EXE 恢复失败: " + Utf8(paths.exe) + ": " + ec.message());
        }
        MakeExecutable(paths.exe);
        restored = true;
    }
    std::error_code remove_ec;
    std::filesystem::remove(paths.current, remove_ec);  // 必摘 current.json
    return restored;
}

void RollbackAfterActivationFailure(const LayoutPaths& paths, Transaction& txn,
                                    const std::string& reason) {
    // 激活后失败:恢复旧指针,保留诊断(§七)。先试指回旧版本目录;平铺
    // 来的就恢复旧根 EXE 并摘掉指针。
    nlohmann::json detail = nlohmann::json::object();
    detail["reason"] = reason;
    detail["rolled_back_at_utc"] = UtcNowIso8601();
    const std::string previous = JsonStringField(txn.data(), "rollback_current");
    bool restored = false;
    if (!previous.empty()) {
        const std::filesystem::path prev_dir = paths.versions / previous;
        const ProbeOutcome probe = ProbeExe(prev_dir / kExeName, std::nullopt, kProbeTimeoutSecs);
        std::error_code ec;
        if (std::filesystem::is_directory(prev_dir, ec) && !ec && probe.ok) {
            std::optional<std::string> old_target;
            const std::string dirname = JsonStringField(txn.data(), "target_dirname");
            if (!dirname.empty()) old_target = dirname;
            WriteCurrent(paths, previous, old_target, txn.id());
            detail["restored_to"] = previous;
            restored = true;
        } else {
            detail["previous_probe"] = probe.detail;
        }
    }
    if (!restored) {
        detail["restored_flat"] = RestoreFlatLegacy(paths, txn);
    }
    txn.Transition(kTxnRolledBack, std::move(detail));
    CleanupStaging(paths, txn.id());
}

void Activate(const LayoutPaths& paths, Transaction& txn, const TxnTarget& target,
              const ProgressSink& sink) {
    const LayoutKind layout = DetectLayout(paths.root);
    const std::filesystem::path version_path = paths.versions / target.dirname;
    const auto old_pointer = ReadCurrent(paths);

    nlohmann::json fields = nlohmann::json::object();
    fields["rollback_current"] =
        old_pointer.has_value() ? nlohmann::json(old_pointer->current) : nlohmann::json();
    fields["layout_before"] = LayoutName(layout);
    txn.Transition(kTxnWaitingForIdle, std::move(fields));
    // 版本化布局:激活不碰旧版本目录、不碰运行中的 EXE,无需等待。
    // 平铺交接的等待逻辑在 HandoverFlatLauncher(改名失败停 needs-review)。
    txn.Transition(kTxnActivating);

    std::error_code ec;
    if (!std::filesystem::is_directory(version_path, ec) || ec) {
        ec.clear();
        const std::filesystem::path staged_pkg = txn.StageDir() / "pkg";
        std::filesystem::create_directories(paths.versions, ec);
        if (ec) {
            throw UpdaterFailure("建 versions/ 失败: " + Utf8(paths.versions) + ": " + ec.message());
        }
        std::error_code rename_ec;
        std::filesystem::rename(staged_pkg, version_path, rename_ec);
        if (rename_ec) {
            throw UpdaterFailure("版本目录就位失败 " + Utf8(version_path) + ": " + rename_ec.message());
        }
        FsyncDir(paths.versions);
    }

    WriteCurrent(paths, target.dirname,
                 old_pointer.has_value() ? std::optional<std::string>(old_pointer->current)
                                         : std::nullopt,
                 txn.id());

    if (layout == LayoutKind::Flat) {
        HandoverFlatLauncher(paths, txn, target);
        Say(sink, "[activate] 平铺安装已交接为固定入口布局;旧 EXE 保存在 " +
                      Utf8(paths.backups / txn.id() / "legacy"));
    }
    Say(sink, "[activate] current -> " + target.dirname);
}

void HealthAndCommit(const LayoutPaths& paths, Transaction& txn, const TxnTarget& target,
                     const nlohmann::json& manifest, const std::string& channel,
                     const ProgressSink& sink) {
    const ProbeOutcome probe =
        ProbeExe(paths.versions / target.dirname / kExeName, target.exe_version, kProbeTimeoutSecs);
    if (!probe.ok) {
        RollbackAfterActivationFailure(paths, txn, "健康检查不过: " + probe.detail);
        throw RolledBackFailure("新版健康检查失败(" + probe.detail + "),已恢复旧版;诊断保留在 " +
                                Utf8(txn.LedgerPath()));
    }
    nlohmann::json fields = nlohmann::json::object();
    fields["health_probe"] = probe.detail;
    txn.Transition(kTxnHealthy, std::move(fields));

    InstallState state;
    state.version = target.version;
    state.platform = target.platform;
    state.channel = channel;
    state.source_repo = target.repo;
    state.source_release_id = target.release_id;
    state.source_release_tag = target.tag;
    state.source_asset_id = target.asset_id;
    state.source_asset_name = target.asset_name;
    state.source_asset_digest = "sha256:" + target.digest_hex;
    state.source_download_url = target.download_url;
    state.installer = "lubancode-updater";  // 口径差:python 写 "updater.py",账面身份如实
    state.transaction = txn.id();
    state.manifest = manifest;
    WriteInstallState(paths, state);

    nlohmann::json commit_fields = nlohmann::json::object();
    commit_fields["committed_at_utc"] = UtcNowIso8601();
    txn.Transition(kTxnCommitted, std::move(commit_fields));
    CleanupStaging(paths, txn.id());
    GcVersions(paths, sink);
    Say(sink, "[commit] " + target.version + " 已上线;事务 " + txn.id() + " 提交。");
}

// ---------------------------------------------------------------------------
// 动词:update
// ---------------------------------------------------------------------------

int RunUpdate(const EngineArgs& args, const ProgressSink& sink, const PrecheckFn& precheck) {
    const std::filesystem::path root =
        std::filesystem::absolute(args.install_root).lexically_normal();
    TxnTarget target = CollectTarget(args, sink);
    const LayoutPaths paths = MakeLayoutPaths(root);

    InstallRootLock lock;
    const auto acquired = InstallRootLock::TryAcquire(paths, &lock);
    if (acquired.status != InstallRootLock::AcquireResult::Status::Acquired) {
        throw UpdaterFailure("取安装锁失败: " + acquired.detail);
    }
    if (acquired.cleared_stale) {
        Say(sink, "[lock] 清掉陈锁(挪到 " + acquired.stale_moved_to + ")");
    }

    std::optional<Transaction> txn;
    try {
        // 在途事务:同目标续跑,异目标作废(§七:每一步可重入)。
        ResumableDecision decision = FindResumable(paths, target.digest_hex);
        if (!decision.resume.has_value()) {
            txn = Transaction(paths, MakeTxnId());
            txn->Create(target);
        } else {
            txn = std::move(*decision.resume);
            Say(sink, "[resume] 续上事务 " + txn->id() + "(状态 " + txn->state() + ")");
            txn->data()["target"] = target.ToJson();
            txn->Flush();
        }

        std::error_code ec;
        const std::filesystem::path version_path = paths.versions / target.dirname;
        nlohmann::json manifest = nlohmann::json::object();
        if (std::filesystem::is_directory(version_path, ec) && !ec) {
            // 已有同摘要版本目录:整包重核对后直接进激活(重跑幂等,防目录
            // 被动手脚)。
            manifest = VerifyPackage(version_path, target);
            const std::string state = txn->state();
            const bool already_staged = state == std::string(kTxnStaged) ||
                                        state == std::string(kTxnWaitingForIdle) ||
                                        state == std::string(kTxnActivating) ||
                                        state == std::string(kTxnHealthy) ||
                                        state == std::string(kTxnNeedsReview);
            if (!already_staged) {
                nlohmann::json fields = nlohmann::json::object();
                fields["note"] = "复用已核对的版本目录,免下载";
                txn->Transition(kTxnStaged, std::move(fields));
            }
            Say(sink, "[stage] 版本目录 " + target.dirname + " 已在且核对通过,跳过下载");
        } else {
            if (txn->state() == std::string(kTxnChecking)) {
                DiskPreflight(root, EstimateNeed(target.asset_size));
                txn->Transition(kTxnDownloading);
            }

            const std::filesystem::path archive = txn->ArchivePath();
            if (!std::filesystem::is_regular_file(archive, ec) || ec) {
                if (args.from_archive.has_value()) {
                    CopyLocalArchive(*args.from_archive, *txn, target);
                } else {
                    if (!target.asset_id.has_value() || !target.repo.has_value()) {
                        throw UpdaterFailure("缺 --asset-id/--repo(联网下载必需);本地包走 --archive");
                    }
                    const std::string url = GithubAssetUrl(*target.repo, *target.asset_id);
                    target.download_url = url;
                    txn->data()["target"] = target.ToJson();
                    txn->Flush();
                    DownloadOptions options;
                    // python download_with_resume 递 GitHub 资产 API 的 Accept 头
                    // (缺了它 API 回 JSON 元数据而不是资产体)。
                    options.extra_headers.emplace_back("Accept", "application/octet-stream");
                    const auto on_progress = [&sink](std::uint64_t bytes) {
                        Say(sink, "[download] " + std::to_string(bytes >> 20) + " MiB");
                    };
                    const auto download = DownloadWithResume(url, archive, target.digest_hex,
                                                             target.asset_size, options, nullptr,
                                                             on_progress);
                    if (!download.has_value()) throw UpdaterFailure(download.error());
                }
            }
            if (txn->state() != std::string(kTxnVerified)) {
                nlohmann::json fields = nlohmann::json::object();
                fields["archive_sha256"] = "sha256:" + target.digest_hex;
                txn->Transition(kTxnVerified, std::move(fields));
            }

            const std::filesystem::path pkg_dir = txn->StageDir() / "pkg";
            std::error_code pkg_ec;
            std::filesystem::remove_all(pkg_dir, pkg_ec);
            std::filesystem::create_directories(pkg_dir, pkg_ec);  // staging 先造,解包件只管拆
            if (pkg_ec) {
                throw UpdaterFailure("staging 建不成: " + Utf8(pkg_dir) + ": " + pkg_ec.message());
            }
            const auto unpacked = UnpackArchive(archive, pkg_dir, target.asset_size);
            if (!unpacked.has_value()) throw UpdaterFailure(unpacked.error());
            manifest = VerifyPackage(pkg_dir, target);
            txn->Transition(kTxnStaged);
        }

        // 激活前技能保护预检(§七):冲突未决停 needs-review,staging 保留待续。
        const std::filesystem::path precheck_dir =
            std::filesystem::is_directory(version_path, ec) && !ec ? version_path
                                                                   : txn->StageDir() / "pkg";
        if (precheck) {
            const auto verdict = precheck(precheck_dir, root);
            if (!verdict.has_value()) {
                nlohmann::json blocking = nlohmann::json::array();
                blocking.push_back(nlohmann::json{{"detail", verdict.error()}});
                nlohmann::json fields = nlohmann::json::object();
                fields["blocking"] = std::move(blocking);
                txn->Transition(kTxnNeedsReview, std::move(fields));
                throw NeedsReviewFailure(
                    "技能保护预检报告冲突未决,已停在 needs-review(安装未变,旧版继续可用)。\n"
                    "逐项处理(移走自改文件或确认采用官方新版)后重跑 lubancode update 续上。\n"
                    "明细: " + Utf8(txn->LedgerPath()) + "\n冲突详情: " + verdict.error());
            }
        }

        if (DetectLayout(paths.root) == LayoutKind::Flat) {
            // 平铺迁移先完整备份(§五.4:宁可多备份),挪进本事务名下。
            bool any_tree = false;
            for (const char* tree : kRoleTopDirs) {
                std::error_code tree_ec;
                if (std::filesystem::is_directory(root / tree, tree_ec) && !tree_ec) {
                    any_tree = true;
                    break;
                }
            }
            if (any_tree) {
                std::filesystem::path backup_root = FullBackup(root);
                const std::filesystem::path txn_backup = paths.backups / txn->id() / "flat";
                std::error_code make_ec;
                std::filesystem::create_directories(txn_backup.parent_path(), make_ec);
                std::error_code rename_ec;
                std::filesystem::rename(backup_root, txn_backup, rename_ec);
                if (!rename_ec) backup_root = txn_backup;  // 挪不动就留原地,账上照记路径
                txn->Note("平铺迁移完整备份: " + Utf8(backup_root));
                Say(sink, "[backup] 平铺安装完整备份: " + Utf8(backup_root));
            }
        }

        Activate(paths, *txn, target, sink);
        HealthAndCommit(paths, *txn, target, manifest,
                        args.channel.empty() ? std::string("stable") : args.channel, sink);
        return 0;
    } catch (const NeedsReviewFailure&) {
        throw;
    } catch (const RolledBackFailure&) {
        throw;
    } catch (UpdaterFailure& error) {
        // 异常分流照 python L1272-1287:指针已换且 state∈{activating,healthy}
        // -> 回滚路退 3;否则 failed 清 staging 退 1。
        if (txn.has_value() && !TxnSettled(txn->state())) {
            const auto swapped = ReadCurrent(paths);
            const std::string state = txn->state();
            if (swapped.has_value() && swapped->current == target.dirname &&
                (state == std::string(kTxnActivating) || state == std::string(kTxnHealthy))) {
                RollbackAfterActivationFailure(paths, *txn,
                                                "激活中途失败: " + std::string(error.what()));
                throw RolledBackFailure("激活中途失败,已恢复旧版;诊断: " + Utf8(txn->LedgerPath()));
            }
            nlohmann::json fields = nlohmann::json::object();
            fields["reason"] = std::string(error.what());
            txn->Transition(kTxnFailed, std::move(fields));
            CleanupStaging(paths, txn->id());
        }
        throw;
    }
}

// ---------------------------------------------------------------------------
// 动词:status / plan / rollback / gc
// ---------------------------------------------------------------------------

int RunStatus(const EngineArgs& args, const ProgressSink& sink) {
    const std::filesystem::path root =
        std::filesystem::absolute(args.install_root).lexically_normal();
    const LayoutPaths paths = MakeLayoutPaths(root);
    const LayoutKind layout = DetectLayout(root);
    const auto pointer = ReadCurrent(paths);
    const auto state = ReadJsonFileTolerant(paths.state);
    const bool state_is_object = state.has_value() && state->is_object();

    const std::vector<Transaction> txns = ListTransactions(paths);
    std::vector<const Transaction*> pending;
    for (const Transaction& txn : txns) {
        if (!TxnSettled(txn.state())) pending.push_back(&txn);
    }

    if (args.json) {
        nlohmann::json out = nlohmann::json::object();
        out["layout"] = LayoutName(layout);
        out["current"] = pointer.has_value() ? nlohmann::json(pointer->current) : nlohmann::json();
        out["previous"] = pointer.has_value() && pointer->previous.has_value()
                              ? nlohmann::json(*pointer->previous)
                              : nlohmann::json();
        if (state_is_object) {
            nlohmann::json installed = nlohmann::json::object();
            installed["version"] = StringFieldOrJson(*state, "version");
            installed["channel"] = StringFieldOrJson(*state, "channel");
            installed["source"] =
                (*state).contains("source") ? (*state)["source"] : nlohmann::json();
            out["installed"] = std::move(installed);
        } else {
            out["installed"] = nlohmann::json();
        }
        nlohmann::json pending_json = nlohmann::json::array();
        for (const Transaction* txn : pending) {
            nlohmann::json row = nlohmann::json::object();
            row["id"] = txn->id();
            row["state"] = txn->state();
            row["target"] = StringFieldOrJson(txn->data(), "target_version");
            pending_json.push_back(std::move(row));
        }
        out["pending_transactions"] = std::move(pending_json);
        Say(sink, CanonicalJsonDump(out));
        return 0;
    }

    Say(sink, "安装根: " + Utf8(root));
    Say(sink, "布局: " + LayoutName(layout));
    if (pointer.has_value()) {
        Say(sink, "当前版本: " + pointer->current);
        Say(sink, "上次可用: " +
                      (pointer->previous.has_value() ? *pointer->previous : std::string("(无)")));
    } else {
        Say(sink, "尚无 current 指针(未做过一键更新)");
    }
    if (state_is_object) {
        const nlohmann::json source =
            (*state).contains("source") && (*state)["source"].is_object() ? (*state)["source"]
                                                                          : nlohmann::json::object();
        Say(sink, "安装来源: " + JsonStringField(source, "repo") + " tag=" +
                      JsonStringField(source, "release_tag") + " asset=" +
                      JsonStringField(source, "asset_name"));
    }
    for (const Transaction* txn : pending) {
        Say(sink, "在途事务: " + txn->id() + " 状态=" + txn->state() + " 目标=" +
                      JsonStringField(txn->data(), "target_version"));
    }
    if (pending.empty()) Say(sink, "没有在途事务。");
    return 0;
}

int RunPlan(const EngineArgs& args, const ProgressSink& sink, const PrecheckFn& precheck) {
    const std::filesystem::path root =
        std::filesystem::absolute(args.install_root).lexically_normal();
    const TxnTarget target = CollectTarget(args, sink);
    const LayoutKind layout = DetectLayout(root);

    Say(sink, "== 更新预演(不改安装、不动用户数据)==");
    Say(sink, "目标版本: " + target.version + "(tag " + target.tag + ",平台 " + target.platform + ")");
    Say(sink, "资产: " +
                  (target.asset_name.has_value() ? *target.asset_name : std::string("(未知)")) +
                  "(" +
                  (target.asset_size.has_value() ? std::to_string(*target.asset_size)
                                                 : std::string("未知")) +
                  " 字节)摘要 sha256:" + target.digest_hex);
    Say(sink, "当前布局: " + LayoutName(layout));

    const std::uint64_t need = EstimateNeed(target.asset_size);
    std::error_code ec;
    if (std::filesystem::is_directory(root, ec) && !ec) {
        std::error_code space_ec;
        const std::filesystem::space_info info = std::filesystem::space(root, space_ec);
        if (!space_ec) {
            Say(sink, "磁盘预检: 需约 " + std::to_string(need >> 20) + " MiB,余 " +
                          std::to_string(info.available >> 20) + " MiB" +
                          (info.available < need + kDiskHeadroomBytes ? "(偏紧)" : "(够)"));
        }
    }

    Say(sink, "将创建版本目录: versions/" + target.dirname + "(整包,不可变)");
    if (layout == LayoutKind::Flat) {
        Say(sink,
            "将执行平铺 -> 版本化迁移:受管树先完整备份;旧根 EXE 改名挪进"
            " backups/<txn>/legacy/,新版 EXE 落根位当固定启动器(不强杀、"
            "不替换运行中的映像)。");
    }
    Say(sink, "用户技能根 / 项目 .lubancode/.agents / 配置会话记忆凭据:一概不动。");

    if (args.from_archive.has_value()) {
        // 本地包:解到系统临时目录读清单,给出预演(不动安装根)。
        const std::filesystem::path tmp_pkg =
            std::filesystem::temp_directory_path() / ("lubancode-plan-" + MakeTxnId());
        std::error_code make_ec;
        std::filesystem::create_directories(tmp_pkg, make_ec);
        if (make_ec) throw UpdaterFailure("临时预演目录建不成: " + Utf8(tmp_pkg));
        try {
            const auto unpacked = UnpackArchive(*args.from_archive, tmp_pkg, target.asset_size);
            if (!unpacked.has_value()) throw UpdaterFailure(unpacked.error());
            const auto manifest = ReadJsonFileTolerant(tmp_pkg / "manifest.json");
            int count = 0;
            if (manifest.has_value() && manifest->is_object() && manifest->contains("file_count") &&
                (*manifest)["file_count"].is_number_integer()) {
                count = (*manifest)["file_count"].get<int>();
            }
            Say(sink, "本地包核对: " + std::to_string(count) +
                          " 个官方文件(解临时目录核对,不动安装)");
            if (precheck) {
                const auto verdict = precheck(tmp_pkg, root);
                if (!verdict.has_value()) {
                    Say(sink, "注意: 技能保护预检有冲突未决。执行更新会停在 needs-review,"
                              "官方新版不落这些路径,原件保留。");
                    Say(sink, "冲突详情: " + verdict.error());
                } else {
                    Say(sink, "技能保护预检: 没有阻断项。");
                }
            }
        } catch (...) {
            std::error_code cleanup_ec;
            std::filesystem::remove_all(tmp_pkg, cleanup_ec);
            throw;
        }
        std::error_code cleanup_ec;
        std::filesystem::remove_all(tmp_pkg, cleanup_ec);
    } else {
        Say(sink, "下载: GitHub Release " + target.tag + " 的 asset " +
                      (target.asset_id.has_value()
                           ? std::to_string(*target.asset_id)
                           : (target.asset_name.has_value() ? *target.asset_name
                                                            : std::string("(未知)"))) +
                      "(固定 asset id 与摘要;断点续传)");
    }
    Say(sink, "激活: current.json 同文件系统原子换指针(写前持久化事务与回退指针),"
              "旧版本目录不动。");
    Say(sink, "健康检查: 隔离数据根跑 " + std::string(kExeName) + " --version;过了才提交。");
    Say(sink, "回滚: lubancode update --rollback 切回上次可用整包。");
    return 0;
}

int RunRollback(const EngineArgs& args, const ProgressSink& sink) {
    const std::filesystem::path root =
        std::filesystem::absolute(args.install_root).lexically_normal();
    const LayoutPaths paths = MakeLayoutPaths(root);

    InstallRootLock lock;
    const auto acquired = InstallRootLock::TryAcquire(paths, &lock);
    if (acquired.status != InstallRootLock::AcquireResult::Status::Acquired) {
        throw UpdaterFailure("取安装锁失败: " + acquired.detail);
    }

    const auto pointer = ReadCurrent(paths);
    if (!pointer.has_value()) {
        Say(sink, "没有 current 指针,无可回滚(平铺安装请用包内安装脚本的备份)。");
        return 0;
    }
    if (!pointer->previous.has_value()) {
        Say(sink, "没有记录上次可用版本。最近一次事务的备份在: " + Utf8(paths.backups));
        return 1;
    }
    const std::string previous = *pointer->previous;
    const std::filesystem::path prev_dir = paths.versions / previous;
    std::error_code ec;
    if (!std::filesystem::is_directory(prev_dir, ec) || ec) {
        Say(sink, "上次可用版本目录不在了: " + Utf8(prev_dir));
        return 1;
    }
    // 数据兼容门禁(§七):先探针能跑,再换指针;不硬切。
    const ProbeOutcome probe = ProbeExe(prev_dir / kExeName, std::nullopt, kProbeTimeoutSecs);
    if (!probe.ok) {
        Say(sink, "上次可用版本探针不过(" + probe.detail + "),不换指针。新版继续生效。");
        return 1;
    }

    Transaction txn(paths, MakeTxnId());
    txn.data() = {
        {"schema", 1},
        {"id", txn.id()},
        {"kind", "rollback"},
        {"created_at_utc", UtcNowIso8601()},
        {"state", std::string(kTxnActivating)},
        {"from_version", pointer->current},
        {"to_version", previous},
    };
    txn.Flush();
    WriteCurrent(paths, previous, pointer->current, txn.id());
    // 原账原样改两键(python 的 raw dict 路线:未知字段一并保留)。
    auto state = ReadJsonFileTolerant(paths.state);
    if (state.has_value() && state->is_object()) {
        const std::size_t dash = previous.find('-');
        (*state)["rolled_back_at_utc"] = UtcNowIso8601();
        (*state)["version"] = dash == std::string::npos ? previous : previous.substr(0, dash);
        AtomicWriteOrThrow(paths.state, CanonicalJsonDump(*state));
    }
    nlohmann::json fields = nlohmann::json::object();
    fields["committed_at_utc"] = UtcNowIso8601();
    txn.Transition(kTxnCommitted, std::move(fields));
    Say(sink, "[rollback] current -> " + previous + "(回滚完成; " + pointer->current +
                  " 保留,随时可再切回)");
    return 0;
}

int RunGc(const EngineArgs& args, const ProgressSink& sink) {
    const std::filesystem::path root =
        std::filesystem::absolute(args.install_root).lexically_normal();
    const LayoutPaths paths = MakeLayoutPaths(root);

    InstallRootLock lock;
    const auto acquired = InstallRootLock::TryAcquire(paths, &lock);
    if (acquired.status != InstallRootLock::AcquireResult::Status::Acquired) {
        throw UpdaterFailure("取安装锁失败: " + acquired.detail);
    }
    const std::vector<std::string> removed = GcVersions(paths, sink);
    Say(sink, "清理完成: 移除 " + std::to_string(removed.size()) + " 个旧版本");
    return 0;
}

}  // namespace

int RunUpdaterEngine(const EngineArgs& args, const ProgressSink& sink, PrecheckFn precheck) {
    try {
        if (args.verb == "status") return RunStatus(args, sink);
        if (args.verb == "plan") return RunPlan(args, sink, precheck);
        if (args.verb == "update") return RunUpdate(args, sink, precheck);
        if (args.verb == "rollback") return RunRollback(args, sink);
        if (args.verb == "gc") return RunGc(args, sink);
        Say(sink, "failed: 不认的动词: " + args.verb);
        return 1;
    } catch (const NeedsReviewFailure& error) {
        Say(sink, std::string("needs-review: ") + error.what());
        return 2;
    } catch (const RolledBackFailure& error) {
        Say(sink, std::string("rolled-back: ") + error.what());
        return 3;
    } catch (const UpdaterFailure& error) {
        Say(sink, std::string("failed: ") + error.what());
        return 1;
    } catch (const std::exception& error) {
        Say(sink, std::string("failed: 引擎内部错误: ") + error.what());
        return 1;
    }
}

}  // namespace lubancode::updater
