// 信任流 UI(plugins 单第 8 步收口)的测试:
//   1. 未信任警告指路 /plugin trust <id>(短指纹保留);
//   2. TrustProjectPluginById:概要齐全(id/工具清单/文件数/完整指纹)、
//      落账(含落盘往返)、幂等;
//   3. 信任后重扫挂载、untrust 后再跳过;
//   4. UntrustProjectPluginById:销账 + 幂等;找不到的 id 给人话;
//   5. 命令层(/plugin trust|untrust 子命令)的回执与用法文案。
// 全程临时目录 + 临时账本文件,不碰真机 ~/.lubancode。

#include <doctest/doctest.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

#include "app/commands/workspace_commands.hpp"  // HandlePluginCommand
#include "cli/terminal_port.hpp"                // TermPort 改道捕获回执
#include "config/plugin_trust.hpp"
#include "hooks/hash.hpp"  // DefinitionHashShort(短指纹口径)
#include "platform/paths.hpp"
#include "runtime/plugin_tool.hpp"
#include "app/tool_runtime.hpp"  // PluginMountInfo

using namespace lubancode;
using namespace lubancode::runtime;
using namespace lubancode::config;

namespace {

struct TempDir {
    std::filesystem::path path;
    TempDir() {
        std::error_code ec;
        path = std::filesystem::temp_directory_path() /
              ("lubancode_plugin_trust_ui_" +
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

// 两件工具的 manifest(工具清单要打全,一件验不出列表拼接)。
std::string MakeManifestText() {
    return R"json({
  "manifest_version": 1, "id": "trust-ui-probe", "version": "2.1.0",
  "language": "python",
  "runtime": {"kind": "process", "command": "python3", "args": ["${plugin_dir}/helper.py"]},
  "tools": [
    {"name": "echo", "description": "d", "input_schema": {"type": "object"}},
    {"name": "ping", "description": "d", "input_schema": {"type": "object"}}
  ]
})json";
}

// 造一个项目级插件(plugin.json + 两个脚本文件):文件数有辨识度(3)。
std::filesystem::path MakeProjectWithPlugin(const std::filesystem::path& project) {
    const std::filesystem::path plugin_dir = project / ".lubancode" / "plugins" / "trust-ui-probe";
    std::filesystem::create_directories(plugin_dir);
    {
        std::ofstream out(plugin_dir / "plugin.json", std::ios::binary);
        out << MakeManifestText();
        std::ofstream helper(plugin_dir / "helper.py", std::ios::binary);
        helper << "print('x')\n";
        std::ofstream extra(plugin_dir / "extra.py", std::ios::binary);
        extra << "print('y')\n";
    }
    return plugin_dir;
}

std::string JoinLines(const std::vector<std::string>& lines) {
    std::string text;
    for (const auto& line : lines) {
        text += line + "\n";
    }
    return text;
}

}  // namespace

// ---------------------------------------------------------------------------
// 警告文案:指路 /plugin trust,短指纹保留
// ---------------------------------------------------------------------------

TEST_CASE("未信任警告:指路 /plugin trust <id>,短指纹照旧打出") {
    TempDir project;
    const auto plugin_dir = MakeProjectWithPlugin(project.path);
    const auto content_hash = ComputePluginContentHash(plugin_dir);
    REQUIRE(content_hash.has_value());

    const auto scan = ScanProjectPluginDirectories(project.path, nullptr);
    CHECK(scan.manifests.empty());
    REQUIRE(scan.warnings.size() == 1);
    const std::string& warning = scan.warnings[0];
    CHECK(warning.find("未经信任") != std::string::npos);
    CHECK(warning.find("/plugin trust trust-ui-probe") != std::string::npos);
    CHECK(warning.find(hooks::DefinitionHashShort(*content_hash)) != std::string::npos);
}

// ---------------------------------------------------------------------------
// trust:概要、落账、幂等、重扫挂载
// ---------------------------------------------------------------------------

TEST_CASE("TrustProjectPluginById:概要齐全、落账、幂等;信任后重扫挂载") {
    TempDir project;
    const auto plugin_dir = MakeProjectWithPlugin(project.path);
    const auto content_hash = ComputePluginContentHash(plugin_dir);
    REQUIRE(content_hash.has_value());
    const std::string dir_key =
        platform::PathToUtf8(std::filesystem::weakly_canonical(plugin_dir));

    TempDir store_home;
    const auto store_path = store_home.path / "plugin-trust.json";
    auto [store, load_error] = PluginTrustStore::Load(platform::PathToUtf8(store_path));
    CHECK_FALSE(load_error.has_value());

    // 起手:未信任,重扫跳过。
    CHECK_FALSE(store.IsTrusted(dir_key, *content_hash));

    SUBCASE("概要与落账") {
        const auto report = TrustProjectPluginById(project.path, &store, "trust-ui-probe");
        REQUIRE(report.ok);
        CHECK(report.error.empty());
        const std::string text = JoinLines(report.lines);
        CHECK(text.find("trust-ui-probe") != std::string::npos);
        CHECK(text.find("v2.1.0") != std::string::npos);
        CHECK(text.find("plugin__trust-ui-probe__echo") != std::string::npos);
        CHECK(text.find("plugin__trust-ui-probe__ping") != std::string::npos);
        CHECK(text.find("工具 2 件") != std::string::npos);
        CHECK(text.find("文件 3 个") != std::string::npos);
        CHECK(text.find(*content_hash) != std::string::npos);  // 完整指纹全量打出
        CHECK(text.find("已信任,重启后挂载") != std::string::npos);
        CHECK(store.IsTrusted(dir_key, *content_hash));
    }

    SUBCASE("幂等:重复批不用重批") {
        REQUIRE(TrustProjectPluginById(project.path, &store, "trust-ui-probe").ok);
        const auto again = TrustProjectPluginById(project.path, &store, "trust-ui-probe");
        REQUIRE(again.ok);
        CHECK(JoinLines(again.lines).find("已在信任账上,不用重批") != std::string::npos);
    }

    SUBCASE("落盘往返:重启(重新装载)后账还在") {
        REQUIRE(TrustProjectPluginById(project.path, &store, "trust-ui-probe").ok);
        auto [reloaded, reload_error] = PluginTrustStore::Load(platform::PathToUtf8(store_path));
        CHECK_FALSE(reload_error.has_value());
        CHECK(reloaded.IsTrusted(dir_key, *content_hash));
    }

    SUBCASE("信任后重扫挂载:manifests 进账、警告清空") {
        REQUIRE(TrustProjectPluginById(project.path, &store, "trust-ui-probe").ok);
        const auto scan = ScanProjectPluginDirectories(project.path, &store);
        REQUIRE(scan.manifests.size() == 1);
        CHECK(scan.manifests[0]->id == "trust-ui-probe");
        CHECK(scan.warnings.empty());
    }

    SUBCASE("找不到的 id:人话报错") {
        const auto report = TrustProjectPluginById(project.path, &store, "no-such-plugin");
        CHECK_FALSE(report.ok);
        CHECK(report.lines.empty());
        CHECK(report.error.find("no-such-plugin") != std::string::npos);
    }

    SUBCASE("trust 为空指针:报不可用,不崩") {
        const auto report = TrustProjectPluginById(project.path, nullptr, "trust-ui-probe");
        CHECK_FALSE(report.ok);
        CHECK(report.error.find("信任账不可用") != std::string::npos);
    }
}

// ---------------------------------------------------------------------------
// v2(manifest-backed Lua):/plugin trust 亮 §10.1 权限真账;未信任零执行
// ---------------------------------------------------------------------------

TEST_CASE("v2 Lua 插件:trust 材料亮 entry/网络/Secret 名/资源帽;未信任 chunk 一字不跑") {
    TempDir project;
    const std::filesystem::path plugin_dir =
        project.path / ".lubancode" / "plugins" / "lua-trust-probe";
    std::filesystem::create_directories(plugin_dir);
    {
        std::ofstream out(plugin_dir / "plugin.json", std::ios::binary);
        out << R"json({
  "manifest_version": 2, "id": "lua-trust-probe", "version": "0.3.0",
  "language": "lua",
  "runtime": {"kind": "embedded-lua", "entry": "probe.lua"},
  "permissions": {
    "network": [
      {"scheme": "https", "host": "api.example.com", "port": 443, "methods": ["GET", "POST"]}
    ],
    "secrets": [{"id": "api_key", "env": "LUA_TRUST_PROBE_KEY", "required": false}]
  },
  "limits": {"http_request_bytes": 65536, "http_response_bytes": 262144, "http_timeout_ms": 10000},
  "tools": [{"name": "search", "entry": "search", "description": "d",
             "input_schema": {"type": "object"}}]
})json";
        // 顶层 error():信任门若失守、chunk 跑了一行,加载失败即露馅。
        std::ofstream lua_out(plugin_dir / "probe.lua", std::ios::binary);
        lua_out << "error(\"TOPLEVEL_RAN\")\nreturn { search = function(input) return \"ok\" end }\n";
    }
    const auto content_hash = ComputePluginContentHash(plugin_dir);
    REQUIRE(content_hash.has_value());

    // 未信任:重扫只给警告,manifest 不进账(Lua state 建都不建)。
    {
        const auto scan = ScanProjectPluginDirectories(project.path, nullptr);
        CHECK(scan.manifests.empty());
        REQUIRE(scan.warnings.size() == 1);
        CHECK(scan.warnings[0].find("未经信任") != std::string::npos);
    }

    PluginTrustStore store;  // 纯内存
    const auto report = TrustProjectPluginById(project.path, &store, "lua-trust-probe");
    REQUIRE(report.ok);
    const std::string text = JoinLines(report.lines);
    // §10.1 材料逐样:Lua entry 相对路径、精确 scheme/host/port/method、
    // Secret 逻辑 id 与 env 名(只亮名字)、资源帽、完整指纹。
    CHECK(text.find("Lua entry: probe.lua") != std::string::npos);
    CHECK(text.find("GET https://api.example.com:443") != std::string::npos);
    CHECK(text.find("POST https://api.example.com:443") != std::string::npos);
    CHECK(text.find("api_key <- LUA_TRUST_PROBE_KEY") != std::string::npos);
    CHECK(text.find("optional") != std::string::npos);
    CHECK(text.find("request 64 KiB") != std::string::npos);
    CHECK(text.find("response 256 KiB") != std::string::npos);
    CHECK(text.find("timeout 10 s") != std::string::npos);
    CHECK(text.find(*content_hash) != std::string::npos);
    CHECK(text.find("plugin__lua-trust-probe__search") != std::string::npos);
    // 材料里不该有任何 Secret 值的影子(本来也没有,钉住这条纪律)。
    CHECK(text.find("value") == std::string::npos);
}

// ---------------------------------------------------------------------------
// v2 的 /plugin inspect:六行权限真账(§10.3)与 doctor 探针(§10.4)
// ---------------------------------------------------------------------------

TEST_CASE("HandlePluginCommand:v2 inspect 六行 runtime/entry/profile/network/secrets/limits") {
    TempDir project;
    const std::filesystem::path plugin_dir =
        project.path / ".lubancode" / "plugins" / "lua-inspect-probe";
    std::filesystem::create_directories(plugin_dir);
    {
        std::ofstream out(plugin_dir / "plugin.json", std::ios::binary);
        out << R"json({
  "manifest_version": 2, "id": "lua-inspect-probe", "version": "0.4.0",
  "language": "lua",
  "runtime": {"kind": "embedded-lua", "entry": "probe.lua"},
  "permissions": {
    "network": [
      {"scheme": "https", "host": "api.example.com", "port": 443, "methods": ["POST"]}
    ],
    "secrets": [{"id": "api_key", "env": "LUA_INSPECT_PROBE_KEY", "required": false}]
  },
  "limits": {"http_request_bytes": 65536, "http_response_bytes": 262144, "http_timeout_ms": 10000},
  "tools": [{"name": "search", "entry": "search", "description": "d",
             "input_schema": {"type": "object"}}]
})json";
        std::ofstream lua_out(plugin_dir / "probe.lua", std::ios::binary);
        lua_out << "return { search = function(input) return \"ok\" end }\n";
    }
    // manifests 是挂载后的账(信任门已过):这里直接给解析产物,inspector 只读。
    const std::string manifest_text = [&] {
        std::ifstream in(plugin_dir / "plugin.json", std::ios::binary);
        return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    }();
    auto parsed = ParsePluginManifest(manifest_text, plugin_dir);
    REQUIRE(parsed.has_value());
    std::vector<std::shared_ptr<const PluginManifest>> manifests;
    manifests.push_back(std::make_shared<const PluginManifest>(std::move(*parsed)));

    std::ostringstream out;
    std::ostringstream err;
    lubancode::cli::TermPort().Redirect(&out, &err);
    struct PortGuard {
        ~PortGuard() { lubancode::cli::TermPort().Reset(); }
    } port_guard;

    app::HandlePluginCommand("inspect lua-inspect-probe", {}, manifests, std::string(), nullptr);
    out.flush();
    const std::string text = out.str();
    // §10.3 的六行。
    CHECK(text.find("runtime: embedded-lua") != std::string::npos);
    CHECK(text.find("entry: probe.lua") != std::string::npos);
    CHECK(text.find("profile: pure + host-http") != std::string::npos);
    CHECK(text.find("network: POST https://api.example.com:443") != std::string::npos);
    CHECK(text.find("secrets: api_key <- LUA_INSPECT_PROBE_KEY (optional, missing") != std::string::npos);
    CHECK(text.find("limits: request 64 KiB, response 256 KiB, timeout 10 s") != std::string::npos);
}

// ---------------------------------------------------------------------------
// untrust:销账、再跳过、幂等
// ---------------------------------------------------------------------------

TEST_CASE("UntrustProjectPluginById:销账后重扫再跳过;幂等给人话") {
    TempDir project;
    const auto plugin_dir = MakeProjectWithPlugin(project.path);
    const auto content_hash = ComputePluginContentHash(plugin_dir);
    REQUIRE(content_hash.has_value());
    const std::string dir_key =
        platform::PathToUtf8(std::filesystem::weakly_canonical(plugin_dir));

    PluginTrustStore store;  // 纯内存模式:销账动作不看落盘路径
    REQUIRE(TrustProjectPluginById(project.path, &store, "trust-ui-probe").ok);
    REQUIRE(store.IsTrusted(dir_key, *content_hash));

    SUBCASE("销账后重扫再跳过,警告重新指路") {
        const auto report = UntrustProjectPluginById(project.path, &store, "trust-ui-probe");
        REQUIRE(report.ok);
        const std::string text = JoinLines(report.lines);
        CHECK(text.find(*content_hash) != std::string::npos);
        CHECK(text.find("/plugin trust trust-ui-probe") != std::string::npos);  // 指路重批
        CHECK_FALSE(store.IsTrusted(dir_key, *content_hash));

        const auto scan = ScanProjectPluginDirectories(project.path, &store);
        CHECK(scan.manifests.empty());
        REQUIRE(scan.warnings.size() == 1);
        CHECK(scan.warnings[0].find("未经信任") != std::string::npos);
        CHECK(scan.warnings[0].find("/plugin trust trust-ui-probe") != std::string::npos);
    }

    SUBCASE("不在账上再销:幂等,明说没有可销的账") {
        REQUIRE(UntrustProjectPluginById(project.path, &store, "trust-ui-probe").ok);
        const auto again = UntrustProjectPluginById(project.path, &store, "trust-ui-probe");
        REQUIRE(again.ok);
        CHECK(JoinLines(again.lines).find("不在信任账上") != std::string::npos);
    }

    SUBCASE("找不到的 id:人话报错") {
        const auto report = UntrustProjectPluginById(project.path, &store, "no-such-plugin");
        CHECK_FALSE(report.ok);
        CHECK(report.lines.empty());
        CHECK(report.error.find("no-such-plugin") != std::string::npos);
    }
}

// ---------------------------------------------------------------------------
// 命令层:/plugin trust|untrust 子命令的回执与用法
// ---------------------------------------------------------------------------

TEST_CASE("HandlePluginCommand:trust/untrust 子命令回执照打,用法文案点名信任流") {
    TempDir project;
    MakeProjectWithPlugin(project.path);
    const std::string project_root_utf8 = platform::PathToUtf8(project.path);
    PluginTrustStore store;  // 纯内存:命令层测试只看回执文本

    std::ostringstream out;
    std::ostringstream err;
    lubancode::cli::TermPort().Redirect(&out, &err);
    // 出了本作用域必先收回端口,断言挂了也不漏改道。
    struct PortGuard {
        ~PortGuard() { lubancode::cli::TermPort().Reset(); }
    } port_guard;

    app::HandlePluginCommand("trust trust-ui-probe", {}, {}, project_root_utf8, &store);
    out.flush();
    CHECK(out.str().find("trust-ui-probe") != std::string::npos);
    CHECK(out.str().find("已信任,重启后挂载") != std::string::npos);
    CHECK(out.str().find("工具 2 件") != std::string::npos);

    out.str("");
    app::HandlePluginCommand("untrust trust-ui-probe", {}, {}, project_root_utf8, &store);
    out.flush();
    CHECK(out.str().find("已销信任") != std::string::npos);

    out.str("");
    app::HandlePluginCommand("", {}, {}, project_root_utf8, &store);  // 用法
    out.flush();
    CHECK(out.str().find("/plugin trust <id>") != std::string::npos);
    CHECK(out.str().find("untrust <id>") != std::string::npos);

    out.str("");
    app::HandlePluginCommand("trust", {}, {}, project_root_utf8, &store);  // 缺 id
    out.flush();
    CHECK(out.str().find("用法:/plugin trust <id>") != std::string::npos);

    out.str("");
    app::HandlePluginCommand("trust no-such-plugin", {}, {}, project_root_utf8, &store);
    out.flush();
    CHECK(out.str().find("no-such-plugin") != std::string::npos);

    out.str("");
    app::HandlePluginCommand("trust trust-ui-probe", {}, {}, std::string(), nullptr);
    out.flush();
    CHECK(out.str().find("信任账不可用") != std::string::npos);
}

// ---------------------------------------------------------------------------
// 命令层:/plugin test 子命令(P3-1)。真跑一路要真解释器,冒烟在真机上
// 验;这里钉三张回执:未声明自测入口、查无此插件、legacy 无自测约定。
// ---------------------------------------------------------------------------

TEST_CASE("HandlePluginCommand:test 子命令对未声明自测的插件明说,不装样子") {
    TempDir project;
    MakeProjectWithPlugin(project.path);  // plugin.json + helper.py/extra.py:没有 test_runner.*
    const std::string project_root_utf8 = platform::PathToUtf8(project.path);

    // manifests:不走信任账,直接扫(用户级口径)拿解析好的清单。
    const auto scan = ScanPluginDirectories(project.path / ".lubancode" / "plugins");
    REQUIRE(scan.manifests.size() == 1);
    REQUIRE(scan.manifests[0]->id == "trust-ui-probe");

    std::ostringstream out;
    std::ostringstream err;
    lubancode::cli::TermPort().Redirect(&out, &err);
    struct PortGuard {
        ~PortGuard() { lubancode::cli::TermPort().Reset(); }
    } port_guard;

    app::HandlePluginCommand("test trust-ui-probe", {}, scan.manifests, project_root_utf8, nullptr);
    out.flush();
    CHECK(out.str().find("该插件未声明自测入口") != std::string::npos);
    CHECK(out.str().find("test_runner") != std::string::npos);  // 指路约定名

    out.str("");
    app::HandlePluginCommand("test no-such-plugin", {}, scan.manifests, project_root_utf8, nullptr);
    out.flush();
    CHECK(out.str().find("no-such-plugin") != std::string::npos);

    // legacy Lua 挂载账:没有自测约定,也明说。
    out.str("");
    const std::vector<app::PluginMountInfo> mounted = {
        {"plugin__legacy-probe__word_count", "lua"},
    };
    app::HandlePluginCommand("test legacy-probe", mounted, scan.manifests, project_root_utf8, nullptr);
    out.flush();
    CHECK(out.str().find("没有自测约定") != std::string::npos);
}
