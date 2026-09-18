// LocalWebServer 册(常驻助理 Web 主界面单 W1,§七合同的逐面验收):
//   1. 同源纯函数:Host/Origin 的回环判定、Cookie 挑值;
//   2. 启动门:manifest 缺资源明错拒启(不起空壳)、非回环绑定拒、指定
//      端口被占准确报错(带端口号,不偷换);
//   3. 静态资源:无 cookie 可加载页面壳、有 cookie 200(带 CSP/
//      nosniff)、manifest 外 404、坏 Host 403、跨源 Origin 403;
//   4. bootstrap 交换:对凭据 204 + Set-Cookie(HttpOnly/SameSite=Strict),
//      重放 403,坏凭据 403;
//   5. /healthz 不鉴权只回身份;/control/open 坏控制凭据 403、对的回新
//      bootstrap URL;
//   6. WS 升级:无 cookie 401 不应 101;有 cookie 应 101 并交棒(带请求头)。
// 全部真监听回环 127.0.0.1 + 系统分配端口(端口冲突案显式占一个)。
#include <doctest/doctest.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include <nlohmann/json.hpp>

#include "app_server/local_web_server.hpp"
#include "app_server/ws_frames.hpp"
#include "app_server/ws_sockets.hpp"
#include "tools/path_utils.hpp"

using namespace lubancode;

namespace {

// ---- 临时资源根:一枚目录、几枚白名单文件、一枚"不在 manifest 里的
// 私文件"(验白名单墙)。 ----
struct AssetsDir {
    std::filesystem::path root;

    explicit AssetsDir(const char* name) {
        root = std::filesystem::temp_directory_path() / name;
        std::error_code ec;
        std::filesystem::remove_all(root, ec);
        std::filesystem::create_directories(root, ec);
        Write("index.html", "<!DOCTYPE html><html><body>assistant page</body></html>");
        Write("assistant.css", "body{margin:0}");
        Write("assistant_core.js", "'use strict';");
        Write("assistant_app.js", "'use strict';");
        Write("secret.txt", "not-in-manifest");
    }

    void Write(const std::string& name, const std::string& text) {
        std::ofstream out(root / tools::Utf8ToPath(name), std::ios::binary);
        out << text;
    }

    ~AssetsDir() {
        std::error_code ec;
        std::filesystem::remove_all(root, ec);
    }
};

// ---- 测试用的门:一次性 bootstrap、会话 cookie、控制凭据 ----
struct TestAuth {
    std::string bootstrap_secret = "feedfacefeedfacefeedfacefeedface";
    std::string control_secret = "0123456789abcdef0123456789abcdef";
    std::string session_value;      // 交换成功后发出去的会话值
    bool bootstrap_used = false;
    std::map<std::string, bool> sessions;

    std::optional<std::string> ConsumeBootstrap(const std::string& given) {
        if (bootstrap_used || given != bootstrap_secret) {
            return std::nullopt;  // 用过即焚/不对——一路 403
        }
        bootstrap_used = true;
        session_value = "session-" + std::to_string(++issued);
        sessions[session_value] = true;
        return session_value;
    }

    bool ValidateSession(const std::string& value) {
        return sessions.count(value) > 0;
    }

    std::optional<nlohmann::json> HandleOpen(const std::string& given, int port) {
        if (given != control_secret) {
            return std::nullopt;
        }
        return nlohmann::json{{"url", "http://127.0.0.1:" + std::to_string(port) + "/#b=fresh"}};
    }

private:
    int issued = 0;
};

// ---- 裸 HTTP 客户端:发一段、读到对端关(Connection: close) ----
struct HttpReply {
    int status = 0;
    std::string header;
    std::string body;
};

class RawHttpClient {
public:
    explicit RawHttpClient(int port) {
        std::string error;
        socket_ = app_server::net::ConnectTcp("127.0.0.1", port, error);
        REQUIRE_MESSAGE(socket_.valid(), ("connect 失败: " + error).c_str());
    }

    HttpReply RoundTrip(const std::string& raw) {
        REQUIRE(socket_.SendAll(raw));
        HttpReply reply;
        std::string all;
        char buffer[4096];
        socket_.SetRecvTimeoutMs(2000);
        while (true) {
            const long got = socket_.Recv(buffer, sizeof(buffer));
            if (got <= 0) {
                break;
            }
            all.append(buffer, buffer + got);
        }
        const std::size_t split = all.find("\r\n\r\n");
        if (split == std::string::npos) {
            return reply;
        }
        reply.header = all.substr(0, split);
        reply.body = all.substr(split + 4);
        reply.status = std::atoi(all.c_str() + 9);
        return reply;
    }

private:
    app_server::net::Socket socket_;
};

std::string Get(int port, const std::string& target, const std::string& extra_headers = "") {
    return "GET " + target + " HTTP/1.1\r\nHost: 127.0.0.1:" + std::to_string(port) +
           "\r\nConnection: close\r\n" + extra_headers + "\r\n";
}

std::string Post(int port, const std::string& target, const std::string& body) {
    return "POST " + target + " HTTP/1.1\r\nHost: 127.0.0.1:" + std::to_string(port) +
           "\r\nContent-Type: text/plain; charset=utf-8\r\nContent-Length: " +
           std::to_string(body.size()) + "\r\nConnection: close\r\n\r\n" + body;
}

// 服务台:一条线程跑 LocalWebServer::Run,交出的 WS Session 记账后收线。
struct WebServerHarness {
    AssetsDir assets;
    TestAuth auth;
    app_server::LocalWebServer server;
    std::thread runner;
    std::atomic<int> ws_sessions{0};
    std::atomic<bool> started{false};

    explicit WebServerHarness(const char* name, int port = 0)
        : assets(name),
          server(MakeOptions(port)) {}

    app_server::LocalWebOptions MakeOptions(int port) {
        app_server::LocalWebOptions options;
        options.port = port;
        options.assets_root = assets.root;
        options.manifest = app_server::AssistantWebManifest();
        options.validate_session = [this](const std::string& value) {
            return auth.ValidateSession(value);
        };
        options.consume_bootstrap = [this](const std::string& secret) {
            return auth.ConsumeBootstrap(secret);
        };
        options.health_body = []() {
            return nlohmann::json{{"ok", true}, {"bootId", "boot-test"}, {"service", "lubancode-assistant"}};
        };
        const std::string control = auth.control_secret;
        options.handle_open = [this, control](const std::string& secret) {
            const int port_now = server.actual_port();
            return auth.HandleOpen(secret, port_now);
        };
        return options;
    }

    void Start() {
        REQUIRE_MESSAGE(server.Start(), server.last_error().c_str());
        started = true;
        runner = std::thread([this] {
            server.Run([this](std::unique_ptr<app_server::WsTransport::Session> session,
                              const app_server::ws::HttpRequestHead& head) {
                ++ws_sessions;
                CHECK(head.method == "GET");  // 升级请求都是 GET
                session->Close();             // 测试里不开协议线,收线即可
            });
        });
    }

    int port() const { return server.actual_port(); }

    ~WebServerHarness() {
        server.Stop();
        if (runner.joinable()) {
            runner.join();
        }
    }
};

// 拿一枚有效会话 cookie(走真 bootstrap 交换)。
std::string ExchangeForCookie(int port, const std::string& secret) {
    RawHttpClient client(port);
    const HttpReply reply = client.RoundTrip(Post(port, "/auth/exchange", secret));
    REQUIRE(reply.status == 204);
    const std::size_t cookie_at = reply.header.find("Set-Cookie: lubancode_assistant_session=");
    REQUIRE(cookie_at != std::string::npos);
    const std::size_t value_at = cookie_at + std::string("Set-Cookie: lubancode_assistant_session=").size();
    const std::size_t line_end = reply.header.find("\r\n", value_at);
    const std::string cookie_line =
        reply.header.substr(value_at, line_end == std::string::npos ? std::string::npos : line_end - value_at);
    CHECK(cookie_line.find("HttpOnly") != std::string::npos);
    CHECK(cookie_line.find("SameSite=Strict") != std::string::npos);
    return cookie_line.substr(0, cookie_line.find(';'));
}

}  // namespace

// ---------------------------------------------------------------------------
// 纯函数
// ---------------------------------------------------------------------------

TEST_CASE("local web:Host/Origin 的回环判定") {
    CHECK(app_server::LocalWebServer::HostIsLocalLoopback("127.0.0.1:8765", 8765));
    CHECK(app_server::LocalWebServer::HostIsLocalLoopback("localhost:8765", 8765));
    CHECK(app_server::LocalWebServer::HostIsLocalLoopback("LOCALHOST:8765", 8765));
    CHECK_FALSE(app_server::LocalWebServer::HostIsLocalLoopback("127.0.0.1:8765", 8766));  // 端口不对
    CHECK_FALSE(app_server::LocalWebServer::HostIsLocalLoopback("evil.example.com:8765", 8765));  // rebinding
    CHECK_FALSE(app_server::LocalWebServer::HostIsLocalLoopback("127.0.0.1", 8765));       // 缺端口
    CHECK_FALSE(app_server::LocalWebServer::HostIsLocalLoopback("", 8765));

    CHECK(app_server::LocalWebServer::OriginIsLocalSame("http://127.0.0.1:8765", 8765));
    CHECK(app_server::LocalWebServer::OriginIsLocalSame("http://localhost:8765", 8765));
    CHECK_FALSE(app_server::LocalWebServer::OriginIsLocalSame("http://evil.example.com", 8765));
    CHECK_FALSE(app_server::LocalWebServer::OriginIsLocalSame("null", 8765));
    CHECK_FALSE(app_server::LocalWebServer::OriginIsLocalSame("http://127.0.0.1:9999", 8765));
}

TEST_CASE("local web:Cookie 头挑值") {
    CHECK(app_server::ws::CookieValue("lubancode_assistant_session=abc; other=1",
                                      "lubancode_assistant_session") == "abc");
    CHECK(app_server::ws::CookieValue("other=1; lubancode_assistant_session=xyz",
                                      "lubancode_assistant_session") == "xyz");
    CHECK(app_server::ws::CookieValue("lubancode_assistant_session=", "lubancode_assistant_session").empty());
    CHECK(app_server::ws::CookieValue("", "lubancode_assistant_session").empty());
    CHECK(app_server::ws::CookieValue("a=b", "missing").empty());
}

// ---------------------------------------------------------------------------
// 启动门
// ---------------------------------------------------------------------------

TEST_CASE("local web:manifest 缺资源明错拒启,不起空壳") {
    AssetsDir assets("lubancode_test_webui_missing");
    app_server::LocalWebOptions options;
    options.assets_root = assets.root;
    options.manifest = app_server::AssistantWebManifest();
    assets.root /= tools::Utf8ToPath("index.html");
    std::error_code ec;
    std::filesystem::remove(assets.root, ec);  // 抽掉一枚
    app_server::LocalWebServer server(std::move(options));
    CHECK_FALSE(server.Start());
    CHECK(server.last_error().find("index.html") != std::string::npos);
    CHECK(server.last_error().find("空壳") != std::string::npos);
}

TEST_CASE("local web:非回环绑定拒启") {
    AssetsDir assets("lubancode_test_webui_nonloopback");
    app_server::LocalWebOptions options;
    options.bind_host = "0.0.0.0";
    options.assets_root = assets.root;
    options.manifest = app_server::AssistantWebManifest();
    app_server::LocalWebServer server(std::move(options));
    CHECK_FALSE(server.Start());
    CHECK(server.last_error().find("回环") != std::string::npos);
}

TEST_CASE("local web:指定端口被占准确报错,不偷换端口") {
    // 先占一个端口。
    std::string error;
    app_server::net::Listener squatter;
    REQUIRE(squatter.Start("127.0.0.1", 0, error));
    const int taken_port = squatter.actual_port();

    AssetsDir assets("lubancode_test_webui_portclash");
    app_server::LocalWebOptions options;
    options.port = taken_port;
    options.assets_root = assets.root;
    options.manifest = app_server::AssistantWebManifest();
    app_server::LocalWebServer server(std::move(options));
    CHECK_FALSE(server.Start());
    CHECK(server.last_error().find(std::to_string(taken_port)) != std::string::npos);
    CHECK(server.last_error().find("不偷偷换端口") != std::string::npos);
    squatter.Stop();
}

// ---------------------------------------------------------------------------
// 资源与认证门
// ---------------------------------------------------------------------------

TEST_CASE("local web:静态页面壳允许首次加载,同源门在前") {
    WebServerHarness harness("lubancode_test_webui_static");
    harness.Start();
    const int port = harness.port();

    SUBCASE("无 cookie:页面与脚本可加载,才能交换 bootstrap") {
        for (const std::string& target : {"/", "/index.html", "/assistant.css", "/assistant_core.js", "/assistant_app.js"}) {
            RawHttpClient client(port);
            const HttpReply reply = client.RoundTrip(Get(port, target));
            CHECK(reply.status == 200);
            CHECK_FALSE(reply.body.empty());
            CHECK(reply.header.find("Content-Security-Policy:") != std::string::npos);
        }
    }

    SUBCASE("无 cookie:artifact 仍拒绝访问") {
        RawHttpClient client(port);
        const HttpReply reply = client.RoundTrip(Get(port, "/artifact/private.png"));
        CHECK(reply.status == 401);
    }

    SUBCASE("有 cookie:200 + CSP/nosniff 头") {
        const std::string cookie = ExchangeForCookie(port, harness.auth.bootstrap_secret);
        RawHttpClient client(port);
        const HttpReply reply = client.RoundTrip(Get(port, "/", "Cookie: lubancode_assistant_session=" + cookie + "\r\n"));
        CHECK(reply.status == 200);
        CHECK(reply.body.find("assistant page") != std::string::npos);
        CHECK(reply.header.find("Content-Security-Policy:") != std::string::npos);
        CHECK(reply.header.find("X-Content-Type-Options: nosniff") != std::string::npos);
    }

    SUBCASE("manifest 外的文件:404(白名单就是墙)") {
        const std::string cookie = ExchangeForCookie(port, harness.auth.bootstrap_secret);
        RawHttpClient client(port);
        const HttpReply reply = client.RoundTrip(Get(port, "/secret.txt", "Cookie: lubancode_assistant_session=" + cookie + "\r\n"));
        CHECK(reply.status == 404);
        CHECK(reply.body.find("not-in-manifest") == std::string::npos);
    }

    SUBCASE("目录穿越形状:404") {
        const std::string cookie = ExchangeForCookie(port, harness.auth.bootstrap_secret);
        for (const std::string& target :
             {"/../secret.txt", "/%2e%2e/secret.txt", "/..\\secret.txt", "/a/b/c.js"}) {
            RawHttpClient client(port);
            const HttpReply reply =
                client.RoundTrip(Get(port, target, "Cookie: lubancode_assistant_session=" + cookie + "\r\n"));
            CHECK(reply.status == 404);
        }
    }

    SUBCASE("坏 Host(DNS rebinding):403,cookie 对也不放") {
        const std::string cookie = ExchangeForCookie(port, harness.auth.bootstrap_secret);
        RawHttpClient client(port);
        const HttpReply reply = client.RoundTrip(
            "GET / HTTP/1.1\r\nHost: evil.example.com:" + std::to_string(port) +
            "\r\nCookie: lubancode_assistant_session=" + cookie + "\r\nConnection: close\r\n\r\n");
        CHECK(reply.status == 403);
    }

    SUBCASE("跨源 Origin:403") {
        const std::string cookie = ExchangeForCookie(port, harness.auth.bootstrap_secret);
        RawHttpClient client(port);
        const HttpReply reply = client.RoundTrip(Get(
            port, "/", "Origin: http://evil.example.com\r\nCookie: lubancode_assistant_session=" + cookie + "\r\n"));
        CHECK(reply.status == 403);
    }
}

TEST_CASE("local web:bootstrap 交换——发放、防重放、坏凭据") {
    WebServerHarness harness("lubancode_test_webui_bootstrap");
    harness.Start();
    const int port = harness.port();

    // 第一次:204 + Set-Cookie(HttpOnly/SameSite=Strict 在 ExchangeForCookie 里断言)。
    const std::string cookie = ExchangeForCookie(port, harness.auth.bootstrap_secret);
    CHECK_FALSE(cookie.empty());

    // 重放:403(一次性,防重放)。
    {
        RawHttpClient client(port);
        const HttpReply reply = client.RoundTrip(Post(port, "/auth/exchange", harness.auth.bootstrap_secret));
        CHECK(reply.status == 403);
    }
    // 坏凭据:403,话面一致。
    {
        RawHttpClient client(port);
        const HttpReply reply = client.RoundTrip(Post(port, "/auth/exchange", "deadbeefdeadbeef"));
        CHECK(reply.status == 403);
        CHECK(reply.body.find("not accepted") != std::string::npos);
    }
    // 跨源的交换请求也拒。
    {
        RawHttpClient client(port);
        const HttpReply reply = client.RoundTrip(
            "POST /auth/exchange HTTP/1.1\r\nHost: 127.0.0.1:" + std::to_string(port) +
            "\r\nOrigin: http://evil.example.com\r\nContent-Length: " +
            std::to_string(harness.auth.bootstrap_secret.size()) + "\r\nConnection: close\r\n\r\n" +
            harness.auth.bootstrap_secret);
        CHECK(reply.status == 403);
    }
}

TEST_CASE("local web:/healthz 不鉴权只回身份;/control/open 吃控制凭据") {
    WebServerHarness harness("lubancode_test_webui_control");
    harness.Start();
    const int port = harness.port();

    SUBCASE("healthz:身份 JSON,零秘密") {
        RawHttpClient client(port);
        const HttpReply reply = client.RoundTrip(Get(port, "/healthz"));
        CHECK(reply.status == 200);
        const nlohmann::json body = nlohmann::json::parse(reply.body, nullptr, false);
        REQUIRE(!body.is_discarded());
        CHECK(body["ok"] == true);
        CHECK(body["bootId"] == "boot-test");
        CHECK_FALSE(body.contains("controlSecret"));
    }

    SUBCASE("control/open:坏凭据 403,对的回新 URL") {
        {
            RawHttpClient client(port);
            const HttpReply reply = client.RoundTrip(Post(port, "/control/open", "wrong-secret-wrong"));
            CHECK(reply.status == 403);
        }
        RawHttpClient client(port);
        const HttpReply reply =
            client.RoundTrip(Post(port, "/control/open", harness.auth.control_secret));
        CHECK(reply.status == 200);
        const nlohmann::json body = nlohmann::json::parse(reply.body, nullptr, false);
        REQUIRE(!body.is_discarded());
        CHECK(body["url"].get<std::string>().rfind("http://127.0.0.1:", 0) == 0);
        CHECK(body["url"].get<std::string>().find("#b=") != std::string::npos);
    }
}

TEST_CASE("local web:WS 升级过会话门——无 cookie 401,有 cookie 应 101 并交棒") {
    WebServerHarness harness("lubancode_test_webui_ws");
    harness.Start();
    const int port = harness.port();
    const std::string key = "dGhlIHNhbXBsZSBub25jZQ==";

    SUBCASE("无 cookie:401,不应 101") {
        RawHttpClient client(port);
        const HttpReply reply = client.RoundTrip(
            "GET /ws HTTP/1.1\r\nHost: 127.0.0.1:" + std::to_string(port) +
            "\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Key: " + key +
            "\r\nSec-WebSocket-Version: 13\r\n\r\n");
        CHECK(reply.status == 401);
        CHECK(harness.ws_sessions.load() == 0);
    }

    SUBCASE("有 cookie:101,Session 交棒(带请求头)") {
        const std::string cookie = ExchangeForCookie(port, harness.auth.bootstrap_secret);
        app_server::net::Socket socket;
        std::string error;
        socket = app_server::net::ConnectTcp("127.0.0.1", port, error);
        REQUIRE(socket.valid());
        const std::string request =
            "GET /ws?takeover=1 HTTP/1.1\r\nHost: 127.0.0.1:" + std::to_string(port) +
            "\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Key: " + key +
            "\r\nSec-WebSocket-Version: 13\r\nCookie: lubancode_assistant_session=" + cookie +
            "\r\n\r\n";
        REQUIRE(socket.SendAll(request));
        socket.SetRecvTimeoutMs(2000);
        std::string head;
        char buffer[1024];
        while (head.find("\r\n\r\n") == std::string::npos) {
            const long got = socket.Recv(buffer, sizeof(buffer));
            if (got <= 0) {
                break;
            }
            head.append(buffer, buffer + got);
        }
        CHECK(head.find("HTTP/1.1 101") != std::string::npos);
        CHECK(head.find("Sec-WebSocket-Accept: s3pPLMBiTxaQ9kYGzzhZRbK+xOo=") != std::string::npos);
        // 交棒发生(harness 的回调收线;稍等记账)。
        for (int i = 0; i < 100 && harness.ws_sessions.load() == 0; ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        CHECK(harness.ws_sessions.load() == 1);
    }
}
