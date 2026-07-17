#include "lsp/transport.hpp"

#include <cctype>

#include "tools/process_exec.hpp"  // 复用 Utf8ToWide,不重复写一份转换代码

#ifdef _WIN32
#include <jobapi2.h>
#include <optional>
#endif

namespace lubancode::lsp {

namespace {

// 在一整块头部文本(不含结尾的 \r\n\r\n)里找 Content-Length 的值。
// 头名大小写不敏感,冒号后允许空格。找不到/不是数字返回 -1。
long long ParseContentLength(std::string_view header_block) {
    std::size_t pos = 0;
    while (pos <= header_block.size()) {
        std::size_t line_end = header_block.find("\r\n", pos);
        if (line_end == std::string_view::npos) {
            line_end = header_block.size();
        }
        std::string_view line = header_block.substr(pos, line_end - pos);
        const std::size_t colon = line.find(':');
        if (colon != std::string_view::npos) {
            std::string name(line.substr(0, colon));
            for (char& c : name) {
                c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            }
            if (name == "content-length") {
                std::string_view value = line.substr(colon + 1);
                while (!value.empty() && (value.front() == ' ' || value.front() == '\t')) {
                    value.remove_prefix(1);
                }
                while (!value.empty() && (value.back() == ' ' || value.back() == '\t' || value.back() == '\r')) {
                    value.remove_suffix(1);
                }
                if (value.empty()) {
                    return -1;
                }
                long long out = 0;
                for (const char c : value) {
                    if (std::isdigit(static_cast<unsigned char>(c)) == 0) {
                        return -1;
                    }
                    out = out * 10 + (c - '0');
                }
                return out;
            }
        }
        if (line_end >= header_block.size()) {
            break;
        }
        pos = line_end + 2;
    }
    return -1;
}

}  // namespace

std::vector<std::string> ContentLengthFramer::Feed(std::string_view chunk) {
    buffer_.append(chunk);

    std::vector<std::string> out;
    while (true) {
        if (!in_body_) {
            // 正在等头:头部块以 \r\n\r\n 收尾。
            const std::size_t header_end = buffer_.find("\r\n\r\n");
            if (header_end == std::string::npos) {
                break;  // 头还没到齐,残包留缓冲
            }
            const long long length = ParseContentLength(std::string_view(buffer_.data(), header_end));
            buffer_.erase(0, header_end + 4);
            if (length < 0) {
                // 坏头(没有 Content-Length/不是数字):丢掉这块头,继续找
                // 下一条,不把整条流搞死。
                continue;
            }
            expected_ = static_cast<std::size_t>(length);
            in_body_ = true;
        }
        // 正在攒正文。
        if (buffer_.size() < expected_) {
            break;  // 正文还没到齐,残包留缓冲
        }
        out.push_back(buffer_.substr(0, expected_));
        buffer_.erase(0, expected_);
        expected_ = 0;
        in_body_ = false;
    }
    return out;
}

#ifdef _WIN32

namespace {

// Windows 命令行参数的标准转义算法(CommandLineToArgvW 的逆过程)——跟
// mcp/transport.cpp 里那份逻辑一致,但这里是 lsp/ 自己的一份(不 include
// mcp/ 的东西,两边各自独立演化)。
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

}  // namespace

StdioTransport::~StdioTransport() {
    Shutdown(2000);
}

TransportStartResult StdioTransport::Start(const std::string& command, const std::vector<std::string>& args,
                                            std::function<void(std::string)> on_message) {
    on_message_ = std::move(on_message);

    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;
    sa.lpSecurityDescriptor = nullptr;

    HANDLE stdin_read = nullptr;
    HANDLE stdout_write = nullptr;
    HANDLE stderr_write = nullptr;

    if (!CreatePipe(&stdin_read, &stdin_write_, &sa, 0)) {
        return TransportStartResult{false, "创建 stdin 管道失败(错误码 " + std::to_string(GetLastError()) + ")"};
    }
    SetHandleInformation(stdin_write_, HANDLE_FLAG_INHERIT, 0);

    if (!CreatePipe(&stdout_read_, &stdout_write, &sa, 0)) {
        CloseHandle(stdin_read);
        CloseHandle(stdin_write_);
        stdin_write_ = nullptr;
        return TransportStartResult{false, "创建 stdout 管道失败(错误码 " + std::to_string(GetLastError()) + ")"};
    }
    SetHandleInformation(stdout_read_, HANDLE_FLAG_INHERIT, 0);

    if (!CreatePipe(&stderr_read_, &stderr_write, &sa, 0)) {
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

    // 子进程该拿到的句柄已经拿到了(继承来的),父进程这边的可以关了。
    CloseHandle(stdin_read);
    CloseHandle(stdout_write);
    CloseHandle(stderr_write);

    if (!ok) {
        const DWORD error_code = GetLastError();
        std::string error = "启动子进程失败(错误码 " + std::to_string(error_code) + "): " + command;
        if (error_code == ERROR_FILE_NOT_FOUND || error_code == ERROR_PATH_NOT_FOUND) {
            error = "未找到命令 " + command + "(错误码 " + std::to_string(error_code) + ")";
        }
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
        const std::vector<std::string> messages = framer_.Feed(std::string_view(buf, n));
        for (auto& message : messages) {
            if (on_message_) {
                on_message_(std::move(message));
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

bool StdioTransport::WriteMessage(const std::string& body) {
    if (!started_ || stdin_write_ == nullptr || !IsAlive()) {
        return false;
    }
    std::lock_guard<std::mutex> lock(write_mutex_);
    const std::string payload = "Content-Length: " + std::to_string(body.size()) + "\r\n\r\n" + body;
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

    // 关 stdin 写端:协议层的 shutdown 请求 + exit 通知在这之前已经发过了
    // (lsp::Client::Shutdown 负责),这里补一个 EOF,行为良好的服务器两样
    // 信号至少能收到一样,自己体面退出。
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

}  // namespace lubancode::lsp
