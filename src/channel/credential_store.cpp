// credential_store.hpp 的实现。Windows 收紧走 SetNamedSecurityInfoW:
// owner 不符时不动手(那要 SeTakeOwnershipPrivilege,本件不提权);
// DACL 用 SDDL 现拼(与 platform/secure_file.cpp 同一拼法)。
#include "channel/credential_store.hpp"

#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <set>

#include "channel/channel_config.hpp"  // IsValidChannelId/IsValidChannelAccountId
#include "channel/credentials.hpp"     // CheckCredentialFileSecurity/Inspect...
#include "config/config.hpp"           // HomeLubancodeDir(默认受管根)
#include "platform/paths.hpp"
#include "platform/secure_file.hpp"
#include "platform/text_encoding.hpp"  // IsValidUtf8

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <sddl.h>
#else
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace lubancode::channel {

namespace {

CredentialStoreError Fail(std::string reason, std::string detail) {
    return CredentialStoreError{std::move(reason), std::move(detail)};
}

// 与 credentials.cpp 的内容规矩同一口径(至多剥末尾一处换行的责任在
// 调用方;这里的输入是用户敲进来的单行,不该带换行)。
std::optional<CredentialStoreError> ValidateSecretContent(const std::string& secret) {
    if (secret.empty()) {
        return Fail("credential_store_write_failed", "密钥内容为空");
    }
    if (!platform::IsValidUtf8(secret)) {
        return Fail("credential_store_write_failed", "密钥内容不是合法 UTF-8");
    }
    if (secret.size() > kCredentialFileMaxBytes) {
        return Fail("credential_store_write_failed", "密钥内容超过上限 " +
                                                         std::to_string(kCredentialFileMaxBytes) +
                                                         " 字节");
    }
    for (const char c : secret) {
        const unsigned char byte = static_cast<unsigned char>(c);
        if (byte < 0x20 || byte == 0x7F) {
            return Fail("credential_store_write_failed", "密钥内容含控制字符");
        }
    }
    return std::nullopt;
}

std::string NextNonce() {
    static std::atomic<std::uint64_t> counter{0};
    const std::uint64_t serial = counter.fetch_add(1, std::memory_order_relaxed);
#ifdef _WIN32
    const std::uint64_t pid = static_cast<std::uint64_t>(GetCurrentProcessId());
#else
    const std::uint64_t pid = static_cast<std::uint64_t>(::getpid());
#endif
    const auto now = std::chrono::system_clock::now().time_since_epoch().count();
    return "v" + std::to_string(now) + "-" + std::to_string(pid) + "-" +
           std::to_string(serial);
}

}  // namespace

CredentialStore::CredentialStore(std::filesystem::path secrets_root)
    : secrets_root_(std::move(secrets_root)) {}

std::optional<std::filesystem::path> CredentialStore::DefaultSecretsRoot() {
    const auto home = config::HomeLubancodeDir();
    if (!home.has_value()) {
        return std::nullopt;
    }
    return platform::Utf8ToPath(*home) / "secrets";
}

std::filesystem::path CredentialStore::ManagedSecretPath(const std::string& channel_id,
                                                         const std::string& account_id,
                                                         const std::string& nonce) const {
    return secrets_root_ / (channel_id + "-" + account_id + "." + nonce + ".secret");
}

std::expected<std::filesystem::path, CredentialStoreError> CredentialStore::WriteNewManagedSecret(
    const std::string& channel_id, const std::string& account_id,
    const std::string& secret) const {
    if (!IsValidChannelId(channel_id) || !IsValidChannelAccountId(account_id)) {
        return std::unexpected(Fail("credential_store_bad_id",
                                    "渠道/账号 id 含路径段或控制字符,不能用作受管文件名"));
    }
    if (const auto invalid = ValidateSecretContent(secret); invalid.has_value()) {
        return std::unexpected(*invalid);
    }
    if (const auto dir = platform::CreateSecureDirectory(secrets_root_); !dir.has_value()) {
        return std::unexpected(Fail("credential_store_dir_failed", dir.error().message));
    }
    // 新密钥 = 独立版本文件:文件名唯一,创建即带严格权限,读者在配置
    // 指过来之前看不见它——"同目录安全临时件 + 原子提交"在这一形态下
    // 由"唯一名 + CREATE_NEW"达成,替换式换名不需要。
    std::filesystem::path target;
    for (int attempt = 0; attempt < 8; ++attempt) {
        target = ManagedSecretPath(channel_id, account_id, NextNonce());
        const auto written = platform::WriteNewSecureFile(target, secret);
        if (written.has_value()) {
            break;
        }
        if (written.error().code != "secure_file.create_failed") {
            return std::unexpected(Fail("credential_store_write_failed",
                                        written.error().message + ": " +
                                            platform::PathToUtf8(target)));
        }
        // create_failed:并发同名撞车(概率近乎零)换 nonce 重试;真权限
        // 问题重试也一样,8 次后如实报。
        target.clear();
    }
    if (target.empty()) {
        return std::unexpected(
            Fail("credential_store_write_failed", "受管密钥文件创建反复失败: " +
                                                      platform::PathToUtf8(secrets_root_)));
    }
    // 写后复验:生产读取器的安全检查(不是本件自说自话)。
    std::error_code ec;
    const auto canonical = std::filesystem::weakly_canonical(target, ec);
    if (const auto check = CheckCredentialFileSecurity(ec ? target : canonical);
        !check.has_value()) {
        // 复验不过:这件不合格,删掉,不留宽松临时文件。
        std::error_code ignored;
        std::filesystem::remove(target, ignored);
        return std::unexpected(Fail("credential_store_verify_failed",
                                    check.error().reason + ": " + check.error().detail));
    }
    return target;
}

std::expected<void, CredentialStoreError> CredentialStore::TightenFilePermissions(
    const std::filesystem::path& path) const {
    std::error_code ec;
    const auto canonical = std::filesystem::weakly_canonical(path, ec);
    const std::filesystem::path effective = ec ? path : canonical;
    const auto before = InspectCredentialFileSecurity(effective);
    if (before.status == CredentialFileSecurityReport::Status::Ok) {
        return {};  // 已合格:幂等成功
    }
    if (before.status == CredentialFileSecurityReport::Status::OwnerMismatch) {
        // owner 不符:不提权、不夺所有权——准确报错,引导重新输入到新
        // 受管文件(§5.3)。
        return std::unexpected(
            Fail("credential_store_tighten_failed",
                 "文件归属不是当前用户,程序不夺取所有权: " + before.detail));
    }
#ifdef _WIN32
    // 当前用户 SID 现拼 D:P(A;;FA;;;<sid>)。
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) {
        return std::unexpected(
            Fail("credential_store_classify_failed", "拿不到当前用户 SID,无法收紧"));
    }
    DWORD size = 0;
    GetTokenInformation(token, TokenUser, nullptr, 0, &size);
    std::string buffer(size, '\0');
    PSID user_sid = nullptr;
    bool got_sid = false;
    if (size > 0 &&
        GetTokenInformation(token, TokenUser, buffer.data(), size, &size)) {
        user_sid = reinterpret_cast<const TOKEN_USER*>(buffer.data())->User.Sid;
        got_sid = true;
    }
    CloseHandle(token);
    if (!got_sid) {
        return std::unexpected(
            Fail("credential_store_classify_failed", "拿不到当前用户 SID,无法收紧"));
    }
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    LPWSTR sid_wide = nullptr;
    if (!ConvertSidToStringSidW(user_sid, &sid_wide) || sid_wide == nullptr) {
        return std::unexpected(
            Fail("credential_store_classify_failed", "当前用户 SID 转换失败,无法收紧"));
    }
    std::string sid;
    for (const wchar_t* p = sid_wide; *p != L'\0'; ++p) {
        sid.push_back(static_cast<char>(*p));
    }
    LocalFree(sid_wide);
    const std::string sddl = "O:" + sid + "D:P(A;;FA;;;" + sid + ")";
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(
            platform::Utf8ToWide(sddl).c_str(), SDDL_REVISION_1, &descriptor, nullptr) ||
        descriptor == nullptr) {
        return std::unexpected(
            Fail("credential_store_classify_failed", "拼收紧用安全描述符失败"));
    }
    BOOL present = FALSE, defaulted = FALSE;
    PACL dacl = nullptr;
    BOOL has_owner = FALSE;
    PSID owner_sid_from_sd = nullptr;
    if (!GetSecurityDescriptorOwner(descriptor, &owner_sid_from_sd, &defaulted)) {
        has_owner = FALSE;
    } else {
        has_owner = owner_sid_from_sd != nullptr ? TRUE : FALSE;
    }
    if (!GetSecurityDescriptorDacl(descriptor, &present, &dacl, &defaulted) || !present ||
        dacl == nullptr) {
        LocalFree(descriptor);
        return std::unexpected(
            Fail("credential_store_classify_failed", "收紧用安全描述符里没有 DACL"));
    }
    // OWNER + DACL(带 PROTECTED,关继承)。owner 已核为当前用户,不越权。
    const DWORD result = SetNamedSecurityInfoW(
        const_cast<LPWSTR>(effective.wstring().c_str()), SE_FILE_OBJECT,
        OWNER_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION |
            PROTECTED_DACL_SECURITY_INFORMATION,
        has_owner ? owner_sid_from_sd : nullptr, nullptr, dacl, nullptr);
    LocalFree(descriptor);
    if (result != ERROR_SUCCESS) {
        return std::unexpected(Fail(
            "credential_store_tighten_failed",
            "重设安全描述符失败(错误码 " + std::to_string(result) +
                ";无权改 ACL 时请重新输入密钥保存到受管位置)"));
    }
#else
    if (::chmod(effective.c_str(), 0600) != 0) {
        return std::unexpected(Fail(
            "credential_store_tighten_failed",
            std::string("chmod 0600 失败: ") + std::strerror(errno)));
    }
#endif
    // 复验:收紧后必须过生产读取器,过不了就如实报(不动内容)。
    const auto after = CheckCredentialFileSecurity(effective);
    if (!after.has_value()) {
        return std::unexpected(
            Fail("credential_store_tighten_failed",
                 "收紧后复验仍不过: " + after.error().reason + ": " + after.error().detail));
    }
    return {};
}

std::expected<std::vector<std::filesystem::path>, CredentialStoreError>
CredentialStore::RemoveUnreferencedManagedSecrets(
    const std::vector<std::filesystem::path>& keep) const {
    std::set<std::filesystem::path> keep_set;
    for (const auto& path : keep) {
        std::error_code ec;
        keep_set.insert(std::filesystem::weakly_canonical(path, ec));
    }
    std::vector<std::filesystem::path> removed;
    std::error_code iter_ec;
    std::filesystem::directory_iterator it(secrets_root_, iter_ec);
    if (iter_ec) {
        if (!std::filesystem::exists(secrets_root_, iter_ec)) {
            return removed;  // 根不在:无孤儿可言
        }
        return std::unexpected(Fail("credential_store_cleanup_failed",
                                    "扫受管目录失败: " + platform::PathToUtf8(secrets_root_)));
    }
    for (const auto& entry : it) {
        std::error_code entry_ec;
        if (entry.is_regular_file(entry_ec) && entry.path().extension() == ".secret") {
            std::error_code canon_ec;
            const auto canonical = std::filesystem::weakly_canonical(entry.path(), canon_ec);
            const std::filesystem::path key = canon_ec ? entry.path() : canonical;
            if (keep_set.count(key) != 0) {
                continue;
            }
            std::error_code remove_ec;
            if (std::filesystem::remove(entry.path(), remove_ec)) {
                removed.push_back(entry.path());
            } else if (remove_ec) {
                return std::unexpected(Fail("credential_store_cleanup_failed",
                                            "删孤儿受管件失败: " + platform::PathToUtf8(entry.path()) +
                                                ": " + remove_ec.message()));
            }
        }
    }
    return removed;
}

}  // namespace lubancode::channel
