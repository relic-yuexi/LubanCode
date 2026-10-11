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
enum class AllocationPhase : unsigned {
    None, BackendReturned, Numbers, Material, Completion,
    ResultWrite, FinalRecord, Projection, Notification, Close
};
// Owned by the caller until end() and actual worker retirement. No reply facts.
struct AllocationFailureControl {
    std::uint64_t refuse_ordinal = 0;
    std::atomic<std::uint64_t> allocations{0};
    std::atomic<std::uint64_t> refusals{0};
    std::atomic<unsigned> last_kind{0};
    bool require_returned_backend = false;
    std::atomic<bool> backend_returned{false};
    std::atomic<bool> worker_retired{false};
    std::atomic<unsigned> refused_phase{0};
};
struct AllocationFailurePort {
    bool (*install)() noexcept;
    bool (*restore)() noexcept;
    bool (*begin)(AllocationFailureControl&) noexcept;
    void (*end)() noexcept;
    bool (*prepare_returned_backend)(AllocationFailureControl&) noexcept;
};
LUBANCORE_API AllocationFailurePort GetAllocationFailurePort() noexcept;
// Read-only installation facts, queried with no active worker or borrow.
// Stages: 0 idle, 1 alias check, 2 collection, 3 import patching, 4 installed.
// Windows bodies: 5 ownership, 6 transaction, 7 attach/detach, 8 commit.
struct AllocationInstallationDiagnostic {
    unsigned stage = 0;
    unsigned forms_mask = 0;
    std::size_t slots = 0;
    unsigned body_targets = 0;
    unsigned long body_error = 0;
    bool aliases = false;
    bool collection_overflow = false;
    void* original_addresses[8]{};
};
LUBANCORE_API AllocationInstallationDiagnostic GetAllocationInstallationDiagnostic() noexcept;
// Called only by the real adapter/worker, never by the host to fabricate return.
void ActivateReturnedBackendAllocationScope() noexcept;
void RetireReturnedBackendAllocationScope() noexcept;
void SetAllocationPhase(AllocationPhase) noexcept;
}
