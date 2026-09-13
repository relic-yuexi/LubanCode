#include "channel/qq/qq_socket.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
using socklen_t = int;
#else
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>
#endif

namespace lubancode::channel::qq {

namespace {

#ifdef _WIN32
using NativeSocket = SOCKET;
constexpr NativeSocket kInvalidNative = INVALID_SOCKET;
int LastSocketError() { return WSAGetLastError(); }
bool WouldBlock(int err) { return err == WSAEWOULDBLOCK || err == WSAEINPROGRESS; }
#else
using NativeSocket = int;
constexpr NativeSocket kInvalidNative = -1;
int LastSocketError() { return errno; }
bool WouldBlock(int err) { return err == EINPROGRESS || err == EWOULDBLOCK; }
#endif

// Windows 一次性 WSAStartup(引用计数由我们自己那只静态守;POSIX 空)。
struct WinsockGuard {
#ifdef _WIN32
    bool ok = false;
    WinsockGuard() {
        WSADATA data;
        ok = WSAStartup(MAKEWORD(2, 2), &data) == 0;
    }
    ~WinsockGuard() {
        if (ok) {
            WSACleanup();
        }
    }
#else
    static constexpr bool ok = true;
#endif
};

bool EnsureSocketRuntime() {
    static WinsockGuard guard;
    return guard.ok;
}

void CloseNative(NativeSocket sock) {
    if (sock == kInvalidNative) {
        return;
    }
#ifdef _WIN32
    closesocket(sock);
#else
    ::close(sock);
#endif
}

std::string ErrnoText(int err) {
#ifdef _WIN32
    char buf[64] = {0};
    std::snprintf(buf, sizeof(buf), "wsa %d", err);
    return buf;
#else
    return std::string(std::strerror(err)) + " (" + std::to_string(err) + ")";
#endif
}

}  // namespace

TcpSocket::TcpSocket(std::int64_t fd) : fd_(fd) {}

TcpSocket::~TcpSocket() { Close(); }

TcpSocket::TcpSocket(TcpSocket&& other) noexcept : fd_(other.fd_) { other.fd_ = kInvalidFd; }

TcpSocket& TcpSocket::operator=(TcpSocket&& other) noexcept {
    if (this != &other) {
        Close();
        fd_ = other.fd_;
        other.fd_ = kInvalidFd;
    }
    return *this;
}

bool TcpSocket::valid() const { return fd_ != kInvalidFd; }

std::expected<TcpSocket, SocketError> TcpSocket::Connect(const std::string& host, int port,
                                                         int connect_timeout_ms) {
    if (!EnsureSocketRuntime()) {
#ifdef _WIN32
        return std::unexpected(SocketError{SocketErrorKind::Failed, "wsa init failed"});
#else
        return std::unexpected(SocketError{SocketErrorKind::Failed, "socket init failed"});
#endif
    }

    // DNS 解析(阻塞;地址串直连也走 getaddrinfo 统一处理)。
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo* result = nullptr;
    const std::string port_text = std::to_string(port);
    const int gai = ::getaddrinfo(host.c_str(), port_text.c_str(), &hints, &result);
    if (gai != 0) {
        return std::unexpected(SocketError{
            SocketErrorKind::Failed, "dns failed: " + std::string(gai_strerror(gai))});
    }

    std::string last_error = "no address";
    for (addrinfo* ai = result; ai != nullptr; ai = ai->ai_next) {
        NativeSocket sock = ::socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (sock == kInvalidNative) {
            last_error = "socket() failed: " + ErrnoText(LastSocketError());
            continue;
        }
        // 非阻塞 connect + select 落锤:Windows 的 connect 不吃 SO_SNDTIMEO。
#ifdef _WIN32
        u_long nonblocking = 1;
        ioctlsocket(sock, FIONBIO, &nonblocking);
#else
        const int flags = fcntl(sock, F_GETFL, 0);
        fcntl(sock, F_SETFL, flags | O_NONBLOCK);
#endif
        const int rc = ::connect(sock, ai->ai_addr, static_cast<socklen_t>(ai->ai_addrlen));
        bool connected = false;
        if (rc == 0) {
            connected = true;
        } else if (WouldBlock(LastSocketError())) {
            fd_set write_set;
            FD_ZERO(&write_set);
            FD_SET(sock, &write_set);
            timeval tv{};
            tv.tv_sec = connect_timeout_ms / 1000;
            tv.tv_usec = (connect_timeout_ms % 1000) * 1000;
            const int ready = ::select(static_cast<int>(sock) + 1, nullptr, &write_set, nullptr,
                                       &tv);
            if (ready > 0) {
                int so_error = 0;
                socklen_t optlen = sizeof(so_error);
                ::getsockopt(sock, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&so_error),
                             &optlen);
                connected = so_error == 0;
                if (!connected) {
                    last_error = "connect failed: " + ErrnoText(so_error);
                }
            } else if (ready == 0) {
                last_error = "connect timeout";
            } else {
                last_error = "select failed: " + ErrnoText(LastSocketError());
            }
        } else {
            last_error = "connect failed: " + ErrnoText(LastSocketError());
        }

        if (connected) {
            // 还原阻塞模式:后续读写用 select 自己控超时。
#ifdef _WIN32
            u_long blocking = 0;
            ioctlsocket(sock, FIONBIO, &blocking);
#else
            const int flags = fcntl(sock, F_GETFL, 0);
            fcntl(sock, F_SETFL, flags & ~O_NONBLOCK);
#endif
#ifdef __APPLE__
            // macOS 无 MSG_NOSIGNAL:向已断开的对端写默认发 SIGPIPE 杀进程,
            // socket 级关掉(对端关闭后 write 走 EPIPE 错误分型)。
            int nosigpipe = 1;
            ::setsockopt(sock, SOL_SOCKET, SO_NOSIGPIPE, &nosigpipe, sizeof(nosigpipe));
#endif
            // 关 Nagle:网关心跳与事件都是小帧,攒包只会白添延迟。
            int nodelay = 1;
            ::setsockopt(sock, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&nodelay),
                         sizeof(nodelay));
            ::freeaddrinfo(result);
            return TcpSocket(static_cast<std::int64_t>(sock));
        }
        CloseNative(sock);
    }
    ::freeaddrinfo(result);
    return std::unexpected(SocketError{SocketErrorKind::Failed, last_error});
}

std::expected<std::size_t, SocketError> TcpSocket::ReadSome(char* buf, std::size_t len,
                                                            int timeout_ms) const {
    if (fd_ == kInvalidFd) {
        return std::unexpected(SocketError{SocketErrorKind::Closed, "socket not open"});
    }
    const NativeSocket sock = static_cast<NativeSocket>(fd_);
    fd_set read_set;
    FD_ZERO(&read_set);
    FD_SET(sock, &read_set);
    timeval tv{};
    tv.tv_sec = timeout_ms / 1000;
    tv.tv_usec = (timeout_ms % 1000) * 1000;
    const int ready = ::select(static_cast<int>(sock) + 1, &read_set, nullptr, nullptr, &tv);
    if (ready == 0) {
        return std::unexpected(SocketError{SocketErrorKind::Timeout, "read timeout"});
    }
    if (ready < 0) {
        const int err = LastSocketError();
        return std::unexpected(SocketError{SocketErrorKind::Failed,
                                           "select failed: " + ErrnoText(err)});
    }
    const int received = static_cast<int>(
        ::recv(sock, buf, static_cast<int>(std::min<std::size_t>(len, 0x3FFFFFFF)), 0));
    if (received == 0) {
        return std::unexpected(SocketError{SocketErrorKind::Closed, "peer closed"});
    }
    if (received < 0) {
        const int err = LastSocketError();
        const bool closed = (err == ECONNRESET) || (err == EPIPE) ||
#ifdef _WIN32
                            (err == WSAECONNABORTED) || (err == WSAESHUTDOWN);
#else
                            (err == ESHUTDOWN);
#endif
        return std::unexpected(SocketError{
            closed ? SocketErrorKind::Closed : SocketErrorKind::Failed,
            "recv failed: " + ErrnoText(err)});
    }
    return static_cast<std::size_t>(received);
}

std::expected<void, SocketError> TcpSocket::WriteAll(std::string_view bytes,
                                                     int timeout_ms) const {
    if (fd_ == kInvalidFd) {
        return std::unexpected(SocketError{SocketErrorKind::Closed, "socket not open"});
    }
    const NativeSocket sock = static_cast<NativeSocket>(fd_);
    std::size_t sent = 0;
    while (sent < bytes.size()) {
        fd_set write_set;
        FD_ZERO(&write_set);
        FD_SET(sock, &write_set);
        timeval tv{};
        tv.tv_sec = timeout_ms / 1000;
        tv.tv_usec = (timeout_ms % 1000) * 1000;
        const int ready =
            ::select(static_cast<int>(sock) + 1, nullptr, &write_set, nullptr, &tv);
        if (ready == 0) {
            return std::unexpected(SocketError{SocketErrorKind::Timeout, "write timeout"});
        }
        if (ready < 0) {
            return std::unexpected(SocketError{SocketErrorKind::Failed,
                                               "select failed: " + ErrnoText(LastSocketError())});
        }
#ifdef MSG_NOSIGNAL
        const int send_flags = MSG_NOSIGNAL;  // Linux:写断开的对端不杀进程
#else
        const int send_flags = 0;  // macOS 走 SO_NOSIGPIPE;Windows 无此问题
#endif
        const int written = static_cast<int>(::send(
            sock, bytes.data() + sent,
            static_cast<int>(std::min<std::size_t>(bytes.size() - sent, 0x3FFFFFFF)),
            send_flags));
        if (written <= 0) {
            const int err = LastSocketError();
            return std::unexpected(SocketError{
                SocketErrorKind::Closed,
                "send failed: " + ErrnoText(err)});
        }
        sent += static_cast<std::size_t>(written);
    }
    return {};
}

void TcpSocket::ShutdownBoth() {
    if (fd_ == kInvalidFd) {
        return;
    }
    ::shutdown(static_cast<NativeSocket>(fd_), 2 /* SD_BOTH / SHUT_RDWR */);
}

void TcpSocket::Close() {
    if (fd_ != kInvalidFd) {
        CloseNative(static_cast<NativeSocket>(fd_));
        fd_ = kInvalidFd;
    }
}

}  // namespace lubancode::channel::qq
