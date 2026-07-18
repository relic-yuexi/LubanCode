// M8:MCP 协议层(mcp::Client)测试,分两块:
//   1) 用一个注入的 FakeTransport(不真起进程)验证请求-响应配对逻辑——
//      id 匹配、乱序响应、超时、协议错误响应都要能翻译成可读的失败结果,
//      绝不能抛异常/卡死。
//   2) 用真正的 Python 测试夹具(tests/fixtures/mcp_test_server.py)走一遍
//      完整流程:起子进程、握手、tools/list、tools/call(echo/add)、
//      调用不存在的工具报错、干净关停不留僵尸进程(Windows 下用 tasklist
//      核实)。

#include <doctest/doctest.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <vector>

#include "mcp/client.hpp"

using namespace lubancode;

namespace {

// 协议层看到的传输层就是 mcp::Transport 这个抽象接口——FakeTransport 只管
// 记下写出去的每一行,真正的"服务器响应"由测试代码手动调 client.OnLine()
// 模拟,不真起任何进程。
//
// 注意 FakeTransport 一律声明在 Client 之前(跟 test_lsp_client.cpp 同一条
// 规矩):Client 析构时还会摸一把 transport_->Shutdown(),声明反了就是对
// 已析构对象调虚函数——MSVC 下侥幸没炸,g++(Linux)下 "pure virtual
// method called" 直接 SIGABRT,跨平台单在 WSL 上真机跑测揪出来的。
class FakeTransport : public mcp::Transport {
public:
    std::function<void(const std::string&)> on_write;
    std::atomic<bool> alive{true};

    bool WriteLine(const std::string& line) override {
        if (on_write) {
            on_write(line);
        }
        return true;
    }
    void Shutdown(int /*wait_ms*/) override { alive = false; }
    bool IsAlive() const override { return alive; }
    std::string StderrTail() const override { return std::string(); }
};

}  // namespace

// ---------------------------------------------------------------------------
// 1) FakeTransport:请求-响应配对逻辑
// ---------------------------------------------------------------------------

TEST_CASE("Client: id 配对——写出去的请求带 id,响应带同样的 id 就能配对成功") {
    FakeTransport transport;
    mcp::Client client("test");
    client.AttachTransportForTest(&transport);

    transport.on_write = [&](const std::string& line) {
        const auto request = nlohmann::json::parse(line);
        CHECK(request.at("method") == "tools/list");
        const auto id = request.at("id").get<std::int64_t>();
        const nlohmann::json response = {
            {"jsonrpc", "2.0"},
            {"id", id},
            {"result", {{"tools", nlohmann::json::array({{{"name", "echo"}, {"description", "回显"}}})}}}};
        client.OnLine(response.dump());
    };

    const auto result = client.ListTools();
    REQUIRE(result.has_value());
    REQUIRE(result->size() == 1);
    CHECK(result->at(0).name == "echo");
    CHECK(result->at(0).description == "回显");
}

TEST_CASE("Client: 两个并发请求乱序响应,各自按 id 配对到正确的结果") {
    FakeTransport transport;
    mcp::Client client("test");
    client.AttachTransportForTest(&transport);

    std::mutex req_mutex;
    std::condition_variable req_cv;
    std::vector<nlohmann::json> requests;

    transport.on_write = [&](const std::string& line) {
        std::lock_guard<std::mutex> lock(req_mutex);
        requests.push_back(nlohmann::json::parse(line));
        req_cv.notify_all();
    };

    std::expected<std::vector<mcp::ToolInfo>, std::string> list_result;
    lubancode::tools::Tool::Result call_result;

    std::thread t_list([&] { list_result = client.ListTools(); });
    std::thread t_call([&] { call_result = client.CallTool("add", {{"a", 1}, {"b", 2}}); });

    // 等两条请求都已经写出去。
    {
        std::unique_lock<std::mutex> lock(req_mutex);
        req_cv.wait(lock, [&] { return requests.size() == 2; });
    }

    // 故意反过来:先回应后写出去的请求(tools/call),再回应先写出去的
    // (tools/list)——验证配对只看 id,跟到达顺序无关。
    nlohmann::json list_request;
    nlohmann::json call_request;
    for (const auto& req : requests) {
        if (req.at("method") == "tools/list") {
            list_request = req;
        } else {
            call_request = req;
        }
    }
    REQUIRE_FALSE(list_request.is_null());
    REQUIRE_FALSE(call_request.is_null());

    const nlohmann::json call_response = {
        {"jsonrpc", "2.0"},
        {"id", call_request.at("id")},
        {"result", {{"content", nlohmann::json::array({{{"type", "text"}, {"text", "3"}}})}, {"isError", false}}}};
    client.OnLine(call_response.dump());

    const nlohmann::json list_response = {
        {"jsonrpc", "2.0"},
        {"id", list_request.at("id")},
        {"result", {{"tools", nlohmann::json::array({{{"name", "add"}, {"description", "加法"}}})}}}};
    client.OnLine(list_response.dump());

    t_list.join();
    t_call.join();

    REQUIRE(list_result.has_value());
    REQUIRE(list_result->size() == 1);
    CHECK(list_result->at(0).name == "add");

    CHECK_FALSE(call_result.is_error);
    CHECK(call_result.content == "3");
}

TEST_CASE("Client: 服务器一直不回应,等到超时,is_error 结果里说明超时") {
    FakeTransport transport;
    mcp::Client client("test");
    client.SetTimeoutsForTest(/*default_timeout_ms=*/50, /*tool_call_timeout_ms=*/50);
    client.AttachTransportForTest(&transport);
    // on_write 不设,永远不回应。

    const auto start = std::chrono::steady_clock::now();
    const auto result = client.ListTools();
    const auto elapsed = std::chrono::steady_clock::now() - start;

    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().find("超时") != std::string::npos);
    CHECK(elapsed < std::chrono::milliseconds(2000));  // 确实是按短超时退出的,不是傻等
}

TEST_CASE("Client: 进程在等待响应期间死掉,is_error 结果里说明进程已退出") {
    FakeTransport transport;
    mcp::Client client("test");
    client.SetTimeoutsForTest(/*default_timeout_ms=*/5000, /*tool_call_timeout_ms=*/5000);
    client.AttachTransportForTest(&transport);

    transport.on_write = [&](const std::string&) {
        // 模拟"请求刚发出去,进程就没了"——不回应,直接把 alive 置假。
        std::thread([&transport] {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
            transport.alive = false;
        }).detach();
    };

    const auto start = std::chrono::steady_clock::now();
    const auto result = client.CallTool("whatever", nlohmann::json::object());
    const auto elapsed = std::chrono::steady_clock::now() - start;

    CHECK(result.is_error);
    CHECK(result.content.find("退出") != std::string::npos);
    // 死进程要快速失败:靠 100ms 分段轮询 IsAlive 发现,不许傻等满 5s 超时。
    CHECK(elapsed < std::chrono::milliseconds(2000));
}

TEST_CASE("Client: OnLine 收到 id 类型不对/字段类型不对的行,不崩、不影响后续配对") {
    FakeTransport transport;
    mcp::Client client("test");
    client.SetTimeoutsForTest(/*default_timeout_ms=*/300, /*tool_call_timeout_ms=*/300);
    client.AttachTransportForTest(&transport);

    // 这些行以前会让 OnLine 在读线程上抛 type_error → std::terminate 全程序暴毙。
    CHECK_NOTHROW(client.OnLine(R"({"jsonrpc":"2.0","id":"not-a-number","result":{}})"));
    CHECK_NOTHROW(client.OnLine(R"({"jsonrpc":"2.0","id":3.14,"result":{}})"));
    CHECK_NOTHROW(client.OnLine(R"(这一行压根不是 JSON)"));
    CHECK_NOTHROW(client.OnLine(R"([1,2,3])"));

    // 坏行只被丢弃,正常请求-响应仍然走得通。
    transport.on_write = [&](const std::string& line) {
        const auto request = nlohmann::json::parse(line);
        const nlohmann::json response = {
            {"jsonrpc", "2.0"},
            {"id", request.at("id")},
            {"result", {{"content", nlohmann::json::array({{{"type", "text"}, {"text", "ok"}}})}, {"isError", false}}}};
        client.OnLine(response.dump());
    };
    const auto result = client.CallTool("whatever", nlohmann::json::object());
    CHECK_FALSE(result.is_error);
    CHECK(result.content == "ok");
}

TEST_CASE("Client: tools/call 响应字段类型不对(isError 是字符串),不崩,翻译成 is_error 结果") {
    FakeTransport transport;
    mcp::Client client("test");
    client.AttachTransportForTest(&transport);

    transport.on_write = [&](const std::string& line) {
        const auto request = nlohmann::json::parse(line);
        const nlohmann::json response = {
            {"jsonrpc", "2.0"},
            {"id", request.at("id")},
            {"result",
             {{"content", nlohmann::json::array({{{"type", 123}, {"text", "x"}}})},
              {"isError", "yes"}}}};  // isError 该是 bool,type 该是字符串——全错
        client.OnLine(response.dump());
    };

    tools::Tool::Result result{"", false};
    CHECK_NOTHROW(result = client.CallTool("whatever", nlohmann::json::object()));
    CHECK(result.is_error);
    CHECK_FALSE(result.content.empty());  // 有人话错误说明,不是空
}

TEST_CASE("Client: 响应里带 error 字段,翻译成可读的失败结果") {
    FakeTransport transport;
    mcp::Client client("test");
    client.AttachTransportForTest(&transport);

    transport.on_write = [&](const std::string& line) {
        const auto request = nlohmann::json::parse(line);
        const nlohmann::json response = {{"jsonrpc", "2.0"},
                                           {"id", request.at("id")},
                                           {"error", {{"code", -32601}, {"message", "unknown tool: bogus"}}}};
        client.OnLine(response.dump());
    };

    const auto result = client.CallTool("bogus", nlohmann::json::object());
    CHECK(result.is_error);
    CHECK(result.content.find("unknown tool: bogus") != std::string::npos);
}

TEST_CASE("Client: CallTool 把多个 text 内容块拼接起来,isError=true 映射到 Result::is_error") {
    FakeTransport transport;
    mcp::Client client("test");
    client.AttachTransportForTest(&transport);

    transport.on_write = [&](const std::string& line) {
        const auto request = nlohmann::json::parse(line);
        const nlohmann::json response = {
            {"jsonrpc", "2.0"},
            {"id", request.at("id")},
            {"result",
             {{"content", nlohmann::json::array({{{"type", "text"}, {"text", "第一段 "}},
                                                   {{"type", "text"}, {"text", "第二段"}}})},
              {"isError", true}}}};
        client.OnLine(response.dump());
    };

    const auto result = client.CallTool("whatever", nlohmann::json::object());
    CHECK(result.is_error);
    CHECK(result.content == "第一段 第二段");
}

TEST_CASE("Client: 非 text 内容块变成不支持提示占位符") {
    FakeTransport transport;
    mcp::Client client("test");
    client.AttachTransportForTest(&transport);

    transport.on_write = [&](const std::string& line) {
        const auto request = nlohmann::json::parse(line);
        const nlohmann::json response = {
            {"jsonrpc", "2.0"},
            {"id", request.at("id")},
            {"result", {{"content", nlohmann::json::array({{{"type", "image"}, {"data", "base64..."}}})}}}};
        client.OnLine(response.dump());
    };

    const auto result = client.CallTool("whatever", nlohmann::json::object());
    CHECK_FALSE(result.is_error);
    CHECK(result.content.find("不支持的内容类型") != std::string::npos);
    CHECK(result.content.find("image") != std::string::npos);
}

TEST_CASE("Client: Initialize 握手成功后会发一条 notifications/initialized 通知(无 id)") {
    FakeTransport transport;
    mcp::Client client("test");
    client.AttachTransportForTest(&transport);

    std::vector<nlohmann::json> written;
    transport.on_write = [&](const std::string& line) {
        const auto msg = nlohmann::json::parse(line);
        written.push_back(msg);
        if (msg.at("method") == "initialize") {
            const nlohmann::json response = {{"jsonrpc", "2.0"},
                                               {"id", msg.at("id")},
                                               {"result", {{"protocolVersion", "2024-11-05"}}}};
            client.OnLine(response.dump());
        }
    };

    const auto result = client.Initialize();
    REQUIRE(result.has_value());
    REQUIRE(written.size() == 2);
    CHECK(written[0].at("method") == "initialize");
    CHECK(written[1].at("method") == "notifications/initialized");
    CHECK_FALSE(written[1].contains("id"));  // 通知没有 id
}

// ---------------------------------------------------------------------------
// 2) 真正的夹具:起 tests/fixtures/mcp_test_server.py,走完整流程。
//    两平台都跑(StdioTransport 底下是 platform::ChildProcess,跨平台单
//    起两平台同实现;跑不了的环境缺的是 python,不是平台)。
// ---------------------------------------------------------------------------

#ifndef LUBANCODE_TEST_FIXTURES_DIR
#define LUBANCODE_TEST_FIXTURES_DIR "."
#endif

// 真夹具用哪个 python:Windows 装的是 python.exe,Linux/macOS 发行版惯例
// 是 python3(裸 python 常常不存在)。
#ifdef _WIN32
constexpr const char* kPythonCmd = "python";
#else
constexpr const char* kPythonCmd = "python3";
#endif

TEST_CASE("Client + 真实 Python 夹具:握手 + tools/list + tools/call(echo/add) + 未知工具报错 + 干净关停") {
    mcp::Client client("test");
    const std::string script = std::string(LUBANCODE_TEST_FIXTURES_DIR) + "/mcp_test_server.py";
    const auto start_result = client.StartProcess(kPythonCmd, {script}, {});
    REQUIRE_MESSAGE(start_result.success, start_result.error);

    const auto init_result = client.Initialize();
    REQUIRE_MESSAGE(init_result.has_value(), init_result.error());

    const auto tools_result = client.ListTools();
    REQUIRE_MESSAGE(tools_result.has_value(), tools_result.error());
    CHECK(tools_result->size() == 2);
    bool has_echo = false;
    bool has_add = false;
    for (const auto& tool : *tools_result) {
        if (tool.name == "echo") has_echo = true;
        if (tool.name == "add") has_add = true;
    }
    CHECK(has_echo);
    CHECK(has_add);

    const auto echo_result = client.CallTool("echo", {{"text", "你好,MCP"}});
    CHECK_FALSE(echo_result.is_error);
    CHECK(echo_result.content == "你好,MCP");

    const auto add_result = client.CallTool("add", {{"a", 17}, {"b", 25}});
    CHECK_FALSE(add_result.is_error);
    CHECK(add_result.content == "42");

    const auto unknown_result = client.CallTool("no_such_tool", nlohmann::json::object());
    CHECK(unknown_result.is_error);

    client.Shutdown();
    CHECK_FALSE(client.Alive());
}

TEST_CASE("Client + 真实 Python 夹具:坏响应模式(tools/call 字段类型全错)不崩,报可读错") {
    mcp::Client client("test");
    const std::string script = std::string(LUBANCODE_TEST_FIXTURES_DIR) + "/mcp_test_server.py";
    const auto start_result = client.StartProcess(kPythonCmd, {script, "--bad-tools-call"}, {});
    REQUIRE_MESSAGE(start_result.success, start_result.error);

    const auto init_result = client.Initialize();
    REQUIRE_MESSAGE(init_result.has_value(), init_result.error());

    tools::Tool::Result result{"", false};
    CHECK_NOTHROW(result = client.CallTool("echo", {{"text", "hi"}}));
    CHECK(result.is_error);
    CHECK_FALSE(result.content.empty());  // 可读的错误说明,不是空串、更不是崩

    client.Shutdown();
}

TEST_CASE("Client + 真实 Python 夹具:服务器在 tools/call 等待期间死掉,快速失败(<5s)") {
    mcp::Client client("test");
    const std::string script = std::string(LUBANCODE_TEST_FIXTURES_DIR) + "/mcp_test_server.py";
    const auto start_result = client.StartProcess(kPythonCmd, {script}, {});
    REQUIRE_MESSAGE(start_result.success, start_result.error);

    const auto init_result = client.Initialize();
    REQUIRE_MESSAGE(init_result.has_value(), init_result.error());

    // die 工具让服务器一声不吭直接退出——tools/call 的 120s 超时不该被
    // 傻等满,IsAlive 分段轮询要在几百毫秒内发现进程没了。
    const auto start = std::chrono::steady_clock::now();
    const auto result = client.CallTool("die", nlohmann::json::object());
    const auto elapsed = std::chrono::steady_clock::now() - start;

    CHECK(result.is_error);
    CHECK(result.content.find("退出") != std::string::npos);
    CHECK(elapsed < std::chrono::seconds(5));

    client.Shutdown();
}

