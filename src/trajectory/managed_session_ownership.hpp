#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>

#include "platform/atomic_write.hpp"
#include "trajectory/session_lock.hpp"

namespace lubancode::trajectory {

inline constexpr char kManagedSessionOwnershipFile[] = "managed-session-ownership.json";
inline constexpr std::size_t kManagedSessionOwnershipMaxBytes = 8192;

// Trusted host expectations, not authentication, a Policy decision or a lease.
struct ManagedSessionOwnership {
    std::string tenant_id, project_id, workspace_key, session_id;
    std::uint64_t binding_version = 0;
    bool operator==(const ManagedSessionOwnership&) const = default;
};

struct ManagedSessionOwnershipCapture {
    enum class State { Absent, Marked };
    State state = State::Absent;
    std::filesystem::path session_dir;
    std::string bytes;
    std::optional<ManagedSessionOwnership> ownership;
};

// Read-only eligibility values; never grants permission or keeps a lock alive.
// The host must keep ancestors stable during each call. Only the final directory
// and opened metadata object are checked; this is not an OS sandbox or a fence.
std::expected<ManagedSessionOwnershipCapture, std::string> CaptureManagedSessionOwnership(
    const std::filesystem::path& session_dir);
std::expected<ManagedSessionOwnershipCapture, std::string> CaptureManagedSessionOwnershipLocked(
    const std::filesystem::path& session_dir, const SessionLock& lock);
std::expected<void, std::string> CheckLocalTrustedSessionOwnership(
    const ManagedSessionOwnershipCapture& capture);
std::expected<void, std::string> CheckManagedSessionOwnership(
    const ManagedSessionOwnershipCapture& capture, const ManagedSessionOwnership& expected);
std::expected<void, std::string> CheckManagedSessionOwnershipCaptureUnchanged(
    const ManagedSessionOwnershipCapture& before, const ManagedSessionOwnershipCapture& locked);

struct ManagedSessionOwnershipPublication {
    enum class Knowledge { RejectedBeforeIO, ExistingMatching, NotCommitted, Committed, Unconfirmed };
    Knowledge knowledge = Knowledge::RejectedBeforeIO;
    ManagedSessionOwnership expected;
    std::string captured_bytes, publication_bytes;
    // Populated only when this attempt actually calls AtomicWriteFile. Existing
    // matching bytes never synthesize a durability request or native receipt.
    std::optional<platform::WriteDurability> requested;
    std::optional<std::expected<platform::AtomicWriteReceipt, platform::AtomicWriteError>> native;
    std::string error_code, message;
};

// One host-serialized attempt. Owns only values: no borrowed lock, Writer, Policy
// or callback survives Prepare/Publish. The host keeps the supplied real lock and
// directory alive and stable for each invocation. Destruction performs no IO.
// First publication is immutable. A later Publish returns that historical value
// without IO, even after lock release; it is never a live authorization/SDK-ready
// token. ExistingMatching is only current same-ownership evidence: a marker with
// no V3 (including unknown residue) needs a later real durable confirmation before
// an actual Manager may open V3. This helper does not implement that confirmation.
class ManagedSessionOwnershipAttempt final {
public:
    static std::expected<std::unique_ptr<ManagedSessionOwnershipAttempt>, std::string> Prepare(
        const std::filesystem::path& session_dir, const SessionLock& lock,
        ManagedSessionOwnership expected);
    ManagedSessionOwnershipPublication Publish(const SessionLock& lock);
    std::optional<ManagedSessionOwnershipPublication> FirstPublication() const { return first_; }
    ManagedSessionOwnershipAttempt(const ManagedSessionOwnershipAttempt&) = delete;
    ManagedSessionOwnershipAttempt& operator=(const ManagedSessionOwnershipAttempt&) = delete;

private:
    ManagedSessionOwnershipAttempt(ManagedSessionOwnershipCapture capture,
        ManagedSessionOwnership expected, std::string bytes);
    ManagedSessionOwnershipCapture capture_;
    ManagedSessionOwnership expected_;
    std::string bytes_;
    std::optional<ManagedSessionOwnershipPublication> first_;
};

} // namespace lubancode::trajectory
