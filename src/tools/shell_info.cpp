#include "tools/shell_info.hpp"

#include <cstdlib>

#include "platform/process.hpp"
#include "platform/text_encoding.hpp"

namespace lubancode::tools {

namespace {

// 跑一条探测命令,拿合并输出(限长 2KB 够看版本串)。失败返回空串。
std::string ProbeOutput(const std::vector<std::string>& argv) {
    const auto result = platform::RunProcess(argv, /*timeout_ms=*/10000, {}, 2048);
    if (result.spawn_failed || result.timed_out) {
        return std::string();
    }
    return platform::SanitizeUtf8(result.output);
}

std::string FirstMeaningfulLine(const std::string& text) {
    // 版本串常混着横幅空行,取第一行有字母数字的。
    std::string line;
    std::size_t start = 0;
    while (start <= text.size()) {
        const std::size_t nl = text.find('\n', start);
        line = text.substr(start, nl == std::string::npos ? std::string::npos : nl - start);
        bool has_alnum = false;
        for (const char c : line) {
            if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')) {
                has_alnum = true;
                break;
            }
        }
        if (has_alnum) {
            // 掐头去尾的空白。
            const std::size_t b = line.find_first_not_of(" \t\r");
            const std::size_t e = line.find_last_not_of(" \t\r");
            return b == std::string::npos ? std::string() : line.substr(b, e - b + 1);
        }
        if (nl == std::string::npos) {
            break;
        }
        start = nl + 1;
    }
    return std::string();
}

}  // namespace

std::vector<ShellReport> ProbeShells() {
    std::vector<ShellReport> reports;
#ifdef _WIN32
    {
        ShellReport ps;
        ps.id = "powershell";
        ps.executable = "powershell.exe";
        ps.version = FirstMeaningfulLine(ProbeOutput({"powershell.exe", "-NoProfile", "-NonInteractive", "-Command",
                                                      "$PSVersionTable.PSVersion.ToString()"}));
        ps.profile_loaded = false;  // -NoProfile
        ps.notes = "Windows PowerShell 5.1 系(不是 PowerShell 7 的 pwsh.exe);-NoProfile 不加载用户 "
                   "profile,alias/函数/rc 一概不见;PATH 与启动 LubanCode 的父进程一致。";
        reports.push_back(std::move(ps));
    }
    {
        ShellReport cmd;
        cmd.id = "cmd";
        cmd.executable = "cmd.exe";
        cmd.version = FirstMeaningfulLine(ProbeOutput({"cmd.exe", "/d", "/c", "ver"}));
        cmd.profile_loaded = false;  // /d 跳过 AutoRun
        cmd.notes = "cmd.exe;/d 跳过注册表 AutoRun 项;输出走系统 OEM/ANSI 代码页,宿主侧转 UTF-8。";
        reports.push_back(std::move(cmd));
    }
#else
    {
        ShellReport sh;
        sh.id = "sh";
        sh.executable = "/bin/sh";
        sh.version = FirstMeaningfulLine(ProbeOutput({"/bin/sh", "-c", "--", "\"$0\" 2>/dev/null"}));
        // /bin/sh 读不出来就看看它指向谁(Ubuntu 常见 dash)。
        if (sh.version.empty()) {
            const std::string link = ProbeOutput({"ls", "-l", "/bin/sh"});
            if (link.find("dash") != std::string::npos) {
                sh.version = "/bin/sh -> dash";
            } else if (link.find("bash") != std::string::npos) {
                sh.version = "/bin/sh -> bash";
            } else if (!link.empty()) {
                sh.version = FirstMeaningfulLine(link);
            }
        }
        sh.login_shell = false;
        sh.profile_loaded = false;  // /bin/sh -c 非交互非 login,不读 profile/rc
        sh.notes = "/bin/sh 不保证是 Bash(Ubuntu 常见 dash);Bash 数组、[[ ]]、source 不能想当然;"
                   "非交互非 login,不读 profile/rc 文件;'我终端里能跑'不等于代理里能跑。";
        reports.push_back(std::move(sh));
    }
#endif
    // stdin/stdout 的 TTY 语义对工具路径恒定:stdin 接 NUL//dev/null,
    // stdout/stderr 是管道不是终端。sudo/ssh 密码/交互安装器/全屏程序/
    // 依赖 TTY 探测的 CLI 会失败或挂住——这是设计(无头工具),不是 bug。
    for (auto& r : reports) {
        r.stdin_is_tty = false;
        r.stdout_is_tty = false;
    }
    return reports;
}

}  // namespace lubancode::tools
