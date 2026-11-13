#pragma once

#include <cstddef>
#include <cstdint>
#include <compare>
#include <string>

#include <lubancore/api.hpp>

namespace lubancore::jobs::v1 {

// Explicit Session-owned command budgets. Omitted SessionOptions disables Jobs.
// Time caps: 1..86400000 ms; output: 1..2097152 bytes; registrations: 1..64.
struct CommandOptions {
    std::uint64_t registration_timeout_ms = 0, command_timeout_ms = 0, max_output_bytes = 0;
    std::size_t max_running = 0, max_registered_jobs = 0;
    bool operator==(const CommandOptions&) const = default;
};

struct Identity {
    // A pre-binding historical/uncertain record has no operation_id. Empty
    // fields stay empty; a registered ID alone never proves an accepted Job.
    std::string session_id, run_id, job_id, operation_id, parent_operation_id, turn_id, action_id;
    std::uint64_t attempt = 0;
    bool operator==(const Identity&) const = default;
};

// Candidate approval provenance, before a Job ID exists. Session-wide approval
// of this request applies only to the command Job that adopts this declaration.
struct ApprovalScope {
    std::string session_id, run_id, parent_operation_id, turn_id, parent_action_id;
    std::string provider_call_id, assistant_message_id, cwd, effective_input_sha256;
    auto operator<=>(const ApprovalScope&) const = default;
};

struct JobView {
    Identity identity;
    std::string state, execution_state, recovery_knowledge, gap;
    bool cancel_requested = false, owner_available = false, command_not_invoked = false;
    bool terminal = false;
};

struct Preview {
    // Trusted local host value. A byte bound is not redaction or permission to
    // transmit. Worker transports must apply their actual result policy.
    Identity identity;
    std::string persisted_event_id, result_id, text, artifact_gap;
    bool capture_complete = false, preview_truncated = false;
};

} // namespace lubancore::jobs::v1
