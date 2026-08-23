// 跨会话传话传输层 · POSIX 实现:Unix domain socket + 0600。
//
// socket 路径 <临时目录>/lubancode-peer-<peer_id>.sock。bind 之后立刻
// chmod 0600——只有本用户的进程连得上,跟 Windows 版"Named Pipe 只给当前
// 用户 SID"语义镜像(规格:名册只准同一系统用户读写)。
//
// accept 循环在专属线程,poll(listen_fd, 200ms) 轮询 stop 标志,Stop()
// 不需要自连踢门。帧格式与 Windows 版一致:4 字节大端长度 + UTF-8 JSON,
// 单请求单应答。
//
// 本机(Windows 主场)编不了这份文件,CMake 按平台门控;逻辑刻意与
// peer_transport_win.cpp 镜像对齐,留待 Linux CI 编译 + 集成测试
// (tests/integration/peer/test_peer_transport.cpp 两平台同一套断言)验证。macOS 未经真机
// 验证,与 console_posix.cpp 同一待遇。

#ifndef _WIN32

#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <thread>

#include "platform/peer_transport.hpp"

namespace lubancode::platform {

namespace {

constexpr std::uint32_t kMaxFrameBytes = 1 * 1024 * 1024;  // 与 Windows 版对齐

bool WriteAll(int fd, const char* data, std::size_t size) {
    std::size_t done = 0;
    while (done < size) {
        const ssize_t n = ::send(fd, data + done, size - done, MSG_NOSIGNAL);
        if (n <= 0) {
            if (n < 0 && errno == EINTR) {
                continue;
            }
            return false;
        }
        done += static_cast<std::size_t>(n);
    }
    return true;
}

bool ReadAll(int fd, char* data, std::size_t size) {
    std::size_t done = 0;
    while (done < size) {
        const ssize_t n = ::recv(fd, data + done, size - done, 0);
        if (n <= 0) {
            if (n < 0 && errno == EINTR) {
                continue;
            }
            return false;
        }
        done += static_cast<std::size_t>(n);
    }
    return true;
}

std::string ReadFrame(int fd) {
    char header[4] = {};
    if (!ReadAll(fd, header, sizeof(header))) {
        return {};
    }
    const std::uint32_t length = (static_cast<unsigned char>(header[0]) << 24) |
                                 (static_cast<unsigned char>(header[1]) << 16) |
                                 (static_cast<unsigned char>(header[2]) << 8) |
                                 static_cast<unsigned char>(header[3]);
    if (length > kMaxFrameBytes) {
        return {};
    }
    std::string payload(length, '\0');
    if (length > 0 && !ReadAll(fd, payload.data(), length)) {
        return {};
    }
    return payload;
}

}  // namespace

struct PeerPipeServer::Impl {
    std::thread thread;
    std::atomic<bool> stop{false};
    std::atomic<bool> running{false};
    int listen_fd = -1;
    std::string socket_path;
    Handler handler;
};

PeerPipeServer::PeerPipeServer() = default;

PeerPipeServer::~PeerPipeServer() { Stop(); }

bool PeerPipeServer::Start(const std::string& endpoint, Handler handler) {
    if (impl_ != nullptr && impl_->running.load()) {
        return true;
    }
    delete impl_;
    impl_ = new Impl();
    impl_->handler = std::move(handler);
    impl_->socket_path = endpoint;

    impl_->listen_fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (impl_->listen_fd < 0) {
        last_error_ = std::string("socket: ") + std::strerror(errno);
        delete impl_;
        impl_ = nullptr;
        return false;
    }
    ::unlink(impl_->socket_path.c_str());  // 上一场没清干净的残留
    struct sockaddr_un address {};
    address.sun_family = AF_UNIX;
    if (impl_->socket_path.size() >= sizeof(address.sun_path)) {
        last_error_ = "socket path too long";
        ::close(impl_->listen_fd);
        impl_->listen_fd = -1;
        delete impl_;
        impl_ = nullptr;
        return false;
    }
    std::strncpy(address.sun_path, impl_->socket_path.c_str(), sizeof(address.sun_path) - 1);
    if (::bind(impl_->listen_fd, reinterpret_cast<struct sockaddr*>(&address), sizeof(address)) != 0) {
        last_error_ = std::string("bind: ") + std::strerror(errno);
        ::close(impl_->listen_fd);
        impl_->listen_fd = -1;
        delete impl_;
        impl_ = nullptr;
        return false;
    }
    ::chmod(impl_->socket_path.c_str(), 0600);  // 只准本用户
    if (::listen(impl_->listen_fd, 4) != 0) {
        last_error_ = std::string("listen: ") + std::strerror(errno);
        ::close(impl_->listen_fd);
        ::unlink(impl_->socket_path.c_str());
        impl_->listen_fd = -1;
        delete impl_;
        impl_ = nullptr;
        return false;
    }

    impl_->running.store(true);
    impl_->thread = std::thread([this] {
        while (!impl_->stop.load()) {
            struct pollfd pfd{impl_->listen_fd, POLLIN, 0};
            if (::poll(&pfd, 1, 200) != 1) {
                continue;  // 超时/被信号打断:回头再看 stop 标志
            }
            const int client = ::accept(impl_->listen_fd, nullptr, nullptr);
            if (client < 0) {
                continue;
            }
            const std::string request = ReadFrame(client);
            std::string reply;
            try {
                if (impl_->handler) {
                    reply = impl_->handler(request);
                }
            } catch (...) {
                reply.clear();
            }
            if (reply.empty()) {
                reply = "{\"status\":\"refused\"}";
            }
            const std::string reply_frame = EncodePeerFrame(reply);  // 应答同帧:长度头 + 正文
            WriteAll(client, reply_frame.data(), reply_frame.size());
            ::shutdown(client, SHUT_WR);
            ::close(client);
        }
    });
    return true;
}

void PeerPipeServer::Stop() {
    if (impl_ == nullptr) {
        return;
    }
    impl_->stop.store(true);
    if (impl_->thread.joinable()) {
        impl_->thread.join();
    }
    if (impl_->listen_fd >= 0) {
        ::close(impl_->listen_fd);
        impl_->listen_fd = -1;
    }
    if (!impl_->socket_path.empty()) {
        ::unlink(impl_->socket_path.c_str());
    }
    impl_->running.store(false);
}

bool PeerPipeServer::running() const { return impl_ != nullptr && impl_->running.load(); }

PeerSendResult PeerPipeSend(const std::string& endpoint, const std::string& payload, int timeout_ms) {
    PeerSendResult result;
    const int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        result.error = std::string("socket: ") + std::strerror(errno);
        return result;
    }
    struct timeval timeout {};
    timeout.tv_sec = timeout_ms / 1000;
    timeout.tv_usec = (timeout_ms % 1000) * 1000;
    ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    ::setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));

    struct sockaddr_un address {};
    address.sun_family = AF_UNIX;
    if (endpoint.size() >= sizeof(address.sun_path)) {
        result.error = "socket path too long";
        ::close(fd);
        return result;
    }
    std::strncpy(address.sun_path, endpoint.c_str(), sizeof(address.sun_path) - 1);
    if (::connect(fd, reinterpret_cast<struct sockaddr*>(&address), sizeof(address)) != 0) {
        result.error = std::string("connect: ") + std::strerror(errno);
        ::close(fd);
        return result;
    }

    const std::string frame = EncodePeerFrame(payload);
    if (!WriteAll(fd, frame.data(), frame.size())) {
        result.error = std::string("send: ") + std::strerror(errno);
        ::close(fd);
        return result;
    }
    result.reply = ReadFrame(fd);
    ::close(fd);
    if (result.reply.empty()) {
        result.error = "peer closed without reply";
        return result;
    }
    result.ok = true;
    return result;
}

}  // namespace lubancode::platform

#endif  // !_WIN32
