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
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "platform/atomic_write.hpp"
#include "platform/paths.hpp"
#include "platform/process.hpp"
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
        REQUIRE_MESSAGE(created.has_value(), created ? "" : created.error());
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
        REQUIRE_MESSAGE(result.has_value(), result ? "" : result.error());
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
    REQUIRE_MESSAGE(launched.has_value(), launched ? "" : launched.error());
    REQUIRE((*launched)->v3_main.has_value());
    const auto id = (*launched)->session_id();
    const auto closed = manager.Close({}, nullptr);
    REQUIRE_MESSAGE(closed.error_code.empty(), closed.error_code + ": " + closed.message);
    return id;
}
} // namespace

TEST_CASE("managed reservation: owned durable publication precedes directories and real V3 writer") {
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
    REQUIRE_MESSAGE(finished.has_value(), finished ? "" : finished.error());
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
    REQUIRE_MESSAGE(writer.has_value(), writer ? "" : writer.error());
    REQUIRE(writer->Close().has_value());
    auto ledger = v3::ReadV3Ledger(finished->directory().v3_stream_path());
    REQUIRE_MESSAGE(ledger.has_value(), ledger ? "" : ledger.error());
    REQUIRE(ledger->session_id == fixture.Owner().session_id);
    REQUIRE(ledger->run_id == "main-0001");
    REQUIRE(ledger->lines == 2);
    REQUIRE(ledger->messages.at(0).message.at("content") == "managed original system");
    finished = std::unexpected(std::string("test releases finished owner"));
    REQUIRE_FALSE(fs::exists(directory / "session.lock"));
    REQUIRE(Read(directory / traj::kManagedSessionOwnershipFile) == published.publication_bytes);
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
    REQUIRE_MESSAGE(resumed.error_code.empty(), resumed.error_code + ": " + resumed.message);
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
    Marker("delete-residue");
}
