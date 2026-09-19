// 更新助手 C++ 化·批三第①单:事务引擎主链册(tests/integration/updater)。
// 本地 --archive 造假包(手写 STORED zip——照 unpack 册的造包法,external_attr
// 全控;manifest 层现算),EXE 用
// updater_probe_stub 夹具(版本走 LUBANCODE_PROBE_STUB_VERSION 注入);真
// EXE 探针另有专测,直接用 LUBANCODE_BINARY_DIR 的主程序——ctest 三平台
// 可跑,这是 C++ 化的红利(python 时代假 exe 只能 POSIX)。矩阵:
//   - 坏包七失败路(篡改摘要/路径穿越/符号链接/清单版本不合/缺 manifest/
//     清单外夹带/磁盘预检)→ 退 1、保旧版、staging 清空、无新指针;
//   - 端到端:平铺 -> 版本化迁移(交接/备份/指针/安装账)+ 幂等重跑免下载
//     ("已在且核对通过"文案);
//   - needs-review:冲突停档(staging 留档)→ 处理后续跑同事务不重下
//     (删掉本地包源再续,重拷必炸——成功即证);
//   - 激活后失败分流:版本化回滚指回 previous 退 3;平铺恢复旧 EXE 并必摘
//     current.json 退 3;
//   - 交接被堵(备份位被占)→ needs-review 退 2,清障后续跑收尾;
//   - rollback:切 previous;无 previous 退 1;
//   - gc:留 current+previous,旧版删掉,清单外件抢救进 stranded-<名>/;
//   - 异目标在途账作废(superseded);status --json 形状;plan 预演不动安装。
#include <doctest/doctest.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include "platform/paths.hpp"
#include "platform/sha256.hpp"
#include "updater/engine.hpp"
#include "updater/layout.hpp"
#include "updater/txn.hpp"

namespace {

namespace fs = std::filesystem;
using lubancode::updater::EngineArgs;
using lubancode::updater::ProgressSink;
using lubancode::updater::RunUpdaterEngine;

#ifdef _WIN32
constexpr const char* kExeName = "lubancode.exe";
#else
constexpr const char* kExeName = "lubancode";
#endif

// ---------------------------------------------------------------------------
// 基础件
// ---------------------------------------------------------------------------

fs::path TempRoot(const char* name) {
    const fs::path dir = fs::temp_directory_path() /
                         ("lubancode_updater_engine_" + std::string(name) + "_" +
                          std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir, ec);
    return dir;
}

class EnvVarGuard {
public:
    EnvVarGuard(const char* name, std::string value) : name_(name) {
        if (const char* current = std::getenv(name)) old_ = std::string(current);
        Set(value);
    }
    ~EnvVarGuard() {
        if (old_.has_value()) {
            Set(*old_);
        } else {
            Unset();
        }
    }
    EnvVarGuard(const EnvVarGuard&) = delete;
    EnvVarGuard& operator=(const EnvVarGuard&) = delete;

private:
    void Set(const std::string& value) const {
#ifdef _WIN32
        _putenv((name_ + "=" + value).c_str());
#else
        setenv(name_.c_str(), value.c_str(), 1);
#endif
    }
    void Unset() const {
#ifdef _WIN32
        _putenv((name_ + "=").c_str());
#else
        unsetenv(name_.c_str());
#endif
    }
    std::string name_;
    std::optional<std::string> old_;
};

void WriteFile(const fs::path& path, std::string_view bytes) {
    std::error_code ec;
    fs::create_directories(path.parent_path(), ec);
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    REQUIRE(out);
    out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    REQUIRE(out.good());
}

std::string ReadFile(const fs::path& path) {
    // doctest 断言宏进不了非 void 函数(册内既有教训):失败改抛,照样炸响。
    std::ifstream in(path, std::ios::binary);
    if (!in.is_open()) throw std::runtime_error("读不了: " + lubancode::platform::PathToUtf8(path));
    std::string bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (in.bad()) throw std::runtime_error("读坏了: " + lubancode::platform::PathToUtf8(path));
    return bytes;
}

std::optional<nlohmann::json> ReadJson(const fs::path& path) {
    return lubancode::updater::ReadJsonFileTolerant(path);
}

struct Lines {
    std::vector<std::string> lines;
    ProgressSink sink() {
        return [this](std::string_view line) { lines.emplace_back(line); };
    }
    std::string joined() const {
        std::string out;
        for (const std::string& line : lines) {
            if (!out.empty()) out += "\n";
            out += line;
        }
        return out;
    }
    bool contains(std::string_view needle) const { return joined().find(needle) != std::string::npos; }
};

// ---------------------------------------------------------------------------
// 手写 STORED zip(external_attr 全控:符号链接案/穿越案;照 unpack 册抄)
// ---------------------------------------------------------------------------

void PutLe16(std::string& out, std::uint16_t v) {
    out.push_back(static_cast<char>(v & 0xff));
    out.push_back(static_cast<char>((v >> 8) & 0xff));
}
void PutLe32(std::string& out, std::uint32_t v) {
    out.push_back(static_cast<char>(v & 0xff));
    out.push_back(static_cast<char>((v >> 8) & 0xff));
    out.push_back(static_cast<char>((v >> 16) & 0xff));
    out.push_back(static_cast<char>((v >> 24) & 0xff));
}

struct ZipEntry {
    std::string name;
    std::uint32_t posix_mode = 0100644;  // external_attr >> 16
    std::string data;
};

std::uint32_t Crc32(const std::string& data) {
    // miniz 头不在本册引——引擎册自己算 CRC 只为造包,借用平台层之外的最小
    // 实现没有意义,直接查表(IEEE 802.3,与 mz_crc32 同源)。
    static std::uint32_t table[256];
    static bool built = false;
    if (!built) {
        for (std::uint32_t i = 0; i < 256; ++i) {
            std::uint32_t c = i;
            for (int k = 0; k < 8; ++k) c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
            table[i] = c;
        }
        built = true;
    }
    std::uint32_t crc = 0xFFFFFFFFu;
    for (const char byte : data) {
        crc = table[(crc ^ static_cast<unsigned char>(byte)) & 0xff] ^ (crc >> 8);
    }
    return crc ^ 0xFFFFFFFFu;
}

std::string BuildStoredZip(const std::vector<ZipEntry>& entries) {
    struct CentralRec {
        std::string name;
        std::uint32_t mode = 0;
        std::uint32_t crc = 0;
        std::uint32_t size = 0;
        std::uint32_t local_ofs = 0;
    };
    std::string out;
    std::vector<CentralRec> cdir;
    for (const ZipEntry& e : entries) {
        CentralRec rec;
        rec.name = e.name;
        rec.mode = e.posix_mode;
        rec.size = static_cast<std::uint32_t>(e.data.size());
        rec.crc = Crc32(e.data);
        rec.local_ofs = static_cast<std::uint32_t>(out.size());
        cdir.push_back(rec);

        PutLe32(out, 0x04034b50);  // 本地头签名
        PutLe16(out, 20);          // 需要的版本
        PutLe16(out, 0);           // 通用位标志
        PutLe16(out, 0);           // STORED
        PutLe16(out, 0);           // 时间
        PutLe16(out, 0);           // 日期
        PutLe32(out, rec.crc);
        PutLe32(out, rec.size);
        PutLe32(out, rec.size);
        PutLe16(out, static_cast<std::uint16_t>(e.name.size()));
        PutLe16(out, 0);  // 本地扩展域长度
        out += e.name;
        out += e.data;
    }
    const std::uint32_t cdir_ofs = static_cast<std::uint32_t>(out.size());
    std::string cdir_buf;
    for (const CentralRec& rec : cdir) {
        PutLe32(cdir_buf, 0x02014b50);           // 中央头签名
        PutLe16(cdir_buf, (3u << 8) | 20u);      // made by:unix/2.0
        PutLe16(cdir_buf, 20);                   // 需要的版本
        PutLe16(cdir_buf, 0);                    // 通用位标志
        PutLe16(cdir_buf, 0);                    // STORED
        PutLe16(cdir_buf, 0);                    // 时间
        PutLe16(cdir_buf, 0);                    // 日期
        PutLe32(cdir_buf, rec.crc);
        PutLe32(cdir_buf, rec.size);
        PutLe32(cdir_buf, rec.size);
        PutLe16(cdir_buf, static_cast<std::uint16_t>(rec.name.size()));
        PutLe16(cdir_buf, 0);  // 中央扩展域长度
        PutLe16(cdir_buf, 0);  // 注记长度
        PutLe16(cdir_buf, 0);  // 起始盘号
        PutLe16(cdir_buf, 0);  // 内部属性
        PutLe32(cdir_buf, rec.mode << 16);  // 外部属性 = POSIX mode
        PutLe32(cdir_buf, rec.local_ofs);
        cdir_buf += rec.name;
    }
    out += cdir_buf;
    PutLe32(out, 0x06054b50);  // EOCD 签名
    PutLe16(out, 0);
    PutLe16(out, 0);
    PutLe16(out, static_cast<std::uint16_t>(entries.size()));
    PutLe16(out, static_cast<std::uint16_t>(entries.size()));
    PutLe32(out, static_cast<std::uint32_t>(cdir_buf.size()));
    PutLe32(out, cdir_ofs);
    PutLe16(out, 0);
    return out;
}

// ---------------------------------------------------------------------------
// 官方包现场造
// ---------------------------------------------------------------------------

const std::string& StubExeBytes() {
    static const std::string bytes = [] {
        std::error_code ec;
        const fs::path stub = fs::path(LUBANCODE_UPDATER_PROBE_STUB_EXE);
        if (!fs::is_regular_file(stub, ec) || ec) {
            throw std::runtime_error("updater_probe_stub 夹具不在: " +
                                     lubancode::platform::PathToUtf8(stub));
        }
        std::ifstream in(stub, std::ios::binary);
        if (!in.is_open()) {
            throw std::runtime_error("updater_probe_stub 打不开: " +
                                     lubancode::platform::PathToUtf8(stub));
        }
        return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    }();
    return bytes;
}

struct PackageSpec {
    std::string version;
    std::vector<std::pair<std::string, std::string>> extra_files;  // 清单内额外文件
    bool with_manifest = true;
    std::optional<std::string> manifest_version;  // 缺省同 version(清单不合案用)
    std::vector<ZipEntry> extra_entries;          // 清单外夹带/坏成员(穿越/链接)
};

struct Package {
    std::string bytes;
    std::string digest_hex;
    std::string dirname;
};

Package BuildPackage(const PackageSpec& spec) {
    // 清单收的文件:EXE(stub 夹具)+ libexec/rg + 额外件。
    std::vector<std::pair<std::string, std::string>> listed;
    listed.emplace_back(kExeName, StubExeBytes());
    listed.emplace_back("libexec/rg", "#!fake ripgrep\n");
    for (const auto& [path, data] : spec.extra_files) {
        listed.emplace_back(path, data);
    }
    std::sort(listed.begin(), listed.end(),
              [](const auto& a, const auto& b) { return a.first < b.first; });

    nlohmann::json files = nlohmann::json::array();
    for (const auto& [path, data] : listed) {
        files.push_back(nlohmann::json{
            {"path", path},
            {"size", data.size()},
            {"sha256", lubancode::platform::Sha256Hex(data)},
        });
    }
    nlohmann::json manifest = nlohmann::json::object();
    manifest["schema"] = 1;
    manifest["name"] = "lubancode";
    manifest["version"] = spec.manifest_version.value_or(spec.version);
    manifest["platform"] = "test-x64";
    manifest["channel"] = "stable";
    manifest["algo"] = "sha256";
    manifest["file_count"] = listed.size();
    manifest["files"] = std::move(files);
    const std::string manifest_bytes = lubancode::updater::CanonicalJsonDump(manifest);

    std::vector<ZipEntry> entries;
    if (spec.with_manifest) {
        entries.push_back(ZipEntry{"manifest.json", 0100644, manifest_bytes});
    }
    for (const auto& [path, data] : listed) {
        const std::uint32_t mode = path == kExeName || path == "libexec/rg" ? 0100755u : 0100644u;
        entries.push_back(ZipEntry{path, mode, data});
    }
    for (const ZipEntry& extra : spec.extra_entries) {
        entries.push_back(extra);
    }

    Package pkg;
    pkg.bytes = BuildStoredZip(entries);
    pkg.digest_hex = lubancode::platform::Sha256Hex(pkg.bytes);
    pkg.dirname = spec.version + "-" + pkg.digest_hex.substr(0, 8);
    return pkg;
}

EngineArgs UpdateArgs(const fs::path& root, const std::optional<fs::path>& archive,
                      const std::string& version, const std::string& digest_hex,
                      std::optional<std::uint64_t> asset_size = std::nullopt) {
    EngineArgs args;
    args.verb = "update";
    args.install_root = root;
    args.version = version;
    args.exe_version = version;
    args.digest = "sha256:" + digest_hex;
    args.asset_size = std::move(asset_size);
    args.from_archive = archive;
    args.channel = "stable";
    return args;
}

// ---------------------------------------------------------------------------
// 安装现场造与对账件
// ---------------------------------------------------------------------------

// 造一枚合法版本目录(EXE+rg+manifest 现算),返回目录名。
std::string MakeVersionDir(const fs::path& root, const std::string& version) {
    const std::string digest = lubancode::platform::Sha256Hex("seed-" + version);
    const std::string dirname = version + "-" + digest.substr(0, 8);
    const fs::path dir = root / "versions" / dirname;
    const std::string exe_bytes = StubExeBytes();
    const std::string rg_bytes = "#!fake ripgrep\n";
    WriteFile(dir / kExeName, exe_bytes);
    WriteFile(dir / "libexec" / "rg", rg_bytes);
#ifndef _WIN32
    std::error_code ec;
    fs::permissions(dir / kExeName, fs::perms::owner_exec | fs::perms::group_exec | fs::perms::others_exec,
                    fs::perm_options::add, ec);
#endif
    nlohmann::json manifest = nlohmann::json::object();
    manifest["schema"] = 1;
    manifest["name"] = "lubancode";
    manifest["version"] = version;
    manifest["platform"] = "test-x64";
    manifest["channel"] = "stable";
    manifest["algo"] = "sha256";
    manifest["file_count"] = 2;
    manifest["files"] = nlohmann::json::array({
        nlohmann::json{{"path", kExeName},
                       {"size", exe_bytes.size()},
                       {"sha256", lubancode::platform::Sha256Hex(exe_bytes)}},
        nlohmann::json{{"path", "libexec/rg"},
                       {"size", rg_bytes.size()},
                       {"sha256", lubancode::platform::Sha256Hex(rg_bytes)}},
    });
    WriteFile(dir / "manifest.json", lubancode::updater::CanonicalJsonDump(manifest));
    return dirname;
}

// 造版本化安装场:一个版本目录 + 指针 + 最小安装账。
std::string MakeVersionedInstall(const fs::path& root, const std::string& version,
                                 std::optional<std::string> previous = std::nullopt) {
    const std::string dirname = MakeVersionDir(root, version);
    lubancode::updater::WriteCurrent(lubancode::updater::MakeLayoutPaths(root), dirname, previous,
                                     "test-txn");
    nlohmann::json state = nlohmann::json::object();
    state["schema"] = 2;
    state["version"] = version;
    state["channel"] = "stable";
    WriteFile(root / "install-state.json", lubancode::updater::CanonicalJsonDump(state));
    return dirname;
}

std::vector<fs::path> LedgerFiles(const fs::path& root) {
    std::vector<fs::path> files;
    std::error_code ec;
    for (fs::directory_iterator it(root / "updates", ec), end; !ec && it != end; it.increment(ec)) {
        if (it->is_regular_file() && it->path().extension() == ".json") {
            files.push_back(it->path());
        }
    }
    std::sort(files.begin(), files.end());
    return files;
}

std::optional<std::string> LedgerField(const fs::path& ledger, const char* key) {
    const auto json = ReadJson(ledger);
    if (!json.has_value() || !json->is_object() || !json->contains(key)) return std::nullopt;
    if (!(*json)[key].is_string()) return std::nullopt;
    return (*json)[key].get<std::string>();
}

std::optional<std::string> CurrentPointerOf(const fs::path& root) {
    const auto pointer = lubancode::updater::ReadCurrent(lubancode::updater::MakeLayoutPaths(root));
    if (!pointer.has_value()) return std::nullopt;
    return pointer->current;
}

// staging/<txn> 一概不在(清场或从未建)。
bool StagingEmpty(const fs::path& root) {
    std::error_code ec;
    if (!fs::is_directory(root / "staging", ec) || ec) return true;
    for (fs::directory_iterator it(root / "staging", ec), end; !ec && it != end; it.increment(ec)) {
        (void)it;
        return false;
    }
    return true;
}

}  // namespace

// ---------------------------------------------------------------------------
// 坏包七失败路
// ---------------------------------------------------------------------------

TEST_CASE("engine.update:坏包七路——退 1、保旧版、staging 清空、无新指针") {
    const std::string wrong_digest(64, 'a');

    SUBCASE("篡改摘要——本地包摘要不符") {
        const fs::path root = TempRoot("bad-digest");
        const std::string old_dir = MakeVersionedInstall(root, "1.0.0");
        EnvVarGuard stub_version("LUBANCODE_PROBE_STUB_VERSION", "2.0.0");
        const Package pkg = BuildPackage({.version = "2.0.0"});
        const fs::path archive = root.parent_path() / (root.filename().string() + "-pkg.zip");
        WriteFile(archive, pkg.bytes);
        Lines out;
        CHECK(RunUpdaterEngine(UpdateArgs(root, archive, "2.0.0", wrong_digest), out.sink(),
                               nullptr) == 1);
        CHECK(out.contains("本地包摘要不符"));
        CHECK(CurrentPointerOf(root) == std::optional<std::string>(old_dir));  // 保旧版
        CHECK(StagingEmpty(root));
        const auto ledgers = LedgerFiles(root);
        REQUIRE(ledgers.size() == 1);
        CHECK(LedgerField(ledgers[0], "state") == std::optional<std::string>("failed"));
    }

    SUBCASE("路径穿越成员——解包整包拒") {
        const fs::path root = TempRoot("bad-traversal");
        EnvVarGuard stub_version("LUBANCODE_PROBE_STUB_VERSION", "2.0.0");
        PackageSpec spec;
        spec.version = "2.0.0";
        spec.extra_entries.push_back(ZipEntry{"../evil.txt", 0100644, "escape"});
        const Package pkg = BuildPackage(spec);
        const fs::path archive = root.parent_path() / (root.filename().string() + "-pkg.zip");
        WriteFile(archive, pkg.bytes);
        Lines out;
        CHECK(RunUpdaterEngine(UpdateArgs(root, archive, "2.0.0", pkg.digest_hex), out.sink(),
                               nullptr) == 1);
        CHECK(out.contains("failed: "));
        CHECK_FALSE(fs::exists(root / "current.json"));
        CHECK(StagingEmpty(root));
        REQUIRE(LedgerFiles(root).size() == 1);
        CHECK(LedgerField(LedgerFiles(root)[0], "state") == std::optional<std::string>("failed"));
    }

    SUBCASE("符号链接成员——解包整包拒") {
        const fs::path root = TempRoot("bad-symlink");
        EnvVarGuard stub_version("LUBANCODE_PROBE_STUB_VERSION", "2.0.0");
        PackageSpec spec;
        spec.version = "2.0.0";
        spec.extra_entries.push_back(ZipEntry{"evil-link", 0120777, "target"});
        const Package pkg = BuildPackage(spec);
        const fs::path archive = root.parent_path() / (root.filename().string() + "-pkg.zip");
        WriteFile(archive, pkg.bytes);
        Lines out;
        CHECK(RunUpdaterEngine(UpdateArgs(root, archive, "2.0.0", pkg.digest_hex), out.sink(),
                               nullptr) == 1);
        CHECK_FALSE(fs::exists(root / "current.json"));
        CHECK(StagingEmpty(root));
        CHECK(LedgerField(LedgerFiles(root)[0], "state") == std::optional<std::string>("failed"));
    }

    SUBCASE("清单版本不合——验包拒") {
        const fs::path root = TempRoot("bad-manifest");
        EnvVarGuard stub_version("LUBANCODE_PROBE_STUB_VERSION", "2.0.0");
        PackageSpec spec;
        spec.version = "2.0.0";
        spec.manifest_version = std::string("9.9.9");
        const Package pkg = BuildPackage(spec);
        const fs::path archive = root.parent_path() / (root.filename().string() + "-pkg.zip");
        WriteFile(archive, pkg.bytes);
        Lines out;
        CHECK(RunUpdaterEngine(UpdateArgs(root, archive, "2.0.0", pkg.digest_hex), out.sink(),
                               nullptr) == 1);
        CHECK(out.contains("清单版本"));
        CHECK_FALSE(fs::exists(root / "current.json"));
        CHECK(StagingEmpty(root));
        CHECK(LedgerField(LedgerFiles(root)[0], "state") == std::optional<std::string>("failed"));
    }

    SUBCASE("缺 manifest——验包拒") {
        const fs::path root = TempRoot("bad-nomanifest");
        EnvVarGuard stub_version("LUBANCODE_PROBE_STUB_VERSION", "2.0.0");
        PackageSpec spec;
        spec.version = "2.0.0";
        spec.with_manifest = false;
        const Package pkg = BuildPackage(spec);
        const fs::path archive = root.parent_path() / (root.filename().string() + "-pkg.zip");
        WriteFile(archive, pkg.bytes);
        Lines out;
        CHECK(RunUpdaterEngine(UpdateArgs(root, archive, "2.0.0", pkg.digest_hex), out.sink(),
                               nullptr) == 1);
        CHECK(out.contains("包里缺 manifest.json"));
        CHECK_FALSE(fs::exists(root / "current.json"));
        CHECK(StagingEmpty(root));
        CHECK(LedgerField(LedgerFiles(root)[0], "state") == std::optional<std::string>("failed"));
    }

    SUBCASE("清单外夹带——验包拒") {
        const fs::path root = TempRoot("bad-smuggle");
        EnvVarGuard stub_version("LUBANCODE_PROBE_STUB_VERSION", "2.0.0");
        PackageSpec spec;
        spec.version = "2.0.0";
        spec.extra_entries.push_back(ZipEntry{"extra.txt", 0100644, "smuggled"});
        const Package pkg = BuildPackage(spec);
        const fs::path archive = root.parent_path() / (root.filename().string() + "-pkg.zip");
        WriteFile(archive, pkg.bytes);
        Lines out;
        CHECK(RunUpdaterEngine(UpdateArgs(root, archive, "2.0.0", pkg.digest_hex), out.sink(),
                               nullptr) == 1);
        CHECK(out.contains("包内有清单外文件"));
        CHECK_FALSE(fs::exists(root / "current.json"));
        CHECK(StagingEmpty(root));
        CHECK(LedgerField(LedgerFiles(root)[0], "state") == std::optional<std::string>("failed"));
    }

    SUBCASE("磁盘预检不够——checking 阶段拒") {
        const fs::path root = TempRoot("bad-disk");
        EnvVarGuard stub_version("LUBANCODE_PROBE_STUB_VERSION", "2.0.0");
        const Package pkg = BuildPackage({.version = "2.0.0"});
        const fs::path archive = root.parent_path() / (root.filename().string() + "-pkg.zip");
        WriteFile(archive, pkg.bytes);
        Lines out;
        // 600 GiB 声明 -> need 1800 GiB,任何 CI 盘都剩不下。
        CHECK(RunUpdaterEngine(UpdateArgs(root, archive, "2.0.0", pkg.digest_hex, 600ull << 30),
                               out.sink(), nullptr) == 1);
        CHECK(out.contains("磁盘空间不够"));
        CHECK_FALSE(fs::exists(root / "current.json"));
        CHECK(StagingEmpty(root));
        CHECK(LedgerField(LedgerFiles(root)[0], "state") == std::optional<std::string>("failed"));
    }
}

// ---------------------------------------------------------------------------
// 端到端
// ---------------------------------------------------------------------------

TEST_CASE("engine.update:平铺 -> 版本化端到端 + 幂等重跑免下载") {
    const fs::path root = TempRoot("e2e-flat");
    EnvVarGuard stub_version("LUBANCODE_PROBE_STUB_VERSION", "2.0.0");
    // 平铺现场:根 EXE(旧字节)+ skills 树里一件用户文件 + 顶层用户件。
    const std::string old_exe_bytes = "old flat exe bytes";
    WriteFile(root / kExeName, old_exe_bytes);
    WriteFile(root / "skills" / "user-note.md", "用户改动");
    WriteFile(root / "config.toml", "user config");

    const Package pkg = BuildPackage({.version = "2.0.0", .extra_files = {{"README.md", "hello"}}});
    const fs::path archive = root.parent_path() / (root.filename().string() + "-pkg.zip");
    WriteFile(archive, pkg.bytes);

    Lines out;
    CHECK(RunUpdaterEngine(UpdateArgs(root, archive, "2.0.0", pkg.digest_hex), out.sink(),
                           nullptr) == 0);
    CHECK(out.contains("[activate] current -> " + pkg.dirname));
    CHECK(out.contains("[backup] 平铺安装完整备份: "));
    CHECK(out.contains("[commit] 2.0.0 已上线"));

    // 指针与版本目录。
    const auto pointer = ReadJson(root / "current.json");
    REQUIRE(pointer.has_value());
    CHECK((*pointer)["current"] == pkg.dirname);
    CHECK((*pointer)["previous"] == nullptr);
    const fs::path version_dir = root / "versions" / pkg.dirname;
    CHECK(ReadFile(version_dir / kExeName) == StubExeBytes());
    CHECK(fs::is_regular_file(version_dir / "libexec" / "rg"));
    CHECK(fs::is_regular_file(version_dir / "manifest.json"));
    CHECK(fs::is_regular_file(version_dir / "README.md"));

    // 交接:根 EXE 换新,旧 EXE 进 backups/<txn>/legacy。
    CHECK(ReadFile(root / kExeName) == StubExeBytes());
    const auto ledgers1 = LedgerFiles(root);
    REQUIRE(ledgers1.size() == 1);
    const std::string txn_id = ledgers1[0].stem().string();
    CHECK(ReadFile(root / "backups" / txn_id / "legacy" / kExeName) == old_exe_bytes);
    CHECK(ReadFile(root / "backups" / txn_id / "flat" / "skills" / "user-note.md") == "用户改动");
    CHECK(ReadFile(root / "backups" / txn_id / "flat" / "config.toml") == "user config");
    CHECK(LedgerField(ledgers1[0], "state") == std::optional<std::string>("committed"));

    // 安装账:schema 2、来源摘要、事务号、清单原样。
    const auto state = ReadJson(root / "install-state.json");
    REQUIRE(state.has_value());
    CHECK((*state)["schema"] == 2);
    CHECK((*state)["version"] == "2.0.0");
    CHECK((*state)["channel"] == "stable");
    CHECK((*state)["transaction"] == txn_id);
    CHECK((*state)["source"]["asset_digest"] == "sha256:" + pkg.digest_hex);
    CHECK((*state)["manifest"]["file_count"] == 3);  // EXE + libexec/rg + README.md

    // 幂等重跑:同目标再跑一遍,版本目录已在且核对通过,免下载。
    Lines out2;
    CHECK(RunUpdaterEngine(UpdateArgs(root, archive, "2.0.0", pkg.digest_hex), out2.sink(),
                           nullptr) == 0);
    CHECK(out2.contains("[stage] 版本目录 " + pkg.dirname + " 已在且核对通过,跳过下载"));
    const auto pointer2 = ReadJson(root / "current.json");
    REQUIRE(pointer2.has_value());
    CHECK((*pointer2)["current"] == pkg.dirname);
    CHECK((*pointer2)["previous"] == pkg.dirname);  // python 同款:previous = 旧 current
    const auto ledgers2 = LedgerFiles(root);
    CHECK(ledgers2.size() == 2);  // 各开新账,双双 committed
    for (const fs::path& ledger : ledgers2) {
        CHECK(LedgerField(ledger, "state") == std::optional<std::string>("committed"));
    }
}

TEST_CASE("engine.update:needs-review 停档续跑——同事务、不重下") {
    const fs::path root = TempRoot("needs-review");
    EnvVarGuard stub_version("LUBANCODE_PROBE_STUB_VERSION", "3.0.0");
    const Package pkg = BuildPackage({.version = "3.0.0"});
    const fs::path archive = root.parent_path() / (root.filename().string() + "-pkg.zip");
    WriteFile(archive, pkg.bytes);

    int calls = 0;
    const std::string conflict = "skills/foo.md 被用户改过(conflict-modified)";
    lubancode::updater::PrecheckFn precheck = [&](const fs::path&, const fs::path&) {
        ++calls;
        if (calls == 1) return std::expected<void, std::string>(std::unexpected(conflict));
        return std::expected<void, std::string>();
    };

    Lines out;
    CHECK(RunUpdaterEngine(UpdateArgs(root, archive, "3.0.0", pkg.digest_hex), out.sink(),
                           precheck) == 2);
    CHECK(out.contains("needs-review: "));
    CHECK(out.contains("冲突详情: " + conflict));
    auto ledgers = LedgerFiles(root);
    REQUIRE(ledgers.size() == 1);
    const std::string txn_id = ledgers[0].stem().string();
    CHECK(LedgerField(ledgers[0], "state") == std::optional<std::string>("needs-review"));
    const auto ledger = ReadJson(ledgers[0]);
    REQUIRE(ledger.has_value());
    CHECK((*ledger)["blocking"].is_array() && (*ledger)["blocking"].size() == 1);
    CHECK_FALSE(fs::exists(root / "current.json"));  // 安装未变
    CHECK(fs::is_regular_file(root / "staging" / txn_id / "archive.bin"));  // 留档续跑

    // 删掉本地包源:续跑若重下/重拷必炸("本地包不存在"),成功即证不重下。
    std::error_code ec;
    fs::remove(archive, ec);
    REQUIRE_FALSE(fs::exists(archive, ec));

    Lines out2;
    CHECK(RunUpdaterEngine(UpdateArgs(root, archive, "3.0.0", pkg.digest_hex), out2.sink(),
                           precheck) == 0);
    CHECK(out2.contains("[resume] 续上事务 " + txn_id));
    CHECK(out2.contains("[commit] 3.0.0 已上线"));
    ledgers = LedgerFiles(root);
    REQUIRE(ledgers.size() == 1);  // 还是同一笔账
    CHECK(ledgers[0].stem().string() == txn_id);
    CHECK(LedgerField(ledgers[0], "state") == std::optional<std::string>("committed"));
    CHECK(calls == 2);
}

TEST_CASE("engine.update:激活后失败——版本化回滚指回 previous 退 3") {
    const fs::path root = TempRoot("rollback-versioned");
    const std::string old_dir = MakeVersionedInstall(root, "1.0.0");
    EnvVarGuard stub_version("LUBANCODE_PROBE_STUB_VERSION", "2.0.0");
    const Package pkg = BuildPackage({.version = "2.0.0"});
    const fs::path archive = root.parent_path() / (root.filename().string() + "-pkg.zip");
    WriteFile(archive, pkg.bytes);
    // install-state.json 占成目录:健康检查过了,写安装账必炸 -> 走回滚路。
    // (MakeVersionedInstall 先落了同名文件,得挪掉再占。)
    std::error_code ec;
    fs::remove(root / "install-state.json", ec);
    fs::create_directories(root / "install-state.json", ec);

    Lines out;
    CHECK(RunUpdaterEngine(UpdateArgs(root, archive, "2.0.0", pkg.digest_hex), out.sink(),
                           nullptr) == 3);
    CHECK(out.contains("rolled-back: "));
    const auto pointer = ReadJson(root / "current.json");
    REQUIRE(pointer.has_value());
    CHECK((*pointer)["current"] == old_dir);
    CHECK((*pointer)["previous"] == pkg.dirname);
    const auto ledgers = LedgerFiles(root);
    REQUIRE(ledgers.size() == 1);
    const auto ledger = ReadJson(ledgers[0]);
    REQUIRE(ledger.has_value());
    CHECK((*ledger)["state"] == "rolled-back");
    CHECK((*ledger)["restored_to"] == old_dir);
    CHECK(StagingEmpty(root));
    CHECK(fs::is_directory(root / "versions" / old_dir));  // 旧版原样
}

TEST_CASE("engine.update:激活后失败——平铺恢复旧 EXE 并必摘 current.json 退 3") {
    const fs::path root = TempRoot("rollback-flat");
    EnvVarGuard stub_version("LUBANCODE_PROBE_STUB_VERSION", "2.0.0");
    const std::string old_exe_bytes = "old flat exe bytes";
    WriteFile(root / kExeName, old_exe_bytes);
    WriteFile(root / "skills" / "keep.md", "用户文件");
    const Package pkg = BuildPackage({.version = "2.0.0"});
    const fs::path archive = root.parent_path() / (root.filename().string() + "-pkg.zip");
    WriteFile(archive, pkg.bytes);
    std::error_code ec;
    fs::create_directories(root / "install-state.json", ec);  // 写账必炸的堵点

    Lines out;
    CHECK(RunUpdaterEngine(UpdateArgs(root, archive, "2.0.0", pkg.digest_hex), out.sink(),
                           nullptr) == 3);
    CHECK_FALSE(fs::exists(root / "current.json"));  // 必摘指针
    CHECK(ReadFile(root / kExeName) == old_exe_bytes);  // 旧根 EXE 恢复
    const auto ledgers = LedgerFiles(root);
    REQUIRE(ledgers.size() == 1);
    const std::string txn_id = ledgers[0].stem().string();
    const auto ledger = ReadJson(ledgers[0]);
    REQUIRE(ledger.has_value());
    CHECK((*ledger)["state"] == "rolled-back");
    CHECK((*ledger)["restored_flat"] == true);
    // 交接顶上去的新 EXE 停进 launcher-parked,旧 EXE 的原件在 legacy。
    CHECK(fs::is_regular_file(root / "backups" / txn_id / "launcher-parked-" + kExeName));
    CHECK(ReadFile(root / "backups" / txn_id / "legacy" / kExeName) == old_exe_bytes);
}

TEST_CASE("engine.update:交接被堵——needs-review 退 2,清障续跑收尾") {
    const fs::path root = TempRoot("handover-blocked");
    EnvVarGuard stub_version("LUBANCODE_PROBE_STUB_VERSION", "2.0.0");
    WriteFile(root / kExeName, "old flat exe bytes");
    WriteFile(root / "skills" / "keep.md", "用户文件");
    const Package pkg = BuildPackage({.version = "2.0.0"});
    const fs::path archive = root.parent_path() / (root.filename().string() + "-pkg.zip");
    WriteFile(archive, pkg.bytes);

    // 预置一笔已知 id 的在途账,堵住 backups/<id>(占成文件):平铺备份挪不
    // 进事务名下(留原地),交接建不成 legacy 目录 -> needs-review。
    const std::string txn_id = "20260920T000000Z-cafe0001";
    {
        lubancode::updater::TxnTarget target;
        target.version = "2.0.0";
        target.tag = "v2.0.0";
        target.exe_version = "2.0.0";
        target.platform = "test-x64";
        target.dirname = pkg.dirname;
        target.digest_hex = pkg.digest_hex;
        lubancode::updater::Transaction txn(lubancode::updater::MakeLayoutPaths(root), txn_id);
        txn.Create(target);
    }
    WriteFile(root / "backups" / txn_id, "占位文件,堵住目录名");

    Lines out;
    CHECK(RunUpdaterEngine(UpdateArgs(root, archive, "2.0.0", pkg.digest_hex), out.sink(),
                           nullptr) == 2);
    CHECK(out.contains("needs-review: "));
    const auto pointer = ReadJson(root / "current.json");
    REQUIRE(pointer.has_value());  // 指针已换(python 同款:needs-review 不回滚)
    CHECK((*pointer)["current"] == pkg.dirname);
    // 账停在 activating(python 同款:交接的 needs-review 是退出码与报错,
    // 账面状态留在 activating,find_resumable 照样可续)。
    CHECK(LedgerField(root / "updates" / (txn_id + ".json"), "state") ==
          std::optional<std::string>("activating"));

    // 清障:挪走占位文件,续跑收尾(版本目录已在,versioned 路径直进提交)。
    std::error_code ec;
    fs::remove(root / "backups" / txn_id, ec);
    Lines out2;
    CHECK(RunUpdaterEngine(UpdateArgs(root, archive, "2.0.0", pkg.digest_hex), out2.sink(),
                           nullptr) == 0);
    CHECK(out2.contains("[resume] 续上事务 " + txn_id));
    CHECK(out2.contains("[stage] 版本目录 " + pkg.dirname + " 已在且核对通过,跳过下载"));
    CHECK(LedgerField(root / "updates" / (txn_id + ".json"), "state") ==
          std::optional<std::string>("committed"));
}

TEST_CASE("engine.rollback:切回 previous;无 previous 退 1") {
    const fs::path root = TempRoot("rollback-cmd");
    const std::string previous_dir = MakeVersionDir(root, "2.0.0");
    const std::string current_dir = MakeVersionDir(root, "1.0.0");
    lubancode::updater::WriteCurrent(lubancode::updater::MakeLayoutPaths(root), current_dir,
                                     previous_dir, "test-txn");
    nlohmann::json state = nlohmann::json::object();
    state["schema"] = 2;
    state["version"] = "1.0.0";
    state["channel"] = "stable";
    state["custom_field"] = "未知字段一并保留";
    WriteFile(root / "install-state.json", lubancode::updater::CanonicalJsonDump(state));

    EngineArgs args;
    args.verb = "rollback";
    args.install_root = root;
    Lines out;
    CHECK(RunUpdaterEngine(args, out.sink(), nullptr) == 0);
    CHECK(out.contains("[rollback] current -> " + previous_dir));
    const auto pointer = ReadJson(root / "current.json");
    REQUIRE(pointer.has_value());
    CHECK((*pointer)["current"] == previous_dir);
    CHECK((*pointer)["previous"] == current_dir);
    const auto state_after = ReadJson(root / "install-state.json");
    REQUIRE(state_after.has_value());
    CHECK((*state_after)["version"] == "2.0.0");
    CHECK((*state_after)["rolled_back_at_utc"].is_string());
    CHECK((*state_after)["custom_field"] == "未知字段一并保留");
    const auto ledgers = LedgerFiles(root);
    REQUIRE(ledgers.size() == 1);
    CHECK(LedgerField(ledgers[0], "kind") == std::optional<std::string>("rollback"));
    CHECK(LedgerField(ledgers[0], "state") == std::optional<std::string>("committed"));

    // 无 previous:退 1,不动指针。
    const fs::path root2 = TempRoot("rollback-noprevious");
    MakeVersionedInstall(root2, "1.0.0");
    EngineArgs args2;
    args2.verb = "rollback";
    args2.install_root = root2;
    Lines out2;
    CHECK(RunUpdaterEngine(args2, out2.sink(), nullptr) == 1);
    CHECK(out2.contains("没有记录上次可用版本"));
}

TEST_CASE("engine.gc:留 current+previous,旧版删掉,清单外件抢救") {
    const fs::path root = TempRoot("gc");
    const std::string current_dir = MakeVersionDir(root, "1.0.0");
    const std::string previous_dir = MakeVersionDir(root, "0.9.0");
    const std::string stale_dir = MakeVersionDir(root, "0.8.0");
    lubancode::updater::WriteCurrent(lubancode::updater::MakeLayoutPaths(root), current_dir,
                                     previous_dir, "test-txn");
    // 旧版本里塞一件清单外文件(用户改动),删前须抢救。
    WriteFile(root / "versions" / stale_dir / "notes" / "user.txt", "用户笔记");
    WriteFile(root / "versions" / current_dir / "keep.txt", "当前版本的清单外件不动");

    EngineArgs args;
    args.verb = "gc";
    args.install_root = root;
    Lines out;
    CHECK(RunUpdaterEngine(args, out.sink(), nullptr) == 0);
    CHECK(out.contains("[gc] 已移除旧版本 " + stale_dir +
                       ";清单外文件 1 个保存在 backups/stranded-" + stale_dir + "/"));
    CHECK(fs::is_directory(root / "versions" / current_dir));
    CHECK(fs::is_directory(root / "versions" / previous_dir));
    CHECK_FALSE(fs::exists(root / "versions" / stale_dir));
    CHECK(ReadFile(root / "backups" / ("stranded-" + stale_dir) / "notes" / "user.txt") == "用户笔记");
    CHECK(ReadFile(root / "versions" / current_dir / "keep.txt") == "当前版本的清单外件不动");
}

TEST_CASE("engine.update:异目标在途账作废(superseded)") {
    const fs::path root = TempRoot("superseded");
    EnvVarGuard stub_version("LUBANCODE_PROBE_STUB_VERSION", "2.0.0");
    // 预置一笔异 digest 在途账 + staging 残档。
    const std::string old_txn_id = "20260920T000000Z-deadbeef";
    const std::string old_digest = lubancode::platform::Sha256Hex("an-old-target");
    {
        lubancode::updater::TxnTarget target;
        target.version = "1.5.0";
        target.tag = "v1.5.0";
        target.exe_version = "1.5.0";
        target.platform = "test-x64";
        target.dirname = "1.5.0-" + old_digest.substr(0, 8);
        target.digest_hex = old_digest;
        lubancode::updater::Transaction txn(lubancode::updater::MakeLayoutPaths(root), old_txn_id);
        txn.Create(target);
        txn.Transition(lubancode::updater::kTxnDownloading);
    }
    WriteFile(root / "staging" / old_txn_id / "junk.txt", "残档");

    const Package pkg = BuildPackage({.version = "2.0.0"});
    const fs::path archive = root.parent_path() / (root.filename().string() + "-pkg.zip");
    WriteFile(archive, pkg.bytes);
    Lines out;
    CHECK(RunUpdaterEngine(UpdateArgs(root, archive, "2.0.0", pkg.digest_hex), out.sink(),
                           nullptr) == 0);
    const auto old_ledger = ReadJson(root / "updates" / (old_txn_id + ".json"));
    REQUIRE(old_ledger.has_value());
    CHECK((*old_ledger)["state"] == "failed");
    CHECK((*old_ledger)["reason"] == "superseded");
    CHECK((*old_ledger)["detail"] == "目标版本已换,旧事务作废");
    CHECK_FALSE(fs::exists(root / "staging" / old_txn_id));  // 清场
    const auto ledgers = LedgerFiles(root);
    REQUIRE(ledgers.size() == 2);
    for (const fs::path& ledger : ledgers) {  // 新账 committed,旧账 failed/superseded
        if (ledger.stem().string() == old_txn_id) continue;
        CHECK(LedgerField(ledger, "state") == std::optional<std::string>("committed"));
    }
}

TEST_CASE("engine.status:--json 形状(空场与版本化+在途)") {
    // 空场。
    const fs::path empty_root = TempRoot("status-empty");
    EngineArgs empty_args;
    empty_args.verb = "status";
    empty_args.install_root = empty_root;
    empty_args.json = true;
    Lines empty_out;
    CHECK(RunUpdaterEngine(empty_args, empty_out.sink(), nullptr) == 0);
    const auto empty_json = nlohmann::json::parse(empty_out.joined(), nullptr, false);
    REQUIRE_FALSE(empty_json.is_discarded());
    CHECK(empty_json["layout"] == "empty");
    CHECK(empty_json["current"] == nullptr);
    CHECK(empty_json["previous"] == nullptr);
    CHECK(empty_json["installed"] == nullptr);
    CHECK(empty_json["pending_transactions"] == nlohmann::json::array());

    // 版本化 + 在途账。
    const fs::path root = TempRoot("status-versioned");
    const std::string current_dir = MakeVersionedInstall(root, "1.0.0");
    {
        lubancode::updater::TxnTarget target;
        target.version = "2.0.0";
        target.tag = "v2.0.0";
        target.exe_version = "2.0.0";
        target.platform = "test-x64";
        target.dirname = "2.0.0-abcd1234";
        target.digest_hex = lubancode::platform::Sha256Hex("pending-target");
        lubancode::updater::Transaction txn(lubancode::updater::MakeLayoutPaths(root),
                                            "20260920T000000Z-beef0002");
        txn.Create(target);
    }
    EngineArgs args;
    args.verb = "status";
    args.install_root = root;
    args.json = true;
    Lines out;
    CHECK(RunUpdaterEngine(args, out.sink(), nullptr) == 0);
    const auto json = nlohmann::json::parse(out.joined(), nullptr, false);
    REQUIRE_FALSE(json.is_discarded());
    CHECK(json["layout"] == "versioned");
    CHECK(json["current"] == current_dir);
    CHECK(json["previous"] == nullptr);
    CHECK(json["installed"]["version"] == "1.0.0");
    CHECK(json["installed"]["channel"] == "stable");
    REQUIRE(json["pending_transactions"].size() == 1);
    CHECK(json["pending_transactions"][0]["id"] == "20260920T000000Z-beef0002");
    CHECK(json["pending_transactions"][0]["state"] == "checking");
    CHECK(json["pending_transactions"][0]["target"] == "2.0.0");
}

TEST_CASE("engine.plan:预演不动安装") {
    const fs::path root = TempRoot("plan");
    EnvVarGuard stub_version("LUBANCODE_PROBE_STUB_VERSION", "2.0.0");
    WriteFile(root / kExeName, "old flat exe bytes");
    const Package pkg = BuildPackage({.version = "2.0.0"});
    const fs::path archive = root.parent_path() / (root.filename().string() + "-pkg.zip");
    WriteFile(archive, pkg.bytes);

    EngineArgs args;
    args.verb = "plan";
    args.install_root = root;
    args.version = "2.0.0";
    args.digest = "sha256:" + pkg.digest_hex;
    args.asset_size = pkg.bytes.size();
    args.from_archive = archive;
    Lines out;
    CHECK(RunUpdaterEngine(args, out.sink(), nullptr) == 0);
    CHECK(out.contains("== 更新预演(不改安装、不动用户数据)=="));
    CHECK(out.contains("当前布局: flat"));
    CHECK(out.contains("本地包核对: 3 个官方文件(解临时目录核对,不动安装)"));
    CHECK(out.contains("磁盘预检: "));
    // 不动安装:无指针、无版本目录、无事务账。
    CHECK_FALSE(fs::exists(root / "current.json"));
    CHECK_FALSE(fs::exists(root / "versions"));
    CHECK_FALSE(fs::exists(root / "updates"));
}

// ---------------------------------------------------------------------------
// 真 EXE 探针(C++ 化红利:三平台 ctest 可跑)
// ---------------------------------------------------------------------------

namespace {

std::string FindMainExe() {
#ifdef LUBANCODE_BINARY_DIR
    const fs::path binary_dir = fs::path(LUBANCODE_BINARY_DIR);
    std::error_code ec;
    for (const char* name :
         {"lubancode", "lubancode.exe", "Debug/lubancode.exe", "Release/lubancode.exe"}) {
        const fs::path candidate = binary_dir / name;
        if (fs::exists(candidate, ec)) {
            return lubancode::platform::PathToUtf8(candidate);
        }
    }
#endif
    return std::string();
}

}  // namespace

TEST_CASE("engine.probe:真 EXE 探针 + 真主程序端到端更新") {
    const std::string exe = FindMainExe();
    REQUIRE_FALSE(exe.empty());

    // 探针:无期望版本只认 "lubancode " 头;拿到真版本再精确对。
    const auto headed = lubancode::updater::ProbeExe(fs::path(exe));
    REQUIRE(headed.ok);
    CHECK(headed.detail.rfind("lubancode ", 0) == 0);
    const std::string version = headed.detail.substr(std::strlen("lubancode "));
    REQUIRE_FALSE(version.empty());
    const auto exact = lubancode::updater::ProbeExe(fs::path(exe), version);
    CHECK(exact.ok);
    const auto mismatch = lubancode::updater::ProbeExe(fs::path(exe), std::string("0.0.0-nope"));
    CHECK_FALSE(mismatch.ok);
    CHECK(mismatch.detail.find("探针版本不合") != std::string::npos);

    // 端到端:包内 EXE 用真主程序,健康检查精确对版本,全链提交。
    const fs::path root = TempRoot("real-exe");
    std::error_code ec;
    fs::create_directories(root, ec);
    const std::string exe_bytes = [&] {
        std::ifstream in(fs::path(exe), std::ios::binary);
        if (!in.is_open()) throw std::runtime_error("真 EXE 打不开: " + exe);
        return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    }();
    const std::string rg_bytes = "#!fake ripgrep\n";

    nlohmann::json manifest = nlohmann::json::object();
    manifest["schema"] = 1;
    manifest["name"] = "lubancode";
    manifest["version"] = version;
    manifest["platform"] = "test-x64";
    manifest["channel"] = "stable";
    manifest["algo"] = "sha256";
    manifest["file_count"] = 2;
    manifest["files"] = nlohmann::json::array({
        nlohmann::json{{"path", kExeName},
                       {"size", exe_bytes.size()},
                       {"sha256", lubancode::platform::Sha256Hex(exe_bytes)}},
        nlohmann::json{{"path", "libexec/rg"},
                       {"size", rg_bytes.size()},
                       {"sha256", lubancode::platform::Sha256Hex(rg_bytes)}},
    });
    const std::string zip = BuildStoredZip({
        ZipEntry{"manifest.json", 0100644, lubancode::updater::CanonicalJsonDump(manifest)},
        ZipEntry{kExeName, 0100755, exe_bytes},
        ZipEntry{"libexec/rg", 0100755, rg_bytes},
    });
    const std::string digest_hex = lubancode::platform::Sha256Hex(zip);
    const std::string dirname = version + "-" + digest_hex.substr(0, 8);
    const fs::path archive = root.parent_path() / (root.filename().string() + "-real.zip");
    WriteFile(archive, zip);

    Lines out;
    CHECK(RunUpdaterEngine(UpdateArgs(root, archive, version, digest_hex), out.sink(), nullptr) == 0);
    CHECK(out.contains("[commit] " + version + " 已上线"));
    const auto pointer = ReadJson(root / "current.json");
    REQUIRE(pointer.has_value());
    CHECK((*pointer)["current"] == dirname);
    const auto state = ReadJson(root / "install-state.json");
    REQUIRE(state.has_value());
    CHECK((*state)["version"] == version);
    CHECK(ReadFile(root / "versions" / dirname / kExeName) == exe_bytes);
    CHECK(LedgerField(LedgerFiles(root)[0], "state") == std::optional<std::string>("committed"));
}
