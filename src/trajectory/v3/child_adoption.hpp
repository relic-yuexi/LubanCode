#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "trajectory/v3/reader.hpp"

namespace lubancode::trajectory::v3 {

enum class ChildAdoptionState { NotApplicable, Incomplete, Rejected, Validated };

// Owned historical material. Producer claims are deliberately separate from
// the independently checked child terminal and the parent's actual adoption.
struct ChildHistoricalAdoption {
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
    std::vector<std::string> effective_persisted_event_ids;
    std::vector<std::string> selected_source_event_ids; // Raw and effective local sources, in actual selected order.
    std::string selected_event_id, original_tool_message_id, admission_event_id;
    std::string consumed_tool_message_id, prepared_event_id;
    std::uint64_t prepared_context_revision = 0;
};

struct ChildAdoptionCheck {
    ChildAdoptionState state = ChildAdoptionState::NotApplicable;
    std::string issue;
    // Populated only for a complete Validated chain, never for a partial check.
    std::optional<ChildHistoricalAdoption> adoption;
};

// Private, read-only seam. The caller supplies a common-reader verified parent
// ledger belonging to parent_dir. It is borrowed only during this call. Child
// bytes/artifacts are read once under fixed bounds; no writer, model, tool or
// restoration callback is invoked. This does not replace SDK result-source
// policy, authenticate producer claims or reconstruct append-after-Close facts.
ChildAdoptionCheck ValidateChildAdoption(
    const V3Ledger& verified_parent, const std::filesystem::path& parent_dir,
    std::string_view turn_id, std::string_view action_id, std::uint64_t attempt,
    const std::function<void(const V3Ledger&)>& checked_child = {},
    const std::function<void(const V3Ledger&)>& checked_terminal_source = {});
// checked_child borrows the original bounded read only after terminal, parent
// source, result selection and input adoption checks. Pending prepared
// consumption retains its Incomplete verdict; the callback cannot upgrade it.
// checked_terminal_source is a distinct accounting-source borrow after child
// identity, parent spawn/source and the real closed terminal are verified. It
// does not prove raw capture, selection, adoption or parent consumption. Both
// callbacks share the original bounded read and cannot change the verdict.

} // namespace lubancode::trajectory::v3
