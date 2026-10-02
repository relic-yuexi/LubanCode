#include <doctest/doctest.h>

#include <algorithm>
#include <atomic>
#include <fstream>
#include <set>

#include "app/turn_runner.hpp"
#include "cli/context_tracker.hpp"
#include "cli/transcript.hpp"
#include "hooks/dispatcher.hpp"
#include "hooks/loader.hpp"
#include "hooks/trust.hpp"
#include "platform/paths.hpp"
#include "platform/process.hpp"
#include "runtime/async_tool_runtime.hpp"
#include "runtime/tool_trace_hub.hpp"
#include "scoped_turn_fixture.hpp"
#include "skills/workflow_recorder.hpp"
#include "tools/todo_tool.hpp"
#include "trajectory/v3/reader.hpp"

namespace {
using namespace lubancode;
namespace scope_fixture = test_support::turn_scope;

std::string Python() {
#ifdef _WIN32
    const char* command = "python";
#else
    const char* command = "python3";
#endif
    const auto located = platform::RunProcess(
        {command, "-c", "import json,sys; print(json.dumps(sys.executable))"}, 10000);
    REQUIRE_FALSE(located.spawn_failed);
    REQUIRE_FALSE(located.timed_out);
    REQUIRE(located.exit_code == 0);
    return nlohmann::json::parse(located.output).get<std::string>();
}

std::size_t CountRecorded(const std::filesystem::path& directory, const std::string& type) {
    const auto events = skills::ReadRecordingEvents(directory);
    return static_cast<std::size_t>(std::count_if(events.begin(), events.end(),
        [&](const auto& event) { return event.type == type; }));
}

class UnknownStopTool final : public tools::Tool {
public:
    int calls = 0;
    std::string name() const override { return "stop_unknown"; }
    std::string description() const override { return "Stop on an unconfirmed test side effect."; }
    nlohmann::json input_schema() const override { return {{"type", "object"}}; }
    Result execute(const nlohmann::json&) override {
        ++calls;
        Result result{"test Stop side effect cannot be confirmed", true};
        result.error_code = "test.stop_indeterminate";
        result.execution_control = tools::ExecutionControl::StopIndeterminate;
        return result;
    }
};
}  // namespace

TEST_CASE("CLI turn scope: real Stop continuation shares bindings and retires the recorder before the next turn") {
    scope_fixture::SessionFixture session;
    scope_fixture::AgentFixture fixture;
    fixture.AddRound("initial");
    fixture.AddRound("stop-continuation");
    fixture.AddRound("next");
    REQUIRE(runtime::AttachDefaultAsyncToolRuntime(*session.session, "responses"));
    auto* async_runtime = session.session->async_tool_runtime();
    REQUIRE(async_runtime != nullptr);
    auto& ids = session.session->ids();
    runtime::ToolTraceHub hub(ids);
    scope_fixture::EventCollector events;
    hub.AttachSink(&events);
    int permanent_projection = 0;
    hub.AttachProjection([&](const agent::ToolTraceEvent&) { ++permanent_projection; });
    agent::TurnWiring previous;
    hub.Install(*fixture.agent, previous, "prior-cli-thread", "prior-cli-turn");

    const auto hook_log = session.directory.root / "stop-hook-inputs.jsonl";
    hooks::HookDefinition stop;
    stop.id = 1;
    stop.event = hooks::HookEvent::Stop;
    stop.source_kind = hooks::HookSourceKind::User;
    stop.source_path = platform::PathToUtf8(session.directory.root / "hooks.json");
    stop.source_label = "scope fixture";
    stop.trusted = true;
    stop.handler.command = Python();
    stop.handler.args = {std::string(LUBANCODE_TEST_FIXTURES_DIR) + "/scoped_turn_stop_hook.py",
                         platform::PathToUtf8(hook_log)};
    stop.handler.timeout_ms = 10000;
    stop.definition_hash = hooks::ComputeDefinitionHash(stop.handler);
    hooks::LoadedHooks loaded;
    loaded.definitions.push_back(std::move(stop));
    auto [trust, trust_error] = hooks::HookTrustStore::Load(std::nullopt);
    REQUIRE_FALSE(trust_error.has_value());
    hooks::HookContext hook_context;
    hook_context.session_id = session.session->thread_id();
    hook_context.cwd = platform::PathToUtf8(session.directory.root / "project");
    hooks::HookDispatcher dispatcher;
    dispatcher.Configure(std::move(loaded), std::move(trust), std::move(hook_context));

    skills::RecordingStartInfo info;
    info.name = "scope recorder";
    info.goal = "Record both Stop rounds.";
    info.cwd = platform::PathToUtf8(session.directory.root / "project");
    auto started = skills::WorkflowRecorder::Start(session.directory.root / "recordings", info);
    REQUIRE(started.has_value());
    auto recorder = std::make_unique<skills::WorkflowRecorder>(std::move(*started));
    const auto recording_directory = recorder->dir();
    cli::ContextTracker tracker(100000);
    std::vector<cli::TranscriptItem> transcript;
    auto todo = std::make_shared<tools::TodoListState>();
    std::atomic<bool> expanded{false};
    std::set<std::string> allowed;
    const auto context = [&](const std::string& text, const std::string& turn_id,
                             runtime::TurnEventAdapter& adapter) {
        app::TurnContext ctx;
        ctx.loop = fixture.agent.get();
        ctx.registry = &fixture.registry;
        ctx.user_input = text;
        ctx.auto_confirm = true;
        ctx.always_allowed_tools = &allowed;
        ctx.context_tracker = &tracker;
        ctx.transcript = &transcript;
        ctx.todo_state = todo;
        ctx.transcript_expanded = &expanded;
        ctx.is_console = false;
        ctx.silent = true;
        ctx.trace_hub = &hub;
        ctx.thread_id_for_trace = session.session->thread_id();
        ctx.turn_id_for_trace = turn_id;
        ctx.turn_events = &adapter;
        ctx.trajectory_ledger = session.session->trajectory();
        ctx.async_tool_runtime = async_runtime;
        ctx.trajectory_provider = "fixture";
        ctx.trajectory_wire = "responses";
        ctx.model_id = "scope-model";
        return ctx;
    };
    {
        auto adapter = session.session->MakeTurnAdapter();
        adapter.Attach([&](const runtime::ServerEvent& event) { events.Emit(event); });
        auto ctx = context("initial", "cli-scope-turn", adapter);
        ctx.hook_dispatcher = &dispatcher;
        ctx.recorder = recorder.get();
        const auto outcome = app::RunTurn(ctx); // Drives the real Stop continuation path.
        CHECK(outcome.status == 0);
        CHECK_FALSE(outcome.cancelled);
    }
    REQUIRE(fixture.backend.requests.size() == 4);
    CHECK(fixture.probe->calls == 2);
    CHECK(scope_fixture::ContainsText(fixture.backend.requests[2], "SCOPED_STOP_CONTINUE"));
    CHECK(hub.FinishedEventsOfTurn("cli-scope-turn").size() == 2);
    CHECK(permanent_projection == 0);
    int turn_started = 0, turn_completed = 0;
    for (const auto& event : events.events) {
        if (event.kind == runtime::ServerEventKind::TurnStarted) {
            ++turn_started;
            CHECK(event.turn_id == "cli-scope-turn");
        }
        if (event.kind == runtime::ServerEventKind::TurnCompleted) {
            ++turn_completed;
            CHECK(event.turn_id == "cli-scope-turn");
        }
    }
    CHECK(turn_started == 1);
    CHECK(turn_completed == 1);
    std::ifstream hook_input(hook_log);
    REQUIRE(hook_input.good());
    std::vector<nlohmann::json> hook_inputs;
    for (std::string line; std::getline(hook_input, line);) hook_inputs.push_back(nlohmann::json::parse(line));
    REQUIRE(hook_inputs.size() == 2);
    CHECK_FALSE(hook_inputs[0].value("stop_hook_active", true));
    CHECK(hook_inputs[1].value("stop_hook_active", false));
    CHECK(hook_inputs[0].value("turn_id", "") == "cli-scope-turn");
    CHECK(hook_inputs[1].value("turn_id", "") == "cli-scope-turn");
    REQUIRE(recorder->Stop("done").has_value());
    CHECK(CountRecorded(recording_directory, skills::kEventToolCall) == 2);
    CHECK(CountRecorded(recording_directory, skills::kEventToolResult) == 2);
    const auto recorded_before = skills::ReadRecordingEvents(recording_directory).size();
    recorder.reset();
    agent::ToolTraceEvent followup;
    followup.kind = agent::ToolTraceEventKind::Scheduled;
    followup.execution_id = "after-recorder";
    followup.tool_use_id = "after-recorder";
    followup.tool_name = "scope_probe";
    hub.OnTrace(followup); // Would call a dangling WorkflowRecorder without scope cleanup.
    CHECK(permanent_projection == 1);
    REQUIRE_FALSE(events.events.empty());
    CHECK(events.events.back().envelope.thread_id == "prior-cli-thread");
    CHECK(events.events.back().turn_id == "prior-cli-turn");
    CHECK(async_runtime->ExchangeTurnBridge(nullptr) == nullptr);
    {
        auto adapter = session.session->MakeTurnAdapter();
        auto ctx = context("next", "cli-next-turn", adapter);
        const auto outcome = app::RunTurn(ctx);
        CHECK(outcome.status == 0);
        CHECK_FALSE(outcome.cancelled);
    }
    CHECK(fixture.probe->calls == 3);
    CHECK(permanent_projection > 1);
    CHECK(hub.FinishedEventsOfTurn("cli-scope-turn").size() == 2);
    CHECK(hub.FinishedEventsOfTurn("cli-next-turn").size() == 1);
    CHECK(skills::ReadRecordingEvents(recording_directory).size() == recorded_before);
}

TEST_CASE("CLI turn scope: unknown Stop tool fails the actual turn and prevents another model request") {
    scope_fixture::SessionFixture session;
    scope_fixture::AgentFixture fixture;
    auto unknown = std::make_unique<UnknownStopTool>();
    auto* unknown_borrow = unknown.get();
    fixture.registry.Register(std::move(unknown));
    fixture.backend.replies.push_back(scope_fixture::TextReply("settled initial response"));
    fixture.backend.replies.push_back({api::MessageStart{"stop-unknown-message", "scope-model"},
        api::ToolUseStart{0, "stop-unknown-call", "stop_unknown"},
        api::ToolUseInputDelta{0, "{}"}, api::ContentBlockDone{0},
        api::MessageDone{"tool_use", api::Usage{}}});
    // A third response must remain unused after the real unknown tool result.
    fixture.backend.replies.push_back(scope_fixture::TextReply("must not claim recovery"));
    REQUIRE(runtime::AttachDefaultAsyncToolRuntime(*session.session, "responses"));
    scope_fixture::EventCollector events;
    runtime::ToolTraceHub hub(session.session->ids());
    hub.AttachSink(&events);

    const auto hook_log = session.directory.root / "unknown-stop-inputs.jsonl";
    hooks::HookDefinition stop;
    stop.id = 1;
    stop.event = hooks::HookEvent::Stop;
    stop.source_kind = hooks::HookSourceKind::User;
    stop.source_path = platform::PathToUtf8(session.directory.root / "hooks.json");
    stop.source_label = "unknown Stop fixture";
    stop.trusted = true;
    stop.handler.command = Python();
    stop.handler.args = {std::string(LUBANCODE_TEST_FIXTURES_DIR) + "/scoped_turn_stop_hook.py",
                         platform::PathToUtf8(hook_log)};
    stop.handler.timeout_ms = 10000;
    stop.definition_hash = hooks::ComputeDefinitionHash(stop.handler);
    hooks::LoadedHooks loaded;
    loaded.definitions.push_back(std::move(stop));
    auto [trust, trust_error] = hooks::HookTrustStore::Load(std::nullopt);
    REQUIRE_FALSE(trust_error.has_value());
    hooks::HookContext hook_context;
    hook_context.session_id = session.session->thread_id();
    hook_context.cwd = platform::PathToUtf8(session.directory.root / "project");
    hooks::HookDispatcher dispatcher;
    dispatcher.Configure(std::move(loaded), std::move(trust), std::move(hook_context));
    cli::ContextTracker tracker(100000);
    std::vector<cli::TranscriptItem> transcript;
    std::atomic<bool> expanded{false};
    std::set<std::string> allowed;
    runtime::TurnView view;
    auto adapter = session.session->MakeTurnAdapter();
    adapter.Attach([&](const runtime::ServerEvent& event) { events.Emit(event); });
    app::TurnContext ctx;
    ctx.loop = fixture.agent.get();
    ctx.registry = &fixture.registry;
    ctx.user_input = "initial input";
    ctx.auto_confirm = true;
    ctx.always_allowed_tools = &allowed;
    ctx.context_tracker = &tracker;
    ctx.transcript = &transcript;
    ctx.transcript_expanded = &expanded;
    ctx.is_console = false;
    ctx.silent = true;
    ctx.trace_hub = &hub;
    ctx.thread_id_for_trace = session.session->thread_id();
    ctx.turn_id_for_trace = "cli-unknown-stop-turn";
    ctx.turn_events = &adapter;
    ctx.turn_view_out = &view;
    ctx.trajectory_ledger = session.session->trajectory();
    ctx.async_tool_runtime = session.session->async_tool_runtime();
    ctx.trajectory_provider = "fixture";
    ctx.trajectory_wire = "responses";
    ctx.model_id = "scope-model";
    ctx.hook_dispatcher = &dispatcher;
    const auto outcome = app::RunTurn(ctx);
    CHECK(outcome.status == 1);
    CHECK_FALSE(outcome.cancelled);
    CHECK(fixture.backend.requests.size() == 2);
    CHECK(unknown_borrow->calls == 1);
    CHECK(fixture.probe->calls == 0);
    CHECK(view.status == runtime::TurnItemViewState::Failed);
    CHECK_FALSE(session.session->trajectory()->OpenMainTurnId().has_value());
    std::size_t completed = 0;
    for (const auto& event : events.events) {
        if (event.kind != runtime::ServerEventKind::TurnCompleted) continue;
        ++completed;
        CHECK(event.turn_id == "cli-unknown-stop-turn");
        REQUIRE(event.outcome.has_value());
        CHECK(*event.outcome == runtime::Outcome::Failed);
        CHECK(event.payload.at("error").get<std::string>().find("test.stop_indeterminate") != std::string::npos);
    }
    CHECK(completed == 1);
    std::ifstream hook_input(hook_log);
    REQUIRE(hook_input.is_open());
    std::vector<nlohmann::json> hook_inputs;
    for (std::string line; std::getline(hook_input, line);) hook_inputs.push_back(nlohmann::json::parse(line));
    REQUIRE_FALSE(hook_input.bad());
    CHECK(hook_inputs.size() == 1); // Unknown Stop never emits the follow-up Stop hook.
    auto* writer = session.session->trajectory()->v3_main_writer();
    REQUIRE(writer != nullptr);
    const auto source = trajectory::v3::ReadV3Ledger(writer->path());
    REQUIRE(source.has_value());
    CHECK(std::count_if(source->events.begin(), source->events.end(), [](const auto& event) {
        return event.kind == trajectory::v3::EventKindV3::ModelRequestPrepared;
    }) == 2);
}
