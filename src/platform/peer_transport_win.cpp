// 跨会话传话传输层 · Windows 实现:Named Pipe + 当前用户 SID ACL。
//
// 管道名 \\.\pipe\lubancode-peer-<peer_id>。安全属性用 SDDL 拼一条 DACL,
// 只给当前进程令牌里的用户 SID GENERIC_ALL——同机别的系统用户(以及别的
// 会话)CreateFileW 打不开这条管道,连"输错内容"的机会都没有。这跟规格
// "名册只准同一系统用户读写"对齐;跨机器/跨用户一律不做。
//
// accept 循环在专属线程上:CreateNamedPipeW(单实例) -> ConnectNamedPipe
// -> 读一帧 -> 问处理器 -> 回一帧 -> DisconnectNamedPipe -> 重新建管道再
// 等。Stop() 用 CancelIoEx 掐断阻塞中的 ConnectNamedPipe/ReadFile,再关
// 句柄 join 线程。
//
// 原生 Windows 上编译验证;POSIX 镜像实现(peer_transport_posix.cpp)在
// Linux CI 上验,本机编不了——两份逻辑刻意对齐,差异只在系统调用。

#ifndef _WIN32
#error "peer_transport_win.cpp 只在 Windows 下编译(CMake 按 WIN32 门控)"
#endif

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <sddl.h>

#include <algorithm>
#include <atomic>
#include <cstring>
#include <thread>

#include "platform/paths.hpp"  // Utf8ToWide
#include "platform/peer_transport.hpp"

namespace lubancode::platform {

namespace {

// 单帧上限 1 MiB:跨会话递的是"一张字条",超过这个量级的一律当坏帧掐掉,
// 不给恶意/失控的对端拿超大长度说明符把内存吃光的机会。
constexpr std::uint32_t kMaxFrameBytes = 1 * 1024 * 1024;

std::string LastErrorText(const char* what) {
    return std::string(what) + " (GetLastError=" + std::to_string(GetLastError()) + ")";
}

// 当前进程令牌里用户的 SID 字符串(SDDL 一节,如 S-1-5-21-...)。
std::string CurrentUserSid() {
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) {
        return {};
    }
    DWORD size = 0;
    GetTokenInformation(token, TokenUser, nullptr, 0, &size);
    if (size == 0) {
        CloseHandle(token);
        return {};
    }
    std::string buffer(size, '\0');
    if (!GetTokenInformation(token, TokenUser, buffer.data(), size, &size)) {
        CloseHandle(token);
        return {};
    }
    CloseHandle(token);
    const auto* user = reinterpret_cast<const TOKEN_USER*>(buffer.data());
    LPWSTR sid_wide = nullptr;
    if (!ConvertSidToStringSidW(user->User.Sid, &sid_wide)) {
        return {};
    }
    std::string sid;
    for (LPWSTR p = sid_wide; *p != L'\0'; ++p) {
        sid.push_back(static_cast<char>(*p));  // SID 是纯 ASCII,宽窄无损
    }
    LocalFree(sid_wide);
    return sid;
}

// 只允许当前用户的 DACL。拿到不齐(SID 空/转换失败)就返回空指针——
// CreateNamedPipeW 用默认安全属性(派生自进程令牌,同样是当前用户),
// 不会因此放宽。
SECURITY_ATTRIBUTES BuildCurrentUserAcl() {
    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa);
    const std::string sid = CurrentUserSid();
    if (sid.empty()) {
        return sa;
    }
    const std::wstring sddl = L"D:P(A;;GA;;;" + std::wstring(sid.begin(), sid.end()) + L")";
    PSECURITY_DESCRIPTOR sd = nullptr;
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl.c_str(), SDDL_REVISION_1, &sd, nullptr)) {
        return sa;
    }
    sa.lpSecurityDescriptor = sd;
    sa.bInheritHandle = FALSE;
    return sa;
}

bool WriteAll(HANDLE handle, const char* data, std::size_t size) {
    std::size_t done = 0;
    while (done < size) {
        const DWORD chunk = static_cast<DWORD>((std::min)(size - done, static_cast<std::size_t>(1 << 20)));
        DWORD written = 0;
        if (!WriteFile(handle, data + done, chunk, &written, nullptr) || written == 0) {
            return false;
        }
        done += written;
    }
    return true;
}

bool ReadAll(HANDLE handle, char* data, std::size_t size) {
    std::size_t done = 0;
    while (done < size) {
        const DWORD chunk = static_cast<DWORD>((std::min)(size - done, static_cast<std::size_t>(1 << 20)));
        DWORD read = 0;
        if (!ReadFile(handle, data + done, chunk, &read, nullptr) || read == 0) {
            return false;
        }
        done += read;
    }
    return true;
}

// 读一帧:4 字节大端长度 + 正文。超限/读失败返回空。
std::string ReadFrame(HANDLE handle) {
    char header[4] = {};
    if (!ReadAll(handle, header, sizeof(header))) {
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
    if (length > 0 && !ReadAll(handle, payload.data(), length)) {
        return {};
    }
    return payload;
}

}  // namespace

struct PeerPipeServer::Impl {
    std::thread thread;
    std::atomic<bool> stop{false};
    std::atomic<bool> running{false};
    HANDLE listen_pipe = INVALID_HANDLE_VALUE;
    Handler handler;
    std::string endpoint;
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
    impl_->endpoint = endpoint;

    const std::wstring wide = Utf8ToWide(endpoint);
    SECURITY_ATTRIBUTES sa = BuildCurrentUserAcl();
    SECURITY_ATTRIBUTES* sa_ptr = sa.lpSecurityDescriptor != nullptr ? &sa : nullptr;
    impl_->listen_pipe = CreateNamedPipeW(
        wide.c_str(), PIPE_ACCESS_DUPLEX | FILE_FLAG_FIRST_PIPE_INSTANCE,
        PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS, 1, 64 * 1024, 64 * 1024, 0,
        sa_ptr);
    if (sa.lpSecurityDescriptor != nullptr) {
        LocalFree(sa.lpSecurityDescriptor);
        sa.lpSecurityDescriptor = nullptr;
    }
    if (impl_->listen_pipe == INVALID_HANDLE_VALUE) {
        last_error_ = LastErrorText("CreateNamedPipeW");
        delete impl_;
        impl_ = nullptr;
        return false;
    }

    impl_->running.store(true);
    impl_->thread = std::thread([this] {
        while (!impl_->stop.load()) {
            // 单实例管道:接完一单要重建。listen_pipe 挂着的期间 Stop() 会
            // CancelIoEx + 关句柄,阻塞中的等待立刻醒。
            if (!ConnectNamedPipe(impl_->listen_pipe, nullptr) && GetLastError() != ERROR_PIPE_CONNECTED) {
                if (impl_->stop.load()) {
                    break;
                }
                continue;
            }
            if (impl_->stop.load()) {
                break;
            }
            const std::string request = ReadFrame(impl_->listen_pipe);
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
            WriteAll(impl_->listen_pipe, reply_frame.data(), reply_frame.size());
            FlushFileBuffers(impl_->listen_pipe);  // 确保客户端读到全文再断开
            DisconnectNamedPipe(impl_->listen_pipe);
        }
    });
    return true;
}

void PeerPipeServer::Stop() {
    if (impl_ == nullptr) {
        return;
    }
    impl_->stop.store(true);
    if (impl_->listen_pipe != INVALID_HANDLE_VALUE) {
        CancelIoEx(impl_->listen_pipe, nullptr);
    }
    if (impl_->thread.joinable()) {
        impl_->thread.join();
    }
    if (impl_->listen_pipe != INVALID_HANDLE_VALUE) {
        CloseHandle(impl_->listen_pipe);
        impl_->listen_pipe = INVALID_HANDLE_VALUE;
    }
    impl_->running.store(false);
}

bool PeerPipeServer::running() const { return impl_ != nullptr && impl_->running.load(); }

PeerSendResult PeerPipeSend(const std::string& endpoint, const std::string& payload, int timeout_ms) {
    // Windows 具名管道没有便携的读写超时旋钮(管道 I/O 是阻塞语义),超时
    // 参数在这边只保留接口对齐;帧上限 + 单问单答的短交互让卡死风险可忽略。
    (void)timeout_ms;
    PeerSendResult result;
    const std::wstring wide = Utf8ToWide(endpoint);
    // 单实例管道:服务端 DisconnectNamedPipe 之后、回到 ConnectNamedPipe
    // 之前有一小段窗口没有监听实例,这个窗口里 CreateFileW 回
    // ERROR_PIPE_BUSY。规范写法是 WaitNamedPipe 等实例就绪再试——慢机器
    // 上窗口不小,连发两帧(坏帧测试、连递两张字条)都会撞上。
    HANDLE pipe = INVALID_HANDLE_VALUE;
    for (int attempt = 0; attempt < 50; ++attempt) {
        pipe = CreateFileW(wide.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
        if (pipe != INVALID_HANDLE_VALUE || GetLastError() != ERROR_PIPE_BUSY) {
            break;  // 连上了,或不是忙(不在了/权限不对),都按真实结果收场
        }
        if (!WaitNamedPipeW(wide.c_str(), 50)) {
            continue;  // 这 50ms 内没等到实例,再试一轮,次数用尽便带着末次错误收场
        }
    }
    if (pipe == INVALID_HANDLE_VALUE) {
        result.error = LastErrorText("CreateFileW(pipe)");
        return result;
    }
    // 管道默认就是 BYTE/_WAIT 模式,对齐服务端。
    const DWORD mode = PIPE_READMODE_BYTE;
    SetNamedPipeHandleState(pipe, const_cast<DWORD*>(&mode), nullptr, nullptr);

    const std::string frame = EncodePeerFrame(payload);
    if (!WriteAll(pipe, frame.data(), frame.size())) {
        result.error = LastErrorText("WriteFile(pipe)");
        CloseHandle(pipe);
        return result;
    }
    // 不在客户端 Flush:双向管道上 FlushFileBuffers 会等到对端把手上的数据
    // 全读走才返回,服务端正等着读请求、和这里的时序咬合没有收益,反而
    // 平添一次内核往返。阻塞模式下 WriteFile 本身就是同步送达。
    result.reply = ReadFrame(pipe);
    CloseHandle(pipe);
    if (result.reply.empty()) {
        result.error = "peer closed without reply";
        return result;
    }
    result.ok = true;
    return result;
}

}  // namespace lubancode::platform
