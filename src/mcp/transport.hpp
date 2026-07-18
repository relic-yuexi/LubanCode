// M8:MCP stdio 传输层。跟 platform/process.hpp 的一次性捕获(起进程、
// 等它跑完、收尸)不是一回事——MCP 服务器是要活到会话结束的长命子进程,
// 这里管的是"起一个双向管道的子进程,stdin 能一直写、stdout 能一直读"。
// 跨平台单(v0.20.x)起,起进程/管道/杀树那套 Win32 代码搬去了
// platform::ChildProcess(POSIX 同接口),这里只剩 MCP 自己的行分帧和
// stderr 环形日志。
//
// 行分帧:MCP stdio 是换行分隔的 JSON-RPC 2.0(一行一条完整消息)。
// LineFramer 是纯函数式的增量分帧器,写法跟 api/sse_framing.hpp 的
// SseFramer 一个路数——喂多少字节都行,一条消息可能被拆成好几段喂进来,
// 也可能好几条消息挤在一次读到的数据里,残行留在内部缓冲,下次 Feed()
// 接着用。单拎出来是为了不真起进程也能单测"劈包/挤包/残行"这几种情形。
//
// 协议层(mcp/client.hpp)不直接依赖这个具体实现——client.hpp 定义了一个
// Transport 抽象接口,StdioTransport 是它的生产实现,测试可以注入假的
// Transport,专门验证请求-响应配对逻辑,不用真起子进程。
#pragma once

#include <atomic>
#include <functional>
#include <mutex>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "platform/process.hpp"

namespace lubancode::mcp {

// 纯函数式增量分帧器:把陆续到达的字节流,按换行符切成一行一行,统一去掉
// 行尾的 \r(不管来源是 \r\n 还是单纯 \n)。
class LineFramer {
public:
    // 单行(未收到换行前的累积缓冲)上限:超过就置 overflowed、清空缓冲,
    // 防止一个不换行狂写的坏服务器把内存吃光。
    static constexpr std::size_t kMaxLineBytes = 8 * 1024 * 1024;

    // 喂一段新到达的字节。返回这次新凑齐的完整行(可能是 0 行、1 行、多行)。
    // 一旦 overflowed() 为真,分帧器进入报废状态,后续 Feed 不再产出任何行。
    std::vector<std::string> Feed(std::string_view chunk);

    // 单行累积超过 kMaxLineBytes,协议已不可信——调用方应当断连。
    bool overflowed() const { return overflowed_; }

private:
    std::string buffer_;  // 还没凑成一整行的残句
    bool overflowed_ = false;
};

// 一次启动子进程的结果。
struct TransportStartResult {
    bool success = false;
    std::string error;
};

// 长命子进程的双向管道传输:stdin 可写、stdout 可读(逐行喂给 on_line
// 回调),stderr 单独捕获进一个环形日志缓冲(出错时给人看,不跟 stdout
// 混在一起,免得污染 JSON-RPC 消息流)。进程本体是
// platform::ChildProcess(两平台同实现)。
class StdioTransport {
public:
    StdioTransport() = default;
    ~StdioTransport();

    StdioTransport(const StdioTransport&) = delete;
    StdioTransport& operator=(const StdioTransport&) = delete;

    // 起子进程:command 是可执行文件(比如 "python"),args 是参数列表,
    // env 是要额外注入子进程环境的键值对(同名覆盖当前进程环境,注入方式
    // 见 platform/process.hpp 的说明)。on_line 在专属的读线程上,对
    // stdout 上凑齐的每一整行调用一次,按到达顺序,调用方自己保证线程
    // 安全(转发进协议层时补一把锁)。
    TransportStartResult Start(const std::string& command, const std::vector<std::string>& args,
                                const std::vector<std::pair<std::string, std::string>>& env,
                                std::function<void(std::string)> on_line);

    // 整条消息(不带换行)+ \n 一次性写出去,内部加锁防止多个请求线程交错。
    // 进程已经退出/没起成功都返回 false。
    bool WriteLine(const std::string& message);

    // 关停:先关 stdin(相当于"发 shutdown 意向"——MCP 没有标准 shutdown
    // 请求,直接关 stdin 让服务器自己感知到 EOF、体面退出),等
    // wait_ms 毫秒;还没退出就把整棵进程树杀掉。幂等,重复调用/析构时再调
    // 都安全。
    void Shutdown(int wait_ms = 2000);

    // 进程是否还活着。
    bool IsAlive() const;

    // stderr 捕获到的内容,最近若干字节(诊断/报错用)。
    std::string StderrTail() const;

private:
    std::function<void(std::string)> on_line_;
    LineFramer line_framer_;

    mutable std::mutex stderr_mutex_;
    std::string stderr_buffer_;

    platform::ChildProcess child_;
};

}  // namespace lubancode::mcp
