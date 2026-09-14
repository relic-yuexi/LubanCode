// WebSocket 客户端(QQ 机器人接入单 Q1):RFC 6455 客户端子集——HTTP 升级
// 握手、文本帧收发、Ping 自动回 Pong、Close 收发。
//
// 定案见 todo §十五。刻意不做:扩展协商(permessage-deflate 不请求,服务端
// RSV 非零由 ws_frame 按协议错断连)、并发(一只连接归一只线程——QQ 网关
// 线程;取消走 Cancel() 的底层 shutdown)、自动重连(退避归 qq_gateway 状态
// 机,不藏在传输层)。
//
// 验握手:Sec-WebSocket-Accept 必须 == base64(SHA1(key + GUID))(RFC 6455
// §4.2.2),不符按协议错断——不静默放行。
#pragma once

#include <cstdint>
#include <expected>
#include <optional>
#include <string>
#include <string_view>

#include "channel/qq/qq_socket.hpp"
#include "channel/qq/qq_tls.hpp"
#include "channel/qq/ws_frame.hpp"

namespace lubancode::channel::qq {

struct WsConnectOptions {
    std::string url;      // ws://host:port/path 或 wss://…
    std::string ca_pem;   // wss 时的信任锚 PEM(w 不用)
    int connect_timeout_ms = 10'000;
    int io_timeout_ms = 10'000;  // 单次 select 落锤
};

struct WsError {
    enum class Kind {
        Closed,    // 对端关流或发了 close 帧(close_code 带码)
        Timeout,
        Protocol,  // 握手/帧协议错
        Failed,    // 网络/IO 错
    };
    Kind kind = Kind::Failed;
    std::string detail;
    std::uint16_t close_code = 0;  // Kind::Closed 且对端带码时
};

// 单条握手响应/帧头部的读缓冲帽(8 KiB——网关握手响应远小于此)。
inline constexpr std::size_t kWsHandshakeHeaderCap = 8 * 1024;

class WsClient {
public:
    WsClient() = default;
    ~WsClient() = default;
    WsClient(WsClient&& other) noexcept = default;
    WsClient& operator=(WsClient&& other) noexcept = default;
    WsClient(const WsClient&) = delete;
    WsClient& operator=(const WsClient&) = delete;

    // 连接 + 升级握手。成功后连接就绪可收发。
    static std::expected<WsClient, WsError> Connect(const WsConnectOptions& options);

    bool valid() const { return socket_.valid(); }

    // 发一条文本帧(mask 随机)。
    std::expected<void, WsError> SendText(std::string_view text);

    // 收下一条完整消息。控制帧在内部消化:Ping 自动回 Pong;Pong 忽略;
    // Close 返回 Kind::Closed(不再自动回 close——调用方 Close() 收尾)。
    std::expected<std::string, WsError> ReadMessage(int timeout_ms);

    // 主动关闭:发 close 帧(尽力),再等对端 close(短超时,尽力),随后
    // socket 关闭由析构负责。重复调用幂等。
    std::expected<void, WsError> Close(std::uint16_t code, std::string_view reason);

    // 从另一线程取消在途 ReadMessage:shutdown 底层 socket,阻塞读立即以
    // Closed 返回。之后连接不可再用。
    void Cancel();

    // 观测:握手以来的收发计数(诊断,不含正文)。
    std::uint64_t sent_messages() const { return sent_messages_; }
    std::uint64_t received_messages() const { return received_messages_; }

private:
    WsClient(TcpSocket socket, std::optional<TlsClientStream> tls)
        : socket_(std::move(socket)), tls_(std::move(tls)) {}
    std::expected<std::size_t, SocketError> ReadSome(char* buf, std::size_t len,
                                                     int timeout_ms) const;
    std::expected<void, SocketError> WriteAll(std::string_view bytes, int timeout_ms) const;

    TcpSocket socket_;
    std::optional<TlsClientStream> tls_;
    // 未消费的握手响应尾巴(粘在响应头后的帧字节)。
    std::string pending_bytes_;
    // 帧解码状态跨 ReadMessage 调用保留(半帧/粘帧)。
    WsFrameDecoder decoder_;
    std::uint64_t sent_messages_ = 0;
    std::uint64_t received_messages_ = 0;
};

}  // namespace lubancode::channel::qq
