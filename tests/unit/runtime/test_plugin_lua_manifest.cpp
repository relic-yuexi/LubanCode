// ManifestLuaRuntime 的单测(Lua 受控 HTTP 与 Secret 宿主能力单·阶段 4)。
// 对号设计单 §13.5 的机制半边与阶段 4 验收线:
//   - LoadManifestLuaPlugin:happy path(state/entries/resolver/limits 配齐)、
//     handler 对账接通到挂载路径(缺 handler 整件拒挂)、entry 盘面变坏即拒;
//   - 加载期零副作用:顶层 chunk 调 Host API,注入的假 transport/resolver
//     计数仍为 0(§九,阶段 3 在机制层钉过,这里钉"经 owner 挂载"同一条);
//   - adapter:模型可见面只有 manifest 三样(name/description/schema),
//     execute 走动态作用域(假 HTTPS 一笔真工具调用冒烟,§13.5),
//     Secret 原文只进最终发包头、不进模型结果;入参先过 schema 门;
//   - wire 名:standalone 用 manifest 本地名,packaged 用 %HH 编码段;
//   - DoctorProbeManifestLua:编译与对账的只读探针。
// 全程假件(FakeHttpTransport/CountingResolver),不碰公网;假 Key 一律
// FAKE_ 前缀。

#include <doctest/doctest.h>

#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "runtime/plugin_contract.hpp"
#include "runtime/plugin_lua_manifest.hpp"

using namespace lubancode;
using namespace lubancode::runtime;
namespace fs = std::filesystem;

namespace {

class TempDir {
public:
    TempDir() {
        dir_ = fs::temp_directory_path() /
               ("lubancode_lua_manifest_" + std::to_string(reinterpret_cast<std::uintptr_t>(this)));
        std::error_code ec;
        fs::remove_all(dir_, ec);
        fs::create_directories(dir_, ec);
    }
    ~TempDir() {
        std::error_code ec;
        fs::remove_all(dir_, ec);
    }
    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;
    const fs::path& Get() const { return dir_; }

private:
    fs::path dir_;
};

void WriteFile(const fs::path& path, const std::string& content) {
    fs::create_directories(path.parent_path());
    std::ofstream file(path, std::ios::binary);
    file << content;
}

// 假 resolver:计数 Resolve/Describe;按 id 发 FAKE_ 前缀假值(§13.2 的
// 泄露扫描拿它当靶子)。
class CountingResolver final : public SecretResolver {
public:
    std::map<std::string, std::string> values;

    int resolve_count = 0;
    int describe_count = 0;

    std::expected<SecretValue, SecretResolveError> Resolve(const SecretDeclaration& declaration) override {
        ++resolve_count;
        const auto it = values.find(declaration.id);
        if (it == values.end()) {
            if (declaration.required) {
                SecretResolveError error;
                error.issue = SecretResolveIssue::Missing;
                error.message = "必需的 Secret 没找到: " + declaration.id;
                return std::unexpected(error);
            }
            return SecretValue(std::string());
        }
        return SecretValue(std::string(it->second));
    }

    SecretStatus Describe(const SecretDeclaration& declaration) override {
        ++describe_count;
        SecretStatus status;
        status.id = declaration.id;
        status.env = declaration.env;
        status.required = declaration.required;
        status.available = values.find(declaration.id) != values.end();
        status.source = status.available ? SecretSource::HostEnv : SecretSource::None;
        return status;
    }
};

// 一份 v2 manifest 文本(permissions.network + secrets + limits 全给)。
std::string MakeV2ManifestText() {
    return R"json({
  "manifest_version": 2,
  "id": "demo-lua",
  "version": "0.1.0",
  "language": "lua",
  "runtime": {"kind": "embedded-lua", "entry": "demo.lua"},
  "permissions": {
    "network": [
      {"scheme": "https", "host": "api.example.com", "port": 443, "methods": ["GET", "POST"]}
    ],
    "secrets": [
      {"id": "api_key", "env": "DEMO_API_KEY", "required": false}
    ]
  },
  "limits": {
    "http_request_bytes": 65536,
    "http_response_bytes": 262144,
    "http_timeout_ms": 10000
  },
  "tools": [
    {
      "name": "search",
      "entry": "search",
      "description": "Search the demo vertical.",
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

// 建一只 v2 插件目录(plugin.json + demo.lua),manifest 解析好递回。
std::shared_ptr<const PluginManifest> MakePluginDir(const fs::path& dir, const std::string& lua_script,
                                                    const std::string& manifest_text = MakeV2ManifestText()) {
    WriteFile(dir / "plugin.json", manifest_text);
    WriteFile(dir / "demo.lua", lua_script);
    auto parsed = ParsePluginManifest(manifest_text, dir);
    REQUIRE(parsed.has_value());
    return std::make_shared<const PluginManifest>(std::move(*parsed));
}

}  // namespace

// ---------------------------------------------------------------------------
// LoadManifestLuaPlugin
// ---------------------------------------------------------------------------

TEST_CASE("LoadManifestLuaPlugin:happy path,state/entries/limits/resolver/transport 配齐") {
    TempDir temp;
    const auto manifest = MakePluginDir(temp.Get() / "demo-lua", "return { search = function(input) return 'ok' end }\n");

    ManifestLuaLoadOptions options;
    options.plugin_data_dir = temp.Get() / "plugin-data";
    auto plugin = LoadManifestLuaPlugin(manifest, std::move(options));
    REQUIRE(plugin.has_value());
    CHECK((*plugin)->state != nullptr);
    REQUIRE((*plugin)->state->entries().size() == 1);
    CHECK((*plugin)->state->entries()[0] == "search");
    CHECK((*plugin)->resolver != nullptr);
    CHECK((*plugin)->transport != nullptr);
    // limits 已按 manifest 下调(64 KiB / 256 KiB / 10s)。
    CHECK((*plugin)->limits.request_body_bytes == 65536);
    CHECK((*plugin)->limits.response_body_bytes == 262144);
    CHECK((*plugin)->limits.timeout_ms == 10000);
    // standalone:wire 名用 manifest 本地名。
    CHECK((*plugin)->ToolWireName("search") == "plugin__demo-lua__search");
}

TEST_CASE("LoadManifestLuaPlugin:handler 对账接通到挂载路径——缺 handler 整件拒挂") {
    TempDir temp;
    // manifest 要 search,脚本只给别的:阶段 3 的 loader 验,经挂载路径同样拒。
    const auto manifest =
        MakePluginDir(temp.Get() / "demo-lua", "return { other = function(input) return 'ok' end }\n");
    auto plugin = LoadManifestLuaPlugin(manifest, ManifestLuaLoadOptions{});
    REQUIRE_FALSE(plugin.has_value());
    CHECK(plugin.error().find("search") != std::string::npos);
    CHECK(plugin.error().find("handler") != std::string::npos);
}

TEST_CASE("LoadManifestLuaPlugin:解析后盘面变坏(entry 被删)即拒,不带半个 state") {
    TempDir temp;
    const fs::path dir = temp.Get() / "demo-lua";
    const auto manifest = MakePluginDir(dir, "return { search = function(input) return 'ok' end }\n");
    fs::remove(dir / "demo.lua");
    auto plugin = LoadManifestLuaPlugin(manifest, ManifestLuaLoadOptions{});
    REQUIRE_FALSE(plugin.has_value());
    CHECK(plugin.error().find("runtime.entry") != std::string::npos);
}

TEST_CASE("LoadManifestLuaPlugin:非 v2 embedded-lua 的 manifest 明拒") {
    TempDir temp;
    const std::string v1 = R"json({
  "manifest_version": 1,
  "id": "v1proc",
  "version": "1.0.0",
  "language": "python",
  "runtime": {"kind": "process", "command": "python", "args": ["${plugin_dir}/runner.py"]},
  "tools": [{"name": "count", "description": "数词", "input_schema": {"type": "object"}}]
})json";
    WriteFile(temp.Get() / "v1proc" / "runner.py", "print('{}')\n");
    WriteFile(temp.Get() / "v1proc" / "plugin.json", v1);
    auto parsed = ParsePluginManifest(v1, temp.Get() / "v1proc");
    REQUIRE(parsed.has_value());
    auto plugin = LoadManifestLuaPlugin(
        std::make_shared<const PluginManifest>(std::move(*parsed)), ManifestLuaLoadOptions{});
    REQUIRE_FALSE(plugin.has_value());
    CHECK(plugin.error().find("v2 embedded-lua") != std::string::npos);
}

TEST_CASE("加载期零副作用:顶层调 Host API,注入的假件计数为 0") {
    TempDir temp;
    // 恶意顶层:调 HTTP、取 Secret、硬翻 error。顶层只有 no_active_tool_call,
    // 脚本自己吃掉 err 照常返回 handler 表——加载成,假件一根毛没碰。
    const std::string script = R"lua(
local response, err = luban.http.request({method = "GET", url = "https://api.example.com/v1"})
assert(err ~= nil and err.code == "no_active_tool_call")
local ok, available = pcall(function() return luban.secrets.available("api_key") end)
assert(ok and available == nil)
return { search = function(input) return "mounted" end }
)lua";
    const auto manifest = MakePluginDir(temp.Get() / "demo-lua", script);

    auto transport = std::make_unique<FakeHttpTransport>();
    auto resolver = std::make_unique<CountingResolver>();
    resolver->values["api_key"] = "FAKE_SECRET_VALUE_XYZ";
    FakeHttpTransport* transport_ptr = transport.get();
    CountingResolver* resolver_ptr = resolver.get();

    ManifestLuaLoadOptions options;
    options.transport = std::move(transport);
    options.resolver = std::move(resolver);
    auto plugin = LoadManifestLuaPlugin(manifest, std::move(options));
    REQUIRE(plugin.has_value());
    CHECK(transport_ptr->call_count() == 0);
    CHECK(resolver_ptr->resolve_count == 0);
    CHECK(resolver_ptr->describe_count == 0);
}

// ---------------------------------------------------------------------------
// adapter:模型可见面 + execute 动态作用域
// ---------------------------------------------------------------------------

TEST_CASE("adapter:模型可见面只有 manifest 三样,确认与延迟挂载齐") {
    TempDir temp;
    const auto manifest = MakePluginDir(temp.Get() / "demo-lua",
                                        "return { search = function(input) return 'ok' end }\n");
    ManifestLuaRuntime runtime;
    const std::vector<std::string> no_warnings = runtime.LoadFromManifests({manifest});
    CHECK(no_warnings.empty());
    REQUIRE(runtime.plugins().size() == 1);

    auto adapters = runtime.MakeAdapters();
    REQUIRE(adapters.size() == 1);
    CHECK(adapters[0]->name() == "plugin__demo-lua__search");
    CHECK(adapters[0]->description() == "Search the demo vertical.");
    CHECK(adapters[0]->input_schema() == manifest->tools[0].input_schema);
    CHECK(adapters[0]->needs_confirm());
    CHECK(adapters[0]->deferred());
}

TEST_CASE("adapter:execute 走假 HTTPS 一笔,Secret 只进最终发包头不进结果") {
    TempDir temp;
    const std::string script = R"lua(
return {
  search = function(input)
    local response, err = luban.http.request({
      method = "POST",
      url = "https://api.example.com/v1/search",
      headers = {["Content-Type"] = "application/json"},
      json = input,
      auth = {type = "bearer", secret = "api_key", optional = true},
    })
    if err ~= nil then
      error(err.code .. ": " .. err.message)
    end
    return response.body
  end,
}
)lua";
    const auto manifest = MakePluginDir(temp.Get() / "demo-lua", script);

    auto transport = std::make_unique<FakeHttpTransport>();
    HttpExchangeResponse response;
    response.status = 200;
    response.headers = {{"content-type", "application/json"}};
    response.body = "{\"hits\": 2}";
    transport->EnqueueResponse(std::move(response));

    auto resolver = std::make_unique<CountingResolver>();
    resolver->values["api_key"] = "FAKE_BEARER_TOKEN_9";
    FakeHttpTransport* transport_ptr = transport.get();

    ManifestLuaRuntime runtime;
    // 直接造件(不经 LoadFromManifests 的 standalone 数据目录路):注入口
    // 就是给这条测试留的。
    ManifestLuaLoadOptions options;
    options.plugin_data_dir = std::nullopt;
    options.transport = std::move(transport);
    options.resolver = std::move(resolver);
    auto loaded = LoadManifestLuaPlugin(manifest, std::move(options));
    REQUIRE(loaded.has_value());
    ManifestLuaPlugin* adopted = runtime.Adopt(std::move(*loaded));
    REQUIRE(adopted != nullptr);

    ManifestLuaToolAdapter adapter(adopted, &manifest->tools[0]);
    const tools::Tool::Result result = adapter.execute({{"query", "hello"}});
    CHECK_FALSE(result.is_error);
    CHECK(result.content == "{\"hits\": 2}");
    CHECK(result.outcome == "succeeded");
    CHECK(result.effect_summary.find("embedded-lua") != std::string::npos);

    // 假 transport 亲见:Bearer 只落在最终发包头;请求体是宿主序列化的 JSON。
    REQUIRE(transport_ptr->call_count() == 1);
    const auto& call = transport_ptr->calls()[0];
    bool saw_bearer = false;
    for (const auto& [name, value] : call.request.headers) {
        if (name == "Authorization") {
            saw_bearer = true;
            CHECK(value == "Bearer FAKE_BEARER_TOKEN_9");
        }
    }
    CHECK(saw_bearer);
    CHECK(call.request.method == "POST");
    CHECK(call.request.body.find("\"query\":\"hello\"") != std::string::npos);
    // Secret 原文不进模型结果(§13.2 泄露扫描)。
    CHECK(result.content.find("FAKE_") == std::string::npos);
}

TEST_CASE("adapter:入参先过 manifest schema 门,坏参不进 Lua") {
    TempDir temp;
    // handler 的返回值是 "ran":schema 拒的文案点名参数问题,与它判然可分
    //——坏参根本没进 Lua。
    const auto manifest = MakePluginDir(temp.Get() / "demo-lua",
                                        "return { search = function(input) return 'ran' end }\n");
    ManifestLuaRuntime runtime;
    CHECK(runtime.LoadFromManifests({manifest}).empty());
    REQUIRE(runtime.plugins().size() == 1);
    auto adapters = runtime.MakeAdapters();
    REQUIRE(adapters.size() == 1);

    // 缺 required 的 query。
    const tools::Tool::Result bad = adapters[0]->execute(nlohmann::json::object());
    CHECK(bad.is_error);
    CHECK(bad.content.find("query") != std::string::npos);
    CHECK(bad.content != "ran");
}

TEST_CASE("packaged 身份:wire 名走 %HH 编码段,ToolWireName 与编码对得上") {
    TempDir temp;
    const auto manifest = MakePluginDir(temp.Get() / "demo-lua",
                                        "return { search = function(input) return 'ok' end }\n");
    ManifestLuaLoadOptions options;
    options.package_id = "moontide.demo-stack";
    options.local_id = "demo-lua";
    options.package_version = "0.2.0";
    auto plugin = LoadManifestLuaPlugin(manifest, std::move(options));
    REQUIRE(plugin.has_value());
    CHECK((*plugin)->package_id == "moontide.demo-stack");
    CHECK((*plugin)->ToolWireName("search") ==
          BuildPackagedToolWireName("plugin", "moontide.demo-stack", "demo-lua", "search"));
    CHECK((*plugin)->ToolWireName("search") == "plugin__moontide%2Edemo-stack%2Edemo-lua__search");
}

TEST_CASE("ManifestLuaRuntime:幂等挂载,单件坏警告跳过不连累其余") {
    TempDir temp;
    const auto good = MakePluginDir(temp.Get() / "good-lua",
                                    "return { search = function(input) return 'ok' end }\n");
    // 坏件:manifest 要 search,脚本缺 handler。
    const auto bad = MakePluginDir(temp.Get() / "bad-lua",
                                   "return { other = function(input) return 'ok' end }\n");

    ManifestLuaRuntime runtime;
    const std::vector<std::string> warnings = runtime.LoadFromManifests({good, bad});
    REQUIRE(runtime.plugins().size() == 1);
    REQUIRE(warnings.size() == 1);
    // 两只 manifest 同 id(demo-lua),坏的按 id 点名。
    CHECK(warnings[0].find("demo-lua") != std::string::npos);
    CHECK(warnings[0].find("handler") != std::string::npos);
    // 第二遍(给另一张 registry)不重挂。
    CHECK(runtime.LoadFromManifests({good, bad}).empty());
    CHECK(runtime.plugins().size() == 1);
    CHECK(runtime.MakeAdapters().size() == 1);
}

TEST_CASE("DoctorProbeManifestLua:好件过,坏件带人话;探针不碰盘以外的东西") {
    TempDir temp;
    const auto manifest = MakePluginDir(temp.Get() / "demo-lua",
                                        "return { search = function(input) return 'ok' end }\n");
    CHECK_FALSE(DoctorProbeManifestLua(*manifest).has_value());

    // 编译坏:syntax error 当场报。
    const fs::path broken_dir = temp.Get() / "broken-lua";
    const auto broken = MakePluginDir(broken_dir, "return { search = function( }\n");
    const auto problem = DoctorProbeManifestLua(*broken);
    REQUIRE(problem.has_value());
    CHECK(problem->find("编译失败") != std::string::npos);

    // entry 被删:探针按人话拒。
    fs::remove(broken_dir / "demo.lua");
    const auto missing = DoctorProbeManifestLua(*broken);
    REQUIRE(missing.has_value());
    CHECK(missing->find("runtime.entry") != std::string::npos);
}
