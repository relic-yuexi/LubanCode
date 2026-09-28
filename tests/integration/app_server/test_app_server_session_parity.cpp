#include <doctest/doctest.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <fstream>
#include <memory>
#include <mutex>
#include <optional>
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

struct EnvGuard {
    std::string name;
    std::optional<std::string> previous;
    EnvGuard(const char* key, const char* value) : name(key) {
        if (const auto* original = std::getenv(key)) previous = original;
#ifdef _WIN32
        _putenv_s(key, value);
#else
        setenv(key, value, 1);
#endif
    }
    ~EnvGuard() {
#ifdef _WIN32
        _putenv_s(name.c_str(), previous ? previous->c_str() : "");
#else
        if (previous) setenv(name.c_str(), previous->c_str(), 1);
        else unsetenv(name.c_str());
#endif
    }
};

nlohmann::json RawCall(app_server::Server& server, const std::string& method,
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
    return nlohmann::json::parse(outcome.outbound[0]);
}

nlohmann::json Call(app_server::Server& server, const std::string& method,
                    const nlohmann::json& params) {
    auto response = RawCall(server, method, params);
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
    EnvGuard format("LUBANCODE_TRAJECTORY_V3_NEW_SESSIONS", "1");
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
    history::CheckRequests(fixture, id);
}

TEST_CASE("AppServer resources: admitted MCP follows each thread cwd and optional failure stays explicit") {
    EnvGuard format("LUBANCODE_TRAJECTORY_V3_NEW_SESSIONS", "1");
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
    const auto first_pid = fixture.ChildPid(0);
    CHECK(platform::IsProcessAlive(first_pid));
    Call(*server, "thread/stop", {{"threadId", ids[0]}});
    CHECK(server->active_thread_count() == 2);
    fixture.CheckChildExited(first_pid);
    CHECK(platform::IsProcessAlive(fixture.ChildPid(1)));
    const auto input = fixture.Input(3);
    Turn(*server, ids[1], input.c_str());
    server->Shutdown();
    server.reset();
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
    EnvGuard format("LUBANCODE_TRAJECTORY_V3_NEW_SESSIONS", "1");
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

TEST_CASE("AppServer ownership: pending stop keeps the worker and ledger until a joined retry") {
    EnvGuard format("LUBANCODE_TRAJECTORY_V3_NEW_SESSIONS", "1");
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
    const auto accepted = server->AcceptTurnStart(id, "held-stop", {}, error, "held-stop");
    REQUIRE(error.empty());
    REQUIRE(accepted.contains("operationId"));
    {
        std::unique_lock lock(gate->mutex);
        REQUIRE(gate->cv.wait_for(lock, 5s, [&] { return gate->entered; }));
    }
    const auto main_path = server->ThreadMainPathForTest(id);
    REQUIRE_FALSE(main_path.empty());
    const auto has_session_end = [&] {
        std::ifstream input(platform::Utf8ToPath(main_path), std::ios::binary);
        REQUIRE(input.good());
        for (std::string line; std::getline(input, line);) {
            if (line.find("session.ended") != std::string::npos) return true;
        }
        return false;
    };
    server->HandleThreadStop(id, error);
    CHECK(error == "thread.stop_pending");
    CHECK(server->active_thread_count() == 1);
    CHECK_FALSE(has_session_end());
    const auto resumed = RawCall(*server, "thread/resume",
        {{"threadId", id}, {"startExecution", true}});
    REQUIRE(resumed.contains("error"));
    CHECK(resumed["error"]["data"].value("code", "") == "active_thread");
    {
        // A separate host has no in-memory record to reject. The still-open
        // durable writer must itself prevent a second owner of this session.
        app_server::ServerOptions competing_options;
        competing_options.cwd = fixture.Cwd();
        competing_options.workspaces_dir = history::Utf8(fixture.root / "data" / "workspaces");
        app_server::Server competing(std::move(competing_options),
            [gate] { return std::make_unique<GatedBackend>(gate); }, nullptr);
        competing.HandleThreadResumeExecution(id, fixture.Cwd(), error);
        INFO(error);
        CHECK_FALSE(error.empty());
        CHECK(competing.active_thread_count() == 0);
    }
    server->AcceptTurnStart(id, "cannot-reenter", {}, error, "cannot-reenter");
    CHECK(error == "thread.stopping");
    const auto operation = server->HandleOperationRead(id, "held-stop", {}, error);
    CHECK(error.empty());
    CHECK(operation.value("operationId", "") == accepted["operationId"].get<std::string>());
    CHECK(operation.value("status", "") != "not_found");
    CHECK(gate->calls.load() == 1);
    gate->Release();
    WaitCompleted(*server, id, accepted["turnId"].get<std::string>());
    const auto stopped = server->HandleThreadStop(id, error);
    CHECK(error.empty());
    CHECK(stopped.is_object());
    CHECK(server->active_thread_count() == 0);
    CHECK(has_session_end());
    server->Shutdown();
}

namespace {
struct ShutdownGate {
    std::mutex mutex;
    std::condition_variable cv;
    int entered = 0;
    int cancelled = 0;
    bool released = false;
    std::atomic<int> destroyed_backends{0};
    std::atomic<bool> server_destroyed{false};
    void Release() {
        std::lock_guard lock(mutex);
        released = true;
        cv.notify_all();
    }
};
struct ReleaseShutdownOnExit {
    std::shared_ptr<ShutdownGate> gate;
    ~ReleaseShutdownOnExit() { gate->Release(); }
};
class ShutdownBackend final : public api::Backend {
public:
    explicit ShutdownBackend(std::shared_ptr<ShutdownGate> gate) : gate_(std::move(gate)) {}
    ~ShutdownBackend() override { ++gate_->destroyed_backends; }
    std::expected<void, api::Error> send_stream(const api::Request&,
        const std::function<void(const api::StreamEvent&)>&,
        const std::atomic<bool>* cancelled) override {
        std::unique_lock lock(gate_->mutex);
        ++gate_->entered;
        gate_->cv.notify_all();
        const auto deadline = std::chrono::steady_clock::now() + 30s;
        while (!gate_->released && !(cancelled && cancelled->load()) &&
               std::chrono::steady_clock::now() < deadline) {
            gate_->cv.wait_for(lock, 5ms);
        }
        if (cancelled && cancelled->load()) {
            ++gate_->cancelled;
            gate_->cv.notify_all();
        }
        gate_->cv.wait_until(lock, deadline, [&] { return gate_->released; });
        return std::unexpected(api::Error{api::ErrorKind::Api, "fixture cancelled", 0});
    }
private:
    std::shared_ptr<ShutdownGate> gate_;
};
}  // namespace

TEST_CASE("AppServer ownership: destruction signals all workers before joining and retains its owner") {
    EnvGuard format("LUBANCODE_TRAJECTORY_V3_NEW_SESSIONS", "1");
    history::Fixture fixture;
    auto gate = std::make_shared<ShutdownGate>();
    app_server::ServerOptions options;
    options.cwd = fixture.Cwd();
    options.workspaces_dir = history::Utf8(fixture.root / "data" / "workspaces");
    options.interrupt_hard_deadline_ms = 20;
    auto server = std::make_unique<app_server::Server>(std::move(options),
        [gate] { return std::make_unique<ShutdownBackend>(gate); }, nullptr);
    Connect(*server);
    // Release before either the destroyer's join or Server destruction on every
    // assertion path, including a failure before the destroyer was started.
    std::jthread destroyer;
    ReleaseShutdownOnExit release{gate};
    for (int index = 0; index != 2; ++index) {
        const auto id = Start(*server);
        std::string error;
        server->AcceptTurnStart(id, "held-shutdown", {}, error, "held-shutdown");
        REQUIRE(error.empty());
    }
    {
        std::unique_lock lock(gate->mutex);
        REQUIRE(gate->cv.wait_for(lock, 5s, [&] { return gate->entered == 2; }));
    }
    destroyer = std::jthread([owned = std::move(server), gate]() mutable {
        owned.reset();
        gate->server_destroyed.store(true);
        gate->cv.notify_all();
    });
    {
        std::unique_lock lock(gate->mutex);
        REQUIRE(gate->cv.wait_for(lock, 5s, [&] { return gate->cancelled == 2; }));
    }
    CHECK_FALSE(gate->server_destroyed.load());
    CHECK(gate->destroyed_backends.load() == 0);
    gate->Release();
    {
        std::unique_lock lock(gate->mutex);
        REQUIRE(gate->cv.wait_for(lock, 5s, [&] { return gate->server_destroyed.load(); }));
    }
    destroyer.join();
    CHECK(gate->destroyed_backends.load() == 2);
}
