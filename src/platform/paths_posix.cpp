// POSIX 实现:环境变量 + 主目录。路径即 UTF-8 直通,不需要 Windows 那套
// 宽窄转换。语义对齐 paths_win.cpp。
//
// 验证状态:WSL Ubuntu 26.04(g++ 15.2)真机编译、单测通过(config/主目录
// 相关用例全绿);macOS 未经真机验证,待 CI 亮灯。
#include "platform/paths.hpp"

#include <cstdint>
#include <cstdlib>
#include <system_error>
#include <vector>

#if defined(__APPLE__)
#include <mach-o/dyld.h>
#else
#include <unistd.h>
#endif

namespace lubancode::platform {

std::optional<std::string> GetEnvVar(const char* name) {
    const char* value = std::getenv(name);
    if (value == nullptr || value[0] == '\0') {
        return std::nullopt;
    }
    return std::string(value);
}

std::optional<std::string> HomeDir() {
    return GetEnvVar("HOME");
}

std::optional<std::filesystem::path> ExecutablePath() {
#if defined(__APPLE__)
    std::uint32_t size = 0;
    _NSGetExecutablePath(nullptr, &size);
    if (size == 0) {
        return std::nullopt;
    }
    std::vector<char> buffer(size);
    if (_NSGetExecutablePath(buffer.data(), &size) != 0) {
        return std::nullopt;
    }
    std::error_code ec;
    const std::filesystem::path resolved = std::filesystem::weakly_canonical(buffer.data(), ec);
    return ec ? std::optional<std::filesystem::path>(std::filesystem::path(buffer.data()))
              : std::optional<std::filesystem::path>(resolved);
#else
    std::vector<char> buffer(1024);
    for (;;) {
        const ssize_t length = ::readlink("/proc/self/exe", buffer.data(), buffer.size());
        if (length < 0) {
            return std::nullopt;
        }
        if (static_cast<std::size_t>(length) < buffer.size()) {
            return std::filesystem::path(std::string(buffer.data(), static_cast<std::size_t>(length)));
        }
        buffer.resize(buffer.size() * 2);
    }
#endif
}

std::optional<std::string> OfficialSkillsDir() {
    const auto executable = ExecutablePath();
    if (!executable.has_value()) {
        return std::nullopt;
    }
    const std::filesystem::path exe_dir = executable->parent_path();
    const std::filesystem::path candidates[] = {
        exe_dir / "skills",
        exe_dir.parent_path() / "share" / "lubancode" / "skills",
    };
    for (const auto& candidate : candidates) {
        std::error_code ec;
        if (std::filesystem::is_directory(candidate, ec) && !ec) {
            const std::u8string value = candidate.u8string();
            return std::string(reinterpret_cast<const char*>(value.data()), value.size());
        }
    }
    return std::nullopt;
}

std::optional<std::string> OfficialPackagesDir() {
    const auto executable = ExecutablePath();
    if (!executable.has_value()) {
        return std::nullopt;
    }
    const std::filesystem::path exe_dir = executable->parent_path();
    const std::filesystem::path candidates[] = {
        exe_dir / "packages",
        exe_dir.parent_path() / "share" / "lubancode" / "packages",
    };
    for (const auto& candidate : candidates) {
        std::error_code ec;
        if (std::filesystem::is_directory(candidate, ec) && !ec) {
            const std::u8string value = candidate.u8string();
            return std::string(reinterpret_cast<const char*>(value.data()), value.size());
        }
    }
    return std::nullopt;
}

std::expected<void, std::string> ReplaceFileAtomically(const std::filesystem::path& source,
                                                        const std::filesystem::path& destination) {
    std::error_code ec;
    std::filesystem::rename(source, destination, ec);
    if (!ec) {
        return {};
    }
    return std::unexpected("原子替换文件失败: " + ec.message());
}

}  // namespace lubancode::platform
