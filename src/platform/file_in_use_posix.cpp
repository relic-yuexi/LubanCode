// file_in_use.hpp 的 POSIX 实现(语义见头注;version_in_use_posix 口径
// 出 scripts/updater.py L747-766:/proc/<pid>/exe 比对,认不出恒 false)。
#include "platform/file_in_use.hpp"

#include <climits>
#include <string>
#include <string_view>
#include <unistd.h>

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

namespace lubancode::platform {

bool IsFileLockedForWrite(const std::filesystem::path& path) {
    (void)path;
    // POSIX:unlink/改名不影响运行中的进程,恒不占(python 口径)。
    return false;
}

bool IsReparsePoint(const std::filesystem::path& path) {
    (void)path;
    return false;  // 无重解析点机制
}

bool ProcessHoldsExe(unsigned long pid, const std::filesystem::path& exe_path) {
    if (pid == 0) return false;
    // /proc/<pid>/exe 是符号链接,readlink 拿绝对路径(Linux);macOS 无
    // /proc,readlink 失败 -> false(认不出,清理侧靠 keep 集合兜底)。
    const std::string link = "/proc/" + std::to_string(pid) + "/exe";
    char buffer[PATH_MAX];
    const ssize_t length = ::readlink(link.c_str(), buffer, sizeof(buffer) - 1);
    if (length <= 0) return false;
    buffer[length] = '\0';
    std::string target(buffer);
    // 被换掉的映像带 " (deleted)" 后缀,剥掉再比。
    constexpr std::string_view kDeletedSuffix = " (deleted)";
    if (target.size() >= kDeletedSuffix.size() &&
        target.compare(target.size() - kDeletedSuffix.size(), kDeletedSuffix.size(), kDeletedSuffix) == 0) {
        target.erase(target.size() - kDeletedSuffix.size());
    }
    const std::filesystem::path target_path(target);
    return target_path.lexically_normal() == exe_path.lexically_normal();
}

}  // namespace lubancode::platform
