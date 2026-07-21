#include "tools/run_command.hpp"

#include <sstream>
#include <string>

#include "platform/process.hpp"
#include "platform/text_encoding.hpp"  // SanitizeUtf8:捕获侧治本,见 execute() 里的调用点注释

#ifdef _WIN32
#include "platform/paths.hpp"  // Utf8ToWide:PowerShell -EncodedCommand 拼接用
#endif

namespace lubancode::tools {

namespace {

constexpr int kDefaultTimeoutMs = 120000;

}  // namespace

std::string RunCommandTool::name() const {
    return "run_command";
}

std::string RunCommandTool::description() const {
#ifdef _WIN32
    return "在 shell 里执行一条命令,拿到合并后的标准输出/标准错误,以及退出码。"
           "shell 参数可选 powershell(默认)或 cmd,分别按对应语法写命令。执行前要经用户确认。"
           "超时会被强制杀掉。"
           "起 dev server、watch 进程这类要跨命令、跨调用存活的长命进程,传 run_in_background=true:"
           "不等它跑完,spawn 成功立刻返回 PID 和日志文件路径,之后用普通命令探活/看日志/收尾。";
#else
    return "在 shell(/bin/sh)里执行一条命令,拿到合并后的标准输出/标准错误,以及退出码。"
           "按 POSIX sh 语法写命令。执行前要经用户确认。超时会被强制杀掉。"
           "起 dev server、watch 进程这类要跨命令、跨调用存活的长命进程,传 run_in_background=true:"
           "不等它跑完,spawn 成功立刻返回 PID 和日志文件路径,之后用普通命令探活/看日志/收尾。";
#endif
}

nlohmann::json RunCommandTool::input_schema() const {
    nlohmann::json schema = nlohmann::json::object();
    schema["type"] = "object";

    nlohmann::json properties = nlohmann::json::object();

    nlohmann::json command_prop = nlohmann::json::object();
    command_prop["type"] = "string";
#ifdef _WIN32
    command_prop["description"] = "要执行的命令,按所选 shell 的语法写(默认 PowerShell 语法)";
#else
    command_prop["description"] = "要执行的命令,按 POSIX sh 语法写";
#endif
    properties["command"] = command_prop;

    nlohmann::json timeout_prop = nlohmann::json::object();
    timeout_prop["type"] = "integer";
    timeout_prop["description"] = "超时时间,单位毫秒,不填默认 120000(2 分钟)";
    properties["timeout_ms"] = timeout_prop;

    nlohmann::json shell_prop = nlohmann::json::object();
    shell_prop["type"] = "string";
#ifdef _WIN32
    shell_prop["enum"] = nlohmann::json::array({"powershell", "cmd"});
    shell_prop["description"] = "用哪个 shell 执行,不填默认 powershell";
#else
    shell_prop["enum"] = nlohmann::json::array({"sh"});
    shell_prop["description"] = "用哪个 shell 执行,本平台只有 sh(/bin/sh);powershell/cmd 是 Windows 专属";
#endif
    properties["shell"] = shell_prop;

    nlohmann::json background_prop = nlohmann::json::object();
    background_prop["type"] = "boolean";
#ifdef _WIN32
    background_prop["description"] =
        "true = 后台运行,不等命令跑完就返回。用于起 dev server、watch 进程这类要跨命令、"
        "跨多轮调用继续存活的长命进程——起完之后你还要接着用别的命令(比如 curl)去验证它。"
        "spawn 成功后立刻返回结果,内含子进程 PID 和一个日志文件路径(该进程的标准输出/"
        "标准错误合并写在这个文件里);之后想看它是否还活着、看它吐了什么,用普通 run_command "
        "跑 Get-Content -Tail 之类的命令读日志文件,要收掉它就 Stop-Process -Id <PID> -Force。"
        "timeout_ms 参数对这个模式没有意义,会被忽略。不填默认 false(前台执行,等命令跑完拿"
        "完整输出和退出码)。";
#else
    background_prop["description"] =
        "true = 后台运行,不等命令跑完就返回。用于起 dev server、watch 进程这类要跨命令、"
        "跨多轮调用继续存活的长命进程——起完之后你还要接着用别的命令(比如 curl)去验证它。"
        "spawn 成功后立刻返回结果,内含子进程 PID 和一个日志文件路径(该进程的标准输出/"
        "标准错误合并写在这个文件里);之后想看它是否还活着、看它吐了什么,用普通 run_command "
        "跑 tail 之类的命令读日志文件,要收掉它就 kill <PID>。"
        "timeout_ms 参数对这个模式没有意义,会被忽略。不填默认 false(前台执行,等命令跑完拿"
        "完整输出和退出码)。";
#endif
    properties["run_in_background"] = background_prop;

    schema["properties"] = properties;
    schema["required"] = nlohmann::json::array({"command"});

    return schema;
}

#ifdef _WIN32

namespace {

// PowerShell 专属的 base64/编码命令拼接。进程执行的公共基建(RunProcess/
// BuildCmdCommandLine/编码转换)在 platform/process.hpp、platform/paths.hpp
// (跨平台单从 tools/process_exec.hpp 搬的家,见那两个文件头注释)。

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

    const std::wstring wide = platform::Utf8ToWide(script_utf8);
    return Base64Encode(reinterpret_cast<const unsigned char*>(wide.data()), wide.size() * sizeof(wchar_t));
}

}  // namespace

#endif  // _WIN32

Tool::Result RunCommandTool::execute(const nlohmann::json& input) {
    if (!input.contains("command") || !input.at("command").is_string()) {
        return {"缺少必填参数 command(字符串)", true};
    }
    const std::string command = input.at("command").get<std::string>();
    if (command.empty()) {
        return {"command 不能是空字符串", true};
    }

    bool run_in_background = false;
    if (auto it = input.find("run_in_background"); it != input.end() && !it->is_null()) {
        if (!it->is_boolean()) {
            return {"run_in_background 参数必须是布尔值", true};
        }
        run_in_background = it->get<bool>();
    }

    // timeout_ms 对后台模式没有意义(spawn 完就返回,根本不等),背景模式下
    // 干脆不解析——模型哪怕手滑传了个非法值也不该在这条不使用它的路径上
    // 报错,忽略得彻底一点。
    int timeout_ms = kDefaultTimeoutMs;
    if (!run_in_background) {
        if (auto it = input.find("timeout_ms"); it != input.end() && !it->is_null()) {
            // 模型偶尔会把数字发成字符串/数组,直接 get<int>() 会抛异常穿透出去,
            // 这里先验类型,不合就体面报错。
            if (!it->is_number_integer()) {
                return {"timeout_ms 得是整数(毫秒)", true};
            }
            timeout_ms = it->get<int>();
            if (timeout_ms <= 0) {
                timeout_ms = kDefaultTimeoutMs;
            }
        }
    }

#ifdef _WIN32
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

    if (run_in_background) {
        platform::BackgroundSpawnResult bg;
        if (is_cmd) {
            bg = platform::RunShellCommandBackground(command);
        } else {
            const std::wstring cmdline = L"powershell.exe -NoProfile -NonInteractive -EncodedCommand " +
                                          platform::Utf8ToWide(BuildEncodedCommand(command));
            bg = platform::RunProcessBackground(cmdline);
        }
        if (!bg.success) {
            return {bg.error, true};
        }
        std::ostringstream oss;
        oss << "已在后台启动(PID " << bg.pid << "),不等它跑完,以上是 spawn 结果。\n"
            << "日志文件: " << bg.log_path << "\n"
            << "看日志: Get-Content -Tail 50 \"" << bg.log_path << "\"(加 -Wait 实时跟随,Ctrl+C 退出)。\n"
            << "收掉它: Stop-Process -Id " << bg.pid << " -Force";
        return {oss.str(), false};
    }

    platform::ProcessResult proc;
    if (is_cmd) {
        // RunShellCommand 内部已把 cmd.exe 的系统 ANSI 代码页输出转成 UTF-8
        // (跟 PowerShell 路径不一样,那边脚本里显式设了
        // [Console]::OutputEncoding=UTF8),这里拿到手就是合法 UTF-8。
        proc = platform::RunShellCommand(command, timeout_ms);
    } else {
        const std::wstring cmdline = L"powershell.exe -NoProfile -NonInteractive -EncodedCommand " +
                                      platform::Utf8ToWide(BuildEncodedCommand(command));
        proc = platform::RunProcess(cmdline, timeout_ms);
    }
#else
    // POSIX:shell 一律 /bin/sh -c。powershell/cmd 是 Windows 专属选项,
    // 模型带着旧习惯传过来就明说不支持,别悄悄换 shell 让语义走样。
    if (auto it = input.find("shell"); it != input.end() && !it->is_null()) {
        if (!it->is_string()) {
            return {"shell 参数必须是字符串", true};
        }
        const std::string shell = it->get<std::string>();
        if (shell == "powershell" || shell == "cmd") {
            return {"shell=" + shell + " 是 Windows 专属,本平台请用 sh(/bin/sh)", true};
        }
        if (shell != "sh") {
            return {"shell 参数只认得 sh,写的是: " + shell, true};
        }
    }

    if (run_in_background) {
        const platform::BackgroundSpawnResult bg = platform::RunShellCommandBackground(command);
        if (!bg.success) {
            return {bg.error, true};
        }
        std::ostringstream oss;
        oss << "已在后台启动(PID " << bg.pid << "),不等它跑完,以上是 spawn 结果。\n"
            << "日志文件: " << bg.log_path << "\n"
            << "看日志: tail -n 50 \"" << bg.log_path << "\"(加 -f 实时跟随,Ctrl+C 退出)。\n"
            << "收掉它: kill " << bg.pid << "(不行就 kill -9 " << bg.pid << ")";
        return {oss.str(), false};
    }

    platform::ProcessResult proc = platform::RunShellCommand(command, timeout_ms);
#endif

    // 捕获侧治本:cmd 分支已经在 platform 层按 CP_ACP 转过一遍,理论上到手
    // 就是合法 UTF-8;但 PowerShell 分支只有脚本真正跑到
    // [Console]::OutputEncoding=UTF8 那一行之后才靠得住——脚本解析期报错
    // (比如用户命令里带 PS 5.1 不认的 `&&`)会绕过那一行,直接吐系统 ANSI
    // 代码页字节。不管走哪条分支,拿到手先原地清洗一遍,保证 proc.output
    // 从这里往后一定是合法 UTF-8,不会把坏字节带进 Tool::Result 再带进
    // 对话历史。
    proc.output = platform::SanitizeUtf8(proc.output);

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
    if (proc.output_truncated) {
        std::ostringstream oss;
        oss << "[输出超过上限(2MB)已截断,命令已被强制终止。以下是截断前捕获到的输出]\n" << proc.output;
        return {oss.str(), false};
    }

    std::ostringstream oss;
    oss << "[退出码 " << proc.exit_code << "]\n" << proc.output;
    return {oss.str(), proc.exit_code != 0};
}

}  // namespace lubancode::tools
