// app-server 浏览器面(可见调试工作台阶段 3):browser/* 方法与事件的
// 单测。真 Runtime 在 Node sidecar,这里注入一只假 sidecar(脚本化的
// mcp::Transport),钉死 C++ 协议层的规矩:
//
//   1. 协议升版与能力表:protocolVersion 1.1,browser 方法在 methods 里;
//   2. 参数表:url/pageId/kind/owner 的必填与枚举,owner=agent 须 threadId;
//   3. 同步查询直答(status/page list 的 camelCase 折算);
//   4. 异步动作:受理即回 actionId,action/started + action/completed
//      事件终态(与 sidecar 的往返、参数折算);
//   5. 审批:owner=agent 发 permission/request,accept/decline/取消三路;
//   6. 取消贯通:审批段取消 + sidecar 段取消(cancelled 通知真发出去);
//   7. journal 批量事件转发(seq 盖章、entries 原样);
//   8. 崩溃终态(browser/crashed)与 must_keep 分型;
//   9. outbox 的 journal 批合并(撞满时同页批量并成一条,dropped 求和);
//   10. 截图只发 artifact 引用:协议出站行里没有 base64;
//   11. 真进程 sidecar(起/复用/崩/收尸;缺 node 或缺脚本则整段跳过)。
#include <doctest/doctest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include <nlohmann/json.hpp>

#include "app_server/browser_service.hpp"
#include "app_server/connection.hpp"
#include "app_server/outbox.hpp"
#include "app_server/protocol.hpp"
#include "app_server/schema.hpp"
#include "app_server/server.hpp"
#include "agent/model_image_store.hpp"
#include "hooks/hash.hpp"
#include "mcp/transport.hpp"
#include "platform/paths.hpp"
#include "platform/process.hpp"

using namespace lubancode;

namespace {

// ---------------------------------------------------------------------------
// 假 sidecar:脚本化的 Transport。收到 app-server 的请求,按 handler 表
// 回响应(handler 可换成 defer 档做取消测试);cancelled 通知记账;
// PushEvent 从"sidecar 侧"递事件。
// ---------------------------------------------------------------------------

class FakeSidecar final : public mcp::Transport {
public:
    using Handler = std::function<nlohmann::json(const nlohmann::json& params)>;

    // 出站(假 sidecar -> app-server)的落点:由夹具绑到
    // BrowserService::OnSidecarLine。
    std::function<void(const std::string&)> feed;

    bool WriteLine(const std::string& line) override {
        nlohmann::json message;
        try {
            message = nlohmann::json::parse(line);
        } catch (const nlohmann::json::exception&) {
            return false;
        }
        if (!message.is_object()) {
            return true;
        }
        const std::string method = message.value("method", std::string());
        if (method == "cancelled") {
            std::lock_guard<std::mutex> lock(mutex_);
            cancelled_requests.push_back(message["params"].value("requestId", std::int64_t{0}));
            return true;
        }
        if (!message.contains("id")) {
            return true; // 别的通知不认
        }
        const std::int64_t id = message["id"].get<std::int64_t>();
        {
            std::lock_guard<std::mutex> lock(mutex_);
            requests.push_back(message);
        }
        Handler handler;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            const auto it = handlers.find(method);
            if (it != handlers.end()) {
                handler = it->second;
            }
        }
        if (handler) {
            nlohmann::json result = handler(message.value("params", nlohmann::json::object()));
            if (result.is_object() && result.contains("__error__")) {
                RespondError(id, result.value("__error__", std::string("browser.sidecar_error")),
                             result.value("__message__", std::string()));
            } else {
                Respond(id, result);
            }
        }
        return alive_.load();
    }

    void Shutdown(int) override { alive_.store(false); }
    bool IsAlive() const override { return alive_.load(); }
    std::string StderrTail() const override { return std::string(); }

    void Respond(std::int64_t id, const nlohmann::json& result) {
        if (feed) {
            feed(nlohmann::json{{"jsonrpc", "2.0"}, {"id", id}, {"result", result}}.dump());
        }
    }

    void RespondError(std::int64_t id, const std::string& browser_code, const std::string& message) {
        if (feed) {
            feed(nlohmann::json{{"jsonrpc", "2.0"},
                                {"id", id},
                                {"error", {{"code", -32000},
                                           {"message", message},
                                           {"data", {{"browserCode", browser_code}}}}}}
                     .dump());
        }
    }

    void PushEvent(const nlohmann::json& params) {
        if (feed) {
            feed(nlohmann::json{{"jsonrpc", "2.0"}, {"method", "event"}, {"params", params}}.dump());
        }
    }

    void SetHandler(const std::string& method, Handler handler) {
        std::lock_guard<std::mutex> lock(mutex_);
        handlers[method] = std::move(handler);
    }

    std::vector<nlohmann::json> TakeRequests() {
        std::lock_guard<std::mutex> lock(mutex_);
        return requests;
    }

    std::vector<std::int64_t> TakeCancellations() {
        std::lock_guard<std::mutex> lock(mutex_);
        return cancelled_requests;
    }

private:
    std::atomic<bool> alive_{true};
    mutable std::mutex mutex_;
    std::map<std::string, Handler> handlers;
    std::vector<nlohmann::json> requests;
    std::vector<std::int64_t> cancelled_requests;
};

// ---------------------------------------------------------------------------
// 夹具:Server + 假连接 + 假 sidecar。
// ---------------------------------------------------------------------------

struct ScriptedWriter {
    std::vector<std::string> written;
    app_server::StdioConnection::LineWriter Writer() {
        return [this](const std::string& line) { written.push_back(line); };
    }
};

struct BrowserHarness {
    ScriptedWriter io;
    FakeSidecar sidecar;
    std::unique_ptr<app_server::Server> server;
    std::string artifact_dir;

    BrowserHarness() {
        // 截图 artifact 落临时目录(测试自己收尾)。
        const std::filesystem::path temp = std::filesystem::temp_directory_path() /
                                           ("lubancode-browser-test-" +
                                            std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        std::filesystem::create_directories(temp);
        artifact_dir = platform::PathToUtf8(temp);

        app_server::ServerOptions options;
        options.sessions_dir = std::string(); // 不落盘
        options.cwd = "/test/cwd";
        options.browser_sidecar_command = "node"; // 有命令才会走 EnsureSidecar 的成功路(测试注入 transport 后不 spawn)
        options.browser_artifact_dir = artifact_dir;
        server = std::make_unique<app_server::Server>(
            std::move(options), []() -> std::unique_ptr<api::Backend> { return nullptr; }, nullptr);
        server->browser_service().AttachTransportForTest(&sidecar);
        sidecar.feed = [this](const std::string& line) { server->browser_service().OnSidecarLine(line); };
        // 假连接:writer 收行,reader 恒 EOF(测试手动驱动 ProcessLine)。
        server->AttachForTest(std::make_unique<app_server::StdioConnection>(
            server->dispatcher_handle(), io.Writer(), []() { return std::string(); }, 4096));
        // 握手一次,后面的测试直接喂业务行。
        Feed(R"({"id":1,"method":"initialize","params":{}})");
        Feed(R"({"method":"initialized"})");
    }

    ~BrowserHarness() {
        server.reset(); // Shutdown 收线(杀真 sidecar 不会碰注入的假 transport)
        std::error_code ec;
        std::filesystem::remove_all(platform::Utf8ToPath(artifact_dir), ec);
    }

    // 喂一行入站协议:解析、路由(dispatcher)、事件出水(connection 的
    // 出站口)。与 StdioConnection::ProcessLine 同一条路(它是 private,
    // 这里照抄装配,行为一致)。principal 是内核盖的连接身份(阶段 B 的
    // owner 裁定用;缺省 "user"——直驱的口径,与生产连接的盖章一致)。
    void Feed(const std::string& line) { Feed(line, "user"); }
    void Feed(const std::string& line, const std::string& principal) {
        app_server::EnvelopeError envelope_error;
        const std::optional<app_server::IncomingMessage> message = app_server::ParseIncoming(line, envelope_error);
        if (!message.has_value()) {
            const nlohmann::json error = envelope_error.has_id
                                             ? app_server::MakeError(envelope_error.id, envelope_error.code,
                                                                     envelope_error.message)
                                             : app_server::MakeErrorForUnparseable(envelope_error.code,
                                                                                   envelope_error.message);
            io.written.push_back(app_server::SerializeMessage(error));
            return;
        }
        app_server::DispatchContext context;
        context.principal = principal;
        context.emit_event = [this](std::string_view method, const nlohmann::json& params, bool) {
            server->connection().EmitEvent(method, params);
        };
        context.resolve_interaction = [this](const app_server::IncomingResponse& response) -> std::string {
            const app_server::InteractionResolution result = server->HandleInteractionResponse(response);
            if (result.ok) {
                return std::string();
            }
            return result.error_code.empty() ? std::string("stale_request_id") : result.error_code;
        };
        app_server::DispatchOutcome outcome;
        switch (message->kind) {
            case app_server::IncomingMessage::Kind::Request:
                outcome = server->dispatcher().HandleRequest(message->request, context);
                break;
            case app_server::IncomingMessage::Kind::Notification:
                outcome = server->dispatcher().HandleNotification(message->notification, context);
                break;
            case app_server::IncomingMessage::Kind::Response:
                outcome = server->dispatcher().HandleResponse(message->response, context);
                break;
        }
        for (const std::string& outbound : outcome.outbound) {
            io.written.push_back(outbound);
        }
    }

    void PumpOutbox() {
        while (auto line = server->connection().outbox().Pop()) {
            io.written.push_back(*line);
        }
    }

    std::optional<nlohmann::json> FindEvent(const std::string& method) {
        PumpOutbox();
        for (const std::string& line : io.written) {
            const nlohmann::json parsed = nlohmann::json::parse(line);
            if (parsed.contains("method") && parsed["method"] == method) {
                return parsed;
            }
        }
        return std::nullopt;
    }

    // 等一条事件出站(异步动作的工作线程要一口时间)。
    std::optional<nlohmann::json> WaitForEvent(const std::string& method, int timeout_ms = 5000) {
        for (int i = 0; i < timeout_ms / 5; ++i) {
            if (auto event = FindEvent(method)) {
                return event;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        return std::nullopt;
    }

    // 最新一条响应(id 配对)。
    std::optional<nlohmann::json> FindResponse(std::int64_t id) {
        PumpOutbox();
        for (const std::string& line : io.written) {
            const nlohmann::json parsed = nlohmann::json::parse(line);
            if (parsed.contains("id") && parsed["id"] == id) {
                return parsed;
            }
        }
        return std::nullopt;
    }

    // 找某 actionId 的 completed 事件。
    std::optional<nlohmann::json> FindActionCompleted(const std::string& action_id) {
        PumpOutbox();
        for (const std::string& line : io.written) {
            const nlohmann::json parsed = nlohmann::json::parse(line);
            if (parsed.value("method", std::string()) == "browser/action/completed" &&
                parsed["params"].value("actionId", std::string()) == action_id) {
                return parsed;
            }
        }
        return std::nullopt;
    }

    // 等某 actionId 的 completed(动作工作线程要一口时间)。
    std::optional<nlohmann::json> WaitForActionCompleted(const std::string& action_id, int timeout_ms = 5000) {
        for (int i = 0; i < timeout_ms / 5; ++i) {
            if (auto event = FindActionCompleted(action_id)) {
                return event;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        return std::nullopt;
    }

    // 前端答复审批(反向请求的响应信封)。
    void AnswerApproval(const std::string& request_id, const std::string& decision) {
        Feed(nlohmann::json{{"id", 0}, {"result", {{"requestId", request_id}, {"decision", decision}}}}.dump());
    }

    // 等一枚没答过的审批请求(WaitForEvent 会翻旧事件,同场多枚审批时
    // 按 requestId 排重,拿最新的未答那枚)。
    std::optional<nlohmann::json> WaitForPermission(const std::vector<std::string>& answered,
                                                    int timeout_ms = 5000) {
        for (int i = 0; i < timeout_ms / 5; ++i) {
            PumpOutbox();
            for (auto it = io.written.rbegin(); it != io.written.rend(); ++it) {
                const nlohmann::json parsed = nlohmann::json::parse(*it);
                if (parsed.value("method", std::string()) == "permission/request") {
                    const std::string rid = parsed["params"].value("requestId", std::string());
                    if (std::find(answered.begin(), answered.end(), rid) == answered.end()) {
                        return parsed;
                    }
                }
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        return std::nullopt;
    }
};

// 一枚 1x1 PNG(base64),截图 artifact 测试用。
const char* kTinyPngBase64 =
    "iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAYAAAAfFcSJAAAADUlEQVR42mP8z8BQDwAEhQGAhKmMIQAAAABJRU5ErkJggg==";

}  // namespace

// ---------------------------------------------------------------------------
// 协议面
// ---------------------------------------------------------------------------

TEST_CASE("协议:版本升 1.1,browser 方法进能力表,must_keep 分型") {
    CHECK(app_server::kProtocolVersion == "1.1");
    BrowserHarness harness;
    const std::optional<nlohmann::json> init = harness.FindResponse(1);
    REQUIRE(init.has_value());
    const nlohmann::json& methods = (*init)["result"]["capabilities"]["methods"];
    bool has_action = false;
    bool has_console_query = false;
    for (const auto& m : methods) {
        if (m == "browser/action") has_action = true;
        if (m == "browser/console/query") has_console_query = true;
    }
    CHECK(has_action);
    CHECK(has_console_query);

    // must_keep:终态与无查询口的保,可补账的可丢。
    CHECK(app_server::EventMustKeep("browser/action/completed"));
    CHECK(app_server::EventMustKeep("browser/crashed"));
    CHECK(app_server::EventMustKeep("browser/stopped"));
    CHECK(app_server::EventMustKeep("browser/screenshot/ready"));
    CHECK_FALSE(app_server::EventMustKeep("browser/console/event"));
    CHECK_FALSE(app_server::EventMustKeep("browser/navigation"));
}

TEST_CASE("参数表:url/pageId/kind/owner 各归各位") {
    BrowserHarness harness;
    harness.Feed(R"({"id":10,"method":"browser/page/open","params":{}})");
    auto response = harness.FindResponse(10);
    REQUIRE(response.has_value());
    CHECK((*response)["error"]["code"] == app_server::kErrInvalidParams);

    harness.Feed(R"({"id":11,"method":"browser/page/navigate","params":{"pageId":"p1"}})");
    response = harness.FindResponse(11);
    REQUIRE(response.has_value());
    CHECK((*response)["error"]["code"] == app_server::kErrInvalidParams);

    harness.Feed(R"({"id":12,"method":"browser/action","params":{"kind":"hover","ref":"e1"}})");
    response = harness.FindResponse(12);
    REQUIRE(response.has_value());
    CHECK((*response)["error"]["code"] == app_server::kErrInvalidParams);
    CHECK((*response)["error"]["message"].get<std::string>().find("click|type|select|wait") != std::string::npos);

    harness.Feed(R"({"id":13,"method":"browser/action","params":{"kind":"wait"}})");
    response = harness.FindResponse(13);
    REQUIRE(response.has_value());
    CHECK((*response)["error"]["code"] == app_server::kErrInvalidParams);

    // owner=agent 须带 threadId。
    harness.Feed(R"({"id":14,"method":"browser/action","params":{"kind":"click","ref":"e1","owner":"agent"}})");
    response = harness.FindResponse(14);
    REQUIRE(response.has_value());
    CHECK((*response)["error"]["data"]["reason"] == "browser.thread_required");

    // journal 查询的 cursor 形状。
    harness.Feed(R"({"id":15,"method":"browser/console/query","params":{"pageId":"p1","sinceSeq":-3}})");
    response = harness.FindResponse(15);
    REQUIRE(response.has_value());
    CHECK((*response)["error"]["code"] == app_server::kErrInvalidParams);

    // 握手前不认:另一台没握手的连接直接调 browser 方法。这里已握手,
    // 钉一条未知 browser 方法名回 -32601。
    harness.Feed(R"({"id":16,"method":"browser/no-such","params":{}})");
    response = harness.FindResponse(16);
    REQUIRE(response.has_value());
    CHECK((*response)["error"]["code"] == app_server::kErrMethodNotFound);
}

// ---------------------------------------------------------------------------
// 同步查询与异步动作
// ---------------------------------------------------------------------------

TEST_CASE("同步查询:status 直答带 sidecarRunning,page/list 折 camelCase") {
    BrowserHarness harness;
    harness.sidecar.SetHandler("session/status", [](const nlohmann::json&) {
        return nlohmann::json{{"sessionId", "s1"}, {"engine", "chromium"}, {"launched", false}, {"pages", 0}};
    });
    harness.sidecar.SetHandler("page/list", [](const nlohmann::json&) {
        return nlohmann::json::array({{{"page_id", "p1"}, {"url", "http://x/"}, {"generation", 2}, {"active", true}}});
    });

    harness.Feed(R"({"id":20,"method":"browser/status","params":{}})");
    auto response = harness.FindResponse(20);
    REQUIRE(response.has_value());
    CHECK((*response)["result"]["sessionId"] == "s1");
    CHECK((*response)["result"]["sidecarRunning"] == true);

    harness.Feed(R"({"id":21,"method":"browser/page/list","params":{}})");
    response = harness.FindResponse(21);
    REQUIRE(response.has_value());
    CHECK((*response)["result"][0]["pageId"] == "p1");
    CHECK_FALSE((*response)["result"][0].contains("page_id"));
}

TEST_CASE("异步动作:受理即回 actionId,started/completed 事件终态,参数折算") {
    BrowserHarness harness;
    harness.sidecar.SetHandler("page/open", [](const nlohmann::json& params) {
        return nlohmann::json{{"pageId", "p1"}, {"url", params.value("url", std::string())},
                              {"title", "验收站"}, {"generation", 1}};
    });

    harness.Feed(R"({"id":30,"method":"browser/page/open","params":{"url":"http://127.0.0.1:1/","newPage":true}})");
    auto response = harness.FindResponse(30);
    REQUIRE(response.has_value());
    REQUIRE((*response)["result"]["accepted"] == true);
    const std::string action_id = (*response)["result"]["actionId"];
    CHECK_FALSE(action_id.empty());

    const auto completed = harness.WaitForEvent("browser/action/completed");
    REQUIRE(completed.has_value());
    CHECK((*completed)["params"]["actionId"] == action_id);
    CHECK((*completed)["params"]["ok"] == true);
    CHECK((*completed)["params"]["result"]["pageId"] == "p1");
    CHECK((*completed)["params"]["durationMs"].is_number());
    // started 也在(先于 completed)。
    const auto started = harness.FindEvent("browser/action/started");
    REQUIRE(started.has_value());
    CHECK((*started)["params"]["actionId"] == action_id);
    CHECK((*started)["params"]["owner"] == "user");

    // sidecar 收到的请求:参数折算后带 url/newPage;owner 随裁定值转给
    // sidecar(阶段 B:owner=user 的输入动作执行后靠它递 userEpoch)。
    const auto requests = harness.sidecar.TakeRequests();
    REQUIRE(requests.size() >= 1);
    const nlohmann::json& sent = requests.back();
    CHECK(sent["method"] == "page/open");
    CHECK(sent["params"]["url"] == "http://127.0.0.1:1/");
    CHECK(sent["params"]["newPage"] == true);
    CHECK(sent["params"]["owner"] == "user");
}

TEST_CASE("异步动作:sidecar 报错折 error.code,completed ok=false") {
    BrowserHarness harness;
    // __error__ 形状 = FakeSidecar 回错误信封(data.browserCode 带浏览器
    // 稳定码,C++ 侧折进 completed 的 error.code)。
    harness.sidecar.SetHandler("action", [](const nlohmann::json&) -> nlohmann::json {
        return nlohmann::json{{"__error__", "browser.stale_ref"}, {"__message__", "ref 已过期,重新快照。"}};
    });

    harness.Feed(R"({"id":40,"method":"browser/action","params":{"kind":"click","ref":"e9"}})");
    auto response = harness.FindResponse(40);
    REQUIRE(response.has_value());
    CHECK((*response)["result"]["accepted"] == true);
    const auto completed = harness.WaitForEvent("browser/action/completed");
    REQUIRE(completed.has_value());
    CHECK((*completed)["params"]["ok"] == false);
    CHECK((*completed)["params"]["error"]["code"] == "browser.stale_ref");
    CHECK((*completed)["params"]["error"]["message"].get<std::string>().find("重新快照") != std::string::npos);
}

// ---------------------------------------------------------------------------
// 审批与取消
// ---------------------------------------------------------------------------

TEST_CASE("审批:owner=agent 发 permission/request,accept 放行到 sidecar") {
    BrowserHarness harness;
    bool sidecar_called = false;
    harness.sidecar.SetHandler("action", [&](const nlohmann::json&) {
        sidecar_called = true;
        return nlohmann::json{{"pageId", "p1"}, {"clickedRef", "e1"}};
    });

    // 起 thread(审批挂这场上)。
    std::string error_code;
    const nlohmann::json start = harness.server->HandleThreadStart(nlohmann::json::object(), error_code);
    REQUIRE(error_code.empty());
    const std::string thread_id = start["threadId"];

    harness.Feed(nlohmann::json{{"id", 50},
                                {"method", "browser/action"},
                                {"params",
                                 {{"kind", "click"}, {"ref", "e1"}, {"owner", "agent"}, {"threadId", thread_id}}}}
                     .dump());
    auto response = harness.FindResponse(50);
    REQUIRE(response.has_value());
    CHECK((*response)["result"]["accepted"] == true);

    const auto permission = harness.WaitForEvent("permission/request");
    REQUIRE(permission.has_value());
    CHECK((*permission)["params"]["threadId"] == thread_id);
    CHECK((*permission)["params"]["tool"] == "browser/action");
    CHECK((*permission)["params"]["input"]["kind"] == "click");
    const std::string request_id = (*permission)["params"]["requestId"];

    harness.AnswerApproval(request_id, "accept");
    const auto completed = harness.WaitForEvent("browser/action/completed");
    REQUIRE(completed.has_value());
    CHECK((*completed)["params"]["ok"] == true);
    CHECK(sidecar_called);
}

TEST_CASE("审批:decline 拒绝,sidecar 一个字都没收到") {
    BrowserHarness harness;
    bool sidecar_called = false;
    harness.sidecar.SetHandler("action", [&](const nlohmann::json&) {
        sidecar_called = true;
        return nlohmann::json::object();
    });
    std::string error_code;
    const std::string thread_id =
        harness.server->HandleThreadStart(nlohmann::json::object(), error_code)["threadId"];

    harness.Feed(nlohmann::json{{"id", 51},
                                {"method", "browser/page/navigate"},
                                {"params",
                                 {{"pageId", "p1"}, {"url", "http://127.0.0.1:1/"}, {"owner", "agent"},
                                  {"threadId", thread_id}}}}
                     .dump());
    const auto permission = harness.WaitForEvent("permission/request");
    REQUIRE(permission.has_value());
    harness.AnswerApproval((*permission)["params"]["requestId"], "decline");

    const auto completed = harness.WaitForEvent("browser/action/completed");
    REQUIRE(completed.has_value());
    CHECK((*completed)["params"]["ok"] == false);
    CHECK((*completed)["params"]["error"]["code"] == "browser.permission_denied");
    CHECK_FALSE(sidecar_called);
}

TEST_CASE("取消:审批悬着时 browser/action/cancel,completed cancelled") {
    BrowserHarness harness;
    harness.sidecar.SetHandler("action", [](const nlohmann::json&) { return nlohmann::json::object(); });
    std::string error_code;
    const std::string thread_id =
        harness.server->HandleThreadStart(nlohmann::json::object(), error_code)["threadId"];

    harness.Feed(nlohmann::json{{"id", 52},
                                {"method", "browser/action"},
                                {"params",
                                 {{"kind", "click"}, {"ref", "e1"}, {"owner", "agent"}, {"threadId", thread_id}}}}
                     .dump());
    const auto permission = harness.WaitForEvent("permission/request");
    REQUIRE(permission.has_value());
    const std::string action_id = harness.FindResponse(52).value()["result"]["actionId"];

    harness.Feed(nlohmann::json{{"id", 53}, {"method", "browser/action/cancel"}, {"params", {{"actionId", action_id}}}}
                     .dump());
    auto cancel_response = harness.FindResponse(53);
    REQUIRE(cancel_response.has_value());
    CHECK((*cancel_response)["result"]["cancelled"] == true);

    const auto completed = harness.WaitForEvent("browser/action/completed");
    REQUIRE(completed.has_value());
    CHECK((*completed)["params"]["ok"] == false);
    CHECK((*completed)["params"]["cancelled"] == true);
    CHECK((*completed)["params"].value("error", nlohmann::json::object()).value("code", std::string()) ==
          "browser.approval_cancelled");

    // 迟到的审批答复报 stale。
    harness.Feed(nlohmann::json{{"id", 0},
                                {"result", {{"requestId", (*permission)["params"]["requestId"]},
                                            {"decision", "accept"}}}}
                     .dump());
    const auto stale = harness.FindResponse(0);
    REQUIRE(stale.has_value());
    CHECK((*stale)["error"]["code"] == app_server::kErrStaleRequestId);
}

TEST_CASE("取消:sidecar 在飞时取消,cancelled 通知真发到 sidecar") {
    BrowserHarness harness;
    // defer 档:sidecar 收到请求先不回,等 cancelled 通知到了才按取消收口
    //(轮询型动作的语义,与 session.wait 的 url/固定等待同款)。
    harness.sidecar.SetHandler("action", [&](const nlohmann::json&) -> nlohmann::json {
        for (int i = 0; i < 600; ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
            if (!harness.sidecar.TakeCancellations().empty()) {
                return nlohmann::json{{"cancelled", true},
                                      {"code", "browser.cancelled"},
                                      {"message", "动作已取消。"}};
            }
        }
        return nlohmann::json{{"pageId", "p1"}};
    });

    harness.Feed(R"({"id":54,"method":"browser/action","params":{"kind":"wait","ms":60000,"timeoutMs":60000}})");
    auto response = harness.FindResponse(54);
    REQUIRE(response.has_value());
    const std::string action_id = (*response)["result"]["actionId"];

    // 等 sidecar 收到请求(在飞)再取消。
    for (int i = 0; i < 400 && harness.sidecar.TakeRequests().empty(); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    REQUIRE_FALSE(harness.sidecar.TakeRequests().empty());

    harness.Feed(nlohmann::json{{"id", 55}, {"method", "browser/action/cancel"}, {"params", {{"actionId", action_id}}}}
                     .dump());
    const auto completed = harness.WaitForEvent("browser/action/completed", 10000);
    REQUIRE(completed.has_value());
    CHECK((*completed)["params"]["cancelled"] == true);
    CHECK_FALSE((*completed)["params"]["ok"]);
    // cancelled 通知确实发给了 sidecar(TakeCancellations 被上面的 handler
    // 消费过一次——重取非空与否不定,这里钉 handler 的退出路径即可)。
    CHECK((*completed)["params"]["error"]["code"] == "browser.cancelled");
}

TEST_CASE("取消:动作收口后再取消报 stale") {
    BrowserHarness harness;
    harness.sidecar.SetHandler("action", [](const nlohmann::json&) { return nlohmann::json{{"pageId", "p1"}}; });
    harness.Feed(R"({"id":56,"method":"browser/action","params":{"kind":"click","ref":"e1"}})");
    const auto completed = harness.WaitForEvent("browser/action/completed");
    REQUIRE(completed.has_value());
    const std::string action_id = (*completed)["params"]["actionId"];
    harness.Feed(nlohmann::json{{"id", 57}, {"method", "browser/action/cancel"}, {"params", {{"actionId", action_id}}}}
                     .dump());
    const auto response = harness.FindResponse(57);
    REQUIRE(response.has_value());
    CHECK((*response)["error"]["code"] == app_server::kErrStaleRequestId);
    CHECK((*response)["error"]["data"]["reason"] == "browser.stale_action");
}

// ---------------------------------------------------------------------------
// 阶段 B:用户输入路由与暂停(多前端外壳单)
// ---------------------------------------------------------------------------

TEST_CASE("阶段B·暂停:Agent 动作受理不执行,用户动作照走,终态照发") {
    BrowserHarness harness;
    std::atomic<int> agent_sidecar_calls{0};
    std::atomic<int> user_sidecar_calls{0};
    harness.sidecar.SetHandler("action", [&](const nlohmann::json& params) {
        if (params.value("owner", std::string()) == "agent") {
            agent_sidecar_calls.fetch_add(1);
        } else {
            user_sidecar_calls.fetch_add(1);
        }
        return nlohmann::json{{"pageId", "p1"}, {"clickedRef", params.value("ref", std::string("e1"))}};
    });
    std::string error_code;
    const std::string thread_id =
        harness.server->HandleThreadStart(nlohmann::json::object(), error_code)["threadId"];

    // pause:用户连接的手闸,同步直答 + must_keep 通报。
    harness.Feed(R"({"id":70,"method":"browser/pause","params":{}})");
    auto response = harness.FindResponse(70);
    REQUIRE(response.has_value());
    CHECK((*response)["result"]["paused"] == true);
    const auto paused_event = harness.WaitForEvent("browser/paused");
    REQUIRE(paused_event.has_value());
    CHECK((*paused_event)["params"]["paused"] == true);
    CHECK(harness.server->browser_service().paused());

    // 暂停期间 agent 动作:受理照回 actionId,终态明报 browser.paused,
    // sidecar 一个字没收到,审批也不发(暂停门在审批之前)。
    harness.Feed(nlohmann::json{{"id", 71},
                                {"method", "browser/action"},
                                {"params",
                                 {{"kind", "click"}, {"ref", "e1"}, {"owner", "agent"}, {"threadId", thread_id}}}}
                     .dump());
    response = harness.FindResponse(71);
    REQUIRE(response.has_value());
    REQUIRE((*response)["result"]["accepted"] == true);
    const std::string paused_action_id = (*response)["result"]["actionId"];
    const auto paused_completed = harness.WaitForActionCompleted(paused_action_id);
    REQUIRE(paused_completed.has_value());
    CHECK((*paused_completed)["params"]["ok"] == false);
    CHECK((*paused_completed)["params"]["error"]["code"] == "browser.paused");
    CHECK(harness.FindEvent("permission/request") == std::nullopt);

    // 暂停期间用户动作照走(不带 owner/threadId——真用户路的形状)。
    harness.Feed(R"({"id":72,"method":"browser/action","params":{"kind":"click","ref":"e2"}})");
    response = harness.FindResponse(72);
    REQUIRE(response.has_value());
    REQUIRE((*response)["result"]["accepted"] == true);
    CHECK((*response)["result"]["owner"] == "user");
    const auto user_completed = harness.WaitForActionCompleted((*response)["result"]["actionId"]);
    REQUIRE(user_completed.has_value());
    CHECK((*user_completed)["params"]["ok"] == true);
    CHECK(user_sidecar_calls.load() == 1);

    // resume:旗落,agent 动作照走(过审批)。
    harness.Feed(R"({"id":73,"method":"browser/resume","params":{}})");
    response = harness.FindResponse(73);
    REQUIRE(response.has_value());
    CHECK((*response)["result"]["paused"] == false);
    CHECK(harness.WaitForEvent("browser/resumed").has_value());
    CHECK_FALSE(harness.server->browser_service().paused());

    harness.Feed(nlohmann::json{{"id", 74},
                                {"method", "browser/action"},
                                {"params",
                                 {{"kind", "click"}, {"ref", "e3"}, {"owner", "agent"}, {"threadId", thread_id}}}}
                     .dump());
    response = harness.FindResponse(74);
    REQUIRE(response.has_value());
    const auto permission = harness.WaitForEvent("permission/request");
    REQUIRE(permission.has_value());
    const std::string answered_request = (*permission)["params"]["requestId"];
    harness.AnswerApproval(answered_request, "accept");
    const auto agent_completed =
        harness.WaitForActionCompleted(response->at("result").value("actionId", std::string()));
    REQUIRE(agent_completed.has_value());
    CHECK((*agent_completed)["params"]["ok"] == true);
    CHECK(agent_sidecar_calls.load() == 1);

    // 审批悬着时才拨下暂停的:放了也不执行——暂停对 owner=agent 的动作
    // 一律生效,不分受理先后。
    harness.Feed(nlohmann::json{{"id", 75},
                                {"method", "browser/action"},
                                {"params",
                                 {{"kind", "click"}, {"ref", "e4"}, {"owner", "agent"}, {"threadId", thread_id}}}}
                     .dump());
    response = harness.FindResponse(75);
    REQUIRE(response.has_value());
    const auto pending_permission = harness.WaitForPermission({answered_request});
    REQUIRE(pending_permission.has_value());
    harness.Feed(R"({"id":76,"method":"browser/pause","params":{}})");
    REQUIRE(harness.FindResponse(76).has_value());
    harness.AnswerApproval((*pending_permission)["params"]["requestId"], "accept");
    const auto late_completed =
        harness.WaitForActionCompleted(response->at("result").value("actionId", std::string()));
    REQUIRE(late_completed.has_value());
    CHECK((*late_completed)["params"]["ok"] == false);
    CHECK((*late_completed)["params"]["error"]["code"] == "browser.paused");
    CHECK(agent_sidecar_calls.load() == 1); // 放了也没碰 sidecar
    // 旗回落,别把暂停留给后续用例。
    harness.Feed(R"({"id":77,"method":"browser/resume","params":{}})");
    REQUIRE(harness.FindResponse(77).has_value());
}

TEST_CASE("阶段B·让路:用户动作不排在 Agent 动作后头") {
    BrowserHarness harness;
    // 慢档:每笔 sidecar 调用悬 250ms——第一笔占住在途,后面的排队。
    std::mutex order_mutex;
    std::vector<std::string> sidecar_order;
    const auto note = [&](const std::string& tag) {
        std::lock_guard<std::mutex> lock(order_mutex);
        sidecar_order.push_back(tag);
    };
    harness.sidecar.SetHandler("action", [&](const nlohmann::json& params) {
        note(std::string("action:") + params.value("owner", std::string("?")));
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
        return nlohmann::json{{"pageId", "p1"}};
    });
    harness.sidecar.SetHandler("snapshot", [&](const nlohmann::json& params) {
        note(std::string("snapshot:") + params.value("owner", std::string("?")));
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
        return nlohmann::json{{"snapshotId", "p1-g1-s1"}, {"text", "- button \"x\" [ref=e1]"}};
    });

    // 在途:agent 快照(读动作,不过审批,但占着唯一的动作工位)。
    // 首笔等待放 40s:慢 runner 上 2s 不够,而 REQUIRE 一 abort,handler
    // 引用的栈账(sidecar_order/note)还在工作线程手里,拆栈即 SIGSEGV
    // (CI ubuntu 实翻)。断言改 CHECK+早退,退场前先摘 handler 再拆栈。
    harness.Feed(R"({"id":80,"method":"browser/snapshot","params":{"owner":"agent","threadId":"t-arb"}})");
    for (int i = 0; i < 8000; i++) {
        std::lock_guard<std::mutex> lock(order_mutex);
        if (!sidecar_order.empty()) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    if ([&] {
            std::lock_guard<std::mutex> lock(order_mutex);
            return sidecar_order.size();
        }() != 1) {
        CHECK_MESSAGE(false, "首笔 agent 快照未在 40s 内进 sidecar(慢 runner 或 dispatch 病)");
        return; // 早退不 REQUIRE-abort:留给析构安全收线
    }

    // 排队:agent 快照二号,再用户点击。用户点击必须先于 agent 二号跑。
    harness.Feed(R"({"id":81,"method":"browser/snapshot","params":{"owner":"agent","threadId":"t-arb"}})");
    harness.Feed(R"({"id":82,"method":"browser/action","params":{"kind":"click","ref":"e1"}})");
    for (int i = 0; i < 6000; i++) {
        std::lock_guard<std::mutex> lock(order_mutex);
        if (sidecar_order.size() == 3) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    std::vector<std::string> order;
    {
        std::lock_guard<std::mutex> lock(order_mutex);
        order = sidecar_order;
    }
    if (order.size() != 3) {
        CHECK_MESSAGE(false, "三笔未在 60s 内收齐");
        return; // 早退,别 REQUIRE-abort
    }
    CHECK(order[0] == "snapshot:agent"); // 在途的让不了(串行底线)
    CHECK(order[1] == "action:user");    // 用户插队,排到 agent 二号前头
    CHECK(order[2] == "snapshot:agent"); // Agent 让路

    // 等三笔都收口再退场:handler 引用的账在本例栈上,工作线程还有在途
    // 调用就拆栈会撞空(teardown 与在途 handler 赛跑的旧坑)。
    const auto action_id_of = [&](std::int64_t id) -> std::string {
        const auto reply = harness.FindResponse(id);
        REQUIRE(reply.has_value());
        return reply->at("result").value("actionId", std::string());
    };
    const std::string id80 = action_id_of(80);
    const std::string id81 = action_id_of(81);
    const std::string id82 = action_id_of(82);
    CHECK(harness.WaitForActionCompleted(id80, 10000).has_value());
    CHECK(harness.WaitForActionCompleted(id81, 10000).has_value());
    CHECK(harness.WaitForActionCompleted(id82, 10000).has_value());

    // owner 随参数转给了 sidecar(用户动作收尾递 epoch 的钩子);threadId 不进 sidecar。
    const auto requests = harness.sidecar.TakeRequests();
    REQUIRE(requests.size() >= 3);
    bool saw_user_owner = false;
    for (const nlohmann::json& request : requests) {
        CHECK_FALSE(request["params"].contains("threadId"));
        if (request["method"] == "action" && request["params"].value("owner", std::string()) == "user") {
            saw_user_owner = true;
        }
    }
    CHECK(saw_user_owner);
}

TEST_CASE("阶段B·owner 裁定:外壳报什么不算数,内核按连接说了算") {
    BrowserHarness harness;
    harness.sidecar.SetHandler("action", [](const nlohmann::json&) {
        return nlohmann::json{{"pageId", "p1"}};
    });
    std::string error_code;
    const std::string thread_id =
        harness.server->HandleThreadStart(nlohmann::json::object(), error_code)["threadId"];

    // 伪造案:agent 连接谎报 owner=user——内核明拒,受理都不给。
    harness.Feed(R"({"id":90,"method":"browser/action","params":{"kind":"click","ref":"e1","owner":"user"}})",
                 "agent");
    auto response = harness.FindResponse(90);
    REQUIRE(response.has_value());
    CHECK((*response)["error"]["code"] == app_server::kErrInvalidParams);
    CHECK((*response)["error"]["data"]["reason"] == "browser.owner_denied");

    // agent 连接不说 owner:缺省裁定为 agent 自己的手——写动作照样要
    // threadId(不许靠"不说"混进用户路)。
    harness.Feed(R"({"id":91,"method":"browser/action","params":{"kind":"click","ref":"e1"}})", "agent");
    response = harness.FindResponse(91);
    REQUIRE(response.has_value());
    CHECK((*response)["error"]["data"]["reason"] == "browser.thread_required");

    // agent 连接报 owner=agent:名实相符,照走审批路。
    harness.Feed(nlohmann::json{{"id", 92},
                                {"method", "browser/action"},
                                {"params", {{"kind", "click"}, {"ref", "e1"}, {"threadId", thread_id}}}}
                     .dump(),
                 "agent");
    response = harness.FindResponse(92);
    REQUIRE(response.has_value());
    CHECK((*response)["result"]["accepted"] == true);
    CHECK((*response)["result"]["owner"] == "agent");
    const auto permission = harness.WaitForEvent("permission/request");
    REQUIRE(permission.has_value());
    harness.AnswerApproval((*permission)["params"]["requestId"], "decline");
    const auto declined = harness.WaitForActionCompleted((*response)["result"]["actionId"]);
    REQUIRE(declined.has_value());
    CHECK((*declined)["params"]["error"]["code"] == "browser.permission_denied");

    // 暂停手闸只归用户连接:agent 连接按不动。
    harness.Feed(R"({"id":93,"method":"browser/pause","params":{}})", "agent");
    response = harness.FindResponse(93);
    REQUIRE(response.has_value());
    CHECK((*response)["error"]["data"]["reason"] == "browser.owner_denied");
    CHECK_FALSE(harness.server->browser_service().paused());

    // 用户连接报 owner=user:名实相符,用户路照走(不带 threadId)。
    harness.Feed(R"({"id":94,"method":"browser/action","params":{"kind":"click","ref":"e1","owner":"user"}})");
    response = harness.FindResponse(94);
    REQUIRE(response.has_value());
    CHECK((*response)["result"]["accepted"] == true);
    CHECK((*response)["result"]["owner"] == "user");

    // 用户动作就算硬塞 threadId 也进不了审批——用户不是 Agent,内核摘掉。
    harness.Feed(nlohmann::json{{"id", 95},
                                {"method", "browser/action"},
                                {"params", {{"kind", "click"}, {"ref", "e1"}, {"threadId", thread_id}}}}
                     .dump());
    response = harness.FindResponse(95);
    REQUIRE(response.has_value());
    CHECK((*response)["result"]["accepted"] == true);
    CHECK((*response)["result"]["owner"] == "user");
    const auto completed = harness.WaitForActionCompleted((*response)["result"]["actionId"]);
    REQUIRE(completed.has_value());
    CHECK((*completed)["params"]["ok"] == true);
    CHECK((*completed)["params"]["owner"] == "user");
}

// ---------------------------------------------------------------------------
// 事件转发与出站纪律
// ---------------------------------------------------------------------------

TEST_CASE("journal 批量事件:seq 盖章,entries/dropped/lastSeq 原样") {
    BrowserHarness harness;
    harness.sidecar.PushEvent({{"type", "journal/console"},
                               {"pageId", "p1"},
                               {"entries", nlohmann::json::array({
                                               {{"seq", 1}, {"level", "log"}, {"text", "hello"}, {"generation", 1}},
                                               {{"seq", 2}, {"level", "error"}, {"text", "boom"}, {"generation", 1}},
                                           })},
                               {"dropped", 0},
                               {"lastSeq", 2}});
    const auto event = harness.WaitForEvent("browser/console/event");
    REQUIRE(event.has_value());
    CHECK((*event)["params"]["pageId"] == "p1");
    CHECK((*event)["params"]["entries"].size() == 2);
    CHECK((*event)["params"]["entries"][1]["text"] == "boom");
    CHECK((*event)["params"]["lastSeq"] == 2);
    CHECK((*event)["params"].contains("seq")); // 连接层统一盖

    // navigation / user_epoch / crashed。
    harness.sidecar.PushEvent({{"type", "page/navigation"}, {"pageId", "p1"}, {"url", "http://x/2"}, {"generation", 2}});
    harness.sidecar.PushEvent({{"type", "user_epoch"}, {"pageId", "p1"}, {"userEpoch", 3}});
    harness.sidecar.PushEvent({{"type", "session/crashed"}, {"reason", "browser process disconnected"}});
    CHECK(harness.WaitForEvent("browser/navigation").has_value());
    CHECK(harness.WaitForEvent("browser/user_epoch").has_value());
    const auto crashed = harness.WaitForEvent("browser/crashed");
    REQUIRE(crashed.has_value());
    CHECK((*crashed)["params"]["reason"] == "browser process disconnected");
}

TEST_CASE("出站纪律:任何行里没有 base64 字样的截图正文") {
    BrowserHarness harness;
    // 全部出站行都不许出现 base64 洪水的特征字段。
    for (const std::string& line : harness.io.written) {
        CHECK(line.find("dataBase64") == std::string::npos);
        CHECK(line.find("iVBORw0KGgo") == std::string::npos);
    }
}

// ---------------------------------------------------------------------------
// 截图 artifact
// ---------------------------------------------------------------------------

TEST_CASE("截图:只发 artifact 引用,字节落盘,ready 事件带引用") {
    BrowserHarness harness;
    harness.sidecar.SetHandler("screenshot", [](const nlohmann::json&) {
        return nlohmann::json{{"pageId", "p1"},
                              {"url", "http://x/"},
                              {"generation", 3},
                              {"fullPage", false},
                              {"sha256", "will-be-checked"},
                              {"bytes", 70},
                              {"dataBase64", kTinyPngBase64}};
    });
    harness.Feed(R"({"id":60,"method":"browser/screenshot","params":{}})");
    const auto completed = harness.WaitForEvent("browser/action/completed");
    REQUIRE(completed.has_value());
    // sha 对不上会按 mismatch 拒——先钉这条路,再换对得上的。
    CHECK((*completed)["params"]["ok"] == false);
    CHECK((*completed)["params"]["error"]["code"] == "browser.artifact_mismatch");
}

TEST_CASE("截图:字节对得上时 artifact 落盘,协议无 base64") {
    BrowserHarness harness;
    // 真算一遍 sha(base64 解码后)。
    const std::string expected_sha = [&] {
        // doctest 环境没有直接的 base64 解码工具;用 hooks::Sha256Hex 对
        // 解码字节算——解码走 agent::DecodeBase64Strict。
        const auto decoded = agent::DecodeBase64Strict(kTinyPngBase64, 1024);
        REQUIRE(decoded.has_value());
        return hooks::Sha256Hex(*decoded);
    }();
    harness.sidecar.SetHandler("screenshot", [&](const nlohmann::json&) {
        return nlohmann::json{{"pageId", "p1"},
                              {"url", "http://x/"},
                              {"generation", 3},
                              {"fullPage", false},
                              {"sha256", expected_sha},
                              {"bytes", 70},
                              {"dataBase64", kTinyPngBase64}};
    });
    harness.Feed(R"({"id":61,"method":"browser/screenshot","params":{"fullPage":true}})");
    const auto completed = harness.WaitForEvent("browser/action/completed");
    REQUIRE(completed.has_value());
    CHECK((*completed)["params"]["ok"] == true);
    const nlohmann::json& image = (*completed)["params"]["result"]["image"];
    CHECK(image["artifact"]["stored"] == true);
    CHECK(image["width"] == 1);
    CHECK(image["height"] == 1);
    CHECK(image["sha256"] == expected_sha);
    const std::string path = image["artifact"]["path"];
    CHECK(path.find("art-") != std::string::npos);
    CHECK(std::filesystem::exists(platform::Utf8ToPath(path)));

    const auto ready = harness.WaitForEvent("browser/screenshot/ready");
    REQUIRE(ready.has_value());
    CHECK((*ready)["params"]["image"]["artifact"]["id"] == image["artifact"]["id"]);

    // 出站的每一条:没有 base64 正文。
    harness.PumpOutbox();
    for (const std::string& line : harness.io.written) {
        CHECK(line.find("dataBase64") == std::string::npos);
        CHECK(line.find(kTinyPngBase64) == std::string::npos);
    }
}

// ---------------------------------------------------------------------------
// outbox 的 journal 批合并
// ---------------------------------------------------------------------------

TEST_CASE("outbox:撞满时同页 journal 批并成一条,dropped 求和") {
    app_server::BoundedOutbox outbox(1);
    const auto make_batch = [](int first, int dropped) {
        nlohmann::json params;
        params["pageId"] = "p1";
        params["dropped"] = dropped;
        params["lastSeq"] = first + 1;
        nlohmann::json entries = nlohmann::json::array();
        entries.push_back({{"seq", first}, {"text", "a"}});
        entries.push_back({{"seq", first + 1}, {"text", "b"}});
        params["entries"] = entries;
        params["seq"] = first;
        return app_server::SerializeMessage(app_server::MakeEvent("browser/console/event", params));
    };
    CHECK(outbox.Push(make_batch(1, 0)));
    CHECK(outbox.Push(make_batch(3, 2))); // 撞满:合并,不丢
    CHECK(outbox.coalesced() == 1);
    CHECK(outbox.dropped() == 0);
    const std::string merged = *outbox.Pop();
    const nlohmann::json parsed = nlohmann::json::parse(merged);
    CHECK(parsed["params"]["entries"].size() == 4);
    CHECK(parsed["params"]["dropped"] == 2);
    CHECK(parsed["params"]["lastSeq"] == 4);
    // 不同页不合并:撞满走丢 + 溢出账。
    CHECK(outbox.Push(make_batch(1, 0)));
    nlohmann::json other = nlohmann::json::parse(make_batch(9, 0));
    other["params"]["pageId"] = "p2";
    CHECK_FALSE(outbox.Push(app_server::SerializeMessage(other)));
    CHECK(outbox.dropped() == 1);
}

// ---------------------------------------------------------------------------
// 真进程 sidecar:起 / 复用 / 收尸(缺 node 或缺脚本整段跳过)
// ---------------------------------------------------------------------------

namespace {

std::string FindSidecarScript() {
#ifdef LUBANCODE_SOURCE_DIR
    const std::filesystem::path candidate =
        std::filesystem::path(LUBANCODE_SOURCE_DIR) / "browser" / "sidecar.js";
    std::error_code ec;
    if (std::filesystem::exists(candidate, ec)) {
        return platform::PathToUtf8(candidate);
    }
#endif
    return std::string();
}

bool NodeAvailable() {
    const platform::ProcessResult result =
        platform::RunProcessWithStdin({"node", "--version"}, std::string(), 10000);
    return result.exit_code == 0;
}

}  // namespace

TEST_CASE("真进程 sidecar:起、复用、收尸(profile 锁释放)") {
    const std::string script = FindSidecarScript();
    // browser/node_modules 是本地目录联接,不进 git——CI checkout 没有
    // playwright 时 sidecar 起不来,15 秒超时红(Windows CI 实翻)。依赖
    // 缺席按"环境不可跑"跳过;本地装齐(node i)才真起进程。
    const bool deps_present = std::filesystem::exists(
        std::filesystem::path(script).parent_path() / "node_modules" / "playwright");
    if (script.empty() || !NodeAvailable() || !deps_present) {
        return; // 缺 node/脚本/依赖:跳过(ctest 裸跑即绿的环境)
    }
    // 临时 profile 目录(收尸的判据:锁文件没了)。
    const std::filesystem::path temp_root = std::filesystem::temp_directory_path() /
                                            ("lubancode-sidecar-test-" +
                                             std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    const std::filesystem::path user_data = temp_root / "profile";
    std::filesystem::create_directories(user_data);

    app_server::BrowserServiceOptions options;
    options.sidecar_command = "node";
    options.sidecar_args = {script, "--headless", "--profile", "persistent",
                            "--user-data-dir", platform::PathToUtf8(user_data)};
    options.query_timeout_ms = 15000;
    app_server::BrowserService service(std::move(options), [](std::string_view, const nlohmann::json&) {});

    // 起:session/status 直答(不 lazy 起浏览器)。
    const app_server::SidecarCallResult first = service.CallForTest("session/status", nlohmann::json::object(), 15000);
    REQUIRE(first.ok);
    CHECK(first.result.value("launched", true) == false);
    CHECK(service.sidecar_spawn_count() == 1);

    // 复用:第二笔调用还是同一只进程。
    const app_server::SidecarCallResult second = service.CallForTest("session/status", nlohmann::json::object(), 15000);
    REQUIRE(second.ok);
    CHECK(service.sidecar_spawn_count() == 1);

    // 收尸:Shutdown 后锁文件应被 sidecar 的退出钩子摘掉(轮询等退场)。
    service.Shutdown();
    const std::filesystem::path lock = user_data / "lock";
    bool lock_gone = false;
    for (int i = 0; i < 100 && !lock_gone; ++i) {
        std::error_code ec;
        lock_gone = !std::filesystem::exists(lock, ec);
        if (!lock_gone) {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
    }
    CHECK(lock_gone);
    std::error_code ec;
    std::filesystem::remove_all(temp_root, ec);
}

TEST_CASE("真进程 sidecar:崩了(进程被杀),下一笔调用重起") {
    const std::string script = FindSidecarScript();
    const bool deps_present = std::filesystem::exists(
        std::filesystem::path(script).parent_path() / "node_modules" / "playwright");
    if (script.empty() || !NodeAvailable() || !deps_present) {
        return;
    }
    app_server::BrowserServiceOptions options;
    options.sidecar_command = "node";
    options.sidecar_args = {script, "--headless", "--profile", "ephemeral"};
    options.query_timeout_ms = 15000;
    app_server::BrowserService service(std::move(options), [](std::string_view, const nlohmann::json&) {});
    const app_server::SidecarCallResult first = service.CallForTest("session/status", nlohmann::json::object(), 15000);
    REQUIRE(first.ok);
    CHECK(service.sidecar_spawn_count() == 1);

    // 崩:直接杀进程树(Shutdown 的杀法),再调一笔——须重起并直答。
    service.KillSidecarForTest();
    const app_server::SidecarCallResult after = service.CallForTest("session/status", nlohmann::json::object(), 20000);
    CHECK(after.ok);
    CHECK(service.sidecar_spawn_count() == 2);
    service.Shutdown();
}
