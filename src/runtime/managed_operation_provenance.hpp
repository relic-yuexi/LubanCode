#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <map>
#include <string>
#include <vector>

#include "trajectory/managed_session_ownership.hpp"

namespace lubancode::runtime {

// Trusted internal observations, not authentication or a cached permission.
// No SDK, Policy, Writer or Session object is retained by these values.
struct ManagedOperationSubject {
    std::string tenant_id, user_id, actor_kind, credential_id;
    bool operator==(const ManagedOperationSubject&) const = default;
};
struct ManagedOperationAdmission {
    trajectory::ManagedSessionOwnership owner;
    ManagedOperationSubject subject;
    std::uint64_t policy_revision = 0;
    std::vector<std::string> allowed_capabilities;
    bool operator==(const ManagedOperationAdmission&) const = default;
};
struct ManagedOperationProvenance {
    ManagedOperationAdmission admission;
    std::string operation_id, input_id, execution_id, run_id;
    std::string input_hash, intent_hash, provenance_hash;
    bool operator==(const ManagedOperationProvenance&) const = default;
};
struct ManagedStoredOperation {
    enum class State { Accepted, RejectedBeforeDispatch };
    ManagedOperationProvenance provenance;
    std::string client_operation_id, text, input_ref, input_sha256;
    std::size_t input_bytes = 0;
    State state = State::Accepted;
    std::string terminal_status, reason_code;
    std::uint64_t terminal_policy_revision = 0;
    std::int64_t received_at_ms = 0, rejected_at_ms = 0;
};
struct PreparedManagedOperation {
    ManagedStoredOperation operation;
    std::string input_bytes, accepted_line;
};

inline constexpr std::size_t kManagedOperationInputBytes = 8 * 1024 * 1024;
inline constexpr std::size_t kManagedOperationInputsTotalBytes = 128 * 1024 * 1024;
inline constexpr std::size_t kManagedOperationInputEntries = 4096;
inline constexpr std::size_t kManagedOperationLedgerBytes = 16 * 1024 * 1024;
inline constexpr std::size_t kManagedOperationLedgerLines = 65536;
inline constexpr std::size_t kManagedOperationLedgerLineBytes = 256 * 1024;
inline constexpr std::size_t kManagedOperationViewBytes = 384 * 1024 * 1024;

struct ManagedOperationMaterials {
    trajectory::ManagedSessionOwnership owner;
    std::string run_id;
    std::string operations;
    // Only accepted inputRef roster, keyed by the actual operation ID.
    std::map<std::string, std::string> inputs;
};

std::expected<std::string, std::string> ManagedOperationIntentHash(
    const ManagedOperationAdmission&, const trajectory::ManagedSessionOwnership& expected,
    const std::string& text);
std::expected<PreparedManagedOperation, std::string> PrepareManagedOperation(
    const ManagedOperationAdmission&, const trajectory::ManagedSessionOwnership& expected,
    const std::string& run_id, const std::string& key, const std::string& text,
    const std::string& operation_id, const std::string& input_id, std::int64_t received_at_ms);
std::expected<std::string, std::string> PrepareManagedOperationRejection(
    const ManagedStoredOperation&, const std::string& terminal_status,
    const std::string& reason_code, std::uint64_t decision_revision, std::int64_t rejected_at_ms);

// Roster stage used by a real locked capture. Does not read input paths.
std::expected<std::vector<ManagedStoredOperation>, std::string> ReadManagedOperationLedgerOwned(
    const std::string& bytes, const trajectory::ManagedSessionOwnership& expected,
    const std::string& run_id);
// Full strict consumer. All input bytes are owned; no path fallback or actor default.
std::expected<std::vector<ManagedStoredOperation>, std::string> ReadManagedOperationsOwned(
    const ManagedOperationMaterials&);

} // namespace lubancode::runtime
