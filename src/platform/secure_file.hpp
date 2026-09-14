// 凭据文件的安全创建原语(QQBot Windows 连接诊断与交互配置修复单 §5.2)。
//
// 合同(与 channel/credentials.hpp 的读取检查同一口径,这里管"生下来就
// 合格"):
//   - Windows:目录与文件创建时即带安全描述符——owner=当前用户,DACL 关
//     继承(PROTECTED),默认只允许当前用户。绝不"先写明文再收紧权限"。
//   - POSIX:目录 0700、文件 0600(O_CREAT|O_EXCL|O_NOFOLLOW,不跟随
//     符号链接);写后 fsync。
//   - 两平台共享同一写入合同,平台实现分别落地。
//   - 路径核验:secrets 根若是符号链接/重解析点,拒收——不借替换目标
//     绕过检查;已有文件更新不跟随任意链接,不递归改父目录权限。
//
// 纯平台件:不认渠道概念,密钥内容只当不透明字节流过路,不落任何日志。
#pragma once

#include <cstdint>
#include <expected>
#include <filesystem>
#include <string>
#include <string_view>

namespace lubancode::platform {

// 稳定码:
//   secure_file.mkdir_failed      目录建不成(权限/占位/盘错)
//   secure_file.path_is_link      路径(或其存在的祖先终点)是符号链接/重解析点
//   secure_file.path_not_directory 已存在但不是目录
//   secure_file.create_failed     文件建不成(已存在/权限/盘错;detail 带 OS 错误)
//   secure_file.write_failed      写入/flush/close 失败
//   secure_file.internal_failed   拼安全描述符等内部步骤失败(拿不到用户 SID)
struct SecureFileError {
    std::string code;
    std::string message;  // 人话,可带路径,不带文件内容
};

// 建凭据目录(幂等):不存在则以严格安全描述符创建;已存在且是目录则原样
// 返回成功——不递归改父目录权限,不改既有目录的 ACL(是否合格由
// channel::InspectCredentialFileSecurity 之类的读取检查说了算)。路径本身
// 是符号链接/重解析点时拒收。
std::expected<void, SecureFileError> CreateSecureDirectory(const std::filesystem::path& path);

// 以严格权限创建并写一枚"全新"文件(绝不覆盖既有文件:已存在即报错,
// CREATE_NEW/O_EXCL 语义)。Windows 的安全描述符在 CreateFileW 创建那一步
// 就带上——磁盘上不存在"明文已落、权限未收紧"的窗口。写完 fsync 再关。
std::expected<void, SecureFileError> WriteNewSecureFile(const std::filesystem::path& path,
                                                        std::string_view bytes);

// 路径形状核验(独立暴露,创建与收紧共用):路径的最终段是符号链接/重
// 解析点时返回 path_is_link。POSIX 用 lstat;Windows 用
// GetFileAttributesW 的 FILE_ATTRIBUTE_REPARSE_POINT。
std::expected<void, SecureFileError> RejectReparsePoint(const std::filesystem::path& path);

// ---------------------------------------------------------------------------
// 跨进程文件锁(配置更新加锁,§5.2"避免两个向导互相覆盖")
// ---------------------------------------------------------------------------

// Windows LockFileEx / POSIX flock 的 RAII。锁文件不存在会顺手建(凭据
// 目录内,权限随目录)。进程退出(含崩溃)锁自动释放——不留假死锁,也
// 不需要可见锁便删那套账。timeout_ms 内抢不到返回 false。
class InterProcessFileLock {
public:
    InterProcessFileLock(const std::filesystem::path& lock_file, int timeout_ms);
    ~InterProcessFileLock();

    InterProcessFileLock(const InterProcessFileLock&) = delete;
    InterProcessFileLock& operator=(const InterProcessFileLock&) = delete;

    bool holds() const { return holds_; }

private:
    bool holds_ = false;
#ifdef _WIN32
    void* handle_ = nullptr;  // HANDLE
#else
    int fd_ = -1;
#endif
};

}  // namespace lubancode::platform
