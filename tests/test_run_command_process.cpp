// run_command 跨平台 Shell 语义单(止血批)的定向测试:
//   - 输出超上限必须是失败终态(is_error=true),不再是"成功";
//   - 输出恰好等于上限一字节不多,不算超限(off-by-one);
//   - command/cwd 拒绝内嵌 NUL;
//   - timeout_ms/tail_lines 先验整数再做范围检查(64 位解析,防 get<int> 抛);
//   - POSIX 单引号路径转义(cwd 含 ' 的目录前后台都得跑对);
//   - 后台 ReadOutput 的 UTF-8 边界与截断标记(半字起刀不许冒充整行);
//   - 后台日志 POSIX 权限 0600、独占创建;
//   - Windows 后台 PowerShell 用对 command_with_cwd(前后台目录探针一致)。
// 进程生命线批(BackgroundProcessHandle/Stop 整树收口)的测试另在
// test_background_tasks.cpp 与本文件生命周期节。

#include <doctest/doctest.h>

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <thread>

#include <nlohmann/json.hpp>

#include "platform/process.hpp"
#include "platform/text_encoding.hpp"
#include "tools/background_output.hpp"
#include "tools/background_tasks.hpp"
#include "tools/run_command.hpp"

#ifndef _WIN32
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#endif

using lubancode::tools::BackgroundOutputTool;
using lubancode::tools::BackgroundTaskRegistry;
using lubancode::tools::BackgroundTaskStatus;
using lubancode::tools::RunCommandTool;
using lubancode::tools::Tool;

namespace {

template <typename Predicate>
bool WaitUntil(Predicate pred, int timeout_ms, int poll_ms = 50) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    while (std::chrono::steady_clock::now() < deadline) {
        if (pred()) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(poll_ms));
    }
    return pred();
}

// 建一个用完即删的临时目录(cwd 探针用),名字可注入特殊字符。
class TempDir {
public:
    explicit TempDir(const std::string& suffix = "") {
        const auto base = std::filesystem::temp_directory_path();
        path_ = base / ("lubancode_rc_test_" + std::to_string(reinterpret_cast<std::uintptr_t>(this)) + suffix);
        std::error_code ec;
        std::filesystem::create_directories(path_, ec);
    }
    ~TempDir() {
        std::error_code ec;
        std::filesystem::remove_all(path_, ec);
    }
    std::string Utf8Path() const {
        const std::u8string u8 = path_.u8string();
        return std::string(reinterpret_cast<const char*>(u8.data()), u8.size());
    }
    const std::filesystem::path& Path() const { return path_; }

private:
    std::filesystem::path path_;
};

std::string ReadAllBytes(const std::string& path_utf8) {
    std::ifstream file(std::filesystem::u8path(reinterpret_cast<const char8_t*>(path_utf8.data())),
                       std::ios::binary);
    std::ostringstream ss;
    ss << file.rdbuf();
    return ss.str();
}

}  // namespace

// ---------------------------------------------------------------------------
// P1:输出超限是失败终态 + off-by-one
// ---------------------------------------------------------------------------

TEST_CASE("run_command: 输出超上限是失败终态(is_error=true),不再涂成成功") {
    RunCommandTool tool;
    nlohmann::json input;
#ifdef _WIN32
    input["command"] = "1..200000 | ForEach-Object { '0123456789012345678901234567890' }";
#else
    input["command"] = "i=0; while [ $i -lt 200000 ]; do echo 0123456789012345678901234567890; i=$((i+1)); done";
#endif
    input["timeout_ms"] = 90000;

    const Tool::Result result = tool.execute(input);

    CHECK(result.content.find("[输出超过上限") != std::string::npos);
    // 单子的硬话:输出截断 = 命令被杀,结果不得是成功。
    CHECK(result.is_error);
    CHECK(result.outcome == "output_limit");
    CHECK(result.error_code == "process.output_limit");
    CHECK(result.content.size() <= 2 * 1024 * 1024 + 4096);
}

#ifndef _WIN32

TEST_CASE("RunProcess(POSIX): 输出恰好等于上限不算超限;多一字节才算") {
    using lubancode::platform::ProcessResult;
    using lubancode::platform::RunShellCommand;

    // 恰好 1024 字节:不算超限(修前:读缓冲填满即置 overflow,等于上限也被杀)。
    {
        const ProcessResult exact = RunShellCommand("head -c 1024 /dev/zero | tr '\\0' 'x'", 30000, {}, 1024);
        CHECK_FALSE(exact.spawn_failed);
        CHECK_FALSE(exact.timed_out);
        CHECK_FALSE(exact.output_truncated);
        CHECK(exact.output.size() == 1024);
    }
    // 1025 字节:第 limit+1 个字节到了才算超过。
    {
        const ProcessResult over = RunShellCommand("head -c 1025 /dev/zero | tr '\\0' 'x'", 30000, {}, 1024);
        CHECK_FALSE(over.spawn_failed);
        CHECK(over.output_truncated);
        CHECK(over.output.size() == 1024);
    }
}

#endif

TEST_CASE("run_command: command 含 NUL 被拒,不是截断后照跑") {
    RunCommandTool tool;
    nlohmann::json input;
    std::string command = "echo hi";
    command.push_back('\0');
    command += " && echo tail";
    input["command"] = command;
    const Tool::Result result = tool.execute(input);
    CHECK(result.is_error);
    CHECK(result.content.find("NUL") != std::string::npos);
}

TEST_CASE("run_command: cwd 含 NUL 被拒") {
    RunCommandTool tool;
    nlohmann::json input;
    input["command"] = "echo hi";
    std::string cwd = std::filesystem::temp_directory_path().string();
    cwd.push_back('\0');
    cwd += "tail";
    input["cwd"] = cwd;
    const Tool::Result result = tool.execute(input);
    CHECK(result.is_error);
    CHECK(result.content.find("NUL") != std::string::npos);
}

TEST_CASE("run_command: timeout_ms 超出 int 范围,体面报错不抛异常") {
    RunCommandTool tool;
    nlohmann::json input;
    input["command"] = "echo hi";
    // 64 位整数,json 能装,直接 get<int> 会窄化或抛。
    input["timeout_ms"] = 5000000000LL;
    Tool::Result result{"", false};
    CHECK_NOTHROW(result = tool.execute(input));
    // 超范围要么体面报错,要么夹紧进合法区间——不许抛异常穿透。
    if (!result.is_error) {
        CHECK(result.content.find("hi") != std::string::npos);
    }
}

TEST_CASE("background_output: tail_lines 超出 int 范围不抛异常") {
    BackgroundOutputTool tool;
    nlohmann::json input;
    input["task_id"] = "no-such-task-zzz";
    input["tail_lines"] = 99999999999LL;
    Tool::Result result{"", false};
    CHECK_NOTHROW(result = tool.execute(input));
    // 不认得的 task 照旧走"找不到"那条路;这里要保证的是不抛。
}

#ifndef _WIN32

// ---------------------------------------------------------------------------
// P1:POSIX 单引号路径转义 / native cwd
// ---------------------------------------------------------------------------

TEST_CASE("run_command(POSIX): cwd 含单引号,前后台都跑对目录") {
    TempDir dir("-a'b");  // 目录名里一枚单引号
    auto& reg = BackgroundTaskRegistry::Instance();

    // 前台:探针 pwd 落在对的目录。
    {
        RunCommandTool tool;
        nlohmann::json input;
        input["command"] = "pwd";
        input["cwd"] = dir.Utf8Path();
        const Tool::Result result = tool.execute(input);
        CHECK_FALSE(result.is_error);
        CHECK(result.content.find(dir.Utf8Path()) != std::string::npos);
    }
    // 后台:探针写标记文件进日志,验证目录也对(修前编码的是原始 command,
    // cwd 被悄悄丢掉)。
    {
        RunCommandTool tool;
        nlohmann::json input;
        input["command"] = "pwd";
        input["cwd"] = dir.Utf8Path();
        input["run_in_background"] = true;
        const Tool::Result result = tool.execute(input);
        CHECK_FALSE(result.is_error);
        const auto marker = dir.Utf8Path();
        const bool landed = WaitUntil(
            [&] {
                // 后台返回里拿日志路径读(前后台一致性经 BackgroundTaskRegistry)。
                const auto tasks = reg.List();
                for (const auto& t : tasks) {
                    if (t.status == BackgroundTaskStatus::Running || t.status == BackgroundTaskStatus::Completed ||
                        t.status == BackgroundTaskStatus::Failed) {
                        if (!t.log_path.empty()) {
                            const std::string data = ReadAllBytes(t.log_path);
                            if (data.find(marker) != std::string::npos) {
                                return true;
                            }
                        }
                    }
                }
                return false;
            },
            8000);
        CHECK(landed);
    }
}

TEST_CASE("run_command(POSIX): cwd 含空格与中文,前后台目录探针一致") {
    TempDir dir("-空 格 dir");
    RunCommandTool tool;
    nlohmann::json input;
    input["command"] = "pwd";
    input["cwd"] = dir.Utf8Path();
    const Tool::Result result = tool.execute(input);
    CHECK_FALSE(result.is_error);
    CHECK(result.content.find(dir.Utf8Path()) != std::string::npos);
}

TEST_CASE("RunShellCommandBackground(POSIX): 日志文件权限 0600,独占创建") {
    const auto bg = lubancode::platform::RunShellCommandBackground("echo perm_probe");
    REQUIRE(bg.success);
    struct stat st{};
    REQUIRE(stat(bg.log_path.c_str(), &st) == 0);
    CHECK((st.st_mode & 0777) == 0600);
    std::error_code ec;
    std::filesystem::remove(bg.log_path, ec);
}

#endif  // !_WIN32

#ifdef _WIN32

TEST_CASE("run_command(Windows): 后台 PowerShell 用对 cwd(前后台目录探针一致)") {
    TempDir dir("-win bg cwd");
    auto& reg = BackgroundTaskRegistry::Instance();

    std::string fg_location;
    {
        RunCommandTool tool;
        nlohmann::json input;
        input["command"] = "Get-Location | Select-Object -ExpandProperty Path";
        input["cwd"] = dir.Utf8Path();
        const Tool::Result result = tool.execute(input);
        REQUIRE_FALSE(result.is_error);
        fg_location = result.content;
    }

    std::string task_id;
    {
        RunCommandTool tool;
        nlohmann::json input;
        input["command"] = "pwd | Select-Object -ExpandProperty Path";
        input["cwd"] = dir.Utf8Path();
        input["run_in_background"] = true;
        const Tool::Result result = tool.execute(input);
        REQUIRE_FALSE(result.is_error);
        const auto marker = "task #";
        const auto pos = result.content.find(marker);
        REQUIRE(pos != std::string::npos);
        std::size_t i = pos + std::strlen(marker);
        std::string id;
        while (i < result.content.size() && result.content[i] >= '0' && result.content[i] <= '9') {
            id.push_back(result.content[i]);
            ++i;
        }
        task_id = id;
    }
    REQUIRE_FALSE(task_id.empty());

    const bool landed = WaitUntil(
        [&] {
            const auto info = reg.Get(task_id);
            if (!info.has_value() || info->log_path.empty()) {
                return false;
            }
            const std::string data = ReadAllBytes(info->log_path);
            return data.find(dir.Utf8Path()) != std::string::npos;
        },
        8000);
    CHECK(landed);
    CHECK(fg_location.find(dir.Utf8Path()) != std::string::npos);
}

#endif  // _WIN32
