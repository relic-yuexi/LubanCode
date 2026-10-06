#pragma once

#include <cstdint>
#include <expected>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>

#include "trajectory/directory.hpp"
#include "trajectory/managed_session_ownership.hpp"

namespace lubancode::trajectory {

// Neutral provenance supplied by a trusted host, never a Policy decision table.
// SDK hosts derive these frozen values from authorization::v1; trajectory does
// not depend on that facade. credential_id is an opaque ID, not a bearer secret.
struct ManagedSessionCreationAudit {
    std::string tenant_id, user_id, actor_kind, credential_id;
    std::uint64_t opening_policy_revision = 0;
};

// Own this value until the writer and its subordinate resources have closed.
// It is an internal opening prerequisite, never authentication or a Policy grant.
class ManagedSessionDirectory final {
public:
    ManagedSessionDirectory(ManagedSessionDirectory&&) noexcept = default;
    ManagedSessionDirectory& operator=(ManagedSessionDirectory&&) noexcept = default;
    ManagedSessionDirectory(const ManagedSessionDirectory&) = delete;
    ManagedSessionDirectory& operator=(const ManagedSessionDirectory&) = delete;
    const TrajectoryDirectory& directory() const { return directory_; }
    const SessionLock& lock() const { return lock_; }

private:
    friend class ManagedSessionReservation;
    friend class SessionManager;
    ManagedSessionDirectory(TrajectoryDirectory directory, SessionLock lock,
                            ManagedSessionOwnershipPublication publication);
    TrajectoryDirectory directory_;
    SessionLock lock_;
    // Only Finish can produce this bundle; Manager consumes it as a whole. A
    // returned durable receipt is retained, never recreated from current bytes.
    ManagedSessionOwnershipPublication publication_;
};

// One host-serialized new opening. Owns the actual lock and immutable first
// publication. The caller stabilizes ancestors, as for the ownership helper.
// There is deliberately no adoption/resume entry point and no receipt setter.
class ManagedSessionReservation final {
public:
    static std::expected<std::unique_ptr<ManagedSessionReservation>, std::string> Reserve(
        const std::filesystem::path& workspaces_root, ManagedSessionOwnership expected,
        const SessionLockOwner& owner);
    ~ManagedSessionReservation();
    ManagedSessionReservation(const ManagedSessionReservation&) = delete;
    ManagedSessionReservation& operator=(const ManagedSessionReservation&) = delete;

    const std::filesystem::path& session_dir() const { return directory_.session_dir(); }
    ManagedSessionOwnershipPublication PublishOwnership();
    std::expected<ManagedSessionDirectory, std::string> Finish();

private:
    explicit ManagedSessionReservation(TrajectoryDirectory directory);
    void Retire() noexcept;
    TrajectoryDirectory directory_;
    SessionLock lock_;
    std::unique_ptr<ManagedSessionOwnershipAttempt> attempt_;
    std::optional<ManagedSessionOwnershipPublication> publication_;
    bool created_ = false;
    bool publication_started_ = false;
    bool terminal_ = false;
};

} // namespace lubancode::trajectory
