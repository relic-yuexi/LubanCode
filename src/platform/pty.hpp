// 伪终端(PTY)子进程:terminal 工具的平台底座。接口头两平台都能 include
// (不带任何 POSIX 头),实现只有 pty_posix.cpp 一份,CMake 只在非 WIN32
// 收录——Windows 本单不做 ConPTY,调用方须自己用 #ifndef _WIN32 门住。
//
// 与 process.hpp 的 run_command 那套分工:那边是一次性捕获/后台日志,
// stdin 接 /dev/null、无控制终端;这里是交互式会话,子进程 setsid 自立
// 会话并把 PTY 从端设成控制终端,stdin/stdout/stderr 全接从端,Ctrl-C 经
// 线路规程变成 SIGINT 打前台进程组,vim/less/REPL 都当自己坐在真终端前。
#pragma once

#include <cstddef>
#include <expected>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace lubancode::platform {

struct PtySpawnOptions {
    std::string program;                // 绝对路径(调用方先解析好,fork 后不再查 PATH)
    std::vector<std::string> argv;      // argv[0] 起;空则用 program
    std::string cwd;                    // 空 = 继承当前目录
    int rows = 40;
    int cols = 160;
    // 在继承来的环境上覆盖/追加的变量。COLUMNS/LINES 一律剔掉(尺寸走
    // TIOCSWINSZ,留着陈旧值反倒骗程序);TERM 缺省、为空或为 dumb 时补
    // xterm-256color,调用方在这里给值则以这里为准。
    std::vector<std::pair<std::string, std::string>> extra_env;
};

struct PtyChild {
    int master_fd = -1;  // 主端,FD_CLOEXEC + O_NONBLOCK
    int pid = -1;        // 子进程 pid = 其会话 id = 其进程组 id
};

struct PtyExitStatus {
    bool exited = false;
    std::optional<int> exit_code;  // 正常退出
    std::optional<int> signal;     // 被信号杀
};

// 起子进程。失败返回人话错误(含失败阶段与 strerror)。
std::expected<PtyChild, std::string> SpawnPty(const PtySpawnOptions& options);

// 等最多 timeout_ms 读一段主端输出。>0 = 读到的字节数;0 = 超时没数据;
// -1 = 从端全关(子进程那头都退了)或读错,之后不会再有数据。
int PtyRead(int master_fd, char* buffer, std::size_t size, int timeout_ms);

// 写全部字节;主端缓冲满时短睡重试,超过 timeout_ms 仍没写完返回 false。
bool PtyWriteAll(int master_fd, std::string_view data, int timeout_ms);

// 非阻塞探子进程有没有退出(内部 waitpid WNOHANG,退出即收尸)。
PtyExitStatus PtyPollExit(int pid);

// 给会话的进程组发信号:子进程自己那组(pid),以及从端当前的前台进程组
// (交互 bash 开了作业控制,vim 等前台作业另立一组,只打 pid 那组打不着)。
void PtySignalGroups(int pid, int master_fd, int sig);

// 关主端(-1 忽略)。
void PtyCloseMaster(int master_fd);

// 常用信号值的平台中立名(调用方不必 include <csignal> 也能用)。
int PtySigHup();
int PtySigTerm();
int PtySigKill();

}  // namespace lubancode::platform
