#include <doctest/doctest.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <iterator>
#include <memory>
#include <new>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>
#include <nlohmann/json.hpp>
#include "api/backend.hpp"
#include "platform/atomic_write.hpp"
#include "platform/paths.hpp"
#include "platform/process.hpp"
#include "platform/sha256.hpp"
#include "runtime/assembly/session_resources.hpp"
#include "runtime/async_tool_runtime.hpp"
#include "runtime/scoped_turn_bindings.hpp"
#include "runtime/session_service.hpp"
#include "runtime/tool_trace_hub.hpp"
#include "trajectory/canonical_json.hpp"
#include "trajectory/cas_store.hpp"
#include "trajectory/managed_session_reservation.hpp"
#include "trajectory/named_result_blobs.hpp"
#include "trajectory/v3/reader.hpp"
#include "workspace/identity.hpp"

namespace {
namespace fs = std::filesystem;
namespace rt = lubancode::runtime;
namespace traj = lubancode::trajectory;
namespace v3 = traj::v3;
namespace api = lubancode::api;
namespace platform = lubancode::platform;
namespace agent = lubancode::agent;
using Json = nlohmann::json;
using Receipt = rt::SessionService::ManagedWriteReceipt;
using Knowledge = Receipt::Knowledge;
using State = rt::ManagedStoredOperation::State;
std::string Read(const fs::path& path) {
    std::ifstream input(platform::FileIoPath(path), std::ios::binary);
    REQUIRE(input.is_open());
    std::string out{std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
    REQUIRE_FALSE(input.bad()); return out;
}
void Mark(const char* path) { std::cout << "[managed-operation-execution-path] " << path << '\n'; }
struct NativeProbe final : traj::JournalNativeIoProbe {
    bool arm = false, fired = false, close_seen = false;
    unsigned close_count = 0;
    bool After(const traj::JournalNativeIoResult& actual) noexcept override {
        if (actual.stage == traj::JournalNativeStage::Close) {
            close_seen = actual.attempted && actual.succeeded;
            if (actual.attempted) ++close_count;
        }
        if (arm && actual.stage == traj::JournalNativeStage::FileSync && actual.attempted && actual.succeeded) {
            arm = false; fired = true; return true;
        }
        return false;
    }
};
struct AllocationProbe final : rt::ManagedCloseAllocationProbe {
    rt::ManagedCloseAllocationStage target;
    bool capture_thrown = false, target_thrown = false;
    explicit AllocationProbe(rt::ManagedCloseAllocationStage stage) : target(stage) {}
    void Before(rt::ManagedCloseAllocationStage stage) override {
        if (target == rt::ManagedCloseAllocationStage::BeforeFailureDiagnostic &&
            stage == rt::ManagedCloseAllocationStage::BeforeMaterialCapture && !capture_thrown) {
            capture_thrown = true; throw std::bad_alloc();
        }
        if (stage == target && !target_thrown) { target_thrown = true; throw std::bad_alloc(); }
    }
};
struct ReentrantCloseState {
    bool called = false, shutdown_rejected = false, close_rejected = false;
    bool writer_still_open = false, lock_still_owned = false, failed = false;
};
struct ReentrantCloseCapture {
    rt::SessionService* service;
    fs::path lock_path;
    std::shared_ptr<ReentrantCloseState> state;
    ReentrantCloseCapture(rt::SessionService* value, fs::path path, std::shared_ptr<ReentrantCloseState> observed)
        : service(value), lock_path(std::move(path)), state(std::move(observed)) {}
    ~ReentrantCloseCapture() noexcept {
        state->called = true;
        try {
            state->shutdown_rejected = !service->ShutdownExecution();
            const auto rejected = service->Close("async-capture-close-reentry");
            state->close_rejected = rejected.error_code == "close.reentrant";
            state->writer_still_open = !service->trajectory()->v3_main_writer()->closed();
            std::error_code error; state->lock_still_owned = fs::exists(platform::FileIoPath(lock_path), error) && !error;
        } catch (...) { state->failed = true; }
    }
};
struct ModelState {
    unsigned calls = 0;
    bool alive = false, fail = false;
    std::vector<api::Request> requests;
    std::function<void()> on_send;
};
struct Backend final : api::Backend {
    std::shared_ptr<ModelState> state;
    explicit Backend(std::shared_ptr<ModelState> value) : state(std::move(value)) { state->alive = true; }
    ~Backend() override { state->alive = false; }
    std::expected<void, api::Error> send_stream(const api::Request& request,
        const std::function<void(const api::StreamEvent&)>& emit, const std::atomic<bool>*) override {
        ++state->calls; state->requests.push_back(request);
        if (state->on_send) state->on_send();
        if (state->fail) return std::unexpected(api::Error{api::ErrorKind::HttpStatus, "actual managed model failure", 400});
        emit(api::MessageStart{"managed-answer", request.model}); emit(api::TextDelta{"actual managed answer"});
        emit(api::ContentBlockDone{0}); emit(api::MessageDone{"end_turn", api::Usage{9, 3, 0, 0, 0}, true}); return {};
    }
};
struct Fixture {
    fs::path root, directory;
    rt::SessionLaunchRequest request;
    traj::ManagedSessionOwnership owner;
    std::shared_ptr<ModelState> model = std::make_shared<ModelState>();
    std::unique_ptr<rt::SessionService> service;
    explicit Fixture(bool text = true, std::shared_ptr<NativeProbe> main_probe = {}) {
        static std::atomic<unsigned> serial{0};
        root = fs::temp_directory_path() / ("managed-operation-execution-" + std::to_string(platform::CurrentProcessId()) + "-" +
            std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + "-" + std::to_string(++serial));
        REQUIRE(fs::create_directory(root)); root = fs::canonical(root);
        REQUIRE(fs::create_directory(root / "project"));
        request.workspace_identity = lubancode::workspace::MakeFallbackIdentity(root / "project");
        request.cwd_utf8 = platform::PathToUtf8(root / "project"); request.workspaces_root = root / "workspaces";
        request.v3_system_content = "managed text test system"; request.lubancode_version = "managed-execution-test";
        request.journal_native_io_probe = std::move(main_probe);
        auto workspace = traj::TrajectoryDirectory::CreateWorkspace(request.workspaces_root, *request.workspace_identity, 1759000000000LL);
        REQUIRE_MESSAGE(workspace.has_value(), (workspace ? "" : workspace.error()));
        owner = {"tenant-a", "project-a", request.workspace_identity->workspace_key, "20261007-120000-TEXT01", 1};
        auto reserved = traj::ManagedSessionReservation::Reserve(request.workspaces_root, owner, traj::SessionManagerClock{}.LockOwner());
        REQUIRE_MESSAGE(reserved.has_value(), (reserved ? "" : reserved.error()));
        directory = (*reserved)->session_dir();
        REQUIRE((*reserved)->PublishOwnership().knowledge == traj::ManagedSessionOwnershipPublication::Knowledge::Committed);
        auto admitted = (*reserved)->Finish(); REQUIRE_MESSAGE(admitted.has_value(), (admitted ? "" : admitted.error()));
        traj::ManagedSessionCreationAudit creation{"tenant-a", "creator", "user", "creator-credential", 10};
        if (text) service = std::make_unique<rt::SessionService>(request, std::move(*admitted), creation, traj::ManagedTextSessionLaunch{});
        else service = std::make_unique<rt::SessionService>(request, std::move(*admitted), creation);
        REQUIRE_MESSAGE(service->runtime() != nullptr, service->launch_error());
        REQUIRE(fs::exists(directory / "session.lock"));
    }
    ~Fixture() {
        platform::SetFileFlushFailureForTest(false); platform::SetDirectoryFlushFailureForTest(false);
        if (service) { (void)service->Close("test_fixture_cleanup"); service.reset(); }
        std::error_code error; fs::remove_all(platform::FileIoPath(root), error);
    }
    fs::path Main() const { return directory / (owner.session_id + ".jsonl"); }
    rt::ManagedOperationAdmission Admission(std::uint64_t revision = 11) const {
        return {owner, {"tenant-a", "initiator", "user", "initiator-credential"}, revision, {"RequestModel"}};
    }
    agent::AgentProfile Profile() const {
        agent::AgentProfile profile; profile.provider = "managed-probe"; profile.request.model = "managed-model";
        profile.system_prompt = request.v3_system_content; profile.runtime.max_steps_per_turn = 4;
        profile.runtime.max_output_tokens = 4096; return profile;
    }
    std::unique_ptr<rt::assembly::SessionResources> Resources() {
        rt::assembly::SessionResourcesRequest plan;
        plan.backend_factory = [value = model] { return std::make_unique<Backend>(value); };
        plan.registry_factory = [](std::span<const rt::assembly::McpServerRuntime>) -> rt::assembly::SessionRegistryResult {
            return std::make_unique<lubancode::tools::ToolRegistry>();
        };
        auto built = rt::assembly::BuildSessionResources(std::move(plan));
        REQUIRE_MESSAGE(built.has_value(), (built ? "" : built.error().code)); return std::move(*built);
    }
    void Assemble() { auto profile = Profile(); service->InitializeManagedTextExecution(Resources(), std::move(profile)); REQUIRE(service->execution() != nullptr); }
    std::shared_ptr<const Receipt> Accept(const std::string& key = "first-key", const std::string& text = "actual managed input") {
        auto accepted = service->SubmitManagedInput({key, text, {}}, Admission());
        REQUIRE(accepted->knowledge == Knowledge::Committed); REQUIRE(accepted->input.accepted);
        REQUIRE(accepted->artifact->outcome == platform::WriteOutcome::CommittedDurable);
        REQUIRE(accepted->append->status == traj::JournalAppendStatus::Committed); return accepted;
    }
    std::shared_ptr<const Receipt> Dispatch(const std::shared_ptr<const Receipt>& accepted) {
        auto dispatched = service->DispatchManagedPendingInput(accepted->operation->provenance, 12);
        REQUIRE_MESSAGE(dispatched->knowledge == Knowledge::Committed, dispatched->input.error_code);
        REQUIRE(dispatched->append->status == traj::JournalAppendStatus::Committed);
        REQUIRE(dispatched->binding_append->status == v3::WriteReceipt::Status::Committed);
        REQUIRE(dispatched->binding_append->journal_append->status == traj::JournalAppendStatus::Committed);
        REQUIRE(dispatched->binding->provenance_hash == accepted->operation->provenance.provenance_hash); return dispatched;
    }
    rt::ManagedOperationResult Run(const std::shared_ptr<const Receipt>& dispatched) {
        auto bridge = service->NewManagedTextTurnBridge(dispatched->operation->provenance, {"managed-probe", "chat", "managed-test", {}});
        REQUIRE(bridge != nullptr);
        rt::ToolTraceHub hub(service->runtime()->ids()); agent::TurnWiring wiring; wiring.turn_id = dispatched->operation->turn_id;
        rt::ScopedTurnBindings scope(service->execution()->agent());
        scope.Bind(wiring, {&hub, bridge.get(), nullptr, owner.session_id, wiring.turn_id, std::nullopt});
        bridge->BeginTurn(wiring.turn_id, "external_user");
        api::Message user{api::Role::User, {api::TextBlock{dispatched->operation->text}}}; bridge->RecordInput(user);
        const auto outcome = service->execution()->agent().Run(std::move(user), wiring);
        REQUIRE_MESSAGE(outcome.has_value(), (outcome ? "" : outcome.error()));
        REQUIRE_FALSE(outcome->cancelled); REQUIRE_FALSE(outcome->hit_step_limit); REQUIRE(outcome->steps_used == 1);
        bridge->EndTurn(true, false, {}); scope.Reset();
        REQUIRE_FALSE(bridge->last_committed_assistant_message_id().empty());
        REQUIRE(model->calls > 0); REQUIRE(model->requests.back().tools.empty());
        return {"success", "actual managed answer", {}, {bridge->last_committed_assistant_message_id()}, true, true};
    }
    rt::ManagedOperationMaterials Capture() {
        auto material = service->CaptureManagedOperationMaterials();
        REQUIRE_MESSAGE(material.has_value(), (material ? "" : material.error())); return std::move(*material);
    }
};
std::vector<Json> Rows(const std::string& bytes) {
    std::vector<Json> out; std::size_t offset = 0;
    while (offset < bytes.size()) { const auto end = bytes.find('\n', offset); REQUIRE(end != std::string::npos);
        out.push_back(Json::parse(bytes.substr(offset, end - offset))); offset = end + 1; }
    return out;
}
std::string Encode(const std::vector<Json>& rows) {
    std::string out; for (const auto& row : rows) { auto line = traj::CanonicalJsonDump(row);
        REQUIRE(line.has_value()); out += *line + "\n"; } return out;
}
void CheckIncompleteEnded(Fixture& fixture) {
    auto ledger = v3::ReadV3Ledger(fixture.Main()); REQUIRE_MESSAGE(ledger.has_value(), (ledger ? "" : ledger.error()));
    const auto ended = std::find_if(ledger->events.begin(), ledger->events.end(), [](const auto& event) { return event.kind == v3::EventKindV3::SessionEnded; });
    REQUIRE(ended != ledger->events.end()); REQUIRE(ended->payload.at("closeQuality") == "incomplete");
    REQUIRE_FALSE(fs::exists(fixture.directory / "session.lock"));
}
} // namespace

TEST_CASE("managed execution: real fresh text owner consumes one input and persists its bound final") {
    Fixture fixture; REQUIRE(fixture.service->admission_mode() == rt::SessionAdmissionMode::ManagedText);
    REQUIRE(fixture.service->runtime()->admission_mode() == rt::SessionAdmissionMode::ManagedText);
    REQUIRE(fixture.service->trajectory()->admission_mode() == rt::SessionAdmissionMode::ManagedText);
    fixture.Assemble(); auto accepted = fixture.Accept(); auto dispatched = fixture.Dispatch(accepted);
    REQUIRE(fixture.service->pending_input_count() == 0); const auto result = fixture.Run(dispatched);
    const auto final = fixture.service->RecordManagedTurnFinal(accepted->operation->provenance, result);
    REQUIRE_MESSAGE(final->knowledge == Knowledge::Committed, final->input.error_code);
    REQUIRE(final->artifact->outcome == platform::WriteOutcome::CommittedDurable);
    REQUIRE(final->append->status == traj::JournalAppendStatus::Committed);
    auto material = fixture.Capture(); auto source = rt::ReadManagedExecutionOwned(material);
    REQUIRE(source.has_value()); REQUIRE(source->size() == 1); REQUIRE(source->front().state == State::Final);
    REQUIRE(source->front().provenance == accepted->operation->provenance);
    REQUIRE(source->front().binding_event_id == dispatched->binding->event_id);
    REQUIRE(source->front().binding_seq == dispatched->binding->seq); REQUIRE(source->front().binding_hash == dispatched->binding->line_hash);
    REQUIRE(source->front().dispatch_policy_revision == 12); REQUIRE(source->front().provenance.admission.policy_revision == 11);
    REQUIRE(source->front().result_sha256 == platform::Sha256Hex(material.results.at(accepted->input.operation_id)));
    REQUIRE_FALSE(rt::ReadManagedOperationsOwned(material).has_value());
    const auto before = Read(fixture.directory / "sdk-results" / (accepted->input.operation_id + ".json"));
    REQUIRE(fixture.service->RecordManagedTurnFinal(accepted->operation->provenance, result)->knowledge == Knowledge::Rejected);
    REQUIRE(Read(fixture.directory / "sdk-results" / (accepted->input.operation_id + ".json")) == before);
    REQUIRE(fixture.model->calls == 1); REQUIRE(fixture.service->Close("complete").error_code.empty());
    REQUIRE_FALSE(fs::exists(fixture.directory / "session.lock")); REQUIRE(fixture.Capture().completion_known);
    REQUIRE(rt::ReadManagedExecutionOwned(fixture.Capture()).has_value());
    Mark("roundtrip");
}

TEST_CASE("managed execution: storage mode and unassembled or foreign fronts cannot dispatch") {
    Fixture storage(false); auto storage_input = storage.Accept();
    REQUIRE(storage.service->DispatchManagedPendingInput(storage_input->operation->provenance, 12)->knowledge == Knowledge::Rejected);
    auto storage_profile = storage.Profile(); REQUIRE_THROWS_AS(storage.service->InitializeManagedTextExecution(storage.Resources(), std::move(storage_profile)), std::logic_error);
    REQUIRE(storage.service->execution() == nullptr); REQUIRE_FALSE(fs::exists(storage.directory / "sdk-results"));
    REQUIRE(rt::ReadManagedOperationsOwned(storage.Capture()).has_value());
    Fixture fixture; auto accepted = fixture.Accept();
    const auto original = Read(fixture.directory / "operations.jsonl");
    REQUIRE(fixture.service->DispatchManagedPendingInput(accepted->operation->provenance, 12)->knowledge == Knowledge::Rejected);
    auto generic = fixture.Profile(); REQUIRE_THROWS_AS(fixture.service->InitializeExecution(fixture.Resources(), std::move(generic)), std::logic_error);
    fixture.Assemble(); auto foreign = accepted->operation->provenance; foreign.admission.subject.user_id = "other-initiator";
    REQUIRE(fixture.service->DispatchManagedPendingInput(foreign, 12)->knowledge == Knowledge::Rejected);
    foreign = accepted->operation->provenance; foreign.run_id = "main-0002";
    REQUIRE(fixture.service->DispatchManagedPendingInput(foreign, 12)->knowledge == Knowledge::Rejected);
    REQUIRE(fixture.service->DispatchManagedPendingInput(accepted->operation->provenance, 0)->knowledge == Knowledge::Rejected);
    REQUIRE(fixture.service->PopPendingInput().status == rt::SessionService::PendingPop::Status::NotAdmitted);
    REQUIRE_FALSE(fixture.service->SubmitInput({"local", "must not admit", {}}).accepted);
    REQUIRE(fixture.service->trajectory()->NewTurnBridge({}) == nullptr);
    REQUIRE(Read(fixture.directory / "operations.jsonl") == original); REQUIRE(fixture.model->calls == 0);
    const auto rejected = fixture.service->RejectManagedPendingInput(accepted->operation->provenance, "rejected", "policy.revoked", 13);
    REQUIRE(rejected->knowledge == Knowledge::Committed); REQUIRE(fixture.service->pending_input_count() == 0);
    REQUIRE(Rows(fixture.Capture().operations).at(1).at("schemaVersion") == 3);
    Mark("admission");
}

TEST_CASE("managed execution: owned strict reader rejects cross scope duplicated phases and changed originals") {
    Fixture fixture; fixture.Assemble(); auto accepted = fixture.Accept(); auto dispatched = fixture.Dispatch(accepted);
    const auto result = fixture.Run(dispatched);
    REQUIRE(fixture.service->RecordManagedTurnFinal(accepted->operation->provenance, result)->knowledge == Knowledge::Committed);
    const auto original = fixture.Capture(); const auto original_rows = Rows(original.operations);
    REQUIRE(original_rows.size() == 3); REQUIRE(rt::ReadManagedExecutionOwned(original).has_value());
    for (const char* field : {"provenanceHash", "runId", "turnId", "bindingEventId", "bindingHash", "resultSha256"}) {
        auto changed = original; auto rows = original_rows; rows.back()[field] = "changed"; changed.operations = Encode(rows);
        REQUIRE_FALSE(rt::ReadManagedExecutionOwned(changed).has_value());
    }
    for (const auto index : {1u, 2u}) {
        auto changed = original; auto rows = original_rows; rows.insert(rows.begin() + index, rows.at(index)); changed.operations = Encode(rows);
        REQUIRE_FALSE(rt::ReadManagedExecutionOwned(changed).has_value());
    }
    { auto changed = original; auto rows = original_rows; rows.at(1)["dispatchPolicyRevision"] = 0; changed.operations = Encode(rows);
      REQUIRE_FALSE(rt::ReadManagedExecutionOwned(changed).has_value()); }
    { auto changed = original; auto rows = original_rows; rows.at(0)["provenance"]["initiatingSubject"]["userId"] = "foreign";
      changed.operations = Encode(rows); REQUIRE_FALSE(rt::ReadManagedExecutionOwned(changed).has_value()); }
    { auto changed = original; changed.inputs.clear(); REQUIRE_FALSE(rt::ReadManagedExecutionOwned(changed).has_value()); }
    { auto changed = original; changed.inputs.at(accepted->input.operation_id) += " "; REQUIRE_FALSE(rt::ReadManagedExecutionOwned(changed).has_value()); }
    { auto changed = original; changed.results.clear(); REQUIRE_FALSE(rt::ReadManagedExecutionOwned(changed).has_value()); }
    { auto changed = original; changed.results["op-foreign"] = original.results.begin()->second; REQUIRE_FALSE(rt::ReadManagedExecutionOwned(changed).has_value()); }
    { auto changed = original; changed.results.begin()->second += " "; REQUIRE_FALSE(rt::ReadManagedExecutionOwned(changed).has_value()); }
    { auto changed = original; auto payload = Json::parse(changed.results.begin()->second); payload["turnId"] = "foreign-turn";
      changed.results.begin()->second = payload.dump(); REQUIRE_FALSE(rt::ReadManagedExecutionOwned(changed).has_value()); }
    { auto changed = original; changed.main.back() = ' '; REQUIRE_FALSE(rt::ReadManagedExecutionOwned(changed).has_value()); }
    Mark("strict");
}

TEST_CASE("managed execution: first dispatch native unknown or semantic gap fences queue and clean close") {
    for (const bool semantic : {false, true}) {
        Fixture fixture; fixture.Assemble(); auto native = std::make_shared<NativeProbe>(); auto arm = std::make_shared<bool>(false);
        fixture.service->SetManagedOperationProbesForTest(native, [arm, semantic] { if (semantic && *arm) throw std::runtime_error("private publication gap"); });
        auto accepted = fixture.Accept(); *arm = true; native->arm = !semantic;
        const auto failed = fixture.service->DispatchManagedPendingInput(accepted->operation->provenance, 12);
        REQUIRE(failed->append.has_value()); REQUIRE(fixture.service->FirstManagedWriteFailure() == failed);
        if (semantic) { REQUIRE(failed->knowledge == Knowledge::NativeCommittedPublicationGap); REQUIRE(failed->append->status == traj::JournalAppendStatus::Committed); }
        else { REQUIRE(native->fired); REQUIRE(failed->knowledge == Knowledge::Unconfirmed); REQUIRE(failed->append->status == traj::JournalAppendStatus::Unconfirmed);
               REQUIRE(failed->append->file_sync.succeeded); REQUIRE(failed->append->file_sync.injected_unconfirmed); }
        const auto original = Read(fixture.directory / "operations.jsonl"); REQUIRE(Rows(original).size() == 2);
        REQUIRE(fixture.service->pending_input_count() == 1); REQUIRE(fixture.model->calls == 0);
        REQUIRE(fixture.service->DispatchManagedPendingInput(accepted->operation->provenance, 12)->knowledge == Knowledge::Rejected);
        REQUIRE_FALSE(fixture.service->SubmitManagedInput({"next", "must not rewrite", {}}, fixture.Admission())->input.accepted);
        auto material = fixture.Capture(); REQUIRE_FALSE(material.completion_known);
        auto source = rt::ReadManagedExecutionOwned(material); REQUIRE(source.has_value()); REQUIRE(source->front().state == State::Dispatched);
        REQUIRE(source->front().binding_event_id.empty());
        REQUIRE_FALSE(fixture.service->Close("unknown-dispatch").error_code.empty()); CheckIncompleteEnded(fixture);
        REQUIRE(Read(fixture.directory / "operations.jsonl") == original); REQUIRE(fixture.service->FirstManagedWriteFailure() == failed);
    }
    Mark("dispatch-first");
}

TEST_CASE("managed execution: actual unconfirmed main binding closes writer and releases its original lock") {
    auto native = std::make_shared<NativeProbe>(); Fixture fixture(true, native); fixture.Assemble(); auto accepted = fixture.Accept();
    native->arm = true; const auto failed = fixture.service->DispatchManagedPendingInput(accepted->operation->provenance, 12);
    REQUIRE(native->fired); REQUIRE(failed->append->status == traj::JournalAppendStatus::Committed);
    REQUIRE(failed->binding_append.has_value()); REQUIRE(failed->binding_append->status != v3::WriteReceipt::Status::Committed);
    REQUIRE(failed->binding_append->journal_append->status == traj::JournalAppendStatus::Unconfirmed);
    REQUIRE(failed->binding_append->journal_append->file_sync.succeeded);
    REQUIRE(fixture.service->trajectory()->v3_main_writer()->broken()); REQUIRE(fixture.model->calls == 0);
    const auto before = Read(fixture.Main()); auto material = fixture.Capture(); REQUIRE_FALSE(material.completion_known);
    REQUIRE(rt::ReadManagedExecutionOwned(material).has_value());
    REQUIRE_FALSE(fixture.service->Close("binding-unconfirmed").error_code.empty());
    const auto retirement = fixture.service->ManagedMainRetirement(); REQUIRE(retirement.has_value());
    REQUIRE(retirement->attempted); REQUIRE(retirement->writer_closed); REQUIRE(retirement->lock_released);
    REQUIRE(retirement->checked_close.has_value()); REQUIRE_FALSE(retirement->checked_close->has_value());
    REQUIRE(native->close_seen); REQUIRE_FALSE(fs::exists(fixture.directory / "session.lock"));
    REQUIRE(Read(fixture.Main()) == before); REQUIRE(fixture.service->FirstManagedWriteFailure() == failed);
    REQUIRE(fixture.service->trajectory()->v3_main_writer()->first_unconfirmed_journal_append().has_value());
    Mark("binding-close");
}

TEST_CASE("managed execution: result artifact and final first failures retain originals without repeat completion") {
    for (const unsigned fault : {0u, 1u, 2u}) {
        Fixture fixture; fixture.Assemble(); auto native = std::make_shared<NativeProbe>(); auto arm = std::make_shared<bool>(false);
        fixture.service->SetManagedOperationProbesForTest(native, [arm, fault] { if (fault == 2 && *arm) throw std::runtime_error("private final gap"); });
        auto accepted = fixture.Accept(); auto dispatched = fixture.Dispatch(accepted); const auto result = fixture.Run(dispatched);
        if (fault == 0) platform::SetDirectoryFlushFailureForTest(true);
        if (fault == 1) native->arm = true;
        *arm = true; const auto failed = fixture.service->RecordManagedTurnFinal(accepted->operation->provenance, result);
        platform::SetDirectoryFlushFailureForTest(false);
        REQUIRE(fixture.service->FirstManagedWriteFailure() == failed); REQUIRE(failed->artifact.has_value());
        if (fault == 0) { REQUIRE(failed->artifact->outcome == platform::WriteOutcome::CommittedDurabilityUnconfirmed);
            REQUIRE_FALSE(failed->append.has_value()); REQUIRE(failed->knowledge == Knowledge::Unconfirmed); }
        if (fault == 1) { REQUIRE(failed->artifact->outcome == platform::WriteOutcome::CommittedDurable);
            REQUIRE(failed->append->status == traj::JournalAppendStatus::Unconfirmed); REQUIRE(failed->append->file_sync.succeeded); }
        if (fault == 2) { REQUIRE(failed->artifact->outcome == platform::WriteOutcome::CommittedDurable);
            REQUIRE(failed->append->status == traj::JournalAppendStatus::Committed); REQUIRE(failed->knowledge == Knowledge::NativeCommittedPublicationGap); }
        const auto bytes = Read(fixture.directory / "sdk-results" / (accepted->input.operation_id + ".json"));
        const auto ledger_bytes = Read(fixture.directory / "operations.jsonl");
        REQUIRE(fixture.service->RecordManagedTurnFinal(accepted->operation->provenance, result)->knowledge == Knowledge::Rejected);
        REQUIRE(Read(fixture.directory / "sdk-results" / (accepted->input.operation_id + ".json")) == bytes);
        REQUIRE(Read(fixture.directory / "operations.jsonl") == ledger_bytes); REQUIRE(fixture.model->calls == 1);
        const auto material = fixture.Capture(); REQUIRE_FALSE(material.completion_known); REQUIRE(rt::ReadManagedExecutionOwned(material).has_value());
        REQUIRE_FALSE(fixture.service->Close("final-unconfirmed").error_code.empty()); CheckIncompleteEnded(fixture);
        REQUIRE(Read(fixture.directory / "operations.jsonl") == ledger_bytes);
    }
    Mark("final-first");
}

TEST_CASE("managed execution: allocation faults retire owned writers before escaping close") {
    using Stage = rt::ManagedCloseAllocationStage;
    for (const auto stage : {Stage::BeforeReasonCopy, Stage::BeforeFallbackDiagnostic,
                            Stage::BeforeFailureDiagnostic, Stage::BeforeOutcomePublication}) {
        auto main_native = std::make_shared<NativeProbe>(); Fixture fixture(true, main_native); fixture.Assemble();
        auto operation_native = std::make_shared<NativeProbe>();
        fixture.service->SetManagedOperationProbesForTest(operation_native);
        auto accepted = fixture.Accept(); auto dispatched = fixture.Dispatch(accepted); const auto result = fixture.Run(dispatched);
        operation_native->arm = true;
        const auto first_failure = fixture.service->RecordManagedTurnFinal(accepted->operation->provenance, result);
        REQUIRE(first_failure->knowledge == Knowledge::Unconfirmed);
        REQUIRE(first_failure->artifact->outcome == platform::WriteOutcome::CommittedDurable);
        REQUIRE(first_failure->append->status == traj::JournalAppendStatus::Unconfirmed);
        REQUIRE(first_failure->append->file_sync.succeeded); REQUIRE(first_failure->append->file_sync.injected_unconfirmed);
        const auto operation_bytes = Read(fixture.directory / "operations.jsonl");
        const auto result_bytes = Read(fixture.directory / "sdk-results" / (accepted->input.operation_id + ".json"));
        const auto main_before = Read(fixture.Main());
        auto memory = fixture.service->trajectory()->memory_capability();
        auto named = fixture.service->trajectory()->named_result_capability();
        REQUIRE(memory != nullptr); REQUIRE(named != nullptr);
        auto allocation = std::make_shared<AllocationProbe>(stage);
        fixture.service->SetManagedCloseAllocationProbeForTest(allocation);
        const std::string reason(256, 'r'); // The real request copy cannot rely on SSO.
        REQUIRE_THROWS_AS(fixture.service->Close(reason), std::bad_alloc);
        REQUIRE(allocation->target_thrown);
        REQUIRE(allocation->capture_thrown == (stage == Stage::BeforeFailureDiagnostic));
        REQUIRE(fixture.service->trajectory()->v3_main_writer()->closed());
        const auto retired = fixture.service->ManagedMainRetirement(); REQUIRE(retired.has_value());
        REQUIRE(retired->attempted); REQUIRE(retired->writer_closed); REQUIRE(retired->lock_released);
        REQUIRE(retired->checked_close.has_value()); REQUIRE(retired->checked_close->has_value());
        REQUIRE(main_native->close_count == 1); REQUIRE(operation_native->close_count == 1);
        const auto closed_operation = fixture.service->ManagedOperationCloseReceipt(); REQUIRE(closed_operation.has_value());
        REQUIRE(closed_operation->native.has_value()); REQUIRE(closed_operation->native->attempted);
        REQUIRE(closed_operation->native->succeeded); REQUIRE(closed_operation->broken_before);
        REQUIRE(memory->Store("must not write", "text/plain").error.code == "cas.owner_closed");
        const auto closed_named = named->BeginMaterial("must-not-write");
        REQUIRE_FALSE(closed_named.has_value()); REQUIRE(closed_named.error().code == "named_result.owner_closed");
        REQUIRE_FALSE(fs::exists(fixture.directory / "session.lock"));
        auto lock = traj::SessionLock::Acquire(fixture.directory, traj::SessionManagerClock{}.LockOwner());
        REQUIRE_MESSAGE(lock.has_value(), (lock ? "" : lock.error())); REQUIRE(lock->holds()); lock->Release();
        REQUIRE(fixture.service->FirstManagedWriteFailure() == first_failure);
        REQUIRE(first_failure->append->status == traj::JournalAppendStatus::Unconfirmed);
        REQUIRE(first_failure->append->file_sync.injected_unconfirmed);
        REQUIRE(Read(fixture.directory / "operations.jsonl") == operation_bytes);
        REQUIRE(Read(fixture.directory / "sdk-results" / (accepted->input.operation_id + ".json")) == result_bytes);
        const auto main_after = Read(fixture.Main());
        if (stage == Stage::BeforeOutcomePublication) {
            REQUIRE(main_after.size() > main_before.size()); REQUIRE(main_after.starts_with(main_before));
            CheckIncompleteEnded(fixture);
            const auto main_fact = fixture.service->ManagedMainCloseOutcome(); REQUIRE(main_fact.has_value());
            REQUIRE(main_fact->error_code.empty()); REQUIRE(main_fact->close_quality == "incomplete");
        } else {
            REQUIRE(main_after == main_before); REQUIRE_FALSE(fixture.service->ManagedMainCloseOutcome().has_value());
        }
        const auto repeat = fixture.service->Close("must-not-rewrite-ended");
        REQUIRE(repeat.error_code == "managed.operation.close_unconfirmed"); REQUIRE(repeat.close_quality == "incomplete");
        REQUIRE(Read(fixture.Main()) == main_after);
        REQUIRE(Read(fixture.directory / "operations.jsonl") == operation_bytes);
        REQUIRE(main_native->close_count == 1); REQUIRE(operation_native->close_count == 1);
        REQUIRE(fixture.model->calls == 1);
        REQUIRE_FALSE(fixture.service->SubmitManagedInput({"after-close", "must not admit", {}}, fixture.Admission())->input.accepted);
    }
    {
        Fixture fixture; fixture.Assemble();
        auto state = std::make_shared<ReentrantCloseState>();
        auto capture = std::make_shared<ReentrantCloseCapture>(fixture.service.get(), fixture.directory / "session.lock", state);
        std::weak_ptr<ReentrantCloseCapture> retained = capture;
        rt::AsyncToolRuntime::Hooks hooks;
        hooks.writer = fixture.service->trajectory()->v3_main_writer();
        hooks.writer_mutex = fixture.service->trajectory()->v3_tool_results_mutex();
        hooks.current_turn_id = [capture] { return std::string{}; };
        auto asynchronous = rt::AsyncToolRuntime::Create(std::move(hooks), {}); REQUIRE(asynchronous != nullptr);
        hooks = {}; capture.reset(); // The real runtime retains the final capture.
        REQUIRE_FALSE(retained.expired());
        fixture.service->runtime()->AttachAsyncToolRuntime(std::move(asynchronous));
        REQUIRE(fixture.service->Close("outer-real-drain").error_code.empty());
        REQUIRE(retained.expired()); REQUIRE(state->called); REQUIRE_FALSE(state->failed);
        REQUIRE(state->shutdown_rejected); REQUIRE(state->close_rejected);
        REQUIRE(state->writer_still_open); REQUIRE(state->lock_still_owned);
        REQUIRE(fixture.service->runtime()->async_tool_runtime()->quiescent());
        REQUIRE(fixture.service->trajectory()->v3_main_writer()->closed());
        REQUIRE_FALSE(fs::exists(fixture.directory / "session.lock"));
        REQUIRE(fixture.service->Close("repeat-regular-close").error_code.empty());
    }
    {
        Fixture fixture; fixture.Assemble(); auto accepted = fixture.Accept(); auto dispatched = fixture.Dispatch(accepted);
        auto bridge = fixture.service->NewManagedTextTurnBridge(dispatched->operation->provenance, {});
        REQUIRE(bridge != nullptr);
        rt::ToolTraceHub hub(fixture.service->runtime()->ids()); agent::TurnWiring wiring; wiring.turn_id = dispatched->operation->turn_id;
        rt::ScopedTurnBindings bindings(fixture.service->execution()->agent());
        bindings.Bind(wiring, {&hub, bridge.get(), nullptr, fixture.owner.session_id, wiring.turn_id, std::nullopt});
        bridge->BeginTurn(wiring.turn_id, "external_user");
        api::Message user{api::Role::User, {api::TextBlock{dispatched->operation->text}}}; bridge->RecordInput(user);
        fixture.service->RequestExecutionShutdown();
        REQUIRE(fixture.service->DispatchManagedPendingInput(accepted->operation->provenance, 12)->knowledge == Knowledge::Rejected);
        REQUIRE(fixture.service->NewManagedTextTurnBridge(accepted->operation->provenance, {}) == nullptr);
        std::atomic<bool> cancel{true};
        const auto outcome = fixture.service->execution()->agent().Run(std::move(user), wiring, &cancel);
        REQUIRE_MESSAGE(outcome.has_value(), (outcome ? "" : outcome.error())); REQUIRE(outcome->cancelled);
        bridge->EndTurn(false, true, "actual-shutdown-cancellation"); bindings.Reset(); REQUIRE(fixture.model->calls == 0);
        rt::ManagedOperationResult cancelled{"cancelled", {}, {}, {}, false, false};
        const auto final = fixture.service->RecordManagedTurnFinal(accepted->operation->provenance, cancelled);
        REQUIRE(final->knowledge == Knowledge::Committed); REQUIRE(final->append->status == traj::JournalAppendStatus::Committed);
        const auto materials = fixture.Capture(); const auto source = rt::ReadManagedExecutionOwned(materials);
        REQUIRE(source.has_value()); REQUIRE(source->front().state == State::Final); REQUIRE(source->front().execution_status == "cancelled");
        const auto closed = fixture.service->Close("joined-real-cancelled-turn");
        REQUIRE(closed.error_code.empty()); REQUIRE(closed.close_quality == "clean");
        REQUIRE_FALSE(fs::exists(fixture.directory / "session.lock"));
    }
    {
        Fixture fixture; fixture.Assemble(); auto accepted = fixture.Accept(); auto dispatched = fixture.Dispatch(accepted);
        auto bridge = fixture.service->NewManagedTextTurnBridge(dispatched->operation->provenance, {});
        REQUIRE(bridge != nullptr);
        rt::ToolTraceHub hub(fixture.service->runtime()->ids()); agent::TurnWiring wiring; wiring.turn_id = dispatched->operation->turn_id;
        rt::ScopedTurnBindings bindings(fixture.service->execution()->agent());
        bindings.Bind(wiring, {&hub, bridge.get(), nullptr, fixture.owner.session_id, wiring.turn_id, std::nullopt});
        bridge->BeginTurn(wiring.turn_id, "external_user");
        api::Message user{api::Role::User, {api::TextBlock{dispatched->operation->text}}}; bridge->RecordInput(user);
        fixture.model->fail = true;
        fixture.model->on_send = [&fixture] { fixture.service->RequestExecutionShutdown(); };
        const auto outcome = fixture.service->execution()->agent().Run(std::move(user), wiring);
        fixture.model->on_send = {};
        REQUIRE_FALSE(outcome.has_value()); REQUIRE_FALSE(outcome.error().empty()); REQUIRE(fixture.model->calls == 1);
        bridge->EndTurn(false, false, outcome.error()); bindings.Reset();
        REQUIRE(fixture.service->DispatchManagedPendingInput(accepted->operation->provenance, 12)->knowledge == Knowledge::Rejected);
        REQUIRE(fixture.service->NewManagedTextTurnBridge(accepted->operation->provenance, {}) == nullptr);
        rt::ManagedOperationResult failed{"error", {}, outcome.error(), {}, false, false};
        const auto final = fixture.service->RecordManagedTurnFinal(accepted->operation->provenance, failed);
        REQUIRE(final->knowledge == Knowledge::Committed); REQUIRE(final->append->status == traj::JournalAppendStatus::Committed);
        const auto materials = fixture.Capture(); const auto source = rt::ReadManagedExecutionOwned(materials);
        REQUIRE(source.has_value()); REQUIRE(source->front().state == State::Final); REQUIRE(source->front().execution_status == "error");
        const auto result = Json::parse(materials.results.at(accepted->input.operation_id));
        REQUIRE(result["error"] == outcome.error()); REQUIRE(result["finalText"] == "");
        const auto closed = fixture.service->Close("joined-real-failed-turn");
        REQUIRE(closed.error_code.empty()); REQUIRE(closed.close_quality == "clean");
        REQUIRE_FALSE(fs::exists(fixture.directory / "session.lock"));
    }
    Mark("close-allocation");
}

TEST_CASE("managed execution: strict final text and error reject escaped NUL after matching hashes") {
    Fixture fixture; fixture.Assemble(); auto accepted = fixture.Accept(); auto dispatched = fixture.Dispatch(accepted);
    const auto result = fixture.Run(dispatched);
    REQUIRE(fixture.service->RecordManagedTurnFinal(accepted->operation->provenance, result)->knowledge == Knowledge::Committed);
    const auto original = fixture.Capture(); REQUIRE(rt::ReadManagedExecutionOwned(original).has_value());
    for (const char* field : {"finalText", "error"}) {
        auto changed = original;
        auto payload = Json::parse(changed.results.at(accepted->input.operation_id));
        std::string text = "matched"; text.push_back('\0'); text += "suffix"; payload[field] = text;
        auto& bytes = changed.results.at(accepted->input.operation_id); bytes = payload.dump();
        REQUIRE(bytes.find("\\u0000") != std::string::npos);
        auto rows = Rows(changed.operations);
        rows.back()["resultSha256"] = platform::Sha256Hex(bytes); rows.back()["resultBytes"] = bytes.size();
        changed.operations = Encode(rows);
        const auto roster = rt::ReadManagedExecutionLedgerOwned(changed.operations, changed.owner, changed.run_id);
        REQUIRE(roster.has_value()); REQUIRE(roster->size() == 1); REQUIRE(roster->front().state == State::Final);
        REQUIRE(roster->front().result_sha256 == platform::Sha256Hex(bytes)); REQUIRE(roster->front().result_bytes == bytes.size());
        REQUIRE_FALSE(rt::ReadManagedExecutionOwned(changed).has_value());
    }
    REQUIRE(fixture.service->Close("nul-reader-test").error_code.empty());
    Mark("result-nul");
}
