#include <doctest/doctest.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <thread>

#include "api/chat/client.hpp"
#include "app_server/connection.hpp"
#include "app_server/protocol.hpp"
#include "app_server/server.hpp"
#include "mcp_cwd_fixture.hpp"
#include "session_history_fixture.hpp"
#include "tools/ask_user.hpp"
#include "tools/registry.hpp"
#include "tools/tool.hpp"

namespace {
using namespace lubancode;
namespace history = test_support::session_history;
using namespace std::chrono_literals;

nlohmann::json Call(app_server::Server& server, const std::string& method,
                    const nlohmann::json& params) {
    const nlohmann::json envelope{{"id", 1}, {"method", method}, {"params", params}};
    app_server::EnvelopeError error;
    auto incoming = app_server::ParseIncoming(envelope.dump(), error);
    REQUIRE(incoming.has_value());
    app_server::DispatchContext context;
    context.emit_event = [&server](std::string_view name, const nlohmann::json& data, bool) {
        server.connection().EmitEvent(name, data);
    };
    const auto outcome = server.dispatcher().HandleRequest(incoming->request, context);
    REQUIRE(outcome.outbound.size() == 1);
    auto response = nlohmann::json::parse(outcome.outbound[0]);
    INFO(response.dump());
    REQUIRE(response.contains("result"));
    return response["result"];
}

void Connect(app_server::Server& server) {
    server.AttachForTest(std::make_unique<app_server::StdioConnection>(
        server.dispatcher_handle(), [](const std::string&) {}, [] { return std::string(); }, 4096));
    Call(server, "initialize", nlohmann::json::object());
    app_server::DispatchContext context;
    server.dispatcher().HandleNotification(
        {"initialized", nlohmann::json::object()}, context);
}

class HistoryTool final : public tools::Tool {
public:
    explicit HistoryTool(std::atomic<int>& calls) : calls_(calls) {}
    std::string name() const override { return history::kTool; }
    std::string description() const override { return "Return a session history marker."; }
    nlohmann::json input_schema() const override {
        return {{"type", "object"}, {"properties", {{"text", {{"type", "string"}}}}},
                {"required", nlohmann::json::array({"text"})}};
    }
    Result execute(const nlohmann::json& input) override {
        CHECK(input == nlohmann::json({{"text", history::kArgument}}));
        ++calls_;
        return Result::Text(history::kToolResult);
    }
private:
    std::atomic<int>& calls_;
};

class CountingHttpBackend final : public api::chat::ChatCompletionsBackend {
public:
    CountingHttpBackend(std::string url, std::atomic<int>& live)
        : ChatCompletionsBackend(std::move(url), "FAKE_HISTORY_KEY"), live_(live) { ++live_; }
    ~CountingHttpBackend() override { --live_; }
private:
    std::atomic<int>& live_;
};

std::unique_ptr<app_server::Server> Open(const history::Fixture& fixture, std::atomic<int>& calls,
                                        std::atomic<int>& live) {
    app_server::ServerOptions options;
    options.cwd = fixture.Cwd();
    options.workspaces_dir = history::Utf8(fixture.root / "data" / "workspaces");
    options.session_wire = "chat";
    options.session_model = "history-model";
    options.auto_confirm = true;
    options.max_steps_per_turn = 4;
    auto server = std::make_unique<app_server::Server>(std::move(options),
        [&fixture, &live] { return std::make_unique<CountingHttpBackend>(fixture.Url(), live); },
        [&calls] {
            auto registry = std::make_unique<tools::ToolRegistry>();
            registry->Register(std::make_unique<HistoryTool>(calls));
            // This tool is never called. Installing its per-turn handler alone
            // must not leave a tool -> ThreadRecord ownership cycle after stop.
            registry->Register(std::make_unique<tools::AskUserTool>(tools::AskUserHandler{}));
            return registry;
        });
    Connect(*server);
    return server;
}

std::string Start(app_server::Server& server) {
    auto result = Call(server, "thread/start", nlohmann::json::object());
    REQUIRE(result.contains("threadId"));
    return result["threadId"].get<std::string>();
}

nlohmann::json WaitCompleted(app_server::Server& server, const std::string& id,
                             const std::string& turn_id = {}) {
    const auto deadline = std::chrono::steady_clock::now() + 30s;
    while (std::chrono::steady_clock::now() < deadline) {
        if (auto line = server.connection().outbox().Pop()) {
            const auto event = nlohmann::json::parse(*line);
            if (event.value("method", "") == "turn/completed" &&
                event["params"].value("threadId", "") == id &&
                (turn_id.empty() || event["params"].value("turnId", "") == turn_id)) {
                // The event is currently queued before the worker clears busy.
                // Observe that public transition without joining the old thread;
                // thread/stop must still reap its completed, joinable handle.
                const auto quiet_deadline = std::chrono::steady_clock::now() + 5s;
                while (std::chrono::steady_clock::now() < quiet_deadline) {
                    std::string error;
                    server.HandleTurnInterrupt(id, event["params"]["turnId"].get<std::string>(), error);
                    if (!error.empty()) {
                        REQUIRE(error == "stale");
                        return event["params"];
                    }
                    std::this_thread::sleep_for(2ms);
                }
                FAIL("Completed turn did not release its busy state");
                return event["params"];
            }
        } else {
            std::this_thread::sleep_for(2ms);
        }
    }
    FAIL("Asynchronous turn did not complete");
    return nlohmann::json();
}

void Turn(app_server::Server& server, const std::string& id, const char* input) {
    const auto accepted = Call(server, "turn/start",
        {{"threadId", id}, {"text", input}, {"clientOperationId", input}});
    REQUIRE(accepted.contains("turnId"));
    const auto result = WaitCompleted(server, id, accepted["turnId"].get<std::string>());
    INFO(result.dump());
    CHECK(result.value("executionStatus", "") == "success");
    CHECK(result.value("resultEnvelopePersisted", true));
    // The completion event omits this field on success; the durable read result
    // carries it explicitly and must confirm the operation was really sealed.
    const auto operation = Call(server, "operation/read",
        {{"threadId", id}, {"clientOperationId", input}});
    CHECK(operation.value("executionStatus", "") == "success");
    CHECK(operation.value("resultEnvelopePersisted", false));
}
}  // namespace

TEST_CASE("AppServer history: HTTP requests preserve two turns and same-ID resume without shared-cwd leakage") {
    history::Fixture fixture;
    std::atomic<int> calls{0};
    std::atomic<int> live_backends{0};
    std::string id;
    {
        auto server = Open(fixture, calls, live_backends);
        id = Start(*server);
        Turn(*server, id, history::kFirst);
        Turn(*server, id, history::kSecond);
        const auto other = Start(*server);
        CHECK(other != id);
        // Exercise the real asynchronous worker's completed-but-joinable state.
        const int before_stop = live_backends.load();
        REQUIRE(before_stop >= 1);
        Call(*server, "thread/stop", {{"threadId", id}});
        CHECK(live_backends.load() == before_stop - 1);
        CHECK(server->active_thread_count() == 1);
        Turn(*server, other, history::kOther);
        server->Shutdown();
    }
    CHECK(live_backends.load() == 0);
    {
        auto server = Open(fixture, calls, live_backends);
        auto resumed = Call(*server, "thread/resume", {{"threadId", id}, {"startExecution", true}});
        CHECK(resumed.value("resumedThreadId", "") == id);
        Turn(*server, id, history::kThird);
        server->Shutdown();
    }
    CHECK(live_backends.load() == 0);
    CHECK(calls.load() == 1);
    history::CheckRequests(fixture);
}

TEST_CASE("AppServer resources: admitted MCP follows each thread cwd and optional failure stays explicit") {
    test_support::McpCwdFixture fixture;
    config::Config config;
    config.model = "cwd-model";
    config.mcp_servers["location"] = {fixture.python, {fixture.Script()}, {}};
    config.mcp_servers["optional"] = {"definitely-missing-cwd-fixture-command", {}, {}};
    app_server::HarnessProfile harness;
    harness.name = "session-cwd";
    harness.features_enabled.insert("mcp");
    harness.mcp_servers = {"location", "optional"};
    harness.tools.mode = app_server::HarnessToolPolicy::Mode::Only;
    harness.tools.allow = {"mcp:location:where"};
    harness.exposure = "direct";
    app_server::ServerOptions options;
    options.cwd = history::Utf8(fixture.root / "project-a");
    options.workspaces_dir = history::Utf8(fixture.root / "data" / "workspaces");
    options.session_wire = "chat";
    options.session_model = "cwd-model";
    options.auto_confirm = true;
    options.max_steps_per_turn = 4;
    options.assembly_factory = [&](const std::string& cwd) {
        app_server::SessionAssemblyRequest request;
        request.config = &config;
        request.harness = &harness;
        request.cwd_utf8 = cwd;
        request.system_prompt = "Inspect the current session's cwd.";
        request.max_steps_per_turn = 4;
        request.backend_factory = [&] {
            return std::make_unique<api::chat::ChatCompletionsBackend>(fixture.Url(), "FAKE_CWD_KEY");
        };
        return app_server::AssembleSession(std::move(request));
    };
    auto server = std::make_unique<app_server::Server>(std::move(options), nullptr, nullptr);
    Connect(*server);
    std::vector<std::string> ids;
    for (int index = 0; index != 3; ++index) {
        const auto cwd = history::Utf8(fixture.root / (index == 2 ? "project-b" : "project-a"));
        auto started = Call(*server, "thread/start", {{"cwd", cwd}});
        REQUIRE(started.contains("threadId"));
        REQUIRE(started.contains("degradedComponents"));
        REQUIRE(started["degradedComponents"].size() == 1);
        CHECK(started["degradedComponents"][0].get<std::string>().find("optional") != std::string::npos);
        ids.push_back(started["threadId"].get<std::string>());
        CHECK(std::filesystem::equivalent(fixture.host_cwd, std::filesystem::current_path()));
    }
    for (int round = 0; round != 3; ++round) {
        const auto input = fixture.Input(round);
        Turn(*server, ids[round], input.c_str());
    }
    Call(*server, "thread/stop", {{"threadId", ids[0]}});
    CHECK(server->active_thread_count() == 2);
    const auto input = fixture.Input(3);
    Turn(*server, ids[1], input.c_str());
    server->Shutdown();
    fixture.Check();
}

namespace {
struct BackendGate {
    std::mutex mutex;
    std::condition_variable cv;
    bool entered = false;
    bool released = false;
    std::atomic<int> calls{0};
    void Release() {
        std::lock_guard lock(mutex);
        released = true;
        cv.notify_all();
    }
};
struct ReleaseBackendOnExit {
    std::shared_ptr<BackendGate> gate;
    app_server::Server* server = nullptr;
    std::string thread_id;
    ~ReleaseBackendOnExit() {
        gate->Release();
        if (server == nullptr || thread_id.empty()) return;
        // Keep the Server alive through callback unwinding even if a REQUIRE
        // above fails while the synchronous caller has already detached.
        const auto deadline = std::chrono::steady_clock::now() + 5s;
        while (std::chrono::steady_clock::now() < deadline) {
            std::string error;
            server->HandleTurnInterrupt(thread_id, {}, error);
            if (!error.empty()) return;
            std::this_thread::sleep_for(2ms);
        }
    }
};
class GatedBackend final : public api::Backend {
public:
    explicit GatedBackend(std::shared_ptr<BackendGate> gate) : gate_(std::move(gate)) {}
    std::expected<void, api::Error> send_stream(const api::Request&,
        const std::function<void(const api::StreamEvent&)>& emit,
        const std::atomic<bool>*) override {
        if (++gate_->calls == 1) {
            std::unique_lock lock(gate_->mutex);
            gate_->entered = true;
            gate_->cv.notify_all();
            if (!gate_->cv.wait_for(lock, 30s, [&] { return gate_->released; })) {
                return std::unexpected(api::Error{api::ErrorKind::Api, "fixture gate timed out", 0});
            }
        }
        emit(api::MessageStart{"gate-message", "gate-model"});
        emit(api::TextDelta{"unblocked"});
        emit(api::ContentBlockDone{0});
        emit(api::MessageDone{"end_turn", api::Usage{10, 5, 0, 0, 0}});
        return {};
    }
private:
    std::shared_ptr<BackendGate> gate_;
};
}  // namespace

TEST_CASE("AppServer ownership: a hard deadline cannot admit another turn over a live worker") {
    history::Fixture fixture;
    auto gate = std::make_shared<BackendGate>();
    app_server::ServerOptions options;
    options.cwd = fixture.Cwd();
    options.workspaces_dir = history::Utf8(fixture.root / "data" / "workspaces");
    options.interrupt_hard_deadline_ms = 20;
    auto server = std::make_unique<app_server::Server>(std::move(options),
        [gate] { return std::make_unique<GatedBackend>(gate); }, nullptr);
    ReleaseBackendOnExit release{gate, server.get(), {}};
    Connect(*server);
    const auto id = Start(*server);
    release.thread_id = id;
    std::string error;
    server->HandleTurnStart(id, "held", {}, error, "held");
    REQUIRE(error == "hard_deadline");
    {
        std::unique_lock lock(gate->mutex);
        REQUIRE(gate->cv.wait_for(lock, 5s, [&] { return gate->entered; }));
    }
    server->AcceptTurnStart(id, "must not overlap", {}, error, "must-not-overlap");
    CHECK(error == "already_running");
    CHECK(gate->calls.load() == 1);
    gate->Release();
    WaitCompleted(*server, id);
    Turn(*server, id, "after-worker-exit");
    CHECK(gate->calls.load() == 2);
    server->Shutdown();
}
