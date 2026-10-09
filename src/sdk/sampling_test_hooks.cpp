#include "sdk/sampling_test_hooks.hpp"
#include <utility>
namespace lubancore::detail::testing {
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
