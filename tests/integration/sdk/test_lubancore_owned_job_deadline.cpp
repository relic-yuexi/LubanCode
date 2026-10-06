#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdio>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <future>
#include <iterator>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>
#include "agent/loop.hpp"
#include "api/types.hpp"
#include "hooks/hash.hpp"
#include "platform/paths.hpp"
#include "runtime/interaction.hpp"
#include "runtime/session_service.hpp"
#include "runtime/trajectory_session.hpp"
#include "runtime/turn_runtime.hpp"
#include "sdk/job_operations.hpp"
#include "sdk/operation_ledger.hpp"
#include "tool_semantics.hpp"
#include "tools/registry.hpp"
#include "tools/run_command.hpp"
#include "tools/tool.hpp"
#include "tools/tool_job_coordinator.hpp"
#include "trajectory/canonical_json.hpp"
#include "trajectory/v3/envelope.hpp"
#include "trajectory/v3/hooks.hpp"
#include "trajectory/v3/reader.hpp"
#include "trajectory/v3/result_store.hpp"
#include "trajectory/v3/tool_action.hpp"
#include "trajectory/v3/writer.hpp"
#include "workspace/identity.hpp"

namespace {
namespace fs = std::filesystem;
namespace v3 = lubancode::trajectory::v3;
namespace rt = lubancode::runtime;
namespace sdk = lubancore::detail;
namespace agent = lubancode::agent;
using namespace lubancode::tools;
using Json = nlohmann::json;
using Clock = std::chrono::steady_clock;
using namespace std::chrono_literals;

struct Directory {
    fs::path root;
    Directory() {
        static std::atomic<unsigned> sequence{0};
        for (unsigned retry = 0; retry < 128; ++retry) {
            auto candidate = fs::temp_directory_path() / ("sdk-owned-deadline-" +
                std::to_string(Clock::now().time_since_epoch().count()) + "-" + std::to_string(++sequence));
            std::error_code error;
            if (fs::create_directory(candidate, error)) { root = fs::canonical(candidate); break; }
            const bool acceptable = !error || error == std::errc::file_exists;
            REQUIRE_MESSAGE(acceptable, error.message());
        }
        REQUIRE_FALSE(root.empty()); REQUIRE(fs::create_directory(root / "project"));
    }
    ~Directory() { std::error_code error; fs::remove_all(root, error); }
};
std::string Utf8(const fs::path& path) { return lubancode::platform::PathToUtf8(path); }
std::string Bytes(const fs::path& path) {
    std::ifstream stream(path, std::ios::binary); REQUIRE(stream.is_open());
    std::string value((std::istreambuf_iterator<char>(stream)), {}); REQUIRE_FALSE(stream.bad()); return value;
}
void Write(const fs::path& path, const std::string& value) {
    std::ofstream stream(path, std::ios::binary | std::ios::trunc); REQUIRE(stream.is_open());
    stream.write(value.data(), static_cast<std::streamsize>(value.size())); stream.close(); REQUIRE_FALSE(stream.fail());
}
v3::V3Ledger Ledger(const fs::path& path) {
    auto result = v3::ReadV3Ledger(path);
    REQUIRE_MESSAGE(result.has_value(), (result ? std::string() : result.error())); return std::move(*result);
}
void Committed(const v3::WriteReceipt& receipt) {
    REQUIRE_MESSAGE(receipt.status == v3::WriteReceipt::Status::Committed, receipt.error_message);
    REQUIRE_FALSE(receipt.id.empty()); REQUIRE(receipt.seq > 0); REQUIRE(v3::IsHex64(receipt.line_hash));
}
std::string Hash(const Json& value) {
    const auto text = lubancode::trajectory::CanonicalJsonDump(value); REQUIRE(text);
    return lubancode::hooks::Sha256Hex(*text);
}
std::string Quote(const std::string& value) {
#ifdef _WIN32
    REQUIRE(value.find_first_of("\"%\r\n") == std::string::npos); return "\"" + value + "\"";
#else
    std::string quoted = "'";
    for (char ch : value) { if (ch == '\'') quoted += "'\\''"; else quoted += ch; }
    return quoted + "'";
#endif
}
struct Probe {
    fs::path executable;
    explicit Probe(const fs::path& root) {
        const auto original = fs::u8path(LUBANCORE_TEST_COMMAND_LIMITS_PROBE);
        REQUIRE(fs::is_regular_file(original)); executable = root / original.filename();
        REQUIRE(fs::copy_file(original, executable)); fs::permissions(executable, fs::status(original).permissions());
    }
    Json Input(const fs::path& cwd, const std::string& tag, bool wait = false, std::uint64_t delay = 0) const {
        std::string command = Quote(Utf8(executable));
        const std::array<std::string, 7> argv = {tag + ".started", tag + ".done", "32",
            std::to_string(delay), tag, "A", wait ? tag + ".release" : "-"};
        for (const auto& argument : argv) command += " " + Quote(argument);
#ifdef _WIN32
        return {{"command", command}, {"shell", "cmd"}, {"cwd", Utf8(cwd)}};
#else
        return {{"command", command}, {"shell", "sh"}, {"cwd", Utf8(cwd)}};
#endif
    }
    void Released() { REQUIRE(fs::remove(executable)); CHECK_FALSE(fs::exists(executable)); }
};
// Wait for the complete actual probe payload, not merely file creation.
bool AwaitStarted(const fs::path& cwd, const std::string& tag) {
    const auto until = Clock::now() + 10s;
    while (Clock::now() < until) {
        std::ifstream stream(cwd / (tag + ".started"), std::ios::binary);
        if (stream.is_open()) {
            const std::string value((std::istreambuf_iterator<char>(stream)), {});
            const auto newline = value.find('\n');
            if (!stream.bad() && newline != std::string::npos && value.substr(0, newline) == tag &&
                newline + 1 < value.size() && value.find('\0') == std::string::npos) {
                const auto actual = fs::u8path(value.substr(newline + 1)); std::error_code error;
                if (actual.is_absolute() && fs::equivalent(actual, cwd, error) && !error) return true;
            }
        }
        std::this_thread::sleep_for(5ms);
    }
    return false;
}
struct Trace {
    std::mutex mutex;
    unsigned command_calls = 0, scope_calls = 0, posts = 0;
    bool scope_valid = true;
    std::optional<CommandExecutionLimits> limits;
    std::optional<Tool::Result> raw;
};
class Command final : public RunCommandTool {
public:
    explicit Command(std::shared_ptr<Trace> trace) : trace_(std::move(trace)) {}
    Result execute(const Json& input, const ToolExecutionContext& context) override {
        { std::lock_guard lock(trace_->mutex); ++trace_->command_calls; trace_->limits = context.command_limits; }
        auto result = RunCommandTool::execute(input, context);
        { std::lock_guard lock(trace_->mutex); trace_->raw = result; }
        return result;
    }
private:
    std::shared_ptr<Trace> trace_;
};
struct Gate {
    std::mutex mutex; std::condition_variable cv; bool entered = false, released = false;
    void Enter() { std::unique_lock lock(mutex); entered = true; cv.notify_all(); cv.wait(lock, [&] { return released; }); }
    bool Await() { std::unique_lock lock(mutex); return cv.wait_for(lock, 10s, [&] { return entered; }); }
    void Release() { { std::lock_guard lock(mutex); released = true; } cv.notify_all(); }
};
// Declare after a Rig so assertions release a gated real thread before Rig destruction joins it.
struct ReleaseOnExit {
    std::shared_ptr<Gate> gate;
    ~ReleaseOnExit() { gate->Release(); }
};
struct Ticket {
    lubancode::api::ToolUseBlock call;
    std::string step, action;
    std::optional<v3::ToolActionSession> parent;
    PreparedJobRequest request;
    PreparedJobRegistration registered;
    OwnedJobAdoption adopted;
    ParentJobAdmissionRefs parent_refs;
    Clock::time_point before_register, after_register;
    std::shared_ptr<Trace> trace = std::make_shared<Trace>();
};
struct Rig {
    fs::path cwd;
    std::unique_ptr<rt::SessionService> service;
    std::shared_ptr<std::recursive_mutex> serial = std::make_shared<std::recursive_mutex>();
    std::shared_ptr<GlobalRunningQuota> quota = std::make_shared<GlobalRunningQuota>();
    std::shared_ptr<ToolJobCoordinator> coordinator;
    sdk::JobOperations table;
    sdk::MainOperationTurnStart main;
    std::atomic<unsigned> legacy{0}, threads{0};
    unsigned sequence = 0;
    explicit Rig(const Directory& directory, ToolJobCoordinator::ThreadStarter starter = {})
        : cwd(directory.root / "project") {
        rt::SessionLaunchRequest launch;
        launch.cwd_utf8 = Utf8(cwd); launch.workspace_identity = lubancode::workspace::MakeFallbackIdentity(cwd);
        launch.workspaces_root = directory.root / "data"; launch.lubancode_version = "deadline-fixture";
        launch.wire_name = "chat"; launch.v3_system_content = "owned registration deadline";
        service = std::make_unique<rt::SessionService>(std::move(launch));
        REQUIRE_MESSAGE(service->runtime() != nullptr, service->launch_error()); REQUIRE(service->v3_format());
        const auto input = service->SubmitInput({"deadline-parent", "owned command", {}}); REQUIRE(input.accepted);
        main = sdk::BeginMainOperationTurn(*service); REQUIRE(main.ready()); REQUIRE(main.facts);
        ToolJobCoordinator::Options options;
        options.prepared_registration = PreparedRegistrationContext{serial, "deadline-project", cwd};
        options.global = quota; options.limits.session_running = 1; options.limits.per_tool = 1;
        options.clock_ms = [this] { ++legacy; return std::int64_t{1}; };
        options.thread_starter = [this, starter = std::move(starter)](std::thread& thread, std::function<void()> body) {
            ++threads; if (starter) starter(thread, std::move(body)); else thread = std::thread(std::move(body));
        };
        coordinator = std::make_shared<ToolJobCoordinator>(Writer(),
            [this](const auto&, const auto&) { ++legacy; return JobAuthDecision{true, false, {}}; },
            [this](const auto&) { ++legacy; return Tool::Result::Text("unexpected legacy"); }, options);
    }
    ~Rig() { (void)table.Close(); coordinator.reset(); if (service) (void)service->Close("fixture_exit"); }
    v3::V3Writer& Writer() { return *service->trajectory()->v3_main_writer(); }
    fs::path Path() { return Writer().path(); }
    fs::path Dir() { return service->trajectory()->session_dir(); }
    Ticket Declare(Json input, std::uint64_t budget) {
        Ticket ticket;
        ticket.call = {"deadline-call-" + std::to_string(++sequence), "run_command", std::move(input)};
        ticket.step = Writer().NewStepId(); ticket.action = Writer().NewActionId();
        v3::MessageDraft message;
        message.turn_id = main.turn_id; message.step_id = ticket.step; message.request_id = Writer().NewRequestId();
        message.origin = v3::MessageOrigin::SessionRuntime; message.provider = "fixture"; message.wire = "chat";
        message.model = "fixture"; message.response_model = "fixture"; message.usage = nullptr;
        message.message = {{"role", "assistant"}, {"content", "actual declaration"}, {"tool_calls", Json::array({
            {{"id", ticket.call.id}, {"type", "function"},
             {"function", {{"name", ticket.call.name}, {"arguments", ticket.call.input.dump()}}}}})}};
        const auto written = Writer().AppendMessage(std::move(message), v3::Durability::PowerLoss); Committed(written);
        Committed(Writer().AdmitMessages({written.id}, v3::Durability::PowerLoss));
        ticket.parent = v3::ToolActionSession::Admit(Writer(), main.turn_id, ticket.step, ticket.action,
            "prepared-parent", written.id, ticket.call.id, Json::object(), v3::Durability::PowerLoss);
        REQUIRE(ticket.parent->last_event_id());
        ToolRegistry registry; ToolRegistration tool;
        tool.tool = std::make_unique<RunCommandTool>(); tool.source_kind = lubancode::ToolSourceKind::Builtin;
        tool.source_instance = "deadline-command"; tool.version_or_digest = "native-v1";
        tool.effect_class = lubancode::EffectClass::LocalProcessUnknown; registry.Register(std::move(tool));
        agent::TurnWiring wiring;
        wiring.on_pre_tool_use_hook = [](const auto&, const auto&, const auto&) {
            rt::ToolHookDecision result; result.decision = rt::ToolHookDecision::Decision::Allow; return result;
        };
        wiring.on_permission_evaluate = [](const auto&, const auto&, auto, const auto&, const auto&) {
            rt::PermissionVerdict result; result.action = rt::PermissionVerdict::Action::Allow; return result;
        };
        const auto prepared = agent::PrepareOwnedToolInput(registry, ticket.call, wiring, {});
        REQUIRE_MESSAGE(prepared.has_value(), (prepared ? std::string() : prepared.error().content));
        const auto owner = coordinator->PreparedOwner(); REQUIRE(owner);
        auto& request = ticket.request;
        request.owner = *owner; request.provider_tool_call_id = ticket.call.id; request.assistant_message_ref = written.id;
        request.parent_action_id = ticket.action; request.turn_id = main.turn_id; request.step_id = ticket.step;
        request.tool_name = ticket.call.name; request.original_input = ticket.call.input; request.effective_input = prepared->effective_input;
        request.tool_identity = {ticket.call.name, prepared->source_instance, "native-v1", Utf8(cwd)};
        request.policy.allow_background = true; request.policy.side_effect_class = "external";
        request.policy.resource_keys = {"deadline-command"}; request.policy.deadline_ms = budget;
        return ticket;
    }
    void Register(Ticket& ticket) {
        ticket.before_register = Clock::now();
        ticket.registered = coordinator->RegisterPreparedJob(ticket.request); ticket.after_register = Clock::now();
        REQUIRE_MESSAGE(ticket.registered.state == PreparedJobRegistrationState::Registered, ticket.registered.error);
        REQUIRE(ticket.registered.facts); Committed(*ticket.registered.facts->registered_receipt);
    }
    void Adopt(Ticket& ticket, std::uint64_t cap, std::function<void(unsigned)> scope = {}) {
        const auto facts = ticket.registered.facts; const auto trace = ticket.trace;
        OwnedJobCapability capability;
        capability.command = std::make_shared<Command>(trace); capability.command_limits = {cap, 1024};
        capability.scope_gate = [facts, trace, scope = std::move(scope)](const OwnedJobScope& actual, const Json& input,
            const v3::ToolIdentity& identity, const JobExecutionPolicy& policy) {
            unsigned call = 0;
            const bool valid = actual.owner == facts->owner && actual.job_id == facts->job_id &&
                actual.action_id == facts->action_id && actual.attempt == 1 && actual.turn_id == facts->turn_id &&
                actual.step_id == facts->step_id && actual.parent_action_id == facts->parent_action_id &&
                actual.provider_tool_call_id == facts->provider_tool_call_id && input == facts->effective_input &&
                identity.ToJson() == facts->tool_identity.ToJson() && policy.ToJson() == facts->policy.ToJson();
            { std::lock_guard lock(trace->mutex); call = ++trace->scope_calls; trace->scope_valid &= valid; }
            if (scope) scope(call);
            return JobAuthDecision{valid, false, valid ? std::string() : "fixture_scope_mismatch"};
        };
        capability.post = [this, trace](const OwnedJobCompletion& completion) {
            { std::lock_guard lock(trace->mutex); ++trace->posts; }
            const v3::HookHandlerSpec handler{"deadline-post", lubancode::hooks::Sha256Hex("deadline-post-v1"), "builtin", 0, "block"};
            const auto id = Writer().NewHookDispatchId();
            auto dispatch = v3::HookDispatchSession::Dispatch(Writer(), id, "PostAction", completion.scope.turn_id,
                completion.scope.step_id, completion.scope.action_id, {handler},
                Json{{"executionEventRef", completion.terminal_receipt.id}, {"resultEventRef", completion.persisted_receipt.id}});
            Committed(dispatch.BeginInvocation(Writer(), "invocation-" + id, handler));
            return dispatch.CompleteInvocation(Writer(), "allow", std::nullopt, 0, v3::Durability::PowerLoss);
        };
        ticket.adopted = coordinator->AdoptPreparedJob(facts->owner, facts->job_id, std::move(capability));
        REQUIRE_MESSAGE(ticket.adopted.state == OwnedJobAdoptionState::Adopted, ticket.adopted.error);
        Committed(*ticket.adopted.receipt);
        const auto bound = table.Bind(*service, *coordinator, facts->job_id);
        REQUIRE_MESSAGE(bound.bound(), bound.error.code); REQUIRE(bound.receipt); Committed(*bound.receipt);
    }
    void Confirm(Ticket& ticket) {
        auto& parent = *ticket.parent; auto& refs = ticket.parent_refs;
        Committed(parent.Start(Writer(), "args-parent-" + Hash(ticket.call.input), ticket.adopted.facts->tool_identity,
            std::nullopt, Json{{"toolName", "run_command"}}, v3::Durability::PowerLoss));
        const auto terminal = parent.Finish(Writer(), std::nullopt, 0); Committed(terminal); refs.terminal_event_id = terminal.id;
        auto store = v3::ResultStore::Open(Path().parent_path()); REQUIRE(store);
        v3::ResultStore::PersistRequest material;
        material.result_kind = "text"; material.tool_call_id = ticket.action; material.attempt = 1;
        material.execution_event_ref = terminal.id;
        v3::ResultStore::ChannelOutput output; output.channel = "combined"; output.data = ticket.adopted.admission_content;
        output.output_bytes = output.data.size(); material.outputs.push_back(std::move(output));
        material.capture_limits = {{"max_output_bytes", 65536}};
        material.preview_policy = {{"policy", "deadline-parent"}, {"maxPreviewBytes", 32768}};
        const auto saved = store->Persist(material); REQUIRE_MESSAGE(saved.ok, saved.error);
        const auto persisted = parent.PersistedResult(Writer(), saved.result_ref, terminal.id, 1); Committed(persisted);
        refs.persisted_event_id = persisted.id;
        const auto selected = parent.SelectResult(Writer(), {persisted.id}, {}, "done", 1); Committed(selected);
        refs.selected_event_id = selected.id;
        v3::MessageDraft message;
        message.turn_id = main.turn_id; message.step_id = ticket.step; message.action_id = ticket.action;
        message.origin = v3::MessageOrigin::SessionRuntime; message.purpose = v3::MessagePurpose::Conversation;
        message.result_selection_ref = selected.id;
        message.message = {{"role", "tool"}, {"tool_call_id", ticket.action}, {"content", ticket.adopted.admission_content}};
        const auto emitted = Writer().AppendMessage(std::move(message), v3::Durability::PowerLoss); Committed(emitted);
        refs.tool_message_id = emitted.id;
        const auto admitted = Writer().AdmitMessages({emitted.id}, v3::Durability::PowerLoss); Committed(admitted);
        refs.admission_event_id = admitted.id;
        const auto confirmed = coordinator->ConfirmParentAdmission(ticket.request.owner, ticket.registered.facts->job_id, refs);
        REQUIRE_MESSAGE(confirmed.confirmed, confirmed.error);
    }
    OwnedJobStatusView Finish(const Ticket& ticket) {
        const auto result = coordinator->WaitOwnedJobs(ticket.request.owner, {ticket.registered.facts->job_id}, 20000, true);
        REQUIRE(result.satisfied); CHECK_FALSE(result.timed_out); REQUIRE(result.statuses.size() == 1);
        return result.statuses.front();
    }
    void Cancel(const Ticket& ticket) {
        REQUIRE(coordinator->CancelOwnedJob(ticket.request.owner, ticket.registered.facts->job_id, "fixture_cancel").ok);
    }
    void Stop() {
        REQUIRE(table.Close()); REQUIRE(coordinator->Shutdown()); REQUIRE(coordinator->shutdown_complete());
        CHECK(quota->running.load() == 0); CHECK(legacy.load() == 0);
    }
};
void Expire(const Ticket& ticket) {
    REQUIRE(ticket.request.policy.deadline_ms > 0);
    std::this_thread::sleep_until(ticket.after_register + std::chrono::milliseconds(ticket.request.policy.deadline_ms) + 25ms);
}
JobRecoveryPlan::Item Recovery(Rig& rig, const Ticket& ticket) {
    const auto plan = ToolJobCoordinator::PlanRecovery(Ledger(rig.Path()), JobRecoveryPolicy::Hold);
    const auto found = std::find_if(plan.items.begin(), plan.items.end(), [&](const auto& item) {
        return item.job_id == ticket.registered.facts->job_id;
    }); REQUIRE(found != plan.items.end()); REQUIRE(found->recovery); return *found;
}
void NotInvoked(Rig& rig, const Ticket& ticket, bool started) {
    const auto view = rig.coordinator->SnapshotOwnedJob(ticket.request.owner, ticket.registered.facts->job_id);
    CHECK(view.state == "cancelled"); CHECK(view.command_not_invoked); CHECK(view.started_receipt.has_value() == started);
    REQUIRE(view.terminal_receipt); Committed(*view.terminal_receipt); REQUIRE(view.observed_receipt); Committed(*view.observed_receipt);
    CHECK_FALSE(view.persisted_receipt); CHECK_FALSE(view.post_receipt);
    { std::lock_guard lock(ticket.trace->mutex); CHECK(ticket.trace->command_calls == 0); CHECK(ticket.trace->posts == 0); CHECK_FALSE(ticket.trace->raw); }
    const auto ledger = Ledger(rig.Path());
    const auto* terminal = ledger.FindEvent(view.terminal_receipt->id); REQUIRE(terminal);
    CHECK(terminal->payload.at("phase") == (started ? "during_execution" : "before_started"));
    CHECK(terminal->payload.at("reason") == "registration_deadline_elapsed");
    const auto* observed = ledger.FindEvent(view.observed_receipt->id); REQUIRE(observed);
    CHECK(observed->payload.contains("commandNotInvoked") == started); REQUIRE(observed->payload.contains("parentAdmission"));
    const auto recovery = Recovery(rig, ticket);
    CHECK(recovery.admission_complete); CHECK(recovery.recovery->admission_complete);
    CHECK(recovery.recovery->command_not_invoked); CHECK(recovery.recovery->knowledge == JobRecoveryKnowledge::TerminalConfirmed);
    const auto held = sdk::ReadJobOperations(rig.Dir(), ledger); CHECK(held.state == sdk::JobOperationRecoveryState::PassiveHold);
}
void Mark(const char* path, Rig& rig, const Ticket& ticket) {
    REQUIRE(rig.coordinator->shutdown_complete()); REQUIRE(rig.quota->running.load() == 0);
    const auto view = rig.coordinator->SnapshotOwnedJob(ticket.request.owner, ticket.registered.facts->job_id);
    REQUIRE(v3::ReadOwnedJobAdoptions(Ledger(rig.Path())));
    std::lock_guard lock(ticket.trace->mutex); REQUIRE(ticket.trace->scope_valid);
    Json timeout = nullptr;
    if (ticket.trace->command_calls) { REQUIRE(ticket.trace->limits); timeout = ticket.trace->limits->timeout_ms; REQUIRE(timeout.get<std::uint64_t>() > 0); }
    const Json fact = {{"path", path}, {"quiescent", true}, {"global_running", rig.quota->running.load()},
        {"started_intent", view.started_receipt.has_value()}, {"command_not_invoked", view.command_not_invoked},
        {"command_calls", ticket.trace->command_calls}, {"effective_timeout_ms", timeout}};
    std::fprintf(stderr, "[owned-job-deadline-fact] %s\n[owned-job-deadline-path] %s\n", fact.dump().c_str(), path);
    std::fflush(stderr);
}

std::vector<Json> Rows(const std::string& bytes) {
    std::vector<Json> rows;
    for (std::size_t offset = 0; offset < bytes.size();) {
        const auto end = bytes.find('\n', offset); REQUIRE(end != std::string::npos);
        rows.push_back(Json::parse(bytes.substr(offset, end - offset))); offset = end + 1;
    }
    return rows;
}
std::string Rehash(const std::vector<Json>& rows) {
    std::string bytes, previous(v3::kGenesisHash);
    for (auto row : rows) {
        row.erase("prevHash"); row.erase("lineHash");
        const auto canonical = lubancode::trajectory::CanonicalJsonDump(row); REQUIRE(canonical);
        const auto hash = v3::ComputeLineHash(previous, *canonical);
        row["prevHash"] = previous; row["lineHash"] = hash; previous = hash;
        const auto complete = lubancode::trajectory::CanonicalJsonDump(row); REQUIRE(complete); bytes += *complete + "\n";
    }
    return bytes;
}
} // namespace

TEST_CASE("Owned registration budget expires before adoption and while queued" * doctest::test_suite("sdk-owned-job-deadline")) {
    Directory directory; Probe probe(directory.root); Rig rig(directory);
    auto impossible = rig.Declare(probe.Input(rig.cwd, "overflow"), (std::numeric_limits<std::uint64_t>::max)());
    const auto before = Bytes(rig.Path()); const auto refused = rig.coordinator->RegisterPreparedJob(impossible.request);
    CHECK(refused.state != PreparedJobRegistrationState::Registered); CHECK(refused.error_code == "job.prepared.deadline_unrepresentable");
    CHECK(Bytes(rig.Path()) == before); CHECK_FALSE(fs::exists(rig.cwd / "overflow.started"));
    // This span fits the clock duration but adding the current positive epoch cannot fit.
    REQUIRE(Clock::now().time_since_epoch().count() > 0);
    impossible.request.policy.deadline_ms = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(Clock::duration::max()).count());
    const auto addition = rig.coordinator->RegisterPreparedJob(impossible.request);
    CHECK(addition.error_code == "job.prepared.deadline_unrepresentable"); CHECK(Bytes(rig.Path()) == before);
    auto early = rig.Declare(probe.Input(rig.cwd, "expired"), 1000); rig.Register(early); Expire(early);
    rig.Adopt(early, 1000); rig.Confirm(early); NotInvoked(rig, early, false);
    CHECK_FALSE(fs::exists(rig.cwd / "expired.started"));
    const auto early_view = rig.coordinator->SnapshotOwnedJob(early.request.owner, early.registered.facts->job_id);
    REQUIRE_FALSE(early_view.dispatched_receipt); REQUIRE(early_view.observed_receipt);
    for (unsigned variant = 0; variant < 3; ++variant) {
        auto corrupt = Ledger(rig.Path());
        auto found = std::find_if(corrupt.events.begin(), corrupt.events.end(), [&](const auto& event) {
            return event.event_id == early_view.observed_receipt->id;
        }); REQUIRE(found != corrupt.events.end());
        if (variant == 0) found->payload.erase("parentAdmission");
        if (variant == 1) found->payload["parentAdmission"]["admissionEventRef"]["hash"] = std::string(64, 'f');
        if (variant == 2) found->payload["parentAdmission"]["admissionEventRef"]["seq"] = found->seq;
        CHECK_FALSE(v3::ReadOwnedJobAdoptions(corrupt));
        CHECK(sdk::ReadJobOperations(rig.Dir(), corrupt).state == sdk::JobOperationRecoveryState::Rejected);
    }

    // Zero keeps its old registration semantics; a real process owns the single slot.
    auto blocker = rig.Declare(probe.Input(rig.cwd, "blocker", true), 0);
    rig.Register(blocker); rig.Adopt(blocker, 15000); rig.Confirm(blocker); REQUIRE(AwaitStarted(rig.cwd, "blocker"));
    // Prove the queue itself with an actual unlimited-registration ticket. The
    // original positive ticket below can expire during its real native delivery.
    auto unlimited = rig.Declare(probe.Input(rig.cwd, "unlimited-queue"), 0);
    rig.Register(unlimited); rig.Adopt(unlimited, 15000); rig.Confirm(unlimited);
    const auto unlimited_queued = rig.coordinator->SnapshotOwnedJob(unlimited.request.owner, unlimited.registered.facts->job_id);
    REQUIRE(unlimited_queued.state == "queued"); REQUIRE_FALSE(unlimited_queued.started_receipt);
    REQUIRE(rig.quota->running.load() == 1); CHECK_FALSE(fs::exists(rig.cwd / "unlimited-queue.started"));
    rig.Cancel(unlimited); const auto unlimited_closed = rig.Finish(unlimited);
    REQUIRE(unlimited_closed.state == "cancelled"); REQUIRE_FALSE(unlimited_closed.started_receipt);
    { std::lock_guard lock(unlimited.trace->mutex); REQUIRE(unlimited.trace->command_calls == 0); }
    auto queued = rig.Declare(probe.Input(rig.cwd, "queued"), 2000);
    rig.Register(queued); rig.Adopt(queued, 2000); rig.Confirm(queued);
    const auto queued_view = rig.coordinator->SnapshotOwnedJob(queued.request.owner, queued.registered.facts->job_id);
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - queued.before_register).count();
    REQUIRE(queued_view.state == "queued" || queued_view.state == "cancelled");
    REQUIRE(elapsed >= 0);
    // Register begins before FreezeOwnedDeadline. The production floor treats
    // the final sub-millisecond as expired; an earlier cancellation is an error.
    if (queued_view.state == "cancelled") REQUIRE(elapsed >= 1999);
    const Json observation = {{"unlimited_budget_ms", unlimited.request.policy.deadline_ms},
        {"unlimited_queued", unlimited_queued.state == "queued"}, {"unlimited_started", unlimited_queued.started_receipt.has_value()},
        {"unlimited_final_state", unlimited_closed.state}, {"registration_budget_ms", queued.request.policy.deadline_ms},
        {"immediate_state", queued_view.state}, {"elapsed_before_register_ms", elapsed}, {"global_running", rig.quota->running.load()}};
    std::fprintf(stderr, "[owned-job-deadline-queue-observation] %s\n", observation.dump().c_str()); std::fflush(stderr);
    Expire(queued); rig.coordinator->PumpOwnedJobs(); NotInvoked(rig, queued, false);
    CHECK_FALSE(fs::exists(rig.cwd / "queued.started")); CHECK(rig.quota->running.load() == 1);
    rig.Cancel(blocker); const auto cancelled = rig.Finish(blocker); REQUIRE(cancelled.worker_finished);
    rig.Stop(); probe.Released(); Mark("registered-queue", rig, queued);
}

TEST_CASE("Owned authorization time consumes the registration budget" * doctest::test_suite("sdk-owned-job-deadline")) {
    Directory directory; Probe probe(directory.root); Rig rig(directory);
    auto target = rig.Declare(probe.Input(rig.cwd, "authorization"), 3000); rig.Register(target);
    std::atomic<bool> waited{false};
    rig.Adopt(target, 3000, [&](unsigned call) { if (call == 3) { waited = true; Expire(target); } });
    rig.Confirm(target); REQUIRE(waited.load()); NotInvoked(rig, target, false);
    CHECK(rig.threads.load() == 0); CHECK_FALSE(fs::exists(rig.cwd / "authorization.started"));
    rig.Stop(); probe.Released(); Mark("authorization", rig, target);
}

TEST_CASE("Owned actual command image takes remaining model and host budgets" * doctest::test_suite("sdk-owned-job-deadline")) {
    Directory directory; Probe probe(directory.root); Rig rig(directory);
    auto blocker = rig.Declare(probe.Input(rig.cwd, "budget-blocker", true), 0);
    rig.Register(blocker); rig.Adopt(blocker, 15000); rig.Confirm(blocker); REQUIRE(AwaitStarted(rig.cwd, "budget-blocker"));
    auto queued = rig.Declare(probe.Input(rig.cwd, "remaining", false, 60000), 5000);
    rig.Register(queued); rig.Adopt(queued, 5000); rig.Confirm(queued);
    std::this_thread::sleep_until(queued.after_register + 1500ms);
    rig.Cancel(blocker); rig.Finish(blocker); rig.coordinator->PumpOwnedJobs();
    REQUIRE(AwaitStarted(rig.cwd, "remaining")); const auto finished = rig.Finish(queued);
    CHECK(finished.state == "failed"); REQUIRE(finished.worker_finished);
    { std::lock_guard lock(queued.trace->mutex); REQUIRE(queued.trace->limits); REQUIRE(queued.trace->raw);
      CHECK(queued.trace->command_calls == 1); CHECK(queued.trace->limits->timeout_ms <= 3500);
      CHECK(queued.trace->limits->timeout_ms > 0); CHECK(queued.trace->raw->outcome == "timed_out");
      CHECK(queued.trace->raw->details.at("timeout_ms") == queued.trace->limits->timeout_ms); CHECK(queued.trace->posts == 1); }
    CHECK_FALSE(fs::exists(rig.cwd / "remaining.done"));
    auto input = probe.Input(rig.cwd, "model", false, 60000); input["timeout_ms"] = 1000;
    auto model = rig.Declare(std::move(input), 5000); rig.Register(model); rig.Adopt(model, 4000); rig.Confirm(model);
    REQUIRE(AwaitStarted(rig.cwd, "model")); rig.Finish(model);
    { std::lock_guard lock(model.trace->mutex); REQUIRE(model.trace->limits); CHECK(model.trace->limits->timeout_ms == 1000);
      REQUIRE(model.trace->raw); CHECK(model.trace->raw->outcome == "timed_out"); CHECK(model.trace->posts == 1); }
    CHECK_FALSE(fs::exists(rig.cwd / "model.done"));
    auto host = rig.Declare(probe.Input(rig.cwd, "host"), 5000);
    rig.Register(host); rig.Adopt(host, 3000); rig.Confirm(host); const auto completed = rig.Finish(host);
    CHECK(completed.state == "succeeded"); REQUIRE(AwaitStarted(rig.cwd, "host")); CHECK(Bytes(rig.cwd / "host.done") == "host");
    { std::lock_guard lock(host.trace->mutex); REQUIRE(host.trace->limits); CHECK(host.trace->limits->timeout_ms == 3000);
      REQUIRE(host.trace->raw); CHECK(host.trace->raw->outcome == "succeeded"); CHECK(host.trace->posts == 1); }
    rig.Stop(); probe.Released(); Mark("runtime-budget", rig, queued);
}

TEST_CASE("Owned late real worker skips command and close joins its actual thread" * doctest::test_suite("sdk-owned-job-deadline")) {
    Directory directory; Probe probe(directory.root); auto gate = std::make_shared<Gate>();
    Rig rig(directory, [gate](std::thread& thread, std::function<void()> body) {
        thread = std::thread([gate, body = std::move(body)] { gate->Enter(); body(); });
        throw std::runtime_error("published real thread wins over starter exception");
    });
    ReleaseOnExit release{gate};
    auto target = rig.Declare(probe.Input(rig.cwd, "late"), 3000);
    rig.Register(target); rig.Adopt(target, 3000); rig.Confirm(target); REQUIRE(gate->Await());
    const auto running = rig.coordinator->SnapshotOwnedJob(target.request.owner, target.registered.facts->job_id);
    REQUIRE(running.started_receipt); REQUIRE(running.dispatched_receipt); CHECK(rig.quota->running.load() == 1);
    Expire(target);
    auto closing = std::async(std::launch::async, [&] { return rig.coordinator->Shutdown(); });
    const auto while_blocked = closing.wait_for(50ms); gate->Release();
    CHECK(while_blocked == std::future_status::timeout); REQUIRE(closing.get());
    NotInvoked(rig, target, true); CHECK_FALSE(fs::exists(rig.cwd / "late.started"));
    rig.Stop(); probe.Released(); Mark("late-worker", rig, target);
}

TEST_CASE("Owned thread factory failures cancel close and return the real quota" * doctest::test_suite("sdk-owned-job-deadline")) {
    {
        Directory directory; Probe probe(directory.root);
        Rig rig(directory, [](std::thread&, std::function<void()>) { throw std::runtime_error("before publication"); });
        auto failed = rig.Declare(probe.Input(rig.cwd, "never"), 3000);
        rig.Register(failed); rig.Adopt(failed, 3000); rig.Confirm(failed);
        const auto view = rig.Finish(failed); CHECK(view.state == "failed"); REQUIRE(view.started_receipt);
        CHECK_FALSE(view.command_not_invoked); CHECK_FALSE(view.persisted_receipt); CHECK_FALSE(view.post_receipt);
        const auto ledger = Ledger(rig.Path()); REQUIRE(view.observed_receipt);
        REQUIRE(ledger.FindEvent(view.observed_receipt->id));
        CHECK(ledger.FindEvent(view.observed_receipt->id)->payload.at("startupFailed") == true);
        { std::lock_guard lock(failed.trace->mutex); CHECK(failed.trace->command_calls == 0); }
        CHECK_FALSE(fs::exists(rig.cwd / "never.started")); rig.Stop(); probe.Released();
    }
    Directory directory; Probe probe(directory.root);
    Rig rig(directory, [](std::thread& thread, std::function<void()> body) {
        thread = std::thread(std::move(body)); throw std::runtime_error("after publication");
    });
    auto running = rig.Declare(probe.Input(rig.cwd, "cancel-close", true), 5000);
    rig.Register(running); rig.Adopt(running, 5000); rig.Confirm(running); REQUIRE(AwaitStarted(rig.cwd, "cancel-close"));
    rig.Cancel(running); rig.Cancel(running); rig.Stop();
    const auto view = rig.coordinator->SnapshotOwnedJob(running.request.owner, running.registered.facts->job_id);
    CHECK(view.state == "cancelled"); CHECK(view.worker_finished); CHECK_FALSE(view.command_not_invoked);
    REQUIRE(view.persisted_receipt); REQUIRE(view.post_receipt); Committed(*view.persisted_receipt); Committed(*view.post_receipt);
    { std::lock_guard lock(running.trace->mutex); CHECK(running.trace->command_calls == 1); CHECK(running.trace->posts == 1);
      REQUIRE(running.trace->raw); CHECK(running.trace->raw->outcome == "cancelled_during_run"); }
    CHECK_FALSE(fs::exists(rig.cwd / "cancel-close.done")); probe.Released(); Mark("startup-close", rig, running);
}

TEST_CASE("Owned deadline history holds native facts and rejects changed receipts" * doctest::test_suite("sdk-owned-job-deadline")) {
    Directory directory; Probe probe(directory.root); auto gate = std::make_shared<Gate>();
    Rig rig(directory, [gate](std::thread& thread, std::function<void()> body) {
        thread = std::thread([gate, body = std::move(body)] { gate->Enter(); body(); });
    });
    ReleaseOnExit release{gate};
    auto target = rig.Declare(probe.Input(rig.cwd, "history"), 3000);
    rig.Register(target); rig.Adopt(target, 3000); rig.Confirm(target); REQUIRE(gate->Await());
    Expire(target); gate->Release(); rig.Finish(target); NotInvoked(rig, target, true);
    // A real, bound admission without Confirm must remain incomplete on Hold.
    auto pending = rig.Declare(probe.Input(rig.cwd, "unconfirmed"), 0);
    rig.Register(pending); rig.Adopt(pending, 15000);
    const auto incomplete = Recovery(rig, pending); CHECK_FALSE(incomplete.admission_complete);
    CHECK_FALSE(incomplete.recovery->admission_complete); CHECK_FALSE(incomplete.recovery->command_not_invoked);
    rig.Stop(); NotInvoked(rig, target, true);
    const auto checked = sdk::ReadJobOperations(rig.Dir(), Ledger(rig.Path()));
    REQUIRE(checked.state == sdk::JobOperationRecoveryState::PassiveHold); REQUIRE(checked.facts.size() == 2);
    const auto original = Bytes(rig.Path()); const auto ledger = Ledger(rig.Path());
    const auto rows = Rows(original);
    const auto view = rig.coordinator->SnapshotOwnedJob(target.request.owner, target.registered.facts->job_id);
    REQUIRE(view.observed_receipt); REQUIRE(view.dispatched_receipt);
    for (unsigned variant = 0; variant < 12; ++variant) {
        auto changed = rows;
        auto found = std::find_if(changed.begin(), changed.end(), [&](const auto& row) {
            return row.value("eventId", std::string()) == view.observed_receipt->id;
        }); REQUIRE(found != changed.end());
        auto& payload = found->at("payload");
        if (variant == 0) payload["commandNotInvoked"]["version"] = 2;
        if (variant == 1) payload["commandNotInvoked"].erase("version");
        if (variant == 2) payload["commandNotInvoked"]["reason"] = "made_up";
        if (variant == 3) payload["observedStatus"] = "succeeded";
        if (variant == 4) payload["resultRef"] = target.parent_refs.persisted_event_id;
        if (variant == 5) payload["postEventRef"] = payload["parentAdmission"]["admissionEventRef"];
        if (variant == 6) payload.erase("parentAdmission");
        if (variant == 7) payload["parentAdmission"].erase("admissionEventRef");
        if (variant == 8) payload["parentAdmission"]["admissionEventRef"]["hash"] = std::string(64, 'f');
        if (variant == 9) payload["parentAdmission"]["admissionEventRef"]["seq"] = found->at("seq");
        if (variant == 10) payload["parentAdmission"]["terminalEventRef"] = payload["parentAdmission"]["persistedEventRef"];
        if (variant == 11) {
            const auto removed = target.parent_refs.admission_event_id;
            changed.erase(std::remove_if(changed.begin(), changed.end(), [&](const auto& row) {
                return row.value("eventId", std::string()) == removed;
            }), changed.end());
        }
        // Validate the actual changed observation as well as the strict owned/SDK readers.
        auto in_memory = ledger;
        auto observation = std::find_if(in_memory.events.begin(), in_memory.events.end(), [&](const auto& event) {
            return event.event_id == view.observed_receipt->id;
        }); REQUIRE(observation != in_memory.events.end());
        if (variant != 11) observation->payload = payload;
        else {
            in_memory.events.erase(std::remove_if(in_memory.events.begin(), in_memory.events.end(), [&](const auto& event) {
                return event.event_id == target.parent_refs.admission_event_id;
            }), in_memory.events.end());
            in_memory.event_index.clear(); in_memory.timeline.clear();
            for (std::size_t i = 0; i < in_memory.events.size(); ++i) {
                in_memory.event_index.emplace(in_memory.events[i].event_id, i);
                in_memory.timeline.push_back({in_memory.events[i].seq, false, i});
            }
            for (std::size_t i = 0; i < in_memory.messages.size(); ++i)
                in_memory.timeline.push_back({in_memory.messages[i].seq, true, i});
            std::sort(in_memory.timeline.begin(), in_memory.timeline.end(), [](const auto& a, const auto& b) { return a.seq < b.seq; });
        }
        CHECK_FALSE(v3::ReadOwnedJobAdoptions(in_memory));
        CHECK(sdk::ReadJobOperations(rig.Dir(), in_memory).state != sdk::JobOperationRecoveryState::PassiveHold);
        const auto path = directory.root / ("bad-deadline-" + std::to_string(variant) + ".jsonl");
        Write(path, Rehash(changed)); CHECK_FALSE(v3::ReadV3Ledger(path));
    }
    CHECK(Bytes(rig.Path()) == original);
    const auto path = rig.Path(); const auto dir = rig.Dir();
    const auto closed = rig.service->Close("deadline_history"); CHECK(closed.error_code.empty());
    const auto closed_bytes = Bytes(path);
    const auto plan = ToolJobCoordinator::PlanRecovery(Ledger(path), JobRecoveryPolicy::Hold);
    auto continued = v3::V3Writer::Continue(path); REQUIRE(continued);
    std::atomic<unsigned> resumed{0};
    ToolJobCoordinator::Options options;
    options.thread_starter = [&](std::thread& thread, std::function<void()> body) { ++resumed; thread = std::thread(std::move(body)); };
    ToolJobCoordinator restored(*continued, [](const auto&, const auto&) { return JobAuthDecision{true, false, {}}; },
        [&](const auto&) { ++resumed; return Tool::Result::Text("must never resume"); }, options);
    REQUIRE(restored.AdoptRecovery(plan) == plan.items.size());
    const auto historical = restored.GetJob(target.registered.facts->job_id); REQUIRE(historical.recovery);
    CHECK(historical.state == "cancelled"); CHECK(historical.recovery->command_not_invoked);
    CHECK(historical.recovery->admission_complete); CHECK(historical.recovery->knowledge == JobRecoveryKnowledge::TerminalConfirmed);
    CHECK(restored.WaitJobs({target.registered.facts->job_id}, 0, true).satisfied);
    CHECK(resumed.load() == 0); CHECK(Bytes(path) == closed_bytes); REQUIRE(restored.Shutdown()); REQUIRE(continued->Close());
    CHECK(sdk::ReadJobOperations(dir, Ledger(path)).state == sdk::JobOperationRecoveryState::PassiveHold);
    CHECK_FALSE(fs::exists(rig.cwd / "history.started")); CHECK_FALSE(fs::exists(rig.cwd / "unconfirmed.started"));
    probe.Released(); Mark("strict-recovery", rig, target);
}
