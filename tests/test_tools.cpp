// read_file:读临时文件(含中文内容)、行号、offset/limit、不存在的文件。
// run_command:跑 echo、非零退出码、超时(用 PowerShell 的 Start-Sleep 制造)。

#include <doctest/doctest.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <thread>

#include "platform/text_encoding.hpp"
#include "tools/read_file.hpp"
#include "tools/run_command.hpp"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>  // ::GetACP(),GBK 样本转换那条测试用来判断本机代码页是不是 936;
                       // OpenProcess/GetExitCodeProcess/TerminateProcess,run_in_background 探活用
#else
#include <csignal>
#include <sys/types.h>
#endif

using lubancode::tools::ReadFileTool;
using lubancode::tools::RunCommandTool;
using lubancode::tools::Tool;

namespace {

// run_in_background 那几条测试的小工具:从工具返回的结果文本里抠出 PID
// 和日志文件路径("已在后台启动(PID 12345)...\n日志文件: <path>\n..."这个
// 格式,见 run_command.cpp execute() 里拼的那段)。
unsigned long ExtractPid(const std::string& content) {
    const std::string marker = "PID ";
    const auto pos = content.find(marker);
    REQUIRE(pos != std::string::npos);
    std::size_t i = pos + marker.size();
    unsigned long pid = 0;
    while (i < content.size() && content[i] >= '0' && content[i] <= '9') {
        pid = pid * 10 + static_cast<unsigned long>(content[i] - '0');
        ++i;
    }
    return pid;
}

std::string ExtractLogPath(const std::string& content) {
    const std::string marker = "日志文件: ";
    const auto pos = content.find(marker);
    REQUIRE(pos != std::string::npos);
    const auto start = pos + marker.size();
    const auto end = content.find('\n', start);
    return content.substr(start, end == std::string::npos ? std::string::npos : end - start);
}

#ifdef _WIN32
bool IsPidAlive(unsigned long pid) {
    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, static_cast<DWORD>(pid));
    if (h == nullptr) {
        return false;
    }
    DWORD exit_code = 0;
    const bool alive = GetExitCodeProcess(h, &exit_code) != 0 && exit_code == STILL_ACTIVE;
    CloseHandle(h);
    return alive;
}

void KillPidForTest(unsigned long pid) {
    HANDLE h = OpenProcess(PROCESS_TERMINATE, FALSE, static_cast<DWORD>(pid));
    if (h != nullptr) {
        TerminateProcess(h, 1);
        CloseHandle(h);
    }
}
#else
bool IsPidAlive(unsigned long pid) {
    // 带 0 信号的 kill:不真发信号,只探测这个 pid 存不存在/是否有权限杀。
    return kill(static_cast<pid_t>(pid), 0) == 0;
}

void KillPidForTest(unsigned long pid) {
    kill(static_cast<pid_t>(pid), SIGKILL);
}
#endif

// 轮询等一个条件成立,最多等 timeout_ms 毫秒,每次间隔 poll_ms 毫秒。
template <typename Predicate>
bool WaitUntil(Predicate pred, int timeout_ms, int poll_ms = 100) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    while (std::chrono::steady_clock::now() < deadline) {
        if (pred()) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(poll_ms));
    }
    return pred();
}

// 在系统临时目录下建一个用完即删的临时文件,内容按 UTF-8 原样写入。
class TempFile {
public:
    explicit TempFile(const std::string& content) {
        path_ = std::filesystem::temp_directory_path() /
                ("lubancode_test_" + std::to_string(reinterpret_cast<std::uintptr_t>(this)) + ".txt");
        std::ofstream file(path_, std::ios::binary);
        file << content;
    }
    ~TempFile() {
        std::error_code ec;
        std::filesystem::remove(path_, ec);
    }

    std::string Utf8Path() const {
        const std::u8string u8 = path_.u8string();
        return std::string(reinterpret_cast<const char*>(u8.data()), u8.size());
    }

private:
    std::filesystem::path path_;
};

}  // namespace

TEST_CASE("read_file: 读整个文件,带行号、含中文") {
    TempFile file("第一行\nhello\n第三行\n");
    ReadFileTool tool;

    nlohmann::json input;
    input["path"] = file.Utf8Path();
    const Tool::Result result = tool.execute(input);

    CHECK_FALSE(result.is_error);
    CHECK(result.content.find("第一行") != std::string::npos);
    CHECK(result.content.find("hello") != std::string::npos);
    CHECK(result.content.find("第三行") != std::string::npos);
    // cat -n 风格:行号 + tab
    CHECK(result.content.find("1\t第一行") != std::string::npos);
    CHECK(result.content.find("2\thello") != std::string::npos);
    CHECK(result.content.find("3\t第三行") != std::string::npos);
}

TEST_CASE("read_file: offset/limit 只读一部分") {
    TempFile file("l1\nl2\nl3\nl4\nl5\n");
    ReadFileTool tool;

    nlohmann::json input;
    input["path"] = file.Utf8Path();
    input["offset"] = 2;
    input["limit"] = 2;
    const Tool::Result result = tool.execute(input);

    CHECK_FALSE(result.is_error);
    CHECK(result.content.find("l2") != std::string::npos);
    CHECK(result.content.find("l3") != std::string::npos);
    CHECK(result.content.find("l1") == std::string::npos);
    CHECK(result.content.find("l4") == std::string::npos);
    CHECK(result.content.find("l5") == std::string::npos);
}

TEST_CASE("read_file: 文件不存在,报可读的错误") {
    ReadFileTool tool;
    nlohmann::json input;
    input["path"] = "D:/lubancode/这个文件肯定不存在_xyz_123.txt";
    const Tool::Result result = tool.execute(input);

    CHECK(result.is_error);
    CHECK_FALSE(result.content.empty());
}

TEST_CASE("read_file: 路径是目录,报错") {
    ReadFileTool tool;
    nlohmann::json input;
    input["path"] = std::filesystem::temp_directory_path().string();
    const Tool::Result result = tool.execute(input);

    CHECK(result.is_error);
}

TEST_CASE("read_file: 缺少必填参数 path,报错不崩") {
    ReadFileTool tool;
    nlohmann::json input = nlohmann::json::object();
    const Tool::Result result = tool.execute(input);

    CHECK(result.is_error);
}

TEST_CASE("run_command: 跑 echo,拿到输出和退出码 0") {
    RunCommandTool tool;
    nlohmann::json input;
    input["command"] = "echo hello-from-test";
    const Tool::Result result = tool.execute(input);

    CHECK_FALSE(result.is_error);
    CHECK(result.content.find("hello-from-test") != std::string::npos);
}

TEST_CASE("run_command: 非零退出码,is_error 置位") {
    RunCommandTool tool;
    nlohmann::json input;
    input["command"] = "exit 3";
    const Tool::Result result = tool.execute(input);

    CHECK(result.is_error);
    CHECK(result.content.find("3") != std::string::npos);
}

TEST_CASE("run_command: 中文输出不乱码") {
    RunCommandTool tool;
    nlohmann::json input;
    input["command"] = "echo 你好世界";
    const Tool::Result result = tool.execute(input);

    CHECK_FALSE(result.is_error);
    CHECK(result.content.find("你好世界") != std::string::npos);
}

TEST_CASE("run_command: 超时会被强制终止,不会真的等满") {
    RunCommandTool tool;
    nlohmann::json input;
#ifdef _WIN32
    input["command"] = "Start-Sleep -Seconds 20";
#else
    input["command"] = "sleep 20";
#endif
    input["timeout_ms"] = 800;

    const auto started = std::chrono::steady_clock::now();
    const Tool::Result result = tool.execute(input);
    const auto elapsed = std::chrono::steady_clock::now() - started;

    CHECK(result.is_error);
    CHECK(result.content.find("超时") != std::string::npos);
    // 给足够的余量(杀进程、收尾都要时间),但远小于 20 秒的睡眠时长。
    CHECK(std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count() < 8000);
}

// ---------------------------------------------------------------------------
// run_in_background:秒级返回、PID 存活可收、日志文件真写了东西。
// ---------------------------------------------------------------------------

TEST_CASE("run_command: run_in_background=true 秒级返回,PID 存活,收掉后确认已死") {
    RunCommandTool tool;
    nlohmann::json input;
#ifdef _WIN32
    input["command"] = "ping -n 30 127.0.0.1";
    input["shell"] = "cmd";
#else
    input["command"] = "sleep 30";
#endif
    input["run_in_background"] = true;

    const auto started = std::chrono::steady_clock::now();
    const Tool::Result result = tool.execute(input);
    const auto elapsed = std::chrono::steady_clock::now() - started;

    CHECK_FALSE(result.is_error);
    CHECK(result.content.find("PID") != std::string::npos);
    CHECK(result.content.find("日志文件") != std::string::npos);
    // spawn 不等待,秒级就该返回——远小于 30 秒的命令时长。
    CHECK(std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count() < 5000);

    const unsigned long pid = ExtractPid(result.content);
    CHECK(pid != 0);
    CHECK(IsPidAlive(pid));

    KillPidForTest(pid);
    const bool dead = WaitUntil([pid] { return !IsPidAlive(pid); }, /*timeout_ms=*/5000);
    CHECK(dead);
}

TEST_CASE("run_command: run_in_background 的输出确实写进了日志文件") {
    RunCommandTool tool;
    nlohmann::json input;
#ifdef _WIN32
    input["command"] = "echo hello-from-bg-log";
    input["shell"] = "cmd";
#else
    input["command"] = "echo hello-from-bg-log";
#endif
    input["run_in_background"] = true;

    const Tool::Result result = tool.execute(input);
    CHECK_FALSE(result.is_error);
    const std::string log_path = ExtractLogPath(result.content);
    REQUIRE_FALSE(log_path.empty());

    std::string log_content;
    const bool got_it = WaitUntil(
        [&] {
            std::ifstream log_file(log_path, std::ios::binary);
            if (!log_file) {
                return false;
            }
            std::ostringstream ss;
            ss << log_file.rdbuf();
            log_content = ss.str();
            return log_content.find("hello-from-bg-log") != std::string::npos;
        },
        /*timeout_ms=*/5000);

    CHECK(got_it);
    CHECK(log_content.find("hello-from-bg-log") != std::string::npos);
}

TEST_CASE("run_command: run_in_background=true 时 timeout_ms 传垃圾值也不报错(忽略)") {
    RunCommandTool tool;
    nlohmann::json input;
#ifdef _WIN32
    input["command"] = "cmd /c exit 0";
    input["shell"] = "cmd";
#else
    input["command"] = "true";
#endif
    input["run_in_background"] = true;
    input["timeout_ms"] = "不是数字";

    const Tool::Result result = tool.execute(input);
    CHECK_FALSE(result.is_error);
    const unsigned long pid = ExtractPid(result.content);
    CHECK(pid != 0);
}

TEST_CASE("run_command: run_in_background 参数不是布尔值,报错不崩") {
    RunCommandTool tool;
    nlohmann::json input;
    input["command"] = "echo hi";
    input["run_in_background"] = "yes";
    Tool::Result result{"", false};
    CHECK_NOTHROW(result = tool.execute(input));
    CHECK(result.is_error);
    CHECK(result.content.find("run_in_background") != std::string::npos);
}

TEST_CASE("run_command: 缺少必填参数 command,报错不崩") {
    RunCommandTool tool;
    nlohmann::json input = nlohmann::json::object();
    const Tool::Result result = tool.execute(input);

    CHECK(result.is_error);
}

TEST_CASE("run_command: shell 参数写了不认得的值,报错不崩") {
    RunCommandTool tool;
    nlohmann::json input;
    input["command"] = "echo hi";
    input["shell"] = "bash";
    const Tool::Result result = tool.execute(input);

    CHECK(result.is_error);
    CHECK(result.content.find("shell") != std::string::npos);
}

#ifndef _WIN32

TEST_CASE("run_command(POSIX): shell=powershell/cmd 明说不支持,不悄悄换 shell") {
    RunCommandTool tool;
    for (const char* shell : {"powershell", "cmd"}) {
        nlohmann::json input;
        input["command"] = "echo hi";
        input["shell"] = shell;
        const Tool::Result result = tool.execute(input);
        CHECK(result.is_error);
        CHECK(result.content.find("Windows") != std::string::npos);
    }
}

TEST_CASE("run_command(POSIX): shell=sh 显式指定,照常执行") {
    RunCommandTool tool;
    nlohmann::json input;
    input["command"] = "echo hello-from-sh-test";
    input["shell"] = "sh";
    const Tool::Result result = tool.execute(input);
    CHECK_FALSE(result.is_error);
    CHECK(result.content.find("hello-from-sh-test") != std::string::npos);
    CHECK(result.content.find("[退出码 0]") != std::string::npos);
}

#endif  // !_WIN32

#ifdef _WIN32

TEST_CASE("run_command: shell=cmd,跑 echo,拿到输出和退出码 0") {
    RunCommandTool tool;
    nlohmann::json input;
    input["command"] = "echo hello-from-cmd-test";
    input["shell"] = "cmd";
    const Tool::Result result = tool.execute(input);

    CHECK_FALSE(result.is_error);
    CHECK(result.content.find("hello-from-cmd-test") != std::string::npos);
    CHECK(result.content.find("[退出码 0]") != std::string::npos);
}

TEST_CASE("run_command: shell=cmd,中文输出不乱码") {
    RunCommandTool tool;
    nlohmann::json input;
    input["command"] = "echo 你好世界";
    input["shell"] = "cmd";
    const Tool::Result result = tool.execute(input);

    CHECK_FALSE(result.is_error);
    CHECK(result.content.find("你好世界") != std::string::npos);
}

TEST_CASE("run_command: shell=cmd,非零退出码,is_error 置位") {
    RunCommandTool tool;
    nlohmann::json input;
    input["command"] = "exit 3";
    input["shell"] = "cmd";
    const Tool::Result result = tool.execute(input);

    CHECK(result.is_error);
    CHECK(result.content.find("3") != std::string::npos);
}

#endif  // _WIN32

// ---------------------------------------------------------------------------
// 0.13.1 加固:参数类型校验、读取/输出上限
// ---------------------------------------------------------------------------

#include "platform/process.hpp"

TEST_CASE("run_command: timeout_ms 传成字符串,返回 is_error,不抛异常") {
    RunCommandTool tool;
    nlohmann::json input;
    input["command"] = "echo hi";
    input["timeout_ms"] = "很快";
    Tool::Result result{"", false};
    CHECK_NOTHROW(result = tool.execute(input));
    CHECK(result.is_error);
    CHECK(result.content.find("timeout_ms") != std::string::npos);
}

TEST_CASE("run_command: timeout_ms 传成数组,返回 is_error,不抛异常") {
    RunCommandTool tool;
    nlohmann::json input;
    input["command"] = "echo hi";
    input["timeout_ms"] = nlohmann::json::array({1000});
    Tool::Result result{"", false};
    CHECK_NOTHROW(result = tool.execute(input));
    CHECK(result.is_error);
}

TEST_CASE("read_file: offset/limit 传成字符串,返回 is_error,不抛异常") {
    TempFile file("a\nb\nc\n");
    ReadFileTool tool;

    nlohmann::json input;
    input["path"] = file.Utf8Path();
    input["offset"] = "2";
    Tool::Result result{"", false};
    CHECK_NOTHROW(result = tool.execute(input));
    CHECK(result.is_error);
    CHECK(result.content.find("offset") != std::string::npos);

    nlohmann::json input2;
    input2["path"] = file.Utf8Path();
    input2["limit"] = "abc";
    CHECK_NOTHROW(result = tool.execute(input2));
    CHECK(result.is_error);
    CHECK(result.content.find("limit") != std::string::npos);
}

TEST_CASE("read_file: 不给 limit 默认最多 2000 行,截断标注告知 offset 翻页") {
    std::string content;
    for (int i = 1; i <= 2300; ++i) {
        content += "line-" + std::to_string(i) + "\n";
    }
    TempFile file(content);
    ReadFileTool tool;

    nlohmann::json input;
    input["path"] = file.Utf8Path();
    const Tool::Result result = tool.execute(input);

    CHECK_FALSE(result.is_error);
    CHECK(result.content.find("line-2000") != std::string::npos);
    CHECK(result.content.find("line-2001") == std::string::npos);
    CHECK(result.content.find("[内容过长已截断") != std::string::npos);
    CHECK(result.content.find("offset=2001") != std::string::npos);

    // 按标注翻页,能接着读到后面的行,并且读到尾就不再标注截断。
    nlohmann::json page2;
    page2["path"] = file.Utf8Path();
    page2["offset"] = 2001;
    const Tool::Result result2 = tool.execute(page2);
    CHECK_FALSE(result2.is_error);
    CHECK(result2.content.find("line-2001") != std::string::npos);
    CHECK(result2.content.find("line-2300") != std::string::npos);
    CHECK(result2.content.find("[内容过长已截断") == std::string::npos);
}

TEST_CASE("read_file: 刚好读到文件末尾,不标注截断") {
    TempFile file("a\nb\nc\n");
    ReadFileTool tool;
    nlohmann::json input;
    input["path"] = file.Utf8Path();
    input["limit"] = 3;
    const Tool::Result result = tool.execute(input);
    CHECK_FALSE(result.is_error);
    CHECK(result.content.find("[内容过长已截断") == std::string::npos);
}

TEST_CASE("RunProcess: 输出超上限被截断,进程连进程树一起被杀,按时返回") {
    using lubancode::platform::ProcessResult;
    using lubancode::platform::RunProcess;

#ifdef _WIN32
    // cmd 的无限循环狂写输出;上限压到 16KB,几乎立刻触发。
    const std::wstring cmdline = lubancode::platform::BuildCmdCommandLine(
        "for /L %i in (1,0,2) do @echo 0123456789012345678901234567890123456789");

    const auto started = std::chrono::steady_clock::now();
    const ProcessResult proc = RunProcess(cmdline, /*timeout_ms=*/60000, {}, /*max_output_bytes=*/16 * 1024);
#else
    // sh 的无限循环狂写输出;走可移植的 argv 入口。
    const std::vector<std::string> argv = {
        "/bin/sh", "-c", "while :; do echo 0123456789012345678901234567890123456789; done"};

    const auto started = std::chrono::steady_clock::now();
    const ProcessResult proc = RunProcess(argv, /*timeout_ms=*/60000, {}, /*max_output_bytes=*/16 * 1024);
#endif
    const auto elapsed = std::chrono::steady_clock::now() - started;

    CHECK_FALSE(proc.spawn_failed);
    CHECK_FALSE(proc.timed_out);  // 是超上限被杀,不是超时
    CHECK(proc.output_truncated);
    CHECK(proc.output.size() <= 16 * 1024);
    CHECK_FALSE(proc.output.empty());
    // 远小于 60s 的超时:说明确实是靠上限提前掐掉的,读线程也没吊死。
    CHECK(std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count() < 15000);
}

TEST_CASE("RunProcessBackground/RunShellCommandBackground: 平台层直接调,spawn 立刻返回、日志真写了东西") {
    using lubancode::platform::BackgroundSpawnResult;
    using lubancode::platform::RunShellCommandBackground;

    const auto started = std::chrono::steady_clock::now();
    const BackgroundSpawnResult bg = RunShellCommandBackground("echo hello-from-platform-bg");
    const auto elapsed = std::chrono::steady_clock::now() - started;

    CHECK(bg.success);
    CHECK(bg.pid != 0);
    CHECK_FALSE(bg.log_path.empty());
    // spawn 不等待 —— echo 这么快的命令,平台层调用本身也该是毫秒级返回。
    CHECK(std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count() < 3000);

    std::string log_content;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (std::chrono::steady_clock::now() < deadline) {
        std::ifstream log_file(bg.log_path, std::ios::binary);
        if (log_file) {
            std::ostringstream ss;
            ss << log_file.rdbuf();
            log_content = ss.str();
            if (log_content.find("hello-from-platform-bg") != std::string::npos) {
                break;
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    CHECK(log_content.find("hello-from-platform-bg") != std::string::npos);

    std::error_code ec;
    std::filesystem::remove(bg.log_path, ec);
}

TEST_CASE("run_command: 海量输出被截断并标注,不吃光内存") {
    RunCommandTool tool;
    nlohmann::json input;
#ifdef _WIN32
    // PowerShell 一口气吐 ~6MB(超过 2MB 上限)。
    input["command"] = "1..200000 | ForEach-Object { '0123456789012345678901234567890' }";
#else
    // sh 一口气吐 ~6.4MB(超过 2MB 上限)。
    input["command"] = "i=0; while [ $i -lt 200000 ]; do echo 0123456789012345678901234567890; i=$((i+1)); done";
#endif
    input["timeout_ms"] = 90000;

    const Tool::Result result = tool.execute(input);

    CHECK(result.content.find("[输出超过上限") != std::string::npos);
    // 截断在 2MB 上限附近,不会整整 6MB 全吞进来。
    CHECK(result.content.size() <= 2 * 1024 * 1024 + 4096);
}

// ---------------------------------------------------------------------------
// 0.22.6 修复:PowerShell 5.1 遇到解析错误(比如命令里带 `&&`,PS 5.1 根本
// 不认这个语法)会在真正执行到 [Console]::OutputEncoding=UTF8 之前就把
// 错误文本吐出来,走的是系统 ANSI 代码页(国内机器是 GBK)。这坨非法
// UTF-8 字节一旦被当 UTF-8 塞进 nlohmann::json,序列化时就是
// type_error.316,异常穿透到顶层,整个会话崩掉。见
// platform/text_encoding.hpp 的 IsValidUtf8/SanitizeUtf8。
// ---------------------------------------------------------------------------

TEST_CASE("SanitizeUtf8: 合法 UTF-8 原样通过") {
    using lubancode::platform::SanitizeUtf8;
    const std::string legal = "hello 你好世界 123";
    CHECK(SanitizeUtf8(legal) == legal);
}

TEST_CASE("SanitizeUtf8: 空串原样返回") {
    using lubancode::platform::SanitizeUtf8;
    CHECK(SanitizeUtf8("") == "");
}

TEST_CASE("IsValidUtf8: 合法/非法字节串判定") {
    using lubancode::platform::IsValidUtf8;
    CHECK(IsValidUtf8(""));
    CHECK(IsValidUtf8("hello 你好世界"));
    // "你好" 的 GBK 字节(C4 E3 BA C3):C4 起头按 UTF-8 该是 2 字节序列,
    // 后随字节 E3 的高两位是 11 不是 10,不是合法延续字节。
    CHECK_FALSE(IsValidUtf8(std::string("\xC4\xE3\xBA\xC3", 4)));
    // 截断的多字节序列(E4 BD 是"你" E4 BD A0 的前两字节,缺最后一个)。
    CHECK_FALSE(IsValidUtf8(std::string("\xE4\xBD", 2)));
    // 单独一个延续字节,不是任何序列的开头。
    CHECK_FALSE(IsValidUtf8(std::string("\x80", 1)));
}

TEST_CASE("SanitizeUtf8: GBK 样本转换正确") {
    using lubancode::platform::IsValidUtf8;
    using lubancode::platform::SanitizeUtf8;

    // "你好" 的 GBK/CP936 字节:你=C4 E3,好=BA C3。
    const std::string gbk_bytes("\xC4\xE3\xBA\xC3", 4);
    REQUIRE_FALSE(IsValidUtf8(gbk_bytes));  // 先确认样本本身确实不是合法 UTF-8

    const std::string sanitized = SanitizeUtf8(gbk_bytes);
    CHECK(IsValidUtf8(sanitized));  // 不管走哪条分支,出口必须合法

#ifdef _WIN32
    // 本机(以及绝大多数国内 Windows 机器)系统 ANSI 代码页就是 936/GBK,
    // AcpBytesToUtf8 应该能把这 4 个字节精确解回"你好"。代码页不是 936 的
    // 机器上(海外机器)按 ACP 转出来的东西不可预测,退化成只断言合法。
    if (::GetACP() == 936) {
        CHECK(sanitized == "\xE4\xBD\xA0\xE5\xA5\xBD");  // "你好" 的 UTF-8 字节
    }
#endif
}

TEST_CASE("SanitizeUtf8: 垃圾字节保底替换,出口一定合法 UTF-8") {
    using lubancode::platform::IsValidUtf8;
    using lubancode::platform::SanitizeUtf8;

    // 0xF9(bug 现场里那个越界字节)混在正常 ASCII 中间。
    const std::string garbage = "A\xF9matched...\x80\xFFZ";
    REQUIRE_FALSE(IsValidUtf8(garbage));

    const std::string sanitized = SanitizeUtf8(garbage);
    CHECK(IsValidUtf8(sanitized));
    // 合法的 ASCII 片段原样保留,不该被误伤。
    CHECK(sanitized.find('A') != std::string::npos);
    CHECK(sanitized.find('Z') != std::string::npos);

#ifndef _WIN32
    // 非 Windows 平台没有 ACP 兜底那一层,直接走"逐段替换成 U+FFFD"的
    // 纯逻辑,行为可以精确断言。
    CHECK(sanitized.find("\xEF\xBF\xBD") != std::string::npos);
#endif
}

TEST_CASE("nlohmann::json: SanitizeUtf8 清洗过的坏字节,序列化不会再抛 type_error.316") {
    using lubancode::platform::SanitizeUtf8;
    const std::string garbage = "bad\xF9" "byte";  // 拆成两段字面量,免得 \xF9b 被解成一个越界的三位十六进制转义
    const std::string sanitized = SanitizeUtf8(garbage);
    nlohmann::json content_only_json;
    CHECK_NOTHROW(content_only_json = sanitized);
    CHECK_NOTHROW(content_only_json.dump());
}

#ifdef _WIN32

TEST_CASE("run_command: PowerShell 解析错误(&&)吐出的坏字节,清洗后是合法 UTF-8,json 不炸") {
    // 复现真实 bug 现场:用户命令里带 `&&`,PowerShell 5.1 根本不认这个
    // 语法,脚本解析期就报错——报错文本走系统 ANSI 代码页(GBK),在
    // [Console]::OutputEncoding=UTF8 那行生效之前就吐出来了。
    RunCommandTool tool;
    nlohmann::json input;
    input["command"] = "cd C:\\ && dir";
    const Tool::Result result = tool.execute(input);

    // 不同 PowerShell 版本/环境对这条命令的具体反应可能有差异(比如装了
    // PowerShell 7 且被优先命中就不会报解析错误),这里不强行断言
    // is_error;真正要保证的是不管吐出什么,content 必须是合法 UTF-8,且
    // 塞进 nlohmann::json 序列化不能抛异常——这正是本次要修的 bug。
    CHECK(lubancode::platform::IsValidUtf8(result.content));
    nlohmann::json wrapped;
    CHECK_NOTHROW(wrapped = result.content);
    CHECK_NOTHROW(wrapped.dump());
}

#endif  // _WIN32
