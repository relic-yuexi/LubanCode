#include "memory_extraction_core_checks.hpp"
#include "app/memory_extract.hpp"
#include <type_traits>
namespace {
auto EnginePort() {
    namespace core = lubancode::agent::memory_extraction;
    return memory_extraction_fixture::Port{core::ClassifyTaskType,core::BuildTurnTranscript,
        core::BuildExtractionSystemPrompt,core::ParseExtractionJson,core::MemoryExtractionOutputSchema,
        core::RunMemoryExtraction,core::FinishMemoryExtraction,lubancode::agent::SampleModel,
        lubancode::runtime::assembly::BuildBackend,
            lubancode::agent::memory_learning_gates::ComputeMeaningfulTextStats,
            lubancode::agent::memory_learning_gates::PassesMinimumTextGate,
            lubancode::agent::memory_learning_gates::EvaluateMustSkipTextGate,
            lubancode::agent::memory_learning_gates::EvaluateDurableSignals,
            lubancode::agent::memory_learning_gates::EvaluateTurnDurableSignals,
            lubancode::agent::memory_learning_gates::ExtractionSkipReasonName};
}
static_assert(std::is_same_v<lubancode::app::MemoryExtraction,lubancode::agent::memory_extraction::MemoryExtraction>);
static_assert(std::is_same_v<lubancode::app::ExtractionError,lubancode::agent::memory_extraction::ExtractionError>);
}
TEST_CASE("CLI Memory extraction: shared strict parser and host facade") {
    memory_extraction_fixture::Parser(EnginePort(),"cli-engine");
    const auto result=lubancode::app::ParseExtractionJson(memory_extraction_fixture::ValidBody());
    REQUIRE(result.has_value()); CHECK(result->summary=="shared summary");
    CHECK(&lubancode::app::MemoryExtractionOutputSchema()==&lubancode::agent::memory_extraction::MemoryExtractionOutputSchema());
}
TEST_CASE("CLI Memory extraction: shared bounded transcript") { memory_extraction_fixture::Transcript(EnginePort(),"cli-engine"); }
TEST_CASE("CLI Memory extraction: shared explicit prompt") { memory_extraction_fixture::Prompt(EnginePort(),"cli-engine"); }
TEST_CASE("CLI Memory extraction: shared actual request") { memory_extraction_fixture::Request(EnginePort(),"cli-engine"); }
TEST_CASE("CLI Memory extraction: shared usage provenance") { memory_extraction_fixture::Usage(EnginePort(),"cli-engine"); }
TEST_CASE("CLI Memory extraction: shared finish and cancellation") { memory_extraction_fixture::Finish(EnginePort(),"cli-engine"); }
TEST_CASE("CLI Memory extraction: actual native Connection") { memory_extraction_fixture::NativeConnection(EnginePort(),"cli-engine"); }
TEST_CASE("CLI Memory extraction: actual native Connection faults retain source facts") { memory_extraction_fixture::NativeConnectionFaults(EnginePort(),"cli-engine"); }

TEST_CASE("CLI Memory extraction: shared learning text gate") { memory_extraction_fixture::LearningTextGate(EnginePort(),"cli-engine"); }
TEST_CASE("CLI Memory extraction: shared learning evidence gate") { memory_extraction_fixture::LearningEvidenceGate(EnginePort(),"cli-engine"); }
TEST_CASE("CLI Memory extraction: shared learning stable reasons") { memory_extraction_fixture::LearningReasonNames(EnginePort(),"cli-engine"); }
