// WS 端口只读 artifact 面(多前端外壳单阶段 D)的测试册:
//   1. 纯函数层:HTTP 请求头解析(方法/目标/查询串/Bearer)、查询参数取值
//      (百分号解码)、内容寻址名形状(穿越一律不认)、应答拼装;
//   2. 真监听回环:GET /artifact 一幕幕——字节与头、404(没这枚/坏形状/
//      没配目录)、token 门(403/放行,token 不落应答)、GET 之后 WS 升级
//      照常(承载面互不搅)。
// 真监听走 127.0.0.1 + 系统分配端口(port 0),不起进程、不碰 stdio。
#include <doctest/doctest.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <future>
#include <memory>
#include <string>
#include <string_view>
#include <thread>

#include "app_server/ws_frames.hpp"
#include "app_server/ws_sockets.hpp"
#include "app_server/ws_transport.hpp"

using namespace lubancode;

namespace {

// ---- 夹具:临时 artifact 目录 + 预放的文件 ----

std::string MakeTempDir(const char* name) {
    const std::filesystem::path dir = std::filesystem::temp_directory_path() / name;
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    std::filesystem::create_directories(dir, ec);
    return dir.string();
}

void PlantFile(const std::string& dir, const std::string& name, std::string_view bytes) {
    std::ofstream file(std::filesystem::path(dir) / name, std::ios::binary);
    file.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
}

// 最小 PNG 头(8 字节签名)+ 假正文;镜像帧/截图在盘上就是这个形状。
std::string PngBytes(std::string_view marker) {
    std::string bytes = std::string("\x89PNG\r\n\x1a\n", 8);
    bytes.append(marker);
    return bytes;
}

// ---- 测试用 HTTP 客户端:裸 socket,一条 GET 读到连接关 ----

class RawHttpClient {
public:
    explicit RawHttpClient(int port) {
        std::string error;
        socket_ = app_server::net::ConnectTcp("127.0.0.1", port, error);
        REQUIRE_MESSAGE(socket_.valid(), ("connect 失败: " + error).c_str());
        socket_.SetRecvTimeoutMs(500);
    }

    std::string Get(std::string_view target, std::string_view extra_headers = {}) {
        std::string request;
        request += "GET ";
        request += target;
        request += " HTTP/1.1\r\nHost: 127.0.0.1\r\n";
        request += extra_headers;
        request += "\r\n";
        if (!socket_.SendAll(request)) {
            return {};
        }
        std::string response;
        char buffer[8192];
        while (true) {
            const long got = socket_.Recv(buffer, sizeof(buffer));
            if (got <= 0) {
                break; // 对端关了(Connection: close)或静默超时
            }
            response.append(buffer, buffer + got);
        }
        return response;
    }

    // 发一段裸字节(升级请求用),把应答读到关/超时。
    std::string SendRaw(std::string_view bytes) {
        if (!socket_.SendAll(bytes)) {
            return {};
        }
        std::string response;
        char buffer[8192];
        while (true) {
            const long got = socket_.Recv(buffer, sizeof(buffer));
            if (got <= 0) {
                break;
            }
            response.append(buffer, buffer + got);
        }
        return response;
    }

    void Close() { socket_.Close(); }

private:
    app_server::net::Socket socket_;
};

// 应答解剖:状态码 / 头 / 正文。
struct HttpResponseShape {
    int status = 0;
    std::string content_type;
    bool has_cors = false;
    bool has_cache = false;
    std::string body;
};

HttpResponseShape ParseResponse(const std::string& response) {
    HttpResponseShape shape;
    const std::size_t header_end = response.find("\r\n\r\n");
    if (response.rfind("HTTP/1.1 ", 0) != 0) {
        return shape;
    }
    shape.status = std::stoi(response.substr(9, 3));
    const std::string header = response.substr(0, header_end);
    const auto has = [&](std::string_view name) {
        return header.find(name) != std::string::npos;
    };
    shape.has_cors = has("Access-Control-Allow-Origin: *");
    shape.has_cache = has("Cache-Control:");
    const std::size_t type_at = header.find("Content-Type: ");
    if (type_at != std::string::npos) {
        const std::size_t line_end = header.find("\r\n", type_at);
        shape.content_type = header.substr(type_at + 14, line_end - type_at - 14);
    }
    if (header_end != std::string::npos) {
        shape.body = response.substr(header_end + 4);
    }
    return shape;
}

// 一台带 artifact 目录的测试 transport(Accept 由调用方起线程)。目录与
// 文件由调用方先备好;transport 不可移动,unique_ptr 持有。
struct ArtifactHarness {
    std::unique_ptr<app_server::WsTransport> transport;

    explicit ArtifactHarness(const std::string& artifact_dir, const std::string& token = "") {
        app_server::WsOptions options;
        options.bind_host = "127.0.0.1";
        options.port = 0;
        options.token = token;
        options.artifact_dir = artifact_dir;
        transport = std::make_unique<app_server::WsTransport>(options);
        REQUIRE(transport->Start());
    }

    int port() const { return transport->actual_port(); }
};

}  // namespace

// ---------------------------------------------------------------------------
// 纯函数:HTTP 请求头解析与查询参数
// ---------------------------------------------------------------------------

TEST_CASE("artifact 面:HTTP 请求头解析(方法/目标/查询/Bearer)") {
    const std::string request =
        "GET /artifact/art-0123456789abcdef.png?token=a%20b HTTP/1.1\r\n"
        "Host: 127.0.0.1\r\n"
        "authorization: Bearer s3cret-token\r\n"
        "User-Agent: test\r\n"
        "\r\n";
    const auto head = app_server::ws::ParseHttpRequestHead(request);
    CHECK(head.method == "GET");
    CHECK(head.target == "/artifact/art-0123456789abcdef.png");
    CHECK(head.query == "token=a%20b");
    CHECK(head.bearer_token == "s3cret-token"); // 头名不区分大小写

    // 没有 Authorization:普通请求,Bearer 空。
    const auto plain = app_server::ws::ParseHttpRequestHead(
        "GET /artifact/art-01234567.png HTTP/1.1\r\nHost: x\r\n\r\n");
    CHECK(plain.method == "GET");
    CHECK(plain.target == "/artifact/art-01234567.png");
    CHECK(plain.query.empty());
    CHECK(plain.bearer_token.empty());

    // Authorization 不是 Bearer 法:不认(空)。
    const auto basic = app_server::ws::ParseHttpRequestHead(
        "GET /x HTTP/1.1\r\nAuthorization: Basic abc\r\n\r\n");
    CHECK(basic.bearer_token.empty());

    // Bearer 方案名大小写不敏感(凭据本体原样)。
    const auto shouty = app_server::ws::ParseHttpRequestHead(
        "GET /x HTTP/1.1\r\nAuthorization: BEARER the-token\r\n\r\n");
    CHECK(shouty.bearer_token == "the-token");
    const auto quiet = app_server::ws::ParseHttpRequestHead(
        "GET /x HTTP/1.1\r\nAuthorization: bearer the-token\r\n\r\n");
    CHECK(quiet.bearer_token == "the-token");

    // 坏形状(没有请求行):method 空,调用方按非 GET 落 400。
    const auto junk = app_server::ws::ParseHttpRequestHead("\r\n\r\n");
    CHECK(junk.method.empty());
}

TEST_CASE("artifact 面:查询参数取值与百分号解码") {
    CHECK(app_server::ws::QueryParam("token=abc", "token") == "abc");
    CHECK(app_server::ws::QueryParam("a=1&token=xyz&b=2", "token") == "xyz");
    // %20 解码成空格;+ 也当空格(application/x-www-form-urlencoded 惯例)。
    CHECK(app_server::ws::QueryParam("token=a%20b", "token") == "a b");
    CHECK(app_server::ws::QueryParam("token=a+b", "token") == "a b");
    // 没有该参数 / 只有键没有值 / 尾巴孤 %:给空串或原样,不炸。
    CHECK(app_server::ws::QueryParam("a=1", "token").empty());
    CHECK(app_server::ws::QueryParam("token=", "token").empty());
    CHECK(app_server::ws::QueryParam("token=100%", "token") == "100%");
    CHECK(app_server::ws::QueryParam("", "token").empty());
}

TEST_CASE("artifact 面:内容寻址名只认一个形状") {
    // 好名字:art-<hex 8..64>.(png|jpeg|jpg)。
    CHECK(app_server::ws::IsValidArtifactName("art-01234567.png"));
    CHECK(app_server::ws::IsValidArtifactName("art-deadbeef.jpeg"));
    CHECK(app_server::ws::IsValidArtifactName("art-abcdef01.jpg"));
    CHECK(app_server::ws::IsValidArtifactName("art-0123456789abcdef0123456789abcdef.png"));
    // 坏名字:hex 不足/超长/非 hex、双点、无点、扩展名不对、穿越花样。
    CHECK_FALSE(app_server::ws::IsValidArtifactName("art-1234567.png"));       // hex 7 位
    CHECK_FALSE(app_server::ws::IsValidArtifactName(
        "art-0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef0.png")); // hex 65 位
    CHECK_FALSE(app_server::ws::IsValidArtifactName("art-zzzzzzzz.png"));      // 非 hex
    CHECK_FALSE(app_server::ws::IsValidArtifactName("art-0123456Z.png"));      // 夹了字母
    CHECK_FALSE(app_server::ws::IsValidArtifactName("art-01234567.PNG"));      // 扩展名大小写敏感
    CHECK_FALSE(app_server::ws::IsValidArtifactName("art-01234567.gif"));
    CHECK_FALSE(app_server::ws::IsValidArtifactName("art-01234567.png.bak"));  // 双点
    CHECK_FALSE(app_server::ws::IsValidArtifactName("art-01234567"));          // 无扩展名
    CHECK_FALSE(app_server::ws::IsValidArtifactName("../secret.txt"));         // 穿越
    CHECK_FALSE(app_server::ws::IsValidArtifactName("sub/art-01234567.png"));  // 带路径
    CHECK_FALSE(app_server::ws::IsValidArtifactName("art-01234567.png%00"));   // 百分号尾巴
    CHECK_FALSE(app_server::ws::IsValidArtifactName(""));
    CHECK_FALSE(app_server::ws::IsValidArtifactName("art-.png"));
}

TEST_CASE("artifact 面:HTTP 应答拼装(头与正文字节)") {
    const std::string response = app_server::ws::MakeHttpResponse(
        "200 OK", "image/png", std::string_view("\x89PNG", 4),
        "Access-Control-Allow-Origin: *\r\n");
    CHECK(response.rfind("HTTP/1.1 200 OK\r\n", 0) == 0);
    CHECK(response.find("Connection: close\r\n") != std::string::npos);
    CHECK(response.find("Content-Type: image/png\r\n") != std::string::npos);
    CHECK(response.find("Content-Length: 4\r\n") != std::string::npos);
    CHECK(response.find("Access-Control-Allow-Origin: *\r\n") != std::string::npos);
    CHECK(response.substr(response.size() - 6) == "\r\n\x89PNG"); // 头收尾 + 正文原样
}

// ---------------------------------------------------------------------------
// 真监听回环:GET /artifact 一幕一幕
// ---------------------------------------------------------------------------

TEST_CASE("ws artifact 面:字节与头(200),没这枚/坏形状/没配目录(404)") {
    const std::string dir = MakeTempDir("lubancode_test_ws_artifact");
    const std::string png = PngBytes("frame-bytes");
    PlantFile(dir, "art-01234567.png", png);
    PlantFile(dir, "art-89abcdef.jpg", "jpeg-marker");
    PlantFile(dir, "secret.txt", "TOP-SECRET-BYTES"); // 目录里的非 artifact 文件

    ArtifactHarness harness(dir);
    std::thread accept_thread([&] {
        while (harness.transport->Accept() != nullptr) {
            // Accept 对 GET 就地应答继续等;这里只在拿到真 Session(升级)
            // 时才非空——本册不升级,循环靠 Stop 收口。
        }
    });

    // 200:字节原样,头齐(png)。
    {
        RawHttpClient client(harness.port());
        const auto shape = ParseResponse(client.Get("/artifact/art-01234567.png"));
        CHECK(shape.status == 200);
        CHECK(shape.content_type == "image/png");
        CHECK(shape.body == png);
        CHECK(shape.has_cors);
        CHECK(shape.has_cache);
    }
    // 200:jpg 与 jpeg 都按 image/jpeg。
    {
        RawHttpClient client(harness.port());
        const auto shape = ParseResponse(client.Get("/artifact/art-89abcdef.jpg"));
        CHECK(shape.status == 200);
        CHECK(shape.content_type == "image/jpeg");
        CHECK(shape.body == "jpeg-marker");
    }
    // 404:形状对但没这枚。
    {
        RawHttpClient client(harness.port());
        const auto shape = ParseResponse(client.Get("/artifact/art-ffffffff.png"));
        CHECK(shape.status == 404);
        CHECK(shape.body.find("TOP-SECRET-BYTES") == std::string::npos);
    }
    // 404:坏形状(穿越/非 hex/带路径/双点)一律同一种话。
    for (const std::string_view target : {
             "/artifact/../secret.txt",
             "/artifact/..%2Fsecret.txt",
             "/artifact/art-zzzzzzzz.png",
             "/artifact/sub/art-01234567.png",
             "/artifact/art-01234567.png.bak",
             "/artifact/",
         }) {
        RawHttpClient client(harness.port());
        const auto shape = ParseResponse(client.Get(target));
        CHECK(shape.status == 404);
        CHECK(shape.body.find("TOP-SECRET-BYTES") == std::string::npos);
    }

    harness.transport->Stop();
    accept_thread.join();
}

TEST_CASE("ws artifact 面:没配 artifact_dir,口子不开(404)") {
    ArtifactHarness harness("");
    std::thread accept_thread([&] {
        while (harness.transport->Accept() != nullptr) {
        }
    });
    RawHttpClient client(harness.port());
    const auto shape = ParseResponse(client.Get("/artifact/art-01234567.png"));
    CHECK(shape.status == 404);
    harness.transport->Stop();
    accept_thread.join();
}

TEST_CASE("ws artifact 面:token 门——没带/带错 403,带上放行,token 不落应答") {
    const std::string dir = MakeTempDir("lubancode_test_ws_artifact_token");
    PlantFile(dir, "art-01234567.png", PngBytes("gated"));
    constexpr const char* kToken = "gate-t0ken-值";

    ArtifactHarness harness(dir, kToken);
    std::thread accept_thread([&] {
        while (harness.transport->Accept() != nullptr) {
        }
    });

    // 没带 token:403,正文不给"带错还是没带"的分辨。
    {
        RawHttpClient client(harness.port());
        const auto shape = ParseResponse(client.Get("/artifact/art-01234567.png"));
        CHECK(shape.status == 403);
        CHECK(shape.body.find(kToken) == std::string::npos);
    }
    // 带错:403。
    {
        RawHttpClient client(harness.port());
        const auto shape = ParseResponse(client.Get("/artifact/art-01234567.png?token=wrong"));
        CHECK(shape.status == 403);
    }
    // 查询参数带对:200(百分号编码的 token 也认)。
    {
        RawHttpClient client(harness.port());
        const std::string encoded = "/artifact/art-01234567.png?token=gate-t0ken-%E5%80%BC";
        const auto shape = ParseResponse(client.Get(encoded));
        CHECK(shape.status == 200);
        CHECK(shape.body == PngBytes("gated"));
    }
    // Bearer 头带对:200。
    {
        RawHttpClient client(harness.port());
        const auto shape = ParseResponse(
            client.Get("/artifact/art-01234567.png", "Authorization: Bearer gate-t0ken-\xe5\x80\xbc\r\n"));
        CHECK(shape.status == 200);
        CHECK(shape.body == PngBytes("gated"));
    }
    // token 恒不落应答。
    {
        RawHttpClient client(harness.port());
        const std::string response = client.Get("/artifact/art-01234567.png");
        CHECK(response.find("gate-t0ken") == std::string::npos);
    }

    harness.transport->Stop();
    accept_thread.join();
}

TEST_CASE("ws artifact 面:GET 之后 WS 升级照常(承载面互不搅)") {
    const std::string dir = MakeTempDir("lubancode_test_ws_artifact_then_ws");
    PlantFile(dir, "art-01234567.png", PngBytes("before-ws"));

    ArtifactHarness harness(dir);
    std::promise<std::unique_ptr<app_server::WsTransport::Session>> session_promise;
    std::thread accept_thread([&] {
        session_promise.set_value(harness.transport->Accept()); // 先后伺候 GET 与升级
    });

    // 第一步:GET(就地应答,Accept 还在等下一条)。
    {
        RawHttpClient client(harness.port());
        const auto shape = ParseResponse(client.Get("/artifact/art-01234567.png"));
        CHECK(shape.status == 200);
    }
    // 第二步:WS 升级——同一只 Accept 必须照常走升级路并交出 Session。
    {
        RawHttpClient client(harness.port());
        const std::string upgrade =
            "GET /ws HTTP/1.1\r\n"
            "Host: 127.0.0.1\r\n"
            "Upgrade: websocket\r\n"
            "Connection: keep-alive, Upgrade\r\n"
            "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
            "Sec-WebSocket-Version: 13\r\n"
            "\r\n";
        const std::string response = client.SendRaw(upgrade);
        CHECK(response.find("HTTP/1.1 101") != std::string::npos); // 升级放行
        client.Close();
    }
    auto session_future = session_promise.get_future();
    REQUIRE(session_future.wait_for(std::chrono::seconds(5)) == std::future_status::ready);
    CHECK(session_future.get() != nullptr); // Accept 交出了升级 Session
    accept_thread.join();
}
