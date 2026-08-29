// app-server WebSocket 承载的跨平台 socket 薄层:Windows Winsock2 与 POSIX
// Berkeley socket 各包一份,语义镜像(阻塞读写、select 带超时的 accept)。
// 只给 ws_transport 与测试客户端用,别处不许碰——更上层的网络活是 cpr 的
// 事,这层只为 WS 承载立一个最小服务端。
#pragma once

#include <atomic>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace lubancode::app_server::net {

// 进程级一次性初始化(Windows WSAStartup;POSIX 空操作)。可重复调,引用
// 计数在进程退出时收。失败返回 false,error 有话。
bool Startup(std::string& error);

// 一只 TCP socket 的 RAII 柄。移动语义;读写阻塞式。
class Socket {
public:
    Socket() = default;
    ~Socket();
    Socket(Socket&& other) noexcept;
    Socket& operator=(Socket&& other) noexcept;
    Socket(const Socket&) = delete;
    Socket& operator=(const Socket&) = delete;

    bool valid() const { return handle_ != kInvalidHandle; }
    void Close();

    // 阻塞读一段(最多 capacity 字节)。返回真读到的字节数;0 = 对端有序
    // 关闭;-1 = 错/断。
    long Recv(char* buffer, std::size_t capacity);

    // 全量写(循环 send,直到写完或错)。false = 断/错。
    bool SendAll(std::string_view bytes);

    // 阻塞收的超时(毫秒;0 = 不限)。超时后 Recv 回 -1。测试客户端用来
    // 把"排干事件"的循环收口。
    void SetRecvTimeoutMs(int ms);

    // 本机侧端口(测试断言与 actual_port 用);拿不到给 0。
    int LocalPort() const;

    // 原生柄(select 用;POSIX int,Windows uintptr_t 的窄化由实现担保——
    // 只当不透明值使)。
    std::int64_t native() const;

    static constexpr std::int64_t kInvalidHandle = -1;

private:
    friend class Listener;
    friend Socket ConnectTcp(const std::string& host, int port, std::string& error);
    explicit Socket(std::int64_t handle) : handle_(handle) {}
    std::int64_t handle_ = kInvalidHandle;
};

// 监听器:bind + listen + 带超时的 accept。
class Listener {
public:
    Listener() = default;
    ~Listener() = default;
    Listener(const Listener&) = delete;
    Listener& operator=(const Listener&) = delete;

    // 起 监听。host 空 = "127.0.0.1";port 0 = 系统分配(测试用)。
    // 失败 false + error(可进日志)。
    bool Start(const std::string& host, int port, std::string& error);

    // 等一条连接:select 带超时轮询 stop 旗。
    //   - 返回 socket:来了一条;
    //   - 返回 nullopt 且 stopped()==true:外部叫停(Stop 置旗);
    //   - 返回 nullopt 且 stopped()==false 且 last_error() 空:超时,再等;
    //   - 返回 nullopt 且 last_error() 非空:监听层真错,该收摊。
    std::optional<Socket> Accept(int timeout_ms);

    // 叫停 Accept(置旗;下一次 select 超时内返回 stopped)。
    void Stop() { stop_.store(true); }
    bool stopped() const { return stop_.load(); }

    int actual_port() const { return actual_port_; }
    const std::string& last_error() const { return last_error_; }

private:
    Socket listen_fd_;
    std::atomic<bool> stop_{false};
    int actual_port_ = 0;
    std::string last_error_;
};

// 客户端连接(测试客户端与外部参考壳用;阻塞,连不上 false + error)。
Socket ConnectTcp(const std::string& host, int port, std::string& error);

}  // namespace lubancode::app_server::net
