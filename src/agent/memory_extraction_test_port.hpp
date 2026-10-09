#pragma once
// Private linked-owner test port. Never installed.
#include "agent/memory_extraction.hpp"
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
};
}  // namespace lubancode::agent::testing
