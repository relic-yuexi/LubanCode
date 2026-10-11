#include "sdk/allocation_test_hooks.hpp"
#include <new>

#ifndef LUBANCORE_PRIVATE_OPENING_TEST_HOOKS
#error "Allocation qualification must only compile in a testing SDK module"
#endif

namespace lubancore::detail::testing {
namespace {
void* Allocate(std::size_t size, std::size_t alignment, AllocationKind kind) {
    const auto align = static_cast<std::align_val_t>(alignment);
    switch (kind) {
        case AllocationKind::Scalar: return ::operator new(size);
        case AllocationKind::Array: return ::operator new[](size);
        case AllocationKind::ScalarNoThrow: return ::operator new(size, std::nothrow);
        case AllocationKind::ArrayNoThrow: return ::operator new[](size, std::nothrow);
        case AllocationKind::AlignedScalar: return ::operator new(size, align);
        case AllocationKind::AlignedArray: return ::operator new[](size, align);
        case AllocationKind::AlignedScalarNoThrow: return ::operator new(size, align, std::nothrow);
        case AllocationKind::AlignedArrayNoThrow: return ::operator new[](size, align, std::nothrow);
    }
    throw std::bad_alloc{};
}
void Release(void* pointer, std::size_t size, std::size_t alignment,
             AllocationKind kind, bool sized) noexcept {
    const auto bits = static_cast<unsigned>(kind);
    const bool array = (bits & 1U) != 0;
    const bool nothrow = (bits & 2U) != 0;
    const bool aligned = (bits & 4U) != 0;
    const auto align = static_cast<std::align_val_t>(alignment);
    if (aligned) {
        if (sized) {
            if (array) ::operator delete[](pointer, size, align);
            else ::operator delete(pointer, size, align);
        } else if (nothrow) {
            if (array) ::operator delete[](pointer, align, std::nothrow);
            else ::operator delete(pointer, align, std::nothrow);
        } else {
            if (array) ::operator delete[](pointer, align);
            else ::operator delete(pointer, align);
        }
    } else if (sized) {
        if (array) ::operator delete[](pointer, size);
        else ::operator delete(pointer, size);
    } else if (nothrow) {
        if (array) ::operator delete[](pointer, std::nothrow);
        else ::operator delete(pointer, std::nothrow);
    } else {
        if (array) ::operator delete[](pointer);
        else ::operator delete(pointer);
    }
}
}
AllocationQualificationPort GetAllocationQualificationPort() noexcept {
    return {Allocate, Release};
}
}
