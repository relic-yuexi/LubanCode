// LSP stdio 传输层。跟 mcp/transport.hpp 是同一个路数——起一个双向管道的
// 长命子进程,stdin 能一直写、stdout 能一直读——但这里是 lsp/ 自己的一份
// 独立实现,不 include mcp/ 的任何东西(mcp/ 归别的分支管,两边各养各的)。
//
// 分帧跟 MCP 的"一行一条"不一样:LSP 用 HTTP 风格的头部分帧,
//   Content-Length: N\r\n
//   \r\n
//   <N 字节的 JSON 正文>
// ContentLengthFramer 是纯函数式的增量分帧器,写法跟 api/sse_framing.hpp 的
// SseFramer 一个路数——喂多少字节都行,一条消息可能劈成好几段喂进来,也
// 可能好几条消息挤在一次读到的数据里,残包留在内部缓冲,下次 Feed() 接着
// 用。单拎出来是为了不真起进程也能单测"劈包/挤包/多消息/大体积"这几种情形。
//
// 协议层(lsp/client.hpp)不直接依赖这个具体实现——client.hpp 定义了一个
// Transport 抽象接口,StdioTransport 是它的生产实现,测试可以注入假的
// Transport,专门验证请求-响应配对和诊断缓存逻辑,不用真起子进程。
#pragma once

#include <atomic>
#include <cstddef>
#include <functional>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

namespace lubancode::lsp {

// 纯函数式增量分帧器:把陆续到达的字节流,按 LSP 的 Content-Length 头切成
// 一条条完整的 JSON 正文。头部块以 \r\n\r\n 结尾,里面可能有多行头(比如
// Content-Type),只认 Content-Length(大小写不敏感);头部块里找不到
// Content-Length 就把这块头整个丢掉,继续找下一条——坏头不该把整条流搞死。
class ContentLengthFramer {
public:
    // 喂一段新到达的字节。返回这次新凑齐的完整消息正文(可能 0 条、1 条、多条)。
    std::vector<std::string> Feed(std::string_view chunk);

private:
    std::string buffer_;          // 还没凑齐的残包
    std::size_t expected_ = 0;    // 当前这条消息还差多少字节的正文;0 = 正在等头
    bool in_body_ = false;        // true = 头已读完,正在攒正文
};

// 一次启动子进程的结果。
struct TransportStartResult {
    bool success = false;
    std::string error;
};

#ifdef _WIN32

// Windows 下长命子进程的双向管道传输:stdin 可写(带 Content-Length 头)、
// stdout 可读(按头分帧,逐条喂给 on_message 回调),stderr 单独捕获进一个
// 环形日志缓冲(出错时给人看,不跟 stdout 混在一起,免得污染 JSON-RPC
// 消息流)。
class StdioTransport {
public:
    StdioTransport() = default;
    ~StdioTransport();

    StdioTransport(const StdioTransport&) = delete;
    StdioTransport& operator=(const StdioTransport&) = delete;

    // 起子进程:command 是可执行文件(比如 "clangd"),args 是参数列表。
    // on_message 在专属的读线程上,对 stdout 上凑齐的每一条完整消息正文
    // 调用一次,按到达顺序;调用方自己保证线程安全(转发进协议层时补一把锁)。
    TransportStartResult Start(const std::string& command, const std::vector<std::string>& args,
                                std::function<void(std::string)> on_message);

    // 整条消息正文加上 "Content-Length: N\r\n\r\n" 头一次性写出去,内部加锁
    // 防止多个请求线程交错。进程已经退出/没起成功都返回 false。
    bool WriteMessage(const std::string& body);

    // 关停:先关 stdin(协议层的 shutdown/exit 该发的已经发过了,关掉写端
    // 等于再补一个 EOF),等 wait_ms 毫秒;还没退出就用 Job Object 把整棵
    // 进程树杀掉。幂等,重复调用/析构时再调都安全。
    void Shutdown(int wait_ms = 2000);

    // 进程是否还活着(GetExitCodeProcess 查 STILL_ACTIVE)。
    bool IsAlive() const;

    // stderr 捕获到的内容,最近若干字节(诊断/报错用)。
    std::string StderrTail() const;

private:
    void StdoutReaderThread();
    void StderrReaderThread();
    void JoinReaderThreads();

    std::function<void(std::string)> on_message_;
    ContentLengthFramer framer_;

    std::mutex write_mutex_;
    mutable std::mutex stderr_mutex_;
    std::string stderr_buffer_;

    HANDLE process_ = nullptr;
    HANDLE job_ = nullptr;
    HANDLE stdin_write_ = nullptr;
    HANDLE stdout_read_ = nullptr;
    HANDLE stderr_read_ = nullptr;

    std::thread stdout_thread_;
    std::thread stderr_thread_;

    std::atomic<bool> started_{false};
    std::atomic<bool> shutdown_done_{false};
};

#else

// 非 Windows 平台没实现,Start() 直接返回失败——上层(lsp::Manager)按
// "这个服务器起不来"处理,报可读错误,不阻塞会话。
class StdioTransport {
public:
    TransportStartResult Start(const std::string&, const std::vector<std::string>&,
                                std::function<void(std::string)>) {
        return TransportStartResult{false, "LSP StdioTransport 眼下只实现了 Windows"};
    }
    bool WriteMessage(const std::string&) { return false; }
    void Shutdown(int = 2000) {}
    bool IsAlive() const { return false; }
    std::string StderrTail() const { return std::string(); }
};

#endif

}  // namespace lubancode::lsp
