#include <doctest/doctest.h>

#include <array>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <future>
#include <iterator>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <stdexcept>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#include "agent/loop.hpp"
#include "hooks/hash.hpp"
#include "platform/paths.hpp"
#include "tools/registry.hpp"
#include "tools/run_command.hpp"
#include "tools/tool_job_coordinator.hpp"
#include "trajectory/canonical_json.hpp"
#include "trajectory/v3/reader.hpp"
#include "trajectory/v3/hooks.hpp"
#include "trajectory/v3/result_store.hpp"
#include "trajectory/v3/tool_action.hpp"
#include "trajectory/v3/writer.hpp"

namespace {
namespace fs = std::filesystem;
namespace v3 = lubancode::trajectory::v3;
namespace agent = lubancode::agent;
namespace runtime = lubancode::runtime;
using namespace lubancode::tools;
using Json = nlohmann::json;
using namespace std::chrono_literals;

struct Watchdog {
    std::mutex mutex;
    std::condition_variable cv;
    bool done = false;
    std::thread thread;
    Watchdog() : thread([this] {
        std::unique_lock lock(mutex);
        if (!cv.wait_for(lock, 120s, [this] { return done; })) std::abort();
    }) {}
    ~Watchdog() {
        { std::lock_guard lock(mutex); done = true; }
        cv.notify_all();
        thread.join();
    }
};

struct Directory {
    fs::path path;
    explicit Directory(const std::string& tag) {
        static std::atomic<unsigned> sequence{0};
        for (unsigned retry = 0; retry < 128; ++retry) {
            const auto candidate = fs::temp_directory_path() / ("lubancode-owned-adoption-" + tag + "-" +
                std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + "-" +
                std::to_string(sequence.fetch_add(1)));
            std::error_code error;
            if (fs::create_directory(candidate, error)) {
                path = candidate;
                path = fs::canonical(path, error);
                REQUIRE_MESSAGE(!error, error.message());
                break;
            }
            const bool acceptable = !error || error == std::errc::file_exists;
            REQUIRE_MESSAGE(acceptable, error.message());
        }
        REQUIRE_FALSE(path.empty());
    }
    ~Directory() {
        std::error_code error;
        fs::remove_all(path, error);
        try { CHECK_MESSAGE(!error, error.message()); } catch (...) {}
    }
};

std::string Utf8(const fs::path& value) { return lubancode::platform::PathToUtf8(value); }
std::string Bytes(const fs::path& path) {
    std::ifstream stream(path, std::ios::binary);
    REQUIRE(stream.is_open());
    std::string value((std::istreambuf_iterator<char>(stream)), {});
    REQUIRE_FALSE(stream.bad());
    return value;
}
v3::V3Ledger Read(const fs::path& path) {
    auto result = v3::ReadV3Ledger(path);
    REQUIRE_MESSAGE(result.has_value(), (result ? std::string() : result.error()));
    return std::move(*result);
}
void Committed(const v3::WriteReceipt& receipt) {
    REQUIRE_MESSAGE(receipt.status == v3::WriteReceipt::Status::Committed, receipt.error_message);
    REQUIRE_FALSE(receipt.id.empty()); REQUIRE(receipt.seq > 0); REQUIRE_FALSE(receipt.line_hash.empty());
}
std::string Hash(const Json& value) {
    const auto bytes = lubancode::trajectory::CanonicalJsonDump(value);
    REQUIRE(bytes.has_value());
    return lubancode::hooks::Sha256Hex(*bytes);
}
std::string Quote(const std::string& value, const std::string& shell) {
#ifdef _WIN32
    if (shell == "cmd") {
        REQUIRE(value.find_first_of("\"%\r\n") == std::string::npos);
        return "\"" + value + "\"";
    }
#endif
    std::string result = "'";
    for (char ch : value) {
        if (ch == '\'') result += shell == "powershell" ? "''" : "'\\''";
        else result.push_back(ch);
    }
    return result + "'";
}
std::string Shell() {
#ifdef _WIN32
    return "cmd";
#else
    return "sh";
#endif
}
struct Probe {
    fs::path executable;
    explicit Probe(const fs::path& root) {
        const fs::path original = fs::u8path(LUBANCORE_TEST_COMMAND_LIMITS_PROBE);
        REQUIRE(fs::is_regular_file(original));
        executable = root / original.filename();
        REQUIRE(fs::copy_file(original, executable));
        fs::permissions(executable, fs::status(original).permissions());
    }
    Json Input(const fs::path& cwd, const std::string& tag, std::uint64_t bytes = 32,
               std::uint64_t delay = 0, bool wait = false) const {
        const std::string shell = Shell();
        std::string command = Quote(Utf8(executable), shell);
        const std::array<std::string, 7> arguments = {tag + ".started", tag + ".done",
            std::to_string(bytes), std::to_string(delay), tag, "A", wait ? tag + ".release" : "-"};
        for (const auto& argument : arguments) command += " " + Quote(argument, shell);
        return {{"command", command}, {"shell", shell}, {"cwd", Utf8(cwd)}};
    }
    void Released() {
        // Actual image deletion proves Windows stopped owning this executable.
        // On POSIX this is cleanup only; completion/join is checked separately.
        REQUIRE(fs::remove(executable));
        CHECK_FALSE(fs::exists(executable));
    }
};
bool AwaitFile(const fs::path& path) {
    const auto deadline = std::chrono::steady_clock::now() + 10s;
    while (std::chrono::steady_clock::now() < deadline) {
        std::error_code error;
        if (fs::is_regular_file(path, error) && !error) return true;
        std::this_thread::sleep_for(5ms);
    }
    return false;
}
void Release(const fs::path& cwd, const std::string& tag) {
    std::ofstream stream(cwd / (tag + ".release"), std::ios::binary);
    REQUIRE(stream.is_open()); stream.put('1'); stream.flush(); REQUIRE(stream.good());
}
void Mark(const char* name) {
    std::fprintf(stderr, "[job-owned-adoption-path] %s\n", name);
    std::fflush(stderr);
}
struct Counts {
    std::atomic<unsigned> execute{0}, legacy_gate{0}, legacy_clock{0}, thread{0}, pre{0}, permission{0}, scope{0}, post{0};
    std::mutex raw_mutex;
    std::vector<Tool::Result> raws;
};
struct Fault {
    unsigned fail_at = 0, writes = 0;
    std::optional<std::string> Next() {
        if (fail_at && ++writes == fail_at) return "owned-adoption-write-fault";
        return std::nullopt;
    }
};
struct Source {
    lubancode::api::ToolUseBlock call;
    std::string message, admission, pending, action;
    std::optional<v3::ToolActionSession> parent;
};
struct Rig {
    Directory root;
    fs::path journal, cwd;
    std::shared_ptr<Counts> counts = std::make_shared<Counts>();
    std::shared_ptr<Fault> fault = std::make_shared<Fault>();
    std::shared_ptr<std::recursive_mutex> serial = std::make_shared<std::recursive_mutex>();
    std::shared_ptr<GlobalRunningQuota> quota = std::make_shared<GlobalRunningQuota>();
    std::shared_ptr<v3::V3Writer> writer;
    std::shared_ptr<ToolJobCoordinator> coordinator;
    unsigned sequence = 0;

    explicit Rig(const std::string& tag, const fs::path& shared_cwd = {},
                 std::string project = "project-native", ToolJobCoordinator::Options extra = {})
        : root(tag), journal(root.path / "session.jsonl"), cwd(shared_cwd.empty() ? root.path : shared_cwd) {
        static std::atomic<unsigned> next_sid{1};
        v3::V3WriterOptions writer_options;
        writer_options.inject_io_failure = [owner = fault] { return owner->Next(); };
        auto opened = v3::V3Writer::Start(journal, "20261003-180000-JOWN" + std::to_string(next_sid.fetch_add(1)),
            "run-000001", "owned adoption fixture", Json::object(), writer_options);
        REQUIRE_MESSAGE(opened.has_value(), (opened ? std::string() : opened.error()));
        writer = std::make_shared<v3::V3Writer>(std::move(*opened));
        if (extra.global) quota = extra.global;
        else extra.global = quota;
        extra.prepared_registration = PreparedRegistrationContext{serial, std::move(project), cwd};
        extra.clock_ms = [owner = counts] { ++owner->legacy_clock; return std::int64_t{1}; };
        if (!extra.thread_starter) extra.thread_starter = [owner = counts](std::thread& thread, std::function<void()> body) {
            ++owner->thread; thread = std::thread(std::move(body));
        };
        coordinator = std::make_shared<ToolJobCoordinator>(*writer,
            [owner = counts](const std::string&, const Json&) { ++owner->legacy_gate; return JobAuthDecision{true, false, {}}; },
            [owner = counts](const JobExecutionContext&) { ++owner->execute; return Tool::Result{"legacy executed", false}; }, extra);
    }
    ~Rig() { coordinator.reset(); writer.reset(); }
    Source Declare(Json input) {
        std::lock_guard lock(*serial);
        Source source;
        const auto number = ++sequence;
        source.call.id = "provider-" + std::to_string(number); source.call.name = "run_command";
        source.call.input = std::move(input); source.action = "action-parent-" + std::to_string(number);
        v3::MessageDraft message;
        message.turn_id = "turn-000001"; message.step_id = "step-000001"; message.request_id = "request-000001";
        message.origin = v3::MessageOrigin::SessionRuntime;
        message.provider = "fixture"; message.wire = "responses"; message.model = "fixture";
        message.response_model = "fixture"; message.usage = {{"inputTokens", 1}, {"outputTokens", 1}};
        message.message = {{"role", "assistant"}, {"content", "declared"},
            {"tool_calls", Json::array({{{"id", source.call.id}, {"type", "function"},
                {"function", {{"name", source.call.name}, {"arguments", source.call.input.dump()}}}}})}};
        const auto written = writer->AppendMessage(std::move(message), v3::Durability::PowerLoss);
        Committed(written); source.message = written.id;
        const auto applied = writer->AdmitMessages({written.id}, v3::Durability::PowerLoss);
        Committed(applied); source.admission = applied.id;
        source.parent = v3::ToolActionSession::Admit(*writer, "turn-000001", "step-000001", source.action,
            "owned-parent", source.message, source.call.id, Json::object(), v3::Durability::PowerLoss);
        REQUIRE(source.parent->last_event_id().has_value()); source.pending = *source.parent->last_event_id();
        return source;
    }
    PreparedJobRequest Prepare(const Source& source, std::optional<Json> rewrite = std::nullopt) {
        ToolRegistry registry; ToolRegistration registration;
        registration.tool = std::make_unique<RunCommandTool>();
        registration.source_kind = lubancode::ToolSourceKind::Builtin;
        registration.source_instance = "native-run-command"; registration.version_or_digest = "native-v1";
        registration.effect_class = lubancode::EffectClass::LocalProcessUnknown;
        registry.Register(std::move(registration));
        agent::TurnWiring wiring;
        wiring.on_pre_tool_use_hook = [owner = counts, rewrite](const auto&, const auto&, const auto&) {
            ++owner->pre; runtime::ToolHookDecision decision; decision.decision = runtime::ToolHookDecision::Decision::Allow;
            decision.updated_input = rewrite;
            return decision;
        };
        wiring.on_permission_evaluate = [owner = counts, expected = rewrite.value_or(source.call.input), id = source.call.id]
            (const auto& actual_id, const auto& name, auto, const auto& input, const auto&) {
            ++owner->permission; CHECK(actual_id == id); CHECK(name == "run_command"); CHECK(bool(input == expected));
            runtime::PermissionVerdict verdict; verdict.action = runtime::PermissionVerdict::Action::Allow; return verdict;
        };
        const auto prepared = agent::PrepareOwnedToolInput(registry, source.call, wiring, {});
        REQUIRE_MESSAGE(prepared.has_value(), (prepared ? std::string() : prepared.error().content));
        const auto owner = coordinator->PreparedOwner(); REQUIRE(owner.has_value());
        PreparedJobRequest request;
        request.owner = *owner; request.provider_tool_call_id = prepared->call_id; request.tool_name = prepared->tool_name;
        request.original_input = source.call.input; request.effective_input = prepared->effective_input;
        request.assistant_message_ref = source.message; request.parent_action_id = source.action;
        request.turn_id = "turn-000001"; request.step_id = "step-000001";
        request.tool_identity = {prepared->tool_name, prepared->source_instance, "native-v1", Utf8(owner->cwd)};
        request.policy.allow_background = true; request.policy.side_effect_class = "external";
        request.policy.resource_keys = {"native-command"};
        return request;
    }
    void LegacyUntouched() const {
        CHECK(counts->legacy_gate.load() == 0); CHECK(counts->legacy_clock.load() == 0);
    }
};

PreparedJobRegistration Register(Rig& rig, const Source& source, std::optional<Json> rewrite = std::nullopt) {
    const auto result = rig.coordinator->RegisterPreparedJob(rig.Prepare(source, std::move(rewrite)));
    REQUIRE_MESSAGE(result.state == PreparedJobRegistrationState::Registered, result.error);
    REQUIRE(result.facts); REQUIRE(result.facts->pending_receipt); REQUIRE(result.facts->registered_receipt);
    Committed(*result.facts->pending_receipt); Committed(*result.facts->registered_receipt);
    CHECK(rig.counts->execute.load() == 0); CHECK(rig.counts->thread.load() == 0);
    rig.LegacyUntouched();
    return result;
}
OwnedJobCapability Capability(Rig& rig, std::shared_ptr<const PreparedJobFacts> facts,
                            CommandExecutionLimits limits = {15000, 1024}) {
    OwnedJobCapability capability;
    capability.command = std::make_shared<RunCommandTool>(); capability.command_limits = limits;
    capability.scope_gate = [facts, counts = rig.counts](const OwnedJobScope& scope, const Json& input,
        const v3::ToolIdentity& identity, const JobExecutionPolicy& policy) {
        ++counts->scope;
        const bool valid = scope.owner == facts->owner && scope.job_id == facts->job_id &&
            scope.action_id == facts->action_id && scope.attempt == 1 && scope.turn_id == facts->turn_id &&
            scope.step_id == facts->step_id && scope.parent_action_id == facts->parent_action_id &&
            scope.provider_tool_call_id == facts->provider_tool_call_id && input == facts->effective_input &&
            identity.ToJson() == facts->tool_identity.ToJson() && policy.ToJson() == facts->policy.ToJson();
        CHECK(valid);
        return JobAuthDecision{valid, false, valid ? std::string() : "actual-scope-mismatch"};
    };
    capability.post = [facts, counts = rig.counts, writer = rig.writer, serial = rig.serial, limits]
        (const OwnedJobCompletion& completion) {
        std::lock_guard lock(*serial);
        ++counts->post;
        { std::lock_guard raw_lock(counts->raw_mutex); counts->raws.push_back(completion.raw); }
        CHECK(completion.scope.owner == facts->owner); CHECK(completion.scope.job_id == facts->job_id);
        CHECK(completion.scope.action_id == facts->action_id); CHECK(completion.scope.attempt == 1);
        Committed(completion.started_receipt); Committed(completion.terminal_receipt); Committed(completion.persisted_receipt);
        CHECK(completion.raw.details.at("timeout_ms").get<std::uint64_t>() <= limits.timeout_ms);
        CHECK(completion.raw.details.at("max_output_bytes").get<std::uint64_t>() == limits.max_output_bytes);
        const auto ledger = Read(writer->path());
        for (const auto& receipt : {completion.started_receipt, completion.terminal_receipt, completion.persisted_receipt}) {
            const auto found = v3::MakeOwnedJobReference(ledger, receipt.id); REQUIRE(found.has_value());
            CHECK(found->at("seq").get<std::uint64_t>() == receipt.seq);
            CHECK(found->at("hash").get<std::string>() == receipt.line_hash);
        }
        const v3::HookHandlerSpec handler{"native-owned-post", lubancode::hooks::Sha256Hex("native-owned-post-v1"),
            "builtin", 0, "block"};
        const auto dispatch_id = writer->NewHookDispatchId();
        auto dispatch = v3::HookDispatchSession::Dispatch(*writer, dispatch_id, "PostAction",
            completion.scope.turn_id, completion.scope.step_id, completion.scope.action_id, {handler},
            Json{{"executionEventRef", completion.terminal_receipt.id}, {"resultEventRef", completion.persisted_receipt.id}});
        Committed(dispatch.BeginInvocation(*writer, "invocation-" + dispatch_id, handler));
        return dispatch.CompleteInvocation(*writer, "allow", std::nullopt, 0, v3::Durability::PowerLoss);
    };
    return capability;
}
OwnedJobAdoption Adopt(Rig& rig, const PreparedJobRegistration& registered, OwnedJobCapability capability) {
    auto result = rig.coordinator->AdoptPreparedJob(registered.facts->owner, registered.facts->job_id, std::move(capability));
    REQUIRE_MESSAGE(result.state == OwnedJobAdoptionState::Adopted, result.error);
    REQUIRE(result.facts); REQUIRE(result.receipt); Committed(*result.receipt);
    const auto body = Json::parse(result.admission_content);
    CHECK(body.at("jobId").get<std::string>() == registered.facts->job_id);
    CHECK(body.at("adoptionEventRef").get<std::string>() == result.receipt->id);
    CHECK(body.at("status").get<std::string>() == "parent_delivery_pending");
    CHECK(body.at("layout").get<std::string>() == std::string(v3::kOwnedJobLayout));
    return result;
}
ParentJobAdmissionRefs ParentChain(Rig& rig, Source& source, const OwnedJobAdoption& adopted, unsigned stop_after = 6,
                                  std::optional<std::string> text_override = std::nullopt) {
    std::lock_guard lock(*rig.serial);
    REQUIRE(source.parent.has_value());
    ParentJobAdmissionRefs refs;
    Committed(source.parent->Start(*rig.writer, "args-parent-" + Hash(source.call.input), adopted.facts->tool_identity,
        std::nullopt, Json{{"toolName", "run_command"}}, v3::Durability::PowerLoss));
    if (stop_after < 2) return refs;
    const auto terminal = source.parent->Finish(*rig.writer, std::nullopt, 0);
    Committed(terminal); refs.terminal_event_id = terminal.id;
    if (stop_after < 3) return refs;
    auto store = v3::ResultStore::Open(rig.writer->path().parent_path());
    REQUIRE_MESSAGE(store.has_value(), (store ? std::string() : store.error()));
    const std::string text = text_override.value_or(adopted.admission_content);
    v3::ResultStore::PersistRequest material;
    material.result_kind = "text"; material.tool_call_id = source.action; material.attempt = 1;
    material.execution_event_ref = terminal.id;
    v3::ResultStore::ChannelOutput output; output.channel = "combined"; output.data = text; output.output_bytes = text.size();
    material.outputs.push_back(std::move(output));
    material.capture_limits = {{"max_output_bytes", 65536}};
    material.preview_policy = {{"policy", "owned-admission-test"}, {"maxPreviewBytes", 32768}};
    const auto saved = store->Persist(material); REQUIRE_MESSAGE(saved.ok, saved.error);
    const auto persisted = source.parent->PersistedResult(*rig.writer, saved.result_ref, terminal.id, 1);
    Committed(persisted); refs.persisted_event_id = persisted.id;
    if (stop_after < 4) return refs;
    const auto selected = source.parent->SelectResult(*rig.writer, {persisted.id}, {}, "done", 1);
    Committed(selected); refs.selected_event_id = selected.id;
    if (stop_after < 5) return refs;
    v3::MessageDraft message;
    message.turn_id = "turn-000001"; message.step_id = "step-000001"; message.action_id = source.action;
    message.origin = v3::MessageOrigin::SessionRuntime; message.purpose = v3::MessagePurpose::Conversation;
    message.result_selection_ref = selected.id;
    message.message = {{"role", "tool"}, {"tool_call_id", source.action}, {"content", text}};
    const auto emitted = rig.writer->AppendMessage(std::move(message), v3::Durability::PowerLoss);
    Committed(emitted); refs.tool_message_id = emitted.id;
    if (stop_after < 6) return refs;
    const auto admitted = rig.writer->AdmitMessages({emitted.id}, v3::Durability::PowerLoss);
    Committed(admitted); refs.admission_event_id = admitted.id;
    return refs;
}
OwnedJobStatusView Finished(Rig& rig, const PreparedJobRegistration& registered, bool expect_global_idle = true) {
    const auto result = rig.coordinator->WaitOwnedJobs(registered.facts->owner, {registered.facts->job_id}, 20000, true);
    REQUIRE(result.satisfied); CHECK_FALSE(result.timed_out); REQUIRE(result.statuses.size() == 1);
    const auto& view = result.statuses.front();
    CHECK(view.scope.owner == registered.facts->owner); CHECK(view.scope.action_id == registered.facts->action_id);
    CHECK(view.scope.attempt == 1); CHECK(view.worker_finished);
    CHECK(rig.coordinator->running_count() == 0);
    if (expect_global_idle) CHECK(rig.quota->running.load() == 0);
    rig.LegacyUntouched();
    return view;
}
void Confirm(Rig& rig, const PreparedJobRegistration& registered, const ParentJobAdmissionRefs& refs) {
    const auto admission = rig.coordinator->ConfirmParentAdmission(registered.facts->owner, registered.facts->job_id, refs);
    REQUIRE_MESSAGE(admission.confirmed, admission.error);
}
unsigned Events(const v3::V3Ledger& ledger, v3::EventKindV3 kind, const std::string& action) {
    unsigned count = 0;
    for (const auto& event : ledger.events) if (event.kind == kind && event.action_id == action) ++count;
    return count;
}
void RawEquals(const Rig& rig, const OwnedJobStatusView& view, const std::string& text) {
    REQUIRE(view.persisted_receipt);
    const auto ledger = Read(rig.journal);
    const auto found = std::find_if(ledger.events.begin(), ledger.events.end(), [&](const auto& e) {
        return e.event_id == view.persisted_receipt->id;
    });
    REQUIRE(found != ledger.events.end());
    bool actual_channel = false;
    for (const auto& ref : found->payload.at("result_ref")) {
        if (ref.at("kind").get<std::string>() == "combined") {
            CHECK(Bytes(rig.journal.parent_path() / fs::u8path(ref.at("path").get<std::string>())) == text);
            actual_channel = true;
        }
    }
    REQUIRE(actual_channel);
}
v3::EventDraft AdoptionDraft(const Rig& rig, const PreparedJobRegistration& registered) {
    const auto& facts = *registered.facts;
    const auto ledger = Read(rig.journal);
    const auto* source = ledger.FindEvent(facts.registered_receipt->id); REQUIRE(source);
    v3::EventDraft draft;
    draft.kind = v3::EventKindV3::ToolJobAdopted; draft.turn_id = facts.turn_id; draft.step_id = facts.step_id;
    draft.action_id = facts.action_id;
    draft.payload = {{"tool_call_id", facts.action_id}, {"attempt", 1}, {"jobId", facts.job_id},
        {"layout", std::string(v3::kOwnedJobLayout)}, {"parentActionId", facts.parent_action_id},
        {"provider_tool_call_id", facts.provider_tool_call_id}, {"toolName", facts.tool_name},
        {"effectiveInput", source->payload.at("effectiveInput")}, {"originalInputSha256", source->payload.at("originalInputSha256")},
        {"effectiveInputSha256", source->payload.at("effectiveInputSha256")}, {"toolIdentity", source->payload.at("toolIdentity")},
        {"executionPolicy", source->payload.at("executionPolicy")}, {"preparedOwner", source->payload.at("preparedOwner")},
        {"commandLimits", {{"timeout_ms", 15000}, {"max_output_bytes", 1024}}}};
    for (const auto& [key, id] : std::vector<std::pair<std::string, std::string>>{
        {"assistantMessageRef", facts.assistant_message_ref}, {"sourcePendingEventRef", facts.source_pending_event_id},
        {"sourceAdmissionEventRef", facts.source_admission_event_id}, {"preparedPendingEventRef", facts.pending_receipt->id},
        {"registeredEventRef", facts.registered_receipt->id}}) {
        const auto actual = v3::MakeOwnedJobReference(ledger, id); REQUIRE(actual.has_value()); draft.payload[key] = *actual;
    }
    return draft;
}
} // namespace

TEST_CASE("Owned Job adoption: complete capability holds and real provenance rejects foreign owners") {
    Watchdog watchdog;
    Rig rig("hold"); Probe probe(rig.root.path);
    auto source = rig.Declare(probe.Input(rig.cwd, "held"));
    const auto registered = Register(rig, source);
    for (unsigned missing = 0; missing < 5; ++missing) {
        auto capability = Capability(rig, registered.facts);
        if (missing == 0) capability.command.reset();
        if (missing == 1) capability.scope_gate = {};
        if (missing == 2) capability.post = {};
        if (missing == 3) capability.command_limits.timeout_ms = 0;
        if (missing == 4) capability.command_limits.max_output_bytes = 0;
        const auto before = Bytes(rig.journal);
        const auto refused = rig.coordinator->AdoptPreparedJob(registered.facts->owner, registered.facts->job_id, std::move(capability));
        CHECK(refused.state == OwnedJobAdoptionState::Rejected); CHECK_FALSE(refused.receipt.has_value());
        CHECK(Bytes(rig.journal) == before); CHECK(rig.counts->thread.load() == 0);
    }
    for (unsigned foreign = 0; foreign < 6; ++foreign) {
        auto owner = registered.facts->owner;
        if (foreign == 0) owner.session_id += "foreign";
        if (foreign == 1) owner.run_id = "run-000002";
        if (foreign == 2) ++owner.coordinator_id;
        if (foreign == 3) ++owner.epoch;
        if (foreign == 4) owner.project_id += "foreign";
        if (foreign == 5) owner.cwd = rig.root.path.parent_path();
        const auto before = Bytes(rig.journal);
        const auto refused = rig.coordinator->AdoptPreparedJob(owner, registered.facts->job_id, Capability(rig, registered.facts));
        CHECK(refused.state == OwnedJobAdoptionState::Rejected); CHECK(Bytes(rig.journal) == before);
    }
    const auto adopted = Adopt(rig, registered, Capability(rig, registered.facts));
    for (unsigned n = 0; n < 3; ++n) rig.coordinator->PumpOwnedJobs();
    const auto view = rig.coordinator->GetOwnedJob(registered.facts->owner, registered.facts->job_id);
    CHECK(view.state == "parent_delivery_pending"); CHECK_FALSE(view.dispatched_receipt); CHECK_FALSE(view.started_receipt);
    CHECK(rig.counts->thread.load() == 0); CHECK(rig.counts->post.load() == 0);
    CHECK_FALSE(fs::exists(rig.cwd / "held.started")); CHECK(rig.quota->running.load() == 0);
    const auto ledger = Read(rig.journal);
    const auto facts = v3::ReadOwnedJobAdoptions(ledger); REQUIRE(facts.has_value()); REQUIRE(facts->size() == 1);
    CHECK(facts->front().adopted_event_id == adopted.receipt->id);
    CHECK(facts->front().registered_event_id == registered.facts->registered_receipt->id);
    CHECK(facts->front().original_input_sha256 == Hash(source.call.input));
    const auto actions = v3::FoldToolActions(ledger);
    unsigned parent_count = 0, business_count = 0;
    for (const auto& action : actions) {
        if (action.tool_call_id == source.action) { CHECK(action.provider_reply_required); ++parent_count; }
        if (action.tool_call_id == registered.facts->action_id) { CHECK_FALSE(action.provider_reply_required); ++business_count; }
    }
    CHECK(parent_count == 1); CHECK(business_count == 1);
    CHECK(Events(ledger, v3::EventKindV3::ToolExecutionFinished, registered.facts->action_id) == 0);
    const auto duplicate = rig.coordinator->AdoptPreparedJob(registered.facts->owner, registered.facts->job_id, Capability(rig, registered.facts));
    CHECK(duplicate.state == OwnedJobAdoptionState::Rejected);
    rig.LegacyUntouched();
    Mark("held-provenance");
}

TEST_CASE("Owned Job adoption: only the actual complete parent handle chain unlocks dispatch") {
    Watchdog watchdog;
    for (unsigned phase = 1; phase <= 6; ++phase) {
        Rig rig("parent-" + std::to_string(phase)); Probe probe(rig.root.path);
        auto source = rig.Declare(probe.Input(rig.cwd, "parent"));
        const auto registered = Register(rig, source);
        const auto adopted = Adopt(rig, registered, Capability(rig, registered.facts));
        const auto refs = ParentChain(rig, source, adopted, phase);
        const auto before = Bytes(rig.journal);
        const auto result = rig.coordinator->ConfirmParentAdmission(registered.facts->owner, registered.facts->job_id, refs);
        if (phase < 6) {
            CHECK_FALSE(result.confirmed); CHECK(Bytes(rig.journal) == before);
            rig.coordinator->PumpOwnedJobs(); CHECK(rig.counts->thread.load() == 0);
            CHECK_FALSE(fs::exists(rig.cwd / "parent.started"));
        } else {
            REQUIRE_MESSAGE(result.confirmed, result.error);
            const auto view = Finished(rig, registered); CHECK(view.execution_state == "succeeded");
            CHECK(rig.counts->thread.load() == 1); CHECK(rig.counts->post.load() == 1);
            CHECK(Bytes(rig.cwd / "parent.done") == "parent");
        }
        rig.LegacyUntouched();
    }
    for (unsigned wrong = 0; wrong < 4; ++wrong) {
        Rig rig("false-handle-" + std::to_string(wrong)); Probe probe(rig.root.path);
        auto source = rig.Declare(probe.Input(rig.cwd, "wrong")); const auto registered = Register(rig, source);
        const auto adopted = Adopt(rig, registered, Capability(rig, registered.facts));
        auto body = Json::parse(adopted.admission_content);
        if (wrong == 0) body["jobId"] = "job-foreign";
        if (wrong == 1) body["adoptionEventRef"] = registered.facts->registered_receipt->id;
        if (wrong == 2) body["layout"] = "unknown-layout";
        if (wrong == 3) body["status"] = "succeeded";
        const auto refs = ParentChain(rig, source, adopted, 6, body.dump());
        const auto before = Bytes(rig.journal);
        CHECK_FALSE(rig.coordinator->ConfirmParentAdmission(registered.facts->owner, registered.facts->job_id, refs).confirmed);
        CHECK(Bytes(rig.journal) == before); CHECK(rig.counts->thread.load() == 0);
    }
    Mark("parent-chain");
}

TEST_CASE("Owned Job adoption: native command success and failure retain actual attempt one raw and Post") {
    Watchdog watchdog;
    for (bool failed : {false, true}) {
        Rig rig(failed ? "failure" : "success"); Probe probe(rig.root.path);
        Json input = probe.Input(rig.cwd, failed ? "failed" : "original");
        if (failed) input["command"] = Quote(Utf8(probe.executable), Shell()); // real probe returns native exit 20
        auto source = rig.Declare(input);
        const auto effective = failed ? std::optional<Json>{} : std::optional<Json>{probe.Input(rig.cwd, "done")};
        const auto registered = Register(rig, source, effective);
        const auto adopted = Adopt(rig, registered, Capability(rig, registered.facts));
        Confirm(rig, registered, ParentChain(rig, source, adopted));
        const auto view = Finished(rig, registered);
        CHECK(view.execution_state == (failed ? "failed" : "succeeded")); CHECK(view.gap.empty());
        REQUIRE(view.terminal_receipt); REQUIRE(view.persisted_receipt); REQUIRE(view.post_receipt); REQUIRE(view.observed_receipt);
        Committed(*view.terminal_receipt); Committed(*view.persisted_receipt); Committed(*view.post_receipt); Committed(*view.observed_receipt);
        const auto ledger = Read(rig.journal);
        CHECK(Events(ledger, failed ? v3::EventKindV3::ToolExecutionFailed : v3::EventKindV3::ToolExecutionFinished, registered.facts->action_id) == 1);
        for (const auto& event : ledger.events) if (event.action_id == registered.facts->action_id && event.payload.contains("attempt"))
            CHECK(event.payload.at("attempt").get<std::uint64_t>() == 1);
        REQUIRE(rig.counts->raws.size() == 1);
        RawEquals(rig, view, rig.counts->raws.front().content);
        CHECK(rig.counts->raws.front().is_error == failed);
        if (!failed) {
            CHECK(Bytes(rig.cwd / "done.done") == "done");
            CHECK_FALSE(fs::exists(rig.cwd / "original.started"));
            CHECK_FALSE(fs::exists(rig.cwd / "original.done"));
            CHECK(registered.facts->original_input_sha256 != registered.facts->effective_input_sha256);
            CHECK(rig.counts->raws.front().content.ends_with(std::string(30, 'A') + "\r\n"));
        }
        CHECK(rig.counts->post.load() == 1); CHECK(rig.counts->execute.load() == 0);
        for (const auto& message : ledger.messages) CHECK(message.action_id != registered.facts->action_id);
        const auto obligations = v3::ProjectProtocolObligations(ledger);
        CHECK(v3::FindProtocolObligation(obligations, registered.facts->action_id) == nullptr);
        const auto parent = v3::FindProtocolObligation(obligations, source.action); REQUIRE(parent); CHECK(parent->paired);
        REQUIRE(rig.coordinator->Shutdown()); probe.Released();
    }
    Mark("native-raw-post");
}

TEST_CASE("Owned Job adoption: four real sessions isolate cancellation and scoped command limits") {
    Watchdog watchdog;
    Directory project("existing-project"); Probe probe(project.path);
    auto global = std::make_shared<GlobalRunningQuota>(); global->limit = 4;
    auto command = std::make_shared<RunCommandTool>();
    std::vector<std::unique_ptr<Rig>> rigs;
    std::vector<Source> sources;
    std::vector<PreparedJobRegistration> registrations;
    for (unsigned n = 0; n < 4; ++n) {
        ToolJobCoordinator::Options options; options.global = global;
        rigs.push_back(std::make_unique<Rig>("parallel-" + std::to_string(n), n < 2 ? project.path : fs::path{},
            n < 2 ? "existing-project" : "other-project-" + std::to_string(n), options));
        auto& rig = *rigs.back();
        sources.push_back(rig.Declare(probe.Input(rig.cwd, "parallel-" + std::to_string(n), 32, 0, true)));
        registrations.push_back(Register(rig, sources.back()));
        auto capability = Capability(rig, registrations.back().facts); capability.command = command;
        const auto adopted = Adopt(rig, registrations.back(), std::move(capability));
        Confirm(rig, registrations.back(), ParentChain(rig, sources.back(), adopted));
    }
    for (unsigned n = 0; n < 4; ++n) {
        REQUIRE(AwaitFile(rigs[n]->cwd / ("parallel-" + std::to_string(n) + ".started")));
        const auto content = Bytes(rigs[n]->cwd / ("parallel-" + std::to_string(n) + ".started"));
        const auto newline = content.find('\n'); REQUIRE(newline != std::string::npos);
        CHECK(fs::equivalent(fs::u8path(content.substr(newline + 1)), rigs[n]->cwd));
        CHECK_FALSE(fs::exists(rigs[n]->cwd / ("parallel-" + std::to_string(n) + ".done")));
    }
    CHECK(global->running.load() == 4);
    const auto denied = rigs[1]->coordinator->GetOwnedJob(registrations[0].facts->owner, registrations[0].facts->job_id);
    CHECK(denied.access_denied);
    const auto cancelled = rigs[0]->coordinator->CancelOwnedJob(registrations[0].facts->owner, registrations[0].facts->job_id, "one-session-only");
    REQUIRE_MESSAGE(cancelled.ok, cancelled.error);
    for (unsigned n = 1; n < 4; ++n) Release(rigs[n]->cwd, "parallel-" + std::to_string(n));
    for (unsigned n = 0; n < 4; ++n) {
        const auto view = Finished(*rigs[n], registrations[n], false);
        CHECK(view.execution_state == (n == 0 ? "cancelled" : "succeeded")); CHECK(view.gap.empty());
        CHECK(rigs[n]->counts->post.load() == 1); CHECK(rigs[n]->counts->thread.load() == 1);
        if (n == 0) CHECK_FALSE(fs::exists(rigs[n]->cwd / "parallel-0.done"));
        else CHECK(Bytes(rigs[n]->cwd / ("parallel-" + std::to_string(n) + ".done")) == "parallel-" + std::to_string(n));
        REQUIRE(rigs[n]->coordinator->Shutdown());
    }
    CHECK(global->running.load() == 0);
    probe.Released();
    for (bool overflow : {false, true}) {
        Rig rig(overflow ? "owned-overflow" : "owned-timeout"); Probe limits_probe(rig.root.path);
        auto source = rig.Declare(limits_probe.Input(rig.cwd, "limited", overflow ? 257 : 32, 60000));
        const auto registered = Register(rig, source);
        const CommandExecutionLimits limits{overflow ? 15000u : 3000u, 256};
        const auto adopted = Adopt(rig, registered, Capability(rig, registered.facts, limits));
        Confirm(rig, registered, ParentChain(rig, source, adopted));
        const auto view = Finished(rig, registered);
        CHECK(view.execution_state == "failed"); REQUIRE(rig.counts->raws.size() == 1);
        const auto& raw = rig.counts->raws.front();
        CHECK(raw.is_error); CHECK(raw.error_code == (overflow ? "process.output_limit" : "process.timeout"));
        CHECK(raw.details.at("timeout_ms").get<std::uint64_t>() == limits.timeout_ms);
        CHECK(raw.details.at("max_output_bytes").get<std::uint64_t>() == limits.max_output_bytes);
        REQUIRE(fs::is_regular_file(rig.cwd / "limited.started")); CHECK_FALSE(fs::exists(rig.cwd / "limited.done"));
        RawEquals(rig, view, raw.content); REQUIRE(rig.coordinator->Shutdown()); limits_probe.Released();
    }
    Mark("sessions-cancel-limits");
}

TEST_CASE("Owned Job adoption: actual thread publication failures and shared serial Close drain ownership") {
    Watchdog watchdog;
    for (bool published : {false, true}) {
        auto starts = std::make_shared<std::atomic<unsigned>>(0);
        ToolJobCoordinator::Options options;
        options.thread_starter = [starts, published](std::thread& destination, std::function<void()> body) {
            ++*starts;
            if (published) destination = std::thread(std::move(body));
            throw std::runtime_error(published ? "after actual destination publication" : "before destination publication");
        };
        Rig rig(published ? "published-throw" : "before-throw", {}, "startup-project", options); Probe probe(rig.root.path);
        auto source = rig.Declare(probe.Input(rig.cwd, "startup")); const auto registered = Register(rig, source);
        const auto adopted = Adopt(rig, registered, Capability(rig, registered.facts));
        Confirm(rig, registered, ParentChain(rig, source, adopted));
        const auto result = rig.coordinator->WaitOwnedJobs(registered.facts->owner, {registered.facts->job_id}, 20000, true);
        REQUIRE(result.satisfied); REQUIRE(result.statuses.size() == 1); CHECK(starts->load() == 1);
        if (published) {
            CHECK(result.statuses.front().execution_state == "succeeded"); CHECK(result.statuses.front().gap.empty());
            CHECK(Bytes(rig.cwd / "startup.done") == "startup"); CHECK(rig.counts->post.load() == 1);
        } else {
            CHECK(result.statuses.front().execution_state != "succeeded");
            CHECK_FALSE(fs::exists(rig.cwd / "startup.started")); CHECK(rig.counts->post.load() == 0);
        }
        REQUIRE(rig.coordinator->Shutdown()); CHECK(rig.coordinator->shutdown_complete()); CHECK(rig.quota->running.load() == 0);
        probe.Released(); rig.LegacyUntouched();
    }
    Rig rig("serial-close"); Probe probe(rig.root.path);
    auto source = rig.Declare(probe.Input(rig.cwd, "closing", 32, 0, true)); const auto registered = Register(rig, source);
    auto capability = Capability(rig, registered.facts);
    std::weak_ptr<RunCommandTool> command_owner = capability.command;
    const auto adopted = Adopt(rig, registered, std::move(capability));
    Confirm(rig, registered, ParentChain(rig, source, adopted));
    REQUIRE(AwaitFile(rig.cwd / "closing.started"));
    std::unique_lock serial(*rig.serial);
    std::promise<void> entered;
    auto closed = std::async(std::launch::async, [&rig, &entered] {
        rig.coordinator->RequestShutdown(); entered.set_value(); return rig.coordinator->Shutdown();
    });
    struct UnlockAndJoin {
        std::unique_lock<std::recursive_mutex>& lock; std::future<bool>& future;
        ~UnlockAndJoin() { if (lock.owns_lock()) lock.unlock(); if (future.valid()) future.wait(); }
    } cleanup{serial, closed};
    REQUIRE(entered.get_future().wait_for(10s) == std::future_status::ready);
    CHECK_FALSE(rig.coordinator->PreparedOwner().has_value()); CHECK_FALSE(rig.writer->closed());
    CHECK(closed.wait_for(50ms) == std::future_status::timeout);
    serial.unlock(); REQUIRE(closed.get());
    CHECK(rig.coordinator->shutdown_complete()); CHECK(rig.quota->running.load() == 0);
    CHECK(command_owner.expired()); CHECK_FALSE(fs::exists(rig.cwd / "closing.done"));
    CHECK(rig.counts->post.load() == 1); probe.Released(); rig.LegacyUntouched();
    Mark("startup-close");
}

TEST_CASE("Owned Job adoption: real native write gaps freeze settlement and all recovered layouts stay Hold") {
    Watchdog watchdog;
    {
        Rig rig("schema-and-source"); Probe probe(rig.root.path);
        const auto source = rig.Declare(probe.Input(rig.cwd, "no-authority")); const auto registered = Register(rig, source);
        auto bad_layout = AdoptionDraft(rig, registered); bad_layout.payload["layout"] = "unknown-layout";
        const auto before = Bytes(rig.journal);
        const auto rejected = rig.writer->AppendEvent(std::move(bad_layout), v3::Durability::PowerLoss);
        CHECK(rejected.status == v3::WriteReceipt::Status::Rejected); CHECK_FALSE(rig.writer->broken());
        CHECK(Bytes(rig.journal) == before);
        auto bad_source = AdoptionDraft(rig, registered);
        // Keep native source ID/seq/owner intact, supply a different actual
        // row's hash. Single-row shape is valid; the common reader must reject.
        bad_source.payload["sourcePendingEventRef"]["hash"] = registered.facts->registered_receipt->line_hash;
        Committed(rig.writer->AppendEvent(std::move(bad_source), v3::Durability::PowerLoss));
        const auto ledger = v3::ReadV3Ledger(rig.journal);
        CHECK_FALSE(ledger.has_value());
        if (!ledger) CHECK(ledger.error().find("adoption_source_mismatch") != std::string::npos);
        CHECK(rig.counts->thread.load() == 0); CHECK(rig.counts->post.load() == 0);
    }
    {
        Rig rig("adoption-fault"); Probe probe(rig.root.path);
        auto source = rig.Declare(probe.Input(rig.cwd, "never")); const auto registered = Register(rig, source);
        rig.fault->writes = 0; rig.fault->fail_at = 1;
        const auto adopted = rig.coordinator->AdoptPreparedJob(registered.facts->owner, registered.facts->job_id, Capability(rig, registered.facts));
        CHECK(adopted.state == OwnedJobAdoptionState::Unconfirmed); REQUIRE(adopted.receipt);
        CHECK(adopted.receipt->status == v3::WriteReceipt::Status::Rejected);
        CHECK(adopted.receipt->error_code == "v3writer.injected"); CHECK(rig.writer->broken());
        const auto bytes = Bytes(rig.journal);
        for (unsigned n = 0; n < 3; ++n) rig.coordinator->PumpOwnedJobs();
        CHECK(Bytes(rig.journal) == bytes); CHECK(rig.fault->writes == 1);
        CHECK(rig.counts->thread.load() == 0); CHECK(rig.counts->post.load() == 0);
    }
    {
        Rig rig("dispatch-fault"); Probe probe(rig.root.path);
        auto source = rig.Declare(probe.Input(rig.cwd, "not-dispatched")); const auto registered = Register(rig, source);
        const auto adopted = Adopt(rig, registered, Capability(rig, registered.facts));
        const auto parent = ParentChain(rig, source, adopted);
        rig.fault->writes = 0; rig.fault->fail_at = 1;
        Confirm(rig, registered, parent);
        const auto view = rig.coordinator->GetOwnedJob(registered.facts->owner, registered.facts->job_id);
        REQUIRE(view.dispatched_receipt); CHECK_FALSE(view.gap.empty());
        CHECK(view.dispatched_receipt->status == v3::WriteReceipt::Status::Rejected);
        CHECK(view.dispatched_receipt->error_code == "v3writer.injected"); CHECK_FALSE(view.started_receipt);
        const auto before = Bytes(rig.journal);
        for (unsigned n = 0; n < 3; ++n) rig.coordinator->PumpOwnedJobs();
        CHECK(Bytes(rig.journal) == before); CHECK(rig.fault->writes == 1);
        CHECK(rig.counts->thread.load() == 0); CHECK_FALSE(fs::exists(rig.cwd / "not-dispatched.started"));
        CHECK(rig.quota->running.load() == 0);
    }
    // Real native writes: terminal, persisted, Post-completed, Observed.
    // Hook dispatch/start are writes 3/4; its genuine completion is write 5.
    for (const unsigned stage : {1u, 2u, 5u, 6u}) {
        Rig rig("settlement-fault-" + std::to_string(stage)); Probe probe(rig.root.path);
        auto source = rig.Declare(probe.Input(rig.cwd, "complete", 32, 0, true)); const auto registered = Register(rig, source);
        const auto adopted = Adopt(rig, registered, Capability(rig, registered.facts));
        Confirm(rig, registered, ParentChain(rig, source, adopted)); REQUIRE(AwaitFile(rig.cwd / "complete.started"));
        rig.fault->writes = 0; rig.fault->fail_at = stage;
        Release(rig.cwd, "complete");
        const auto view = Finished(rig, registered);
        CHECK(view.execution_state == "succeeded"); CHECK_FALSE(view.gap.empty()); REQUIRE(view.terminal_receipt);
        std::optional<v3::WriteReceipt> native;
        if (stage == 1) native = view.terminal_receipt;
        if (stage == 2) native = view.persisted_receipt;
        if (stage == 5) native = view.post_receipt;
        if (stage == 6) native = view.observed_receipt;
        REQUIRE(native); CHECK(native->status == v3::WriteReceipt::Status::Rejected);
        CHECK(native->error_code == "v3writer.injected"); CHECK(rig.writer->broken());
        if (stage > 1) Committed(*view.terminal_receipt);
        if (stage > 2) { REQUIRE(view.persisted_receipt); Committed(*view.persisted_receipt); }
        if (stage > 5) { REQUIRE(view.post_receipt); Committed(*view.post_receipt); }
        CHECK(Bytes(rig.cwd / "complete.done") == "complete"); CHECK(rig.counts->post.load() == (stage >= 5 ? 1 : 0));
        const auto before = Bytes(rig.journal);
        for (unsigned n = 0; n < 4; ++n) rig.coordinator->PumpOwnedJobs();
        CHECK(Bytes(rig.journal) == before); CHECK(rig.fault->writes == stage);
        const auto ledger = Read(rig.journal);
        CHECK(Events(ledger, v3::EventKindV3::ToolExecutionFinished, registered.facts->action_id) == (stage > 1 ? 1 : 0));
        (void)rig.coordinator->Shutdown(); CHECK(rig.coordinator->shutdown_complete()); probe.Released();
    }
    {
        // Execute a real command first, then lose its executor report. A
        // thrown callback is not proof that the command never took effect.
        class ThrowAfterCommand : public RunCommandTool {
        public:
            std::optional<Tool::Result> actual_raw;
            Result execute(const Json& input, const ToolExecutionContext& context) override {
                actual_raw = RunCommandTool::execute(input, context);
                throw std::runtime_error("report lost after real command");
            }
        };
        Rig rig("executor-unknown"); Probe probe(rig.root.path);
        auto source = rig.Declare(probe.Input(rig.cwd, "already-executed"));
        const auto registered = Register(rig, source);
        auto capability = Capability(rig, registered.facts);
        auto command = std::make_shared<ThrowAfterCommand>(); capability.command = command;
        const auto adopted = Adopt(rig, registered, std::move(capability));
        Confirm(rig, registered, ParentChain(rig, source, adopted));
        const auto view = Finished(rig, registered);
        REQUIRE(command->actual_raw.has_value()); CHECK_FALSE(command->actual_raw->is_error);
        CHECK(command->actual_raw->details.at("timeout_ms").get<std::uint64_t>() == 15000);
        CHECK(command->actual_raw->details.at("max_output_bytes").get<std::uint64_t>() == 1024);
        CHECK(Bytes(rig.cwd / "already-executed.done") == "already-executed");
        CHECK(view.execution_state == "unknown"); CHECK(view.state == "unknown"); CHECK_FALSE(view.gap.empty());
        REQUIRE(view.terminal_receipt); Committed(*view.terminal_receipt);
        CHECK_FALSE(view.persisted_receipt); CHECK_FALSE(view.post_receipt); CHECK_FALSE(view.observed_receipt);
        CHECK(rig.counts->post.load() == 0);
        const auto ledger = Read(rig.journal);
        CHECK(Events(ledger, v3::EventKindV3::ToolExecutionUnknown, registered.facts->action_id) == 1);
        CHECK(Events(ledger, v3::EventKindV3::ToolExecutionFinished, registered.facts->action_id) == 0);
        CHECK(Events(ledger, v3::EventKindV3::ToolResultPersisted, registered.facts->action_id) == 0);
        const auto before = Bytes(rig.journal);
        for (unsigned n = 0; n < 3; ++n) rig.coordinator->PumpOwnedJobs();
        CHECK(Bytes(rig.journal) == before);
        (void)rig.coordinator->Shutdown(); CHECK(rig.coordinator->shutdown_complete()); probe.Released();
    }
    for (unsigned phase = 0; phase < 3; ++phase) {
        Rig rig("recovery-" + std::to_string(phase)); Probe probe(rig.root.path);
        auto source = rig.Declare(probe.Input(rig.cwd, "recover")); const auto registered = Register(rig, source);
        if (phase > 0) {
            const auto adopted = Adopt(rig, registered, Capability(rig, registered.facts));
            if (phase == 2) { Confirm(rig, registered, ParentChain(rig, source, adopted)); (void)Finished(rig, registered); }
        }
        // Preserve the exact real prefix before clean shutdown adds new
        // cancellation facts. This is a crash-window copy, not invented rows.
        const auto snapshot = rig.root.path / "crash-snapshot.jsonl";
        const auto bytes = Bytes(rig.journal);
        {
            std::ofstream output(snapshot, std::ios::binary);
            REQUIRE(output.is_open()); output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
            output.flush(); REQUIRE(output.good());
        }
        CHECK(Bytes(snapshot) == bytes); const auto ledger = Read(snapshot);
        const auto folded = v3::FoldJobExecutions(ledger);
        REQUIRE(folded.size() == 1);
        if (phase == 0) {
            CHECK_FALSE(folded.front().owned_adoption.has_value());
            CHECK(folded.front().state == "registered");
        } else {
            REQUIRE(folded.front().owned_adoption.has_value());
            CHECK(folded.front().owned_adoption->attempt == 1);
            CHECK(folded.front().owned_adoption->job_id == registered.facts->job_id);
            if (phase == 1) CHECK(folded.front().state == "parent_delivery_pending");
            else {
                CHECK(folded.front().state == "succeeded");
                CHECK(folded.front().dispatched);
            }
        }
        REQUIRE(rig.coordinator->Shutdown()); rig.coordinator.reset();
        const auto source_closed = rig.writer->Close();
        REQUIRE_MESSAGE(source_closed.has_value(), (source_closed ? std::string() : source_closed.error()));
        for (const auto policy : {JobRecoveryPolicy::Legacy, JobRecoveryPolicy::Hold}) {
            auto writer = v3::V3Writer::Continue(snapshot); REQUIRE(writer.has_value());
            unsigned executions = 0;
            ToolJobCoordinator recovered(*writer, [](const auto&, const auto&) { return JobAuthDecision{true, false, {}}; },
                [&executions](const auto&) { ++executions; return Tool::Result{"must not run", false}; });
            const auto plan = ToolJobCoordinator::PlanRecovery(ledger, policy);
            REQUIRE(plan.items.size() == 1);
            if (phase == 2) { REQUIRE(plan.items.front().recovery.has_value()); CHECK(plan.items.front().recovery->execution_attempt == 1); }
            CHECK(recovered.AdoptRecovery(plan) == (phase == 0 ? 0 : 1)); recovered.PumpCompletions();
            CHECK(executions == 0); CHECK(recovered.running_count() == 0); CHECK(recovered.queued_count() == 0);
            REQUIRE(recovered.Shutdown()); CHECK(Bytes(snapshot) == bytes);
            const auto recovered_closed = writer->Close();
            REQUIRE_MESSAGE(recovered_closed.has_value(), (recovered_closed ? std::string() : recovered_closed.error()));
        }
        probe.Released();
    }
    Mark("native-gap-hold");
}
