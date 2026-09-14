// secure_file.hpp 的实现(见合同注释)。Windows 用 SDDL 拼安全描述符:
// O:<sid> 定 owner,D:P 关继承并只放当前用户,文件不带继承旗标、目录带
// OI|CI(目录里新生的文件就算忘了带描述符,继承到的也只有当前用户)。
#include "platform/secure_file.hpp"

#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <thread>

#include "platform/paths.hpp"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <sddl.h>  // ConvertStringSecurityDescriptorToSecurityDescriptorW
#else
#include <cerrno>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/file.h>
#include <unistd.h>
#endif

namespace lubancode::platform {

namespace {

SecureFileError Fail(std::string code, std::string message) {
    return SecureFileError{std::move(code), std::move(message)};
}

#ifdef _WIN32

std::string LastErrorText(const char* what) {
    return std::string(what) + " (错误码 " + std::to_string(GetLastError()) + ")";
}

// 当前用户 SID(SDDL 形式,S-1-5-...)。与 channel/credentials.cpp 的
// CurrentUserSidString 同一拼法;这里独立成份,platform 不反向依赖 channel。
std::string CurrentUserSddlSid() {
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) {
        return {};
    }
    DWORD size = 0;
    GetTokenInformation(token, TokenUser, nullptr, 0, &size);
    if (size == 0) {
        CloseHandle(token);
        return {};
    }
    std::string buffer(size, '\0');
    if (!GetTokenInformation(token, TokenUser, buffer.data(), size, &size)) {
        CloseHandle(token);
        return {};
    }
    CloseHandle(token);
    const auto* user = reinterpret_cast<const TOKEN_USER*>(buffer.data());
    LPWSTR sid_wide = nullptr;
    if (!ConvertSidToStringSidW(user->User.Sid, &sid_wide) || sid_wide == nullptr) {
        return {};
    }
    std::string sid;
    for (const wchar_t* p = sid_wide; *p != L'\0'; ++p) {
        sid.push_back(static_cast<char>(*p));  // SID 是纯 ASCII,宽窄无损
    }
    LocalFree(sid_wide);
    return sid;
}

// 拼安全描述符:O:<sid>D:P + ACE。inheritable=true 给目录(OI|CI)。
std::expected<SECURITY_ATTRIBUTES, SecureFileError> MakeOwnerOnlyAttributes(bool inheritable) {
    const std::string sid = CurrentUserSddlSid();
    if (sid.empty()) {
        return std::unexpected(Fail("secure_file.internal_failed", "拿不到当前用户 SID"));
    }
    const std::string sddl =
        "O:" + sid + (inheritable ? std::string("D:P(A;OICI;FA;;;") : std::string("D:P(A;;FA;;;")) +
        sid + ")";
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(
            Utf8ToWide(sddl).c_str(), SDDL_REVISION_1, &descriptor, nullptr)) {
        return std::unexpected(
            Fail("secure_file.internal_failed", LastErrorText("拼安全描述符失败")));
    }
    SECURITY_ATTRIBUTES attributes{};
    attributes.nLength = sizeof(attributes);
    attributes.lpSecurityDescriptor = descriptor;
    attributes.bInheritHandle = FALSE;
    return attributes;
}

#endif

}  // namespace

std::expected<void, SecureFileError> RejectReparsePoint(const std::filesystem::path& path) {
#ifdef _WIN32
    const DWORD attrs = GetFileAttributesW(path.c_str());
    if (attrs == INVALID_FILE_ATTRIBUTES) {
        return {};  // 不存在:没有可拒的重解析点
    }
    if ((attrs & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
        return std::unexpected(Fail("secure_file.path_is_link",
                                    "路径是符号链接/重解析点,拒绝在这里建凭据文件: " + PathToUtf8(path)));
    }
    return {};
#else
    struct stat info {};
    if (::lstat(path.c_str(), &info) != 0) {
        return {};  // 不存在:没有可拒的链接
    }
    if (S_ISLNK(info.st_mode)) {
        return std::unexpected(Fail("secure_file.path_is_link",
                                    "路径是符号链接,拒绝在这里建凭据文件: " + PathToUtf8(path)));
    }
    return {};
#endif
}

std::expected<void, SecureFileError> CreateSecureDirectory(const std::filesystem::path& path) {
    if (auto rejected = RejectReparsePoint(path); !rejected.has_value()) {
        return rejected;
    }
    std::error_code ec;
    if (std::filesystem::is_directory(path, ec)) {
        return {};  // 已存在且是目录:原样收下,不动既有 ACL
    }
    // 父目录不在就先建普通目录(这里不递归收紧权限——只管凭据目录本身)。
    if (!path.parent_path().empty()) {
        std::filesystem::create_directories(path.parent_path(), ec);
        // 父目录已存在时 create_directories 也可能回 ec==已存在;判成败
        // 只看父目录最终在不在。
        std::error_code parent_ec;
        if (!std::filesystem::is_directory(path.parent_path(), parent_ec)) {
            return std::unexpected(
                Fail("secure_file.mkdir_failed", "父目录建不成: " + PathToUtf8(path.parent_path())));
        }
    }
#ifdef _WIN32
    auto attributes = MakeOwnerOnlyAttributes(/*inheritable=*/true);
    if (!attributes.has_value()) {
        return std::unexpected(attributes.error());
    }
    if (!CreateDirectoryW(path.c_str(), &*attributes)) {
        const DWORD error = GetLastError();
        LocalFree(attributes->lpSecurityDescriptor);
        if (error == ERROR_ALREADY_EXISTS) {
            // 并发撞车:另一路刚建成同目录是正常事(那一路带着安全描述符),
            // 重验是目录即收;真被非目录占着才报。
            std::error_code exists_ec;
            if (std::filesystem::is_directory(path, exists_ec)) {
                return {};
            }
            return std::unexpected(Fail("secure_file.path_not_directory",
                                        "路径已被非目录占着: " + PathToUtf8(path)));
        }
        return std::unexpected(Fail("secure_file.mkdir_failed", LastErrorText("建目录失败")));
    }
    LocalFree(attributes->lpSecurityDescriptor);
    return {};
#else
    if (::mkdir(path.c_str(), 0700) != 0) {
        if (errno == EEXIST) {
            // 并发撞车:另一路刚建成(0700)是正常事,重验是目录即收。
            std::error_code exists_ec;
            if (std::filesystem::is_directory(path, exists_ec)) {
                return {};
            }
            return std::unexpected(Fail("secure_file.path_not_directory",
                                        "路径已被非目录占着: " + PathToUtf8(path)));
        }
        return std::unexpected(Fail("secure_file.mkdir_failed",
                                    std::string("建目录失败: ") + std::strerror(errno) + ": " +
                                        PathToUtf8(path)));
    }
    return {};
#endif
}

std::expected<void, SecureFileError> WriteNewSecureFile(const std::filesystem::path& path,
                                                        std::string_view bytes) {
    if (auto rejected = RejectReparsePoint(path); !rejected.has_value()) {
        return rejected;
    }
#ifdef _WIN32
    auto attributes = MakeOwnerOnlyAttributes(/*inheritable=*/false);
    if (!attributes.has_value()) {
        return std::unexpected(attributes.error());
    }
    HANDLE handle = CreateFileW(path.c_str(), GENERIC_WRITE, 0 /*不共享*/, &*attributes,
                                CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    LocalFree(attributes->lpSecurityDescriptor);
    if (handle == INVALID_HANDLE_VALUE) {
        return std::unexpected(Fail("secure_file.create_failed",
                                    LastErrorText("创建文件失败(create-new 语义,已存在即报错)")));
    }
    bool write_ok = true;
    std::string write_detail;
    if (!bytes.empty()) {
        DWORD written = 0;
        if (!WriteFile(handle, bytes.data(), static_cast<DWORD>(bytes.size()), &written, nullptr) ||
            written != bytes.size()) {
            write_ok = false;
            write_detail = LastErrorText("写文件失败");
        }
    }
    if (write_ok && !FlushFileBuffers(handle)) {
        write_ok = false;
        write_detail = LastErrorText("flush 失败");
    }
    if (!CloseHandle(handle)) {
        write_ok = false;
        if (write_detail.empty()) {
            write_detail = LastErrorText("关文件失败");
        }
    }
    if (!write_ok) {
        std::error_code ignored;
        std::filesystem::remove(path, ignored);
        return std::unexpected(Fail("secure_file.write_failed", write_detail));
    }
    return {};
#else
    const int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW, 0600);
    if (fd < 0) {
        return std::unexpected(Fail("secure_file.create_failed",
                                    std::string("创建文件失败(excl 语义,已存在/是链接即报错): ") +
                                        std::strerror(errno)));
    }
    bool write_ok = true;
    std::string write_detail;
    if (!bytes.empty()) {
        std::size_t done = 0;
        while (done < bytes.size()) {
            const ssize_t wrote = ::write(fd, bytes.data() + done, bytes.size() - done);
            if (wrote <= 0) {
                if (errno == EINTR) {
                    continue;
                }
                write_ok = false;
                write_detail = std::string("写文件失败: ") + std::strerror(errno);
                break;
            }
            done += static_cast<std::size_t>(wrote);
        }
    }
    if (write_ok && ::fsync(fd) != 0) {
        write_ok = false;
        write_detail = std::string("fsync 失败: ") + std::strerror(errno);
    }
    if (::close(fd) != 0) {
        write_ok = false;
        if (write_detail.empty()) {
            write_detail = std::string("关文件失败: ") + std::strerror(errno);
        }
    }
    if (!write_ok) {
        std::error_code ignored;
        std::filesystem::remove(path, ignored);
        return std::unexpected(Fail("secure_file.write_failed", write_detail));
    }
    return {};
#endif
}

// ---------------------------------------------------------------------------
// InterProcessFileLock
// ---------------------------------------------------------------------------

namespace {

void SleepSlice() { std::this_thread::sleep_for(std::chrono::milliseconds(25)); }

}  // namespace

InterProcessFileLock::InterProcessFileLock(const std::filesystem::path& lock_file, int timeout_ms) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
#ifdef _WIN32
    HANDLE handle = CreateFileW(lock_file.c_str(), GENERIC_READ | GENERIC_WRITE,
                                FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS,
                                FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        return;
    }
    while (true) {
        if (LockFileEx(handle, LOCKFILE_EXCLUSIVE_LOCK | LOCKFILE_FAIL_IMMEDIATELY, 0, MAXDWORD,
                       MAXDWORD)) {
            handle_ = handle;
            holds_ = true;
            return;
        }
        if (std::chrono::steady_clock::now() >= deadline) {
            break;
        }
        SleepSlice();
    }
    CloseHandle(handle);
#else
    const int fd = ::open(lock_file.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0600);
    if (fd < 0) {
        return;
    }
    while (true) {
        if (::flock(fd, LOCK_EX | LOCK_NB) == 0) {
            fd_ = fd;
            holds_ = true;
            return;
        }
        if (std::chrono::steady_clock::now() >= deadline) {
            break;
        }
        SleepSlice();
    }
    ::close(fd);
#endif
}

InterProcessFileLock::~InterProcessFileLock() {
#ifdef _WIN32
    if (handle_ != nullptr) {
        OVERLAPPED overlapped{};
        UnlockFileEx(handle_, 0, MAXDWORD, MAXDWORD, &overlapped);
        CloseHandle(handle_);
    }
#else
    if (fd_ >= 0) {
        ::flock(fd_, LOCK_UN);
        ::close(fd_);
    }
#endif
}

}  // namespace lubancode::platform
