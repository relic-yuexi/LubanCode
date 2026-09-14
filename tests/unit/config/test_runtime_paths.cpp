// 应用根语义(应用Worker接入补齐单 P1)的测试册:LUBANCODE_HOME /
// LUBANCODE_DATA_HOME / LUBANCODE_MANAGED 三变量的解析、校验、来源裁剪
// 与双根隔离。合同冻结合同见 docs/reference/capability-contract.md §13。
//
// 册内环境一律显式构造:ResolveRuntimePaths 是纯函数,喂 RuntimeEnvSnapshot
// 注入表(两套快照即可模拟两只 Worker 并行双根);进程级函数
// (HomeLubancodeDir/StateRootDir/LoadFileConfigs)用 EnvGuard 设假
// USERPROFILE/HOME + LUBANCODE_HOME,观测文件系统副作用。与测试套注入的
// LUBANCODE_TRAJECTORY_V3_NEW_SESSIONS=0 不冲突(不同变量名,互不触碰)。
//
// 路径入参全部用真实绝对路径(临时树):"/tmp/..." 这类字面量在 Windows
// 上不是绝对路径,会先撞相对路径拒绝分支,测不到目标语义。
// 不动真机用户目录:假家目录在临时树里造,断言它零读写副作用。
#include <doctest/doctest.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <optional>
#include <string>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>  // SetEnvironmentVariableW:EnvGuard 的 Win32 面
#endif

#include "config/config.hpp"
#include "config/runtime_paths.hpp"
#include "platform/paths.hpp"
#include "ptc/profile.hpp"
#include "tools/search_ripgrep.hpp"

namespace {

// 设一枚环境变量,析构删除。空值也是"设了"——用 Set("") 表达,与合同
// "未设置与设置为空分开处理"对齐。
//
// Windows 双写(实证见 paths_win.cpp GetEnvVarPresent 注释,2026-09-14
// 本机探针):CRT 的 _putenv("NAME=") 文档语义是删除变量,活进程造不出
// 空值条目;要设出 "NAME=" 空条目只有 Win32 面 SetEnvironmentVariableW。
// 双写让 CRT 消费面(getenv/_dupenv_s,读不到空值、当未设)与 Win32 面
// (生产 GetEnvVarPresent,能区分空值/未设)各自看到一致事实:
//   值非空 → CRT+Win32 都设上;
//   值为空 → 只有 Win32 面设上(CRT 面 _putenv 的空值=删除,恰好保持
//            "CRT 消费方当未设"的既有语义);
//   析构   → 两面都删(_putenv 会同步删 Win32,显式再删一道兜底)。
// POSIX 无此分家,setenv/unsetenv 原样。
struct EnvGuard {
    explicit EnvGuard(const char* name, const std::string& value) : name_(name) {
#ifdef _WIN32
        _putenv((std::string(name_) + "=" + value).c_str());
        // 值是 UTF-8 路径,转宽走正道,不逐字节窄转(非 ASCII 会坏)。
        // Win32 面统一用成员 name_(构造函数里参数 name 等值,但析构函数
        // 没有参数——上一版在 #ifdef _WIN32 块里裸写 name,macos 腿不编
        // 这块,MSVC 编到才炸 C2065;统一走成员,两函数一个写法)。
        SetEnvironmentVariableW(lubancode::platform::Utf8ToWide(name_).c_str(),
                                lubancode::platform::Utf8ToWide(value).c_str());
#else
        setenv(name_, value.c_str(), 1);
#endif
    }
    ~EnvGuard() {
#ifdef _WIN32
        _putenv((std::string(name_) + "=").c_str());
        SetEnvironmentVariableW(lubancode::platform::Utf8ToWide(name_).c_str(), nullptr);
#else
        unsetenv(name_);
#endif
    }
    const char* name_;
};

std::filesystem::path FreshRoot(const char* tag) {
    const auto dir = std::filesystem::temp_directory_path() /
                     ("lubancode-runtime-paths-" + std::string(tag));
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    std::filesystem::create_directories(dir, ec);
    return dir;
}

std::string U8(const std::filesystem::path& path) {
    return lubancode::platform::PathToUtf8(path);
}

// 路径等值断言专用:两条写法不同的路径是不是同一个地方(斜杠/大小写)。
std::string Key(const std::string& utf8) {
    return lubancode::platform::PathComparisonKey(lubancode::platform::Utf8ToPath(utf8));
}

// 快照速记:vars 直接送 map,home_dir 可缺省。
lubancode::config::RuntimeEnvSnapshot Snap(
    std::map<std::string, std::string> vars = {},
    std::optional<std::string> home_dir = std::nullopt) {
    lubancode::config::RuntimeEnvSnapshot env;
    env.vars = std::move(vars);
    env.home_dir = std::move(home_dir);
    return env;
}

}  // namespace

// ---------------------------------------------------------------------------
// ResolveRuntimePaths:纯解析与校验
// ---------------------------------------------------------------------------

TEST_CASE("runtime_paths:未设三变量即个人默认布局") {
    const auto resolved = lubancode::config::ResolveRuntimePaths(Snap());
    REQUIRE(resolved.has_value());
    CHECK_FALSE(resolved->app_root_active);
    CHECK_FALSE(resolved->managed);
    CHECK_FALSE(resolved->config_root.has_value());
    CHECK_FALSE(resolved->data_root.has_value());
}

TEST_CASE("runtime_paths:LUBANCODE_HOME 即参数根,数据根默认根内 data") {
    const std::string root = U8(FreshRoot("resolve-a"));
    const auto resolved = lubancode::config::ResolveRuntimePaths(
        Snap({{"LUBANCODE_HOME", root}}));
    REQUIRE(resolved.has_value());
    CHECK(resolved->app_root_active);
    CHECK_FALSE(resolved->managed);
    REQUIRE(resolved->config_root.has_value());
    CHECK(Key(U8(*resolved->config_root)) == Key(root));
    REQUIRE(resolved->data_root.has_value());
    CHECK(Key(U8(*resolved->data_root)) == Key(root + "/data"));
}

TEST_CASE("runtime_paths:显式数据根生效") {
    const std::string root = U8(FreshRoot("resolve-b"));
    const std::string state = U8(FreshRoot("resolve-b-state"));
    const auto resolved = lubancode::config::ResolveRuntimePaths(
        Snap({{"LUBANCODE_HOME", root}, {"LUBANCODE_DATA_HOME", state}}));
    REQUIRE(resolved.has_value());
    REQUIRE(resolved->data_root.has_value());
    CHECK(Key(U8(*resolved->data_root)) == Key(state));
}

TEST_CASE("runtime_paths:空值与相对路径都是配置错误,不静默回个人目录") {
    const std::string root = U8(FreshRoot("resolve-c"));
    // 空值(设了但为空)。
    auto empty_home = lubancode::config::ResolveRuntimePaths(
        Snap({{"LUBANCODE_HOME", ""}}));
    REQUIRE_FALSE(empty_home.has_value());
    CHECK(empty_home.error().find("LUBANCODE_HOME") != std::string::npos);

    auto empty_data = lubancode::config::ResolveRuntimePaths(
        Snap({{"LUBANCODE_HOME", root}, {"LUBANCODE_DATA_HOME", ""}}));
    REQUIRE_FALSE(empty_data.has_value());
    CHECK(empty_data.error().find("LUBANCODE_DATA_HOME") != std::string::npos);

    // 相对路径("rel/config" 无盘符/根锚,两平台都不是绝对路径)。
    auto rel_home = lubancode::config::ResolveRuntimePaths(
        Snap({{"LUBANCODE_HOME", "rel/config"}}));
    REQUIRE_FALSE(rel_home.has_value());
    CHECK(rel_home.error().find("绝对路径") != std::string::npos);

    auto rel_data = lubancode::config::ResolveRuntimePaths(
        Snap({{"LUBANCODE_HOME", root}, {"LUBANCODE_DATA_HOME", "rel/state"}}));
    REQUIRE_FALSE(rel_data.has_value());
    CHECK(rel_data.error().find("绝对路径") != std::string::npos);
}

TEST_CASE("runtime_paths:孤立数据根(无参数根)是配置错误") {
    const std::string state = U8(FreshRoot("resolve-d-state"));
    const auto resolved = lubancode::config::ResolveRuntimePaths(
        Snap({{"LUBANCODE_DATA_HOME", state}}));
    REQUIRE_FALSE(resolved.has_value());
    CHECK(resolved.error().find("LUBANCODE_DATA_HOME") != std::string::npos);
}

TEST_CASE("runtime_paths:参数根与数据根的不合法重叠被拒,合法嵌套放行") {
    const std::string root = U8(FreshRoot("resolve-e"));
    const std::string state = U8(FreshRoot("resolve-e-state"));

    // 数据根 == 参数根:读写不分。
    auto equal = lubancode::config::ResolveRuntimePaths(
        Snap({{"LUBANCODE_HOME", root}, {"LUBANCODE_DATA_HOME", root}}));
    REQUIRE_FALSE(equal.has_value());

    // 同一条路径两种写法(尾斜杠)也算相等。
    auto trailing = lubancode::config::ResolveRuntimePaths(
        Snap({{"LUBANCODE_HOME", root}, {"LUBANCODE_DATA_HOME", root + "/"}}));
    REQUIRE_FALSE(trailing.has_value());

    // 参数根落数据根之内:只读根被可写根吞。
    auto swallowed = lubancode::config::ResolveRuntimePaths(
        Snap({{"LUBANCODE_HOME", state + "/config"},
              {"LUBANCODE_DATA_HOME", state}}));
    REQUIRE_FALSE(swallowed.has_value());

    // 数据根在参数根之内:合法(默认 <HOME>/data 即如此)。
    auto nested = lubancode::config::ResolveRuntimePaths(
        Snap({{"LUBANCODE_HOME", root}, {"LUBANCODE_DATA_HOME", root + "/data"}}));
    REQUIRE(nested.has_value());
}

TEST_CASE("runtime_paths:LUBANCODE_MANAGED 只认 1/0,托管必须有参数根") {
    const std::string root = U8(FreshRoot("resolve-f"));

    auto bad = lubancode::config::ResolveRuntimePaths(
        Snap({{"LUBANCODE_MANAGED", "yes"}}));
    REQUIRE_FALSE(bad.has_value());
    CHECK(bad.error().find("LUBANCODE_MANAGED") != std::string::npos);

    auto empty = lubancode::config::ResolveRuntimePaths(
        Snap({{"LUBANCODE_MANAGED", ""}}));
    REQUIRE_FALSE(empty.has_value());

    auto no_root = lubancode::config::ResolveRuntimePaths(
        Snap({{"LUBANCODE_MANAGED", "1"}}));
    REQUIRE_FALSE(no_root.has_value());
    CHECK(no_root.error().find("LUBANCODE_HOME") != std::string::npos);

    auto off = lubancode::config::ResolveRuntimePaths(
        Snap({{"LUBANCODE_HOME", root}, {"LUBANCODE_MANAGED", "0"}}));
    REQUIRE(off.has_value());
    CHECK(off->app_root_active);
    CHECK_FALSE(off->managed);

    auto on = lubancode::config::ResolveRuntimePaths(
        Snap({{"LUBANCODE_HOME", root}, {"LUBANCODE_MANAGED", "1"}}));
    REQUIRE(on.has_value());
    CHECK(on->managed);
}

// ---------------------------------------------------------------------------
// 双根隔离:两套 env 快照并行解析,互不见对方材料(AW-04 的进程内面)
// ---------------------------------------------------------------------------

TEST_CASE("runtime_paths:双应用根并行解析互不串") {
    const auto root_a = FreshRoot("dual-a");
    const auto root_b = FreshRoot("dual-b");
    // 两只"Worker"各在各的参数根里放了一份不同 provider 的配置材料。
    const auto write_config = [](const std::filesystem::path& root, const std::string& model) {
        std::filesystem::create_directories(root);
        std::ofstream out(root / "config.json", std::ios::binary | std::ios::trunc);
        out << "{\"model\": \"" << model << "\"}";
    };
    write_config(root_a, "model-alpha");
    write_config(root_b, "model-beta");

    // 进程环境形状参照只证明快照可造,不参与两根解析。
    const auto process_shape = lubancode::config::CaptureProcessEnv();
    (void)process_shape;

    lubancode::config::RuntimeEnvSnapshot snap_a;
    snap_a.vars.emplace("LUBANCODE_HOME", U8(root_a));
    lubancode::config::RuntimeEnvSnapshot snap_b;
    snap_b.vars.emplace("LUBANCODE_HOME", U8(root_b));

    const auto paths_a = lubancode::config::ResolveRuntimePaths(snap_a);
    const auto paths_b = lubancode::config::ResolveRuntimePaths(snap_b);
    REQUIRE(paths_a.has_value());
    REQUIRE(paths_b.has_value());
    REQUIRE(paths_a->config_root.has_value());
    REQUIRE(paths_b->config_root.has_value());
    // 解析结果各归各的根;路径文本比较走 UTF-8 通道,不碰 path 窄口。
    CHECK(Key(U8(*paths_a->config_root)) == Key(U8(root_a)));
    CHECK(Key(U8(*paths_b->config_root)) == Key(U8(root_b)));
    CHECK(Key(U8(*paths_a->data_root)) == Key(U8(root_a / "data")));
    CHECK(Key(U8(*paths_b->data_root)) == Key(U8(root_b / "data")));
    CHECK(U8(*paths_a->config_root).find(U8(root_b)) == std::string::npos);
    CHECK(U8(*paths_b->config_root).find(U8(root_a)) == std::string::npos);
}

// ---------------------------------------------------------------------------
// 进程级函数:HomeLubancodeDir/StateRootDir 重定向 + 个人家目录零副作用
// ---------------------------------------------------------------------------

TEST_CASE("runtime_paths:应用根语义下进程级路径重定向,个人家目录零写入") {
    const auto fake_home = FreshRoot("fake-home");
    const auto app_root = FreshRoot("app-root");
    {
        EnvGuard home_guard("USERPROFILE", U8(fake_home));
        EnvGuard luban_home("LUBANCODE_HOME", U8(app_root));
#ifndef _WIN32
        EnvGuard posix_home("HOME", U8(fake_home));
#endif

        // 参数根即 HomeLubancodeDir,不追加 .lubancode。
        const auto materials = lubancode::config::HomeLubancodeDir();
        REQUIRE(materials.has_value());
        CHECK(Key(*materials) == Key(U8(app_root)));

        // 状态根默认 <HOME>/data。
        const auto state = lubancode::config::StateRootDir();
        REQUIRE(state.has_value());
        CHECK(Key(*state) == Key(U8(app_root / "data")));

        // 个人材料层(个人 skills 扫描的 home 入参)整层裁掉。
        CHECK_FALSE(lubancode::config::PersonalMaterialsHomeDir().has_value());
        CHECK(lubancode::config::AppRootActive());

        // 路径解析结果不指向假家目录:个人家目录进不了视野。
        CHECK(materials->find(U8(fake_home.filename())) == std::string::npos);
        CHECK(state->find(U8(fake_home.filename())) == std::string::npos);
    }
    // 假家目录零写入副作用:没有 .lubancode、没有任何新文件。
    std::error_code ec;
    CHECK_FALSE(std::filesystem::exists(fake_home / ".lubancode", ec));
    CHECK(std::filesystem::is_empty(fake_home, ec));
}

TEST_CASE("runtime_paths:显式数据根的进程级重定向") {
    const auto fake_home = FreshRoot("fake-home2");
    const auto app_root = FreshRoot("app-root2");
    const auto state_root = FreshRoot("state-root2");
    {
        EnvGuard home_guard("USERPROFILE", U8(fake_home));
        EnvGuard luban_home("LUBANCODE_HOME", U8(app_root));
        EnvGuard luban_data("LUBANCODE_DATA_HOME", U8(state_root));
#ifndef _WIN32
        EnvGuard posix_home("HOME", U8(fake_home));
#endif
        const auto state = lubancode::config::StateRootDir();
        REQUIRE(state.has_value());
        CHECK(Key(*state) == Key(U8(state_root)));
    }
    CHECK_FALSE(std::filesystem::exists(fake_home / ".lubancode"));
}

TEST_CASE("runtime_paths:未设变量时个人默认行为原样(AW-01)") {
    const auto fake_home = FreshRoot("personal-home");
    {
        EnvGuard home_guard("USERPROFILE", U8(fake_home));
#ifndef _WIN32
        EnvGuard posix_home("HOME", U8(fake_home));
#endif
        // 不设 LUBANCODE_HOME:个人布局原样——假家目录下 .lubancode。
        const auto materials = lubancode::config::HomeLubancodeDir();
        REQUIRE(materials.has_value());
        CHECK(Key(*materials) == Key(U8(fake_home / ".lubancode")));
        // 状态根与材料根同目录(个人单根布局)。
        const auto state = lubancode::config::StateRootDir();
        REQUIRE(state.has_value());
        CHECK(Key(*state) == Key(*materials));
        CHECK_FALSE(lubancode::config::AppRootActive());
        // 个人材料层照常=主目录。
        const auto personal = lubancode::config::PersonalMaterialsHomeDir();
        REQUIRE(personal.has_value());
        CHECK(Key(*personal) == Key(U8(fake_home)));
    }
}

TEST_CASE("runtime_paths:坏值在库级不静默回个人目录") {
    const auto fake_home = FreshRoot("fake-home3");
    {
        EnvGuard home_guard("USERPROFILE", U8(fake_home));
        EnvGuard empty_home("LUBANCODE_HOME", "");
#ifndef _WIN32
        EnvGuard posix_home("HOME", U8(fake_home));
#endif
        // 启动门会拒;库级兜底是"无根可用",不是回落个人目录。
        CHECK_FALSE(lubancode::config::HomeLubancodeDir().has_value());
        CHECK_FALSE(lubancode::config::StateRootDir().has_value());
        CHECK_FALSE(lubancode::config::AppRootActive());
    }
    std::error_code ec;
    CHECK(std::filesystem::is_empty(fake_home, ec));
}

// ---------------------------------------------------------------------------
// LoadFileConfigs:托管模式的来源裁剪
// ---------------------------------------------------------------------------

TEST_CASE("runtime_paths:托管模式不读 cwd 项目级,全局层走参数根") {
    const auto fake_home = FreshRoot("fake-home4");
    const auto app_root = FreshRoot("app-root4");
    const auto work_dir = FreshRoot("cwd4");
    // 参数根放一份全局配置(全局层该读到它)。
    {
        std::ofstream out(app_root / "config.json", std::ios::binary | std::ios::trunc);
        out << "{\"model\": \"app-model\"}";
    }
    // cwd 放探针项目配置(托管模式下不得进视野)。
    std::filesystem::create_directories(work_dir / ".lubancode");
    {
        std::ofstream out(work_dir / ".lubancode" / "config.json",
                          std::ios::binary | std::ios::trunc);
        out << "{\"model\": \"cwd-model\"}";
    }

    const std::filesystem::path restore_cwd = std::filesystem::current_path();
    std::error_code ec;
    std::filesystem::current_path(work_dir, ec);
    REQUIRE_FALSE(ec);

    {
        EnvGuard home_guard("USERPROFILE", U8(fake_home));
        EnvGuard luban_home("LUBANCODE_HOME", U8(app_root));
        EnvGuard managed("LUBANCODE_MANAGED", "1");
#ifndef _WIN32
        EnvGuard posix_home("HOME", U8(fake_home));
#endif
        const auto loaded = lubancode::config::LoadFileConfigs();
        REQUIRE(loaded.has_value());
        // 项目级整层裁掉:探针不进视野。
        CHECK_FALSE(loaded->project.has_value());
        // 全局层=参数根 config.json(不是 <home>/.lubancode,不是 cwd)。
        REQUIRE(loaded->global.has_value());
        CHECK(loaded->global->model == "app-model");
        CHECK(lubancode::config::AppRootActive());
    }

    // 同一 cwd、个人模式(不设应用根):项目级探针照常读——证明裁剪只
    // 发生在托管档,不是把项目级扫描整个删了。
    {
        EnvGuard home_guard("USERPROFILE", U8(fake_home));
#ifndef _WIN32
        EnvGuard posix_home("HOME", U8(fake_home));
#endif
        const auto loaded = lubancode::config::LoadFileConfigs();
        REQUIRE(loaded.has_value());
        REQUIRE(loaded->project.has_value());
        CHECK(loaded->project->model == "cwd-model");
    }

    std::filesystem::current_path(restore_cwd, ec);
    // 假家目录在两轮装载后依旧零写入(配置装载不播种个人目录)。
    CHECK_FALSE(std::filesystem::exists(fake_home / ".lubancode", ec));
}

TEST_CASE("runtime_paths:应用根(非托管)全局层读参数根,项目级照旧") {
    const auto fake_home = FreshRoot("fake-home5");
    const auto app_root = FreshRoot("app-root5");
    const auto work_dir = FreshRoot("cwd5");
    {
        std::ofstream out(app_root / "config.json", std::ios::binary | std::ios::trunc);
        out << "{\"model\": \"app-model\"}";
    }
    std::filesystem::create_directories(work_dir / ".lubancode");
    {
        std::ofstream out(work_dir / ".lubancode" / "config.json",
                          std::ios::binary | std::ios::trunc);
        out << "{\"model\": \"cwd-model\"}";
    }

    const std::filesystem::path restore_cwd = std::filesystem::current_path();
    std::error_code ec;
    std::filesystem::current_path(work_dir, ec);
    REQUIRE_FALSE(ec);

    {
        EnvGuard home_guard("USERPROFILE", U8(fake_home));
        EnvGuard luban_home("LUBANCODE_HOME", U8(app_root));
#ifndef _WIN32
        EnvGuard posix_home("HOME", U8(fake_home));
#endif
        const auto loaded = lubancode::config::LoadFileConfigs();
        REQUIRE(loaded.has_value());
        // 非托管:cwd 项目级照旧。
        REQUIRE(loaded->project.has_value());
        CHECK(loaded->project->model == "cwd-model");
        // 全局层=参数根 config.json。
        REQUIRE(loaded->global.has_value());
        CHECK(loaded->global->model == "app-model");
    }

    std::filesystem::current_path(restore_cwd, ec);
    CHECK_FALSE(std::filesystem::exists(fake_home / ".lubancode", ec));
}

TEST_CASE("runtime_paths:EnsureRuntimeRootsAccessible 在获准父目录内建根") {
    const auto base = FreshRoot("accessible");
    lubancode::config::RuntimePaths paths;
    paths.app_root_active = true;
    paths.config_root = base / "config";
    paths.data_root = base / "state";
    REQUIRE(lubancode::config::EnsureRuntimeRootsAccessible(paths).has_value());
    CHECK(std::filesystem::is_directory(base / "config"));
    CHECK(std::filesystem::is_directory(base / "state"));

    // 已存在(幂等)不算错。
    CHECK(lubancode::config::EnsureRuntimeRootsAccessible(paths).has_value());

    // 根位置被同名文件占住:不是目录,明拒。
    const auto occupied = FreshRoot("occupied");
    {
        std::ofstream out(occupied / "config", std::ios::binary);
        out << "x";
    }
    lubancode::config::RuntimePaths bad;
    bad.app_root_active = true;
    bad.config_root = occupied / "config";
    const auto refused = lubancode::config::EnsureRuntimeRootsAccessible(bad);
    REQUIRE_FALSE(refused.has_value());
    CHECK(refused.error().find("不是目录") != std::string::npos);
}

// ---------------------------------------------------------------------------
// P1 遗留销账(一):rg-stage 读口随状态根;播种脚本 fetch_ripgrep.sh 的
// --target 缺省同语义(脚本侧矩阵断言在 ctest scripts.fetch_ripgrep_paths)
// ---------------------------------------------------------------------------

TEST_CASE("runtime_paths:rg-stage 候选层随状态根,个人布局原样") {
    using lubancode::tools::CollectRipgrepCandidates;
    using lubancode::tools::RipgrepSource;
    const auto fake_home = FreshRoot("rg-home");
    const auto app_root = FreshRoot("rg-app");
    const auto state_root = FreshRoot("rg-state");

    // 找 UserStage 那一层(全三层里只此一层),校它落哪——只对目录校,
    // 不掺平台差异的可执行名(rg/rg.exe)。
    const auto user_stage_parent = [](const std::vector<lubancode::tools::RipgrepCandidate>& candidates) {
        for (const auto& candidate : candidates) {
            if (candidate.source == RipgrepSource::UserStage) {
                return lubancode::platform::PathToUtf8(candidate.exe.parent_path());
            }
        }
        return std::string{};
    };

    // 个人模式:UserStage=<home>/.lubancode/rg-stage/libexec 原样(旧布局)。
    {
        EnvGuard home_guard("USERPROFILE", U8(fake_home));
#ifndef _WIN32
        EnvGuard posix_home("HOME", U8(fake_home));
#endif
        const auto parent = user_stage_parent(CollectRipgrepCandidates());
        REQUIRE_FALSE(parent.empty());
        CHECK(Key(parent) == Key(U8(fake_home / ".lubancode" / "rg-stage" / "libexec")));
    }

    // 应用根+显式数据根:UserStage=数据根(不是参数根——工具缓存是状态)。
    {
        EnvGuard home_guard("USERPROFILE", U8(fake_home));
        EnvGuard luban_home("LUBANCODE_HOME", U8(app_root));
        EnvGuard luban_data("LUBANCODE_DATA_HOME", U8(state_root));
#ifndef _WIN32
        EnvGuard posix_home("HOME", U8(fake_home));
#endif
        const auto parent = user_stage_parent(CollectRipgrepCandidates());
        REQUIRE_FALSE(parent.empty());
        CHECK(Key(parent) == Key(U8(state_root / "rg-stage" / "libexec")));
    }
}

// ---------------------------------------------------------------------------
// P1 遗留销账(二):ptc_profiles.json 归用户偏好材料,留参数根不落数据根
// (合同 §13.2;个人布局=旧位置原样,零迁移零惊扰)
// ---------------------------------------------------------------------------

TEST_CASE("runtime_paths:ptc 画像存档留参数根,不落数据根") {
    const auto fake_home = FreshRoot("ptc-home");
    const auto app_root = FreshRoot("ptc-app");
    const auto state_root = FreshRoot("ptc-state");

    // 应用根+显式数据根:存档在参数根——即使数据根另设也不切过去。
    {
        EnvGuard home_guard("USERPROFILE", U8(fake_home));
        EnvGuard luban_home("LUBANCODE_HOME", U8(app_root));
        EnvGuard luban_data("LUBANCODE_DATA_HOME", U8(state_root));
#ifndef _WIN32
        EnvGuard posix_home("HOME", U8(fake_home));
#endif
        const std::string store = lubancode::ptc::DefaultProfileStorePath();
        CHECK(Key(store) == Key(U8(app_root / "ptc_profiles.json")));
        CHECK(Key(store) != Key(U8(state_root / "ptc_profiles.json")));
    }

    // 应用根、数据根未设:同样在参数根(默认数据根 <HOME>/data 不沾)。
    {
        EnvGuard home_guard("USERPROFILE", U8(fake_home));
        EnvGuard luban_home("LUBANCODE_HOME", U8(app_root));
#ifndef _WIN32
        EnvGuard posix_home("HOME", U8(fake_home));
#endif
        CHECK(Key(lubancode::ptc::DefaultProfileStorePath()) ==
              Key(U8(app_root / "ptc_profiles.json")));
    }

    // 个人模式:旧位置原样(<home>/.lubancode)——旧文件照读,不暗迁移。
    {
        EnvGuard home_guard("USERPROFILE", U8(fake_home));
#ifndef _WIN32
        EnvGuard posix_home("HOME", U8(fake_home));
#endif
        CHECK(Key(lubancode::ptc::DefaultProfileStorePath()) ==
              Key(U8(fake_home / ".lubancode" / "ptc_profiles.json")));
    }

    // 坏值:库级无根返回空串(调用方按"没有存档"处理),不回落个人目录。
    {
        EnvGuard home_guard("USERPROFILE", U8(fake_home));
        EnvGuard empty_home("LUBANCODE_HOME", "");
#ifndef _WIN32
        EnvGuard posix_home("HOME", U8(fake_home));
#endif
        CHECK(lubancode::ptc::DefaultProfileStorePath().empty());
    }
}
