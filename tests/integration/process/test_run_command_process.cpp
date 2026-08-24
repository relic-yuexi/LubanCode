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

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include <nlohmann/json.hpp>

#include "platform/process.hpp"
#include "platform/text_encoding.hpp"
#include "tools/background_output.hpp"
#include "tools/background_tasks.hpp"
#include "tools/path_utils.hpp"
#include "tools/run_command.hpp"
#include "tools/shell_info.hpp"

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
    std::ifstream file(lubancode::tools::Utf8ToPath(path_utf8), std::ios::binary);
    std::ostringstream ss;
    ss << file.rdbuf();
    return ss.str();
}

bool RefersToPath(const std::string& output_utf8, const std::filesystem::path& expected) {
    std::istringstream lines(output_utf8);
    for (std::string line; std::getline(lines, line);) {
        const auto first = line.find_first_not_of(" \t\r\n");
        if (first == std::string::npos) {
            continue;
        }
        const auto last = line.find_last_not_of(" \t\r\n");
        line = line.substr(first, last - first + 1);
        std::error_code ec;
        if (std::filesystem::equivalent(lubancode::tools::Utf8ToPath(line), expected, ec) && !ec) {
            return true;
        }
    }
    return false;
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

TEST_CASE("RunProcess: 输出帽的刀口对齐 UTF-8 边界,中文不劈半个字(316 根因)") {
    using lubancode::platform::RunShellCommand;
    // 修前:字节刀把三字节汉字拦腰斩断,0xE5 开头的半截汉字流进
    // nlohmann::json 序列化就是 type_error.316(0.26.41 真机崩)。
    // 造一段中文字流,让帽恰好压在某个汉字的腰上:截断后必须仍是合法
    // UTF-8 且长度 <= 上限。
    // 409 个"汉"字 = 1227 字节,帽压在 1024:第 342 个汉字被劈(1024 =
    // 341*3 + 1,第 342 字只吃到首字节 0xE5)。
    {
        const auto result = RunShellCommand("i=0; while [ $i -lt 409 ]; do printf '汉'; i=$((i+1)); done",
                                            30000, {}, 1024);
        REQUIRE_FALSE(result.spawn_failed);
        CHECK(result.output_truncated);
        CHECK(result.output.size() <= 1024);
        CHECK(result.output.size() % 3 == 0);  // 整字收口,没有半截尾巴
        CHECK(lubancode::platform::IsValidUtf8(result.output));
        // 刀口还给了上限,但不多给:1024 压在字腰上,退到 1023(341 个整字)。
        CHECK(result.output.size() == 1023);
    }
    // 帽恰好落在字缝上(1023 = 341*3):一字节不退,照旧满帽。
    {
        const auto result = RunShellCommand("i=0; while [ $i -lt 409 ]; do printf '汉'; i=$((i+1)); done",
                                            30000, {}, 1023);
        REQUIRE_FALSE(result.spawn_failed);
        CHECK(result.output_truncated);
        CHECK(result.output.size() == 1023);
        CHECK(lubancode::platform::IsValidUtf8(result.output));
    }
    // WithStdin 路径(hooks v2 / RunProcessWithStdin)同款刀口:那一路是全量
    // 攒、超限置旗后再对齐,stdout 与 stderr 各自 <= 帽且合法。
    {
        const auto result = lubancode::platform::RunShellCommandWithStdin(
            "i=0; while [ $i -lt 409 ]; do printf '汉'; i=$((i+1)); done; printf '汉' >&2", "",
            30000, {}, 1024);
        REQUIRE_FALSE(result.spawn_failed);
        CHECK(result.output_truncated);
        CHECK(result.stdout_bytes.size() <= 1024);
        CHECK(result.stderr_bytes.size() <= 1024);
        CHECK(lubancode::platform::IsValidUtf8(result.stdout_bytes));
        CHECK(lubancode::platform::IsValidUtf8(result.stderr_bytes));
        // 汉字三字节整除:整字收口,没有半截尾巴。
        CHECK(result.stdout_bytes.size() % 3 == 0);
        CHECK(result.stderr_bytes.size() % 3 == 0);
    }
}

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

TEST_CASE("run_command: command 超长被拒,不硬顶系统命令行") {
    RunCommandTool tool;
    nlohmann::json input;
    input["command"] = std::string(30000, 'x');
    const Tool::Result result = tool.execute(input);
    CHECK(result.is_error);
    CHECK(result.content.find("太长") != std::string::npos);
    CHECK(result.content.find("write_file") != std::string::npos);
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

    std::string observed_background;
    const bool landed = WaitUntil(
        [&] {
            const auto info = reg.Get(task_id);
            if (!info.has_value() || info->log_path.empty()) {
                return false;
            }
            observed_background = ReadAllBytes(info->log_path);
            return RefersToPath(observed_background, dir.Path());
        },
        8000);
    CAPTURE(dir.Utf8Path());
    CAPTURE(fg_location);
    CAPTURE(observed_background);
    CHECK(landed);
    CHECK(RefersToPath(fg_location, dir.Path()));
}

#endif  // _WIN32

// ---------------------------------------------------------------------------
// 进程生命线批(P0/P1):Register 竞态、退出码不丢、Stop 整树收口、
// 快进程连起不留永久 Running、UTF-8 尾读边界、前台取消。
// ---------------------------------------------------------------------------

#ifndef _WIN32

TEST_CASE("background(POSIX): 后台 exit 7 稳定报 7,不再偶发 Completed/0") {
    auto& reg = BackgroundTaskRegistry::Instance();
    // 修前:平台层 detach 线程 waitpid 丢了 status,watcher kill(pid,0) 探活,
    // 进程一死 exit_code 乐观填 0 —— exit 7 报成"完成(退出码 0)"。
    const auto bg = lubancode::platform::RunShellCommandBackground("exit 7");
    REQUIRE(bg.success);
    REQUIRE(bg.handle != nullptr);

    const std::string task_id = reg.Register("exit 7", "sh", bg.pid, bg.log_path, bg.handle);

    const bool finished = WaitUntil(
        [&] {
            const auto info = reg.Get(task_id);
            return info.has_value() && info->status != BackgroundTaskStatus::Running &&
                   info->status != BackgroundTaskStatus::Stopping;
        },
        8000);
    REQUIRE(finished);

    const auto info = reg.Get(task_id);
    REQUIRE(info.has_value());
    CHECK(info->status == BackgroundTaskStatus::Failed);
    REQUIRE(info->exit.exit_code.has_value());
    CHECK(*info->exit.exit_code == 7);
    CHECK_FALSE(info->exit.signal.has_value());
}

TEST_CASE("background(POSIX): 快进程连起数百枚,每枚都进终态,无永久 Running") {
    auto& reg = BackgroundTaskRegistry::Instance();
    constexpr int kCount = 300;
    std::vector<std::string> task_ids;
    task_ids.reserve(kCount);
    for (int i = 0; i < kCount; ++i) {
        // 0ms/1ms 短命命令:最容易撞上"watcher 抢先跑,表里找不到自己"
        // 那枚 P0 竞态(修前次序是先起线程后进表)。
        const auto bg = lubancode::platform::RunShellCommandBackground("true");
        REQUIRE(bg.success);
        task_ids.push_back(reg.Register("true", "sh", bg.pid, bg.log_path, bg.handle));
    }
    const bool all_terminal = WaitUntil(
        [&] {
            for (const auto& id : task_ids) {
                const auto info = reg.Get(id);
                if (!info.has_value() || info->status == BackgroundTaskStatus::Running ||
                    info->status == BackgroundTaskStatus::Stopping) {
                    return false;
                }
            }
            return true;
        },
        /*timeout_ms=*/20000);
    CHECK(all_terminal);
    // 而且都是真终态:exit 0 的 Completed,不是未知的退化账。
    int completed = 0;
    for (const auto& id : task_ids) {
        const auto info = reg.Get(id);
        if (info.has_value() && info->status == BackgroundTaskStatus::Completed) {
            ++completed;
        }
    }
    CHECK(completed == kCount);
}

TEST_CASE("background(POSIX): Stop 收掉整棵树(根、子、孙),孙进程真死透") {
    auto& reg = BackgroundTaskRegistry::Instance();
    // sh -> sleep(子) -> sleep(孙):杀根进程组,两代都得死。
    const auto bg = lubancode::platform::RunShellCommandBackground(
        "sleep 30 & sleep 30 & wait");
    REQUIRE(bg.success);
    REQUIRE(bg.handle != nullptr);
    const std::string task_id = reg.Register("sleep tree", "sh", bg.pid, bg.log_path, bg.handle);

    // 给 sh 一拍把两个子进程拉起来。
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    {
        const auto info = reg.Get(task_id);
        REQUIRE(info.has_value());
        CHECK(info->status == BackgroundTaskStatus::Running);
    }

    CHECK(reg.Stop(task_id));

    const bool stopped = WaitUntil(
        [&] {
            const auto info = reg.Get(task_id);
            return info.has_value() && info->status == BackgroundTaskStatus::Stopped;
        },
        8000);
    CHECK(stopped);

    // 根进程真死了(setsid 的 pid,kill(pid,0) 探不到 = 死)。
    CHECK_FALSE(lubancode::platform::IsProcessAlive(bg.pid));
}

TEST_CASE("background(POSIX): ReadOutput 尾读从换行边界起刀,半行加省略标记") {
    auto& reg = BackgroundTaskRegistry::Instance();
    // 造一份超 64KB 的日志:一行超长的(没有换行可退,只能从 UTF-8 边界退)
    // 加几行短的。首段被截必须带标记。
    std::string content;
    content += "pad";
    content.append(70 * 1024, 'x');  // 70KB 无换行:64KB 尾读必从中间切
    content += "\nline-tail-1\nline-tail-2\n";
    std::string log_path = (std::filesystem::temp_directory_path() / "lubancode_bg_tail_test.log").string();
    {
        std::ofstream file(log_path, std::ios::binary);
        file << content;
    }
    const std::string task_id = reg.Register("tail probe", "sh", /*pid=*/1, log_path);

    const std::string tail = reg.ReadOutput(task_id, /*tail_lines=*/0);
    CHECK(tail.find("[日志前部已省略]") != std::string::npos);
    CHECK(tail.find("line-tail-1") != std::string::npos);
    CHECK(tail.find("line-tail-2") != std::string::npos);
    // 出口必须合法 UTF-8。
    CHECK(lubancode::platform::IsValidUtf8(tail));
    CHECK(tail.size() <= 64 * 1024 + 256);

    std::error_code ec;
    std::filesystem::remove(lubancode::tools::Utf8ToPath(log_path), ec);
}

TEST_CASE("background(POSIX): ReadOutput 坏 UTF-8 日志清洗后出口合法") {
    auto& reg = BackgroundTaskRegistry::Instance();
    const std::string bad = std::string("ok\xC4\xE3\xBA\xC3gbk-tail\n", 16) + std::string("tail-end\n");
    std::string log_path = (std::filesystem::temp_directory_path() / "lubancode_bg_bad_utf8.log").string();
    {
        std::ofstream file(log_path, std::ios::binary);
        file << bad;
    }
    const std::string task_id = reg.Register("bad utf8", "sh", /*pid=*/1, log_path);
    const std::string out = reg.ReadOutput(task_id, 0);
    CHECK(lubancode::platform::IsValidUtf8(out));
    CHECK(out.find("tail-end") != std::string::npos);

    std::error_code ec;
    std::filesystem::remove(lubancode::tools::Utf8ToPath(log_path), ec);
}

TEST_CASE("run_command(POSIX): 前台命令带 cancel 旗,置位即收树返回 cancelled") {
    RunCommandTool tool;
    std::atomic<bool> cancel_flag{false};
    tool.SetCancel(&cancel_flag);

    // 1 秒后另一线程置取消旗;命令本身要跑 30 秒。
    std::thread setter([&cancel_flag] {
        std::this_thread::sleep_for(std::chrono::milliseconds(1000));
        cancel_flag.store(true);
    });

    nlohmann::json input;
    input["command"] = "sleep 30";
    input["timeout_ms"] = 60000;  // 超时墙足够远:被取消而不是超时
    const auto started = std::chrono::steady_clock::now();
    const Tool::Result result = tool.execute(input);
    const auto elapsed = std::chrono::steady_clock::now() - started;
    setter.join();

    CHECK(result.is_error);
    CHECK(result.content.find("取消") != std::string::npos);
    CHECK(result.outcome == "cancelled_during_run");
    // ESC 后短时间返回(远小于 30 秒命令时长)。
    CHECK(std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count() < 10000);
}

TEST_CASE("RunProcess(POSIX): cwd 原生参数,不存在时报 spawn 失败定位到目录") {
    const auto result = lubancode::platform::RunShellCommandBackground(
        "pwd", "/definitely/not/a/real/dir/lubancode-test-zzz");
    CHECK_FALSE(result.success);
    CHECK(result.error.find("启动子进程失败") != std::string::npos);
}

TEST_CASE("RunProcessBackground(POSIX): 原生 cwd 真生效(不拼 cd)") {
    TempDir dir("-native-cwd");
    const auto bg = lubancode::platform::RunShellCommandBackground("pwd", dir.Utf8Path());
    REQUIRE(bg.success);
    const bool landed = WaitUntil(
        [&] {
            std::ifstream file(bg.log_path, std::ios::binary);
            std::ostringstream ss;
            ss << file.rdbuf();
            return ss.str().find(dir.Utf8Path()) != std::string::npos;
        },
        8000);
    CHECK(landed);
    std::error_code ec;
    std::filesystem::remove(lubancode::tools::Utf8ToPath(bg.log_path), ec);
}

TEST_CASE("BackgroundProcessHandle(POSIX): Wait/Peek/TerminateTree 的完成态口径") {
    const auto bg = lubancode::platform::RunShellCommandBackground("exit 9");
    REQUIRE(bg.success);
    REQUIRE(bg.handle != nullptr);
    CHECK(bg.handle->Wait(8000));
    const auto completion = bg.handle->Peek();
    CHECK(completion.known);
    CHECK(completion.exit_code == 9);
    CHECK_FALSE(completion.terminated_by_stop);

    // 长命进程走 TerminateTree:SIGTERM 那一拍就退,SIGKILL 兜底不用等。
    const auto bg2 = lubancode::platform::RunShellCommandBackground("sleep 30");
    REQUIRE(bg2.success);
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    CHECK(bg2.handle->TerminateTree(1500));
    const auto completion2 = bg2.handle->Peek();
    CHECK(completion2.known);
    const bool signaled_or_nonzero = completion2.signal != 0 || completion2.exit_code != 0;
    CHECK(signaled_or_nonzero);
    CHECK(completion2.terminated_by_stop);
}

#endif  // !_WIN32

TEST_CASE("RunProcess: 并发 extra_env 不串值(P0 并发契约)") {
    using lubancode::platform::EnvPairs;
    using lubancode::platform::RunShellCommand;
    // 8 只"hook"并发跑,每只只该看见自己的 marker。老路(Windows 临时改
    // 父环境再还原)同拍启动会串值;新契约下任何平台都不许串。
    constexpr int kWorkers = 8;
    std::vector<std::thread> workers;
    std::vector<std::string> seen(kWorkers);
    std::atomic<int> mismatches{0};
    for (int i = 0; i < kWorkers; ++i) {
        workers.emplace_back([&, i] {
            const EnvPairs env{{"LUBANCODE_TEST_MARKER", "worker-" + std::to_string(i)}};
#ifdef _WIN32
            const auto result = RunShellCommand("echo %LUBANCODE_TEST_MARKER%", 30000, env);
#else
            const auto result = RunShellCommand("echo $LUBANCODE_TEST_MARKER", 30000, env);
#endif
            seen[static_cast<std::size_t>(i)] = result.output;
            if (result.output.find("worker-" + std::to_string(i)) == std::string::npos) {
                mismatches.fetch_add(1);
            }
        });
    }
    for (auto& w : workers) {
        w.join();
    }
    CHECK(mismatches.load() == 0);
    // 串值的实锤形态:A 的 marker 出现在 B 的输出里。
    for (int i = 0; i < kWorkers; ++i) {
        for (int j = 0; j < kWorkers; ++j) {
            if (i != j && seen[static_cast<std::size_t>(j)].find("worker-" + std::to_string(i)) != std::string::npos) {
                FAIL("并发 extra_env 串值: worker ", j, " 看到了 worker ", i, " 的 marker");
            }
        }
    }
}

TEST_CASE("RunProcess: env 值含 NUL 被拒(Windows 键名非法同拒)") {
    using lubancode::platform::EnvPairs;
    using lubancode::platform::RunShellCommand;
    {
        std::string value = "v";
        value.push_back('\0');
        const auto r = RunShellCommand("echo hi", 5000, EnvPairs{{"K", value}});
        CHECK(r.spawn_failed);
    }
#ifdef _WIN32
    {
        const auto r1 = RunShellCommand("echo hi", 5000, EnvPairs{{"BAD=KEY", "v"}});
        CHECK(r1.spawn_failed);
        const auto r2 = RunShellCommand("echo hi", 5000, EnvPairs{{"", "v"}});
        CHECK(r2.spawn_failed);
    }
#endif
}

#ifndef _WIN32

TEST_CASE("background(POSIX): max_runtime_ms 到点自动收树,状态进 Stopped") {
    auto& reg = BackgroundTaskRegistry::Instance();
    const auto bg = lubancode::platform::RunShellCommandBackground("sleep 30");
    REQUIRE(bg.success);
    // 1 秒的墙:到点收树,不再等 30 秒,也不改 timeout_ms 旧义。
    const std::string task_id = reg.Register("sleep 30", "sh", bg.pid, bg.log_path, bg.handle,
                                             /*max_runtime_ms=*/1000);
    const bool stopped = WaitUntil(
        [&] {
            const auto info = reg.Get(task_id);
            return info.has_value() && info->status == BackgroundTaskStatus::Stopped;
        },
        /*timeout_ms=*/8000);
    CHECK(stopped);
    CHECK_FALSE(lubancode::platform::IsProcessAlive(bg.pid));
}

#endif  // !_WIN32

TEST_CASE("run_command: max_runtime_ms 传垃圾值体面报错") {
    RunCommandTool tool;
    nlohmann::json input;
    input["command"] = "echo hi";
    input["run_in_background"] = true;
    input["max_runtime_ms"] = "soon";
    Tool::Result result{"", false};
    CHECK_NOTHROW(result = tool.execute(input));
    CHECK(result.is_error);
    CHECK(result.content.find("max_runtime_ms") != std::string::npos);

    nlohmann::json input2;
    input2["command"] = "echo hi";
    input2["run_in_background"] = true;
    input2["max_runtime_ms"] = 99999999999LL;
    Tool::Result result2{"", false};
    CHECK_NOTHROW(result2 = tool.execute(input2));
    CHECK(result2.is_error);
}

TEST_CASE("ProbeShells: 报出本平台的 shell 画像(版本/TTY 语义明牌)") {
    const auto shells = lubancode::tools::ProbeShells();
    REQUIRE_FALSE(shells.empty());
#ifdef _WIN32
    REQUIRE(shells.size() >= 2);
#else
    REQUIRE(shells.size() >= 1);
    CHECK(shells[0].id == "sh");
    CHECK(shells[0].executable == "/bin/sh");
#endif
    // 工具路径下 stdin/stdout 都不是 TTY(无头契约),profile 不加载。
    for (const auto& s : shells) {
        CHECK_FALSE(s.stdin_is_tty);
        CHECK_FALSE(s.stdout_is_tty);
        CHECK_FALSE(s.profile_loaded);
        CHECK_FALSE(s.notes.empty());
    }
}

#ifndef _WIN32

TEST_CASE("run_command(POSIX): shell=bash 装了就真用 bash(Bash 语法跑通)") {
    // 本测试机装了 bash(没装的 CI 环境整条跳过——"无安装便不进 schema")。
    const auto probe = lubancode::platform::RunProcess({"/bin/bash", "-c", "echo ok"}, 5000);
    if (probe.spawn_failed) {
        return;  // 没装 bash:不测(契约允许)
    }
    RunCommandTool tool;
    nlohmann::json input;
    // [[ ]] 是 bash/dash 分水岭:dash 不认(语法错,非零退出)。
    input["command"] = "if [[ -n hello ]]; then echo bash-arrives; fi";
    input["shell"] = "bash";
    const Tool::Result result = tool.execute(input);
    CHECK_FALSE(result.is_error);
    CHECK(result.content.find("bash-arrives") != std::string::npos);
    CHECK(result.content.find("[退出码 0]") != std::string::npos);
}

TEST_CASE("run_command(POSIX): 默认 shell 固定走 /bin/sh,不偷换 /bin/bash") {
    RunCommandTool tool;
    nlohmann::json input;
    // macOS 的 /bin/sh 本就由 Bash 提供,BASH_VERSION 并不为空;拿实现
    // 特征判断会冤枉它。$0 则是调用入口:若宿主把默认 shell 偷换成
    // /bin/bash,这里会直接露出 /bin/bash。
    input["command"] = "printf 'shell0=[%s]\\n' \"$0\"";
    const Tool::Result result = tool.execute(input);
    CHECK_FALSE(result.is_error);
    CHECK(result.content.find("[退出码 0]") != std::string::npos);
    CHECK(result.content.find("shell0=[/bin/sh]") != std::string::npos);
}

#endif  // !_WIN32

#ifdef _WIN32

// ---------------------------------------------------------------------------
// P1:PowerShell 退出码包装(状态在 scriptblock 返回的第一拍捕获,输出
// 格式化不再盖掉 $?)。单子钉的各案:命令找不到、Write-Error、throw、
// native exit 7、native 失败后 cmdlet 成功、cmdlet 失败后 native 成功、
// 显式 exit、多行与解析错误。
// ---------------------------------------------------------------------------

TEST_CASE("run_command(Windows): PowerShell 退出码契约各案") {
    struct Case {
        const char* name;
        const char* command;
        bool expect_error;
    };
    // native 案用 powershell.exe 而不是 cmd.exe:部分机器的策略(AppLocker
    // 类)限制 powershell 起 cmd.exe,用 cmd 会把"策略拦"误判成 wrapper 错。
    const Case cases[] = {
        {"命令找不到", "Get-Command definitely-not-a-cmdlet-zzz", true},
        {"cmdlet Write-Error", "Write-Error 'planned failure'", true},
        {"throw", "throw 'planned throw'", true},
        {"native exit 7", "powershell -NoProfile -Command \"exit 7\"", true},
        {"native 失败后 cmdlet 成功", "powershell -NoProfile -Command \"exit 5\"; Get-Date > $null", true},   // 末次 native 码保留
        {"cmdlet 失败后 native 成功", "Write-Error 'x'; powershell -NoProfile -Command \"exit 0\"", false},  // 末次 native = 0
        {"显式 exit 0", "exit 0", false},
        {"显式 exit 9", "exit 9", true},
        {"多语句末条成功", "Write-Output first; Get-Date > $null", false},
    };
    for (const auto& c : cases) {
        CAPTURE(c.name);
        RunCommandTool tool;
        nlohmann::json input;
        input["command"] = c.command;
        const Tool::Result result = tool.execute(input);
        CHECK(result.is_error == c.expect_error);
    }
}

TEST_CASE("run_command(Windows): shell=pwsh 装了就真用 pwsh 7") {
    // 先探本机装没装(没装整条跳过——"无安装便不进 schema")。
    const auto probe = lubancode::platform::RunProcess(
        lubancode::platform::BuildCmdCommandLine("where pwsh"), 10000);
    if (probe.output.find("pwsh.exe") == std::string::npos) {
        return;
    }
    RunCommandTool tool;
    nlohmann::json input;
    // ?? 运算符是 PowerShell 7 的语法(5.1 不认)。
    input["command"] = "$v = $null; $v ?? 'pwsh-arrives'";
    input["shell"] = "pwsh";
    const Tool::Result result = tool.execute(input);
    CHECK_FALSE(result.is_error);
    CHECK(result.content.find("pwsh-arrives") != std::string::npos);
}

#endif  // _WIN32
