// mbedTLS 客户端 TLS 流(QQ 机器人接入单 Q1):wss 的 TLS 半层。
//
// 定案见 todo §十五:mbedTLS 3.6(LTS)FetchContent 自建,三平台同源码。
// 这里只包客户端角色:连接既有 TcpSocket 之上做握手、读写、close_notify。
// 证书验证恒 REQUIRED:CA 桩 PEM 由调用方注入(测试用自签根;生产默认探测
// 平台 PEM 路径,真锚核验归 Q3 联调——测试绿不等于真平台 TLS 联调过)。
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
#include <string>
#include <string_view>

#include "channel/qq/qq_socket.hpp"

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

    // 接管 socket 所有权并在其上做 TLS 握手。host 同时做 SNI 与证书主机名
    // 验证。ca_pem 为 PEM 拼串(可含多证);验证恒 REQUIRED,不提供降级开关。
    // 按值接管是刻意的:socket 与 mbedtls 上下文同住堆上 TlsContext——
    // BIO 回调的自引用指针在移动语义下必须锚在不动窝的实体上。
    static std::expected<TlsClientStream, TlsError> Connect(TcpSocket socket,
                                                            const std::string& host,
                                                            const std::string& ca_pem,
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
