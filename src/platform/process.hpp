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
#include <condition_variable>
#include <cstddef>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace lubancode::platform {

struct ProcessResult {
    std::string output;              // 合并捕获的 stdout/stderr,原始字节
    // stdout/stderr 分开捕获的原始字节(RunProcessWithStdin 一族才有值;
    // 其余入口两路本来就合一条管道,保持空串)。hooks 层拿它做"明示编码
    // 解码"——stdout 按 UTF-8 契约解 JSON,stderr 解码后进台账,不与
    // stdout 混作一锅。
    std::string stdout_bytes;
    std::string stderr_bytes;
    unsigned long exit_code = 0;
    bool timed_out = false;
    bool cancelled = false;          // 进程生命线单:cancel 旗置位,树已收,cancelled 与超时分开记账
    bool spawn_failed = false;
    bool output_truncated = false;   // 输出超过上限被截断,进程树已被强制终止
    std::string spawn_error;
};

// 单次执行捕获输出的默认上限(字节)。超过就截断、杀掉整棵进程树,防止
// 一条命令把内存吃光(比如 ping -t、dir /s C:\)。
constexpr std::size_t kDefaultMaxOutputBytes = 2 * 1024 * 1024;

using EnvPairs = std::vector<std::pair<std::string, std::string>>;

// 子进程环境的两条路(plugins 单第 8 步:整环境替换的硬保证):
//   Inherit —— 继承宿主全部环境 + extra_env 覆盖/追加(历史行为,MCP/LSP/
//              hooks/run_command 共用,不动)。
//   Replace —— 清空宿主环境,子进程只见 extra_env 里列的条目。宿主的
//              API key、用户变量一概不递(插件进程的最小环境合同)。
// 调用方自己负责把 PATH 之类必要变量挑进 extra_env(Replace 模式不偷偷
// 补,补了就不是最小集了;要不要 PATH 是策略,不是平台层的事)。
enum class EnvMode {
    Inherit,
    Replace,
};

// 后台模式(v0.22.x,run_command 的 run_in_background):spawn 成功立刻
// 返回,不等命令跑完、不捕获输出进内存——stdout/stderr 直接重定向到磁盘上
// 的日志文件。子进程挂进"会话级"存活域(跟一次性捕获那种"命令收尾就
// 杀全家"的临时 job/进程组不是一回事):
//   - Windows:懒创建一个进程级单例的会话级 Job Object,句柄由主进程一直
//     攥着不关;lubancode 退出(正常退出或崩溃)时系统自动关掉未显式关闭
//     的句柄,KILL_ON_JOB_CLOSE 顺带把挂在这个 job 上的所有后台子进程全部
//     杀光,不留孤儿,不需要额外的退出钩子。
//   - POSIX:子进程 setsid() 脱离 lubancode 的会话/进程组/控制终端,PID
//     记进进程内的会话级注册表;atexit 钩子在 lubancode 退出时逐个
//     SIGTERM→SIGKILL 收尾(atexit 不覆盖被信号杀死等异常终止路径,这点
//     跟 Windows 靠内核收句柄的强保证不同,是两平台唯一的语义差)。
struct BackgroundSpawnResult {
    bool success = false;
    std::string error;
    unsigned long pid = 0;    // 子进程 PID,给模型回填进结果文本,方便之后 kill/Stop-Process
    std::string log_path;     // 合并 stdout/stderr 写入的日志文件路径(UTF-8,临时目录下)
    // 进程生命线单(P0):可等待的原生句柄,不再只回 PID。Watch/Stop 都落到
    // 这份句柄上——不凭 PID 猜生死,退出码由唯一收尸方写进共享完成态。
    // success=false 时为空。
    std::shared_ptr<struct BackgroundProcessHandle> handle;
};

// 后台子进程的可等待原生句柄(进程生命线单 P0)。约定:
//   - Wait(timeout_ms):等进程退出。true = 已退出(完成态已填);false = 还
//     活着/超时。退出码经 ExitCode() 取,不知道便是 nullopt——绝不借 0
//     冒充成功。
//   - Signal():POSIX 受信号终止另记 signal;Windows 没有这层,恒 nullopt。
//   - TerminateTree(grace_ms):收整棵进程树。Windows 每任务一个 Job
//     Object(创建时挂上),TerminateJobObject 一锅端;POSIX 对原进程组
//     先 SIGTERM 等 grace 再 SIGKILL。返回值 = 收口是否有把握(系统调用
//     失败如实报 false,调用方据此进 stop_failed/留 Running,不先盖章)。
//   - 线程安全:Wait/ExitCode/TerminateTree 可从多线程调(registry 的
//     watcher 与 Stop 并发)。
struct BackgroundProcessHandle {
  public:
    BackgroundProcessHandle();
    ~BackgroundProcessHandle();
    BackgroundProcessHandle(const BackgroundProcessHandle&) = delete;
    BackgroundProcessHandle& operator=(const BackgroundProcessHandle&) = delete;

    struct Completion {
        bool known = false;             // 收尸方到底拿到退出状态没有
        int exit_code = 0;              // known && WIFEXITED 时有效
        int signal = 0;                 // known && WIFSIGNALED(POSIX)时有效,信号号
        bool terminated_by_stop = false;  // TerminateTree 收掉的(退出码无业务含义)
    };

    // 等进程退出(带超时;timeout_ms<=0 只查一眼)。已退出后调立刻返回 true。
    bool Wait(int timeout_ms);
    // 完成态快照。进程还没退出时 known=false。
    Completion Peek() const;
    // 进程是否还活着(以完成态/句柄为准,不查 PID)。
    bool IsAlive();

    // 收整棵树。Windows:TerminateJobObject(体面信号这层 Windows 没有,
    // grace 只给树内自清理留一点时间再硬杀)。POSIX:SIGTERM → 等 grace →
    // SIGKILL,waitpid 由本对象独占收。返回 true = 收口调用链没报错。
    bool TerminateTree(int grace_ms);

    // POSIX 收尸出口:waitpid 拿到 status 后落完成态(唯一收尸方调用)。
    void RecordExit(int status);
    // 收不到状态时如实标未知(实现侧用)。
    void MarkUnknown();

    // 日志编码提示(后台日志单):shell 种类决定 background_output 出口怎么
    // 清洗。powershell wrapper 落盘的是 UTF-8;cmd 落盘 OEM/ACP;POSIX sh
    // 约定 UTF-8 但不保证;unknown = 未知程序。
    std::string encoding_hint;

    // 平台实现持有原生身份(句柄/pid),定义在 process_win/posix.cpp。
    struct Impl;
    std::unique_ptr<Impl> impl;

  private:
    mutable std::mutex mutex_;
    Completion completion_;
    bool completion_known_ = false;
};

// 可移植入口:按 argv 数组起一个子进程(argv[0] 是可执行文件,按 PATH 查
// 找;不经过 shell,参数原样传递),合并捕获 stdout/stderr,超时杀树。
//
// extra_env 是要注入子进程环境的键值对(UTF-8,同名覆盖当前进程环境)。
// 两平台都不改父进程环境(进程生命线单 P0 的并发修复):Windows 构建显式
// UTF-16 environment block 交给 lpEnvironment,POSIX 在 fork 后的子进程里
// 改 environ——Hook dispatcher 给每只 handler 一条线程并发跑,也不会串值。
//
// max_output_bytes:捕获输出的上限,超过就截断保留前段、置 output_truncated
// 并强制终止整棵进程树。测试用小值,生产路径用默认值即可。
ProcessResult RunProcess(const std::vector<std::string>& argv, int timeout_ms,
                          const EnvPairs& extra_env = {},
                          std::size_t max_output_bytes = kDefaultMaxOutputBytes);

// 进程生命线单(P1:前台取消通道):同 RunProcess,但等待循环每拍查 cancel
// 旗。置位即收整棵树,result 里 cancelled=true(与 timed_out 分开记账,
// 两者都收树,但终态语义不同)。cancel 为空/未置位时行为与上面完全一致。
// cwd_utf8 非空则走操作系统参数(Windows lpCurrentDirectory;POSIX exec 前
// chdir,失败回 spawn_failed)——不向命令文本拼 cd,验收口径"cwd 不再拼
// 进 shell 字符串"的前台半边。
ProcessResult RunProcess(const std::vector<std::string>& argv, int timeout_ms, const std::atomic<bool>* cancel,
                          const EnvPairs& extra_env = {}, std::size_t max_output_bytes = kDefaultMaxOutputBytes,
                          const std::string& cwd_utf8 = std::string());
#ifdef _WIN32
ProcessResult RunProcess(const std::wstring& cmdline, int timeout_ms, const std::atomic<bool>* cancel,
                         const EnvPairs& extra_env = {}, std::size_t max_output_bytes = kDefaultMaxOutputBytes,
                         const std::string& cwd_utf8 = std::string());
#endif
ProcessResult RunShellCommand(const std::string& command_utf8, int timeout_ms, const std::atomic<bool>* cancel,
                              const EnvPairs& extra_env = {}, std::size_t max_output_bytes = kDefaultMaxOutputBytes,
                              const std::string& cwd_utf8 = std::string());

// 按平台默认 shell 跑一条命令:Windows 是 `cmd.exe /d /s /c "<command>"`
// (输出按系统 ANSI 代码页转成 UTF-8,原因见 paths.hpp 的 AcpBytesToUtf8),
// POSIX 是 `/bin/sh -c '<command>'`(输出天然 UTF-8 直通)。hooks 与
// run_command 的 cmd/sh 分支共用。
ProcessResult RunShellCommand(const std::string& command_utf8, int timeout_ms,
                               const EnvPairs& extra_env = {},
                               std::size_t max_output_bytes = kDefaultMaxOutputBytes);

// hooks schema 2 的 stdin JSON 通道(不经过 shell 的 exec form):起进程、
// 把 stdin_data 一次性写进子进程 stdin 后关写端、捕获 stdout/stderr、等跑完
// 或超时杀树。与 RunProcess 的差别:stdout/stderr 各走一条管道分开捕获
// (stdout_bytes/stderr_bytes,原始字节;合并账照旧写 output)。编码解码不
// 在这里做——hooks 层按"先认 UTF-8、次选明示代码页"的契约解,cmd/PowerShell
// 写出来的 ANSI/OEM 字节不至于被无声替换。
//
// stdin 的写入在独立线程:子进程不读 stdin(比如 `echo hi`)而数据大过
// 管道缓冲时,写会阻塞——没关系,超时杀树后子进程的读端关闭,阻塞的写
// 立刻以失败收场,写线程收尸,绝不吊死父进程。stdin_data 为空 = 立刻关
// 写端(子进程读到 EOF),行为等同 RunProcess 的 stdin=/dev/null。
ProcessResult RunProcessWithStdin(const std::vector<std::string>& argv, const std::string& stdin_data,
                                  int timeout_ms, const EnvPairs& extra_env = {},
                                  std::size_t max_output_bytes = kDefaultMaxOutputBytes);

// 同上,但按平台默认 shell 跑一条命令串(shell 字符串形式的 v2 hook 用;
// legacy 钩子继续走 RunShellCommand,不吃 stdin)。stdout/stderr 同样分开
// 捕获为原始字节,不做代码页转换——cmd.exe 的 ANSI 输出由 hooks 解码层
// 明示处理。
ProcessResult RunShellCommandWithStdin(const std::string& command_utf8, const std::string& stdin_data,
                                       int timeout_ms, const EnvPairs& extra_env = {},
                                       std::size_t max_output_bytes = kDefaultMaxOutputBytes);

// 一个 PID 的进程还活着吗。给跨会话名册清陈条用(会话崩了,名片得清)。
// Windows: OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION) + GetExitCodeProcess;
// POSIX: kill(pid, 0)。自己这个 pid 恒算活着。探测不到权限/出错按"活着"
// 算——宁可多留一张名片走心跳过期那条路,不误删活会话。
bool IsProcessAlive(unsigned long pid);

// 本进程的 pid(名册名片用)。Windows: GetCurrentProcessId;POSIX: getpid。
unsigned long CurrentProcessId();

// 交互式子进程(0.30.x 外部编辑器 $VISUAL/$EDITOR 用):不捕获输出、不给
// stdin 喂数据——子进程直接继承本进程的控制台/终端,跑完返回退出码。
// Windows 用 _wsystem(UTF-8 命令串内部转宽字符),POSIX 用 system()。
// 调用方须已收掉屏幕上的常驻帧(composer 框等),给编辑器让出整屏。
int RunInteractiveCommand(const std::string& command_utf8);

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

// Windows 专属重载:cmdline 是完整命令行(调用方拼好,run_command 的
// PowerShell -EncodedCommand 后台路径要用,跟前台的 RunProcess(wstring, ...)
// 重载对称)。
BackgroundSpawnResult RunProcessBackground(const std::wstring& cmdline, const EnvPairs& extra_env = {});

#endif

// 可移植入口:按 argv 数组后台起一个子进程,不经过 shell,不等待完成。
// extra_env 语义同 RunProcess。
BackgroundSpawnResult RunProcessBackground(const std::vector<std::string>& argv, const EnvPairs& extra_env = {});

// 按平台默认 shell 后台跑一条命令,语义同 RunShellCommand 但不等待完成:
// Windows 是 `cmd.exe /d /s /c "<command>"`,POSIX 是 `/bin/sh -c '<command>'`。
BackgroundSpawnResult RunShellCommandBackground(const std::string& command_utf8, const EnvPairs& extra_env = {});

// 进程生命线单(P1 根治:cwd 走操作系统参数,不拼 cd):带 cwd 的后台起进程。
// Windows 落 CreateProcessW 的 lpCurrentDirectory;POSIX 子进程 exec 前
// chdir,失败经 exec-error 管道回报(spawn_failed)。cwd 为空 = 继承本进程。
BackgroundSpawnResult RunProcessBackground(const std::vector<std::string>& argv, const std::string& cwd_utf8,
                                            const EnvPairs& extra_env = {});
BackgroundSpawnResult RunShellCommandBackground(const std::string& command_utf8, const std::string& cwd_utf8,
                                                 const EnvPairs& extra_env = {});
#ifdef _WIN32
// Windows 专属:完整命令行(调用方拼好)+ 原生 cwd(不拼 cd)。
BackgroundSpawnResult RunProcessBackground(const std::wstring& cmdline, const std::string& cwd_utf8,
                                            const EnvPairs& extra_env);
#endif

// 一次启动长命子进程的结果。
struct SpawnResult {
    bool success = false;
    std::string error;
    bool command_not_found = false;  // 可执行文件不存在(LSP 层要给更友好的报错)
};

// PTC 沙箱(P1):长命子进程的 OS 级资源约束。0/false = 不设这道墙。
//   - Windows:Job Object 的 CPU 时间/进程内存上限 + KILL_ON_JOB_CLOSE,
//     restricted_token 再叠一枚受限 token(禁用全部特权);
//   - POSIX:RLIMIT_CPU / RLIMIT_AS(fork 后 exec 前 setrlimit),没有
//     文件系统/网络隔离,restricted_token 无对应实现(忽略);
//   - 两平台都只是资源墙,不是文件系统/网络隔离——那层靠 Python 侧
//     护栏(白名单 import),画像据此分级,如实交账。
struct SpawnConstraints {
    int cpu_seconds = 0;           // CPU 时间上限(墙钟之外的第五道墙之一)
    std::size_t memory_bytes = 0;  // 地址空间上限
    bool restricted_token = false; // Windows:CreateRestrictedToken 起进程
};

// 一份已经退出(或仍在跑)的子进程资源快照,撞线归因用。字段 0 = 未知
// (平台没实现那一路)。CPU 是 100ns 单位(Windows Job 口径),内存取峰值。
struct ChildResourceUsage {
    std::uint64_t cpu_100ns = 0;
    std::size_t peak_memory_bytes = 0;
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
    // 是要注入子进程的环境键值对(Inherit 模式 = 继承全部 + 同名覆盖;
    // Replace 模式 = 只见 env 里列的,宿主环境一概不递)。cwd_utf8 空 =
    // 继承本进程当前目录;非空则子进程在该目录里跑(插件进程的 cwd 缺省
    // 项目根,plugins 单第 7 步接上)。
    SpawnResult Start(const std::string& command, const std::vector<std::string>& args, const EnvPairs& env,
                      std::function<bool(std::string_view)> on_stdout,
                      std::function<void(std::string_view)> on_stderr,
                      const std::string& cwd_utf8 = std::string(),
                      EnvMode env_mode = EnvMode::Inherit);

    // 同上,但带 PTC 沙箱约束(Job/rlimit 的 CPU、内存上限,Windows 受限
    // token)。constraints 全默认时与四参版本行为一致。job 限额设置失败只
    // 降级(照常起进程),由调用方拿 SandboxGrade 自己记档,不硬失败。
    SpawnResult Start(const std::string& command, const std::vector<std::string>& args, const EnvPairs& env,
                      std::function<bool(std::string_view)> on_stdout,
                      std::function<void(std::string_view)> on_stderr, const SpawnConstraints& constraints,
                      const std::string& cwd_utf8 = std::string(),
                      EnvMode env_mode = EnvMode::Inherit);

    // 原始字节一次性写进子进程 stdin,内部加锁防止多个请求线程交错。进程
    // 已经退出/没起成功都返回 false。
    bool Write(const std::string& data);

    // 关掉 stdin 写端(给子进程发 EOF)。与 Write 的锁共用,关掉后 Write
    // 返回 false。插件进程协议(stdin 一份 JSON 写完即关,脚本读到 EOF)
    // 与"行为良好的服务器见到 EOF 自己退"两条路都靠它;此前 Shutdown 里
    // 才关,一次性请求等不起。幂等。
    void CloseStdin();

    // 立刻强制终止根进程(不等、不关 stdin)。MCP 协议错误断连用;进程树
    // 的收尾(连带后代)还是要靠 Shutdown。
    void Kill();

    // 关停:先关 stdin(等于给子进程发 EOF,行为良好的服务器自己体面退出),
    // 等 wait_ms 毫秒;还没退出就把整棵进程树杀掉,然后收读线程、关句柄。
    // 幂等,重复调用/析构时再调都安全。
    void Shutdown(int wait_ms = 2000);

    // 进程是否还活着。
    bool IsAlive() const;

    // PTC 沙箱:退出后的资源快照(撞线归因用)。没起过/平台没实现那一路
    // 返回全零。可在 Shutdown 之后调,job 句柄关掉前读到的值缓存于此。
    ChildResourceUsage ResourceUsageSnapshot() const;

    // 子进程退出码;没起过或还没退出返回 -1。Shutdown 之后有效(收尸完)。
    int exit_code() const;

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

    // PTC 沙箱的收尾快照(Shutdown 时填,ResourceUsageSnapshot/exit_code 读)。
    ChildResourceUsage resource_usage_cache_{};
    int exit_code_cache_ = -1;

    std::atomic<bool> started_{false};
    std::atomic<bool> shutdown_done_{false};
    std::atomic<bool> reader_stop_{false};
    std::atomic<bool> stdout_reader_done_{false};
    std::atomic<bool> stderr_reader_done_{false};
};

}  // namespace lubancode::platform
