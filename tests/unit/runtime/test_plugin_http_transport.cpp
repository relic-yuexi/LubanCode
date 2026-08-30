// CprBoundedHttpTransport 与单笔编排的单测(Lua 受控 HTTP 与 Secret 宿主
// 能力单·阶段 2)。§13.3 假服务矩阵的运行时侧全案。
//
// 章法:
//   - 五道网络边界(§8.2)用假 DNS seam 钉:越权/越段的请求 DNS 查完
//     就死,零连接;
//   - 四处字节帽(§8.3)前后一字节:帽前过、帽后拒,拒的路径 DNS 都
//     不问(不发请求);
//   - Secret 注入(§5.4/§6.2):假 Key 一律 FAKE_ 前缀;断言"只在最终
//     发包头出现"——入参头表与全部错误文案都搜不到原文;
//   - 并发帽(§8.5):每插件与全局两道闸,超限即回不排队;
//   - 编排层(ExecutePluginHttp)配 FakeHttpTransport 钉合同,真网行为
//     由上面真传输各案覆盖。
#include <doctest/doctest.h>

#include <atomic>
#include <chrono>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <cpr/cpr.h>

#include "fake_http_server.hpp"
#include "net/http_transport.hpp"
#include "runtime/plugin_http.hpp"

using namespace lubancode;
using namespace lubancode::runtime;

namespace {

constexpr const char* kFakeHost = "api.example.test";

// 假 DNS:递什么回什么,数着调用次数(证明"帽前拒连 DNS 都不问")。
class FakeDns final : public net::DnsResolver {
public:
    std::expected<std::vector<std::string>, std::string> Resolve(const std::string& host) override {
        ++calls;
        last_host = host;
        if (fail) {
            return std::unexpected("假 DNS 拒绝解析(" + host + ")");
        }
        return addresses;
    }

    std::vector<std::string> addresses{"93.184.216.34"};  // 公网示例地址,只当形状用
    bool fail = false;
    std::atomic<int> calls{0};
    std::string last_host;
};

// 假 SecretResolver:id -> 值;值带 FAKE_ 前缀明标假 Key。
class FakeSecretResolver final : public SecretResolver {
public:
    std::expected<SecretValue, SecretResolveError> Resolve(const SecretDeclaration& declaration) override {
        ++calls;
        const auto it = values.find(declaration.id);
        if (it == values.end()) {
            if (!declaration.required) {
                return SecretValue{};  // optional 未找到 -> 空值(匿名)
            }
            SecretResolveError error;
            error.issue = SecretResolveIssue::Missing;
            error.message = "假账里没有 " + declaration.id;
            return std::unexpected(error);
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

    std::map<std::string, std::string> values;
    std::atomic<int> calls{0};
};

NetworkPermission HttpPermission(int port, std::vector<std::string> methods = {"GET", "POST"}) {
    NetworkPermission permission;
    permission.scheme = "http";
    permission.host = kFakeHost;
    permission.port = port;
    permission.methods = std::move(methods);
    return permission;
}

HttpExchangeRequest SimpleRequest(std::string method, std::string url) {
    HttpExchangeRequest request;
    request.method = std::move(method);
    request.url = std::move(url);
    return request;
}

CprBoundedHttpTransport::Options LoopbackOptions(int port) {
    CprBoundedHttpTransport::Options options;
    options.permissions.push_back(HttpPermission(port));
    options.allow_loopback_targets = true;  // 测试口:连接钉本机假服务
    return options;
}

}  // namespace

// ---------------------------------------------------------------------------
// 五道网络边界(§8.2):越权不发包
// ---------------------------------------------------------------------------

TEST_CASE("真传输:manifest 未声明网络 -> network_not_declared") {
    FakeDns dns;
    CprBoundedHttpTransport::Options options;  // permissions 空 = 禁网
    options.dns = &dns;
    CprBoundedHttpTransport transport(std::move(options));
    auto result = transport.Execute(SimpleRequest("GET", "https://api.example.test/v1"), EffectiveHttpLimits{},
                                    nullptr);
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().code == LuaHostErrorCode::NetworkNotDeclared);
    CHECK(dns.calls == 0);  // 没到 DNS 那一步
}

TEST_CASE("真传输:scheme/host/port/method 不命中 -> network_target_denied") {
    FakeDns dns;
    CprBoundedHttpTransport::Options options;
    options.permissions.push_back(HttpPermission(8080, {"GET"}));
    options.dns = &dns;
    CprBoundedHttpTransport transport(std::move(options));

    const auto denied = [&](const char* method, const char* url) {
        auto result = transport.Execute(SimpleRequest(method, url), EffectiveHttpLimits{}, nullptr);
        REQUIRE_FALSE(result.has_value());
        CHECK(result.error().code == LuaHostErrorCode::NetworkTargetDenied);
    };
    denied("GET", "http://other.example.test:8080/");       // host 不中
    denied("GET", "http://api.example.test:9090/");         // port 不中
    denied("GET", "https://api.example.test/");             // scheme 不中(443 vs 8080)
    denied("POST", "http://api.example.test:8080/");        // method 不在声明表
    CHECK(dns.calls == 0);
}

TEST_CASE("真传输:userinfo/fragment/IP host -> 解析期拒绝") {
    FakeDns dns;
    CprBoundedHttpTransport::Options options;
    options.permissions.push_back(HttpPermission(8080));
    options.dns = &dns;
    CprBoundedHttpTransport transport(std::move(options));

    auto result = transport.Execute(SimpleRequest("GET", "http://user@api.example.test:8080/"), EffectiveHttpLimits{},
                                    nullptr);
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().code == LuaHostErrorCode::InvalidRequest);

    result = transport.Execute(SimpleRequest("GET", "http://api.example.test:8080/#frag"), EffectiveHttpLimits{},
                               nullptr);
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().code == LuaHostErrorCode::InvalidRequest);

    // IP 字面量 host 落地址否决(§8.2 第 3 步)。
    result = transport.Execute(SimpleRequest("GET", "http://10.0.0.1:8080/"), EffectiveHttpLimits{}, nullptr);
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().code == LuaHostErrorCode::NetworkAddressDenied);
    CHECK(dns.calls == 0);
}

TEST_CASE("真传输:DNS 候选落私网/loopback/metadata/组播 -> network_address_denied,零连接") {
    test_support::FakeHttpServer server;  // 全程零连接的对照
    FakeDns dns;
    CprBoundedHttpTransport::Options options;
    options.permissions.push_back(HttpPermission(8080));
    options.dns = &dns;
    CprBoundedHttpTransport transport(std::move(options));

    for (const std::string bad : {"10.1.2.3", "192.168.0.9", "172.16.5.5", "127.0.0.1", "169.254.169.254",
                                  "100.100.100.200", "224.0.0.1", "0.0.0.9", "::1", "fd00::1", "fe80::1"}) {
        dns.addresses = {bad};
        auto result = transport.Execute(SimpleRequest("GET", "http://api.example.test:8080/"), EffectiveHttpLimits{},
                                        nullptr);
        REQUIRE_FALSE(result.has_value());
        CHECK(result.error().code == LuaHostErrorCode::NetworkAddressDenied);
    }
    CHECK(dns.calls == 11);
    CHECK(server.connection_count() == 0);  // 越权不发包:连一次都没连
}

TEST_CASE("真传输:候选里混一枚禁连段即整体否决(最严口径,不留 rebinding 缝)") {
    FakeDns dns;
    CprBoundedHttpTransport::Options options;
    options.permissions.push_back(HttpPermission(8080));
    options.dns = &dns;
    CprBoundedHttpTransport transport(std::move(options));

    dns.addresses = {"93.184.216.34", "10.0.0.5"};  // 公网 + 私网
    auto result = transport.Execute(SimpleRequest("GET", "http://api.example.test:8080/"), EffectiveHttpLimits{},
                                    nullptr);
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().code == LuaHostErrorCode::NetworkAddressDenied);
    CHECK(result.error().message.find("10.0.0.5") != std::string::npos);

    // 测试口(loopback 放行)也救不了私网候选。
    CprBoundedHttpTransport::Options hatched = LoopbackOptions(8080);
    hatched.dns = &dns;
    CprBoundedHttpTransport loopback_transport(std::move(hatched));
    dns.addresses = {"127.0.0.1", "192.168.1.1"};
    auto still_denied = loopback_transport.Execute(SimpleRequest("GET", "http://api.example.test:8080/"),
                                                   EffectiveHttpLimits{}, nullptr);
    REQUIRE_FALSE(still_denied.has_value());
    CHECK(still_denied.error().code == LuaHostErrorCode::NetworkAddressDenied);

    // 全部候选不可放行/解析失败。
    dns.addresses = {};
    auto empty = transport.Execute(SimpleRequest("GET", "http://api.example.test:8080/"), EffectiveHttpLimits{},
                                   nullptr);
    REQUIRE_FALSE(empty.has_value());
    CHECK(empty.error().code == LuaHostErrorCode::NetworkAddressDenied);

    dns.fail = true;
    auto failed = transport.Execute(SimpleRequest("GET", "http://api.example.test:8080/"), EffectiveHttpLimits{},
                                    nullptr);
    REQUIRE_FALSE(failed.has_value());
    CHECK(failed.error().code == LuaHostErrorCode::DnsFailed);
}

// ---------------------------------------------------------------------------
// 四处字节帽(§8.3):前后一字节,拒的路径 DNS 都不问
// ---------------------------------------------------------------------------

TEST_CASE("真传输:URL 帽前后一字节") {
    FakeDns dns;
    dns.addresses = {"127.0.0.1"};  // 帽过了才轮到 DNS,然后被边界 4 拦(测试口关着)
    CprBoundedHttpTransport::Options options;
    options.permissions.push_back(HttpPermission(8080));
    options.dns = &dns;
    CprBoundedHttpTransport transport(std::move(options));

    const std::string url = "http://api.example.test:8080/search?q=0000000000";
    EffectiveHttpLimits limits;
    limits.url_bytes = static_cast<std::int64_t>(url.size());  // 帽前:正好放下
    auto fits = transport.Execute(SimpleRequest("GET", url), limits, nullptr);
    REQUIRE_FALSE(fits.has_value());
    CHECK(fits.error().code == LuaHostErrorCode::NetworkAddressDenied);  // 过了 URL 帽,死在地址分类
    CHECK(dns.calls == 1);

    limits.url_bytes = static_cast<std::int64_t>(url.size()) - 1;  // 帽后:一字节即拒
    auto overflow = transport.Execute(SimpleRequest("GET", url), limits, nullptr);
    REQUIRE_FALSE(overflow.has_value());
    CHECK(overflow.error().code == LuaHostErrorCode::RequestTooLarge);
    CHECK(dns.calls == 1);  // 不发请求:DNS 都没问第二次
}

TEST_CASE("真传输:请求头帽前后一字节(口径 name+value+4)") {
    FakeDns dns;
    dns.addresses = {"127.0.0.1"};
    CprBoundedHttpTransport::Options options;
    options.permissions.push_back(HttpPermission(8080));
    options.dns = &dns;
    CprBoundedHttpTransport transport(std::move(options));

    HttpExchangeRequest request = SimpleRequest("GET", "http://api.example.test:8080/");
    request.headers.emplace_back("X-Pad", std::string(100, 'p'));
    const std::int64_t exact = 5 + 100 + 4;  // name + value + 4 字节行开销

    EffectiveHttpLimits limits;
    limits.request_header_bytes = exact;  // 帽前
    auto fits = transport.Execute(request, limits, nullptr);
    REQUIRE_FALSE(fits.has_value());
    CHECK(fits.error().code == LuaHostErrorCode::NetworkAddressDenied);  // 头帽过了,死在地址分类
    CHECK(dns.calls == 1);

    limits.request_header_bytes = exact - 1;  // 帽后
    auto overflow = transport.Execute(request, limits, nullptr);
    REQUIRE_FALSE(overflow.has_value());
    CHECK(overflow.error().code == LuaHostErrorCode::RequestTooLarge);
    CHECK(dns.calls == 1);
}

TEST_CASE("真传输:请求体帽前后一字节") {
    FakeDns dns;
    dns.addresses = {"127.0.0.1"};
    CprBoundedHttpTransport::Options options;
    options.permissions.push_back(HttpPermission(8080));
    options.dns = &dns;
    CprBoundedHttpTransport transport(std::move(options));

    HttpExchangeRequest request = SimpleRequest("POST", "http://api.example.test:8080/submit");
    request.body = std::string(100, 'b');

    EffectiveHttpLimits limits;
    limits.request_body_bytes = 100;  // 帽前
    auto fits = transport.Execute(request, limits, nullptr);
    REQUIRE_FALSE(fits.has_value());
    CHECK(fits.error().code == LuaHostErrorCode::NetworkAddressDenied);
    CHECK(dns.calls == 1);

    limits.request_body_bytes = 99;  // 帽后
    auto overflow = transport.Execute(request, limits, nullptr);
    REQUIRE_FALSE(overflow.has_value());
    CHECK(overflow.error().code == LuaHostErrorCode::RequestTooLarge);
    CHECK(dns.calls == 1);
}

// ---------------------------------------------------------------------------
// 请求形状与禁写头(§6.2)
// ---------------------------------------------------------------------------

TEST_CASE("真传输:禁写头/坏方法/GET 带体全拒") {
    FakeDns dns;
    CprBoundedHttpTransport::Options options;
    options.permissions.push_back(HttpPermission(8080));
    options.dns = &dns;
    CprBoundedHttpTransport transport(std::move(options));

    // 禁写表四枚(Lua 侧由 BuildOutgoingHeaders 拦;最终表里冒出来也拒)。
    for (const std::string forbidden : {"Proxy-Authorization", "Cookie", "Host", "Content-Length"}) {
        HttpExchangeRequest request = SimpleRequest("GET", "http://api.example.test:8080/");
        request.headers.emplace_back(forbidden, "x");
        auto result = transport.Execute(request, EffectiveHttpLimits{}, nullptr);
        REQUIRE_FALSE(result.has_value());
        CHECK(result.error().code == LuaHostErrorCode::InvalidRequest);
    }
    // 头值带 CTL(注入面)。
    HttpExchangeRequest smuggle = SimpleRequest("GET", "http://api.example.test:8080/");
    smuggle.headers.emplace_back("X-Bad", "a\r\nHost: evil");
    auto result = transport.Execute(smuggle, EffectiveHttpLimits{}, nullptr);
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().code == LuaHostErrorCode::InvalidRequest);

    // 坏方法与 GET 带体。
    result = transport.Execute(SimpleRequest("DELETE", "http://api.example.test:8080/"), EffectiveHttpLimits{},
                               nullptr);
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().code == LuaHostErrorCode::InvalidRequest);

    HttpExchangeRequest get_with_body = SimpleRequest("GET", "http://api.example.test:8080/");
    get_with_body.body = "payload";
    result = transport.Execute(get_with_body, EffectiveHttpLimits{}, nullptr);
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().code == LuaHostErrorCode::InvalidRequest);
    CHECK(dns.calls == 0);
}

// ---------------------------------------------------------------------------
// 真传输走本机假服务:positive 全链(测试口)
// ---------------------------------------------------------------------------

TEST_CASE("真传输+假服务:GET/POST 落地、非 2xx 保留 status、3xx 不跟、敏感头过滤") {
    test_support::FakeHttpServer server;
    FakeDns dns;
    dns.addresses = {"127.0.0.1"};
    CprBoundedHttpTransport::Options options = LoopbackOptions(server.port());
    options.dns = &dns;
    CprBoundedHttpTransport transport(std::move(options));

    // GET 200。
    test_support::FakeHttpResponse ok;
    ok.status = 200;
    ok.headers.emplace_back("Content-Type", "application/json");
    ok.headers.emplace_back("Set-Cookie", "session=FAKE_COOKIE");
    ok.body = "{\"items\":[]}";
    server.Enqueue(ok);
    auto get = transport.Execute(SimpleRequest("GET", "http://api.example.test:" + std::to_string(server.port()) +
                                                      "/v1/list?page=2"),
                                 EffectiveHttpLimits{}, nullptr);
    REQUIRE(get.has_value());
    CHECK(get->status == 200);
    CHECK(get->body == "{\"items\":[]}");
    CHECK(get->final_url.find("/v1/list?page=2") != std::string::npos);
    bool saw_content_type = false;
    for (const auto& [name, value] : get->headers) {
        CHECK(name != "set-cookie");  // 敏感头过滤(§6.2)
        if (name == "content-type") {
            saw_content_type = value == "application/json";
        }
    }
    CHECK(saw_content_type);

    // POST 404:status 原样带回,不混作网络错(§11)。
    test_support::FakeHttpResponse not_found;
    not_found.status = 404;
    not_found.headers.emplace_back("Content-Type", "application/json");
    not_found.body = "{\"error\":\"missing\"}";
    server.Enqueue(not_found);
    HttpExchangeRequest post = SimpleRequest("POST", "http://api.example.test:" + std::to_string(server.port()) +
                                                          "/v1/none");
    post.headers.emplace_back("Content-Type", "application/json");
    post.body = "{\"q\":1}";
    auto missing = transport.Execute(post, EffectiveHttpLimits{}, nullptr);
    REQUIRE(missing.has_value());
    CHECK(missing->status == 404);
    CHECK(missing->body == "{\"error\":\"missing\"}");

    // 301:不跟,只此一跳(§8.2)。
    test_support::FakeHttpResponse moved;
    moved.status = 301;
    moved.headers.emplace_back("Location", "http://elsewhere.example.test/x");
    server.Enqueue(moved);
    auto redirect = transport.Execute(SimpleRequest("GET", "http://api.example.test:" + std::to_string(server.port()) +
                                                            "/old"),
                                      EffectiveHttpLimits{}, nullptr);
    REQUIRE(redirect.has_value());
    CHECK(redirect->status == 301);

    const std::vector<test_support::FakeHttpRequest> received = server.requests();
    REQUIRE(received.size() == 3);
    CHECK(received[0].target == "/v1/list?page=2");
    CHECK(received[1].method == "POST");
    CHECK(received[1].body == "{\"q\":1}");
    CHECK(received[2].target == "/old");
    // DNS 只问了一次一枚:验地址与钉连接同一份答案(rebinding 窗口不存在)。
    CHECK(dns.last_host == kFakeHost);
}

TEST_CASE("真传输+假服务:取消三档与超帽(timeout 与 cancelled 不串码)") {
    test_support::FakeHttpServer server;
    FakeDns dns;
    dns.addresses = {"127.0.0.1"};
    CprBoundedHttpTransport::Options options = LoopbackOptions(server.port());
    options.dns = &dns;
    CprBoundedHttpTransport transport(std::move(options));

    // 调用前已置位。
    std::atomic<bool> pre_cancel{true};
    auto pre = transport.Execute(SimpleRequest("GET", "http://api.example.test:" + std::to_string(server.port()) + "/"),
                                 EffectiveHttpLimits{}, &pre_cancel);
    REQUIRE_FALSE(pre.has_value());
    CHECK(pre.error().code == LuaHostErrorCode::Cancelled);
    CHECK(server.connection_count() == 0);

    // 等首字节阶段取消:墙钟放足,只看取消。
    test_support::FakeHttpResponse stall;
    stall.status = 200;
    stall.body = "never";
    stall.delay_before_response = std::chrono::seconds(10);
    server.Enqueue(stall);
    std::atomic<bool> cancel{false};
    std::thread canceller([&cancel]() {
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
        cancel.store(true);
    });
    const auto start = std::chrono::steady_clock::now();
    auto waiting = transport.Execute(SimpleRequest("GET", "http://api.example.test:" + std::to_string(server.port()) +
                                                             "/"),
                                     EffectiveHttpLimits{}, &cancel);
    const auto elapsed = std::chrono::steady_clock::now() - start;
    canceller.join();
    REQUIRE_FALSE(waiting.has_value());
    CHECK(waiting.error().code == LuaHostErrorCode::Cancelled);
    CHECK(waiting.error().message.find("已取消") != std::string::npos);
    CHECK(elapsed < std::chrono::seconds(5));

    // 墙钟:同样装死的服务器,不取消,墙钟 700ms 落锤 -> timeout。
    server.Enqueue(stall);
    EffectiveHttpLimits tight;
    tight.timeout_ms = 700;
    auto timed_out = transport.Execute(SimpleRequest("GET", "http://api.example.test:" + std::to_string(server.port()) +
                                                               "/"),
                                       tight, nullptr);
    REQUIRE_FALSE(timed_out.has_value());
    CHECK(timed_out.error().code == LuaHostErrorCode::Timeout);

    // 响应体超帽:回 4MiB+1,帽是缺省 4MiB -> response_too_large。
    test_support::FakeHttpResponse huge;
    huge.status = 200;
    huge.body = std::string(4 * 1024 * 1024 + 1, 'z');
    server.Enqueue(huge);
    auto huge_result = transport.Execute(SimpleRequest("GET", "http://api.example.test:" + std::to_string(server.port()) +
                                                                   "/big"),
                                         EffectiveHttpLimits{}, nullptr);
    REQUIRE_FALSE(huge_result.has_value());
    CHECK(huge_result.error().code == LuaHostErrorCode::ResponseTooLarge);
}

// ---------------------------------------------------------------------------
// Secret 头注入(§5.4/§6.2)
// ---------------------------------------------------------------------------

TEST_CASE("BuildOutgoingHeaders:bearer/header 注入与禁写表") {
    const std::string fake_key = "FAKE_SECRET_KEY_123";

    HttpAuthSpec bearer;
    bearer.type = "bearer";
    bearer.secret_id = "api_key";
    auto headers = BuildOutgoingHeaders({}, &bearer, fake_key);
    REQUIRE(headers.has_value());
    REQUIRE(headers->size() == 1);
    CHECK((*headers)[0].first == "Authorization");
    CHECK((*headers)[0].second == "Bearer " + fake_key);

    HttpAuthSpec header_auth;
    header_auth.type = "header";
    header_auth.secret_id = "api_key";
    header_auth.name = "x-api-key";
    headers = BuildOutgoingHeaders({}, &header_auth, fake_key);
    REQUIRE(headers.has_value());
    CHECK((*headers)[0].first == "X-Api-Key");  // 规范名大写定形
    CHECK((*headers)[0].second == fake_key);

    header_auth.prefix = "Bearer";
    headers = BuildOutgoingHeaders({}, &header_auth, fake_key);
    REQUIRE(headers.has_value());
    CHECK((*headers)[0].second == "Bearer " + fake_key);

    header_auth.name = "Authorization";
    header_auth.prefix.clear();
    headers = BuildOutgoingHeaders({}, &header_auth, fake_key);
    REQUIRE(headers.has_value());
    CHECK((*headers)[0].first == "Authorization");

    header_auth.name = "Api-Key";
    headers = BuildOutgoingHeaders({}, &header_auth, fake_key);
    REQUIRE(headers.has_value());
    CHECK((*headers)[0].first == "Api-Key");

    // 第四类名拒。
    header_auth.name = "X-Custom-Business";
    headers = BuildOutgoingHeaders({}, &header_auth, fake_key);
    REQUIRE_FALSE(headers.has_value());
    CHECK(headers.error() == LuaHostErrorCode::InvalidRequest);
    header_auth.name.clear();
    headers = BuildOutgoingHeaders({}, &header_auth, fake_key);
    REQUIRE_FALSE(headers.has_value());

    // 空 secret(optional 匿名):不注入,Lua 头原样过。
    headers = BuildOutgoingHeaders({{"Accept", "application/json"}}, &bearer, "");
    REQUIRE(headers.has_value());
    REQUIRE(headers->size() == 1);
    CHECK((*headers)[0].first == "Accept");
}

TEST_CASE("BuildOutgoingHeaders:Lua 自写禁写头全拒") {
    HttpAuthSpec auth;
    auth.type = "bearer";
    for (const std::string forbidden :
         {"Authorization", "authorization", "Proxy-Authorization", "Cookie", "cookie", "Host", "Content-Length"}) {
        auto headers = BuildOutgoingHeaders({{forbidden, "x"}}, &auth, "FAKE_X");
        REQUIRE_FALSE(headers.has_value());
        CHECK(headers.error() == LuaHostErrorCode::InvalidRequest);
    }
    // 坏头名(非 token 字符)与坏头值(CTL)。
    CHECK_FALSE(BuildOutgoingHeaders({{"Bad Name", "x"}}, nullptr, "").has_value());
    CHECK_FALSE(BuildOutgoingHeaders({{"X-Bad", "line\r\ninject"}}, nullptr, "").has_value());
    CHECK(BuildOutgoingHeaders({{"X-Good", "plain value"}}, nullptr, "").has_value());
}

// ---------------------------------------------------------------------------
// §11 错误映射(runtime 侧)
// ---------------------------------------------------------------------------

TEST_CASE("ClassifyCurlErrorCode:§11 映射表") {
    const auto code = [](cpr::ErrorCode value) { return static_cast<long>(value); };
    CHECK(ClassifyCurlErrorCode(code(cpr::ErrorCode::COULDNT_RESOLVE_HOST)) == LuaHostErrorCode::DnsFailed);
    CHECK(ClassifyCurlErrorCode(code(cpr::ErrorCode::SSL_CONNECT_ERROR)) == LuaHostErrorCode::TlsFailed);
    CHECK(ClassifyCurlErrorCode(code(cpr::ErrorCode::PEER_FAILED_VERIFICATION)) == LuaHostErrorCode::TlsFailed);
    CHECK(ClassifyCurlErrorCode(code(cpr::ErrorCode::OPERATION_TIMEDOUT)) == LuaHostErrorCode::Timeout);
    CHECK(ClassifyCurlErrorCode(code(cpr::ErrorCode::COULDNT_CONNECT)) == LuaHostErrorCode::NetworkFailed);
    CHECK(ClassifyCurlErrorCode(code(cpr::ErrorCode::GOT_NOTHING)) == LuaHostErrorCode::NetworkFailed);
}

// ---------------------------------------------------------------------------
// 并发帽(§8.5)
// ---------------------------------------------------------------------------

TEST_CASE("InFlightGate:到帽拒、释放回收;全局闸单例 32") {
    InFlightGate gate(3);
    CHECK(gate.TryAcquire());
    CHECK(gate.TryAcquire());
    CHECK(gate.TryAcquire());
    CHECK_FALSE(gate.TryAcquire());  // 第 4 笔拒
    gate.Release();
    CHECK(gate.TryAcquire());  // 空出一个位

    // RAII 释手。
    {
        const InFlightGate::Releaser releaser(&gate);
        CHECK_FALSE(gate.TryAcquire());
    }
    CHECK(gate.TryAcquire());

    const std::shared_ptr<InFlightGate> global = GlobalHttpInFlightGate();
    CHECK(global->max_in_flight() == kHttpMaxInFlightGlobal);
    CHECK(kHttpMaxInFlightPerPlugin == 4);
    CHECK(kHttpMaxInFlightGlobal == 32);
    CHECK(GlobalHttpInFlightGate().get() == global.get());  // 同一份单例
}

TEST_CASE("真传输:每插件并发帽超限即回 concurrency_limit,不排死队") {
    test_support::FakeHttpServer server;
    FakeDns dns;
    dns.addresses = {"127.0.0.1"};
    CprBoundedHttpTransport::Options options = LoopbackOptions(server.port());
    options.dns = &dns;
    options.per_plugin_max_in_flight = 2;
    CprBoundedHttpTransport transport(std::move(options));

    test_support::FakeHttpResponse stall;
    stall.status = 200;
    stall.body = "slow";
    stall.delay_before_response = std::chrono::seconds(2);
    server.Enqueue(stall);
    server.Enqueue(stall);

    const std::string url = "http://api.example.test:" + std::to_string(server.port()) + "/";
    std::vector<std::thread> workers;
    for (int i = 0; i < 2; ++i) {
        workers.emplace_back([&transport, &url]() {
            (void)transport.Execute(SimpleRequest("GET", url), EffectiveHttpLimits{}, nullptr);
        });
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(400));  // 等两笔都进 cpr

    const auto start = std::chrono::steady_clock::now();
    auto rejected = transport.Execute(SimpleRequest("GET", url), EffectiveHttpLimits{}, nullptr);
    const auto elapsed = std::chrono::steady_clock::now() - start;
    REQUIRE_FALSE(rejected.has_value());
    CHECK(rejected.error().code == LuaHostErrorCode::ConcurrencyLimit);
    CHECK(rejected.error().message.find("2") != std::string::npos);
    CHECK(elapsed < std::chrono::seconds(1));  // 即回,没排队

    for (std::thread& worker : workers) {
        worker.join();
    }
}

TEST_CASE("真传输:全局并发闸两插件共享,超限即回") {
    test_support::FakeHttpServer server;
    FakeDns dns;
    dns.addresses = {"127.0.0.1"};
    const auto shared_gate = std::make_shared<InFlightGate>(3);

    CprBoundedHttpTransport::Options first_options = LoopbackOptions(server.port());
    first_options.dns = &dns;
    first_options.global_gate = shared_gate;
    CprBoundedHttpTransport first(std::move(first_options));

    CprBoundedHttpTransport::Options second_options = LoopbackOptions(server.port());
    second_options.dns = &dns;
    second_options.global_gate = shared_gate;
    CprBoundedHttpTransport second(std::move(second_options));

    test_support::FakeHttpResponse stall;
    stall.status = 200;
    stall.body = "slow";
    stall.delay_before_response = std::chrono::seconds(2);
    server.Enqueue(stall);
    server.Enqueue(stall);
    server.Enqueue(stall);

    const std::string url = "http://api.example.test:" + std::to_string(server.port()) + "/";
    std::vector<std::thread> workers;
    workers.emplace_back([&first, &url]() { (void)first.Execute(SimpleRequest("GET", url), EffectiveHttpLimits{}, nullptr); });
    workers.emplace_back([&first, &url]() { (void)first.Execute(SimpleRequest("GET", url), EffectiveHttpLimits{}, nullptr); });
    workers.emplace_back([&second, &url]() { (void)second.Execute(SimpleRequest("GET", url), EffectiveHttpLimits{}, nullptr); });
    std::this_thread::sleep_for(std::chrono::milliseconds(400));

    auto rejected = second.Execute(SimpleRequest("GET", url), EffectiveHttpLimits{}, nullptr);
    REQUIRE_FALSE(rejected.has_value());
    CHECK(rejected.error().code == LuaHostErrorCode::ConcurrencyLimit);
    CHECK(rejected.error().message.find("全局") != std::string::npos);

    for (std::thread& worker : workers) {
        worker.join();
    }
}

// ---------------------------------------------------------------------------
// 编排层(ExecutePluginHttp):合同用 FakeHttpTransport 钉
// ---------------------------------------------------------------------------

TEST_CASE("编排:GET/POST json 端到端,Content-Type 自动补,响应 json 解析") {
    FakeHttpTransport transport;
    HttpExchangeResponse response;
    response.status = 200;
    response.headers.emplace_back("content-type", "application/json");
    response.body = "{\"result\":\"hit\",\"n\":2}";
    response.final_url = "https://api.anysearch.com/v1/search";
    transport.EnqueueResponse(response);

    FakeSecretResolver secrets;
    PluginHttpCallSpec spec;
    spec.transport = &transport;

    PluginHttpApiRequest request;
    request.method = "POST";
    request.url = "https://api.anysearch.com/v1/search?q=luban";
    request.json = nlohmann::json{{"query", "luban"}};
    request.has_json = true;
    auto outcome = ExecutePluginHttp(request, spec);
    REQUIRE(outcome.has_value());
    CHECK(outcome->status == 200);
    CHECK(outcome->json_parsed);
    CHECK(outcome->json["result"] == "hit");
    CHECK(outcome->bytes == response.body.size());
    CHECK(outcome->url == "https://api.anysearch.com/v1/search");

    REQUIRE(transport.call_count() == 1);
    const auto& sent = transport.calls()[0].request;
    CHECK(sent.method == "POST");
    CHECK(sent.body == "{\"query\":\"luban\"}");
    bool saw_json_type = false;
    for (const auto& [name, value] : sent.headers) {
        if (name == "Content-Type") {
            saw_json_type = value == "application/json";
        }
    }
    CHECK(saw_json_type);

    // 非 json Content-Type:不解析,json_parsed=false。
    transport.EnqueueResponse([]() {
        HttpExchangeResponse plain;
        plain.status = 200;
        plain.headers.emplace_back("content-type", "text/plain");
        plain.body = "just text";
        return plain;
    }());
    auto plain = ExecutePluginHttp(request, spec);
    REQUIRE(plain.has_value());
    CHECK_FALSE(plain->json_parsed);
    CHECK(plain->body == "just text");
}

TEST_CASE("编排:非 2xx 走成功形状带 status;坏 JSON 落 invalid_json 且 status 不丢") {
    FakeHttpTransport transport;
    PluginHttpCallSpec spec;
    spec.transport = &transport;

    PluginHttpApiRequest request;
    request.method = "GET";
    request.url = "https://api.anysearch.com/v1/x";

    HttpExchangeResponse not_found;
    not_found.status = 404;
    not_found.headers.emplace_back("content-type", "application/json");
    not_found.body = "{\"error\":\"nope\"}";
    transport.EnqueueResponse(not_found);
    auto missing = ExecutePluginHttp(request, spec);
    REQUIRE(missing.has_value());  // 不混作网络错
    CHECK(missing->status == 404);
    CHECK(missing->json_parsed);

    HttpExchangeResponse bad_json;
    bad_json.status = 200;
    bad_json.headers.emplace_back("content-type", "application/json");
    bad_json.body = "{\"result\": oops}";
    transport.EnqueueResponse(bad_json);
    auto broken = ExecutePluginHttp(request, spec);
    REQUIRE_FALSE(broken.has_value());
    CHECK(broken.error().code == LuaHostErrorCode::InvalidJson);
    CHECK(broken.error().status == 200);
}

TEST_CASE("编排:Secret 只在最终发包头出现,错误文案搜不到原文") {
    FakeHttpTransport transport;
    HttpExchangeResponse response;
    response.status = 204;
    transport.EnqueueResponse(response);

    FakeSecretResolver resolver;
    resolver.values["api_key"] = "FAKE_BEARER_TOKEN_456";

    PluginHttpCallSpec spec;
    spec.transport = &transport;
    spec.secrets.push_back(SecretDeclaration{"api_key", "ANYSEARCH_API_KEY", true});
    spec.secret_resolver = &resolver;

    PluginHttpApiRequest request;
    request.method = "GET";
    request.url = "https://api.anysearch.com/v1/sub-domains";
    request.headers.emplace_back("Accept", "application/json");
    request.has_auth = true;
    request.auth.type = "bearer";
    request.auth.secret_id = "api_key";
    auto outcome = ExecutePluginHttp(request, spec);
    REQUIRE(outcome.has_value());

    // 只在最终头表:入参头表没有,最终发包头有。
    REQUIRE(transport.call_count() == 1);
    const auto& sent_headers = transport.calls()[0].request.headers;
    bool saw_bearer = false;
    for (const auto& [name, value] : sent_headers) {
        if (name == "Authorization") {
            saw_bearer = value == "Bearer FAKE_BEARER_TOKEN_456";
        }
    }
    CHECK(saw_bearer);

    // Lua 自写 Authorization 被拒,Secret 也不会借道混进来。
    request.headers.emplace_back("Authorization", "Bearer manual");
    auto rejected = ExecutePluginHttp(request, spec);
    REQUIRE_FALSE(rejected.has_value());
    CHECK(rejected.error().code == LuaHostErrorCode::InvalidRequest);
    CHECK(rejected.error().message.find("FAKE_BEARER_TOKEN_456") == std::string::npos);

    // header 型 auth + 前缀。
    request.headers.pop_back();
    request.auth.type = "header";
    request.auth.name = "X-Api-Key";
    request.auth.prefix = "Key";
    transport.EnqueueResponse(response);
    auto keyed = ExecutePluginHttp(request, spec);
    REQUIRE(keyed.has_value());
    REQUIRE(transport.call_count() == 2);
    bool saw_key_header = false;
    for (const auto& [name, value] : transport.calls()[1].request.headers) {
        if (name == "X-Api-Key") {
            saw_key_header = value == "Key FAKE_BEARER_TOKEN_456";
        }
    }
    CHECK(saw_key_header);
}

TEST_CASE("编排:required/optional Secret 分流") {
    FakeHttpTransport transport;
    HttpExchangeResponse response;
    response.status = 200;
    transport.EnqueueResponse(response);

    FakeSecretResolver empty_resolver;  // 一枚都没有

    PluginHttpCallSpec spec;
    spec.transport = &transport;
    spec.secrets.push_back(SecretDeclaration{"api_key", "ANYSEARCH_API_KEY", true});

    PluginHttpApiRequest request;
    request.method = "GET";
    request.url = "https://api.anysearch.com/v1/x";
    request.has_auth = true;
    request.auth.type = "bearer";
    request.auth.secret_id = "api_key";

    // 声明 required,resolver 里没有 -> secret_missing。
    spec.secret_resolver = &empty_resolver;
    auto missing = ExecutePluginHttp(request, spec);
    REQUIRE_FALSE(missing.has_value());
    CHECK(missing.error().code == LuaHostErrorCode::SecretMissing);
    CHECK(transport.call_count() == 0);  // 没发包

    // optional + 缺失 -> 匿名发,不带 auth 头。
    spec.secrets[0].required = false;
    request.auth.optional = true;
    transport.EnqueueResponse(response);
    auto anonymous = ExecutePluginHttp(request, spec);
    REQUIRE(anonymous.has_value());
    REQUIRE(transport.call_count() == 1);
    for (const auto& [name, value] : transport.calls()[0].request.headers) {
        CHECK(name != "Authorization");
    }

    // 未声明的 secret id -> secret_not_declared。
    request.auth.optional = false;
    request.auth.secret_id = "other_key";
    auto undeclared = ExecutePluginHttp(request, spec);
    REQUIRE_FALSE(undeclared.has_value());
    CHECK(undeclared.error().code == LuaHostErrorCode::SecretNotDeclared);
}

TEST_CASE("编排:请求形状(方法/json+body/GET 体/bearer 带 name)") {
    FakeHttpTransport transport;
    PluginHttpCallSpec spec;
    spec.transport = &transport;

    PluginHttpApiRequest request;
    request.method = "DELETE";
    request.url = "https://api.anysearch.com/v1/x";
    auto bad_method = ExecutePluginHttp(request, spec);
    REQUIRE_FALSE(bad_method.has_value());
    CHECK(bad_method.error().code == LuaHostErrorCode::InvalidRequest);

    request.method = "POST";
    request.has_json = true;
    request.has_body = true;
    request.body = "raw";
    auto both = ExecutePluginHttp(request, spec);
    REQUIRE_FALSE(both.has_value());
    CHECK(both.error().code == LuaHostErrorCode::InvalidRequest);

    request.has_json = false;
    request.method = "GET";
    auto get_body = ExecutePluginHttp(request, spec);
    REQUIRE_FALSE(get_body.has_value());
    CHECK(get_body.error().code == LuaHostErrorCode::InvalidRequest);

    request.has_body = false;
    request.method = "POST";
    request.has_auth = true;
    request.auth.type = "bearer";
    request.auth.secret_id = "api_key";
    request.auth.name = "X-Api-Key";  // bearer 不收 name
    auto bad_bearer = ExecutePluginHttp(request, spec);
    REQUIRE_FALSE(bad_bearer.has_value());
    CHECK(bad_bearer.error().code == LuaHostErrorCode::InvalidRequest);

    CHECK(transport.call_count() == 0);
}

TEST_CASE("编排:timeout 只降不升;传输错误带 retryable") {
    FakeHttpTransport transport;
    PluginHttpCallSpec spec;
    spec.transport = &transport;
    spec.limits.timeout_ms = 5'000;

    PluginHttpApiRequest request;
    request.method = "GET";
    request.url = "https://api.anysearch.com/v1/x";
    request.timeout_ms = 999'999;  // 想抬 -> 被压回生效帽
    HttpExchangeResponse response;
    response.status = 200;
    transport.EnqueueResponse(response);
    auto outcome = ExecutePluginHttp(request, spec);
    REQUIRE(outcome.has_value());
    REQUIRE(transport.call_count() == 1);
    CHECK(transport.calls()[0].limits.timeout_ms == 5'000);

    request.timeout_ms = 1'200;  // 下调生效
    transport.EnqueueResponse(response);
    outcome = ExecutePluginHttp(request, spec);
    REQUIRE(outcome.has_value());
    CHECK(transport.calls()[1].limits.timeout_ms == 1'200);

    // 传输错误分型照 §11:timeout retryable=true,cancelled=false。
    HttpTransportError timeout_error;
    timeout_error.code = LuaHostErrorCode::Timeout;
    timeout_error.message = "宿主墙钟到点";
    transport.EnqueueError(timeout_error);
    request.timeout_ms = 0;
    auto timed_out = ExecutePluginHttp(request, spec);
    REQUIRE_FALSE(timed_out.has_value());
    CHECK(timed_out.error().code == LuaHostErrorCode::Timeout);
    CHECK(timed_out.error().retryable);

    HttpTransportError cancel_error;
    cancel_error.code = LuaHostErrorCode::Cancelled;
    cancel_error.message = "已取消";
    transport.EnqueueError(cancel_error);
    auto cancelled = ExecutePluginHttp(request, spec);
    REQUIRE_FALSE(cancelled.has_value());
    CHECK(cancelled.error().code == LuaHostErrorCode::Cancelled);
    CHECK_FALSE(cancelled.error().retryable);
}
