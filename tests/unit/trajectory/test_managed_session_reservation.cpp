#include <doctest/doctest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <expected>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <iterator>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

#include "platform/atomic_write.hpp"
#include "platform/paths.hpp"
#include "platform/process.hpp"
#include "platform/sha256.hpp"
#include "api/backend.hpp"
#include "runtime/session_service.hpp"
#include "trajectory/canonical_json.hpp"
#include "trajectory/managed_session_reservation.hpp"
#include "trajectory/session_manager.hpp"
#include "trajectory/v3/reader.hpp"
#include "trajectory/v3/writer.hpp"
#include "workspace/identity.hpp"

namespace {
namespace fs = std::filesystem;
namespace platform = lubancode::platform;
namespace traj = lubancode::trajectory;
namespace v3 = traj::v3;
using Knowledge = traj::ManagedSessionOwnershipPublication::Knowledge;

struct Fixture {
    fs::path root;
    traj::SessionManagerOptions options;
    traj::TrajectoryDirectory workspace;
    explicit Fixture(bool beneath_cwd = false) {
        static std::atomic<unsigned> serial{0};
        root = (beneath_cwd ? fs::current_path() : fs::temp_directory_path()) / ("managed-reservation-" +
            std::to_string(platform::CurrentProcessId()) + "-" +
            std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + "-" +
            std::to_string(++serial));
        REQUIRE(fs::create_directory(root));
        root = fs::canonical(root);
        options.workspaces_root = root / "workspaces";
        options.workspace_root = root / "project";
        REQUIRE(fs::create_directory(options.workspace_root));
        options.identity = lubancode::workspace::MakeFallbackIdentity(options.workspace_root);
        options.launch_cwd = platform::PathToUtf8(options.workspace_root);
        options.v3_system_content = "local original system";
        auto created = traj::TrajectoryDirectory::CreateWorkspace(
            options.workspaces_root, options.identity, 1759000000000LL);
        REQUIRE_MESSAGE(created.has_value(), (created ? "" : created.error()));
        workspace = std::move(*created);
    }
    ~Fixture() {
        platform::SetFileFlushFailureForTest(false);
        platform::SetDirectoryFlushFailureForTest(false);
        std::error_code ignored;
        fs::remove_all(platform::FileIoPath(root), ignored);
    }
    traj::ManagedSessionOwnership Owner(const std::string& id = "20261005-120000-MAN001") const {
        return {"tenant-a", "project-a", options.identity.workspace_key, id, 1};
    }
    fs::path Session(const std::string& id) const {
        return workspace.workspace_dir() / "sessions" / platform::Utf8ToPath(id);
    }
    std::unique_ptr<traj::ManagedSessionReservation> Reserve(const std::string& id = "20261005-120000-MAN001") const {
        const auto owner = traj::SessionManagerClock{}.LockOwner();
        auto result = traj::ManagedSessionReservation::Reserve(options.workspaces_root, Owner(id), owner);
        REQUIRE_MESSAGE(result.has_value(), (result ? "" : result.error()));
        return std::move(*result);
    }
};

void Write(const fs::path& path, const std::string& bytes) {
    std::ofstream file(platform::FileIoPath(path), std::ios::binary | std::ios::trunc);
    REQUIRE(file.is_open());
    file.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    file.close();
    REQUIRE_FALSE(file.fail());
}

std::string Read(const fs::path& path) {
    std::ifstream file(platform::FileIoPath(path), std::ios::binary);
    REQUIRE(file.is_open());
    std::string bytes{std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
    REQUIRE_FALSE(file.bad());
    return bytes;
}

std::vector<std::string> Entries(const fs::path& directory) {
    std::vector<std::string> result;
    for (const auto& entry : fs::directory_iterator(directory))
        result.push_back(platform::PathToUtf8(entry.path().filename()));
    std::sort(result.begin(), result.end());
    return result;
}

void NoBody(const fs::path& directory) {
    REQUIRE_FALSE(fs::exists(directory / "artifacts"));
    REQUIRE_FALSE(fs::exists(directory / "subagents"));
    REQUIRE_FALSE(fs::exists(traj::TrajectoryDirectory::OpenExisting(directory).v3_stream_path()));
}

void Marker(const char* name) { std::cout << "[managed-session-reservation-path] " << name << '\n'; }

struct OpeningClock : traj::SessionManagerClock {
    mutable std::function<void()> before_lock;
    traj::SessionLockOwner LockOwner() const override {
        if (before_lock) {
            auto action = std::exchange(before_lock, {});
            action();
        }
        return traj::SessionManagerClock::LockOwner();
    }
};

std::string ClosedLocal(Fixture& fixture) {
    traj::SessionManager manager(fixture.options);
    auto launched = manager.LaunchSession();
    REQUIRE_MESSAGE(launched.has_value(), (launched ? "" : launched.error()));
    REQUIRE((*launched)->v3_main.has_value());
    const auto id = (*launched)->session_id();
    const auto closed = manager.Close({}, nullptr);
    REQUIRE_MESSAGE(closed.error_code.empty(), (closed.error_code + ": " + closed.message));
    return id;
}

traj::ManagedSessionCreationAudit Creator() { return {"tenant-a", "user-a", "user", "credential-a", 7}; }
static_assert(!std::is_constructible_v<traj::ManagedSessionDirectory, traj::TrajectoryDirectory,
    traj::SessionLock, traj::ManagedSessionOwnershipPublication>);

void SameDurablePublication(const traj::ManagedSessionOwnershipPublication& actual,
                            const traj::ManagedSessionOwnershipPublication& expected) {
    REQUIRE(actual.knowledge == expected.knowledge);
    REQUIRE(actual.expected == expected.expected);
    REQUIRE(actual.captured_bytes == expected.captured_bytes);
    REQUIRE(actual.publication_bytes == expected.publication_bytes);
    REQUIRE(actual.requested == expected.requested);
    REQUIRE(actual.native.has_value());
    REQUIRE(actual.native->has_value());
    REQUIRE(expected.native.has_value());
    REQUIRE(expected.native->has_value());
    REQUIRE(actual.native->value().outcome == expected.native->value().outcome);
    REQUIRE(actual.error_code == expected.error_code);
    REQUIRE(actual.message == expected.message);
}

void ManagedOnly(traj::SessionManager& manager, const Fixture& fixture, const std::string& id) {
    const std::string unavailable = "managed.manager.operation_unavailable";
    const auto local = manager.LaunchSession(); REQUIRE_FALSE(local); REQUIRE(local.error() == unavailable);
    REQUIRE(manager.Clear({}, nullptr).error_code == unavailable);
    traj::ResumeRequest resume; resume.source_session_id = id;
    REQUIRE(manager.ResumeAsNew(resume).error_code == unavailable);
    REQUIRE(manager.ProbeResumeSource(id).error_code == unavailable);
    REQUIRE(manager.LatestResumableSessionId().empty());
    const auto recovery = manager.RecoverWorkspace();
    REQUIRE(recovery.sessions.empty()); REQUIRE(recovery.adopted_session_id.empty());
    REQUIRE(recovery.notes == std::vector<std::string>{unavailable});
    const auto archive = manager.ArchiveSession(id); REQUIRE_FALSE(archive); REQUIRE(archive.error() == unavailable);
    const auto unarchive = manager.UnarchiveSession(id); REQUIRE_FALSE(unarchive); REQUIRE(unarchive.error() == unavailable);
    const auto remove = manager.DeleteSession(id, "must not mutate"); REQUIRE_FALSE(remove); REQUIRE(remove.error() == unavailable);
    const auto reference = manager.RecordResumeReference(id, "must not mutate"); REQUIRE_FALSE(reference); REQUIRE(reference.error() == unavailable);
    const auto registration = manager.RegisterCheckout(fixture.options.identity); REQUIRE_FALSE(registration); REQUIRE(registration.error() == unavailable);
    const auto approval = manager.UpdateApprovalMode(lubancode::ApprovalMode::Default); REQUIRE_FALSE(approval); REQUIRE(approval.error() == unavailable);
}

void CheckManagedManagerOpening(Fixture& fixture) {
    const std::string id = "20261006-120000-MAN101";
    auto pending = fixture.Reserve(id);
    const auto published = pending->PublishOwnership();
    REQUIRE(published.knowledge == Knowledge::Committed);
    auto finished = pending->Finish(); REQUIRE(finished);
    const auto directory = finished->directory().session_dir();
    const auto stream = finished->directory().v3_stream_path();
    const auto lock_bytes = Read(directory / "session.lock");
    auto options = fixture.options;
    unsigned callbacks = 0;
    std::shared_ptr<traj::MemoryCapability> retained_memory;
    std::shared_ptr<traj::NamedResultCapability> retained_named;
    options.v3_opening_participant = [&](const traj::V3OpeningContext& context)
        -> std::expected<nlohmann::json, std::string> {
        ++callbacks;
        retained_memory = context.memory_capability; retained_named = context.named_result_capability;
        REQUIRE(context.session_id == id);
        REQUIRE(context.session_dir == directory);
        REQUIRE(Read(directory / "session.lock") == lock_bytes);
        REQUIRE_FALSE(traj::SessionLock::Acquire(directory, traj::SessionManagerClock{}.LockOwner()));
        return nlohmann::json{{"hostBindings", {{"managedFixture", true}}}};
    };
    traj::SessionManager manager(options);
    auto opened = manager.LaunchManagedSession(std::move(*finished), Creator());
    REQUIRE_MESSAGE(opened.has_value(), (opened ? "" : opened.error()));
    REQUIRE(callbacks == 1);
    REQUIRE_FALSE(finished->lock().holds());
    pending.reset(); finished = std::unexpected(std::string("consumed real opening"));
    REQUIRE((*opened)->lock.holds());
    REQUIRE((*opened)->lock.path() == directory / "session.lock");
    REQUIRE(Read(directory / "session.lock") == lock_bytes);
    REQUIRE_FALSE(traj::SessionLock::Acquire(directory, traj::SessionManagerClock{}.LockOwner()));
    REQUIRE((*opened)->session_id() == id);
    REQUIRE((*opened)->managed_publication);
    SameDurablePublication(*(*opened)->managed_publication, published);
    REQUIRE_FALSE(fs::exists(directory / "session.json"));
    REQUIRE_FALSE(fs::exists(directory / "main.jsonl"));
    auto ledger = v3::ReadV3Ledger(stream); REQUIRE(ledger);
    REQUIRE(ledger->session_id == id); REQUIRE(ledger->run_id == "main-0001"); REQUIRE(ledger->lines == 3);
    const auto initial_bytes = Read(stream);
    const auto first_line = nlohmann::json::parse(initial_bytes.substr(0, initial_bytes.find('\n')));
    REQUIRE(first_line.at("schemaVersion") == 3); REQUIRE(first_line.at("seq") == 1);
    REQUIRE(first_line.at("sessionId") == id); REQUIRE(first_line.at("runId") == "main-0001");
    const auto metadata = first_line.at("systemMeta").at("managedSession");
    REQUIRE(metadata.size() == 9);
    REQUIRE(metadata.at("schemaVersion") == 1); REQUIRE(metadata.at("mode") == "Managed");
    REQUIRE(metadata.at("tenantId") == fixture.Owner(id).tenant_id);
    REQUIRE(metadata.at("projectId") == fixture.Owner(id).project_id);
    REQUIRE(metadata.at("workspaceKey") == fixture.Owner(id).workspace_key);
    REQUIRE(metadata.at("sessionId") == id); REQUIRE(metadata.at("bindingVersion") == 1);
    const nlohmann::json subject{{"tenantId", "tenant-a"}, {"userId", "user-a"},
        {"actorKind", "user"}, {"credentialId", "credential-a"}};
    REQUIRE(metadata.at("creationSubject") == subject); REQUIRE(metadata.at("openingPolicyRevision") == 7);
    ManagedOnly(manager, fixture, id);
    REQUIRE(Read(stream) == initial_bytes);
    REQUIRE(Read(directory / traj::kManagedSessionOwnershipFile) == published.publication_bytes);
    const auto closed = manager.Close({}, nullptr); REQUIRE(closed.error_code.empty());
    REQUIRE(manager.active()->status == traj::SessionStatus::Closed);
    REQUIRE_FALSE(manager.active()->lock.holds()); REQUIRE_FALSE(fs::exists(directory / "session.lock"));
    REQUIRE(retained_memory); REQUIRE(retained_named);
    REQUIRE(retained_memory->Store("after real Close", "text/plain").error.code == "cas.owner_closed");
    const auto late_material = retained_named->BeginMaterial("managed-cleanup");
    REQUIRE_FALSE(late_material); REQUIRE(late_material.error().code == "named_result.owner_closed");
    SameDurablePublication(*manager.active()->managed_publication, published);
    auto closed_ledger = v3::ReadV3Ledger(stream); REQUIRE(closed_ledger);
    REQUIRE(closed_ledger->lines == 4);
    const auto closed_bytes = Read(stream); REQUIRE(closed_bytes.starts_with(initial_bytes));
    ManagedOnly(manager, fixture, id); REQUIRE(Read(stream) == closed_bytes);
    auto replacement = traj::SessionLock::Acquire(directory, traj::SessionManagerClock{}.LockOwner()); REQUIRE(replacement);
    replacement->Release();
    traj::SessionManager local(fixture.options);
    REQUIRE(local.ProbeResumeSource(id).error_code == "managed.ownership.local_trusted_rejected");
    REQUIRE(Read(stream) == closed_bytes);
}

void CheckManagedOpeningFailures(Fixture& fixture) {
    for (unsigned mode = 0; mode != 10; ++mode) {
        const auto id = "20261006-120000-FAIL" + std::to_string(mode);
        auto pending = fixture.Reserve(id);
        const auto published = pending->PublishOwnership(); REQUIRE(published.knowledge == Knowledge::Committed);
        auto finished = pending->Finish(); REQUIRE(finished);
        const auto directory = finished->directory().session_dir(); const auto stream = finished->directory().v3_stream_path();
        auto creation = Creator();
        if (mode == 0) creation.tenant_id = "foreign-tenant";
        if (mode == 1) creation.user_id.clear();
        if (mode == 2) creation.actor_kind = "administrator";
        if (mode == 3) creation.credential_id.clear();
        if (mode == 4) creation.opening_policy_revision = 0;
        if (mode == 5) creation.user_id = std::string(513, 'x');
        if (mode == 6) creation.user_id = std::string(1, static_cast<char>(0xff));
        if (mode == 7) creation.credential_id = "bad\ncredential";
        auto options = fixture.options;
        unsigned callbacks = 0;
        options.v3_opening_participant = [&](const traj::V3OpeningContext&)
            -> std::expected<nlohmann::json, std::string> { ++callbacks; return nlohmann::json::object(); };
        if (mode == 8) Write(directory / traj::kManagedSessionOwnershipFile, "changed owned marker");
        std::unique_ptr<Fixture> foreign;
        if (mode == 9) { foreign = std::make_unique<Fixture>(); options = foreign->options; }
        traj::SessionManager manager(options);
        auto rejected = manager.LaunchManagedSession(std::move(*finished), creation);
        REQUIRE_FALSE(rejected);
        if (mode < 8) REQUIRE(rejected.error() == "managed.session.creation_invalid");
        if (mode == 8) REQUIRE(rejected.error() == "managed.ownership.invalid_metadata");
        if (mode == 9) REQUIRE(rejected.error() == "managed.session.workspace_mismatch");
        REQUIRE(callbacks == 0); REQUIRE_FALSE(manager.active());
        REQUIRE_FALSE(fs::exists(directory / "session.lock")); REQUIRE_FALSE(fs::exists(stream));
        REQUIRE(Read(directory / traj::kManagedSessionOwnershipFile) ==
            (mode == 8 ? "changed owned marker" : published.publication_bytes));
        SameDurablePublication(pending->PublishOwnership(), published);
        const auto entries = Entries(directory);
        ManagedOnly(manager, fixture, id); REQUIRE(Entries(directory) == entries);
    }
    for (unsigned mode = 0; mode != 5; ++mode) {
        const auto id = "20261006-120000-CB" + std::to_string(mode);
        auto pending = fixture.Reserve(id);
        const auto published = pending->PublishOwnership(); REQUIRE(published.knowledge == Knowledge::Committed);
        auto finished = pending->Finish(); REQUIRE(finished);
        const auto directory = finished->directory().session_dir(); const auto stream = finished->directory().v3_stream_path();
        auto options = fixture.options;
        unsigned callbacks = 0, append_checks = 0;
        std::shared_ptr<traj::MemoryCapability> retained_memory;
        std::shared_ptr<traj::NamedResultCapability> retained_named;
        options.v3_opening_participant = [&](const traj::V3OpeningContext& context)
            -> std::expected<nlohmann::json, std::string> {
            ++callbacks; REQUIRE(context.session_id == id);
            retained_memory = context.memory_capability; retained_named = context.named_result_capability;
            REQUIRE_FALSE(traj::SessionLock::Acquire(directory, traj::SessionManagerClock{}.LockOwner()));
            if (mode == 0) { Write(directory / traj::kManagedSessionOwnershipFile, "drift during actual callback"); return nlohmann::json::object(); }
            if (mode == 1) throw std::runtime_error("PRIVATE_OPENING_FAILURE");
            if (mode == 2) return nlohmann::json{{"managedSession", {{"tenantId", "foreign"}}}};
            return nlohmann::json::object();
        };
        if (mode >= 3) options.v3_main_io_fault = [&]() -> std::optional<std::string> {
            if (++append_checks == (mode == 3 ? 2u : 3u)) return "fixture.initial_append_failed";
            return std::nullopt;
        };
        traj::SessionManager manager(options);
        auto rejected = manager.LaunchManagedSession(std::move(*finished), Creator()); REQUIRE_FALSE(rejected);
        REQUIRE(callbacks == 1); REQUIRE_FALSE(manager.active()); REQUIRE_FALSE(fs::exists(directory / "session.lock"));
        REQUIRE(retained_memory); REQUIRE(retained_named);
        const auto after_failure = retained_memory->Store("after failed opening", "text/plain");
        REQUIRE(after_failure.state == traj::CasCommitState::NotCommitted);
        REQUIRE(after_failure.error.code == "cas.owner_closed");
        const auto late_material = retained_named->BeginMaterial("managed-cleanup");
        REQUIRE_FALSE(late_material); REQUIRE(late_material.error().code == "named_result.owner_closed");
        if (mode == 0) REQUIRE(rejected.error() == "managed.ownership.invalid_metadata");
        if (mode == 1) REQUIRE(rejected.error() == "session.opening_failed: opening.participant_exception");
        if (mode == 2) REQUIRE(rejected.error() == "session.opening_failed: opening.reserved_metadata");
        if (mode >= 3) {
            if (mode == 3) REQUIRE(rejected.error().starts_with("session.v3_start_failed: v3writer.start_event_failed:"));
            else REQUIRE(rejected.error() == "managed.session.approval_baseline_failed: v3writer.injected");
            REQUIRE(append_checks == (mode == 3 ? 2u : 3u));
            const auto partial_bytes = Read(stream); REQUIRE_FALSE(partial_bytes.empty());
            const auto partial = nlohmann::json::parse(partial_bytes.substr(0, partial_bytes.find('\n')));
            REQUIRE(partial.at("systemMeta").at("managedSession").at("sessionId") == id);
            REQUIRE(partial.at("systemMeta").at("managedSession").at("creationSubject").at("userId") == Creator().user_id);
        } else REQUIRE_FALSE(fs::exists(stream));
        SameDurablePublication(pending->PublishOwnership(), published);
        REQUIRE(Read(directory / traj::kManagedSessionOwnershipFile) ==
            (mode == 0 ? "drift during actual callback" : published.publication_bytes));
        const auto entries = Entries(directory);
        ManagedOnly(manager, fixture, id); REQUIRE(Entries(directory) == entries);
        REQUIRE_FALSE(traj::ManagedSessionReservation::Reserve(fixture.options.workspaces_root,
            fixture.Owner(id), traj::SessionManagerClock{}.LockOwner()));
        auto free_lock = traj::SessionLock::Acquire(directory, traj::SessionManagerClock{}.LockOwner()); REQUIRE(free_lock);
    }
}

void CheckManagedRejectsActiveLocal(Fixture& fixture) {
    traj::SessionManager manager(fixture.options);
    auto local = manager.LaunchSession(); REQUIRE(local);
    const auto local_id = (*local)->session_id(); const auto local_stream = (*local)->directory.v3_stream_path();
    const auto local_bytes = Read(local_stream);
    const auto id = "20261006-120000-ACTIVE";
    auto pending = fixture.Reserve(id); const auto published = pending->PublishOwnership();
    REQUIRE(published.knowledge == Knowledge::Committed);
    auto finished = pending->Finish(); REQUIRE(finished); const auto directory = finished->directory().session_dir();
    auto rejected = manager.LaunchManagedSession(std::move(*finished), Creator()); REQUIRE_FALSE(rejected);
    REQUIRE(rejected.error() == "managed.session.active_exists");
    REQUIRE(manager.active()->session_id() == local_id); REQUIRE(manager.active()->lock.holds());
    REQUIRE(Read(local_stream) == local_bytes); REQUIRE_FALSE(fs::exists(directory / "session.lock"));
    REQUIRE_FALSE(fs::exists(traj::TrajectoryDirectory::OpenExisting(directory).v3_stream_path()));
    REQUIRE(Read(directory / traj::kManagedSessionOwnershipFile) == published.publication_bytes);
    // Rejecting conversion did not put the existing Local owner into Managed mode.
    REQUIRE(manager.UpdateApprovalMode(lubancode::ApprovalMode::Default));
    REQUIRE(manager.Close({}, nullptr).error_code.empty());
}
lubancode::runtime::SessionLaunchRequest ManagedStackRequest(const Fixture& fixture) {
    lubancode::runtime::SessionLaunchRequest request;
    request.workspace_identity = fixture.options.identity;
    request.workspaces_root = fixture.options.workspaces_root;
    request.cwd_utf8 = fixture.options.launch_cwd;
    request.launch_cwd = fixture.options.launch_cwd;
    request.v3_system_content = "managed storage stack system";
    request.wire_name = "fixture";
    return request;
}

struct StackResourceAudit {
    bool backend_alive = false, capture_retired_with_backend = false;
    unsigned sends = 0, capture_retirements = 0;
};
class StackNeverBackend final : public lubancode::api::Backend {
public:
    explicit StackNeverBackend(std::shared_ptr<StackResourceAudit> audit) : audit_(std::move(audit)) {
        audit_->backend_alive = true;
    }
    ~StackNeverBackend() override { audit_->backend_alive = false; }
    std::expected<void, lubancode::api::Error> send_stream(const lubancode::api::Request&,
        const std::function<void(const lubancode::api::StreamEvent&)>&, const std::atomic<bool>*) override {
        ++audit_->sends;
        return std::unexpected(lubancode::api::Error{lubancode::api::ErrorKind::Api, "must not execute"});
    }
private:
    std::shared_ptr<StackResourceAudit> audit_;
};
struct StackBorrowCapture {
    explicit StackBorrowCapture(std::shared_ptr<StackResourceAudit> value) : audit(std::move(value)) {}
    std::shared_ptr<StackResourceAudit> audit;
    ~StackBorrowCapture() { ++audit->capture_retirements; audit->capture_retired_with_backend = audit->backend_alive; }
};

void ManagedStorageOnly(lubancode::runtime::SessionService& service, const Fixture& fixture) {
    namespace rt = lubancode::runtime;
    REQUIRE(service.admission_mode() == rt::SessionAdmissionMode::ManagedStorageOnly);
    REQUIRE(service.runtime());
    REQUIRE(service.runtime()->admission_mode() == rt::SessionAdmissionMode::ManagedStorageOnly);
    auto* ledger = service.trajectory(); REQUIRE(ledger);
    REQUIRE(ledger->admission_mode() == rt::SessionAdmissionMode::ManagedStorageOnly);
    const auto stream = traj::TrajectoryDirectory::OpenExisting(ledger->session_dir()).v3_stream_path();
    const auto before = Read(stream);
    const auto input = service.SubmitInput({"must-not-admit", "private user input", {}});
    REQUIRE_FALSE(input.accepted); REQUIRE_FALSE(input.duplicate);
    REQUIRE(input.error_code == rt::kManagedStorageOnlyError);
    const auto pop = service.PopPendingInput();
    REQUIRE(pop.status == rt::SessionService::PendingPop::Status::NotAdmitted);
    REQUIRE(pop.error_code == rt::kManagedStorageOnlyError);
    REQUIRE(pop.input.operation_id.empty()); REQUIRE(service.pending_input_count() == 0);
    REQUIRE(service.PendingInputsSnapshot().empty());
    REQUIRE_FALSE(service.RecordTurnFinal({"fake-op", "fake-turn", "success", {}, false}));
    const auto command = service.ExecuteDomainCommand("must-not-run", {}, nullptr, nullptr, "", 0);
    REQUIRE_FALSE(command.accepted); REQUIRE(command.error_code == rt::kManagedStorageOnlyError);
    REQUIRE(service.runtime()->NoteWorkingDirectoryChanged(fixture.root / "must-not-resolve") == rt::kManagedStorageOnlyError);
    REQUIRE(ledger->HandleCwdChange(fixture.options.identity).error == rt::kManagedStorageOnlyError);
    REQUIRE(ledger->ClearSession({}, nullptr).error_code == rt::kManagedStorageOnlyError);
    REQUIRE(ledger->ResumeInteractive("missing-source", "fixture").outcome.error_code == rt::kManagedStorageOnlyError);
    REQUIRE_FALSE(ledger->NewTurnBridge({}));
    REQUIRE_FALSE(ledger->NewBypassBridge({}, lubancode::accounting::RequestPurpose::MemoryExtract));

    auto audit = std::make_shared<StackResourceAudit>();
    rt::assembly::SessionResourcesRequest resources;
    resources.backend_factory = [audit] { return std::make_unique<StackNeverBackend>(audit); };
    resources.registry_factory = [](std::span<const rt::assembly::McpServerRuntime>) -> rt::assembly::SessionRegistryResult {
        return std::make_unique<lubancode::tools::ToolRegistry>();
    };
    auto built = rt::assembly::BuildSessionResources(std::move(resources)); REQUIRE(built);
    auto capture = std::make_shared<StackBorrowCapture>(audit);
    std::weak_ptr<StackBorrowCapture> weak = capture;
    lubancode::agent::AgentProfile profile;
    profile.deferred_index_provider = [capture] { return std::string(); };
    capture.reset();
    REQUIRE_THROWS_WITH(service.InitializeExecution(std::move(*built), std::move(profile)), rt::kManagedStorageOnlyError);
    REQUIRE(weak.expired()); REQUIRE_FALSE(profile.deferred_index_provider);
    REQUIRE(audit->capture_retirements == 1); REQUIRE(audit->capture_retired_with_backend);
    REQUIRE_FALSE(audit->backend_alive); REQUIRE(audit->sends == 0);
    REQUIRE_FALSE(service.execution());
    REQUIRE(Read(stream) == before);
    REQUIRE_FALSE(fs::exists(ledger->session_dir() / "operations.jsonl"));
    REQUIRE_FALSE(fs::exists(ledger->session_dir() / "operations-inputs"));
    REQUIRE_FALSE(fs::exists(fixture.root / "must-not-resolve"));
}

void CheckManagedRuntimeOpeningStack() {
    namespace rt = lubancode::runtime;
    Fixture fixture;
    std::vector<std::unique_ptr<rt::SessionService>> services;
    std::vector<std::shared_ptr<const traj::ManagedSessionOwnershipPublication>> publications;
    for (unsigned i = 0; i != 2; ++i) {
        const auto id = "20261006-150000-STACK" + std::to_string(i);
        auto pending = fixture.Reserve(id);
        const auto published = pending->PublishOwnership(); REQUIRE(published.knowledge == Knowledge::Committed);
        auto finished = pending->Finish(); REQUIRE(finished);
        const auto directory = finished->directory().session_dir();
        const auto stream = finished->directory().v3_stream_path();
        const auto lock_bytes = Read(directory / "session.lock");
        auto request = ManagedStackRequest(fixture);
        unsigned callback_calls = 0;
        request.v3_opening_participant = [&](const traj::V3OpeningContext& context)
            -> std::expected<nlohmann::json, std::string> {
            ++callback_calls;
            REQUIRE(context.session_id == id); REQUIRE(context.session_dir == directory);
            REQUIRE(Read(directory / "session.lock") == lock_bytes);
            REQUIRE_FALSE(traj::SessionLock::Acquire(directory, traj::SessionManagerClock{}.LockOwner()));
            return nlohmann::json::object();
        };
        auto service = std::make_unique<rt::SessionService>(std::move(request), std::move(*finished), Creator());
        REQUIRE_MESSAGE(service->runtime() != nullptr, service->launch_error());
        REQUIRE(service->v3_format()); REQUIRE(callback_calls == 1);
        REQUIRE_FALSE(finished->lock().holds());
        pending.reset(); finished = std::unexpected(std::string("consumed stack opening"));
        REQUIRE(Read(directory / "session.lock") == lock_bytes);
        REQUIRE_FALSE(traj::SessionLock::Acquire(directory, traj::SessionManagerClock{}.LockOwner()));
        auto publication = service->trajectory()->managed_publication(); REQUIRE(publication);
        SameDurablePublication(*publication, published);
        REQUIRE(service->trajectory()->session_id() == id);
        auto ledger = v3::ReadV3Ledger(stream); REQUIRE(ledger);
        REQUIRE(ledger->session_id == id); REQUIRE(ledger->run_id == "main-0001");
        const auto bytes = Read(stream);
        const auto first = nlohmann::json::parse(bytes.substr(0, bytes.find('\n')));
        const auto managed = first.at("systemMeta").at("managedSession");
        REQUIRE(managed.at("sessionId") == id);
        REQUIRE(managed.at("tenantId") == published.expected.tenant_id);
        REQUIRE(managed.at("projectId") == published.expected.project_id);
        REQUIRE(managed.at("workspaceKey") == published.expected.workspace_key);
        REQUIRE(managed.at("bindingVersion") == published.expected.binding_version);
        REQUIRE(managed.at("creationSubject").at("userId") == Creator().user_id);
        REQUIRE(managed.at("openingPolicyRevision") == Creator().opening_policy_revision);
        ManagedStorageOnly(*service, fixture);
        publications.push_back(std::move(publication)); services.push_back(std::move(service));
    }
    const auto second_dir = services[1]->trajectory()->session_dir();
    const auto second_stream = traj::TrajectoryDirectory::OpenExisting(second_dir).v3_stream_path();
    const auto second_before = Read(second_stream);
    REQUIRE(services[0]->Close("managed-storage-only-test").error_code.empty());
    REQUIRE(Read(second_stream) == second_before);
    REQUIRE_FALSE(traj::SessionLock::Acquire(second_dir, traj::SessionManagerClock{}.LockOwner()));
    REQUIRE(services[1]->Close("managed-storage-only-test").error_code.empty());
    for (unsigned i = 0; i != 2; ++i) {
        const auto directory = services[i]->trajectory()->session_dir();
        REQUIRE(services[i]->trajectory()->managed_publication() == publications[i]);
        REQUIRE_FALSE(fs::exists(directory / "session.lock"));
        auto free = traj::SessionLock::Acquire(directory, traj::SessionManagerClock{}.LockOwner()); REQUIRE(free);
        free->Release();
        REQUIRE(Read(directory / traj::kManagedSessionOwnershipFile) == publications[i]->publication_bytes);
    }
    services.clear();
    REQUIRE(publications[0]->expected.session_id != publications[1]->expected.session_id);
    REQUIRE(publications[0]->expected.workspace_key == publications[1]->expected.workspace_key);

    for (unsigned mode = 0; mode != 6; ++mode) {
        const auto id = "20261006-150000-STACKFAIL" + std::to_string(mode);
        auto pending = fixture.Reserve(id);
        const auto published = pending->PublishOwnership(); REQUIRE(published.knowledge == Knowledge::Committed);
        auto finished = pending->Finish(); REQUIRE(finished);
        const auto directory = finished->directory().session_dir(); const auto stream = finished->directory().v3_stream_path();
        auto request = ManagedStackRequest(fixture); auto creation = Creator();
        unsigned callback_calls = 0;
        if (mode == 0) request.workspace_identity.reset();
        if (mode == 1) { request.resume_at_launch = true; request.resume_source_session_id = "must-not-read"; }
        if (mode == 2) creation.tenant_id = "foreign";
        if (mode == 3) Write(directory / traj::kManagedSessionOwnershipFile, "changed before transfer");
        request.v3_opening_participant = [&](const traj::V3OpeningContext&) -> std::expected<nlohmann::json, std::string> {
            ++callback_calls;
            REQUIRE_FALSE(traj::SessionLock::Acquire(directory, traj::SessionManagerClock{}.LockOwner()));
            if (mode == 4) throw std::runtime_error("PRIVATE_STACK_FAILURE");
            if (mode == 5) Write(directory / traj::kManagedSessionOwnershipFile, "changed during transfer callback");
            return nlohmann::json::object();
        };
        rt::SessionService failed(std::move(request), std::move(*finished), creation);
        REQUIRE_FALSE(failed.runtime()); REQUIRE_FALSE(failed.launch_error().empty());
        REQUIRE(failed.launch_error().find("PRIVATE_STACK_FAILURE") == std::string::npos);
        REQUIRE(failed.admission_mode() == rt::SessionAdmissionMode::ManagedStorageOnly);
        REQUIRE(callback_calls == (mode >= 4 ? 1 : 0));
        REQUIRE_FALSE(fs::exists(directory / "session.lock")); REQUIRE_FALSE(fs::exists(stream));
        REQUIRE(failed.SubmitInput({"key", "must not start", {}}).error_code == rt::kManagedStorageOnlyError);
        REQUIRE(failed.PopPendingInput().status == rt::SessionService::PendingPop::Status::NotAdmitted);
        REQUIRE_FALSE(fs::exists(directory / "operations.jsonl"));
        SameDurablePublication(pending->PublishOwnership(), published);
        REQUIRE(Read(directory / traj::kManagedSessionOwnershipFile) ==
            (mode == 3 ? "changed before transfer" : mode == 5 ? "changed during transfer callback" : published.publication_bytes));
        auto free = traj::SessionLock::Acquire(directory, traj::SessionManagerClock{}.LockOwner()); REQUIRE(free);
    }

    // The lower explicit entry points cannot turn an invalid Managed opening
    // into Local launch/resume or a later cwd-triggered opening.
    for (unsigned layer = 0; layer != 2; ++layer) {
        const auto id = "20261006-150000-STACKLAYER" + std::to_string(layer);
        auto pending = fixture.Reserve(id);
        const auto published = pending->PublishOwnership(); REQUIRE(published.knowledge == Knowledge::Committed);
        auto finished = pending->Finish(); REQUIRE(finished);
        const auto directory = finished->directory().session_dir(); const auto stream = finished->directory().v3_stream_path();
        if (layer == 0) {
            auto options = rt::SessionService::BuildRuntimeOptions(ManagedStackRequest(fixture));
            options.trajectory_resume_at_launch = true;
            rt::SessionRuntime rejected(std::move(options), std::move(*finished), Creator());
            REQUIRE_FALSE(rejected.trajectory());
            REQUIRE(rejected.trajectory_open_error() == "managed.session.invalid_launch_options");
            REQUIRE(rejected.admission_mode() == rt::SessionAdmissionMode::ManagedStorageOnly);
            REQUIRE(rejected.NoteWorkingDirectoryChanged(fixture.root / "unopened-cwd") == rt::kManagedStorageOnlyError);
        } else {
            rt::TrajectorySessionLedger::Options options;
            options.workspaces_root = fixture.options.workspaces_root;
            options.workspace_identity = fixture.options.identity;
            options.resume_at_launch = true;
            const auto rejected = rt::TrajectorySessionLedger::OpenManaged(std::move(options), std::move(*finished), Creator());
            REQUIRE_FALSE(rejected); REQUIRE(rejected.error() == "managed.session.invalid_launch_options");
        }
        REQUIRE_FALSE(fs::exists(directory / "session.lock")); REQUIRE_FALSE(fs::exists(stream));
        REQUIRE_FALSE(fs::exists(fixture.root / "unopened-cwd"));
        REQUIRE(Read(directory / traj::kManagedSessionOwnershipFile) == published.publication_bytes);
        SameDurablePublication(pending->PublishOwnership(), published);
        auto free = traj::SessionLock::Acquire(directory, traj::SessionManagerClock{}.LockOwner()); REQUIRE(free);
    }
}
// All fixtures below exercise the real move-only admission, native writers and
// strict owned consumer. They extend an existing CASE, without another runtime.
struct OperationFixture {
    Fixture fixture;
    std::unique_ptr<lubancode::runtime::SessionService> service;
    fs::path directory, stream;
    OperationFixture() {
        auto reserved = fixture.Reserve();
        const auto published = reserved->PublishOwnership();
        REQUIRE(published.knowledge == Knowledge::Committed);
        REQUIRE(published.native); REQUIRE(published.native->has_value());
        REQUIRE(published.native->value().outcome == platform::WriteOutcome::CommittedDurable);
        auto finished = reserved->Finish(); REQUIRE(finished);
        directory = finished->directory().session_dir(); stream = finished->directory().v3_stream_path();
        service = std::make_unique<lubancode::runtime::SessionService>(
            ManagedStackRequest(fixture), std::move(*finished), Creator());
        REQUIRE_MESSAGE(service->launch_error().empty(), service->launch_error());
        REQUIRE(service->runtime()); REQUIRE_FALSE(service->execution());
        REQUIRE_FALSE(traj::SessionLock::Acquire(directory, traj::SessionManagerClock{}.LockOwner()));
    }
    ~OperationFixture() {
        platform::SetFileFlushFailureForTest(false); platform::SetDirectoryFlushFailureForTest(false);
        try { if (service) (void)service->Close("exit"); } catch (...) {}
    }
    lubancode::runtime::ManagedOperationAdmission Admission() const {
        // The actual input initiator differs from the session's creator.
        return {fixture.Owner(), {"tenant-a", "operator-b", "agent", "credential-b"}, 41, {"RequestModel"}};
    }
    void Unlocked() const {
        auto lock = traj::SessionLock::Acquire(directory, traj::SessionManagerClock{}.LockOwner());
        REQUIRE_MESSAGE(lock.has_value(), (lock ? "" : lock.error()));
    }
};

std::string OperationJson(const nlohmann::json& value) {
    auto bytes = traj::CanonicalJsonDump(value); REQUIRE(bytes);
    return *bytes;
}
std::vector<nlohmann::json> OperationRows(const std::string& bytes) {
    std::vector<nlohmann::json> rows;
    std::size_t start = 0;
    while (start != bytes.size()) {
        const auto end = bytes.find('\n', start); REQUIRE(end != std::string::npos);
        auto row = nlohmann::json::parse(bytes.substr(start, end - start), nullptr, false);
        REQUIRE(row.is_object()); rows.push_back(std::move(row)); start = end + 1;
    }
    return rows;
}
std::string OperationLines(const std::vector<nlohmann::json>& rows) {
    std::string bytes;
    for (const auto& row : rows) bytes += OperationJson(row) + "\n";
    return bytes;
}
void CheckManagedOperationMaterials(const lubancode::runtime::ManagedOperationMaterials& actual) {
    namespace rt = lubancode::runtime;
    const auto source_rows = OperationRows(actual.operations);
    REQUIRE(source_rows.size() == 4);
    REQUIRE(rt::ReadManagedOperationsOwned(actual));
    auto bad = actual; bad.inputs.erase("op-1"); REQUIRE_FALSE(rt::ReadManagedOperationsOwned(bad));
    bad = actual; bad.inputs.emplace("foreign", actual.inputs.at("op-1")); REQUIRE_FALSE(rt::ReadManagedOperationsOwned(bad));
    bad = actual; std::swap(bad.inputs.at("op-1"), bad.inputs.at("op-2")); REQUIRE_FALSE(rt::ReadManagedOperationsOwned(bad));
    bad = actual; bad.inputs.at("op-1")[0] = '['; REQUIRE_FALSE(rt::ReadManagedOperationsOwned(bad));
    bad = actual; ++bad.owner.binding_version; REQUIRE_FALSE(rt::ReadManagedOperationsOwned(bad));
    bad = actual; bad.owner.tenant_id = "foreign-tenant"; REQUIRE_FALSE(rt::ReadManagedOperationsOwned(bad));
    bad = actual; bad.run_id = "foreign-run"; REQUIRE_FALSE(rt::ReadManagedOperationsOwned(bad));
    for (unsigned variant = 0; variant != 11; ++variant) {
        auto rows = source_rows;
        switch (variant) {
        case 0: rows[0].erase("provenance"); break;
        case 1: rows[0]["schemaVersion"] = 2; break; // Local cannot silently become Managed.
        case 2: rows[0]["provenance"]["initiatingSubject"].erase("credentialId"); break;
        case 3: rows[0]["provenance"]["admissionPolicyRevision"] = 0; break;
        case 4: rows[0]["provenance"]["allowedCapabilities"] = nlohmann::json::array({"RequestModel", "RunTool"}); break;
        case 5: rows[0]["provenance"]["intentKind"] = "managed.text_submit.v2"; break;
        case 6: rows[2]["decisionPolicyRevision"] = 0; break; // Not a confirmed denial.
        case 7: rows[2]["provenanceHash"] = source_rows[1]["provenanceHash"]; break;
        case 8: rows[2]["kind"] = "operation.dispatched"; break;
        case 9: rows[2]["kind"] = "operation.final"; break;
        case 10: rows[2]["rejectedAtMs"] = -1; break;
        }
        bad = actual; bad.operations = OperationLines(rows); REQUIRE_FALSE(rt::ReadManagedOperationsOwned(bad));
    }
    bad = actual; bad.operations += OperationJson(source_rows[0]) + "\n"; REQUIRE_FALSE(rt::ReadManagedOperationsOwned(bad));
    bad = actual; bad.operations += OperationJson(source_rows[2]) + "\n"; REQUIRE_FALSE(rt::ReadManagedOperationsOwned(bad));
    bad = actual; bad.operations.pop_back(); REQUIRE_FALSE(rt::ReadManagedOperationsOwned(bad));
    bad = actual; bad.operations.insert(1, " "); REQUIRE_FALSE(rt::ReadManagedOperationsOwned(bad));
    // Even a new input SHA/length cannot adopt a different original text or ID.
    for (unsigned variant = 0; variant != 3; ++variant) {
        bad = actual;
        auto input = nlohmann::json::parse(bad.inputs.at("op-1"));
        if (variant == 0) input["text"] = "rehashed foreign text";
        if (variant == 1) input["inputId"] = "foreign-input";
        if (variant == 2) input["provenance"]["initiatingSubject"]["userId"] = Creator().user_id;
        bad.inputs.at("op-1") = OperationJson(input);
        auto rows = source_rows;
        rows[0]["inputSha256"] = platform::Sha256Hex(bad.inputs.at("op-1"));
        rows[0]["inputBytes"] = bad.inputs.at("op-1").size();
        bad.operations = OperationLines(rows); REQUIRE_FALSE(rt::ReadManagedOperationsOwned(bad));
    }
    REQUIRE_FALSE(rt::SessionService::ReadOperationFactsOwned(actual.operations));
}

struct OperationNativeProbe final : traj::JournalNativeIoProbe {
    traj::JournalNativeStage stage = traj::JournalNativeStage::FileSync;
    unsigned observations = 0;
    bool After(const traj::JournalNativeIoResult& actual) noexcept override {
        if (actual.stage != stage || !actual.attempted || !actual.succeeded) return false;
        ++observations;
        return true; // Uncertainty after an actual native success, not a fake IO.
    }
};

void CheckManagedOperationProvenance() {
    namespace rt = lubancode::runtime;
    using Receipt = rt::SessionService::ManagedWriteReceipt;
    using State = rt::ManagedStoredOperation::State;
    {
        Fixture fixture;
        rt::SessionService local(ManagedStackRequest(fixture));
        REQUIRE(local.runtime()); REQUIRE(local.admission_mode() == rt::SessionAdmissionMode::LocalTrusted);
        const auto input = local.SubmitManagedInput({"must-not-promote-local", "text", {}},
            {fixture.Owner(), {"tenant-a", "operator-b", "agent", "credential-b"}, 41, {"RequestModel"}});
        REQUIRE(input->input.error_code == "managed.operation.not_admitted");
        REQUIRE_FALSE(input->artifact); REQUIRE_FALSE(input->append); REQUIRE_FALSE(input->operation);
        const auto capture = local.CaptureManagedOperationMaterials(); REQUIRE_FALSE(capture);
        REQUIRE(capture.error() == "managed.operation.not_admitted");
        REQUIRE(local.pending_input_count() == 0); REQUIRE(local.Close("exit").error_code.empty());
    }
    {
        OperationFixture fixture; auto& service = *fixture.service;
        auto empty = service.CaptureManagedOperationMaterials(); REQUIRE(empty);
        REQUIRE(empty->completion_known); REQUIRE(empty->operations.empty()); REQUIRE(empty->inputs.empty());
        const auto empty_operations = rt::ReadManagedOperationsOwned(*empty);
        REQUIRE(empty_operations); REQUIRE(empty_operations->empty());
        REQUIRE_FALSE(fs::exists(fixture.directory / "operations.jsonl"));
        const auto original_main = Read(fixture.stream);
        const auto ownership_path = fixture.directory / traj::kManagedSessionOwnershipFile;
        const auto original_owner = Read(ownership_path);
        Write(ownership_path, "malformed-owner");
        const auto changed = service.SubmitManagedInput({"key-a", "first text", {}}, fixture.Admission());
        REQUIRE(changed->input.error_code == "managed.operation.owner_changed"); REQUIRE_FALSE(changed->artifact);
        Write(ownership_path, original_owner);
        const auto first = service.SubmitManagedInput({"key-a", "first text", {}}, fixture.Admission());
        REQUIRE(first->knowledge == Receipt::Knowledge::Committed); REQUIRE(first->input.accepted);
        REQUIRE(first->operation); REQUIRE(first->artifact); REQUIRE(first->append);
        REQUIRE(first->artifact->outcome == platform::WriteOutcome::CommittedDurable);
        REQUIRE(first->artifact->body.succeeded); REQUIRE(first->artifact->file_sync.succeeded);
        REQUIRE(first->artifact->publish.succeeded); REQUIRE(first->artifact->parent_sync.succeeded);
        REQUIRE_FALSE(first->artifact->ancestor_chain_confirmed);
        REQUIRE(first->append->status == traj::JournalAppendStatus::Committed);
        REQUIRE(first->append->confirmed_durability == traj::Durability::PowerLoss);
        REQUIRE(first->append->body.succeeded); REQUIRE(first->append->newline.succeeded);
        REQUIRE(first->append->flush.succeeded); REQUIRE(first->append->file_sync.succeeded); REQUIRE_FALSE(first->append->failure);
        REQUIRE(first->operation->provenance.admission.subject == fixture.Admission().subject);
        REQUIRE(first->operation->provenance.admission.subject.user_id != Creator().user_id);
        REQUIRE(first->input.operation_id == "op-1"); REQUIRE(first->input.input_id == "in-1");
        REQUIRE(first->operation->provenance.execution_id == "op-1");
        const auto accepted_bytes = Read(fixture.directory / "operations.jsonl");
        const auto original_input = Read(fixture.directory / "operations-inputs" / "op-1.json");
        REQUIRE(platform::Sha256Hex(original_input) == first->operation->input_sha256);
        auto current = fixture.Admission(); current.policy_revision = 99;
        const auto duplicate = service.SubmitManagedInput({"key-a", "first text", {}}, current);
        REQUIRE(duplicate->input.duplicate); REQUIRE_FALSE(duplicate->input.accepted);
        REQUIRE(duplicate->operation == first->operation); REQUIRE(duplicate->operation->provenance.admission.policy_revision == 41);
        REQUIRE_FALSE(duplicate->artifact); REQUIRE_FALSE(duplicate->append);
        for (unsigned variant = 0; variant != 8; ++variant) {
            auto foreign = fixture.Admission(); std::string text = "first text";
            if (variant == 0) foreign.subject.user_id = "foreign-user";
            if (variant == 1) foreign.subject.credential_id = "foreign-credential";
            if (variant == 2) foreign.owner.project_id = "foreign-project";
            if (variant == 3) ++foreign.owner.binding_version;
            if (variant == 4) foreign.allowed_capabilities.push_back("RunTool");
            if (variant == 5) text = "different text";
            if (variant == 6) foreign.policy_revision = 0;
            if (variant == 7) foreign.subject.actor_kind = "foreign-actor-kind";
            const auto denied = service.SubmitManagedInput({"key-a", text, {}}, foreign);
            REQUIRE_FALSE(denied->input.accepted); REQUIRE_FALSE(denied->input.duplicate);
            REQUIRE(denied->input.operation_id.empty()); REQUIRE(denied->input.input_id.empty());
            REQUIRE(denied->input.payload_hash.empty()); REQUIRE_FALSE(denied->operation);
            REQUIRE_FALSE(denied->artifact); REQUIRE_FALSE(denied->append);
        }
        REQUIRE_FALSE(service.SubmitManagedInput({"", "no key", {}}, fixture.Admission())->input.accepted);
        rt::SessionService::InputRequest image_input{"with-image", "text", {}};
        image_input.images.emplace_back();
        REQUIRE_FALSE(service.SubmitManagedInput(image_input, fixture.Admission())->input.accepted);
        REQUIRE(Read(fixture.directory / "operations.jsonl") == accepted_bytes);
        REQUIRE(service.pending_input_count() == 1); REQUIRE(service.ManagedPendingFront() == first->operation);
        const auto second = service.SubmitManagedInput({"key-b", "second text", {}}, fixture.Admission());
        REQUIRE(second->input.accepted); REQUIRE(second->input.operation_id == "op-2");
        auto wrong_front = first->operation->provenance; wrong_front.input_id = "foreign-input";
        REQUIRE_FALSE(service.RejectManagedPendingInput(wrong_front, "rejected", "policy.denied", 42)->append);
        REQUIRE_FALSE(service.RejectManagedPendingInput(first->operation->provenance, "rejected", "policy.denied", 0)->append);
        REQUIRE(service.pending_input_count() == 2);
        const auto rejected = service.RejectManagedPendingInput(first->operation->provenance, "rejected", "policy.denied", 42);
        REQUIRE(rejected->knowledge == Receipt::Knowledge::Committed); REQUIRE(rejected->append);
        REQUIRE(rejected->operation->state == State::RejectedBeforeDispatch);
        REQUIRE(service.pending_input_count() == 1); REQUIRE(service.ManagedPendingFront() == second->operation);
        REQUIRE(service.PopPendingInput().status == rt::SessionService::PendingPop::Status::NotAdmitted);
        REQUIRE_FALSE(service.RecordTurnFinal({"op-2", "fake-turn", "success", {}, false}));
        REQUIRE_FALSE(service.execution()); REQUIRE(Read(fixture.stream) == original_main);
        const auto closed = service.Close("exit"); REQUIRE_MESSAGE(closed.error_code.empty(), closed.error_code);
        REQUIRE(service.pending_input_count() == 0); REQUIRE(service.ManagedOperationCloseReceipt());
        const auto operation_close = service.ManagedOperationCloseReceipt(); REQUIRE(operation_close);
        REQUIRE(operation_close->status == traj::JournalCloseReceipt::Status::Closed);
        const auto main_close = service.ManagedMainCloseOutcome(); REQUIRE(main_close);
        REQUIRE(main_close->error_code.empty()); fixture.Unlocked();
        const auto captured = service.CaptureManagedOperationMaterials(); REQUIRE(captured); REQUIRE(captured->completion_known);
        REQUIRE(captured->operations == Read(fixture.directory / "operations.jsonl"));
        REQUIRE(captured->inputs.at("op-1") == original_input);
        const auto stored = rt::ReadManagedOperationsOwned(*captured); REQUIRE(stored); REQUIRE(stored->size() == 2);
        REQUIRE((*stored)[0].terminal_status == "rejected"); REQUIRE((*stored)[0].terminal_policy_revision == 42);
        REQUIRE((*stored)[1].terminal_status == "cancelled"); REQUIRE((*stored)[1].terminal_policy_revision == 0);
        REQUIRE((*stored)[1].reason_code == "session.closed");
        CheckManagedOperationMaterials(*captured);
        const auto main = v3::ReadV3Ledger(fixture.stream); REQUIRE(main); REQUIRE(main->messages.size() == 1);
        for (const auto& event : main->events) {
            const std::string kind = v3::EventKindV3Name(event.kind);
            REQUIRE_FALSE(kind.starts_with("turn.")); REQUIRE_FALSE(kind.starts_with("model."));
            REQUIRE_FALSE(kind.starts_with("action.")); REQUIRE(kind != "sdk.operation.turn.bound");
        }
        REQUIRE_FALSE(fs::exists(fixture.directory / "sdk-results"));
        const auto final_bytes = captured->operations;
        REQUIRE(service.Close("exit").error_code == closed.error_code);
        REQUIRE(service.SubmitManagedInput({"late-key", "late text", {}}, current)->input.error_code == "session.stopping");
        fixture.service.reset(); REQUIRE(rt::ReadManagedOperationsOwned(*captured));
        REQUIRE(Read(fixture.directory / "operations.jsonl") == final_bytes);
    }
    for (const bool after_publish : {false, true}) {
        OperationFixture fixture; auto& service = *fixture.service;
        if (after_publish) platform::SetDirectoryFlushFailureForTest(true);
        else platform::SetFileFlushFailureForTest(true);
        const auto failed = service.SubmitManagedInput({"native-key", "real artifact write", {}}, fixture.Admission());
        platform::SetFileFlushFailureForTest(false); platform::SetDirectoryFlushFailureForTest(false);
        REQUIRE_FALSE(failed->input.accepted); REQUIRE(failed->artifact); REQUIRE_FALSE(failed->append);
        REQUIRE(failed->artifact->outcome == (after_publish ? platform::WriteOutcome::CommittedDurabilityUnconfirmed : platform::WriteOutcome::NotCommitted));
        REQUIRE(service.FirstManagedWriteFailure() == failed); REQUIRE(service.pending_input_count() == 0);
        REQUIRE(fs::exists(fixture.directory / "operations-inputs" / "op-1.json") == after_publish);
        const auto late = service.SubmitManagedInput({"native-key", "real artifact write", {}}, fixture.Admission());
        REQUIRE(late->input.error_code == "managed.operation.storage_unconfirmed"); REQUIRE_FALSE(late->artifact);
        REQUIRE_FALSE(service.Close("exit").error_code.empty()); fixture.Unlocked();
        const auto operation_close = service.ManagedOperationCloseReceipt(); REQUIRE(operation_close);
        REQUIRE(operation_close->status == traj::JournalCloseReceipt::Status::NoOpenHandle);
        const auto main_close = service.ManagedMainCloseOutcome(); REQUIRE(main_close);
        REQUIRE(main_close->error_code.empty()); REQUIRE(service.FirstManagedWriteFailure() == failed);
        const auto captured = service.CaptureManagedOperationMaterials();
        if (after_publish) REQUIRE_FALSE(captured); // The orphan does not become a fabricated acceptance.
        else { REQUIRE(captured); REQUIRE_FALSE(captured->completion_known); REQUIRE(captured->operations.empty()); }
    }
    {
        OperationFixture fixture; auto& service = *fixture.service;
        auto probe = std::make_shared<OperationNativeProbe>(); service.SetManagedOperationProbesForTest(probe);
        const auto failed = service.SubmitManagedInput({"append-key", "real committed bytes", {}}, fixture.Admission());
        REQUIRE(failed->knowledge == Receipt::Knowledge::Unconfirmed); REQUIRE(failed->append); REQUIRE(failed->artifact);
        REQUIRE(failed->artifact->outcome == platform::WriteOutcome::CommittedDurable);
        REQUIRE(failed->append->status == traj::JournalAppendStatus::Unconfirmed); REQUIRE(failed->append->failure);
        REQUIRE(failed->append->failure->stage == traj::JournalNativeStage::FileSync);
        REQUIRE(failed->append->file_sync.succeeded); REQUIRE(failed->append->file_sync.injected_unconfirmed);
        REQUIRE(probe->observations == 1); REQUIRE(service.FirstManagedWriteFailure() == failed);
        const auto captured = service.CaptureManagedOperationMaterials(); REQUIRE(captured); REQUIRE_FALSE(captured->completion_known);
        const auto stored = rt::ReadManagedOperationsOwned(*captured); REQUIRE(stored);
        REQUIRE(stored->size() == 1); // Reading never upgrades the native witness.
        REQUIRE_FALSE(service.Close("exit").error_code.empty()); fixture.Unlocked();
        const auto operation_close = service.ManagedOperationCloseReceipt(); REQUIRE(operation_close);
        REQUIRE_FALSE(operation_close->ok()); REQUIRE(service.FirstManagedWriteFailure() == failed);
        REQUIRE(failed->append->status == traj::JournalAppendStatus::Unconfirmed);
    }
    for (const bool rejection_gap : {false, true}) {
        OperationFixture fixture; auto& service = *fixture.service;
        auto publications = std::make_shared<unsigned>(0);
        service.SetManagedOperationProbesForTest({}, [publications, rejection_gap] {
            ++*publications;
            if (*publications == (rejection_gap ? 2u : 1u)) throw std::runtime_error("private semantic failure must not escape");
        });
        const auto accepted = service.SubmitManagedInput({"semantic-key", "real successful native append", {}}, fixture.Admission());
        REQUIRE(accepted->operation);
        if (rejection_gap) REQUIRE(accepted->input.accepted);
        const auto failed = rejection_gap ? service.RejectManagedPendingInput(
            accepted->operation->provenance, "rejected", "policy.denied", 42) : accepted;
        REQUIRE(failed->knowledge == Receipt::Knowledge::NativeCommittedPublicationGap); REQUIRE(failed->append);
        REQUIRE(failed->append->status == traj::JournalAppendStatus::Committed); REQUIRE_FALSE(failed->append->failure);
        REQUIRE(failed->append->confirmed_durability == traj::Durability::PowerLoss);
        REQUIRE(service.FirstManagedWriteFailure() == failed); REQUIRE_FALSE(service.ManagedPendingFront());
        REQUIRE(service.pending_input_count() == (rejection_gap ? 1u : 0u));
        REQUIRE(failed->input.error_code == "managed.operation.publication_unconfirmed");
        const auto captured = service.CaptureManagedOperationMaterials(); REQUIRE(captured); REQUIRE_FALSE(captured->completion_known);
        const auto read = rt::ReadManagedOperationsOwned(*captured); REQUIRE(read); REQUIRE(read->size() == 1);
        REQUIRE(read->front().state == (rejection_gap ? State::RejectedBeforeDispatch : State::Accepted));
        REQUIRE(service.SubmitManagedInput({"semantic-key", "real successful native append", {}}, fixture.Admission())->input.error_code == "managed.operation.storage_unconfirmed");
        const auto first_bytes = Read(fixture.directory / "operations.jsonl");
        const auto close = service.Close("exit"); REQUIRE_FALSE(close.error_code.empty()); REQUIRE(close.close_quality == "incomplete");
        REQUIRE(Read(fixture.directory / "operations.jsonl") == first_bytes); REQUIRE(service.FirstManagedWriteFailure() == failed);
        const auto operation_close = service.ManagedOperationCloseReceipt(); REQUIRE(operation_close);
        const auto main_close = service.ManagedMainCloseOutcome(); REQUIRE(main_close);
        REQUIRE(operation_close->ok()); REQUIRE(main_close->error_code.empty());
        REQUIRE(service.Close("exit").error_code == close.error_code); fixture.Unlocked();
        const auto closed_materials = service.CaptureManagedOperationMaterials(); REQUIRE(closed_materials);
        REQUIRE_FALSE(closed_materials->completion_known);
        fixture.service.reset();
        REQUIRE_FALSE(captured->completion_known); REQUIRE_FALSE(closed_materials->completion_known);
        REQUIRE(rt::ReadManagedOperationsOwned(*captured));
        REQUIRE(failed->append->status == traj::JournalAppendStatus::Committed); REQUIRE_FALSE(failed->append->failure);
    }
    {
        OperationFixture fixture; auto& service = *fixture.service;
        auto probe = std::make_shared<OperationNativeProbe>(); probe->stage = traj::JournalNativeStage::Close;
        service.SetManagedOperationProbesForTest(probe);
        REQUIRE(service.SubmitManagedInput({"close-key", "real accepted input", {}}, fixture.Admission())->input.accepted);
        const auto close = service.Close("exit"); REQUIRE_FALSE(close.error_code.empty());
        const auto receipt = service.ManagedOperationCloseReceipt(); REQUIRE(receipt); REQUIRE(receipt->native);
        REQUIRE(receipt->status == traj::JournalCloseReceipt::Status::Unconfirmed);
        REQUIRE(receipt->native->succeeded); REQUIRE(receipt->native->injected_unconfirmed); REQUIRE(probe->observations == 1);
        const auto main_close = service.ManagedMainCloseOutcome(); REQUIRE(main_close);
        const auto closed_materials = service.CaptureManagedOperationMaterials(); REQUIRE(closed_materials);
        REQUIRE(main_close->error_code.empty()); REQUIRE_FALSE(closed_materials->completion_known);
        REQUIRE(service.Close("exit").error_code == close.error_code); REQUIRE(probe->observations == 1);
        fixture.Unlocked();
    }
}

} // namespace

TEST_CASE("managed reservation: owned durable publication precedes directories and real V3 writer") {
    CheckManagedRuntimeOpeningStack();
    Fixture fixture;
    auto pending = fixture.Reserve();
    const auto directory = pending->session_dir();
    REQUIRE(Entries(directory) == std::vector<std::string>{"session.lock"});
    const auto lock_bytes = Read(directory / "session.lock");
    NoBody(directory);
    const auto published = pending->PublishOwnership();
    REQUIRE(published.knowledge == Knowledge::Committed);
    REQUIRE(published.native.has_value());
    REQUIRE(published.native->has_value());
    REQUIRE(published.native->value().outcome == platform::WriteOutcome::CommittedDurable);
    REQUIRE(published.requested == platform::WriteDurability::ProcessCrashDurability);
    REQUIRE(Read(directory / traj::kManagedSessionOwnershipFile) == published.publication_bytes);
    NoBody(directory);
    REQUIRE(pending->PublishOwnership().publication_bytes == published.publication_bytes);
    auto finished = pending->Finish();
    REQUIRE_MESSAGE(finished.has_value(), (finished ? "" : finished.error()));
    REQUIRE(finished->lock().holds());
    REQUIRE(Read(directory / "session.lock") == lock_bytes);
    REQUIRE(fs::is_directory(directory / "artifacts"));
    REQUIRE(fs::is_directory(directory / "subagents"));
    REQUIRE_FALSE(fs::exists(finished->directory().v3_stream_path()));
    REQUIRE_FALSE(pending->Finish().has_value());
    pending.reset();
    REQUIRE(finished->lock().holds());
    REQUIRE_FALSE(traj::SessionLock::Acquire(directory, traj::SessionManagerClock{}.LockOwner()).has_value());
    auto writer = v3::V3Writer::Start(finished->directory().v3_stream_path(), fixture.Owner().session_id,
        "main-0001", "managed original system");
    REQUIRE_MESSAGE(writer.has_value(), (writer ? "" : writer.error()));
    REQUIRE(writer->Close().has_value());
    auto ledger = v3::ReadV3Ledger(finished->directory().v3_stream_path());
    REQUIRE_MESSAGE(ledger.has_value(), (ledger ? "" : ledger.error()));
    REQUIRE(ledger->session_id == fixture.Owner().session_id);
    REQUIRE(ledger->run_id == "main-0001");
    REQUIRE(ledger->lines == 2);
    REQUIRE(ledger->messages.at(0).message.at("content") == "managed original system");
    finished = std::unexpected(std::string("test releases finished owner"));
    REQUIRE_FALSE(fs::exists(directory / "session.lock"));
    REQUIRE(Read(directory / traj::kManagedSessionOwnershipFile) == published.publication_bytes);
    CheckManagedManagerOpening(fixture);
    Marker("actual");
}

TEST_CASE("managed reservation: only a fresh empty directory can be reserved and abandoned empties retire") {
    Fixture fixture;
    const auto id = fixture.Owner().session_id;
    const auto directory = fixture.Session(id);
    {
        auto pending = fixture.Reserve();
        REQUIRE(fs::exists(directory / "session.lock"));
    }
    REQUIRE_FALSE(fs::exists(directory));
    {
        auto pending = fixture.Reserve();
        const auto early = pending->Finish();
        REQUIRE_FALSE(early.has_value());
        REQUIRE(early.error() == "managed.reservation.durable_publication_required");
        REQUIRE_FALSE(fs::exists(directory));
        REQUIRE(pending->PublishOwnership().knowledge == Knowledge::RejectedBeforeIO);
        REQUIRE_FALSE(fs::exists(directory));
    }
    auto invalid = fixture.Owner();
    invalid.binding_version = 0;
    auto rejected = traj::ManagedSessionReservation::Reserve(fixture.options.workspaces_root,
        invalid, traj::SessionManagerClock{}.LockOwner());
    REQUIRE_FALSE(rejected.has_value());
    REQUIRE_FALSE(fs::exists(directory));
    REQUIRE(fs::create_directory(directory));
    Write(directory / "legacy.jsonl", "legacy exact bytes");
    const auto legacy = traj::ManagedSessionReservation::Reserve(fixture.options.workspaces_root,
        fixture.Owner(), traj::SessionManagerClock{}.LockOwner());
    REQUIRE_FALSE(legacy.has_value());
    REQUIRE(Read(directory / "legacy.jsonl") == "legacy exact bytes");
    REQUIRE(Entries(directory) == std::vector<std::string>{"legacy.jsonl"});
    REQUIRE_FALSE(traj::ManagedSessionReservation::Reserve(fs::path("relative"), fixture.Owner(),
        traj::SessionManagerClock{}.LockOwner()).has_value());
    invalid = fixture.Owner(); invalid.session_id = "../escape";
    REQUIRE_FALSE(traj::ManagedSessionReservation::Reserve(fixture.options.workspaces_root, invalid,
        traj::SessionManagerClock{}.LockOwner()).has_value());
    Marker("reserve");
}

TEST_CASE("managed reservation: an unknown real write retains its marker and never unlocks Finish") {
    Fixture fixture;
    auto pending = fixture.Reserve();
    const auto directory = pending->session_dir();
    platform::SetDirectoryFlushFailureForTest(true);
    const auto unknown = pending->PublishOwnership();
    platform::SetDirectoryFlushFailureForTest(false);
    REQUIRE(unknown.knowledge == Knowledge::Unconfirmed);
    REQUIRE(unknown.native.has_value());
    REQUIRE_FALSE(unknown.native->has_value());
    REQUIRE(unknown.native->error().outcome == platform::WriteOutcome::CommittedDurabilityUnconfirmed);
    REQUIRE(Read(directory / traj::kManagedSessionOwnershipFile) == unknown.publication_bytes);
    REQUIRE_FALSE(fs::exists(directory / "session.lock"));
    NoBody(directory);
    REQUIRE_FALSE(pending->Finish().has_value());
    REQUIRE(pending->PublishOwnership().knowledge == Knowledge::Unconfirmed);
    REQUIRE(pending->PublishOwnership().native->error().code == unknown.native->error().code);
    pending.reset();
    REQUIRE_FALSE(traj::ManagedSessionReservation::Reserve(fixture.options.workspaces_root,
        fixture.Owner(), traj::SessionManagerClock{}.LockOwner()).has_value());
    auto lock = traj::SessionLock::Acquire(directory, traj::SessionManagerClock{}.LockOwner());
    REQUIRE(lock.has_value());
    auto fresh = traj::ManagedSessionOwnershipAttempt::Prepare(directory, *lock, fixture.Owner());
    REQUIRE(fresh.has_value());
    const auto matching = (*fresh)->Publish(*lock);
    REQUIRE(matching.knowledge == Knowledge::ExistingMatching);
    REQUIRE_FALSE(matching.native.has_value());
    REQUIRE_FALSE(matching.requested.has_value());
    auto capture = traj::CaptureManagedSessionOwnershipLocked(directory, *lock);
    REQUIRE(capture.has_value());
    REQUIRE_FALSE(traj::CheckLocalTrustedSessionOwnership(*capture).has_value());
    NoBody(directory);
    REQUIRE(Read(directory / traj::kManagedSessionOwnershipFile) == unknown.publication_bytes);
    Marker("unknown");
}

TEST_CASE("managed reservation: pre-rename failure retains the actual first result and closes resources") {
    Fixture fixture;
    auto pending = fixture.Reserve();
    const auto directory = pending->session_dir();
    platform::SetFileFlushFailureForTest(true);
    const auto failed = pending->PublishOwnership();
    platform::SetFileFlushFailureForTest(false);
    REQUIRE(failed.knowledge == Knowledge::NotCommitted);
    REQUIRE(failed.native.has_value());
    REQUIRE_FALSE(failed.native->has_value());
    REQUIRE(failed.native->error().outcome == platform::WriteOutcome::NotCommitted);
    REQUIRE(Entries(directory).empty());
    REQUIRE_FALSE(pending->Finish().has_value());
    REQUIRE(pending->PublishOwnership().knowledge == Knowledge::NotCommitted);
    NoBody(directory);
    pending.reset();
    REQUIRE(fs::is_directory(directory));
    REQUIRE(Entries(directory).empty());
    REQUIRE_FALSE(traj::ManagedSessionReservation::Reserve(fixture.options.workspaces_root,
        fixture.Owner(), traj::SessionManagerClock{}.LockOwner()).has_value());
    Marker("not-committed");
}

TEST_CASE("managed reservation: Finish rechecks metadata and late entries without deleting residue") {
    Fixture fixture;
    for (const bool change_marker : {false, true}) {
        const auto id = change_marker ? "20261005-120000-DRIFT1" : "20261005-120000-ENTRY1";
        auto pending = fixture.Reserve(id);
        const auto directory = pending->session_dir();
        const auto receipt = pending->PublishOwnership();
        REQUIRE(receipt.knowledge == Knowledge::Committed);
        if (change_marker) Write(directory / traj::kManagedSessionOwnershipFile, "bad retained marker");
        else Write(directory / "late-entry", "foreign retained bytes");
        const auto result = pending->Finish();
        REQUIRE_FALSE(result.has_value());
        REQUIRE_FALSE(fs::exists(directory / "session.lock"));
        NoBody(directory);
        REQUIRE_FALSE(pending->Finish().has_value());
        pending.reset();
        REQUIRE(Read(directory / traj::kManagedSessionOwnershipFile) ==
            (change_marker ? "bad retained marker" : receipt.publication_bytes));
        if (!change_marker) REQUIRE(Read(directory / "late-entry") == "foreign retained bytes");
    }
    // Even an unpublished reservation must not recursively erase a late entry.
    auto abandoned = fixture.Reserve();
    const auto abandoned_dir = abandoned->session_dir();
    Write(abandoned_dir / "late-entry", "retain before publication");
    abandoned.reset();
    REQUIRE_FALSE(fs::exists(abandoned_dir / "session.lock"));
    REQUIRE(Read(abandoned_dir / "late-entry") == "retain before publication");
    CheckManagedOpeningFailures(fixture);
    CheckManagedRejectsActiveLocal(fixture);
    Marker("finish");
}

TEST_CASE("managed reservation: LocalTrusted old create and same-session resume remain available") {
    Fixture fixture(true);
    const auto directory = traj::TrajectoryDirectory::CreateSessionV3(fixture.options.workspaces_root,
        fixture.options.identity.workspace_key, "20261005-120000-LOCAL1");
    REQUIRE(directory.has_value());
    REQUIRE(fs::is_directory(directory->artifacts_root()));
    REQUIRE(fs::is_directory(directory->session_dir() / "subagents"));
    REQUIRE_FALSE(fs::exists(directory->session_dir() / traj::kManagedSessionOwnershipFile));
    REQUIRE_FALSE(traj::TrajectoryDirectory::CreateSessionV3(fixture.options.workspaces_root,
        fixture.options.identity.workspace_key, "20261005-120000-LOCAL1").has_value());
    const auto id = ClosedLocal(fixture);
    const auto stream = traj::TrajectoryDirectory::OpenExisting(fixture.Session(id)).v3_stream_path();
    const auto before = Read(stream);
    auto relative_options = fixture.options;
    relative_options.workspaces_root = fs::relative(fixture.options.workspaces_root, fs::current_path());
    REQUIRE(relative_options.workspaces_root.is_relative());
    traj::SessionManager manager(relative_options);
    REQUIRE(manager.ProbeResumeSource(id).ok());
    REQUIRE(manager.LatestResumableSessionId() == id);
    traj::ResumeRequest request; request.source_session_id = id;
    const auto resumed = manager.ResumeAsNew(request);
    REQUIRE_MESSAGE(resumed.error_code.empty(), (resumed.error_code + ": " + resumed.message));
    REQUIRE(resumed.active_switched);
    REQUIRE(manager.active()->session_id() == id);
    REQUIRE(manager.active()->lock.holds());
    REQUIRE(Read(stream).starts_with(before));
    REQUIRE(manager.Close({}, nullptr).error_code.empty());
    REQUIRE(v3::ReadV3Ledger(stream).has_value());
    Marker("local");
}

TEST_CASE("managed reservation: LocalTrusted point reads reject markers before poisoned content and enumeration skips them") {
    Fixture fixture;
    const auto local_id = ClosedLocal(fixture);
    std::vector<std::pair<fs::path, std::string>> marked;
    {
        auto pending = fixture.Reserve();
        const auto receipt = pending->PublishOwnership();
        REQUIRE(receipt.knowledge == Knowledge::Committed);
        auto finished = pending->Finish();
        REQUIRE(finished.has_value());
        marked.emplace_back(pending->session_dir(), receipt.publication_bytes);
    }
    const auto broken_dir = fixture.Session("20261005-120000-BROKEN");
    REQUIRE(fs::create_directory(broken_dir));
    Write(broken_dir / traj::kManagedSessionOwnershipFile, "broken-marker");
    marked.emplace_back(broken_dir, "broken-marker");
    traj::SessionManager manager(fixture.options);
    for (const auto& [directory, marker] : marked) {
        const auto id = platform::PathToUtf8(directory.filename());
        Write(directory / "session.json", "POISONED_MANIFEST");
        Write(directory / "main.jsonl", "POISONED_MAIN");
        Write(directory / platform::Utf8ToPath(id + ".jsonl"), "POISONED_V3");
        const auto probe = manager.ProbeResumeSource(id);
        REQUIRE_FALSE(probe.ok());
        REQUIRE(probe.error_code.starts_with("managed.ownership."));
        traj::ResumeRequest request; request.source_session_id = id;
        const auto resumed = manager.ResumeAsNew(request);
        REQUIRE(resumed.error_code.starts_with("managed.ownership."));
        REQUIRE_FALSE(manager.RecordResumeReference(id, "must not record").has_value());
        REQUIRE(traj::ArchiveSessionDir(fixture.workspace.workspace_dir(), id, 1759000000001LL)
            .error_code.starts_with("managed.ownership."));
        REQUIRE(traj::UnarchiveSessionDir(fixture.workspace.workspace_dir(), id, 1759000000002LL)
            .error_code.starts_with("managed.ownership."));
        REQUIRE(traj::DeleteSessionDir(fixture.workspace.workspace_dir(), id, "must not delete", 1759000000003LL)
            .error_code.starts_with("managed.ownership."));
        REQUIRE(Read(directory / traj::kManagedSessionOwnershipFile) == marker);
        REQUIRE(Read(directory / "main.jsonl") == "POISONED_MAIN");
        REQUIRE(Read(directory / platform::Utf8ToPath(id + ".jsonl")) == "POISONED_V3");
    }
    REQUIRE(manager.LatestResumableSessionId() == local_id);
    const auto recovery = manager.RecoverWorkspace();
    for (const auto& [directory, marker] : marked) {
        REQUIRE(Read(directory / traj::kManagedSessionOwnershipFile) == marker);
        REQUIRE(std::none_of(recovery.sessions.begin(), recovery.sessions.end(), [&](const auto& entry) {
            return entry.session_id == platform::PathToUtf8(directory.filename());
        }));
    }
    const auto refs = traj::ScanIncomingSessionRefs(fixture.workspace.workspace_dir(), local_id);
    REQUIRE(refs.ownership_unreadable_sessions.size() == marked.size());
    REQUIRE(refs.resume_referrers.empty());
    REQUIRE(refs.subagent_referrers.empty());
    const auto deleted = traj::DeleteSessionDir(fixture.workspace.workspace_dir(), local_id, "unknown peers", 1759000000004LL);
    REQUIRE(deleted.error_code == "session.delete_references_unknown");
    REQUIRE(fs::exists(fixture.Session(local_id)));
    Marker("refusal");
}

TEST_CASE("managed reservation: a marker arriving after resume preflight prevents locked opening") {
    Fixture fixture;
    const auto id = ClosedLocal(fixture);
    const auto directory = fixture.Session(id);
    const auto stream = traj::TrajectoryDirectory::OpenExisting(directory).v3_stream_path();
    const auto original = Read(stream);
    unsigned participants = 0;
    fixture.options.v3_opening_participant = [&](const traj::V3OpeningContext&)
        -> std::expected<nlohmann::json, std::string> {
        ++participants;
        return nlohmann::json::object();
    };
    OpeningClock clock;
    traj::SessionManager manager(fixture.options, &clock);
    REQUIRE(manager.ProbeResumeSource(id).ok());
    clock.before_lock = [&] { Write(directory / traj::kManagedSessionOwnershipFile, "late-bad-marker"); };
    traj::ResumeRequest request; request.source_session_id = id;
    const auto resumed = manager.ResumeAsNew(request);
    REQUIRE(resumed.error_code == "resume.step5_failed");
    REQUIRE(resumed.message.find("managed.ownership.") != std::string::npos);
    REQUIRE(participants == 0);
    REQUIRE_FALSE(fs::exists(directory / "session.lock"));
    REQUIRE(Read(stream) == original);
    REQUIRE(Read(directory / traj::kManagedSessionOwnershipFile) == "late-bad-marker");
    Marker("locked-refusal");
}

TEST_CASE("managed reservation: a valid local source cannot read a marked ancestor through its resolver") {
    Fixture fixture;
    auto pending = fixture.Reserve();
    REQUIRE(pending->PublishOwnership().knowledge == Knowledge::Committed);
    auto finished = pending->Finish();
    REQUIRE(finished.has_value());
    const auto ancestor_stream = finished->directory().v3_stream_path();
    auto ancestor = v3::V3Writer::Start(ancestor_stream, fixture.Owner().session_id, "main-0001", "ancestor");
    REQUIRE(ancestor.has_value());
    REQUIRE(ancestor->Close().has_value());
    auto ledger = v3::ReadV3Ledger(ancestor_stream);
    REQUIRE(ledger.has_value());
    const auto tail = ledger->LastEntry();
    REQUIRE(tail.has_value());
    const auto tail_id = tail->is_message ? ledger->messages[tail->index].message_id
                                          : ledger->events[tail->index].event_id;
    const auto tail_hash = tail->is_message ? ledger->messages[tail->index].line_hash
                                            : ledger->events[tail->index].line_hash;
    const std::string local_id = "20261005-120000-CHILD1";
    auto local_dir = traj::TrajectoryDirectory::CreateSessionV3(fixture.options.workspaces_root,
        fixture.options.identity.workspace_key, local_id);
    REQUIRE(local_dir.has_value());
    auto local = v3::V3Writer::Start(local_dir->v3_stream_path(), local_id, "main-0002", "local");
    REQUIRE(local.has_value());
    v3::EventDraft attached;
    attached.kind = v3::EventKindV3::ResumeSourceAttached;
    attached.payload = {{"sourceRef", {{"sessionId", ledger->session_id}, {"runId", ledger->run_id},
        {"seq", tail->seq}, {"id", tail_id}, {"hash", tail_hash}}},
        {"contextRevision", ledger->context.revision}, {"systemMessageRef", ledger->context.system_message_ref},
        {"branch", "main"}};
    REQUIRE(local->AppendEvent(std::move(attached), traj::Durability::PowerLoss).status ==
        v3::WriteReceipt::Status::Committed);
    REQUIRE(local->Close().has_value());
    auto baseline = v3::ProjectResume(local_dir->v3_stream_path());
    REQUIRE(baseline.has_value());
    REQUIRE(baseline->source_chain_ok);
    REQUIRE(baseline->source_chain.size() == 1);
    const auto local_bytes = Read(local_dir->v3_stream_path());
    Write(ancestor_stream, "POISONED_MANAGED_ANCESTOR");
    traj::SessionManager manager(fixture.options);
    traj::ResumeRequest request; request.source_session_id = local_id;
    const auto refused = manager.ResumeAsNew(request);
    REQUIRE_FALSE(refused.error_code.empty());
    REQUIRE(refused.message == "managed.ownership.local_trusted_rejected");
    REQUIRE(Read(local_dir->v3_stream_path()) == local_bytes);
    REQUIRE(Read(ancestor_stream) == "POISONED_MANAGED_ANCESTOR");
    REQUIRE_FALSE(fs::exists(local_dir->session_dir() / "session.lock"));
    Marker("ancestor");
}

TEST_CASE("managed reservation: pending local delete cannot erase an unknown ownership publication") {
    Fixture fixture;
    auto pending = fixture.Reserve();
    const auto id = fixture.Owner().session_id;
    const auto directory = pending->session_dir();
    platform::SetDirectoryFlushFailureForTest(true);
    const auto receipt = pending->PublishOwnership();
    platform::SetDirectoryFlushFailureForTest(false);
    REQUIRE(receipt.knowledge == Knowledge::Unconfirmed);
    pending.reset();
    traj::LifecycleIntent intent;
    intent.operation_id = "delete-unknown-owner";
    intent.operation = traj::LifecycleOperationName(traj::LifecycleOperation::DeleteSession);
    intent.workspace_key = fixture.options.identity.workspace_key;
    intent.session_id = id;
    intent.requested_at_ms = 1759000000000LL;
    const traj::WorkspaceLifecycle lifecycle(fixture.workspace.workspace_dir());
    const auto intent_dir = lifecycle.WriteIntent(intent);
    REQUIRE(intent_dir.has_value());
    traj::SessionTombstone tombstone;
    tombstone.session_id = id;
    tombstone.operation_id = intent.operation_id;
    tombstone.reason = "old local delete intent";
    tombstone.deleted_at_ms = intent.requested_at_ms;
    REQUIRE(traj::WriteSessionTombstone(fixture.workspace.workspace_dir() / "tombstones", tombstone).has_value());
    REQUIRE(traj::RecoverPendingDeletes(fixture.workspace.workspace_dir(), 1759000000001LL).empty());
    REQUIRE_FALSE(traj::WorkspaceLifecycle::ReadResult(*intent_dir).has_value());
    REQUIRE(Read(directory / traj::kManagedSessionOwnershipFile) == receipt.publication_bytes);
    NoBody(directory);
    CheckManagedOperationProvenance();
    Marker("delete-residue");
}
