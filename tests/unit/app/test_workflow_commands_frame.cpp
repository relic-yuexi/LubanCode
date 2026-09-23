// TUI 排版批 3(tui优化todo.todo P3:/workflow list/history/graph 标题)的
// 输出形状册。
//   - list:主表(workflow/id/source/alias/state)+ 明细列表(描述/诊断以
//     id 标签跟出);损坏条目 state 列 [损坏] error 档;
//   - history:run/workflow/state/started 表格;空账 "(没有运行账)" 进框;
//   - graph:标题 frame(图名进框顶、format 进框行),ascii/mermaid 图本体
//     框外原样——图自身有排版,塞框会被列帽截断劈行(批 1 裁量 3);
//     json 分支是机器面,字节级不变(合同第 2 条);
//   - graph 标题 frame 在 80 列与 200 列都不破框:命令侧的标题帽(预算-7
//     截断保字头)与 RenderKeyValues 同一配方,窄/宽两档在册里钉;
//   - plain 主题(--no-color/T3 降级路径)钉零转义字节、无框字形——合同
//     第 3 条。
//
// 走真 HandleWorkflowCommand(临时目录里造 catalog 与运行账,不碰会话);
// show/validate/doctor 等其余分支批 3 不收口,照旧裸打。

#include <doctest/doctest.h>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "app/commands/workflow_commands.hpp"
#include "cli/line_editor.hpp"  // DisplayWidthUtf8/TruncateUtf8ToDisplayWidth
#include "cli/terminal_frame.hpp"
#include "cli/terminal_port.hpp"
#include "cli/theme.hpp"

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

constexpr const char* kProbeYaml = R"yaml(schema_version: 1
id: probe
version: 1.0.0
name: 探针工作流
alias: probe
description: 形状册的最小工作流
enabled: true
entry: start
nodes:
  start:
    type: tool
    label: 开工
    tool: echo_tool
    input: {}
  done:
    type: end
    label: 收口
edges:
  - { from: start, on: success, to: done }
)yaml";

// 临时根:catalog(project 层)+ 运行账(home 层)都落这里,测完自动清。
struct WorkflowFixture {
    std::filesystem::path base;
    cli::Theme theme;
    cli::Theme plain_theme;

    WorkflowFixture()
        : base(std::filesystem::temp_directory_path() /
               ("lubancode_wf_frame_test_" + std::to_string(reinterpret_cast<std::uintptr_t>(this)))),
          theme(cli::BuiltinTheme("dark")),
          plain_theme(cli::BuiltinTheme("plain")) {
        std::error_code ec;
        std::filesystem::create_directories(base / ".lubancode" / "workflows" / "probe", ec);
        std::ofstream(base / ".lubancode" / "workflows" / "probe" / "workflow.yaml", std::ios::binary)
            << kProbeYaml;
        // 一只坏定义:YAML 根本不是映射 → broken 条目进 list 主表 [损坏] 行。
        std::filesystem::create_directories(base / ".lubancode" / "workflows" / "broken", ec);
        std::ofstream(base / ".lubancode" / "workflows" / "broken" / "workflow.yaml", std::ios::binary)
            << "just a scalar\n";
        // 一条运行账:manifest.json 记 workflow/版本/终态/起点。
        const std::filesystem::path run_dir = base / "workflow-runs" / "run-20260923-000000";
        std::filesystem::create_directories(run_dir, ec);
        std::ofstream(run_dir / "manifest.json", std::ios::binary)
            << R"json({"workflow_id": "probe", "workflow_version": "1.0.0", "started_at": "2026-09-23 00:00:00", "final_state": "succeeded"})json";
    }
    ~WorkflowFixture() {
        std::error_code ec;
        std::filesystem::remove_all(base, ec);
    }

    app::WorkflowCommandContext Context(const cli::Theme& use_theme) {
        app::WorkflowCommandContext ctx;
        ctx.project_root = base;
        ctx.user_root = std::nullopt;
        ctx.home_lubancode = base;
        ctx.theme = &use_theme;
        return ctx;
    }

    std::string Run(const std::string& args, const cli::Theme& use_theme) {
        OutputCapture capture;
        app::HandleWorkflowCommand(args, Context(use_theme));
        return capture.text();
    }
};

}  // namespace

TEST_CASE("list: 主表 + 明细列表,损坏条目 [损坏] error 档") {
    WorkflowFixture fx;
    const std::string out = fx.Run("list", fx.theme);
    const std::string plain = StripAnsi(out);

    REQUIRE(Contains(out, kBoxLightTopLeft));
    bool saw_header = false;
    std::istringstream scan(plain);
    std::string line;
    while (std::getline(scan, line)) {
        if (Contains(line, "workflow") && Contains(line, "id") && Contains(line, "source") &&
            Contains(line, "alias") && Contains(line, "state")) {
            saw_header = true;
        }
    }
    CHECK(saw_header);
    CHECK(Contains(plain, "探针工作流"));
    CHECK(Contains(plain, "probe v1.0.0"));
    CHECK(Contains(plain, "project"));
    CHECK(Contains(plain, "/probe"));
    // 明细:描述以 id 标签跟出;坏定义的诊断行同列。
    CHECK(LineWithText(plain, {"probe", "形状册的最小工作流"}));
    CHECK(Contains(plain, "broken"));
    CHECK(Contains(out, fx.theme.error + "[损坏]"));
}

TEST_CASE("list project / 空范围: 过滤照走,空态进框") {
    WorkflowFixture fx;
    {
        const std::string out = fx.Run("list home", fx.theme);
        const std::string plain = StripAnsi(out);
        CHECK(Contains(plain, "(没有 home workflow;.lubancode/workflows/ 下装一份就有)"));
        CHECK(!Contains(plain, "探针工作流"));
    }
    {
        const std::string out = fx.Run("list project", fx.theme);
        CHECK(Contains(StripAnsi(out), "探针工作流"));
    }
}

TEST_CASE("history: 运行账表格;空账进框") {
    WorkflowFixture fx;
    {
        const std::string out = fx.Run("history", fx.theme);
        const std::string plain = StripAnsi(out);
        REQUIRE(Contains(out, kBoxLightTopLeft));
        bool saw_header = false;
        std::istringstream scan(plain);
        std::string line;
        while (std::getline(scan, line)) {
            if (Contains(line, "run") && Contains(line, "workflow") && Contains(line, "state") &&
                Contains(line, "started")) {
                saw_header = true;
            }
        }
        CHECK(saw_header);
        CHECK(Contains(plain, "run-20260923-000000"));
        CHECK(Contains(plain, "probe v1.0.0"));
        CHECK(Contains(plain, "succeeded"));
        CHECK(Contains(plain, "2026-09-23 00:00:00"));
    }
    {
        const std::string out = fx.Run("history no-such", fx.theme);
        const std::string plain = StripAnsi(out);
        CHECK(Contains(plain, "(没有运行账)"));
    }
}

TEST_CASE("graph: 标题 frame + 图本体框外原样(ascii 与 mermaid)") {
    WorkflowFixture fx;
    {
        const std::string out = fx.Run("graph probe", fx.theme);
        const std::string plain = StripAnsi(out);
        REQUIRE(Contains(out, kBoxLightTopLeft));
        CHECK(Contains(plain, "探针工作流 [probe v1.0.0]"));
        CHECK(Contains(plain, "graph  ascii"));
        // 图本体框外原样:树形缩进字符与节点行在框外裸打。
        CHECK(Contains(plain, "`- "));
        CHECK(Contains(plain, "start"));
    }
    {
        const std::string out = fx.Run("graph probe mermaid", fx.theme);
        const std::string plain = StripAnsi(out);
        REQUIRE(Contains(out, kBoxLightTopLeft));
        CHECK(Contains(plain, "graph  mermaid"));
        CHECK(Contains(plain, "flowchart TD"));
        CHECK(Contains(plain, "start --> done"));
    }
    {
        // json 是机器面,不走新排版(字节级不变)。
        const std::string out = fx.Run("graph probe json", fx.theme);
        CHECK(!Contains(out, kBoxLightTopLeft));
        CHECK(Contains(StripAnsi(out), "\"id\""));
    }
}

TEST_CASE("graph 标题 frame: 80 列与 200 列都不破框(窄/宽两档钉)") {
    WorkflowFixture fx;
    // 与命令侧同一配方(workflow_commands.cpp 的 emit_graph_title):标题
    // = "<名> [<id> v<版本>]",超预算先按 预算-7 截断保字头再进框。这里
    // 用 80 列宽字长名把配方钉死:每一行显示宽不超预算。
    std::string wide_name;
    for (int i = 0; i < 40; ++i) {
        wide_name += "\xe9\x95\xbf";  // "长" 的 UTF-8;40 个宽字 = 80 列
    }
    const std::string raw_title = wide_name + " [probe v1.0.0]";
    const std::vector<cli::frame::Field> fields{cli::frame::Field{"graph", "ascii"}};
    for (const int width : {80, 200}) {
        CAPTURE(width);
        const std::string capped =
            cli::TruncateUtf8ToDisplayWidth(raw_title, width - 7);  // 命令侧同一顶帽
        const std::vector<std::string> lines =
            cli::frame::RenderKeyValues(capped, fields, fx.theme, cli::frame::Light(), width);
        REQUIRE(lines.size() >= 3);
        for (const std::string& line : lines) {
            const std::string bare = StripAnsi(line);
            CHECK(static_cast<int>(cli::DisplayWidthUtf8(bare)) <= width);
        }
        // 顶边框行是整行铺满的那一行:恰好抵预算,不越一格。
        const std::string bare_top = StripAnsi(lines.front());
        CHECK(static_cast<int>(cli::DisplayWidthUtf8(bare_top)) == width);
    }
    // 80 列窄档下标题被截(保字头),200 列宽档全名在。
    const std::string capped80 = cli::TruncateUtf8ToDisplayWidth(raw_title, 80 - 7);
    const std::string capped200 = cli::TruncateUtf8ToDisplayWidth(raw_title, 200 - 7);
    CHECK(cli::DisplayWidthUtf8(capped80) <= 73);
    CHECK(capped200 == raw_title);
}

TEST_CASE("plain 主题: list/history/graph 零转义字节、无框字形(T3/--no-color 路径)") {
    WorkflowFixture fx;
    for (const std::string& args : {"list", "history", "graph probe", "graph probe mermaid"}) {
        CAPTURE(args);
        const std::string out = fx.Run(args, fx.plain_theme);
        CHECK(out.find("\x1b") == std::string::npos);
        CHECK(!Contains(out, kBoxLightTopLeft));
        CHECK(!Contains(out, kBoxLightVert));
        // 信息一字不少:表头与图本体都在,只是没了色与框。
        if (args == "history") {
            CHECK(Contains(out, "run-20260923-000000"));
        }
        if (args == "graph probe mermaid") {
            CHECK(Contains(out, "flowchart TD"));
        }
    }
}
