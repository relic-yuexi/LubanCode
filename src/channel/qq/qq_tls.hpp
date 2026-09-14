// mbedTLS 客户端 TLS 流(QQ 机器人接入单 Q1):wss 的 TLS 半层。
//
// 定案见 todo §十五:mbedTLS 3.6(LTS)FetchContent 自建,三平台同源码。
// 这里只包客户端角色:连接既有 TcpSocket 之上做握手、读写、close_notify。
// 证书验证恒 REQUIRED,不提供降级开关;失败绝不改走明文。
//
// 信任锚两种模式(Windows 信任根单 §四):
//   - ExplicitCa:调用方显式给 PEM(测试注入自签根;生产测试位)。验证纯
//     mbedTLS(链/时间/主机名),三平台行为一致。
//   - SystemDefault:平台默认信任。Linux/macOS 探系统 PEM 路径;Windows 从
//     系统证书店(Root/Ca,CurrentUser+LocalMachine)导出信任根喂 mbedTLS,
//     并在 mbedTLS 握手验证里接 Windows 链构建 + SSL 策略校验
//     (CertGetCertificateChain + CertVerifyCertificateChainPolicy):主机名/
//     有效期/链信任/用途/显式不信任(Disallowed 店)以系统裁决为权威。
//     用户不必下载 PEM、造目录、设环境变量。
//
// 头不递 mbedtls include(engine 对 mbedTLS 是 PRIVATE 链,头一传染,消费方
// 就得都链):内部上下文以 void* 隐藏,生命周期归本类(.cpp 内定义实体)。
//
// 线程模型:一只实例归一只线程(单连接单线程);entropy/ctr_drbg 每实例
// 私有,不共享。密钥/证书不落日志,错误 detail 只带 mbed 错误码与 flags。
#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <functional>
#include <string>
#include <string_view>

#include "channel/qq/qq_socket.hpp"

namespace lubancode::channel::qq {

// 平台默认信任 PEM 的探测路径(macOS /etc/ssl/cert.pem;Linux 各发行版
// ca-certificates 常见两处)。返回空串 = 没探测到。Windows 不走文件探测
// (系统证书店导出,见 ResolveChannelTrustRoots)。
std::string DetectSystemCaPemPath();

// TLS 稳定错误码(连接状态快照/日志用,见 Windows 信任根单 §四):
//   tls_trust_store_empty       信任根为空(没给锚也导不出系统根)
//   tls_trust_store_load_failed 信任根加载/解析失败(Windows 店打不开等)
//   tls_cert_expired            证书过期/未生效
//   tls_cert_hostname_mismatch  主机名不符
//   tls_cert_not_trusted        链不被信任
//   tls_cert_policy_rejected    系统策略拒(Windows SSL 策略/EKU/显式不信任)
//   tls_cert_verify_failed      其他证书验证失败(带 flags)
//   tls_handshake_timeout       握手超时
//   tls_handshake_failed        其他握手失败
inline constexpr char kTlsCodeTrustStoreEmpty[] = "tls_trust_store_empty";
inline constexpr char kTlsCodeTrustStoreLoadFailed[] = "tls_trust_store_load_failed";
inline constexpr char kTlsCodeCertExpired[] = "tls_cert_expired";
inline constexpr char kTlsCodeCertHostnameMismatch[] = "tls_cert_hostname_mismatch";
inline constexpr char kTlsCodeCertNotTrusted[] = "tls_cert_not_trusted";
inline constexpr char kTlsCodeCertPolicyRejected[] = "tls_cert_policy_rejected";
inline constexpr char kTlsCodeCertVerifyFailed[] = "tls_cert_verify_failed";
inline constexpr char kTlsCodeHandshakeTimeout[] = "tls_handshake_timeout";
inline constexpr char kTlsCodeHandshakeFailed[] = "tls_handshake_failed";

// mbedTLS 验证 flags -> 稳定错误码(优先级:主机名 > 过期 > 不信任 > 兜底)。
// 独立成纯函数供测试钉映射。
std::string TlsVerifyFlagsToCode(std::uint32_t flags);
// mbedTLS 握手错误码 -> 稳定错误码(只认超时;其余归 handshake_failed)。
std::string TlsHandshakeCodeToCode(int mbed_code);

// 信任锚模式(见文件头)。
enum class TlsTrustMode {
    ExplicitCa,
    SystemDefault,
};

// 渠道连接信任根的统一解析(wiring 装配时调一次;结果透给 transport
// factory)。explicit_ca_pem 非空 = 调用方全权指定信任锚(测试位),不回退
// 平台来源;无效(解析出 0 张证书)记入 error,由调用方明报,不静默退。
//   source 稳定名:explicit | windows_system_store | system_pem | none
struct ResolvedTrustStore {
    std::string ca_pem;         // 喂 mbedTLS 的 PEM 拼串(空 = 无可用信任根)
    std::string source;
    int certificate_count = 0;  // 解析出的证书张数
    std::string error;          // 非空 = 加载/解析失败(明报,不静默退)
    std::string detail;         // 人话(路径/店名,不含敏感内容)
};
// detect_pem_path 是探测 seam(测试注入;空 = 用 DetectSystemCaPemPath)。
ResolvedTrustStore ResolveChannelTrustRoots(const std::string& explicit_ca_pem,
                                            const std::function<std::string()>&
                                                detect_pem_path = nullptr);

enum class TlsErrorKind {
    HandshakeFailed,
    CertVerifyFailed,
    Failed,
};

struct TlsError {
    TlsErrorKind kind = TlsErrorKind::Failed;
    std::string detail;
    std::string error_code;  // kTlsCode* 稳定码(见上)
};

class TlsClientStream {
public:
    TlsClientStream() = default;
    ~TlsClientStream();
    TlsClientStream(TlsClientStream&& other) noexcept;
    TlsClientStream& operator=(TlsClientStream&& other) noexcept;
    TlsClientStream(const TlsClientStream&) = delete;
    TlsClientStream& operator=(const TlsClientStream&) = delete;

    // 接管 socket 所有权并在其上做 TLS 握手。host 同时做 SNI 与证书主机名
    // 验证。ca_pem 为 PEM 拼串(可含多证);验证恒 REQUIRED,不提供降级开关。
    // trust_mode 见文件头:SystemDefault 在 Windows 额外接系统策略校验。
    // 按值接管是刻意的:socket 与 mbedtls 上下文同住堆上 TlsContext——
    // BIO 回调的自引用指针在移动语义下必须锚在不动窝的实体上。
    static std::expected<TlsClientStream, TlsError> Connect(TcpSocket socket,
                                                            const std::string& host,
                                                            const std::string& ca_pem,
                                                            TlsTrustMode trust_mode,
                                                            int handshake_timeout_ms);

    bool valid() const { return context_ != nullptr; }

    // 语义同 TcpSocket::ReadSome/WriteAll(分型沿用 SocketError)。
    std::expected<std::size_t, SocketError> ReadSome(char* buf, std::size_t len,
                                                     int timeout_ms) const;
    std::expected<void, SocketError> WriteAll(std::string_view bytes, int timeout_ms) const;

    // 打断在途读写(shutdown 底层 socket;取消路径用)。
    void CancelUnderlying();

    // 尽力发 close_notify;socket 随本类析构关闭(所有权在此)。
    void CloseNotify();

private:
    TlsClientStream(void* context) : context_(context) {}
    void* context_ = nullptr;  // .cpp 内的 TlsContext 实体(mbedtls 全家桶+socket)
};

}  // namespace lubancode::channel::qq
