// file_in_use.hpp 的 Windows 实现(语义见头注;exe_in_use 口径出
// scripts/updater.py L728-744:打不开的任何形态一律当占用,保守优先)。
#include "platform/file_in_use.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

namespace lubancode::platform {

bool IsFileLockedForWrite(const std::filesystem::path& path) {
    // 独占写打开(dwShareMode=0):运行中的 EXE 会吃 sharing violation。
    // GENERIC_WRITE/OPEN_EXISTING 的取值与 python ctypes 路逐字相同。
    constexpr DWORD kGenericWrite = 0x40000000;
    constexpr DWORD kOpenExisting = 3;
    const HANDLE handle =
        CreateFileW(path.c_str(), kGenericWrite, /*dwShareMode=*/0, nullptr, kOpenExisting,
                    /*dwFlagsAndAttributes=*/0, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        return true;  // 探不到/被占/权限/不在:一律当占用(保守)
    }
    CloseHandle(handle);
    return false;
}

bool IsReparsePoint(const std::filesystem::path& path) {
    const DWORD attrs = GetFileAttributesW(path.c_str());
    if (attrs == INVALID_FILE_ATTRIBUTES) {
        return false;  // 查不到属性(路径不在):谈不上挡路
    }
    return (attrs & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
}

bool ProcessHoldsExe(unsigned long pid, const std::filesystem::path& exe_path) {
    (void)pid;
    (void)exe_path;
    return false;  // Windows 的占用判定走 IsFileLockedForWrite,恒 false
}

}  // namespace lubancode::platform
