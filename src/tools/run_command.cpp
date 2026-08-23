#include "tools/run_command.hpp"

#include <cstdint>
#include <cstring>
#include <filesystem>
#include <sstream>
#include <string>
#include <system_error>

#include "platform/process.hpp"
#include "tools/background_tasks.hpp"  // BackgroundTaskRegistry:后台模式登记 task_id + 起 watcher 探活
#include "platform/text_encoding.hpp"  // SanitizeUtf8:捕获侧治本,见 execute() 里的调用点注释
#include "tools/command_safety.hpp"    // 隔离的 git 改道闸
#include "tools/isolation.hpp"
#include "tools/path_utils.hpp"
#include "tools/tool_text.hpp"  // 模型可见文案(描述/参数说明)查表,源头 prompts/tools/

#ifdef _WIN32
#include "platform/paths.hpp"  // Utf8ToWide:PowerShell -EncodedCommand 拼接用
#endif

namespace lubancode::tools {

namespace {

constexpr int kDefaultTimeoutMs = 120000;
// timeout_ms 的合法上限(int 全域放进来没意义:Windows 的 DWORD 等得起,
// 会话也早没了)。超大值体面报错,不窄化走样。
constexpr int kMaxTimeoutMs = 86400000;  // 24 小时

// 把"命令跑在哪个目录"翻译进各 shell 的命令串:起手先 cd 到位,再跑原命令。
// 隔离子代理靠这层把自己的命令钉在自己的房里(进程 cwd 全进程一份,不能
// chdir);主代理也可显式传 cwd。引号各按各家语法转义。
std::string QuotePowerShellSingle(const std::string& path) {
    std::string out = path;
    for (std::size_t pos = 0; pos < out.size(); ++pos) {
        if (out[pos] == '\'') {
            out.insert(pos, 1, '\'');
            ++pos;
        }
    }
    return out;
}

std::string QuotePosixSingle(const std::string& path) {
    // POSIX sh 的单引号里没有转义这一说:翻倍两枚单引号会被解成两枚字面
    // 单引号,串就拆坏了。严谨写法是闭合、插入一枚转义过的单引号、再打开:
    //   'it' -> '\''  (即: '...' + "'" + '...')
    std::string out = "'";
    for (const char c : path) {
        if (c == '\'') {
            out += "'\\''";  // 闭单引号 + 转义单引号 + 开单引号
        } else {
            out.push_back(c);
        }
    }
    out += "'";
    return out;
}

std::string ApplyWorkingDirectory(const std::string& command, const std::string& cwd_utf8,
                                  const std::string& shell) {
    if (cwd_utf8.empty()) {
        return command;
    }
    if (shell == "cmd") {
        return "cd /d \"" + cwd_utf8 + "\" && " + command;
    }
    if (shell == "sh") {
        // QuotePosixSingle 自己带首尾单引号(见它内部对单引号的处理)。
        return "cd -- " + QuotePosixSingle(cwd_utf8) + " && " + command;
    }
    // PowerShell:Set-Location 在 & { ... } 外面先跑,后面的命令都在新目录里。
    return "Set-Location -LiteralPath '" + QuotePowerShellSingle(cwd_utf8) + "' ; " + command;
}

}  // namespace

std::string RunCommandTool::name() const {
    return "run_command";
}

std::string RunCommandTool::description() const {
    // 文案在 src/prompts/tools/<语言>/run_command.md,兜底是迁移前的原文。
    // 文件工具批的成例是"键名直接写死、cpp 不留非迁键字面量";这里描述与
    // command/shell 两个参数的平台分档键(POSIX 节)只在非 Windows 用到,
    // 键名随 #ifdef 走,Windows 下不查也不留 POSIX 兜底——查表键不属于
    // "给模型看的文案",不留在 cpp 里。
#ifdef _WIN32
    return ToolText("run_command", "description",
                    "在 shell 里执行一条命令,拿到合并后的标准输出/标准错误,以及退出码。"
                    "shell 参数可选 powershell(默认)或 cmd,分别按对应语法写命令。执行前要经用户确认。"
                    "超时会被强制杀掉。"
                    "起 dev server、watch 进程这类要跨命令、跨调用存活的长命进程,或者想后台跑完不阻塞对话的短任务,"
                    "传 run_in_background=true:不等它跑完,spawn 成功立刻返回 task_id、PID 和日志文件路径;"
                    "命令跑完时下一次给提示符会打一行完成通知。之后用 background_output 工具(传 task_id)"
                    "查状态/读输出,stop_background 工具收尾。");
#else
    return ToolText("run_command", "description (POSIX)",
                    "在 shell(/bin/sh)里执行一条命令,拿到合并后的标准输出/标准错误,以及退出码。"
                    "按 POSIX sh 语法写命令。执行前要经用户确认。超时会被强制杀掉。"
                    "起 dev server、watch 进程这类要跨命令、跨调用存活的长命进程,或者想后台跑完不阻塞对话的短任务,"
                    "传 run_in_background=true:不等它跑完,spawn 成功立刻返回 task_id、PID 和日志文件路径;"
                    "命令跑完时下一次给提示符会打一行完成通知。之后用 background_output 工具(传 task_id)"
                    "查状态/读输出,stop_background 工具收尾。");
#endif
}

nlohmann::json RunCommandTool::input_schema() const {
    nlohmann::json schema = nlohmann::json::object();
    schema["type"] = "object";

    nlohmann::json properties = nlohmann::json::object();

    nlohmann::json command_prop = nlohmann::json::object();
    command_prop["type"] = "string";
#ifdef _WIN32
    command_prop["description"] =
        ToolText("run_command", "param.command", "要执行的命令,按所选 shell 的语法写(默认 PowerShell 语法)");
#else
    command_prop["description"] =
        ToolText("run_command", "param.command (POSIX)", "要执行的命令,按 POSIX sh 语法写");
#endif
    properties["command"] = command_prop;

    nlohmann::json timeout_prop = nlohmann::json::object();
    timeout_prop["type"] = "integer";
    timeout_prop["description"] =
        ToolText("run_command", "param.timeout_ms", "超时时间,单位毫秒,不填默认 120000(2 分钟)");
    properties["timeout_ms"] = timeout_prop;

    nlohmann::json shell_prop = nlohmann::json::object();
    shell_prop["type"] = "string";
#ifdef _WIN32
    shell_prop["enum"] = nlohmann::json::array({"powershell", "cmd"});
    shell_prop["description"] = ToolText("run_command", "param.shell", "用哪个 shell 执行,不填默认 powershell");
#else
    shell_prop["enum"] = nlohmann::json::array({"sh"});
    shell_prop["description"] =
        ToolText("run_command", "param.shell (POSIX)",
                 "用哪个 shell 执行,本平台只有 sh(/bin/sh);powershell/cmd 是 Windows 专属");
#endif
    properties["shell"] = shell_prop;

    nlohmann::json background_prop = nlohmann::json::object();
    background_prop["type"] = "boolean";
    // 两个平台这段一字不差,只留一份档(不分 POSIX 键),兜底也只带一份。
    background_prop["description"] =
        ToolText("run_command", "param.run_in_background",
                 "true = 后台运行,不等命令跑完就返回。用于起 dev server、watch 进程这类要跨命令、"
                 "跨多轮调用继续存活的长命进程——起完之后你还要接着用别的命令(比如 curl)去验证它;"
                 "也用于后台跑一个短任务,不想阻塞当前对话、跑完通知你即可。"
                 "spawn 成功后立刻返回结果,内含 task_id、子进程 PID 和一个日志文件路径(该进程的标准"
                 "输出/标准错误合并写在这个文件里);命令跑完时,下一次给提示符会打一行完成通知。"
                 "之后想看它是否还活着、看它吐了什么,用 background_output 工具(传 task_id)查状态读输出,"
                 "要收掉它就用 stop_background 工具。"
                 "timeout_ms 参数对这个模式没有意义,会被忽略。不填默认 false(前台执行,等命令跑完拿"
                 "完整输出和退出码)。");
    properties["run_in_background"] = background_prop;

    nlohmann::json cwd_prop = nlohmann::json::object();
    cwd_prop["type"] = "string";
    cwd_prop["description"] =
        ToolText("run_command", "param.cwd",
                 "命令的工作目录,相对或绝对均可;不填用当前会话工作目录。目录必须真实存在。"
                 "住隔离 worktree 的会话里,指向主 checkout 的目录会被拒绝");
    properties["cwd"] = cwd_prop;

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
    // JSON 字符串可含 NUL;C API 的命令行在 NUL 处截断,界面看到的是全串、
    // 系统执行的却是前半串。一律拒绝,不猜。
    if (command.find('\0') != std::string::npos) {
        return {"command 里带 NUL 字符,系统命令行会在 NUL 处截断,拒绝执行", true};
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
            // 模型偶尔会把数字发成字符串/数组;直接 get<int>() 遇到超范围的
            // JSON integer 会抛异常穿透出去。先按 64 位取,再做范围检查,
            // 超出 int 的体面报错并写明允许区间。
            if (!it->is_number_integer()) {
                return {"timeout_ms 得是整数(毫秒)", true};
            }
            const std::int64_t raw = it->get<std::int64_t>();
            if (raw < 1 || raw > static_cast<std::int64_t>(kMaxTimeoutMs)) {
                return {"timeout_ms 得在 1~" + std::to_string(kMaxTimeoutMs) + " 毫秒之间(写的是 " +
                            std::to_string(raw) + ")",
                        true};
            }
            timeout_ms = static_cast<int>(raw);
        }
    }

    // 工作目录(0.27.x):不填用当前会话工作目录。shell 先取个值(完整的
    // 合法性校验在各平台分支里做),隔离两道闸要用。
#ifdef _WIN32
    std::string shell_value = "powershell";
#else
    std::string shell_value = "sh";
#endif
    if (auto it = input.find("shell"); it != input.end() && it->is_string()) {
        shell_value = it->get<std::string>();
    }

    std::string effective_cwd;
    if (auto it = input.find("cwd"); it != input.end() && !it->is_null()) {
        if (!it->is_string()) {
            return {"cwd 得是字符串(目录路径)", true};
        }
        effective_cwd = it->get<std::string>();
        if (effective_cwd.find('\0') != std::string::npos) {
            return {"cwd 里带 NUL 字符,系统路径会在 NUL 处截断,拒绝执行", true};
        }
    }

    // 隔离的 cwd 闸 + git 改道闸:住在 worktree 房里的会话/子代理,命令不得
    // 绕回主 checkout。
    const IsolationScope* isolation = IsolationGuard::Current();
    if (!effective_cwd.empty()) {
        std::filesystem::path cwd_path = Utf8ToPath(effective_cwd);
        if (cwd_path.is_relative()) {
            const std::filesystem::path base = isolation != nullptr
                                                   ? Utf8ToPath(isolation->base_dir)
                                                   : std::filesystem::current_path();
            cwd_path = base / cwd_path;
        }
        std::error_code cwd_ec;
        if (!std::filesystem::is_directory(cwd_path, cwd_ec)) {
            return {"cwd 不是真实存在的目录: " + effective_cwd, true};
        }
        effective_cwd = PathToUtf8(cwd_path);
    }
    if (isolation != nullptr) {
        if (!effective_cwd.empty() && PathBlockedByIsolation(effective_cwd, *isolation)) {
            return {"[隔离] 命令工作目录指回主 checkout,已拦: " + effective_cwd +
                        "(会话住在 worktree " + isolation->name + " 里)。请在房内跑命令。",
                    true};
        }
        if (auto violation = FindIsolationGitRedirect(command, shell_value, *isolation);
            violation.has_value()) {
            return {"[隔离] " + *violation, true};
        }
    }
    // 起手 cd 到位,再跑原命令(不传 cwd 时是原样透传)。
    const std::string command_with_cwd = ApplyWorkingDirectory(command, effective_cwd, shell_value);

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
            bg = platform::RunShellCommandBackground(command_with_cwd);
        } else {
            // 止血(P1):后台 PowerShell 此前编码的是原始 command,用户传的
            // cwd 被悄悄丢掉,执行落回宿主目录。前台编码的可是 command_with_cwd,
            // 后台得跟它一致。
            const std::wstring cmdline = L"powershell.exe -NoProfile -NonInteractive -EncodedCommand " +
                                          platform::Utf8ToWide(BuildEncodedCommand(command_with_cwd));
            bg = platform::RunProcessBackground(cmdline);
        }
        if (!bg.success) {
            return {bg.error, true};
        }
        // 登记进后台任务台账:拿一个 task_id,起 watcher 线程探活——
        // 完成时主循环会收到通知,模型也能用 background_output 工具查输出。
        const std::string task_id =
            BackgroundTaskRegistry::Instance().Register(command, shell, bg.pid, bg.log_path);
        std::ostringstream oss;
        oss << "已在后台启动(task #" << task_id << ", PID " << bg.pid << "),不等它跑完。\n"
            << "日志文件: " << bg.log_path << "\n"
            << "查状态/输出: 用 background_output 工具(传 task_id=" << task_id << ")。\n"
            << "收掉它: 用 stop_background 工具,或 Stop-Process -Id " << bg.pid << " -Force";
        return {oss.str(), false};
    }

    platform::ProcessResult proc;
    if (is_cmd) {
        // RunShellCommand 内部已把 cmd.exe 的系统 ANSI 代码页输出转成 UTF-8
        // (跟 PowerShell 路径不一样,那边脚本里显式设了
        // [Console]::OutputEncoding=UTF8),这里拿到手就是合法 UTF-8。
        proc = platform::RunShellCommand(command_with_cwd, timeout_ms);
    } else {
        const std::wstring cmdline = L"powershell.exe -NoProfile -NonInteractive -EncodedCommand " +
                                      platform::Utf8ToWide(BuildEncodedCommand(command_with_cwd));
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
        const platform::BackgroundSpawnResult bg = platform::RunShellCommandBackground(command_with_cwd);
        if (!bg.success) {
            return {bg.error, true};
        }
        // 登记进后台任务台账(同 Windows 分支):task_id + watcher 探活,完成时通知。
        const std::string task_id =
            BackgroundTaskRegistry::Instance().Register(command, "sh", bg.pid, bg.log_path);
        std::ostringstream oss;
        oss << "已在后台启动(task #" << task_id << ", PID " << bg.pid << "),不等它跑完。\n"
            << "日志文件: " << bg.log_path << "\n"
            << "查状态/输出: 用 background_output 工具(传 task_id=" << task_id << ")。\n"
            << "收掉它: 用 stop_background 工具,或 kill " << bg.pid << "(不行就 kill -9 " << bg.pid << ")";
        return {oss.str(), false};
    }

    platform::ProcessResult proc = platform::RunShellCommand(command_with_cwd, timeout_ms);
#endif

    // 捕获侧治本:cmd 分支已经在 platform 层按 CP_ACP 转过一遍,理论上到手
    // 就是合法 UTF-8;但 PowerShell 分支只有脚本真正跑到
    // [Console]::OutputEncoding=UTF8 那一行之后才靠得住——脚本解析期报错
    // (比如用户命令里带 PS 5.1 不认的 `&&`)会绕过那一行,直接吐系统 ANSI
    // 代码页字节。不管走哪条分支,拿到手先原地清洗一遍,保证 proc.output
    // 从这里往后一定是合法 UTF-8,不会把坏字节带进 Tool::Result 再带进
    // 对话历史。
    proc.output = platform::SanitizeUtf8(proc.output);

    // 逐枚追踪单:稳定 outcome/error_code 不靠中文正文分辨(spawn/超时/
    // 非零退出/输出超限各自有码;人话照旧给模型)。
    if (proc.spawn_failed) {
        Tool::Result spawn{proc.spawn_error, true};
        spawn.outcome = "spawn_failed";
        spawn.error_code = "process.spawn_failed";
        return spawn;
    }
    if (proc.timed_out) {
        std::ostringstream oss;
        oss << "命令执行超时(超过 " << timeout_ms << " 毫秒),已强制终止。\n";
        if (!proc.output.empty()) {
            oss << "终止前捕获到的输出:\n" << proc.output;
        }
        Tool::Result timed{oss.str(), true};
        timed.outcome = "timed_out";
        timed.error_code = "process.timeout";
        return timed;
    }
    if (proc.output_truncated) {
        std::ostringstream oss;
        oss << "[输出超过上限(2MB)已截断,命令已被强制终止。以下是截断前捕获到的输出]\n" << proc.output;
        // 单子的硬话:输出超限 = 命令被杀,结果不是成功。Agent Loop、workflow
        // recorder、UI 只看布尔值时也不许把半截构建当成功。
        Tool::Result truncated{oss.str(), true};
        truncated.outcome = "output_limit";
        truncated.error_code = "process.output_limit";
        return truncated;
    }

    std::ostringstream oss;
    oss << "[退出码 " << proc.exit_code << "]\n" << proc.output;
    Tool::Result exited{oss.str(), proc.exit_code != 0};
    if (proc.exit_code != 0) {
        exited.outcome = "process_exit_nonzero";
        exited.error_code = "process.exit_nonzero";
    } else {
        exited.outcome = "succeeded";
    }
    exited.details = nlohmann::json{{"exit_code", proc.exit_code}};
    exited.effect_summary = "run (exit=" + std::to_string(proc.exit_code) + ")";
    return exited;
}

}  // namespace lubancode::tools
