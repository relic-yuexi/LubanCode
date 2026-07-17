// LSP 协议层(lsp::Client)测试,分两块:
//   1) 注入 FakeTransport(不真起进程)验证:initialize 握手会补 initialized
//      通知、请求-响应按 id 配对、超时快速失败、didOpen 只发一次、
//      publishDiagnostics 进缓存、缓存没有时限时等待返回 nullopt、服务器
//      反向请求收到 result=null 的空响应。
//   2) 真正的 Python 夹具(tests/fixtures/lsp_test_server.py)走完整流程:
//      起子进程、握手、didOpen + 诊断推送、definition/references/
//      documentSymbol、shutdown/exit 干净关停。
//
// 注意 FakeTransport 一律声明在 Client 之前——Client 析构时还会摸一把
// transport(Shutdown 收尾),声明顺序保证 transport 活得比 client 久。

#include <doctest/doctest.h>

#include <atomic>
#include <chrono>
#include <functional>
#include <string>
#include <vector>

#include "lsp/client.hpp"

using namespace lubancode;

namespace {

class FakeTransport : public lsp::Transport {
public:
    std::function<void(const std::string&)> on_write;
    std::atomic<bool> alive{true};
    std::vector<std::string> written;

    bool WriteMessage(const std::string& body) override {
        written.push_back(body);
        if (on_write) {
            on_write(body);
        }
        return true;
    }
    void Shutdown(int /*wait_ms*/) override { alive = false; }
    bool IsAlive() const override { return alive; }
    std::string StderrTail() const override { return std::string(); }
};

}  // namespace

// ---------------------------------------------------------------------------
// 0) 路径 <-> URI 纯函数
// ---------------------------------------------------------------------------

TEST_CASE("PathToUri/UriToPath: Windows 盘符路径来回转,空格转义还原") {
    const std::string uri = lsp::PathToUri("D:\\a b\\f.py");
    CHECK(uri == "file:///D:/a%20b/f.py");
#ifdef _WIN32
    CHECK(lsp::UriToPath(uri) == "D:\\a b\\f.py");
#endif
}

TEST_CASE("UriToPath: 认得 %3A 转义的盘符冒号(clangd 会这么发)") {
#ifdef _WIN32
    CHECK(lsp::UriToPath("file:///d%3A/proj/x.cpp") == "d:\\proj\\x.cpp");
#else
    CHECK(lsp::UriToPath("file:///home/u/x.cpp") == "/home/u/x.cpp");
#endif
}

// ---------------------------------------------------------------------------
// 1) FakeTransport:协议逻辑
// ---------------------------------------------------------------------------

TEST_CASE("Client: Initialize 发 initialize 请求(rootUri/最小 capabilities),成功后补 initialized 通知") {
    FakeTransport transport;
    lsp::Client client("cpp");
    client.AttachTransportForTest(&transport);

    transport.on_write = [&](const std::string& body) {
        const auto msg = nlohmann::json::parse(body);
        if (msg.value("method", std::string()) == "initialize") {
            CHECK(msg.at("params").at("rootUri") == "file:///D:/proj");
            CHECK(msg.at("params").at("capabilities").is_object());
            const nlohmann::json response = {
                {"jsonrpc", "2.0"}, {"id", msg.at("id")}, {"result", {{"capabilities", nlohmann::json::object()}}}};
            client.OnMessage(response.dump());
        }
    };

    const auto result = client.Initialize("file:///D:/proj");
    REQUIRE(result.has_value());
    REQUIRE(transport.written.size() == 2);
    const auto second = nlohmann::json::parse(transport.written[1]);
    CHECK(second.at("method") == "initialized");
    CHECK_FALSE(second.contains("id"));
    transport.alive = false;  // 析构时跳过 shutdown 请求,别再等 2s
}

TEST_CASE("Client: Definition 请求带 0 基 position,响应按 id 配对回来") {
    FakeTransport transport;
    lsp::Client client("cpp");
    client.AttachTransportForTest(&transport);

    transport.on_write = [&](const std::string& body) {
        const auto msg = nlohmann::json::parse(body);
        // Client 析构时还会通过同一个 transport 发 shutdown/exit,这个回调
        // 只管 definition 这一条,别的放过。
        if (msg.value("method", std::string()) != "textDocument/definition") {
            return;
        }
        CHECK(msg.at("params").at("position").at("line") == 9);
        CHECK(msg.at("params").at("position").at("character") == 4);
        const nlohmann::json response = {
            {"jsonrpc", "2.0"},
            {"id", msg.at("id")},
            {"result",
             {{{"uri", "file:///D:/proj/a.cpp"},
               {"range", {{"start", {{"line", 1}, {"character", 2}}}, {"end", {{"line", 1}, {"character", 5}}}}}}}}};
        client.OnMessage(response.dump());
    };

    const auto result = client.Definition("file:///D:/proj/a.cpp", 9, 4);
    REQUIRE(result.has_value());
    REQUIRE(result->is_array());
    CHECK(result->at(0).at("uri") == "file:///D:/proj/a.cpp");
    transport.alive = false;  // 让 Client 析构时跳过 shutdown 请求,不再回到上面的回调
}

TEST_CASE("Client: 服务器一直不回应,按收窄的超时快速失败,错误里说明超时") {
    FakeTransport transport;
    lsp::Client client("cpp");
    client.AttachTransportForTest(&transport);
    client.SetTimeoutForTest(50);

    const auto start = std::chrono::steady_clock::now();
    const auto result = client.DocumentSymbol("file:///D:/x.cpp");
    const auto elapsed = std::chrono::steady_clock::now() - start;

    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().find("超时") != std::string::npos);
    CHECK(elapsed < std::chrono::milliseconds(2000));
    transport.alive = false;
}

TEST_CASE("Client: 响应带 error 字段,翻译成人话错误") {
    FakeTransport transport;
    lsp::Client client("cpp");
    client.AttachTransportForTest(&transport);

    transport.on_write = [&](const std::string& body) {
        const auto msg = nlohmann::json::parse(body);
        const nlohmann::json response = {{"jsonrpc", "2.0"},
                                          {"id", msg.at("id")},
                                          {"error", {{"code", -32601}, {"message", "method not found"}}}};
        client.OnMessage(response.dump());
    };

    const auto result = client.References("file:///D:/x.cpp", 0, 0);
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().find("method not found") != std::string::npos);
    transport.alive = false;
}

TEST_CASE("Client: EnsureDidOpen 同一个 uri 只发一次通知") {
    FakeTransport transport;
    lsp::Client client("python");
    client.AttachTransportForTest(&transport);

    client.EnsureDidOpen("file:///D:/x.py", "python", "print(1)\n");
    client.EnsureDidOpen("file:///D:/x.py", "python", "print(1)\n");
    client.EnsureDidOpen("file:///D:/y.py", "python", "print(2)\n");

    REQUIRE(transport.written.size() == 2);
    const auto first = nlohmann::json::parse(transport.written[0]);
    CHECK(first.at("method") == "textDocument/didOpen");
    CHECK(first.at("params").at("textDocument").at("uri") == "file:///D:/x.py");
    CHECK(first.at("params").at("textDocument").at("languageId") == "python");
    CHECK(first.at("params").at("textDocument").at("version") == 1);
    const auto second = nlohmann::json::parse(transport.written[1]);
    CHECK(second.at("params").at("textDocument").at("uri") == "file:///D:/y.py");
    transport.alive = false;
}

TEST_CASE("Client: publishDiagnostics 通知进缓存,WaitDiagnostics 直接读到") {
    FakeTransport transport;
    lsp::Client client("python");
    client.AttachTransportForTest(&transport);

    const nlohmann::json notification = {
        {"jsonrpc", "2.0"},
        {"method", "textDocument/publishDiagnostics"},
        {"params",
         {{"uri", "file:///D:/x.py"},
          {"diagnostics",
           {{{"range", {{"start", {{"line", 3}, {"character", 0}}}, {"end", {{"line", 3}, {"character", 5}}}}},
             {"severity", 1},
             {"message", "boom"}}}}}}};
    client.OnMessage(notification.dump());

    const auto diagnostics = client.WaitDiagnostics("file:///D:/x.py", 10);
    REQUIRE(diagnostics.has_value());
    REQUIRE(diagnostics->is_array());
    CHECK(diagnostics->at(0).at("message") == "boom");
    transport.alive = false;
}

TEST_CASE("Client: 缓存里没有这个 uri,WaitDiagnostics 限时等待后返回 nullopt(不傻等)") {
    FakeTransport transport;
    lsp::Client client("python");
    client.AttachTransportForTest(&transport);

    const auto start = std::chrono::steady_clock::now();
    const auto diagnostics = client.WaitDiagnostics("file:///D:/never.py", 50);
    const auto elapsed = std::chrono::steady_clock::now() - start;

    CHECK_FALSE(diagnostics.has_value());
    CHECK(elapsed >= std::chrono::milliseconds(40));
    CHECK(elapsed < std::chrono::milliseconds(2000));
    transport.alive = false;
}

TEST_CASE("Client: 服务器反向请求(带 method 又带 id)收到 result=null 的空响应") {
    FakeTransport transport;
    lsp::Client client("cpp");
    client.AttachTransportForTest(&transport);

    const nlohmann::json server_request = {
        {"jsonrpc", "2.0"}, {"id", 99}, {"method", "window/workDoneProgress/create"}, {"params", {{"token", "t"}}}};
    client.OnMessage(server_request.dump());

    REQUIRE(transport.written.size() == 1);
    const auto reply = nlohmann::json::parse(transport.written[0]);
    CHECK(reply.at("id") == 99);
    CHECK(reply.at("result").is_null());
    transport.alive = false;
}

// ---------------------------------------------------------------------------
// 2) 真正的夹具:起 tests/fixtures/lsp_test_server.py,走完整流程。
//    只在 Windows 下跑(StdioTransport 眼下只实现了 Windows)。
// ---------------------------------------------------------------------------

#ifdef _WIN32

#ifndef LUBANCODE_TEST_FIXTURES_DIR
#define LUBANCODE_TEST_FIXTURES_DIR "."
#endif

TEST_CASE("Client + 真实 Python 夹具:握手 + didOpen 诊断推送 + definition/references/documentSymbol + 干净关停") {
    lsp::Client client("python");
    const std::string script = std::string(LUBANCODE_TEST_FIXTURES_DIR) + "/lsp_test_server.py";
    const auto start_result = client.StartProcess("python", {script});
    REQUIRE_MESSAGE(start_result.success, start_result.error);

    const auto init_result = client.Initialize("file:///D:/proj");
    REQUIRE_MESSAGE(init_result.has_value(), init_result.error());

    const std::string uri = "file:///D:/proj/sample.py";
    client.EnsureDidOpen(uri, "python", "def top_func():\n    pass\n");

    // didOpen 之后夹具会推一条假诊断,进缓存。
    const auto diagnostics = client.WaitDiagnostics(uri, 2000);
    REQUIRE(diagnostics.has_value());
    REQUIRE(diagnostics->is_array());
    REQUIRE(diagnostics->size() == 1);
    CHECK(diagnostics->at(0).at("message").get<std::string>().find("fixture-warning") != std::string::npos);

    const auto definition = client.Definition(uri, 0, 4);
    REQUIRE_MESSAGE(definition.has_value(), definition.error());
    REQUIRE(definition->is_array());
    CHECK(definition->at(0).at("uri") == uri);
    CHECK(definition->at(0).at("range").at("start").at("line") == 2);

    const auto references = client.References(uri, 0, 4);
    REQUIRE_MESSAGE(references.has_value(), references.error());
    REQUIRE(references->is_array());
    CHECK(references->size() == 2);

    const auto symbols = client.DocumentSymbol(uri);
    REQUIRE_MESSAGE(symbols.has_value(), symbols.error());
    REQUIRE(symbols->is_array());
    CHECK(symbols->at(0).at("name") == "top_func");
    CHECK(symbols->at(0).at("children").at(0).at("name") == "inner");

    client.Shutdown();
    CHECK_FALSE(client.Alive());
}

#endif  // _WIN32
