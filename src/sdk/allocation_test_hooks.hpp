#pragma once
// Private qualification port; absent from ordinary SDKs and public headers.
#include <cstddef>
#include <atomic>
#include <cstdint>
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
// Owned by the caller until end() and actual worker retirement. No reply facts.
struct AllocationFailureControl {
    std::uint64_t refuse_ordinal = 0;
    std::atomic<std::uint64_t> allocations{0};
    std::atomic<std::uint64_t> refusals{0};
    std::atomic<unsigned> last_kind{0};
    bool require_returned_backend = false;
    std::atomic<bool> backend_returned{false};
};
struct AllocationFailurePort {
    bool (*install)() noexcept;
    bool (*restore)() noexcept;
    bool (*begin)(AllocationFailureControl&) noexcept;
    void (*end)() noexcept;
    bool (*prepare_returned_backend)(AllocationFailureControl&) noexcept;
};
LUBANCORE_API AllocationFailurePort GetAllocationFailurePort() noexcept;
// Called only by the real adapter/worker, never by the host to fabricate return.
void ActivateReturnedBackendAllocationScope() noexcept;
void RetireReturnedBackendAllocationScope() noexcept;
}
