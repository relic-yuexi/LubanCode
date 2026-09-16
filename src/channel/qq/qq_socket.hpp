// 跨平台阻塞 TCP 字节流(QQ 机器人接入单 Q1):自实现 WS 客户端的传输半层。
//
// 只给 src/channel/qq 内部件用(engine 私件),接口刻意收窄:
//   - Connect:DNS(getaddrinfo)+ 非阻塞 connect + select 落锤,带超时;
//   - ReadSome/WriteAll:select 超时;ReadSome 返回 0 字节一律按 Closed 分型;
//   - ShutdownBoth:双向 shutdown——收线程阻塞在 ReadSome 上时,另一线程
//     shutdown 触发其立即返回(取消路径,QQ 网关停止时用);
//   - 不做重连、不做 TLS(TLS 在 qq_tls)、不做缓冲(拼帧在 ws_frame)。
//
// Windows 走 Winsock2(ws2_32 已链 engine),POSIX 走 sys/socket。错误只分
// 三型:Timeout/Closed/Failed;detail 不带目标地址正文以外的敏感值。
#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <string>
#include <string_view>

namespace lubancode::channel::qq {

enum class SocketErrorKind {
    Timeout,
    Closed,  // 对端关流(或本端已 shutdown)
    Failed,  // 连不上/读写出错/DNS 失败
};

struct SocketError {
    SocketErrorKind kind = SocketErrorKind::Failed;
    std::string detail;
};

class TcpSocket {
public:
    TcpSocket() = default;
    ~TcpSocket();
    TcpSocket(TcpSocket&& other) noexcept;
    TcpSocket& operator=(TcpSocket&& other) noexcept;
    TcpSocket(const TcpSocket&) = delete;
    TcpSocket& operator=(const TcpSocket&) = delete;

    // 连接 host:port(connect 阶段超时 connect_timeout_ms)。失败给分型错误。
    static std::expected<TcpSocket, SocketError> Connect(const std::string& host, int port,
                                                         int connect_timeout_ms);

    bool valid() const;

    // 收最多 len 字节(有多少收多少,不凑整帧——拼帧是 ws_frame 的事)。
    // 返回 0 不可能:对端关流按 Closed 分型返回错误。
    std::expected<std::size_t, SocketError> ReadSome(char* buf, std::size_t len,
                                                     int timeout_ms) const;
    // 全量写出(循环写,select 落锤每次 timeout_ms)。
    std::expected<void, SocketError> WriteAll(std::string_view bytes, int timeout_ms) const;

    // 双向 shutdown(不 close fd):把阻塞中的 ReadSome 打成 Closed。
    void ShutdownBoth();

    void Close();

    // 原生句柄(只读;A08 建立期取消用——取消方持句柄直接 shutdown,把
    // 连接方阻塞在 select/recv 的流程立即打断)。句柄随 Close 失效,登记
    // 方须保证 Close 前先撤销登记(见 WsConnectCancelState)。
    std::int64_t native_handle() const { return fd_; }

private:
    explicit TcpSocket(std::int64_t fd);
    std::int64_t fd_ = kInvalidFd;
    static constexpr std::int64_t kInvalidFd = -1;
};

// 对原生句柄直接双向 shutdown(跨线程取消建立中的连接用;A08)。
// 句柄必须仍在登记方的所有权内——关闭后的 fd 号可能被复用,登记方负责
// 在 Close 前撤销登记。
void ShutdownNativeFd(std::int64_t fd);

}  // namespace lubancode::channel::qq
