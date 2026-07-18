// read_file:读临时文件(含中文内容)、行号、offset/limit、不存在的文件。
// run_command:跑 echo、非零退出码、超时(用 PowerShell 的 Start-Sleep 制造)。

#include <doctest/doctest.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>

#include "tools/read_file.hpp"
#include "tools/run_command.hpp"

using lubancode::tools::ReadFileTool;
using lubancode::tools::RunCommandTool;
using lubancode::tools::Tool;

namespace {

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
