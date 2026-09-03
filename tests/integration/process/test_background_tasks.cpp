// BackgroundTaskRegistry:登记/查询/读输出/完成通知的单测。
// 进程级单例,task_id 跨测试单调递增——不硬编码具体编号,只比较相对大小
// 和拿登记时返回的 task_id 往后查。watcher 线程真起、真探活,端到端测试
// 用 platform 层 spawn 一个短命后台命令验证 DrainCompleted。

#include <doctest/doctest.h>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <string>
#include <thread>

#include <nlohmann/json.hpp>

#include "platform/process.hpp"
#include "tools/background_output.hpp"
#include "tools/background_tasks.hpp"
#include "tools/task_ledger.hpp"

using lubancode::tools::BackgroundTaskRegistry;
using lubancode::tools::BackgroundTaskInfo;
using lubancode::tools::BackgroundTaskStatus;
using lubancode::tools::BackgroundLogRead;

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
    const std::string id1 = reg.Register("echo first", "sh", /*cwd=*/"/w/one", fake_pid, "/tmp/a.log");
    const std::string id2 = reg.Register("echo second", "sh", /*cwd=*/"/w/two", fake_pid + 1, "/tmp/b.log");

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

    // Get 找得到、字段对得上(cwd 也一并入账:/background show 要给人看)。
    const auto info = reg.Get(id2);
    REQUIRE(info.has_value());
    CHECK(info->command == "echo second");
    CHECK(info->pid == fake_pid + 1);
    CHECK(info->log_path == "/tmp/b.log");
    CHECK(info->cwd == "/w/two");

    // 乱填 task_id 找不到。
    CHECK_FALSE(reg.Get("nonexistent-zzz-999").has_value());
}

TEST_CASE("background: ReadOutput 读日志文件尾部") {
    auto& reg = BackgroundTaskRegistry::Instance();
    // 造一个有内容的日志文件。
    TempLogFile file("line1\nline2\nline3\nline4\nline5\n");
    const std::string task_id = reg.Register("some cmd", "sh", /*cwd=*/"", /*pid=*/1, file.Utf8Path());

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

// ReadLogDetail(background 管理面单):四类"没内容"各有各的 kind,清洗与
// 截断各带旗子——/background logs 的如实话全靠这本明细账。
TEST_CASE("background: ReadLogDetail 对空/删/坏/超长日志各说各的") {
    auto& reg = BackgroundTaskRegistry::Instance();

    // 不认得的 task_id。
    const auto not_found = reg.ReadLogDetail("no-such-task-zzz", 100);
    CHECK(not_found.kind == BackgroundLogRead::Kind::TaskNotFound);

    // 空文件:进程还没开写。
    {
        TempLogFile empty_file("");
        const std::string id = reg.Register("empty log cmd", "sh", /*cwd=*/"", /*pid=*/1, empty_file.Utf8Path());
        const auto empty = reg.ReadLogDetail(id, 100);
        CHECK(empty.kind == BackgroundLogRead::Kind::Empty);
        CHECK(empty.text.empty());
    }

    // 正常读取:Ok + 文本 + 文件大小。
    {
        TempLogFile file("hello\nworld\n");
        const std::string id = reg.Register("ok log cmd", "sh", /*cwd=*/"", /*pid=*/1, file.Utf8Path());
        const auto ok = reg.ReadLogDetail(id, 100);
        CHECK(ok.kind == BackgroundLogRead::Kind::Ok);
        CHECK(ok.text.find("world") != std::string::npos);
        CHECK(ok.file_size == static_cast<long long>(std::string("hello\nworld\n").size()));
        CHECK_FALSE(ok.sanitized);
        CHECK_FALSE(ok.head_omitted);
    }

    // 非法 UTF-8 字节:sanitized 旗子如实亮,出口保证合法 UTF-8。
    {
        const std::string bad = "good\xFF\xFE bad\n";
        TempLogFile file(bad);
        const std::string id = reg.Register("bad utf8 cmd", "sh", /*cwd=*/"", /*pid=*/1, file.Utf8Path());
        const auto dirty = reg.ReadLogDetail(id, 100);
        CHECK(dirty.kind == BackgroundLogRead::Kind::Ok);
        CHECK(dirty.sanitized);
    }

    // 超过 64KB 读档:head_omitted 如实亮,读的还是末尾。
    {
        std::string big(70 * 1024, 'x');
        big += "\ntail-marker\n";
        TempLogFile file(big);
        const std::string id = reg.Register("big log cmd", "sh", /*cwd=*/"", /*pid=*/1, file.Utf8Path());
        const auto capped = reg.ReadLogDetail(id, 10);
        CHECK(capped.kind == BackgroundLogRead::Kind::Ok);
        CHECK(capped.head_omitted);
        CHECK(capped.text.find("tail-marker") != std::string::npos);
        CHECK(capped.file_size == static_cast<long long>(big.size()));
    }

    // 日志被删:FileMissing(与"还没写"分开说)。
    {
        std::string ghost_path;
        {
            TempLogFile doomed("soon gone\n");
            ghost_path = doomed.Utf8Path();
        }  // 析构已删
        const std::string id = reg.Register("deleted log cmd", "sh", /*cwd=*/"", /*pid=*/1, ghost_path);
        const auto missing = reg.ReadLogDetail(id, 100);
        CHECK(missing.kind == BackgroundLogRead::Kind::FileMissing);
    }
}

TEST_CASE("background: 短命后台命令完成时 DrainCompleted 取到通知") {
    auto& reg = BackgroundTaskRegistry::Instance();

    // 起一个真后台命令:echo 一行就退,经平台默认 shell。watcher 会探到它结束。
    const auto bg = lubancode::platform::RunShellCommandBackground("echo bg_done_marker_xyz");
    REQUIRE(bg.success);
    CHECK_FALSE(bg.log_path.empty());

    const std::string task_id =
        reg.Register("echo bg_done_marker_xyz", "sh-or-cmd", /*cwd=*/"", bg.pid, bg.log_path);

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

    // 不给 task_id:列全部(至少有前面测试登记的那些)。两本账合并后命令段的
    // 段头是"后台命令"(Bug B 收口:命令与面板代理分段列)。
    const Tool::Result list_result = tool.execute(nlohmann::json::object());
    CHECK_FALSE(list_result.is_error);
    CHECK(list_result.content.find("后台命令") != std::string::npos);

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
    const std::string task_id = reg.Register(delay_cmd, "shell", /*cwd=*/"", bg.pid, bg.log_path);

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

// HasUnreportedCompletions(background 管理面单):只看不取——空闲唤醒源
// 每拍问它,DrainCompleted 才许动账。
TEST_CASE("background: HasUnreportedCompletions 只报不取,drain 后归零") {
    auto& reg = BackgroundTaskRegistry::Instance();

    const auto bg = lubancode::platform::RunShellCommandBackground("echo wake_marker_xyz");
    REQUIRE(bg.success);
    const std::string task_id = reg.Register("echo wake_marker_xyz", "sh-or-cmd", /*cwd=*/"", bg.pid, bg.log_path);

    const bool finished = WaitUntil(
        [&] {
            const auto info = reg.Get(task_id);
            return info.has_value() && info->status != BackgroundTaskStatus::Running;
        },
        /*timeout_ms=*/5000);
    REQUIRE(finished);

    // 有未报的完成;问几遍都还在(不消费)。
    CHECK(reg.HasUnreportedCompletions());
    CHECK(reg.HasUnreportedCompletions());

    (void)reg.DrainCompleted();
    CHECK_FALSE(reg.HasUnreportedCompletions());  // 取走即归零
}

// stop 的三段语义(background 管理面单):真起一只长命后台命令,Stop 走
// Running -> Stopping -> Stopped;树死透了才报已停;对已终态任务不重复杀。
TEST_CASE("background: Stop 收长命命令进 Stopped,已终态不重复杀") {
    auto& reg = BackgroundTaskRegistry::Instance();

#ifdef _WIN32
    const std::string delay_cmd = "ping -n 30 127.0.0.1 > nul";  // 约 30 秒,足够停
#else
    const std::string delay_cmd = "sleep 30";
#endif
    const auto bg = lubancode::platform::RunShellCommandBackground(delay_cmd);
    REQUIRE(bg.success);
    REQUIRE(bg.handle != nullptr);
    const std::string task_id = reg.Register(delay_cmd, "shell", /*cwd=*/"", bg.pid, bg.log_path,
                                             bg.handle, /*max_runtime_ms=*/0);

    // 起手 Running。
    {
        const auto info = reg.Get(task_id);
        REQUIRE(info.has_value());
        CHECK(info->status == BackgroundTaskStatus::Running);
        CHECK(info->stop_error.empty());
    }

    // Stop 同步走完 Stopping -> Stopped:返回时树已死透(Windows 是 Job 一锅端,
    // POSIX 是进程组 SIGTERM/SIGKILL),台账停在终态。
    CHECK(reg.Stop(task_id));
    {
        const auto info = reg.Get(task_id);
        REQUIRE(info.has_value());
        CHECK(info->status == BackgroundTaskStatus::Stopped);
        CHECK(info->stop_error.empty());
        CHECK(info->finish_time > info->start_time);
    }

    // 已终态:Stop 返回 true(认得),状态不动,不重复杀。
    CHECK(reg.Stop(task_id));
    {
        const auto info = reg.Get(task_id);
        REQUIRE(info.has_value());
        CHECK(info->status == BackgroundTaskStatus::Stopped);
    }

    // 终止通知进 DrainCompleted(停止也是"新终态",要报一回)。
    bool stop_reported = false;
    for (const auto& t : reg.DrainCompleted()) {
        if (t.task_id == task_id) {
            stop_reported = true;
            CHECK(t.status == BackgroundTaskStatus::Stopped);
        }
    }
    CHECK(stop_reported);
}

// ---------------------------------------------------------------------------
// 停控两本账收口(后台代理管控三连 bug 单,Bug B):面板账(TaskLedger,int
// id)与后台命令登记簿(BackgroundTaskRegistry,字符串 id)互认——模型拿
// 面板可见的编号,stop_background/background_output 必须一击命中。旧码
// 两本账互不相认:面板三只在跑,stop 三连"找不到"、background_output 说
// "当前没有后台任务"——真机 30M+ tokens 停不掉。
// ---------------------------------------------------------------------------

namespace {

// 一只挂在面板账上的"在跑"后台代理:TaskLedger 是会话级实例(非单例),
// 测试里现开一份、Register 一只 Running 任务即可;id 由台账发,取面板
// 同款显示口径(快照 id)。
std::optional<int> ParseIdForTest(const std::string& text) {
    if (text.empty()) {
        return std::nullopt;
    }
    int value = 0;
    for (const char c : text) {
        if (c < '0' || c > '9') {
            return std::nullopt;
        }
        value = value * 10 + (c - '0');
    }
    return value;
}

std::shared_ptr<lubancode::tools::TaskRecord> RegisterPanelAgent(lubancode::tools::TaskLedger& ledger,
                                                                 const std::string& title) {
    lubancode::tools::AgentTaskSnapshot snapshot;
    snapshot.agent_type = "general-purpose";
    snapshot.title = title;
    snapshot.prompt = "测试派工";
    snapshot.state = lubancode::tools::AgentTaskState::Running;
    snapshot.start_time = std::chrono::steady_clock::now();
    return ledger.Register(std::move(snapshot));
}

// 命令登记簿(进程单例)在本册前面的测试里已登记过若干条,id 从 "1" 起单调。
// 面板台账的 id 也从 1 起——直接取第一只会与命令存量撞号(stop 先认命令账,
// 测试就测不到面板分支)。把面板号顶过命令存量的最大号,确保独占。
int PanelIdBeyondCommandRegistry(lubancode::tools::TaskLedger& ledger) {
    int max_command_id = 0;
    for (const auto& t : lubancode::tools::BackgroundTaskRegistry::Instance().List()) {
        const auto parsed = ParseIdForTest(t.task_id);
        if (parsed.has_value() && *parsed > max_command_id) {
            max_command_id = *parsed;
        }
    }
    while (ledger.Summaries().size() < static_cast<std::size_t>(max_command_id)) {
        (void)RegisterPanelAgent(ledger, "占位(顶号)");
    }
    return max_command_id + 1;
}

}  // namespace

TEST_CASE("background: stop_background 用面板后台代理的显示 id 一击停掉") {
    using namespace lubancode::tools;
    TaskLedger ledger;
    const int wanted_id = PanelIdBeyondCommandRegistry(ledger);
    const auto task = RegisterPanelAgent(ledger, "面板代理停控");
    REQUIRE(task != nullptr);
    const int panel_id = task->snapshot.id;
    REQUIRE(panel_id == wanted_id);  // 号已顶过命令存量,面板分支独占

    StopBackgroundTool tool;
    tool.SetAgentLedgerProvider([&ledger]() -> TaskLedger* { return &ledger; });

    nlohmann::json input;
    input["task_id"] = std::to_string(panel_id);
    const Tool::Result result = tool.execute(input);
    CHECK_FALSE(result.is_error);
    CHECK(result.content.find("后台子代理") != std::string::npos);
    CHECK(result.content.find(std::to_string(panel_id)) != std::string::npos);

    // 停止信号真落账:任务收到 cancel(面板行随后显"停止中")。
    CHECK(task->cancel.load(std::memory_order_acquire));

    // 再停一次:停止中/活态照旧受理(CancelTask 幂等发信号),不报"找不到"。
    const Tool::Result again = tool.execute(input);
    CHECK_FALSE(again.is_error);
}

TEST_CASE("background: background_output 空列表合并面板后台代理,不再'当前没有'") {
    using namespace lubancode::tools;
    TaskLedger ledger;
    const int wanted_id = PanelIdBeyondCommandRegistry(ledger);
    const auto task = RegisterPanelAgent(ledger, "面板代理列账");
    REQUIRE(task != nullptr);
    REQUIRE(task->snapshot.id == wanted_id);

    BackgroundOutputTool tool;
    tool.SetAgentLedgerProvider([&ledger]() -> TaskLedger* { return &ledger; });

    // 不给 task_id:面板有活代理,列表必须把它列出来——"当前没有后台任务"
    // 在有活代理的场合就是两本账裂开的症状。
    const Tool::Result list_result = tool.execute(nlohmann::json::object());
    CHECK_FALSE(list_result.is_error);
    CHECK(list_result.content.find("后台子代理") != std::string::npos);
    CHECK(list_result.content.find(std::to_string(task->snapshot.id)) != std::string::npos);

    // 给面板 id:查得到该代理的摘要(状态/title),不是"找不到"。
    nlohmann::json detail_input;
    detail_input["task_id"] = std::to_string(task->snapshot.id);
    const Tool::Result detail = tool.execute(detail_input);
    CHECK_FALSE(detail.is_error);
    CHECK(detail.content.find("面板代理列账") != std::string::npos);

    // 两本账都没有的 id:照旧如实"找不到"。
    nlohmann::json bad_input;
    bad_input["task_id"] = "no-such-panel-agent-999";
    const Tool::Result bad = tool.execute(bad_input);
    CHECK(bad.is_error);
    CHECK(bad.content.find("找不到") != std::string::npos);
}
