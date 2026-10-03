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
#include <type_traits>
#include <utility>
#include <vector>

#include "agent/loop.hpp"
#include "hooks/hash.hpp"
#include "platform/paths.hpp"
#include "runtime/middleware_v3_sink.hpp"
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
            const auto candidate = fs::temp_directory_path() / ("lubancode-post-live-" + tag + "-" +
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
bool AwaitStarted(const fs::path& cwd, const std::string& tag) {
    const auto deadline = std::chrono::steady_clock::now() + 10s;
    const std::string prefix = tag + "\n";
    while (std::chrono::steady_clock::now() < deadline) {
        std::ifstream stream(cwd / (tag + ".started"), std::ios::binary);
        if (stream.is_open()) {
            const std::string bytes((std::istreambuf_iterator<char>(stream)), {});
            if (!stream.bad() && bytes.starts_with(prefix)) {
                const auto path = fs::u8path(bytes.substr(prefix.size()));
                std::error_code error;
                if (path.is_absolute() && fs::equivalent(path, cwd, error) && !error) return true;
            }
        }
        std::this_thread::sleep_for(5ms);
    }
    return false;
}
void Release(const fs::path& cwd, const std::string& tag) {
    std::ofstream stream(cwd / (tag + ".release"), std::ios::binary);
    REQUIRE(stream.is_open()); stream.put('1'); stream.flush(); REQUIRE(stream.good());
}
void Mark(const char* name) {
    std::fprintf(stderr, "[job-post-live-invocation-path] %s\n", name);
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

struct PostValues {
    OwnedJobPostInvocation saved;
    std::optional<OwnedJobPostSnapshot> snapshot;
    unsigned callbacks = 0;
};
using ExtraPost = std::function<void(ToolJobCoordinator&, const OwnedJobCompletion&,
                                    const OwnedJobPostInvocation&)>;
OwnedJobCapability LiveCapability(Rig& rig, std::shared_ptr<const PreparedJobFacts> facts,
                                  std::shared_ptr<PostValues> values, ExtraPost extra = {}) {
    auto capability = Capability(rig, facts);
    auto original = std::move(capability.post);
    capability.post = {};
    capability.live_post = [weak = std::weak_ptr<ToolJobCoordinator>(rig.coordinator), facts,
        values = std::move(values), extra = std::move(extra), original = std::move(original)]
        (const OwnedJobCompletion& completion, const OwnedJobPostInvocation& invocation) {
        const auto owner = weak.lock(); REQUIRE(owner);
        const auto checked = owner->CheckOwnedPostInvocation(invocation);
        REQUIRE_MESSAGE(checked.has_value(), (checked ? std::string() : checked.error()));
        ++values->callbacks; values->saved = invocation; values->snapshot = *checked;
        CHECK(checked->facts == facts); CHECK(checked->completion.scope.owner == facts->owner);
        CHECK(checked->completion.scope.job_id == facts->job_id);
        CHECK(checked->completion.scope.action_id == facts->action_id);
        CHECK(checked->completion.scope.attempt == 1);
        CHECK(checked->completion.raw.content == completion.raw.content);
        Committed(checked->adopted_receipt);
        Committed(checked->completion.started_receipt);
        Committed(checked->completion.terminal_receipt);
        Committed(checked->completion.persisted_receipt);
        if (extra) extra(*owner, completion, invocation);
        return original(completion);
    };
    return capability;
}
void Invalid(ToolJobCoordinator& coordinator, const OwnedJobPostInvocation& handle,
             const std::string& code = "job.post.invalid_invocation") {
    const auto checked = coordinator.CheckOwnedPostInvocation(handle);
    REQUIRE_FALSE(checked.has_value()); CHECK(checked.error() == code);
}
void SameReceipt(const v3::WriteReceipt& a, const v3::WriteReceipt& b) {
    CHECK(a.status == b.status); CHECK(a.id == b.id); CHECK(a.seq == b.seq);
    CHECK(a.line_hash == b.line_hash); CHECK(a.error_code == b.error_code);
    CHECK(a.error_message == b.error_message);
}
void ActualReferences(Rig& rig, const OwnedJobPostSnapshot& snapshot) {
    const auto ledger = Read(rig.journal);
    for (const auto& receipt : {snapshot.adopted_receipt, snapshot.completion.started_receipt,
                               snapshot.completion.terminal_receipt, snapshot.completion.persisted_receipt}) {
        const auto* event = ledger.FindEvent(receipt.id); REQUIRE(event);
        CHECK(event->session_id == snapshot.completion.scope.owner.session_id);
        CHECK(event->run_id == snapshot.completion.scope.owner.run_id);
        CHECK(event->action_id == snapshot.completion.scope.action_id);
        CHECK(event->turn_id == snapshot.completion.scope.turn_id);
        CHECK(event->step_id == snapshot.completion.scope.step_id);
        CHECK(event->seq == receipt.seq); CHECK(event->line_hash == receipt.line_hash);
    }
}
void AwaitFinishedWithoutPump(Rig& rig, const PreparedJobRegistration& registered) {
    const auto deadline = std::chrono::steady_clock::now() + 10s;
    while (std::chrono::steady_clock::now() < deadline) {
        if (rig.coordinator->SnapshotOwnedJob(registered.facts->owner,
                registered.facts->job_id).worker_finished) return;
        std::this_thread::sleep_for(5ms);
    }
    FAIL("actual owned command did not finish before host settlement");
}
void NoQueryEffects(Rig& rig, const PreparedJobRegistration& registered) {
    const auto before = Bytes(rig.journal);
    const auto threads = rig.counts->thread.load(), posts = rig.counts->post.load();
    const auto clocks = rig.counts->legacy_clock.load(), gates = rig.counts->scope.load();
    for (unsigned n = 0; n < 3; ++n) {
        const auto view = rig.coordinator->SnapshotOwnedJob(registered.facts->owner, registered.facts->job_id);
        CHECK_FALSE(view.access_denied); CHECK(view.scope.owner == registered.facts->owner);
    }
    CHECK(Bytes(rig.journal) == before); CHECK(rig.counts->thread.load() == threads);
    CHECK(rig.counts->post.load() == posts); CHECK(rig.counts->legacy_clock.load() == clocks);
    CHECK(rig.counts->scope.load() == gates);
}
void Healthy(const OwnedJobStatusView& view) {
    CHECK(view.state == "succeeded"); CHECK(view.execution_state == "succeeded"); CHECK(view.gap.empty());
    REQUIRE(view.started_receipt); REQUIRE(view.terminal_receipt); REQUIRE(view.persisted_receipt);
    REQUIRE(view.post_receipt); REQUIRE(view.observed_receipt);
    Committed(*view.post_receipt); Committed(*view.observed_receipt);
}
void CloseWriter(Rig& rig) {
    const auto closed = rig.writer->Close();
    REQUIRE_MESSAGE(closed.has_value(), (closed ? std::string() : closed.error()));
}
} // namespace

TEST_CASE("Job Post live invocation: legacy" "[unit][tools][tool_job_post_live_invocation]") {
    Watchdog watchdog;
    Rig rig("legacy"); Probe probe(rig.root.path);
    auto source = rig.Declare(probe.Input(rig.cwd, "legacy"));
    const auto registered = Register(rig, source);
    Invalid(*rig.coordinator, OwnedJobPostInvocation{});
    // Both/neither fail before gate or adoption append. The default old post
    // remains the actual native HookCompleted producer, not a compatibility fake.
    for (const bool both : {false, true}) {
        auto cap = Capability(rig, registered.facts);
        if (both) cap.live_post = [](const auto&, const auto&) { return v3::WriteReceipt{}; };
        else cap.post = {};
        const auto before = Bytes(rig.journal);
        const auto gates = rig.counts->scope.load();
        const auto rejected = rig.coordinator->AdoptPreparedJob(registered.facts->owner,
            registered.facts->job_id, std::move(cap));
        CHECK(rejected.state == OwnedJobAdoptionState::Rejected);
        CHECK(rejected.error_code == "job.owned.missing_capability");
        CHECK_FALSE(rejected.receipt); CHECK(Bytes(rig.journal) == before);
        CHECK(rig.counts->scope.load() == gates); CHECK(rig.counts->thread.load() == 0);
    }
    const auto adopted = Adopt(rig, registered, Capability(rig, registered.facts));
    Confirm(rig, registered, ParentChain(rig, source, adopted));
    Healthy(Finished(rig, registered)); CHECK(rig.counts->post.load() == 1);
    CHECK(rig.counts->scope.load() == 3); NoQueryEffects(rig, registered);
    REQUIRE(rig.coordinator->Shutdown()); CloseWriter(rig); probe.Released();
    Mark("legacy");
}

TEST_CASE("Job Post live invocation: actual" "[unit][tools][tool_job_post_live_invocation]") {
    Watchdog watchdog;
    static_assert(!std::is_constructible_v<OwnedJobPostInvocation, OwnedJobCompletion>);
    static_assert(!std::is_constructible_v<OwnedJobPostInvocation, PreparedJobOwner>);
    Rig rig("actual"); Probe probe(rig.root.path);
    const auto values = std::make_shared<PostValues>();
    auto source = rig.Declare(probe.Input(rig.cwd, "actual"));
    const auto registered = Register(rig, source);
    const auto adopted = Adopt(rig, registered, LiveCapability(rig, registered.facts, values,
        [](auto& coordinator, const auto&, const auto& handle) {
            const auto first = coordinator.CheckOwnedPostInvocation(handle); REQUIRE(first.has_value());
            CHECK(first->phase == OwnedJobPostPhase::Open);
            const auto second = coordinator.CheckOwnedPostInvocation(handle); REQUIRE(second.has_value());
            SameReceipt(first->completion.terminal_receipt, second->completion.terminal_receipt);
        }));
    Confirm(rig, registered, ParentChain(rig, source, adopted));
    Healthy(Finished(rig, registered)); REQUIRE(values->snapshot);
    CHECK(values->callbacks == 1); ActualReferences(rig, *values->snapshot);
    Invalid(*rig.coordinator, values->saved);
    NoQueryEffects(rig, registered); CHECK(rig.counts->post.load() == 1);
    REQUIRE(rig.coordinator->Shutdown()); Invalid(*rig.coordinator, values->saved);
    CloseWriter(rig); probe.Released(); Mark("actual");
}

TEST_CASE("Job Post live invocation: isolation" "[unit][tools][tool_job_post_live_invocation]") {
    Watchdog watchdog;
    Rig a("isolation-a"); Rig b("isolation-b", a.cwd);
    Rig c("isolation-c", {}, "project-other"); Rig d("isolation-d", c.cwd, "project-other");
    Probe probe(a.root.path); Probe other_probe(c.root.path);
    const auto first = std::make_shared<PostValues>(), next = std::make_shared<PostValues>();
    auto source_a = a.Declare(probe.Input(a.cwd, "isolation-a"));
    const auto registered_a = Register(a, source_a);
    const auto adopted_a = Adopt(a, registered_a, LiveCapability(a, registered_a.facts, first,
        [peer = std::weak_ptr<ToolJobCoordinator>(b.coordinator)]
        (auto& coordinator, const auto&, const auto& handle) {
            const auto foreign = peer.lock(); REQUIRE(foreign); Invalid(*foreign, handle);
            // Caller owns serial while waiting. The foreign-thread Check must
            // reject before trying it; this is a real joined observer thread.
            auto observer = std::async(std::launch::async, [&coordinator, handle] {
                return coordinator.CheckOwnedPostInvocation(handle);
            });
            REQUIRE(observer.wait_for(1s) == std::future_status::ready);
            const auto result = observer.get(); REQUIRE_FALSE(result.has_value());
            CHECK(result.error() == "job.post.invalid_invocation");
        }));
    Confirm(a, registered_a, ParentChain(a, source_a, adopted_a)); Healthy(Finished(a, registered_a));
    auto source_b = b.Declare(probe.Input(b.cwd, "isolation-b"));
    const auto registered_b = Register(b, source_b);
    const auto adopted_b = Adopt(b, registered_b, LiveCapability(b, registered_b.facts, next,
        [first](auto& coordinator, const auto&, const auto& handle) {
            Invalid(coordinator, first->saved); Invalid(coordinator, OwnedJobPostInvocation{});
            REQUIRE(coordinator.CheckOwnedPostInvocation(handle).has_value());
        }));
    Confirm(b, registered_b, ParentChain(b, source_b, adopted_b)); Healthy(Finished(b, registered_b));
    // A second real Post in the same coordinator cannot revive A's first token.
    auto source_again = a.Declare(probe.Input(a.cwd, "isolation-again"));
    const auto again = Register(a, source_again); const auto last = std::make_shared<PostValues>();
    const auto adopted_again = Adopt(a, again, LiveCapability(a, again.facts, last,
        [first](auto& coordinator, const auto&, const auto& handle) {
            Invalid(coordinator, first->saved);
            REQUIRE(coordinator.CheckOwnedPostInvocation(handle).has_value());
        }));
    Confirm(a, again, ParentChain(a, source_again, adopted_again)); Healthy(Finished(a, again));
    REQUIRE(first->snapshot); REQUIRE(next->snapshot); REQUIRE(last->snapshot);
    CHECK(first->snapshot->completion.scope.owner.coordinator_id !=
          next->snapshot->completion.scope.owner.coordinator_id);
    CHECK(first->snapshot->completion.scope.job_id != last->snapshot->completion.scope.job_id);
    ActualReferences(a, *first->snapshot); ActualReferences(b, *next->snapshot); ActualReferences(a, *last->snapshot);
    CHECK(first->callbacks == 1); CHECK(next->callbacks == 1); CHECK(last->callbacks == 1);
    for (auto* other : std::array<Rig*, 2>{&c, &d}) {
        const auto values = std::make_shared<PostValues>();
        auto declared = other->Declare(other_probe.Input(other->cwd, other == &c ? "isolation-c" : "isolation-d"));
        const auto registered = Register(*other, declared);
        const auto adopted = Adopt(*other, registered, LiveCapability(*other, registered.facts, values,
            [peer = std::weak_ptr<ToolJobCoordinator>(a.coordinator), first]
            (auto& coordinator, const auto&, const auto& handle) {
                const auto foreign = peer.lock(); REQUIRE(foreign); Invalid(*foreign, handle);
                Invalid(coordinator, first->saved);
                REQUIRE(coordinator.CheckOwnedPostInvocation(handle).has_value());
            }));
        Confirm(*other, registered, ParentChain(*other, declared, adopted)); Healthy(Finished(*other, registered));
        REQUIRE(values->snapshot); CHECK(values->callbacks == 1);
        CHECK(values->snapshot->completion.scope.owner.project_id == "project-other");
        CHECK(values->snapshot->completion.scope.owner.project_id != first->snapshot->completion.scope.owner.project_id);
        ActualReferences(*other, *values->snapshot); Invalid(*other->coordinator, values->saved);
    }
    REQUIRE(a.coordinator->Shutdown()); REQUIRE(b.coordinator->Shutdown());
    REQUIRE(c.coordinator->Shutdown()); REQUIRE(d.coordinator->Shutdown());
    CloseWriter(a); CloseWriter(b); CloseWriter(c); CloseWriter(d);
    probe.Released(); other_probe.Released(); Mark("isolation");
}

TEST_CASE("Job Post live invocation: drain" "[unit][tools][tool_job_post_live_invocation]") {
    Watchdog watchdog;
    for (const bool close_inside : {false, true}) {
        Rig rig(close_inside ? "drain-inside" : "drain-owner"); Probe probe(rig.root.path);
        const auto values = std::make_shared<PostValues>();
        auto source = rig.Declare(probe.Input(rig.cwd, "drain", 32, 100));
        const auto request = rig.Prepare(source);
        const auto registered = Register(rig, source);
        const auto adopted = Adopt(rig, registered, LiveCapability(rig, registered.facts, values,
            [close_inside](auto& coordinator, const auto& completion, const auto& handle) {
                if (close_inside) coordinator.RequestShutdown();
                const auto snapshot = coordinator.CheckOwnedPostInvocation(handle); REQUIRE(snapshot.has_value());
                CHECK(snapshot->phase == OwnedJobPostPhase::Draining);
                CHECK(snapshot->completion.scope.job_id == completion.scope.job_id);
                CHECK_FALSE(coordinator.Shutdown()); // Actual self-join rejection, not a fake Close success.
                const auto after = coordinator.CheckOwnedPostInvocation(handle); REQUIRE(after.has_value());
                CHECK(after->phase == OwnedJobPostPhase::Draining);
            }));
        Confirm(rig, registered, ParentChain(rig, source, adopted));
        rig.coordinator->PumpOwnedJobs(); REQUIRE(AwaitStarted(rig.cwd, "drain"));
        AwaitFinishedWithoutPump(rig, registered);
        REQUIRE(Bytes(rig.cwd / "drain.done") == "drain");
        const auto before = Bytes(rig.journal); CHECK(values->callbacks == 0);
        NoQueryEffects(rig, registered); CHECK(Bytes(rig.journal) == before);
        if (close_inside) rig.coordinator->PumpOwnedJobs();
        REQUIRE(rig.coordinator->Shutdown());
        Healthy(rig.coordinator->SnapshotOwnedJob(registered.facts->owner, registered.facts->job_id));
        CHECK(values->callbacks == 1); CHECK(rig.counts->thread.load() == 1); CHECK(rig.counts->post.load() == 1);
        Invalid(*rig.coordinator, values->saved); NoQueryEffects(rig, registered);
        // Retired never admits another job or signs another token.
        auto refused = rig.coordinator->RegisterPreparedJob(request);
        CHECK(refused.state == PreparedJobRegistrationState::Rejected);
        CHECK(refused.error_code == "job.prepared.closed");
        CloseWriter(rig); probe.Released();
    }
    Mark("drain");
}

TEST_CASE("Job Post live invocation: gap" "[unit][tools][tool_job_post_live_invocation]") {
    Watchdog watchdog;
    for (const unsigned fault : {0U, 1U, 2U}) {
        Rig rig("gap-" + std::to_string(fault)); Probe probe(rig.root.path);
        const auto values = std::make_shared<PostValues>();
        auto source = rig.Declare(probe.Input(rig.cwd, "gap")); const auto registered = Register(rig, source);
        auto cap = LiveCapability(rig, registered.facts, values);
        cap.live_post = [weak = std::weak_ptr<ToolJobCoordinator>(rig.coordinator), values,
            writer = rig.writer, io = rig.fault, fault]
            (const OwnedJobCompletion& completion, const OwnedJobPostInvocation& handle) -> v3::WriteReceipt {
            const auto coordinator = weak.lock(); REQUIRE(coordinator);
            const auto checked = coordinator->CheckOwnedPostInvocation(handle); REQUIRE(checked.has_value());
            ++values->callbacks; values->saved = handle; values->snapshot = *checked;
            if (fault == 1) throw std::runtime_error("actual Post callback failure");
            REQUIRE(fault == 0);
            io->writes = 0; io->fail_at = 1;
            v3::EventDraft event; event.kind = v3::EventKindV3::ToolJobCancelRequested;
            event.turn_id = completion.scope.turn_id; event.step_id = completion.scope.step_id;
            event.action_id = completion.scope.action_id;
            event.payload = {{"tool_call_id", completion.scope.action_id}, {"jobId", completion.scope.job_id},
                             {"reason", "actual Post writer fault"}};
            const auto receipt = writer->AppendEvent(std::move(event), v3::Durability::PowerLoss);
            CHECK(receipt.status == v3::WriteReceipt::Status::Rejected); CHECK(writer->broken());
            Invalid(*coordinator, handle, "job.post.writer_unavailable");
            return receipt;
        };
        const auto adopted = Adopt(rig, registered, std::move(cap));
        Confirm(rig, registered, ParentChain(rig, source, adopted));
        rig.coordinator->PumpOwnedJobs(); AwaitFinishedWithoutPump(rig, registered);
        if (fault == 2) CloseWriter(rig); // A true closed writer, not a fabricated failed receipt.
        const auto view = Finished(rig, registered);
        CHECK(view.state == "unknown"); CHECK_FALSE(view.gap.empty()); CHECK_FALSE(view.observed_receipt);
        CHECK(values->callbacks == (fault == 2 ? 0U : 1U));
        if (fault != 2) {
            CHECK(view.execution_state == "succeeded"); REQUIRE(values->snapshot);
            CHECK_FALSE(values->snapshot->completion.raw.is_error);
            ActualReferences(rig, *values->snapshot); Invalid(*rig.coordinator, values->saved);
        }
        const auto before = Bytes(rig.journal); const auto count = values->callbacks;
        rig.coordinator->PumpOwnedJobs(); rig.coordinator->GetOwnedJob(registered.facts->owner, registered.facts->job_id);
        CHECK(Bytes(rig.journal) == before); CHECK(values->callbacks == count);
        CHECK_FALSE(rig.coordinator->Shutdown()); CHECK(rig.coordinator->shutdown_complete());
        CHECK(Bytes(rig.journal) == before); Invalid(*rig.coordinator, values->saved); probe.Released();
    }
    Mark("gap");
}

TEST_CASE("Job Post live invocation: passive" "[unit][tools][tool_job_post_live_invocation]") {
    Watchdog watchdog;
    Rig rig("passive"); Probe probe(rig.root.path); const auto values = std::make_shared<PostValues>();
    auto source = rig.Declare(probe.Input(rig.cwd, "passive")); const auto registered = Register(rig, source);
    const auto adopted = Adopt(rig, registered, LiveCapability(rig, registered.facts, values));
    Confirm(rig, registered, ParentChain(rig, source, adopted)); Healthy(Finished(rig, registered));
    REQUIRE(values->snapshot); ActualReferences(rig, *values->snapshot);
    REQUIRE(rig.coordinator->Shutdown()); CloseWriter(rig);
    const auto original = Bytes(rig.journal); const auto ledger = Read(rig.journal);
    const auto original_run = rig.writer->run_id();
    for (const auto policy : {JobRecoveryPolicy::Legacy, JobRecoveryPolicy::Hold}) {
        auto continued = v3::V3Writer::Continue(rig.journal);
        REQUIRE_MESSAGE(continued.has_value(), (continued ? std::string() : continued.error()));
        CHECK(continued->run_id() == original_run);
        unsigned executions = 0;
        ToolJobCoordinator recovered(*continued, [](const auto&, const auto&) { return JobAuthDecision{true, false, {}}; },
            [&executions](const auto&) { ++executions; return Tool::Result{"must not execute", false}; });
        const auto plan = ToolJobCoordinator::PlanRecovery(ledger, policy); REQUIRE(plan.items.size() == 1);
        CHECK(recovered.AdoptRecovery(plan) == 1); recovered.PumpCompletions();
        // The historical raw/terminal still satisfy the neutral material
        // factory after Continue kept the run. This does not revive live Post.
        {
            lubancode::hooks::middleware::MiddlewarePool pool;
            const auto registry = pool.Publish(); REQUIRE(registry.has_value());
            const auto& completion = values->snapshot->completion;
            const auto terminal = v3::MakeOwnedJobReference(ledger, completion.terminal_receipt.id);
            const auto raw = v3::MakeOwnedJobReference(ledger, completion.persisted_receipt.id);
            REQUIRE(terminal.has_value()); REQUIRE(raw.has_value());
            runtime::MiddlewareReceiptScope scope{continued->session_id(), continued->run_id(),
                completion.scope.turn_id, completion.scope.step_id, completion.scope.action_id,
                1, (*registry)->revision(), *terminal, *raw, 256};
            auto material = runtime::CaptureMiddlewareReceipts(*continued, std::move(scope));
            REQUIRE_MESSAGE(material.has_value(), (material ? std::string() : material.error()));
            const auto saved = material->lease.Snapshot();
            CHECK(saved.scope.terminal_ref == *terminal); CHECK(saved.scope.persisted_ref == *raw);
            CHECK(saved.receipts.empty()); CHECK_FALSE(saved.finished);
            CHECK(Bytes(rig.journal) == original);
            material->sink.reset(); material->lease.Close();
        }
        Invalid(recovered, values->saved); Invalid(recovered, OwnedJobPostInvocation{});
        CHECK(recovered.SnapshotOwnedJob(registered.facts->owner, registered.facts->job_id).access_denied);
        CHECK(executions == 0); CHECK(recovered.running_count() == 0); CHECK(recovered.queued_count() == 0);
        REQUIRE(recovered.Shutdown()); CHECK(Bytes(rig.journal) == original);
        const auto closed = continued->Close(); REQUIRE(closed.has_value());
        CHECK(Bytes(rig.journal) == original); CHECK(values->callbacks == 1);
    }
    probe.Released(); Mark("passive");
}
