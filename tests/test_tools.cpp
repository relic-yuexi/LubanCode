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
    input["command"] = "Start-Sleep -Seconds 20";
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
