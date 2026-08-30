// 中立 HTTP 传输底座的单测(Lua 受控 HTTP 与 Secret 宿主能力单·阶段 2)。
//
// 章法:
//   - 纯函数件(IP 解析/禁连段分类/curl 错误映射)直钉全表,不接网;
//   - 真传输件用本机回环假服务(test_support::FakeHttpServer)钉:
//     GET/POST 字节、DNS 钉地址、响应头/体帽前后一字节、墙钟、三段取消;
//   - SystemDnsResolver 只解析 localhost(hosts 文件,不碰公网);
//   - 全程不碰公网;夹具一律本机回环。
#include <doctest/doctest.h>

#include <atomic>
#include <chrono>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <cpr/cpr.h>

#include "fake_http_server.hpp"
#include "net/http_transport.hpp"

using namespace lubancode;
using namespace lubancode::net;

namespace {

// 假 DNS 与钉地址的常客:一枚系统 DNS 永远解析不出的名字——请求能通,
// 唯一的解释就是连接钉在了我们递进去的地址上(rebinding 防御的机制证据)。
constexpr const char* kFakeHost = "api.example.test";

std::string Url(test_support::FakeHttpServer& server, const char* path) {
    return std::string("http://") + kFakeHost + ":" + std::to_string(server.port()) + path;
}

net::PinnedDnsResolve LoopbackPin(test_support::FakeHttpServer& server) {
    net::PinnedDnsResolve pin;
    pin.host = kFakeHost;
    pin.port = server.port();
    pin.addr = "127.0.0.1";
    return pin;
}

test_support::FakeHttpResponse TextResponse(int status, std::string body) {
    test_support::FakeHttpResponse response;
    response.status = status;
    response.headers.emplace_back("Content-Type", "text/plain");
    response.body = std::move(body);
    return response;
}

}  // namespace

// ---------------------------------------------------------------------------
// ParseIpAddress:形状全表
// ---------------------------------------------------------------------------

TEST_CASE("ParseIpAddress:v4 合法形状") {
    CHECK(ParseIpAddress("8.8.8.8").has_value());
    CHECK(ParseIpAddress("0.0.0.0").has_value());
    CHECK(ParseIpAddress("255.255.255.255").has_value());
    CHECK(ParseIpAddress("203.0.113.9").has_value());
    const auto parsed = ParseIpAddress("192.0.2.1");
    REQUIRE(parsed.has_value());
    CHECK(parsed->is_ipv4);
    CHECK(parsed->bytes[0] == 192);
    CHECK(parsed->bytes[1] == 0);
    CHECK(parsed->bytes[2] == 2);
    CHECK(parsed->bytes[3] == 1);
}

TEST_CASE("ParseIpAddress:v4 坏形状全拒") {
    for (const std::string bad : {"", "1", "1.2.3", "1.2.3.4.5", "01.2.3.4", "256.1.1.1", "1.2.3.256",
                                  "a.b.c.d", "1..2.3", ".1.2.3", "1.2.3.4.", "1.2.3.-4", " 1.2.3.4"}) {
        CHECK_FALSE(ParseIpAddress(bad).has_value());
    }
}

TEST_CASE("ParseIpAddress:v6 合法形状(压缩/内嵌 v4/zone)") {
    CHECK(ParseIpAddress("::1").has_value());
    CHECK(ParseIpAddress("::").has_value());
    CHECK(ParseIpAddress("2001:4860:4860::8888").has_value());
    CHECK(ParseIpAddress("fe80::1").has_value());
    CHECK(ParseIpAddress("fe80::1%eth0").has_value());   // zone 剥掉
    CHECK(ParseIpAddress("::ffff:192.0.2.1").has_value());  // v4 映射
    CHECK(ParseIpAddress("2001:db8::192.0.2.1").has_value());  // 内嵌 v4 尾巴
    const auto parsed = ParseIpAddress("2001:db8::a");
    REQUIRE(parsed.has_value());
    CHECK_FALSE(parsed->is_ipv4);
    CHECK(parsed->bytes[0] == 0x20);
    CHECK(parsed->bytes[1] == 0x01);
    CHECK(parsed->bytes[2] == 0x0d);
    CHECK(parsed->bytes[3] == 0xb8);
    CHECK(parsed->bytes[15] == 0x0a);
}

TEST_CASE("ParseIpAddress:v6 坏形状全拒") {
    for (const std::string bad : {"1:2:3:4:5:6:7",          // 少一段且无 "::"
                                  "1:2:3:4:5:6:7:8:9",      // 多一段
                                  "1::2::3",                // 两个 "::"
                                  "12345::", "::ffff:1.2.3.4.5", ":1111:", "gg::1"}) {
        CHECK_FALSE(ParseIpAddress(bad).has_value());
    }
}

// ---------------------------------------------------------------------------
// BlockedAddressRange:§8.2 第 4 步的段表
// ---------------------------------------------------------------------------

TEST_CASE("BlockedAddressRange:v4 禁连段全表") {
    struct Row {
        const char* ip;
        const char* label_head;  // 文案开头(段名)
    };
    const Row table[] = {
        {"0.1.2.3", "本网段"},
        {"10.0.0.5", "RFC1918"},
        {"10.255.1.1", "RFC1918"},
        {"100.64.0.1", "CGNAT"},
        {"100.100.100.200", "CGNAT"},   // 阿里云 metadata
        {"127.0.0.1", "loopback"},
        {"127.8.8.8", "loopback"},
        {"169.254.169.254", "link-local"},  // AWS/GCP/Azure metadata
        {"169.254.0.9", "link-local"},
        {"172.16.0.1", "RFC1918"},
        {"172.31.255.255", "RFC1918"},
        {"192.0.0.8", "IETF"},
        {"192.0.2.7", "TEST-NET-1"},
        {"192.88.99.1", "保留段"},
        {"192.168.1.5", "RFC1918"},
        {"198.18.0.1", "基准测试段"},
        {"198.19.255.1", "基准测试段"},
        {"198.51.100.7", "TEST-NET-2"},
        {"203.0.113.7", "TEST-NET-3"},
        {"224.0.0.1", "组播"},
        {"239.1.1.1", "组播"},
        {"240.0.0.1", "保留段"},
        {"255.255.255.255", "保留段"},
    };
    for (const Row& row : table) {
        const auto blocked = BlockedAddressRange(row.ip);
        REQUIRE(blocked.has_value());
        CHECK(blocked->find(row.label_head) != std::string::npos);
    }
}

TEST_CASE("BlockedAddressRange:v6 禁连段全表") {
    CHECK(BlockedAddressRange("::").has_value());                    // 未指定
    CHECK(BlockedAddressRange("::1").has_value());                   // loopback
    CHECK(BlockedAddressRange("::ffff:10.0.0.1").has_value());       // v4 映射 -> 私网
    CHECK(BlockedAddressRange("::ffff:169.254.169.254").has_value());  // v4 映射 -> metadata
    CHECK(BlockedAddressRange("64:ff9b::0.0.0.1").has_value());      // NAT64
    CHECK(BlockedAddressRange("100::1").has_value());                // 丢弃段
    CHECK(BlockedAddressRange("2001:db8::1").has_value());           // 文档段
    CHECK(BlockedAddressRange("fe80::1").has_value());               // link-local
    CHECK(BlockedAddressRange("fc00::1").has_value());               // ULA
    CHECK(BlockedAddressRange("fd12:3456::1").has_value());          // ULA
    CHECK(BlockedAddressRange("ff02::1").has_value());               // 组播
}

TEST_CASE("BlockedAddressRange:公网地址放行(不误伤)") {
    for (const std::string ok : {"8.8.8.8", "1.1.1.1", "93.184.216.34", "172.32.0.1", "172.15.0.1", "100.128.0.1",
                                 "2001:4860:4860::8888", "2606:4700:4700::1111"}) {
        CHECK_FALSE(BlockedAddressRange(ok).has_value());
        CHECK(IsRoutablePublicAddress(ok));
    }
    // 界石:172.15(公)/172.16(私)、100.127(CGNAT 尾)/100.128(公)。
    CHECK_FALSE(BlockedAddressRange("172.15.255.255").has_value());
    CHECK(BlockedAddressRange("172.16.0.0").has_value());
    CHECK(BlockedAddressRange("100.127.255.255").has_value());
    CHECK_FALSE(BlockedAddressRange("100.128.0.0").has_value());
}

TEST_CASE("IsLoopbackAddress:v4 127/8 与 v6 ::1") {
    CHECK(IsLoopbackAddress("127.0.0.1"));
    CHECK(IsLoopbackAddress("127.255.0.9"));
    CHECK(IsLoopbackAddress("::1"));
    CHECK_FALSE(IsLoopbackAddress("10.0.0.1"));
    CHECK_FALSE(IsLoopbackAddress("169.254.169.254"));
    CHECK_FALSE(IsLoopbackAddress("::ffff:127.0.0.1"));  // v4 映射段不是裸 loopback 判定
    CHECK_FALSE(IsLoopbackAddress("not-an-ip"));
}

// ---------------------------------------------------------------------------
// SystemDnsResolver:只解析 localhost(本地 hosts,不碰公网)
// ---------------------------------------------------------------------------

TEST_CASE("SystemDnsResolver:localhost 解析得出,且落 loopback 禁连段") {
    SystemDnsResolver resolver;
    const auto resolved = resolver.Resolve("localhost");
    if (resolved.has_value()) {
        // 有系统的 localhost 走 hosts 文件直出;无论 v4/v6 都该被禁连段拦下。
        REQUIRE(!resolved->empty());
        bool any_blocked = false;
        for (const std::string& address : *resolved) {
            if (BlockedAddressRange(address).has_value()) {
                any_blocked = true;
            }
        }
        CHECK(any_blocked);
    } else {
        // 个别精简容器没有 localhost 条目:解析失败本身也是合法终态。
        CHECK(!resolved.error().empty());
    }
}

// ---------------------------------------------------------------------------
// ClassifyCurlErrorCode:net 分型全表
// ---------------------------------------------------------------------------

TEST_CASE("ClassifyCurlErrorCode:dns/tls/timeout/其余 各就各位") {
    const auto code = [](cpr::ErrorCode value) { return static_cast<long>(value); };
    CHECK(ClassifyCurlErrorCode(code(cpr::ErrorCode::COULDNT_RESOLVE_HOST)) == FullHttpErrorKind::DnsFailed);
    CHECK(ClassifyCurlErrorCode(code(cpr::ErrorCode::COULDNT_RESOLVE_PROXY)) == FullHttpErrorKind::DnsFailed);
    CHECK(ClassifyCurlErrorCode(code(cpr::ErrorCode::SSL_CONNECT_ERROR)) == FullHttpErrorKind::TlsFailed);
    CHECK(ClassifyCurlErrorCode(code(cpr::ErrorCode::PEER_FAILED_VERIFICATION)) == FullHttpErrorKind::TlsFailed);
    CHECK(ClassifyCurlErrorCode(code(cpr::ErrorCode::SSL_CERTPROBLEM)) == FullHttpErrorKind::TlsFailed);
    CHECK(ClassifyCurlErrorCode(code(cpr::ErrorCode::SSL_PINNEDPUBKEYNOTMATCH)) == FullHttpErrorKind::TlsFailed);
    CHECK(ClassifyCurlErrorCode(code(cpr::ErrorCode::OPERATION_TIMEDOUT)) == FullHttpErrorKind::Timeout);
    CHECK(ClassifyCurlErrorCode(code(cpr::ErrorCode::COULDNT_CONNECT)) == FullHttpErrorKind::NetworkFailed);
    CHECK(ClassifyCurlErrorCode(code(cpr::ErrorCode::WEIRD_SERVER_REPLY)) == FullHttpErrorKind::NetworkFailed);
    CHECK(ClassifyCurlErrorCode(code(cpr::ErrorCode::SEND_ERROR)) == FullHttpErrorKind::NetworkFailed);
    CHECK(ClassifyCurlErrorCode(0) == FullHttpErrorKind::NetworkFailed);
}

// ---------------------------------------------------------------------------
// 真传输:本机假服务
// ---------------------------------------------------------------------------

TEST_CASE("PerformFullHttpRequest:GET query 与 POST 体字节逐字对上(连接钉地址)") {
    test_support::FakeHttpServer server;
    server.Enqueue(TextResponse(200, "ok-get"));
    server.Enqueue(TextResponse(200, "ok-post"));

    const net::PinnedDnsResolve pin = LoopbackPin(server);
    net::FullHttpLimits limits;
    limits.hard_timeout_ms = 5'000;

    net::FullHttpRequest get_request;
    get_request.method = "GET";
    get_request.url = Url(server, "/v1/search?q=luban+code&lang=zh");
    get_request.headers.emplace_back("X-Client", "luban-test");
    auto get_result = PerformFullHttpRequest(get_request, limits, nullptr, &pin);
    REQUIRE(get_result.has_value());
    CHECK(get_result->status == 200);
    CHECK(get_result->body == "ok-get");
    bool saw_content_type = false;
    for (const auto& [name, value] : get_result->headers) {
        if (name == "Content-Type") {
            saw_content_type = value == "text/plain";
        }
    }
    CHECK(saw_content_type);

    net::FullHttpRequest post_request;
    post_request.method = "POST";
    post_request.url = Url(server, "/v1/extract");
    post_request.headers.emplace_back("Content-Type", "application/json");
    post_request.body = "{\"query\":\"字节账\",\"n\":3}";
    auto post_result = PerformFullHttpRequest(post_request, limits, nullptr, &pin);
    REQUIRE(post_result.has_value());
    CHECK(post_result->status == 200);
    CHECK(post_result->body == "ok-post");

    // 假服务收到的账:目标、头、体逐字对上;Host 带的是假 DNS 名——请求
    // 能到 127.0.0.1,靠的是钉地址,系统 DNS 解析不出这个名字。
    const std::vector<test_support::FakeHttpRequest> received = server.requests();
    REQUIRE(received.size() == 2);
    CHECK(received[0].method == "GET");
    CHECK(received[0].target == "/v1/search?q=luban+code&lang=zh");
    CHECK(received[0].body.empty());
    bool saw_client_header = false;
    bool saw_host = false;
    for (const auto& [name, value] : received[0].headers) {
        if (name == "x-client") {
            saw_client_header = value == "luban-test";
        }
        if (name == "host") {
            saw_host = value.find(kFakeHost) != std::string::npos;
        }
    }
    CHECK(saw_client_header);
    CHECK(saw_host);
    CHECK(received[1].method == "POST");
    CHECK(received[1].target == "/v1/extract");
    CHECK(received[1].body == "{\"query\":\"字节账\",\"n\":3}");
}

TEST_CASE("PerformFullHttpRequest:响应头帽前后一字节") {
    test_support::FakeHttpServer server;
    // 第一笔:常规头块,帽给足(常规头块约百来字节,200 放得下)。
    server.Enqueue(TextResponse(200, "fine"));
    // 第二笔:巨响应头,同一顶小帽必炸。
    test_support::FakeHttpResponse big;
    big.status = 200;
    big.headers.emplace_back("X-Big", std::string(4'000, 'a'));
    big.body = "x";
    server.Enqueue(big);

    const net::PinnedDnsResolve pin = LoopbackPin(server);
    net::FullHttpRequest request;
    request.method = "GET";
    request.url = Url(server, "/");

    net::FullHttpLimits limits;
    limits.hard_timeout_ms = 5'000;
    limits.response_header_bytes = 200;  // 放得下常规头块,放不下巨头
    auto fits = PerformFullHttpRequest(request, limits, nullptr, &pin);
    REQUIRE(fits.has_value());
    CHECK(fits->body == "fine");

    auto blocked = PerformFullHttpRequest(request, limits, nullptr, &pin);
    REQUIRE_FALSE(blocked.has_value());
    CHECK(blocked.error().kind == FullHttpErrorKind::ResponseHeaderTooLarge);
    CHECK(blocked.error().message.find("200") != std::string::npos);
}

TEST_CASE("PerformFullHttpRequest:响应体帽前后一字节,达帽即中止") {
    test_support::FakeHttpServer server;
    server.Enqueue(TextResponse(200, std::string(100, 'x')));
    server.Enqueue(TextResponse(200, std::string(100, 'x')));

    const net::PinnedDnsResolve pin = LoopbackPin(server);
    net::FullHttpRequest request;
    request.method = "GET";
    request.url = Url(server, "/");

    net::FullHttpLimits at_cap;
    at_cap.hard_timeout_ms = 5'000;
    at_cap.response_body_bytes = 100;  // 帽前一字节:正好 100,放行
    auto fits = PerformFullHttpRequest(request, at_cap, nullptr, &pin);
    REQUIRE(fits.has_value());
    CHECK(fits->body.size() == 100);

    net::FullHttpLimits under_cap = at_cap;
    under_cap.response_body_bytes = 99;  // 帽后一字节:第 100 字节即中止
    const auto start = std::chrono::steady_clock::now();
    auto overflow = PerformFullHttpRequest(request, under_cap, nullptr, &pin);
    const auto elapsed = std::chrono::steady_clock::now() - start;
    REQUIRE_FALSE(overflow.has_value());
    CHECK(overflow.error().kind == FullHttpErrorKind::ResponseBodyTooLarge);
    CHECK(overflow.error().message.find("99") != std::string::npos);
    // 中止是立刻的,不是等服务器发完 10s 挂死。
    CHECK(elapsed < std::chrono::seconds(4));
}

TEST_CASE("PerformFullHttpRequest:墙钟分型 timeout") {
    test_support::FakeHttpServer server;
    test_support::FakeHttpResponse stall;
    stall.status = 200;
    stall.body = "never";
    stall.delay_before_response = std::chrono::seconds(10);  // 收下连接装死
    server.Enqueue(stall);

    const net::PinnedDnsResolve pin = LoopbackPin(server);
    net::FullHttpRequest request;
    request.method = "GET";
    request.url = Url(server, "/");
    net::FullHttpLimits limits;
    limits.hard_timeout_ms = 700;
    const auto start = std::chrono::steady_clock::now();
    auto result = PerformFullHttpRequest(request, limits, nullptr, &pin);
    const auto elapsed = std::chrono::steady_clock::now() - start;
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().kind == FullHttpErrorKind::Timeout);
    CHECK_FALSE(result.error().received_any_bytes);
    CHECK(elapsed < std::chrono::seconds(5));
}

TEST_CASE("PerformFullHttpRequest:等首字节阶段取消,分型 cancelled 不串 timeout") {
    test_support::FakeHttpServer server;
    test_support::FakeHttpResponse stall;
    stall.status = 200;
    stall.body = "never";
    stall.delay_before_response = std::chrono::seconds(10);
    server.Enqueue(stall);

    const net::PinnedDnsResolve pin = LoopbackPin(server);
    net::FullHttpRequest request;
    request.method = "GET";
    request.url = Url(server, "/");
    net::FullHttpLimits limits;
    limits.hard_timeout_ms = 30'000;  // 墙钟放足,只看取消

    std::atomic<bool> cancel{false};
    std::thread canceller([&cancel]() {
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
        cancel.store(true);
    });
    const auto start = std::chrono::steady_clock::now();
    auto result = PerformFullHttpRequest(request, limits, &cancel, &pin);
    const auto elapsed = std::chrono::steady_clock::now() - start;
    canceller.join();
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().kind == FullHttpErrorKind::Cancelled);
    CHECK(elapsed < std::chrono::seconds(5));
}

TEST_CASE("PerformFullHttpRequest:读到半截响应体时取消") {
    test_support::FakeHttpServer server;
    test_support::FakeHttpResponse partial;
    partial.status = 200;
    partial.body = std::string(10'000, 'y');
    partial.stall_after_body_bytes = 64;  // 发 64 字节后挂死
    server.Enqueue(partial);

    const net::PinnedDnsResolve pin = LoopbackPin(server);
    net::FullHttpRequest request;
    request.method = "GET";
    request.url = Url(server, "/");
    net::FullHttpLimits limits;
    limits.hard_timeout_ms = 30'000;

    std::atomic<bool> cancel{false};
    std::thread canceller([&cancel]() {
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        cancel.store(true);
    });
    auto result = PerformFullHttpRequest(request, limits, &cancel, &pin);
    canceller.join();
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().kind == FullHttpErrorKind::Cancelled);
    CHECK(result.error().received_any_bytes);
}

TEST_CASE("PerformFullHttpRequest:调用前已置位,直接 cancelled 不发包") {
    test_support::FakeHttpServer server;
    server.Enqueue(TextResponse(200, "unused"));

    const net::PinnedDnsResolve pin = LoopbackPin(server);
    net::FullHttpRequest request;
    request.method = "GET";
    request.url = Url(server, "/");
    net::FullHttpLimits limits;
    limits.hard_timeout_ms = 5'000;

    std::atomic<bool> cancel{true};
    auto result = PerformFullHttpRequest(request, limits, &cancel, &pin);
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().kind == FullHttpErrorKind::Cancelled);
    // 进度/写回调都没轮到跑,连接都没起。
    CHECK(server.connection_count() == 0);
}

TEST_CASE("PerformFullHttpRequest:非 2xx 与 3xx 原样交,不跟重定向") {
    test_support::FakeHttpServer server;
    test_support::FakeHttpResponse moved;
    moved.status = 301;
    moved.headers.emplace_back("Location", "http://elsewhere.example.test/x");
    moved.body = "";
    server.Enqueue(moved);

    test_support::FakeHttpResponse not_found;
    not_found.status = 404;
    not_found.headers.emplace_back("Content-Type", "application/json");
    not_found.body = "{\"error\":\"nope\"}";
    server.Enqueue(not_found);

    const net::PinnedDnsResolve pin = LoopbackPin(server);
    net::FullHttpRequest request;
    request.method = "GET";
    request.url = Url(server, "/old");
    net::FullHttpLimits limits;
    limits.hard_timeout_ms = 5'000;

    auto redirect = PerformFullHttpRequest(request, limits, nullptr, &pin);
    REQUIRE(redirect.has_value());
    CHECK(redirect->status == 301);

    auto missing = PerformFullHttpRequest(request, limits, nullptr, &pin);
    REQUIRE(missing.has_value());
    CHECK(missing->status == 404);
    CHECK(missing->body == "{\"error\":\"nope\"}");

    // 3xx 只来了一份请求:没跟。
    CHECK(server.requests().size() == 2);
    CHECK(server.connection_count() == 2);
}

TEST_CASE("PerformFullHttpRequest:重复响应头按到达顺序保留") {
    test_support::FakeHttpServer server;
    test_support::FakeHttpResponse response;
    response.status = 200;
    response.headers.emplace_back("X-Multi", "one");
    response.headers.emplace_back("X-Multi", "two");
    response.body = "x";
    server.Enqueue(response);

    const net::PinnedDnsResolve pin = LoopbackPin(server);
    net::FullHttpRequest request;
    request.method = "GET";
    request.url = Url(server, "/");
    net::FullHttpLimits limits;
    auto result = PerformFullHttpRequest(request, limits, nullptr, &pin);
    REQUIRE(result.has_value());
    int seen = 0;
    int values = 0;
    for (const auto& [name, value] : result->headers) {
        if (name == "X-Multi") {
            ++seen;
            values += (value == "one" ? 1 : 2);
        }
    }
    CHECK(seen == 2);
    CHECK(values == 3);
}
