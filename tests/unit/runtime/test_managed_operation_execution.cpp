#include <doctest/doctest.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <memory>
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
#include "runtime/scoped_turn_bindings.hpp"
#include "runtime/session_service.hpp"
#include "runtime/tool_trace_hub.hpp"
#include "trajectory/canonical_json.hpp"
#include "trajectory/managed_session_reservation.hpp"
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
    bool After(const traj::JournalNativeIoResult& actual) noexcept override {
        if (actual.stage == traj::JournalNativeStage::Close) close_seen = actual.attempted && actual.succeeded;
        if (arm && actual.stage == traj::JournalNativeStage::FileSync && actual.attempted && actual.succeeded) {
            arm = false; fired = true; return true;
        }
        return false;
    }
};
struct ModelState { unsigned calls = 0; bool alive = false; std::vector<api::Request> requests; };
struct Backend final : api::Backend {
    std::shared_ptr<ModelState> state;
    explicit Backend(std::shared_ptr<ModelState> value) : state(std::move(value)) { state->alive = true; }
    ~Backend() override { state->alive = false; }
    std::expected<void, api::Error> send_stream(const api::Request& request,
        const std::function<void(const api::StreamEvent&)>& emit, const std::atomic<bool>*) override {
        ++state->calls; state->requests.push_back(request);
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
