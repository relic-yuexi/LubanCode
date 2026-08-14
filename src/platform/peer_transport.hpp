// 跨会话传话(同机首版):本机进程间的收发传输。
//
// Windows 用 Named Pipe(名字 \\.\pipe\lubancode-peer-<peer_id>,DACL 只
// 给当前系统用户 GENERIC_ALL,别的系统用户连不上,见 peer_transport_win.cpp);
// POSIX 用 Unix domain socket(路径 <临时目录>/lubancode-peer-<peer_id>.sock,
// 文件权限 0600,见 peer_transport_posix.cpp)。两份实现语义镜像,由 CMake
// 按平台二选一编进 lubancode_core。
//
// 线上帧格式(两平台一致):4 字节大端长度 + UTF-8 JSON 正文,单请求单
// 应答,一问一答后连接即断。应答也是一帧 JSON,{"status": "..."}。
//
// 服务端只做"收一帧、问一句处理器、回一帧";信封解析、去重、限速、入队
// 全在 agent/peer_mailbox 那层。处理器在传输线程上被调,必须线程安全
// (PeerMailbox 自己有锁)。
#pragma once

#include <cstdint>
#include <functional>
#include <string>

namespace lubancode::platform {

// 服务端:Start 起 accept 线程,Stop 停掉并 join。endpoint 在 Windows 是
// 完整管道名(含 \\.\pipe\ 前缀),在 POSIX 是 socket 文件路径。
// handler 返回的字符串原样作为应答帧写回;handler 抛异常或返回空串时回
// {"status":"refused"}。
class PeerPipeServer {
public:
    using Handler = std::function<std::string(const std::string& request_payload)>;

    PeerPipeServer();
    ~PeerPipeServer();
    PeerPipeServer(const PeerPipeServer&) = delete;
    PeerPipeServer& operator=(const PeerPipeServer&) = delete;

    bool Start(const std::string& endpoint, Handler handler);
    void Stop();
    bool running() const;
    const std::string& last_error() const { return last_error_; }

private:
    struct Impl;
    Impl* impl_ = nullptr;
    std::string last_error_;
};

// 客户端一次性发送:连 endpoint、写一帧、读一帧应答。
struct PeerSendResult {
    bool ok = false;
    std::string reply;  // 应答帧正文(通常是 {"status":"..."})
    std::string error;  // 失败时的说明;连不上(对方不在)置 ok=false,
                        // 调用方据此回 unavailable
};
PeerSendResult PeerPipeSend(const std::string& endpoint, const std::string& payload, int timeout_ms = 5000);

// 帧编解码(两平台共用,头文件内联):4 字节大端长度 + 正文。
inline std::string EncodePeerFrame(const std::string& payload) {
    const std::uint32_t length = static_cast<std::uint32_t>(payload.size());
    std::string out;
    out.resize(4 + payload.size());
    out[0] = static_cast<char>((length >> 24) & 0xFF);
    out[1] = static_cast<char>((length >> 16) & 0xFF);
    out[2] = static_cast<char>((length >> 8) & 0xFF);
    out[3] = static_cast<char>(length & 0xFF);
    out.replace(4, payload.size(), payload);
    return out;
}

}  // namespace lubancode::platform
