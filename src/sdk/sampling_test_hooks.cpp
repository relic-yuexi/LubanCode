#include "sdk/sampling_test_hooks.hpp"
#include <utility>
namespace lubancore::detail::testing {
lubancode::agent::testing::MemoryExtractionTestPort GetMemoryExtractionTestPort() {
    namespace extraction = lubancode::agent::memory_extraction;
    return {extraction::ClassifyTaskType, extraction::BuildTurnTranscript,
            extraction::BuildExtractionSystemPrompt, extraction::ParseExtractionJson,
            extraction::MemoryExtractionOutputSchema, extraction::RunMemoryExtraction,
            extraction::FinishMemoryExtraction, lubancode::agent::SampleModel,
            lubancode::runtime::assembly::BuildBackend,
            lubancode::agent::memory_learning_gates::ComputeMeaningfulTextStats,
            lubancode::agent::memory_learning_gates::PassesMinimumTextGate,
            lubancode::agent::memory_learning_gates::EvaluateMustSkipTextGate,
            lubancode::agent::memory_learning_gates::EvaluateDurableSignals,
            lubancode::agent::memory_learning_gates::EvaluateTurnDurableSignals,
            lubancode::agent::memory_learning_gates::ExtractionSkipReasonName};
}
lubancode::agent::testing::WatchdogHooksHandle ReplaceSamplingWatchdogHooks(
    lubancode::agent::testing::WatchdogHooksHandle replacement) noexcept {
    return lubancode::agent::testing::ReplaceWatchdogHooks(std::move(replacement));
}
lubancode::agent::SampleResult SampleInsideSharedSdk(
    lubancode::api::Backend& backend, const lubancode::agent::SampleRequest& request,
    const lubancode::agent::SampleOptions& options) {
    return lubancode::agent::SampleModel(backend, request, options);
}
}
