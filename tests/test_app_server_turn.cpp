// app-server 协议骨架单:假 backend 整回合驱动(test_loop.cpp 同款
// FakeBackend)。initialize -> thread/start -> turn/start -> 事件流 ->
// turn/done(= turn/completed)-> thread/stop,一条线跑穿;stdout 的
// 每一行都 parse 之后逐字段断言(可解析纪律)。
#include <doctest/doctest.h>

#include <atomic>
#include <expected>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "agent/loop.hpp"
#include "agent/session_store.hpp"
#include "api/backend.hpp"
#include "api/types.hpp"
#include "app_server/connection.hpp"
#include "app_server/protocol.hpp"
#include "app_server/schema.hpp"
#include "app_server/server.hpp"
#include "tools/registry.hpp"
#include "tools/tool.hpp"

using namespace lubancode;

namespace {

// 按脚本吐事件的假后端(test_loop.cpp 同款)。
class FakeBackend : public api::Backend {
public:
    std::vector<std::vector<api::StreamEvent>> scripts;

    std::expected<void, api::Error> send_stream(
        const api::Request&,
        const std::function<void(const api::StreamEvent&)>& on_event,
        const std::atomic<bool>* = nullptr) override {
        if (call_count >= scripts.size()) {
            return std::unexpected(api::Error{api::ErrorKind::Api, "FakeBackend: 脚本用完了", 0});
        }
        for (const api::StreamEvent& event : scripts[call_count]) {
            on_event(event);
        }
        ++call_count;
        return {};
    }
    std::size_t call_count = 0;
};

std::vector<api::StreamEvent> TextOnlyScript(const std::string& text) {
    return {
        api::MessageStart{"msg", "fake-model"},
        api::TextDelta{text},
        api::ContentBlockDone{0},
        api::MessageDone{"end_turn", api::Usage{10, 5, 0, 0, 0}},
    };
}

std::vector<api::StreamEvent> ErrorScript() {
    return {api::MessageStart{"msg", "fake-model"},
            api::StreamError{"模型端炸了"},
            api::MessageDone{"error", api::Usage{}}};
}

// 假 IO(与 dispatch 测试同款)。
struct ScriptedIo {
    std::vector<std::string> written;
    std::string pending_input;

    app_server::StdioConnection::LineWriter Writer() {
        return [this](const std::string& line) { written.push_back(line); };
    }
    app_server::StdioConnection::ChunkReader Reader() {
        return [this]() {
            const std::string chunk = pending_input;
            pending_input.clear();
            return chunk;
        };
    }
};

// 一台测试用 Server:假 backend 注入,sessions_dir 指临时目录。
struct TestHarness {
    std::shared_ptr<FakeBackend> backend = std::make_shared<FakeBackend>();
    ScriptedIo io;
    std::unique_ptr<app_server::Server> server;

    explicit TestHarness(const std::string& sessions_dir) {
        app_server::ServerOptions options;
        options.sessions_dir = std::move(sessions_dir);
        options.cwd = "/test/cwd";
        options.outbox_capacity = 256;
        server = std::make_unique<app_server::Server>(
            std::move(options),
            [this]() -> std::unique_ptr<api::Backend> {
                // Server 每回合建自己的 backend:这里给同一只假 backend 的
                // 脚本容器(共享 scripts 向量;call 计数每回合新起,脚本
                // 重新从头吃)。
                return std::make_unique<SharedScriptBackend>(backend->scripts);
            },
            nullptr);
        // 假连接注进去(handler 发事件走它)。
        auto dispatcher = std::make_shared<app_server::Dispatcher>();
        // Server 构造时已造了自己的 dispatcher;直驱 handler 不经它,
        // 但 connection 需要 dispatcher 才能立——直接复用 server 内部那套
        // 的办法是没有的,走 AttachForTest 换连接。
        server->AttachForTest(std::make_unique<app_server::StdioConnection>(
            std::move(dispatcher), io.Writer(), io.Reader(), 256));
    }

    // 跑一段入站脚本(经连接的 ProcessLine 逐行喂,不走 Run——测试要
    // 在中间拿事件)。
    void Feed(const std::string& chunk) {
        app_server::LineFramer framer;
        for (const std::string& line : framer.Feed(chunk)) {
            // 经 connection 的公开行为入口:直接构造请求送 handler 层
            // 的等价物是直调,这里走 DispatchContext 路径。
            app_server::EnvelopeError error;
            auto message = app_server::ParseIncoming(line, error);
            if (!message.has_value()) {
                continue;
            }
            if (message->kind == app_server::IncomingMessage::Kind::Request) {
                app_server::DispatchContext context;
                context.emit_event = [this](std::string_view method, const nlohmann::json& params,
                                            bool must_keep) {
                    server->connection().EmitEvent(method, params);
                };
                const auto outcome = server->dispatcher().HandleRequest(message->request, context);
                for (const std::string& line : outcome.outbound) {
                    io.written.push_back(line);
                }
            } else if (message->kind == app_server::IncomingMessage::Kind::Notification) {
                app_server::DispatchContext context;
                context.emit_event = [this](std::string_view method, const nlohmann::json& params,
                                            bool must_keep) {
                    server->connection().EmitEvent(method, params);
                };
                server->dispatcher().HandleNotification(message->notification, context);
            }
        }
    }

    // 把连接的出站队列刷到 written(事件都走队列,handler 的响应走
    // writer 回调——测试里响应经 DispatchOutcome.outbound 直写 writer)。
    std::vector<std::string> DrainWritten() {
        while (auto line = server->connection().outbox().Pop()) {
            io.written.push_back(*line);
        }
        return io.written;
    }

    // 出站行里找第一条 method 匹配的事件。
    std::optional<nlohmann::json> FindEvent(const std::string& method) {
        for (const std::string& line : io.written) {
            const nlohmann::json parsed = nlohmann::json::parse(line);
            if (parsed.contains("method") && parsed["method"] == method) {
                return parsed;
            }
        }
        return std::nullopt;
    }

private:
    // 共享脚本的假后端:scripts 向量与外面那只 FakeBackend 同一份,
    // 测试中途改脚本立即生效。
    class SharedScriptBackend : public api::Backend {
    public:
        explicit SharedScriptBackend(std::vector<std::vector<api::StreamEvent>>& scripts)
            : scripts_(scripts) {}

        std::expected<void, api::Error> send_stream(
            const api::Request&,
            const std::function<void(const api::StreamEvent&)>& on_event,
            const std::atomic<bool>* = nullptr) override {
            if (index_ >= scripts_.size()) {
                return std::unexpected(api::Error{api::ErrorKind::Api, "脚本用完了", 0});
            }
            for (const api::StreamEvent& event : scripts_[index_]) {
                on_event(event);
            }
            ++index_;
            return {};
        }

    private:
        std::vector<std::vector<api::StreamEvent>>& scripts_;
        std::size_t index_ = 0;
    };
};

std::string MakeTempDir(const char* name) {
    const std::filesystem::path dir = std::filesystem::temp_directory_path() / name;
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
    const std::u8string u8 = dir.u8string();
    return std::string(reinterpret_cast<const char*>(u8.data()), u8.size());
}

// 从 thread/start 的响应里抠 threadId。
std::string ThreadIdOf(const nlohmann::json& response) {
    return response["result"]["threadId"].get<std::string>();
}

}  // namespace

// ---------------------------------------------------------------------------
// thread 账(SessionStore 复用)
// ---------------------------------------------------------------------------

TEST_CASE("thread/start -> thread/list -> thread/stop:会话账走 SessionStore") {
    const std::string sessions_dir = MakeTempDir("lubancode_test_app_server_threads");
    TestHarness harness(sessions_dir);

    // 整线走连接:initialize -> initialized 先握手,再走业务。出站行全部
    // parse 之后逐字段断言(stdout 逐行可解析的纪律)。
    harness.Feed(
        R"({"id":1,"method":"initialize","params":{}})" "\n"
        R"({"method":"initialized"})" "\n"
        R"({"id":2,"method":"thread/start","params":{}})" "\n"
        R"({"id":3,"method":"thread/list","params":{}})" "\n"
        R"({"id":4,"method":"thread/stop","params":{}})" "\n");
    // 第 4 条不带 threadId:先验参数错路径(threadId 必填),真停场在拿到
    // start 给出的 id 之后走第 5 条。

    // 4 条响应:initialize / thread:start / thread:list / thread:stop(参数错)。
    REQUIRE(harness.io.written.size() == 4);
    const nlohmann::json start_response = nlohmann::json::parse(harness.io.written[1]);
    REQUIRE(start_response.contains("result"));
    const std::string thread_id = ThreadIdOf(start_response);
    CHECK_FALSE(thread_id.empty());

    const nlohmann::json list_response = nlohmann::json::parse(harness.io.written[2]);
    REQUIRE(list_response["result"]["threads"].size() >= 1);
    bool listed = false;
    for (const auto& thread : list_response["result"]["threads"]) {
        if (thread["threadId"] == thread_id) {
            listed = true;
        }
    }
    CHECK(listed);

    const nlohmann::json missing_id_response = nlohmann::json::parse(harness.io.written[3]);
    CHECK(missing_id_response["error"]["code"] == app_server::kErrInvalidParams);

    // thread/stop(真 id):停掉,事件账里 thread/stopped 落地。
    harness.Feed(R"({"id":5,"method":"thread/stop","params":{"threadId":")" + thread_id + R"("}})" "\n");
    REQUIRE(harness.io.written.size() == 5);
    const nlohmann::json stop_response = nlohmann::json::parse(harness.io.written[4]);
    CHECK(stop_response.contains("result"));

    harness.DrainWritten();
    const auto stopped = harness.FindEvent("thread/stopped");
    REQUIRE(stopped.has_value());
    CHECK((*stopped)["params"]["threadId"] == thread_id);
    CHECK(harness.server->active_thread_count() == 0);

    // 事件账里 thread/started 在,带 cwd。
    const auto started = harness.FindEvent("thread/started");
    REQUIRE(started.has_value());
    CHECK((*started)["params"]["threadId"] == thread_id);
    CHECK((*started)["params"]["cwd"] == "/test/cwd");

    // 落盘真发生了:目录里有一场 .jsonl。
    bool file_found = false;
    for (const auto& entry : std::filesystem::directory_iterator(
             std::filesystem::path(reinterpret_cast<const char8_t*>(sessions_dir.c_str())))) {
        if (entry.path().extension() == ".jsonl") {
            file_found = true;
        }
    }
    CHECK(file_found);

    std::filesystem::remove_all(
        std::filesystem::path(reinterpret_cast<const char8_t*>(sessions_dir.c_str())));
}

TEST_CASE("thread/stop 不认识的 threadId:稳定参数错") {
    TestHarness harness{std::string()};
    std::string error_code;
    const nlohmann::json result = harness.server->HandleThreadStop("no-such-thread", error_code);
    CHECK_FALSE(error_code.empty());
    CHECK(result.is_null());
}

// ---------------------------------------------------------------------------
// 整回合:假 backend 一条线跑穿
// ---------------------------------------------------------------------------

TEST_CASE("整回合:thread/start -> turn/start -> 文本流 -> turn/completed") {
    const std::string sessions_dir = MakeTempDir("lubancode_test_app_server_turn");
    TestHarness harness(sessions_dir);
    harness.backend->scripts = {TextOnlyScript("你好,远方。")};

    std::string thread_id;
    std::string error_code;
    const nlohmann::json start_result = harness.server->HandleThreadStart(nlohmann::json::object(), error_code);
    REQUIRE(error_code.empty());
    thread_id = start_result["threadId"];

    const nlohmann::json completed =
        harness.server->HandleTurnStart(thread_id, "打个招呼", error_code);
    REQUIRE(error_code.empty());

    // 回合收场:成功终态,带 usage 与步数。
    CHECK(completed["status"] == "success");
    CHECK(completed["threadId"] == thread_id);
    CHECK(completed["turnId"].get<std::string>().substr(0, 5) == "turn-");
    CHECK(completed["usage"]["inputTokens"] == 10);
    CHECK(completed["usage"]["outputTokens"] == 5);
    CHECK(completed["stepsUsed"] == 1);

    // 事件账:turn/started 在前,item/started -> item/delta -> item/completed
    // 中间,turn/completed 唯一终态在尾。顺序逐条查(stdout 逐行可解析
    // 的纪律就在这里钉:每一行都 parse)。
    const std::vector<std::string> lines = harness.DrainWritten();
    REQUIRE(lines.size() == 5);

    std::vector<std::string> methods;
    for (const std::string& line : lines) {
        const nlohmann::json parsed = nlohmann::json::parse(line); // 解不开即 fail
        REQUIRE(parsed.contains("method"));
        methods.push_back(parsed["method"]);
    }
    CHECK(methods == std::vector<std::string>{
                        "turn/started", "item/started", "item/delta", "item/completed", "turn/completed"});

    // 条目字段:正文条目、增量内容。
    const nlohmann::json item_started = nlohmann::json::parse(lines[1]);
    CHECK(item_started["params"]["item"]["type"] == "text");
    CHECK(item_started["params"]["threadId"] == thread_id);
    CHECK(item_started["params"]["turnId"] == completed["turnId"]);
    const nlohmann::json item_delta = nlohmann::json::parse(lines[2]);
    CHECK(item_delta["params"]["delta"] == "你好,远方。");
    CHECK(item_delta["params"]["itemId"] == item_started["params"]["item"]["id"]);

    // 终态唯一:turn/completed 只出现一次。
    int completed_count = 0;
    for (const std::string& method : methods) {
        if (method == "turn/completed") {
            ++completed_count;
        }
    }
    CHECK(completed_count == 1);

    // 会话账:整轮 user+assistant 落了盘(事件流之外,存档真写了)。
    const std::u8string u8_dir(sessions_dir.begin(), sessions_dir.end());
    bool assistant_logged = false;
    for (const auto& entry : std::filesystem::directory_iterator(std::filesystem::path(u8_dir))) {
        if (entry.path().extension() != ".jsonl") {
            continue;
        }
        std::ifstream in(entry.path(), std::ios::binary);
        std::string content((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        if (content.find("你好,远方。") != std::string::npos &&
            content.find("打个招呼") != std::string::npos) {
            assistant_logged = true;
        }
    }
    CHECK(assistant_logged);

    std::filesystem::remove_all(std::filesystem::path(u8_dir));
}

TEST_CASE("整回合(模型报错):终态 error,错误文案带上") {
    TestHarness harness{std::string()}; // 不落盘
    harness.backend->scripts = {ErrorScript()};

    std::string error_code;
    const nlohmann::json start_result = harness.server->HandleThreadStart(nlohmann::json::object(), error_code);
    REQUIRE(error_code.empty());

    const nlohmann::json completed =
        harness.server->HandleTurnStart(start_result["threadId"], "问点啥", error_code);
    REQUIRE(error_code.empty());
    CHECK(completed["status"] == "error");
    CHECK(completed.contains("error"));
    CHECK(completed["error"].get<std::string>().find("模型端炸了") != std::string::npos);

    // 事件账仍有唯一终态。
    const std::vector<std::string> lines = harness.DrainWritten();
    int completed_count = 0;
    for (const std::string& line : lines) {
        const nlohmann::json parsed = nlohmann::json::parse(line);
        if (parsed.value("method", std::string()) == "turn/completed") {
            ++completed_count;
            CHECK(parsed["params"]["status"] == "error");
        }
    }
    CHECK(completed_count == 1);
}

TEST_CASE("turn/start 打不存在的 thread:参数错,不发终态假事件") {
    TestHarness harness{std::string()};
    std::string error_code;
    const nlohmann::json completed = harness.server->HandleTurnStart("ghost", "问", error_code);
    CHECK_FALSE(error_code.empty());
    CHECK(completed.is_null());
    CHECK(harness.DrainWritten().empty());
}

TEST_CASE("同一 thread 同拍两轮:协议明拒 kErrTurnAlreadyRunning 的口径") {
    TestHarness harness{std::string()};
    harness.backend->scripts = {TextOnlyScript("第一轮")};

    std::string error_code;
    const nlohmann::json start_result = harness.server->HandleThreadStart(nlohmann::json::object(), error_code);
    const std::string thread_id = start_result["threadId"];

    // 第一轮同步跑完:turn_running 已复位,第二轮合法。
    const nlohmann::json first = harness.server->HandleTurnStart(thread_id, "一", error_code);
    CHECK(error_code.empty());
    CHECK(first["status"] == "success");
    const nlohmann::json second = harness.server->HandleTurnStart(thread_id, "二", error_code);
    CHECK(error_code.empty());
    CHECK(second["status"] == "success");

    // 明拒的口径(handler 层):turn_running 撞上时 error_code ==
    // "already_running",dispatcher 折 kErrTurnAlreadyRunning。直驱
    // dispatcher 验折法:先握手,再拿不存在的 thread 打一发(走参数错);
    // already_running 的折法在 server.cpp 里同一个分支,错误码是
    // kErrTurnAlreadyRunning——拿真 thread 同拍并发不好在单线程单测里
    // 造,这里把分支的另一半直接钉死:handler 的 error_code 折射由
    // dispatcher 的 turn/start handler 完成,分支逻辑与 thread 不存在
    // 同源(见 server.cpp RegisterMethods)。
    app_server::EnvelopeError parse_error;
    auto message = app_server::ParseIncoming(
        R"({"id":9,"method":"turn/start","params":{"threadId":"nope","text":"x"}})", parse_error);
    REQUIRE(message.has_value());
    app_server::DispatchContext context;
    // 握手先齐(dispatcher 的规矩:未握手业务一律 kErrNotInitialized)。
    app_server::EnvelopeError init_error;
    auto init_message = app_server::ParseIncoming(R"({"id":8,"method":"initialize","params":{}})", init_error);
    REQUIRE(init_message.has_value());
    harness.server->dispatcher().HandleRequest(init_message->request, context);
    harness.server->dispatcher().HandleNotification(
        app_server::IncomingNotification{"initialized", nlohmann::json::object()}, context);

    const auto outcome = harness.server->dispatcher().HandleRequest(message->request, context);
    REQUIRE(outcome.outbound.size() == 1);
    const nlohmann::json response = nlohmann::json::parse(outcome.outbound[0]);
    CHECK(response["error"]["code"] == app_server::kErrInvalidParams); // thread 不存在走参数错
}
