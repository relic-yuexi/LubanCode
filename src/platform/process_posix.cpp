// POSIX 实现:fork/execvp + 管道 + setpgid 进程组。语义对齐
// process_win.cpp——
//   - 进程组 = Windows 的 Job Object:子进程 setpgid(0,0) 自立门户,超时/
//     关停时 killpg 先 SIGTERM 客气两秒,再 SIGKILL 连后代一起清;根进程
//     正常退出后同样补一记 killpg,对齐 KILL_ON_JOB_CLOSE"后代不许赖着
//     握管道写端"的行为。
//   - 一次性捕获:合并 stdout/stderr 进同一条管道,输出超上限截断保留前段、
//     杀树、继续读空到 EOF(不读的话子进程卡在写上死不掉)。
//   - env 注入:fork 后在子进程里把预先拼好的 envp 赋给 environ 再 exec,
//     不碰父进程环境(比 Windows 的"临时改了再还原"更干净,但语义按更严
//     的那边算,调用方仍然单线程顺序调用)。
//   - exec 失败检测:一条 O_CLOEXEC 管道,exec 成了自动关闭、父进程读到
//     EOF;exec 败了子进程把 errno 写回来,父进程借此分清"命令不存在"
//     (ENOENT -> command_not_found,LSP 层要给友好报错)和其他失败。
//
// 验证状态:WSL Ubuntu 26.04(g++ 15.2)真机编译、单测通过(801 例全绿,
// 含一次性捕获的超时/截断、MCP/LSP 真 Python 夹具走 ChildProcess 全链路),
// 真端点带工具冒烟(run_command 经 /bin/sh)通过;macOS 未经真机验证,
// 待 CI 亮灯。
#include "platform/process.hpp"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <vector>

#include <fcntl.h>
#include <poll.h>
#include <sys/wait.h>
#include <unistd.h>

extern char** environ;

namespace lubancode::platform {

namespace {

// 子进程 stdin 关掉/退出后再往管道写会招 SIGPIPE,默认动作是整个进程死。
// 统一忽略,让 write 返回 EPIPE 错误码走正常失败路径。
void IgnoreSigpipeOnce() {
    static const bool done = [] {
        std::signal(SIGPIPE, SIG_IGN);
        return true;
    }();
    (void)done;
}

// 预先拼好子进程环境块:当前 environ 里被 extra_env 覆盖的条目剔掉,再把
// extra_env 追加进去。storage 持有字符串本体,ptrs 是交给 environ 的指针
// 数组(以 nullptr 收尾)。必须在 fork 之前拼好——fork 之后的子进程上下文
// 里不宜再 malloc。
struct EnvBlock {
    std::vector<std::string> storage;
    std::vector<char*> ptrs;
};

EnvBlock BuildEnvBlock(const EnvPairs& extra_env) {
    EnvBlock block;
    for (char** p = environ; p != nullptr && *p != nullptr; ++p) {
        const std::string entry(*p);
        const std::size_t eq = entry.find('=');
        const std::string key = eq == std::string::npos ? entry : entry.substr(0, eq);
        bool overridden = false;
        for (const auto& [k, v] : extra_env) {
            if (k == key) {
                overridden = true;
                break;
            }
        }
        if (!overridden) {
            block.storage.push_back(entry);
        }
    }
    for (const auto& [k, v] : extra_env) {
        block.storage.push_back(k + "=" + v);
    }
    block.ptrs.reserve(block.storage.size() + 1);
    for (auto& s : block.storage) {
        block.ptrs.push_back(s.data());
    }
    block.ptrs.push_back(nullptr);
    return block;
}

// argv 的 char* 数组(exec 要的形状)。
std::vector<char*> BuildArgvPtrs(std::vector<std::string>& argv) {
    std::vector<char*> ptrs;
    ptrs.reserve(argv.size() + 1);
    for (auto& s : argv) {
        ptrs.push_back(s.data());
    }
    ptrs.push_back(nullptr);
    return ptrs;
}

// 读端全量吸干 exec 失败管道:exec 成了读到 EOF(0 字节),败了读到 errno。
// 返回 std::nullopt = exec 成功。
std::optional<int> ReadExecErrno(int fd) {
    int err = 0;
    ssize_t n = 0;
    do {
        n = read(fd, &err, sizeof(err));
    } while (n < 0 && errno == EINTR);
    if (n <= 0) {
        return std::nullopt;
    }
    return err;
}

// 限时等一个进程退出(不杀)。返回 true = 已退出并收尸,exit_status 有效。
bool WaitPidWithDeadline(pid_t pid, int timeout_ms, int* exit_status) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    while (true) {
        int status = 0;
        const pid_t r = waitpid(pid, &status, WNOHANG);
        if (r == pid) {
            *exit_status = status;
            return true;
        }
        if (r < 0 && errno != EINTR) {
            return false;  // 没有这个孩子(已被别处收尸?)——按"等不到"处理
        }
        if (std::chrono::steady_clock::now() >= deadline) {
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
}

// 杀整个进程组:先 SIGTERM 客气 grace_ms 毫秒,还不退就 SIGKILL,最后
// 阻塞收尸(SIGKILL 之后必定很快退)。对齐 Windows 关 Job 句柄那一下。
void KillProcessGroup(pid_t pid, int grace_ms, int* exit_status) {
    killpg(pid, SIGTERM);
    if (WaitPidWithDeadline(pid, grace_ms, exit_status)) {
        killpg(pid, SIGKILL);  // 根进程退了也补一记,清掉可能残留的后代
        return;
    }
    killpg(pid, SIGKILL);
    int status = 0;
    pid_t r = 0;
    do {
        r = waitpid(pid, &status, 0);
    } while (r < 0 && errno == EINTR);
    if (r == pid) {
        *exit_status = status;
    }
}

unsigned long ExitCodeFromStatus(int status) {
    if (WIFEXITED(status)) {
        return static_cast<unsigned long>(WEXITSTATUS(status));
    }
    if (WIFSIGNALED(status)) {
        return static_cast<unsigned long>(128 + WTERMSIG(status));
    }
    return 1;
}

// ---------------------------------------------------------------------------
// 后台模式(run_command 的 run_in_background)专用小工具:会话级 PID 注册表
// + 退出钩子。语义对齐 process_win.cpp 的会话级 Job Object,但 POSIX 没有
// "关句柄连带杀光"这种内核机制,只能自己记账、自己在退出时补杀——
// atexit 覆盖正常退出(main 返回、std::exit)那条路径,覆盖不了被信号杀死
// 之类的异常终止,这是跟 Windows 那边唯一的语义差(Windows 靠内核收句柄
// 是更强的保证)。
// ---------------------------------------------------------------------------

struct BackgroundRegistryState {
    std::mutex mutex;
    std::vector<pid_t> pids;
};

BackgroundRegistryState& BackgroundRegistry() {
    // atexit 回调和 detach 的收尸线程都可能活到普通静态对象析构之后。
    // 这份进程级状态故意留到操作系统收走，免得退出途中碰到已析构的锁。
    static auto* state = new BackgroundRegistryState;
    return *state;
}

// atexit 钩子:逐个把会话级注册表里还没被摘掉(即还没被收尸线程 waitpid
// 收走)的后台子进程杀干净。SIGTERM 客气一下——但 atexit 阶段不适合像
// KillProcessGroup 那样阻塞等 grace period,直接紧跟着补一记 SIGKILL,
// 反正 lubancode 马上就要退出了,没有谁还等着这些子进程体面退出的机会。
// kill(-pid, ...) 打的是整个会话/进程组(子进程 fork 后 setsid() 了,
// pid == 会话首进程 pid == 进程组 pid)。
void KillAllBackgroundOnExit() {
    BackgroundRegistryState& state = BackgroundRegistry();
    std::lock_guard<std::mutex> lock(state.mutex);
    for (const pid_t pid : state.pids) {
        kill(-pid, SIGTERM);
    }
    for (const pid_t pid : state.pids) {
        kill(-pid, SIGKILL);
    }
}

void RegisterBackgroundPid(pid_t pid) {
    static const bool registered = [] {
        std::atexit(KillAllBackgroundOnExit);
        return true;
    }();
    (void)registered;
    BackgroundRegistryState& state = BackgroundRegistry();
    std::lock_guard<std::mutex> lock(state.mutex);
    state.pids.push_back(pid);
}

void UnregisterBackgroundPid(pid_t pid) {
    BackgroundRegistryState& state = BackgroundRegistry();
    std::lock_guard<std::mutex> lock(state.mutex);
    state.pids.erase(std::remove(state.pids.begin(), state.pids.end(), pid), state.pids.end());
}

// 后台日志文件路径:系统临时目录下,文件名带毫秒时间戳 + 单调计数器,
// 两者叠加保证同一毫秒内并发起多个后台命令也不会撞名。
std::string BuildBackgroundLogPath() {
    static std::atomic<unsigned long long> counter{0};
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::system_clock::now().time_since_epoch())
                        .count();
    const unsigned long long seq = counter.fetch_add(1);
    const std::filesystem::path dir = std::filesystem::temp_directory_path();
    const std::string filename = "lubancode_bg_" + std::to_string(ms) + "_" + std::to_string(seq) + ".log";
    return (dir / filename).string();
}

struct SpawnedMerged {
    pid_t pid = -1;
    int output_fd = -1;  // 合并的 stdout+stderr 读端
};

// 起一个"合并输出、stdin 接 /dev/null"的子进程。失败时 result 里带人话。
bool SpawnMergedOutput(std::vector<std::string> argv, const EnvPairs& extra_env, SpawnedMerged* spawned,
                        ProcessResult* result) {
    IgnoreSigpipeOnce();

    int out_pipe[2] = {-1, -1};
    if (pipe(out_pipe) != 0) {
        result->spawn_failed = true;
        result->spawn_error = std::string("创建管道失败: ") + std::strerror(errno);
        return false;
    }
    int exec_pipe[2] = {-1, -1};
    if (pipe(exec_pipe) != 0) {
        result->spawn_failed = true;
        result->spawn_error = std::string("创建管道失败: ") + std::strerror(errno);
        close(out_pipe[0]);
        close(out_pipe[1]);
        return false;
    }
    fcntl(exec_pipe[1], F_SETFD, FD_CLOEXEC);
    // 父进程留的读端不给子进程继承(CLOEXEC),不然后代握着写端 EOF 等不到。
    fcntl(out_pipe[0], F_SETFD, FD_CLOEXEC);
    fcntl(exec_pipe[0], F_SETFD, FD_CLOEXEC);

    EnvBlock env_block = BuildEnvBlock(extra_env);
    std::vector<char*> argv_ptrs = BuildArgvPtrs(argv);

    const pid_t pid = fork();
    if (pid < 0) {
        result->spawn_failed = true;
        result->spawn_error = std::string("fork 失败: ") + std::strerror(errno);
        close(out_pipe[0]);
        close(out_pipe[1]);
        close(exec_pipe[0]);
        close(exec_pipe[1]);
        return false;
    }
    if (pid == 0) {
        // 子进程:自立进程组(超时好一锅端),stdin 接 /dev/null,stdout/
        // stderr 都指到管道写端。只用 async-signal-safe 的调用。
        setpgid(0, 0);
        const int devnull = open("/dev/null", O_RDONLY);
        if (devnull >= 0) {
            dup2(devnull, STDIN_FILENO);
            if (devnull > STDERR_FILENO) {
                close(devnull);
            }
        }
        dup2(out_pipe[1], STDOUT_FILENO);
        dup2(out_pipe[1], STDERR_FILENO);
        if (out_pipe[1] > STDERR_FILENO) {
            close(out_pipe[1]);
        }
        environ = env_block.ptrs.data();
        execvp(argv_ptrs[0], argv_ptrs.data());
        const int err = errno;
        (void)!write(exec_pipe[1], &err, sizeof(err));
        _exit(127);
    }

    // 父进程:setpgid 两头都调,谁先跑到都不留窗口。
    setpgid(pid, pid);
    close(out_pipe[1]);
    close(exec_pipe[1]);

    const std::optional<int> exec_err = ReadExecErrno(exec_pipe[0]);
    close(exec_pipe[0]);
    if (exec_err.has_value()) {
        int status = 0;
        waitpid(pid, &status, 0);  // 子进程 _exit(127) 了,收尸
        close(out_pipe[0]);
        result->spawn_failed = true;
        result->spawn_error = "启动子进程失败(" + std::string(std::strerror(*exec_err)) + "): " + argv[0];
        return false;
    }

    spawned->pid = pid;
    spawned->output_fd = out_pipe[0];
    return true;
}

}  // namespace

ProcessResult RunProcess(const std::vector<std::string>& argv, int timeout_ms, const EnvPairs& extra_env,
                          std::size_t max_output_bytes) {
    ProcessResult result;
    if (argv.empty()) {
        result.spawn_failed = true;
        result.spawn_error = "argv 不能为空";
        return result;
    }

    SpawnedMerged spawned;
    if (!SpawnMergedOutput(argv, extra_env, &spawned, &result)) {
        return result;
    }

    // 读线程:poll + read,攒到上限就置 overflow(主循环看到立刻杀树),但
    // 继续读空丢弃到 EOF——跟 Windows 版一个道理,别让子进程卡在写上。
    std::string output;
    std::atomic<bool> output_over_limit{false};
    std::atomic<bool> reader_stop{false};
    std::atomic<bool> reader_done{false};
    std::thread reader([&] {
        char buf[4096];
        while (!reader_stop.load()) {
            struct pollfd pfd{spawned.output_fd, POLLIN, 0};
            const int pr = poll(&pfd, 1, 100);
            if (pr < 0) {
                if (errno == EINTR) {
                    continue;
                }
                break;
            }
            if (pr == 0) {
                continue;  // 超时,回头看看停止标志
            }
            const ssize_t n = read(spawned.output_fd, buf, sizeof(buf));
            if (n < 0 && errno == EINTR) {
                continue;
            }
            if (n <= 0) {
                break;  // EOF / 出错,写端全关了
            }
            if (output.size() < max_output_bytes) {
                const std::size_t room = max_output_bytes - output.size();
                output.append(buf, std::min(static_cast<std::size_t>(n), room));
                if (output.size() >= max_output_bytes) {
                    output_over_limit.store(true);
                }
            }
            // 超限之后继续读但直接丢弃——别让子进程卡在写上,读空到 EOF。
        }
        reader_done.store(true);
    });

    // 主循环:等退出 / 超时 / 输出超限,三个条件谁先到算谁的。
    const bool has_timeout = timeout_ms > 0;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(has_timeout ? timeout_ms : 0);
    int exit_status = 0;
    bool exited = false;
    while (true) {
        int status = 0;
        const pid_t r = waitpid(spawned.pid, &status, WNOHANG);
        if (r == spawned.pid) {
            exit_status = status;
            exited = true;
            break;
        }
        if (r < 0 && errno != EINTR) {
            break;  // 不该发生;当已退出处理,exit_status 保持 0
        }
        if (output_over_limit.load()) {
            KillProcessGroup(spawned.pid, 2000, &exit_status);
            exited = true;
            break;
        }
        if (has_timeout && std::chrono::steady_clock::now() >= deadline) {
            result.timed_out = true;
            KillProcessGroup(spawned.pid, 2000, &exit_status);
            exited = true;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    // 根进程退了不代表后代也退了(Windows 关 Job 那一下的对齐):补杀整组,
    // 后代握着的写端全关,读线程才能等到 EOF 收尾。
    killpg(spawned.pid, SIGKILL);

    // 限时等读线程把尾巴读到 EOF 自然退出,等不到就置停止标志——poll 循环
    // 100ms 一醒,最多再等一轮就收,绝不吊死。
    const auto reader_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (!reader_done.load() && std::chrono::steady_clock::now() < reader_deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    reader_stop.store(true);
    reader.join();

    if (!exited) {
        int status = 0;
        if (waitpid(spawned.pid, &status, WNOHANG) == spawned.pid) {
            exit_status = status;
        }
    }

    close(spawned.output_fd);

    result.exit_code = ExitCodeFromStatus(exit_status);
    result.output = std::move(output);
    result.output_truncated = output_over_limit.load();
    return result;
}

ProcessResult RunShellCommand(const std::string& command_utf8, int timeout_ms, const EnvPairs& extra_env,
                               std::size_t max_output_bytes) {
    // POSIX 的 shell 一律 /bin/sh -c;输出天然 UTF-8,不需要 Windows 那道
    // ANSI 代码页转换。
    return RunProcess({"/bin/sh", "-c", command_utf8}, timeout_ms, extra_env, max_output_bytes);
}

// ---------------------------------------------------------------------------
// 后台模式:spawn 立刻返回,不等待、不捕获进内存,stdout/stderr 直接 dup2
// 到日志文件描述符上。
// ---------------------------------------------------------------------------

BackgroundSpawnResult RunProcessBackground(const std::vector<std::string>& argv, const EnvPairs& extra_env) {
    BackgroundSpawnResult result;
    if (argv.empty()) {
        result.error = "argv 不能为空";
        return result;
    }
    IgnoreSigpipeOnce();

    const std::string log_path = BuildBackgroundLogPath();
    const int log_fd = open(log_path.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (log_fd < 0) {
        result.error = std::string("创建日志文件失败: ") + std::strerror(errno);
        return result;
    }

    int exec_pipe[2] = {-1, -1};
    if (pipe(exec_pipe) != 0) {
        result.error = std::string("创建管道失败: ") + std::strerror(errno);
        close(log_fd);
        return result;
    }
    fcntl(exec_pipe[0], F_SETFD, FD_CLOEXEC);
    fcntl(exec_pipe[1], F_SETFD, FD_CLOEXEC);

    EnvBlock env_block = BuildEnvBlock(extra_env);
    std::vector<std::string> argv_copy = argv;
    std::vector<char*> argv_ptrs = BuildArgvPtrs(argv_copy);

    const pid_t pid = fork();
    if (pid < 0) {
        result.error = std::string("fork 失败: ") + std::strerror(errno);
        close(log_fd);
        close(exec_pipe[0]);
        close(exec_pipe[1]);
        return result;
    }
    if (pid == 0) {
        // 子进程:setsid() 脱离 lubancode 所在的会话/进程组/控制终端,
        // 自成一个新会话(pid 同时成了新会话的会话首 = 新进程组的组长)。
        // 这样 lubancode 退出/它所在终端挂断,不会顺带给这个后台子进程
        // 发 SIGHUP,也不会被"杀当前进程组"这类操作连坐。
        setsid();
        const int devnull = open("/dev/null", O_RDONLY);
        if (devnull >= 0) {
            dup2(devnull, STDIN_FILENO);
            if (devnull > STDERR_FILENO) {
                close(devnull);
            }
        }
        dup2(log_fd, STDOUT_FILENO);
        dup2(log_fd, STDERR_FILENO);
        if (log_fd > STDERR_FILENO) {
            close(log_fd);
        }
        environ = env_block.ptrs.data();
        execvp(argv_ptrs[0], argv_ptrs.data());
        const int err = errno;
        (void)!write(exec_pipe[1], &err, sizeof(err));
        _exit(127);
    }

    // 父进程:子进程该拿到的 fd 已经拿到了(继承来的),这两个可以关了。
    close(log_fd);
    close(exec_pipe[1]);

    const std::optional<int> exec_err = ReadExecErrno(exec_pipe[0]);
    close(exec_pipe[0]);
    if (exec_err.has_value()) {
        int status = 0;
        waitpid(pid, &status, 0);  // 子进程 _exit(127) 了,收尸
        result.error = "启动子进程失败(" + std::string(std::strerror(*exec_err)) + "): " + argv[0];
        return result;
    }

    RegisterBackgroundPid(pid);
    // 收尸线程:一次性 detach 的 waitpid,防止子进程结束后没人收尸变成
    // 僵尸(lubancode 还没退出、atexit 钩子还没跑之前,子进程可能早就
    // 退出了)。退出时把自己从会话级注册表摘掉,免得 atexit 钩子对着一个
    // 早就死透、pid 可能已被系统回收复用的号码瞎杀。
    std::thread([pid] {
        int status = 0;
        waitpid(pid, &status, 0);
        UnregisterBackgroundPid(pid);
    }).detach();

    result.success = true;
    result.pid = static_cast<unsigned long>(pid);
    result.log_path = log_path;
    return result;
}

BackgroundSpawnResult RunShellCommandBackground(const std::string& command_utf8, const EnvPairs& extra_env) {
    return RunProcessBackground({"/bin/sh", "-c", command_utf8}, extra_env);
}

// ---------------------------------------------------------------------------
// ChildProcess:长命双向管道(语义对齐 process_win.cpp 的同名类)。
// ---------------------------------------------------------------------------

ChildProcess::~ChildProcess() {
    Shutdown(2000);
}

SpawnResult ChildProcess::Start(const std::string& command, const std::vector<std::string>& args,
                                  const EnvPairs& env, std::function<bool(std::string_view)> on_stdout,
                                  std::function<void(std::string_view)> on_stderr) {
    IgnoreSigpipeOnce();
    on_stdout_ = std::move(on_stdout);
    on_stderr_ = std::move(on_stderr);

    int in_pipe[2] = {-1, -1};   // 父写 -> 子 stdin
    int out_pipe[2] = {-1, -1};  // 子 stdout -> 父读
    int err_pipe[2] = {-1, -1};  // 子 stderr -> 父读
    int exec_pipe[2] = {-1, -1};
    const auto close_all = [&] {
        for (int* p : {in_pipe, out_pipe, err_pipe, exec_pipe}) {
            if (p[0] >= 0) close(p[0]);
            if (p[1] >= 0) close(p[1]);
        }
    };
    if (pipe(in_pipe) != 0 || pipe(out_pipe) != 0 || pipe(err_pipe) != 0 || pipe(exec_pipe) != 0) {
        const std::string err = std::strerror(errno);
        close_all();
        return SpawnResult{false, "创建管道失败: " + err, false};
    }
    // 父进程留的端一律 CLOEXEC,不给后代继承。
    fcntl(in_pipe[1], F_SETFD, FD_CLOEXEC);
    fcntl(out_pipe[0], F_SETFD, FD_CLOEXEC);
    fcntl(err_pipe[0], F_SETFD, FD_CLOEXEC);
    fcntl(exec_pipe[0], F_SETFD, FD_CLOEXEC);
    fcntl(exec_pipe[1], F_SETFD, FD_CLOEXEC);

    std::vector<std::string> argv;
    argv.reserve(args.size() + 1);
    argv.push_back(command);
    argv.insert(argv.end(), args.begin(), args.end());
    EnvBlock env_block = BuildEnvBlock(env);
    std::vector<char*> argv_ptrs = BuildArgvPtrs(argv);

    const pid_t pid = fork();
    if (pid < 0) {
        const std::string err = std::strerror(errno);
        close_all();
        return SpawnResult{false, "fork 失败: " + err, false};
    }
    if (pid == 0) {
        setpgid(0, 0);
        dup2(in_pipe[0], STDIN_FILENO);
        dup2(out_pipe[1], STDOUT_FILENO);
        dup2(err_pipe[1], STDERR_FILENO);
        for (int fd : {in_pipe[0], in_pipe[1], out_pipe[0], out_pipe[1], err_pipe[0], err_pipe[1]}) {
            if (fd > STDERR_FILENO) {
                close(fd);
            }
        }
        environ = env_block.ptrs.data();
        execvp(argv_ptrs[0], argv_ptrs.data());
        const int err = errno;
        (void)!write(exec_pipe[1], &err, sizeof(err));
        _exit(127);
    }

    setpgid(pid, pid);
    close(in_pipe[0]);
    close(out_pipe[1]);
    close(err_pipe[1]);
    close(exec_pipe[1]);

    const std::optional<int> exec_err = ReadExecErrno(exec_pipe[0]);
    close(exec_pipe[0]);
    if (exec_err.has_value()) {
        int status = 0;
        waitpid(pid, &status, 0);
        close(in_pipe[1]);
        close(out_pipe[0]);
        close(err_pipe[0]);
        SpawnResult spawn{};
        spawn.success = false;
        spawn.error = "启动子进程失败(" + std::string(std::strerror(*exec_err)) + "): " + command;
        spawn.command_not_found = (*exec_err == ENOENT);
        return spawn;
    }

    pid_ = pid;
    stdin_fd_ = in_pipe[1];
    stdout_fd_ = out_pipe[0];
    stderr_fd_ = err_pipe[0];

    started_ = true;
    stdout_thread_ = std::thread([this] { StdoutReaderThread(); });
    stderr_thread_ = std::thread([this] { StderrReaderThread(); });

    return SpawnResult{true, std::string(), false};
}

namespace {

// 读线程共用的骨架:poll(100ms)+ read,停止标志一置最多再等一轮。
template <typename Callback>
void ReaderLoop(int fd, std::atomic<bool>& stop, Callback&& deliver) {
    char buf[4096];
    while (!stop.load()) {
        struct pollfd pfd{fd, POLLIN, 0};
        const int pr = poll(&pfd, 1, 100);
        if (pr < 0) {
            if (errno == EINTR) {
                continue;
            }
            break;
        }
        if (pr == 0) {
            continue;
        }
        const ssize_t n = read(fd, buf, sizeof(buf));
        if (n < 0 && errno == EINTR) {
            continue;
        }
        if (n <= 0) {
            break;  // EOF / 出错
        }
        if (!deliver(std::string_view(buf, static_cast<std::size_t>(n)))) {
            break;
        }
    }
}

}  // namespace

void ChildProcess::StdoutReaderThread() {
    ReaderLoop(stdout_fd_, reader_stop_, [this](std::string_view chunk) {
        return !on_stdout_ || on_stdout_(chunk);
    });
    stdout_reader_done_.store(true);
}

void ChildProcess::StderrReaderThread() {
    ReaderLoop(stderr_fd_, reader_stop_, [this](std::string_view chunk) {
        if (on_stderr_) {
            on_stderr_(chunk);
        }
        return true;
    });
    stderr_reader_done_.store(true);
}

void ChildProcess::JoinReaderThreads() {
    // 读线程的 poll 循环 100ms 一醒:先给 2 秒让它把尾巴读到 EOF 自然退出,
    // 等不到就置停止标志,最多再一轮 poll 就收——绝不吊死。
    const auto wait_reader = [this](std::thread& thread, std::atomic<bool>& done) {
        if (!thread.joinable()) {
            return;
        }
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        while (!done.load() && std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        reader_stop_.store(true);
        thread.join();
    };
    wait_reader(stdout_thread_, stdout_reader_done_);
    wait_reader(stderr_thread_, stderr_reader_done_);
}

bool ChildProcess::Write(const std::string& data) {
    if (!started_ || stdin_fd_ < 0 || !IsAlive()) {
        return false;
    }
    std::lock_guard<std::mutex> lock(write_mutex_);
    std::size_t offset = 0;
    while (offset < data.size()) {
        const ssize_t n = write(stdin_fd_, data.data() + offset, data.size() - offset);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            return false;  // EPIPE(对端死了)等,SIGPIPE 已忽略
        }
        if (n == 0) {
            return false;
        }
        offset += static_cast<std::size_t>(n);
    }
    return true;
}

void ChildProcess::Kill() {
    if (pid_ > 0) {
        kill(static_cast<pid_t>(pid_), SIGKILL);
    }
}

void ChildProcess::Shutdown(int wait_ms) {
    if (!started_ || shutdown_done_.exchange(true)) {
        return;
    }

    // 先关 stdin——给子进程发 EOF,行为良好的服务器自己体面退出。
    if (stdin_fd_ >= 0) {
        close(stdin_fd_);
        stdin_fd_ = -1;
    }

    if (pid_ > 0) {
        std::lock_guard<std::mutex> lock(wait_mutex_);
        const pid_t pid = static_cast<pid_t>(pid_);
        int status = 0;
        bool exited = reaped_;
        if (!exited && WaitPidWithDeadline(pid, wait_ms > 0 ? wait_ms : 0, &status)) {
            exited = true;
            reaped_ = true;
            exit_status_ = status;
        }
        if (!exited) {
            KillProcessGroup(pid, 2000, &status);
            reaped_ = true;
            exit_status_ = status;
        }
        // 后代若还握着 stdout/stderr 写端,读线程等不到 EOF——补杀整组,
        // 对齐 Windows 关 Job 句柄的行为。
        killpg(pid, SIGKILL);
    }

    JoinReaderThreads();

    if (stdout_fd_ >= 0) {
        close(stdout_fd_);
        stdout_fd_ = -1;
    }
    if (stderr_fd_ >= 0) {
        close(stderr_fd_);
        stderr_fd_ = -1;
    }
    pid_ = -1;
}

bool ChildProcess::IsAlive() const {
    if (pid_ <= 0) {
        return false;
    }
    std::lock_guard<std::mutex> lock(wait_mutex_);
    if (reaped_) {
        return false;
    }
    int status = 0;
    const pid_t r = waitpid(static_cast<pid_t>(pid_), &status, WNOHANG);
    if (r == static_cast<pid_t>(pid_)) {
        reaped_ = true;
        exit_status_ = status;
        return false;
    }
    return r == 0;  // 0 = 还活着;-1 = 查不到,当死了算
}

bool IsProcessAlive(unsigned long pid) {
    if (pid == 0) {
        return false;
    }
    if (static_cast<pid_t>(pid) == ::getpid()) {
        return true;
    }
    // kill(pid, 0):0 = 活着;EPERM = 活着但没权限;ESRCH = 不在了。
    return ::kill(static_cast<pid_t>(pid), 0) == 0 || errno == EPERM;
}

unsigned long CurrentProcessId() { return static_cast<unsigned long>(::getpid()); }

}  // namespace lubancode::platform
