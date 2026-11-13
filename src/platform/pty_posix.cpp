// POSIX 实现:posix_openpt + fork + setsid + TIOCSCTTY。
//
// 不用 forkpty/openpty:那两只在 Linux 住 libutil(<pty.h>,老 glibc 要
// -lutil)、macOS 住 <util.h>,还得为链接分平台;posix_openpt/grantpt/
// unlockpt/ptsname 是 POSIX 标准件,两边都在 libc,一份代码通吃。
//
// fork 之后子进程只做 async-signal-safe 调用(close/setsid/ioctl/dup2/
// sigaction/sigprocmask/chdir/execve/write/_exit)——lubancode 是多线程
// 进程,fork 出来的子进程里别的线程手里的锁(malloc 的也算)永远不会放,
// 碰一下就可能死锁。所以环境块、argv 指针数组、最大 fd、信号集全在
// fork 前备好。exec 失败经一条 CLOEXEC 管道回报(阶段 + errno),与
// process_posix.cpp 同一手法。
//
// 验证状态:未经本地真机编译,交 CI 的 ubuntu/macos 两腿亮灯。
#include "platform/pty.hpp"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <fcntl.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>

extern char** environ;

namespace lubancode::platform {

namespace {

// fd 落在 0/1/2 上时挪到 3 以上(带 CLOEXEC):子进程 dup2(slave, 0) 若
// slave 本来就是 0,dup2 是空操作、不清 CLOEXEC,exec 时 stdin 反被关掉;
// 错误管道落在 0..2 也会被 dup2 覆盖。lubancode 自己 stdin 被关过时会撞上。
int MoveAboveStdio(int fd) {
    if (fd < 0 || fd > 2) {
        return fd;
    }
    const int moved = ::fcntl(fd, F_DUPFD_CLOEXEC, 3);
    ::close(fd);
    return moved;
}

void SetCloexec(int fd) {
    const int flags = ::fcntl(fd, F_GETFD);
    if (flags >= 0) {
        ::fcntl(fd, F_SETFD, flags | FD_CLOEXEC);
    }
}

std::vector<std::string> BuildEnvStrings(const PtySpawnOptions& options) {
    const auto overridden = [&options](std::string_view key) {
        if (key == "COLUMNS" || key == "LINES") {
            return true;
        }
        return std::any_of(options.extra_env.begin(), options.extra_env.end(),
                           [key](const auto& kv) { return kv.first == key; });
    };
    std::vector<std::string> out;
    bool has_term = false;
    for (char** p = environ; p != nullptr && *p != nullptr; ++p) {
        const std::string_view entry(*p);
        const std::size_t eq = entry.find('=');
        if (eq == std::string_view::npos || eq == 0) {
            continue;
        }
        const std::string_view key = entry.substr(0, eq);
        if (overridden(key)) {
            continue;
        }
        if (key == "TERM") {
            const std::string_view value = entry.substr(eq + 1);
            if (value.empty() || value == "dumb") {
                continue;  // 非交互宿主常见 TERM=dumb:PTY 里的程序要的是正经终端
            }
            has_term = true;
        }
        out.emplace_back(entry);
    }
    for (const auto& [key, value] : options.extra_env) {
        if (key == "COLUMNS" || key == "LINES") {
            continue;
        }
        out.push_back(key + "=" + value);
        if (key == "TERM") {
            has_term = true;
        }
    }
    if (!has_term) {
        out.emplace_back("TERM=xterm-256color");
    }
    return out;
}

// 子进程失败回报:阶段号 + errno,写进 CLOEXEC 管道后 _exit。
[[noreturn]] void ChildFail(int err_fd, int stage) {
    const int message[2] = {stage, errno};
    ssize_t ignored = ::write(err_fd, message, sizeof(message));
    (void)ignored;
    ::_exit(127);
}

const char* StageName(int stage) {
    switch (stage) {
        case 1: return "setsid";
        case 2: return "设置控制终端(TIOCSCTTY)";
        case 3: return "dup2 标准流";
        case 4: return "chdir";
        case 5: return "execve";
        default: return "未知阶段";
    }
}

}  // namespace

std::expected<PtyChild, std::string> SpawnPty(const PtySpawnOptions& options) {
    if (options.program.empty()) {
        return std::unexpected(std::string("PTY 子进程程序路径为空"));
    }

    int master = ::posix_openpt(O_RDWR | O_NOCTTY);
    if (master < 0) {
        return std::unexpected(std::string("posix_openpt 失败: ") + std::strerror(errno));
    }
    master = MoveAboveStdio(master);
    if (master < 0) {
        return std::unexpected(std::string("PTY 主端挪位失败: ") + std::strerror(errno));
    }
    SetCloexec(master);  // 别让 run_command 起的别的子进程继承主端
    if (::grantpt(master) != 0 || ::unlockpt(master) != 0) {
        const std::string err = std::strerror(errno);
        ::close(master);
        return std::unexpected("grantpt/unlockpt 失败: " + err);
    }
    std::string slave_name;
    {
        // ptsname 返回静态缓冲区、不可重入:进程内只此一处调用,上把锁足矣
        //(ptsname_r 在 macOS 上的可用性随 SDK 版本漂,不赌)。
        static std::mutex ptsname_mutex;
        std::lock_guard<std::mutex> lock(ptsname_mutex);
        const char* name = ::ptsname(master);
        if (name == nullptr) {
            const std::string err = std::strerror(errno);
            ::close(master);
            return std::unexpected("ptsname 失败: " + err);
        }
        slave_name = name;
    }
    int slave = ::open(slave_name.c_str(), O_RDWR | O_NOCTTY | O_CLOEXEC);
    if (slave < 0) {
        const std::string err = std::strerror(errno);
        ::close(master);
        return std::unexpected("打开 PTY 从端 " + slave_name + " 失败: " + err);
    }
    slave = MoveAboveStdio(slave);
    if (slave < 0) {
        const std::string err = std::strerror(errno);
        ::close(master);
        return std::unexpected("PTY 从端挪位失败: " + err);
    }

    // 尺寸与线路规程:新 PTY 的默认 termios 各平台大体一致,这里把交互
    // 要紧的几位明确钉上(回显、规范模式、信号键、CR->NL、DEL 退格)。
    struct winsize ws {};
    ws.ws_row = static_cast<unsigned short>(std::clamp(options.rows, 1, 1000));
    ws.ws_col = static_cast<unsigned short>(std::clamp(options.cols, 1, 1000));
    ::ioctl(slave, TIOCSWINSZ, &ws);
    struct termios tio {};
    if (::tcgetattr(slave, &tio) == 0) {
        tio.c_iflag |= ICRNL;
#ifdef IUTF8
        tio.c_iflag |= IUTF8;  // Linux:退格按 UTF-8 整字删
#endif
        tio.c_oflag |= OPOST | ONLCR;
        tio.c_lflag |= ECHO | ECHOE | ECHOK | ICANON | ISIG | IEXTEN;
        tio.c_cc[VERASE] = 0x7f;
        ::tcsetattr(slave, TCSANOW, &tio);
    }

    int err_pipe[2] = {-1, -1};
    if (::pipe(err_pipe) != 0) {
        const std::string err = std::strerror(errno);
        ::close(master);
        ::close(slave);
        return std::unexpected("创建 exec 回报管道失败: " + err);
    }
    err_pipe[0] = MoveAboveStdio(err_pipe[0]);
    err_pipe[1] = MoveAboveStdio(err_pipe[1]);
    SetCloexec(err_pipe[0]);
    SetCloexec(err_pipe[1]);

    // ---- fork 前把子进程要用的一切备好 ----
    std::vector<std::string> env_strings = BuildEnvStrings(options);
    std::vector<char*> envp;
    envp.reserve(env_strings.size() + 1);
    for (auto& s : env_strings) {
        envp.push_back(s.data());
    }
    envp.push_back(nullptr);

    std::vector<std::string> argv_strings = options.argv;
    if (argv_strings.empty()) {
        argv_strings.push_back(options.program);
    }
    std::vector<char*> argv;
    argv.reserve(argv_strings.size() + 1);
    for (auto& s : argv_strings) {
        argv.push_back(s.data());
    }
    argv.push_back(nullptr);

    const char* program = options.program.c_str();
    const char* cwd = options.cwd.empty() ? nullptr : options.cwd.c_str();
    long max_fd = ::sysconf(_SC_OPEN_MAX);
    // 容器里 RLIMIT_NOFILE 可能是百万级,逐个 close 太慢;上限 65536 之外
    // 的 fd 极罕见,且本进程自己开的 fd 多数带 CLOEXEC。
    if (max_fd < 0 || max_fd > 65536) {
        max_fd = 65536;
    }
    struct sigaction default_action {};
    default_action.sa_handler = SIG_DFL;
    sigemptyset(&default_action.sa_mask);
    sigset_t empty_mask;
    sigemptyset(&empty_mask);
#ifdef NSIG
    const int signal_count = NSIG;
#else
    const int signal_count = 65;
#endif
    const int err_write = err_pipe[1];

    const pid_t pid = ::fork();
    if (pid < 0) {
        const std::string err = std::strerror(errno);
        ::close(master);
        ::close(slave);
        ::close(err_pipe[0]);
        ::close(err_pipe[1]);
        return std::unexpected("fork 失败: " + err);
    }
    if (pid == 0) {
        // ---- 子进程:只许 async-signal-safe ----
        ::close(master);
        ::close(err_pipe[0]);
        if (::setsid() < 0) {
            ChildFail(err_write, 1);
        }
        if (::ioctl(slave, TIOCSCTTY, 0) < 0) {
            ChildFail(err_write, 2);
        }
        if (::dup2(slave, 0) < 0 || ::dup2(slave, 1) < 0 || ::dup2(slave, 2) < 0) {
            ChildFail(err_write, 3);
        }
        ::close(slave);
        // 信号处置复位:lubancode 忽略了 SIGPIPE、可能装了 SIGINT 等处理器,
        // 忽略态会穿过 exec 遗传给 bash 及其作业——bash 里 `yes | head`
        // 就不会收 SIGPIPE 了。逐个复位默认,信号屏蔽字清空。
        for (int s = 1; s < signal_count; ++s) {
            ::sigaction(s, &default_action, nullptr);  // SIGKILL/SIGSTOP 失败无妨
        }
        ::sigprocmask(SIG_SETMASK, &empty_mask, nullptr);
        for (long fd = 3; fd < max_fd; ++fd) {
            if (fd != err_write) {
                ::close(static_cast<int>(fd));
            }
        }
        if (cwd != nullptr && ::chdir(cwd) != 0) {
            ChildFail(err_write, 4);
        }
        ::execve(program, argv.data(), envp.data());
        ChildFail(err_write, 5);
    }

    // ---- 父进程 ----
    ::close(slave);
    ::close(err_pipe[1]);
    int message[2] = {0, 0};
    ssize_t got = 0;
    for (;;) {
        got = ::read(err_pipe[0], message, sizeof(message));
        if (got < 0 && errno == EINTR) {
            continue;
        }
        break;
    }
    ::close(err_pipe[0]);
    if (got == static_cast<ssize_t>(sizeof(message))) {
        int status = 0;
        while (::waitpid(pid, &status, 0) < 0 && errno == EINTR) {
        }
        ::close(master);
        return std::unexpected(std::string("PTY 子进程启动失败(") + StageName(message[0]) +
                               "): " + std::strerror(message[1]));
    }

    const int flags = ::fcntl(master, F_GETFL);
    if (flags >= 0) {
        ::fcntl(master, F_SETFL, flags | O_NONBLOCK);
    }
    return PtyChild{master, static_cast<int>(pid)};
}

int PtyRead(int master_fd, char* buffer, std::size_t size, int timeout_ms) {
    if (master_fd < 0) {
        return -1;
    }
    timeout_ms = std::max(0, timeout_ms);
#ifdef __APPLE__
    // macOS 的 poll(2) 手册 BUGS 一节明说不支持设备文件,PTY 主端用 select;
    // fd 超出 FD_SETSIZE(select 管不了)退回非阻塞读 + 短睡。
    if (master_fd < FD_SETSIZE) {
        fd_set read_set;
        FD_ZERO(&read_set);
        FD_SET(master_fd, &read_set);
        struct timeval tv {};
        tv.tv_sec = timeout_ms / 1000;
        tv.tv_usec = (timeout_ms % 1000) * 1000;
        const int ready = ::select(master_fd + 1, &read_set, nullptr, nullptr, &tv);
        if (ready == 0 || (ready < 0 && errno == EINTR)) {
            return 0;
        }
    } else {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
        for (;;) {
            const ssize_t n = ::read(master_fd, buffer, size);
            if (n > 0) {
                return static_cast<int>(n);
            }
            if (n == 0 || (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)) {
                return -1;
            }
            if (std::chrono::steady_clock::now() >= deadline) {
                return 0;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    }
#else
    struct pollfd pfd {};
    pfd.fd = master_fd;
    pfd.events = POLLIN;
    const int ready = ::poll(&pfd, 1, timeout_ms);
    if (ready == 0 || (ready < 0 && errno == EINTR)) {
        return 0;
    }
    if (ready < 0) {
        return -1;
    }
    if ((pfd.revents & POLLIN) == 0 && (pfd.revents & (POLLHUP | POLLERR | POLLNVAL)) != 0) {
        return -1;
    }
#endif
    const ssize_t n = ::read(master_fd, buffer, size);
    if (n > 0) {
        return static_cast<int>(n);
    }
    if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) {
        return 0;
    }
    return -1;  // 0 字节或 EIO:从端全关
}

bool PtyWriteAll(int master_fd, std::string_view data, int timeout_ms) {
    if (master_fd < 0) {
        return false;
    }
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(std::max(0, timeout_ms));
    std::size_t offset = 0;
    while (offset < data.size()) {
        const ssize_t n = ::write(master_fd, data.data() + offset, data.size() - offset);
        if (n > 0) {
            offset += static_cast<std::size_t>(n);
            continue;
        }
        if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
            return false;
        }
        if (std::chrono::steady_clock::now() >= deadline) {
            return false;
        }
        // 主端输入缓冲满(大段粘贴、程序不读 stdin):读线程照常在抽输出,
        // 短睡等线路规程消化。
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return true;
}

PtyExitStatus PtyPollExit(int pid) {
    PtyExitStatus status;
    if (pid <= 0) {
        status.exited = true;
        return status;
    }
    int raw = 0;
    const pid_t r = ::waitpid(static_cast<pid_t>(pid), &raw, WNOHANG);
    if (r == static_cast<pid_t>(pid)) {
        status.exited = true;
        if (WIFEXITED(raw)) {
            status.exit_code = WEXITSTATUS(raw);
        } else if (WIFSIGNALED(raw)) {
            status.signal = WTERMSIG(raw);
        }
    } else if (r < 0 && errno == ECHILD) {
        status.exited = true;  // 已被别处收尸:退出码不可知,如实留空
    }
    return status;
}

void PtySignalGroups(int pid, int master_fd, int sig) {
    if (pid > 0) {
        ::killpg(static_cast<pid_t>(pid), sig);
    }
    if (master_fd >= 0) {
        const pid_t foreground = ::tcgetpgrp(master_fd);
        if (foreground > 0 && foreground != static_cast<pid_t>(pid)) {
            ::killpg(foreground, sig);
        }
    }
}

void PtyCloseMaster(int master_fd) {
    if (master_fd >= 0) {
        ::close(master_fd);
    }
}

int PtySigHup() {
    return SIGHUP;
}
int PtySigTerm() {
    return SIGTERM;
}
int PtySigKill() {
    return SIGKILL;
}

}  // namespace lubancode::platform
