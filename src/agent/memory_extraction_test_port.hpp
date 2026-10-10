#pragma once
// Private linked-owner test port. Never installed.
#include "agent/memory_extraction.hpp"
#include "agent/memory_learning_gates.hpp"
#include "runtime/assembly/backend.hpp"
namespace lubancode::agent::testing {
struct MemoryExtractionTestPort {
    decltype(&memory_extraction::ClassifyTaskType) classify;
    decltype(&memory_extraction::BuildTurnTranscript) transcript;
    decltype(&memory_extraction::BuildExtractionSystemPrompt) prompt;
    decltype(&memory_extraction::ParseExtractionJson) parse;
    decltype(&memory_extraction::MemoryExtractionOutputSchema) schema;
    decltype(&memory_extraction::RunMemoryExtraction) run;
    decltype(&memory_extraction::FinishMemoryExtraction) finish;
    decltype(&SampleModel) sample;
    decltype(&runtime::assembly::BuildBackend) connection;
    decltype(&memory_learning_gates::ComputeMeaningfulTextStats) text_stats;
    decltype(&memory_learning_gates::PassesMinimumTextGate) minimum_text;
    decltype(&memory_learning_gates::EvaluateMustSkipTextGate) text_gate;
    decltype(&memory_learning_gates::EvaluateDurableSignals) durable_signals;
    decltype(&memory_learning_gates::EvaluateTurnDurableSignals) turn_signals;
    decltype(&memory_learning_gates::ExtractionSkipReasonName) skip_name;
};
}  // namespace lubancode::agent::testing
