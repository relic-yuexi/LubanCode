// BackgroundTaskRegistry:登记/查询/读输出/完成通知的单测。
// 进程级单例,task_id 跨测试单调递增——不硬编码具体编号,只比较相对大小
// 和拿登记时返回的 task_id 往后查。watcher 线程真起、真探活,端到端测试
// 用 platform 层 spawn 一个短命后台命令验证 DrainCompleted。

#include <doctest/doctest.h>

#include <chrono>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>

#include <nlohmann/json.hpp>

#include "platform/process.hpp"
#include "tools/background_output.hpp"
#include "tools/background_tasks.hpp"

using lubancode::tools::BackgroundTaskRegistry;
using lubancode::tools::BackgroundTaskInfo;
using lubancode::tools::BackgroundTaskStatus;

namespace {

// 轮询等条件成立,最多等 timeout_ms。watcher 探活 200ms 一拍,给足窗口。
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

// 临时日志文件,用完即删。给 ReadOutput 测试用。
class TempLogFile {
public:
    explicit TempLogFile(const std::string& content) {
        path_ = std::filesystem::temp_directory_path() /
                ("lubancode_bg_test_" + std::to_string(reinterpret_cast<std::uintptr_t>(this)) + ".log");
        std::ofstream file(path_, std::ios::binary);
        file << content;
    }
    ~TempLogFile() {
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

TEST_CASE("background: Register 返回单调递增的 task_id,List/Get 能查到") {
    auto& reg = BackgroundTaskRegistry::Instance();

    // 用一个几乎肯定不存在的 pid——watcher 会立刻探到"已结束"标终态,不影响
    // 这里测登记/查询本身。pid 取大数,降低撞上真实进程的概率。
    const unsigned long fake_pid = 40000;
    const std::string id1 = reg.Register("echo first", "sh", fake_pid, "/tmp/a.log");
    const std::string id2 = reg.Register("echo second", "sh", fake_pid + 1, "/tmp/b.log");

    CHECK_FALSE(id1.empty());
    CHECK_FALSE(id2.empty());
    // 单调递增:id2 的数字 > id1 的数字。
    CHECK(std::stoull(id2) > std::stoull(id1));

    // List 包含刚登记的两条。
    const auto tasks = reg.List();
    bool found1 = false, found2 = false;
    for (const auto& t : tasks) {
        if (t.task_id == id1) found1 = true;
        if (t.task_id == id2) found2 = true;
    }
    CHECK(found1);
    CHECK(found2);

    // Get 找得到、字段对得上。
    const auto info = reg.Get(id2);
    REQUIRE(info.has_value());
    CHECK(info->command == "echo second");
    CHECK(info->pid == fake_pid + 1);
    CHECK(info->log_path == "/tmp/b.log");

    // 乱填 task_id 找不到。
    CHECK_FALSE(reg.Get("nonexistent-zzz-999").has_value());
}

TEST_CASE("background: ReadOutput 读日志文件尾部") {
    auto& reg = BackgroundTaskRegistry::Instance();
    // 造一个有内容的日志文件。
    TempLogFile file("line1\nline2\nline3\nline4\nline5\n");
    const std::string task_id = reg.Register("some cmd", "sh", /*pid=*/1, file.Utf8Path());

    // 读最后 2 行。
    const std::string tail2 = reg.ReadOutput(task_id, /*tail_lines=*/2);
    CHECK(tail2.find("line4") != std::string::npos);
    CHECK(tail2.find("line5") != std::string::npos);
    CHECK(tail2.find("line3") == std::string::npos);  // 只要最后 2 行

    // tail_lines=1 只要最后一行。
    const std::string tail1 = reg.ReadOutput(task_id, /*tail_lines=*/1);
    CHECK(tail1.find("line5") != std::string::npos);
    CHECK(tail1.find("line4") == std::string::npos);

    // 不认得的 task_id 返回空串。
    CHECK(reg.ReadOutput("no-such-task-zzz").empty());
}

TEST_CASE("background: 短命后台命令完成时 DrainCompleted 取到通知") {
    auto& reg = BackgroundTaskRegistry::Instance();

    // 起一个真后台命令:echo 一行就退,经平台默认 shell。watcher 会探到它结束。
    const auto bg = lubancode::platform::RunShellCommandBackground("echo bg_done_marker_xyz");
    REQUIRE(bg.success);
    CHECK_FALSE(bg.log_path.empty());

    const std::string task_id = reg.Register("echo bg_done_marker_xyz", "sh-or-cmd", bg.pid, bg.log_path);

    // 等 watcher 探到完成(命令秒退,5 秒窗口绰绰有余)。
    const bool finished = WaitUntil(
        [&] {
            const auto info = reg.Get(task_id);
            return info.has_value() && info->status != BackgroundTaskStatus::Running;
        },
        /*timeout_ms=*/5000);
    CHECK(finished);

    // DrainCompleted 应当取到这一条。
    const auto drained = reg.DrainCompleted();
    bool found_in_drain = false;
    for (const auto& t : drained) {
        if (t.task_id == task_id) {
            found_in_drain = true;
            CHECK(t.status != BackgroundTaskStatus::Running);
        }
    }
    CHECK(found_in_drain);

    // 再 drain 一次,这一条不再出现(已标 completed_reported)。
    const auto drained_again = reg.DrainCompleted();
    bool found_again = false;
    for (const auto& t : drained_again) {
        if (t.task_id == task_id) {
            found_again = true;
        }
    }
    CHECK_FALSE(found_again);

    // 日志文件里确实写进了 echo 的输出。
    const std::string output = reg.ReadOutput(task_id, /*tail_lines=*/10);
    CHECK(output.find("bg_done_marker_xyz") != std::string::npos);
}

TEST_CASE("background: background_output 工具列任务/查单个") {
    using namespace lubancode::tools;
    BackgroundOutputTool tool;

    // 不给 task_id:列全部(至少有前面测试登记的那些)。
    const Tool::Result list_result = tool.execute(nlohmann::json::object());
    CHECK_FALSE(list_result.is_error);
    CHECK(list_result.content.find("后台任务") != std::string::npos);

    // 给一个不存在的 task_id:报错。
    nlohmann::json bad_input;
    bad_input["task_id"] = "no-such-task-zzz-999";
    const Tool::Result bad_result = tool.execute(bad_input);
    CHECK(bad_result.is_error);
    CHECK(bad_result.content.find("找不到") != std::string::npos);
}

TEST_CASE("background: stop_background 工具对不存在的 task 报错") {
    using namespace lubancode::tools;
    StopBackgroundTool tool;

    nlohmann::json input;
    input["task_id"] = "no-such-task-zzz-999";
    const Tool::Result result = tool.execute(input);
    CHECK(result.is_error);
    CHECK(result.content.find("找不到") != std::string::npos);

    // 缺 task_id 参数。
    const Tool::Result no_param = tool.execute(nlohmann::json::object());
    CHECK(no_param.is_error);
}

TEST_CASE("background: 长命命令先 Running 再自然完成") {
    // 跨平台延时命令:Windows 用 ping 发 3 个包(约 2 秒,>nul 压输出);
    // POSIX 用 sleep 2。验证 watcher 对"活一阵子才退"的命令正确追踪——
    // 起手是 Running,延时过后才翻成终态(不是秒退那种来不及看 Running)。
#ifdef _WIN32
    const std::string delay_cmd = "ping -n 3 127.0.0.1 > nul";
#else
    const std::string delay_cmd = "sleep 2";
#endif
    auto& reg = BackgroundTaskRegistry::Instance();

    const auto bg = lubancode::platform::RunShellCommandBackground(delay_cmd);
    REQUIRE(bg.success);
    const std::string task_id = reg.Register(delay_cmd, "shell", bg.pid, bg.log_path);

    // 起手应该是 Running(命令要跑约 2 秒,此刻还活着)。
    auto info = reg.Get(task_id);
    REQUIRE(info.has_value());
    CHECK(info->status == BackgroundTaskStatus::Running);

    // 等它自然完成(最多 6 秒:2 秒命令 + watcher 200ms 一拍 + 余量)。
    const bool finished = WaitUntil(
        [&] {
            const auto i = reg.Get(task_id);
            return i.has_value() && i->status != BackgroundTaskStatus::Running;
        },
        /*timeout_ms=*/6000);
    CHECK(finished);

    // DrainCompleted 取到这条完成通知。
    const auto drained = reg.DrainCompleted();
    bool found = false;
    for (const auto& t : drained) {
        if (t.task_id == task_id) {
            found = true;
            CHECK(t.status != BackgroundTaskStatus::Running);
        }
    }
    CHECK(found);
}
