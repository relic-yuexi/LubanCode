#include <doctest/doctest.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <system_error>
#include <thread>
#include <utility>

#include "runtime/async_tool_runtime.hpp"
#include "tools/job_tools.hpp"
#include "tools/tool_job_coordinator.hpp"
#include "trajectory/v3/reader.hpp"
#include "trajectory/v3/result_store.hpp"
#include "trajectory/v3/tool_action.hpp"
#include "trajectory/v3/writer.hpp"

namespace {
namespace fs = std::filesystem;
namespace v3 = lubancode::trajectory::v3;
namespace runtime = lubancode::runtime;
using namespace std::chrono_literals;
using namespace lubancode::tools;

struct Watchdog {
    std::mutex mutex;
    std::condition_variable cv;
    bool done = false;
    std::thread worker;
    Watchdog() : worker([this] {
        std::unique_lock lock(mutex);
        if (!cv.wait_for(lock, 15s, [this] { return done; })) std::abort();
    }) {}
    ~Watchdog() {
        { std::lock_guard lock(mutex); done = true; }
        cv.notify_all();
        worker.join();
    }
};
struct OwnedDirectory {
    fs::path path;
    explicit OwnedDirectory(const std::string& tag) {
        static std::atomic<unsigned> sequence{0};
        const auto root = fs::temp_directory_path();
        for (unsigned attempt = 0; attempt != 64; ++attempt) {
            auto candidate = root / ("lubancode-job-hold-" + tag + "-" +
                std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) +
                "-" + std::to_string(sequence.fetch_add(1)));
            std::error_code error;
            const bool created = fs::create_directory(candidate, error);
            if (error == std::errc::file_exists) continue;
            REQUIRE_MESSAGE(!error, error.message());
            if (created) { path = std::move(candidate); break; }
        }
        REQUIRE_FALSE(path.empty());
    }
    ~OwnedDirectory() {
        std::error_code error;
        fs::remove_all(path, error);
        try { CHECK_MESSAGE(!error, error.message()); } catch (...) {}
    }
};
std::string Bytes(const fs::path& path) {
    std::ifstream input(path, std::ios::binary);
    REQUIRE(input.is_open());
    const std::string bytes((std::istreambuf_iterator<char>(input)), {});
    REQUIRE_FALSE(input.bad());
    return bytes;
}
void Committed(const v3::WriteReceipt& receipt) {
    REQUIRE_MESSAGE(receipt.status == v3::WriteReceipt::Status::Committed, receipt.error_message);
}
v3::V3Ledger Read(const fs::path& path) {
    auto result = v3::ReadV3Ledger(path);
    REQUIRE_MESSAGE(result.has_value(), (result ? std::string() : result.error()));
    return std::move(*result);
}
JobAuthDecision Allow(const std::string&, const nlohmann::json&) { return {true, false, ""}; }
void Mark(const char* path) { std::printf("[job-hold-path] %s\n", path); }

// Seed actual native protocol facts at an exact recovery window. No producer
// coordinator is destroyed and then misrepresented as a process-crash prefix.
// Every persisted result below is a real immutable ResultStore artifact.
struct Seed {
    OwnedDirectory root;
    fs::path journal;
    std::optional<v3::V3Writer> writer;
    std::optional<v3::ToolActionSession> action;
    std::string job_id = "job-000001", action_id = "action-job-000001";
    std::string mode = "job_handle", admission_text, terminal_ref, persisted_ref;
    explicit Seed(const std::string& tag) : root(tag), journal(root.path / "s1.jsonl") {
        auto opened = v3::V3Writer::Start(journal, "20261003-120000-JHOLD", "run-000001", "hold fixture");
        REQUIRE(opened.has_value());
        writer = std::move(*opened);
    }
    JobStartRequest Request(const std::string& call) {
        v3::MessageDraft draft;
        draft.turn_id = "turn-000001"; draft.step_id = "step-000001";
        draft.request_id = "request-000001";
        draft.origin = v3::MessageOrigin::SessionRuntime;
        draft.provider = "fixture"; draft.wire = "responses"; draft.model = "fixture";
        draft.response_model = "fixture";
        draft.usage = {{"inputTokens", 1}, {"outputTokens", 1}};
        draft.message = {{"role", "assistant"}, {"content", "start"},
            {"tool_calls", nlohmann::json::array({{{"id", call}, {"type", "function"},
                {"function", {{"name", "project_write"}, {"arguments", "{}"}}}}})}};
        const auto message = writer->AppendMessage(std::move(draft), v3::Durability::PowerLoss);
        Committed(message); Committed(writer->AdmitMessages({message.id}));
        JobStartRequest request;
        request.tool_name = "project_write"; request.tool_input = nlohmann::json::object();
        request.turn_id = "turn-000001"; request.step_id = "step-000001";
        request.assistant_message_ref = message.id;
        request.policy.side_effect_class = "local_write";
        request.policy.resource_keys = {"project:same"};
        return request;
    }
    v3::WriteReceipt Event(v3::EventKindV3 kind, nlohmann::json payload) {
        v3::EventDraft event;
        event.kind = kind; event.turn_id = "turn-000001"; event.step_id = "step-000001";
        event.action_id = action_id; event.payload = std::move(payload);
        auto receipt = writer->AppendEvent(std::move(event), v3::Durability::PowerLoss);
        Committed(receipt); return receipt;
    }
    void Register(bool approval = false, bool native = false) {
        const auto request = Request("old-call");
        mode = native ? "native_deferred" : "job_handle";
        action = v3::ToolActionSession::Admit(*writer, request.turn_id, request.step_id,
            action_id, "queued", request.assistant_message_ref, "old-call");
        REQUIRE(action->last_event_id().has_value());
        nlohmann::json payload{{"tool_call_id", action_id}, {"attempt", 1u}, {"jobId", job_id},
            {"mode", mode}, {"assistantMessageRef", request.assistant_message_ref},
            {"executionPolicy", request.policy.ToJson()}};
        if (approval) payload["approvalRequired"] = true;
        if (native) payload["wireCallRef"] = {{"provider", "fixture"}, {"wire", "responses"},
            {"callId", "old-call"}, {"async", true}};
        Event(v3::EventKindV3::ToolJobRegistered, std::move(payload));
    }
    std::string Persist(const std::string& execution_ref, const std::string& text) {
        auto store = v3::ResultStore::Open(root.path);
        REQUIRE(store.has_value());
        v3::ResultStore::PersistRequest request;
        request.result_kind = "text"; request.tool_call_id = action_id;
        request.attempt = action->attempt(); request.execution_event_ref = execution_ref;
        v3::ResultStore::ChannelOutput output;
        output.channel = "combined"; output.data = text; output.output_bytes = text.size();
        request.outputs.push_back(std::move(output));
        const auto stored = store->Persist(request);
        REQUIRE_MESSAGE(stored.ok, stored.error);
        REQUIRE_FALSE(stored.result_ref.empty());
        const auto receipt = action->PersistedResult(*writer, stored.result_ref, execution_ref);
        Committed(receipt); return receipt.id;
    }
    void Admission(bool message = true) {
        Committed(action->Start(*writer, "args-admission", {"project_write", "fixture", "", ""}));
        const auto finished = action->Finish(*writer, std::nullopt); Committed(finished);
        const auto persisted = Persist(finished.id, "actual admission");
        const auto selected = action->SelectResult(*writer, {persisted}, {}, "done"); Committed(selected);
        admission_text = nlohmann::json{{"jobId", job_id}, {"status", "queued"}}.dump();
        if (message) Committed(action->AppendToolMessage(*writer, admission_text, selected.id));
    }
    void Dispatch(bool start = true) {
        if (mode == "job_handle") Committed(action->BeginNextAttempt(*writer, "job_dispatch"));
        Event(v3::EventKindV3::ToolJobDispatched, {{"tool_call_id", action_id},
            {"attempt", action->attempt()}, {"jobId", job_id}, {"ownerEpoch", "old-owner-epoch"}});
        if (start) Committed(action->Start(*writer, "args-business", {"project_write", "fixture", "", ""}));
    }
    void Terminal(const std::string& state, bool observe = true, bool persist = true) {
        v3::WriteReceipt terminal;
        if (state == "succeeded") terminal = action->Finish(*writer, std::nullopt);
        else if (state == "failed") terminal = action->Fail(*writer, "actual-business-failure");
        else if (state == "cancelled") terminal = action->Cancel(*writer, "during_execution", "actual-cancel");
        else terminal = action->MarkUnknown(*writer, "actual-execution-unconfirmed");
        Committed(terminal); terminal_ref = terminal.id;
        if (persist) persisted_ref = Persist(terminal.id, "actual business " + state);
        if (observe) {
            nlohmann::json payload{{"tool_call_id", action_id}, {"jobId", job_id}, {"observedStatus", state}};
            if (!persisted_ref.empty()) { payload["resultRef"] = persisted_ref; payload["resultVersion"] = 1u; }
            Event(v3::EventKindV3::ToolJobObserved, std::move(payload));
        }
    }
};
struct Restored {
    OwnedDirectory root;
    fs::path journal;
    std::optional<v3::V3Writer> writer;
    std::shared_ptr<ToolJobCoordinator> coordinator;
    explicit Restored(Seed& seed, const std::string& tag) : root(tag), journal(root.path / "s1.jsonl") {
        const auto prefix = Bytes(seed.journal);
        { std::ofstream output(journal, std::ios::binary); REQUIRE(output.is_open()); output << prefix;
          output.close(); REQUIRE_FALSE(output.fail()); }
        if (fs::exists(seed.root.path / "artifacts"))
            fs::copy(seed.root.path / "artifacts", root.path / "artifacts", fs::copy_options::recursive);
        // Independent owned prefix, real reader and Continue; no forged hash.
        const auto verified = Read(journal); REQUIRE(verified.session_id == seed.writer->session_id());
        auto continued = v3::V3Writer::Continue(journal);
        REQUIRE_MESSAGE(continued.has_value(), (continued ? std::string() : continued.error()));
        writer = std::move(*continued);
    }
    void Attach(JobExecutor executor = nullptr, ToolJobCoordinator::Options options = {},
                JobAuthorizationGate auth = Allow) {
        coordinator = std::make_shared<ToolJobCoordinator>(*writer, std::move(auth), std::move(executor), std::move(options));
    }
    ~Restored() { coordinator.reset(); writer.reset(); }
    JobRecoveryPlan Plan(JobRecoveryPolicy policy = JobRecoveryPolicy::Hold) const { return ToolJobCoordinator::PlanRecovery(Read(journal), policy); }
    JobStartRequest NewRequest(const std::string& call = "new-call") {
        v3::MessageDraft draft;
        draft.turn_id = "turn-000002"; draft.step_id = "step-000001";
        draft.request_id = "request-000002"; draft.origin = v3::MessageOrigin::SessionRuntime;
        draft.provider = "fixture"; draft.wire = "responses"; draft.model = "fixture";
        draft.response_model = "fixture"; draft.usage = {{"inputTokens", 1}, {"outputTokens", 1}};
        draft.message = {{"role", "assistant"}, {"content", "new"},
            {"tool_calls", nlohmann::json::array({{{"id", call}, {"type", "function"},
                {"function", {{"name", "project_write"}, {"arguments", "{}"}}}}})}};
        const auto message = writer->AppendMessage(std::move(draft), v3::Durability::PowerLoss);
        Committed(message); Committed(writer->AdmitMessages({message.id}));
        JobStartRequest request;
        request.tool_name = "project_write"; request.tool_input = nlohmann::json::object();
        request.turn_id = "turn-000002"; request.step_id = "step-000001"; request.assistant_message_ref = message.id;
        return request;
    }
};
void Passive(Restored& restored, const Seed& seed, JobRecoveryKnowledge knowledge) {
    const auto before = Bytes(restored.journal);
    const auto plan = restored.Plan(); REQUIRE(plan.items.size() == 1);
    REQUIRE(plan.items.front().recovery.has_value());
    CHECK(plan.items.front().recovery->knowledge == knowledge);
    REQUIRE(restored.coordinator->AdoptRecovery(plan) == 1);
    CHECK(restored.coordinator->AdoptRecovery(plan) == 0);
    const auto status = restored.coordinator->GetJob(seed.job_id);
    REQUIRE(status.recovery.has_value()); CHECK(status.recovery->knowledge == knowledge);
    CHECK(status.action_id == seed.action_id);
    CHECK(restored.coordinator->running_count() == 0); CHECK(restored.coordinator->queued_count() == 0);
    restored.coordinator->PumpCompletions();
    CHECK_FALSE(restored.coordinator->DebugSubmitEnvelope(seed.job_id, "old-owner-epoch", Tool::Result{"late", false}));
    restored.coordinator->PumpCompletions();
    CHECK(restored.coordinator->stale_envelopes_rejected() == 1);
    CHECK(Bytes(restored.journal) == before);
}
}

TEST_CASE("Hold recovery leaves registered and incomplete admission records passive") {
    Watchdog watchdog;
    for (bool admission_done : {false, true}) {
        Seed seed(admission_done ? "admission-gap" : "registered"); seed.Register();
        if (admission_done) seed.Admission(false);
        std::atomic<unsigned> calls{0};
        Restored restored(seed, "registered-restored");
        restored.Attach([&](const JobExecutionContext&) { ++calls; return Tool::Result{"not-old", false}; });
        Passive(restored, seed, JobRecoveryKnowledge::KnownNotDispatched);
        const auto before = Bytes(restored.journal);
        std::string text = "must-be-cleared";
        CHECK_FALSE(restored.coordinator->CompleteAdmission(seed.job_id, &text)); CHECK(text.empty());
        const auto granted = restored.coordinator->GrantApproval(seed.job_id);
        CHECK_FALSE(granted.ok); CHECK(granted.error_code == "job.recovery.held");
        const auto cancelled = restored.coordinator->CancelJob(seed.job_id, "test-held");
        CHECK_FALSE(cancelled.ok); CHECK(cancelled.status == "recovery_held");
        auto get = MakeJobGetTool(restored.coordinator);
        const auto result = get->execute({{"jobId", seed.job_id}}); CHECK_FALSE(result.is_error);
        const auto json = nlohmann::json::parse(result.content);
        CHECK(json.at("status").get<std::string>() == "registered");
        CHECK(json.at("recovery").at("knowledge").get<std::string>() == "known_not_dispatched");
        CHECK(json.at("recovery").at("executionAttempt").get<unsigned>() == 0);
        auto wait = MakeJobWaitTool(restored.coordinator);
        const auto waited = nlohmann::json::parse(wait->execute({{"jobIds", {seed.job_id}}, {"timeout_ms", 5u}, {"mode", "all"}}).content);
        CHECK(waited.at("timedOut").get<bool>());
        CHECK(waited.at("statuses").at(0).at("recovery").at("originalState").get<std::string>() == "registered");
        CHECK(calls == 0); REQUIRE(restored.coordinator->Shutdown()); CHECK(Bytes(restored.journal) == before);
        CHECK(restored.coordinator->GetJob(seed.job_id).access_denied);
    }
    Seed denied_seed("denied"); denied_seed.Register();
    Restored denied(denied_seed, "denied-restored");
    denied.Attach(nullptr, {}, [](const std::string&, const nlohmann::json&) { return JobAuthDecision{false, false, "owned-auth-denied"}; });
    REQUIRE(denied.coordinator->AdoptRecovery(denied.Plan()) == 1);
    const auto denied_before = Bytes(denied.journal);
    auto denied_get = MakeJobGetTool(denied.coordinator);
    const auto denied_json = nlohmann::json::parse(denied_get->execute({{"jobId", denied_seed.job_id}}).content);
    CHECK(denied_json.at("accessDenied").get<bool>()); CHECK_FALSE(denied_json.contains("recovery"));
    CHECK_FALSE(denied.coordinator->CancelJob(denied_seed.job_id, "test-denied").ok);
    REQUIRE(denied.coordinator->Shutdown()); CHECK(Bytes(denied.journal) == denied_before);
    Mark("registered-admission");
}

TEST_CASE("Hold recovery reserves old queued approval keys while new jobs execute") {
    Watchdog watchdog;
    for (bool approval : {false, true}) {
        Seed seed(approval ? "approval" : "queue"); seed.Register(approval); seed.Admission();
        std::atomic<unsigned> calls{0}; std::string executed_id;
        Restored restored(seed, "new-live");
        auto quota = std::make_shared<GlobalRunningQuota>(); quota->limit = 1;
        ToolJobCoordinator::Options options; options.global = quota; options.limits.session_running = 1;
        restored.Attach([&](const JobExecutionContext& context) { executed_id = context.job_id; ++calls; return Tool::Result{"new-live-result", false}; }, options);
        Passive(restored, seed, JobRecoveryKnowledge::KnownNotDispatched);
        const auto old_status = restored.coordinator->GetJob(seed.job_id);
        CHECK(old_status.state == (approval ? "awaiting_approval" : "registered"));
        CHECK(old_status.recovery->admission_complete);
        std::string text; REQUIRE(restored.coordinator->CompleteAdmission(seed.job_id, &text)); CHECK(text == seed.admission_text);
        const auto before_new = Bytes(restored.journal);
        CHECK_FALSE(restored.coordinator->GrantApproval(seed.job_id).ok); CHECK(Bytes(restored.journal) == before_new);
        const auto new_job = restored.coordinator->StartJob(restored.NewRequest()); REQUIRE(new_job.ok);
        CHECK(new_job.job_id != seed.job_id); CHECK(new_job.action_id != seed.action_id);
        const auto waited = restored.coordinator->WaitJobs({new_job.job_id}, 2000, true); REQUIRE(waited.satisfied);
        REQUIRE(waited.statuses.size() == 1); CHECK(waited.statuses.front().state == "succeeded");
        CHECK_FALSE(waited.statuses.front().recovery.has_value()); CHECK(calls == 1); CHECK(executed_id == new_job.job_id);
        CHECK(quota->running == 0); CHECK(restored.coordinator->GetJob(seed.job_id).state == old_status.state);
        auto get = MakeJobGetTool(restored.coordinator);
        const auto json = nlohmann::json::parse(get->execute({{"jobId", new_job.job_id}}).content); CHECK_FALSE(json.contains("recovery"));
        const auto complete = Bytes(restored.journal); REQUIRE(restored.coordinator->Shutdown()); CHECK(Bytes(restored.journal) == complete);
    }
    Mark("new-job-isolation");
}

TEST_CASE("Hold recovery keeps dispatched and started gaps unknown without new execution") {
    Watchdog watchdog;
    for (bool started : {false, true}) {
        Seed seed(started ? "started" : "dispatched"); seed.Register(); seed.Admission(); seed.Dispatch(started);
        std::atomic<unsigned> calls{0}; Restored restored(seed, "unknown");
        restored.Attach([&](const JobExecutionContext&) { ++calls; return Tool::Result{"not-old", false}; });
        Passive(restored, seed, JobRecoveryKnowledge::ExecutionUnconfirmed);
        const auto status = restored.coordinator->GetJob(seed.job_id); REQUIRE(status.recovery.has_value());
        CHECK(status.state == "unknown"); CHECK(status.recovery->original_state == "running");
        CHECK(status.recovery->execution_attempt == 2); CHECK(status.recovery->execution_started == started);
        CHECK(status.recovery->dispatched_count == 1); CHECK(status.recovery->execution_terminal_event.empty());
        const auto wait = restored.coordinator->WaitJobs({seed.job_id}, 0, true); CHECK(wait.satisfied);
        REQUIRE(wait.statuses.size() == 1); CHECK(wait.statuses.front().recovery->knowledge == JobRecoveryKnowledge::ExecutionUnconfirmed);
        CHECK_FALSE(restored.coordinator->CancelJob(seed.job_id, "test-held").ok);
        const auto before = Bytes(restored.journal); REQUIRE(restored.coordinator->Shutdown()); CHECK(Bytes(restored.journal) == before); CHECK(calls == 0);
    }
    Mark("dispatched-unknown");
}

TEST_CASE("Hold recovery distinguishes actual business terminals from admission and delivery gaps") {
    Watchdog watchdog;
    for (const std::string state : {"succeeded", "failed", "cancelled", "unknown"}) {
        for (bool observe : {false, true}) {
            Seed seed(state + (observe ? "-complete" : "-gap")); seed.Register(); seed.Admission(); seed.Dispatch(); seed.Terminal(state, observe);
            std::atomic<unsigned> calls{0}; Restored restored(seed, "terminal");
            restored.Attach([&](const JobExecutionContext&) { ++calls; return Tool::Result{"not-old", false}; });
            const auto expected = state == "unknown" ? JobRecoveryKnowledge::ExecutionUnconfirmed :
                observe ? JobRecoveryKnowledge::TerminalConfirmed : JobRecoveryKnowledge::TerminalDeliveryGap;
            Passive(restored, seed, expected);
            const auto status = restored.coordinator->GetJob(seed.job_id); REQUIRE(status.recovery.has_value());
            CHECK(status.recovery->execution_attempt == 2); CHECK(status.recovery->execution_started);
            CHECK(status.recovery->execution_terminal_event == seed.terminal_ref);
            CHECK(status.recovery->execution_state == (state == "succeeded" ? "done" : state));
            CHECK(status.recovery->original_state == (observe ? state : "running"));
            CHECK(status.result_ref == (observe ? seed.persisted_ref : std::string()));
            std::string text; REQUIRE(restored.coordinator->CompleteAdmission(seed.job_id, &text)); CHECK(text == seed.admission_text);
            const auto cancelled = restored.coordinator->CancelJob(seed.job_id, "test-held");
            CHECK(cancelled.ok == (expected == JobRecoveryKnowledge::TerminalConfirmed));
            const auto before = Bytes(restored.journal); REQUIRE(restored.coordinator->Shutdown()); CHECK(Bytes(restored.journal) == before); CHECK(calls == 0);
        }
    }
    for (bool missing_body : {false, true}) {
        Seed seed(missing_body ? "body-gap" : "terminal-admission-gap"); seed.Register();
        seed.Admission(missing_body); seed.Dispatch(); seed.Terminal("succeeded", true, !missing_body);
        Restored restored(seed, "terminal-delivery-gap"); restored.Attach();
        Passive(restored, seed, JobRecoveryKnowledge::TerminalDeliveryGap);
        const auto status = restored.coordinator->GetJob(seed.job_id); REQUIRE(status.recovery.has_value());
        CHECK(status.state == "succeeded"); CHECK(status.recovery->original_state == "succeeded");
        CHECK(status.recovery->admission_complete == missing_body);
        std::string text; CHECK(restored.coordinator->CompleteAdmission(seed.job_id, &text) == missing_body);
        CHECK(text == (missing_body ? seed.admission_text : std::string()));
        const auto before = Bytes(restored.journal); REQUIRE(restored.coordinator->Shutdown()); CHECK(Bytes(restored.journal) == before);
    }
    Mark("terminal-facts");
}

TEST_CASE("Async Hold recovery does not rebuild old deferred delivery but runs new orders") {
    Watchdog watchdog;
    Seed seed("native"); seed.Register(false, true); seed.Dispatch(); seed.Terminal("succeeded");
    Restored restored(seed, "async");
    std::atomic<unsigned> calls{0};
    runtime::AsyncToolRuntime::Hooks hooks;
    hooks.writer = &*restored.writer; hooks.writer_mutex = std::make_shared<std::recursive_mutex>();
    hooks.auth = Allow;
    hooks.executor = [&](const JobExecutionContext&) { ++calls; return Tool::Result{"new-async-result", false}; };
    auto origin = std::make_shared<std::optional<JobStartRequest>>();
    hooks.call_origin_resolver = [origin](const std::string& call) { return call == "new-call" ? *origin : std::optional<JobStartRequest>{}; };
    runtime::AsyncToolRuntimeOptions options; options.recovery_policy = JobRecoveryPolicy::Hold;
    options.provider = "fixture"; options.wire = "responses"; options.model = "fixture";
    options.tools["project_write"] = runtime::AsyncToolPolicy{};
    auto async = runtime::AsyncToolRuntime::Create(std::move(hooks), options); REQUIRE(async != nullptr);
    auto* planner = dynamic_cast<runtime::ResultDeliveryPlannerImpl*>(async->planner()); REQUIRE(planner != nullptr);
    const auto before = Bytes(restored.journal);
    async->RestoreFromLedger(); async->RestoreFromLedger(); async->gate()->PumpBatchBoundary();
    CHECK(calls == 0); CHECK(planner->pending_native_count() == 0); CHECK(planner->mailbox_size() == 0);
    CHECK(async->planner()->SelectForRequestBoundary().empty()); CHECK(Bytes(restored.journal) == before);
    const auto held = async->coordinator()->GetJob(seed.job_id); REQUIRE(held.recovery.has_value());
    CHECK(held.recovery->knowledge == JobRecoveryKnowledge::UnsupportedMode);
    // Independent legacy planner consumes the same true deferred source; this
    // proves the Hold no-op did not merely use a source with no delivery debt.
    runtime::ResultDeliveryPlannerImpl legacy({&*restored.writer, std::make_shared<std::recursive_mutex>(), {}, {}});
    CHECK(legacy.RestoreFromLedger(Read(restored.journal)) == 1); CHECK(legacy.pending_native_count() == 1);
    *origin = restored.NewRequest();
    lubancode::api::ToolUseBlock call; call.id = "new-call"; call.name = "project_write"; call.input = nlohmann::json::object();
    const auto decisions = async->gate()->AdjudicateBatch({call}); REQUIRE(decisions.size() == 1);
    REQUIRE(decisions.front().mode == lubancode::agent::ToolProtocolMode::JobHandle);
    const auto order = async->gate()->TakeJobOrder(call, decisions.front()); REQUIRE(order.has_value()); CHECK_FALSE(order->is_error);
    const auto receipt = nlohmann::json::parse(order->content); const auto job_id = receipt.at("jobId").get<std::string>();
    CHECK(job_id != seed.job_id); REQUIRE(async->coordinator()->WaitJobs({job_id}, 2000, true).satisfied);
    async->gate()->PumpBatchBoundary(); CHECK(calls == 1); CHECK(planner->mailbox_size() == 1);
    CHECK(async->coordinator()->GetJob(job_id).state == "succeeded");
    REQUIRE(async->Shutdown());
    Mark("async-propagation");
}

TEST_CASE("Legacy recovery remains default and Hold rejects foreign plans before publication") {
    Watchdog watchdog;
    Seed seed("legacy"); seed.Register(); seed.Admission();
    {
        std::atomic<unsigned> calls{0}; Restored restored(seed, "legacy-restored");
        restored.Attach([&](const JobExecutionContext&) { ++calls; return Tool::Result{"legacy-actual", false}; });
        const auto legacy = ToolJobCoordinator::PlanRecovery(Read(restored.journal));
        CHECK(legacy.policy == JobRecoveryPolicy::Legacy); REQUIRE(legacy.items.size() == 1);
        CHECK_FALSE(legacy.items.front().recovery.has_value()); REQUIRE(restored.coordinator->AdoptRecovery(legacy) == 1);
        const auto waited = restored.coordinator->WaitJobs({seed.job_id}, 2000, true); REQUIRE(waited.satisfied);
        REQUIRE(waited.statuses.size() == 1); CHECK(waited.statuses.front().state == "succeeded"); CHECK(calls == 1);
        CHECK_FALSE(waited.statuses.front().recovery.has_value()); REQUIRE(restored.coordinator->Shutdown());
    }
    for (const std::string state : {"succeeded", "unknown"}) {
        Seed terminal("legacy-" + state); terminal.Register(); terminal.Admission(); terminal.Dispatch();
        if (state == "succeeded") terminal.Terminal(state);
        std::atomic<unsigned> calls{0}; Restored restored(terminal, "legacy-terminal");
        restored.Attach([&](const JobExecutionContext&) { ++calls; return Tool::Result{"must-not-replay", false}; });
        const auto plan = ToolJobCoordinator::PlanRecovery(Read(restored.journal));
        REQUIRE(plan.items.size() == 1); CHECK(plan.policy == JobRecoveryPolicy::Legacy);
        REQUIRE(restored.coordinator->AdoptRecovery(plan) == 1); restored.coordinator->PumpCompletions();
        CHECK(restored.coordinator->GetJob(terminal.job_id).state == state); CHECK(calls == 0);
        CHECK_FALSE(restored.coordinator->GetJob(terminal.job_id).recovery.has_value());
        REQUIRE(restored.coordinator->Shutdown());
    }
    Restored restored(seed, "invalid-plan"); restored.Attach();
    const auto before = Bytes(restored.journal);
    for (int variant = 0; variant != 4; ++variant) {
        auto plan = restored.Plan(); REQUIRE(plan.items.size() == 1);
        if (variant == 0) plan.source_session_id = "foreign-session";
        if (variant == 1) { auto bad = plan.items.front(); bad.job_id = "job-184467440737095516160"; plan.items.push_back(std::move(bad)); }
        if (variant == 2) plan.items.front().action_id.clear();
        if (variant == 3) plan.items.front().recovery->turn_id = "foreign-turn";
        CHECK_THROWS_AS(restored.coordinator->AdoptRecovery(plan), std::invalid_argument);
        CHECK(restored.coordinator->GetJob(seed.job_id).state == "unknown_job");
        CHECK(restored.coordinator->running_count() == 0); CHECK(restored.coordinator->queued_count() == 0); CHECK(Bytes(restored.journal) == before);
    }
    REQUIRE(restored.coordinator->AdoptRecovery(restored.Plan()) == 1);
    CHECK(Bytes(restored.journal) == before); REQUIRE(restored.coordinator->Shutdown()); CHECK(Bytes(restored.journal) == before);
    Mark("legacy-and-owner");
}
