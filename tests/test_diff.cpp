// UI-C(0.13.0):diff 预览纯函数单测——ComputeLineDiff 的八类形状(纯增/
// 纯删/改中间/首尾改/无变化/完全重写/单行/空对空)、FormatDiff 的前缀与
// 行号栏、Claude Code Update 样式的染色(- 行红底、+ 行绿底、上下文行号
// 栏淡色、折叠标注不上底、截宽后 reset 仍在行尾)、行数与字节双限截断、
// 上下文折叠,以及 BuildEditDiff 的定位(单行/跨行/找不到回退/replace_all
// 处数)和 BuildWriteDiff 的新文件/覆盖两条路。

#include <doctest/doctest.h>

#include <optional>
#include <string>
#include <vector>

#include "cli/diff.hpp"
#include "cli/theme.hpp"

using lubancode::cli::BuildEditDiff;
using lubancode::cli::BuildWriteDiff;
using lubancode::cli::BuiltinTheme;
using lubancode::cli::ComputeLineDiff;
using lubancode::cli::DiffLine;
using lubancode::cli::DiffLineKind;
using lubancode::cli::FormatDiff;
using lubancode::cli::SplitDiffLines;

namespace {

int CountKind(const std::vector<DiffLine>& diff, DiffLineKind kind) {
    int n = 0;
    for (const auto& line : diff) {
        if (line.kind == kind) {
            ++n;
        }
    }
    return n;
}

std::vector<std::string> Lines(const std::string& text) {
    std::vector<std::string> out;
    std::size_t pos = 0;
    while (pos < text.size()) {
        const std::size_t nl = text.find('\n', pos);
        if (nl == std::string::npos) {
            out.push_back(text.substr(pos));
            break;
        }
        out.push_back(text.substr(pos, nl - pos));
        pos = nl + 1;
    }
    return out;
}

}  // namespace

// ---- SplitDiffLines --------------------------------------------------------

TEST_CASE("SplitDiffLines: 空串给空 vector,末尾无换行的最后一截也算一行") {
    CHECK(SplitDiffLines("").empty());
    CHECK(SplitDiffLines("a\nb\n") == std::vector<std::string>{"a", "b"});
    CHECK(SplitDiffLines("a\nb") == std::vector<std::string>{"a", "b"});
}

TEST_CASE("SplitDiffLines: 行尾 \\r 剥掉,CRLF 文件跟 LF 文本对得上") {
    CHECK(SplitDiffLines("a\r\nb\r\n") == std::vector<std::string>{"a", "b"});
}

// ---- ComputeLineDiff -------------------------------------------------------

TEST_CASE("ComputeLineDiff: 纯增(空对有)") {
    const auto diff = ComputeLineDiff({}, {"a", "b"});
    REQUIRE(diff.size() == 2);
    CHECK(diff[0].kind == DiffLineKind::Add);
    CHECK(diff[0].text == "a");
    CHECK(diff[0].new_no == 1);
    CHECK(diff[0].old_no == 0);
    CHECK(diff[1].kind == DiffLineKind::Add);
    CHECK(diff[1].new_no == 2);
}

TEST_CASE("ComputeLineDiff: 纯删(有对空)") {
    const auto diff = ComputeLineDiff({"a", "b"}, {});
    REQUIRE(diff.size() == 2);
    CHECK(diff[0].kind == DiffLineKind::Del);
    CHECK(diff[0].old_no == 1);
    CHECK(diff[0].new_no == 0);
    CHECK(diff[1].kind == DiffLineKind::Del);
    CHECK(diff[1].old_no == 2);
}

TEST_CASE("ComputeLineDiff: 改中间一行,前后是上下文") {
    const auto diff = ComputeLineDiff({"a", "b", "c"}, {"a", "x", "c"});
    REQUIRE(diff.size() == 4);
    CHECK(diff[0].kind == DiffLineKind::Context);
    CHECK(diff[0].old_no == 1);
    CHECK(diff[0].new_no == 1);
    CHECK(diff[1].kind == DiffLineKind::Del);
    CHECK(diff[1].text == "b");
    CHECK(diff[1].old_no == 2);
    CHECK(diff[2].kind == DiffLineKind::Add);
    CHECK(diff[2].text == "x");
    CHECK(diff[2].new_no == 2);
    CHECK(diff[3].kind == DiffLineKind::Context);
    CHECK(diff[3].text == "c");
}

TEST_CASE("ComputeLineDiff: 首行改、尾行改") {
    const auto head = ComputeLineDiff({"a", "b"}, {"x", "b"});
    REQUIRE(head.size() == 3);
    CHECK(head[0].kind == DiffLineKind::Del);
    CHECK(head[1].kind == DiffLineKind::Add);
    CHECK(head[2].kind == DiffLineKind::Context);

    const auto tail = ComputeLineDiff({"a", "b"}, {"a", "x"});
    REQUIRE(tail.size() == 3);
    CHECK(tail[0].kind == DiffLineKind::Context);
    CHECK(tail[1].kind == DiffLineKind::Del);
    CHECK(tail[2].kind == DiffLineKind::Add);
}

TEST_CASE("ComputeLineDiff: 无变化——全是上下文,行号一一对应") {
    const auto diff = ComputeLineDiff({"a", "b"}, {"a", "b"});
    REQUIRE(diff.size() == 2);
    CHECK(CountKind(diff, DiffLineKind::Del) == 0);
    CHECK(CountKind(diff, DiffLineKind::Add) == 0);
    CHECK(diff[1].old_no == 2);
    CHECK(diff[1].new_no == 2);
}

TEST_CASE("ComputeLineDiff: 完全重写——旧全删、新全增") {
    const auto diff = ComputeLineDiff({"a", "b"}, {"x", "y"});
    CHECK(CountKind(diff, DiffLineKind::Del) == 2);
    CHECK(CountKind(diff, DiffLineKind::Add) == 2);
    CHECK(CountKind(diff, DiffLineKind::Context) == 0);
}

TEST_CASE("ComputeLineDiff: 单行文件改一行") {
    const auto diff = ComputeLineDiff({"old"}, {"new"});
    REQUIRE(diff.size() == 2);
    CHECK(diff[0].kind == DiffLineKind::Del);
    CHECK(diff[0].old_no == 1);
    CHECK(diff[1].kind == DiffLineKind::Add);
    CHECK(diff[1].new_no == 1);
}

TEST_CASE("ComputeLineDiff: 空对空——空 diff") {
    CHECK(ComputeLineDiff({}, {}).empty());
}

TEST_CASE("ComputeLineDiff: 中间既有公共行又有增删,LCS 把公共行留成上下文") {
    // a c d e  ->  a b c e:增 b,删 d。
    const auto diff = ComputeLineDiff({"a", "c", "d", "e"}, {"a", "b", "c", "e"});
    CHECK(CountKind(diff, DiffLineKind::Del) == 1);
    CHECK(CountKind(diff, DiffLineKind::Add) == 1);
    CHECK(CountKind(diff, DiffLineKind::Context) == 3);
}

// ---- FormatDiff ------------------------------------------------------------

TEST_CASE("FormatDiff: plain 主题下前缀与行号栏——'  2 - b' 样式") {
    const auto theme = BuiltinTheme("plain");
    const auto diff = ComputeLineDiff({"a", "b", "c"}, {"a", "x", "c"});
    const std::string text = FormatDiff(diff, theme, /*width=*/0, /*max_lines=*/0, /*max_bytes=*/0);
    CHECK(text.find("  1   a") != std::string::npos);
    CHECK(text.find("  2 - b") != std::string::npos);
    CHECK(text.find("  2 + x") != std::string::npos);
    CHECK(text.find("  3   c") != std::string::npos);
    CHECK(text.find('\x1b') == std::string::npos);  // plain 不夹 ANSI
}

TEST_CASE("FormatDiff: 彩色主题——删除行红底、新增行绿底,整行套色、行尾 reset") {
    const auto theme = BuiltinTheme("dark");
    const auto diff = ComputeLineDiff({"a", "b", "c"}, {"a", "x", "c"});
    const std::string text = FormatDiff(diff, theme, 0, 0, 0);
    const auto lines = Lines(text);
    bool saw_del = false;
    bool saw_add = false;
    for (const auto& line : lines) {
        if (line.find(" - b") != std::string::npos) {
            // 整行(行号 + '- ' + 内容)以红底起头,行尾紧跟 reset——底色
            // 只铺到内容实际结尾,不填充到终端宽。
            CHECK(line.find(theme.diff_del_bg) == 0);
            CHECK(line.rfind(theme.reset) == line.size() - theme.reset.size());
            saw_del = true;
        } else if (line.find(" + x") != std::string::npos) {
            CHECK(line.find(theme.diff_add_bg) == 0);
            CHECK(line.rfind(theme.reset) == line.size() - theme.reset.size());
            saw_add = true;
        }
    }
    CHECK(saw_del);
    CHECK(saw_add);
}

TEST_CASE("FormatDiff: 彩色主题——上下文行行号栏淡色、正文原色、不上底") {
    const auto theme = BuiltinTheme("dark");
    const auto diff = ComputeLineDiff({"a", "b", "c"}, {"a", "x", "c"});
    const std::string text = FormatDiff(diff, theme, 0, 0, 0);
    for (const auto& line : Lines(text)) {
        if (line.find("  a") == std::string::npos && line.find("  c") == std::string::npos) {
            continue;
        }
        CHECK(line.find(theme.diff_line_no) == 0);  // 行号栏淡色起头
        CHECK(line.find(theme.diff_del_bg) == std::string::npos);  // 无底色
        CHECK(line.find(theme.diff_add_bg) == std::string::npos);
        // 行号栏染完就 reset,正文素面(reset 之后不再有 ANSI)。
        const std::size_t reset_pos = line.find(theme.reset);
        REQUIRE(reset_pos != std::string::npos);
        CHECK(line.find('\x1b', reset_pos + theme.reset.size()) == std::string::npos);
    }
}

TEST_CASE("FormatDiff: 彩色主题——折叠标注走淡色,不上底色") {
    const auto theme = BuiltinTheme("dark");
    std::vector<std::string> old_lines;
    std::vector<std::string> new_lines;
    for (int i = 1; i <= 20; ++i) {
        old_lines.push_back("line" + std::to_string(i));
        new_lines.push_back(i == 10 ? "CHANGED" : "line" + std::to_string(i));
    }
    const std::string text = FormatDiff(ComputeLineDiff(old_lines, new_lines), theme, 0, 0, 0);
    for (const auto& line : Lines(text)) {
        if (line.find("行未变") == std::string::npos) {
            continue;
        }
        CHECK(line.find(theme.stats) == 0);
        CHECK(line.find(theme.diff_del_bg) == std::string::npos);
        CHECK(line.find(theme.diff_add_bg) == std::string::npos);
    }
}

TEST_CASE("FormatDiff: 彩色主题——生成时截宽,截断后 reset 仍在行尾") {
    const auto theme = BuiltinTheme("dark");
    const std::string long_line(120, 'y');
    const std::string text = FormatDiff(ComputeLineDiff({}, {long_line}), theme, /*width=*/40, 0, 0);
    const auto lines = Lines(text);
    REQUIRE(!lines.empty());
    const std::string& line = lines[0];
    CHECK(line.find(theme.diff_add_bg) == 0);
    CHECK(line.rfind(theme.reset) == line.size() - theme.reset.size());  // 截断点在 reset 之前
    CHECK(line.find("...") != std::string::npos);
    // 去掉 ANSI 后正文不超宽(全 ASCII,字节数即显示宽)。
    const std::string plain =
        line.substr(theme.diff_add_bg.size(), line.size() - theme.diff_add_bg.size() - theme.reset.size());
    CHECK(plain.size() <= 40);
    CHECK(plain.find('\x1b') == std::string::npos);
}

TEST_CASE("FormatDiff: 无变化给固定提示") {
    const auto theme = BuiltinTheme("plain");
    const auto diff = ComputeLineDiff({"a"}, {"a"});
    CHECK(FormatDiff(diff, theme, 0, 0, 0) == "(无变化)\n");
    CHECK(FormatDiff({}, theme, 0, 0, 0) == "(无变化)\n");
}

TEST_CASE("FormatDiff: 变更行前后各 3 行上下文,更远的折叠标注") {
    const auto theme = BuiltinTheme("plain");
    // 20 行里只改第 10 行:1..6 折叠、7..9 上下文、10 变更、11..13 上下文、
    // 14..20 折叠。
    std::vector<std::string> old_lines;
    std::vector<std::string> new_lines;
    for (int i = 1; i <= 20; ++i) {
        old_lines.push_back("line" + std::to_string(i));
        new_lines.push_back(i == 10 ? "CHANGED" : "line" + std::to_string(i));
    }
    const std::string text = FormatDiff(ComputeLineDiff(old_lines, new_lines), theme, 0, 0, 0);
    CHECK(text.find("…(6 行未变)") != std::string::npos);
    CHECK(text.find("…(7 行未变)") != std::string::npos);
    CHECK(text.find(" 10 - line10") != std::string::npos);
    CHECK(text.find(" 10 + CHANGED") != std::string::npos);
    CHECK(text.find("line6") == std::string::npos);   // 折叠掉了
    CHECK(text.find("line7") != std::string::npos);   // ±3 上下文在
    CHECK(text.find("line13") != std::string::npos);
    CHECK(text.find("line14") == std::string::npos);
}

TEST_CASE("FormatDiff: 行数超限截断并标注省略行数") {
    const auto theme = BuiltinTheme("plain");
    std::vector<std::string> new_lines;
    for (int i = 0; i < 30; ++i) {
        new_lines.push_back("n" + std::to_string(i));
    }
    const std::string text = FormatDiff(ComputeLineDiff({}, new_lines), theme, 0, /*max_lines=*/10, 0);
    const auto lines = Lines(text);
    REQUIRE(lines.size() == 11);  // 10 行正文 + 1 行省略标注
    CHECK(lines.back() == "… 已省略 20 行(Ctrl+E 查看完整)");
}

TEST_CASE("FormatDiff: 字节超限同样截断标注") {
    const auto theme = BuiltinTheme("plain");
    std::vector<std::string> new_lines;
    for (int i = 0; i < 100; ++i) {
        new_lines.push_back(std::string(50, 'x'));
    }
    const std::string text = FormatDiff(ComputeLineDiff({}, new_lines), theme, 0, 0, /*max_bytes=*/600);
    CHECK(text.find("… 已省略 ") != std::string::npos);
    // 正文行数应该只有 600/(50+8) ≈ 10 行左右,绝没到 100 行。
    CHECK(Lines(text).size() < 15);
}

TEST_CASE("FormatDiff: 超宽行按显示宽度截断加 ...") {
    const auto theme = BuiltinTheme("plain");
    const std::string long_line(120, 'y');
    const std::string text = FormatDiff(ComputeLineDiff({}, {long_line}), theme, /*width=*/40, 0, 0);
    const auto lines = Lines(text);
    REQUIRE(!lines.empty());
    CHECK(lines[0].size() <= 40);
    CHECK(lines[0].find("...") != std::string::npos);
}

// ---- BuildEditDiff ---------------------------------------------------------

TEST_CASE("BuildEditDiff: 单行 old_string 定位——真实行号,前后带上下文") {
    const std::string content = "a\nb\nc\nd\ne\nf\ng\nh\ni\nj\n";
    const auto out = BuildEditDiff(content, "f", "F", /*replace_all=*/false);
    CHECK(out.located);
    CHECK(out.replaced_count == 1);
    const std::string text = FormatDiff(out.lines, BuiltinTheme("plain"), 0, 0, 0);
    CHECK(text.find("  6 - f") != std::string::npos);
    CHECK(text.find("  6 + F") != std::string::npos);
    CHECK(text.find("  3   c") != std::string::npos);  // 前 3 行上下文
    CHECK(text.find("  9   i") != std::string::npos);  // 后 3 行上下文
    CHECK(text.find("…(2 行未变)") != std::string::npos);  // 更远的折叠
}

TEST_CASE("BuildEditDiff: 跨行 old_string 正常替换") {
    const std::string content = "a\nb\nc\nd\n";
    const auto out = BuildEditDiff(content, "b\nc", "x", false);
    CHECK(out.located);
    CHECK(CountKind(out.lines, DiffLineKind::Del) == 2);
    CHECK(CountKind(out.lines, DiffLineKind::Add) == 1);
}

TEST_CASE("BuildEditDiff: 找不到 old_string 回退成只对比新旧两段") {
    const auto out = BuildEditDiff("a\nb\n", "zzz", "yyy", false);
    CHECK(!out.located);
    CHECK(out.replaced_count == 0);
    REQUIRE(out.lines.size() == 2);
    CHECK(out.lines[0].kind == DiffLineKind::Del);
    CHECK(out.lines[0].text == "zzz");
    CHECK(out.lines[1].kind == DiffLineKind::Add);
    CHECK(out.lines[1].text == "yyy");
}

TEST_CASE("BuildEditDiff: 文件内容为空(读不出来)同样走回退,不崩") {
    const auto out = BuildEditDiff("", "old", "new", false);
    CHECK(!out.located);
    CHECK(out.lines.size() == 2);
}

TEST_CASE("BuildEditDiff: replace_all 标注替换处数,每处各自成块") {
    const std::string content = "foo\na\nfoo\nb\n";
    const auto out = BuildEditDiff(content, "foo", "qux", /*replace_all=*/true);
    CHECK(out.located);
    CHECK(out.replaced_count == 2);
    CHECK(CountKind(out.lines, DiffLineKind::Del) == 2);
    CHECK(CountKind(out.lines, DiffLineKind::Add) == 2);
}

TEST_CASE("BuildEditDiff: replace_all=false 只预览第一处") {
    const std::string content = "foo\na\nfoo\n";
    const auto out = BuildEditDiff(content, "foo", "qux", false);
    CHECK(out.replaced_count == 1);
    CHECK(CountKind(out.lines, DiffLineKind::Del) == 1);
    CHECK(CountKind(out.lines, DiffLineKind::Add) == 1);
}

// ---- BuildWriteDiff --------------------------------------------------------

TEST_CASE("BuildWriteDiff: 新文件全部新增") {
    const auto diff = BuildWriteDiff(std::nullopt, "a\nb\n");
    REQUIRE(diff.size() == 2);
    CHECK(diff[0].kind == DiffLineKind::Add);
    CHECK(diff[0].new_no == 1);
    CHECK(diff[1].kind == DiffLineKind::Add);
    CHECK(diff[1].new_no == 2);
}

TEST_CASE("BuildWriteDiff: 覆盖已有文件做行级对比") {
    const auto diff = BuildWriteDiff(std::string("a\nb\n"), "a\nc\n");
    REQUIRE(diff.size() == 3);
    CHECK(diff[0].kind == DiffLineKind::Context);
    CHECK(diff[1].kind == DiffLineKind::Del);
    CHECK(diff[1].text == "b");
    CHECK(diff[2].kind == DiffLineKind::Add);
    CHECK(diff[2].text == "c");
}

TEST_CASE("BuildWriteDiff: CRLF 旧文件对 LF 新内容——行尾差异不算变化") {
    const auto diff = BuildWriteDiff(std::string("a\r\nb\r\n"), "a\nb\n");
    CHECK(CountKind(diff, DiffLineKind::Del) == 0);
    CHECK(CountKind(diff, DiffLineKind::Add) == 0);
}
