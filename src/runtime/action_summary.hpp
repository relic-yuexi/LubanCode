#pragma once

#include <atomic>
#include <cstddef>
#include <string>
#include <vector>

#include "api/backend.hpp"
#include "trajectory/v3/writer.hpp"

namespace lubancode::runtime {

struct ActionSummaryProfile {
    std::string provider;
    std::string wire;
    std::string model;
    std::size_t window_tokens = 32768;
    std::size_t output_tokens = 1024;
    std::size_t margin_tokens = 512;
    std::size_t max_chunk_bytes = 32768;
    int max_calls = 8;                 // shared by the entire tool batch
    int max_depth = 2;                 // map + one reduce, never recursive
    const std::atomic<bool>* cancel = nullptr;
};

struct ActionSummarySource {
    std::string action_id;
    std::string parent_turn_id;
    std::string persisted_event_ref;
    std::vector<std::string> source_result_event_refs;
    std::uint64_t attempt = 1;
    std::vector<nlohmann::json> result_refs;
    std::string text;
    std::string execution_state;
    bool execution_started = true;
    bool capture_complete = true;
    std::string capture_reason;
    std::size_t budget_bytes = 32768;
};

struct ActionSummaryResult {
    bool accepted = false;
    bool persistence_failed = false;
    std::string text;
    std::string terminal_event_ref;
    std::string reason;
    int model_calls = 0;
};

// Source must already be immutable and have a committed persisted event. All
// internal prompts/responses and usage stay outside the main context chain.
// The terminal event is a candidate receipt, not adoption: caller must select it
// and append/admit the matching tool message before publishing runtime history.
ActionSummaryResult SummarizeActionResult(trajectory::v3::V3Writer& writer,
                                           api::Backend& backend,
                                           const ActionSummaryProfile& profile,
                                           const ActionSummarySource& source,
                                           int& calls_remaining);

}  // namespace lubancode::runtime
