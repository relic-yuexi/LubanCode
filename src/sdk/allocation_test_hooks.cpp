#include "sdk/allocation_test_hooks.hpp"
#include <new>
#include <array>
#include <cstdint>
#include <initializer_list>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <dlfcn.h>
#ifdef __APPLE__
#include <mach/mach.h>
#include <mach/mach_vm.h>
#include <mach-o/loader.h>
#else
#include <link.h>
#include <sys/mman.h>
#include <unistd.h>
#endif
#endif

#ifndef LUBANCORE_PRIVATE_OPENING_TEST_HOOKS
#error "Allocation qualification must only compile in a testing SDK module"
#endif

namespace lubancore::detail::testing {
namespace {
using Scalar = void* (*)(std::size_t);
using NoThrow = void* (*)(std::size_t, const std::nothrow_t&) noexcept;
using Aligned = void* (*)(std::size_t, std::align_val_t);
using AlignedNoThrow = void* (*)(std::size_t, std::align_val_t, const std::nothrow_t&) noexcept;
// Force actual saved-address reads: a constant-folded direct operator call would
// go through the patched import again instead of delegating to the original.
Scalar volatile original_scalar = static_cast<Scalar>(&::operator new);
Scalar volatile original_array = static_cast<Scalar>(&::operator new[]);
NoThrow volatile original_nothrow = static_cast<NoThrow>(&::operator new);
NoThrow volatile original_array_nothrow = static_cast<NoThrow>(&::operator new[]);
Aligned volatile original_aligned = static_cast<Aligned>(&::operator new);
Aligned volatile original_aligned_array = static_cast<Aligned>(&::operator new[]);
AlignedNoThrow volatile original_aligned_nothrow = static_cast<AlignedNoThrow>(&::operator new);
AlignedNoThrow volatile original_aligned_array_nothrow = static_cast<AlignedNoThrow>(&::operator new[]);
thread_local AllocationFailureControl* active_control = nullptr;
thread_local AllocationFailureControl* prepared_control = nullptr;
thread_local AllocationPhase allocation_phase = AllocationPhase::None;
std::atomic<unsigned> active_borrows{0};
bool imports_installed = false;

bool Refuse(AllocationKind kind) noexcept {
    if (!active_control) return false;
    if (active_control->require_returned_backend && !active_control->backend_returned.load()) return false;
    active_control->last_kind.store(static_cast<unsigned>(kind));
    const auto ordinal = active_control->allocations.fetch_add(1) + 1;
    if (!active_control->refuse_ordinal || ordinal != active_control->refuse_ordinal) return false;
    active_control->refusals.fetch_add(1);
    active_control->refused_phase.store(static_cast<unsigned>(allocation_phase));
    return true;
}
void* RefusedScalar(std::size_t size) {
    if (Refuse(AllocationKind::Scalar)) throw std::bad_alloc{};
    return original_scalar(size);
}
void* RefusedArray(std::size_t size) {
    if (Refuse(AllocationKind::Array)) throw std::bad_alloc{};
    return original_array(size);
}
void* RefusedNoThrow(std::size_t size, const std::nothrow_t& tag) noexcept {
    return Refuse(AllocationKind::ScalarNoThrow) ? nullptr : original_nothrow(size, tag);
}
void* RefusedArrayNoThrow(std::size_t size, const std::nothrow_t& tag) noexcept {
    return Refuse(AllocationKind::ArrayNoThrow) ? nullptr : original_array_nothrow(size, tag);
}
void* RefusedAligned(std::size_t size, std::align_val_t align) {
    if (Refuse(AllocationKind::AlignedScalar)) throw std::bad_alloc{};
    return original_aligned(size, align);
}
void* RefusedAlignedArray(std::size_t size, std::align_val_t align) {
    if (Refuse(AllocationKind::AlignedArray)) throw std::bad_alloc{};
    return original_aligned_array(size, align);
}
void* RefusedAlignedNoThrow(std::size_t size, std::align_val_t align, const std::nothrow_t& tag) noexcept {
    return Refuse(AllocationKind::AlignedScalarNoThrow) ? nullptr : original_aligned_nothrow(size, align, tag);
}
void* RefusedAlignedArrayNoThrow(std::size_t size, std::align_val_t align, const std::nothrow_t& tag) noexcept {
    return Refuse(AllocationKind::AlignedArrayNoThrow) ? nullptr : original_aligned_array_nothrow(size, align, tag);
}
const std::array<void*, 8> originals{
    reinterpret_cast<void*>(original_scalar), reinterpret_cast<void*>(original_array),
    reinterpret_cast<void*>(original_nothrow), reinterpret_cast<void*>(original_array_nothrow),
    reinterpret_cast<void*>(original_aligned), reinterpret_cast<void*>(original_aligned_array),
    reinterpret_cast<void*>(original_aligned_nothrow), reinterpret_cast<void*>(original_aligned_array_nothrow)};
const std::array<void*, 8> replacements{
    reinterpret_cast<void*>(&RefusedScalar), reinterpret_cast<void*>(&RefusedArray),
    reinterpret_cast<void*>(&RefusedNoThrow), reinterpret_cast<void*>(&RefusedArrayNoThrow),
    reinterpret_cast<void*>(&RefusedAligned), reinterpret_cast<void*>(&RefusedAlignedArray),
    reinterpret_cast<void*>(&RefusedAlignedNoThrow), reinterpret_cast<void*>(&RefusedAlignedArrayNoThrow)};
struct ImportSlot { void** address = nullptr; void* original = nullptr; unsigned kind = 0; int protection = 0; };
std::array<ImportSlot, 256> import_slots{};
std::size_t slot_count = 0;
bool collection_failed = false;
std::array<bool, 8> forms_found{};

void Collect(void** address, int protection = 0) noexcept {
    for (std::size_t i = 0; i != slot_count; ++i) if (import_slots[i].address == address) return;
    for (unsigned kind = 0; kind != originals.size(); ++kind) {
        if (*address != originals[kind]) continue;
        if (slot_count == import_slots.size()) { collection_failed = true; return; }
        import_slots[slot_count++] = {address, *address, kind, protection};
        forms_found[kind] = true;
        return;
    }
}

// All import inspection/patching happens outside scopes and with no SDK worker.
bool CollectModuleImports() noexcept {
#ifdef _WIN32
    HMODULE module = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
            GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            reinterpret_cast<LPCWSTR>(&GetAllocationFailurePort), &module)) return false;
    auto* base = reinterpret_cast<unsigned char*>(module);
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return false;
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return false;
    const auto directory = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!directory.VirtualAddress || directory.Size < sizeof(IMAGE_IMPORT_DESCRIPTOR)) return false;
    const auto* descriptors = reinterpret_cast<const IMAGE_IMPORT_DESCRIPTOR*>(base + directory.VirtualAddress);
    for (std::size_t i = 0; i != directory.Size / sizeof(*descriptors) && descriptors[i].Name; ++i) {
        auto* slots = reinterpret_cast<IMAGE_THUNK_DATA*>(base + descriptors[i].FirstThunk);
        for (; slots->u1.Function; ++slots) Collect(reinterpret_cast<void**>(&slots->u1.Function));
    }
#elif defined(__APPLE__)
    Dl_info image{};
    if (!dladdr(reinterpret_cast<const void*>(&GetAllocationFailurePort), &image)) return false;
    const auto* header = static_cast<const mach_header_64*>(image.dli_fbase);
    if (!header || header->magic != MH_MAGIC_64) return false;
    const unsigned char* command = reinterpret_cast<const unsigned char*>(header + 1);
    const auto* end = command + header->sizeofcmds;
    std::intptr_t slide = 0;
    bool slide_found = false;
    for (std::uint32_t i = 0; i != header->ncmds; ++i) {
        if (command + sizeof(load_command) > end) return false;
        const auto* load = reinterpret_cast<const load_command*>(command);
        if (load->cmdsize < sizeof(load_command) || command + load->cmdsize > end) return false;
        if (load->cmd == LC_SEGMENT_64) {
            const auto* segment = reinterpret_cast<const segment_command_64*>(load);
            if (load->cmdsize < sizeof(*segment)) return false;
            if (segment->fileoff == 0 && segment->filesize) {
                slide = reinterpret_cast<std::intptr_t>(header) - static_cast<std::intptr_t>(segment->vmaddr);
                slide_found = true;
            }
        }
        command += load->cmdsize;
    }
    if (!slide_found) return false;
    command = reinterpret_cast<const unsigned char*>(header + 1);
    for (std::uint32_t i = 0; i != header->ncmds; ++i) {
        const auto* load = reinterpret_cast<const load_command*>(command);
        if (load->cmd == LC_SEGMENT_64) {
            const auto* segment = reinterpret_cast<const segment_command_64*>(load);
            if (segment->nsects > (load->cmdsize - sizeof(*segment)) / sizeof(section_64)) return false;
            const auto* sections = reinterpret_cast<const section_64*>(segment + 1);
            for (std::uint32_t j = 0; j != segment->nsects; ++j) {
                const auto type = sections[j].flags & SECTION_TYPE;
                if (type != S_LAZY_SYMBOL_POINTERS && type != S_NON_LAZY_SYMBOL_POINTERS) continue;
                auto** slots = reinterpret_cast<void**>(static_cast<std::intptr_t>(sections[j].addr) + slide);
                for (std::uint64_t n = 0; n != sections[j].size / sizeof(void*); ++n) Collect(slots + n);
            }
        }
        command += load->cmdsize;
    }
#else
    Dl_info image{};
    if (!dladdr(reinterpret_cast<const void*>(&GetAllocationFailurePort), &image)) return false;
    struct Context { void* base; bool found = false; } context{image.dli_fbase};
    dl_iterate_phdr([](dl_phdr_info* info, std::size_t, void* opaque) {
        auto& context = *static_cast<Context*>(opaque);
        if (reinterpret_cast<void*>(info->dlpi_addr) != context.base) return 0;
        context.found = true;
        const ElfW(Dyn)* dynamic = nullptr;
        for (ElfW(Half) i = 0; i != info->dlpi_phnum; ++i)
            if (info->dlpi_phdr[i].p_type == PT_DYNAMIC)
                dynamic = reinterpret_cast<const ElfW(Dyn)*>(info->dlpi_addr + info->dlpi_phdr[i].p_vaddr);
        if (!dynamic) { collection_failed = true; return 1; }
        const void* rela = nullptr; std::size_t rela_size = 0;
        const void* rel = nullptr; std::size_t rel_size = 0;
        const void* plt = nullptr; std::size_t plt_size = 0; std::intptr_t plt_kind = DT_RELA;
        const auto dynamic_pointer = [&](ElfW(Addr) value) -> const void* {
            // glibc versions/architectures can retain relative dynamic pointers
            // or expose already relocated ones. Only accept this SDK's mappings.
            for (const auto candidate : {value, info->dlpi_addr + value}) {
                for (ElfW(Half) i = 0; i != info->dlpi_phnum; ++i) {
                    const auto& ph = info->dlpi_phdr[i];
                    if (ph.p_type == PT_LOAD && candidate >= info->dlpi_addr + ph.p_vaddr &&
                            candidate < info->dlpi_addr + ph.p_vaddr + ph.p_memsz)
                        return reinterpret_cast<const void*>(candidate);
                }
            }
            collection_failed = true;
            return nullptr;
        };
        for (const auto* value = dynamic; value->d_tag != DT_NULL; ++value) {
            switch (value->d_tag) {
                case DT_RELA: rela = dynamic_pointer(value->d_un.d_ptr); break;
                case DT_RELASZ: rela_size = value->d_un.d_val; break;
                case DT_REL: rel = dynamic_pointer(value->d_un.d_ptr); break;
                case DT_RELSZ: rel_size = value->d_un.d_val; break;
                case DT_JMPREL: plt = dynamic_pointer(value->d_un.d_ptr); break;
                case DT_PLTRELSZ: plt_size = value->d_un.d_val; break;
                case DT_PLTREL: plt_kind = value->d_un.d_val; break;
            }
        }
        const auto collect_relocation = [&](ElfW(Addr) offset) {
            const auto address = info->dlpi_addr + offset;
            int protection = 0; bool mapped = false;
            for (ElfW(Half) i = 0; i != info->dlpi_phnum; ++i) {
                const auto& ph = info->dlpi_phdr[i];
                if (ph.p_type == PT_LOAD && address >= info->dlpi_addr + ph.p_vaddr &&
                        address + sizeof(void*) <= info->dlpi_addr + ph.p_vaddr + ph.p_memsz) {
                    mapped = true;
                    protection = ((ph.p_flags & PF_R) ? PROT_READ : 0) |
                                 ((ph.p_flags & PF_W) ? PROT_WRITE : 0) |
                                 ((ph.p_flags & PF_X) ? PROT_EXEC : 0);
                }
            }
            if (!mapped) { collection_failed = true; return; }
            const auto page_size = sysconf(_SC_PAGESIZE);
            if (page_size <= 0) { collection_failed = true; return; }
            const auto page = static_cast<std::uintptr_t>(page_size);
            for (ElfW(Half) i = 0; i != info->dlpi_phnum; ++i) {
                const auto& ph = info->dlpi_phdr[i];
                if (ph.p_type == PT_GNU_RELRO && address >= (info->dlpi_addr + ph.p_vaddr) / page * page &&
                        address < (info->dlpi_addr + ph.p_vaddr + ph.p_memsz) / page * page)
                    protection &= ~PROT_WRITE;
            }
            Collect(reinterpret_cast<void**>(address), protection);
        };
        const auto is_pointer_import = [](auto information) {
#if __SIZEOF_POINTER__ == 8
            const auto type = ELF64_R_TYPE(information);
#else
            const auto type = ELF32_R_TYPE(information);
#endif
#if defined(__aarch64__)
            return type == R_AARCH64_JUMP_SLOT || type == R_AARCH64_GLOB_DAT;
#elif defined(__arm__)
            return type == R_ARM_JUMP_SLOT || type == R_ARM_GLOB_DAT;
#elif defined(__i386__)
            return type == R_386_JMP_SLOT || type == R_386_GLOB_DAT;
#elif defined(__x86_64__)
            return type == R_X86_64_JUMP_SLOT || type == R_X86_64_GLOB_DAT;
#else
            return false;
#endif
        };
        if (rela) for (std::size_t n = 0; n != rela_size / sizeof(ElfW(Rela)); ++n) {
            const auto& item = static_cast<const ElfW(Rela)*>(rela)[n];
            if (is_pointer_import(item.r_info))
                collect_relocation(item.r_offset);
        }
        if (rel) for (std::size_t n = 0; n != rel_size / sizeof(ElfW(Rel)); ++n) {
            const auto& item = static_cast<const ElfW(Rel)*>(rel)[n];
            if (is_pointer_import(item.r_info))
                collect_relocation(item.r_offset);
        }
        if (plt && plt_kind == DT_RELA) for (std::size_t n = 0; n != plt_size / sizeof(ElfW(Rela)); ++n)
            collect_relocation(static_cast<const ElfW(Rela)*>(plt)[n].r_offset);
        if (plt && plt_kind == DT_REL) for (std::size_t n = 0; n != plt_size / sizeof(ElfW(Rel)); ++n)
            collect_relocation(static_cast<const ElfW(Rel)*>(plt)[n].r_offset);
        return 1;
    }, &context);
    if (!context.found) return false;
#endif
    if (collection_failed) return false;
    for (const bool found : forms_found) if (!found) return false;
    return true;
}

bool WriteSlot(const ImportSlot& slot, void* value) noexcept {
    void* previous = *slot.address;
#ifdef _WIN32
    DWORD protection = 0, ignored = 0;
    if (!VirtualProtect(slot.address, sizeof(void*), PAGE_READWRITE, &protection)) return false;
    *slot.address = value;
    if (VirtualProtect(slot.address, sizeof(void*), protection, &ignored)) return true;
    *slot.address = previous;
    VirtualProtect(slot.address, sizeof(void*), protection, &ignored);
    return false;
#elif defined(__APPLE__)
    mach_vm_address_t region = reinterpret_cast<mach_vm_address_t>(slot.address);
    mach_vm_size_t size = 0;
    vm_region_basic_info_data_64_t information{};
    mach_msg_type_number_t count = VM_REGION_BASIC_INFO_COUNT_64;
    mach_port_t object = MACH_PORT_NULL;
    if (mach_vm_region(mach_task_self(), &region, &size, VM_REGION_BASIC_INFO_64,
            reinterpret_cast<vm_region_info_t>(&information), &count, &object) != KERN_SUCCESS) return false;
    if (object != MACH_PORT_NULL) mach_port_deallocate(mach_task_self(), object);
    const auto address = reinterpret_cast<mach_vm_address_t>(slot.address);
    if (address < region || address + sizeof(void*) > region + size) return false;
    if (mach_vm_protect(mach_task_self(), address, sizeof(void*), false,
            information.protection | VM_PROT_WRITE | VM_PROT_COPY) != KERN_SUCCESS) return false;
    *slot.address = value;
    if (mach_vm_protect(mach_task_self(), address, sizeof(void*), false, information.protection) == KERN_SUCCESS) return true;
    *slot.address = previous;
    mach_vm_protect(mach_task_self(), address, sizeof(void*), false, information.protection);
    return false;
#else
    const auto page_size = sysconf(_SC_PAGESIZE);
    if (page_size <= 0) return false;
    const auto page = static_cast<std::uintptr_t>(page_size);
    void* start = reinterpret_cast<void*>(reinterpret_cast<std::uintptr_t>(slot.address) / page * page);
    if (mprotect(start, page, slot.protection | PROT_WRITE) != 0) return false;
    *slot.address = value;
    if (mprotect(start, page, slot.protection) == 0) return true;
    *slot.address = previous;
    mprotect(start, page, slot.protection);
    return false;
#endif
}
bool RestoreImports() noexcept {
    if (!imports_installed || active_borrows.load()) return false;
    bool restored = true;
    for (std::size_t i = 0; i != slot_count; ++i) if (!WriteSlot(import_slots[i], import_slots[i].original)) restored = false;
    if (restored) imports_installed = false;
    return restored;
}
bool InstallImports() noexcept {
    if (imports_installed || active_borrows.load()) return false;
    slot_count = 0; collection_failed = false; forms_found.fill(false);
    // Aliased operators need a symbol-aware resolver; never mislabel their kind.
    for (unsigned i = 0; i != originals.size(); ++i)
        for (unsigned j = i + 1; j != originals.size(); ++j) if (originals[i] == originals[j]) return false;
    if (!CollectModuleImports()) return false;
    std::size_t applied = 0;
    for (; applied != slot_count; ++applied) {
        if (!WriteSlot(import_slots[applied], replacements[import_slots[applied].kind])) {
            bool restored = true;
            for (std::size_t i = 0; i != applied; ++i) if (!WriteSlot(import_slots[i], import_slots[i].original)) restored = false;
            imports_installed = !restored;
            return false;
        }
    }
    imports_installed = true;
    return true;
}
bool BeginScope(AllocationFailureControl& control) noexcept {
    if (!imports_installed || active_control || prepared_control) return false;
    active_control = &control;
    active_borrows.fetch_add(1);
    return true;
}
void EndScope() noexcept {
    if (!active_control && !prepared_control) return;
    active_control = nullptr;
    prepared_control = nullptr;
    active_borrows.fetch_sub(1);
}
bool PrepareReturnedScope(AllocationFailureControl& control) noexcept {
    if (!imports_installed || active_control || prepared_control || !control.require_returned_backend) return false;
    prepared_control = &control;
    active_borrows.fetch_add(1);
    return true;
}
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
AllocationFailurePort GetAllocationFailurePort() noexcept {
    return {InstallImports, RestoreImports, BeginScope, EndScope, PrepareReturnedScope};
}
void ActivateReturnedBackendAllocationScope() noexcept {
    if (!prepared_control || active_control) return;
    active_control = prepared_control;
    prepared_control = nullptr;
    active_control->backend_returned.store(true);
    allocation_phase = AllocationPhase::BackendReturned;
}
void RetireReturnedBackendAllocationScope() noexcept {
    auto* control = active_control ? active_control : prepared_control;
    if (control && control->require_returned_backend) control->worker_retired.store(true);
    EndScope();
}
void SetAllocationPhase(AllocationPhase phase) noexcept { allocation_phase = phase; }
}
