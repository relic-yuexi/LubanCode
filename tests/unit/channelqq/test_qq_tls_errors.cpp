// QQ TLS 稳定错误码册(Windows 信任根单 §四 P0-B):mbedTLS 验证 flags /
// 握手错误码的稳定映射、信任根解析(explicit 优先不回退、无效明报、探测
// seam 只归 Unix 路)。windows-msvc 腿(main push)会真跑
// ExportWindowsTrustRoots——空显式两案按平台分叉断言,兼作店导出的 CI
// 冒烟(不崩 + 数账)。SSL 策略校验回调要真 TLS 握手,CI 验不了——单内
// 如实标"未验",归 Q3 Windows 真机复测。
#include <doctest/doctest.h>

#include <mbedtls/ssl.h>       // MBEDTLS_ERR_SSL_TIMEOUT 等握手错误码
#include <mbedtls/x509_crt.h>  // MBEDTLS_X509_BADCERT_* 验证 flags

#include <string>

#include "channel/qq/qq_tls.hpp"

namespace lubancode::channel::qq {
namespace {
// 说明:合法 PEM 的解析计数路径由 test_qq_ws_client.cpp 的真握手用例覆
// 盖(自签 CA 注入 + 信任锚不匹配拒握手);本册钉纯映射与解析决策。
}  // namespace

TEST_CASE("qq_tls: 验证 flags -> 稳定码映射(主机名>过期>不信任>兜底)") {
    CHECK(TlsVerifyFlagsToCode(MBEDTLS_X509_BADCERT_CN_MISMATCH) == kTlsCodeCertHostnameMismatch);
    CHECK(TlsVerifyFlagsToCode(MBEDTLS_X509_BADCERT_EXPIRED) == kTlsCodeCertExpired);
    CHECK(TlsVerifyFlagsToCode(MBEDTLS_X509_BADCERT_NOT_TRUSTED) == kTlsCodeCertNotTrusted);
    CHECK(TlsVerifyFlagsToCode(MBEDTLS_X509_BADCERT_OTHER) == kTlsCodeCertVerifyFailed);
    // 组合位:优先级取最要紧的(主机名 > 过期 > 不信任)。
    CHECK(TlsVerifyFlagsToCode(MBEDTLS_X509_BADCERT_CN_MISMATCH |
                               MBEDTLS_X509_BADCERT_NOT_TRUSTED) ==
          kTlsCodeCertHostnameMismatch);
    CHECK(TlsVerifyFlagsToCode(MBEDTLS_X509_BADCERT_EXPIRED |
                               MBEDTLS_X509_BADCERT_NOT_TRUSTED) == kTlsCodeCertExpired);
    CHECK(TlsVerifyFlagsToCode(0) == kTlsCodeCertVerifyFailed);
}

TEST_CASE("qq_tls: 握手错误码映射——超时独立成码,其余归 handshake_failed") {
    CHECK(TlsHandshakeCodeToCode(MBEDTLS_ERR_SSL_TIMEOUT) == kTlsCodeHandshakeTimeout);
    CHECK(TlsHandshakeCodeToCode(MBEDTLS_ERR_SSL_FATAL_ALERT_MESSAGE) ==
          kTlsCodeHandshakeFailed);
    CHECK(TlsHandshakeCodeToCode(0) == kTlsCodeHandshakeFailed);
}

TEST_CASE("qq_tls: ResolveChannelTrustRoots——explicit 优先且不回退,无效明报") {
    // 非法 PEM(非空但解析失败):source 仍 explicit,error 非空(明报,不
    // 静默退回平台来源——§四);报 load_failed 不报 empty(§三:非空解析
    // 失败带真实负码)。
    const ResolvedTrustStore bad = ResolveChannelTrustRoots("not a pem at all");
    CHECK(bad.source == "explicit");
    CHECK(bad.certificate_count == 0);
    CHECK_FALSE(bad.error.empty());
    CHECK(bad.ca_pem.empty());  // 无效锚不喂 mbedTLS(连接报 load_failed)
    CHECK(bad.load.parse_rc < 0);  // 纯文本垃圾:整批负错误码
    CHECK(bad.load.first_negative_rc < 0);

    // 探测 seam:显式锚给定时,探测函数不该被调用。
    bool detect_called = false;
    const ResolvedTrustStore explicit_kept =
        ResolveChannelTrustRoots("garbage", [&detect_called]() {
            detect_called = true;
            return std::string("/nonexistent/path.pem");
        });
    CHECK(explicit_kept.source == "explicit");
    CHECK_FALSE(detect_called);
}

TEST_CASE("qq_tls: ResolveChannelTrustRoots——空显式走平台默认(探测 seam 只归 Unix 路)") {
    bool detect_called = false;
    const ResolvedTrustStore resolved = ResolveChannelTrustRoots("", [&detect_called]() {
        detect_called = true;
        return std::string();  // 模拟 Unix 路径全探测不到
    });
#ifdef _WIN32
    // Windows 的平台默认 = 系统证书店导出(探测 seam 不参与):真店非空,
    // 必给得出信任根——这正是 CI windows 腿上真跑 ExportWindowsTrustRoots
    // 的路径(枚举生命周期修订后在此冒烟)。
    CHECK_FALSE(detect_called);
    CHECK(resolved.source == "windows_system_store");
    CHECK(resolved.certificate_count > 0);
    CHECK(resolved.ca_pem.find("BEGIN CERTIFICATE") != std::string::npos);
    CHECK(resolved.error.empty());
#else
    CHECK(detect_called);
    CHECK(resolved.source == "none");
    CHECK(resolved.ca_pem.empty());
    CHECK_FALSE(resolved.error.empty());
#endif
}

TEST_CASE("qq_tls: ResolveChannelTrustRoots——探测到但文件打不开/读不懂明报") {
    const ResolvedTrustStore unreadable = ResolveChannelTrustRoots("", []() {
        return std::string("/definitely/not/here/ca.pem");
    });
#ifdef _WIN32
    // Windows 不走文件探测:注入的路径不参与,结果与系统店导出同上一案。
    CHECK(unreadable.source == "windows_system_store");
    CHECK(unreadable.certificate_count > 0);
#else
    CHECK(unreadable.source == "none");
    CHECK_FALSE(unreadable.error.empty());
#endif
}

TEST_CASE("qq_tls: DetectSystemCaPemPath 在本进程不崩(返回值不定,只验调用)") {
    const std::string path = DetectSystemCaPemPath();
    CHECK(path.size() <= 260);  // 没有异常/崩溃即过;具体值随环境
}

}  // namespace lubancode::channel::qq
