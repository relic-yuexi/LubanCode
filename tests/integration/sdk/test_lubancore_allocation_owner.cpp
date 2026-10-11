#include <doctest/doctest.h>
#include <array>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <new>
#include <stdexcept>
#include "sdk/allocation_test_hooks.hpp"
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <dlfcn.h>
#endif

namespace {
using Kind = lubancore::detail::testing::AllocationKind;
std::filesystem::path ImageOf(const void* address) {
#ifdef _WIN32
    HMODULE module = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                           GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCWSTR>(address), &module))
        throw std::runtime_error("allocation qualification module lookup failed");
    std::array<wchar_t, 32768> path{};
    const auto length = GetModuleFileNameW(module, path.data(), static_cast<DWORD>(path.size()));
    if (!length || length >= path.size()) throw std::runtime_error("allocation qualification module path failed");
    return std::filesystem::path(path.data());
#else
    Dl_info image{};
    if (!dladdr(address, &image) || !image.dli_fname)
        throw std::runtime_error("allocation qualification module lookup failed");
    return std::filesystem::path(image.dli_fname);
#endif
}
// This implementation belongs to the actual host executable, never SDK sources.
void* HostAllocate(std::size_t size, std::size_t alignment, Kind kind) {
    const auto align = static_cast<std::align_val_t>(alignment);
    switch (kind) {
        case Kind::Scalar: return ::operator new(size);
        case Kind::Array: return ::operator new[](size);
        case Kind::ScalarNoThrow: return ::operator new(size, std::nothrow);
        case Kind::ArrayNoThrow: return ::operator new[](size, std::nothrow);
        case Kind::AlignedScalar: return ::operator new(size, align);
        case Kind::AlignedArray: return ::operator new[](size, align);
        case Kind::AlignedScalarNoThrow: return ::operator new(size, align, std::nothrow);
        case Kind::AlignedArrayNoThrow: return ::operator new[](size, align, std::nothrow);
    }
    throw std::bad_alloc{};
}
void HostRelease(void* pointer, std::size_t size, std::size_t alignment, Kind kind, bool sized) noexcept {
    const unsigned bits = static_cast<unsigned>(kind);
    const bool array = (bits & 1U) != 0, nothrow = (bits & 2U) != 0, aligned = (bits & 4U) != 0;
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
struct OwnedAllocation {
    void* pointer;
    decltype(lubancore::detail::testing::AllocationQualificationPort::release) release;
    std::size_t size, alignment;
    Kind kind;
    bool sized;
    ~OwnedAllocation() { if (pointer) release(pointer, size, alignment, kind, sized); }
};
struct OwnedRefusalImports {
    lubancore::detail::testing::AllocationFailurePort port;
    bool installed;
    ~OwnedRefusalImports() { port.end(); if (installed) port.restore(); }
};
struct OwnedRefusalScope {
    lubancore::detail::testing::AllocationFailurePort port;
    bool active;
    ~OwnedRefusalScope() { if (active) port.end(); }
    void End() { if (active) { port.end(); active = false; } }
};
}

TEST_CASE("SDK allocation ownership: real shared image and matching host release") {
    const auto port = lubancore::detail::testing::GetAllocationQualificationPort();
    REQUIRE(port.allocate);
    REQUIRE(port.release);
    REQUIRE(port.allocate != HostAllocate);
    REQUIRE(port.release != HostRelease);
    const auto allocate_image = ImageOf(reinterpret_cast<const void*>(port.allocate));
    const auto release_image = ImageOf(reinterpret_cast<const void*>(port.release));
    const auto host_image = ImageOf(reinterpret_cast<const void*>(&HostAllocate));
    const std::filesystem::path expected(LUBANCORE_TEST_SHARED_MODULE);
    std::error_code ec;
    REQUIRE(std::filesystem::equivalent(allocate_image, expected, ec));
    REQUIRE_FALSE(ec);
    REQUIRE(std::filesystem::equivalent(release_image, expected, ec));
    REQUIRE_FALSE(ec);
    REQUIRE_FALSE(std::filesystem::equivalent(host_image, expected, ec));
    REQUIRE_FALSE(ec);
    unsigned paths = 0;
    for (unsigned form = 0; form != 8; ++form) {
        const auto kind = static_cast<Kind>(form);
        for (const bool sized : {false, true}) for (const std::size_t size : {0U, 1U, 257U}) {
            for (unsigned route = 0; route != 3; ++route) {
                CAPTURE(form);
                CAPTURE(sized);
                CAPTURE(size);
                CAPTURE(route);
                const std::size_t alignment = 4096;
                const auto allocate = route == 2 ? HostAllocate : port.allocate;
                const auto release = route == 0 ? port.release : route == 1 ? HostRelease : port.release;
                OwnedAllocation value{allocate(size, alignment, kind), release, size, alignment, kind, sized};
                REQUIRE(value.pointer);
                if (form & 4U) CHECK(reinterpret_cast<std::uintptr_t>(value.pointer) % alignment == 0);
                auto* bytes = static_cast<unsigned char*>(value.pointer);
                for (std::size_t i = 0; i != size; ++i) bytes[i] = 0xA5;
                for (std::size_t i = 0; i != size; ++i) CHECK(bytes[i] == 0xA5);
                // Release now, before counting or reporting the successful path.
                value.release(value.pointer, size, alignment, kind, sized);
                value.pointer = nullptr;
                ++paths;
                std::cout << "[sdk-allocation-owner-route] " << form << ' ' << sized << ' ' << size << ' ' << route << '\n';
            }
        }
    }
    CHECK(paths == 144);
    std::cout << "[sdk-allocation-owner] qualified_paths=" << paths
              << " image=" << allocate_image.string() << '\n';
    // Warm all eight actual SDK imports above before inspecting their resolved
    // addresses. The host executable's allocation imports are never patched.
    const auto failure = lubancore::detail::testing::GetAllocationFailurePort();
    REQUIRE(failure.install);
    REQUIRE(failure.restore);
    REQUIRE(failure.begin);
    REQUIRE(failure.end);
    REQUIRE(std::filesystem::equivalent(ImageOf(reinterpret_cast<const void*>(failure.install)), expected));
    REQUIRE(std::filesystem::equivalent(ImageOf(reinterpret_cast<const void*>(failure.restore)), expected));
    REQUIRE(std::filesystem::equivalent(ImageOf(reinterpret_cast<const void*>(failure.begin)), expected));
    REQUIRE(std::filesystem::equivalent(ImageOf(reinterpret_cast<const void*>(failure.end)), expected));
    OwnedRefusalImports imports{failure, failure.install()};
    REQUIRE(imports.installed);
    for (unsigned form = 0; form != 8; ++form) {
        CAPTURE(form);
        const auto kind = static_cast<Kind>(form);
        lubancore::detail::testing::AllocationFailureControl baseline;
        OwnedRefusalScope normal{failure, failure.begin(baseline)};
        REQUIRE(normal.active);
        // Nested scopes and import restoration during a live borrow are errors.
        CHECK_FALSE(failure.begin(baseline));
        CHECK_FALSE(failure.restore());
        OwnedAllocation host{HostAllocate(257, 4096, kind), HostRelease, 257, 4096, kind, false};
        REQUIRE(host.pointer);
        const auto host_count = baseline.allocations.load();
        OwnedAllocation sdk{port.allocate(257, 4096, kind), port.release, 257, 4096, kind, false};
        REQUIRE(sdk.pointer);
        normal.End();
        CHECK(host_count == 0);
        CHECK(baseline.allocations.load() == 1);
        CHECK(baseline.refusals.load() == 0);
        CHECK(baseline.last_kind.load() == form);
        // Release real storage with the unchanged corresponding operators.
        sdk.release(sdk.pointer, 257, 4096, kind, false); sdk.pointer = nullptr;
        host.release(host.pointer, 257, 4096, kind, false); host.pointer = nullptr;
        lubancore::detail::testing::AllocationFailureControl refused;
        refused.refuse_ordinal = 1;
        OwnedRefusalScope fault{failure, failure.begin(refused)};
        REQUIRE(fault.active);
        void* returned = nullptr;
        bool bad_alloc = false;
        try { returned = port.allocate(257, 4096, kind); }
        catch (const std::bad_alloc&) { bad_alloc = true; }
        fault.End();
        // If a broken controller allocated anyway, release before assertions.
        if (returned) port.release(returned, 257, 4096, kind, false);
        CHECK(returned == nullptr);
        CHECK(bad_alloc == ((form & 2U) == 0));
        CHECK(refused.allocations.load() == 1);
        CHECK(refused.refusals.load() == 1);
        CHECK(refused.last_kind.load() == form);
        std::cout << "[sdk-module-refusal] form=" << form
                  << " baseline=" << baseline.allocations.load()
                  << " refused=" << refused.refusals.load() << " host=" << host_count << '\n';
    }
    REQUIRE(failure.restore());
    imports.installed = false;
    // Real allocation still works after restoring all imports.
    OwnedAllocation restored{port.allocate(257, 4096, Kind::Scalar), port.release, 257, 4096, Kind::Scalar, false};
    REQUIRE(restored.pointer);
    std::cout << "[sdk-module-refusal] qualified_forms=8 imports_restored=1\n";
}
