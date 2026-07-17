#include "mcp/transport.hpp"

#include "tools/process_exec.hpp"  // 复用 Utf8ToWide/WideToUtf8,不重复写一份转换代码

#ifdef _WIN32
#include <jobapi2.h>
#include <optional>
#endif

namespace lubancode::mcp {

std::vector<std::string> LineFramer::Feed(std::string_view chunk) {
    buffer_.append(chunk);

    std::vector<std::string> out;
    std::size_t start = 0;
    while (true) {
        const std::size_t newline_pos = buffer_.find('\n', start);
        if (newline_pos == std::string::npos) {
            break;
        }
        std::string_view line(buffer_.data() + start, newline_pos - start);
        if (!line.empty() && line.back() == '\r') {
            line.remove_suffix(1);
        }
        out.emplace_back(line);
        start = newline_pos + 1;
    }
    buffer_.erase(0, start);

    return out;
}

#ifdef _WIN32

namespace {

// Windows 命令行参数的标准转义算法(CommandLineToArgvW 的逆过程):没有
// 空白/引号就原样返回,否则用双引号包起来,内部的反斜杠+引号按规则转义。
// command/args 里任何一段带空格(比如子进程脚本路径含空格)都得靠这个才能
// 被子进程正确解析成一个参数,不会被从中间断开。
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
    std::wstring cmdline = QuoteArgW(tools::Utf8ToWide(command));
    for (const auto& arg : args) {
        cmdline += L' ';
        cmdline += QuoteArgW(tools::Utf8ToWide(arg));
    }
    return cmdline;
}

std::optional<std::wstring> GetEnvVarW(const std::wstring& name) {
    const DWORD size = GetEnvironmentVariableW(name.c_str(), nullptr, 0);
    if (size == 0) {
        return std::nullopt;
    }
    std::wstring buf(size, L'\0');
    const DWORD written = GetEnvironmentVariableW(name.c_str(), buf.data(), size);
    buf.resize(written);
    return buf;
}

}  // namespace

StdioTransport::~StdioTransport() {
    Shutdown(2000);
}

TransportStartResult StdioTransport::Start(const std::string& command, const std::vector<std::string>& args,
                                            const std::vector<std::pair<std::string, std::string>>& env,
                                            std::function<void(std::string)> on_line) {
    on_line_ = std::move(on_line);

    // 临时把 env 写进当前进程的环境变量,CreateProcessW 的 lpEnvironment
    // 传 nullptr 时子进程继承一份快照,创建完就能立刻还原——跟
    // tools/process_exec.cpp 的 RunProcess 是同一套办法。
    struct EnvBackup {
        std::wstring name;
        std::optional<std::wstring> old_value;
    };
    std::vector<EnvBackup> backups;
    backups.reserve(env.size());
    for (const auto& [key, value] : env) {
        const std::wstring wkey = tools::Utf8ToWide(key);
        backups.push_back(EnvBackup{wkey, GetEnvVarW(wkey)});
        SetEnvironmentVariableW(wkey.c_str(), tools::Utf8ToWide(value).c_str());
    }

    const auto restore_env = [&backups]() {
        for (const auto& backup : backups) {
            SetEnvironmentVariableW(backup.name.c_str(), backup.old_value.has_value() ? backup.old_value->c_str() : nullptr);
        }
    };

    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;
    sa.lpSecurityDescriptor = nullptr;

    HANDLE stdin_read = nullptr;
    HANDLE stdout_write = nullptr;
    HANDLE stderr_write = nullptr;

    if (!CreatePipe(&stdin_read, &stdin_write_, &sa, 0)) {
        restore_env();
        return TransportStartResult{false, "创建 stdin 管道失败(错误码 " + std::to_string(GetLastError()) + ")"};
    }
    SetHandleInformation(stdin_write_, HANDLE_FLAG_INHERIT, 0);

    if (!CreatePipe(&stdout_read_, &stdout_write, &sa, 0)) {
        restore_env();
        CloseHandle(stdin_read);
        CloseHandle(stdin_write_);
        stdin_write_ = nullptr;
        return TransportStartResult{false, "创建 stdout 管道失败(错误码 " + std::to_string(GetLastError()) + ")"};
    }
    SetHandleInformation(stdout_read_, HANDLE_FLAG_INHERIT, 0);

    if (!CreatePipe(&stderr_read_, &stderr_write, &sa, 0)) {
        restore_env();
        CloseHandle(stdin_read);
        CloseHandle(stdin_write_);
        stdin_write_ = nullptr;
        CloseHandle(stdout_read_);
        stdout_read_ = nullptr;
        CloseHandle(stdout_write);
        return TransportStartResult{false, "创建 stderr 管道失败(错误码 " + std::to_string(GetLastError()) + ")"};
    }
    SetHandleInformation(stderr_read_, HANDLE_FLAG_INHERIT, 0);

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

    restore_env();

    // 子进程该拿到的句柄已经拿到了(继承来的),父进程这边的可以关了。
    CloseHandle(stdin_read);
    CloseHandle(stdout_write);
    CloseHandle(stderr_write);

    if (!ok) {
        const std::string error = "启动子进程失败(错误码 " + std::to_string(GetLastError()) + "): " + command;
        CloseHandle(stdin_write_);
        stdin_write_ = nullptr;
        CloseHandle(stdout_read_);
        stdout_read_ = nullptr;
        CloseHandle(stderr_read_);
        stderr_read_ = nullptr;
        return TransportStartResult{false, error};
    }

    process_ = pi.hProcess;

    // Job Object:超时/主动关停时,关掉 job 句柄能把整棵进程树一起杀掉。
    job_ = CreateJobObjectW(nullptr, nullptr);
    if (job_ != nullptr) {
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limit{};
        limit.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        SetInformationJobObject(job_, JobObjectExtendedLimitInformation, &limit, sizeof(limit));
        if (!AssignProcessToJobObject(job_, process_)) {
            CloseHandle(job_);
            job_ = nullptr;
        }
    }
    ResumeThread(pi.hThread);
    CloseHandle(pi.hThread);

    started_ = true;
    stdout_thread_ = std::thread([this] { StdoutReaderThread(); });
    stderr_thread_ = std::thread([this] { StderrReaderThread(); });

    return TransportStartResult{true, std::string()};
}

void StdioTransport::StdoutReaderThread() {
    char buf[4096];
    DWORD n = 0;
    while (ReadFile(stdout_read_, buf, sizeof(buf), &n, nullptr) && n > 0) {
        const std::vector<std::string> lines = line_framer_.Feed(std::string_view(buf, n));
        for (const auto& line : lines) {
            if (on_line_) {
                on_line_(line);
            }
        }
    }
}

void StdioTransport::StderrReaderThread() {
    char buf[4096];
    DWORD n = 0;
    while (ReadFile(stderr_read_, buf, sizeof(buf), &n, nullptr) && n > 0) {
        std::lock_guard<std::mutex> lock(stderr_mutex_);
        stderr_buffer_.append(buf, n);
        // 环形日志缓冲:只留最近 8KB,出错时给人看够用,不无限增长。
        constexpr std::size_t kMaxStderrBytes = 8192;
        if (stderr_buffer_.size() > kMaxStderrBytes) {
            stderr_buffer_.erase(0, stderr_buffer_.size() - kMaxStderrBytes);
        }
    }
}

void StdioTransport::JoinReaderThreads() {
    if (stdout_thread_.joinable()) {
        stdout_thread_.join();
    }
    if (stderr_thread_.joinable()) {
        stderr_thread_.join();
    }
}

bool StdioTransport::WriteLine(const std::string& message) {
    if (!started_ || stdin_write_ == nullptr || !IsAlive()) {
        return false;
    }
    std::lock_guard<std::mutex> lock(write_mutex_);
    const std::string payload = message + "\n";
    DWORD written = 0;
    std::size_t offset = 0;
    while (offset < payload.size()) {
        if (!WriteFile(stdin_write_, payload.data() + offset, static_cast<DWORD>(payload.size() - offset), &written,
                        nullptr)) {
            return false;
        }
        if (written == 0) {
            return false;
        }
        offset += written;
    }
    return true;
}

void StdioTransport::Shutdown(int wait_ms) {
    if (!started_ || shutdown_done_.exchange(true)) {
        return;
    }

    // 先关 stdin——MCP 没有标准的 shutdown 请求,直接关掉写端相当于给
    // 子进程发了个 EOF,行为良好的服务器看到 stdin EOF 会自己体面退出。
    if (stdin_write_ != nullptr) {
        CloseHandle(stdin_write_);
        stdin_write_ = nullptr;
    }

    if (process_ != nullptr) {
        const DWORD wait_result = WaitForSingleObject(process_, wait_ms > 0 ? static_cast<DWORD>(wait_ms) : 0);
        if (wait_result != WAIT_OBJECT_0) {
            // 还没退出:Job Object 连带子子进程一起杀掉;没有 job(创建/绑定
            // 失败过)就退化成只杀主进程。
            if (job_ != nullptr) {
                CloseHandle(job_);
                job_ = nullptr;
            } else {
                TerminateProcess(process_, 1);
            }
            WaitForSingleObject(process_, 5000);
        }
    }

    // 进程已经死透(或者被杀了),两条读线程的 ReadFile 会自然返回 0/失败,
    // 线程能正常退出——这里 join 收尾。
    JoinReaderThreads();

    if (stdout_read_ != nullptr) {
        CloseHandle(stdout_read_);
        stdout_read_ = nullptr;
    }
    if (stderr_read_ != nullptr) {
        CloseHandle(stderr_read_);
        stderr_read_ = nullptr;
    }
    if (job_ != nullptr) {
        CloseHandle(job_);
        job_ = nullptr;
    }
    if (process_ != nullptr) {
        CloseHandle(process_);
        process_ = nullptr;
    }
}

bool StdioTransport::IsAlive() const {
    if (process_ == nullptr) {
        return false;
    }
    DWORD exit_code = 0;
    if (!GetExitCodeProcess(process_, &exit_code)) {
        return false;
    }
    return exit_code == STILL_ACTIVE;
}

std::string StdioTransport::StderrTail() const {
    std::lock_guard<std::mutex> lock(stderr_mutex_);
    return stderr_buffer_;
}

#endif  // _WIN32

}  // namespace lubancode::mcp
