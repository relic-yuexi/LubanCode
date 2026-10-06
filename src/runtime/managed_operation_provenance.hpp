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
    enum class State { Accepted, RejectedBeforeDispatch, Dispatched, Final };
    ManagedOperationProvenance provenance;
    std::string client_operation_id, text, input_ref, input_sha256;
    std::size_t input_bytes = 0;
    State state = State::Accepted;
    std::string terminal_status, reason_code;
    std::uint64_t terminal_policy_revision = 0;
    std::int64_t received_at_ms = 0, rejected_at_ms = 0;
    std::string turn_id, binding_event_id, binding_hash;
    std::uint64_t dispatch_policy_revision = 0, binding_seq = 0;
    std::int64_t dispatched_at_ms = 0, finalized_at_ms = 0;
    std::string execution_status, result_ref, result_sha256;
    std::size_t result_bytes = 0;
    std::vector<std::string> final_message_refs;
    bool usage_reported = false, complete = false;
};
struct ManagedOperationResult {
    std::string execution_status, final_text, error;
    std::vector<std::string> final_message_refs;
    bool usage_reported = false, complete = false;
};
struct PreparedManagedFinal {
    ManagedStoredOperation operation;
    std::string result_bytes, final_line;
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
inline constexpr std::size_t kManagedOperationMainBytes = 128 * 1024 * 1024;
inline constexpr std::size_t kManagedOperationResultBytes = 8 * 1024 * 1024;
inline constexpr std::size_t kManagedOperationResultsTotalBytes = 128 * 1024 * 1024;

struct ManagedOperationMaterials {
    trajectory::ManagedSessionOwnership owner;
    std::string run_id;
    std::string operations;
    // A readable valid row never upgrades a first unknown publication/close.
    // This live owner's owned observation is not a persistent commit boundary,
    // cross-process completion receipt or durable authorization.
    bool completion_known = true;
    // Only accepted inputRef roster, keyed by the actual operation ID.
    std::map<std::string, std::string> inputs;
    // Only the fresh-text capture fills these actual originals. The storage-only
    // strict reader continues to reject schema4, independent of their presence.
    std::string main;
    std::map<std::string, std::string> results;
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
std::expected<std::string, std::string> PrepareManagedOperationDispatch(
    const ManagedStoredOperation&, const std::string& turn_id, std::uint64_t decision_revision,
    std::int64_t dispatched_at_ms);
std::expected<PreparedManagedFinal, std::string> PrepareManagedOperationFinal(
    const ManagedStoredOperation& bound, const ManagedOperationResult&, std::int64_t finalized_at_ms);

// Roster stage used by a real locked capture. Does not read input paths.
std::expected<std::vector<ManagedStoredOperation>, std::string> ReadManagedOperationLedgerOwned(
    const std::string& bytes, const trajectory::ManagedSessionOwnership& expected,
    const std::string& run_id);
// Full strict consumer. All input bytes are owned; no path fallback or actor default.
std::expected<std::vector<ManagedStoredOperation>, std::string> ReadManagedOperationsOwned(
    const ManagedOperationMaterials&);
// Explicit mixed schema3 acceptance/rejection + schema4 dispatch/final roster.
std::expected<std::vector<ManagedStoredOperation>, std::string> ReadManagedExecutionLedgerOwned(
    const std::string&, const trajectory::ManagedSessionOwnership&, const std::string& run_id);
// Strict actual main/input/result relationship, no path or live owner fallback.
std::expected<std::vector<ManagedStoredOperation>, std::string> ReadManagedExecutionOwned(
    const ManagedOperationMaterials&);

} // namespace lubancode::runtime
