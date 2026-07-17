#include "tools/run_command.hpp"

#include <sstream>
#include <string>

#include "tools/process_exec.hpp"

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

// Utf8ToWide/WideToUtf8/AcpBytesToUtf8/BuildCmdCommandLine/RunProcess 这几个
// 都挪到 tools/process_exec.hpp 里了(M9:hooks 系统也要起子进程,抽出来
// 两边共用,见该文件头注释)。这里只留 PowerShell 专属的 base64/编码命令
// 拼接——hooks 不走 PowerShell 这条路,不需要共用。

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
    ProcessResult proc = RunProcess(cmdline, timeout_ms);
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
