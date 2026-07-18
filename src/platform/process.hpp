// 平台抽象层:子进程(v0.20.x 跨平台单)。两种模式:
//
//   1. RunProcess —— 一次性捕获:起进程、合并捕获 stdout/stderr、等它跑完
//      或超时杀树、拿回输出。前身是 tools/process_exec.hpp(M4 run_command、
//      M9 hooks 共用),Windows 实现(CreateProcessW + Job Object)原样搬进
//      process_win.cpp,签名不变;新增按 argv 数组起进程的可移植重载,和
//      RunShellCommand(按平台默认 shell 跑一条命令)这个共用入口。
//
//   2. ChildProcess —— 长命双向管道:stdin 能一直写、stdout/stderr 能一直
//      读,MCP / LSP 的 stdio 传输层共用。前身是 mcp/transport.cpp 与
//      lsp/transport.cpp 里两份几乎一样的 Win32 代码(CreateProcessW 三管道
//      + Job Object + 读线程),搬进来合成一份;两边的传输层瘦身成
//      "分帧器 + 这里的 ChildProcess"。
//
// 语义要点(两平台对齐):
//   - 超时/关停都要把整棵进程树杀干净(Windows: Job Object 的
//     KILL_ON_JOB_CLOSE;POSIX: setpgid 进程组 + killpg SIGTERM→SIGKILL)。
//   - 输出上限:一次性捕获超过 max_output_bytes 截断保留前段、置
//     output_truncated、杀树,但继续读空管道到 EOF(不读的话子进程会卡在
//     写上死不掉)。
//   - env 注入:UTF-8 键值对,同名覆盖当前进程环境。
#pragma once

#include <atomic>
#include <cstddef>
#include <functional>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace lubancode::platform {

struct ProcessResult {
    std::string output;              // 合并捕获的 stdout/stderr,原始字节
    unsigned long exit_code = 0;
    bool timed_out = false;
    bool spawn_failed = false;
    bool output_truncated = false;   // 输出超过上限被截断,进程树已被强制终止
    std::string spawn_error;
};

// 单次执行捕获输出的默认上限(字节)。超过就截断、杀掉整棵进程树,防止
// 一条命令把内存吃光(比如 ping -t、dir /s C:\)。
constexpr std::size_t kDefaultMaxOutputBytes = 2 * 1024 * 1024;

using EnvPairs = std::vector<std::pair<std::string, std::string>>;

// 可移植入口:按 argv 数组起一个子进程(argv[0] 是可执行文件,按 PATH 查
// 找;不经过 shell,参数原样传递),合并捕获 stdout/stderr,超时杀树。
//
// extra_env 是要临时注入子进程环境的键值对(UTF-8,同名覆盖当前进程环境)。
// Windows 实现上是"临时 SetEnvironmentVariableW -> 起子进程 -> 立刻还原"
// 这个套路,要求调用方单线程顺序调用(tools 层的工具执行、钩子执行本来
// 就是顺序跑的,见 agent/loop.cpp);POSIX 是 fork 后在子进程里改 environ,
// 不碰父进程,没有这层约束,但语义按更严的 Windows 算。
//
// max_output_bytes:捕获输出的上限,超过就截断保留前段、置 output_truncated
// 并强制终止整棵进程树。测试用小值,生产路径用默认值即可。
ProcessResult RunProcess(const std::vector<std::string>& argv, int timeout_ms,
                          const EnvPairs& extra_env = {},
                          std::size_t max_output_bytes = kDefaultMaxOutputBytes);

// 按平台默认 shell 跑一条命令:Windows 是 `cmd.exe /d /s /c "<command>"`
// (输出按系统 ANSI 代码页转成 UTF-8,原因见 paths.hpp 的 AcpBytesToUtf8),
// POSIX 是 `/bin/sh -c '<command>'`(输出天然 UTF-8 直通)。hooks 与
// run_command 的 cmd/sh 分支共用。
ProcessResult RunShellCommand(const std::string& command_utf8, int timeout_ms,
                               const EnvPairs& extra_env = {},
                               std::size_t max_output_bytes = kDefaultMaxOutputBytes);

#ifdef _WIN32

// Windows 专属重载:cmdline 是完整的"可执行文件 + 参数"命令行,调用方
// 拼好(run_command 的 PowerShell -EncodedCommand 路径要用)。搬自
// tools/process_exec.hpp,签名未动。
ProcessResult RunProcess(const std::wstring& cmdline, int timeout_ms,
                          const EnvPairs& extra_env = {},
                          std::size_t max_output_bytes = kDefaultMaxOutputBytes);

// 拼一条 `cmd.exe /d /s /c "<command>"` 命令行,原样交给 cmd 执行,不做任何
// 代码页预处理。搬自 tools/process_exec.hpp。
std::wstring BuildCmdCommandLine(const std::string& user_command_utf8);

#endif

// 一次启动长命子进程的结果。
struct SpawnResult {
    bool success = false;
    std::string error;
    bool command_not_found = false;  // 可执行文件不存在(LSP 层要给更友好的报错)
};

// 长命子进程的双向管道:stdin 可写、stdout/stderr 各有一条专属读线程,把
// 读到的原始字节块喂给回调(分帧不归这里管,MCP 的行分帧、LSP 的
// Content-Length 分帧都在各自传输层)。
//
// 回调约定:
//   - on_stdout 在 stdout 读线程上按到达顺序调用;返回 false 表示"这条流
//     报废了,别再读了"(MCP 单行超限断连那条路),读线程随即退出——调用
//     方通常配合 Kill() 把进程也杀掉。
//   - on_stderr 在 stderr 读线程上调用,无返回值。
//   - 两个回调都可能在 Shutdown 之前的任意时刻被调,调用方自己保证线程安全。
class ChildProcess {
public:
    ChildProcess() = default;
    ~ChildProcess();

    ChildProcess(const ChildProcess&) = delete;
    ChildProcess& operator=(const ChildProcess&) = delete;

    // 起子进程:command 是可执行文件(按 PATH 查找),args 是参数列表,env
    // 是要额外注入子进程环境的键值对(同名覆盖当前进程环境;注入方式同
    // RunProcess 的注释)。
    SpawnResult Start(const std::string& command, const std::vector<std::string>& args, const EnvPairs& env,
                       std::function<bool(std::string_view)> on_stdout,
                       std::function<void(std::string_view)> on_stderr);

    // 原始字节一次性写进子进程 stdin,内部加锁防止多个请求线程交错。进程
    // 已经退出/没起成功都返回 false。
    bool Write(const std::string& data);

    // 立刻强制终止根进程(不等、不关 stdin)。MCP 协议错误断连用;进程树
    // 的收尾(连带后代)还是要靠 Shutdown。
    void Kill();

    // 关停:先关 stdin(等于给子进程发 EOF,行为良好的服务器自己体面退出),
    // 等 wait_ms 毫秒;还没退出就把整棵进程树杀掉,然后收读线程、关句柄。
    // 幂等,重复调用/析构时再调都安全。
    void Shutdown(int wait_ms = 2000);

    // 进程是否还活着。
    bool IsAlive() const;

private:
    void JoinReaderThreads();

    std::function<bool(std::string_view)> on_stdout_;
    std::function<void(std::string_view)> on_stderr_;

    void StdoutReaderThread();
    void StderrReaderThread();

    std::mutex write_mutex_;

#ifdef _WIN32
    void* process_ = nullptr;      // HANDLE(不在头文件里拖 windows.h)
    void* job_ = nullptr;          // HANDLE
    void* stdin_write_ = nullptr;  // HANDLE
    void* stdout_read_ = nullptr;  // HANDLE
    void* stderr_read_ = nullptr;  // HANDLE
#else
    long pid_ = -1;                // pid_t
    int stdin_fd_ = -1;
    int stdout_fd_ = -1;
    int stderr_fd_ = -1;
    mutable std::mutex wait_mutex_;      // waitpid 只许一家收尸
    mutable bool reaped_ = false;        // 已 waitpid 收过尸
    mutable int exit_status_ = 0;        // reaped_ 为真时有效
#endif

    std::thread stdout_thread_;
    std::thread stderr_thread_;

    std::atomic<bool> started_{false};
    std::atomic<bool> shutdown_done_{false};
    std::atomic<bool> reader_stop_{false};
    std::atomic<bool> stdout_reader_done_{false};
    std::atomic<bool> stderr_reader_done_{false};
};

}  // namespace lubancode::platform
