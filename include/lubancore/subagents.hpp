#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace lubancore::subagents::v1 {

// Trusted host values, not a catalog or a child-supplied permission request.
struct Profile {
    std::string type; // exactly general-purpose or Explore
    std::vector<std::string> tools; // exact names already admitted to this Session
    std::string model; // empty inherits the frozen parent request model
    std::int64_t max_steps_per_turn = 0; // explicit positive value is required
    std::int64_t max_wall_seconds = 0; // explicit positive integer seconds
};

struct Options {
    bool enabled = true;
    // At most one of each supported type. disabled requires an empty list.
    std::vector<Profile> profiles;
};

struct FrozenProfile {
    Profile requested;
    std::vector<std::string> effective_tools;
    std::string effective_model;
};

struct Snapshot {
    bool enabled = false;
    std::string session_id, cwd, permission_floor, plan_sha256;
    std::vector<FrozenProfile> profiles;
    int max_depth = 1;
    std::string execution_mode = "foreground";
};

// A copied, Session-owned ticket scope. The parent operation is causal/root
// ownership only; it is not a child operation. IDs may collide across hosts.
struct ApprovalScope {
    std::string host_session_id, host_run_id, host_operation_id;
    std::string parent_session_id, parent_run_id, child_session_id, child_run_id;
    std::string cwd, permission_floor;
    std::string child_turn_id, child_declared_action_id, child_declared_message_id;
};

enum class AdoptionState { Incomplete, Rejected, Validated };

// Actual in-process Finish/Close evidence. Resume has historical producer claims
// only; it never invents a new live receipt from a journal terminal.
struct LiveTerminalReceipt {
    std::string child_session_id, child_run_id, terminal_kind;
    std::string execution_state, append_confirmation, seal_state;
    std::string terminal_event_id, terminal_hash;
    std::uint64_t terminal_seq = 0;
    bool broken_after_append = false, broken_after_close = false;
    std::string append_error_code, close_error_code;
};

// These are independently checked historical facts, with producer claims named
// separately. A durable child terminal does not reconstruct a live Close call.
struct HistoricalAdoption {
    std::string parent_session_id, parent_run_id, turn_id, action_id;
    std::uint64_t attempt = 0;
    std::string spawn_event_id, observation_event_id, raw_persisted_event_id;
    std::string child_session_id, child_run_id, child_terminal_event_id;
    std::uint64_t child_terminal_seq = 0;
    std::string child_terminal_hash, child_terminal_reason, child_close_quality;
    std::string producer_execution_claim, producer_append_claim, producer_seal_claim;
    std::string raw_text_sha256;
    std::uint64_t raw_text_bytes = 0;
    std::optional<std::string> raw_structured_sha256;
    std::optional<std::uint64_t> raw_structured_bytes;
    std::vector<std::string> effective_persisted_event_ids, selected_source_event_ids;
    std::string selected_event_id, original_tool_message_id, admission_event_id;
    std::string consumed_tool_message_id, prepared_event_id;
    std::uint64_t prepared_context_revision = 0;
};

struct Report {
    std::string operation_id, parent_turn_id, parent_action_id;
    std::uint64_t parent_attempt = 0;
    AdoptionState adoption_state = AdoptionState::Incomplete;
    std::string issue;
    // Present only for a fully checked chain. No live writer/Agent is retained.
    std::optional<HistoricalAdoption> adoption;
    std::optional<LiveTerminalReceipt> live_receipt;
};

} // namespace lubancore::subagents::v1
