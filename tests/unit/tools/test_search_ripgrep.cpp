// search 工具 ripgrep 后端合同的单测(ripgrep 迁移单 P0-2/P0-3):
//   - 定位器只认 ExecutableDir/libexec:PATH 前排放假 rg 也不被采用,
//     LUBANCODE_RG_PATH 之类的环境变量不读(读工具不能借 PATH/env 变成
//     任意执行口——本册最要紧的一条);
//   - 缺件/不可执行/版本错/spawn fail 四路稳定错误,全走注入(fake
//     filesystem + 假版本探针),不起真进程、不依赖真 rg;
//   - argv 纯函数:基线逐元素钉、参数边界(`--`)、特殊字符逐项、flag 墙、
//     用户正向 glob 的 `!` 转义;
//   - 策略构造器:硬排除、观察边界登记目录 -> root-relative 排除 glob、
//     显式点名两路对账;
//   - SearchTool 注入口:注入 fake 后行为与默认构造一字不差(P0-5 前
//     execute 不消费 runner)。

#include <doctest/doctest.h>

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "tools/observation_filter.hpp"
#include "tools/path_utils.hpp"
#include "tools/search.hpp"
#include "tools/search_ripgrep.hpp"

using lubancode::tools::BundledRipgrepLocator;
using lubancode::tools::BundledRipgrepRunner;
using lubancode::tools::BuildGlobArgv;
using lubancode::tools::BuildGrepArgv;
using lubancode::tools::BuildObservationExcludes;
using lubancode::tools::BuildSearchPolicy;
using lubancode::tools::CheckRipgrepFile;
using lubancode::tools::EscapeGlobLiteral;
using lubancode::tools::IRipgrepRunner;
using lubancode::tools::IsNeverAllowedRipgrepFlag;
using lubancode::tools::kBundledRipgrepVersion;
using lubancode::tools::ObservationBoundary;
using lubancode::tools::ParseRipgrepVersion;
using lubancode::tools::PathToUtf8;
using lubancode::tools::RipgrepFileStatus;
using lubancode::tools::RipgrepInvocation;
using lubancode::tools::RipgrepRunResult;
using lubancode::tools::RipgrepSmokeResult;
using lubancode::tools::RipgrepSmokeStatus;
using lubancode::tools::RipgrepVersionProbe;
using lubancode::tools::RunRipgrepSmoke;
using lubancode::tools::SanitizeUserIncludeGlob;
using lubancode::tools::SearchBackendError;
using lubancode::tools::SearchBackendErrorInfo;
using lubancode::tools::SearchMode;
using lubancode::tools::SearchPolicy;
using lubancode::tools::SearchRequest;
using lubancode::tools::SearchTool;
using lubancode::tools::Tool;
using lubancode::tools::ToolExecutionContext;
using lubancode::tools::ToString;
using lubancode::tools::Utf8ToPath;

namespace {

// 系统临时目录下一个独立的子目录,用完即删。
class TempDir {
public:
    TempDir() {
        path_ = std::filesystem::temp_directory_path() /
                ("lubancode_rg_test_" + std::to_string(reinterpret_cast<std::uintptr_t>(this)));
        std::filesystem::create_directories(path_);
    }
    ~TempDir() {
        std::error_code ec;
        std::filesystem::remove_all(path_, ec);
    }

    const std::filesystem::path& Path() const { return path_; }

    std::string Utf8Path(const std::string& child = "") const {
        return PathToUtf8(child.empty() ? path_ : path_ / Utf8ToPath(child));
    }

    void WriteFile(const std::string& child, const std::string& content) const {
        const std::filesystem::path full = path_ / Utf8ToPath(child);
        std::filesystem::create_directories(full.parent_path());
        std::ofstream file(full, std::ios::binary);
        file << content;
    }

    void MakeExecutable(const std::string& child) const {
        std::error_code ec;
        std::filesystem::permissions(path_ / Utf8ToPath(child),
                                     std::filesystem::perms::owner_exec |
                                         std::filesystem::perms::group_exec |
                                         std::filesystem::perms::others_exec,
                                     std::filesystem::perm_options::add, ec);
    }

private:
    std::filesystem::path path_;
};

// 观察边界是进程级单例:进出场各清一次账,别污染别的册。
class BoundaryResetGuard {
public:
    BoundaryResetGuard() { ObservationBoundary::Instance().Reset(); }
    ~BoundaryResetGuard() { ObservationBoundary::Instance().Reset(); }
};

// 环境变量 RAII:改完就还原(整册单进程顺序跑,不会与别的册并发)。
class EnvGuard {
public:
    EnvGuard(const char* name, const std::string& value) : name_(name) {
        const char* old = std::getenv(name);
        had_old_ = old != nullptr;
        old_value_ = old != nullptr ? std::string(old) : std::string();
        Set(value);
    }
    ~EnvGuard() {
        if (had_old_) {
            Set(old_value_);
        } else {
#ifdef _WIN32
            _putenv_s(name_.c_str(), "");
#else
            unsetenv(name_.c_str());
#endif
        }
    }

private:
    void Set(const std::string& value) const {
#ifdef _WIN32
        _putenv_s(name_.c_str(), value.c_str());
#else
        setenv(name_.c_str(), value.c_str(), /*overwrite=*/1);
#endif
    }

    std::string name_;
    bool had_old_ = false;
    std::string old_value_;
};

// 造一枚"形态可执行"的假 rg:Windows 上 .exe 后缀的常规文件(可执行性预检
// 只看形态,真起不起得来是 spawn 的事);POSIX 上补执行位。
std::filesystem::path MakeFakeRg(const TempDir& dir, const std::string& name, const std::string& content) {
    dir.WriteFile(name, content);
#ifndef _WIN32
    dir.MakeExecutable(name);
#else
    (void)0;
#endif
    return dir.Path() / Utf8ToPath(name);
}

// 记录调用账的假版本探针(不起进程)。
struct FakeProbe {
    int calls = 0;
    std::expected<std::string, SearchBackendErrorInfo> reply =
        std::string("ripgrep ") + std::string(kBundledRipgrepVersion) + " (rev test)\nfeatures:+pcre2\n";

    RipgrepVersionProbe AsProbe() {
        return [this](const std::filesystem::path&) { ++calls; return reply; };
    }
};

// 记录调用账的假 runner(SearchTool 注入口测试用)。
class RecordingRunner : public IRipgrepRunner {
public:
    int calls = 0;
    std::expected<RipgrepRunResult, SearchBackendErrorInfo>
    Run(const SearchRequest&, const SearchPolicy&, const ToolExecutionContext&) override {
        ++calls;
        RipgrepRunResult result;
        return result;
    }
};

bool ContainsArg(const RipgrepInvocation& invocation, const std::string& arg) {
    return std::find(invocation.args.begin(), invocation.args.end(), arg) != invocation.args.end();
}

SearchRequest GrepRequest(std::string pattern, std::filesystem::path root) {
    SearchRequest request;
    request.mode = SearchMode::Grep;
    request.pattern = std::move(pattern);
    request.root = std::move(root);
    return request;
}

SearchRequest GlobRequest(std::string pattern, std::filesystem::path root) {
    SearchRequest request;
    request.mode = SearchMode::Glob;
    request.pattern = std::move(pattern);
    request.root = std::move(root);
    return request;
}

const std::filesystem::path kFakeExe = Utf8ToPath("/nowhere/rg");  // 纯 argv 测试无关盘上真假

}  // namespace

// ---------------------------------------------------------------------------
// P0-2:定位器——只认 ExecutableDir/libexec
// ---------------------------------------------------------------------------

TEST_CASE("ripgrep locator: 只认 ExecutableDir/libexec,路径尾段与平台后缀") {
    const std::optional<std::filesystem::path> path = BundledRipgrepLocator::BundledRipgrepPath();
    if (path.has_value()) {
        const std::string utf8 = PathToUtf8(*path);
        // 尾段必须是 libexec/rg(.exe):不放在包根、不搜别处。
#ifdef _WIN32
        CHECK(utf8.find("libexec\\rg.exe") != std::string::npos);
#else
        CHECK(utf8.find("libexec/rg") != std::string::npos);
#endif
    } else {
        // ExecutablePath 拿不到(nullopt)是唯一返回 nullopt 的路,也是缺件,
        // 不是"换一条路找"。
        CHECK_FALSE(path.has_value());
    }
}

TEST_CASE("ripgrep locator: 注入路径原样奉还,不做任何回退") {
    BoundaryResetGuard boundary;
    const TempDir dir;
    const std::filesystem::path injected = dir.Path() / Utf8ToPath("custom") / Utf8ToPath("rg.exe");
    BundledRipgrepLocator locator(injected);
    const std::optional<std::filesystem::path> located = locator.Locate();
    REQUIRE(located.has_value());
    CHECK(*located == injected);  // 路径不存在也原样回:注入方要看的就是"不回退"
}

TEST_CASE("ripgrep locator: PATH 前排放假 rg 也不被采用") {
    BoundaryResetGuard boundary;
    const TempDir dir;
    // PATH 上放一枚"形态完整"的假 rg(POSIX 还真有执行位,跑了会写 marker)。
    const std::string marker = dir.Utf8Path("hijacked.marker");
    const std::string script = "#!/bin/sh\ntouch '" + marker + "'\nexit 0\n";
#ifdef _WIN32
    MakeFakeRg(dir, "rg.exe", "MZ fake rg for hijack test");
#else
    MakeFakeRg(dir, "rg", script);
#endif
    const std::string fake_dir = dir.Utf8Path();
    const char* old_path = std::getenv("PATH");
    const std::string new_path = fake_dir + (old_path != nullptr ? ":" + std::string(old_path) : "");
    {
        EnvGuard path_guard("PATH", new_path);

        // 定位结果不受 PATH 影响:要么 nullopt,要么绝不含假目录。
        const std::optional<std::filesystem::path> path = BundledRipgrepLocator::BundledRipgrepPath();
        if (path.has_value()) {
            CHECK(PathToUtf8(*path).find(fake_dir) == std::string::npos);
        }
        // 注入口的定位同样不落进假目录。
        BundledRipgrepLocator default_locator;
        const std::optional<std::filesystem::path> via_locator = default_locator.Locate();
        if (via_locator.has_value()) {
            CHECK(PathToUtf8(*via_locator).find(fake_dir) == std::string::npos);
        }

        // smoke 走生产唯一路径:libexec 无 rg 就明报缺件,绝不转身执行 PATH
        // 上的假 rg。POSIX 下假 rg 跑过会留 marker——marker 不在,才是铁证。
        const std::optional<std::filesystem::path> exe = BundledRipgrepLocator::BundledRipgrepPath();
        if (exe.has_value()) {
            const RipgrepSmokeResult smoke = RunRipgrepSmoke(*exe);
            // 本机 libexec 通常无 rg:缺件;即便有(将来 Release 包),也不该
            // 是假目录里那枚。两种情形都排除"执行了 PATH 假 rg"。
            if (smoke.status == RipgrepSmokeStatus::Missing) {
                CHECK(smoke.code == SearchBackendError::BackendMissing);
            }
        }
#ifndef _WIN32
        CHECK_FALSE(std::filesystem::exists(Utf8ToPath(marker)));
#endif
    }
    // 还原后再看一眼:PATH 还原不影响定位(定位从来不看 PATH)。
    const std::optional<std::filesystem::path> after = BundledRipgrepLocator::BundledRipgrepPath();
    (void)after;
}

TEST_CASE("ripgrep locator: LUBANCODE_RG_PATH 环境变量不读") {
    BoundaryResetGuard boundary;
    const TempDir dir;
    {
        // 明令禁止的口子:设了也当没看见,定位结果与不设时一字不差。
        const std::optional<std::filesystem::path> before = BundledRipgrepLocator::BundledRipgrepPath();
        EnvGuard guard("LUBANCODE_RG_PATH", dir.Utf8Path("evil.exe"));
        const std::optional<std::filesystem::path> after = BundledRipgrepLocator::BundledRipgrepPath();
        CHECK(before.has_value() == after.has_value());
        if (before.has_value() && after.has_value()) {
            CHECK(*before == *after);
        }
    }
}

// ---------------------------------------------------------------------------
// P0-2:文件校验与版本 smoke
// ---------------------------------------------------------------------------

TEST_CASE("ripgrep 文件校验: 缺件/不可执行/形态可执行三态") {
    const TempDir dir;
    // 不存在。
    CHECK(CheckRipgrepFile(dir.Path() / Utf8ToPath("nope") / Utf8ToPath("rg.exe")) == RipgrepFileStatus::Missing);
    CHECK(CheckRipgrepFile(dir.Path() / Utf8ToPath("absent_rg.exe")) == RipgrepFileStatus::Missing);
    // 存在但是目录。
    std::filesystem::create_directories(dir.Path() / Utf8ToPath("adir"));
    CHECK(CheckRipgrepFile(dir.Path() / Utf8ToPath("adir")) == RipgrepFileStatus::NotExecutable);
    // 文本文件:Windows 上扩展名不是 .exe;POSIX 上没有执行位。
    dir.WriteFile("plain.txt", "i am not rg");
    CHECK(CheckRipgrepFile(dir.Path() / Utf8ToPath("plain.txt")) == RipgrepFileStatus::NotExecutable);
#ifndef _WIN32
    dir.WriteFile("noexec_rg", "#!/bin/sh\n");
    CHECK(CheckRipgrepFile(dir.Path() / Utf8ToPath("noexec_rg")) == RipgrepFileStatus::NotExecutable);
    // 补上执行位即形态可执行(内容是不是 rg 由版本 smoke 管)。
    dir.MakeExecutable("noexec_rg");
    CHECK(CheckRipgrepFile(dir.Path() / Utf8ToPath("noexec_rg")) == RipgrepFileStatus::Ok);
#else
    // Windows:.exe 后缀的常规文件即形态可执行。
    dir.WriteFile("rg.exe", "MZ fake but shaped right");
    CHECK(CheckRipgrepFile(dir.Path() / Utf8ToPath("rg.exe")) == RipgrepFileStatus::Ok);
#endif
}

TEST_CASE("ripgrep 版本解析: 首行第二枚记号,rev 段不钉") {
    CHECK(ParseRipgrepVersion("ripgrep 15.2.0 (rev e89fff89ac)\nfeatures:+pcre2\n") == std::string("15.2.0"));
    CHECK(ParseRipgrepVersion("ripgrep 14.1.1\n") == std::string("14.1.1"));
    CHECK(ParseRipgrepVersion("ripgrep 15.2.0") == std::string("15.2.0"));        // 无换行
    CHECK(ParseRipgrepVersion("ripgrep 15.2.0\r\nfeatures:+pcre2\r\n") ==
          std::string("15.2.0"));                                                  // CRLF
    CHECK_FALSE(ParseRipgrepVersion("").has_value());                              // 空
    CHECK_FALSE(ParseRipgrepVersion("rg 15.2.0\n").has_value());                   // 程序名不合
    CHECK_FALSE(ParseRipgrepVersion("ripgrep \n").has_value());                    // 版本记号缺
    CHECK_FALSE(ParseRipgrepVersion("Segmentation fault\n").has_value());          // 完全不是版本行
}

TEST_CASE("ripgrep smoke: 缺件与不可执行不起进程") {
    const TempDir dir;
    FakeProbe probe;  // 若真走到探针,calls 会露馅
    const RipgrepSmokeResult missing = RunRipgrepSmoke(dir.Path() / Utf8ToPath("absent.exe"), probe.AsProbe());
    CHECK(missing.status == RipgrepSmokeStatus::Missing);
    CHECK(missing.code == SearchBackendError::BackendMissing);
    std::filesystem::create_directories(dir.Path() / Utf8ToPath("d"));
    const RipgrepSmokeResult not_exec = RunRipgrepSmoke(dir.Path() / Utf8ToPath("d"), probe.AsProbe());
    CHECK(not_exec.status == RipgrepSmokeStatus::NotExecutable);
    CHECK(not_exec.code == SearchBackendError::NotExecutable);
    CHECK(probe.calls == 0);  // 文件这关都没过,不碰进程
}

TEST_CASE("ripgrep smoke: 版本精确校验(过/错/认不出)") {
    const TempDir dir;
    const std::filesystem::path exe = MakeFakeRg(dir, "rg.exe", "fake");

    FakeProbe ok_probe;
    ok_probe.reply = std::string("ripgrep ") + std::string(kBundledRipgrepVersion) + " (rev e89fff89ac)\n";
    const RipgrepSmokeResult ok = RunRipgrepSmoke(exe, ok_probe.AsProbe());
    CHECK(ok.status == RipgrepSmokeStatus::Ready);
    CHECK_FALSE(ok.code.has_value());
    CHECK(ok.found_version == kBundledRipgrepVersion);

    FakeProbe old_probe;
    old_probe.reply = "ripgrep 14.1.0 (rev old)\n";
    const RipgrepSmokeResult old = RunRipgrepSmoke(exe, old_probe.AsProbe());
    CHECK(old.status == RipgrepSmokeStatus::VersionMismatch);
    CHECK(old.code == SearchBackendError::VersionMismatch);
    CHECK(old.found_version == "14.1.0");

    FakeProbe garbage_probe;
    garbage_probe.reply = "totally not ripgrep output\n";
    const RipgrepSmokeResult garbage = RunRipgrepSmoke(exe, garbage_probe.AsProbe());
    CHECK(garbage.status == RipgrepSmokeStatus::VersionMismatch);  // 身份不合按版本口径报
    CHECK(garbage.code == SearchBackendError::VersionMismatch);
    CHECK(garbage.found_version.empty());

    FakeProbe spawn_probe;
    spawn_probe.reply = std::unexpected(SearchBackendErrorInfo{SearchBackendError::SpawnFailed, "boom"});
    const RipgrepSmokeResult spawn_fail = RunRipgrepSmoke(exe, spawn_probe.AsProbe());
    CHECK(spawn_fail.status == RipgrepSmokeStatus::SmokeFailed);
    CHECK(spawn_fail.code == SearchBackendError::SpawnFailed);
}

// ---------------------------------------------------------------------------
// P0-2/P0-4:runner——四路稳定错误 + 真起进程 + smoke 只做一次
// ---------------------------------------------------------------------------

TEST_CASE("ripgrep runner: 缺件/不可执行/版本错/spawn fail 四路稳定错误") {
    const TempDir dir;

    BundledRipgrepRunner missing(dir.Path() / Utf8ToPath("absent") / Utf8ToPath("rg.exe"));
    auto result = missing.Run(GrepRequest("x", dir.Path()), BuildSearchPolicy(GrepRequest("x", dir.Path())),
                              ToolExecutionContext{});
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().code == SearchBackendError::BackendMissing);
    CHECK(ToString(result.error().code) == "search_backend_missing");

    std::filesystem::create_directories(dir.Path() / Utf8ToPath("notfile"));
    BundledRipgrepRunner not_exec(dir.Path() / Utf8ToPath("notfile"));
    result = not_exec.Run(GrepRequest("x", dir.Path()), SearchPolicy{}, ToolExecutionContext{});
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().code == SearchBackendError::NotExecutable);
    CHECK(ToString(result.error().code) == "search_backend_not_executable");

    FakeProbe old;
    old.reply = "ripgrep 13.0.0\n";
    BundledRipgrepRunner wrong_version(MakeFakeRg(dir, "rg_old.exe", "x"), old.AsProbe());
    result = wrong_version.Run(GrepRequest("x", dir.Path()), SearchPolicy{}, ToolExecutionContext{});
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().code == SearchBackendError::VersionMismatch);
    CHECK(ToString(result.error().code) == "search_backend_version_mismatch");

    FakeProbe boom;
    boom.reply = std::unexpected(SearchBackendErrorInfo{SearchBackendError::SpawnFailed, "cannot spawn"});
    BundledRipgrepRunner spawn_fail(MakeFakeRg(dir, "rg_spawn.exe", "x"), boom.AsProbe());
    result = spawn_fail.Run(GrepRequest("x", dir.Path()), SearchPolicy{}, ToolExecutionContext{});
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().code == SearchBackendError::SpawnFailed);
    CHECK(ToString(result.error().code) == "search_backend_spawn_failed");
}

TEST_CASE("ripgrep runner: 前置全过后真起进程,假件起不来报 spawn failed") {
    // P0-4 起流式执行已接线:前置(缺件/不可执行/版本)全过的下一站是
    // ChildProcess 真起进程。形态可执行但不是真 exe 的假件在这里如实报
    // spawn 失败——没有 NotWired 这一站,也没有本地内核可退。
    const TempDir dir;
    FakeProbe probe;
    BundledRipgrepRunner runner(MakeFakeRg(dir, "rg_ok.exe", "x"), probe.AsProbe());
    const auto result = runner.Run(GrepRequest("x", dir.Path()), SearchPolicy{}, ToolExecutionContext{});
    REQUIRE_FALSE(result.has_value());
    const bool spawn_failed = result.error().code == SearchBackendError::SpawnFailed;
    CHECK(spawn_failed);
    CHECK(ToString(result.error().code) == "search_backend_spawn_failed");
    CHECK(runner.smoke_result().status == RipgrepSmokeStatus::Ready);  // smoke 过了,死在起进程
}

TEST_CASE("ripgrep runner: smoke 每实例只做一次(缓存)") {
    const TempDir dir;
    FakeProbe probe;
    BundledRipgrepRunner runner(MakeFakeRg(dir, "rg_cache.exe", "x"), probe.AsProbe());
    ToolExecutionContext context;
    for (int i = 0; i < 3; ++i) {
        (void)runner.Run(GrepRequest("x", dir.Path()), SearchPolicy{}, context);
    }
    CHECK(probe.calls == 1);  // 后两次走缓存,不再起进程/探针
}

TEST_CASE("ripgrep runner: 默认构造走生产唯一路径") {
    // 不注入任何东西:定位只能来自 ExecutableDir/libexec。开发构建的运行
    // 目录 libexec 通常无 rg -> 缺件;分期入位(CTest 目录里有 rg)时搜索
    // 照常。这条钉的是"默认构造绝不改道 PATH/环境变量",两态都不冒充。
    BundledRipgrepRunner runner;
    const auto result = runner.Run(GrepRequest("x", std::filesystem::temp_directory_path()), SearchPolicy{},
                                   ToolExecutionContext{});
    if (result.has_value()) {
        // rg 在位:真搜了一把(temp 目录里搜 "x"),成功即对。
        CHECK(runner.smoke_result().status == RipgrepSmokeStatus::Ready);
        return;
    }
    const bool honest_terminal_state = result.error().code == SearchBackendError::BackendMissing ||
                                       result.error().code == SearchBackendError::NotExecutable;
    CHECK(honest_terminal_state);
}

// ---------------------------------------------------------------------------
// P0-2:SearchTool 注入口——行为一字不差
// ---------------------------------------------------------------------------

TEST_CASE("search 注入口: 注入 fake runner 后 execute 行为与默认构造一致,fake 不被调") {
    const TempDir dir;
    dir.WriteFile("a.txt", "needle here\nplain line\n");

    nlohmann::json input;
    input["mode"] = "grep";
    input["pattern"] = "needle";
    input["path"] = dir.Utf8Path();

    SearchTool default_tool;
    const Tool::Result default_result = default_tool.execute(input);

    auto runner = std::make_shared<RecordingRunner>();
    SearchTool injected_tool(runner);
    const Tool::Result injected_result = injected_tool.execute(input);

    CHECK_FALSE(default_result.is_error);
    CHECK_FALSE(injected_result.is_error);
    CHECK(injected_result.content.find("a.txt:1:needle here") != std::string::npos);
    CHECK(injected_result.content == default_result.content);  // 一字不差
    CHECK(runner->calls == 0);  // P0-5 切主路之前 execute 不消费 runner

    // 工具身份合同不动:名字、schema、effect class 照旧。
    CHECK(injected_tool.name() == "search");
    CHECK(injected_tool.effect_class() == lubancode::tools::EffectClass::ReadOnlyLocal);
    CHECK(injected_tool.idempotency() == lubancode::tools::Idempotency::Idempotent);
}

// ---------------------------------------------------------------------------
// P0-3:argv 纯函数——基线逐元素钉
// ---------------------------------------------------------------------------

TEST_CASE("grep argv 基线: 目录 root,默认 policy,无用户 glob") {
    const std::filesystem::path root = Utf8ToPath("D:/proj") / Utf8ToPath("src");
    const RipgrepInvocation argv = BuildGrepArgv(GrepRequest("SearchTool", root), SearchPolicy{}, kFakeExe);
    CHECK(argv.exe == kFakeExe);
    CHECK(argv.cwd_utf8 == PathToUtf8(root));
    const std::vector<std::string> expected = {
        "--no-config", "--json", "--line-buffered", "--color=never", "--hidden",
        "--engine=default", "--no-multiline", "--", "SearchTool", ".",
    };
    CHECK(argv.args == expected);
}

TEST_CASE("glob argv 基线: --files --null,用户 pattern 作正向 -g") {
    const std::filesystem::path root = Utf8ToPath("D:/proj");
    const RipgrepInvocation argv =
        BuildGlobArgv(GlobRequest("**/*.cpp", root), SearchPolicy{}, kFakeExe);
    CHECK(argv.cwd_utf8 == PathToUtf8(root));
    const std::vector<std::string> expected = {
        "--no-config", "--files", "--null", "--hidden", "-g", "**/*.cpp", "--", ".",
    };
    CHECK(argv.args == expected);
}

TEST_CASE("argv: 单文件 root 用父目录当 cwd、文件名当 scope") {
    const std::filesystem::path file = Utf8ToPath("D:/proj") / Utf8ToPath("src") / Utf8ToPath("a.cpp");
    SearchRequest request = GrepRequest("needle", file);
    request.root_is_single_file = true;
    const RipgrepInvocation argv = BuildGrepArgv(request, SearchPolicy{}, kFakeExe);
    CHECK(argv.cwd_utf8 == PathToUtf8(file.parent_path()));
    REQUIRE(argv.args.size() >= 2);
    CHECK(argv.args[argv.args.size() - 2] == "needle");
    CHECK(argv.args[argv.args.size() - 1] == "a.cpp");

    SearchRequest glob_request = GlobRequest("*.cpp", file);
    glob_request.root_is_single_file = true;
    const RipgrepInvocation glob_argv = BuildGlobArgv(glob_request, SearchPolicy{}, kFakeExe);
    CHECK(glob_argv.cwd_utf8 == PathToUtf8(file.parent_path()));
    CHECK(glob_argv.args.back() == "a.cpp");
}

TEST_CASE("grep argv: fixed_strings 与用户 glob/宿主排除的次序") {
    SearchRequest request = GrepRequest("a.b", Utf8ToPath("/p"));
    request.fixed_strings = true;
    request.glob = "*.cpp";
    SearchPolicy policy;
    policy.exclude_globs = {"**/.git/**", "!logs/**"};
    const RipgrepInvocation argv = BuildGrepArgv(request, policy, kFakeExe);
    const std::vector<std::string> expected = {
        "--no-config", "--json", "--line-buffered", "--color=never", "--hidden",
        "--engine=default", "--no-multiline", "--fixed-strings",
        "-g", "*.cpp",
        "-g", "**/.git/**",
        "-g", "!logs/**",
        "--", "a.b", ".",
    };
    CHECK(argv.args == expected);
}

// ---------------------------------------------------------------------------
// P0-3:参数边界——开头是 '-' 也变不成 flag
// ---------------------------------------------------------------------------

TEST_CASE("argv 参数边界: pattern/path 以 - 开头不串成 flag") {
    // pattern="-g"、pattern="--help":都落在 -- 之后,只是 positional。
    const RipgrepInvocation argv = BuildGrepArgv(GrepRequest("-g", Utf8ToPath("/p")), SearchPolicy{}, kFakeExe);
    REQUIRE(argv.args.size() >= 3);
    const std::size_t sep = std::find(argv.args.begin(), argv.args.end(), "--") - argv.args.begin();
    CHECK(sep == argv.args.size() - 3);  // -- 是倒数第三:后面只有 pattern 与 scope
    CHECK(argv.args[sep + 1] == "-g");   // 原样,没被吃成 flag
    CHECK(argv.args[sep + 2] == ".");

    const RipgrepInvocation help_argv =
        BuildGrepArgv(GrepRequest("--help", Utf8ToPath("/p")), SearchPolicy{}, kFakeExe);
    CHECK(ContainsArg(help_argv, "--help"));
    CHECK(std::count(help_argv.args.begin(), help_argv.args.end(), "--") == 1);

    // glob="-x":作为 -g 的独立值元素,不会被解析成 flag(argv 直传,无 shell)。
    SearchRequest request = GrepRequest("x", Utf8ToPath("/p"));
    request.glob = "-x";
    const RipgrepInvocation glob_argv = BuildGrepArgv(request, SearchPolicy{}, kFakeExe);
    const auto it = std::find(glob_argv.args.begin(), glob_argv.args.end(), "-g");
    REQUIRE(it != glob_argv.args.end());
    CHECK(*(it + 1) == "-x");
}

TEST_CASE("argv 参数边界: 单文件 scope 以 - 开头同样只是 positional") {
    const std::filesystem::path file = Utf8ToPath("/p") / Utf8ToPath("-weird.txt");
    SearchRequest request = GrepRequest("x", file);
    request.root_is_single_file = true;
    const RipgrepInvocation argv = BuildGrepArgv(request, SearchPolicy{}, kFakeExe);
    // 尾三枚恒为 [--, pattern, scope]:边界紧挨在 pattern 之前。
    REQUIRE(argv.args.size() >= 3);
    CHECK(argv.args[argv.args.size() - 3] == "--");
    CHECK(argv.args[argv.args.size() - 2] == "x");
    CHECK(argv.args.back() == "-weird.txt");
}

// ---------------------------------------------------------------------------
// P0-3:特殊字符逐项——空格/引号/中文/元字符/Windows 盘符
// ---------------------------------------------------------------------------

TEST_CASE("argv: 空格、单双引号、中文逐项原样保留") {
    struct Case {
        std::string pattern;
    };
    const std::vector<Case> cases = {
        {"word with spaces"},
        {"it's \"quoted\""},
        {"中文搜索词"},
        {"混合 mixed 中文 and spaces"},
        {R"(\backslash\)"},
        {"tab\tand\nnewline"},
    };
    for (const Case& c : cases) {
        const RipgrepInvocation argv = BuildGrepArgv(GrepRequest(c.pattern, Utf8ToPath("/p")), SearchPolicy{}, kFakeExe);
        // argv 数组直起进程:pattern 就是那一枚元素,逐字节原样,无转义层。
        CHECK(ContainsArg(argv, c.pattern));
    }
}

TEST_CASE("argv: 正则/通配元字符在 pattern 里原样(语法归 Rust regex 解释)") {
    const std::vector<std::string> patterns = {
        "[abc]{2,3}", "foo.*bar", "a?b*c", "**/*.cpp", "a|b", "(x)?(?=y)", R"(\d+\w+)",
    };
    for (const std::string& pattern : patterns) {
        const RipgrepInvocation argv = BuildGrepArgv(GrepRequest(pattern, Utf8ToPath("/p")), SearchPolicy{}, kFakeExe);
        CHECK(ContainsArg(argv, pattern));
    }
}

TEST_CASE("argv: Windows 盘符路径当 cwd/scope 原样") {
    const std::filesystem::path root = Utf8ToPath("D:\\proj\\my repo");
    const RipgrepInvocation argv = BuildGrepArgv(GrepRequest("x", root), SearchPolicy{}, kFakeExe);
    CHECK(argv.cwd_utf8 == PathToUtf8(root));  // cwd 走 OS 参数,不拼命令行字符串
    CHECK(argv.args.back() == ".");
}

TEST_CASE("argv: 文件名含空格引号中文,scope 逐字节原样") {
    const std::vector<std::string> names = {
        "a b.txt", "it's.md", "引号\"文\"件.txt", "中文文件.cpp", "star*name.txt", "brack[et].hpp",
    };
    for (const std::string& name : names) {
        const std::filesystem::path file = Utf8ToPath("/p") / Utf8ToPath(name);
        SearchRequest request = GrepRequest("x", file);
        request.root_is_single_file = true;
        const RipgrepInvocation argv = BuildGrepArgv(request, SearchPolicy{}, kFakeExe);
        CHECK(argv.args.back() == name);
        CHECK(argv.cwd_utf8 == PathToUtf8(Utf8ToPath("/p")));
    }
}

// ---------------------------------------------------------------------------
// P0-3:用户正向 glob 不可偷变排除规则
// ---------------------------------------------------------------------------

TEST_CASE("glob 转义: 用户正向 glob 首字符 ! 按字面,不偷变排除") {
    CHECK(SanitizeUserIncludeGlob("!secret") == "[!]secret");
    CHECK(SanitizeUserIncludeGlob("!!double") == "[!]!double");
    CHECK(SanitizeUserIncludeGlob("a!b") == "a!b");   // 中间的 ! 本就是字面
    CHECK(SanitizeUserIncludeGlob("*.cpp") == "*.cpp");
    CHECK(SanitizeUserIncludeGlob("").empty());

    // grep 的 glob 过滤与 glob 模式的 pattern 都过这道转义。
    SearchRequest request = GrepRequest("x", Utf8ToPath("/p"));
    request.glob = "!secret";
    const RipgrepInvocation argv = BuildGrepArgv(request, SearchPolicy{}, kFakeExe);
    const auto it = std::find(argv.args.begin(), argv.args.end(), "-g");
    REQUIRE(it != argv.args.end());
    CHECK(*(it + 1) == "[!]secret");
    CHECK(ContainsArg(argv, "!secret") == false);  // 原样 '!secret' 不许出现

    const RipgrepInvocation glob_argv =
        BuildGlobArgv(GlobRequest("!logs/**", Utf8ToPath("/p")), SearchPolicy{}, kFakeExe);
    const auto git = std::find(glob_argv.args.begin(), glob_argv.args.end(), "-g");
    REQUIRE(git != glob_argv.args.end());
    CHECK(*(git + 1) == "[!]logs/**");
}

TEST_CASE("glob 转义: EscapeGlobLiteral 把元字符全变字面") {
    CHECK(EscapeGlobLiteral("plain") == "plain");
    CHECK(EscapeGlobLiteral("a*b?c") == "a\\*b\\?c");
    CHECK(EscapeGlobLiteral("[x]{y}") == "\\[x\\]\\{y\\}");
    CHECK(EscapeGlobLiteral("a!b") == "a\\!b");
    CHECK(EscapeGlobLiteral(R"(back\slash)") == R"(back\\slash)");
}

// ---------------------------------------------------------------------------
// P0-3:flag 墙——首版不开放的 rg 能力没有输入面
// ---------------------------------------------------------------------------

TEST_CASE("argv flag 墙: 绝不出现的 flag 一枚不漏") {
    // IsNeverAllowedRipgrepFlag 自身先对账。
    CHECK(IsNeverAllowedRipgrepFlag("--pre"));
    CHECK(IsNeverAllowedRipgrepFlag("--pre-glob"));
    CHECK(IsNeverAllowedRipgrepFlag("--search-zip"));
    CHECK(IsNeverAllowedRipgrepFlag("--pcre2"));
    CHECK(IsNeverAllowedRipgrepFlag("--auto-hybrid-regex"));
    CHECK(IsNeverAllowedRipgrepFlag("--type-add"));
    CHECK(IsNeverAllowedRipgrepFlag("-u"));
    CHECK(IsNeverAllowedRipgrepFlag("--unrestricted"));
    // --engine=default 合法;裸 --engine(换引擎)不许。
    CHECK(IsNeverAllowedRipgrepFlag("--engine"));
    CHECK_FALSE(IsNeverAllowedRipgrepFlag("--engine=default"));
    CHECK_FALSE(IsNeverAllowedRipgrepFlag("--hidden"));
    CHECK_FALSE(IsNeverAllowedRipgrepFlag("--json"));

    // 各种输入形状下扫整条 argv:一枚都不许出现。
    SearchPolicy policy = BuildSearchPolicy(GrepRequest("x", Utf8ToPath("/p")));
    std::vector<RipgrepInvocation> invocations;
    invocations.push_back(BuildGrepArgv(GrepRequest("--pre", Utf8ToPath("/p")), policy, kFakeExe));
    invocations.push_back(BuildGrepArgv(GrepRequest("x", Utf8ToPath("/p")), policy, kFakeExe));
    SearchRequest with_glob = GrepRequest("x", Utf8ToPath("/p"));
    with_glob.glob = "*";
    invocations.push_back(BuildGrepArgv(with_glob, policy, kFakeExe));
    SearchRequest fixed = GrepRequest("--type-add", Utf8ToPath("/p"));
    fixed.fixed_strings = true;
    invocations.push_back(BuildGrepArgv(fixed, policy, kFakeExe));
    invocations.push_back(BuildGlobArgv(GlobRequest("**/*", Utf8ToPath("/p")), policy, kFakeExe));
    for (const RipgrepInvocation& argv : invocations) {
        // 只扫 `--` 之前的 flag 区:`--` 之后是 pattern/scope 的 positional,
        // 用户输入长得像 flag 也不许被吃(flag 注入边界另有专测)。
        const std::size_t sep = std::find(argv.args.begin(), argv.args.end(), "--") - argv.args.begin();
        for (std::size_t i = 0; i < sep; ++i) {
            CHECK_FALSE(IsNeverAllowedRipgrepFlag(argv.args[i]));
            CHECK_FALSE(argv.args[i] == "--follow");
            CHECK_FALSE(argv.args[i] == "--text");
            CHECK_FALSE(argv.args[i] == "--no-ignore");
        }
        // 默认 policy 下这些也不出现(有 policy 开关,生产恒关)。
        CHECK_FALSE(ContainsArg(argv, "--follow"));
        CHECK_FALSE(ContainsArg(argv, "--text"));
        CHECK_FALSE(ContainsArg(argv, "--no-ignore"));
    }
}

TEST_CASE("argv policy 开关位: include_hidden/ignore/链接/二进制的映射") {
    SearchPolicy policy;
    policy.include_hidden = false;
    const RipgrepInvocation no_hidden = BuildGrepArgv(GrepRequest("x", Utf8ToPath("/p")), policy, kFakeExe);
    CHECK_FALSE(ContainsArg(no_hidden, "--hidden"));

    policy = SearchPolicy{};
    policy.respect_ignore_files = false;
    const RipgrepInvocation no_ignore = BuildGlobArgv(GlobRequest("*", Utf8ToPath("/p")), policy, kFakeExe);
    CHECK(ContainsArg(no_ignore, "--no-ignore"));  // 宿主策略位可关(生产不开)

    policy = SearchPolicy{};
    policy.follow_symlinks = true;
    const RipgrepInvocation follow = BuildGlobArgv(GlobRequest("*", Utf8ToPath("/p")), policy, kFakeExe);
    CHECK(ContainsArg(follow, "--follow"));

    policy = SearchPolicy{};
    policy.search_binary = true;
    const RipgrepInvocation text = BuildGrepArgv(GrepRequest("x", Utf8ToPath("/p")), policy, kFakeExe);
    CHECK(ContainsArg(text, "--text"));
}

TEST_CASE("argv 三平台同语义: scope 恒 '.' 或文件名字面,不含平台分隔符") {
    const std::filesystem::path root = Utf8ToPath("/a/b");
    const RipgrepInvocation argv = BuildGrepArgv(GrepRequest("x", root), SearchPolicy{}, kFakeExe);
    CHECK(argv.args.back() == ".");
    // builder 内没有平台分支:args 由 request/policy 决定,与编译平台无关。
    // 钉一条可观测面:目录模式下 args 不含 '\\'/'/' 之外的路径拼装痕迹。
    for (const std::string& arg : argv.args) {
        CHECK(arg != root.string());  // 绝不把 root 绝对路径塞进 argv
    }
}

// ---------------------------------------------------------------------------
// P0-3:策略构造器——硬排除、观察边界、显式点名两路对账
// ---------------------------------------------------------------------------

TEST_CASE("policy: 默认硬排除四目录,任意深度(!**/<名>/**)") {
    BoundaryResetGuard boundary;
    const SearchPolicy policy = BuildSearchPolicy(GrepRequest("x", Utf8ToPath("/proj")));
    // 排除项必须带 ! 前缀:裸 glob 在 rg 眼里是包含项(P0-4 真机差分翻过
    // 这车),钉死带 ! 的四条。
    const std::vector<std::string> expected = {
        "!**/.git/**", "!**/build/**", "!**/node_modules/**", "!**/.evidence/**",
    };
    CHECK(policy.include_hidden);
    CHECK(policy.respect_ignore_files);
    CHECK_FALSE(policy.follow_symlinks);
    CHECK_FALSE(policy.search_binary);
    CHECK(policy.exclude_globs == expected);  // 无登记时恰四条,不多不少
}

TEST_CASE("policy: 登记目录落在 root 下才生成 root-relative 排除") {
    BoundaryResetGuard boundary;
    const TempDir dir;
    const std::filesystem::path root = dir.Path();
    std::filesystem::create_directories(root / Utf8ToPath("logs") / Utf8ToPath("sub"));
    std::filesystem::create_directories(root / Utf8ToPath("plain"));

    ObservationBoundary::Instance().AddExcludedDir(root / Utf8ToPath("logs"));
    // root 之外:登记到 temp 根下另一处(不落在 root 之下)。
    const std::filesystem::path outside = dir.Path().parent_path() /
                                          Utf8ToPath("lubancode_rg_test_outside_" +
                                                     std::to_string(reinterpret_cast<std::uintptr_t>(&dir)));
    std::filesystem::create_directories(outside);
    ObservationBoundary::Instance().AddExcludedDir(outside);

    const SearchPolicy policy = BuildSearchPolicy(GrepRequest("x", root));
    CHECK(std::find(policy.exclude_globs.begin(), policy.exclude_globs.end(), "!logs/**") !=
          policy.exclude_globs.end());
    // root 外的登记不生成。
    for (const std::string& glob : policy.exclude_globs) {
        CHECK(glob.find("outside") == std::string::npos);
    }
}

TEST_CASE("policy: root 本身是登记目录 = 显式点名,不生观察排除") {
    BoundaryResetGuard boundary;
    const TempDir dir;
    const std::filesystem::path logs = dir.Path() / Utf8ToPath("logs");
    std::filesystem::create_directories(logs);
    ObservationBoundary::Instance().AddExcludedDir(logs);

    // 点名进目录:硬排除仍在(与旧内核 ShouldSkipDir 无条件跳名一致),但该
    // 目录的观察排除不生成——相对 root 的路径不再咬根下文件。
    const SearchPolicy policy = BuildSearchPolicy(GrepRequest("x", logs));
    CHECK(std::find(policy.exclude_globs.begin(), policy.exclude_globs.end(), "!logs/**") ==
          policy.exclude_globs.end());
    CHECK(std::find(policy.exclude_globs.begin(), policy.exclude_globs.end(), "!**/.git/**") !=
          policy.exclude_globs.end());
}

TEST_CASE("policy: root 落在 .evidence 内(名字口径)同样按显式点名放行") {
    BoundaryResetGuard boundary;
    const TempDir dir;
    const std::filesystem::path evidence = dir.Path() / Utf8ToPath(".evidence");
    std::filesystem::create_directories(evidence / Utf8ToPath("inner"));
    // .evidence 名字口径:root 在边界内,不生观察排除(硬排除里的
    // **/.evidence/** 按 root-relative 语义咬不到根下文件——对账旧内核)。
    const SearchPolicy policy = BuildSearchPolicy(GrepRequest("x", evidence));
    CHECK(policy.exclude_globs.size() == 4);  // 只有硬排除四条
}

TEST_CASE("policy: 嵌套登记目录转相对 glob,深层路径逐段保留") {
    BoundaryResetGuard boundary;
    const TempDir dir;
    const std::filesystem::path root = dir.Path();
    std::filesystem::create_directories(root / Utf8ToPath("a") / Utf8ToPath("b"));
    ObservationBoundary::Instance().AddExcludedDir(root / Utf8ToPath("a") / Utf8ToPath("b"));
    const SearchPolicy policy = BuildSearchPolicy(GrepRequest("x", root));
    CHECK(std::find(policy.exclude_globs.begin(), policy.exclude_globs.end(), "!a/b/**") !=
          policy.exclude_globs.end());
}

TEST_CASE("BuildObservationExcludes: 目录名带 glob 元字符按字面转义,斜杠统一") {
    const TempDir dir;
    const std::filesystem::path root = dir.Path();
    // star*dir 在 Windows 是非法文件名,不落盘——BuildObservationExcludes 是
    // 纯路径函数,不碰盘,直接喂路径对象即可。
    std::filesystem::create_directories(root / Utf8ToPath("带 空格") / Utf8ToPath("b"));
    std::vector<std::filesystem::path> registered = {
        root / Utf8ToPath("star*dir"),
        root / Utf8ToPath("带 空格") / Utf8ToPath("b"),
        root,  // root 本身,滤掉(显式点名路径);root 外的情形前一条测试已钉
    };
    const std::vector<std::string> excludes = BuildObservationExcludes(root, registered);
    REQUIRE(excludes.size() == 2);
    // 排序后:转义过的 star*dir(ASCII 在前)与 UTF-8 中文目录各一条。
    CHECK(excludes[0] == "!star\\*dir/**");
    CHECK(excludes[1] == "!带 空格/b/**");
}

TEST_CASE("ObservationBoundary::ExcludedDirsSnapshot: 快照即登记账,线程安全拷贝") {
    BoundaryResetGuard boundary;
    const TempDir dir;
    CHECK(ObservationBoundary::Instance().ExcludedDirsSnapshot().empty());
    ObservationBoundary::Instance().AddExcludedDir(dir.Path());
    const std::vector<std::filesystem::path> snapshot =
        ObservationBoundary::Instance().ExcludedDirsSnapshot();
    REQUIRE(snapshot.size() == 1);
    // 登记侧做过 weakly_canonical 规范化(如短名 MOONTI~1 解析成真实拼写),
    // 同一道规范化后比较,不比原始拼写。
    CHECK(snapshot[0] == std::filesystem::weakly_canonical(dir.Path()));
    ObservationBoundary::Instance().Reset();
    CHECK(ObservationBoundary::Instance().ExcludedDirsSnapshot().empty());
}
