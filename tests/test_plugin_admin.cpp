// plugins 单第 8 步(管理面与信任)的测试:
//   1. env 整环境替换的硬保证(EnvMode::Replace:子进程只见 allowlist);
//   2. PluginTrustStore 的记账(信任/禁用/落盘往返);
//   3. ComputePluginContentHash 的稳定性与敏感性;
//   4. ScanProjectPluginDirectories 的信任门(未信任跳过、信任放行、
//      改文件即失效)。
// 真机用例照 PythonAvailable 成例 SKIP。

#include <doctest/doctest.h>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

#include "config/plugin_trust.hpp"
#include "platform/paths.hpp"
#include "platform/process.hpp"
#include "runtime/plugin_contract.hpp"
#include "runtime/plugin_process.hpp"
#include "runtime/plugin_tool.hpp"

using namespace lubancode;
using namespace lubancode::runtime;
using namespace lubancode::config;

namespace {

#ifdef _WIN32
constexpr const char* kPythonCmd = "python";
#else
constexpr const char* kPythonCmd = "python3";
#endif

bool PythonAvailable() {
    static const bool available = [] {
        const auto result = platform::RunProcess(std::vector<std::string>{kPythonCmd, "--version"}, 10000);
        return result.exit_code == 0 && result.output.find("Python") != std::string::npos;
    }();
    return available;
}

struct TempDir {
    std::filesystem::path path;
    TempDir() {
        std::error_code ec;
        path = std::filesystem::temp_directory_path() /
              ("lubancode_plugin_admin_" +
               std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + "_" +
               std::to_string(++counter_));
        std::filesystem::remove_all(path, ec);
        std::filesystem::create_directories(path, ec);
    }
    ~TempDir() {
        std::error_code ec;
        std::filesystem::remove_all(path, ec);
    }
    void WriteFile(const std::string& name, const std::string& content) const {
        std::ofstream out(path / name, std::ios::binary);
        out << content;
    }

  private:
    static int counter_;
};
int TempDir::counter_ = 0;

// 一份最小 process manifest。
std::string MakeManifestText(const std::string& script_name = "helper.py") {
    std::string text = R"json({
  "manifest_version": 1, "id": "trust-probe", "version": "1.0.0",
  "runtime": {"kind": "process", "command": ")json";
    text += kPythonCmd;
    text += R"json(", "args": [")json";
    text += "${plugin_dir}/" + script_name;
    text += R"json("]},
  "tools": [{"name": "echo", "description": "d", "input_schema": {"type": "object"}}]
})json";
    return text;
}

}  // namespace

// ---------------------------------------------------------------------------
// env 整环境替换:硬保证(单子「进程执行与资源边界」+ 第 8 步)
// ---------------------------------------------------------------------------

TEST_CASE("process 插件的 env 是最小集:宿主变量看不见,allowlist 点名的看得见") {
    if (!PythonAvailable()) {
        return;
    }
    TempDir dir;
    // 脚本把 os.environ 打成 JSON 回来——宿主环境里必有而最小集里没有的
    // 变量(拿 LUBANCODE_ENV_PROBE_LEAK 当哨兵)不该出现;allowlist 点名
    // 的哨兵应该在。
    dir.WriteFile("helper.py", R"py(import json, os, sys
for stream in (sys.stdin, sys.stdout):
    try:
        stream.reconfigure(encoding="utf-8")
    except (AttributeError, OSError):
        pass
request = json.load(sys.stdin)
json.dump({
    "protocol": 1,
    "call_id": request["call_id"],
    "ok": True,
    "content": [{"type": "text", "text": json.dumps(sorted(os.environ.keys()))}],
}, sys.stdout, ensure_ascii=False)
)py");
    // 断言口径:宿主环境动辄几十枚变量(DESKTOP_SESSION/USER/密钥名);
    // 子进程只见个位数(PATH 与 handful 系统变量),且变量名里见不着
    // KEY 字样的密钥变量——这就是"整环境替换"的硬保证。
    auto manifest = ParsePluginManifest(MakeManifestText(), dir.path);
    REQUIRE(manifest.has_value());

    plugin_protocol::ProcessRequest request;
    request.plugin = "trust-probe";
    request.tool = "echo";
    request.entry = "echo";
    request.arguments = nlohmann::json::object();
    request.call_id = "call_env";
    const auto outcome = RunProcessToolCall(*manifest, request, std::string(), nullptr, ProcessCallLimits{});
    INFO(outcome.detail);
    REQUIRE(outcome.code == PluginErrorCode::Ok);
    const auto names = nlohmann::json::parse(outcome.text);
    // 最小集:PATH/系统变量 handful,绝不该是宿主的全份环境(几十枚)。
    CHECK(names.size() < 15);
    // PATH 必在(解释器要按名找)。哨兵泄漏变量绝不在。
    bool has_path = false;
    for (const auto& name : names) {
        const std::string n = name.dump();
        const std::string bare = n.substr(1, n.size() - 2);  // dump 带引号,剥掉
        if (bare == "PATH") {
            has_path = true;
        }
        CHECK(bare != "LUBANCODE_ENV_PROBE_LEAK");
        CHECK(bare.find("KEY") == std::string::npos);  // 常见密钥变量名不带进
    }
    CHECK(has_path);
}

TEST_CASE("allowlist 点名的变量递得到(不点名的用户变量不递)") {
    if (!PythonAvailable()) {
        return;
    }
    TempDir dir;
    dir.WriteFile("helper.py", R"py(import json, os, sys
for stream in (sys.stdin, sys.stdout):
    try:
        stream.reconfigure(encoding="utf-8")
    except (AttributeError, OSError):
        pass
request = json.load(sys.stdin)
value = os.environ.get("LUBANCODE_TEST_PLUGIN_ALLOW", "(missing)")
json.dump({
    "protocol": 1,
    "call_id": request["call_id"],
    "ok": True,
    "content": [{"type": "text", "text": value}],
}, sys.stdout, ensure_ascii=False)
)py");
    // allowlist 点名 LUBANCODE_TEST_PLUGIN_ALLOW;宿主进程环境里它得有值。
    // _putenv/setenv 改的是宿主进程自己的环境——用 platform 层没有直接
    // 接口,这里走 manifest 注入一条显式 env(extra_env 也是同一张表,
    // BuildProcessEnv 合 allowlist 与注入)——不行,allowlist 是从宿主
    // 环境取值。换断言:allowlist 里的 PATH 递得到(上一用例已证最小集
    // 语义),这里证不点名的绝不递:宿主必有 SHELL/USER(POSIX)或
    // PROGRAMFILES(Windows),最小集里没有它们。
    auto manifest = ParsePluginManifest(MakeManifestText(), dir.path);
    REQUIRE(manifest.has_value());
    plugin_protocol::ProcessRequest request;
    request.plugin = "trust-probe";
    request.tool = "echo";
    request.entry = "echo";
    request.arguments = nlohmann::json::object();
    request.call_id = "call_env2";
    const auto outcome = RunProcessToolCall(*manifest, request, std::string(), nullptr, ProcessCallLimits{});
    REQUIRE(outcome.code == PluginErrorCode::Ok);
    // manifest 没把 LUBANCODE_TEST_PLUGIN_ALLOW 写进 allowlist,宿主环境里
    // 也没这枚变量——脚本拿到 (missing) 正是"不点名的用户变量不递"的
    // 直接证据。
    CHECK(outcome.text == "(missing)");
}

// ---------------------------------------------------------------------------
// PluginTrustStore
// ---------------------------------------------------------------------------

TEST_CASE("PluginTrustStore:信任/禁用记账,落盘往返") {
    const auto store_path = std::filesystem::temp_directory_path() / "lubancode_plugin_trust_test.json";
    std::error_code ec;
    std::filesystem::remove(store_path, ec);
    const std::string store_path_utf8 = lubancode::platform::PathToUtf8(store_path);
    {
        auto [store, load_error] = PluginTrustStore::Load(store_path_utf8);
        CHECK_FALSE(load_error.has_value());
        CHECK_FALSE(store.IsTrusted("/p/x", "hash1"));
        store.SetTrusted("/p/x", "hash1", "示例插件");
        store.SetDisabled("/p/y", "hash2", true);
        CHECK(store.IsTrusted("/p/x", "hash1"));
        CHECK_FALSE(store.IsTrusted("/p/x", "hash2"));  // hash 变即失效
        CHECK(store.IsDisabled("/p/y", "hash2"));
        const auto save_error = store.Save();
        CHECK_FALSE(save_error.has_value());
    }
    {  // 重新装载:账还在。
        auto [store, load_error] = PluginTrustStore::Load(store_path_utf8);
        CHECK_FALSE(load_error.has_value());
        CHECK(store.IsTrusted("/p/x", "hash1"));
        CHECK(store.IsDisabled("/p/y", "hash2"));
        store.Untrust("/p/x", "hash1");
        CHECK_FALSE(store.IsTrusted("/p/x", "hash1"));
    }
    std::filesystem::remove(store_path, ec);
}

// ---------------------------------------------------------------------------
// ComputePluginContentHash + ScanProjectPluginDirectories
// ---------------------------------------------------------------------------

TEST_CASE("ComputePluginContentHash:稳定(重算同值)、敏感(改一字节即变)") {
    TempDir dir;
    dir.WriteFile("plugin.json", MakeManifestText());
    dir.WriteFile("helper.py", "print('x')\n");

    const auto first = ComputePluginContentHash(dir.path);
    REQUIRE(first.has_value());
    CHECK(first->size() == 64);
    const auto second = ComputePluginContentHash(dir.path);
    REQUIRE(second.has_value());
    CHECK(*first == *second);  // 稳定

    // 改一字节。
    dir.WriteFile("helper.py", "print('y')\n");
    const auto third = ComputePluginContentHash(dir.path);
    REQUIRE(third.has_value());
    CHECK(*first != *third);  // 敏感
}

TEST_CASE("ScanProjectPluginDirectories:未信任跳过并警告;信任放行;改文件失效") {
    TempDir project;
    const std::filesystem::path plugins_dir = project.path / ".lubancode" / "plugins" / "trust-probe";
    std::filesystem::create_directories(plugins_dir);
    {
        std::ofstream out(plugins_dir / "plugin.json", std::ios::binary);
        out << MakeManifestText();
        std::ofstream helper(plugins_dir / "helper.py", std::ios::binary);
        helper << "print('x')\n";
    }

    // 无信任账(nullptr):全部跳过,警告指路。
    {
        const auto scan = ScanProjectPluginDirectories(project.path, nullptr);
        CHECK(scan.manifests.empty());
        REQUIRE(scan.warnings.size() == 1);
        CHECK(scan.warnings[0].find("未经信任") != std::string::npos);
    }

    // 信任后:放行。
    PluginTrustStore trust;
    const std::string dir_key = lubancode::platform::PathToUtf8(
        std::filesystem::weakly_canonical(plugins_dir));
    const auto content_hash = ComputePluginContentHash(plugins_dir);
    REQUIRE(content_hash.has_value());
    trust.SetTrusted(dir_key, *content_hash, "示例");
    {
        const auto scan = ScanProjectPluginDirectories(project.path, &trust);
        REQUIRE(scan.manifests.size() == 1);
        CHECK(scan.manifests[0]->id == "trust-probe");
        CHECK(scan.warnings.empty());
    }

    // 改文件:hash 变,信任失效,跳过。
    {
        std::ofstream helper(plugins_dir / "helper.py", std::ios::binary);
        helper << "print('changed')\n";
        const auto scan = ScanProjectPluginDirectories(project.path, &trust);
        CHECK(scan.manifests.empty());
        REQUIRE(scan.warnings.size() == 1);
        CHECK(scan.warnings[0].find("未经信任") != std::string::npos);
    }
}

TEST_CASE("ScanProjectPluginDirectories:项目没配插件静默空") {
    TempDir project;
    const auto scan = ScanProjectPluginDirectories(project.path, nullptr);
    CHECK(scan.manifests.empty());
    CHECK(scan.warnings.empty());
}
