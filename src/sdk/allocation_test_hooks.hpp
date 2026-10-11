#pragma once
// Private qualification port; absent from ordinary SDKs and public headers.
#include <cstddef>
#include "lubancore/core.hpp"

namespace lubancore::detail::testing {
enum class AllocationKind : unsigned {
    Scalar, Array, ScalarNoThrow, ArrayNoThrow,
    AlignedScalar, AlignedArray, AlignedScalarNoThrow, AlignedArrayNoThrow
};
struct AllocationQualificationPort {
    void* (*allocate)(std::size_t, std::size_t, AllocationKind);
    void (*release)(void*, std::size_t, std::size_t, AllocationKind, bool) noexcept;
};
LUBANCORE_API AllocationQualificationPort GetAllocationQualificationPort() noexcept;
}
