// interactive_process.hpp 的实现。Windows 走 CreatePipe+CreateProcessW
//(读线程独立收字节);POSIX 走 pipe+fork+execvp(进程组)。杀树语义:
// Windows 进程挂 Job Object(TerminateJobObject);POSIX 杀进程组。
#include "interactive_process.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <functional>
#include <map>
#include <mutex>
#include <thread>
#include <utility>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
using RawHandle = HANDLE;
constexpr RawHandle kInvalidHandle = nullptr;
// MSVC 的 CRT 头不一定带 ssize_t,自己统一一枚(与 POSIX read/write 的
// 返回口径对上)。
using ssize_t = long long;
#else
#include <signal.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#include <cstdlib>
using RawHandle = int;
constexpr RawHandle kInvalidHandle = -1;
#endif

namespace lubancode::test_support {

namespace {

#ifdef _WIN32
// 阻塞读一份字节;0/负 = 流尽或断管。
ssize_t ReadFromHandle(RawHandle handle, char* buffer, std::size_t size) {
    DWORD got = 0;
    if (!ReadFile(handle, buffer, static_cast<DWORD>(size), &got, nullptr)) {
        return -1;
    }
    return got == 0 ? -1 : static_cast<ssize_t>(got);
}
#else
ssize_t ReadFromHandle(RawHandle fd, char* buffer, std::size_t size) {
    const ssize_t got = ::read(fd, buffer, size);
    return got <= 0 ? -1 : got;
}
#endif

// 行缓冲的读侧:独立线程逐块收,按 \n 切行进队列。管道关闭后线程自然
// 收口;对象寿命由 Impl 持有,线程 join 在 Shutdown(析构必达)。
class LineReader {
public:
    explicit LineReader(std::function<ssize_t(char*, std::size_t)> read_fn)
        : read_fn_(std::move(read_fn)) {
        worker_ = std::thread([this] { Run(); });
    }

    std::optional<std::string> ReadLine(int timeout_ms) {
        std::unique_lock<std::mutex> lock(mutex_);
        if (!cv_.wait_for(lock, std::chrono::milliseconds(timeout_ms),
                          [this] { return !lines_.empty() || done_; })) {
            return std::nullopt;  // 超时
        }
        if (lines_.empty()) {
            return std::nullopt;  // done(流尽)
        }
        std::string line = std::move(lines_.front());
        lines_.pop_front();
        return line;
    }

    std::string RawText() {
        std::lock_guard<std::mutex> lock(mutex_);
        return raw_;
    }

    void Shutdown() {
        if (worker_.joinable()) {
            worker_.join();
        }
    }

private:
    void Run() {
        std::string pending;
        char buffer[4096];
        while (true) {
            const ssize_t got = read_fn_(buffer, sizeof(buffer));
            if (got <= 0) {
                break;
            }
            std::lock_guard<std::mutex> lock(mutex_);
            raw_.append(buffer, static_cast<std::size_t>(got));
            pending.append(buffer, static_cast<std::size_t>(got));
            std::size_t newline = pending.find('\n');
            while (newline != std::string::npos) {
                std::string line = pending.substr(0, newline);
                if (!line.empty() && line.back() == '\r') {
                    line.pop_back();
                }
                lines_.push_back(std::move(line));
                pending.erase(0, newline + 1);
                newline = pending.find('\n');
            }
            cv_.notify_all();
        }
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (!pending.empty()) {
                lines_.push_back(std::move(pending));  // 末行无换行也交出
            }
            done_ = true;
        }
        cv_.notify_all();
    }

    std::function<ssize_t(char*, std::size_t)> read_fn_;
    std::thread worker_;
    std::mutex mutex_;
    std::condition_variable cv_;
    std::deque<std::string> lines_;
    std::string raw_;
    bool done_ = false;
};

}  // namespace

struct InteractiveProcess::Impl {
    RawHandle stdin_write = kInvalidHandle;    // 父侧写端
    RawHandle stdout_read = kInvalidHandle;    // 父侧读端
    RawHandle stderr_read = kInvalidHandle;    // 父侧读端(stderr)
#ifdef _WIN32
    HANDLE process = nullptr;
    HANDLE job = nullptr;
#else
    pid_t pid = -1;
    bool waited = false;
    int exit_status = -1;
#endif
    std::unique_ptr<LineReader> stdout_reader;
    std::unique_ptr<LineReader> stderr_reader;
    std::atomic<bool> stdin_broken{false};

    void KillTree() {
#ifdef _WIN32
        if (job != nullptr) {
            TerminateJobObject(job, 1);
        } else if (process != nullptr) {
            TerminateProcess(process, 1);
        }
#else
        if (pid > 0) {
            kill(-pid, SIGKILL);
            kill(pid, SIGKILL);
        }
#endif
    }
};

InteractiveProcess::~InteractiveProcess() {
    CloseStdin();  // 子进程 stdin 见 EOF,自然收线
    impl_->KillTree();
    impl_->stdout_reader->Shutdown();
    impl_->stderr_reader->Shutdown();
#ifdef _WIN32
    if (impl_->process != nullptr) {
        WaitForSingleObject(impl_->process, 5000);
        CloseHandle(impl_->process);
    }
    if (impl_->job != nullptr) {
        CloseHandle(impl_->job);
    }
#else
    if (impl_->pid > 0 && !impl_->waited) {
        waitpid(impl_->pid, &impl_->exit_status, 0);
        impl_->waited = true;
    }
#endif
    if (impl_->stdout_read != kInvalidHandle) {
#ifdef _WIN32
        CloseHandle(impl_->stdout_read);
        CloseHandle(impl_->stderr_read);
#else
        ::close(impl_->stdout_read);
        ::close(impl_->stderr_read);
#endif
    }
    delete impl_;
}

bool InteractiveProcess::WriteLine(const std::string& line) {
    if (impl_->stdin_broken.load() || impl_->stdin_write == kInvalidHandle) {
        return false;
    }
    const std::string bytes = line + "\n";
#ifdef _WIN32
    DWORD written = 0;
    if (!WriteFile(impl_->stdin_write, bytes.data(), static_cast<DWORD>(bytes.size()), &written,
                   nullptr)) {
        impl_->stdin_broken.store(true);
        return false;
    }
#else
    const ssize_t wrote = ::write(impl_->stdin_write, bytes.data(), bytes.size());
    if (wrote != static_cast<ssize_t>(bytes.size())) {
        impl_->stdin_broken.store(true);
        return false;
    }
#endif
    return true;
}

void InteractiveProcess::CloseStdin() {
    if (impl_->stdin_write == kInvalidHandle) {
        return;
    }
#ifdef _WIN32
    CloseHandle(impl_->stdin_write);
#else
    ::close(impl_->stdin_write);
#endif
    impl_->stdin_write = kInvalidHandle;
}

std::optional<std::string> InteractiveProcess::ReadLine(int timeout_ms) {
    return impl_->stdout_reader->ReadLine(timeout_ms);
}

std::string InteractiveProcess::StderrText() {
    return impl_->stderr_reader->RawText();
}

bool InteractiveProcess::Wait(int timeout_ms, int* exit_code) {
#ifdef _WIN32
    if (impl_->process == nullptr) {
        return false;
    }
    const DWORD wait_ms = timeout_ms < 0 ? INFINITE : static_cast<DWORD>(timeout_ms);
    if (WaitForSingleObject(impl_->process, wait_ms) != WAIT_OBJECT_0) {
        return false;
    }
    DWORD code = 0;
    GetExitCodeProcess(impl_->process, &code);
    if (exit_code != nullptr) {
        *exit_code = static_cast<int>(code);
    }
    return true;
#else
    if (impl_->waited) {
        if (exit_code != nullptr) {
            *exit_code = WIFEXITED(impl_->exit_status) ? WEXITSTATUS(impl_->exit_status) : -1;
        }
        return true;
    }
    // 带超时的等待:短轮询(测试场景秒级精度,waitpid 无原生超时)。
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    while (std::chrono::steady_clock::now() < deadline) {
        const pid_t got = waitpid(impl_->pid, &impl_->exit_status, WNOHANG);
        if (got == impl_->pid) {
            impl_->waited = true;
            if (exit_code != nullptr) {
                *exit_code = WIFEXITED(impl_->exit_status) ? WEXITSTATUS(impl_->exit_status) : -1;
            }
            return true;
        }
        if (got < 0) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return false;
#endif
}

std::unique_ptr<InteractiveProcess> InteractiveProcess::Spawn(
    const std::vector<std::string>& argv,
    const std::vector<std::pair<std::string, std::string>>& extra_env, const std::string& cwd_utf8,
    std::string* error) {
    if (argv.empty()) {
        if (error != nullptr) {
            *error = "argv 为空";
        }
        return nullptr;
    }
    std::unique_ptr<Impl> impl = std::make_unique<Impl>();
#ifdef _WIN32
    SECURITY_ATTRIBUTES inheritable{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
    HANDLE stdin_read = nullptr, stdin_write = nullptr;
    HANDLE stdout_read = nullptr, stdout_write = nullptr;
    HANDLE stderr_read = nullptr, stderr_write = nullptr;
    if (!CreatePipe(&stdin_read, &stdin_write, &inheritable, 0) ||
        !CreatePipe(&stdout_read, &stdout_write, &inheritable, 0) ||
        !CreatePipe(&stderr_read, &stderr_write, &inheritable, 0)) {
        if (error != nullptr) {
            *error = "CreatePipe 失败";
        }
        return nullptr;
    }
    // 父侧的端不许被子进程继承。
    SetHandleInformation(stdin_write, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(stdout_read, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(stderr_read, HANDLE_FLAG_INHERIT, 0);

    // 命令行:含空格的段引号包住(简单形状,测试参数不含内嵌引号)。
    std::string cmdline;
    for (const std::string& arg : argv) {
        if (!cmdline.empty()) {
            cmdline += " ";
        }
        if (arg.find_first_of(" \t\"") == std::string::npos) {
            cmdline += arg;
        } else {
            cmdline += "\"" + arg + "\"";
        }
    }
    const auto to_wide = [](const std::string& text) {
        const int len = MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, nullptr, 0);
        std::wstring wide(static_cast<std::size_t>(len > 0 ? len - 1 : 0), L'\0');
        if (len > 0) {
            MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, wide.data(), len);
        }
        return wide;
    };
    std::wstring wide_cmdline = to_wide(cmdline);
    const std::wstring wide_cwd = cwd_utf8.empty() ? std::wstring() : to_wide(cwd_utf8);

    // 环境块:当前环境 + 追加覆盖,按 KEY\0VALUE\0…\0 排布。
    std::wstring env_block;
    {
        std::map<std::wstring, std::wstring> merged;
        LPWCH current = GetEnvironmentStringsW();
        if (current != nullptr) {
            for (LPWCH cursor = current; *cursor != L'\0'; cursor += std::wcslen(cursor) + 1) {
                const std::wstring entry(cursor);
                const std::size_t eq = entry.find(L'=');
                if (eq != std::wstring::npos) {
                    merged[entry.substr(0, eq)] = entry.substr(eq + 1);
                }
            }
            FreeEnvironmentStringsW(current);
        }
        for (const auto& [key, value] : extra_env) {
            merged[to_wide(key)] = to_wide(value);
        }
        for (const auto& [key, value] : merged) {
            env_block += key + L"=" + value + L'\0';
        }
        env_block += L'\0';
    }

    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdInput = stdin_read;
    startup.hStdOutput = stdout_write;
    startup.hStdError = stderr_write;
    PROCESS_INFORMATION proc{};
    impl->job = CreateJobObjectW(nullptr, nullptr);
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    SetInformationJobObject(impl->job, JobObjectExtendedLimitInformation, &limits, sizeof(limits));

    const BOOL spawned = CreateProcessW(
        nullptr, wide_cmdline.data(), nullptr, nullptr, TRUE,
        CREATE_SUSPENDED | CREATE_UNICODE_ENVIRONMENT, env_block.data(),
        wide_cwd.empty() ? nullptr : wide_cwd.c_str(), &startup, &proc);
    // 子进程侧的端在父进程里关掉(子进程有自己的继承副本)。
    CloseHandle(stdin_read);
    CloseHandle(stdout_write);
    CloseHandle(stderr_write);
    if (!spawned) {
        CloseHandle(stdin_write);
        CloseHandle(stdout_read);
        CloseHandle(stderr_read);
        if (impl->job != nullptr) {
            CloseHandle(impl->job);
        }
        if (error != nullptr) {
            *error = "CreateProcessW 失败(GetLastError=" + std::to_string(GetLastError()) + ")";
        }
        return nullptr;
    }
    if (impl->job != nullptr) {
        AssignProcessToJobObject(impl->job, proc.hProcess);
    }
    ResumeThread(proc.hThread);
    CloseHandle(proc.hThread);
    impl->process = proc.hProcess;
    impl->stdin_write = stdin_write;
    impl->stdout_read = stdout_read;
    impl->stderr_read = stderr_read;
#else
    int stdin_fds[2], stdout_fds[2], stderr_fds[2];
    if (pipe(stdin_fds) != 0 || pipe(stdout_fds) != 0 || pipe(stderr_fds) != 0) {
        if (error != nullptr) {
            *error = "pipe 失败";
        }
        return nullptr;
    }
    const pid_t pid = fork();
    if (pid < 0) {
        if (error != nullptr) {
            *error = "fork 失败";
        }
        return nullptr;
    }
    if (pid == 0) {
        // 子进程:进程组自立(杀树一锅端),管道接好,exec。
        setpgid(0, 0);
        dup2(stdin_fds[0], STDIN_FILENO);
        dup2(stdout_fds[1], STDOUT_FILENO);
        dup2(stderr_fds[1], STDERR_FILENO);
        for (int fd : {stdin_fds[0], stdin_fds[1], stdout_fds[0], stdout_fds[1], stderr_fds[0],
                       stderr_fds[1]}) {
            close(fd);
        }
        for (const auto& [key, value] : extra_env) {
            setenv(key.c_str(), value.c_str(), 1);
        }
        if (!cwd_utf8.empty() && chdir(cwd_utf8.c_str()) != 0) {
            _exit(127);
        }
        std::vector<char*> raw_argv;
        raw_argv.reserve(argv.size() + 1);
        for (const std::string& arg : argv) {
            raw_argv.push_back(const_cast<char*>(arg.c_str()));
        }
        raw_argv.push_back(nullptr);
        execvp(raw_argv[0], raw_argv.data());
        _exit(127);
    }
    close(stdin_fds[0]);
    close(stdout_fds[1]);
    close(stderr_fds[1]);
    impl->pid = pid;
    impl->stdin_write = stdin_fds[1];
    impl->stdout_read = stdout_fds[0];
    impl->stderr_read = stderr_fds[0];
#endif
    // 读侧线程:分支后的句柄各自捕获。
    const RawHandle stdout_handle = impl->stdout_read;
    const RawHandle stderr_handle = impl->stderr_read;
    impl->stdout_reader = std::make_unique<LineReader>(
        [stdout_handle](char* buffer, std::size_t size) { return ReadFromHandle(stdout_handle, buffer, size); });
    impl->stderr_reader = std::make_unique<LineReader>(
        [stderr_handle](char* buffer, std::size_t size) { return ReadFromHandle(stderr_handle, buffer, size); });

    auto out = std::make_unique<InteractiveProcess>();
    out->impl_ = impl.release();
    return out;
}

}  // namespace lubancode::test_support
