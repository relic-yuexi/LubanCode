#pragma once
// Test-build only. The slot belongs to each actual linked engine instance.
#include <functional>
#include <memory>
namespace lubancode::agent::testing {
struct WatchdogHooks {
    std::function<void()> before_start;
    std::function<void()> on_thread_exit;
    std::function<void()> after_join;
};
using WatchdogHooksHandle = std::shared_ptr<const WatchdogHooks>;
WatchdogHooksHandle ReplaceWatchdogHooks(WatchdogHooksHandle replacement) noexcept;
} // namespace lubancode::agent::testing
