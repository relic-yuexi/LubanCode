#pragma once
// Private test bridge, never installed or compiled into an ordinary SDK.
#include "lubancore/api.hpp"
#include "agent/sample_model.hpp"
#include "agent/sample_model_test_hooks.hpp"
namespace lubancore::detail::testing {
LUBANCORE_API lubancode::agent::testing::WatchdogHooksHandle ReplaceSamplingWatchdogHooks(
    lubancode::agent::testing::WatchdogHooksHandle replacement) noexcept;
LUBANCORE_API lubancode::agent::SampleResult SampleInsideSharedSdk(
    lubancode::api::Backend& backend, const lubancode::agent::SampleRequest& request,
    const lubancode::agent::SampleOptions& options);
}
