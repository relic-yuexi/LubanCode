#include "tools/process_exec.hpp"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <jobapi2.h>
#include <optional>
#include <thread>
#endif

namespace lubancode::tools {

#ifdef _WIN32

std::wstring Utf8ToWide(const std::string& utf8) {
    if (utf8.empty()) {
        return std::wstring();
    }
    const int len = MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), nullptr, 0);
    std::wstring wide(static_cast<std::size_t>(len), L'\0');
    if (len > 0) {
        MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), wide.data(), len);
    }
    return wide;
}

std::string WideToUtf8(const std::wstring& wide) {
    if (wide.empty()) {
        return std::string();
    }
    const int len = WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), nullptr, 0, nullptr, nullptr);
    std::string utf8(static_cast<std::size_t>(len), '\0');
    if (len > 0) {
        WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), utf8.data(), len, nullptr, nullptr);
    }
    return utf8;
}

std::string AcpBytesToUtf8(const std::string& acp_bytes) {
    if (acp_bytes.empty()) {
        return std::string();
    }
    const int wlen = MultiByteToWideChar(CP_ACP, 0, acp_bytes.data(), static_cast<int>(acp_bytes.size()), nullptr, 0);
    if (wlen <= 0) {
        return acp_bytes;  // 解不出来就原样返回,好歹不丢数据
    }
    std::wstring wide(static_cast<std::size_t>(wlen), L'\0');
    MultiByteToWideChar(CP_ACP, 0, acp_bytes.data(), static_cast<int>(acp_bytes.size()), wide.data(), wlen);
    return WideToUtf8(wide);
}

std::wstring BuildCmdCommandLine(const std::string& user_command_utf8) {
    const std::wstring wide_script = Utf8ToWide(user_command_utf8);
    // cmd /d /s /c "<script>":/d 不跑注册表 AutoRun 项,/s 让外层这对引号
    // 整体当"一段"解析、不逐个匹配内部引号——这是拿 cmd 跑任意命令行
    // (命令里本身可能也带引号)的标准写法。
    return L"cmd.exe /d /s /c \"" + wide_script + L"\"";
}

namespace {

// 读一个环境变量的当前值(找不到就是 std::nullopt)。RunProcess 临时改
// extra_env 之前先备份旧值,跑完子进程立刻还原用。
std::optional<std::wstring> GetEnvVarW(const std::wstring& name) {
    const DWORD size = GetEnvironmentVariableW(name.c_str(), nullptr, 0);
    if (size == 0) {
        return std::nullopt;  // 不存在(或者是空串,简化处理不细分,下面还原时按“删掉”处理没什么大碍)
    }
    std::wstring buf(size, L'\0');
    const DWORD written = GetEnvironmentVariableW(name.c_str(), buf.data(), size);
    buf.resize(written);
    return buf;
}

}  // namespace

ProcessResult RunProcess(const std::wstring& cmdline, int timeout_ms,
                          const std::vector<std::pair<std::string, std::string>>& extra_env) {
    ProcessResult result;

    // 临时把 extra_env 写进当前进程的环境变量。CreateProcessW 的
    // lpEnvironment 传 nullptr 时,子进程会继承父进程环境的一份快照——
    // 子进程一创建,这份快照就跟父进程后续怎么改环境变量没关系了,所以
    // 用完立刻还原不会影响已经起来的子进程。要求调用方单线程顺序调用,
    // 见头文件里的说明。
    struct EnvBackup {
        std::wstring name;
        std::optional<std::wstring> old_value;
    };
    std::vector<EnvBackup> backups;
    backups.reserve(extra_env.size());
    for (const auto& [key, value] : extra_env) {
        const std::wstring wkey = Utf8ToWide(key);
        backups.push_back(EnvBackup{wkey, GetEnvVarW(wkey)});
        SetEnvironmentVariableW(wkey.c_str(), Utf8ToWide(value).c_str());
    }

    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;
    sa.lpSecurityDescriptor = nullptr;

    HANDLE read_pipe = nullptr;
    HANDLE write_pipe = nullptr;
    if (!CreatePipe(&read_pipe, &write_pipe, &sa, 0)) {
        result.spawn_failed = true;
        result.spawn_error = "创建管道失败(错误码 " + std::to_string(GetLastError()) + ")";
        for (const auto& backup : backups) {
            SetEnvironmentVariableW(backup.name.c_str(), backup.old_value.has_value() ? backup.old_value->c_str() : nullptr);
        }
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
    for (const auto& backup : backups) {
        SetEnvironmentVariableW(backup.name.c_str(), backup.old_value.has_value() ? backup.old_value->c_str() : nullptr);
    }

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

    std::string output;
    std::thread reader([&] {
        char buf[4096];
        DWORD n = 0;
        while (ReadFile(read_pipe, buf, sizeof(buf), &n, nullptr) && n > 0) {
            output.append(buf, n);
        }
    });

    const DWORD wait_ms = timeout_ms > 0 ? static_cast<DWORD>(timeout_ms) : INFINITE;
    const DWORD wait_result = WaitForSingleObject(pi.hProcess, wait_ms);
    if (wait_result == WAIT_TIMEOUT) {
        result.timed_out = true;
        if (job != nullptr) {
            CloseHandle(job);  // KILL_ON_JOB_CLOSE:关句柄的一瞬间,job 里所有进程全杀
            job = nullptr;
        } else {
            TerminateProcess(pi.hProcess, 1);
        }
        WaitForSingleObject(pi.hProcess, 5000);
    }

    reader.join();  // 写端全关了(进程死透),ReadFile 自然返回 0,线程能退出

    DWORD exit_code = 0;
    GetExitCodeProcess(pi.hProcess, &exit_code);
    result.exit_code = exit_code;
    result.output = std::move(output);

    if (job != nullptr) {
        CloseHandle(job);
    }
    CloseHandle(read_pipe);
    CloseHandle(pi.hProcess);

    return result;
}

#else  // !_WIN32

ProcessResult RunProcess(const std::wstring&, int, const std::vector<std::pair<std::string, std::string>>&) {
    ProcessResult result;
    result.spawn_failed = true;
    result.spawn_error = "RunProcess 眼下只实现了 Windows";
    return result;
}

#endif

}  // namespace lubancode::tools
