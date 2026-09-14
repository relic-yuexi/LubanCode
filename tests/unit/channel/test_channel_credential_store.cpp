// 统一凭据写入服务册(QQBot Windows 修复单 §5.2/§5.3)。
//
// 钉的合同:
//   - 新建受管文件生来合格:POSIX 0600/owner;Windows owner=当前用户、
//     DACL 关继承且仅当前用户(经生产读取器 CheckCredentialFileSecurity
//     复验,不信本件自说自话);
//   - 收紧:权限过宽的文件收紧后过生产读取器;owner 不符不动手、准确报错;
//   - 孤儿回收只动 *.secret 命名,不碰外部文件;
//   - 写路径拒收符号链接/重解析点;中文/空格路径可用;
//   - 并发两路写同一账号:锁序化,配置最终指向其一且可读。
//
// 未验边界(如实记账,单内 §七):
//   - Windows 重解析点/junction 的拒收路径:CI windows 腿上创建符号链接
//     须开发者模式,不做硬依赖;POSIX 符号链接拒收在此册覆盖;
//   - owner 不符的真文件:普通用户改不了 owner(chown/SetNamedSecurityInfo
//     都要特权),该路径靠"分类器拒动"分支的稳定码断言覆盖,不造真文件。
#include <doctest/doctest.h>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <set>
#include <string>
#include <thread>
#include <vector>

#ifndef _WIN32
#include <sys/stat.h>
#include <unistd.h>
#else
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <aclapi.h>  // SetNamedSecurityInfoW(放宽 DACL 用例)
#include <sddl.h>    // ConvertStringSecurityDescriptorToSecurityDescriptorW
#endif

#include "channel/credential_store.hpp"
#include "channel/credentials.hpp"
#include "platform/paths.hpp"

using namespace lubancode::channel;

namespace {

std::filesystem::path MakeTempDir(const char* tag) {
    const auto dir = std::filesystem::temp_directory_path() /
                     ("lubancode_credstore_" + std::string(tag) + "_" +
                      std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    std::filesystem::create_directories(dir, ec);
    return dir;
}

std::string ToUtf8(const std::filesystem::path& path) { return lubancode::platform::PathToUtf8(path); }

std::string ReadAllBytes(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

ChannelAccountUserConfig AccountWithFile(const std::filesystem::path& path) {
    ChannelAccountUserConfig account;
    account.secret_file = ToUtf8(path);
    return account;
}

}  // namespace

TEST_CASE("WriteNewManagedSecret:生来合格,生产读取器可读") {
    const auto dir = MakeTempDir("create");
    CredentialStore store(dir / "secrets");

    const std::string secret = "test-secret-ABC123";
    const auto written = store.WriteNewManagedSecret("qqbot", "main", secret);
    REQUIRE(written.has_value());

    // 文件落受管根下,名字带账号前缀与版本段。
    const std::string name = written->filename().string();
    CHECK(name.rfind("qqbot-main.", 0) == 0);
    CHECK(written->extension() == ".secret");
    CHECK(written->parent_path() == dir / "secrets");

    // 内容逐字节。
    CHECK(ReadAllBytes(*written) == secret);

    // 权限:生产读取器复验(POSIX 0600+owner;Windows owner/DACL)。
    const auto check = CheckCredentialFileSecurity(*written);
    CHECK(check.has_value());

    // 分类器口径一致。
    const auto report = InspectCredentialFileSecurity(*written);
    CHECK(report.status == CredentialFileSecurityReport::Status::Ok);

    // resolver 全链(绝对路径+安全+内容)。
    const auto resolved = ResolveChannelCredential(AccountWithFile(*written));
    REQUIRE(resolved.has_value());
    CHECK(resolved->secret == secret);
    CHECK(resolved->source == ResolvedChannelCredential::Source::File);

#ifndef _WIN32
    // POSIX 直接看位:0600。
    struct stat info {};
    REQUIRE(::stat(written->c_str(), &info) == 0);
    CHECK((info.st_mode & 0777) == 0600);
    CHECK(static_cast<unsigned long>(info.st_uid) == static_cast<unsigned long>(::geteuid()));
    // 目录 0700。
    struct stat dir_info {};
    REQUIRE(::stat((dir / "secrets").c_str(), &dir_info) == 0);
    CHECK((dir_info.st_mode & 0777) == 0700);
#endif
}

TEST_CASE("WriteNewManagedSecret:中文/空格路径可用") {
    // §七"中文/空格路径"用例:路径里带 CJK 与空格,整链照走。
    const auto dir = MakeTempDir("路径 空格");
    CredentialStore store(dir / "secrets 目录");
    const auto written = store.WriteNewManagedSecret("qqbot", "main", "secret-value");
    REQUIRE(written.has_value());
    CHECK(CheckCredentialFileSecurity(*written).has_value());
    const auto resolved = ResolveChannelCredential(AccountWithFile(*written));
    REQUIRE(resolved.has_value());
    CHECK(resolved->secret == "secret-value");
}

TEST_CASE("WriteNewManagedSecret:坏 id/坏内容拒收,错误不带内容") {
    const auto dir = MakeTempDir("reject");
    CredentialStore store(dir / "secrets");

    SUBCASE("账号 id 带路径分隔符") {
        const auto written = store.WriteNewManagedSecret("qqbot", "a/b", "x");
        REQUIRE_FALSE(written.has_value());
        CHECK(written.error().reason == "credential_store_bad_id");
    }
    SUBCASE("账号 id 是 ..") {
        const auto written = store.WriteNewManagedSecret("qqbot", "..", "x");
        REQUIRE_FALSE(written.has_value());
        CHECK(written.error().reason == "credential_store_bad_id");
    }
    SUBCASE("空密钥") {
        const auto written = store.WriteNewManagedSecret("qqbot", "main", "");
        REQUIRE_FALSE(written.has_value());
        CHECK(written.error().reason == "credential_store_write_failed");
        CHECK(written.error().detail.find("为空") != std::string::npos);
    }
    SUBCASE("控制字符") {
        const auto written = store.WriteNewManagedSecret("qqbot", "main", "abc\ndef");
        REQUIRE_FALSE(written.has_value());
        CHECK(written.error().reason == "credential_store_write_failed");
        // 错误文案绝不引用密钥内容。
        CHECK(written.error().detail.find("abc") == std::string::npos);
    }
}

TEST_CASE("写路径拒收符号链接(POSIX 实测;Windows 重解析点同款判据见 secure_file)") {
#ifndef _WIN32
    const auto dir = MakeTempDir("symlink");
    CredentialStore store(dir / "secrets");
    // 先建真受管根,再把同名目录换不上(目录已存在)——换成对"根是链接"
    // 的场景:secrets 指到别处。
    const auto real = dir / "real-secrets";
    std::filesystem::create_directories(real);
    std::filesystem::create_directory_symlink(real, dir / "linked-secrets");
    CredentialStore linked(dir / "linked-secrets");
    const auto written = linked.WriteNewManagedSecret("qqbot", "main", "value");
    REQUIRE_FALSE(written.has_value());
    CHECK(written.error().reason == "credential_store_dir_failed");
#else
    // Windows:创建符号链接要特权/开发者模式,CI 不硬依赖;拒收判据
    //(RejectReparsePoint 的 FILE_ATTRIBUTE_REPARSE_POINT)与 POSIX 同款
    // 语义,此处不造假证据。
    INFO("Windows 重解析点拒收:未在本册跑(CI 不保证开发者模式),POSIX 腿覆盖同款判据");
#endif
}

TEST_CASE("TightenFilePermissions:过宽文件收紧后过生产读取器") {
    const auto dir = MakeTempDir("tighten");
    CredentialStore store(dir / "secrets");
    const auto written = store.WriteNewManagedSecret("qqbot", "main", "tighten-value");
    REQUIRE(written.has_value());

    // 放宽(POSIX 0644;Windows 加 Everyone 全权 ACE),分类器须报过宽。
#ifndef _WIN32
    REQUIRE(::chmod(written->c_str(), 0644) == 0);
    const auto wide = InspectCredentialFileSecurity(*written);
    REQUIRE(wide.status == CredentialFileSecurityReport::Status::DaclTooWide);
#else
    {
        // SDDL D:P(A;;GA;;;WD):Everyone 全权 + 关继承。
        PSECURITY_DESCRIPTOR sd = nullptr;
        REQUIRE(ConvertStringSecurityDescriptorToSecurityDescriptorW(
            L"D:P(A;;GA;;;WD)", SDDL_REVISION_1, &sd, nullptr));
        PACL dacl = nullptr;
        BOOL present = FALSE, defaulted = FALSE;
        const BOOL got = GetSecurityDescriptorDacl(sd, &present, &dacl, &defaulted);
        // doctest 在 MSVC 分解不动 && 链(Expression Too Complex)——拆成单目断言
        REQUIRE(got);
        REQUIRE(present);
        REQUIRE(dacl != nullptr);
        REQUIRE(SetNamedSecurityInfoW(const_cast<LPWSTR>(written->wstring().c_str()),
                                      SE_FILE_OBJECT, DACL_SECURITY_INFORMATION |
                                              PROTECTED_DACL_SECURITY_INFORMATION,
                                      nullptr, nullptr, dacl, nullptr) == ERROR_SUCCESS);
        LocalFree(sd);
    }
    const auto wide = InspectCredentialFileSecurity(*written);
    REQUIRE(wide.status == CredentialFileSecurityReport::Status::DaclTooWide);
#endif

    // 收紧 → 复验过;内容不动。
    const auto tightened = store.TightenFilePermissions(*written);
    REQUIRE(tightened.has_value());
    CHECK(CheckCredentialFileSecurity(*written).has_value());
    CHECK(ReadAllBytes(*written) == "tighten-value");

    // 幂等:已合格再收紧照样成功。
    CHECK(store.TightenFilePermissions(*written).has_value());
}

TEST_CASE("TightenFilePermissions:分类器拒动档(不存在的文件如实报,不造 owner 不符真件)") {
    const auto dir = MakeTempDir("tighten2");
    CredentialStore store(dir / "secrets");
    // owner 不符的真文件普通用户造不出来(chown 要特权);用"描述符读不出"
    // 的不存在路径钉"分类失败 → 不动手 → 稳定码"的分支形状。
    const auto tightened = store.TightenFilePermissions(dir / "ghost.secret");
    REQUIRE_FALSE(tightened.has_value());
    CHECK(tightened.error().reason == "credential_store_tighten_failed");
    CHECK(tightened.error().detail.find("ghost.secret") == std::string::npos);  // detail 只报原因
}

TEST_CASE("RemoveUnreferencedManagedSecrets:只清 *.secret 孤儿,外部文件不碰") {
    const auto dir = MakeTempDir("orphan");
    CredentialStore store(dir / "secrets");
    const auto kept = store.WriteNewManagedSecret("qqbot", "main", "kept-value");
    const auto orphan = store.WriteNewManagedSecret("qqbot", "main", "old-value");
    REQUIRE(kept.has_value());
    REQUIRE(orphan.has_value());
    // 同目录放一枚非 .secret 命名的文件:不许被回收。
    const auto bystander = dir / "secrets" / "readme.txt";
    { std::ofstream out(bystander, std::ios::binary); out << "not a secret"; }

    const auto removed = store.RemoveUnreferencedManagedSecrets({*kept});
    REQUIRE(removed.has_value());
    CHECK(removed->size() == 1);
    CHECK((*removed)[0] == *orphan);
    CHECK(std::filesystem::exists(*kept));
    CHECK_FALSE(std::filesystem::exists(*orphan));
    CHECK(std::filesystem::exists(bystander));
}

TEST_CASE("并发两路写同一账号:配置可指向其一,其余成孤儿可回收") {
    const auto dir = MakeTempDir("race");
    CredentialStore store(dir / "secrets");
    std::atomic<int> failures{0};
    std::vector<std::filesystem::path> paths[2];
    std::thread workers[2];
    for (int i = 0; i < 2; ++i) {
        workers[i] = std::thread([&, i]() {
            const auto written = store.WriteNewManagedSecret("qqbot", "main",
                                                             "secret-from-" + std::to_string(i));
            if (written.has_value()) {
                paths[i].push_back(*written);
            } else {
                ++failures;
            }
        });
    }
    for (auto& worker : workers) {
        worker.join();
    }
    REQUIRE(failures.load() == 0);
    // 两枚文件都生来合格(各自 CREATE_NEW,名字唯一)。
    for (int i = 0; i < 2; ++i) {
        REQUIRE(paths[i].size() == 1);
        CHECK(CheckCredentialFileSecurity(paths[i][0]).has_value());
    }
    // 留一枚,另一枚按孤儿回收(与 ChannelConfigService 提交后的动作同款)。
    const auto removed = store.RemoveUnreferencedManagedSecrets({paths[0][0]});
    REQUIRE(removed.has_value());
    CHECK(removed->size() == 1);
    CHECK((*removed)[0] == paths[1][0]);
}
