// ws_sockets.hpp 的实现:Winsock2 与 POSIX 两份在同一文件里按平台择一
// (connection.cpp 的 ReadStdinChunk 同路数)。
#include "app_server/ws_sockets.hpp"

#include <cstring>

#if defined(_WIN32)
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>
#endif

namespace lubancode::app_server::net {

namespace {

#if defined(_WIN32)
using RawSocket = SOCKET;
constexpr RawSocket kInvalidRaw = INVALID_SOCKET;

std::string SocketErrorMessage() {
    const int code = WSAGetLastError();
    return "winsock 错误 " + std::to_string(code);
}
#else
using RawSocket = int;
constexpr RawSocket kInvalidRaw = -1;

std::string SocketErrorMessage() {
    return std::string(std::strerror(errno));
}
#endif

std::int64_t ToHandle(RawSocket raw) {
#if defined(_WIN32)
    return static_cast<std::int64_t>(static_cast<std::uintptr_t>(raw));
#else
    return static_cast<std::int64_t>(raw);
#endif
}

RawSocket FromHandle(std::int64_t handle) {
#if defined(_WIN32)
    return static_cast<RawSocket>(static_cast<std::uintptr_t>(handle));
#else
    return static_cast<RawSocket>(handle);
#endif
}

// host 文本 -> sockaddr_in。只认点分 IP 与 "localhost"(回环);不做完整
// DNS 解析——WS 承载的绑定面就这么宽,域名绑定不在首版账上。
bool ResolveBindAddress(const std::string& host, sockaddr_in& address, std::string& error) {
    const std::string target = host.empty() ? std::string("127.0.0.1") : host;
    std::memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_port = 0; // 端口由调用方填(网络序)
    if (target == "localhost") {
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        return true;
    }
    in_addr parsed{};
    if (inet_pton(AF_INET, target.c_str(), &parsed) != 1) {
        error = "绑定地址不是点分 IPv4 或 localhost: " + target;
        return false;
    }
    address.sin_addr = parsed;
    return true;
}

}  // namespace

bool Startup(std::string& error) {
#if defined(_WIN32)
    static bool initialized = [] {
        WSADATA data;
        return WSAStartup(MAKEWORD(2, 2), &data) == 0;
    }();
    if (!initialized) {
        error = SocketErrorMessage();
        return false;
    }
#else
    (void)0;
#endif
    return true;
}

Socket::~Socket() {
    Close();
}

Socket::Socket(Socket&& other) noexcept : handle_(other.handle_) {
    other.handle_ = kInvalidHandle;
}

Socket& Socket::operator=(Socket&& other) noexcept {
    if (this != &other) {
        Close();
        handle_ = other.handle_;
        other.handle_ = kInvalidHandle;
    }
    return *this;
}

void Socket::Close() {
    if (handle_ == kInvalidHandle) {
        return;
    }
#if defined(_WIN32)
    closesocket(FromHandle(handle_));
#else
    ::close(FromHandle(handle_));
#endif
    handle_ = kInvalidHandle;
}

long Socket::Recv(char* buffer, std::size_t capacity) {
    if (handle_ == kInvalidHandle) {
        return -1;
    }
    const int got = ::recv(FromHandle(handle_), buffer, static_cast<int>(capacity), 0);
    if (got < 0) {
        return -1;
    }
    return got;
}

bool Socket::SendAll(std::string_view bytes) {
    if (handle_ == kInvalidHandle) {
        return false;
    }
    std::size_t sent = 0;
    while (sent < bytes.size()) {
#if defined(_WIN32)
        const int chunk = ::send(FromHandle(handle_), bytes.data() + sent,
                                 static_cast<int>(bytes.size() - sent), 0);
#else
        // MSG_NOSIGNAL:WS 客户端断线后继续 send,POSIX 默认会递 SIGPIPE 把
        // 整个测试进程打死(CI ubuntu 的 app_server_ws SIGPIPE 实翻)。Linux
        // 走 flag;macOS 无此 flag,建 socket 时设 SO_NOSIGPIPE(见下)。
        const int chunk = ::send(FromHandle(handle_), bytes.data() + sent,
                                 static_cast<int>(bytes.size() - sent), MSG_NOSIGNAL);
#endif
        if (chunk <= 0) {
            return false;
        }
        sent += static_cast<std::size_t>(chunk);
    }
    return true;
}

void Socket::SetRecvTimeoutMs(int ms) {
    if (handle_ == kInvalidHandle) {
        return;
    }
#if defined(_WIN32)
    const DWORD milliseconds = static_cast<DWORD>(ms);
    ::setsockopt(FromHandle(handle_), SOL_SOCKET, SO_RCVTIMEO,
                 reinterpret_cast<const char*>(&milliseconds), sizeof(milliseconds));
#else
    timeval timeout{};
    timeout.tv_sec = ms / 1000;
    timeout.tv_usec = (ms % 1000) * 1000;
    ::setsockopt(FromHandle(handle_), SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
#endif
}

int Socket::LocalPort() const {
    if (handle_ == kInvalidHandle) {
        return 0;
    }
    sockaddr_in address{};
    socklen_t length = sizeof(address);
    if (::getsockname(FromHandle(handle_), reinterpret_cast<sockaddr*>(&address), &length) != 0) {
        return 0;
    }
    return ntohs(address.sin_port);
}

std::int64_t Socket::native() const {
    return handle_;
}

bool Listener::Start(const std::string& host, int port, std::string& error) {
    if (!Startup(error)) {
        return false;
    }
    sockaddr_in address{};
    if (!ResolveBindAddress(host, address, error)) {
        return false;
    }
    address.sin_port = htons(static_cast<std::uint16_t>(port));
    const RawSocket fd = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (fd == kInvalidRaw) {
        error = "socket 建不起: " + SocketErrorMessage();
        return false;
    }
#if defined(_WIN32)
    // Windows 的 SO_REUSEADDR 会放行端口劫持(别人正在用的端口也能绑),
    // 这里反着来:SO_EXCLUSIVEADDRUSE 把独占焊死。
    int exclusive = 1;
    ::setsockopt(fd, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, reinterpret_cast<const char*>(&exclusive),
                 sizeof(exclusive));
#else
    // POSIX 侧地址复用:测试反复起停同端口,TIME_WAIT 不许挡路。
    int reuse = 1;
    ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&reuse), sizeof(reuse));
#endif
    if (::bind(fd, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) != 0) {
        error = "bind 失败(" + (host.empty() ? std::string("127.0.0.1") : host) + ":" +
                std::to_string(port) + "): " + SocketErrorMessage();
#if defined(_WIN32)
        closesocket(fd);
#else
        ::close(fd);
#endif
        return false;
    }
    if (::listen(fd, 8) != 0) {
        error = "listen 失败: " + SocketErrorMessage();
#if defined(_WIN32)
        closesocket(fd);
#else
        ::close(fd);
#endif
        return false;
    }
    listen_fd_ = Socket(ToHandle(fd));
    actual_port_ = listen_fd_.LocalPort();
    return true;
}

std::optional<Socket> Listener::Accept(int timeout_ms) {
    if (!listen_fd_.valid()) {
        last_error_ = "监听没起";
        return std::nullopt;
    }
    while (!stop_.load()) {
        fd_set read_set;
        FD_ZERO(&read_set);
        const RawSocket listen_raw = FromHandle(listen_fd_.native());
        FD_SET(listen_raw, &read_set);
        timeval timeout{};
        timeout.tv_sec = timeout_ms / 1000;
        timeout.tv_usec = (timeout_ms % 1000) * 1000;
        const int ready = ::select(static_cast<int>(listen_raw) + 1, &read_set, nullptr, nullptr, &timeout);
        if (ready == 0) {
            return std::nullopt; // 超时:调用方看 stopped() 再决定等不等
        }
        if (ready < 0) {
#if defined(_WIN32)
            if (WSAGetLastError() == WSAEINTR) {
                continue;
            }
#else
            if (errno == EINTR) {
                continue;
            }
#endif
            last_error_ = "select 失败: " + SocketErrorMessage();
            return std::nullopt;
        }
        sockaddr_in peer{};
        socklen_t peer_length = sizeof(peer);
        const RawSocket accepted =
            ::accept(listen_raw, reinterpret_cast<sockaddr*>(&peer), &peer_length);
        if (accepted == kInvalidRaw) {
            last_error_ = "accept 失败: " + SocketErrorMessage();
            return std::nullopt;
        }
        return Socket(ToHandle(accepted));
    }
    return std::nullopt;
}

Socket ConnectTcp(const std::string& host, int port, std::string& error) {
    if (!Startup(error)) {
        return Socket();
    }
    sockaddr_in address{};
    if (!ResolveBindAddress(host, address, error)) {
        return Socket();
    }
    address.sin_port = htons(static_cast<std::uint16_t>(port));
    const RawSocket fd = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (fd == kInvalidRaw) {
        error = "socket 建不起: " + SocketErrorMessage();
        return Socket();
    }
    if (::connect(fd, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) != 0) {
        error = "connect 失败(" + host + ":" + std::to_string(port) + "): " + SocketErrorMessage();
#if defined(_WIN32)
        closesocket(fd);
#else
        ::close(fd);
#endif
        return Socket();
    }
#if defined(__APPLE__)
    // macOS 无 MSG_NOSIGNAL,逐 socket 关 SIGPIPE(配合 SendAll 的 Linux flag,
    // 两平台都把"对端断了还 send"从致命信号降回普通错误)。
    int nosigpipe = 1;
    ::setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &nosigpipe, sizeof(nosigpipe));
#endif
    return Socket(ToHandle(fd));
}

}  // namespace lubancode::app_server::net
