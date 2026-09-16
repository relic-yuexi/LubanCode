// 信任根加载合同册(证书部分解析误判修复单 §三/§六):mbedTLS v3.6.3
// 批量 parse 合同(0=全成/正数=失败张数且成功链保留/负数=错误)的钉与
// 修后行为——混合集合(合法+带完整 PEM 边界的坏 DER)不再整批判死,系统
// 模式保留成功链;显式锚严格拒绝;空输入才报 empty;非空解析失败报
// load_failed 带真实返回码。混合集合还要过真 TLS 本地握手(合法主机成/
// 错主机名/不信任/过期败)。Windows 真机"导出→解析报告→真 TLS"归 Q3
// 真机复测,此处 windows 腿只冒烟系统店导出与非空路径。
#include <doctest/doctest.h>

#include <mbedtls/x509_crt.h>

#include <cstdio>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

#include "channel/qq/qq_socket.hpp"
#include "channel/qq/qq_tls.hpp"
#include "mock_ws_server.hpp"
#include "platform/base64.hpp"

namespace lubancode::channel::qq {
namespace {

// ---------------------------------------------------------------------------
// 夹具:合法证书(进程内生成)与"完整 PEM 边界 + 坏 DER"证书(现场形状:
// Windows 导出 157 张里一张 mbedTLS 不认的旧格式证)。
// ---------------------------------------------------------------------------

std::string BadPemCertificate() {
    // 完整 PEM 边界,base64 载荷是垃圾字节——不是证书。批量 parse 时
    // mbedTLS 跳过它并计失败(返回正数),其余成功项保留在链上。
    static const char kJunk[] = "\x01not-a-cert-der-junk-payload-!!";
    const std::string encoded =
        platform::Base64Encode(std::string_view(kJunk, sizeof(kJunk) - 1));
    std::string pem = "-----BEGIN CERTIFICATE-----\n";
    for (std::size_t i = 0; i < encoded.size(); i += 64) {
        pem += encoded.substr(i, 64);
        pem += "\n";
    }
    pem += "-----END CERTIFICATE-----\n";
    return pem;
}

// 链上张数(与 qq_tls.cpp 内部同口径;这里直钉 mbedTLS 行为)。
int ChainSize(const mbedtls_x509_crt* chain) {
    int count = 0;
    for (const mbedtls_x509_crt* it = chain; it != nullptr; it = it->next) {
        if (it->raw.len > 0) {
            ++count;
        }
    }
    return count;
}

}  // namespace

// ---------------------------------------------------------------------------
// §六第一刀:先钉 pinned mbedTLS 的批量合同本身——修的是对它的误读,
// 合同钉死了,后面的策略断言才有锚。
// ---------------------------------------------------------------------------

TEST_CASE("qq_tls_trust_load: 钉 mbedTLS 批量 parse 合同——混合集合返回正数、成功链非空") {
    const auto good = test_support::GenerateSelfSignedCert();
    REQUIRE(good.has_value());
    const std::string mixed = good->ca_pem + BadPemCertificate();

    mbedtls_x509_crt chain;
    mbedtls_x509_crt_init(&chain);
    const int rc = mbedtls_x509_crt_parse(&chain,
                                          reinterpret_cast<const unsigned char*>(mixed.data()),
                                          mixed.size() + 1);
    const int parsed = ChainSize(&chain);
    mbedtls_x509_crt_free(&chain);
    // 合同:正数 = 未解析成功的证书数量(此处 1 张坏证),且已有成功项
    // 留在链上(合法证)。旧代码把任何非零折叠成 -1 再报 empty,正是
    // 现场误报的根。
    CHECK(rc > 0);
    CHECK(rc == 1);
    CHECK(parsed == 1);
}

TEST_CASE("qq_tls_trust_load: EvaluateTrustLoad——空输入与纯垃圾(负码)") {
    const TrustLoadReport empty = EvaluateTrustLoad("system_pem", "");
    CHECK(empty.input_empty);
    CHECK_FALSE(empty.ok_to_continue);
    CHECK_FALSE(empty.error.empty());

    const TrustLoadReport garbage = EvaluateTrustLoad("system_pem", "not a pem at all");
    CHECK_FALSE(garbage.input_empty);
    CHECK_FALSE(garbage.ok_to_continue);
    CHECK(garbage.parse_rc < 0);  // 无 PEM 边界:整批负错误码
    CHECK(garbage.first_negative_rc < 0);
    CHECK_FALSE(garbage.error.empty());
}

TEST_CASE("qq_tls_trust_load: EvaluateTrustLoad——全合法集合全成,无警告") {
    const auto good = test_support::GenerateSelfSignedCert();
    REQUIRE(good.has_value());
    const TrustLoadReport report = EvaluateTrustLoad("system_pem", good->ca_pem);
    CHECK(report.ok_to_continue);
    CHECK_FALSE(report.partial);
    CHECK(report.parse_rc == 0);
    CHECK(report.parsed_count == 1);
    CHECK(report.failed_count == 0);
    CHECK(report.unique_count == 1);
    CHECK(report.warning.empty());
    CHECK(report.error.empty());
    CHECK(report.bad_cert_notes.empty());
}

TEST_CASE("qq_tls_trust_load: EvaluateTrustLoad——系统集合部分兼容,显式锚严格拒绝") {
    const auto good = test_support::GenerateSelfSignedCert();
    REQUIRE(good.has_value());
    const std::string mixed = good->ca_pem + BadPemCertificate();

    // 系统集合:rc>0 且成功链非空 → 保留成功链继续,警告失败张(§三)。
    const TrustLoadReport system_report = EvaluateTrustLoad("system_pem", mixed);
    CHECK(system_report.ok_to_continue);
    CHECK(system_report.partial);
    CHECK(system_report.parse_rc > 0);
    CHECK(system_report.failed_count == 1);
    CHECK(system_report.parsed_count == 1);
    CHECK(system_report.exported_count == 2);
    CHECK_FALSE(system_report.warning.empty());
    CHECK_FALSE(system_report.bad_cert_notes.empty());
    CHECK(system_report.bad_cert_notes.size() <= 3);  // 有界诊断
    // 坏证 note 只带指纹与负码,不带 PEM/主体。
    CHECK(system_report.bad_cert_notes[0].find("BEGIN CERTIFICATE") == std::string::npos);
    CHECK(system_report.bad_cert_notes[0].find("sha256:") != std::string::npos);

    // 显式锚:同一集合严格拒绝——含坏证即 load_failed,不回退系统证书,
    // 错误信息保留源头(§三)。
    const TrustLoadReport explicit_report = EvaluateTrustLoad("explicit", mixed);
    CHECK_FALSE(explicit_report.ok_to_continue);
    CHECK_FALSE(explicit_report.partial);
    CHECK(explicit_report.failed_count == 1);
    CHECK(explicit_report.parsed_count == 1);  // 解析得动 1 张(但仍不喂)
    CHECK_FALSE(explicit_report.error.empty());
    CHECK(explicit_report.error.find("严格拒绝") != std::string::npos);
}

TEST_CASE("qq_tls_trust_load: EvaluateTrustLoad——坏证在首/中/尾都部分成功") {
    const auto good = test_support::GenerateSelfSignedCert();
    REQUIRE(good.has_value());
    const std::string bad = BadPemCertificate();
    const std::string layouts[] = {
        bad + good->ca_pem,                  // 首
        good->ca_pem + bad,                  // 尾
        good->ca_pem + bad + good->ca_pem,   // 中(重复合法证夹坏证)
    };
    for (const std::string& pem : layouts) {
        const TrustLoadReport report = EvaluateTrustLoad("system_pem", pem);
        CHECK(report.ok_to_continue);
        CHECK(report.partial);
        CHECK(report.failed_count >= 1);
        CHECK(report.parsed_count >= 1);
    }
}

TEST_CASE("qq_tls_trust_load: EvaluateTrustLoad——全部坏证不可继续(完整边界)") {
    const std::string all_bad = BadPemCertificate() + BadPemCertificate();
    const TrustLoadReport report = EvaluateTrustLoad("system_pem", all_bad);
    CHECK_FALSE(report.ok_to_continue);
    CHECK_FALSE(report.input_empty);  // 非空输入:不是 empty 案
    CHECK(report.parsed_count == 0);  // 成功链空:不许继续
    CHECK_FALSE(report.error.empty());
}

TEST_CASE("qq_tls_trust_load: EvaluateTrustLoad——重复证书按指纹去重") {
    const auto good = test_support::GenerateSelfSignedCert();
    REQUIRE(good.has_value());
    const std::string triple = good->ca_pem + good->ca_pem + good->ca_pem;
    const TrustLoadReport report = EvaluateTrustLoad("system_pem", triple);
    CHECK(report.ok_to_continue);
    CHECK(report.exported_count == 3);  // 输入 3 条
    CHECK(report.unique_count == 1);    // 去重后 1 张(§三:跨店重复只记一张)
    CHECK(report.parsed_count == 3);    // mbedTLS 全都解析得动(链上 3 节点)
}

TEST_CASE("qq_tls_trust_load: EvaluateTrustLoad——大输入(数百张)不崩、账目对") {
    const auto good = test_support::GenerateSelfSignedCert();
    REQUIRE(good.has_value());
    std::string big;
    big.reserve(good->ca_pem.size() * 301);
    for (int i = 0; i < 300; ++i) {
        big += good->ca_pem;
    }
    big += BadPemCertificate();  // 大集合里埋一张坏证
    const TrustLoadReport report = EvaluateTrustLoad("system_pem", big);
    CHECK(report.ok_to_continue);
    CHECK(report.partial);
    CHECK(report.unique_count == 1);      // 300 张都是同一张证
    CHECK(report.failed_count == 1);
    CHECK(report.bad_cert_notes.size() == 1);  // 有界:只报一张
}

// ---------------------------------------------------------------------------
// ResolveChannelTrustRoots:显式严格不回退;Unix 系统文件路径部分兼容。
// ---------------------------------------------------------------------------

TEST_CASE("qq_tls_trust_load: ResolveChannelTrustRoots——显式混合锚严格拒绝不回退") {
    const auto good = test_support::GenerateSelfSignedCert();
    REQUIRE(good.has_value());
    const std::string mixed = good->ca_pem + BadPemCertificate();
    bool detect_called = false;
    const ResolvedTrustStore resolved = ResolveChannelTrustRoots(mixed, [&detect_called]() {
        detect_called = true;
        return std::string();
    });
    CHECK(resolved.source == "explicit");
    CHECK(resolved.ca_pem.empty());  // 坏锚不喂 mbedTLS
    CHECK_FALSE(resolved.error.empty());
    CHECK(resolved.error.find("严格拒绝") != std::string::npos);
    CHECK(resolved.certificate_count == 0);
    CHECK_FALSE(detect_called);  // 不回退平台来源

    // 全合法显式锚照常通过。
    const ResolvedTrustStore ok = ResolveChannelTrustRoots(good->ca_pem);
    CHECK(ok.source == "explicit");
    CHECK(ok.error.empty());
    CHECK(ok.ca_pem == good->ca_pem);
    CHECK(ok.certificate_count == 1);
}

#ifndef _WIN32
// Unix 腿:系统 PEM 文件路径可注入临时文件,系统集合部分兼容可端到端验。
// Windows 的对应行为(系统店导出→部分兼容)归 Q3 真机:CI 机器证书店
// 内容不可控,不硬造。
TEST_CASE("qq_tls_trust_load: ResolveChannelTrustRoots——系统 PEM 混合文件部分兼容") {
    const auto good = test_support::GenerateSelfSignedCert();
    REQUIRE(good.has_value());
    const std::filesystem::path dir =
        std::filesystem::temp_directory_path() / "lubancode-qq-trust-load-test";
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    const std::filesystem::path mixed_path = dir / "mixed-ca.pem";
    {
        std::FILE* out = std::fopen(mixed_path.string().c_str(), "wb");
        REQUIRE(out != nullptr);
        const std::string mixed = good->ca_pem + BadPemCertificate();
        std::fwrite(mixed.data(), 1, mixed.size(), out);
        std::fclose(out);
    }
    const ResolvedTrustStore resolved =
        ResolveChannelTrustRoots("", [&mixed_path]() { return mixed_path.string(); });
    CHECK(resolved.source == "system_pem");
    CHECK(resolved.error.empty());
    CHECK(resolved.ca_pem.find("BEGIN CERTIFICATE") != std::string::npos);  // 保留成功链
    CHECK(resolved.certificate_count == 1);
    CHECK(resolved.load.ok_to_continue);
    CHECK(resolved.load.partial);
    CHECK(resolved.load.failed_count == 1);
    CHECK_FALSE(resolved.load.warning.empty());
    CHECK(resolved.detail.find("跳过 1") != std::string::npos);

    // 全坏文件:非空但解析失败 → 明报不可用(不报 empty)。
    const std::filesystem::path bad_path = dir / "all-bad-ca.pem";
    {
        std::FILE* out = std::fopen(bad_path.string().c_str(), "wb");
        REQUIRE(out != nullptr);
        const std::string all_bad = BadPemCertificate() + BadPemCertificate();
        std::fwrite(all_bad.data(), 1, all_bad.size(), out);
        std::fclose(out);
    }
    const ResolvedTrustStore bad_resolved =
        ResolveChannelTrustRoots("", [&bad_path]() { return bad_path.string(); });
    CHECK(bad_resolved.source == "none");
    CHECK(bad_resolved.ca_pem.empty());
    CHECK_FALSE(bad_resolved.error.empty());
    CHECK_FALSE(bad_resolved.load.ok_to_continue);
}
#endif  // !_WIN32

// ---------------------------------------------------------------------------
// 真 TLS 本地握手(§六:处理后的混合集合进 mbedTLS 真握手;只验计数不算
// TLS 修复通过)。
// ---------------------------------------------------------------------------

TEST_CASE("qq_tls_trust_load: 混合集合真握手——SystemDefault 保留成功链(Unix)") {
#ifndef _WIN32
    const auto cert = test_support::GenerateSelfSignedCert();
    REQUIRE(cert.has_value());
    test_support::MockTlsServer server;
    const auto port = server.Start(*cert);
    REQUIRE(port.has_value());
    std::thread acceptor([&]() { (void)server.AcceptNext(10'000); });

    const std::string mixed = cert->ca_pem + BadPemCertificate();
    auto socket = TcpSocket::Connect("127.0.0.1", *port, 5'000);
    REQUIRE(socket.has_value());
    const auto stream = TlsClientStream::Connect(std::move(*socket), "127.0.0.1", mixed,
                                                 TlsTrustMode::SystemDefault, 10'000);
    // 修后:坏证跳过、合法锚保留,握手照常成功。修前:混合集合被整批
    // 判死,这里报 trust_store_empty。
    if (!stream.has_value()) {
        acceptor.join();
        FAIL(stream.error().detail);
    }
    CHECK(stream.has_value());
    acceptor.join();
#else
    // Windows:SystemDefault 接系统策略回调,自签测试证书被系统链构建
    // 拒绝(不可信根)——顺带冒烟回调路径真跑过(拒绝说明书带
    // "windows policy rejected" 前缀,它只在回调写了 reject_code 后出现)、
    // 回调状态地址有效不崩(§五)。不赌具体 CERT_E_* 映射。
    const auto cert = test_support::GenerateSelfSignedCert();
    REQUIRE(cert.has_value());
    test_support::MockTlsServer server;
    const auto port = server.Start(*cert);
    REQUIRE(port.has_value());
    std::thread acceptor([&]() { (void)server.AcceptNext(10'000); });
    auto socket = TcpSocket::Connect("127.0.0.1", *port, 5'000);
    REQUIRE(socket.has_value());
    const auto stream = TlsClientStream::Connect(std::move(*socket), "127.0.0.1",
                                                 cert->ca_pem + BadPemCertificate(),
                                                 TlsTrustMode::SystemDefault, 10'000);
    REQUIRE_FALSE(stream.has_value());
    CHECK(stream.error().kind == TlsErrorKind::CertVerifyFailed);
    CHECK_FALSE(stream.error().error_code.empty());
    CHECK(stream.error().detail.find("windows policy rejected") != std::string::npos);
    acceptor.join();
#endif
}

TEST_CASE("qq_tls_trust_load: 连接层稳定码——空输入 empty、非空解析失败 load_failed") {
    const auto cert = test_support::GenerateSelfSignedCert();
    REQUIRE(cert.has_value());
    test_support::MockTlsServer server;
    const auto port = server.Start(*cert);
    REQUIRE(port.has_value());
    std::thread acceptor([&]() { (void)server.AcceptNext(10'000); });

    // 空输入:唯一报 empty 的情形。
    {
        auto socket = TcpSocket::Connect("127.0.0.1", *port, 5'000);
        REQUIRE(socket.has_value());
        const auto stream = TlsClientStream::Connect(std::move(*socket), "127.0.0.1", "",
                                                     TlsTrustMode::SystemDefault, 10'000);
        REQUIRE_FALSE(stream.has_value());
        CHECK(stream.error().error_code == kTlsCodeTrustStoreEmpty);
    }
    // 纯垃圾(非空、负码):load_failed 带真实返回码——修前误报 empty。
    {
        auto socket = TcpSocket::Connect("127.0.0.1", *port, 5'000);
        REQUIRE(socket.has_value());
        const auto stream = TlsClientStream::Connect(std::move(*socket), "127.0.0.1",
                                                     "not a pem at all",
                                                     TlsTrustMode::SystemDefault, 10'000);
        REQUIRE_FALSE(stream.has_value());
        CHECK(stream.error().error_code == kTlsCodeTrustStoreLoadFailed);
        CHECK(stream.error().detail.find("rc=") != std::string::npos);
    }
    // 显式混合锚:连接层同策略严格拒绝(部分成功也不喂)。
    {
        auto socket = TcpSocket::Connect("127.0.0.1", *port, 5'000);
        REQUIRE(socket.has_value());
        const auto stream =
            TlsClientStream::Connect(std::move(*socket), "127.0.0.1",
                                     cert->ca_pem + BadPemCertificate(),
                                     TlsTrustMode::ExplicitCa, 10'000);
        REQUIRE_FALSE(stream.has_value());
        CHECK(stream.error().error_code == kTlsCodeTrustStoreLoadFailed);
        CHECK(stream.error().detail.find("严格拒绝") != std::string::npos);
    }
    acceptor.join();
}

TEST_CASE("qq_tls_trust_load: 显式锚真握手——合法成/错主机名/不信任/过期败") {
    const auto cert = test_support::GenerateSelfSignedCert();
    REQUIRE(cert.has_value());

    // 合法主机(ExplicitCa 纯 mbedTLS,三平台一致)。
    {
        test_support::MockTlsServer server;
        const auto port = server.Start(*cert);
        REQUIRE(port.has_value());
        std::thread acceptor([&]() { (void)server.AcceptNext(10'000); });
        auto socket = TcpSocket::Connect("127.0.0.1", *port, 5'000);
        REQUIRE(socket.has_value());
        const auto stream = TlsClientStream::Connect(std::move(*socket), "127.0.0.1",
                                                     cert->ca_pem, TlsTrustMode::ExplicitCa,
                                                     10'000);
        if (!stream.has_value()) {
            acceptor.join();
            FAIL(stream.error().detail);
        }
        acceptor.join();
    }
    // 错主机名:证书 CN=127.0.0.1,验证名给别的。
    {
        test_support::MockTlsServer server;
        const auto port = server.Start(*cert);
        REQUIRE(port.has_value());
        std::thread acceptor([&]() { (void)server.AcceptNext(10'000); });
        auto socket = TcpSocket::Connect("127.0.0.1", *port, 5'000);
        REQUIRE(socket.has_value());
        const auto stream = TlsClientStream::Connect(std::move(*socket), "wrong-name.test",
                                                     cert->ca_pem, TlsTrustMode::ExplicitCa,
                                                     10'000);
        REQUIRE_FALSE(stream.has_value());
        CHECK(stream.error().kind == TlsErrorKind::CertVerifyFailed);
        CHECK(stream.error().error_code == kTlsCodeCertHostnameMismatch);
        acceptor.join();
    }
    // 不信任:锚用另一张无关证书。
    {
        const auto other = test_support::GenerateSelfSignedCert();
        REQUIRE(other.has_value());
        test_support::MockTlsServer server;
        const auto port = server.Start(*cert);
        REQUIRE(port.has_value());
        std::thread acceptor([&]() { (void)server.AcceptNext(10'000); });
        auto socket = TcpSocket::Connect("127.0.0.1", *port, 5'000);
        REQUIRE(socket.has_value());
        const auto stream = TlsClientStream::Connect(std::move(*socket), "127.0.0.1",
                                                     other->ca_pem, TlsTrustMode::ExplicitCa,
                                                     10'000);
        REQUIRE_FALSE(stream.has_value());
        CHECK(stream.error().kind == TlsErrorKind::CertVerifyFailed);
        CHECK(stream.error().error_code == kTlsCodeCertNotTrusted);
        acceptor.join();
    }
    // 过期:有效期落在过去(2020-2021),锚是同一张过期证。
    {
        test_support::MockTlsCertOptions options;
        options.not_before = "20200101000000";
        options.not_after = "20210101000000";
        const auto expired = test_support::GenerateSelfSignedCert(options);
        REQUIRE(expired.has_value());
        test_support::MockTlsServer server;
        const auto port = server.Start(*expired);
        REQUIRE(port.has_value());
        std::thread acceptor([&]() { (void)server.AcceptNext(10'000); });
        auto socket = TcpSocket::Connect("127.0.0.1", *port, 5'000);
        REQUIRE(socket.has_value());
        const auto stream = TlsClientStream::Connect(std::move(*socket), "127.0.0.1",
                                                     expired->ca_pem, TlsTrustMode::ExplicitCa,
                                                     10'000);
        REQUIRE_FALSE(stream.has_value());
        CHECK(stream.error().kind == TlsErrorKind::CertVerifyFailed);
        CHECK(stream.error().error_code == kTlsCodeCertExpired);
        acceptor.join();
    }
}

}  // namespace lubancode::channel::qq
