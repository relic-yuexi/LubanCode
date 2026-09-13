// mbedTLS 客户端 TLS 流(QQ 机器人接入单 Q1):wss 的 TLS 半层。
//
// 定案见 todo §十五:mbedTLS 3.6(LTS)FetchContent 自建,三平台同源码。
// 这里只包客户端角色:连接既有 TcpSocket 之上做握手、读写、close_notify。
// 证书验证恒 REQUIRED:CA 桩 PEM 由调用方注入(测试用自签根;生产默认探测
// 平台 PEM 路径,真锚核验归 Q3 联调——测试绿不等于真平台 TLS 联调过)。
//
// 线程模型:一只实例归一只线程(单连接单线程);内部 entropy/ctr_drbg
// 每实例私有,不共享。密钥/证书不落日志,错误 detail 只带 mbed 错误码与
// verify flags。
#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <string>
#include <string_view>

#include "channel/qq/qq_socket.hpp"

namespace mbedtls {
typedef struct ssl_context ssl_context;
typedef struct ssl_config ssl_config;
typedef struct x509_crt x509_crt;
typedef struct ctr_drbg_context ctr_drbg_context;
typedef struct entropy_context entropy_context;
}  // namespace mbedtls

namespace lubancode::channel::qq {

// 平台默认信任 PEM 的探测路径(macOS /etc/ssl/cert.pem;Linux 各发行版
// ca-certificates 常见两处;Windows 无系统 PEM 文件——走随包 bundle 或
// 显式配置)。返回空串 = 没探测到,由调用方决定是否放行装配。
std::string DetectSystemCaPemPath();

enum class TlsErrorKind {
    HandshakeFailed,
    CertVerifyFailed,
    Failed,
};

struct TlsError {
    TlsErrorKind kind = TlsErrorKind::Failed;
    std::string detail;
};

class TlsClientStream {
public:
    TlsClientStream() = default;
    ~TlsClientStream();
    TlsClientStream(TlsClientStream&& other) noexcept;
    TlsClientStream& operator=(TlsClientStream&& other) noexcept;
    TlsClientStream(const TlsClientStream&) = delete;
    TlsClientStream& operator=(const TlsClientStream&) = delete;

    // 在已连的 sock 上做 TLS 握手。host 同时做 SNI 与证书主机名验证。
    // ca_pem 为 PEM 拼串(可含多证);验证恒 REQUIRED,不提供降级开关。
    static std::expected<TlsClientStream, TlsError> Connect(TcpSocket* sock,
                                                            const std::string& host,
                                                            const std::string& ca_pem,
                                                            int handshake_timeout_ms);

    bool valid() const { return ssl_ != nullptr; }

    // 语义同 TcpSocket::ReadSome/WriteAll(分型沿用 SocketError)。
    std::expected<std::size_t, SocketError> ReadSome(char* buf, std::size_t len,
                                                     int timeout_ms) const;
    std::expected<void, SocketError> WriteAll(std::string_view bytes, int timeout_ms) const;

    // 尽力发 close_notify 后不再可用(不关底层 socket,socket 归调用方)。
    void CloseNotify();

private:
    void Cleanup();
    TlsClientStream(mbedtls::ssl_context* ssl, mbedtls::ssl_config* config,
                    mbedtls::x509_crt* ca, mbedtls::ctr_drbg_context* drbg,
                    mbedtls::entropy_context* entropy, TcpSocket* sock)
        : ssl_(ssl),
          config_(config),
          ca_(ca),
          drbg_(drbg),
          entropy_(entropy),
          sock_(sock) {}
    mbedtls::ssl_context* ssl_ = nullptr;
    mbedtls::ssl_config* config_ = nullptr;
    mbedtls::x509_crt* ca_ = nullptr;
    mbedtls::ctr_drbg_context* drbg_ = nullptr;
    mbedtls::entropy_context* entropy_ = nullptr;
    TcpSocket* sock_ = nullptr;
};

}  // namespace lubancode::channel::qq
