// manifest v2(Lua 受控 HTTP 与 Secret 宿主能力单·阶段 0)的合同单测:
// v1 兼容路、v1 embedded-lua 明报升级、v2 的 network/secrets/limits 强校
// 验、entry 越界/symlink/缺文件/非 .lua、错误码与行列。
//
// 章法:全部纯函数直测(ParsePluginManifestDetailed),不碰进程不碰 Lua
// 不接网——网络执法是阶段 2,Lua 挂载是阶段 4。

#include <doctest/doctest.h>

#include <filesystem>
#include <fstream>
#include <string>

#include "runtime/plugin_contract.hpp"

using namespace lubancode;
using namespace lubancode::runtime;

namespace {

// 临时插件目录(每 CASE 各建各的,收尾自删;Windows 文件柄规矩:先关流
// 再删,remove_all 走 error_code 形态)。
struct TempDir {
    std::filesystem::path path;
    TempDir() {
        std::error_code ec;
        path = std::filesystem::temp_directory_path() /
              ("lubancode_manifest_v2_" + std::to_string(++counter_));
        std::filesystem::remove_all(path, ec);
        std::filesystem::create_directories(path, ec);
    }
    ~TempDir() {
        std::error_code ec;
        std::filesystem::remove_all(path, ec);
    }

  private:
    static int counter_;
};
int TempDir::counter_ = 0;

void WriteFile(const std::filesystem::path& path, const std::string& text) {
    std::ofstream(path, std::ios::binary) << text;
}

// 一份合法 v2 manifest 的样板(§5.2 示例的收口版);各 CASE 掰弯一处。
// entry 文件由调用方先落盘。
std::string GoodV2Manifest() {
    return R"json({
  "manifest_version": 2,
  "id": "anysearch",
  "version": "0.1.0",
  "language": "lua",
  "runtime": {"kind": "embedded-lua", "entry": "anysearch.lua"},
  "permissions": {
    "network": [
      {"scheme": "https", "host": "api.anysearch.com", "port": 443, "methods": ["GET", "POST"]}
    ],
    "secrets": [
      {"id": "api_key", "env": "ANYSEARCH_API_KEY", "required": false}
    ]
  },
  "limits": {
    "http_timeout_ms": 20000,
    "http_request_bytes": 1048576,
    "http_response_bytes": 4194304
  },
  "tools": [
    {
      "name": "search",
      "entry": "search",
      "description": "Search the web.",
      "input_schema": {
        "type": "object",
        "properties": {"query": {"type": "string"}},
        "required": ["query"],
        "additionalProperties": false
      }
    }
  ]
})json";
}

// 掰弯 JSON 里的一处文本(朴素替换;测试样板里各串唯一)。
std::string Bend(std::string manifest, const std::string& from, const std::string& to) {
    const std::size_t hit = manifest.find(from);
    REQUIRE(hit != std::string::npos);
    manifest.replace(hit, from.size(), to);
    return manifest;
}

}  // namespace

// ---------------------------------------------------------------------------
// 兼容策略(§5.1)
// ---------------------------------------------------------------------------

TEST_CASE("v1 process 照旧解析:v1 样板的字段一概不动") {
    TempDir dir;
    const std::string v1 = R"json({
  "manifest_version": 1, "id": "local-math", "version": "1.0.0", "language": "python",
  "runtime": {"kind": "process", "command": "python3", "timeout_ms": 30000},
  "tools": [{"name": "add", "input_schema": {"type": "object"}}],
  "permissions": {"network": true, "env": ["HOME"]}
})json";
    auto manifest = ParsePluginManifestDetailed(v1, dir.path);
    REQUIRE(manifest.has_value());
    CHECK(manifest->manifest_version == 1);
    CHECK(manifest->kind == RuntimeKind::Process);
    CHECK(manifest->network_allowed == true);
    REQUIRE(manifest->env_allowlist.size() == 1);
    CHECK(manifest->env_allowlist[0] == "HOME");
    CHECK(manifest->network_permissions.empty());
    CHECK(manifest->secret_declarations.empty());
    CHECK_FALSE(manifest->http_limits.timeout_ms.has_value());
}

TEST_CASE("v1 写 embedded-lua 明报需 v2,不拿半份合同猜") {
    TempDir dir;
    const std::string v1_lua = R"json({
  "manifest_version": 1, "id": "lua-probe", "version": "1.0.0",
  "runtime": {"kind": "embedded-lua"},
  "tools": [{"name": "echo", "input_schema": {"type": "object"}}]
})json";
    auto manifest = ParsePluginManifestDetailed(v1_lua, dir.path);
    REQUIRE_FALSE(manifest.has_value());
    CHECK(manifest.error().code == PluginManifestIssueCode::EmbeddedLuaNeedsV2);
    CHECK(PluginManifestIssueCodeName(manifest.error().code) == "embedded_lua_needs_v2");
    CHECK(manifest.error().Format().find("manifest_version 2") != std::string::npos);
}

TEST_CASE("v2 只收 embedded-lua:process 写 v2 即拒") {
    TempDir dir;
    const std::string v2_process = R"json({
  "manifest_version": 2, "id": "a", "version": "1",
  "runtime": {"kind": "process", "command": "x"},
  "tools": [{"name": "t", "input_schema": {"type": "object"}}]
})json";
    auto manifest = ParsePluginManifestDetailed(v2_process, dir.path);
    REQUIRE_FALSE(manifest.has_value());
    CHECK(manifest.error().code == PluginManifestIssueCode::V2KindUnsupported);
}

TEST_CASE("版本墙:不认得的版本整件拒绝且码稳定") {
    TempDir dir;
    const std::string v3 = R"json({
  "manifest_version": 3, "id": "a", "version": "1",
  "runtime": {"kind": "embedded-lua", "entry": "a.lua"},
  "tools": [{"name": "t", "input_schema": {"type": "object"}}]
})json";
    auto manifest = ParsePluginManifestDetailed(v3, dir.path);
    REQUIRE_FALSE(manifest.has_value());
    CHECK(manifest.error().code == PluginManifestIssueCode::VersionUnsupported);
    CHECK(PluginManifestIssueCodeName(manifest.error().code) == "version_unsupported");
}

// ---------------------------------------------------------------------------
// v2 全字段解析(§5.2 示例按字面收)
// ---------------------------------------------------------------------------

TEST_CASE("v2 样板全字段解析:network/secrets/limits 各归其位") {
    TempDir dir;
    WriteFile(dir.path / "anysearch.lua", "return {}\n");
    auto manifest = ParsePluginManifestDetailed(GoodV2Manifest(), dir.path);
    REQUIRE(manifest.has_value());
    CHECK(manifest->manifest_version == 2);
    CHECK(manifest->kind == RuntimeKind::EmbeddedLua);
    CHECK(manifest->runtime_entry == "anysearch.lua");
    REQUIRE(manifest->network_permissions.size() == 1);
    CHECK(manifest->network_permissions[0].scheme == "https");
    CHECK(manifest->network_permissions[0].host == "api.anysearch.com");
    CHECK(manifest->network_permissions[0].port == 443);
    REQUIRE(manifest->network_permissions[0].methods.size() == 2);
    CHECK(manifest->network_permissions[0].methods[0] == "GET");
    CHECK(manifest->network_permissions[0].methods[1] == "POST");
    REQUIRE(manifest->secret_declarations.size() == 1);
    CHECK(manifest->secret_declarations[0].id == "api_key");
    CHECK(manifest->secret_declarations[0].env == "ANYSEARCH_API_KEY");
    CHECK(manifest->secret_declarations[0].required == false);
    // 示例的三枚字段名照字面收(§5.2 的名字就是合同)。
    CHECK(manifest->http_limits.request_body_bytes.value() == 1048576);
    CHECK(manifest->http_limits.response_body_bytes.value() == 4194304);
    CHECK(manifest->http_limits.timeout_ms.value() == 20000);
    // 未声明的帽走缺省表(§5.3)。
    const auto effective = ApplyHttpLimits(manifest->http_limits);
    CHECK(effective.url_bytes == kHttpUrlDefaultBytes);
    CHECK(effective.request_header_bytes == kHttpRequestHeaderDefaultBytes);
    CHECK(effective.response_header_bytes == kHttpResponseHeaderDefaultBytes);
    CHECK(effective.request_body_bytes == 1048576);
    CHECK(effective.response_body_bytes == 4194304);
    CHECK(effective.timeout_ms == 20000);
}

TEST_CASE("v2 缺省与硬帽的表(§5.3):六顶帽缺省值与硬帽各就各位") {
    CHECK(kHttpUrlDefaultBytes == 8192);
    CHECK(kHttpUrlMaxBytes == 16384);
    CHECK(kHttpRequestHeaderDefaultBytes == 32768);
    CHECK(kHttpRequestHeaderMaxBytes == 65536);
    CHECK(kHttpRequestBodyDefaultBytes == 1048576);
    CHECK(kHttpRequestBodyMaxBytes == 8388608);
    CHECK(kHttpResponseHeaderDefaultBytes == 65536);
    CHECK(kHttpResponseHeaderMaxBytes == 131072);
    CHECK(kHttpResponseBodyDefaultBytes == 4194304);
    CHECK(kHttpResponseBodyMaxBytes == 16777216);
    CHECK(kHttpTimeoutDefaultMs == 30000);
    CHECK(kHttpTimeoutMaxMs == 120000);
    // 全缺省:ApplyHttpLimits 给缺省表。
    const auto effective = ApplyHttpLimits(HttpLimits{});
    CHECK(effective.url_bytes == 8192);
    CHECK(effective.request_header_bytes == 32768);
    CHECK(effective.request_body_bytes == 1048576);
    CHECK(effective.response_header_bytes == 65536);
    CHECK(effective.response_body_bytes == 4194304);
    CHECK(effective.timeout_ms == 30000);
}

// ---------------------------------------------------------------------------
// entry 规矩(§5.3)
// ---------------------------------------------------------------------------

TEST_CASE("v2 entry:缺文件/非 .lua/绝对路径/上跳段/占位符全拒") {
    TempDir dir;
    // 缺文件(不写 anysearch.lua)。
    auto missing = ParsePluginManifestDetailed(GoodV2Manifest(), dir.path);
    REQUIRE_FALSE(missing.has_value());
    CHECK(missing.error().code == PluginManifestIssueCode::EntryInvalid);

    // 非 .lua。
    WriteFile(dir.path / "anysearch.txt", "return {}\n");
    auto wrong_ext = ParsePluginManifestDetailed(Bend(GoodV2Manifest(), "anysearch.lua", "anysearch.txt"), dir.path);
    REQUIRE_FALSE(wrong_ext.has_value());
    CHECK(wrong_ext.error().code == PluginManifestIssueCode::EntryInvalid);

    // 绝对路径。
    auto absolute = ParsePluginManifestDetailed(Bend(GoodV2Manifest(), "anysearch.lua", "/etc/passwd"), dir.path);
    REQUIRE_FALSE(absolute.has_value());
    CHECK(absolute.error().code == PluginManifestIssueCode::EntryInvalid);

    // 上跳段。
    auto escape = ParsePluginManifestDetailed(Bend(GoodV2Manifest(), "anysearch.lua", "../outside.lua"), dir.path);
    REQUIRE_FALSE(escape.has_value());
    CHECK(escape.error().code == PluginManifestIssueCode::EntryInvalid);

    // 占位符。
    auto placeholder =
        ParsePluginManifestDetailed(Bend(GoodV2Manifest(), "anysearch.lua", "${plugin_dir}/anysearch.lua"), dir.path);
    REQUIRE_FALSE(placeholder.has_value());
    CHECK(placeholder.error().code == PluginManifestIssueCode::EntryInvalid);

    // 子目录里的合法 .lua 收。
    std::error_code ec;
    std::filesystem::create_directories(dir.path / "src", ec);
    WriteFile(dir.path / "src" / "anysearch.lua", "return {}\n");
    auto nested = ParsePluginManifestDetailed(Bend(GoodV2Manifest(), "anysearch.lua", "src/anysearch.lua"), dir.path);
    REQUIRE(nested.has_value());
    CHECK(nested->runtime_entry == "src/anysearch.lua");
}

TEST_CASE("v2 entry 是 symlink 明拒(建不成 symlink 的机器走缺文件路)") {
    TempDir dir;
    std::error_code ec;
    std::filesystem::create_directories(dir.path / "elsewhere", ec);
    WriteFile(dir.path / "elsewhere" / "real.lua", "return {}\n");
    std::filesystem::create_symlink(dir.path / "elsewhere" / "real.lua", dir.path / "anysearch.lua", ec);
    auto manifest = ParsePluginManifestDetailed(GoodV2Manifest(), dir.path);
    REQUIRE_FALSE(manifest.has_value());
    CHECK(manifest.error().code == PluginManifestIssueCode::EntryInvalid);
    // 能建 symlink 的平台(POSIX/开发者模式 Windows)错误文案点名 symlink;
    // 建不成的机器文件不存在,同样 EntryInvalid——两条路都拒,断言码即可。
    if (!ec) {
        CHECK(manifest.error().Format().find("symlink") != std::string::npos);
    }
}

TEST_CASE("v2 runtime 段不收 process 的字(command/args/timeout_ms)") {
    TempDir dir;
    WriteFile(dir.path / "anysearch.lua", "return {}\n");
    auto with_command =
        ParsePluginManifestDetailed(Bend(GoodV2Manifest(), "\"entry\": \"anysearch.lua\"",
                                          "\"entry\": \"anysearch.lua\", \"command\": \"python3\""),
                                    dir.path);
    REQUIRE_FALSE(with_command.has_value());
    CHECK(with_command.error().code == PluginManifestIssueCode::FieldInvalid);
}

// ---------------------------------------------------------------------------
// network 规矩(§5.3)
// ---------------------------------------------------------------------------

TEST_CASE("v2 network:通配符/IP/http/非 443 端口/未知 method 全拒") {
    TempDir dir;
    WriteFile(dir.path / "anysearch.lua", "return {}\n");
    const auto reject = [&](const std::string& from, const std::string& to, PluginManifestIssueCode code) {
        auto manifest = ParsePluginManifestDetailed(Bend(GoodV2Manifest(), from, to), dir.path);
        REQUIRE_FALSE(manifest.has_value());
        CHECK(manifest.error().code == code);
        return manifest.error();
    };
    // 通配符。
    reject("\"host\": \"api.anysearch.com\"", "\"host\": \"*.anysearch.com\"", PluginManifestIssueCode::HostInvalid);
    reject("\"host\": \"api.anysearch.com\"", "\"host\": \"*\"", PluginManifestIssueCode::HostInvalid);
    // IP 字面量。
    reject("\"host\": \"api.anysearch.com\"", "\"host\": \"93.184.216.34\"", PluginManifestIssueCode::HostInvalid);
    // http 明文不做。
    auto http = reject("\"scheme\": \"https\"", "\"scheme\": \"http\"", PluginManifestIssueCode::FieldInvalid);
    CHECK(http.Format().find("只收 https") != std::string::npos);
    // 443 外端口。
    reject("\"port\": 443", "\"port\": 8443", PluginManifestIssueCode::FieldInvalid);
    // 未知 method。
    reject("\"methods\": [\"GET\", \"POST\"]", "\"methods\": [\"GET\", \"DELETE\"]",
           PluginManifestIssueCode::FieldInvalid);
    // methods 空。
    reject("\"methods\": [\"GET\", \"POST\"]", "\"methods\": []", PluginManifestIssueCode::FieldMissing);
    // host 缺失。
    reject("\"host\": \"api.anysearch.com\", ", "", PluginManifestIssueCode::FieldMissing);
}

TEST_CASE("v2 network:大小写/去重/缺省 scheme+port/尾点规范化") {
    TempDir dir;
    WriteFile(dir.path / "anysearch.lua", "return {}\n");
    // scheme 大写、host 大写带尾点、methods 小写重复——全部规范化收账。
    std::string manifest_json = Bend(GoodV2Manifest(),
                                     "{\"scheme\": \"https\", \"host\": \"api.anysearch.com\", \"port\": 443, "
                                     "\"methods\": [\"GET\", \"POST\"]}",
                                     "{\"scheme\": \"HTTPS\", \"host\": \"API.AnySearch.com.\", \"methods\": "
                                     "[\"post\", \"get\", \"POST\"]}");
    auto manifest = ParsePluginManifestDetailed(manifest_json, dir.path);
    REQUIRE(manifest.has_value());
    REQUIRE(manifest->network_permissions.size() == 1);
    CHECK(manifest->network_permissions[0].scheme == "https");
    CHECK(manifest->network_permissions[0].host == "api.anysearch.com");
    CHECK(manifest->network_permissions[0].port == 443);  // 缺省 443
    REQUIRE(manifest->network_permissions[0].methods.size() == 2);
    CHECK(manifest->network_permissions[0].methods[0] == "POST");  // 声明序保留,重复的并掉
    CHECK(manifest->network_permissions[0].methods[1] == "GET");
}

TEST_CASE("v2 network:同目的地重复声明拒") {
    TempDir dir;
    WriteFile(dir.path / "anysearch.lua", "return {}\n");
    std::string manifest_json = Bend(GoodV2Manifest(),
                                     "{\"scheme\": \"https\", \"host\": \"api.anysearch.com\", \"port\": 443, "
                                     "\"methods\": [\"GET\", \"POST\"]}",
                                     "{\"host\": \"api.anysearch.com\", \"methods\": [\"GET\"]}, "
                                     "{\"host\": \"api.anysearch.com\", \"port\": 443, \"methods\": [\"POST\"]}");
    auto manifest = ParsePluginManifestDetailed(manifest_json, dir.path);
    REQUIRE_FALSE(manifest.has_value());
    CHECK(manifest.error().code == PluginManifestIssueCode::DuplicateEntry);
}

TEST_CASE("v2 未声明 network/permissions:空账合法(HTTP 执法规矩在阶段 2/3)") {
    TempDir dir;
    WriteFile(dir.path / "anysearch.lua", "return {}\n");
    auto stripped = ParsePluginManifestDetailed(Bend(GoodV2Manifest(), R"(  "permissions": {
    "network": [
      {"scheme": "https", "host": "api.anysearch.com", "port": 443, "methods": ["GET", "POST"]}
    ],
    "secrets": [
      {"id": "api_key", "env": "ANYSEARCH_API_KEY", "required": false}
    ]
  },
)", ""),
                                                dir.path);
    REQUIRE(stripped.has_value());
    CHECK(stripped->network_permissions.empty());
    CHECK(stripped->secret_declarations.empty());
}

// ---------------------------------------------------------------------------
// secrets 规矩(§5.3)
// ---------------------------------------------------------------------------

TEST_CASE("v2 secrets:inline value/default/env 展开全拒") {
    TempDir dir;
    WriteFile(dir.path / "anysearch.lua", "return {}\n");
    const auto reject = [&](const std::string& from, const std::string& to) {
        auto manifest = ParsePluginManifestDetailed(Bend(GoodV2Manifest(), from, to), dir.path);
        REQUIRE_FALSE(manifest.has_value());
        CHECK(manifest.error().code == PluginManifestIssueCode::SecretInvalid);
        return manifest.error();
    };
    auto with_value = reject("{\"id\": \"api_key\", \"env\": \"ANYSEARCH_API_KEY\", \"required\": false}",
                             "{\"id\": \"api_key\", \"env\": \"ANYSEARCH_API_KEY\", \"value\": \"FAKE-secret\"}");
    CHECK(with_value.Format().find("value") != std::string::npos);
    reject("{\"id\": \"api_key\", \"env\": \"ANYSEARCH_API_KEY\", \"required\": false}",
           "{\"id\": \"api_key\", \"env\": \"ANYSEARCH_API_KEY\", \"default\": \"FAKE-secret\"}");
    reject("{\"id\": \"api_key\", \"env\": \"ANYSEARCH_API_KEY\", \"required\": false}",
           "{\"id\": \"api_key\", \"env\": \"${ANYSEARCH_API_KEY}\"}");
}

TEST_CASE("v2 secrets:id/env 重名与坏字符全拒;required 缺省 true") {
    TempDir dir;
    WriteFile(dir.path / "anysearch.lua", "return {}\n");
    // env 名坏字符(数字开头)。
    auto bad_env = ParsePluginManifestDetailed(
        Bend(GoodV2Manifest(), "\"env\": \"ANYSEARCH_API_KEY\"", "\"env\": \"1BAD_NAME\""), dir.path);
    REQUIRE_FALSE(bad_env.has_value());
    CHECK(bad_env.error().code == PluginManifestIssueCode::SecretInvalid);
    // id 坏字符。
    auto bad_id =
        ParsePluginManifestDetailed(Bend(GoodV2Manifest(), "\"id\": \"api_key\"", "\"id\": \"api;key\""), dir.path);
    REQUIRE_FALSE(bad_id.has_value());
    CHECK(bad_id.error().code == PluginManifestIssueCode::SecretInvalid);
    // id 重复。
    std::string dup_id = Bend(GoodV2Manifest(), "{\"id\": \"api_key\", \"env\": \"ANYSEARCH_API_KEY\", \"required\": false}",
                              "{\"id\": \"api_key\", \"env\": \"A_KEY\"}, {\"id\": \"api_key\", \"env\": \"B_KEY\"}");
    auto dup = ParsePluginManifestDetailed(dup_id, dir.path);
    REQUIRE_FALSE(dup.has_value());
    CHECK(dup.error().code == PluginManifestIssueCode::DuplicateEntry);
    // env 重复(一个变量只供一个逻辑 id)。
    std::string dup_env = Bend(GoodV2Manifest(), "{\"id\": \"api_key\", \"env\": \"ANYSEARCH_API_KEY\", \"required\": false}",
                               "{\"id\": \"a\", \"env\": \"ANYSEARCH_API_KEY\"}, {\"id\": \"b\", \"env\": \"ANYSEARCH_API_KEY\"}");
    auto dup2 = ParsePluginManifestDetailed(dup_env, dir.path);
    REQUIRE_FALSE(dup2.has_value());
    CHECK(dup2.error().code == PluginManifestIssueCode::DuplicateEntry);
    // required 缺省 = true(漏写按必需,作者显式写 false 才可退匿名)。
    auto no_required = ParsePluginManifestDetailed(
        Bend(GoodV2Manifest(), "{\"id\": \"api_key\", \"env\": \"ANYSEARCH_API_KEY\", \"required\": false}",
             "{\"id\": \"api_key\", \"env\": \"ANYSEARCH_API_KEY\"}"),
        dir.path);
    REQUIRE(no_required.has_value());
    REQUIRE(no_required->secret_declarations.size() == 1);
    CHECK(no_required->secret_declarations[0].required == true);
}

TEST_CASE("v2 permissions 不收 env allowlist(Secret 走声明,不递子进程)") {
    TempDir dir;
    WriteFile(dir.path / "anysearch.lua", "return {}\n");
    auto with_env = ParsePluginManifestDetailed(
        Bend(GoodV2Manifest(), "\"secrets\": [", "\"env\": [\"HOME\"],\n      \"secrets\": ["), dir.path);
    REQUIRE_FALSE(with_env.has_value());
    CHECK(with_env.error().code == PluginManifestIssueCode::FieldInvalid);
    CHECK(with_env.error().Format().find("env allowlist") != std::string::npos);
}

// ---------------------------------------------------------------------------
// limits 规矩(§5.3)
// ---------------------------------------------------------------------------

TEST_CASE("v2 limits:0/负数/越硬帽全拒;恰好等于硬帽收") {
    TempDir dir;
    WriteFile(dir.path / "anysearch.lua", "return {}\n");
    const auto reject = [&](const std::string& from, const std::string& to) {
        auto manifest = ParsePluginManifestDetailed(Bend(GoodV2Manifest(), from, to), dir.path);
        REQUIRE_FALSE(manifest.has_value());
        CHECK(manifest.error().code == PluginManifestIssueCode::LimitInvalid);
    };
    reject("\"http_timeout_ms\": 20000", "\"http_timeout_ms\": 0");
    reject("\"http_timeout_ms\": 20000", "\"http_timeout_ms\": -1");
    reject("\"http_timeout_ms\": 20000", "\"http_timeout_ms\": 120001");
    reject("\"http_request_bytes\": 1048576", "\"http_request_bytes\": 8388609");
    reject("\"http_response_bytes\": 4194304", "\"http_response_bytes\": 16777217");
    // 六顶帽全可声明,且恰好等于硬帽是合法的"不下调"。
    std::string all_limits = Bend(GoodV2Manifest(), R"("limits": {
    "http_timeout_ms": 20000,
    "http_request_bytes": 1048576,
    "http_response_bytes": 4194304
  },)",
                                  R"("limits": {
    "http_url_bytes": 16384,
    "http_request_header_bytes": 65536,
    "http_request_bytes": 8388608,
    "http_response_header_bytes": 131072,
    "http_response_bytes": 16777216,
    "http_timeout_ms": 120000
  },)");
    auto manifest = ParsePluginManifestDetailed(all_limits, dir.path);
    REQUIRE(manifest.has_value());
    const auto effective = ApplyHttpLimits(manifest->http_limits);
    CHECK(effective.url_bytes == 16384);
    CHECK(effective.request_header_bytes == 65536);
    CHECK(effective.request_body_bytes == 8388608);
    CHECK(effective.response_header_bytes == 131072);
    CHECK(effective.response_body_bytes == 16777216);
    CHECK(effective.timeout_ms == 120000);
}

TEST_CASE("v2 limits:非整数(浮点/字符串)拒") {
    TempDir dir;
    WriteFile(dir.path / "anysearch.lua", "return {}\n");
    auto bad = ParsePluginManifestDetailed(Bend(GoodV2Manifest(), "\"http_timeout_ms\": 20000",
                                                "\"http_timeout_ms\": 20.5"),
                                           dir.path);
    REQUIRE_FALSE(bad.has_value());
    CHECK(bad.error().code == PluginManifestIssueCode::LimitInvalid);
}

// ---------------------------------------------------------------------------
// 错误码与行列
// ---------------------------------------------------------------------------

TEST_CASE("语法错的行列精确;字段错的码稳定(best-effort 坐标)") {
    TempDir dir;
    WriteFile(dir.path / "anysearch.lua", "return {}\n");
    // 语法错:第 2 行第 3 列(缺逗号处由解析器定位;断言行号即可,列随库
    // 版本可能有 0/1 基差,断 >=1)。
    const std::string broken = "{\n  \"manifest_version\" 2\n}";
    auto syntax = ParsePluginManifestDetailed(broken, dir.path);
    REQUIRE_FALSE(syntax.has_value());
    CHECK(syntax.error().code == PluginManifestIssueCode::JsonSyntax);
    CHECK(syntax.error().line == 2);
    CHECK(syntax.error().line > 0);
    CHECK(syntax.error().Format().find("[json_syntax]") != std::string::npos);

    // 字段错:码稳定;坐标 best-effort(键名在原文找得到就给 >=1 的坐标)。
    auto bad_host = ParsePluginManifestDetailed(
        Bend(GoodV2Manifest(), "\"host\": \"api.anysearch.com\"", "\"host\": \"*.anysearch.com\""), dir.path);
    REQUIRE_FALSE(bad_host.has_value());
    CHECK(bad_host.error().code == PluginManifestIssueCode::HostInvalid);
    CHECK(bad_host.error().line >= 1);
    CHECK(bad_host.error().column >= 1);
    CHECK(bad_host.error().Format().find("[host_invalid]") != std::string::npos);
}

// ---------------------------------------------------------------------------
// DNS host 规范化(NormalizeDnsHost 纯函数)
// ---------------------------------------------------------------------------

TEST_CASE("NormalizeDnsHost:小写/去尾点/punycode;禁 userinfo") {
    const auto lower = NormalizeDnsHost("API.Example.COM");
    REQUIRE(lower.has_value());
    CHECK(*lower == "api.example.com");
    const auto trailing_dot = NormalizeDnsHost("api.example.com.");
    REQUIRE(trailing_dot.has_value());
    CHECK(*trailing_dot == "api.example.com");
    // 中文域名:punycode(xn--)编码后记账。
    const auto idn = NormalizeDnsHost("例子.测试");
    REQUIRE(idn.has_value());
    CHECK(idn->rfind("xn--", 0) == 0);
    CHECK(idn->find(".xn--") != std::string::npos);
    // RFC 3492 §7.1 的标准向量:bücher -> xn--bcher-kva(混合 ASCII 与
    // 非 ASCII 的标签)。
    const auto buecher = NormalizeDnsHost("bücher.example.com");
    REQUIRE(buecher.has_value());
    CHECK(*buecher == "xn--bcher-kva.example.com");
    // userinfo。
    CHECK_FALSE(NormalizeDnsHost("user@api.example.com").has_value());
    CHECK_FALSE(NormalizeDnsHost("").has_value());
}

TEST_CASE("NormalizeDnsHost:单标签/localhost/.local/IP/坏标签全拒") {
    CHECK_FALSE(NormalizeDnsHost("localhost").has_value());
    CHECK_FALSE(NormalizeDnsHost("intranet").has_value());
    CHECK_FALSE(NormalizeDnsHost("printer.local").has_value());
    CHECK_FALSE(NormalizeDnsHost("10.0.0.1").has_value());
    CHECK_FALSE(NormalizeDnsHost("::1").has_value());
    CHECK_FALSE(NormalizeDnsHost("[2001:db8::1]").has_value());
    CHECK_FALSE(NormalizeDnsHost("api..example.com").has_value());   // 空标签
    CHECK_FALSE(NormalizeDnsHost("-bad.example.com").has_value());   // 连字符开头
    CHECK_FALSE(NormalizeDnsHost("bad-.example.com").has_value());   // 连字符收尾
    CHECK_FALSE(NormalizeDnsHost("under_score.example.com").has_value());  // 下划线不是 DNS 名
}
