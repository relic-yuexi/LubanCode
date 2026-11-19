// Shared host-neutral owner policies for automatic turn memory learning:
// pre-dispatch gating/freeze, candidate auto-write classification, SaveRequest
// construction, and candidate settlement coordination across CLI and SDK.

#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <vector>

#include "agent/memory_extraction.hpp"
#include "agent/memory_learning_gates.hpp"
#include "api/types.hpp"
#include "memory/types.hpp"

namespace lubancode::agent::memory_learning_owner {

struct TurnSliceFacts {
    std::string assistant_text;
    std::vector<std::string> tool_names;
    bool has_tool_evidence = false;
    bool has_assistant_message = false;
};

// Extracts assistant prose, invoked tool names, and tool evidence flags from
// a single turn's message slice.
TurnSliceFacts ExtractTurnSliceFacts(const std::vector<api::Message>& slice);

struct TurnLearningDispatchInput {
    bool enabled = true;
    bool has_new_history = false;
    bool turn_mutated = false;
    bool has_tool_evidence = false;
    std::string user_text;
    std::string assistant_text;
    const std::vector<api::Message>* turn_messages = nullptr;
    const std::vector<std::string>* tool_names = nullptr;
    std::string prompts_dir;
    std::size_t max_transcript_bytes = 8 * 1024;
};

struct TurnLearningDispatchResult {
    bool ready = false;
    bool history_grew = false;
    bool has_tool_evidence = false;
    bool durable_signals_evaluated = false;
    std::optional<memory_learning_gates::ExtractionSkipReason> skip_reason;
    std::vector<std::string> durable_signals;
    std::string task_type;
    std::string system_prompt;
    std::string transcript;
};

// Evaluates local pre-dispatch gates in canonical order and freezes
// {task_type, system_prompt, transcript} when all gates pass.
TurnLearningDispatchResult EvaluateTurnLearningDispatch(const TurnLearningDispatchInput& input);

struct CandidateClassification {
    memory::MemoryKind kind = memory::MemoryKind::Fact;
    bool auto_writable = false;
};

// Parses candidate kind and checks LearnMode::Auto durability rules:
// - Fact requires confidence == "verified" and non-empty paths
// - Feedback requires confidence == "user-stated"
// - Preference requires confidence == "verified" or "user-stated"
// Anything else (including Review mode) routes to candidate review.
std::optional<CandidateClassification> ClassifyLearnedCandidate(
    memory::LearnMode mode,
    const memory_extraction::ProposedCandidate& proposed);

memory::MemoryCandidate BuildReviewCandidate(
    memory::MemoryKind kind,
    const memory_extraction::ProposedCandidate& proposed,
    const std::string& task_type = {});

memory::SaveRequest BuildAutoWriteSaveRequest(
    memory::MemoryKind kind,
    const memory_extraction::ProposedCandidate& proposed,
    const std::string& source_session = {});

struct SettlementOptions {
    memory::LearnMode mode = memory::LearnMode::Review;
    std::string task_type;
    std::string source_session;
    std::size_t max_candidates = 3;
    bool include_candidate_keywords_in_hints = false;
};

enum class SettlementActionStatus {
    Completed,
    Cancelled,
    Aborted,
};

enum class AutoWriteAttemptOutcome {
    Handled,
    FallbackToReview,
    Cancelled,
    Aborted,
};

// Narrow host sink implemented by CLI and SDK settlement adapters.
class LearningSettlementSink {
public:
    virtual ~LearningSettlementSink() = default;

    virtual void ApplyRetrievalHints(std::vector<std::string> /*hints*/) {}

    virtual bool IsCancelled() const { return false; }

    virtual AutoWriteAttemptOutcome TryAutoWrite(
        std::size_t candidate_index,
        const memory_extraction::ProposedCandidate& proposed,
        const memory::SaveRequest& request) = 0;

    virtual SettlementActionStatus AddReviewCandidate(
        std::size_t candidate_index,
        const memory_extraction::ProposedCandidate& proposed,
        memory::MemoryCandidate candidate,
        bool auto_write_eligible,
        bool fallback_from_auto_write) = 0;
};

struct SettlementSummary {
    bool cancelled = false;
    bool aborted = false;
    std::size_t processed_candidates = 0;
    std::size_t auto_attempted = 0;
    std::size_t review_routed = 0;
};

// Runs retrieval hint updates and shared candidate auto-write vs review
// routing through the caller-provided host sink.
SettlementSummary SettleLearnedCandidates(
    const memory_extraction::MemoryExtraction& extraction,
    const SettlementOptions& options,
    LearningSettlementSink& sink);

}  // namespace lubancode::agent::memory_learning_owner
