#include "tools/run_command.hpp"

#include <algorithm>
#include <chrono>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <iterator>
#include <optional>
#include <sstream>
#include <string>
#include <system_error>

#ifndef _WIN32
#include <unistd.h>  // access(X_OK):PATH 里探 bash 用
#endif

#include "platform/paths.hpp"          // GetEnvVar:环境变量读取的唯一口(审计 P3 候选并口);Windows 另有 Utf8ToWide
#include "platform/process.hpp"
#include "tools/background_tasks.hpp"  // BackgroundTaskRegistry:后台模式登记 task_id + 起 watcher 探活
#include "platform/text_encoding.hpp"  // SanitizeUtf8:捕获侧治本,见 execute() 里的调用点注释
#include "tools/command_safety.hpp"    // 隔离的 git 改道闸
#include "tools/isolation.hpp"
#include "tools/path_utils.hpp"
#include "tools/tool_text.hpp"  // 模型可见文案(描述/参数说明)查表,源头 prompts/tools/

#include <atomic>

#ifdef _WIN32
#include <span>

#include "platform/base64.hpp"  // Base64Encode:-EncodedCommand 的公共内核(审计 P2)
#endif

namespace lubancode::tools {

namespace {

constexpr int kDefaultTimeoutMs = 120000;
constexpr int kBuildTimeoutMs = 900000;  // 构建/configure/装依赖/测试类默认 15 分钟

// 命令开头切词(只切到够判首词为止):空白分词,单双引号内的空白不断词、
// 引号本身剥掉。反斜杠不当转义——Windows 不带引号的 C:\tools\cmake.exe
// 得原样留着给下面剥路径。不做变量展开,不认重定向,够判首词即可。
std::vector<std::string> LeadingWords(const std::string& command, std::size_t max_words) {
    std::vector<std::string> words;
    std::size_t i = 0;
    const std::size_t n = command.size();
    while (i < n && words.size() < max_words) {
        while (i < n && std::isspace(static_cast<unsigned char>(command[i])) != 0) {
            ++i;
        }
        if (i >= n) {
            break;
        }
        std::string word;
        char quote = '\0';
        for (; i < n; ++i) {
            const char c = command[i];
            if (quote != '\0') {
                if (c == quote) {
                    quote = '\0';
                } else {
                    word.push_back(c);
                }
                continue;
            }
            if (c == '\'' || c == '"') {
                quote = c;
                continue;
            }
            if (std::isspace(static_cast<unsigned char>(c)) != 0) {
                break;
            }
            word.push_back(c);
        }
        words.push_back(std::move(word));
    }
    return words;
}

// 可执行名归一:剥目录、转小写、去 .exe/.cmd/.bat。
std::string NormalizeExecutable(std::string executable) {
    const auto slash = executable.find_last_of("/\\");
    if (slash != std::string::npos) {
        executable.erase(0, slash + 1);
    }
    std::transform(executable.begin(), executable.end(), executable.begin(),
                   [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    if (executable.ends_with(".exe") || executable.ends_with(".cmd") || executable.ends_with(".bat")) {
        executable.erase(executable.find_last_of('.'));
    }
    return executable;
}

// VAR=val 形式的环境赋值前缀(变量名:字母/下划线起头,后跟字母数字下划线)。
bool IsEnvAssignment(const std::string& word) {
    const auto eq = word.find('=');
    if (eq == std::string::npos || eq == 0) {
        return false;
    }
    const auto head = static_cast<unsigned char>(word[0]);
    if (std::isalpha(head) == 0 && head != '_') {
        return false;
    }
    for (std::size_t k = 1; k < eq; ++k) {
        const auto c = static_cast<unsigned char>(word[k]);
        if (std::isalnum(c) == 0 && c != '_') {
            return false;
        }
    }
    return true;
}

// sudo 带参数的选项(选项后跟一个词);其余 -x 一律当无参开关跳。
bool SudoOptionTakesValue(const std::string& word) {
    return word == "-u" || word == "-g" || word == "-h" || word == "-p" || word == "-C" || word == "-D" ||
           word == "-r" || word == "-t" || word == "-U";
}

}  // namespace

namespace run_command_detail {

int DefaultTimeoutMsForCommand(const std::string& command) {
    // 前缀最多剥这么多词;正常命令十来个词以内就见到真首词。
    const std::vector<std::string> words = LeadingWords(command, 32);
    std::size_t i = 0;
    while (i < words.size()) {
        const std::string& w = words[i];
        if (w == "cd") {
            // cd <dir> && / cd <dir> ; ——跳过,看后半句。没接连接符就是单纯 cd。
            if (i + 2 < words.size() && (words[i + 2] == "&&" || words[i + 2] == ";")) {
                i += 3;
                continue;
            }
            return kDefaultTimeoutMs;
        }
        if (w == "sudo") {
            ++i;
            while (i < words.size() && words[i].size() > 1 && words[i][0] == '-') {
                if (words[i] == "--") {
                    ++i;
                    break;
                }
                i += SudoOptionTakesValue(words[i]) ? 2 : 1;
            }
            continue;
        }
        if (w == "env") {
            ++i;
            while (i < words.size() && ((words[i].size() > 1 && words[i][0] == '-') || IsEnvAssignment(words[i]))) {
                ++i;
            }
            continue;
        }
        if (w == "time" || w == "nohup" || IsEnvAssignment(w)) {
            ++i;
            continue;
        }
        break;
    }
    if (i >= words.size()) {
        return kDefaultTimeoutMs;
    }
    const std::string executable = NormalizeExecutable(words[i]);
    static constexpr const char* kLongCommands[] = {
        // 构建
        "cmake", "msbuild", "ninja", "ctest", "gradle", "gradlew", "cargo", "npx", "make", "gmake",
        "xcodebuild", "mvn", "mvnw", "dotnet", "bazel", "buck2", "configure", "go", "rustc", "gcc", "g++",
        "clang", "clang++",
        // 装依赖
        "pip", "pip3", "uv", "poetry", "conda", "mamba", "npm", "pnpm", "yarn", "bun", "apt", "apt-get",
        // 测试
        "pytest", "tox",
    };
    if (std::find(std::begin(kLongCommands), std::end(kLongCommands), executable) != std::end(kLongCommands)) {
        return kBuildTimeoutMs;
    }
    // python -m pip / python3 -m pytest(含 python3.11 这类带版本号的名字):
    // 只认紧跟的 -m 模块,不扫后面的参数。
    if (executable == "python" || executable == "py" || executable.starts_with("python3")) {
        for (std::size_t k = i + 1; k + 1 < words.size(); ++k) {
            if (words[k] == "-m") {
                const std::string& module = words[k + 1];
                return (module == "pip" || module == "pytest") ? kBuildTimeoutMs : kDefaultTimeoutMs;
            }
            if (words[k].empty() || words[k][0] != '-') {
                break;  // 跑脚本文件,不是 -m
            }
        }
    }
    return kDefaultTimeoutMs;
}

ShellChoice ChoosePosixShell(const std::string& requested, const std::optional<std::string>& bash_path) {
    if (requested == "sh") {
        return {"sh", "/bin/sh"};
    }
    // 没指定或点名 bash:有 bash 就用(非 login、不读 rc,只是 -c);没有
    // 回落 /bin/sh——Debian 的 /bin/sh 是 dash,source/[[ ]] 跑不通,
    // 能用 bash 就别让模型撞这堵墙。
    if (bash_path.has_value() && !bash_path->empty()) {
        return {"bash", *bash_path};
    }
    return {"sh", "/bin/sh"};
}

std::vector<std::pair<std::string, std::string>> NonInteractiveEnvDefaults() {
    return {
        {"DEBIAN_FRONTEND", "noninteractive"},
        {"PIP_NO_INPUT", "1"},
        {"PYTHONUNBUFFERED", "1"},
        {"GIT_TERMINAL_PROMPT", "0"},
        {"PAGER", "cat"},
        {"GIT_PAGER", "cat"},
    };
}

std::vector<std::pair<std::string, std::string>> FillMissingEnv(
    const std::vector<std::pair<std::string, std::string>>& defaults,
    const std::function<bool(const std::string&)>& is_set) {
    std::vector<std::pair<std::string, std::string>> missing;
    for (const auto& [key, value] : defaults) {
        if (!is_set || !is_set(key)) {
            missing.emplace_back(key, value);
        }
    }
    return missing;
}

int CapTimeoutToDeadlineMs(int requested_ms, std::optional<std::int64_t> remaining_ms) {
    if (!remaining_ms.has_value()) {
        return requested_ms;
    }
    // 封顶值先在 64 位里算,再夹回 int:剩余再多也不会越过 requested。
    // 不用 std::max:Windows 头的 max 宏会把它咬坏。
    const std::int64_t usable = *remaining_ms - kDeadlineCommandReserveMs;
    const std::int64_t ceiling = usable > kDeadlineCommandFloorMs ? usable : kDeadlineCommandFloorMs;
    if (static_cast<std::int64_t>(requested_ms) <= ceiling) {
        return requested_ms;
    }
    return static_cast<int>(ceiling);
}

}  // namespace run_command_detail

namespace {

// timeout_ms 的合法上限(int 全域放进来没意义:Windows 的 DWORD 等得起,
// 会话也早没了)。超大值体面报错,不窄化走样。
constexpr int kMaxTimeoutMs = 86400000;  // 24 小时
// 命令字节上限(进程生命线单 P1):Windows 命令行 32767 个 UTF-16 码位,
// wrapper 头尾约 300 字符 + base64 膨胀 4/3,故用户命令压到 22000 字节
// (UTF-8 字节数;多字节字符折成码位更少,余量足够)。超长提前拒绝并给
// 可操作的说法,不让 CreateProcessW/exec 吞一个莫名错误码。POSIX 的
// ARG_MAX 同一张表(典型 2MB,22000 远在墙内)。
constexpr std::size_t kMaxCommandBytes = 22000;

// (进程生命线单 P1 根治:cwd 一律走操作系统参数,命令文本不拼 cd。
// ApplyWorkingDirectory 与 QuotePowerShellSingle/QuotePosixSingle 已删,
// 见 execute() 里的注释。)

// ---------------------------------------------------------------------------
// ShellResolver(进程生命线单 P2"评估显式 bash、pwsh"):可移植 shell 探测。
// bash/pwsh 不随包附送——装了才进 schema、才认得;没装就当不存在,不猜
// 路径、不偷换 shell。探测结果进程内缓存(spool 一次即可,PATH 不会中途变)。
// ---------------------------------------------------------------------------

bool ShellExecutableExists(const std::string& path_utf8) {
    std::error_code ec;
    const std::filesystem::path p = Utf8ToPath(path_utf8);
    return std::filesystem::exists(p, ec) && !std::filesystem::is_directory(p, ec);
}

// PATH 里找一枚可执行文件(POSIX 查 x 位,Windows 查 PATH 目录里的 .exe)。
// 找到返回它的 UTF-8 完整路径;找不到 nullopt。
std::optional<std::string> FindOnPath(const std::string& name_utf8, const char* path_env) {
    if (path_env == nullptr || *path_env == '\0') {
        return std::nullopt;
    }
    const char sep =
#ifdef _WIN32
        ';';
#else
        ':';
#endif
    std::stringstream ss(path_env);
    std::string dir;
    while (std::getline(ss, dir, sep)) {
        if (dir.empty()) {
            continue;
        }
        std::error_code ec;
        const std::filesystem::path base = Utf8ToPath(dir);
#ifdef _WIN32
        const std::filesystem::path candidate = base / (name_utf8 + ".exe");
        if (std::filesystem::exists(candidate, ec) && !std::filesystem::is_directory(candidate, ec)) {
            const std::u8string u8 = candidate.u8string();
            return std::string(reinterpret_cast<const char*>(u8.data()), u8.size());
        }
#else
        const std::filesystem::path candidate = base / name_utf8;
        if (access(candidate.c_str(), X_OK) == 0) {
            const std::u8string u8 = candidate.u8string();
            return std::string(reinterpret_cast<const char*>(u8.data()), u8.size());
        }
#endif
    }
    return std::nullopt;
}

// 本机 bash 的落点(POSIX)。常见落点先直查(/bin/bash、/usr/bin/bash),
// 再走 PATH(NixOS 之类没有 /bin/bash)。Windows 恒 nullopt。结果缓存。
std::optional<std::string> ResolveBashPath() {
    // PATH 读取并入 platform::GetEnvVar(src 收口审计 P3 候选:私有读取把
    // "空 PATH"当 optional("")、平台口当 nullopt——三格语义测试钉住平台
    // 合同后并口。对本处行为无差:FindOnPath 对空串与缺席同样找不到,
    // bash/pwsh 都按"PATH 里没有"收场。
    static const std::optional<std::string> cached = []() -> std::optional<std::string> {
#ifdef _WIN32
        return std::nullopt;  // bash 是 POSIX 侧的可选 shell;Windows 不认(WSL 那只 bash 不是宿主 shell)
#else
        for (const char* candidate : {"/bin/bash", "/usr/bin/bash"}) {
            if (ShellExecutableExists(candidate)) {
                return std::string(candidate);
            }
        }
        const auto path = platform::GetEnvVar("PATH");
        if (!path.has_value()) {
            return std::nullopt;
        }
        return FindOnPath("bash", path->c_str());
#endif
    }();
    return cached;
}

// 本机装没装 bash(POSIX)/pwsh(Windows)。结果缓存。
bool OptionalShellAvailable(const std::string& shell_id) {
    static const bool bash_ok = ResolveBashPath().has_value();
    static const bool pwsh_ok = [] {
#ifdef _WIN32
        // pwsh 按名字过 PATH 找(PowerShell 7 默认装进 Program Files 并入 PATH;
        // 没装(只有 Windows PowerShell 5.1)就找不到,如实不可用)。
        const auto path = platform::GetEnvVar("PATH");
        return path.has_value() && FindOnPath("pwsh", path->c_str()).has_value();
#else
        return false;  // pwsh 是 Windows 侧的可选 shell;POSIX 不认(装了 pwsh 的 Linux 用户极少,不替他们猜)
#endif
    }();
    if (shell_id == "bash") {
        return bash_ok;
    }
    if (shell_id == "pwsh") {
        return pwsh_ok;
    }
    return false;
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
                    "超时会被强制杀掉。构建/configure/测试类命令默认 900000 毫秒(15 分钟),"
                    "其余默认 120000 毫秒(2 分钟);预计更久的长命令请显式传 timeout_ms,或传 "
                    "run_in_background=true 后台运行并用 background_output 轮询。"
                    "起 dev server、watch 进程这类要跨命令、跨调用存活的长命进程,或者想后台跑完不阻塞对话的短任务,"
                    "传 run_in_background=true:不等它跑完,spawn 成功立刻返回 task_id、PID 和日志文件路径;"
                    "命令跑完时下一次给提示符会打一行完成通知。之后用 background_output 工具(传 task_id)"
                    "查状态/读输出,stop_background 工具收尾。");
#else
    return ToolText("run_command", "description (POSIX)",
                    "在 shell 里执行一条命令,拿到合并后的标准输出/标准错误,以及退出码。"
                    "默认用 bash(/bin/bash -c,不读 rc 文件),本机没有 bash 才回落 /bin/sh;"
                    "shell=sh 可强制走 POSIX sh。命令在非交互环境里跑:stdin 接 /dev/null,"
                    "已设 DEBIAN_FRONTEND=noninteractive、PIP_NO_INPUT=1、GIT_TERMINAL_PROMPT=0、PAGER=cat 等,"
                    "要交互输入的命令请改用 -y 之类的非交互参数。执行前要经用户确认。超时会被强制杀掉。"
                    "构建/configure/装依赖/测试类命令默认 900000 毫秒(15 分钟),其余默认 120000 毫秒(2 分钟);"
                    "预计更久的长命令请显式传 timeout_ms,或传 run_in_background=true 后台运行并用 "
                    "background_output 轮询。输出超过 2MB 时只保留前 1MB 与后 1MB,中间注明省略字节数,"
                    "命令照常跑完。"
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
        ToolText("run_command", "param.command (POSIX)",
                 "要执行的命令,按所选 shell 的语法写(默认 Bash 语法;本机没有 bash 时为 POSIX sh)");
#endif
    properties["command"] = command_prop;

    nlohmann::json timeout_prop = nlohmann::json::object();
    timeout_prop["type"] = "integer";
    timeout_prop["description"] =
        ToolText("run_command", "param.timeout_ms",
                 "超时时间,单位毫秒。不填时构建/configure/测试类命令默认 900000(15 分钟),其余默认 "
                 "120000(2 分钟);长命令应显式加大此值或用 run_in_background");
    properties["timeout_ms"] = timeout_prop;

    nlohmann::json shell_prop = nlohmann::json::object();
    shell_prop["type"] = "string";
#ifdef _WIN32
    {
        nlohmann::json shell_enum = nlohmann::json::array({"powershell", "cmd"});
        // pwsh(PowerShell 7):装了才进 schema(P2"无安装便不进 schema"),
        // 不装不提——模型见不着这个选项就不会拿 pwsh 专属语法来赌。
        if (OptionalShellAvailable("pwsh")) {
            shell_enum.push_back("pwsh");
        }
        shell_prop["enum"] = std::move(shell_enum);
        shell_prop["description"] =
            ToolText("run_command", "param.shell",
                     "用哪个 shell 执行,不填默认 powershell(Windows PowerShell 5.1)。pwsh 只在本机"
                     "装了 PowerShell 7 时可选。三者语法与编码细节不同,不可混用。");
    }
#else
    {
        nlohmann::json shell_enum = nlohmann::json::array({"sh"});
        // bash:装了才进 schema。不填 shell 时有 bash 默认就走 bash(Terminal-Bench
        // 加固单:Debian 的 /bin/sh 是 dash,source/[[ ]] 跑不通);sh 显式点名仍走 /bin/sh。
        if (OptionalShellAvailable("bash")) {
            shell_enum.push_back("bash");
        }
        shell_prop["enum"] = std::move(shell_enum);
        shell_prop["description"] =
            ToolText("run_command", "param.shell (POSIX)",
                     "用哪个 shell 执行。不填时本机有 bash 就用 bash(/bin/bash),没有再用 sh(/bin/sh);"
                     "sh 强制走 POSIX sh(Debian/Ubuntu 上是 dash,不认 source、[[ ]])。"
                     "powershell/cmd 是 Windows 专属");
    }
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

    // 进程生命线单 P2:后台任务的可选最长运行时间。另立参数,不改
    // timeout_ms 旧义(后台照旧忽略 timeout_ms——dev server 缺省无限活)。
    // 显式传值便由后台 supervisor 到点收整棵树。
    nlohmann::json max_runtime_prop = nlohmann::json::object();
    max_runtime_prop["type"] = "integer";
    max_runtime_prop["description"] =
        ToolText("run_command", "param.max_runtime_ms",
                 "后台任务最长运行时间(毫秒),只对 run_in_background=true 有意义。不填 = 无限"
                 "(dev server 这类长命进程的缺省);显式传值则到点自动收掉整棵进程树,防止误起的"
                 "死循环一直跑。前台命令用 timeout_ms,不要传这个。");
    properties["max_runtime_ms"] = max_runtime_prop;

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
// base64 编码统一走 platform::Base64Encode(审计 P2:五处手写收一)——
// 这里只管把脚本先编成 UTF-16LE 字节,-EncodedCommand 要的格式。

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
    // 退出码契约(进程生命线单 P1"PowerShell 退出码包装会误判"),先定后写:
    //   1. 显式 exit N —— 原样(N 直接终止进程,下面的判定碰不到它);
    //   2. 用户脚本跑过 native 程序 —— $LASTEXITCODE 的末次值优先保留;
    //   3. 没跑过 native —— 捕获流里含 ErrorRecord(cmdlet 报错/找不到
    //      命令)则 exit 1,否则 exit 0。throw 自会终止脚本进程(exit 1)。
    // 三只坑的解法:
    //   - Out-String/Write-Output 会盖掉 $?——状态在 scriptblock 返回的
    //     第一拍捕获(先收进 $oco,之后才格式化输出);
    //   - $LASTEXITCODE 保留最近一只 native 的码——先清账($LASTEXITCODE
    //     = $null),末次值才算数;
    //   - "cmdlet 失败被 2>&1 合并后 $? = true"是 5.1 的实情——不能只
    //     靠 $?,逐枚检查捕获流里有没有 ErrorRecord。
    const std::string script_utf8 =
        "$ProgressPreference='SilentlyContinue'\r\n"
        "[Console]::OutputEncoding=[System.Text.Encoding]::UTF8\r\n"
        "$LASTEXITCODE = $null\r\n"  // 清账:别让宿主/前一只 native 的旧码冒充本段的退出码
        "$oco = & { " + user_command_utf8 + " } 2>&1\r\n"  // scriptblock 返回的第一拍捕获
        "$lec = $LASTEXITCODE\r\n"
        "$errseen = $false\r\n"
        "foreach ($it in @($oco)) { if ($it -is [System.Management.Automation.ErrorRecord]) { $errseen = $true } }\r\n"
        "$oco | Out-String -Stream | Write-Output\r\n"  // 之后才格式化输出
        "if ($lec -ne $null) { exit $lec }\r\n"  // 末次 native 的码优先
        "if ($errseen) { exit 1 } else { exit 0 }\r\n";  // cmdlet 报错/找不到命令 vs 干净

    const std::wstring wide = platform::Utf8ToWide(script_utf8);
    return platform::Base64Encode(std::span<const std::byte>(
        reinterpret_cast<const std::byte*>(wide.data()), wide.size() * sizeof(wchar_t)));
}

// 后台专用的流式 wrapper(background 管理面单):与上面那颗的差异只有
// 一处——命令裸执行,输出不落变量、不套 Out-String,stdout/stderr 由
// platform 层直接接进日志文件。捕获式 wrapper 会把输出攒在内存里,进程
// 不退日志一个字不落:dev server 这类长命进程被停掉时,攒的那点输出全
// 跟着陪葬——"查看启动后持续增长的日志"就成了空话(实测:3 秒时日志
// 0 字节,进程退出才 40 字节)。流式 wrapper 边跑边落盘(实测:3 秒时
// 已有两行)。
// 退出码契约与前台同款三条:
//   1. 显式 exit N —— 原样(N 直接终止进程,下面的判定碰不到它);
//   2. 跑过 native —— $LASTEXITCODE 末次值;
//   3. 没跑过 native —— $? 真=0 假=1。
// 两只坑都实测过:命令必须裸跑(套 & { } 的 scriptblock 会把 $? 吞成
// 真,cmdlet 报错也测不出);`2>&1` 不能要(合并会叫 $? 恒真)——stderr
// 由 hStdError 直接落日志,不劳脚本合并。
std::string BuildBackgroundEncodedCommand(const std::string& user_command_utf8) {
    const std::string script_utf8 =
        "$ProgressPreference='SilentlyContinue'\r\n"
        "[Console]::OutputEncoding=[System.Text.Encoding]::UTF8\r\n"
        "$LASTEXITCODE = $null\r\n"  // 清账:别让宿主/前一只 native 的旧码冒充本段的退出码
        + user_command_utf8 + "\r\n" +  // 裸执行:边跑边流,日志实时增长
        "if ($LASTEXITCODE -ne $null) { exit $LASTEXITCODE }\r\n"
        "if ($?) { exit 0 } else { exit 1 }\r\n";

    const std::wstring wide = platform::Utf8ToWide(script_utf8);
    return platform::Base64Encode(std::span<const std::byte>(
        reinterpret_cast<const std::byte*>(wide.data()), wide.size() * sizeof(wchar_t)));
}

}  // namespace

#endif  // _WIN32

Tool::Result RunCommandTool::execute(const nlohmann::json& input) {
    return Run(input, cancel_);
}

Tool::Result RunCommandTool::execute(const nlohmann::json& input, const ToolExecutionContext& context) {
    // context 的取消旗优先(本次调用真用的那根:主回合 ESC / 子代理的
    // CancelChain 合并旗);没递进来(旧调用方)退回 SetCancel 灌的那根。
    return Run(input, context.cancel != nullptr ? context.cancel : cancel_, context.deadline);
}

Tool::Result RunCommandTool::Run(const nlohmann::json& input, const std::atomic<bool>* effective_cancel,
                                 std::optional<std::chrono::steady_clock::time_point> deadline) {
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
    // 命令字节上限:见 kMaxCommandBytes 的注。长脚本请先写进文件再调用
    // 脚本文件,不要硬顶系统命令行。
    if (command.size() > kMaxCommandBytes) {
        return {"command 太长(" + std::to_string(command.size()) + " 字节,上限 " +
                    std::to_string(kMaxCommandBytes) + ")。长脚本请先用 write_file 写成脚本文件,再执行该文件",
                true};
    }

    bool run_in_background = false;
    if (auto it = input.find("run_in_background"); it != input.end() && !it->is_null()) {
        if (!it->is_boolean()) {
            return {"run_in_background 参数必须是布尔值", true};
        }
        run_in_background = it->get<bool>();
    }

    // 后台最长运行时间(P2):64 位解析 + 范围检查,与 timeout_ms 同一张
    // 边界表。前台命令不吃这个参数(传了也只在后台分支使用)。
    long long max_runtime_ms = 0;
    if (auto it = input.find("max_runtime_ms"); it != input.end() && !it->is_null()) {
        if (!it->is_number_integer()) {
            return {"max_runtime_ms 得是整数(毫秒)", true};
        }
        const std::int64_t raw = it->get<std::int64_t>();
        if (raw < 0 || raw > static_cast<std::int64_t>(kMaxTimeoutMs)) {
            return {"max_runtime_ms 得在 0~" + std::to_string(kMaxTimeoutMs) + " 毫秒之间(0 = 不限;写的是 " +
                        std::to_string(raw) + ")",
                    true};
        }
        max_runtime_ms = raw;
    }

    // timeout_ms 对后台模式没有意义(spawn 完就返回,根本不等),背景模式下
    // 干脆不解析——模型哪怕手滑传了个非法值也不该在这条不使用它的路径上
    // 报错,忽略得彻底一点。
    int timeout_ms = run_command_detail::DefaultTimeoutMsForCommand(command);
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
    // 墙钟预算封顶(one-shot 墙钟预算单):默认值与显式值一并受封——到点
    // 宿主进程会被评测侧直接杀掉,单条命令不许把整场的收尾时间吃光。
    int budget_capped_from_ms = 0;  // 非 0 = 本次被预算压过,原值记下给超时文案
    if (!run_in_background && deadline.has_value()) {
        const std::int64_t remaining_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                              *deadline - std::chrono::steady_clock::now())
                                              .count();
        const int capped = run_command_detail::CapTimeoutToDeadlineMs(timeout_ms, remaining_ms);
        if (capped < timeout_ms) {
            budget_capped_from_ms = timeout_ms;
            timeout_ms = capped;
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
    // cwd 走操作系统参数(进程生命线单 P1 根治,前台后台全量):Windows 落
    // CreateProcessW 的 lpCurrentDirectory,POSIX 子进程 exec 前 chdir,
    // 失败经 exec-error 管道回报 spawn_failed。命令文本一律不拼 cd——
    // 路径含引号、'%'、'!'、'$' 各家 shell 各有各的坑,拼字符串只会越补
    // 越厚;ApplyWorkingDirectory 与两枚 Quote*Single 已删。

#ifdef _WIN32
    std::string shell = "powershell";
    if (auto it = input.find("shell"); it != input.end() && !it->is_null()) {
        if (!it->is_string()) {
            return {"shell 参数必须是字符串", true};
        }
        shell = it->get<std::string>();
        const bool legal = (shell == "powershell" || shell == "cmd" ||
                            (shell == "pwsh" && OptionalShellAvailable("pwsh")));
        if (!legal) {
            if (shell == "pwsh") {
                return {"shell=pwsh 需要本机装有 PowerShell 7(pwsh.exe);没装,请用默认 powershell", true};
            }
            return {"shell 参数只认得 powershell 或 cmd,写的是: " + shell, true};
        }
    }

    const bool is_cmd = (shell == "cmd");
    // pwsh 与 powershell 5.1 共用同一套 -EncodedCommand wrapper 与单引号
    // 语法;差异只在可执行名(pwsh.exe)与版本语义(文档里明说,不偷偷混)。
    const wchar_t* ps_exe = (shell == "pwsh") ? L"pwsh.exe" : L"powershell.exe";

    if (run_in_background) {
        platform::BackgroundSpawnResult bg;
        if (is_cmd) {
            bg = platform::RunShellCommandBackground(command, effective_cwd);
        } else {
            // 后台 PowerShell:命令本体不用拼 cd(cwd 走 lpCurrentDirectory),
            // wrapper 用流式那颗(BuildBackgroundEncodedCommand)——输出边跑
            // 边落日志,长命进程不等退出就能看输出。
            const std::wstring cmdline = std::wstring(ps_exe) + L" -NoProfile -NonInteractive -EncodedCommand " +
                                          platform::Utf8ToWide(BuildBackgroundEncodedCommand(command));
            bg = platform::RunProcessBackground(cmdline, effective_cwd);
        }
        if (!bg.success) {
            return {bg.error, true};
        }
        // 登记进后台任务台账:拿一个 task_id,watcher 持原生句柄探活——
        // 完成时主循环会收到通知,模型也能用 background_output 工具查输出。
        if (bg.handle != nullptr && !is_cmd) {
            // wrapper 脚本设了 [Console]::OutputEncoding=UTF8,落盘的是 UTF-8
            //(解析期报错那点尾巴由出口清洗兜底)。
            bg.handle->encoding_hint = "utf-8";
        }
        const std::string task_id = BackgroundTaskRegistry::Instance().Register(
            command, shell,
            effective_cwd.empty() ? PathToUtf8(std::filesystem::current_path())
                                  : effective_cwd,
            bg.pid, bg.log_path, std::move(bg.handle), max_runtime_ms);
        std::ostringstream oss;
        oss << "已在后台启动(task #" << task_id << ", PID " << bg.pid << "),不等它跑完。\n"
            << "日志文件: " << bg.log_path << "\n"
            << "查状态/输出: 用 background_output 工具(传 task_id=" << task_id << ")。\n"
            << "收掉它: 用 stop_background 工具,或 /background stop " << task_id
            << ",或 Stop-Process -Id " << bg.pid << " -Force";
        if (max_runtime_ms > 0) {
            oss << "\n最长运行 " << max_runtime_ms << " 毫秒,到点自动收树。";
        }
        return {oss.str(), false};
    }

    platform::ProcessResult proc;
    if (is_cmd) {
        // RunShellCommand 内部已把 cmd.exe 的系统 ANSI 代码页输出转成 UTF-8
        // (跟 PowerShell 路径不一样,那边脚本里显式设了
        // [Console]::OutputEncoding=UTF8),这里拿到手就是合法 UTF-8。
        // cwd 走 lpCurrentDirectory(P1 根治:cmd 的 %VAR% 展开坑一并绕开)。
        proc = platform::RunShellCommand(command, timeout_ms, effective_cancel, {}, platform::kDefaultMaxOutputBytes,
                                         effective_cwd, platform::OutputOverflow::KeepHeadTail);
    } else {
        // 前台 PowerShell 同上:cwd 走 lpCurrentDirectory,命令本体只保留
        // wrapper 的编码设置,不再前置 Set-Location。cancel 从前写死
        // nullptr(子代理 x 停止失效单逮出的旧账:默认 shell 反而收不到
        // 取消,ESC/面板 x 都只能等超时)——现在与 cmd 路同走 effective_cancel,
        // 置位即收整棵树。
        const std::wstring cmdline = std::wstring(ps_exe) + L" -NoProfile -NonInteractive -EncodedCommand " +
                                      platform::Utf8ToWide(BuildEncodedCommand(command));
        proc = platform::RunProcess(cmdline, timeout_ms, effective_cancel, {},
                                    platform::kDefaultMaxOutputBytes,
                                    effective_cwd, platform::OutputOverflow::KeepHeadTail);
    }
#else
    // POSIX(Terminal-Bench 加固单):不填 shell 时本机有 bash 就走
    // `bash -c`(非 login、不读 rc,免副作用),没有再回落 /bin/sh——Debian
    // 的 /bin/sh 是 dash,source venv/bin/activate、[[ ]] 都跑不通。sh 显式
    // 点名仍走 /bin/sh。powershell/cmd 是 Windows 专属选项,模型带着旧习惯
    // 传过来就明说不支持,别悄悄换 shell 让语义走样。
    std::string requested_shell;
    if (auto it = input.find("shell"); it != input.end() && !it->is_null()) {
        if (!it->is_string()) {
            return {"shell 参数必须是字符串", true};
        }
        requested_shell = it->get<std::string>();
        if (requested_shell == "powershell" || requested_shell == "cmd") {
            return {"shell=" + requested_shell + " 是 Windows 专属,本平台请用 bash 或 sh", true};
        }
        if (requested_shell != "sh" && requested_shell != "bash") {
            return {"shell 参数只认得 sh 或 bash,写的是: " + requested_shell, true};
        }
        if (requested_shell == "bash" && !OptionalShellAvailable("bash")) {
            return {"shell=bash 需要本机装有 bash;没装,请用 sh(/bin/sh)", true};
        }
    }
    const run_command_detail::ShellChoice shell_choice =
        run_command_detail::ChoosePosixShell(requested_shell, ResolveBashPath());
    const std::string& shell = shell_choice.id;
    const std::string& shell_exe = shell_choice.executable;

    // 非交互环境:只补父环境没设的(用户自己设过的,哪怕是空串,都不动)。
    // 前台后台同一份。
    const platform::EnvPairs noninteractive_env = run_command_detail::FillMissingEnv(
        run_command_detail::NonInteractiveEnvDefaults(),
        [](const std::string& key) { return platform::GetEnvVarPresent(key.c_str()).has_value(); });

    if (run_in_background) {
        const platform::BackgroundSpawnResult bg =
            platform::RunProcessBackground({shell_exe, "-c", command}, effective_cwd, noninteractive_env);
        if (!bg.success) {
            return {bg.error, true};
        }
        // 登记进后台任务台账(同 Windows 分支):task_id + watcher 持原生句柄
        // 探活,完成时通知;退出码由唯一收尸方写进完成态,不再丢。
        const std::string task_id = BackgroundTaskRegistry::Instance().Register(
            command, shell,
            effective_cwd.empty() ? PathToUtf8(std::filesystem::current_path())
                                  : effective_cwd,
            bg.pid, bg.log_path, std::move(bg.handle), max_runtime_ms);
        std::ostringstream oss;
        oss << "已在后台启动(task #" << task_id << ", PID " << bg.pid << "),不等它跑完。\n"
            << "日志文件: " << bg.log_path << "\n"
            << "查状态/输出: 用 background_output 工具(传 task_id=" << task_id << ")。\n"
            << "收掉它: 用 stop_background 工具,或 /background stop " << task_id << ",或 kill " << bg.pid
            << "(不行就 kill -9 " << bg.pid << ")";
        if (max_runtime_ms > 0) {
            oss << "\n最长运行 " << max_runtime_ms << " 毫秒,到点自动收树。";
        }
        return {oss.str(), false};
    }

    // 前台 POSIX:cwd 走操作系统参数(子进程 exec 前 chdir),命令文本不
    // 拼 cd——验收口径"cwd 不再拼进 shell 字符串"的前台半边。输出超帽
    // 留首尾、不杀(编译报错多在尾巴)。
    platform::ProcessResult proc =
        platform::RunProcess({shell_exe, "-c", command}, timeout_ms, effective_cancel, noninteractive_env,
                             platform::kDefaultMaxOutputBytes, effective_cwd, platform::OutputOverflow::KeepHeadTail);
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
        if (budget_capped_from_ms > 0) {
            // 预算封顶压出来的超时:加大 timeout_ms 无用,明说,别让模型白重跑。
            oss << "[墙钟预算所限:本次超时已从 " << budget_capped_from_ms << " 毫秒封顶到 " << timeout_ms
                << " 毫秒,整场剩余时间不多;加大 timeout_ms 无效,请改用更快的检查或直接收尾]\n";
        }
        oss << "可改用以下策略：\n"
            << "1. 加大 timeout_ms 后重跑。\n"
            << "2. 传 run_in_background=true 后台运行，再用 background_output 查进度。\n"
            << "3. 首次构建检查依赖下载是否卡网；离线环境先备好 _deps。\n";
        if (!proc.output.empty()) {
            oss << "终止前捕获到的输出:\n" << proc.output;
        }
        Tool::Result timed{oss.str(), true};
        timed.outcome = "timed_out";
        timed.error_code = "process.timeout";
        return timed;
    }
    if (proc.cancelled) {
        std::ostringstream oss;
        oss << "命令被取消(停止信号:ESC 或子代理停止),进程树已终止。\n";
        if (!proc.output.empty()) {
            oss << "取消前捕获到的输出:\n" << proc.output;
        }
        Tool::Result cancelled{oss.str(), true};
        cancelled.outcome = "cancelled_during_run";
        return cancelled;
    }
    // 输出超帽(Terminal-Bench 加固单):platform 层走 KeepHeadTail,不再杀
    // 命令——命令照常跑完,退出码照实算;输出只留前 1MB 与后 1MB,中间换成
    // 一行省略标记。成败仍按退出码,截断另在正文头与 details 里注明。
    std::ostringstream oss;
    oss << "[退出码 " << proc.exit_code << "]\n";
    if (proc.output_truncated) {
        oss << "[输出超过上限(" << platform::kDefaultMaxOutputBytes / (1024 * 1024)
            << "MB)已截断:只保留前后各一半,中间省略 " << proc.omitted_bytes
            << " 字节;命令已照常跑完。要看全文,把输出重定向到文件再分段读]\n";
    }
    oss << proc.output;
    Tool::Result exited{oss.str(), proc.exit_code != 0};
    if (proc.exit_code != 0) {
        exited.outcome = "process_exit_nonzero";
        exited.error_code = "process.exit_nonzero";
    } else {
        exited.outcome = "succeeded";
    }
    exited.details = nlohmann::json{{"exit_code", proc.exit_code}, {"timeout_ms", timeout_ms}};
    if (budget_capped_from_ms > 0) {
        exited.details["timeout_capped_by_budget"] = true;
    }
    if (proc.output_truncated) {
        exited.details["output_truncated"] = true;
        exited.details["omitted_bytes"] = proc.omitted_bytes;
    }
    exited.effect_summary = "run (exit=" + std::to_string(proc.exit_code) + ")";
    return exited;
}

}  // namespace lubancode::tools
