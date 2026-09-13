// 渠道凭据 resolver 册(QQ 机器人接入单 Q0)。
// 真源:docs/architecture/channels/configuration.md §4(来源三种、优先级、
// secret_file 读取规矩)与 security.md §6(泄露禁令)。
//
// 钉的合同:
//   - 优先级 secret_file > secret_env > inline;高优先级配置了但无效时
//     明报稳定码,不静默降级;
//   - secret_file:绝对路径、常规文件、归属当前用户、无组/其他读写位
//     (POSIX chmod / Windows DACL)、上限 8 KiB、原值 + 至多剥末尾换行、
//     拒空值与内部控制字符;
//   - 错误文案不带密钥值(凭据不可出日志)。
#include <doctest/doctest.h>

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <stdlib.h>  // setenv/unsetenv/_putenv_s(平台各取所需)
#include <string>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <stdlib.h>  // _putenv_s/_dupenv_s 同源
#include <windows.h>
#include <sddl.h>
#else
#include <sys/stat.h>
#endif

#include "channel/credentials.hpp"

using namespace lubancode::channel;

namespace {

std::filesystem::path MakeTempDir(const char* tag) {
    const auto dir = std::filesystem::temp_directory_path() /
                     ("lubancode_cred_" + std::string(tag) + "_" +
                      std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    std::filesystem::create_directories(dir, ec);
    return dir;
}

// 建一枚"当前用户私有"的密钥文件:POSIX 0600;Windows 默认 DACL
//(当前用户继承)。
std::filesystem::path WriteSecretFile(const std::filesystem::path& dir, const char* tag,
                                      const std::string& content) {
    const auto path = dir / (std::string(tag) + ".key");
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    file << content;
    file.close();
#ifndef _WIN32
    ::chmod(path.c_str(), 0600);
#endif
    return path;
}

std::string ToUtf8(const std::filesystem::path& path) {
    const std::u8string u8 = path.u8string();
    return std::string(reinterpret_cast<const char*>(u8.data()), u8.size());
}

ChannelAccountUserConfig AccountWithFile(const std::filesystem::path& path) {
    ChannelAccountUserConfig account;
    account.secret_file = ToUtf8(path);
    return account;
}

}  // namespace

TEST_CASE("secret_file 正常路:原值保留,至多剥末尾一处换行") {
    const auto dir = MakeTempDir("ok");
    const std::string secret = "AppSecret-ABC123xyz!@#";

    SUBCASE("无换行:原值一字不动") {
        const auto path = WriteSecretFile(dir, "plain", secret);
        const auto resolved = ResolveChannelCredential(AccountWithFile(path));
        REQUIRE(resolved.has_value());
        CHECK(resolved->secret == secret);
        CHECK(resolved->source == ResolvedChannelCredential::Source::File);
    }
    SUBCASE("末尾一个 \\n:剥掉") {
        const auto path = WriteSecretFile(dir, "lf", secret + "\n");
        const auto resolved = ResolveChannelCredential(AccountWithFile(path));
        REQUIRE(resolved.has_value());
        CHECK(resolved->secret == secret);
    }
    SUBCASE("末尾 \\r\\n:整处剥掉") {
        const auto path = WriteSecretFile(dir, "crlf", secret + "\r\n");
        const auto resolved = ResolveChannelCredential(AccountWithFile(path));
        REQUIRE(resolved.has_value());
        CHECK(resolved->secret == secret);
    }
}

TEST_CASE("secret_file 拒收清单:稳定码逐项") {
    const auto dir = MakeTempDir("reject");
    ChannelAccountUserConfig account;

    SUBCASE("相对路径") {
        account.secret_file = "relative/qq.key";
        const auto resolved = ResolveChannelCredential(account);
        REQUIRE_FALSE(resolved.has_value());
        CHECK(resolved.error().reason == "secret_file_relative");
    }
    SUBCASE("文件不存在") {
        account = AccountWithFile(dir / "ghost.key");
        const auto resolved = ResolveChannelCredential(account);
        REQUIRE_FALSE(resolved.has_value());
        CHECK(resolved.error().reason == "secret_file_missing");
    }
    SUBCASE("目录不是常规文件") {
        account = AccountWithFile(dir);
        const auto resolved = ResolveChannelCredential(account);
        REQUIRE_FALSE(resolved.has_value());
        CHECK(resolved.error().reason == "secret_file_not_regular");
    }
    SUBCASE("空值(剥换行后为空)") {
        const auto path = WriteSecretFile(dir, "empty", "\n");
        const auto resolved = ResolveChannelCredential(AccountWithFile(path));
        REQUIRE_FALSE(resolved.has_value());
        CHECK(resolved.error().reason == "secret_file_empty");
    }
    SUBCASE("内部控制字符(换行在中间)") {
        const auto path = WriteSecretFile(dir, "ctrl", "abc\ndef");
        const auto resolved = ResolveChannelCredential(AccountWithFile(path));
        REQUIRE_FALSE(resolved.has_value());
        CHECK(resolved.error().reason == "secret_file_bad_content");
    }
    SUBCASE("超限") {
        const auto path = WriteSecretFile(dir, "huge", std::string(kCredentialFileMaxBytes + 1, 'x'));
        const auto resolved = ResolveChannelCredential(AccountWithFile(path));
        REQUIRE_FALSE(resolved.has_value());
        CHECK(resolved.error().reason == "secret_file_too_large");
    }
}

TEST_CASE("secret_file 不安全文件:组/其他可读拒收(POSIX)") {
#ifndef _WIN32
    const auto dir = MakeTempDir("perms");
    ChannelAccountUserConfig account;
    SUBCASE("0644(其他可读)") {
        const auto path = WriteSecretFile(dir, "world", "s3cret-value");
        ::chmod(path.c_str(), 0644);
        account = AccountWithFile(path);
        const auto resolved = ResolveChannelCredential(account);
        REQUIRE_FALSE(resolved.has_value());
        CHECK(resolved.error().reason == "secret_file_insecure");
        CHECK(resolved.error().detail.find("s3cret-value") == std::string::npos);
    }
    SUBCASE("0660(组可读写)") {
        const auto path = WriteSecretFile(dir, "group", "s3cret-value");
        ::chmod(path.c_str(), 0660);
        account = AccountWithFile(path);
        const auto resolved = ResolveChannelCredential(account);
        REQUIRE_FALSE(resolved.has_value());
        CHECK(resolved.error().reason == "secret_file_insecure");
    }
    SUBCASE("0600 放行") {
        const auto path = WriteSecretFile(dir, "tight", "good-value");
        ::chmod(path.c_str(), 0600);
        const auto resolved = ResolveChannelCredential(AccountWithFile(path));
        REQUIRE(resolved.has_value());
        CHECK(resolved->secret == "good-value");
    }
#else
    // Windows 侧:DACL 放行 Everyone 的文件拒收。
    const auto dir = MakeTempDir("dacl");
    const auto path = WriteSecretFile(dir, "everyone", "s3cret-value");
    // 拼一条 D:P(A;;GA;;;WD) —— WD = Everyone 全权。
    PSECURITY_DESCRIPTOR sd = nullptr;
    if (ConvertStringSecurityDescriptorToSecurityDescriptorW(
            L"D:P(A;;GA;;;WD)", SDDL_REVISION_1, &sd, nullptr)) {
        PACL dacl = nullptr;
        BOOL present = FALSE, defaulted = FALSE;
        if (GetSecurityDescriptorDacl(sd, &present, &dacl, &defaulted) && present &&
            SetNamedSecurityInfoW(const_cast<LPWSTR>(path.wstring().c_str()), SE_FILE_OBJECT,
                                  DACL_SECURITY_INFORMATION, nullptr, nullptr, dacl,
                                  nullptr) == ERROR_SUCCESS) {
            const auto resolved = ResolveChannelCredential(AccountWithFile(path));
            REQUIRE_FALSE(resolved.has_value());
            CHECK(resolved.error().reason == "secret_file_insecure");
            CHECK(resolved.error().detail.find("s3cret-value") == std::string::npos);
            LocalFree(sd);
            return;
        }
        LocalFree(sd);
    }
    // 拿不到权限改 DACL(异常环境):至少默认 DACL 的正常路要过。
    const auto clean = WriteSecretFile(dir, "clean", "good-value");
    const auto resolved = ResolveChannelCredential(AccountWithFile(clean));
    REQUIRE(resolved.has_value());
    CHECK(resolved->secret == "good-value");
#endif
}

TEST_CASE("来源优先级:file > env > inline;高优先级无效明报不降级") {
    const auto dir = MakeTempDir("prio");
    const auto good = WriteSecretFile(dir, "good", "from-file-value");

    SUBCASE("env 单独配:取得到就用") {
        ChannelAccountUserConfig account;
        account.secret_env = "LUBANCODE_TEST_QQ_SECRET_OK";
#ifdef _WIN32
        REQUIRE(_putenv_s("LUBANCODE_TEST_QQ_SECRET_OK", "from-env-value") == 0);
#else
        REQUIRE(::setenv("LUBANCODE_TEST_QQ_SECRET_OK", "from-env-value", 1) == 0);
#endif
        const auto resolved = ResolveChannelCredential(account);
        REQUIRE(resolved.has_value());
        CHECK(resolved->secret == "from-env-value");
        CHECK(resolved->source == ResolvedChannelCredential::Source::Env);
    }
    SUBCASE("env 没设:稳定码明报") {
        ChannelAccountUserConfig account;
        account.secret_env = "LUBANCODE_TEST_QQ_SECRET_ABSENT";
#ifdef _WIN32
        _putenv_s("LUBANCODE_TEST_QQ_SECRET_ABSENT", "");
#else
        ::unsetenv("LUBANCODE_TEST_QQ_SECRET_ABSENT");
#endif
        const auto resolved = ResolveChannelCredential(account);
        REQUIRE_FALSE(resolved.has_value());
        CHECK(resolved.error().reason == "secret_env_missing");
    }
    SUBCASE("inline 兼容路") {
        ChannelAccountUserConfig account;
        account.secret = "plain-inline";
        const auto resolved = ResolveChannelCredential(account);
        REQUIRE(resolved.has_value());
        CHECK(resolved->secret == "plain-inline");
        CHECK(resolved->source == ResolvedChannelCredential::Source::InlinePlaintext);
    }
    SUBCASE("file 配了但文件不存在:明报,不落回 env/inline") {
        ChannelAccountUserConfig account;
        account.secret_file = ToUtf8(dir / "ghost.key");
        account.secret_env = "LUBANCODE_TEST_QQ_SECRET_OK";
        account.secret = "plain-inline";
        const auto resolved = ResolveChannelCredential(account);
        REQUIRE_FALSE(resolved.has_value());
        CHECK(resolved.error().reason == "secret_file_missing");
        // 错误文案只有路径与稳定码,没有另两个来源的值。
        CHECK(resolved.error().detail.find("from-env-value") == std::string::npos);
        CHECK(resolved.error().detail.find("plain-inline") == std::string::npos);
    }
    SUBCASE("file 与 env 并配:file 赢") {
        ChannelAccountUserConfig account;
        account.secret_file = ToUtf8(good);
        account.secret_env = "LUBANCODE_TEST_QQ_SECRET_OK";
        const auto resolved = ResolveChannelCredential(account);
        REQUIRE(resolved.has_value());
        CHECK(resolved->secret == "from-file-value");
    }
    SUBCASE("三种都没配") {
        ChannelAccountUserConfig account;
        const auto resolved = ResolveChannelCredential(account);
        REQUIRE_FALSE(resolved.has_value());
        CHECK(resolved.error().reason == "credential_missing");
    }
}

TEST_CASE("泄露禁令:错误文案与 RedactSecret 都不带密钥值") {
    const auto dir = MakeTempDir("leak");
    const std::string secret = "SUPER-SECRET-XYZ";
    const auto path = WriteSecretFile(dir, "leaky", secret);

    // 一切失败路径的错误文案拼起来不许出现密钥值。用"内容含控制字符"
    // 的文件制造失败:值本身不会进文案,但换行变体可能露——构造一个
    // 前半截等于密钥的坏文件。
    const auto bad = WriteSecretFile(dir, "bad", secret + "\ninner\n");
    const auto resolved = ResolveChannelCredential(AccountWithFile(bad));
    REQUIRE_FALSE(resolved.has_value());
    CHECK(resolved.error().reason == "secret_file_bad_content");
    CHECK(resolved.error().detail.find(secret) == std::string::npos);

    // 防御性脱敏:文本里撞上已知密钥值就换 <redacted>。
    const std::string log_line = "resolve failed for secret [" + secret + "] at " + ToUtf8(path);
    const std::string redacted = RedactSecret(log_line, secret);
    CHECK(redacted.find(secret) == std::string::npos);
    CHECK(redacted.find("<redacted>") != std::string::npos);
    CHECK(redacted.find(ToUtf8(path)) != std::string::npos);  // 路径留着
    // 空密钥:原样返回。
    CHECK(RedactSecret(log_line, "") == log_line);
}

TEST_CASE("sidecar 环境白名单:宿主模型 key 一类不在名单") {
    const auto& allowlist = SidecarEnvAllowlist();
    REQUIRE_FALSE(allowlist.empty());
    bool has_path = false;
    bool has_model_key = false;
    for (const std::string& name : allowlist) {
        if (name == "PATH") has_path = true;
        if (name.find("API_KEY") != std::string::npos ||
            name.find("SECRET") != std::string::npos) {
            has_model_key = true;
        }
    }
    CHECK(has_path);
    CHECK_FALSE(has_model_key);  // 白名单里不该出现任何密钥变量
}
