// Lua 受控 HTTP 的合同单测(阶段 0):错误码总表、URL 规范化纯函数、
// 网络账对账与 fake transport 的 seam 形状。
//
// 章法:纯函数直测,不接网、不碰 Lua state(真传输阶段 2,Lua 注册
// 阶段 3)。

#include <doctest/doctest.h>

#include <atomic>
#include <string>

#include "runtime/plugin_http.hpp"

using namespace lubancode;
using namespace lubancode::runtime;

// ---------------------------------------------------------------------------
// 错误码总表(§11):17 枚码、稳定串、缺省 retryable
// ---------------------------------------------------------------------------

TEST_CASE("§11 错误码表:每枚码的稳定串与 retryable 各就各位") {
    struct Row {
        LuaHostErrorCode code;
        const char* name;
        bool retryable;
    };
    const Row table[] = {
        {LuaHostErrorCode::NoActiveToolCall, "no_active_tool_call", false},
        {LuaHostErrorCode::NetworkNotDeclared, "network_not_declared", false},
        {LuaHostErrorCode::NetworkTargetDenied, "network_target_denied", false},
        {LuaHostErrorCode::NetworkAddressDenied, "network_address_denied", false},
        {LuaHostErrorCode::SecretNotDeclared, "secret_not_declared", false},
        {LuaHostErrorCode::SecretMissing, "secret_missing", false},
        {LuaHostErrorCode::InvalidRequest, "invalid_request", false},
        {LuaHostErrorCode::RequestTooLarge, "request_too_large", false},
        {LuaHostErrorCode::ResponseTooLarge, "response_too_large", false},
        {LuaHostErrorCode::Timeout, "timeout", true},
        {LuaHostErrorCode::Cancelled, "cancelled", false},
        {LuaHostErrorCode::DnsFailed, "dns_failed", true},
        {LuaHostErrorCode::TlsFailed, "tls_failed", false},   // 缺省 false;连接类由分型覆盖
        {LuaHostErrorCode::NetworkFailed, "network_failed", true},
        {LuaHostErrorCode::HttpStatus, "http_status", false}, // 由 Lua 按 408/429/5xx 自判
        {LuaHostErrorCode::InvalidJson, "invalid_json", false},
        {LuaHostErrorCode::ConcurrencyLimit, "concurrency_limit", true},
    };
    for (const Row& row : table) {
        CHECK(LuaHostErrorCodeName(row.code) == row.name);
        CHECK(LuaHostErrorCodeDefaultRetryable(row.code) == row.retryable);
        // 缺省文案非空,且不带值类内容。
        CHECK_FALSE(LuaHostErrorCodeDefaultMessage(row.code).empty());
    }
}

// ---------------------------------------------------------------------------
// URL 规范化(纯函数;§8.2 第 1/3 步的静态半边)
// ---------------------------------------------------------------------------

TEST_CASE("NormalizeHttpUrl:大小写/尾点/缺省端口/path 与 query 规范化") {
    auto url = NormalizeHttpUrl("HTTPS://API.AnySearch.com./v1/search?q=luban");
    REQUIRE(url.has_value());
    CHECK(url->scheme == "https");
    CHECK(url->host == "api.anysearch.com");
    CHECK(url->port == 443);
    CHECK_FALSE(url->has_explicit_port);
    CHECK(url->path == "/v1/search");
    CHECK(url->query == "q=luban");
    CHECK(url->text == "https://api.anysearch.com/v1/search?q=luban");

    // 显式 443 端口记账保留。
    auto explicit_port = NormalizeHttpUrl("https://api.anysearch.com:443/v1");
    REQUIRE(explicit_port.has_value());
    CHECK(explicit_port->port == 443);
    CHECK(explicit_port->has_explicit_port);
    CHECK(explicit_port->text == "https://api.anysearch.com:443/v1");

    // 无 path:补 "/"。
    auto bare = NormalizeHttpUrl("https://api.anysearch.com");
    REQUIRE(bare.has_value());
    CHECK(bare->path == "/");
    CHECK(bare->query.empty());

    // 只有 query:path 补 "/",query 存整段查询串(参数名与值都在)。
    auto query_only = NormalizeHttpUrl("https://api.anysearch.com?q=1");
    REQUIRE(query_only.has_value());
    CHECK(query_only->path == "/");
    CHECK(query_only->query == "q=1");
}

TEST_CASE("NormalizeHttpUrl:userinfo 与 fragment 禁(invalid_request)") {
    const auto rejected = [&](std::string_view raw) {
        auto url = NormalizeHttpUrl(raw);
        REQUIRE_FALSE(url.has_value());
        CHECK(url.error() == LuaHostErrorCode::InvalidRequest);
    };
    rejected("https://user@api.example.com/");
    rejected("https://user:pass@api.example.com/");
    rejected("https://api.example.com/#frag");
    // 坏形状:没 scheme、别的 scheme、坏端口、0 端口。
    rejected("api.example.com/");
    rejected("ftp://api.example.com/");
    rejected("https://api.example.com:notaport/");
    rejected("https://api.example.com:0/");
}

TEST_CASE("NormalizeHttpUrl:IP 字面量/localhost/.local 落地址否决") {
    const auto denied = [&](std::string_view raw) {
        auto url = NormalizeHttpUrl(raw);
        REQUIRE_FALSE(url.has_value());
        CHECK(url.error() == LuaHostErrorCode::NetworkAddressDenied);
    };
    denied("https://10.0.0.1/v1");
    denied("https://[2001:db8::1]/");
    denied("https://localhost/");
    denied("https://printer.local/");
}

// ---------------------------------------------------------------------------
// 网络账对账(纯函数)
// ---------------------------------------------------------------------------

TEST_CASE("NetworkPermissionAllows:scheme/host/port 精确命中才放行") {
    std::vector<NetworkPermission> permissions;
    NetworkPermission permission;
    permission.scheme = "https";
    permission.host = "api.anysearch.com";
    permission.port = 443;
    permission.methods = {"GET", "POST"};
    permissions.push_back(permission);

    auto hit = NormalizeHttpUrl("https://api.anysearch.com/v1/search?q=1");
    REQUIRE(hit.has_value());
    CHECK(NetworkPermissionAllows(permissions, *hit));

    // 大小写/尾点规范化后仍命中。
    auto normalized = NormalizeHttpUrl("https://API.ANYSEARCH.COM./v1");
    REQUIRE(normalized.has_value());
    CHECK(NetworkPermissionAllows(permissions, *normalized));

    // host 不中。
    auto miss_host = NormalizeHttpUrl("https://evil.example.com/v1");
    REQUIRE(miss_host.has_value());
    CHECK_FALSE(NetworkPermissionAllows(permissions, *miss_host));
    // 端口不中(显式 8443)。
    auto miss_port = NormalizeHttpUrl("https://api.anysearch.com:8443/v1");
    REQUIRE(miss_port.has_value());
    CHECK_FALSE(NetworkPermissionAllows(permissions, *miss_port));
    // 空账一概不放(§5.1:未声明网络 = network_not_declared)。
    CHECK_FALSE(NetworkPermissionAllows({}, *hit));
}

// ---------------------------------------------------------------------------
// FakeHttpTransport:seam 形状(记账 + 编排)
// ---------------------------------------------------------------------------

TEST_CASE("FakeHttpTransport:记账请求与编排结果先到先得") {
    FakeHttpTransport transport;
    HttpExchangeRequest request;
    request.method = "POST";
    request.url = "https://api.anysearch.com/v1/search";
    request.headers.emplace_back("Content-Type", "application/json");
    request.body = "{}";

    HttpExchangeResponse response;
    response.status = 200;
    response.body = "{\"ok\":true}";
    transport.EnqueueResponse(response);

    std::atomic<bool> cancel{false};
    auto first = transport.Execute(request, EffectiveHttpLimits{}, &cancel);
    REQUIRE(first.has_value());
    CHECK(first->status == 200);
    CHECK(first->body == "{\"ok\":true}");

    REQUIRE(transport.call_count() == 1);
    CHECK(transport.calls()[0].request.method == "POST");
    CHECK(transport.calls()[0].request.url == "https://api.anysearch.com/v1/search");
    CHECK(transport.calls()[0].cancel_observed);

    // 脚本耗尽:NetworkFailed。
    auto second = transport.Execute(request, EffectiveHttpLimits{}, nullptr);
    REQUIRE_FALSE(second.has_value());
    CHECK(second.error().code == LuaHostErrorCode::NetworkFailed);

    // 错误编排 + 清脚本。
    HttpTransportError error;
    error.code = LuaHostErrorCode::ResponseTooLarge;
    error.message = "响应体超过 4194304 字节,已中止";
    transport.EnqueueError(error);
    auto third = transport.Execute(request, EffectiveHttpLimits{}, nullptr);
    REQUIRE_FALSE(third.has_value());
    CHECK(third.error().code == LuaHostErrorCode::ResponseTooLarge);
    CHECK(third.error().message.find("4194304") != std::string::npos);
    transport.ClearScript();
    CHECK(transport.call_count() == 3);
}

TEST_CASE("BoundedHttpTransport 是多态 seam:fake 经接口调用") {
    // 阶段 0 的验收线:看头文件与测试已能说清谁持 Secret、谁能联网、何时
    // 取消、哪里落帽。这里钉接口形状——经基类指针走 fake,阶段 2 换真传输
    // 不动调用方。
    FakeHttpTransport fake;
    BoundedHttpTransport& transport = fake;
    HttpExchangeRequest request;
    request.method = "GET";
    request.url = "https://api.anysearch.com/v1/sub-domains";
    HttpExchangeResponse response;
    response.status = 200;
    fake.EnqueueResponse(response);
    std::atomic<bool> cancel{false};
    auto result = transport.Execute(request, EffectiveHttpLimits{}, &cancel);
    REQUIRE(result.has_value());
    CHECK(result->status == 200);
    CHECK(fake.call_count() == 1);
}
