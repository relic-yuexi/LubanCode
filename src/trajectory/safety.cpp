// §12.1 的实现。Windows 侧 DACL 与 peer_transport_win 同一路数:SDDL 拼
// "D:P(A;OICI;GA;;;<sid>" 只准当前用户,SetNamedSecurityInfoW 落到对象上
//(PROTECTED,不并入目录继承来的宽 ACE)。POSIX 侧 chmod 0700/0600。

#include "trajectory/safety.hpp"

#include <array>
#include <cctype>

#include "platform/paths.hpp"  // Utf8ToPath/PathToUtf8

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <aclapi.h>  // SetNamedSecurityInfoW/SE_FILE_OBJECT(user-only DACL 落对象)
#include <sddl.h>    // ConvertStringSecurityDescriptorToSecurityDescriptorW
#else
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace lubancode::trajectory {
namespace {

constexpr std::size_t kMaxSegmentLength = 128;

// Windows 保留设备名(不带扩展名比较,大小写不敏感)。
bool IsWindowsReservedDeviceName(std::string_view name) {
    static constexpr std::array<std::string_view, 11> kReserved = {
        "con",  "prn",  "aux",  "nul",  "com1", "com2", "com3", "com4",
        "com5", "lpt1", "lpt2"};
    for (const std::string_view reserved : kReserved) {
        if (name.size() == reserved.size()) {
            bool same = true;
            for (std::size_t i = 0; i < name.size(); ++i) {
                if (std::tolower(static_cast<unsigned char>(name[i])) !=
                    std::tolower(static_cast<unsigned char>(reserved[i]))) {
                    same = false;
                    break;
                }
            }
            if (same) {
                return true;
            }
        }
    }
    return false;
}

#ifdef _WIN32

// 当前用户 SID 的字符串形("S-1-5-21-…");拿不到给空串。
std::string CurrentUserSid() {
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token) || token == nullptr) {
        return {};
    }
    DWORD needed = 0;
    GetTokenInformation(token, TokenUser, nullptr, 0, &needed);
    if (needed == 0) {
        CloseHandle(token);
        return {};
    }
    std::string buffer(needed, '\0');
    if (!GetTokenInformation(token, TokenUser, buffer.data(), needed, &needed)) {
        CloseHandle(token);
        return {};
    }
    CloseHandle(token);
    const auto* user = reinterpret_cast<const TOKEN_USER*>(buffer.data());
    LPWSTR sid_wide = nullptr;
    if (!ConvertSidToStringSidW(user->User.Sid, &sid_wide)) {
        return {};
    }
    std::string sid;
    for (LPWSTR p = sid_wide; *p != L'\0'; ++p) {
        sid.push_back(static_cast<char>(*p));  // SID 纯 ASCII,宽窄无损
    }
    LocalFree(sid_wide);
    return sid;
}

// SDDL "D:P(A;OICI;GA;;;<sid>" 的 PROTECTED user-only DACL。落对象用
// SetNamedSecurityInfoW(DACL_SECURITY_INFORMATION|PROTECTED_DACL_…);
// OICI 令目录的子项继承同一份 ACE,文件上无子项、同一 SDDL 无害。
bool ApplyUserOnlyDacl(const std::wstring& path) {
    const std::string sid = CurrentUserSid();
    if (sid.empty()) {
        return false;  // 拿不到 SID 不放宽——交给调用方告警
    }
    const std::wstring sddl =
        std::wstring(L"D:P(A;OICI;GA;;;") + std::wstring(sid.begin(), sid.end()) + L")";
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl.c_str(), SDDL_REVISION_1,
                                                              &descriptor, nullptr) ||
        descriptor == nullptr) {
        return false;
    }
    BOOL has_dacl = FALSE;
    BOOL dacl_defaulted = FALSE;
    PACL dacl = nullptr;
    BOOL ok = GetSecurityDescriptorDacl(descriptor, &has_dacl, &dacl, &dacl_defaulted);
    bool applied = false;
    if (ok && has_dacl && dacl != nullptr) {
        const DWORD flags = DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION;
        applied = SetNamedSecurityInfoW(const_cast<LPWSTR>(path.c_str()), SE_FILE_OBJECT, flags,
                                        nullptr, nullptr, dacl, nullptr) == ERROR_SUCCESS;
    }
    LocalFree(descriptor);
    return applied;
}

#endif  // _WIN32

}  // namespace

bool IsSafeSingleSegment(std::string_view name) {
    if (name.empty() || name.size() > kMaxSegmentLength) {
        return false;
    }
    if (name.front() == '.') {
        return false;  // "."、".."、".hidden" 一并拒(导出/记录名用不着点打头)
    }
    for (const char c : name) {
        const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                        (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '.';
        if (!ok) {
            return false;  // 分隔符/盘符冒号/通配符/空白全落在这条上
        }
    }
    return !IsWindowsReservedDeviceName(name);
}

bool IsContainedCanonicalPath(const std::filesystem::path& child, const std::filesystem::path& root) {
    std::error_code ec;
    const std::filesystem::path canonical_child = std::filesystem::weakly_canonical(child, ec);
    if (ec) {
        return false;
    }
    const std::filesystem::path canonical_root = std::filesystem::weakly_canonical(root, ec);
    if (ec) {
        return false;
    }
    auto child_it = canonical_child.begin();
    auto root_it = canonical_root.begin();
    for (; root_it != canonical_root.end(); ++root_it, ++child_it) {
        if (child_it == canonical_child.end() || *root_it != *child_it) {
            return false;
        }
    }
    // child 与 root 完全相等不算"包含之下"(那是同一文件,不是受控子项)。
    return child_it != canonical_child.end();
}

bool ContainsSymlinkOrReparse(const std::filesystem::path& root, const std::filesystem::path& child) {
    std::error_code ec;
    const std::filesystem::path canonical_root = std::filesystem::weakly_canonical(root, ec);
    if (ec) {
        return true;  // root 都解析不了:按有逃逸嫌疑处理
    }
    const std::filesystem::path canonical_child = std::filesystem::weakly_canonical(child, ec);
    if (ec) {
        return true;
    }
    // 先对齐 root 的分量:child 解析后不在 root 分支下,说明有链接把它带
    // 出去了——直接按含逃逸处理(前缀不等的另一头,canonical containment
    // 也会拦,两道门合起来才是 §12.1 的"双重防线")。
    auto child_it = canonical_child.begin();
    for (auto root_it = canonical_root.begin(); root_it != canonical_root.end(); ++root_it, ++child_it) {
        if (child_it == canonical_child.end() || *root_it != *child_it) {
            return true;
        }
    }
    // 只核 root 之下那一段;child 尚不存在的尾段跳过(建出来之前谈不上
    // 重解析点,create-new 会守住"存在即失败")。
    std::filesystem::path probe = canonical_root;
    for (; child_it != canonical_child.end(); ++child_it) {
        probe /= *child_it;
#ifdef _WIN32
        const DWORD attrs = GetFileAttributesW(probe.c_str());
        if (attrs == INVALID_FILE_ATTRIBUTES) {
            continue;  // 不存在:后续分量更不存在,继续走无害
        }
        if ((attrs & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
            return true;
        }
#else
        struct ::stat st {};
        if (::lstat(probe.c_str(), &st) != 0) {
            continue;
        }
        if (S_ISLNK(st.st_mode)) {
            return true;
        }
#endif
    }
    return false;
}

bool IsSafeContainedPath(const std::filesystem::path& child, const std::filesystem::path& root) {
    return IsContainedCanonicalPath(child, root) && !ContainsSymlinkOrReparse(root, child);
}

bool HardenDirectoryUserOnly(const std::filesystem::path& dir) {
    std::error_code ec;
    if (!std::filesystem::is_directory(dir, ec) || ec) {
        return false;
    }
#ifdef _WIN32
    return ApplyUserOnlyDacl(dir.c_str());
#else
    return ::chmod(dir.c_str(), 0700) == 0;
#endif
}

bool HardenFileUserOnly(const std::filesystem::path& file) {
    std::error_code ec;
    if (!std::filesystem::is_regular_file(file, ec) || ec) {
        return false;
    }
#ifdef _WIN32
    return ApplyUserOnlyDacl(file.c_str());
#else
    return ::chmod(file.c_str(), 0600) == 0;
#endif
}

}  // namespace lubancode::trajectory
