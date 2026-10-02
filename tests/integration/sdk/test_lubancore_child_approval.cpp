#include <doctest/doctest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "agent/tool_trace.hpp"
#include "api/backend.hpp"
#include "runtime/id_authority.hpp"
#include "runtime/session_service.hpp"
#include "runtime/tool_trace_hub.hpp"
#include "runtime/turn_runtime.hpp"
#include "sdk/approval.hpp"
#include "tools/agent_tool.hpp"
#include "tools/path_utils.hpp"
#include "tools/registry.hpp"
#include "trajectory/v3/reader.hpp"
#include "trajectory/v3/subagent.hpp"

namespace {
using namespace std::chrono_literals;
using namespace lubancode;
namespace rt = runtime;
namespace sdk = lubancore;
namespace v3 = trajectory::v3;
namespace fs = std::filesystem;
using Json = nlohmann::json;
using Host = sdk::detail::SessionApprovals;

class Watchdog {
public:
    Watchdog() : thread_([this] {
        std::unique_lock lock(mutex_);
        if (!cv_.wait_for(lock, 40s, [&] { return done_; })) std::abort();
    }) {}
    ~Watchdog() {
        { std::lock_guard lock(mutex_); done_ = true; }
        cv_.notify_all();
        thread_.join();
    }
private:
    std::mutex mutex_;
    std::condition_variable cv_;
    bool done_ = false;
    std::thread thread_;
};
struct Cleanup {
    std::function<void()> release;
    ~Cleanup() { release(); }
};
class V3Mode {
public:
    V3Mode() {
        if (const char* value = std::getenv("LUBANCODE_TRAJECTORY_V3_NEW_SESSIONS")) old_ = value;
        Set("1");
    }
    ~V3Mode() { Set(old_ ? old_->c_str() : nullptr); }
private:
    static void Set(const char* value) {
#ifdef _WIN32
        (void)_putenv_s("LUBANCODE_TRAJECTORY_V3_NEW_SESSIONS", value ? value : "");
#else
        if (value) (void)setenv("LUBANCODE_TRAJECTORY_V3_NEW_SESSIONS", value, 1);
        else (void)unsetenv("LUBANCODE_TRAJECTORY_V3_NEW_SESSIONS");
#endif
    }
    std::optional<std::string> old_;
};
struct Paths {
    fs::path root, cwd;
    Paths() {
        static std::atomic<unsigned> serial{0};
        root = fs::temp_directory_path() / ("child-approval-" +
            std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + "-" +
            std::to_string(++serial));
        fs::create_directories(root / "project");
        root = fs::canonical(root);
        cwd = fs::canonical(root / "project");
    }
    ~Paths() { std::error_code ec; fs::remove_all(root, ec); }
};
std::vector<api::StreamEvent> TextScript() {
    return {api::MessageStart{"msg", "model"}, api::TextDelta{"child complete"},
            api::ContentBlockDone{0}, api::MessageDone{"end_turn", api::Usage{}}};
}
std::vector<api::StreamEvent> ToolScript(std::string id = "child-call-1",
                                       std::string name = "guarded", Json input = Json::object()) {
    return {api::MessageStart{"msg", "model"}, api::ToolUseStart{0, std::move(id), std::move(name)},
            api::ToolUseInputDelta{0, input.dump()}, api::ContentBlockDone{0},
            api::MessageDone{"tool_use", api::Usage{}}};
}
class Backend final : public api::Backend {
public:
    std::vector<std::vector<api::StreamEvent>> scripts{ToolScript(), TextScript()};
    std::atomic<std::size_t> calls{0};
    // Fixture-only borrow, inspected synchronously inside this active child
    // turn's phase callback; never used after Run returns or stored in a ticket.
    const std::atomic<bool>* live_cancel = nullptr;
    std::expected<void, api::Error> send_stream(const api::Request&,
        const std::function<void(const api::StreamEvent&)>& emit, const std::atomic<bool>* cancel = nullptr) override {
        live_cancel = cancel;
        const auto index = calls.fetch_add(1);
        if (index >= scripts.size())
            return std::unexpected(api::Error{api::ErrorKind::Api, "child fixture script exhausted", 0});
        for (const auto& event : scripts[index]) emit(event);
        return {};
    }
};
struct Executions {
    std::atomic<int> tools{0};
    std::mutex mutex;
    std::vector<tools::ToolInvocationIdentity> invocations;
};
class Guarded final : public tools::Tool {
public:
    explicit Guarded(std::shared_ptr<Executions> state) : state_(std::move(state)) {}
    std::string name() const override { return "guarded"; }
    std::string description() const override { return "real child protected tool"; }
    Json input_schema() const override { return Json::object(); }
    bool needs_confirm() const override { return true; }
    tools::ApprovalClass approval_class() const override { return tools::ApprovalClass::Command; }
    Result execute(const Json&) override { return {"context required", true}; }
    Result execute(const Json&, const tools::ToolExecutionContext& context) override {
        { std::lock_guard lock(state_->mutex); state_->invocations.push_back(context.invocation); }
        ++state_->tools;
        return {"guarded child result", false};
    }
private:
    std::shared_ptr<Executions> state_;
};
std::vector<Json> Rows(const fs::path& path) {
    std::ifstream input(path, std::ios::binary);
    REQUIRE(input.good());
    std::vector<Json> rows;
    for (std::string line; std::getline(input, line);) {
        if (line.empty()) continue;
        auto row = Json::parse(line, nullptr, false);
        REQUIRE_FALSE(row.is_discarded());
        rows.push_back(std::move(row));
    }
    return rows;
}
int Started(const fs::path& path, const std::string& action = {}) {
    int count = 0;
    for (const auto& row : Rows(path))
        if (row.value("kind", std::string()) == "tool.execution.started" &&
            (action.empty() || row.value("actionId", std::string()) == action)) ++count;
    return count;
}
bool Empty(const tools::ToolInvocationIdentity& value) {
    return value.session_id.empty() && value.operation_id.empty() && value.turn_id.empty() &&
        value.action_id.empty() && value.attempt == 0;
}
rt::ApprovalResponse Accept(rt::InteractionDecision decision = rt::InteractionDecision::Accept) {
    return {decision, "fixture reply"};
}

// Fixed equal child identities use native Request/Bootstrap/Link and the
// production OwnV3 bridge, with its typed append/checked-Close receipt.
struct Rig {
    V3Mode mode;
    Paths paths;
    Backend backend;
    tools::ToolRegistry registry;
    std::shared_ptr<Executions> executions = std::make_shared<Executions>();
    std::unique_ptr<rt::SessionService> service;
    std::unique_ptr<rt::TrajectoryTurnBridge> parent;
    rt::IdAuthority ids;
    rt::ToolTraceHub parent_hub{ids};
    Host host;
    std::unique_ptr<tools::AgentTool> tool;
    tools::AgentTool::Hooks hooks;
    std::atomic<bool> cancel{false};
    tools::ToolInvocationIdentity cause;
    std::mutex mutex;
    std::condition_variable cv;
    std::vector<std::pair<sdk::Approval, rt::ChildApprovalRequest>> tickets;
    std::vector<rt::ChildApprovalScope> closed;
    fs::path child_path;
    int next_ticket = 0, legacy_asks = 0, finishes = 0;
    std::chrono::milliseconds timeout{10s};
    bool publish_throw = false;
    std::optional<rt::InteractionDecision> automatic;
    std::function<void(const rt::ChildApprovalRequest&)> on_publish;

    explicit Rig(bool fixed_child = false) {
        rt::SessionLaunchRequest launch;
        launch.cwd_utf8 = tools::PathToUtf8(paths.cwd);
        launch.workspace_identity = workspace::MakeFallbackIdentity(paths.cwd);
        launch.workspaces_root = paths.root / "workspaces";
        launch.lubancode_version = "child-approval-fixture";
        launch.v3_system_content = "parent fixture";
        service = std::make_unique<rt::SessionService>(std::move(launch));
        REQUIRE(service->runtime() != nullptr);
        REQUIRE(service->v3_format());
        const auto accepted = service->SubmitInput({"actual-client-key", "dispatch child", {}});
        REQUIRE(accepted.accepted);
        const auto input = service->PopPendingInput();
        REQUIRE(input.status == rt::SessionService::PendingPop::Status::Ok);
        CHECK(input.input.operation_id == accepted.operation_id);
        auto* ledger = service->trajectory();
        REQUIRE(ledger != nullptr);
        parent = ledger->NewTurnBridge({"fixture", "fixture", "fixture"});
        REQUIRE(parent != nullptr);
        parent_hub.AttachTrajectory(parent.get());
        parent->BeginTurn("parent-turn", "external_user");
        api::Message user;
        user.role = api::Role::User;
        user.content.push_back(api::TextBlock{"dispatch child"});
        parent->RecordInput(user);
        const auto request = parent->OnRequestPrepared(api::Request{}, agent::RequestPreparedContext{});
        REQUIRE_FALSE(request.empty());
        parent->OnRequestSent(request);
        api::Message output;
        output.role = api::Role::Assistant;
        output.content.push_back(api::ToolUseBlock{"parent-agent-call", "agent", Json{{"prompt", "child task"}}});
        REQUIRE(parent->OnOutputCompleted(request, output, "tool_use", "parent-response"));
        agent::ToolTraceEvent event;
        event.kind = agent::ToolTraceEventKind::Scheduled;
        event.execution_id = "parent-execution";
        event.tool_use_id = "parent-agent-call";
        event.tool_name = "agent";
        event.batch_id = "parent-batch";
        event.sequence_in_batch = 0;
        event.timestamp_ms = 1759000000000LL;
        parent_hub.OnTrace(event);
        event.kind = agent::ToolTraceEventKind::ExecutionStarted;
        event.effective_input_sha256 = std::string(64, '0');
        event.effective_arguments = Json{{"prompt", "child task"}};
        event.effect_class = agent::EffectClass::InProcessUnknown;
        parent_hub.OnTrace(event);
        const auto identity = parent->V3ExecutingCallIdentity("parent-agent-call");
        REQUIRE(identity.has_value());
        cause = {ledger->session_id(), input.input.operation_id, "parent-turn", identity->first,
                 static_cast<int>(identity->second)};
        host.SetOperationOwner(cause.session_id, cause.operation_id, ledger->v3_main_writer()->run_id());
        registry.Register(std::make_unique<Guarded>(executions));
        tool = std::make_unique<tools::AgentTool>(backend, registry, tools::PathToUtf8(paths.cwd));
        hooks.trajectory_spawn = [this, fixed_child](const std::string& label, const std::string& parent_run,
                                                    rt::SubagentSpawnFailure*) {
            auto* ledger = service->trajectory();
            if (!fixed_child) {
                auto child = ledger->SpawnSubagent("parent-agent-call", label, parent_run);
                REQUIRE(child.has_value());
                const auto* writer = (*child)->turn_bridge().v3_writer();
                child_path = ledger->session_dir() / "subagents" / writer->session_id() /
                    (writer->session_id() + ".jsonl");
                return std::move(*child);
            }
            auto* writer = ledger->v3_main_writer();
            const auto origin = parent->V3DeclaredCallOrigin("parent-agent-call");
            REQUIRE(origin.has_value());
            v3::ParentActionRef ref{writer->session_id(), writer->run_id(), origin->turn_id,
                origin->step_id, origin->action_id, origin->message_id};
            const std::string sid = "20261003-000000-S000001", run = "agent-fixed-child-run";
            child_path = ledger->session_dir() / "subagents" / sid / (sid + ".jsonl");
            auto spawn = v3::SubagentSpawn::Request(*writer, ref.action_id, ref.turn_id, ref.step_id,
                writer->NewTaskId(), {sid, run, "subagents/" + sid + "/" + sid + ".jsonl"}, ref,
                Json{{"taskLabel", label}}, Json::object());
            auto initialized = spawn.BootstrapChild(*writer, run, "parent fixture", label);
            REQUIRE(initialized.error.empty());
            REQUIRE(initialized.child_writer.has_value());
            const auto linked = spawn.Link(*writer, initialized.checkpoint);
            REQUIRE(linked.status == v3::WriteReceipt::Status::Committed);
            auto owned_writer = std::make_unique<v3::V3Writer>(std::move(*initialized.child_writer));
            auto books = std::make_unique<rt::V3SessionBooks>();
            books->system_content = "parent fixture";
            books->settings_version = 1;
            trajectory::EventScope scope;
            scope.session_id = sid; scope.run_id = run;
            auto bridge = std::make_unique<rt::TrajectoryTurnBridge>(owned_writer.get(), books.get(),
                std::move(scope), rt::TrajectoryTurnBridge::Identity{"subagent", "subagent", "subagent"});
            return rt::TrajectorySubagentBridge::OwnV3(std::move(owned_writer), std::move(books),
                std::move(bridge), parent->child_terminal_registry(),
                rt::ChildApprovalParent{ref.session_id, ref.run_id, ref.turn_id, ref.action_id, ref.declared_message_ref});
        };
        hooks.trajectory_child_finished = [this](const rt::SubagentTerminalReceipt& receipt) {
            CHECK(receipt.durable());
            CHECK(receipt.format == rt::SubagentJournalFormat::V3);
            CHECK(receipt.seal == rt::SubagentSealState::Closed);
            CHECK(std::holds_alternative<v3::WriteReceipt>(receipt.append));
            ++finishes;
            parent->NoteChildTerminal(receipt);
        };
        hooks.on_permission_evaluate = [](const std::string&, const std::string& name,
            tools::ApprovalClass kind, const Json& input, const rt::ToolHookDecision& pre) {
            return rt::EvaluatePermission(rt::PermissionContext{}, pre, kind, name, input);
        };
        hooks.on_child_permission_evaluate = [](const rt::ChildApprovalScope& scope,
            const rt::ToolHookDecision& pre, tools::ApprovalClass kind, const std::string& name, const Json& input) {
            rt::PermissionContext rules;
            if (scope.permission_floor != "inherit") {
                const auto floor = ParseApprovalMode(scope.permission_floor);
                REQUIRE(floor.has_value());
                rules.mode = *floor;
            }
            // The child uses the same rule function, without the host's
            // ordinary temporary allowed account. ModePolicy remains a
            // separate earlier loop gate.
            return rt::EvaluatePermission(rules, pre, kind, name, input);
        };
        hooks.on_tool_confirm = [this](const auto&, const auto&, const auto&) { ++legacy_asks; return false; };
        hooks.on_child_tool_confirm_scoped = [this](const rt::ChildApprovalRequest& request) {
            sdk::Approval approval;
            approval.request_id = "actual-child-ticket-" + std::to_string(++next_ticket);
            approval.operation_id = request.scope.host_operation_id;
            approval.tool_call_id = request.request.tool_use_id;
            approval.tool_name = request.request.tool_name;
            approval.input_json = request.request.input.dump();
            approval.cwd = request.scope.effective_cwd;
            auto registered = host.RegisterChildScoped(request, std::move(approval), timeout,
                [this](const sdk::Approval& ticket, const rt::ChildApprovalRequest& actual) {
                    { std::lock_guard lock(mutex); tickets.emplace_back(ticket, actual); }
                    cv.notify_all();
                    if (on_publish) on_publish(actual);
                    if (publish_throw) throw std::runtime_error("fixture publisher failed");
                    if (automatic) CHECK(host.Resolve(ticket.request_id, Accept(*automatic)));
                });
            if (!registered) return rt::ApprovalLease{};
            return std::move(*registered);
        };
        hooks.on_child_tool_granted = [this](const auto& scope, const auto& name) { return host.ChildAllowed(scope, name); };
        hooks.on_child_approval_scope_closed = [this](const auto& scope) {
            host.CloseChildScope(scope);
            { std::lock_guard lock(mutex); closed.push_back(scope); }
        };
        Install();
    }
    ~Rig() {
        cancel.store(true);
        host.Close();
        tool.reset();
        parent.reset();
        if (service) (void)service->Close("fixture");
    }
    void Install() { tool->SetHooks(hooks); }
    Json Input() const { return Json{{"title", "child approval"}, {"prompt", "child task"},
                                     {"execution_mode", "foreground"}}; }
    tools::Tool::Result Run(Json input = Json()) {
        tools::ToolExecutionContext context{&cancel};
        context.invocation = cause;
        return tool->execute(input.is_null() ? Input() : input, context);
    }
    auto Ticket(std::size_t index = 0) {
        std::unique_lock lock(mutex);
        REQUIRE(cv.wait_for(lock, 10s, [&] { return tickets.size() > index; }));
        return tickets.at(index);
    }
    void Refused() {
        CHECK(executions->tools.load() == 0);
        CHECK(Started(child_path) == 0);
        CHECK(legacy_asks == 0);
        CHECK(host.Pending().empty());
        CHECK(finishes == 1);
        CHECK(v3::VerifyV3File(child_path).ok);
    }
};
void Ready(std::future<tools::Tool::Result>& future) {
    REQUIRE(future.wait_for(10s) == std::future_status::ready);
    (void)future.get();
}
} // namespace

TEST_CASE("Child approval: real declaration publishes before Started and owns no invented child operation") {
    Watchdog watchdog;
    Rig rig;
    std::future<tools::Tool::Result> future;
    Cleanup cleanup{[&] { rig.cancel.store(true); rig.host.Close(); }};
    future = std::async(std::launch::async, [&] { return rig.Run(); });
    const auto [ticket, request] = rig.Ticket();
    CHECK(request.scope.host_session_id == rig.cause.session_id);
    CHECK(request.scope.host_operation_id == rig.cause.operation_id);
    CHECK(request.scope.parent_session_id == rig.cause.session_id);
    CHECK(request.scope.parent_run_id == rig.service->trajectory()->v3_main_writer()->run_id());
    CHECK(request.owner_task_id > 0);
    CHECK_FALSE(request.child_turn_id.empty());
    CHECK_FALSE(request.child_declared_action_id.empty());
    CHECK_FALSE(request.child_declared_message_id.empty());
    const auto before = Rows(rig.child_path);
    const auto declaration = std::find_if(before.begin(), before.end(), [&](const auto& row) {
        return row.value("messageId", std::string()) == request.child_declared_message_id;
    });
    REQUIRE(declaration != before.end());
    CHECK(declaration->at("sessionId").get<std::string>() == request.scope.child_session_id);
    CHECK(declaration->at("runId").get<std::string>() == request.scope.child_run_id);
    CHECK(declaration->at("turnId").get<std::string>() == request.child_turn_id);
    CHECK(declaration->at("message").at("role").get<std::string>() == "assistant");
    CHECK(Started(rig.child_path) == 0);
    CHECK(rig.executions->tools.load() == 0);
    REQUIRE(rig.host.Resolve(ticket.request_id, Accept()));
    Ready(future);
    CHECK(rig.executions->tools.load() == 1);
    CHECK(Started(rig.child_path, request.child_declared_action_id) == 1);
    REQUIRE(rig.executions->invocations.size() == 1);
    CHECK(Empty(rig.executions->invocations.front()));
    CHECK(rig.closed.size() == 1);
    CHECK(rig.legacy_asks == 0);
    CHECK(v3::VerifyV3File(rig.child_path).ok);
}

TEST_CASE("Child approval: session grant survives ticket retirement and clears only at child completion") {
    Rig rig;
    rig.backend.scripts = {ToolScript(), ToolScript("child-call-2"), TextScript()};
    rig.automatic = rt::InteractionDecision::AcceptForSession;
    int grant_queries = 0;
    rig.hooks.on_child_tool_granted = [&](const auto& scope, const auto& name) {
        const bool allowed = rig.host.ChildAllowed(scope, name);
        if (allowed) { ++grant_queries; CHECK(rig.host.Pending().empty()); CHECK(rig.closed.empty()); }
        return allowed;
    };
    rig.Install();
    CHECK_FALSE(rig.Run().is_error);
    CHECK(rig.executions->tools.load() == 2);
    CHECK(rig.tickets.size() == 1);
    CHECK(grant_queries == 1);
    REQUIRE(rig.closed.size() == 1);
    const auto scope = rig.closed.front();
    CHECK_FALSE(rig.host.ChildAllowed(scope, "guarded"));
    CHECK(rig.host.AllowedTools().empty());
    CHECK(Started(rig.child_path) == 2);
    sdk::Approval late = rig.tickets.front().first;
    late.request_id = "late-child";
    const auto rejected = rig.host.RegisterChildScoped(rig.tickets.front().second, late, 10s, [](const auto&, const auto&) {});
    CHECK_FALSE(rejected.has_value());
    CHECK(v3::VerifyV3File(rig.child_path).ok);
    {
        Rig automatic;
        automatic.hooks.on_child_permission_evaluate = [](const auto&, const auto&, auto, const auto&, const auto&) {
            rt::PermissionVerdict result; result.action = rt::PermissionVerdict::Action::Allow; return result;
        };
        automatic.Install();
        CHECK_FALSE(automatic.Run().is_error);
        CHECK(automatic.executions->tools.load() == 1);
        CHECK(automatic.tickets.empty());
        REQUIRE(automatic.closed.size() == 1);
        const auto actual_scope = automatic.closed.front();
        const auto rows = Rows(automatic.child_path);
        const auto started = std::find_if(rows.begin(), rows.end(), [](const auto& row) {
            return row.value("kind", std::string()) == "tool.execution.started";
        });
        REQUIRE(started != rows.end());
        const auto declared = std::find_if(rows.begin(), rows.end(), [](const auto& row) {
            return row.value("type", std::string()) == "message" &&
                row.at("message").value("role", std::string()) == "assistant" &&
                row.at("message").dump().find("child-call-1") != std::string::npos;
        });
        REQUIRE(declared != rows.end());
        const auto tasks = automatic.tool->TaskSnapshots();
        REQUIRE(tasks.size() == 1);
        rt::ChildApprovalRequest actual{actual_scope, started->at("turnId").get<std::string>(),
            started->at("actionId").get<std::string>(), declared->at("messageId").get<std::string>(),
            tasks.front().id, {"child-call-1", "guarded", Json::object(), ""}};
        sdk::Approval after;
        after.request_id = "never-registered-before-child-close";
        after.operation_id = actual_scope.host_operation_id;
        after.tool_call_id = actual.request.tool_use_id;
        after.tool_name = actual.request.tool_name;
        after.input_json = actual.request.input.dump();
        after.cwd = actual_scope.effective_cwd;
        CHECK_FALSE(automatic.host.RegisterChildScoped(actual, after, 10s, [](const auto&, const auto&) {}).has_value());
        CHECK(automatic.host.Pending().empty());
        CHECK(v3::VerifyV3File(automatic.child_path).ok);
    }
}

TEST_CASE("Child approval: two actual parents with equal child SID and run never share grants or tickets") {
    Watchdog watchdog;
    Rig first(true), second(true);
    first.backend.scripts = {ToolScript(), ToolScript("child-call-2"), TextScript()};
    std::future<tools::Tool::Result> a, b;
    Cleanup cleanup{[&] { first.cancel.store(true); second.cancel.store(true); first.host.Close(); second.host.Close(); }};
    a = std::async(std::launch::async, [&] { return first.Run(); });
    b = std::async(std::launch::async, [&] { return second.Run(); });
    const auto one = first.Ticket(), two = second.Ticket();
    CHECK(one.second.scope.child_session_id == two.second.scope.child_session_id);
    CHECK(one.second.scope.child_run_id == two.second.scope.child_run_id);
    // Parent session/run strings also belong to separate workspace domains;
    // only actual ownership plus cwd/ledger provenance distinguish the hosts.
    CHECK(one.second.scope != two.second.scope);
    CHECK(one.second.scope.effective_cwd != two.second.scope.effective_cwd);
    CHECK(first.child_path != second.child_path);
    REQUIRE(first.host.Resolve(one.first.request_id, Accept(rt::InteractionDecision::AcceptForSession)));
    Ready(a);
    CHECK(first.executions->tools.load() == 2);
    CHECK(first.tickets.size() == 1);
    CHECK(second.executions->tools.load() == 0);
    CHECK(second.host.Pending().size() == 1);
    CHECK_FALSE(second.host.ChildAllowed(one.second.scope, "guarded"));
    auto foreign = two.second;
    foreign.scope.host_session_id = "unowned-host-session";
    foreign.scope.parent_session_id = foreign.scope.host_session_id;
    auto foreign_ticket = two.first;
    foreign_ticket.request_id = "foreign-parent-ticket";
    CHECK_FALSE(second.host.RegisterChildScoped(foreign, foreign_ticket, 10s, [](const auto&, const auto&) {}).has_value());
    REQUIRE(second.host.Resolve(two.first.request_id, Accept()));
    Ready(b);
    CHECK(second.executions->tools.load() == 1);
    CHECK(first.host.AllowedTools().empty());
    CHECK(second.host.AllowedTools().empty());
    CHECK(v3::VerifyV3File(first.child_path).ok);
    CHECK(v3::VerifyV3File(second.child_path).ok);
}

TEST_CASE("Child approval: pending foreground cancellation retires its ticket before any execution") {
    Watchdog watchdog;
    Rig rig;
    std::future<tools::Tool::Result> future;
    Cleanup cleanup{[&] { rig.cancel.store(true); rig.host.Close(); }};
    future = std::async(std::launch::async, [&] { return rig.Run(); });
    const auto ticket = rig.Ticket();
    rig.cancel.store(true, std::memory_order_release);
    Ready(future);
    rig.Refused();
    CHECK_FALSE(rig.host.Resolve(ticket.first.request_id, Accept()));
    CHECK(rig.closed.size() == 1);
}

TEST_CASE("Child approval: cancellation in the Running phase still precedes ToolExecutionStarted") {
    Rig rig;
    rig.automatic = rt::InteractionDecision::Accept;
    rig.hooks.on_tool_phase = [&](const auto&, const auto&, rt::ToolPhase phase) {
        if (phase == rt::ToolPhase::Running) {
            REQUIRE(rig.backend.live_cancel != nullptr);
            rig.cancel.store(true, std::memory_order_release);
            const auto deadline = std::chrono::steady_clock::now() + 2s;
            // Let the existing CancelChain observe the parent's flag before
            // returning from this callback, then test the final checkpoint.
            while (!rig.backend.live_cancel->load(std::memory_order_acquire) &&
                   std::chrono::steady_clock::now() < deadline) std::this_thread::yield();
            REQUIRE(rig.backend.live_cancel->load(std::memory_order_acquire));
        }
    };
    rig.Install();
    (void)rig.Run();
    rig.Refused();
    REQUIRE(rig.tickets.size() == 1);
    CHECK_FALSE(rig.host.Resolve(rig.tickets.front().first.request_id, Accept()));
}

TEST_CASE("Child approval: registered deadline expires and late reply cannot start the tool") {
    Watchdog watchdog;
    Rig rig;
    rig.timeout = 60ms;
    std::future<tools::Tool::Result> future;
    Cleanup cleanup{[&] { rig.cancel.store(true); rig.host.Close(); }};
    future = std::async(std::launch::async, [&] { return rig.Run(); });
    const auto ticket = rig.Ticket();
    Ready(future);
    rig.Refused();
    CHECK_FALSE(rig.host.Resolve(ticket.first.request_id, Accept()));
    CHECK(rig.closed.size() == 1);
}

TEST_CASE("Child approval: closing one host wakes its child and leaves another actual host pending") {
    Watchdog watchdog;
    Rig first, second;
    std::future<tools::Tool::Result> a, b;
    Cleanup cleanup{[&] { first.cancel.store(true); second.cancel.store(true); first.host.Close(); second.host.Close(); }};
    a = std::async(std::launch::async, [&] { return first.Run(); });
    b = std::async(std::launch::async, [&] { return second.Run(); });
    const auto one = first.Ticket(), two = second.Ticket();
    first.host.Close();
    Ready(a);
    first.Refused();
    CHECK_FALSE(first.host.Resolve(one.first.request_id, Accept()));
    CHECK(second.host.Pending().size() == 1);
    CHECK(second.executions->tools.load() == 0);
    REQUIRE(second.host.Resolve(two.first.request_id, Accept()));
    Ready(b);
    CHECK(second.executions->tools.load() == 1);
}

TEST_CASE("Child approval: publisher failure uses normal child Finish and no synchronous fallback") {
    Rig rig;
    rig.publish_throw = true;
    (void)rig.Run();
    rig.Refused();
    CHECK(rig.closed.size() == 1);
    REQUIRE(rig.tickets.size() == 1);
    CHECK_FALSE(rig.host.Resolve(rig.tickets.front().first.request_id, Accept()));
}

TEST_CASE("Child approval: missing or foreign actual parent cause refuses without parent Yolo fallback") {
    for (const bool absent : {false, true}) {
        Rig rig;
        if (absent) rig.cause = {};
        else rig.cause.action_id += "-foreign";
        rig.hooks.on_permission_evaluate = [](const auto&, const auto&, auto, const auto&, const auto&) {
            rt::PermissionVerdict result; result.action = rt::PermissionVerdict::Action::Allow; return result;
        };
        rig.Install();
        (void)rig.Run();
        rig.Refused();
        CHECK(rig.tickets.empty());
        CHECK(rig.closed.empty());
    }
}

TEST_CASE("Child approval: incomplete scoped capability refuses and old CLI routed confirmation remains") {
    for (const int missing : {0, 1}) {
        Rig rig;
        if (missing == 0) rig.hooks.on_child_tool_granted = {};
        else rig.hooks.on_child_permission_evaluate = {};
        rig.Install();
        (void)rig.Run();
        rig.Refused();
        CHECK(rig.tickets.empty());
    }
    {
        Rig rig;
        sdk::Approval ordinary;
        ordinary.request_id = "real-ordinary-parent-grant";
        ordinary.operation_id = rig.cause.operation_id;
        ordinary.tool_call_id = "parent-provider-call";
        ordinary.tool_name = "guarded";
        ordinary.input_json = "{}";
        ordinary.cwd = tools::PathToUtf8(rig.paths.cwd);
        bool registered = false;
        const auto parent_future = rig.host.Register(ordinary, 10s, nullptr, &registered);
        REQUIRE(registered);
        REQUIRE(rig.host.Resolve(ordinary.request_id, Accept(rt::InteractionDecision::AcceptForSession)));
        REQUIRE(parent_future->WaitApproval().has_value());
        REQUIRE(rig.host.AllowedTools().contains("guarded"));
        rig.hooks.on_permission_evaluate = [&](const auto&, const std::string& name,
            tools::ApprovalClass kind, const Json& input, const rt::ToolHookDecision& pre) {
            const auto allowed = rig.host.AllowedTools();
            rt::PermissionContext parent;
            parent.always_allowed = &allowed;
            return rt::EvaluatePermission(parent, pre, kind, name, input);
        };
        rig.automatic = rt::InteractionDecision::Accept;
        rig.Install();
        CHECK_FALSE(rig.Run().is_error);
        // A true ordinary Allow exists, but the child still requires its own
        // ticket through the explicitly child-scoped evaluator.
        CHECK(rig.tickets.size() == 1);
        CHECK(rig.executions->tools.load() == 1);
        CHECK(rig.host.AllowedTools().contains("guarded"));
        CHECK(rig.legacy_asks == 0);
    }
    {
        Rig rig;
        rig.hooks.on_child_tool_confirm_scoped = {};
        rig.hooks.on_child_tool_granted = {};
        rig.hooks.on_child_approval_scope_closed = {};
        int routes = 0;
        rig.hooks.on_tool_confirm = [&](const auto&, const auto&, const auto&) { ++rig.legacy_asks; return true; };
        rig.hooks.on_tool_confirm_routed = [&](int owner, auto presenter, const auto& name) {
            CHECK(owner > 0); CHECK(name == "guarded"); ++routes; return presenter();
        };
        rig.Install();
        CHECK_FALSE(rig.Run().is_error);
        CHECK(routes == 1);
        CHECK(rig.legacy_asks == 1);
        CHECK(rig.executions->tools.load() == 1);
        CHECK(rig.tickets.empty());
    }
}

TEST_CASE("Child approval: child grant cannot bypass PreToolUse ask or PermissionRequest denial") {
    Rig rig;
    rig.backend.scripts = {ToolScript(), ToolScript("child-call-2"), ToolScript("child-call-3"), TextScript()};
    rig.automatic = rt::InteractionDecision::AcceptForSession;
    int pre_calls = 0, requests = 0;
    rig.hooks.on_pre_tool_use_hook = [&](const auto&, const auto&, const auto&) {
        rt::ToolHookDecision result;
        if (++pre_calls == 3) result.decision = rt::ToolHookDecision::Decision::Ask;
        return result;
    };
    rig.hooks.on_permission_request = [&](const auto&, const auto&, const auto&) {
        rt::ToolHookDecision result;
        if (++requests == 2) {
            result.decision = rt::ToolHookDecision::Decision::Deny;
            result.reason = "child request denied";
        } else if (requests == 3) {
            result.decision = rt::ToolHookDecision::Decision::Allow;
        }
        return result;
    };
    rig.Install();
    (void)rig.Run();
    CHECK(rig.executions->tools.load() == 2);
    CHECK(rig.tickets.size() == 2);
    CHECK(pre_calls == 3);
    CHECK(requests == 3);
    CHECK(Started(rig.child_path) == 2);
    CHECK(rig.closed.size() == 1);
    CHECK(rig.host.AllowedTools().empty());
}

TEST_CASE("Child approval: real custom child floor wins parent Yolo and missing floor evaluator refuses") {
    for (const bool missing : {false, true}) {
        Rig rig;
        rig.tool->SetCustomAgentResolver([](const std::string& name) -> std::optional<tools::CustomAgentMaterial> {
            if (name != "strict-child") return std::nullopt;
            tools::CustomAgentMaterial material;
            material.definition.name = name;
            material.definition.description = "strict child fixture";
            material.definition.permissions_mode = "default";
            return material;
        });
        rig.tool->SetResolveEnvironment([] {
            agent::AgentProfileResolveEnvironment environment;
            environment.parent_permission = ApprovalMode::Yolo;
            return environment;
        });
        rig.automatic = rt::InteractionDecision::Accept;
        rig.hooks.on_permission_evaluate = [](const auto&, const auto&, auto, const auto&, const auto&) {
            rt::PermissionVerdict result; result.action = rt::PermissionVerdict::Action::Allow; return result;
        };
        if (missing) rig.hooks.on_child_permission_evaluate = {};
        else {
            const auto actual = rig.hooks.on_child_permission_evaluate;
            rig.hooks.on_child_permission_evaluate = [actual](const auto& scope, const auto& pre,
                auto kind, const auto& name, const auto& input) {
                CHECK(scope.permission_floor == "default");
                return actual(scope, pre, kind, name, input);
            };
        }
        rig.Install();
        auto input = rig.Input(); input["agent_type"] = "strict-child";
        (void)rig.Run(input);
        CHECK(rig.executions->tools.load() == (missing ? 0 : 1));
        CHECK(rig.tickets.size() == (missing ? 0 : 1));
        CHECK(rig.legacy_asks == 0);
        CHECK(Started(rig.child_path) == (missing ? 0 : 1));
        if (!missing) CHECK(rig.tickets.front().second.scope.permission_floor == "default");
    }
}

TEST_CASE("Child approval: unsupported nested owner never borrows the root ticket or child grant") {
    Rig rig;
    rig.registry.Register(std::make_unique<tools::AgentDispatchTool>(*rig.tool));
    auto nested = rig.Input();
    nested["title"] = "nested child";
    rig.backend.scripts = {ToolScript(), ToolScript("nested-dispatch", "agent", nested),
                           ToolScript("nested-protected-call"), TextScript(), TextScript()};
    rig.automatic = rt::InteractionDecision::AcceptForSession;
    (void)rig.Run();
    CHECK(rig.executions->tools.load() == 1);
    REQUIRE(rig.tickets.size() == 1);
    CHECK(rig.tickets.front().second.request.tool_use_id == "child-call-1");
    CHECK(rig.closed.size() == 1);
    CHECK(rig.host.Pending().empty());
    CHECK(rig.host.AllowedTools().empty());
    CHECK(rig.legacy_asks == 0);
    const auto tasks = rig.tool->TaskSnapshots();
    REQUIRE(tasks.size() == 2);
    CHECK(std::count_if(tasks.begin(), tasks.end(), [](const auto& task) { return task.parent_task_id != 0; }) == 1);
}

TEST_CASE("Child approval: resolved auto foreground selects the capability while background never retains it") {
    {
        Rig rig;
        rig.automatic = rt::InteractionDecision::Accept;
        auto input = rig.Input(); input["execution_mode"] = "auto";
        CHECK_FALSE(rig.Run(input).is_error);
        CHECK(rig.executions->tools.load() == 1);
        CHECK(rig.tickets.size() == 1);
    }
    {
        Rig rig;
        class BorrowBackend final : public api::Backend {
        public:
            explicit BorrowBackend(Backend& backend) : backend_(backend) {}
            std::expected<void, api::Error> send_stream(const api::Request& request,
                const std::function<void(const api::StreamEvent&)>& emit, const std::atomic<bool>* cancel = nullptr) override {
                return backend_.send_stream(request, emit, cancel);
            }
        private:
            Backend& backend_;
        };
        rig.tool->SetDetachedBackendFactory([&] {
            tools::DetachedAgentBackend detached;
            detached.backend = std::make_unique<BorrowBackend>(rig.backend);
            detached.request_profile.model = "fixture-model";
            return detached;
        });
        rig.tool->SetBackgroundPermissionSource([] {
            tools::BackgroundPermissionLedger allowed;
            allowed.always_allowed.insert("guarded");
            return allowed;
        });
        Cleanup cleanup{[&] { if (rig.tool) rig.tool->CancelAllTasks(); }};
        auto input = rig.Input(); input["execution_mode"] = "background";
        REQUIRE_FALSE(rig.Run(input).is_error);
        const auto deadline = std::chrono::steady_clock::now() + 10s;
        while (rig.tool->HasRunningTasks() && std::chrono::steady_clock::now() < deadline)
            std::this_thread::sleep_for(5ms);
        REQUIRE_FALSE(rig.tool->HasRunningTasks());
        // Join and release the detached backend/borrow before Rig destruction.
        rig.tool.reset();
        CHECK(rig.executions->tools.load() == 1);
        CHECK(rig.tickets.empty());
        CHECK(rig.closed.empty());
        CHECK(rig.host.Pending().empty());
    }
}
