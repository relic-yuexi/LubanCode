// app-server 协议骨架单:握手状态机与方法路由(Dispatcher + 假连接的
// 整线驱动)。假 writer/reader 注入 StdioConnection,stdout 逐行可解析
// 的断言钉在这里——每一条出站行都 parse 之后逐字段查。
#include <doctest/doctest.h>

#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#include "app_server/connection.hpp"
#include "app_server/dispatcher.hpp"
#include "app_server/schema.hpp"

using namespace lubancode::app_server;

namespace {

// 假 writer:收行。假 reader:每次 Run 前喂一段脚本,读尽即 EOF。
struct ScriptedIo {
    std::vector<std::string> written;
    std::string pending_input;

    StdioConnection::LineWriter Writer() {
        return [this](const std::string& line) { written.push_back(line); };
    }
    StdioConnection::ChunkReader Reader() {
        return [this]() {
            const std::string chunk = pending_input;
            pending_input.clear();
            return chunk; // 空串 = EOF
        };
    }
};

nlohmann::json ParseLine(const std::string& line) {
    return nlohmann::json::parse(line);
}

// 造一条连接,dispatcher 由调用方给。
std::unique_ptr<StdioConnection> MakeConnection(std::shared_ptr<Dispatcher> dispatcher, ScriptedIo& io) {
    return std::make_unique<StdioConnection>(std::move(dispatcher), io.Writer(), io.Reader(), 256);
}

// 给 dispatcher 挂一个业务法子(测试握手门槛用)。
void RegisterEchoMethod(const std::shared_ptr<Dispatcher>& dispatcher) {
    dispatcher->RegisterMethod(
        "test/echo", [](const IncomingRequest& request, DispatchContext&) -> std::optional<nlohmann::json> {
            return MakeResult(request.id, nlohmann::json{{"echo", request.params}});
        });
}

}  // namespace

// ---------------------------------------------------------------------------
// 握手状态机
// ---------------------------------------------------------------------------

TEST_CASE("握手三步:initialize -> initialized -> 业务放行") {
    ScriptedIo io;
    auto dispatcher = std::make_shared<Dispatcher>();
    dispatcher->SetInitializeResultFactory(
        []() { return MakeInitializeResult("test", "test"); });
    RegisterEchoMethod(dispatcher);
    auto connection = MakeConnection(dispatcher, io);

    io.pending_input =
        R"({"id":1,"method":"initialize","params":{}})" "\n"
        R"({"method":"initialized"})" "\n"
        R"({"id":2,"method":"test/echo","params":{"x":1}})" "\n";
    connection->Run();

    REQUIRE(io.written.size() == 2);
    // 第一条:initialize 的响应(parse 后逐字段查,不写死黄金报文)。
    const nlohmann::json init = ParseLine(io.written[0]);
    CHECK(init["id"] == 1);
    CHECK(init["result"]["protocolVersion"] == kProtocolVersion);
    CHECK(init["result"]["lubancodeVersion"] == "test");
    // 第二条:业务响应。
    const nlohmann::json echo = ParseLine(io.written[1]);
    CHECK(echo["id"] == 2);
    CHECK(echo["result"]["echo"]["x"] == 1);
    CHECK(dispatcher->state() == HandshakeState::Ready);
}

TEST_CASE("未握手调业务:稳定错误码 kErrNotInitialized") {
    ScriptedIo io;
    auto dispatcher = std::make_shared<Dispatcher>();
    RegisterEchoMethod(dispatcher);
    auto connection = MakeConnection(dispatcher, io);

    io.pending_input = R"({"id":5,"method":"test/echo","params":{}})" "\n";
    connection->Run();

    REQUIRE(io.written.size() == 1);
    const nlohmann::json response = ParseLine(io.written[0]);
    CHECK(response["id"] == 5);
    CHECK(response["error"]["code"] == kErrNotInitialized);
}

TEST_CASE("initialize 应答完但 initialized 没来:业务仍被拒") {
    ScriptedIo io;
    auto dispatcher = std::make_shared<Dispatcher>();
    dispatcher->SetInitializeResultFactory([]() { return nlohmann::json::object(); });
    RegisterEchoMethod(dispatcher);
    auto connection = MakeConnection(dispatcher, io);

    io.pending_input =
        R"({"id":1,"method":"initialize","params":{}})" "\n"
        R"({"id":2,"method":"test/echo","params":{}})" "\n";
    connection->Run();

    REQUIRE(io.written.size() == 2);
    const nlohmann::json response = ParseLine(io.written[1]);
    CHECK(response["error"]["code"] == kErrNotInitialized);
    CHECK(dispatcher->state() == HandshakeState::WaitingInitialized);
}

TEST_CASE("重复 initialize:kErrInvalidRequest") {
    ScriptedIo io;
    auto dispatcher = std::make_shared<Dispatcher>();
    dispatcher->SetInitializeResultFactory([]() { return nlohmann::json::object(); });
    auto connection = MakeConnection(dispatcher, io);

    io.pending_input =
        R"({"id":1,"method":"initialize","params":{}})" "\n"
        R"({"method":"initialized"})" "\n"
        R"({"id":2,"method":"initialize","params":{}})" "\n";
    connection->Run();

    REQUIRE(io.written.size() == 2);
    const nlohmann::json response = ParseLine(io.written[1]);
    CHECK(response["id"] == 2);
    CHECK(response["error"]["code"] == kErrInvalidRequest);
}

TEST_CASE("坏 JSON 不崩:稳定错误码,null id") {
    ScriptedIo io;
    auto dispatcher = std::make_shared<Dispatcher>();
    auto connection = MakeConnection(dispatcher, io);

    io.pending_input = "压根不是 JSON\n{\"id\":1,\n";
    connection->Run();

    REQUIRE(io.written.size() >= 1);
    const nlohmann::json response = ParseLine(io.written[0]);
    CHECK(response["id"].is_null());
    CHECK(response["error"]["code"] == kErrParseError);
    // 后面那条残行(凑不齐的 id 片段)解不出 id,但服务还活着:再喂
    // 一条好报文,还能收(用第二条连接续验——Run 读完即退,这里验的是
    // ProcessLine 不炸)。
}

TEST_CASE("坏信封(有 id 没 method):错误码带原 id 回") {
    ScriptedIo io;
    auto dispatcher = std::make_shared<Dispatcher>();
    auto connection = MakeConnection(dispatcher, io);

    io.pending_input = R"({"id":9,"params":{}})" "\n";
    connection->Run();

    REQUIRE(io.written.size() == 1);
    const nlohmann::json response = ParseLine(io.written[0]);
    CHECK(response["id"] == 9);
    CHECK(response["error"]["code"] == kErrInvalidRequest);
}

TEST_CASE("未知方法:kErrMethodNotFound,连接活着") {
    ScriptedIo io;
    auto dispatcher = std::make_shared<Dispatcher>();
    dispatcher->SetInitializeResultFactory([]() { return nlohmann::json::object(); });
    RegisterEchoMethod(dispatcher);
    auto connection = MakeConnection(dispatcher, io);

    io.pending_input =
        R"({"id":1,"method":"initialize","params":{}})" "\n"
        R"({"method":"initialized"})" "\n"
        R"({"id":2,"method":"no/such-method","params":{}})" "\n"
        R"({"id":3,"method":"test/echo","params":{}})" "\n";
    connection->Run();

    REQUIRE(io.written.size() == 3);
    const nlohmann::json not_found = ParseLine(io.written[1]);
    CHECK(not_found["error"]["code"] == kErrMethodNotFound);
    CHECK_FALSE(io.written[1].empty());
    // 第四条进来还有响应:一条坏消息没撞死服务。
    const nlohmann::json echo = ParseLine(io.written[2]);
    CHECK(echo["id"] == 3);
    CHECK(echo.contains("result"));
}

TEST_CASE("留位方法名认识也照实回不认识(没接线就是不接线)") {
    ScriptedIo io;
    auto dispatcher = std::make_shared<Dispatcher>();
    dispatcher->SetInitializeResultFactory([]() { return nlohmann::json::object(); });
    auto connection = MakeConnection(dispatcher, io);

    io.pending_input =
        R"({"id":1,"method":"initialize","params":{}})" "\n"
        R"({"method":"initialized"})" "\n"
        R"({"id":2,"method":"turn/steer","params":{}})" "\n"
        R"({"id":3,"method":"thread/resume","params":{}})" "\n"
        R"({"id":4,"method":"model/list","params":{}})" "\n";
    connection->Run();

    REQUIRE(io.written.size() == 4);
    for (std::size_t i = 1; i < io.written.size(); ++i) {
        const nlohmann::json response = ParseLine(io.written[i]);
        CHECK(response["error"]["code"] == kErrMethodNotFound);
    }
}

// ---------------------------------------------------------------------------
// shutdown / exit 收线
// ---------------------------------------------------------------------------

TEST_CASE("shutdown:应答完收线,后续业务一律 kErrShutdownRequested") {
    ScriptedIo io;
    auto dispatcher = std::make_shared<Dispatcher>();
    dispatcher->SetInitializeResultFactory([]() { return nlohmann::json::object(); });
    RegisterEchoMethod(dispatcher);
    auto connection = MakeConnection(dispatcher, io);

    io.pending_input =
        R"({"id":1,"method":"initialize","params":{}})" "\n"
        R"({"method":"initialized"})" "\n"
        R"({"id":2,"method":"shutdown"})" "\n";
    connection->Run();

    // shutdown 的响应在收线前发出。
    REQUIRE(io.written.size() == 2);
    const nlohmann::json shutdown_response = ParseLine(io.written[1]);
    CHECK(shutdown_response["id"] == 2);
    CHECK(shutdown_response.contains("result"));
    CHECK(connection->closed());
    CHECK(dispatcher->state() == HandshakeState::ShutdownRequested);

    // shutdown 之后业务一律 kErrShutdownRequested:直驱 dispatcher 验
    // (连接层已收线,不再读新请求是它的本分)。
    IncomingRequest late;
    late.id = 3;
    late.method = "test/echo";
    DispatchContext context;
    const DispatchOutcome late_outcome = dispatcher->HandleRequest(late, context);
    REQUIRE(late_outcome.outbound.size() == 1);
    const nlohmann::json rejected = ParseLine(late_outcome.outbound[0]);
    CHECK(rejected["id"] == 3);
    CHECK(rejected["error"]["code"] == kErrShutdownRequested);
}

TEST_CASE("exit 通知:立即收线,不回话") {
    ScriptedIo io;
    auto dispatcher = std::make_shared<Dispatcher>();
    auto connection = MakeConnection(dispatcher, io);

    io.pending_input =
        R"({"id":1,"method":"initialize","params":{}})" "\n"
        R"({"method":"exit"})" "\n"
        R"({"id":2,"method":"anything"})" "\n";
    connection->Run();

    // exit 前那条 initialize 有响应,exit 本身不回话,之后不再读。
    REQUIRE(io.written.size() == 1);
    CHECK(connection->closed());
}

TEST_CASE("EOF:静默收线,出站队列冲刷完") {
    ScriptedIo io;
    auto dispatcher = std::make_shared<Dispatcher>();
    auto connection = MakeConnection(dispatcher, io);
    connection->EmitEvent(kEventThreadStarted, nlohmann::json{{"threadId", "t"}});

    connection->Run(); // reader 立即 EOF

    // 队列里那条事件被刷出去:终态不悄悄丢的规矩,冲刷也守。
    REQUIRE(io.written.size() == 1);
    const nlohmann::json event = ParseLine(io.written[0]);
    CHECK(event["method"] == "thread/started");
    CHECK(event["params"]["threadId"] == "t");
}

// ---------------------------------------------------------------------------
// handler 兜底与响应配对
// ---------------------------------------------------------------------------

TEST_CASE("handler 抛异常:折成 kErrInternalError,服务不崩") {
    ScriptedIo io;
    auto dispatcher = std::make_shared<Dispatcher>();
    dispatcher->SetInitializeResultFactory([]() { return nlohmann::json::object(); });
    dispatcher->RegisterMethod(
        "test/boom", [](const IncomingRequest&, DispatchContext&) -> std::optional<nlohmann::json> {
            throw std::runtime_error("炸了");
        });
    RegisterEchoMethod(dispatcher);
    auto connection = MakeConnection(dispatcher, io);

    io.pending_input =
        R"({"id":1,"method":"initialize","params":{}})" "\n"
        R"({"method":"initialized"})" "\n"
        R"({"id":2,"method":"test/boom","params":{}})" "\n"
        R"({"id":3,"method":"test/echo","params":{}})" "\n";
    connection->Run();

    REQUIRE(io.written.size() == 3);
    const nlohmann::json boom = ParseLine(io.written[1]);
    CHECK(boom["id"] == 2);
    CHECK(boom["error"]["code"] == kErrInternalError);
    const nlohmann::json echo = ParseLine(io.written[2]);
    CHECK(echo["id"] == 3);
    CHECK(echo.contains("result"));
}

TEST_CASE("响应信封(前端对反向请求的答复,留位):吃下不炸不回话") {
    ScriptedIo io;
    auto dispatcher = std::make_shared<Dispatcher>();
    dispatcher->SetInitializeResultFactory([]() { return nlohmann::json::object(); });
    auto connection = MakeConnection(dispatcher, io);

    io.pending_input =
        R"({"id":1,"method":"initialize","params":{}})" "\n"
        R"({"method":"initialized"})" "\n"
        R"({"id":77,"result":{"decision":"accept"}})" "\n";
    connection->Run();

    // 反向请求未接线:响应被丢弃,但握手响应照常、服务活着。
    REQUIRE(io.written.size() == 1);
    CHECK(ParseLine(io.written[0])["id"] == 1);
}

TEST_CASE("超长行:回 parse error 后退线") {
    ScriptedIo io;
    auto dispatcher = std::make_shared<Dispatcher>();
    auto connection = MakeConnection(dispatcher, io);

    std::string input(9 * 1024 * 1024, 'x'); // 超 8MB 上限,无换行
    io.pending_input = input;
    connection->Run();

    REQUIRE(io.written.size() == 1);
    const nlohmann::json response = ParseLine(io.written[0]);
    CHECK(response["id"].is_null());
    CHECK(response["error"]["code"] == kErrParseError);
    CHECK(connection->closed());
}

TEST_CASE("空行:静默跳过,不回错误") {
    ScriptedIo io;
    auto dispatcher = std::make_shared<Dispatcher>();
    auto connection = MakeConnection(dispatcher, io);

    io.pending_input = "\n\n{\"id\":1,\"method\":\"initialize\",\"params\":{}}\n";
    connection->Run();

    REQUIRE(io.written.size() == 1);
    CHECK(ParseLine(io.written[0])["id"] == 1);
}
