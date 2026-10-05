#include "trajectory/managed_session_reservation.hpp"

#include <exception>
#include <utility>

#include "platform/paths.hpp"
#include "trajectory/safety.hpp"
#include "workspace/index.hpp"

namespace lubancode::trajectory {
namespace {

bool ExplicitRoot(const std::filesystem::path& path) {
    if (!path.is_absolute()) return false;
    for (const auto& part : path) {
        if (part == "." || part == "..") return false;
    }
    return true;
}

std::expected<void, std::string> OnlyPublishedEntries(const std::filesystem::path& path) {
    std::error_code error;
    std::filesystem::directory_iterator it(path, error), end;
    if (error) return std::unexpected("managed.reservation.directory_unreadable");
    while (it != end) {
        const auto name = it->path().filename();
        if (name != "session.lock" && name != kManagedSessionOwnershipFile)
            return std::unexpected("managed.reservation.directory_changed");
        it.increment(error);
        if (error) return std::unexpected("managed.reservation.directory_unreadable");
    }
    return {};
}

} // namespace

ManagedSessionDirectory::ManagedSessionDirectory(TrajectoryDirectory directory, SessionLock lock)
    : directory_(std::move(directory)), lock_(std::move(lock)) {}

ManagedSessionReservation::ManagedSessionReservation(TrajectoryDirectory directory)
    : directory_(std::move(directory)) {}

ManagedSessionReservation::~ManagedSessionReservation() { Retire(); }

void ManagedSessionReservation::Retire() noexcept {
    lock_.Release();
    terminal_ = true;
    // No recursive cleanup and no cleanup after publication was attempted: even
    // an exception can leave visible committed bytes without a returned receipt.
    if (created_ && !publication_started_) {
        try {
            std::error_code error;
            const auto status = std::filesystem::symlink_status(session_dir(), error);
            if (!error && status.type() == std::filesystem::file_type::directory &&
                IsSafeContainedPath(session_dir(), directory_.workspace_dir())) {
                std::filesystem::remove(session_dir(), error); // only succeeds if still empty
            }
        } catch (...) {
            // Resource retirement must not throw. An unremoved reservation is
            // left for inspection; never broaden cleanup after an error.
        }
        created_ = false;
    }
}

std::expected<std::unique_ptr<ManagedSessionReservation>, std::string>
ManagedSessionReservation::Reserve(const std::filesystem::path& workspaces_root,
    ManagedSessionOwnership expected, const SessionLockOwner& owner) {
    if (!ExplicitRoot(workspaces_root) || !IsSafeSingleSegment(expected.workspace_key) ||
        !IsSafeSingleSegment(expected.session_id) || expected.session_id.size() < 8 ||
        expected.session_id.find('-') == std::string::npos)
        return std::unexpected("managed.reservation.invalid_path");
    const auto room = workspace::index::ResolveDirByWorkspaceKey(workspaces_root, expected.workspace_key);
    if (!room) return std::unexpected("managed.reservation.workspace_not_found");
    const auto path = *room / "sessions" / platform::Utf8ToPath(expected.session_id);
    if (!IsSafeContainedPath(path, workspaces_root))
        return std::unexpected("managed.reservation.unsafe_path");
    auto result = std::unique_ptr<ManagedSessionReservation>(
        new ManagedSessionReservation(TrajectoryDirectory::OpenExisting(path)));
    std::error_code error;
    // A single atomic create-new, not exists()+create_directories(). The trusted
    // workspace registration must have created the parent sessions directory.
    if (!std::filesystem::create_directory(path, error))
        return std::unexpected(error ? "managed.reservation.create_failed" : "managed.reservation.exists");
    result->created_ = true;
    auto lock = SessionLock::Acquire(path, owner);
    if (!lock) return std::unexpected(lock.error());
    result->lock_ = std::move(*lock);
    auto attempt = ManagedSessionOwnershipAttempt::Prepare(path, result->lock_, std::move(expected));
    if (!attempt) return std::unexpected(attempt.error());
    result->attempt_ = std::move(*attempt);
    return result;
}

ManagedSessionOwnershipPublication ManagedSessionReservation::PublishOwnership() {
    try {
        if (publication_) return *publication_;
        if (terminal_) {
            ManagedSessionOwnershipPublication rejected;
            rejected.error_code = "managed.reservation.retired";
            return rejected;
        }
        publication_started_ = true;
        publication_ = attempt_->Publish(lock_);
        if (publication_->knowledge != ManagedSessionOwnershipPublication::Knowledge::Committed)
            Retire();
        return *publication_;
    } catch (...) {
        Retire();
        throw;
    }
}

std::expected<ManagedSessionDirectory, std::string> ManagedSessionReservation::Finish() {
    if (terminal_) return std::unexpected("managed.reservation.retired");
    const auto fail = [this](std::string error) -> std::expected<ManagedSessionDirectory, std::string> {
        Retire();
        return std::unexpected(std::move(error));
    };
    try {
        if (!publication_ ||
            publication_->knowledge != ManagedSessionOwnershipPublication::Knowledge::Committed ||
            !publication_->native || !publication_->native->has_value() ||
            publication_->native->value().outcome != platform::WriteOutcome::CommittedDurable)
            return fail("managed.reservation.durable_publication_required");
        const auto captured = CaptureManagedSessionOwnershipLocked(session_dir(), lock_);
        if (!captured) return fail(captured.error());
        const auto matching = CheckManagedSessionOwnership(*captured, publication_->expected);
        if (!matching) return fail(matching.error());
        if (captured->bytes != publication_->publication_bytes)
            return fail("managed.ownership.source_changed");
        const auto entries = OnlyPublishedEntries(session_dir());
        if (!entries) return fail(entries.error());
        for (const auto* name : {"artifacts", "subagents"}) {
            std::error_code error;
            if (!std::filesystem::create_directory(session_dir() / name, error))
                return fail("managed.reservation.finish_failed");
        }
        // Construct all potentially throwing path values before transferring the
        // lock. Failure retains the ownership marker and retires this instance.
        auto directory = directory_;
        ManagedSessionDirectory finished(std::move(directory), std::move(lock_));
        terminal_ = true;
        return finished;
    } catch (...) {
        Retire();
        throw;
    }
}

} // namespace lubancode::trajectory
