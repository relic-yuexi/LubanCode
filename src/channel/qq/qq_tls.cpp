#include "channel/qq/qq_tls.hpp"

#include <algorithm>
#include <cstdio>
#include <map>
#include <memory>
#include <set>
#include <string_view>
#include <utility>
#include <vector>

#include <mbedtls/ctr_drbg.h>
#include <mbedtls/entropy.h>
#include <mbedtls/error.h>
#include <mbedtls/net_sockets.h>  // MBEDTLS_ERR_NET_* 错误码
#include <mbedtls/sha256.h>
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

// Windows SSL 策略校验的回调状态(mbedtls_ssl_conf_verify 的 p_ctx)。
// 声明在 TlsContext 前(它作为堆成员住进去,§五生命期)。
#ifdef _WIN32
struct WindowsVerifyContext {
    std::string reject_code;  // 系统拒绝时的稳定码(kTlsCode*)
};
#endif

// mbedtls 全家桶(实体只在 .cpp 可见,头里是 void*)。socket 也住这里:
// BIO 回调指针锚在堆上不动窝的实体,移动 TlsClientStream 不断链。
struct TlsContext {
    mbedtls_ssl_context ssl{};
    mbedtls_ssl_config config{};
    mbedtls_x509_crt ca{};
    mbedtls_ctr_drbg_context drbg{};
    mbedtls_entropy_context entropy{};
    TcpSocket sock{};
#ifdef _WIN32
    // 验证回调状态(§五 P1):ssl/config 在堆上活多久它就活多久——栈对象
    // 交出去会在 Connect 返回后悬空;unique_ptr 再堆一层,移动
    // TlsClientStream(搬 TlsContext*)不改回调目标地址。
    std::unique_ptr<WindowsVerifyContext> windows_verify;
#endif

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

// ---------------------------------------------------------------------------
// 信任根加载合同(证书部分解析误判修复单 §三)
//
// mbedTLS v3.6.3 批量 parse 合同:0 = 全部成功;正数 = 未解析成功的证书
// 数量,已成功项保留在链上;负数 = 错误(含内存错误),全败。旧代码把"任何
// 非零"折叠成 -1 再误报 trust_store_empty——Windows 导出 157 张里一张坏
// 证(正数返回)就把整批可用信任根判死。这里按合同逐项分账。
// ---------------------------------------------------------------------------

// 链上张数(raw.len>0 的节点;parse 部分成功时链上只留成功项)。
int CountChainCertificates(const mbedtls_x509_crt* chain) {
    int count = 0;
    for (const mbedtls_x509_crt* it = chain; it != nullptr; it = it->next) {
        if (it->raw.len > 0) {
            ++count;
        }
    }
    return count;
}

// rc + 成功链张数 -> 加载判定。连接路径与装配预检(EvaluateTrustLoad)
// 共用这一张表,不两套判断走岔。
//   strict(显式 CA):任何失败张都拒——显式锚是调用方全权指定的干净集合,
//   部分成功也报 load_failed,不回退系统证书。
//   系统集合:部分兼容——rc>0 且成功链非空时保留成功链继续正常证书验证
//   ("解析跳过"绝不变成"跳过对端验证");rc<0(含内存错误)全败,不因链中
//   残留项继续。
struct LoadVerdict {
    bool ok = false;
    bool partial = false;
    int failed = 0;
};
LoadVerdict VerdictOfParse(int rc, int parsed_in_chain, bool strict) {
    LoadVerdict verdict;
    verdict.failed = rc > 0 ? rc : 0;
    if (rc < 0) {
        return verdict;  // 负数:错误(含 ALLOC 失败),全败
    }
    if (rc == 0 && parsed_in_chain > 0) {
        verdict.ok = true;  // 全部成功
        return verdict;
    }
    if (rc > 0 && parsed_in_chain > 0) {
        if (strict) {
            return verdict;  // 显式锚严格:部分成功也拒
        }
        verdict.ok = true;  // 系统集合部分兼容:保留成功链
        verdict.partial = true;
        return verdict;
    }
    // 剩余情形:链空(输入空/全部失败/正数但成功链为空)一律不可继续。
    return verdict;
}

// 证书 DER 的 SHA-256 指纹(十六进制,前 16 字节——诊断够用,不输出整证)。
std::string CertFingerprintHex(const unsigned char* der, std::size_t len) {
    unsigned char digest[32] = {0};
    mbedtls_sha256(der, len, digest, /*is_sha224=*/0);
    static const char kHex[] = "0123456789abcdef";
    std::string out;
    out.reserve(32);
    for (std::size_t i = 0; i < 16; ++i) {
        out.push_back(kHex[digest[i] >> 4]);
        out.push_back(kHex[digest[i] & 0xF]);
    }
    return out;
}

// PEM 段(完整 BEGIN..END 边界)文本的 SHA-256 指纹——导出去重与坏证
// 诊断的统一口径:自家导出的 PEM 段与 DER 一一对应,段文本哈希即证书
// 指纹;坏证 parse 不动时 DER 拿不到,段文本指纹仍可定位是哪一张。
std::string PemBlockFingerprint(const std::string& pem_block) {
    std::string_view text(pem_block);
    while (!text.empty() && (text.front() == '\n' || text.front() == '\r')) {
        text.remove_prefix(1);
    }
    while (!text.empty() && (text.back() == '\n' || text.back() == '\r')) {
        text.remove_suffix(1);
    }
    return CertFingerprintHex(reinterpret_cast<const unsigned char*>(text.data()), text.size());
}

// 链上证书按指纹去重后的张数(同证跨店重复导出只记一张)。
int CountUniqueByFingerprint(const mbedtls_x509_crt* chain) {
    std::set<std::string> seen;
    for (const mbedtls_x509_crt* it = chain; it != nullptr; it = it->next) {
        if (it->raw.len > 0) {
            seen.insert(CertFingerprintHex(it->raw.p, it->raw.len));
        }
    }
    return static_cast<int>(seen.size());
}

// 有界坏证诊断条数上限(§三:限条数,不输出 PEM/主体/整店清单)。
constexpr int kMaxBadCertNotes = 3;

// 拆 PEM 拼串为单证 PEM 段(含 BEGIN/END 行),供逐证定位坏证。拆不出
// 边界的段(纯文本垃圾)原样成段——parse 自会给出负码。
std::vector<std::string> SplitPemCertificates(const std::string& ca_pem) {
    std::vector<std::string> parts;
    std::string_view rest(ca_pem);
    while (true) {
        const std::size_t begin = rest.find("-----BEGIN CERTIFICATE-----");
        if (begin == std::string::npos) {
            break;
        }
        const std::size_t end = rest.find("-----END CERTIFICATE-----", begin);
        if (end == std::string::npos) {
            parts.emplace_back(rest.substr(begin));
            break;
        }
        const std::size_t part_end = end + std::string_view("-----END CERTIFICATE-----").size();
        parts.emplace_back(rest.substr(begin, part_end - begin));
        rest = rest.substr(part_end);
    }
    if (parts.empty() && !ca_pem.empty()) {
        parts.emplace_back(ca_pem);  // 无 PEM 边界:整段喂(负码即诊断)
    }
    return parts;
}

// 逐证解析定位坏证(有界诊断路径,只在装配预检走一次):记录段指纹
//(PemBlockFingerprint 口径)与负错误码。指纹是"这张证书"的标识——
// 不输出 PEM、主体或整店清单。
void DiagnoseBadCertificates(const std::string& ca_pem, TrustLoadReport& report) {
    for (const std::string& part : SplitPemCertificates(ca_pem)) {
        if (static_cast<int>(report.bad_cert_notes.size()) >= kMaxBadCertNotes) {
            break;
        }
        mbedtls_x509_crt single;
        mbedtls_x509_crt_init(&single);
        const int rc = mbedtls_x509_crt_parse(
            &single, reinterpret_cast<const unsigned char*>(part.data()), part.size() + 1);
        const bool bad = rc != 0 || single.raw.len == 0;
        if (bad) {
            if (report.first_negative_rc == 0 && rc < 0) {
                report.first_negative_rc = rc;
            }
            // 指纹用段文本口径(PemBlockFingerprint):Windows 导出侧同口径
            // 记账,Resolve 能按它定位来源店。
            std::string note =
                "证书指纹 sha256:" + PemBlockFingerprint(part) + " mbedTLS rc=" + std::to_string(rc);
            report.bad_cert_notes.push_back(std::move(note));
        }
        mbedtls_x509_crt_free(&single);
    }
}

#ifdef _WIN32

// ---------------------------------------------------------------------------
// Windows 系统信任(Windows 信任根单 §四)
// ---------------------------------------------------------------------------

// Windows 证书店的证 -> PEM 拼串。店:Root 与 Ca(中间),CurrentUser 与
// LocalMachine 各开一遍(Disallowed 里的证跳过——显式不信任的锚不喂
// mbedTLS;链构建侧还会再拦一道)。打不开店不致命:跳过该店继续拼。
// Root 与 Ca 来源分开记;同证跨店/跨位置重复导出按 SHA-256 指纹去重
//(§三:导出数与去重数分账,不重复计数)。
struct WindowsTrustExport {
    std::string pem;
    int count = 0;               // 去重后导出条目(喂解析的就是这些)
    int raw_count = 0;           // 去重前枚举条目(诊断用)
    int root_count = 0;          // Root 店去重后条目
    int ca_count = 0;            // Ca 店去重后条目
    int skipped_disallowed = 0;
    std::string first_error;     // 首个店打开失败(诊断)
    std::map<std::string, std::string> fingerprint_store;  // 指纹 -> 店标签(坏证诊断定位来源)
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
                                     const std::string& store_label,
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
        // 先拼 PEM 段再按段指纹去重(§三):同证跨店/跨位置只导出一次;
        // 指纹与坏证诊断同口径(PemBlockFingerprint),可按它定位来源店。
        std::string pem_block = "-----BEGIN CERTIFICATE-----\n";
        {
            const std::string_view der(reinterpret_cast<const char*>(cert->pbCertEncoded),
                                       cert->cbCertEncoded);
            std::string encoded = platform::Base64Encode(der);
            for (std::size_t i = 0; i < encoded.size(); i += 64) {
                pem_block += encoded.substr(i, 64);
                pem_block += "\n";
            }
            pem_block += "-----END CERTIFICATE-----";
        }
        const std::string fingerprint = PemBlockFingerprint(pem_block);
        ++out.raw_count;
        if (out.fingerprint_store.find(fingerprint) != out.fingerprint_store.end()) {
            continue;  // 已从别家店导出过同一张
        }
        out.pem += pem_block;
        out.pem += "\n";
        ++out.count;
        out.fingerprint_store[fingerprint] = store_label;
        if (store_label.rfind("Root", 0) == 0) {
            ++out.root_count;
        } else {
            ++out.ca_count;
        }
        // 此处到下一次推进前 cert 仍有效;推进语句释放它,PEM 已拷出。
    }
    CertCloseStore(store, 0);
}

WindowsTrustExport ExportWindowsTrustRoots() {
    WindowsTrustExport out;
    AppendStoreCertificatesToExport(L"Root", CERT_SYSTEM_STORE_CURRENT_USER,
                                    "Root/CurrentUser", out);
    AppendStoreCertificatesToExport(L"Root", CERT_SYSTEM_STORE_LOCAL_MACHINE,
                                    "Root/LocalMachine", out);
    AppendStoreCertificatesToExport(L"Ca", CERT_SYSTEM_STORE_CURRENT_USER, "Ca/CurrentUser",
                                    out);
    AppendStoreCertificatesToExport(L"Ca", CERT_SYSTEM_STORE_LOCAL_MACHINE,
                                    "Ca/LocalMachine", out);
    return out;
}

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
//
// §五核查记录(证书部分解析误判修复单):链来源 = 系统默认引擎
//(hChainEngine=NULL)自动构建——覆盖系统证书店全集 + AIA 网络取中间
// 证,超时未参数化(走系统默认,SDK 兼容口径如上,如实记"未覆盖参数化
// 超时/取消");用途 = CERT_CHAIN_POLICY_SSL 基础策略(EKU/服务器鉴定);
// 深度 = 只在 leaf(depth 0) 跑一次,其余 depth 不动 flags。本回调绝不
// 清空全部 flags 求成功——系统裁决通过只放行"链不可信"位(导入子集缺
// 中间证书/根的误报),主机名/时间/用途位保留。
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

TrustLoadReport EvaluateTrustLoad(const std::string& source, const std::string& ca_pem) {
    TrustLoadReport report;
    report.source = source;
    report.input_empty = ca_pem.empty();
    if (ca_pem.empty()) {
        report.error = "信任根输入为空";
        return report;
    }
    mbedtls_x509_crt chain;
    mbedtls_x509_crt_init(&chain);
    const int rc = mbedtls_x509_crt_parse(
        &chain, reinterpret_cast<const unsigned char*>(ca_pem.data()), ca_pem.size() + 1);
    report.parse_rc = rc;
    const int parsed = CountChainCertificates(&chain);
    report.parsed_count = parsed;
    report.failed_count = rc > 0 ? rc : 0;
    // 评估侧输入条目 = 成功张 + 失败张(Windows 导出侧会覆盖为真实导出数)。
    report.exported_count = parsed + report.failed_count;
    report.unique_count = CountUniqueByFingerprint(&chain);
    mbedtls_x509_crt_free(&chain);  // 报告只记数与码,不持有链

    const bool strict = source == "explicit";
    const LoadVerdict verdict = VerdictOfParse(rc, parsed, strict);
    report.ok_to_continue = verdict.ok;
    report.partial = verdict.partial;
    if (rc < 0) {
        // 负数 = 错误(含内存错误):全败,不因链中残留项继续。
        report.first_negative_rc = rc;
        report.error = "非空证书集合解析失败,mbedTLS 错误码 " + std::to_string(rc) + " (" +
                       MbedErrorText(rc) + ")";
        return report;
    }
    if (!verdict.ok) {
        if (strict && report.failed_count > 0) {
            report.error = "显式信任锚含 " + std::to_string(report.failed_count) +
                           " 张坏证——严格拒绝,不回退系统证书";
        } else if (report.failed_count > 0) {
            report.error = "证书全部解析失败:共 " + std::to_string(report.failed_count) + " 张";
        } else {
            report.error = "解析出 0 张证书";
        }
        if (report.failed_count > 0) {
            DiagnoseBadCertificates(ca_pem, report);
        }
        return report;
    }
    if (verdict.partial) {
        // 系统集合部分兼容:保留成功链、警告失败张,仍校验对端证书——
        // "解析跳过"绝不变成"跳过对端验证"。
        report.warning = "部分加载:跳过 " + std::to_string(report.failed_count) +
                         " 张,保留 " + std::to_string(parsed) + " 张,仍校验服务端证书";
        DiagnoseBadCertificates(ca_pem, report);
    }
    return report;
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
        // 显式信任锚:调用方全权指定,不回退平台来源;严格语义——含坏证
        // 即 load_failed,错误信息保留源头(§三)。
        resolved.source = "explicit";
        resolved.detail = "显式配置的信任锚";
        resolved.load = EvaluateTrustLoad("explicit", explicit_ca_pem);
        if (!resolved.load.ok_to_continue) {
            resolved.error = "explicit ca_pem: " + resolved.load.error;
            resolved.detail = "显式配置的信任锚(不可用)";
            if (!resolved.load.bad_cert_notes.empty()) {
                resolved.detail += ";" + resolved.load.bad_cert_notes[0];
            }
        } else {
            resolved.certificate_count = resolved.load.parsed_count;
            resolved.ca_pem = explicit_ca_pem;
        }
        return resolved;
    }
#ifdef _WIN32
    // Windows:导出后立即走与连接相同的解析/策略函数(§三:export 与 load
    // 分清)。启动诊断报"导出/去重/可解析/失败"四个数,不拿导出数冒充
    // 可用数;部分兼容时保留成功链、warning 明示,不拦装配(阻断重试归
    // 适配器的 trust 预检短路,§四)。
    const WindowsTrustExport exported = ExportWindowsTrustRoots();
    resolved.source = "windows_system_store";
    resolved.load = EvaluateTrustLoad("windows_system_store", exported.pem);
    resolved.load.exported_count = exported.raw_count;  // 导出侧真实枚举数
    if (exported.pem.empty()) {
        resolved.error =
            exported.first_error.empty()
                ? "Windows 系统证书库导出 0 张信任根"
                : "Windows 系统证书库打开失败: " + exported.first_error;
        return resolved;
    }
    if (!resolved.load.ok_to_continue) {
        resolved.error = "Windows 系统证书库: " + resolved.load.error;
        return resolved;
    }
    resolved.certificate_count = resolved.load.parsed_count;
    // 可继续(含部分):PEM 原样透传——mbedTLS 连接侧解析自动跳过坏证、
    // 保留成功项,装配到消费字节不变。
    resolved.ca_pem = exported.pem;
    resolved.detail = "Windows 系统证书库 Root " + std::to_string(exported.root_count) +
                      " 张/Ca " + std::to_string(exported.ca_count) + " 张(枚举 " +
                      std::to_string(exported.raw_count) + ",去重 " +
                      std::to_string(exported.count) + ",可解析 " +
                      std::to_string(resolved.load.parsed_count);
    if (resolved.load.failed_count > 0) {
        resolved.detail += ",跳过 " + std::to_string(resolved.load.failed_count);
    }
    if (exported.skipped_disallowed > 0) {
        resolved.detail += ";跳过 Disallowed " + std::to_string(exported.skipped_disallowed) +
                           " 张";
    }
    if (!exported.first_error.empty()) {
        resolved.detail += ";" + exported.first_error + "(该店跳过)";
    }
    resolved.detail += ")";
    // 坏证 note 按段指纹对账来源店(§三:指纹+来源店+负码三件套)——
    // 指纹对不上(非本进程导出的集合)就不标,如实只有指纹与负码。
    for (std::string& note : resolved.load.bad_cert_notes) {
        for (const auto& [fingerprint, store] : exported.fingerprint_store) {
            if (note.find(fingerprint) != std::string::npos) {
                note += " 来源=" + store;
                break;
            }
        }
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
    // 与连接相同的解析/策略函数(§三):部分兼容(混合 CA 文件含坏证)保留
    // 成功链 + warning;非空但解析失败报 load_failed 不报 empty。
    resolved.load = EvaluateTrustLoad("system_pem", content);
    resolved.load.exported_count = resolved.load.parsed_count + resolved.load.failed_count;
    if (!resolved.load.ok_to_continue) {
        resolved.source = "none";
        resolved.error = "系统 CA PEM " + resolved.load.error + ": " + detected;
        return resolved;
    }
    resolved.certificate_count = resolved.load.parsed_count;
    resolved.source = "system_pem";
    resolved.ca_pem = std::move(content);
    resolved.detail = detected + "(可解析 " + std::to_string(resolved.load.parsed_count) + " 张";
    if (resolved.load.failed_count > 0) {
        resolved.detail += ",跳过 " + std::to_string(resolved.load.failed_count) + " 张";
    }
    resolved.detail += ")";
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
    // 信任根加载:一次 parse 进将用于握手的链(去掉旧"先计数释放再解析"
    // 的重复解析),按加载合同判定——与 EvaluateTrustLoad 同一张判定表
    //(VerdictOfParse),不两套判断走岔(§三)。空输入才报 empty;非空但
    // 解析失败/全坏报 load_failed 带真实返回码;系统模式部分成功保留
    // 成功链继续验证,显式模式严格拒绝。
    if (ca_pem.empty()) {
        return fail(TlsErrorKind::Failed, "trust store empty: no CA certificates provided",
                    kTlsCodeTrustStoreEmpty);
    }
    rc = mbedtls_x509_crt_parse(&context->ca,
                                reinterpret_cast<const unsigned char*>(ca_pem.data()),
                                ca_pem.size() + 1);
    const int parsed_in_chain = CountChainCertificates(&context->ca);
    const LoadVerdict verdict =
        VerdictOfParse(rc, parsed_in_chain, trust_mode == TlsTrustMode::ExplicitCa);
    if (!verdict.ok) {
        if (rc < 0) {
            return fail(TlsErrorKind::Failed,
                        "ca load: 非空证书集合解析失败 rc=" + std::to_string(rc) + " (" +
                            MbedErrorText(rc) + ")",
                        kTlsCodeTrustStoreLoadFailed);
        }
        if (verdict.failed > 0 && parsed_in_chain > 0) {
            return fail(TlsErrorKind::Failed,
                        "ca load: 显式信任锚含 " + std::to_string(verdict.failed) +
                            " 张坏证——严格拒绝,不回退系统证书(rc=" + std::to_string(rc) +
                            ",已解析 " + std::to_string(parsed_in_chain) + " 张不喂)",
                        kTlsCodeTrustStoreLoadFailed);
        }
        return fail(TlsErrorKind::Failed,
                    "ca load: 全部 " + std::to_string(verdict.failed) +
                        " 张解析失败(rc=" + std::to_string(rc) + ")",
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
    if (trust_mode == TlsTrustMode::SystemDefault) {
        // §五 P1:回调状态住进 TlsContext(堆上,随连接一起释放)——栈对象
        // 交出去会在 Connect 返回后悬空;unique_ptr 再堆一层,移动
        // TlsClientStream(只搬 TlsContext*)不改回调目标地址。
        context->windows_verify = std::make_unique<WindowsVerifyContext>();
        mbedtls_ssl_conf_verify(&context->config, WindowsPolicyVerify,
                                context->windows_verify.get());
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
            // 稳定码以系统裁决为准(CERT_E_* -> kTlsCode*)。回调状态在
            // 堆上(§五),Connect 内引用不悬空。
            if (trust_mode == TlsTrustMode::SystemDefault &&
                context->windows_verify != nullptr &&
                !context->windows_verify->reject_code.empty()) {
                return fail(TlsErrorKind::CertVerifyFailed,
                            "windows policy rejected: flags=0x" + std::to_string(flags) +
                                " code=" + context->windows_verify->reject_code,
                            context->windows_verify->reject_code);
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
