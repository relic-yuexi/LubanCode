#pragma once

// Private test-build seam. This header is never installed. The slot and its
// snapshot live in the SDK DLL, not in an inline copy in a test executable.
#include <functional>
#include <memory>

#include "lubancore/api.hpp"

namespace lubancore::detail::testing {
using OpeningStartHook = std::function<void()>;
using OpeningStartHookHandle = std::shared_ptr<const OpeningStartHook>;

// Called on the opening's caller thread. Retirement of the returned handle is
// the caller's responsibility; no Runtime or Session lock is taken here.
LUBANCORE_API OpeningStartHookHandle ReplaceOpeningStartHook(
    OpeningStartHookHandle replacement) noexcept;
} // namespace lubancore::detail::testing
