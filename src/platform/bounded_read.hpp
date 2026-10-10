#pragma once

#include <algorithm>
#include <array>
#include <cerrno>
#include <expected>
#include <filesystem>
#include <string>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace lubancode::platform {

// The caller owns path containment. Check the opened object, rather than a
// preceding status result, so a replacement FIFO/device cannot block a reader.
// This is a byte bound and regular-file check, not a filesystem sandbox.
inline std::expected<std::string, std::string> ReadBoundedRegularFile(
    const std::filesystem::path& path, std::size_t cap) {
    const auto fail = [](const char* code) { return std::unexpected(std::string(code)); };
#ifdef _WIN32
    const HANDLE file = CreateFileW(path.c_str(), GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
        FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (file == INVALID_HANDLE_VALUE) return fail("read.failed");
    struct Close { HANDLE file; ~Close() { CloseHandle(file); } } close{file};
    BY_HANDLE_FILE_INFORMATION info{};
    if (GetFileType(file) != FILE_TYPE_DISK || !GetFileInformationByHandle(file, &info) ||
        (info.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)))
        return fail("read.nonregular");
#else
    const int file = ::open(path.c_str(), O_RDONLY | O_NONBLOCK | O_NOFOLLOW | O_CLOEXEC);
    if (file < 0) return fail("read.failed");
    struct Close { int file; ~Close() { ::close(file); } } close{file};
    struct stat info{};
    if (::fstat(file, &info) != 0 || !S_ISREG(info.st_mode)) return fail("read.nonregular");
#endif
    std::string bytes;
    std::array<char, 4096> buffer{};
    for (;;) {
        const auto request = (std::min)(buffer.size(), cap - bytes.size() + 1);
        std::size_t count = 0;
#ifdef _WIN32
        DWORD read = 0;
        if (!ReadFile(file, buffer.data(), static_cast<DWORD>(request), &read, nullptr)) return fail("read.failed");
        count = read;
#else
        ssize_t read;
        do { read = ::read(file, buffer.data(), request); } while (read < 0 && errno == EINTR);
        if (read < 0) return fail("read.failed");
        count = static_cast<std::size_t>(read);
#endif
        if (!count) return bytes;
        bytes.append(buffer.data(), count);
        if (bytes.size() > cap) return fail("read.limit_exceeded");
    }
}

} // namespace lubancode::platform
