// TUI 排版批 7:`lubancode kanban`(含 --no-open)产物提示的输出形状册。
//   - RenderKanbanNotice 纯函数直调:两句既有文案("看板已生成: <路径>" /
//     "N 个项目,M 场会话")原样进键值对框,标题 kanban 嵌上边框;
//   - plain 主题(T3/--no-color 同路)零转义字节、无框字形——合同第 3 条;
//   - dark 主题有 ┌ 框角、标题行在框上;
//   - RunKanbanCommand 主路径要真状态根与 workspace 扫描,落盘路径真机
//     未验(本册只钉渲染形状)。

#include <doctest/doctest.h>

#include <sstream>
#include <string>
#include <vector>

#include "cli/kanban_command.hpp"
#include "cli/theme.hpp"

using namespace lubancode;

namespace {

bool Contains(const std::string& haystack, const std::string& needle) {
    return haystack.find(needle) != std::string::npos;
}

std::string Join(const std::vector<std::string>& lines) {
    std::string out;
    for (const std::string& line : lines) {
        out += line;
        out += "\n";
    }
    return out;
}

const cli::Theme dark = cli::BuiltinTheme("dark");
const cli::Theme plain = cli::BuiltinTheme("plain");

constexpr const char* kBoxTopLeft = "\xe2\x94\x8c";  // ┌
constexpr const char* kBoxVert = "\xe2\x94\x82";     // │

}  // namespace

TEST_CASE("kanban 产物提示:键值对框,标题 kanban,两句文案拆列") {
    const std::vector<std::string> lines =
        cli::RenderKanbanNotice("D:/tmp/kanban.html", 3, 12, dark, /*width=*/0);
    const std::string out = Join(lines);
    CHECK(!lines.empty());

    // 标题嵌上边框;框角在。
    CHECK(Contains(out, "kanban"));
    CHECK(Contains(out, kBoxTopLeft));
    CHECK(Contains(out, kBoxVert));

    // 第一句按句内冒号拆两列:key=看板已生成,value=路径。
    CHECK(Contains(out, "看板已生成"));
    CHECK(Contains(out, "D:/tmp/kanban.html"));
    // 第二句(无冒号)整句进 value:项目数/会话数都在。
    CHECK(Contains(out, "3 个项目"));
    CHECK(Contains(out, "12 场会话"));
}

TEST_CASE("kanban 产物提示:plain 零转义、无框、标题独立成行") {
    const std::vector<std::string> lines =
        cli::RenderKanbanNotice("out.html", 0, 0, plain, /*width=*/0);
    const std::string out = Join(lines);

    CHECK(out.find("\x1b") == std::string::npos);  // 一个转义字节都没有
    CHECK(Contains(out, "kanban"));                // 标题降为独立内容行
    CHECK(Contains(out, "看板已生成"));
    CHECK(Contains(out, "0 个项目"));
    CHECK(out.find(kBoxTopLeft) == std::string::npos);
    CHECK(out.find(kBoxVert) == std::string::npos);
}
