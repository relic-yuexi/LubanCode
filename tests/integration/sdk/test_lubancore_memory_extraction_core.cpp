#include "memory_extraction_core_checks.hpp"
#include "sdk/sampling_test_hooks.hpp"
namespace {
auto SharedPort() { return lubancore::detail::testing::GetMemoryExtractionTestPort(); }
}
TEST_CASE("SDK Memory extraction: strict parser inside shared module") { memory_extraction_fixture::Parser(SharedPort(),"shared-sdk"); }
TEST_CASE("SDK Memory extraction: bounded transcript inside shared module") { memory_extraction_fixture::Transcript(SharedPort(),"shared-sdk"); }
TEST_CASE("SDK Memory extraction: explicit prompt inside shared module") { memory_extraction_fixture::Prompt(SharedPort(),"shared-sdk"); }
TEST_CASE("SDK Memory extraction: actual request inside shared module") { memory_extraction_fixture::Request(SharedPort(),"shared-sdk"); }
TEST_CASE("SDK Memory extraction: usage provenance inside shared module") { memory_extraction_fixture::Usage(SharedPort(),"shared-sdk"); }
TEST_CASE("SDK Memory extraction: finish and cancellation inside shared module") { memory_extraction_fixture::Finish(SharedPort(),"shared-sdk"); }
TEST_CASE("SDK Memory extraction: native Connection inside shared module") { memory_extraction_fixture::NativeConnection(SharedPort(),"shared-sdk"); }
