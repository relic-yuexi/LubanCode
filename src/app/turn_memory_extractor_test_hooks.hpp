// Private native-test hook at the actual worker creation boundary.
// Definitions exist only in BUILD_TESTING; never installed or exported by SDK.
#pragma once
#include <functional>

namespace lubancode::app::testing {
std::function<void()> ExchangeMemoryWorkerStartHook(std::function<void()> hook);
}  // namespace lubancode::app::testing
