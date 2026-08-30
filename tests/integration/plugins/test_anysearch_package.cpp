// AnySearch 参考包的集成测试(Lua 受控 HTTP 与 Secret 宿主能力单·阶段 5)。
// 对号设计单 §十二与阶段 5 清单:
//   - manifest 静态账:四件工具、精确网络账、optional Secret、三顶帽
//     (真读 examples/packages/anysearch 的交付物,不是复印件);
//   - 包盘点:package.yaml 过、code-bearing、plugins/skills/docs 组件齐,
//     包内无真 .env;
//   - 假 AnySearch server 回归:本机回环假 HTTP 服务 + 假 DNS 把
//     api.anysearch.com 指 127.0.0.1(受控 HTTP 拒 IP 字面量 host,故走
//     DNS seam;照阶段 2 的 FakeDnsResolver 先例)。manifest 原文不动,
//     只把 anysearch.lua 里的 API_BASE 常量改指测试端口。覆盖:
//       auth 头只在 keyed 案出现(匿名无 Authorization);
//       401/429/5xx/坏 JSON/响应超帽各分型对号;
//       取消:在途取消落 cancelled,batch 余笔不再发出;
//       batch 串行次序与共享参数;
//       Key 铁律(§12.2):auto_registered.api_key 丢弃——值不进模型
//       结果,只留非敏感提示。
//
// 不烧真网:全部走 127.0.0.1 假服务;假 Key 一律 FAKE_ 前缀。匿名真网
// 一笔属 §13.5 的验收账,不进 ctest(网络不可用的机器上硬跑即假红)。

#include <doctest/doctest.h>

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "fake_http_server.hpp"
#include "net/http_transport.hpp"
#include "package/inventory.hpp"
#include "package/manifest.hpp"
#include "runtime/plugin_contract.hpp"
#include "runtime/plugin_http.hpp"
#include "runtime/plugin_lua_manifest.hpp"
#include "runtime/secret_resolver.hpp"

using namespace lubancode;
using namespace lubancode::runtime;
namespace fs = std::filesystem;

namespace {

constexpr const char* kApiHost = "api.anysearch.com";

// 示例目录:从源码树真读——示例改坏(工具改名、manifest 坏了、Lua 挪位)
// 当场红,文档与代码不两张皮。
fs::path PackageDir() { return fs::path(LUBANCODE_TEST_SOURCE_DIR) / "examples" / "packages" / "anysearch"; }
fs::path PluginDir() { return PackageDir() / "plugins" / "anysearch"; }

std::string ReadFileText(const fs::path& path) {
    std::ifstream file(path, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
}

void WriteFileText(const fs::path& path, const std::string& content) {
    fs::create_directories(path.parent_path());
    std::ofstream file(path, std::ios::binary);
    file << content;
}

// 临时目录(收尾自删;Windows 文件柄规矩:流先关再删)。
class TempDir {
public:
    TempDir() {
        std::error_code ec;
        dir_ = fs::temp_directory_path() /
               ("lubancode_anysearch_pkg_" + std::to_string(reinterpret_cast<std::uintptr_t>(this)));
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

// 假 DNS(阶段 2 先例):api.anysearch.com -> 127.0.0.1。递什么名都指
// 本机,manifest 的 host 名一个字节不用改。
class FakeDns final : public net::DnsResolver {
public:
    std::expected<std::vector<std::string>, std::string> Resolve(const std::string& host) override {
        ++calls;
        last_host = host;
        return addresses;
    }

    std::vector<std::string> addresses{"127.0.0.1"};
    int calls = 0;
    std::string last_host;
};

// 假 SecretResolver:id -> FAKE_ 前缀假值;空表 = 匿名(optional 缺失)。
class FakeSecretResolver final : public SecretResolver {
public:
    std::map<std::string, std::string> values;
    int resolve_count = 0;

    std::expected<SecretValue, SecretResolveError> Resolve(const SecretDeclaration& declaration) override {
        ++resolve_count;
        const auto it = values.find(declaration.id);
        if (it == values.end()) {
            return SecretValue(std::string());  // optional 缺失:匿名降级
        }
        return SecretValue(std::string(it->second));
    }

    SecretStatus Describe(const SecretDeclaration& declaration) override {
        SecretStatus status;
        status.id = declaration.id;
        status.env = declaration.env;
        status.required = declaration.required;
        status.available = values.count(declaration.id) > 0;
        status.source = status.available ? SecretSource::HostEnv : SecretSource::None;
        return status;
    }
};

// 200 + 标准 AnySearch 响应壳(code/message/request_id/data)。
test_support::FakeHttpResponse MakeApiResponse(const std::string& data_json, const std::string& extra_top_level = "") {
    test_support::FakeHttpResponse response;
    response.status = 200;
    response.headers.emplace_back("Content-Type", "application/json");
    response.body = "{\"code\":0,\"message\":\"success\",\"request_id\":\"req-fake\"," + extra_top_level +
                    "\"data\":" + data_json + "}";
    return response;
}

test_support::FakeHttpResponse MakeErrorPage(int status, const std::string& message) {
    test_support::FakeHttpResponse response;
    response.status = status;
    response.headers.emplace_back("Content-Type", "application/json");
    response.body = "{\"code\":-1,\"message\":\"" + message + "\"}";
    return response;
}

// ---------------------------------------------------------------------------
// 假 server 场景的一只插件:真 manifest + 真 Lua(只改 API_BASE 指本机测
// 试端口),真 CprBoundedHttpTransport(假 DNS + loopback 测试口)。
// ---------------------------------------------------------------------------
class AnysearchOnFakeServer {
public:
    explicit AnysearchOnFakeServer(std::map<std::string, std::string> secret_values) {
        // 1) 拷真 manifest(原文一字不动)+ 真 Lua(只换 API_BASE 常量)。
        const std::string manifest_text = ReadFileText(PluginDir() / "plugin.json");
        std::string script = ReadFileText(PluginDir() / "anysearch.lua");
        const std::string original_base = "local API_BASE = \"https://api.anysearch.com\"";
        REQUIRE(script.find(original_base) != std::string::npos);
        const std::string fake_base =
            "local API_BASE = \"http://api.anysearch.com:" + std::to_string(server_.port()) + "\"";
        script.replace(script.find(original_base), original_base.size(), fake_base);
        WriteFileText(temp_.Get() / "anysearch" / "plugin.json", manifest_text);
        WriteFileText(temp_.Get() / "anysearch" / "anysearch.lua", script);

        // 2) 真 manifest 解析(交付物校验与挂载用同一份)。
        auto parsed = ParsePluginManifest(manifest_text, temp_.Get() / "anysearch");
        REQUIRE(parsed.has_value());
        manifest_ = std::make_shared<const PluginManifest>(std::move(*parsed));

        // 3) 真传输 + 假 DNS + loopback 测试口(生产路径不开这个口)。
        CprBoundedHttpTransport::Options options;
        NetworkPermission permission;
        permission.scheme = "http";
        permission.host = kApiHost;
        permission.port = server_.port();
        permission.methods = {"GET", "POST"};
        options.permissions.push_back(permission);
        options.dns = &dns_;
        options.allow_loopback_targets = true;
        transport_ = std::make_unique<CprBoundedHttpTransport>(std::move(options));

        // 4) 挂载:注入 transport/resolver(测试口),standalone 身份。
        auto resolver = std::make_unique<FakeSecretResolver>();
        resolver->values = std::move(secret_values);
        resolver_ = resolver.get();
        ManifestLuaLoadOptions load_options;
        load_options.transport = std::move(transport_);
        load_options.resolver = std::move(resolver);
        auto loaded = LoadManifestLuaPlugin(manifest_, std::move(load_options));
        REQUIRE(loaded.has_value());
        plugin_ = std::move(*loaded);

        // 工具名 -> adapter 查找表(manifest 是唯一账本)。
        for (const auto& definition : manifest_->tools) {
            tools_.emplace(definition.name, std::make_unique<ManifestLuaToolAdapter>(plugin_.get(), &definition));
        }
        REQUIRE(tools_.size() == 4);
    }

    tools::Tool::Result Run(const std::string& tool, const nlohmann::json& input) { return Tool(tool)->execute(input); }

    tools::Tool::Result Run(const std::string& tool, const nlohmann::json& input,
                            const tools::ToolExecutionContext& context) {
        return Tool(tool)->execute(input, context);
    }

    tools::Tool* Tool(const std::string& name) {
        const auto it = tools_.find(name);
        REQUIRE(it != tools_.end());
        return it->second.get();
    }

    test_support::FakeHttpServer server_;
    FakeDns dns_;
    FakeSecretResolver* resolver_ = nullptr;
    std::shared_ptr<const PluginManifest> manifest_;

private:
    TempDir temp_;
    std::unique_ptr<CprBoundedHttpTransport> transport_;
    std::unique_ptr<ManifestLuaPlugin> plugin_;
    std::map<std::string, std::unique_ptr<ManifestLuaToolAdapter>> tools_;
};

// 假服务的请求头查名(名字已小写化)。
std::optional<std::string> HeaderOf(const test_support::FakeHttpRequest& request, const std::string& name) {
    for (const auto& [header_name, value] : request.headers) {
        if (header_name == name) {
            return value;
        }
    }
    return std::nullopt;
}

}  // namespace

// ---------------------------------------------------------------------------
// manifest 与包静态账(真交付物)
// ---------------------------------------------------------------------------

TEST_CASE("manifest 静态账:四件工具、精确网络、optional Secret、三顶帽") {
    const std::string text = ReadFileText(PluginDir() / "plugin.json");
    auto manifest = ParsePluginManifest(text, PluginDir());
    REQUIRE(manifest.has_value());
    CHECK(manifest->manifest_version == kPluginManifestVersionV2);
    CHECK(manifest->kind == RuntimeKind::EmbeddedLua);
    CHECK(manifest->id == "anysearch");
    CHECK(manifest->runtime_entry == "anysearch.lua");

    // 四件工具对号 §12.1:名字、entry、HTTP 动词的面(get=GET,余=POST)。
    REQUIRE(manifest->tools.size() == 4);
    const std::vector<std::string> expected = {"get_sub_domains", "search", "batch_search", "extract"};
    for (std::size_t i = 0; i < expected.size(); ++i) {
        CHECK(manifest->tools[i].name == expected[i]);
        CHECK(manifest->tools[i].entry == expected[i]);
        CHECK_FALSE(manifest->tools[i].description.empty());
        CHECK(manifest->tools[i].input_schema.is_object());
    }
    CHECK(manifest->tools[0].input_schema["required"] == nlohmann::json::array({"domains"}));
    CHECK(manifest->tools[1].input_schema["required"] == nlohmann::json::array({"query"}));
    CHECK(manifest->tools[2].input_schema["required"] == nlohmann::json::array({"queries"}));
    CHECK(manifest->tools[3].input_schema["required"] == nlohmann::json::array({"url"}));
    // batch 的 queries 帽:1-5 笔。
    CHECK(manifest->tools[2].input_schema["properties"]["queries"]["maxItems"] == 5);

    // 网络账:一条精确声明,https/443/GET+POST,host 一个字节不差。
    REQUIRE(manifest->network_permissions.size() == 1);
    CHECK(manifest->network_permissions[0].scheme == "https");
    CHECK(manifest->network_permissions[0].host == kApiHost);
    CHECK(manifest->network_permissions[0].port == 443);
    REQUIRE(manifest->network_permissions[0].methods.size() == 2);
    CHECK(manifest->network_permissions[0].methods[0] == "GET");
    CHECK(manifest->network_permissions[0].methods[1] == "POST");

    // Secret:api_key <- ANYSEARCH_API_KEY,optional(§12.2)。
    REQUIRE(manifest->secret_declarations.size() == 1);
    CHECK(manifest->secret_declarations[0].id == "api_key");
    CHECK(manifest->secret_declarations[0].env == "ANYSEARCH_API_KEY");
    CHECK_FALSE(manifest->secret_declarations[0].required);

    // 帽照 §5.2 示例:20s / 1 MiB / 4 MiB(只许下调的三顶)。
    CHECK(manifest->http_limits.timeout_ms.value() == 20000);
    CHECK(manifest->http_limits.request_body_bytes.value() == 1048576);
    CHECK(manifest->http_limits.response_body_bytes.value() == 4194304);

    // doctor 探针(§10.4):编译与 handler 对账过,只读零网络。
    auto parsed_detailed = ParsePluginManifest(text, PluginDir());
    REQUIRE(parsed_detailed.has_value());
    CHECK_FALSE(DoctorProbeManifestLua(*parsed_detailed).has_value());
}

TEST_CASE("包盘点:package.yaml 过、code-bearing、组件齐、包内无真 .env") {
    const auto package_manifest = package::ParsePackageManifest(ReadFileText(PackageDir() / "package.yaml"));
    REQUIRE(package_manifest.has_value());
    CHECK(package_manifest->id == "luban.anysearch");
    CHECK(package_manifest->schema == 1);

    package::ScanOptions options;
    options.current_platform = "windows";
    package::PackageCandidate candidate;
    candidate.scope = package::PackageScope::Dev;
    candidate.layer_root = PackageDir().parent_path();
    candidate.package_root = PackageDir();
    candidate.dir_name = "anysearch";
    candidate.manifest = *package_manifest;
    const package::PackageInventory inventory = package::BuildPackageInventory(candidate, options);

    CHECK(inventory.manifest_ok);
    CHECK(inventory.valid);
    CHECK(inventory.code_bearing());
    REQUIRE(inventory.plugins.size() == 1);
    CHECK(inventory.plugins[0].local_id == "anysearch");
    CHECK(inventory.plugins[0].canonical_id == "luban.anysearch:anysearch");
    REQUIRE(inventory.skills.size() == 1);
    CHECK(inventory.skills[0].local_id == "anysearch");
    CHECK(inventory.docs_file_count >= 1);  // docs/.env.example

    // Skill 只写策略:frontmatter 在,正文不携带 CLI 行(§12.1 策略分账)。
    const std::string skill = ReadFileText(PackageDir() / "skills" / "anysearch" / "SKILL.md");
    CHECK(skill.find("name: anysearch") != std::string::npos);
    CHECK(skill.find("description:") != std::string::npos);
    CHECK(skill.find("anysearch_cli") == std::string::npos);
    CHECK(skill.find("powershell") == std::string::npos);
    CHECK(skill.find("python3 ") == std::string::npos);

    // docs/.env.example:占位样板,无真实凭据。
    const std::string env_example = ReadFileText(PackageDir() / "docs" / ".env.example");
    CHECK(env_example.find("ANYSEARCH_API_KEY=") != std::string::npos);
    CHECK(env_example.find("<your_api_key_here>") != std::string::npos);

    // 真钥匙不住包内(§7.2):包根与插件目录都不得有 .env。
    CHECK_FALSE(fs::exists(PackageDir() / ".env"));
    CHECK_FALSE(fs::exists(PluginDir() / ".env"));
}

// ---------------------------------------------------------------------------
// 假 server 回归:正常路
// ---------------------------------------------------------------------------

TEST_CASE("假服务:search 匿名一笔——POST 落点、JSON 体、无 Authorization") {
    AnysearchOnFakeServer harness({});
    harness.server_.Enqueue(
        MakeApiResponse("{\"results\":[{\"title\":\"LubanCode\",\"url\":\"https://example.com/luban\"}],"
                        "\"metadata\":{\"total_results\":1}}"));

    const tools::Tool::Result result = harness.Run("search", {{"query", "lubancode"}});
    CAPTURE(result.content);
    CHECK_FALSE(result.is_error);
    CHECK(result.outcome == "succeeded");
    CHECK(result.content.find("LubanCode") != std::string::npos);
    CHECK(result.content.find("\"status\":200") != std::string::npos);
    CHECK(result.content.find("req-fake") != std::string::npos);

    const std::vector<test_support::FakeHttpRequest> received = harness.server_.requests();
    REQUIRE(received.size() == 1);
    const auto& sent = received[0];
    CHECK(sent.method == "POST");
    CHECK(sent.target == "/v1/search");
    CHECK(sent.body.find("\"query\":\"lubancode\"") != std::string::npos);
    CHECK(HeaderOf(sent, "content-type").value_or("") == "application/json");
    CHECK_FALSE(HeaderOf(sent, "authorization").has_value());  // 匿名:无 auth 头
    CHECK(harness.dns_.last_host == kApiHost);                 // 走假 DNS,不碰真网
    CHECK(harness.resolver_->resolve_count == 1);              // optional 仍解析过(空值降级)

    // schema 门:缺 required 的 query,坏参不进 Lua。
    const tools::Tool::Result bad = harness.Run("search", nlohmann::json::object());
    CHECK(bad.is_error);
    CHECK(bad.content.find("query") != std::string::npos);
    REQUIRE(harness.server_.requests().size() == 1);  // 一个包都没多发
}

TEST_CASE("假服务:search keyed 一笔——Bearer 只在最终发包头") {
    AnysearchOnFakeServer harness({{"api_key", "FAKE_ANYSEARCH_BEARER_1"}});
    harness.server_.Enqueue(MakeApiResponse("{\"results\":[],\"metadata\":{}}"));

    const tools::Tool::Result result = harness.Run("search", {{"query", "secret probe"}});
    CHECK_FALSE(result.is_error);

    REQUIRE(harness.server_.requests().size() == 1);
    CHECK(HeaderOf(harness.server_.requests()[0], "authorization").value_or("") ==
          "Bearer FAKE_ANYSEARCH_BEARER_1");
    // Secret 原文不进模型结果(§13.2 泄露扫描)。
    CHECK(result.content.find("FAKE_") == std::string::npos);
}

TEST_CASE("假服务:get_sub_domains 走 GET,domains 逐个进 query") {
    AnysearchOnFakeServer harness({});
    harness.server_.Enqueue(
        MakeApiResponse("{\"domains\":[{\"domain\":\"finance\",\"sub_domains\":[{\"sub_domain\":\"finance.news\"}]}]}"));

    const tools::Tool::Result result =
        harness.Run("get_sub_domains", {{"domains", nlohmann::json::array({"finance", "health"})}});
    CAPTURE(result.content);
    CHECK_FALSE(result.is_error);
    CHECK(result.content.find("finance.news") != std::string::npos);

    const std::vector<test_support::FakeHttpRequest> received = harness.server_.requests();
    REQUIRE(received.size() == 1);
    const auto& sent = received[0];
    CHECK(sent.method == "GET");
    CHECK(sent.target == "/v1/sub-domains?domain=finance&domain=health");
    CHECK(sent.body.empty());  // GET 不带体
}

TEST_CASE("假服务:extract 落 POST /v1/extract,data 照回") {
    AnysearchOnFakeServer harness({});
    harness.server_.Enqueue(
        MakeApiResponse("{\"url\":\"https://example.com/\",\"title\":\"Example Domain\",\"content\":\"# Example\"}"));

    const tools::Tool::Result result = harness.Run("extract", {{"url", "https://example.com/"}});
    CAPTURE(result.content);
    CHECK_FALSE(result.is_error);
    CHECK(result.content.find("Example Domain") != std::string::npos);

    const std::vector<test_support::FakeHttpRequest> received = harness.server_.requests();
    REQUIRE(received.size() == 1);
    const auto& sent = received[0];
    CHECK(sent.method == "POST");
    CHECK(sent.target == "/v1/extract");
    CHECK(sent.body.find("https://example.com/") != std::string::npos);
}

// ---------------------------------------------------------------------------
// 假 server 回归:错误分型对号
// ---------------------------------------------------------------------------

TEST_CASE("假服务:401/429/5xx 保留 status,人话分型") {
    AnysearchOnFakeServer harness({});
    const std::vector<std::pair<int, std::string>> cases = {
        {401, "未授权"}, {429, "限流"}, {500, "服务端错误"}};
    for (const auto& [status, word] : cases) {
        harness.server_.Enqueue(MakeErrorPage(status, "server said no"));
        const tools::Tool::Result result = harness.Run("search", {{"query", "probe"}});
        CAPTURE(result.content);
        CHECK_FALSE(result.is_error);  // HTTP 非 2xx 不冒充网络错(§11)
        CHECK(result.content.find("\"status\":" + std::to_string(status)) != std::string::npos);
        CHECK(result.content.find("http_" + std::to_string(status)) != std::string::npos);
        CHECK(result.content.find(word) != std::string::npos);
        CHECK(result.content.find("server said no") != std::string::npos);  // 服务端话带回
    }
    REQUIRE(harness.server_.requests().size() == 3);
}

TEST_CASE("假服务:坏 JSON 与响应超帽各归各位") {
    AnysearchOnFakeServer harness({});
    {
        test_support::FakeHttpResponse bad_json;
        bad_json.status = 200;
        bad_json.headers.emplace_back("Content-Type", "application/json");
        bad_json.body = "{\"results\": oops}";
        harness.server_.Enqueue(bad_json);
        const tools::Tool::Result result = harness.Run("search", {{"query", "x"}});
        CAPTURE(result.content);
        CHECK_FALSE(result.is_error);
        CHECK(result.content.find("invalid_json") != std::string::npos);
    }
    {
        test_support::FakeHttpResponse huge;
        huge.status = 200;
        huge.headers.emplace_back("Content-Type", "application/json");
        huge.body = std::string(4 * 1024 * 1024 + 1, 'z');  // 帽是 manifest 的 4 MiB
        harness.server_.Enqueue(huge);
        const tools::Tool::Result result = harness.Run("search", {{"query", "x"}});
        CAPTURE(result.content);
        CHECK_FALSE(result.is_error);
        CHECK(result.content.find("response_too_large") != std::string::npos);
    }
    REQUIRE(harness.server_.requests().size() == 2);
}

TEST_CASE("假服务:在途取消落 cancelled,batch 余笔不再发出") {
    AnysearchOnFakeServer harness({});
    // 第一笔装死 5 秒,取消在 250ms 落锤;若 batch 是并行,三笔会同时上
    // 门,服务端就收到 3 个请求——串行 + 取消停手,只该见到 1 个。
    test_support::FakeHttpResponse stall = MakeApiResponse("{\"results\":[]}");
    stall.delay_before_response = std::chrono::seconds(5);
    harness.server_.Enqueue(stall);
    harness.server_.Enqueue(MakeApiResponse("{\"results\":[]}"));
    harness.server_.Enqueue(MakeApiResponse("{\"results\":[]}"));

    std::atomic<bool> cancel{false};
    std::thread canceller([&cancel] {
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
        cancel.store(true);
    });
    tools::ToolExecutionContext context;
    context.cancel = &cancel;
    const tools::Tool::Result result = harness.Run(
        "batch_search",
        nlohmann::json::parse(R"json({"queries":[{"query":"a"},{"query":"b"},{"query":"c"}]})json"),
        context);
    canceller.join();
    CAPTURE(result.content);
    CHECK_FALSE(result.is_error);
    CHECK(result.content.find("cancelled") != std::string::npos);
    CHECK(result.content.find("skipped_after_cancel") != std::string::npos);

    REQUIRE(harness.server_.requests().size() == 1);  // 只发出第一笔
    CHECK(harness.server_.requests()[0].body.find("\"query\":\"a\"") != std::string::npos);
}

// ---------------------------------------------------------------------------
// 假 server 回归:batch 串行次序 + Key 铁律
// ---------------------------------------------------------------------------

TEST_CASE("假服务:batch 三笔按序,共享参数逐笔生效") {
    AnysearchOnFakeServer harness({});
    harness.server_.Enqueue(MakeApiResponse("{\"marker\":\"first\"}"));
    harness.server_.Enqueue(MakeApiResponse("{\"marker\":\"second\"}"));
    harness.server_.Enqueue(MakeApiResponse("{\"marker\":\"third\"}"));

    const tools::Tool::Result result = harness.Run(
        "batch_search",
        nlohmann::json::parse(
            R"json({"queries":[{"query":"alpha"},{"query":"beta"},{"query":"gamma"}],"max_results":3})json"));
    CAPTURE(result.content);
    CHECK_FALSE(result.is_error);
    CHECK(result.content.find("cancelled") == std::string::npos);

    // 次序:服务端收到的三笔按 alpha/beta/gamma,回执 marker 同序。
    REQUIRE(harness.server_.requests().size() == 3);
    const std::vector<std::string> sent_queries = {"alpha", "beta", "gamma"};
    const std::vector<std::string> markers = {"first", "second", "third"};
    std::size_t cursor = 0;
    for (std::size_t i = 0; i < 3; ++i) {
        CHECK(harness.server_.requests()[i].body.find("\"query\":\"" + sent_queries[i] + "\"") != std::string::npos);
        CHECK(harness.server_.requests()[i].body.find("\"max_results\":3") != std::string::npos);  // 共享参数
        const auto at = result.content.find(markers[i], cursor);
        REQUIRE(at != std::string::npos);
        cursor = at;  // 下一枚 marker 不得出现在前一枚之前
    }
}

TEST_CASE("Key 铁律:auto_registered 的新 Key 丢弃,不回模型不落盘") {
    AnysearchOnFakeServer harness({{"api_key", "FAKE_ANYSEARCH_BEARER_2"}});
    // 服务端回一枚自动注册的新 Key:Lua 须丢弃值,只留非敏感提示。
    harness.server_.Enqueue(MakeApiResponse(
        "{\"results\":[{\"title\":\"keep me\"}]}",
        "\"auto_registered\":{\"api_key\":\"FAKE_AUTO_REGISTERED_KEY_9\"},"));

    const tools::Tool::Result result = harness.Run("search", {{"query", "key rotation"}});
    CAPTURE(result.content);
    CHECK_FALSE(result.is_error);
    // 值不进模型结果;auto_registered 字段整体摘除。
    CHECK(result.content.find("FAKE_AUTO_REGISTERED_KEY_9") == std::string::npos);
    CHECK(result.content.find("auto_registered") == std::string::npos);
    // 非敏感提示在,业务数据照回。
    CHECK(result.content.find("key_notice") != std::string::npos);
    CHECK(result.content.find("不自动保存") != std::string::npos);
    CHECK(result.content.find("keep me") != std::string::npos);
    // 请求仍带着宿主代填的旧 Key(解析自声明来源,与响应无关)。
    REQUIRE(harness.server_.requests().size() == 1);
    CHECK(HeaderOf(harness.server_.requests()[0], "authorization").value_or("") ==
          "Bearer FAKE_ANYSEARCH_BEARER_2");
}

// ---------------------------------------------------------------------------
// 加载期零副作用经真包脚本钉一道(§九)
// ---------------------------------------------------------------------------

TEST_CASE("加载期零副作用:真 anysearch.lua 顶层不碰网络不碰 Secret") {
    // 假件直注:加载完成后两枚计数器都为 0;四枚 handler 全对上账。
    auto transport = std::make_unique<FakeHttpTransport>();
    auto resolver = std::make_unique<FakeSecretResolver>();
    resolver->values["api_key"] = "FAKE_LOAD_PROBE";
    FakeHttpTransport* transport_ptr = transport.get();
    FakeSecretResolver* resolver_ptr = resolver.get();

    const std::string manifest_text = ReadFileText(PluginDir() / "plugin.json");
    auto parsed = ParsePluginManifest(manifest_text, PluginDir());
    REQUIRE(parsed.has_value());
    auto manifest = std::make_shared<const PluginManifest>(std::move(*parsed));

    ManifestLuaLoadOptions options;
    options.transport = std::move(transport);
    options.resolver = std::move(resolver);
    auto plugin = LoadManifestLuaPlugin(manifest, std::move(options));
    REQUIRE(plugin.has_value());
    REQUIRE((*plugin)->state->entries().size() == 4);
    CHECK(transport_ptr->call_count() == 0);
    CHECK(resolver_ptr->resolve_count == 0);
}

// ---------------------------------------------------------------------------
// 匿名真网一笔(§13.5/阶段 5 清单):默认 SKIP,显式 opt-in 才跑。
// 生产构造路:不注 transport/resolver——CprBoundedHttpTransport 吃真
// manifest 网络账,DNS 走系统;匿名访问(不保证进程环境无 Key,有则带
// 头,断言不看 Key)。要烧外网的机器上:
//   LUBANCODE_ANYSEARCH_REAL_NET=1 ctest -R integration.plugins.test_anysearch_package
// (走代理的机器另设 HTTPS_PROXY,libcurl 自会认。)
// ---------------------------------------------------------------------------

TEST_CASE("匿名真网一笔:默认 SKIP,设 LUBANCODE_ANYSEARCH_REAL_NET=1 才跑") {
    const char* gate = std::getenv("LUBANCODE_ANYSEARCH_REAL_NET");
    if (gate == nullptr || std::string_view(gate) != "1") {
        MESSAGE("SKIP:未设 LUBANCODE_ANYSEARCH_REAL_NET=1,匿名真网不硬跑(设计单 §13.5)");
        return;
    }

    const std::string manifest_text = ReadFileText(PluginDir() / "plugin.json");
    auto parsed = ParsePluginManifest(manifest_text, PluginDir());
    REQUIRE(parsed.has_value());
    auto manifest = std::make_shared<const PluginManifest>(std::move(*parsed));

    // 生产缺省:EnvDotEnv resolver(本测试不给数据目录,匿名)+ 真传输。
    auto plugin = LoadManifestLuaPlugin(manifest, ManifestLuaLoadOptions{});
    REQUIRE(plugin.has_value());
    ManifestLuaToolAdapter adapter(plugin->get(), &manifest->tools[1]);  // tools[1] = search

    const tools::Tool::Result result = adapter.execute({{"query", "lubancode"}});
    CAPTURE(result.content);
    CHECK_FALSE(result.is_error);
    // 官方目标的形状:200 + code 0 + data.results 数组在。
    CHECK(result.content.find("\"status\":200") != std::string::npos);
    CHECK(result.content.find("\"results\":") != std::string::npos);
    CHECK(result.content.find("request_id") != std::string::npos);
    // 假 Key 一枚都不该出现在真网回执里(本路没灌过 Key)。
    CHECK(result.content.find("FAKE_") == std::string::npos);
}
