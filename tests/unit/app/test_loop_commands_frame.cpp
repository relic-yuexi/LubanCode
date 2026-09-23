// TUI 排版批 3(tui优化todo.todo P3:/loop 全族)的输出形状册。
//   - list:id/state/interval/next/prompt 表格,state 列语义色(在跑 pass
//     档),标题嵌 "loop 任务(N 只)";
//   - status:键值对框(标题嵌 "<id>  [状态]");prompt 全稿多行时首行进
//     框、余稿裸行跟出(长正文不塞框);
//   - create/pause/resume/stop/run 反馈句收键值对框;错误句 error 档;
//   - plain 主题(--no-color/T3 降级路径)钉零转义字节、无框字形——合同
//     第 3 条;
//   - /loop 无 --json/机器消费分支(源码核实),字节级不变一条天然满足。
//
// 走真 HandleLoopCommand(自备 LoopScheduler + LoopWiring,不碰会话装配);
// now 走系统钟,"下一拍"的相对人话只断前缀,不断秒数。交互式 stop all 的
// 确认流(ReadLine)单测不可驱动,真机验收(见 PR body 未验项)。

#include <doctest/doctest.h>

#include <sstream>
#include <string>
#include <vector>

#include "app/commands/loop_commands.hpp"
#include "cli/line_editor.hpp"  // DisplayWidthUtf8:对齐断言按显示列量
#include "cli/slash_commands.hpp"  // ParseLoopCommand
#include "cli/terminal_port.hpp"
#include "cli/theme.hpp"
#include "runtime/loop_scheduler.hpp"

using namespace lubancode;

namespace {

class OutputCapture {
public:
    OutputCapture() { cli::TermPort().Redirect(&stream_, nullptr); }
    ~OutputCapture() { cli::TermPort().Reset(); }
    std::string text() const { return stream_.str(); }

private:
    std::ostringstream stream_;
};

std::string StripAnsi(const std::string& text) {
    std::string out;
    std::size_t i = 0;
    while (i < text.size()) {
        if (text[i] == '\x1b' && i + 1 < text.size() && text[i + 1] == '[') {
            i += 2;
            while (i < text.size() &&
                   !((text[i] >= 'a' && text[i] <= 'z') || (text[i] >= 'A' && text[i] <= 'Z'))) {
                ++i;
            }
            if (i < text.size()) {
                ++i;  // 吃掉终结字母
            }
            continue;
        }
        out += text[i];
        ++i;
    }
    return out;
}

bool Contains(const std::string& haystack, const std::string& needle) {
    return haystack.find(needle) != std::string::npos;
}

int DisplayColOf(const std::string& row, const std::string& needle) {
    const std::size_t at = row.find(needle);
    if (at == std::string::npos) {
        return -1;
    }
    return static_cast<int>(cli::DisplayWidthUtf8(row.substr(0, at)));
}

constexpr const char* kBoxLightTopLeft = "\xe2\x94\x8c";  // ┌
constexpr const char* kBoxLightVert = "\xe2\x94\x82";     // │
// 键值对的键按最宽键补空,分隔不是恒等两格——"键与值在同一行"才是稳定
// 形状(列宽随同框字段浮动,断言不跟补空走)。
bool LineWithText(const std::string& text, std::initializer_list<std::string> parts) {
    std::istringstream scan(text);
    std::string line;
    while (std::getline(scan, line)) {
        bool all = true;
        for (const std::string& part : parts) {
            if (line.find(part) == std::string::npos) {
                all = false;
                break;
            }
        }
        if (all) {
            return true;
        }
    }
    return false;
}

struct LoopRig {
    runtime::loop::LoopScheduler scheduler;
    cli::Theme theme;
    cli::Theme plain_theme;

    LoopRig() : theme(cli::BuiltinTheme("dark")), plain_theme(cli::BuiltinTheme("plain")) {}

    LoopWiring Wiring(const cli::Theme& use_theme) {
        LoopWiring wiring;
        wiring.interactive = true;
        wiring.feature_enabled = true;
        wiring.theme = &use_theme;
        wiring.scheduler = &scheduler;
        return wiring;
    }

    std::string Run(const std::string& args, const cli::Theme& use_theme) {
        OutputCapture capture;
        app::HandleLoopCommand(cli::ParseLoopCommand(args), Wiring(use_theme));
        return capture.text();
    }

    std::string FirstTaskId() {
        const auto views = scheduler.Snapshot(0);
        if (views.empty()) {
            return std::string();
        }
        return views.front().task.task_id;
    }
};

}  // namespace

TEST_CASE("create: 反馈句收 frame,任务真的建进 scheduler") {
    LoopRig rig;
    const std::string out = rig.Run("5m 定时巡检仓库状态", rig.theme);
    const std::string plain = StripAnsi(out);
    REQUIRE(Contains(out, kBoxLightTopLeft));
    CHECK(Contains(plain, "loop 任务已建"));
    CHECK(Contains(plain, "5 分钟"));
    CHECK(Contains(plain, "查看 /loop list"));
    CHECK(rig.scheduler.Snapshot(0).size() == 1);
}

TEST_CASE("list: 五列表格 + 标题,state 列 pass 档、多行对齐") {
    LoopRig rig;
    rig.Run("5m 巡检甲", rig.theme);
    rig.Run("1h 巡检乙", rig.theme);
    const std::string out = rig.Run("list", rig.theme);
    const std::string plain = StripAnsi(out);

    REQUIRE(Contains(out, kBoxLightTopLeft));
    CHECK(Contains(plain, "loop 任务(2 只)"));
    bool saw_header = false;
    std::istringstream scan(plain);
    std::string line;
    while (std::getline(scan, line)) {
        if (Contains(line, "id") && Contains(line, "state") && Contains(line, "interval") &&
            Contains(line, "next") && Contains(line, "prompt")) {
            saw_header = true;
        }
    }
    CHECK(saw_header);
    CHECK(Contains(plain, "运行中"));
    CHECK(Contains(plain, "5 分钟"));
    CHECK(Contains(plain, "1 小时"));
    CHECK(Contains(plain, "巡检甲"));
    // 在跑 pass 档 + 两行 state 单元格同列。
    CHECK(Contains(out, rig.theme.table_pass + "运行中"));
    std::istringstream align_lines(plain);
    int col_a = -1;
    int col_b = -1;
    while (std::getline(align_lines, line)) {
        if (!Contains(line, "interval")) {
            const int col = DisplayColOf(line, "运行中");
            if (col >= 0) {
                if (col_a < 0) {
                    col_a = col;
                } else {
                    col_b = col;
                }
            }
        }
    }
    CHECK(col_a > 0);
    CHECK(col_b > 0);
    CHECK(col_a == col_b);
}

TEST_CASE("status: 键值对框标题嵌 id+状态,prompt 首行进框余稿裸行") {
    LoopRig rig;
    rig.Run("5m 首行要旨\n第二行的余稿\n第三行的尾巴", rig.theme);
    const std::string id = rig.FirstTaskId();
    REQUIRE_FALSE(id.empty());
    const std::string out = rig.Run("status " + id, rig.theme);
    const std::string plain = StripAnsi(out);

    REQUIRE(Contains(out, kBoxLightTopLeft));
    CHECK(Contains(plain, id + "  [运行中]"));
    CHECK(LineWithText(plain, {"间隔", "5 分钟 · 已跑 0 拍"}));
    CHECK(Contains(plain, "下一拍"));
    // prompt 全稿:首行进框(key 是 prompt(inline)),余稿裸行跟出。
    CHECK(Contains(plain, "prompt(inline)  首行要旨"));
    CHECK(Contains(plain, "第二行的余稿"));
    CHECK(Contains(plain, "第三行的尾巴"));
    // status all 走 list 同一副表格。
    const std::string all = rig.Run("status all", rig.theme);
    CHECK(Contains(StripAnsi(all), "loop 任务(1 只)"));
}

TEST_CASE("pause/resume/run/stop: 反馈句收 frame;错误句 error 档") {
    LoopRig rig;
    rig.Run("5m 巡检", rig.theme);
    const std::string id = rig.FirstTaskId();
    REQUIRE_FALSE(id.empty());
    {
        const std::string out = rig.Run("pause " + id, rig.theme);
        REQUIRE(Contains(out, kBoxLightTopLeft));
        CHECK(Contains(StripAnsi(out), "已暂停"));
    }
    {
        const std::string out = rig.Run("resume " + id, rig.theme);
        REQUIRE(Contains(out, kBoxLightTopLeft));
        CHECK(Contains(StripAnsi(out), "已续跑"));
    }
    {
        const std::string out = rig.Run("run " + id, rig.theme);
        REQUIRE(Contains(out, kBoxLightTopLeft));
        CHECK(Contains(StripAnsi(out), "排一次立即补拍"));
    }
    {
        const std::string out = rig.Run("stop " + id, rig.theme);
        REQUIRE(Contains(out, kBoxLightTopLeft));
        CHECK(Contains(StripAnsi(out), "已停止"));
    }
    {
        // 任务不存在:错误句 error 档。
        const std::string out = rig.Run("pause loop-999", rig.theme);
        REQUIRE(Contains(out, kBoxLightTopLeft));
        CHECK(Contains(StripAnsi(out), "暂停失败"));
        CHECK(Contains(out, rig.theme.error));
    }
}

TEST_CASE("门禁与用法错误收 error 框") {
    LoopRig rig;
    {
        // 间隔写法不对:create 前置校验。
        const std::string out = rig.Run("xx 巡检", rig.theme);
        REQUIRE(Contains(out, kBoxLightTopLeft));
        CHECK(Contains(StripAnsi(out), "间隔写法不对"));
        CHECK(Contains(out, rig.theme.error));
    }
    {
        // 正文以 / 开头:首版拒绝调度 slash 命令。
        const std::string out = rig.Run("5m /exit", rig.theme);
        REQUIRE(Contains(out, kBoxLightTopLeft));
        CHECK(Contains(StripAnsi(out), "loop 正文不能以 / 开头"));
    }
    {
        // 非交互终端:明拒。
        LoopWiring wiring = rig.Wiring(rig.theme);
        wiring.interactive = false;
        OutputCapture capture;
        app::HandleLoopCommand(cli::ParseLoopCommand("5m x"), wiring);
        const std::string out = capture.text();
        REQUIRE(Contains(out, kBoxLightTopLeft));
        CHECK(Contains(StripAnsi(out), "当前不是交互终端"));
    }
    {
        // feature 关:明说开法。
        LoopWiring wiring = rig.Wiring(rig.theme);
        wiring.feature_enabled = false;
        OutputCapture capture;
        app::HandleLoopCommand(cli::ParseLoopCommand("5m x"), wiring);
        const std::string out = capture.text();
        REQUIRE(Contains(out, kBoxLightTopLeft));
        CHECK(Contains(StripAnsi(out), "loop 功能未开启"));
    }
}

TEST_CASE("plain 主题: 全族输出零转义字节、无框字形(T3/--no-color 路径)") {
    LoopRig rig;
    for (const std::string& args : {"5m plain 巡检", "list", "status loop-999", "xx 巡检"}) {
        CAPTURE(args);
        const std::string out = rig.Run(args, rig.plain_theme);
        CHECK(out.find("\x1b") == std::string::npos);
        CHECK(!Contains(out, kBoxLightTopLeft));
        CHECK(!Contains(out, kBoxLightVert));
        // 信息一字不少:标题与状态文本都在,只是没了色与框。
        if (args == "list") {
            CHECK(Contains(out, "loop 任务("));
            CHECK(Contains(out, "运行中"));
        }
    }
}
