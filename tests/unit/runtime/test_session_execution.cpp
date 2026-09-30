#include <doctest/doctest.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "agent/agent.hpp"
#include "mcp/mcp_tool.hpp"
#include "runtime/assembly/session_resources.hpp"
#include "runtime/async_tool_runtime.hpp"
#include "runtime/scoped_turn_bindings.hpp"
#include "runtime/session_execution.hpp"
#include "runtime/session_service.hpp"
#include "runtime/tool_trace_hub.hpp"
#include "scripted_mcp_endpoint.hpp"
#include "tools/path_utils.hpp"
#include "trajectory/v3/reader.hpp"
#include "workspace/identity.hpp"

namespace {
using namespace lubancode;
using namespace std::chrono_literals;
namespace fs = std::filesystem;
namespace assembly = runtime::assembly;
namespace v3 = trajectory::v3;

struct FormatGuard {
    std::optional<std::string> before;
    FormatGuard() {
        if (const char* value = std::getenv("LUBANCODE_TRAJECTORY_V3_NEW_SESSIONS")) before = value;
#ifdef _WIN32
        _putenv_s("LUBANCODE_TRAJECTORY_V3_NEW_SESSIONS", "1");
#else
        setenv("LUBANCODE_TRAJECTORY_V3_NEW_SESSIONS", "1", 1);
#endif
    }
    ~FormatGuard() {
#ifdef _WIN32
        _putenv_s("LUBANCODE_TRAJECTORY_V3_NEW_SESSIONS", before ? before->c_str() : "");
#else
        if (before) setenv("LUBANCODE_TRAJECTORY_V3_NEW_SESSIONS", before->c_str(), 1);
        else unsetenv("LUBANCODE_TRAJECTORY_V3_NEW_SESSIONS");
#endif
    }
};
struct Fixture {
    FormatGuard format;
    fs::path root;
    Fixture() {
        static std::atomic<unsigned> serial{0};
        root = fs::temp_directory_path() / ("session-execution-" +
            std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + "-" + std::to_string(++serial));
        fs::create_directories(root / "project");
    }
    ~Fixture() { std::error_code error; fs::remove_all(root, error); }
    runtime::SessionLaunchRequest Launch() const {
        runtime::SessionLaunchRequest request;
        request.cwd_utf8 = tools::PathToUtf8(root / "project");
        request.workspace_identity = workspace::MakeFallbackIdentity(root / "project");
        request.workspaces_root = root / "workspaces";
        request.lubancode_version = "session-execution-test";
        request.wire_name = "chat";
        request.v3_system_content = "Execution fixture system.";
        return request;
    }
};
struct CallbackGate {
    std::mutex mutex;
    std::condition_variable cv;
    bool entered = false;
    bool cancelled = false;
    bool released = false;
    bool exited = false;
    bool timed_out = false;
    void Release() {
        std::lock_guard lock(mutex);
        released = true;
        cv.notify_all();
    }
};
struct ReleaseGateOnExit {
    std::shared_ptr<CallbackGate> gate;
    ~ReleaseGateOnExit() { gate->Release(); }
};
struct ResourceState {
    std::mutex mutex;
    std::vector<std::string> destruction;
    std::vector<api::Request> requests;
    std::weak_ptr<void> agent_token;
    std::atomic<bool> backend_alive{false};
    std::atomic<bool> tool_alive{false};
    std::atomic<bool> tool_saw_agent_destroyed{false};
    std::atomic<bool> tool_saw_client_alive{false};
    std::atomic<bool> backend_saw_borrowers_destroyed{false};
    std::atomic<bool> callback_used_dependencies_after_cancel{false};
    std::atomic<unsigned> primary_calls{0};
    std::atomic<unsigned> authorization_calls{0};
    std::atomic<unsigned> clock_calls{0};
    std::atomic<unsigned> worker_shutdown_attempts{0};
    std::atomic<bool> worker_shutdown_rejected{false};
    std::atomic<bool> hooks_destroyed_with_dependencies{false};
    tools::ToolJobCoordinator* coordinator = nullptr;
    std::thread::id callback_thread;
    bool issue_background = false;
    std::shared_ptr<CallbackGate> gate;
    void Destroyed(const std::string& name) { std::lock_guard lock(mutex); destruction.push_back(name); }
    std::vector<std::string> Destruction() { std::lock_guard lock(mutex); return destruction; }
    std::vector<api::Request> Requests() { std::lock_guard lock(mutex); return requests; }
};
struct AgentToken {
    std::shared_ptr<ResourceState> state;
    explicit AgentToken(std::shared_ptr<ResourceState> value) : state(std::move(value)) {}
    ~AgentToken() { state->Destroyed("agent"); }
};
class ProbeBackend final : public api::Backend {
public:
    explicit ProbeBackend(std::shared_ptr<ResourceState> state) : state_(std::move(state)) {
        state_->backend_alive.store(true);
    }
    ~ProbeBackend() override {
        state_->backend_saw_borrowers_destroyed.store(state_->agent_token.expired() && !state_->tool_alive.load());
        state_->backend_alive.store(false);
        state_->Destroyed("backend");
    }
    std::expected<void, api::Error> send_stream(const api::Request& request,
        const std::function<void(const api::StreamEvent&)>& emit, const std::atomic<bool>*) override {
        {
            std::lock_guard lock(state_->mutex);
            state_->requests.push_back(request);
        }
        emit(api::MessageStart{"execution-message", request.model});
        const bool borrowed_probe = request.model == "callback-borrow";
        if (!borrowed_probe && state_->issue_background && state_->primary_calls.fetch_add(1) == 0) {
            emit(api::ToolUseStart{0, "execution-background-call", "execution_probe"});
            emit(api::ToolUseInputDelta{0, "{}"});
            emit(api::ContentBlockDone{0});
            emit(api::MessageDone{"tool_use", api::Usage{10, 5, 0, 0, 0}});
        } else {
            emit(api::TextDelta{"execution-answer"});
            emit(api::ContentBlockDone{0});
            emit(api::MessageDone{"end_turn", api::Usage{10, 5, 0, 0, 0}});
        }
        return {};
    }
private:
    std::shared_ptr<ResourceState> state_;
};
class ProbeTool final : public tools::Tool {
public:
    ProbeTool(std::shared_ptr<ResourceState> state, api::Backend& backend,
              test_support::ScriptedMcpEndpoint* endpoint, mcp::Client* client, std::optional<mcp::ToolInfo> info)
        : state_(std::move(state)), backend_(backend), endpoint_(endpoint) {
        if (client && info) mcp_ = std::make_unique<mcp::McpTool>(*client, "fixture", std::move(*info));
        state_->tool_alive.store(true);
    }
    ~ProbeTool() override {
        state_->tool_saw_agent_destroyed.store(state_->agent_token.expired());
        state_->tool_saw_client_alive.store(endpoint_ == nullptr || endpoint_->IsAlive());
        state_->tool_alive.store(false);
        state_->Destroyed("tool");
    }
    std::string name() const override { return "execution_probe"; }
    std::string description() const override { return "Use the dependencies owned by this session."; }
    nlohmann::json input_schema() const override { return {{"type", "object"}}; }
    tools::EffectClass effect_class() const override { return tools::EffectClass::ReadOnlyLocal; }
    Result execute(const nlohmann::json& input) override { return execute(input, {}); }
    Result execute(const nlohmann::json& input, const tools::ToolExecutionContext& context) override {
        if (state_->gate) {
            auto& gate = *state_->gate;
            std::unique_lock lock(gate.mutex);
            gate.entered = true;
            gate.cv.notify_all();
            const auto deadline = std::chrono::steady_clock::now() + 20s;
            while (!gate.released && std::chrono::steady_clock::now() < deadline) {
                if (context.cancel && context.cancel->load()) { gate.cancelled = true; gate.cv.notify_all(); }
                gate.cv.wait_for(lock, 2ms);
            }
            gate.timed_out = !gate.released;
        }
        // These calls happen after the close request, while the callback still
        // holds plain references. ASan catches premature owner destruction.
        api::Request probe;
        probe.model = "callback-borrow";
        const auto backend_result = backend_.send_stream(probe, [](const auto&) {}, nullptr);
        const auto mcp_result = mcp_ ? mcp_->execute(input) : Result::Text("fixture ok");
        const bool cancel = context.cancel && context.cancel->load();
        state_->callback_used_dependencies_after_cancel.store(cancel && backend_result.has_value() && !mcp_result.is_error);
        if (state_->gate) {
            std::lock_guard lock(state_->gate->mutex);
            state_->gate->exited = true;
            state_->gate->cv.notify_all();
        }
        return cancel ? Result::Error("execution callback acknowledged cancellation") : Result::Text("execution tool answer");
    }
private:
    std::shared_ptr<ResourceState> state_;
    api::Backend& backend_;
    test_support::ScriptedMcpEndpoint* endpoint_;
    std::unique_ptr<mcp::McpTool> mcp_;
};
struct BorrowedHookCapture {
    std::shared_ptr<ResourceState> state;
    tools::ToolRegistry& registry;
    api::Backend& backend;
    BorrowedHookCapture(std::shared_ptr<ResourceState> value, tools::ToolRegistry& tools, api::Backend& model)
        : state(std::move(value)), registry(tools), backend(model) {}
    ~BorrowedHookCapture() {
        const bool alive = state->tool_alive.load() && state->backend_alive.load();
        if (!alive) return; // record false without deliberately using a dead pointer
        api::Request request;
        request.model = "callback-borrow";
        state->hooks_destroyed_with_dependencies.store(registry.Find("execution_probe") != nullptr &&
            backend.send_stream(request, [](const auto&) {}, nullptr).has_value());
    }
};
struct ReentrantJobExecutor {
    tools::ToolRegistry* registry;
    std::shared_ptr<ResourceState> state;
    std::shared_ptr<BorrowedHookCapture> capture;
    ~ReentrantJobExecutor() {
        // std::thread frees its captured std::function after the worker body and
        // its local TLS scope have exited. This tail still belongs to the worker.
        bool worker_tail = false;
        { std::lock_guard lock(state->mutex); worker_tail = std::this_thread::get_id() == state->callback_thread; }
        if (state->coordinator && worker_tail) {
            ++state->worker_shutdown_attempts;
            state->worker_shutdown_rejected.store(!state->coordinator->Shutdown());
        }
    }
    tools::Tool::Result operator()(const tools::JobExecutionContext& context) const {
        { std::lock_guard lock(state->mutex); state->callback_thread = std::this_thread::get_id(); }
        auto* tool = registry->Find("execution_probe");
        if (!tool) return tools::Tool::Result::Error("background registry disappeared");
        tools::ToolExecutionContext execution;
        execution.cancel = context.cancel;
        return tool->execute(context.input, execution);
    }
};
std::unique_ptr<assembly::SessionResources> Resources(const Fixture& fixture,
    const std::shared_ptr<ResourceState>& state, test_support::ScriptedMcpEndpoint* endpoint = nullptr) {
    assembly::SessionResourcesRequest request;
    api::Backend* backend = nullptr;
    request.backend_factory = [&] {
        auto owned = std::make_unique<ProbeBackend>(state);
        backend = owned.get();
        return owned;
    };
    if (endpoint) {
        request.mcp_servers.push_back({{"fixture", "explicit-fixture", {}, {}, platform::EnvMode::Replace,
            tools::PathToUtf8(fixture.root / "project")}, {1000, 1000, nullptr}, true});
        request.mcp_launcher = endpoint->Launcher();
    }
    request.registry_factory = [&](std::span<const assembly::McpServerRuntime> servers) -> assembly::SessionRegistryResult {
        auto registry = std::make_unique<tools::ToolRegistry>();
        mcp::Client* client = nullptr;
        std::optional<mcp::ToolInfo> info;
        if (!servers.empty()) {
            client = servers[0].client.get();
            for (const auto& tool : servers[0].tools) if (tool.name == "echo") info = tool;
            REQUIRE(info.has_value());
        }
        registry->Register(std::make_unique<ProbeTool>(state, *backend, endpoint, client, std::move(info)));
        return registry;
    };
    auto built = assembly::BuildSessionResources(std::move(request));
    REQUIRE(built.has_value());
    return std::move(*built);
}
agent::AgentProfile Profile(const std::string& label) {
    agent::AgentProfile profile;
    profile.provider = label + "-provider";
    profile.request.model = label + "-model";
    profile.system_prompt = label + "-system";
    profile.runtime.max_steps_per_turn = 4;
    profile.runtime.max_output_tokens = 8192;
    return profile;
}
agent::AgentProfile LifetimeProfile(const std::shared_ptr<ResourceState>& state) {
    auto profile = Profile("owned");
    auto token = std::make_shared<AgentToken>(state);
    state->agent_token = token;
    profile.tool_filter = [token](const tools::Tool&) { return true; };
    token.reset();
    return profile;
}
api::Message User(const std::string& text) {
    return {api::Role::User, {api::TextBlock{text}}};
}
bool HasText(const api::Request& request, const std::string& text) {
    for (const auto& message : request.messages)
        for (const auto& block : message.content)
            if (const auto* value = std::get_if<api::TextBlock>(&block); value && value->text == text) return true;
    return false;
}
bool HasSessionEnd(const v3::V3Ledger& ledger) {
    for (const auto& event : ledger.events) if (event.kind == v3::EventKindV3::SessionEnded) return true;
    return false;
}
tools::JobStartRequest DeclaredJob(v3::V3Writer& writer, const std::string& name) {
    v3::MessageDraft draft;
    draft.turn_id = "turn-000001";
    draft.step_id = "step-000001";
    draft.request_id = "request-000001";
    draft.origin = v3::MessageOrigin::SessionRuntime;
    draft.provider = "fixture";
    draft.wire = "chat";
    draft.model = "owned-model";
    draft.response_model = "owned-model";
    draft.usage = nlohmann::json{{"inputTokens", 10}, {"outputTokens", 5}};
    draft.message = nlohmann::json{{"role", "assistant"}, {"content", "run " + name},
        {"tool_calls", nlohmann::json::array({nlohmann::json{{"id", "call-" + name}, {"type", "function"},
            {"function", nlohmann::json{{"name", name}, {"arguments", "{}"}}}}})}};
    const auto receipt = writer.AppendMessage(std::move(draft), v3::Durability::PowerLoss);
    REQUIRE(receipt.status == v3::WriteReceipt::Status::Committed);
    REQUIRE(writer.AdmitMessages({receipt.id}).status == v3::WriteReceipt::Status::Committed);
    tools::JobStartRequest request;
    request.tool_name = name;
    request.tool_input = nlohmann::json::object();
    request.turn_id = "turn-000001";
    request.step_id = "step-000001";
    request.assistant_message_ref = receipt.id;
    request.policy.allow_background = true;
    request.policy.resume_policy = "requeue_when_registered";
    return request;
}
} // namespace

TEST_CASE("session execution: fresh and explicit empty restore preserve distinct context semantics") {
    Fixture fixture;
    for (const std::string mode : {"fresh", "empty-restore", "history-restore"}) {
        CAPTURE(mode);
        auto state = std::make_shared<ResourceState>();
        runtime::SessionService service(fixture.Launch());
        REQUIRE(service.runtime() != nullptr);
        std::optional<std::vector<api::Message>> history;
        if (mode != "fresh") history.emplace();
        if (mode == "history-restore") {
            history->push_back(User("restored user marker"));
            history->push_back({api::Role::Assistant, {api::TextBlock{"restored assistant marker"}}});
        }
        service.InitializeExecution(Resources(fixture, state), Profile(mode), std::move(history));
        REQUIRE(service.execution() != nullptr);
        auto& agent = service.execution()->agent();
        CHECK(agent.cache_epoch() == (mode == "fresh" ? 1 : 2));
        CHECK(agent.history().size() == (mode == "history-restore" ? 2 : 0));
        REQUIRE(agent.Run("first new marker", {}).has_value());
        REQUIRE(agent.Run("second new marker", {}).has_value());
        const auto requests = state->Requests();
        REQUIRE(requests.size() == 2);
        for (const auto& request : requests) {
            CHECK(request.model == mode + "-model");
            CHECK(request.system.find(mode + "-system") != std::string::npos);
            CHECK(HasText(request, "restored user marker") == (mode == "history-restore"));
            CHECK(HasText(request, "restored assistant marker") == (mode == "history-restore"));
        }
        CHECK(HasText(requests[1], "first new marker"));
        CHECK(HasText(requests[1], "execution-answer"));
        CHECK(service.Close("test_complete").error_code.empty());
    }
}

TEST_CASE("session execution: Agent is destroyed before tools MCP clients and backend") {
    Fixture fixture;
    auto state = std::make_shared<ResourceState>();
    test_support::ScriptedMcpEndpoint endpoint;
    endpoint.on_method = [&](const std::string& method) {
        if (method == "tools/call") CHECK(state->backend_alive.load());
    };
    auto service = std::make_unique<runtime::SessionService>(fixture.Launch());
    service->InitializeExecution(Resources(fixture, state, &endpoint), LifetimeProfile(state), std::nullopt);
    REQUIRE(service->execution() != nullptr);
    REQUIRE(service->execution()->agent().Run("exercise owned agent", {}).has_value());
    CHECK_FALSE(state->agent_token.expired());
    CHECK(state->backend_alive.load());
    CHECK(endpoint.IsAlive());
    REQUIRE(service->Close("test_complete").error_code.empty());
    service.reset();
    CHECK(state->Destruction() == std::vector<std::string>{"agent", "tool", "backend"});
    CHECK(state->tool_saw_agent_destroyed.load());
    CHECK(state->tool_saw_client_alive.load());
    CHECK(state->backend_saw_borrowers_destroyed.load());
    CHECK(endpoint.shutdown_count == 1);
}

TEST_CASE("session execution: rejected initialization preserves the installed live execution") {
    Fixture fixture;
    runtime::SessionService service(fixture.Launch());
    CHECK_THROWS_AS(service.InitializeExecution(nullptr, Profile("invalid"), std::nullopt), std::invalid_argument);
    CHECK(service.execution() == nullptr);
    auto old = std::make_shared<ResourceState>();
    service.InitializeExecution(Resources(fixture, old), Profile("original"), std::nullopt);
    auto* installed = service.execution();
    REQUIRE(installed != nullptr);
    REQUIRE(installed->agent().Run("before rejected replacement", {}).has_value());
    auto rejected = std::make_shared<ResourceState>();
    CHECK_THROWS_AS(service.InitializeExecution(Resources(fixture, rejected), Profile("replacement"),
        std::vector<api::Message>{User("replacement history must not leak")}), std::logic_error);
    CHECK(service.execution() == installed);
    CHECK_FALSE(rejected->backend_alive.load());
    CHECK(rejected->Destruction() == std::vector<std::string>{"tool", "backend"});
    REQUIRE(installed->agent().Run("after rejected replacement", {}).has_value());
    const auto requests = old->Requests();
    REQUIRE(requests.size() == 2);
    CHECK(requests[1].model == "original-model");
    CHECK(HasText(requests[1], "before rejected replacement"));
    CHECK_FALSE(HasText(requests[1], "replacement history must not leak"));
    CHECK(service.Close("test_complete").error_code.empty());
}

namespace {
struct ThrowingProfileCopy {
    std::shared_ptr<std::atomic<bool>> armed;
    explicit ThrowingProfileCopy(std::shared_ptr<std::atomic<bool>> flag) : armed(std::move(flag)) {}
    ThrowingProfileCopy(const ThrowingProfileCopy& other) : armed(other.armed) {
        if (armed->load()) throw std::runtime_error("profile copy rejected");
    }
    bool operator()(const tools::Tool&) const { return true; }
};
} // namespace

TEST_CASE("session execution: throwing profile preparation publishes no partial execution") {
    Fixture fixture;
    runtime::SessionService service(fixture.Launch());
    auto state = std::make_shared<ResourceState>();
    auto resources = Resources(fixture, state);
    auto profile = Profile("copy-failure");
    auto armed = std::make_shared<std::atomic<bool>>(false);
    profile.tool_filter = ThrowingProfileCopy(armed);
    armed->store(true);
    CHECK_THROWS_WITH(service.InitializeExecution(std::move(resources), profile, std::nullopt), "profile copy rejected");
    CHECK(service.execution() == nullptr);
    // Argument evaluation may either move resources before copying the profile
    // or leave them here. Both paths retain an owner and unwind cleanly.
    resources.reset();
    CHECK_FALSE(state->backend_alive.load());
    CHECK(state->Destruction() == std::vector<std::string>{"tool", "backend"});
    auto recovered = std::make_shared<ResourceState>();
    service.InitializeExecution(Resources(fixture, recovered), Profile("retry"), std::nullopt);
    REQUIRE(service.execution() != nullptr);
    REQUIRE(service.execution()->agent().Run("retry after failed initialization", {}).has_value());
    CHECK(recovered->Requests()[0].model == "retry-model");
    CHECK(service.Close("test_complete").error_code.empty());
}

namespace {
struct RollbackQueriesService {
    runtime::SessionService& service;
    std::atomic<bool>& observed;
    RollbackQueriesService(runtime::SessionService& owner, std::atomic<bool>& result)
        : service(owner), observed(result) {}
    ~RollbackQueriesService() { observed.store(service.pending_input_count() == 0); }
};
} // namespace

TEST_CASE("session execution: constructor rollback releases profile captures outside the service lock") {
    Fixture fixture;
    std::atomic<bool> rollback_queried{false};
    runtime::SessionService service(fixture.Launch());
    auto profile = Profile("constructor-failure");
    auto capture = std::make_shared<RollbackQueriesService>(service, rollback_queried);
    profile.tool_filter = [capture](const tools::Tool&) { return true; };
    capture.reset();
    CHECK_THROWS_AS(service.InitializeExecution(nullptr, std::move(profile), std::nullopt), std::invalid_argument);
    CHECK(rollback_queried.load());
    CHECK(service.execution() == nullptr);
    auto state = std::make_shared<ResourceState>();
    service.InitializeExecution(Resources(fixture, state), Profile("after-rollback"), std::nullopt);
    REQUIRE(service.execution() != nullptr);
    REQUIRE(service.execution()->agent().Run("session survives failed constructor", {}).has_value());
    CHECK(state->Requests()[0].model == "after-rollback-model");
    CHECK(service.Close("test_complete").error_code.empty());
}

namespace {
void CheckLiveBackgroundShutdown(bool injected_terminal) {
    Fixture fixture;
    auto state = std::make_shared<ResourceState>();
    state->issue_background = true;
    state->gate = std::make_shared<CallbackGate>();
    test_support::ScriptedMcpEndpoint endpoint;
    auto service = std::make_unique<runtime::SessionService>(fixture.Launch());
    service->InitializeExecution(Resources(fixture, state, &endpoint), LifetimeProfile(state), std::nullopt);
    auto* ledger = service->trajectory();
    REQUIRE(ledger != nullptr);
    REQUIRE(ledger->v3_main_writer() != nullptr);
    const auto main_path = ledger->v3_main_writer()->path();
    auto& registry = service->execution()->resources().registry();
    auto capture = std::make_shared<BorrowedHookCapture>(state, registry, service->execution()->resources().backend());
    const std::weak_ptr<BorrowedHookCapture> captured_dependencies = capture;
    runtime::AsyncToolRuntime::Hooks hooks;
    hooks.writer = ledger->v3_main_writer();
    hooks.writer_mutex = ledger->v3_tool_results_mutex();
    hooks.auth = [state, capture](const auto&, const auto&) {
        ++state->authorization_calls;
        return tools::JobAuthDecision{true, false, {}};
    };
    hooks.executor = ReentrantJobExecutor{&registry, state, capture};
    runtime::AsyncToolRuntimeOptions options;
    options.provider = "fixture";
    options.wire = "chat";
    options.model = "owned-model";
    options.tools["execution_probe"].execution.allow_background = true;
    options.coordinator.clock_ms = [state, capture] {
        ++state->clock_calls;
        return std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
    };
    auto asynchronous = runtime::AsyncToolRuntime::Create(std::move(hooks), std::move(options));
    REQUIRE(asynchronous != nullptr);
    capture.reset();
    // A tool family or host may retain this shared handle after runtime close.
    // Shutdown must still drain workers before borrowed owners are destroyed.
    auto coordinator = asynchronous->coordinator();
    state->coordinator = coordinator.get();
    service->runtime()->AttachAsyncToolRuntime(std::move(asynchronous));
    const auto receipt = service->SubmitInput({"background-operation", "start real background callback", {}});
    REQUIRE(receipt.accepted);
    const auto pending = service->PopPendingInput();
    REQUIRE(pending.status == runtime::SessionService::PendingPop::Status::Ok);
    const auto turn_id = ledger->v3_main_writer()->NewTurnId();
    {
        auto bridge = ledger->NewTurnBridge({"fixture", "chat", "execution-test", {}});
        REQUIRE(bridge != nullptr);
        runtime::ToolTraceHub hub(service->runtime()->ids());
        agent::TurnWiring wiring;
        wiring.turn_id = turn_id;
        runtime::ScopedTurnBindings bindings(service->execution()->agent());
        bindings.Bind(wiring, {&hub, bridge.get(), service->runtime()->async_tool_runtime(),
                               ledger->session_id(), turn_id, std::nullopt});
        bridge->BeginTurn(turn_id, "external_user");
        auto input = User(pending.input.text);
        bridge->RecordInput(input);
        const auto outcome = service->execution()->agent().Run(std::move(input), wiring);
        REQUIRE(outcome.has_value());
        REQUIRE_FALSE(outcome->cancelled);
        bridge->EndTurn(true, false, {});
        bindings.Reset();
        REQUIRE_FALSE(bridge->last_committed_assistant_message_id().empty());
        REQUIRE(service->RecordTurnFinal({receipt.operation_id, turn_id, "success",
            {bridge->last_committed_assistant_message_id()}, true}));
    }
    std::atomic<bool> closed{false};
    std::string close_error;
    std::jthread closer;
    ReleaseGateOnExit release{state->gate}; // release before joining on assertion failure
    {
        std::unique_lock lock(state->gate->mutex);
        REQUIRE(state->gate->cv.wait_for(lock, 5s, [&] { return state->gate->entered; }));
    }
    CHECK(coordinator->running_count() == 1);
    std::string job_id;
    std::string owner_epoch;
    const auto running_ledger = v3::ReadV3Ledger(main_path);
    REQUIRE(running_ledger.has_value());
    for (const auto& event : running_ledger->events) {
        if (event.kind != v3::EventKindV3::ToolJobDispatched) continue;
        job_id = event.payload.at("jobId").get<std::string>();
        owner_epoch = event.payload.at("ownerEpoch").get<std::string>();
    }
    REQUIRE_FALSE(job_id.empty());
    REQUIRE_FALSE(owner_epoch.empty());
    if (injected_terminal) {
        // The existing diagnostic seam simulates a terminal observation racing
        // ahead of actual callback exit. It cannot authorize owner destruction.
        REQUIRE(coordinator->DebugSubmitEnvelope(job_id, owner_epoch, tools::Tool::Result::Text("observed early")));
        CHECK(coordinator->running_count() == 0);
        CHECK(coordinator->GetJob(job_id).state == "succeeded");
        std::lock_guard lock(state->gate->mutex);
        CHECK_FALSE(state->gate->exited);
    }
    auto* closing_service = service.get();
    closer = std::jthread([owned = std::move(service), &closed, &close_error]() mutable {
        close_error = owned->Close("execution_closed").error_code;
        owned.reset();
        closed.store(true);
    });
    {
        std::unique_lock lock(state->gate->mutex);
        REQUIRE(state->gate->cv.wait_for(lock, 5s, [&] { return state->gate->cancelled; }));
        CHECK_FALSE(state->gate->exited);
    }
    CHECK_FALSE(closed.load());
    CHECK(state->backend_alive.load());
    CHECK(state->tool_alive.load());
    CHECK_FALSE(state->agent_token.expired());
    CHECK(endpoint.IsAlive());
    const auto during_close = closing_service->SubmitInput({"during-close", "must remain rejected", {}});
    CHECK_FALSE(during_close.accepted);
    CHECK_FALSE(during_close.duplicate);
    CHECK_FALSE(during_close.error_code.empty());
    const auto while_live = v3::ReadV3Ledger(main_path);
    REQUIRE(while_live.has_value());
    CHECK_FALSE(HasSessionEnd(*while_live));
    state->gate->Release();
    closer.join();
    CHECK(closed.load());
    CHECK(close_error.empty());
    CHECK(state->callback_used_dependencies_after_cancel.load());
    CHECK(captured_dependencies.expired());
    CHECK(state->hooks_destroyed_with_dependencies.load());
    CHECK(state->worker_shutdown_attempts.load() > 0);
    CHECK(state->worker_shutdown_rejected.load());
    CHECK_FALSE(state->gate->timed_out);
    CHECK(state->gate->exited);
    CHECK(state->Destruction() == std::vector<std::string>{"agent", "tool", "backend"});
    CHECK(state->tool_saw_agent_destroyed.load());
    CHECK(state->tool_saw_client_alive.load());
    CHECK(state->backend_saw_borrowers_destroyed.load());
    CHECK(endpoint.shutdown_count == 1);
    const auto ledger_after = v3::ReadV3Ledger(main_path);
    REQUIRE(ledger_after.has_value());
    std::size_t observed_sequence = 0, ended_sequence = 0;
    unsigned terminal_observations = 0;
    for (const auto& event : ledger_after->events) {
        if (event.kind == v3::EventKindV3::ToolJobObserved &&
            event.payload.value("observedStatus", "") == (injected_terminal ? "succeeded" : "cancelled")) {
            ++terminal_observations;
            observed_sequence = event.seq;
        }
        if (event.kind == v3::EventKindV3::SessionEnded) ended_sequence = event.seq;
    }
    CHECK(terminal_observations == 1);
    CHECK(observed_sequence > 0);
    CHECK(ended_sequence > observed_sequence);
    const auto authorization_calls = state->authorization_calls.load();
    const auto clock_calls = state->clock_calls.load();
    const auto rejected_read = coordinator->GetJob(job_id);
    CHECK(rejected_read.access_denied);
    CHECK(rejected_read.access_reason == "job.coordinator.closed");
    CHECK(state->authorization_calls.load() == authorization_calls);
    CHECK(state->clock_calls.load() == clock_calls);
    const auto rejected_wait = coordinator->WaitJobs({job_id}, 1, true);
    REQUIRE(rejected_wait.statuses.size() == 1);
    CHECK(rejected_wait.statuses[0].access_denied);
    CHECK(rejected_wait.statuses[0].access_reason == "job.coordinator.closed");
    CHECK(state->authorization_calls.load() == authorization_calls);
    CHECK(state->clock_calls.load() == clock_calls);
}
} // namespace

TEST_CASE("session execution: close retains borrowed dependencies until a live background callback exits") {
    CheckLiveBackgroundShutdown(false);
}

TEST_CASE("session execution: a terminal observation cannot stand in for live callback exit") {
    CheckLiveBackgroundShutdown(true);
}

TEST_CASE("session execution: a stop request rejects input and closes a late attached async runtime") {
    Fixture fixture;
    auto state = std::make_shared<ResourceState>();
    runtime::SessionService service(fixture.Launch());
    service.InitializeExecution(Resources(fixture, state), Profile("late-attach"), std::nullopt);
    REQUIRE(service.execution() != nullptr);
    auto* ledger = service.trajectory();
    REQUIRE(ledger != nullptr);
    auto request = DeclaredJob(*ledger->v3_main_writer(), "execution_probe");
    service.RequestExecutionShutdown();
    const auto rejected_input = service.SubmitInput({"after-stop", "must not be accepted", {}});
    CHECK_FALSE(rejected_input.accepted);
    CHECK_FALSE(rejected_input.error_code.empty());
    runtime::AsyncToolRuntime::Hooks hooks;
    hooks.writer = ledger->v3_main_writer();
    hooks.writer_mutex = ledger->v3_tool_results_mutex();
    hooks.auth = [state](const auto&, const auto&) {
        ++state->authorization_calls;
        return tools::JobAuthDecision{true, false, {}};
    };
    std::atomic<unsigned> executions{0};
    hooks.executor = [&](const auto&) { ++executions; return tools::Tool::Result::Text("must not execute"); };
    auto asynchronous = runtime::AsyncToolRuntime::Create(std::move(hooks), {});
    REQUIRE(asynchronous != nullptr);
    auto coordinator = asynchronous->coordinator();
    service.runtime()->AttachAsyncToolRuntime(std::move(asynchronous));
    const auto authorization_calls = state->authorization_calls.load();
    const auto rejected_job = coordinator->StartJob(request);
    CHECK_FALSE(rejected_job.ok);
    CHECK(rejected_job.error_code == "job.start.closing");
    CHECK(executions.load() == 0);
    CHECK(state->authorization_calls.load() == authorization_calls);
    const auto fresh_work = service.PendingInputsSnapshot();
    CHECK(fresh_work.empty());
    service.ShutdownExecution();
    CHECK(service.Close("test_complete").error_code.empty());
}
