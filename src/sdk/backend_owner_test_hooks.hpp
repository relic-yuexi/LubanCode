#pragma once
// Private testing-only SDK seam. No slot or export in an ordinary installation.
#include <functional>
#include <memory>
#include "lubancore/core.hpp"
#include "api/backend.hpp"
namespace lubancore::detail::testing {
using BackendOwnerObserver = std::function<void(const std::shared_ptr<Backend>&)>;
using BackendOwnerObserverHandle = std::shared_ptr<const BackendOwnerObserver>;
LUBANCORE_API BackendOwnerObserverHandle ReplaceBackendOwnerObserver(
    BackendOwnerObserverHandle replacement) noexcept;
LUBANCORE_API bool ReplaceBackendOwnerAllocationFailure(bool replacement) noexcept;
void ObserveBackendOwner(const std::shared_ptr<Backend>& owner);
// Calls the real adapter in the SDK module, never a test-executable substitute.
LUBANCORE_API std::unique_ptr<lubancode::api::Backend> AdaptObservedBackend(
    std::shared_ptr<Backend> backend);
}
