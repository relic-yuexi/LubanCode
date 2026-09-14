#include "channel/qq/qq_tls.hpp"

#include <algorithm>
#include <cstdio>
#include <utility>
#include <vector>

#include <mbedtls/ctr_drbg.h>
#include <mbedtls/entropy.h>
#include <mbedtls/error.h>
#include <mbedtls/net_sockets.h>  // MBEDTLS_ERR_NET_* 错误码
#include <mbedtls/ssl.h>
#include <mbedtls/version.h>
#include <mbedtls/x509_crt.h>

#include "platform/base64.hpp"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <wincrypt.h>
#endif

namespace lubancode::channel::qq {

namespace {

// mbedtls 全家桶(实体只在 .cpp 可见,头里是 void*)。socket 也住这里:
// BIO 回调指针锚在堆上不动窝的实体,移动 TlsClientStream 不断链。
struct TlsContext {
    mbedtls_ssl_context ssl{};
    mbedtls_ssl_config config{};
    mbedtls_x509_crt ca{};
    mbedtls_ctr_drbg_context drbg{};
    mbedtls_entropy_context entropy{};
    TcpSocket sock{};

    TlsContext() {
        mbedtls_ssl_init(&ssl);
        mbedtls_ssl_config_init(&config);
        mbedtls_x509_crt_init(&ca);
        mbedtls_ctr_drbg_init(&drbg);
        mbedtls_entropy_init(&entropy);
    }
    ~TlsContext() {
        mbedtls_ssl_free(&ssl);
        mbedtls_ssl_config_free(&config);
        mbedtls_x509_crt_free(&ca);
        mbedtls_ctr_drbg_free(&drbg);
        mbedtls_entropy_free(&entropy);
    }
    TlsContext(const TlsContext&) = delete;
    TlsContext& operator=(const TlsContext&) = delete;
};

int MbedSend(void* ctx, const unsigned char* buf, std::size_t len) {
    auto* sock = static_cast<TcpSocket*>(ctx);
    const auto result = sock->WriteAll(
        std::string_view(reinterpret_cast<const char*>(buf), len), 10'000);
    if (!result.has_value()) {
        switch (result.error().kind) {
            case SocketErrorKind::Timeout:
                return MBEDTLS_ERR_SSL_TIMEOUT;
            default:
                return MBEDTLS_ERR_NET_SEND_FAILED;
        }
    }
    return static_cast<int>(len);
}

int MbedRecv(void* ctx, unsigned char* buf, std::size_t len) {
    auto* sock = static_cast<TcpSocket*>(ctx);
    const auto result = sock->ReadSome(reinterpret_cast<char*>(buf), len, 10'000);
    if (!result.has_value()) {
        switch (result.error().kind) {
            case SocketErrorKind::Timeout:
                return MBEDTLS_ERR_SSL_TIMEOUT;
            case SocketErrorKind::Closed:
                return MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY;
            default:
                return MBEDTLS_ERR_NET_RECV_FAILED;
        }
    }
    return static_cast<int>(*result);
}

int MbedRecvTimeout(void* ctx, unsigned char* buf, std::size_t len, std::uint32_t timeout_ms) {
    auto* sock = static_cast<TcpSocket*>(ctx);
    const auto result =
        sock->ReadSome(reinterpret_cast<char*>(buf), len, static_cast<int>(timeout_ms));
    if (!result.has_value()) {
        switch (result.error().kind) {
            case SocketErrorKind::Timeout:
                return MBEDTLS_ERR_SSL_TIMEOUT;
            case SocketErrorKind::Closed:
                return MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY;
            default:
                return MBEDTLS_ERR_NET_RECV_FAILED;
        }
    }
    return static_cast<int>(*result);
}

std::string MbedErrorText(int code) {
    char buf[256] = {0};
    mbedtls_strerror(code, buf, sizeof(buf));
    return std::string(buf) + " (" + std::to_string(code) + ")";
}

SocketErrorKind ToSocketKind(int mbed_code) {
    if (mbed_code == MBEDTLS_ERR_SSL_TIMEOUT) {
        return SocketErrorKind::Timeout;
    }
    if (mbed_code == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY ||
        mbed_code == MBEDTLS_ERR_SSL_CONN_EOF ||
        mbed_code == MBEDTLS_ERR_NET_CONN_RESET) {
        return SocketErrorKind::Closed;
    }
    return SocketErrorKind::Failed;
}

// 数 PEM 拼串里解析得动的证书张数(显式锚有效性检查;解析失败返回 -1)。
int CountParsedCertificates(const std::string& ca_pem) {
    if (ca_pem.empty()) {
        return 0;
    }
    mbedtls_x509_crt parsed;
    mbedtls_x509_crt_init(&parsed);
    const int rc = mbedtls_x509_crt_parse(&parsed,
                                          reinterpret_cast<const unsigned char*>(ca_pem.data()),
                                          ca_pem.size() + 1);
    if (rc != 0) {
        mbedtls_x509_crt_free(&parsed);
        return -1;
    }
    int count = 0;
    for (const mbedtls_x509_crt* it = &parsed; it != nullptr; it = it->next) {
        if (it->raw.len > 0) {
            ++count;
        }
    }
    mbedtls_x509_crt_free(&parsed);
    return count;
}

#ifdef _WIN32

// ---------------------------------------------------------------------------
// Windows 系统信任(Windows 信任根单 §四)
// ---------------------------------------------------------------------------

// Windows 证书店的证 -> PEM 拼串。店:Root 与 Ca(中间),CurrentUser 与
// LocalMachine 各开一遍(Disallowed 里的证跳过——显式不信任的锚不喂
// mbedTLS;链构建侧还会再拦一道)。打不开店不致命:跳过该店继续拼。
struct WindowsTrustExport {
    std::string pem;
    int count = 0;
    int skipped_disallowed = 0;
    std::string first_error;  // 首个店打开失败(诊断)
};

bool CertificateIsDisallowed(PCCERT_CONTEXT cert) {
    // 逐店查 Disallowed(CurrentUser 优先):命中指纹即显式不信任。
    static const DWORD kLocations[] = {CERT_SYSTEM_STORE_CURRENT_USER,
                                       CERT_SYSTEM_STORE_LOCAL_MACHINE};
    for (DWORD location : kLocations) {
        HCERTSTORE disallowed = CertOpenStore(
            CERT_STORE_PROV_SYSTEM_W, PKCS_7_ASN_ENCODING | X509_ASN_ENCODING, NULL,
            location | CERT_STORE_READONLY_FLAG, L"Disallowed");
        if (disallowed == NULL) {
            continue;
        }
        DWORD hash_size = 0;
        if (!CertGetCertificateContextProperty(cert, CERT_SHA1_HASH_PROP_ID, NULL,
                                               &hash_size) ||
            hash_size == 0 || hash_size > 64) {
            CertCloseStore(disallowed, 0);
            continue;
        }
        unsigned char hash[64] = {0};
        if (CertGetCertificateContextProperty(cert, CERT_SHA1_HASH_PROP_ID, hash,
                                              &hash_size)) {
            CRYPT_HASH_BLOB blob;
            blob.cbData = hash_size;
            blob.pbData = hash;
            PCCERT_CONTEXT found =
                CertFindCertificateInStore(disallowed, PKCS_7_ASN_ENCODING | X509_ASN_ENCODING,
                                           0, CERT_FIND_SHA1_HASH, &blob, NULL);
            if (found != NULL) {
                CertFreeCertificateContext(found);
                CertCloseStore(disallowed, 0);
                return true;
            }
        }
        CertCloseStore(disallowed, 0);
    }
    return false;
}

// 证书店 -> PEM 拼串。店:Root 与 Ca(中间),CurrentUser 与 LocalMachine
// 各开一遍(Disallowed 里的证跳过——显式不信任的锚不喂 mbedTLS;链构建
// 侧还会再拦一道)。打不开店不致命:跳过该店继续拼。
//
// context 所有权约定(CI 实测 SIGSEGV 的根因修订):
// CertEnumCertificatesInStore 释放传入的 pPrevCertContext 并返回下一枚
// (MSDN:pPrevCertContext is always freed by this function)。因此调用方
// 对"当前枚"只借读、在推进语句里交还所有权——绝不对它调
// CertFreeCertificateContext(那是 double free),也绝不跨推进语句引用
// 它(那是 use-after-free)。循环出口 NULL 自带清账,无泄漏。
void AppendStoreCertificatesToExport(const wchar_t* store_name, DWORD location,
                                     WindowsTrustExport& out) {
    HCERTSTORE store = CertOpenStore(
        CERT_STORE_PROV_SYSTEM_W, PKCS_7_ASN_ENCODING | X509_ASN_ENCODING, NULL,
        location | CERT_STORE_READONLY_FLAG, store_name);
    if (store == NULL) {
        if (out.first_error.empty()) {
            out.first_error = "CertOpenStore(" +
                              std::to_string(static_cast<long>(location)) + ") failed " +
                              std::to_string(GetLastError());
        }
        return;
    }
    for (PCCERT_CONTEXT cert = CertEnumCertificatesInStore(store, NULL); cert != NULL;
         cert = CertEnumCertificatesInStore(store, cert)) {
        // 非证书条目(空指针/零长编码)不喂解析,跳过——店内容不保证全可编码。
        if (cert->pbCertEncoded == NULL || cert->cbCertEncoded == 0) {
            continue;
        }
        if (CertificateIsDisallowed(cert)) {
            ++out.skipped_disallowed;
            continue;
        }
        const std::string_view der(reinterpret_cast<const char*>(cert->pbCertEncoded),
                                   cert->cbCertEncoded);
        out.pem += "-----BEGIN CERTIFICATE-----\n";
        std::string encoded = platform::Base64Encode(der);
        for (std::size_t i = 0; i < encoded.size(); i += 64) {
            out.pem += encoded.substr(i, 64);
            out.pem += "\n";
        }
        out.pem += "-----END CERTIFICATE-----\n";
        ++out.count;
        // 此处到下一次推进前 cert 仍有效;推进语句释放它,PEM 已拷出。
    }
    CertCloseStore(store, 0);
}

WindowsTrustExport ExportWindowsTrustRoots() {
    WindowsTrustExport out;
    AppendStoreCertificatesToExport(L"Root", CERT_SYSTEM_STORE_CURRENT_USER, out);
    AppendStoreCertificatesToExport(L"Root", CERT_SYSTEM_STORE_LOCAL_MACHINE, out);
    AppendStoreCertificatesToExport(L"Ca", CERT_SYSTEM_STORE_CURRENT_USER, out);
    AppendStoreCertificatesToExport(L"Ca", CERT_SYSTEM_STORE_LOCAL_MACHINE, out);
    return out;
}

// Windows SSL 策略校验的回调状态(mbedtls_ssl_conf_verify 的 p_ctx)。
struct WindowsVerifyContext {
    std::string reject_code;      // 系统拒绝时的稳定码(kTlsCode*)
};

// wincrypt.h 的 CERT_TRUST_IS_NOT_TRUSTED(链上有显式不信任/不可信的
// 锚,值自 Win SDK 一贯为 0x00000020)。CI 实测部分 SDK/宏组合下该常量
// 不在展开集内(C2065),按官方文档数值本地兜底,不赌目标宏。
constexpr DWORD kCertTrustIsNotTrusted = 0x00000020;

std::string WindowsChainErrorToCode(DWORD policy_error, DWORD chain_error_status) {
    if (policy_error == CERT_E_CN_NO_MATCH) {
        return kTlsCodeCertHostnameMismatch;
    }
    if (policy_error == CERT_E_EXPIRED ||
        (chain_error_status & CERT_TRUST_IS_NOT_TIME_VALID) != 0 ||
        (chain_error_status & CERT_TRUST_IS_NOT_TIME_NESTED) != 0) {
        return kTlsCodeCertExpired;
    }
    if (policy_error == CERT_E_UNTRUSTEDROOT ||
        (chain_error_status & kCertTrustIsNotTrusted) != 0) {
        return kTlsCodeCertNotTrusted;
    }
    if (policy_error == CERT_E_WRONG_USAGE ||
        (chain_error_status & CERT_TRUST_IS_NOT_VALID_FOR_USAGE) != 0) {
        return kTlsCodeCertPolicyRejected;
    }
    return kTlsCodeCertPolicyRejected;
}

// mbedTLS 证书验证回调(SystemDefault 模式,仅 Windows):对 leaf(depth 0)
// 跑 Windows 链构建 + SSL 策略校验(CertGetCertificateChain +
// CertVerifyCertificateChainPolicy)。系统拒 -> flags 落 BADCERT 位(稳定码
// 记进 ctx);系统过 -> 只清"链不可信"位——系统链构建是链信任的权威(能
// 走 AIA 拉中间证书、应用 Disallowed 店与系统弱算法策略,本地导出的证书
// 子集做不到);主机名/有效期/EKU 的 mbedTLS 判定位保留,双保险不互盖。
// 其余 depth 不动 flags(链账归系统构建)。
//
// SDK 兼容口径:CERT_CHAIN_PARA/CERT_CHAIN_POLICY_PARA 只写 cbSize,
// 不碰 dwUrlRetrievalTimeout/pvExtraPara(部分 SDK 展开集缺这两个成员,
// CI 实测 C2039)——URL 拉取走系统默认超时;SSL 主机名校验不靠
// pvExtraPara 传 SSL_EXTRA_CERT_CHAIN_POLICY_PARA,由 mbedTLS 内置
// hostname 验证(mbedtls_ssl_set_hostname + BADCERT_CN_MISMATCH)承担,
// 系统侧的用途/显式不信任判定在链构建与基础 SSL 策略里本就有。
int WindowsPolicyVerify(void* ctx, mbedtls_x509_crt* crt, int depth, std::uint32_t* flags) {
    auto* verify = static_cast<WindowsVerifyContext*>(ctx);
    if (verify == nullptr || crt == nullptr || flags == nullptr) {
        return 0;
    }
    if (depth != 0) {
        return 0;  // 只在 leaf 上跑一次系统校验
    }
    PCCERT_CONTEXT win_cert = CertCreateCertificateContext(
        PKCS_7_ASN_ENCODING | X509_ASN_ENCODING, crt->raw.p,
        static_cast<DWORD>(crt->raw.len));
    if (win_cert == NULL) {
        verify->reject_code = kTlsCodeCertVerifyFailed;
        *flags |= MBEDTLS_X509_BADCERT_OTHER;
        return 0;
    }
    CERT_CHAIN_PARA chain_para;
    ZeroMemory(&chain_para, sizeof(chain_para));
    chain_para.cbSize = sizeof(chain_para);
    PCCERT_CHAIN_CONTEXT chain = NULL;
    if (!CertGetCertificateChain(NULL, win_cert, NULL, NULL, &chain_para, 0, NULL, &chain) ||
        chain == NULL) {
        CertFreeCertificateContext(win_cert);
        verify->reject_code = kTlsCodeCertNotTrusted;
        *flags |= MBEDTLS_X509_BADCERT_OTHER;
        return 0;
    }
    CERT_CHAIN_POLICY_PARA policy_para;
    ZeroMemory(&policy_para, sizeof(policy_para));
    policy_para.cbSize = sizeof(policy_para);
    CERT_CHAIN_POLICY_STATUS policy_status;
    ZeroMemory(&policy_status, sizeof(policy_status));
    policy_status.cbSize = sizeof(policy_status);
    const BOOL policy_ok = CertVerifyCertificateChainPolicy(
        CERT_CHAIN_POLICY_SSL, chain, &policy_para, &policy_status);
    const bool rejected = !policy_ok || policy_status.dwError != 0 ||
                          chain->TrustStatus.dwErrorStatus != 0;
    if (rejected) {
        verify->reject_code =
            WindowsChainErrorToCode(policy_status.dwError, chain->TrustStatus.dwErrorStatus);
        *flags |= MBEDTLS_X509_BADCERT_OTHER;
    } else {
        // 系统裁决通过:只放行链信任类误报(导入子集缺中间证书/根——
        // 系统走 AIA 能闭链,子集不能);主机名/时间/用途位保留。
        *flags &= ~static_cast<std::uint32_t>(MBEDTLS_X509_BADCERT_NOT_TRUSTED);
    }
    CertFreeCertificateChain(chain);
    CertFreeCertificateContext(win_cert);
    return 0;
}

#endif  // _WIN32

}  // namespace

std::string DetectSystemCaPemPath() {
    static const char* kCandidates[] = {
        "/etc/ssl/cert.pem",                    // macOS / 部分 Linux
        "/etc/ssl/certs/ca-certificates.crt",   // Debian 系
        "/etc/pki/tls/certs/ca-bundle.crt",     // RHEL 系
        "/etc/openssl/cert.pem",                // FreeBSD
    };
    for (const char* path : kCandidates) {
        std::FILE* probe = std::fopen(path, "rb");
        if (probe != nullptr) {
            std::fclose(probe);
            return path;
        }
    }
    return std::string();
}

std::string TlsVerifyFlagsToCode(std::uint32_t flags) {
    if ((flags & MBEDTLS_X509_BADCERT_CN_MISMATCH) != 0) {
        return kTlsCodeCertHostnameMismatch;
    }
    if ((flags & MBEDTLS_X509_BADCERT_EXPIRED) != 0) {
        return kTlsCodeCertExpired;
    }
    if ((flags & MBEDTLS_X509_BADCERT_NOT_TRUSTED) != 0) {
        return kTlsCodeCertNotTrusted;
    }
    return kTlsCodeCertVerifyFailed;
}

std::string TlsHandshakeCodeToCode(int mbed_code) {
    if (mbed_code == MBEDTLS_ERR_SSL_TIMEOUT) {
        return kTlsCodeHandshakeTimeout;
    }
    return kTlsCodeHandshakeFailed;
}

ResolvedTrustStore ResolveChannelTrustRoots(const std::string& explicit_ca_pem,
                                            const std::function<std::string()>&
                                                detect_pem_path) {
    ResolvedTrustStore resolved;
    if (!explicit_ca_pem.empty()) {
        // 显式信任锚:调用方全权指定,不回退平台来源;解析不动明报,
        // 无效锚不喂 mbedTLS(连接时按 trust_store_empty 稳定码失败)。
        resolved.source = "explicit";
        resolved.detail = "显式配置的信任锚";
        const int count = CountParsedCertificates(explicit_ca_pem);
        if (count <= 0) {
            resolved.certificate_count = 0;
            resolved.error = count < 0 ? "explicit ca_pem 解析失败(mbedtls parse 非零)"
                                       : "explicit ca_pem 解析出 0 张证书";
        } else {
            resolved.certificate_count = count;
            resolved.ca_pem = explicit_ca_pem;
        }
        return resolved;
    }
#ifdef _WIN32
    const WindowsTrustExport exported = ExportWindowsTrustRoots();
    resolved.source = "windows_system_store";
    resolved.ca_pem = exported.pem;
    resolved.certificate_count = exported.count;
    resolved.detail = "Windows 系统证书库(Root/Ca,CurrentUser+LocalMachine";
    if (exported.skipped_disallowed > 0) {
        resolved.detail += ";跳过 Disallowed " + std::to_string(exported.skipped_disallowed) +
                           " 张";
    }
    resolved.detail += ")";
    if (exported.count == 0) {
        resolved.ca_pem.clear();
        resolved.error = exported.first_error.empty()
                             ? "Windows 系统证书库导出 0 张信任根"
                             : "Windows 系统证书库打开失败: " + exported.first_error;
    }
    return resolved;
#else
    const std::string detected =
        detect_pem_path ? detect_pem_path() : DetectSystemCaPemPath();
    if (detected.empty()) {
        resolved.source = "none";
        resolved.error = "未探测到系统 CA PEM(Linux/macOS 常见路径都没有)";
        return resolved;
    }
    std::FILE* file = std::fopen(detected.c_str(), "rb");
    if (file == nullptr) {
        resolved.source = "none";
        resolved.error = "系统 CA PEM 打不开: " + detected;
        return resolved;
    }
    std::string content;
    char buffer[8192];
    std::size_t got = 0;
    while ((got = std::fread(buffer, 1, sizeof(buffer), file)) > 0) {
        content.append(buffer, got);
    }
    std::fclose(file);
    const int count = CountParsedCertificates(content);
    if (count <= 0) {
        resolved.source = "none";
        resolved.error = "系统 CA PEM 解析失败或为空: " + detected;
        return resolved;
    }
    resolved.source = "system_pem";
    resolved.ca_pem = std::move(content);
    resolved.certificate_count = count;
    resolved.detail = detected;
    return resolved;
#endif
}

TlsClientStream::~TlsClientStream() {
    delete static_cast<TlsContext*>(context_);
    context_ = nullptr;
}

TlsClientStream::TlsClientStream(TlsClientStream&& other) noexcept
    : context_(other.context_) {
    other.context_ = nullptr;
}

TlsClientStream& TlsClientStream::operator=(TlsClientStream&& other) noexcept {
    if (this != &other) {
        delete static_cast<TlsContext*>(context_);
        context_ = other.context_;
        other.context_ = nullptr;
    }
    return *this;
}

std::expected<TlsClientStream, TlsError> TlsClientStream::Connect(TcpSocket socket,
                                                                  const std::string& host,
                                                                  const std::string& ca_pem,
                                                                  TlsTrustMode trust_mode,
                                                                  int handshake_timeout_ms) {
    if (!socket.valid()) {
        return std::unexpected(TlsError{TlsErrorKind::Failed, "socket not connected",
                                        kTlsCodeHandshakeFailed});
    }

    auto* context = new TlsContext();
    context->sock = std::move(socket);  // 所有权落进堆上实体

    const auto fail = [&](TlsErrorKind kind, std::string detail, std::string code) {
        delete context;
        return std::unexpected(TlsError{kind, std::move(detail), std::move(code)});
    };

    int rc = mbedtls_ctr_drbg_seed(&context->drbg, mbedtls_entropy_func,
                                   &context->entropy, nullptr, 0);
    if (rc != 0) {
        return fail(TlsErrorKind::Failed, "drbg seed: " + MbedErrorText(rc),
                    kTlsCodeHandshakeFailed);
    }
    // 信任根为空 = 稳定明错(不静默装个空锚让握手报含混的 not trusted)。
    if (CountParsedCertificates(ca_pem) <= 0) {
        return fail(TlsErrorKind::Failed, "trust store empty: no CA certificates provided",
                    kTlsCodeTrustStoreEmpty);
    }
    rc = mbedtls_x509_crt_parse(&context->ca,
                                reinterpret_cast<const unsigned char*>(ca_pem.data()),
                                ca_pem.size() + 1);
    if (rc != 0) {
        return fail(TlsErrorKind::Failed, "ca parse: " + MbedErrorText(rc),
                    kTlsCodeTrustStoreLoadFailed);
    }
    rc = mbedtls_ssl_config_defaults(&context->config, MBEDTLS_SSL_IS_CLIENT,
                                     MBEDTLS_SSL_TRANSPORT_STREAM,
                                     MBEDTLS_SSL_PRESET_DEFAULT);
    if (rc != 0) {
        return fail(TlsErrorKind::Failed, "config defaults: " + MbedErrorText(rc),
                    kTlsCodeHandshakeFailed);
    }
    mbedtls_ssl_conf_authmode(&context->config, MBEDTLS_SSL_VERIFY_REQUIRED);
    mbedtls_ssl_conf_ca_chain(&context->config, &context->ca, nullptr);
    mbedtls_ssl_conf_rng(&context->config, mbedtls_ctr_drbg_random, &context->drbg);
    mbedtls_ssl_conf_read_timeout(&context->config,
                                  static_cast<std::uint32_t>(handshake_timeout_ms));
#ifdef _WIN32
    WindowsVerifyContext windows_verify;
    if (trust_mode == TlsTrustMode::SystemDefault) {
        mbedtls_ssl_conf_verify(&context->config, WindowsPolicyVerify, &windows_verify);
    }
#else
    (void)trust_mode;  // 非 Windows:纯 mbedTLS 验证(链/时间/主机名)
#endif

    rc = mbedtls_ssl_setup(&context->ssl, &context->config);
    if (rc != 0) {
        return fail(TlsErrorKind::Failed, "ssl setup: " + MbedErrorText(rc),
                    kTlsCodeHandshakeFailed);
    }
    rc = mbedtls_ssl_set_hostname(&context->ssl, host.c_str());
    if (rc != 0) {
        return fail(TlsErrorKind::Failed, "set hostname: " + MbedErrorText(rc),
                    kTlsCodeHandshakeFailed);
    }
    mbedtls_ssl_set_bio(&context->ssl, &context->sock, MbedSend, MbedRecv, MbedRecvTimeout);

    while (true) {
        rc = mbedtls_ssl_handshake(&context->ssl);
        if (rc == 0) {
            break;
        }
        if (rc == MBEDTLS_ERR_SSL_WANT_READ || rc == MBEDTLS_ERR_SSL_WANT_WRITE) {
            continue;
        }
        if (rc == MBEDTLS_ERR_SSL_TIMEOUT) {
            return fail(TlsErrorKind::HandshakeFailed,
                        "handshake timeout: " + MbedErrorText(rc), kTlsCodeHandshakeTimeout);
        }
        if (rc == MBEDTLS_ERR_X509_CERT_VERIFY_FAILED) {
            const std::uint32_t flags = mbedtls_ssl_get_verify_result(&context->ssl);
#ifdef _WIN32
            // Windows 系统校验拒绝:flags 里是我们落的 BADCERT_OTHER 位,
            // 稳定码以系统裁决为准(CERT_E_* -> kTlsCode*)。
            if (trust_mode == TlsTrustMode::SystemDefault &&
                !windows_verify.reject_code.empty()) {
                return fail(TlsErrorKind::CertVerifyFailed,
                            "windows policy rejected: flags=0x" + std::to_string(flags) +
                                " code=" + windows_verify.reject_code,
                            windows_verify.reject_code);
            }
#endif
            return fail(TlsErrorKind::CertVerifyFailed,
                        "cert verify failed: flags=0x" + std::to_string(flags),
                        TlsVerifyFlagsToCode(flags));
        }
        return fail(TlsErrorKind::HandshakeFailed, "handshake: " + MbedErrorText(rc),
                    TlsHandshakeCodeToCode(rc));
    }
    // 握手成功后仍须核 verify flags(链不完整等在 REQUIRED 模式下应由
    // handshake 返回,这里再核一道,双保险)。
    const std::uint32_t flags = mbedtls_ssl_get_verify_result(&context->ssl);
    if (flags != 0) {
        return fail(TlsErrorKind::CertVerifyFailed,
                    "post-handshake verify flags=0x" + std::to_string(flags),
                    TlsVerifyFlagsToCode(flags));
    }

    return TlsClientStream(static_cast<void*>(context));
}

std::expected<std::size_t, SocketError> TlsClientStream::ReadSome(char* buf, std::size_t len,
                                                                  int timeout_ms) const {
    auto* context = static_cast<TlsContext*>(context_);
    if (context == nullptr) {
        return std::unexpected(SocketError{SocketErrorKind::Closed, "tls not open"});
    }
    mbedtls_ssl_conf_read_timeout(&context->config, static_cast<std::uint32_t>(timeout_ms));
    while (true) {
        const int rc = mbedtls_ssl_read(&context->ssl, reinterpret_cast<unsigned char*>(buf),
                                        len);
        if (rc >= 0) {
            return static_cast<std::size_t>(rc);
        }
        if (rc == MBEDTLS_ERR_SSL_WANT_READ || rc == MBEDTLS_ERR_SSL_WANT_WRITE) {
            continue;
        }
        if (rc == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY) {
            return std::unexpected(SocketError{SocketErrorKind::Closed, "tls close notify"});
        }
        return std::unexpected(
            SocketError{ToSocketKind(rc), "tls read: " + MbedErrorText(rc)});
    }
}

std::expected<void, SocketError> TlsClientStream::WriteAll(std::string_view bytes,
                                                           int timeout_ms) const {
    auto* context = static_cast<TlsContext*>(context_);
    if (context == nullptr) {
        return std::unexpected(SocketError{SocketErrorKind::Closed, "tls not open"});
    }
    mbedtls_ssl_conf_read_timeout(&context->config, static_cast<std::uint32_t>(timeout_ms));
    std::size_t sent = 0;
    while (sent < bytes.size()) {
        const int rc = mbedtls_ssl_write(
            &context->ssl, reinterpret_cast<const unsigned char*>(bytes.data() + sent),
            bytes.size() - sent);
        if (rc > 0) {
            sent += static_cast<std::size_t>(rc);
            continue;
        }
        if (rc == MBEDTLS_ERR_SSL_WANT_READ || rc == MBEDTLS_ERR_SSL_WANT_WRITE) {
            continue;
        }
        return std::unexpected(SocketError{ToSocketKind(rc),
                                           "tls write: " + MbedErrorText(rc)});
    }
    return {};
}

void TlsClientStream::CancelUnderlying() {
    auto* context = static_cast<TlsContext*>(context_);
    if (context == nullptr) {
        return;
    }
    context->sock.ShutdownBoth();
}

void TlsClientStream::CloseNotify() {
    auto* context = static_cast<TlsContext*>(context_);
    if (context == nullptr) {
        return;
    }
    (void)mbedtls_ssl_close_notify(&context->ssl);
}

}  // namespace lubancode::channel::qq
