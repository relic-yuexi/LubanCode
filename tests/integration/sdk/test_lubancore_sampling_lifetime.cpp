#include "sample_lifetime_checks.hpp"
#include "sdk/sampling_test_hooks.hpp"
namespace {
sample_lifetime_fixture::Port SharedPort() {
    return {lubancore::detail::testing::SampleInsideSharedSdk,
            lubancore::detail::testing::ReplaceSamplingWatchdogHooks,"shared-sdk"};
}
}

TEST_CASE("SDK sampling lifetime: Startup in actual shared module") {
    sample_lifetime_fixture::Startup(SharedPort());
}

TEST_CASE("SDK sampling lifetime: Exceptions in actual shared module") {
    sample_lifetime_fixture::Exceptions(SharedPort());
}

TEST_CASE("SDK sampling lifetime: Preflight in actual shared module") {
    sample_lifetime_fixture::Preflight(SharedPort());
}

TEST_CASE("SDK sampling lifetime: Gates in actual shared module") {
    sample_lifetime_fixture::Gates(SharedPort());
}

TEST_CASE("SDK sampling lifetime: Isolation in actual shared module") {
    sample_lifetime_fixture::Isolation(SharedPort());
}

TEST_CASE("SDK sampling lifetime: Cancellation in actual shared module") {
    sample_lifetime_fixture::Cancellation(SharedPort());
}
