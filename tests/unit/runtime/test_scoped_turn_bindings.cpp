#include <doctest/doctest.h>

#include <stdexcept>

#include "runtime/async_tool_runtime.hpp"
#include "runtime/result_delivery_planner.hpp"
#include "runtime/scoped_turn_bindings.hpp"
#include "runtime/tool_trace_hub.hpp"
#include "scoped_turn_fixture.hpp"
#include "trajectory/v3/reader.hpp"
#include "trajectory/v3/tool_action.hpp"

namespace {
using namespace lubancode;
namespace scope_fixture = test_support::turn_scope;
namespace v3 = trajectory::v3;

struct ProjectionTarget {
    int& calls;
    void Record(const agent::ToolTraceEvent&) { ++calls; }
};

struct DurableTarget final : runtime::ToolTrajectorySink {
    int calls = 0;
    void OnToolTrace(const agent::ToolTraceEvent&) override { ++calls; }
    runtime::ToolResultsCommitReceipt OnToolResultsCommitted(const std::string&,
                                                            const api::Message&) override { return {}; }
    bool ShouldBlockExecution(const agent::ToolTraceEvent&) override { return false; }
};

struct BoundaryTarget final : agent::LoopBoundaryRecorder {
    int requests = 0;
    std::string OnRequestPrepared(const api::Request&, const agent::RequestPreparedContext&) override {
        return "existing-boundary-request-" + std::to_string(++requests);
    }
    bool OnRequestSent(const std::string&) override { return true; }
    void OnUsageRecorded(const std::string&, const api::Usage&, bool, const std::string&,
                         int, bool, bool, bool, const std::string&) override {}
    bool OnOutputCompleted(const std::string&, const api::Message&, const std::string&,
                           const std::string&) override { return true; }
    void OnOutputFailed(const std::string&, const std::string&) override {}
    void OnOutputCancelled(const std::string&, agent::OutputCancelSource) override {}
};

struct PreviewTarget final : runtime::ToolTrajectorySink {
    int rewrites = 0;
    bool ManagesToolResultPreviews() const override { return true; }
    runtime::ToolResultsCommitReceipt RewriteToolResultsForHistory(api::Message&) override {
        ++rewrites;
        return {};
    }
    void OnToolTrace(const agent::ToolTraceEvent&) override {}
    runtime::ToolResultsCommitReceipt OnToolResultsCommitted(const std::string&,
                                                            const api::Message&) override { return {}; }
    bool ShouldBlockExecution(const agent::ToolTraceEvent&) override { return false; }
};

agent::ToolTraceEvent Trace(const std::string& execution) {
    agent::ToolTraceEvent event;
    event.kind = agent::ToolTraceEventKind::Scheduled;
    event.execution_id = execution;
    event.tool_use_id = execution;
    event.tool_name = "scope_probe";
    event.batch_id = "scope-batch";
    return event;
}

struct ThrowOnCopy {
    std::shared_ptr<bool> armed;
    explicit ThrowOnCopy(std::shared_ptr<bool> flag) : armed(std::move(flag)) {}
    ThrowOnCopy(const ThrowOnCopy& other) : armed(other.armed) {
        if (*armed) throw std::runtime_error("armed AgentWiring copy");
    }
    void operator()(const agent::ContextPressure&) const {}
};

struct ThrowingTraceCopy {
    std::shared_ptr<bool> armed;
    int* calls;
    ThrowingTraceCopy(std::shared_ptr<bool> flag, int& counter) : armed(std::move(flag)), calls(&counter) {}
    ThrowingTraceCopy(const ThrowingTraceCopy& other) : armed(other.armed), calls(other.calls) {
        if (*armed) throw std::runtime_error("armed TurnWiring copy");
    }
    void operator()(const agent::ToolTraceEvent&) const { ++*calls; }
};

struct ThrowingCapability final : runtime::ToolTrajectorySink {
    bool armed = false;
    int traces = 0;
    bool ManagesToolResultPreviews() const override {
        if (armed) throw std::runtime_error("capability probe failed after Install changed routing");
        return false;
    }
    void OnToolTrace(const agent::ToolTraceEvent&) override { ++traces; }
    runtime::ToolResultsCommitReceipt OnToolResultsCommitted(const std::string&,
                                                            const api::Message&) override { return {}; }
    bool ShouldBlockExecution(const agent::ToolTraceEvent&) override { return false; }
};

std::string PersistResult(v3::V3Writer& writer) {
    auto action = v3::ToolActionSession::Admit(writer, "turn-000001", "step-000001",
        "action-scope-delivery", "queued", std::nullopt, std::nullopt, nlohmann::json::object());
    REQUIRE(action.Start(writer, "args-scope", v3::ToolIdentity{}, std::nullopt,
        nlohmann::json::object()).status == v3::WriteReceipt::Status::Committed);
    REQUIRE(action.Finish(writer, std::nullopt, std::nullopt).status == v3::WriteReceipt::Status::Committed);
    nlohmann::json metadata;
    metadata["artifactId"] = "scope-result";
    metadata["kind"] = "result_metadata";
    metadata["path"] = "artifacts/scope-result.json";
    metadata["sha256"] = std::string(64, 'a');
    metadata["bytes"] = 32;
    metadata["mediaType"] = "application/json";
    auto persisted = action.PersistedResult(writer, {metadata}, std::nullopt, std::nullopt);
    REQUIRE(persisted.status == v3::WriteReceipt::Status::Committed);
    return persisted.id;
}
}  // namespace

TEST_CASE("ScopedTurnBindings: actual turns retain inbox pressure and soul hooks after temporary hubs die") {
    scope_fixture::AgentFixture fixture;
    runtime::IdAuthority ids;
    int inbox_messages = 0, pressure_calls = 0, soul_locks = 0, persistent_ids = 0, persistent_traces = 0;
    bool pending_inbox = false;
    std::string inbox_text;
    agent::AgentWiring long_lived;
    long_lived.inbox = [&]() -> std::optional<api::Message> {
        if (!pending_inbox) return std::nullopt;
        pending_inbox = false;
        ++inbox_messages;
        api::Message message;
        message.role = api::Role::User;
        message.content.push_back(api::TextBlock{inbox_text});
        return message;
    };
    long_lived.execution_id_issuer = [&] { return "persistent-exec-" + std::to_string(++persistent_ids); };
    long_lived.on_session_soul_locked = [&] { ++soul_locks; };
    long_lived.on_context_pressure = [&](const agent::ContextPressure& pressure) {
        ++pressure_calls;
        if (!pressure.projected_overflow) return;
        api::Message shortened;
        shortened.role = api::Role::User;
        shortened.content.push_back(api::TextBlock{"Preserved pressure hook compacted the old history."});
        fixture.agent->ReplaceHistory({shortened});
    };
    fixture.agent->SetWiring(std::move(long_lived));
    agent::TurnWiring wiring;
    wiring.on_tool_trace = [&](const agent::ToolTraceEvent&) { ++persistent_traces; };
    for (int index = 0; index != 2; ++index) {
        fixture.AddRound(std::to_string(index));
        inbox_text = "INBOX_FOR_ROUND_" + std::to_string(index);
        pending_inbox = true;
        auto hub = std::make_unique<runtime::ToolTraceHub>(ids);
        {
            runtime::ScopedTurnBindings scope(*fixture.agent);
            runtime::ScopedTurnBindings::Bindings bindings;
            bindings.hub = hub.get();
            bindings.thread_id = "temporary-thread";
            bindings.turn_id = "temporary-turn-" + std::to_string(index);
            scope.Bind(wiring, std::move(bindings));
            const auto outcome = fixture.agent->Run("round " + std::to_string(index), wiring);
            REQUIRE(outcome.has_value());
            CHECK_FALSE(outcome->cancelled);
        }
        hub.reset();
        CHECK(fixture.probe->calls == index + 1);
        REQUIRE(fixture.backend.requests.size() == static_cast<std::size_t>((index + 1) * 2));
        CHECK(scope_fixture::ContainsText(fixture.backend.requests[index * 2], inbox_text));
    }
    CHECK(inbox_messages == 2);
    CHECK(soul_locks == 1);
    CHECK(persistent_traces == 0);
    CHECK(persistent_ids == 0);
    // Force the real pre-request pressure path after both temporary hubs died.
    api::Message large;
    large.role = api::Role::User;
    std::string words;
    for (int index = 0; index != 50000; ++index) words += "a ";
    large.content.push_back(api::TextBlock{std::move(words)});
    fixture.agent->ReplaceHistory({large});
    fixture.agent->SetContextWindowTokens(32768);
    fixture.agent->AdoptSessionSoul("default", "", false);
    fixture.AddRound("after-scope");
    pending_inbox = true;
    inbox_text = "INBOX_AFTER_SCOPE";
    const int pressure_before = pressure_calls;
    const auto after = fixture.agent->Run("after scope", wiring);
    REQUIRE(after.has_value());
    CHECK_FALSE(after->cancelled);
    CHECK(fixture.probe->calls == 3);
    CHECK(inbox_messages == 3);
    CHECK(pressure_calls > pressure_before);
    REQUIRE(fixture.backend.requests.size() == 6);
    CHECK(scope_fixture::ContainsText(fixture.backend.requests[4],
        "Preserved pressure hook compacted the old history."));
    CHECK(soul_locks == 2);
    CHECK(persistent_ids == 1);
    CHECK(persistent_traces > 0);
}

TEST_CASE("ScopedTurnBindings: normal cancellation and exception exits restore projections and routing") {
    bool cancel = false, throw_after_turn = false;
    SUBCASE("normal return") {}
    SUBCASE("cancelled turn") { cancel = true; }
    SUBCASE("host exception after execution") { throw_after_turn = true; }
    scope_fixture::AgentFixture fixture;
    runtime::IdAuthority ids;
    runtime::ToolTraceHub hub(ids);
    scope_fixture::EventCollector events;
    DurableTarget durable;
    int permanent_projection = 0, temporary_projection = 0;
    hub.AttachSink(&events);
    hub.AttachTrajectory(&durable);
    hub.AttachProjection([&](const agent::ToolTraceEvent&) { ++permanent_projection; });
    agent::TurnWiring wiring;
    hub.Install(*fixture.agent, wiring, "permanent-thread", "permanent-turn");
    auto recorder = std::make_unique<ProjectionTarget>(ProjectionTarget{temporary_projection});
    std::atomic<bool> cancelled{false};
    fixture.backend.replies.push_back(scope_fixture::ToolReply("scope-first"));
    if (!cancel) fixture.backend.replies.push_back(scope_fixture::TextReply("scope-first-answer"));
    fixture.probe->cancel_on_call = cancel ? &cancelled : nullptr;
    bool exception_seen = false;
    try {
        runtime::ScopedTurnBindings scope(*fixture.agent);
        runtime::ScopedTurnBindings::Bindings bindings;
        bindings.hub = &hub;
        bindings.trajectory = nullptr; // Explicitly suspend the previous durable target.
        bindings.thread_id = "temporary-thread";
        bindings.turn_id = "temporary-turn";
        bindings.projection = [target = recorder.get()](const agent::ToolTraceEvent& event) { target->Record(event); };
        scope.Bind(wiring, std::move(bindings));
        const auto outcome = fixture.agent->Run("scope first", wiring, &cancelled);
        REQUIRE(outcome.has_value());
        CHECK(outcome->cancelled == cancel);
        CHECK(temporary_projection > 0);
        CHECK(permanent_projection == 0);
        CHECK(durable.calls == 0);
        if (throw_after_turn) throw std::runtime_error("fixture host exception");
    } catch (const std::runtime_error& error) {
        CHECK(std::string(error.what()) == "fixture host exception");
        exception_seen = true;
    }
    CHECK(exception_seen == throw_after_turn);
    const int temporary_before = temporary_projection;
    recorder.reset();
    hub.OnTrace(Trace("after-recorder-destruction"));
    CHECK(temporary_projection == temporary_before);
    CHECK(permanent_projection == 1);
    CHECK(durable.calls == 1);
    REQUIRE_FALSE(events.events.empty());
    CHECK(events.events.back().envelope.thread_id == "permanent-thread");
    CHECK(events.events.back().turn_id == "permanent-turn");
    // Execution facts survive Reset; binding restoration must not erase history.
    CHECK_FALSE(hub.BuildRecentLedger().executions().empty());
    fixture.probe->cancel_on_call = nullptr;
    cancelled.store(false);
    fixture.AddRound("next");
    const auto next = fixture.agent->Run("next turn", wiring, &cancelled);
    REQUIRE(next.has_value());
    CHECK_FALSE(next->cancelled);
    CHECK(fixture.probe->calls == 2);
    CHECK(temporary_projection == temporary_before);
    CHECK(permanent_projection > 1);
}

TEST_CASE("ScopedTurnBindings: partial Bind failure immediately restores live targets without test hooks") {
    scope_fixture::AgentFixture fixture;
    scope_fixture::SessionFixture session;
    runtime::IdAuthority ids;
    runtime::ToolTraceHub hub(ids);
    scope_fixture::EventCollector events;
    DurableTarget durable;
    int permanent_projection = 0, temporary_projection = 0, original_trace = 0;
    hub.AttachSink(&events);
    hub.AttachTrajectory(&durable);
    hub.AttachProjection([&](const agent::ToolTraceEvent&) { ++permanent_projection; });
    agent::TurnWiring wiring;
    hub.Install(*fixture.agent, wiring, "permanent-thread", "permanent-turn");
    wiring.on_tool_trace = [&](const agent::ToolTraceEvent&) { ++original_trace; };
    auto bridge = session.session->trajectory()->NewTurnBridge({"fixture", "responses", "test"});
    REQUIRE(bridge != nullptr);
    auto recorder = std::make_unique<ProjectionTarget>(ProjectionTarget{temporary_projection});
    runtime::ScopedTurnBindings scope(*fixture.agent);
    auto armed = std::make_shared<bool>(false);
    auto changing = fixture.agent->wiring();
    changing.on_context_pressure = ThrowOnCopy(armed);
    fixture.agent->SetWiring(std::move(changing));
    *armed = true;
    runtime::ScopedTurnBindings::Bindings bindings;
    bindings.hub = &hub;
    bindings.trajectory = bridge.get();
    bindings.thread_id = "rejected-thread";
    bindings.turn_id = "rejected-turn";
    bindings.projection = [target = recorder.get()](const agent::ToolTraceEvent& event) { target->Record(event); };
    CHECK_THROWS_AS(scope.Bind(wiring, std::move(bindings)), std::runtime_error);
    // Scope is still alive here. Failure itself must have rolled back already.
    bridge.reset();
    recorder.reset();
    hub.OnTrace(Trace("after-partial-failure"));
    CHECK(permanent_projection == 1);
    CHECK(temporary_projection == 0);
    CHECK(durable.calls == 1);
    REQUIRE_FALSE(events.events.empty());
    CHECK(events.events.back().envelope.thread_id == "permanent-thread");
    CHECK(events.events.back().turn_id == "permanent-turn");
    fixture.AddRound("after-failed-bind");
    REQUIRE(fixture.agent->Run("after failure", wiring).has_value());
    CHECK(fixture.probe->calls == 1);
    CHECK(original_trace > 0);
    scope.Reset();
    scope.Reset();
    CHECK_THROWS_AS(scope.Bind(wiring, {}), std::logic_error);
}

TEST_CASE("ScopedTurnBindings: absent trajectory preserves existing owners and explicit null suspends them") {
    bool clear = false;
    SUBCASE("absent override preserves existing targets") {}
    SUBCASE("explicit null temporarily removes targets") { clear = true; }
    scope_fixture::AgentFixture fixture;
    runtime::IdAuthority ids;
    runtime::ToolTraceHub hub(ids);
    DurableTarget durable;
    BoundaryTarget boundary;
    hub.AttachTrajectory(&durable);
    agent::TurnWiring wiring;
    wiring.boundary_recorder = &boundary;
    hub.Install(*fixture.agent, wiring, "existing-thread", "existing-turn");
    fixture.AddRound("inside");
    {
        runtime::ScopedTurnBindings scope(*fixture.agent);
        runtime::ScopedTurnBindings::Bindings bindings;
        bindings.hub = &hub;
        if (clear) bindings.trajectory = nullptr;
        scope.Bind(wiring, std::move(bindings));
        REQUIRE(fixture.agent->Run("inside", wiring).has_value());
        CHECK(boundary.requests == (clear ? 0 : 2));
        CHECK((durable.calls > 0) == !clear);
    }
    const auto boundary_before = boundary.requests;
    const auto durable_before = durable.calls;
    fixture.AddRound("outside");
    REQUIRE(fixture.agent->Run("outside", wiring).has_value());
    CHECK(boundary.requests == boundary_before + 2);
    CHECK(durable.calls > durable_before);
}

TEST_CASE("ScopedTurnBindings: late capability exceptions restore routing and all earlier writes") {
    scope_fixture::AgentFixture fixture;
    runtime::IdAuthority ids;
    runtime::ToolTraceHub hub(ids);
    scope_fixture::EventCollector events;
    ThrowingCapability target;
    int permanent_projection = 0, temporary_projection = 0;
    hub.AttachSink(&events);
    hub.AttachTrajectory(&target);
    hub.AttachProjection([&](const agent::ToolTraceEvent&) { ++permanent_projection; });
    agent::TurnWiring wiring;
    hub.Install(*fixture.agent, wiring, "prior-thread", "prior-turn");
    auto recorder = std::make_unique<ProjectionTarget>(ProjectionTarget{temporary_projection});
    runtime::ScopedTurnBindings scope(*fixture.agent);
    target.armed = true;
    runtime::ScopedTurnBindings::Bindings bindings;
    bindings.hub = &hub;
    bindings.thread_id = "failed-thread";
    bindings.turn_id = "failed-turn";
    bindings.projection = [target = recorder.get()](const agent::ToolTraceEvent& event) { target->Record(event); };
    CHECK_THROWS_AS(scope.Bind(wiring, std::move(bindings)), std::runtime_error);
    recorder.reset();
    hub.OnTrace(Trace("after-late-failure"));
    CHECK(target.traces == 1);
    CHECK(permanent_projection == 1);
    CHECK(temporary_projection == 0);
    REQUIRE_FALSE(events.events.empty());
    CHECK(events.events.back().envelope.thread_id == "prior-thread");
    CHECK(events.events.back().turn_id == "prior-turn");
    fixture.AddRound("after-late-failure");
    REQUIRE(fixture.agent->Run("after late failure", wiring).has_value());
    CHECK(fixture.probe->calls == 1);
    CHECK(permanent_projection > 1);
    CHECK(temporary_projection == 0);
}

TEST_CASE("ScopedTurnBindings: failed TurnWiring snapshot leaves the caller's original wiring usable") {
    scope_fixture::AgentFixture fixture;
    runtime::IdAuthority ids;
    runtime::ToolTraceHub hub(ids);
    auto armed = std::make_shared<bool>(false);
    int trace_calls = 0, projection_calls = 0;
    hub.AttachProjection([&](const agent::ToolTraceEvent&) { ++projection_calls; });
    agent::TurnWiring wiring;
    wiring.on_tool_trace = ThrowingTraceCopy(armed, trace_calls);
    runtime::ScopedTurnBindings scope(*fixture.agent);
    *armed = true;
    runtime::ScopedTurnBindings::Bindings bindings;
    bindings.hub = &hub;
    CHECK_THROWS_AS(scope.Bind(wiring, std::move(bindings)), std::runtime_error);
    *armed = false;
    wiring.on_tool_trace(Trace("original-callback"));
    CHECK(trace_calls == 1);
    hub.OnTrace(Trace("unchanged-hub"));
    CHECK(projection_calls == 1);
    fixture.AddRound("after-snapshot-failure");
    REQUIRE(fixture.agent->Run("after snapshot failure", wiring).has_value());
    CHECK(fixture.probe->calls == 1);
    CHECK(trace_calls > 1);
}

TEST_CASE("ScopedTurnBindings: clearing a preview owner disables its old rewrite callback until restore") {
    bool bind_v2 = false;
    SUBCASE("explicit null target") {}
    SUBCASE("a non-preview legacy bridge") { bind_v2 = true; }
    scope_fixture::AgentFixture fixture;
    scope_fixture::TempDirectory directory;
    trajectory::EventScope identity;
    identity.workspace_key = "scope-000000000000";
    identity.session_id = "20260929-120000-SCOPEV2";
    identity.run_id = "main-scope-v2";
    identity.run_kind = trajectory::RunKind::MainSession;
    identity.visibility = {trajectory::Visibility::HostOnly};
    trajectory::RecorderOptions options;
    options.event_schema_version = 2;
    auto recorder = trajectory::TrajectoryRecorder::Start(directory.root / "main.jsonl",
        directory.root / "artifacts", identity, options);
    REQUIRE(recorder.has_value());
    REQUIRE(recorder->WriteRunStarted({{"run_kind", "main_session"}}, trajectory::Durability::PowerLoss)
        .status == trajectory::RecordReceipt::Status::Committed);
    runtime::TrajectoryTurnBridge legacy(*recorder, identity, {"fixture", "responses", "test"});
    legacy.BeginTurn("turn-v2", "external_user");
    runtime::IdAuthority ids;
    runtime::ToolTraceHub hub(ids);
    PreviewTarget preview;
    hub.AttachTrajectory(&preview);
    agent::TurnWiring wiring;
    hub.Install(*fixture.agent, wiring, "existing-thread", "existing-turn");
    api::Message results;
    results.role = api::Role::User;
    results.content.push_back(api::ToolResultBlock{"prior-call", "prior-result", false});
    REQUIRE(wiring.rewrite_tool_results_for_history);
    CHECK(wiring.rewrite_tool_results_for_history(results).ok());
    CHECK(preview.rewrites == 1);
    fixture.AddRound("without-preview");
    {
        runtime::ScopedTurnBindings scope(*fixture.agent);
        runtime::ScopedTurnBindings::Bindings bindings;
        bindings.hub = &hub;
        bindings.trajectory = bind_v2 ? &legacy : nullptr;
        scope.Bind(wiring, std::move(bindings));
        // A leaked rewrite lambda would dereference the now-null trajectory.
        REQUIRE(fixture.agent->Run("without preview", wiring).has_value());
        CHECK(preview.rewrites == 1);
    }
    REQUIRE(wiring.rewrite_tool_results_for_history);
    CHECK(wiring.rewrite_tool_results_for_history(results).ok());
    CHECK(preview.rewrites == 2);
    legacy.EndTurn(true, false, "");
}

TEST_CASE("ScopedTurnBindings: async default gate and planner queries never retain a destroyed bridge") {
    bool previous_binding = false;
    SUBCASE("return to an empty bridge binding") {}
    SUBCASE("restore a live previous bridge") { previous_binding = true; }
    scope_fixture::SessionFixture session;
    scope_fixture::AgentFixture fixture;
    auto* ledger = session.session->trajectory();
    auto* writer = ledger->v3_main_writer();
    REQUIRE(writer != nullptr);
    int executions = 0;
    runtime::AsyncToolRuntime::Hooks hooks;
    hooks.writer = writer;
    hooks.writer_mutex = ledger->v3_tool_results_mutex();
    hooks.auth = [](const std::string&, const nlohmann::json&) { return tools::JobAuthDecision{true, false, ""}; };
    hooks.executor = [&](const tools::JobExecutionContext&) {
        ++executions;
        return tools::Tool::Result{"must not execute", false};
    };
    runtime::AsyncToolRuntimeOptions options;
    options.provider = "fixture";
    options.wire = "responses";
    options.model = "scope-model";
    runtime::AsyncToolPolicy policy;
    policy.execution.side_effect_class = "read_only";
    policy.dispatch_point = agent::ToolDispatchPoint::OnCallItemComplete;
    options.tools["scope_probe"] = policy;
    auto async_runtime = runtime::AsyncToolRuntime::Create(std::move(hooks), std::move(options));
    REQUIRE(async_runtime != nullptr);
    std::unique_ptr<runtime::TrajectoryTurnBridge> previous;
    if (previous_binding) {
        previous = ledger->NewTurnBridge({"fixture", "responses", "test"});
        previous->BeginTurn("turn-000001", "external_user");
        async_runtime->InstallTurnBridge(previous.get());
    }
    auto temporary = ledger->NewTurnBridge({"fixture", "responses", "test"});
    temporary->BeginTurn("turn-000002", "external_user");
    agent::TurnWiring wiring;
    {
        runtime::ScopedTurnBindings scope(*fixture.agent);
        runtime::ScopedTurnBindings::Bindings bindings;
        bindings.trajectory = temporary.get();
        bindings.async_runtime = async_runtime.get();
        scope.Bind(wiring, std::move(bindings));
    }
    temporary->EndTurn(true, false, "");
    temporary.reset();
    api::ToolUseBlock call;
    call.id = "unrecorded-call";
    call.name = "scope_probe";
    call.input = nlohmann::json::object();
    CHECK_FALSE(async_runtime->gate()->OnCallItemComplete(call, {"turn-000003", "step-000003", "missing-request"}));
    agent::ToolCallAdjudication decision;
    decision.mode = agent::ToolProtocolMode::JobHandle;
    CHECK_FALSE(async_runtime->gate()->TakeJobOrder(call, decision).has_value());
    CHECK(executions == 0);
    // A real pending delivery forces both default planner bridge callbacks.
    auto* planner = dynamic_cast<runtime::ResultDeliveryPlannerImpl*>(async_runtime->planner());
    REQUIRE(planner != nullptr);
    runtime::CompletionNotice notice;
    notice.job_id = "job-scope-delivery";
    notice.action_id = "action-scope-delivery";
    notice.mode = "native_deferred";
    notice.provider_call_id = "scope-delivery-call";
    notice.turn_id = previous_binding ? "" : "turn-000001";
    notice.step_id = "step-000001";
    notice.result_ref = PersistResult(*writer);
    notice.result_version = 1;
    notice.preview = "scope-delivery-result";
    notice.attempt = 1;
    notice.terminal_kind = 1;
    notice.branch = writer->session_id();
    planner->NotifyCompletion(std::move(notice));
    const auto delivered = planner->SelectForRequestBoundary();
    REQUIRE(delivered.size() == 1);
    REQUIRE(delivered[0].content.size() == 1);
    const auto* result = std::get_if<api::ToolResultBlock>(&delivered[0].content[0]);
    REQUIRE(result != nullptr);
    CHECK(result->content == "scope-delivery-result");
    planner->NoteRequestPrepared("request-after-bridge-destruction");
    planner->NoteResponseOutcome("request-after-bridge-destruction", true);
    auto persisted = v3::ReadV3Ledger(writer->path());
    REQUIRE(persisted.has_value());
    const auto deliveries = v3::FoldDeliveries(*persisted);
    const auto* state = v3::FindDelivery(deliveries, "delivery-job-scope-delivery-v1");
    REQUIRE(state != nullptr);
    CHECK(state->state == "uncertain");
    CHECK(async_runtime->ExchangeTurnBridge(nullptr) == previous.get());
    if (previous) previous->EndTurn(true, false, "");
    previous.reset();
    CHECK_FALSE(async_runtime->gate()->OnCallItemComplete(call, {"turn-000003", "step-000003", "missing-request"}));
    CHECK(executions == 0);
}
