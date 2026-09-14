// 渠道凭据 resolver 实现(QQ 机器人接入单 Q0)。合同见 credentials.hpp
// 与 configuration.md §4。稳定码清单在头文件;这里只给脱敏 detail。
#include "channel/credentials.hpp"

#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <memory>
#include <system_error>

#include "platform/paths.hpp"          // Utf8ToPath/PathToUtf8
#include "platform/text_encoding.hpp"  // IsValidUtf8

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <aclapi.h>  // GetNamedSecurityInfoW(文件 owner/DACL)
#include <sddl.h>    // ConvertSidToStringSidW
#else
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace lubancode::channel {

namespace {

ChannelCredentialError MakeError(const std::string& reason, std::string detail) {
    ChannelCredentialError error;
    error.reason = reason;
    error.detail = std::move(detail);
    return error;
}

// 环境变量原值读取:区分"没设"(nullptr)与"设了但是空串"。MSVC 的
// std::getenv 吃 C4996(platform::GetEnvVar 因此把两者并成一档,这里
// 不能并),走 _dupenv_s。
#ifdef _WIN32
std::unique_ptr<char, decltype(&std::free)> ReadEnvRaw(const char* name) {
    char* value = nullptr;
    std::size_t size = 0;
    if (_dupenv_s(&value, &size, name) != 0 || value == nullptr) {
        return {nullptr, &std::free};
    }
    return {value, &std::free};
}
#else
const char* ReadEnvRaw(const char* name) { return std::getenv(name); }
#endif

// 内容规矩(§三):原值即 AppSecret;至多剥末尾一处换行(\r\n 或 \n);
// 其余字节一概不动;内部控制字符与空值拒收。
std::expected<std::string, ChannelCredentialError> NormalizeSecretContent(std::string raw) {
    if (!platform::IsValidUtf8(raw)) {
        return std::unexpected(
            MakeError("secret_file_invalid_utf8", "secret_file 内容不是合法 UTF-8"));
    }
    if (!raw.empty() && raw.back() == '\n') {
        raw.pop_back();
        if (!raw.empty() && raw.back() == '\r') {
            raw.pop_back();
        }
    }
    for (const char c : raw) {
        const unsigned char byte = static_cast<unsigned char>(c);
        if (byte < 0x20 || byte == 0x7F) {
            return std::unexpected(MakeError(
                "secret_file_bad_content", "内容含内部控制字符(换行只许末尾那一处)"));
        }
    }
    if (raw.empty()) {
        return std::unexpected(MakeError("secret_file_empty", "剥掉末尾换行后为空"));
    }
    return raw;
}

}  // namespace

// ---- 平台半边:文件归属与权限检查 -------------------------------------------

#ifdef _WIN32

namespace {

std::string WideToNarrow(const wchar_t* wide) {
    std::string out;
    for (const wchar_t* p = wide; *p != L'\0'; ++p) {
        out.push_back(static_cast<char>(*p));  // SID 是纯 ASCII,宽窄无损
    }
    return out;
}

std::string CurrentUserSidString() {
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
    if (!ConvertSidToStringSidW(user->User.Sid, &sid_wide)) {
        return {};
    }
    const std::string sid = WideToNarrow(sid_wide);
    LocalFree(sid_wide);
    return sid;
}

// DACL 里允许的账户:当前用户本人、SYSTEM、内建 Administrators,加两枚
// 应用容器组(部分系统目录的继承默认项;沙箱容器,不是交互用户)。其余
// 账户(Users 组、Everyone、Anonymous)拿到任何访问权都算不安全。
bool SidAllowedForCredentialFile(const std::string& sid, const std::string& current_user) {
    if (sid == current_user) return true;
    if (sid == "S-1-5-18") return true;      // LOCAL_SYSTEM
    if (sid == "S-1-5-32-544") return true;  // BUILTIN\Administrators
    if (sid == "S-1-15-2-1") return true;    // ALL APPLICATION PACKAGES(继承默认)
    if (sid == "S-1-15-2-2") return true;    // ALL RESTRICTED APPLICATION PACKAGES
    return false;
}

// §5.3 权限分类:把"不安全"拆成可处置的几档;CheckFileSecurityWin 消费
// 同一份结论,读取口径一字不变。
CredentialFileSecurityReport InspectFileSecurityWin(const std::wstring& wide_path) {
    CredentialFileSecurityReport report;
    const std::string current_user = CurrentUserSidString();
    if (current_user.empty()) {
        report.status = CredentialFileSecurityReport::Status::DescriptorUnreadable;
        report.detail = "拿不到当前用户 SID,无法核对文件归属";
        return report;
    }
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    PSID owner_sid = nullptr;
    PACL dacl = nullptr;
    const DWORD result = GetNamedSecurityInfoW(wide_path.c_str(), SE_FILE_OBJECT,
                                               OWNER_SECURITY_INFORMATION |
                                                   DACL_SECURITY_INFORMATION,
                                               &owner_sid, nullptr, &dacl, nullptr,
                                               &descriptor);
    if (result != ERROR_SUCCESS || descriptor == nullptr) {
        report.status = CredentialFileSecurityReport::Status::DescriptorUnreadable;
        report.detail = "读不到文件安全描述符(错误码 " + std::to_string(result) + ")";
        return report;
    }
    LPWSTR owner_wide = nullptr;
    const bool owner_ok = ConvertSidToStringSidW(owner_sid, &owner_wide) && owner_wide != nullptr;
    const std::string owner = owner_ok ? WideToNarrow(owner_wide) : std::string();
    if (owner_wide != nullptr) {
        LocalFree(owner_wide);
    }
    if (!owner_ok || owner != current_user) {
        LocalFree(descriptor);
        report.status = CredentialFileSecurityReport::Status::OwnerMismatch;
        report.detail = "文件归属不是当前用户(owner SID 不符)";
        return report;
    }
    if (dacl == nullptr) {
        // 无 DACL = 全员完全访问,POSIX 侧的 0666 同罪。
        LocalFree(descriptor);
        report.status = CredentialFileSecurityReport::Status::NoDacl;
        report.detail = "文件没有 DACL(全员可访问)";
        return report;
    }
    for (WORD i = 0; i < dacl->AceCount; ++i) {
        void* ace = nullptr;
        if (!GetAce(dacl, i, &ace)) {
            continue;
        }
        const auto* header = static_cast<const ACE_HEADER*>(ace);
        if (header->AceType != ACCESS_ALLOWED_ACE_TYPE) {
            continue;  // deny ACE 不放宽权限
        }
        const auto* allowed = static_cast<const ACCESS_ALLOWED_ACE*>(ace);
        LPWSTR ace_sid_wide = nullptr;
        if (!ConvertSidToStringSidW(reinterpret_cast<PSID>(const_cast<ACCESS_MASK*>(
                                        &allowed->SidStart)),
                                    &ace_sid_wide) ||
            ace_sid_wide == nullptr) {
            continue;
        }
        const std::string ace_sid = WideToNarrow(ace_sid_wide);
        LocalFree(ace_sid_wide);
        if (!SidAllowedForCredentialFile(ace_sid, current_user)) {
            LocalFree(descriptor);
            report.status = CredentialFileSecurityReport::Status::DaclTooWide;
            report.detail = "DACL 放行了当前用户以外的账户(SID " + ace_sid + ")";
            return report;
        }
    }
    LocalFree(descriptor);
    report.status = CredentialFileSecurityReport::Status::Ok;
    return report;
}

std::expected<void, ChannelCredentialError> CheckFileSecurityWin(const std::wstring& wide_path) {
    const CredentialFileSecurityReport report = InspectFileSecurityWin(wide_path);
    if (report.status == CredentialFileSecurityReport::Status::Ok) {
        return {};
    }
    return std::unexpected(MakeError("secret_file_insecure", report.detail));
}

}  // namespace

std::expected<void, ChannelCredentialError> CheckCredentialFileSecurity(
    const std::filesystem::path& canonical_path) {
    return CheckFileSecurityWin(canonical_path.wstring());
}

CredentialFileSecurityReport InspectCredentialFileSecurity(
    const std::filesystem::path& canonical_path) {
    return InspectFileSecurityWin(canonical_path.wstring());
}

#else  // POSIX

std::expected<void, ChannelCredentialError> CheckCredentialFileSecurity(
    const std::filesystem::path& canonical_path) {
    const CredentialFileSecurityReport report = InspectCredentialFileSecurity(canonical_path);
    if (report.status == CredentialFileSecurityReport::Status::Ok) {
        return {};
    }
    return std::unexpected(MakeError("secret_file_insecure", report.detail));
}

CredentialFileSecurityReport InspectCredentialFileSecurity(
    const std::filesystem::path& canonical_path) {
    CredentialFileSecurityReport report;
    struct stat info {};
    if (::stat(canonical_path.c_str(), &info) != 0) {
        report.status = CredentialFileSecurityReport::Status::DescriptorUnreadable;
        report.detail = "stat 失败: " + std::string(std::strerror(errno));
        return report;
    }
    if (info.st_uid != ::geteuid()) {
        report.status = CredentialFileSecurityReport::Status::OwnerMismatch;
        report.detail = "文件归属不是当前用户";
        return report;
    }
    if (info.st_mode & (S_IRGRP | S_IWGRP | S_IROTH | S_IWOTH)) {
        report.status = CredentialFileSecurityReport::Status::DaclTooWide;
        report.detail = "组/其他用户有读写位(须 0600 一档)";
        return report;
    }
    report.status = CredentialFileSecurityReport::Status::Ok;
    return report;
}

#endif

// ---- resolver 主路 ----------------------------------------------------------

std::expected<ResolvedChannelCredential, ChannelCredentialError> ResolveChannelCredential(
    const ChannelAccountUserConfig& account) {
    // 1) secret_file(最高优先级;QQ 首版推荐)。
    if (account.secret_file.has_value() && !account.secret_file->empty()) {
        const std::filesystem::path raw = platform::Utf8ToPath(*account.secret_file);
        if (!raw.is_absolute()) {
            return std::unexpected(MakeError("secret_file_relative",
                                             "secret_file 须是绝对路径: " + *account.secret_file));
        }
        // canonical 解析:符号链接/重解析点解析到真实目标再验(weakly_
        // canonical 对不存在的尾巴保持词法,存在段全解)。
        std::error_code ec;
        const std::filesystem::path canonical = std::filesystem::weakly_canonical(raw, ec);
        if (ec) {
            return std::unexpected(
                MakeError("secret_file_missing", "路径解析失败: " + *account.secret_file));
        }
        if (!std::filesystem::exists(canonical, ec)) {
            return std::unexpected(MakeError("secret_file_missing",
                                             "文件不存在: " + platform::PathToUtf8(canonical)));
        }
        if (!std::filesystem::is_regular_file(canonical, ec)) {
            return std::unexpected(MakeError("secret_file_not_regular",
                                             "不是常规文件: " + platform::PathToUtf8(canonical)));
        }
        if (const auto secure = CheckCredentialFileSecurity(canonical); !secure.has_value()) {
            return std::unexpected(secure.error());
        }
        std::error_code size_ec;
        const std::uintmax_t size = std::filesystem::file_size(canonical, size_ec);
        if (size_ec) {
            return std::unexpected(MakeError("secret_file_read_fail",
                                             "读不到文件大小: " + platform::PathToUtf8(canonical)));
        }
        if (size > kCredentialFileMaxBytes) {
            return std::unexpected(MakeError(
                "secret_file_too_large",
                "文件 " + std::to_string(size) + " 字节,超过上限 " +
                    std::to_string(kCredentialFileMaxBytes)));
        }
        std::ifstream file(canonical, std::ios::binary);
        if (!file.is_open()) {
            return std::unexpected(MakeError("secret_file_read_fail",
                                             "打不开文件: " + platform::PathToUtf8(canonical)));
        }
        std::string raw_content(size, '\0');
        file.read(raw_content.data(), static_cast<std::streamsize>(size));
        if (!file) {
            return std::unexpected(MakeError("secret_file_read_fail",
                                             "读取中断: " + platform::PathToUtf8(canonical)));
        }
        auto content = NormalizeSecretContent(std::move(raw_content));
        if (!content.has_value()) {
            return std::unexpected(content.error());
        }
        ResolvedChannelCredential resolved;
        resolved.source = ResolvedChannelCredential::Source::File;
        resolved.secret = std::move(*content);
        return resolved;
    }
    // 2) secret_env。高优先级来源(secret_file)配置了但无效,上面已经
    // 明报返回——不落到这里,这就是"不静默降级"。
    if (account.secret_env.has_value() && !account.secret_env->empty()) {
#ifdef _WIN32
        const auto value = ReadEnvRaw(account.secret_env->c_str());
        if (value == nullptr) {
            return std::unexpected(
                MakeError("secret_env_missing", "环境变量未设置: " + *account.secret_env));
        }
        if (value.get()[0] == '\0') {
            return std::unexpected(
                MakeError("secret_env_empty", "环境变量为空串: " + *account.secret_env));
        }
        ResolvedChannelCredential resolved;
        resolved.source = ResolvedChannelCredential::Source::Env;
        resolved.secret = value.get();
        return resolved;
#else
        const char* value = ReadEnvRaw(account.secret_env->c_str());
        if (value == nullptr) {
            return std::unexpected(
                MakeError("secret_env_missing", "环境变量未设置: " + *account.secret_env));
        }
        if (*value == '\0') {
            return std::unexpected(
                MakeError("secret_env_empty", "环境变量为空串: " + *account.secret_env));
        }
        ResolvedChannelCredential resolved;
        resolved.source = ResolvedChannelCredential::Source::Env;
        resolved.secret = value;
        return resolved;
#endif
    }
    // 3) 明文兼容(doctor 报 warning 的那档)。
    if (account.secret.has_value() && !account.secret->empty()) {
        ResolvedChannelCredential resolved;
        resolved.source = ResolvedChannelCredential::Source::InlinePlaintext;
        resolved.secret = *account.secret;
        return resolved;
    }
    return std::unexpected(MakeError("credential_missing", "secret_file/secret_env/secret 都没配"));
}

const std::vector<std::string>& SidecarEnvAllowlist() {
    // 渠道子进程只继承必要环境(Q0 定形合同;Q1 若定案进程内直连则本表
    // 不消费,定案受管子进程则 spawn 实装照此):
    //   - 进程基件(SystemRoot/SystemDrive/PATHEXT:Windows 下缺了连 CRT
    //     都起不稳;PATH 找运行时);
    //   - 临时目录(TEMP/TMP:运行时内部要用);
    //   - 语言时区(LANG/LC_ALL/TZ/HOME/USERPROFILE:日志与编码兜底)。
    // 凭据不走环境(交付通道见 configuration.md §4,传输无关);宿主模型
    // API key、其他账号的密钥环境变量不在名单里,一律不递。
    static const std::vector<std::string> allowlist = {
        "PATH",       "PATHEXT",   "SystemRoot", "SystemDrive", "TEMP",
        "TMP",        "LANG",      "LC_ALL",     "TZ",          "HOME",
        "USERPROFILE"};
    return allowlist;
}

std::string RedactSecret(std::string text, const std::string& secret) {
    if (secret.empty()) {
        return text;
    }
    std::size_t at = text.find(secret);
    while (at != std::string::npos) {
        text.replace(at, secret.size(), "<redacted>");
        at = text.find(secret, at);
    }
    return text;
}

}  // namespace lubancode::channel
