// Windows 实现:一次性捕获搬自 tools/process_exec.cpp(CreateProcessW +
// Job Object + 管道捕获,逻辑一字未改);长命双向管道搬自
// mcp/transport.cpp(lsp/transport.cpp 那份与之几乎相同,合并时取了 MCP
// 版——它的 JoinReaderThreads 多一层"限时等待 + CancelSynchronousIo"兜底,
// 对 LSP 只是更稳,不改变正常路径行为)。
#include "platform/process.hpp"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <jobapi2.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <optional>

#include "platform/paths.hpp"

namespace lubancode::platform {

namespace {

// 读一个环境变量的当前值(找不到就是 std::nullopt)。RunProcess/Start 临时
// 改 extra_env 之前先备份旧值,起完子进程立刻还原用。
std::optional<std::wstring> GetEnvVarW(const std::wstring& name) {
    const DWORD size = GetEnvironmentVariableW(name.c_str(), nullptr, 0);
    if (size == 0) {
        return std::nullopt;  // 不存在(或者是空串,简化处理不细分,还原时按"删掉"处理没什么大碍)
    }
    std::wstring buf(size, L'\0');
    const DWORD written = GetEnvironmentVariableW(name.c_str(), buf.data(), size);
    buf.resize(written);
    return buf;
}

// 临时把 env 写进当前进程的环境变量。CreateProcessW 的 lpEnvironment 传
// nullptr 时,子进程会继承父进程环境的一份快照——子进程一创建,这份快照
// 就跟父进程后续怎么改环境变量没关系了,所以用完立刻还原不会影响已经起来
// 的子进程。要求调用方单线程顺序调用,见头文件里的说明。
struct EnvBackup {
    std::wstring name;
    std::optional<std::wstring> old_value;
};

std::vector<EnvBackup> ApplyEnv(const EnvPairs& extra_env) {
    std::vector<EnvBackup> backups;
    backups.reserve(extra_env.size());
    for (const auto& [key, value] : extra_env) {
        const std::wstring wkey = Utf8ToWide(key);
        backups.push_back(EnvBackup{wkey, GetEnvVarW(wkey)});
        SetEnvironmentVariableW(wkey.c_str(), Utf8ToWide(value).c_str());
    }
    return backups;
}

void RestoreEnv(const std::vector<EnvBackup>& backups) {
    for (const auto& backup : backups) {
        SetEnvironmentVariableW(backup.name.c_str(), backup.old_value.has_value() ? backup.old_value->c_str() : nullptr);
    }
}

// Windows 命令行参数的标准转义算法(CommandLineToArgvW 的逆过程):没有
// 空白/引号就原样返回,否则用双引号包起来,内部的反斜杠+引号按规则转义。
// command/args 里任何一段带空格(比如子进程脚本路径含空格)都得靠这个才能
// 被子进程正确解析成一个参数,不会被从中间断开。(搬自 mcp/transport.cpp,
// lsp/transport.cpp 里那份逻辑相同,合并成这一份。)
std::wstring QuoteArgW(const std::wstring& arg) {
    if (!arg.empty() && arg.find_first_of(L" \t\n\v\"") == std::wstring::npos) {
        return arg;
    }
    std::wstring result = L"\"";
    for (auto it = arg.begin();; ++it) {
        int backslashes = 0;
        while (it != arg.end() && *it == L'\\') {
            ++backslashes;
            ++it;
        }
        if (it == arg.end()) {
            result.append(static_cast<std::size_t>(backslashes) * 2, L'\\');
            break;
        }
        if (*it == L'"') {
            result.append(static_cast<std::size_t>(backslashes) * 2 + 1, L'\\');
            result.push_back(*it);
        } else {
            result.append(static_cast<std::size_t>(backslashes), L'\\');
            result.push_back(*it);
        }
    }
    result.push_back(L'"');
    return result;
}

std::wstring BuildProcessCommandLine(const std::string& command, const std::vector<std::string>& args) {
    std::wstring cmdline = QuoteArgW(Utf8ToWide(command));
    for (const auto& arg : args) {
        cmdline += L' ';
        cmdline += QuoteArgW(Utf8ToWide(arg));
    }
    return cmdline;
}

// ---------------------------------------------------------------------------
// 后台模式(run_command 的 run_in_background)专用小工具。
// ---------------------------------------------------------------------------

// 会话级 Job Object:进程级单例,懒创建,句柄一直攥在主进程手里不主动关。
// 跟 RunProcess/ChildProcess 里那种"一条命令/一条长连接一个 job、用完就关
// 掉杀全家"的临时 job 是两码事——这个 job 只在 lubancode 进程终止时才失效
// (Windows 内核在进程退出时自动关闭它没显式关掉的全部句柄,包括这个),
// KILL_ON_JOB_CLOSE 那一下顺带把挂在上面的所有后台子进程一次杀光,不留
// 孤儿。也正因为靠内核这条强保证,不需要额外注册 atexit/信号处理钩子。
// C++11 起 static 局部变量的初始化本身是线程安全的(magic statics),多个
// 线程并发起后台命令不会重复创建。
HANDLE GetBackgroundSessionJob() {
    static HANDLE job = [] {
        HANDLE j = CreateJobObjectW(nullptr, nullptr);
        if (j != nullptr) {
            JOBOBJECT_EXTENDED_LIMIT_INFORMATION limit{};
            limit.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
            SetInformationJobObject(j, JobObjectExtendedLimitInformation, &limit, sizeof(limit));
        }
        return j;
    }();
    return job;
}

// 后台日志文件路径:系统临时目录下,文件名带毫秒时间戳 + 单调计数器,
// 两者叠加保证同一毫秒内并发起多个后台命令也不会撞名。
std::wstring BuildBackgroundLogPathW() {
    static std::atomic<unsigned long long> counter{0};
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::system_clock::now().time_since_epoch())
                        .count();
    const unsigned long long seq = counter.fetch_add(1);
    const std::filesystem::path dir = std::filesystem::temp_directory_path();
    const std::wstring filename =
        L"lubancode_bg_" + std::to_wstring(ms) + L"_" + std::to_wstring(seq) + L".log";
    return (dir / filename).wstring();
}

}  // namespace

std::wstring BuildCmdCommandLine(const std::string& user_command_utf8) {
    const std::wstring wide_script = Utf8ToWide(user_command_utf8);
    // cmd /d /s /c "<script>":/d 不跑注册表 AutoRun 项,/s 让外层这对引号
    // 整体当"一段"解析、不逐个匹配内部引号——这是拿 cmd 跑任意命令行
    // (命令里本身可能也带引号)的标准写法。
    return L"cmd.exe /d /s /c \"" + wide_script + L"\"";
}

ProcessResult RunProcess(const std::wstring& cmdline, int timeout_ms, const EnvPairs& extra_env,
                          std::size_t max_output_bytes) {
    ProcessResult result;

    const std::vector<EnvBackup> backups = ApplyEnv(extra_env);

    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;
    sa.lpSecurityDescriptor = nullptr;

    HANDLE read_pipe = nullptr;
    HANDLE write_pipe = nullptr;
    if (!CreatePipe(&read_pipe, &write_pipe, &sa, 0)) {
        result.spawn_failed = true;
        result.spawn_error = "创建管道失败(错误码 " + std::to_string(GetLastError()) + ")";
        RestoreEnv(backups);
        return result;
    }
    // 父进程这边留着的读端不能被子进程继承,不然子进程退出后管道写端还有一份
    // 在父进程手里,ReadFile 会一直等不到 EOF。
    SetHandleInformation(read_pipe, HANDLE_FLAG_INHERIT, 0);

    HANDLE stdin_null = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ, &sa, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = write_pipe;
    si.hStdError = write_pipe;
    si.hStdInput = stdin_null;

    PROCESS_INFORMATION pi{};

    std::vector<wchar_t> cmdline_buf(cmdline.begin(), cmdline.end());
    cmdline_buf.push_back(L'\0');

    const BOOL ok = CreateProcessW(
        nullptr, cmdline_buf.data(), nullptr, nullptr, TRUE,
        CREATE_NO_WINDOW | CREATE_SUSPENDED, nullptr, nullptr, &si, &pi);

    // 子进程该拿到的句柄已经拿到了(继承来的),这里可以还原环境变量了——
    // 不用等子进程跑完。
    RestoreEnv(backups);

    // 子进程已经拿到了自己那份继承来的句柄,父进程这边的可以关了。
    CloseHandle(write_pipe);
    if (stdin_null != nullptr && stdin_null != INVALID_HANDLE_VALUE) {
        CloseHandle(stdin_null);
    }

    if (!ok) {
        CloseHandle(read_pipe);
        result.spawn_failed = true;
        result.spawn_error = "启动子进程失败(错误码 " + std::to_string(GetLastError()) + ")";
        return result;
    }

    // Job Object:进程挂在 CREATE_SUSPENDED 状态先分进 job,再恢复运行,
    // 这样超时时关掉 job 句柄,进程本身和它派生出来的所有子进程一起死,
    // 不会漏杀。
    HANDLE job = CreateJobObjectW(nullptr, nullptr);
    if (job != nullptr) {
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limit{};
        limit.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        SetInformationJobObject(job, JobObjectExtendedLimitInformation, &limit, sizeof(limit));
        if (!AssignProcessToJobObject(job, pi.hProcess)) {
            CloseHandle(job);
            job = nullptr;
        }
    }
    ResumeThread(pi.hThread);
    CloseHandle(pi.hThread);

    // overflow_event:读线程发现输出超上限时置信号,主线程的等待立刻醒来
    // 杀进程,不用等超时。手动重置事件,只置一次。
    HANDLE overflow_event = CreateEventW(nullptr, TRUE, FALSE, nullptr);

    std::string output;
    std::atomic<bool> output_over_limit{false};
    std::atomic<bool> reader_stop{false};
    std::atomic<bool> reader_done{false};
    std::thread reader([&] {
        char buf[4096];
        DWORD n = 0;
        while (!reader_stop.load() && ReadFile(read_pipe, buf, sizeof(buf), &n, nullptr) && n > 0) {
            if (output.size() < max_output_bytes) {
                const std::size_t room = max_output_bytes - output.size();
                output.append(buf, std::min<std::size_t>(n, room));
                if (output.size() >= max_output_bytes && !output_over_limit.load()) {
                    output_over_limit.store(true);
                    if (overflow_event != nullptr) {
                        SetEvent(overflow_event);
                    }
                }
            }
            // 超限之后继续读但直接丢弃——不读的话管道缓冲区一满,子进程会
            // 卡在写上死不掉;反正马上就要被杀,读空到 EOF 为止。
        }
        reader_done.store(true);
    });

    const DWORD wait_ms = timeout_ms > 0 ? static_cast<DWORD>(timeout_ms) : INFINITE;
    HANDLE wait_handles[2] = {pi.hProcess, overflow_event};
    const DWORD wait_count = overflow_event != nullptr ? 2 : 1;
    const DWORD wait_result = WaitForMultipleObjects(wait_count, wait_handles, FALSE, wait_ms);
    if (wait_result == WAIT_TIMEOUT || wait_result == WAIT_OBJECT_0 + 1) {
        // 超时,或者输出超上限:都要把整个 Job 杀干净。
        if (wait_result == WAIT_TIMEOUT) {
            result.timed_out = true;
        }
        if (job != nullptr) {
            CloseHandle(job);  // KILL_ON_JOB_CLOSE:关句柄的一瞬间,job 里所有进程全杀
            job = nullptr;
        } else {
            TerminateProcess(pi.hProcess, 1);
        }
        WaitForSingleObject(pi.hProcess, 5000);
    }

    // 根进程退出不等于后代也退出:后代若继承了管道写端还活着,ReadFile 永远
    // 等不到 EOF,直接 join 会吊死。先把 Job 收掉(连带杀光可能残留的后代),
    // 写端才会全关、读线程才能收尾。
    if (job != nullptr) {
        CloseHandle(job);
        job = nullptr;
    }

    // 兜底(比如 Job 创建/绑定失败过、杀不到后代):限时等读线程,等不到就
    // 取消挂起的 ReadFile,绝不无限期阻塞。
    const auto reader_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (!reader_done.load() && std::chrono::steady_clock::now() < reader_deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    if (!reader_done.load()) {
        reader_stop.store(true);
        while (!reader_done.load()) {
            CancelSynchronousIo(reader.native_handle());
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
    }
    reader.join();

    DWORD exit_code = 0;
    GetExitCodeProcess(pi.hProcess, &exit_code);
    result.exit_code = exit_code;
    result.output = std::move(output);
    result.output_truncated = output_over_limit.load();

    if (job != nullptr) {
        CloseHandle(job);
    }
    if (overflow_event != nullptr) {
        CloseHandle(overflow_event);
    }
    CloseHandle(read_pipe);
    CloseHandle(pi.hProcess);

    return result;
}

ProcessResult RunProcess(const std::vector<std::string>& argv, int timeout_ms, const EnvPairs& extra_env,
                          std::size_t max_output_bytes) {
    if (argv.empty()) {
        ProcessResult result;
        result.spawn_failed = true;
        result.spawn_error = "argv 不能为空";
        return result;
    }
    return RunProcess(BuildProcessCommandLine(argv[0], std::vector<std::string>(argv.begin() + 1, argv.end())),
                       timeout_ms, extra_env, max_output_bytes);
}

ProcessResult RunShellCommand(const std::string& command_utf8, int timeout_ms, const EnvPairs& extra_env,
                               std::size_t max_output_bytes) {
    ProcessResult result = RunProcess(BuildCmdCommandLine(command_utf8), timeout_ms, extra_env, max_output_bytes);
    // cmd.exe 走的是系统 ANSI 代码页(国内机器上是 GBK)往管道里写字节,
    // 捕获回来的原始字节要单独转一道才是合法 UTF-8。
    result.output = AcpBytesToUtf8(result.output);
    return result;
}

// ---------------------------------------------------------------------------
// 后台模式:spawn 立刻返回,不等待、不捕获进内存,stdout/stderr 直接指到
// 日志文件的句柄上(CreateProcessW 层面重定向,不经过管道/读线程)。
// ---------------------------------------------------------------------------

BackgroundSpawnResult RunProcessBackground(const std::wstring& cmdline, const EnvPairs& extra_env) {
    BackgroundSpawnResult result;

    const std::vector<EnvBackup> backups = ApplyEnv(extra_env);

    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;
    sa.lpSecurityDescriptor = nullptr;

    const std::wstring log_path_w = BuildBackgroundLogPathW();
    // FILE_SHARE_READ:日志还在写的时候,模型那边"用普通命令看日志"
    // (Get-Content 之类)要能同时打开读,不能被这里的写句柄独占锁死。
    HANDLE log_file = CreateFileW(log_path_w.c_str(), GENERIC_WRITE, FILE_SHARE_READ, &sa, CREATE_ALWAYS,
                                   FILE_ATTRIBUTE_NORMAL, nullptr);
    if (log_file == INVALID_HANDLE_VALUE) {
        result.error = "创建日志文件失败(错误码 " + std::to_string(GetLastError()) + ")";
        RestoreEnv(backups);
        return result;
    }

    HANDLE stdin_null = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ, &sa, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = log_file;
    si.hStdError = log_file;
    si.hStdInput = stdin_null;

    PROCESS_INFORMATION pi{};
    std::vector<wchar_t> cmdline_buf(cmdline.begin(), cmdline.end());
    cmdline_buf.push_back(L'\0');

    const BOOL ok = CreateProcessW(nullptr, cmdline_buf.data(), nullptr, nullptr, TRUE,
                                    CREATE_NO_WINDOW | CREATE_SUSPENDED, nullptr, nullptr, &si, &pi);

    RestoreEnv(backups);
    CloseHandle(log_file);
    if (stdin_null != nullptr && stdin_null != INVALID_HANDLE_VALUE) {
        CloseHandle(stdin_null);
    }

    if (!ok) {
        result.error = "启动子进程失败(错误码 " + std::to_string(GetLastError()) + ")";
        return result;
    }

    // 挂进会话级 job(懒创建的进程级单例,句柄不主动关,见其定义处注释),
    // 不是命令级"用完就杀"的临时 job——这个子进程要活到模型自己收掉,或者
    // lubancode 进程终止为止。AssignProcessToJobObject 失败也不致命,顶多
    // 这个子进程逃逸出会话级收尾(比如它自己又被套进了别的 job),不影响
    // 正常起停流程。
    AssignProcessToJobObject(GetBackgroundSessionJob(), pi.hProcess);
    ResumeThread(pi.hThread);
    CloseHandle(pi.hThread);

    result.success = true;
    result.pid = pi.dwProcessId;
    result.log_path = WideToUtf8(log_path_w);
    // 会话级 job 已经接管这个子进程的生死,父进程这份进程句柄不需要留着。
    CloseHandle(pi.hProcess);
    return result;
}

BackgroundSpawnResult RunProcessBackground(const std::vector<std::string>& argv, const EnvPairs& extra_env) {
    if (argv.empty()) {
        BackgroundSpawnResult result;
        result.error = "argv 不能为空";
        return result;
    }
    return RunProcessBackground(
        BuildProcessCommandLine(argv[0], std::vector<std::string>(argv.begin() + 1, argv.end())), extra_env);
}

BackgroundSpawnResult RunShellCommandBackground(const std::string& command_utf8, const EnvPairs& extra_env) {
    return RunProcessBackground(BuildCmdCommandLine(command_utf8), extra_env);
}

// ---------------------------------------------------------------------------
// ChildProcess:长命双向管道(搬自 mcp/transport.cpp 的 StdioTransport)。
// ---------------------------------------------------------------------------

ChildProcess::~ChildProcess() {
    Shutdown(2000);
}

SpawnResult ChildProcess::Start(const std::string& command, const std::vector<std::string>& args,
                                  const EnvPairs& env, std::function<bool(std::string_view)> on_stdout,
                                  std::function<void(std::string_view)> on_stderr) {
    on_stdout_ = std::move(on_stdout);
    on_stderr_ = std::move(on_stderr);

    const std::vector<EnvBackup> backups = ApplyEnv(env);

    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;
    sa.lpSecurityDescriptor = nullptr;

    HANDLE stdin_read = nullptr;
    HANDLE stdout_write = nullptr;
    HANDLE stderr_write = nullptr;
    HANDLE stdin_write = nullptr;
    HANDLE stdout_read = nullptr;
    HANDLE stderr_read = nullptr;

    if (!CreatePipe(&stdin_read, &stdin_write, &sa, 0)) {
        RestoreEnv(backups);
        return SpawnResult{false, "创建 stdin 管道失败(错误码 " + std::to_string(GetLastError()) + ")", false};
    }
    SetHandleInformation(stdin_write, HANDLE_FLAG_INHERIT, 0);

    if (!CreatePipe(&stdout_read, &stdout_write, &sa, 0)) {
        RestoreEnv(backups);
        CloseHandle(stdin_read);
        CloseHandle(stdin_write);
        return SpawnResult{false, "创建 stdout 管道失败(错误码 " + std::to_string(GetLastError()) + ")", false};
    }
    SetHandleInformation(stdout_read, HANDLE_FLAG_INHERIT, 0);

    if (!CreatePipe(&stderr_read, &stderr_write, &sa, 0)) {
        RestoreEnv(backups);
        CloseHandle(stdin_read);
        CloseHandle(stdin_write);
        CloseHandle(stdout_read);
        CloseHandle(stdout_write);
        return SpawnResult{false, "创建 stderr 管道失败(错误码 " + std::to_string(GetLastError()) + ")", false};
    }
    SetHandleInformation(stderr_read, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = stdin_read;
    si.hStdOutput = stdout_write;
    si.hStdError = stderr_write;

    PROCESS_INFORMATION pi{};
    std::wstring cmdline = BuildProcessCommandLine(command, args);
    std::vector<wchar_t> cmdline_buf(cmdline.begin(), cmdline.end());
    cmdline_buf.push_back(L'\0');

    const BOOL ok = CreateProcessW(nullptr, cmdline_buf.data(), nullptr, nullptr, TRUE,
                                    CREATE_NO_WINDOW | CREATE_SUSPENDED, nullptr, nullptr, &si, &pi);

    RestoreEnv(backups);

    // 子进程该拿到的句柄已经拿到了(继承来的),父进程这边的可以关了。
    CloseHandle(stdin_read);
    CloseHandle(stdout_write);
    CloseHandle(stderr_write);

    if (!ok) {
        const DWORD error_code = GetLastError();
        CloseHandle(stdin_write);
        CloseHandle(stdout_read);
        CloseHandle(stderr_read);
        SpawnResult spawn{};
        spawn.success = false;
        spawn.error = "启动子进程失败(错误码 " + std::to_string(error_code) + "): " + command;
        spawn.command_not_found = (error_code == ERROR_FILE_NOT_FOUND || error_code == ERROR_PATH_NOT_FOUND);
        return spawn;
    }

    process_ = pi.hProcess;
    stdin_write_ = stdin_write;
    stdout_read_ = stdout_read;
    stderr_read_ = stderr_read;

    // Job Object:超时/主动关停时,关掉 job 句柄能把整棵进程树一起杀掉。
    HANDLE job = CreateJobObjectW(nullptr, nullptr);
    if (job != nullptr) {
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limit{};
        limit.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        SetInformationJobObject(job, JobObjectExtendedLimitInformation, &limit, sizeof(limit));
        if (!AssignProcessToJobObject(job, pi.hProcess)) {
            CloseHandle(job);
            job = nullptr;
        }
    }
    job_ = job;
    ResumeThread(pi.hThread);
    CloseHandle(pi.hThread);

    started_ = true;
    stdout_thread_ = std::thread([this] { StdoutReaderThread(); });
    stderr_thread_ = std::thread([this] { StderrReaderThread(); });

    return SpawnResult{true, std::string(), false};
}

void ChildProcess::StdoutReaderThread() {
    char buf[4096];
    DWORD n = 0;
    while (!reader_stop_.load() && ReadFile(static_cast<HANDLE>(stdout_read_), buf, sizeof(buf), &n, nullptr) &&
           n > 0) {
        if (on_stdout_ && !on_stdout_(std::string_view(buf, n))) {
            break;  // 回调宣布这条流报废(MCP 单行超限断连那条路),不再读
        }
    }
    stdout_reader_done_.store(true);
}

void ChildProcess::StderrReaderThread() {
    char buf[4096];
    DWORD n = 0;
    while (!reader_stop_.load() && ReadFile(static_cast<HANDLE>(stderr_read_), buf, sizeof(buf), &n, nullptr) &&
           n > 0) {
        if (on_stderr_) {
            on_stderr_(std::string_view(buf, n));
        }
    }
    stderr_reader_done_.store(true);
}

void ChildProcess::JoinReaderThreads() {
    // 兜底:进程树都该死透了,正常情况下写端全关、ReadFile 返回 0,线程
    // 自己收尾。万一有后代进程漏杀(没有 Job)还握着写端,ReadFile 会一直
    // 挂着——限时等一等,等不到就置停止标志 + 取消挂起的同步 IO,绝不吊死。
    const auto wait_reader = [this](std::thread& thread, std::atomic<bool>& done) {
        if (!thread.joinable()) {
            return;
        }
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        while (!done.load() && std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        if (!done.load()) {
            reader_stop_.store(true);
            while (!done.load()) {
                CancelSynchronousIo(thread.native_handle());
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
            }
        }
        thread.join();
    };
    wait_reader(stdout_thread_, stdout_reader_done_);
    wait_reader(stderr_thread_, stderr_reader_done_);
}

bool ChildProcess::Write(const std::string& data) {
    if (!started_ || stdin_write_ == nullptr || !IsAlive()) {
        return false;
    }
    std::lock_guard<std::mutex> lock(write_mutex_);
    DWORD written = 0;
    std::size_t offset = 0;
    while (offset < data.size()) {
        if (!WriteFile(static_cast<HANDLE>(stdin_write_), data.data() + offset,
                        static_cast<DWORD>(data.size() - offset), &written, nullptr)) {
            return false;
        }
        if (written == 0) {
            return false;
        }
        offset += written;
    }
    return true;
}

void ChildProcess::Kill() {
    if (process_ != nullptr) {
        TerminateProcess(static_cast<HANDLE>(process_), 1);
    }
}

void ChildProcess::Shutdown(int wait_ms) {
    if (!started_ || shutdown_done_.exchange(true)) {
        return;
    }

    // 先关 stdin——关掉写端相当于给子进程发了个 EOF,行为良好的服务器看到
    // stdin EOF 会自己体面退出(协议层要发的 shutdown 请求在这之前已经发过)。
    if (stdin_write_ != nullptr) {
        CloseHandle(static_cast<HANDLE>(stdin_write_));
        stdin_write_ = nullptr;
    }

    if (process_ != nullptr) {
        const DWORD wait_result =
            WaitForSingleObject(static_cast<HANDLE>(process_), wait_ms > 0 ? static_cast<DWORD>(wait_ms) : 0);
        if (wait_result != WAIT_OBJECT_0) {
            // 还没退出:Job Object 连带子子进程一起杀掉;没有 job(创建/绑定
            // 失败过)就退化成只杀主进程。
            if (job_ != nullptr) {
                CloseHandle(static_cast<HANDLE>(job_));
                job_ = nullptr;
            } else {
                TerminateProcess(static_cast<HANDLE>(process_), 1);
            }
            WaitForSingleObject(static_cast<HANDLE>(process_), 5000);
        }
    }

    // 根进程退了不代表后代也退了:后代若继承了 stdout/stderr 写端还活着,
    // 读线程等不到 EOF。先把 Job 收掉(KILL_ON_JOB_CLOSE 连带清光后代),
    // 写端才会全关,读线程才能正常收尾;JoinReaderThreads 里另有限时+取消
    // IO 的兜底。
    if (job_ != nullptr) {
        CloseHandle(static_cast<HANDLE>(job_));
        job_ = nullptr;
    }
    JoinReaderThreads();

    if (stdout_read_ != nullptr) {
        CloseHandle(static_cast<HANDLE>(stdout_read_));
        stdout_read_ = nullptr;
    }
    if (stderr_read_ != nullptr) {
        CloseHandle(static_cast<HANDLE>(stderr_read_));
        stderr_read_ = nullptr;
    }
    if (process_ != nullptr) {
        CloseHandle(static_cast<HANDLE>(process_));
        process_ = nullptr;
    }
}

bool ChildProcess::IsAlive() const {
    if (process_ == nullptr) {
        return false;
    }
    DWORD exit_code = 0;
    if (!GetExitCodeProcess(static_cast<HANDLE>(process_), &exit_code)) {
        return false;
    }
    return exit_code == STILL_ACTIVE;
}

}  // namespace lubancode::platform
