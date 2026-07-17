#include "tools/run_command.hpp"

#include <sstream>
#include <string>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <jobapi2.h>
#include <thread>
#include <vector>
#endif

namespace lubancode::tools {

namespace {

constexpr int kDefaultTimeoutMs = 120000;

}  // namespace

std::string RunCommandTool::name() const {
    return "run_command";
}

std::string RunCommandTool::description() const {
    return "在 shell 里执行一条命令,拿到合并后的标准输出/标准错误,以及退出码。"
           "shell 参数可选 powershell(默认)或 cmd,分别按对应语法写命令。执行前要经用户确认。"
           "超时会被强制杀掉。";
}

nlohmann::json RunCommandTool::input_schema() const {
    nlohmann::json schema = nlohmann::json::object();
    schema["type"] = "object";

    nlohmann::json properties = nlohmann::json::object();

    nlohmann::json command_prop = nlohmann::json::object();
    command_prop["type"] = "string";
    command_prop["description"] = "要执行的命令,按所选 shell 的语法写(默认 PowerShell 语法)";
    properties["command"] = command_prop;

    nlohmann::json timeout_prop = nlohmann::json::object();
    timeout_prop["type"] = "integer";
    timeout_prop["description"] = "超时时间,单位毫秒,不填默认 120000(2 分钟)";
    properties["timeout_ms"] = timeout_prop;

    nlohmann::json shell_prop = nlohmann::json::object();
    shell_prop["type"] = "string";
    shell_prop["enum"] = nlohmann::json::array({"powershell", "cmd"});
    shell_prop["description"] = "用哪个 shell 执行,不填默认 powershell";
    properties["shell"] = shell_prop;

    schema["properties"] = properties;
    schema["required"] = nlohmann::json::array({"command"});

    return schema;
}

#ifdef _WIN32

namespace {

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

// cmd.exe 路径专用:把捕获到的字节按系统 ANSI 代码页(国内机器是 GBK,
// CP_ACP)解出来,转成 UTF-8。
//
// 踩过的坑:一开始想学 PowerShell 那条路,在跑命令前先 `chcp 65001>nul`
// 把这个隐藏控制台的活动代码页切到 UTF-8,指望后续输出自然就是 UTF-8——
// 实测不管用。chcp 改的是"控制台对象"本身的代码页,只在输出真的经
// WriteConsole 写向一个显示中的控制台屏幕缓冲区时才起作用;这里 stdout/
// stderr 被重定向到匿名管道(不是控制台),cmd.exe 内置命令(echo/dir/type
// 之类)走的是 WriteFile 直接写字节,这条路径下的宽字符转窄字节固定用的
// 是系统 ANSI 代码页,不受 chcp 影响——用十六进制实测验证过:即使先
// `chcp 65001` 再 `echo 你好世界`,管道里收到的字节原样是 GBK
// (C4E3 BAC3 CAC0 BDE7),不是 UTF-8。索性放弃"提前切代码页"这条路,
// 改成事后按 CP_ACP 解码、转 UTF-8——这个办法实测靠谱。
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

// 手写的标准 base64 编码,-EncodedCommand 要的就是这个格式,不引额外依赖。
std::string Base64Encode(const unsigned char* data, std::size_t len) {
    static const char kTable[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve(((len + 2) / 3) * 4);

    std::size_t i = 0;
    while (i + 3 <= len) {
        const unsigned int n = (static_cast<unsigned int>(data[i]) << 16) |
                                (static_cast<unsigned int>(data[i + 1]) << 8) |
                                static_cast<unsigned int>(data[i + 2]);
        out.push_back(kTable[(n >> 18) & 0x3F]);
        out.push_back(kTable[(n >> 12) & 0x3F]);
        out.push_back(kTable[(n >> 6) & 0x3F]);
        out.push_back(kTable[n & 0x3F]);
        i += 3;
    }
    const std::size_t rem = len - i;
    if (rem == 1) {
        const unsigned int n = static_cast<unsigned int>(data[i]) << 16;
        out.push_back(kTable[(n >> 18) & 0x3F]);
        out.push_back(kTable[(n >> 12) & 0x3F]);
        out.push_back('=');
        out.push_back('=');
    } else if (rem == 2) {
        const unsigned int n = (static_cast<unsigned int>(data[i]) << 16) | (static_cast<unsigned int>(data[i + 1]) << 8);
        out.push_back(kTable[(n >> 18) & 0x3F]);
        out.push_back(kTable[(n >> 12) & 0x3F]);
        out.push_back(kTable[(n >> 6) & 0x3F]);
        out.push_back('=');
    }
    return out;
}

// 拼一段 PowerShell 脚本,再编码成 -EncodedCommand 要的 UTF-16LE + base64。
// 用 -EncodedCommand 而不是直接拼 -Command "...",是为了绕开用户命令里
// 可能带的引号、特殊字符,不用操心转义。
//
// 脚本本身做了三件事(踩过的坑,都是在这台机器上实测验证过的):
//   1. $ProgressPreference='SilentlyContinue':压掉 PowerShell 第一次用某些
//      模块时打印的进度信息(会混进合并流里,污染输出)。
//   2. [Console]::OutputEncoding=UTF8:不设的话,输出走的是系统 ANSI 代码页
//      (国内机器上是 GBK),中文会变成乱码;这行必须在真正跑命令之前设置。
//   3. 用 `& { 命令 } 2>&1 | Out-String -Stream | Write-Output` 而不是直接
//      `命令 2>&1`:PowerShell 的错误流(Write-Error、命令找不到之类)一旦被
//      重定向到管道(不是真终端),会被序列化成一大坨 CLIXML,人和模型都读不懂。
//      经 Out-String 转成纯文本以后就是正常可读的文本了。
//      代价是这条路径下 $? 会被 Out-String/Write-Output 这两级管道盖掉,
//      所以退出码改靠 $LASTEXITCODE(外部程序、或者脚本里显式 exit N)来判断,
//      查不到 $LASTEXITCODE 时才退回去看 $?。
std::string BuildEncodedCommand(const std::string& user_command_utf8) {
    const std::string script_utf8 =
        "$ProgressPreference='SilentlyContinue'\r\n"
        "[Console]::OutputEncoding=[System.Text.Encoding]::UTF8\r\n"
        "& { " + user_command_utf8 + " } 2>&1 | Out-String -Stream | Write-Output\r\n"
        "if ($LASTEXITCODE -ne $null) { exit $LASTEXITCODE } else { if ($?) { exit 0 } else { exit 1 } }\r\n";

    const std::wstring wide = Utf8ToWide(script_utf8);
    return Base64Encode(reinterpret_cast<const unsigned char*>(wide.data()), wide.size() * sizeof(wchar_t));
}

// 命令原样交给 cmd 执行,不做代码页预处理(chcp 那条路实测对重定向管道
// 不起作用,见 AcpBytesToUtf8 的注释)——输出捕获回来以后统一按 CP_ACP
// 解码转 UTF-8。
std::wstring BuildCmdCommandLine(const std::string& user_command_utf8) {
    const std::wstring wide_script = Utf8ToWide(user_command_utf8);
    // cmd /d /s /c "<script>":/d 不跑注册表 AutoRun 项,/s 让外层这对引号
    // 整体当"一段"解析、不逐个匹配内部引号——这是拿 cmd 跑任意命令行
    // (命令里本身可能也带引号)的标准写法。
    return L"cmd.exe /d /s /c \"" + wide_script + L"\"";
}

struct ProcResult {
    std::string output;
    DWORD exit_code = 0;
    bool timed_out = false;
    bool spawn_failed = false;
    std::string spawn_error;
};

// 起一个子进程(cmdline 是完整的"可执行文件 + 参数"命令行,调用方拼好),
// 合并捕获 stdout/stderr,超时就连同它派生出的子子进程一起杀掉(靠 Job
// Object 的 KILL_ON_JOB_CLOSE)。PowerShell 路径、cmd 路径共用这一份实现,
// 区别只在调用方怎么拼 cmdline。
ProcResult RunChildProcess(const std::wstring& cmdline, int timeout_ms) {
    ProcResult result;

    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;
    sa.lpSecurityDescriptor = nullptr;

    HANDLE read_pipe = nullptr;
    HANDLE write_pipe = nullptr;
    if (!CreatePipe(&read_pipe, &write_pipe, &sa, 0)) {
        result.spawn_failed = true;
        result.spawn_error = "创建管道失败(错误码 " + std::to_string(GetLastError()) + ")";
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
    // 不会漏杀(比如 run_command 跑 `ping` 这种会起孙进程的命令)。
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

}  // namespace

Tool::Result RunCommandTool::execute(const nlohmann::json& input) {
    if (!input.contains("command") || !input.at("command").is_string()) {
        return {"缺少必填参数 command(字符串)", true};
    }
    const std::string command = input.at("command").get<std::string>();
    if (command.empty()) {
        return {"command 不能是空字符串", true};
    }

    int timeout_ms = kDefaultTimeoutMs;
    if (auto it = input.find("timeout_ms"); it != input.end() && !it->is_null()) {
        timeout_ms = it->get<int>();
        if (timeout_ms <= 0) {
            timeout_ms = kDefaultTimeoutMs;
        }
    }

    std::string shell = "powershell";
    if (auto it = input.find("shell"); it != input.end() && !it->is_null()) {
        if (!it->is_string()) {
            return {"shell 参数必须是字符串", true};
        }
        shell = it->get<std::string>();
        if (shell != "powershell" && shell != "cmd") {
            return {"shell 参数只认得 powershell 或 cmd,写的是: " + shell, true};
        }
    }

    const bool is_cmd = (shell == "cmd");
    const std::wstring cmdline = is_cmd ? BuildCmdCommandLine(command)
                                          : (L"powershell.exe -NoProfile -NonInteractive -EncodedCommand " +
                                             Utf8ToWide(BuildEncodedCommand(command)));
    ProcResult proc = RunChildProcess(cmdline, timeout_ms);
    // cmd.exe 走的是系统 ANSI 代码页(国内机器上是 GBK)往管道里写字节,
    // 跟 PowerShell 路径(脚本里显式设了 [Console]::OutputEncoding=UTF8)
    // 不一样,这里捕获回来的原始字节要单独转一道才是合法 UTF-8。
    if (is_cmd) {
        proc.output = AcpBytesToUtf8(proc.output);
    }

    if (proc.spawn_failed) {
        return {proc.spawn_error, true};
    }
    if (proc.timed_out) {
        std::ostringstream oss;
        oss << "命令执行超时(超过 " << timeout_ms << " 毫秒),已强制终止。\n";
        if (!proc.output.empty()) {
            oss << "终止前捕获到的输出:\n" << proc.output;
        }
        return {oss.str(), true};
    }

    std::ostringstream oss;
    oss << "[退出码 " << proc.exit_code << "]\n" << proc.output;
    return {oss.str(), proc.exit_code != 0};
}

#else  // !_WIN32

Tool::Result RunCommandTool::execute(const nlohmann::json&) {
    return {"run_command 眼下只实现了 Windows(经 PowerShell 执行)", true};
}

#endif

}  // namespace lubancode::tools
