// TUI 排版批 3(tui优化todo.todo P3:/background 全族)的输出形状册。
//   - list:id/status/pid/elapsed/command/log 表格,status 列语义色(在跑
//     pass 档);每任务最近三行非空输出以 "#id" 标签跟在表后;
//   - show:键值对框(标题嵌 "后台任务 #<id>"),状态字段按语义档着色;
//   - logs:头部键值对框,正文与首尾横线框外原样(长正文不塞框,与批 2
//     plugin test 的日志正文同一条裁量);
//   - stop:进度行保原样(流式行不进框,批 2 裁量 2),终态反馈收框;
//   - plain 主题(--no-color/T3 降级路径)钉零转义字节、无框字形——合同
//     第 3 条;
//   - /background 无 --json/机器消费分支(源码核实),字节级不变一条天然满足。
//
// 任务台账是进程级单例:测试用 Register 造任务——pid 用本进程 id(探活
// 恒真,任务稳在 Running),终态任务用天假 pid(watcher 首轮即判死,落
// Completed),不生真进程、不误杀。台账里可能有同册其他用例的任务,断言
// 只认自己注册的 id,不数全表。stop all 要 ReadLine 交互,单测不可驱动,
// 真机验收(见 PR body 未验项)。

#include <doctest/doctest.h>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#endif

#include "app/commands/background_commands.hpp"
#include "cli/line_editor.hpp"  // DisplayWidthUtf8:对齐断言按显示列量
#include "cli/terminal_port.hpp"
#include "cli/theme.hpp"
#include "platform/paths.hpp"  // PathToUtf8:Windows 日志路径进表格
#include "tools/background_tasks.hpp"

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

unsigned long SelfPid() {
#ifdef _WIN32
    return static_cast<unsigned long>(GetCurrentProcessId());
#else
    return static_cast<unsigned long>(getpid());
#endif
}

// 临时目录:日志文件落这里,测试完自动清。
struct TempDir {
    std::filesystem::path base;
    TempDir() {
        base = std::filesystem::temp_directory_path() /
               ("lubancode_bg_frame_test_" +
                std::to_string(reinterpret_cast<std::uintptr_t>(this)));
        std::error_code ec;
        std::filesystem::create_directories(base, ec);
    }
    ~TempDir() {
        std::error_code ec;
        std::filesystem::remove_all(base, ec);
    }
    std::filesystem::path Write(const std::string& name, const std::string& content) const {
        const std::filesystem::path full = base / name;
        std::ofstream out(full, std::ios::binary);
        out << content;
        return full;
    }
};

std::string Utf8Path(const std::filesystem::path& p) {
#ifdef _WIN32
    return lubancode::platform::PathToUtf8(p);
#else
    return p.string();
#endif
}

struct BackgroundRig {
    cli::Theme theme;
    cli::Theme plain_theme;
    TempDir dir;

    BackgroundRig() : theme(cli::BuiltinTheme("dark")), plain_theme(cli::BuiltinTheme("plain")) {}

    // 造一只稳在 Running 的任务(pid = 本进程,探活恒真)。返回 task_id。
    std::string SpawnRunning(const std::string& tag, const std::filesystem::path& log) {
        return tools::BackgroundTaskRegistry::Instance().Register(
            "echo frame-" + tag, "sh", "/tmp", SelfPid(), Utf8Path(log));
    }
    // 造一只很快落 Completed 的任务(天假 pid,首轮探活即死)。
    std::string SpawnDying(const std::string& tag, const std::filesystem::path& log) {
        return tools::BackgroundTaskRegistry::Instance().Register(
            "echo dying-" + tag, "sh", "/tmp", 2147483600UL, Utf8Path(log));
    }

    std::string Run(const std::string& args, const cli::Theme& use_theme) {
        OutputCapture capture;
        app::BackgroundCommandContext ctx;
        ctx.theme = &use_theme;
        cli::ParsedSlashCommand parsed;
        parsed.args = args;
        app::HandleSlashBackground(ctx, parsed);
        return capture.text();
    }
};

}  // namespace

TEST_CASE("list: 六列表格 + 尾巴行,status 列 pass 档、列对齐") {
    BackgroundRig rig;
    const std::filesystem::path log =
        rig.dir.Write("frame-list.log", "noise-1\nnoise-2\ntail-a\ntail-b\ntail-c\n");
    const std::string a = rig.SpawnRunning("alpha", log);
    const std::string b = rig.SpawnRunning("beta", log);
    const std::string out = rig.Run("list", rig.theme);
    const std::string plain = StripAnsi(out);

    REQUIRE(Contains(out, kBoxLightTopLeft));
    CHECK(Contains(plain, "后台任务共"));
    // 表头 schema 名六列。
    bool saw_header = false;
    std::istringstream scan(plain);
    std::string line;
    while (std::getline(scan, line)) {
        if (Contains(line, "id") && Contains(line, "status") && Contains(line, "pid") &&
            Contains(line, "elapsed") && Contains(line, "command") && Contains(line, "log")) {
            saw_header = true;
        }
    }
    CHECK(saw_header);
    CHECK(Contains(plain, "#" + a));
    CHECK(Contains(plain, "#" + b));
    CHECK(Contains(plain, "echo frame-alpha"));
    // 在跑走 pass 档(table_pass 连着状态字)。
    CHECK(Contains(out, rig.theme.table_pass + "运行中"));
    // 尾巴:最近三行非空以 "#id" 标签跟出(tail-a/b/c 在,two noise 不在)。
    CHECK(LineWithText(plain, {"tail-a"}));
    CHECK(Contains(plain, "tail-c"));
    CHECK(!Contains(plain, "noise-1"));
    // 列对齐:两只 Running 任务的"运行中"单元格起始显示列一致。
    std::istringstream align_lines(plain);
    int col_a = -1;
    int col_b = -1;
    while (std::getline(align_lines, line)) {
        if (Contains(line, "echo frame-alpha")) col_a = DisplayColOf(line, "运行中");
        if (Contains(line, "echo frame-beta")) col_b = DisplayColOf(line, "运行中");
    }
    CHECK(col_a > 0);
    CHECK(col_b > 0);
    CHECK(col_a == col_b);
}

TEST_CASE("show: 键值对框带标题,查无此任务给 error 框") {
    BackgroundRig rig;
    const std::filesystem::path log = rig.dir.Write("frame-show.log", "x\n");
    const std::string id = rig.SpawnRunning("show", log);
    {
        const std::string out = rig.Run("show #" + id, rig.theme);
        const std::string plain = StripAnsi(out);
        REQUIRE(Contains(out, kBoxLightTopLeft));
        CHECK(Contains(plain, "后台任务 #" + id));
        CHECK(LineWithText(plain, {"状态", "运行中"}));
        CHECK(LineWithText(plain, {"命令", "echo frame-show"}));
        CHECK(Contains(plain, "日志"));
        CHECK(LineWithText(plain, {"查看", "/background logs " + id}));
    }
    {
        const std::string out = rig.Run("show 999999", rig.theme);
        REQUIRE(Contains(out, kBoxLightTopLeft));
        CHECK(Contains(StripAnsi(out), "找不到 #999999"));
        CHECK(Contains(out, rig.theme.error));
    }
}

TEST_CASE("logs: 头部键值对框,正文框外原样,首尾横线保原样") {
    BackgroundRig rig;
    const std::filesystem::path log = rig.dir.Write("frame-logs.log", "hello-log-line-1\nhello-log-line-2\n");
    const std::string id = rig.SpawnRunning("logs", log);
    const std::string out = rig.Run("logs #" + id, rig.theme);
    const std::string plain = StripAnsi(out);

    REQUIRE(Contains(out, kBoxLightTopLeft));
    CHECK(Contains(plain, "后台任务 #" + id + " 日志(查看日志:只读,不动进程)"));
    CHECK(LineWithText(plain, {"状态", "运行中"}));
    // 正文:框外原样两行,一条不截。
    CHECK(Contains(plain, "hello-log-line-1"));
    CHECK(Contains(plain, "hello-log-line-2"));
    CHECK(Contains(plain, "── 完(共读"));
    // 日志被删:notice 框里说人话。
    const std::string missing = rig.SpawnRunning("gone", rig.dir.base / "no-such.log");
    const std::string gone = rig.Run("logs #" + missing, rig.theme);
    REQUIRE(Contains(gone, kBoxLightTopLeft));
    CHECK(Contains(StripAnsi(gone), "日志文件已不存在"));
}

TEST_CASE("stop: 终态任务不重复杀,反馈收框;进度行照旧裸打") {
    BackgroundRig rig;
    const std::filesystem::path log = rig.dir.Write("frame-stop.log", "x\n");
    const std::string dying = rig.SpawnDying("stop", log);
    // 天假 pid 的 watcher 首轮探活即判死 → Completed;轮询等它落终态。
    bool terminal = false;
    for (int i = 0; i < 100 && !terminal; ++i) {
        const auto info = tools::BackgroundTaskRegistry::Instance().Get(dying);
        terminal = info.has_value() && info->status != tools::BackgroundTaskStatus::Running &&
                   info->status != tools::BackgroundTaskStatus::Stopping;
        if (!terminal) {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
    }
    REQUIRE(terminal);
    {
        const std::string out = rig.Run("stop #" + dying, rig.theme);
        REQUIRE(Contains(out, kBoxLightTopLeft));
        CHECK(Contains(StripAnsi(out), "已是终态"));
        CHECK(Contains(StripAnsi(out), "不重复杀"));
    }
    {
        const std::string out = rig.Run("stop 999999", rig.theme);
        REQUIRE(Contains(out, kBoxLightTopLeft));
        CHECK(Contains(StripAnsi(out), "找不到 #999999"));
        CHECK(Contains(out, rig.theme.error));
    }
}

TEST_CASE("invalid: 认不得一句收 error 框,用法块照旧裸打(对齐正文不塞框)") {
    BackgroundRig rig;
    const std::string out = rig.Run("frobnicate", rig.theme);
    const std::string plain = StripAnsi(out);
    REQUIRE(Contains(out, kBoxLightTopLeft));
    CHECK(Contains(plain, "认不得 \"frobnicate\""));
    CHECK(Contains(out, rig.theme.error));
    // 用法块是手工对齐的多行帮助文,不塞框,原样落盘。
    CHECK(Contains(plain, "/background show <id>"));
    CHECK(!Contains(plain, kBoxLightVert + "      /background"));
}

TEST_CASE("plain 主题: 全族输出零转义字节、无框字形(T3/--no-color 路径)") {
    BackgroundRig rig;
    const std::filesystem::path log = rig.dir.Write("frame-plain.log", "plain-tail\n");
    const std::string id = rig.SpawnRunning("plain", log);
    for (const std::string& args : {"list", "show #" + id, "logs #" + id, "stop 999999", "frobnicate"}) {
        CAPTURE(args);
        const std::string out = rig.Run(args, rig.plain_theme);
        CHECK(out.find("\x1b") == std::string::npos);
        CHECK(!Contains(out, kBoxLightTopLeft));
        CHECK(!Contains(out, kBoxLightVert));
        // 信息一字不少:状态与日志路径都在,只是没了色与框。
        if (args.rfind("show", 0) == 0) {
            CHECK(LineWithText(StripAnsi(out), {"状态", "运行中"}));
        }
    }
}
