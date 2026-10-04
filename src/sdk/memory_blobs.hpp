#pragma once

#include <memory>

#include "lubancore/memory_blobs.hpp"
#include "trajectory/cas_store.hpp"

namespace lubancore::detail {

// Actual synchronous provider stack, including resource/capture destruction.
bool InMemoryBlobProvider() noexcept;
std::shared_ptr<lubancode::trajectory::MemoryCapabilityFactory> MakeMemoryBlobFactory(
    std::unique_ptr<memory_blobs::v1::Provider> provider);

} // namespace lubancore::detail
