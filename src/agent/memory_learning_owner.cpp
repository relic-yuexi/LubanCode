#include "agent/memory_learning_owner.hpp"

#include <algorithm>
#include <utility>
#include <variant>

namespace lubancode::agent::memory_learning_owner {

TurnSliceFacts ExtractTurnSliceFacts(const std::vector<api::Message>& slice) {
    TurnSliceFacts facts;
    for (const auto& message : slice) {
        if (message.role == api::Role::Assistant) {
            facts.has_assistant_message = true;
        }
        for (const auto& block : message.content) {
            if (message.role == api::Role::Assistant) {
                if (const auto* text = std::get_if<api::TextBlock>(&block)) {
                    facts.assistant_text += text->text + "\n";
                }
            }
            if (const auto* use = std::get_if<api::ToolUseBlock>(&block)) {
                facts.tool_names.push_back(use->name);
                facts.has_tool_evidence = true;
            } else if (std::get_if<api::ToolResultBlock>(&block) != nullptr) {
                facts.has_tool_evidence = true;
            }
        }
    }
    return facts;
}

TurnLearningDispatchResult EvaluateTurnLearningDispatch(const TurnLearningDispatchInput& input) {
    TurnLearningDispatchResult out;
    if (!input.enabled) {
        out.skip_reason = memory_learning_gates::ExtractionSkipReason::Disabled;
        return out;
    }
    if (!input.has_new_history) {
        out.skip_reason = memory_learning_gates::ExtractionSkipReason::NoNewHistory;
        return out;
    }

    out.history_grew = true;
    out.has_tool_evidence = input.has_tool_evidence;

    if (input.turn_mutated) {
        out.skip_reason = memory_learning_gates::ExtractionSkipReason::AlreadyMutated;
        return out;
    }

    const auto text_stats = memory_learning_gates::ComputeMeaningfulTextStats(input.user_text);
    out.durable_signals = memory_learning_gates::EvaluateTurnDurableSignals(
        input.user_text, input.assistant_text, input.has_tool_evidence);
    out.durable_signals_evaluated = true;

    if (const auto blocked =
            memory_learning_gates::EvaluateMustSkipTextGate(text_stats, input.has_tool_evidence);
        blocked.has_value()) {
        // A short explicit preference or a tool-backed conclusion still matters.
        if (*blocked != memory_learning_gates::ExtractionSkipReason::ShortText ||
            out.durable_signals.empty()) {
            out.skip_reason = *blocked;
            return out;
        }
    }
    if (out.durable_signals.empty()) {
        out.skip_reason = memory_learning_gates::ExtractionSkipReason::NoDurableSignal;
        return out;
    }

    static const std::vector<api::Message> kEmptyMessages;
    const auto& messages = input.turn_messages != nullptr ? *input.turn_messages : kEmptyMessages;
    out.transcript = memory_extraction::BuildTurnTranscript(messages, input.max_transcript_bytes);
    if (out.transcript.empty()) {
        out.skip_reason = memory_learning_gates::ExtractionSkipReason::EmptyTranscript;
        return out;
    }

    static const std::vector<std::string> kEmptyToolNames;
    const auto& tool_names = input.tool_names != nullptr ? *input.tool_names : kEmptyToolNames;
    out.task_type = memory_extraction::ClassifyTaskType(input.user_text, tool_names);
    out.system_prompt = memory_extraction::BuildExtractionSystemPrompt(input.prompts_dir, out.task_type);
    if (out.system_prompt.empty()) {
        out.skip_reason = memory_learning_gates::ExtractionSkipReason::PromptMissing;
        return out;
    }

    out.ready = true;
    return out;
}

std::optional<CandidateClassification> ClassifyLearnedCandidate(
    memory::LearnMode mode,
    const memory_extraction::ProposedCandidate& proposed) {
    const auto kind = memory::ParseMemoryKind(proposed.kind);
    if (!kind.has_value()) {
        return std::nullopt;
    }

    bool auto_writable = false;
    if (mode == memory::LearnMode::Auto) {
        switch (*kind) {
        case memory::MemoryKind::Fact:
            auto_writable = proposed.confidence == "verified" && !proposed.paths.empty();
            break;
        case memory::MemoryKind::Feedback:
            auto_writable = proposed.confidence == "user-stated";
            break;
        case memory::MemoryKind::Preference:
            auto_writable = proposed.confidence == "verified" || proposed.confidence == "user-stated";
            break;
        }
    }
    return CandidateClassification{*kind, auto_writable};
}

memory::MemoryCandidate BuildReviewCandidate(
    memory::MemoryKind kind,
    const memory_extraction::ProposedCandidate& proposed,
    const std::string& task_type) {
    memory::MemoryCandidate candidate;
    candidate.kind = kind;
    candidate.title = proposed.title;
    candidate.summary = proposed.summary;
    candidate.content = proposed.content;
    candidate.keywords = proposed.keywords;
    candidate.paths = proposed.paths;
    candidate.confidence = proposed.confidence;
    candidate.occurred_at = proposed.occurred_at;
    candidate.task_type = task_type;
    return candidate;
}

memory::SaveRequest BuildAutoWriteSaveRequest(
    memory::MemoryKind kind,
    const memory_extraction::ProposedCandidate& proposed,
    const std::string& source_session) {
    memory::SaveRequest request;
    request.kind = kind;
    request.title = proposed.title;
    request.summary = proposed.summary;
    request.content = proposed.content;
    request.keywords = proposed.keywords;
    request.paths = proposed.paths;
    request.confidence = proposed.confidence;
    request.source_session = source_session;
    request.occurred_at = proposed.occurred_at;
    request.scope.level = "project";
    request.scope.kind = "project";
    request.evidence.reserve(proposed.paths.size());
    for (const auto& path : proposed.paths) {
        request.evidence.push_back(memory::MemoryEvidence{path, {}});
    }
    return request;
}

SettlementSummary SettleLearnedCandidates(
    const memory_extraction::MemoryExtraction& extraction,
    const SettlementOptions& options,
    LearningSettlementSink& sink) {
    SettlementSummary summary;

    std::vector<std::string> hints = extraction.retrieval_terms;
    if (options.include_candidate_keywords_in_hints) {
        std::size_t extra = 0;
        for (const auto& candidate : extraction.candidates) {
            extra += candidate.keywords.size();
        }
        hints.reserve(hints.size() + extra);
        for (const auto& candidate : extraction.candidates) {
            hints.insert(hints.end(), candidate.keywords.begin(), candidate.keywords.end());
        }
    }
    if (!hints.empty()) {
        sink.ApplyRetrievalHints(std::move(hints));
    }

    const std::size_t limit = std::min(extraction.candidates.size(), options.max_candidates);
    for (std::size_t i = 0; i < limit; ++i) {
        if (sink.IsCancelled()) {
            summary.cancelled = true;
            break;
        }
        const auto& proposed = extraction.candidates[i];
        const auto classification = ClassifyLearnedCandidate(options.mode, proposed);
        if (!classification.has_value()) {
            continue;
        }
        ++summary.processed_candidates;

        bool route_to_review = !classification->auto_writable;
        bool fallback_from_auto = false;
        if (classification->auto_writable) {
            ++summary.auto_attempted;
            const memory::SaveRequest request = BuildAutoWriteSaveRequest(
                classification->kind, proposed, options.source_session);
            const auto outcome = sink.TryAutoWrite(i, proposed, request);
            if (outcome == AutoWriteAttemptOutcome::Cancelled) {
                summary.cancelled = true;
                break;
            }
            if (outcome == AutoWriteAttemptOutcome::Aborted) {
                summary.aborted = true;
                break;
            }
            if (outcome == AutoWriteAttemptOutcome::Handled) {
                continue;
            }
            route_to_review = true;
            fallback_from_auto = true;
        }

        if (route_to_review) {
            memory::MemoryCandidate candidate = BuildReviewCandidate(
                classification->kind, proposed, options.task_type);
            const auto status = sink.AddReviewCandidate(
                i, proposed, std::move(candidate),
                classification->auto_writable, fallback_from_auto);
            if (status == SettlementActionStatus::Cancelled) {
                summary.cancelled = true;
                break;
            }
            if (status == SettlementActionStatus::Aborted) {
                summary.aborted = true;
                break;
            }
            ++summary.review_routed;
        }
    }

    return summary;
}

}  // namespace lubancode::agent::memory_learning_owner
