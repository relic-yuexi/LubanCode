// POSIX 实现:fork/execvp + 管道 + setpgid 进程组。语义对齐
// process_win.cpp——
//   - 进程组 = Windows 的 Job Object:子进程 setpgid(0,0) 自立门户,超时/
//     关停时 killpg 先 SIGTERM 客气两秒,再 SIGKILL 连后代一起清;根进程
//     正常退出后同样补一记 killpg,对齐 KILL_ON_JOB_CLOSE"后代不许赖着
//     握管道写端"的行为。
//   - 一次性捕获:合并 stdout/stderr 进同一条管道,输出超上限截断保留前段、
//     杀树、继续读空到 EOF(不读的话子进程卡在写上死不掉)。
//   - env 注入:fork 后在子进程里把预先拼好的 envp 赋给 environ 再 exec,
//     不碰父进程环境(Windows 侧进程生命线单 P0 之后同样不改父环境,两
//     平台一张合同:并发调用不串值)。
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
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>

#include "platform/text_encoding.hpp"  // Utf8PrefixBoundary:输出帽对齐 UTF-8 边界
extern char** environ;

namespace lubancode::platform {

namespace {

// 输出帽是字节刀,但刀口不许劈进多字节序列的腰里——中文输出恰好在
// limit 上断成 0xE5 开头的半截汉字,这坨字节流到 nlohmann::json 序列化
// 就是 type_error.316(0.26.41 真机崩的根因)。截断后对齐到码点边界。
// 两条路(与 process_win.cpp 同一份逻辑,别写第二份解码器):
//   - output 超过帽(WithStdin 全量攒再置旗那路):Utf8PrefixBoundary
//     正常语义,退到截断点前的整字边界。
//   - output 恰好满帽(主路径 reader 的 take 填满即停):offset ==
//     size 时帮手直接返回 size,得换姿势——先看整段是否合法,非法且
//     首个坏字节落在"去掉最后一字节的安全前缀"之外(即只在尾巴上),
//     才退到安全前缀。正文中间就有坏字节的(整段 GBK 那类)不动——
//     清洗归调用方,这里只治"自己这刀劈出来的悬空"。
void AlignOutputToUtf8Boundary(std::string& output, std::size_t max_output_bytes) {
    if (output.size() > max_output_bytes) {
        output.resize(Utf8PrefixBoundary(output, max_output_bytes));
        return;
    }
    if (output.size() < 2 || output.size() != max_output_bytes) {
        return;  // 没满帽:整段完整交付,悬空只可能来自子进程自己,不归这刀管
    }
    const std::size_t first_bad = FirstInvalidUtf8Offset(output);
    if (first_bad == std::string::npos) {
        return;  // 满帽且合法:刀口恰好落在字缝上
    }
    const std::size_t tail_safe = Utf8PrefixBoundary(output, output.size() - 1);
    if (first_bad >= tail_safe) {
        output.resize(tail_safe);  // 坏在尾巴:退掉悬空的半截字
    }
}

// 子进程 stdin 关掉/退出后再往管道写会招 SIGPIPE,默认动作是整个进程死。
// 统一忽略,让 write 返回 EPIPE 错误码走正常失败路径。
void IgnoreSigpipeOnce() {
    static const bool done = [] {
        std::signal(SIGPIPE, SIG_IGN);
        return true;
    }();
    (void)done;
}

// 预先拼好子进程环境块:Inherit 模式把当前 environ 里被 extra_env 覆盖的
// 条目剔掉再追加 extra_env;Replace 模式只放 extra_env(宿主环境一概不
// 递——plugins 单第 8 步的最小环境硬保证)。storage 持有字符串本体,ptrs
// 是交给 environ 的指针数组(以 nullptr 收尾)。必须在 fork 之前拼好——
// fork 之后的子进程上下文里不宜再 malloc。
struct EnvBlock {
    std::vector<std::string> storage;
    std::vector<char*> ptrs;
};

// env 条目合法性预检(与 Windows 同一张合同):键空/含 '='/含 NUL,值含
// NUL —— 一律 spawn_failed,人话写明哪条。返回空串 = 全部合法。
std::string ValidateEnvPairs(const EnvPairs& extra_env) {
    for (const auto& [k, v] : extra_env) {
        if (k.empty() || k.find('=') != std::string::npos || k.find('\0') != std::string::npos) {
            return "环境变量名非法(空/含 '=' 或 NUL): " + k;
        }
        if (v.find('\0') != std::string::npos) {
            return "环境变量值含 NUL: " + k;
        }
    }
    return std::string();
}

EnvBlock BuildEnvBlock(const EnvPairs& extra_env, EnvMode env_mode) {
    EnvBlock block;
    // 预检(与 Windows 的 BuildMergedEnvironmentBlock 同一张合同):键名空/
    // 含 '='(POSIX env key 的 '=' 会改语义)/含 NUL,值含 NUL,一律拒绝。
    // BuildEnvBlock 没有 error 出口,非法条目直接丢弃——调用方(RunProcess
    // 一族)在 fork 之前另行预检并报 spawn_failed,这里只保底不把坏条目
    // 塞进子进程环境。
    if (env_mode == EnvMode::Inherit) {
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
    }
    for (const auto& [k, v] : extra_env) {
        if (k.empty() || k.find('=') != std::string::npos || k.find('\0') != std::string::npos ||
            v.find('\0') != std::string::npos) {
            continue;  // 非法条目丢弃(调用方预检在先,这里保底)
        }
        block.storage.push_back(k + "=" + v);
    }
    block.ptrs.reserve(block.storage.size() + 1);
    for (auto& s : block.storage) {
        block.ptrs.push_back(s.data());
    }
    block.ptrs.push_back(nullptr);
    return block;
}

// argv 的 char* 数组(exec 要的形状)。exec 家族不改 argv 指向的字符串,
// 签名里的 char* 是 POSIX 历史遗留,const_cast 安全。
std::vector<char*> BuildArgvPtrs(const std::vector<std::string>& argv) {
    std::vector<char*> ptrs;
    ptrs.reserve(argv.size() + 1);
    for (const auto& s : argv) {
        ptrs.push_back(const_cast<char*>(s.data()));
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

// 后台日志文件路径:系统临时目录下,文件名带毫秒时间戳 + 单调计数器 + 一段
// 随机尾巴,三者叠加保证同一毫秒内并发起多个后台命令也不撞名(也堵住
// "文件名可猜 + O_TRUNC 无 O_EXCL"的共享临时目录竞态/链接攻击面)。
std::string BuildBackgroundLogPath() {
    static std::atomic<unsigned long long> counter{0};
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::system_clock::now().time_since_epoch())
                        .count();
    const unsigned long long seq = counter.fetch_add(1);
    // 随机尾巴:mkstemp 风格的不可猜后缀。/dev/urandom 读不到就退化为
    // pid + steady_clock 计数(仍是进程内单调,只是可猜;O_EXCL 兜底不撞)。
    char rand_suffix[16] = {};
    bool have_rand = false;
    const int urnd = open("/dev/urandom", O_RDONLY | O_CLOEXEC);
    if (urnd >= 0) {
        have_rand = read(urnd, rand_suffix, sizeof(rand_suffix)) == static_cast<ssize_t>(sizeof(rand_suffix));
        close(urnd);
    }
    std::string suffix;
    if (have_rand) {
        static const char kHex[] = "0123456789abcdef";
        suffix.reserve(sizeof(rand_suffix) * 2);
        for (const char c : rand_suffix) {
            suffix.push_back(kHex[(static_cast<unsigned char>(c) >> 4) & 0xF]);
            suffix.push_back(kHex[static_cast<unsigned char>(c) & 0xF]);
        }
    } else {
        suffix = std::to_string(static_cast<unsigned long long>(::getpid())) + "_" +
                 std::to_string(static_cast<unsigned long long>(
                                    std::chrono::steady_clock::now().time_since_epoch().count()));
    }
    const std::filesystem::path dir = std::filesystem::temp_directory_path();
    const std::string filename =
        "lubancode_bg_" + std::to_string(ms) + "_" + std::to_string(seq) + "_" + suffix + ".log";
    // POSIX 例外:path 的窄口就是本机字节串(UTF-8),.string() 在这里既
    // 正确又必要,不换成 PathToUtf8(那是 Windows ACP 窄口的保险)。
    return (dir / filename).string();
}

// 以 0600 独占创建后台日志(O_CREAT|O_EXCL|O_NOFOLLOW:文件已存在/被人预置
// symlink 就换名重来,不开别人的文件)。命令输出可能带 token,0644 会让
// 同机其他账号读到——权限这道墙必须落。
int OpenBackgroundLogExclusive(const std::string& path) {
    return open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
}

// ---------------------------------------------------------------------------
// BackgroundProcessHandle(进程生命线单 P0):POSIX 侧。子进程照旧 setsid
// 脱离会话(后台长命的语义不变),waitpid 只留一处——由 spawn 时起的
// 唯一收尸线程做,退出状态写进共享完成态、广播给所有等待方;registry 的
// watcher/Stop 只读完成态与发信号,不再抢收尸、不再 kill(pid,0) 猜。
// ---------------------------------------------------------------------------

}  // namespace

struct BackgroundProcessHandle::Impl {
    pid_t pid = -1;
    // 收尸线程广播完成用(Wait 挂在这上面,不轮询不抢 waitpid)。
    std::condition_variable done_cv;
};

BackgroundProcessHandle::BackgroundProcessHandle() : impl(std::make_unique<Impl>()) {}

BackgroundProcessHandle::~BackgroundProcessHandle() {
    // 析构时进程可能还活着(dev server 这类)。会话级 atexit 注册表还记着
    // 它,lubancode 退出时统一收尾;这里不做析构即杀(句柄语义 = 观察窗,
    // 不是所有权——Stop 才是杀的口)。
}

bool BackgroundProcessHandle::Wait(int timeout_ms) {
    std::unique_lock<std::mutex> lock(mutex_);
    if (impl->pid <= 0) {
        return true;  // 没起过
    }
    if (timeout_ms <= 0) {
        return completion_known_;
    }
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    while (!completion_known_) {
        if (impl->done_cv.wait_until(lock, deadline) == std::cv_status::timeout) {
            break;
        }
    }
    return completion_known_;
}

void BackgroundProcessHandle::RecordExit(int status) {
    // 唯一收尸线程调用:status 是 waitpid 的原样返回(失败的调用方走
    // MarkUnknown,不许把 -1 之类塞进来——WIF* 宏会把它解成信号)。
    std::lock_guard<std::mutex> lock(mutex_);
    if (completion_known_) {
        return;
    }
    if (WIFEXITED(status)) {
        completion_.known = true;
        completion_.exit_code = WEXITSTATUS(status);
    } else if (WIFSIGNALED(status)) {
        completion_.known = true;
        completion_.signal = WTERMSIG(status);
        // shell 风格展示码可派生 128+signal,原始字段(signal)保留不丢。
        completion_.exit_code = 128 + WTERMSIG(status);
    } else {
        completion_.known = false;
    }
    completion_known_ = true;
    impl->done_cv.notify_all();
}

void BackgroundProcessHandle::MarkUnknown() {
    // 收尸方拿不到状态(收不到/ECHILD):如实标未知,绝不借 0 冒充成功。
    std::lock_guard<std::mutex> lock(mutex_);
    if (!completion_known_) {
        completion_.known = false;
        completion_known_ = true;
        impl->done_cv.notify_all();
    }
}

BackgroundProcessHandle::Completion BackgroundProcessHandle::Peek() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return completion_;
}

bool BackgroundProcessHandle::IsAlive() {
    if (impl->pid <= 0) {
        return false;
    }
    return !Peek().known;
}

bool BackgroundProcessHandle::TerminateTree(int grace_ms) {
    if (impl->pid <= 0) {
        return true;
    }
    // 子进程 setsid 了,pid 就是它自己的进程组 id。先 SIGTERM 整组,等一个
    // 短 grace(树内进程可自行清理),再 SIGKILL 整组。收尸由唯一收尸线程
    // 做(它阻塞在 waitpid 上,进程一退自然返回),这里只等完成态翻面。
    if (kill(-impl->pid, SIGTERM) != 0) {
        // 组发不动(极少见):单发根。
        kill(impl->pid, SIGTERM);
    }
    const int grace = grace_ms > 0 ? grace_ms : 0;
    if (Wait(grace)) {
        std::lock_guard<std::mutex> lock(mutex_);
        completion_.terminated_by_stop = true;
        kill(-impl->pid, SIGKILL);  // 根退了也补一记,清残留后代
        return true;
    }
    kill(-impl->pid, SIGKILL);
    kill(impl->pid, SIGKILL);
    // SIGKILL 后必退:收尸线程会把完成态广播过来,限时等它。
    if (Wait(5000)) {
        std::lock_guard<std::mutex> lock(mutex_);
        completion_.terminated_by_stop = true;
        return true;
    }
    last_terminate_error = "SIGKILL 已发,5 秒后进程树仍未退净";
    return false;  // 5 秒还没死透:如实报,调用方进 stop_failed
}

namespace {

struct SpawnedMerged {
    pid_t pid = -1;
    int output_fd = -1;  // 合并的 stdout+stderr 读端
};

// 起一个"合并输出、stdin 接 /dev/null"的子进程。失败时 result 里带人话。
bool SpawnMergedOutput(std::vector<std::string> argv, const EnvPairs& extra_env, const std::string& cwd_utf8,
                        SpawnedMerged* spawned, ProcessResult* result) {
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

    EnvBlock env_block = BuildEnvBlock(extra_env, EnvMode::Inherit);
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
        // 全量验错(P1:前置 setup 错误原先无声吞掉,父进程只认得 execvp
        // 一处的失败):每步失败都经 exec_pipe 送回 errno 再 _exit(127)。
        const auto die = [&](int err) {
            (void)!write(exec_pipe[1], &err, sizeof(err));
            _exit(127);
        };
        if (setpgid(0, 0) != 0 && errno != EPERM) {
            // EPERM = 已经是组长(fork 竞态里父进程那边先 set 好了),不算失败。
            die(errno);
        }
        const int devnull = open("/dev/null", O_RDONLY);
        if (devnull < 0) {
            die(errno);
        }
        if (dup2(devnull, STDIN_FILENO) < 0) {
            die(errno);
        }
        if (devnull > STDERR_FILENO) {
            close(devnull);
        }
        if (dup2(out_pipe[1], STDOUT_FILENO) < 0 || dup2(out_pipe[1], STDERR_FILENO) < 0) {
            die(errno);
        }
        if (out_pipe[1] > STDERR_FILENO) {
            close(out_pipe[1]);
        }
        // cwd(P1 根治,前台半边):exec 前 chdir,失败回报,不向命令文本拼 cd。
        if (!cwd_utf8.empty() && chdir(cwd_utf8.c_str()) != 0) {
            die(errno);
        }
        environ = env_block.ptrs.data();
        execvp(argv_ptrs[0], argv_ptrs.data());
        die(errno);
    }

    // 父进程:setpgid 两头都调,谁先跑到都不留窗口(允许的竞态 EACCES/EPERM
    // 单列,别吞真失败)。
    if (setpgid(pid, pid) != 0 && errno != EACCES && errno != EPERM) {
        // 罕见:子进程已退/被收。清管道、报 spawn 失败,不带坏账往下走。
        close(out_pipe[0]);
        close(out_pipe[1]);
        close(exec_pipe[0]);
        close(exec_pipe[1]);
        result->spawn_failed = true;
        result->spawn_error = std::string("建进程组失败: ") + std::strerror(errno);
        return false;
    }
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
    return RunProcess(argv, timeout_ms, /*cancel=*/nullptr, extra_env, max_output_bytes);
}

ProcessResult RunProcess(const std::vector<std::string>& argv, int timeout_ms, const std::atomic<bool>* cancel,
                          const EnvPairs& extra_env, std::size_t max_output_bytes, const std::string& cwd_utf8) {
    ProcessResult result;
    if (argv.empty()) {
        result.spawn_failed = true;
        result.spawn_error = "argv 不能为空";
        return result;
    }
    if (const std::string env_error = ValidateEnvPairs(extra_env); !env_error.empty()) {
        result.spawn_failed = true;
        result.spawn_error = env_error;
        return result;
    }

    SpawnedMerged spawned;
    if (!SpawnMergedOutput(argv, extra_env, cwd_utf8, &spawned, &result)) {
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
                const std::size_t take = std::min(static_cast<std::size_t>(n), room);
                output.append(buf, take);
                // off-by-one 修正:恰好填到 max_output_bytes 不算"超过"。
                // 读到了第 limit+1 个字节(总长超出一字节)才置 overflow。
                if (static_cast<std::size_t>(n) > take) {
                    output_over_limit.store(true);
                }
            } else {
                output_over_limit.store(true);
            }
            // 超限之后继续读但直接丢弃——别让子进程卡在写上,读空到 EOF。
        }
        reader_done.store(true);
    });

    // 主循环:等退出 / 超时 / 取消 / 输出超限,四个条件谁先到算谁的。
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
        // 取消(进程生命线单):ESC 置旗即收整棵树,与超时分开记账。
        if (cancel != nullptr && cancel->load()) {
            result.cancelled = true;
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
    // 截断的刀口对齐 UTF-8 码点边界(见文件头 AlignOutputToUtf8Boundary)。
    AlignOutputToUtf8Boundary(output, max_output_bytes);
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

ProcessResult RunShellCommand(const std::string& command_utf8, int timeout_ms, const std::atomic<bool>* cancel,
                              const EnvPairs& extra_env, std::size_t max_output_bytes,
                              const std::string& cwd_utf8) {
    return RunProcess({"/bin/sh", "-c", command_utf8}, timeout_ms, cancel, extra_env, max_output_bytes, cwd_utf8);
}

ProcessResult RunProcessWithStdin(const std::vector<std::string>& argv, const std::string& stdin_data,
                                  int timeout_ms, const EnvPairs& extra_env, std::size_t max_output_bytes) {
    ProcessResult result;
    if (argv.empty()) {
        result.spawn_failed = true;
        result.spawn_error = "argv 不能为空";
        return result;
    }
    if (const std::string env_error = ValidateEnvPairs(extra_env); !env_error.empty()) {
        result.spawn_failed = true;
        result.spawn_error = env_error;
        return result;
    }

    IgnoreSigpipeOnce();

    int out_pipe[2] = {-1, -1};
    int err_pipe[2] = {-1, -1};
    int in_pipe[2] = {-1, -1};
    int exec_pipe[2] = {-1, -1};
    if (pipe(out_pipe) != 0 || pipe(err_pipe) != 0 || pipe(in_pipe) != 0 || pipe(exec_pipe) != 0) {
        result.spawn_failed = true;
        result.spawn_error = std::string("创建管道失败: ") + std::strerror(errno);
        for (int fd : {out_pipe[0], out_pipe[1], err_pipe[0], err_pipe[1], in_pipe[0], in_pipe[1], exec_pipe[0],
                       exec_pipe[1]}) {
            if (fd >= 0) {
                close(fd);
            }
        }
        return result;
    }
    fcntl(exec_pipe[1], F_SETFD, FD_CLOEXEC);
    // 父进程留的读端不给子进程继承(CLOEXEC),不然后代握着写端 EOF 等不到。
    fcntl(out_pipe[0], F_SETFD, FD_CLOEXEC);
    fcntl(err_pipe[0], F_SETFD, FD_CLOEXEC);
    fcntl(exec_pipe[0], F_SETFD, FD_CLOEXEC);
    // stdin 写端留在父进程,不继承;读端给子进程。
    fcntl(in_pipe[1], F_SETFD, FD_CLOEXEC);

    EnvBlock env_block = BuildEnvBlock(extra_env, EnvMode::Inherit);
    std::vector<char*> argv_ptrs = BuildArgvPtrs(argv);

    const pid_t pid = fork();
    if (pid < 0) {
        result.spawn_failed = true;
        result.spawn_error = std::string("fork 失败: ") + std::strerror(errno);
        for (int fd : {out_pipe[0], out_pipe[1], err_pipe[0], err_pipe[1], in_pipe[0], in_pipe[1], exec_pipe[0],
                       exec_pipe[1]}) {
            close(fd);
        }
        return result;
    }
    if (pid == 0) {
        // 子进程:自立进程组,stdin 接管道读端,stdout/stderr 各接各的管道
        // 写端(hooks 层要分开解码,不混作一锅)。只用 async-signal-safe 的
        // 调用。全量验错(P1):每步失败经 exec_pipe 送回 errno 再 _exit(127),
        // 前置 setup 错误不再无声吞掉(dup2 失败会把输出落回旧 fd,父进程
        // 拿到的是错位的流,比报错更坏)。
        const auto die = [&](int err) {
            (void)!write(exec_pipe[1], &err, sizeof(err));
            _exit(127);
        };
        if (setpgid(0, 0) != 0 && errno != EPERM) {
            die(errno);
        }
        if (dup2(in_pipe[0], STDIN_FILENO) < 0 || dup2(out_pipe[1], STDOUT_FILENO) < 0 ||
            dup2(err_pipe[1], STDERR_FILENO) < 0) {
            die(errno);
        }
        // 注意:exec_pipe[1] 不关——execvp 失败后还要靠它把 errno 送回父进程
        // (这里关了,父进程只读到 EOF,启动失败就误报成"跑了但退出码 127")。
        // exec 成功那头由预先置好的 CLOEXEC 自动收口。
        for (int fd : {in_pipe[0], in_pipe[1], out_pipe[0], out_pipe[1], err_pipe[0], err_pipe[1], exec_pipe[0]}) {
            if (fd > STDERR_FILENO) {
                close(fd);
            }
        }
        environ = env_block.ptrs.data();
        execvp(argv_ptrs[0], argv_ptrs.data());
        die(errno);
    }

    // 父进程:setpgid 两头都调,谁先跑到都不留窗口(竞态 errno 单列)。
    if (setpgid(pid, pid) != 0 && errno != EACCES && errno != EPERM) {
        close(out_pipe[0]);
        close(out_pipe[1]);
        close(err_pipe[0]);
        close(err_pipe[1]);
        close(in_pipe[0]);
        close(in_pipe[1]);
        close(exec_pipe[0]);
        close(exec_pipe[1]);
        result.spawn_failed = true;
        result.spawn_error = std::string("建进程组失败: ") + std::strerror(errno);
        return result;
    }
    close(out_pipe[1]);
    close(err_pipe[1]);
    close(in_pipe[0]);
    close(exec_pipe[1]);

    const std::optional<int> exec_err = ReadExecErrno(exec_pipe[0]);
    close(exec_pipe[0]);
    if (exec_err.has_value()) {
        int status = 0;
        waitpid(pid, &status, 0);  // 子进程 _exit(127) 了,收尸
        close(out_pipe[0]);
        close(err_pipe[0]);
        close(in_pipe[1]);
        result.spawn_failed = true;
        result.spawn_error = "启动子进程失败(" + std::string(std::strerror(*exec_err)) + "): " + argv[0];
        return result;
    }

    // stdin 写线程:一次性写完就关写端(子进程读到 EOF)。子进程不读而数据
    // 超过管道缓冲时,write 阻塞——SIGPIPE 已忽略,子进程死掉后读端关闭,
    // write 以 EPIPE 失败收场,写线程退,绝不吊死。
    std::atomic<bool> stdin_done{false};
    std::thread stdin_writer([&] {
        std::size_t written_total = 0;
        while (written_total < stdin_data.size()) {
            const ssize_t n = write(in_pipe[1], stdin_data.data() + written_total, stdin_data.size() - written_total);
            if (n < 0) {
                if (errno == EINTR) {
                    continue;
                }
                break;  // EPIPE 等:子进程死了/不收,写不完就写不完
            }
            written_total += static_cast<std::size_t>(n);
        }
        close(in_pipe[1]);
        in_pipe[1] = -1;
        stdin_done.store(true);
    });

    // 两条读线程:stdout 与 stderr 各自攒各自的原始字节(与 Windows 实现对
    // 齐,hooks 层分开做明示解码)。超限判定按两路合计,超限后继续读空管道
    // ——不读的话缓冲区一满子进程会卡在写上死不掉。
    std::string stdout_bytes;
    std::string stderr_bytes;
    std::atomic<std::size_t> captured_total{0};
    std::atomic<bool> output_over_limit{false};
    std::atomic<bool> reader_stop{false};
    std::atomic<bool> out_done{false};
    std::atomic<bool> err_done{false};
    const auto stream_reader = [&](int fd, std::string* sink, std::atomic<bool>* done) {
        char buf[4096];
        while (!reader_stop.load()) {
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
                break;
            }
            if (!output_over_limit.load()) {
                sink->append(buf, static_cast<std::size_t>(n));
                const std::size_t total = captured_total.fetch_add(static_cast<std::size_t>(n)) +
                                          static_cast<std::size_t>(n);
                // off-by-one 对齐:total == limit 不算超限;读到第 limit+1 个
                // 字节(total > limit)才算。
                if (total > max_output_bytes) {
                    output_over_limit.store(true);
                }
            }
        }
        done->store(true);
    };
    std::thread out_reader([&] { stream_reader(out_pipe[0], &stdout_bytes, &out_done); });
    std::thread err_reader([&] { stream_reader(err_pipe[0], &stderr_bytes, &err_done); });

    const bool has_timeout = timeout_ms > 0;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(has_timeout ? timeout_ms : 0);
    int exit_status = 0;
    bool exited = false;
    while (true) {
        int status = 0;
        const pid_t r = waitpid(pid, &status, WNOHANG);
        if (r == pid) {
            exit_status = status;
            exited = true;
            break;
        }
        if (r < 0 && errno != EINTR) {
            break;
        }
        if (output_over_limit.load()) {
            KillProcessGroup(pid, 2000, &exit_status);
            exited = true;
            break;
        }
        if (has_timeout && std::chrono::steady_clock::now() >= deadline) {
            result.timed_out = true;
            KillProcessGroup(pid, 2000, &exit_status);
            exited = true;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    // 补杀整组,后代握着的写端全关,读线程才能等到 EOF 收尾。
    killpg(pid, SIGKILL);

    const auto reader_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while ((!out_done.load() || !err_done.load()) && std::chrono::steady_clock::now() < reader_deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    reader_stop.store(true);
    out_reader.join();
    err_reader.join();

    // stdin 写线程:子进程死透了,write 早就 EPIPE 退出;限时兜底等一把。
    const auto stdin_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (!stdin_done.load() && std::chrono::steady_clock::now() < stdin_deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    if (!stdin_done.load()) {
        // 极罕见:还有后代握着 stdin 读端活着。杀完组了仍不退,直接关 fd
        // 逼 write 失败(EPIPE/EIO),写线程自然收场。
        if (in_pipe[1] >= 0) {
            close(in_pipe[1]);
            in_pipe[1] = -1;
        }
    }
    stdin_writer.join();

    if (!exited) {
        int status = 0;
        if (waitpid(pid, &status, WNOHANG) == pid) {
            exit_status = status;
        }
    }

    close(out_pipe[0]);
    close(err_pipe[0]);
    if (in_pipe[1] >= 0) {
        close(in_pipe[1]);
    }

    result.exit_code = ExitCodeFromStatus(exit_status);
    // 两路各自对齐 UTF-8 边界(超限判定按两路合计,帽是合计帽:先合再对齐
    // 会把 stderr 的刀口错落在 stdout 尾上,所以各切各的)。
    AlignOutputToUtf8Boundary(stdout_bytes, max_output_bytes);
    AlignOutputToUtf8Boundary(stderr_bytes, max_output_bytes);
    result.output = stdout_bytes + stderr_bytes;  // 合并账(stdout 在前)
    result.stdout_bytes = std::move(stdout_bytes);
    result.stderr_bytes = std::move(stderr_bytes);
    result.output_truncated = output_over_limit.load();
    return result;
}

ProcessResult RunShellCommandWithStdin(const std::string& command_utf8, const std::string& stdin_data,
                                       int timeout_ms, const EnvPairs& extra_env, std::size_t max_output_bytes) {
    return RunProcessWithStdin({"/bin/sh", "-c", command_utf8}, stdin_data, timeout_ms, extra_env, max_output_bytes);
}

// ---------------------------------------------------------------------------
// 后台模式:spawn 立刻返回,不等待、不捕获进内存,stdout/stderr 直接 dup2
// 到日志文件描述符上。
// ---------------------------------------------------------------------------

BackgroundSpawnResult RunProcessBackground(const std::vector<std::string>& argv, const EnvPairs& extra_env) {
    return RunProcessBackground(argv, std::string(), extra_env);
}

BackgroundSpawnResult RunProcessBackground(const std::vector<std::string>& argv, const std::string& cwd_utf8,
                                            const EnvPairs& extra_env) {
    BackgroundSpawnResult result;
    if (argv.empty()) {
        result.error = "argv 不能为空";
        return result;
    }
    if (const std::string env_error = ValidateEnvPairs(extra_env); !env_error.empty()) {
        result.error = env_error;
        return result;
    }
    IgnoreSigpipeOnce();

    // 独占创建 + 0600:文件名可猜的 O_TRUNC 在共享临时目录里有预置文件/
    // symlink 的攻击面;命令输出可能带 token,别的账号不许读。
    std::string log_path = BuildBackgroundLogPath();
    int log_fd = OpenBackgroundLogExclusive(log_path);
    if (log_fd < 0) {
        // 撞名/被人预置(O_EXCL|O_NOFOLLOW 拒了):换一个名字再来一把。
        log_path = BuildBackgroundLogPath();
        log_fd = OpenBackgroundLogExclusive(log_path);
    }
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

    EnvBlock env_block = BuildEnvBlock(extra_env, EnvMode::Inherit);
    std::vector<char*> argv_ptrs = BuildArgvPtrs(argv);

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
        // 全量验错(P1):setsid/open/dup2/chdir 每步失败都经 exec_pipe 把
        // errno 送回父进程——前置 setup 错误不再无声吞掉。
        const auto report_and_die = [&](int err) {
            (void)!write(exec_pipe[1], &err, sizeof(err));
            _exit(127);
        };
        if (setsid() < 0 && errno != EPERM) {
            report_and_die(errno);
        }
        const int devnull = open("/dev/null", O_RDONLY);
        if (devnull < 0) {
            report_and_die(errno);
        }
        if (dup2(devnull, STDIN_FILENO) < 0) {
            report_and_die(errno);
        }
        if (devnull > STDERR_FILENO) {
            close(devnull);
        }
        if (dup2(log_fd, STDOUT_FILENO) < 0 || dup2(log_fd, STDERR_FILENO) < 0) {
            report_and_die(errno);
        }
        if (log_fd > STDERR_FILENO) {
            close(log_fd);
        }
        // cwd(P1 根治):exec 前 chdir,失败回报,不再向命令文本拼 cd。
        if (!cwd_utf8.empty() && chdir(cwd_utf8.c_str()) != 0) {
            report_and_die(errno);
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

    // 会话级注册表:atexit 兜底收尾照旧(handle 之外的第二道保险)。
    RegisterBackgroundPid(pid);

    auto handle = std::make_shared<BackgroundProcessHandle>();
    handle->impl->pid = pid;
    // 收尸线程:一次性 detach 的 waitpid,防止子进程结束后没人收尸变成
    // 僵尸(lubancode 还没退出、atexit 钩子还没跑之前,子进程可能早就
    // 退出了)。拿到 status 立刻写进 handle 的共享完成态——退出码从此不
    // 丢。退出时把自己从会话级注册表摘掉,免得 atexit 钩子对着一个早就
    // 死透、pid 可能已被系统回收复用的号码瞎杀。
    std::thread([pid, handle] {
        int status = 0;
        const pid_t r = waitpid(pid, &status, 0);
        if (r == pid) {
            handle->RecordExit(status);
        } else {
            // 收不到(EINTR 之外的不该发生):如实标未知,绝不借 0 冒充成功。
            // (注意不能把 -1 当 status 传进 RecordExit——宏会把它解成信号。)
            handle->MarkUnknown();
        }
        UnregisterBackgroundPid(pid);
    }).detach();

    result.success = true;
    result.pid = static_cast<unsigned long>(pid);
    result.log_path = log_path;
    result.handle = std::move(handle);
    return result;
}

BackgroundSpawnResult RunShellCommandBackground(const std::string& command_utf8, const EnvPairs& extra_env) {
    return RunShellCommandBackground(command_utf8, std::string(), extra_env);
}

BackgroundSpawnResult RunShellCommandBackground(const std::string& command_utf8, const std::string& cwd_utf8,
                                                 const EnvPairs& extra_env) {
    BackgroundSpawnResult result = RunProcessBackground({"/bin/sh", "-c", command_utf8}, cwd_utf8, extra_env);
    if (result.success) {
        // /bin/sh 约定 UTF-8 但不保证(二进制程序/旧工具/坏 locale 都可能
        // 吐坏字节);出口按"先验后洗"处理。
        result.handle->encoding_hint = "utf-8-assumed";
    }
    return result;
}

// ---------------------------------------------------------------------------
// ChildProcess:长命双向管道(语义对齐 process_win.cpp 的同名类)。
// ---------------------------------------------------------------------------

ChildProcess::~ChildProcess() {
    Shutdown(2000);
}

SpawnResult ChildProcess::Start(const std::string& command, const std::vector<std::string>& args,
                                  const EnvPairs& env, std::function<bool(std::string_view)> on_stdout,
                                  std::function<void(std::string_view)> on_stderr, const std::string& cwd_utf8,
                                  EnvMode env_mode) {
    return Start(command, args, env, std::move(on_stdout), std::move(on_stderr), SpawnConstraints{}, cwd_utf8,
                 env_mode);
}

SpawnResult ChildProcess::Start(const std::string& command, const std::vector<std::string>& args,
                                  const EnvPairs& env, std::function<bool(std::string_view)> on_stdout,
                                  std::function<void(std::string_view)> on_stderr,
                                  const SpawnConstraints& constraints, const std::string& cwd_utf8,
                                  EnvMode env_mode) {
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
    EnvBlock env_block = BuildEnvBlock(env, env_mode);
    std::vector<char*> argv_ptrs = BuildArgvPtrs(argv);

    const pid_t pid = fork();
    if (pid < 0) {
        const std::string err = std::strerror(errno);
        close_all();
        return SpawnResult{false, "fork 失败: " + err, false};
    }
    if (pid == 0) {
        // 全量验错(P1):setup 每步失败经 exec_pipe 送回 errno 再 _exit(127)。
        const auto die = [&](int err) {
            (void)!write(exec_pipe[1], &err, sizeof(err));
            _exit(127);
        };
        if (setpgid(0, 0) != 0 && errno != EPERM) {
            die(errno);
        }
        if (dup2(in_pipe[0], STDIN_FILENO) < 0 || dup2(out_pipe[1], STDOUT_FILENO) < 0 ||
            dup2(err_pipe[1], STDERR_FILENO) < 0) {
            die(errno);
        }
        for (int fd : {in_pipe[0], in_pipe[1], out_pipe[0], out_pipe[1], err_pipe[0], err_pipe[1]}) {
            if (fd > STDERR_FILENO) {
                close(fd);
            }
        }
        // cwd:非空先切目录再 exec(UTF-8 路径,POSIX 字节串直通)。切不动
        // 按起失败收场(exec_pipe 把 errno 带回父进程)。
        if (!cwd_utf8.empty() && chdir(cwd_utf8.c_str()) != 0) {
            const int err = errno;
            (void)!write(exec_pipe[1], &err, sizeof(err));
            _exit(127);
        }
        // PTC 沙箱:exec 前落资源墙。setrlimit 是 async-signal-safe 白名单外
        // 的调用,但 fork 后单线程 exec 前的窗口里可用(glibc 文档允许)。
        if (constraints.cpu_seconds > 0) {
            struct rlimit cpu_limit{};
            cpu_limit.rlim_cur = static_cast<rlim_t>(constraints.cpu_seconds);
            cpu_limit.rlim_max = static_cast<rlim_t>(constraints.cpu_seconds);
            setrlimit(RLIMIT_CPU, &cpu_limit);
        }
        if (constraints.memory_bytes > 0) {
            struct rlimit as_limit{};
            as_limit.rlim_cur = static_cast<rlim_t>(constraints.memory_bytes);
            as_limit.rlim_max = static_cast<rlim_t>(constraints.memory_bytes);
            setrlimit(RLIMIT_AS, &as_limit);
        }
        environ = env_block.ptrs.data();
        execvp(argv_ptrs[0], argv_ptrs.data());
        die(errno);
    }

    // 父进程:setpgid 两头都调,谁先跑到都不留窗口(竞态 errno 单列)。
    if (setpgid(pid, pid) != 0 && errno != EACCES && errno != EPERM) {
        close_all();
        return SpawnResult{false, std::string("建进程组失败: ") + std::strerror(errno), false};
    }
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

void ChildProcess::CloseStdin() {
    std::lock_guard<std::mutex> lock(write_mutex_);
    if (stdin_fd_ >= 0) {
        close(stdin_fd_);
        stdin_fd_ = -1;
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
        // PTC 沙箱归因:退出码缓存(WIFEXITED 取退出码;被信号杀记负数,
        // SIGXCPU/SIGKILL 这类资源墙信号靠它辨认)。资源峰值(RUSAGE_CHILDREN
        // 混着后台任务账)不可信,POSIX 这路留未知。
        if (WIFEXITED(exit_status_)) {
            exit_code_cache_ = WEXITSTATUS(exit_status_);
        } else if (WIFSIGNALED(exit_status_)) {
            exit_code_cache_ = -WTERMSIG(exit_status_);
        }
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

ChildResourceUsage ChildProcess::ResourceUsageSnapshot() const {
    // POSIX:RUSAGE_CHILDREN 混着后台命令的账,拆不出这家子进程自己的数,
    // 如实返回未知(全零);撞线归因走 exit_code 的负数信号编码。
    return resource_usage_cache_;
}

int ChildProcess::exit_code() const { return exit_code_cache_; }

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

int RunInteractiveCommand(const std::string& command_utf8) {
    if (command_utf8.empty()) {
        return -1;
    }
    return ::system(command_utf8.c_str());
}

}  // namespace lubancode::platform
