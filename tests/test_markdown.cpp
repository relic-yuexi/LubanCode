// markdown(0.18.x):终端 markdown 渲染的纯函数单测——DetectMarkdownStructure
// 的有无判定,RenderMarkdown 的标题三级/列表嵌套/粗斜体码混排/代码块内不
// 解析/表格对齐(含 CJK 列)/表格超宽截断/引用/非 markdown 原样/混合文档。

#include <doctest/doctest.h>

#include <string>
#include <vector>

#include "cli/line_editor.hpp"  // DisplayWidthUtf8
#include "cli/markdown.hpp"
#include "cli/theme.hpp"

using lubancode::cli::BuiltinTheme;
using lubancode::cli::DetectMarkdownStructure;
using lubancode::cli::DisplayWidthUtf8;
using lubancode::cli::RenderMarkdown;

namespace {

// 剥掉一行里的全部 ANSI 转义序列(\x1b[ ... 终止字母),量显示宽度用。
std::string StripAnsi(const std::string& line) {
    std::string out;
    std::size_t i = 0;
    while (i < line.size()) {
        if (line[i] == '\x1b' && i + 1 < line.size() && line[i + 1] == '[') {
            i += 2;
            while (i < line.size() && !((line[i] >= 'a' && line[i] <= 'z') || (line[i] >= 'A' && line[i] <= 'Z'))) {
                ++i;
            }
            if (i < line.size()) {
                ++i;  // 终止字母本身
            }
            continue;
        }
        out += line[i];
        ++i;
    }
    return out;
}

bool Contains(const std::string& haystack, const std::string& needle) {
    return haystack.find(needle) != std::string::npos;
}

// 在渲染结果里找第一个含 needle 的行号,找不到给 -1。
int FindLine(const std::vector<std::string>& lines, const std::string& needle) {
    for (std::size_t i = 0; i < lines.size(); ++i) {
        if (Contains(lines[i], needle)) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

constexpr const char* kBullet = "\xE2\x80\xA2";    // •
constexpr const char* kVBar = "\xE2\x94\x82";      // │
constexpr const char* kHBar = "\xE2\x94\x80";      // ─
constexpr const char* kEllipsis = "\xE2\x80\xA6";  // …

}  // namespace

// ---- 结构检测 -------------------------------------------------------------

TEST_CASE("DetectMarkdownStructure: 各类结构逐一命中") {
    CHECK(DetectMarkdownStructure("# 标题"));
    CHECK(DetectMarkdownStructure("前文\n## 二级标题\n后文"));
    CHECK(DetectMarkdownStructure("- 列表项"));
    CHECK(DetectMarkdownStructure("* 列表项"));
    CHECK(DetectMarkdownStructure("1. 第一条"));
    CHECK(DetectMarkdownStructure("```\ncode\n```"));
    CHECK(DetectMarkdownStructure("| a | b |\n| c | d |"));
    CHECK(DetectMarkdownStructure("> 引用一句"));
    CHECK(DetectMarkdownStructure("这里有 **重点** 一处"));
    CHECK(DetectMarkdownStructure("行内 `code` 一段"));
    CHECK(DetectMarkdownStructure("  - 缩进的列表也算"));
}

TEST_CASE("DetectMarkdownStructure: 纯文本不命中") {
    CHECK_FALSE(DetectMarkdownStructure(""));
    CHECK_FALSE(DetectMarkdownStructure("今天天气不错,适合写代码。"));
    CHECK_FALSE(DetectMarkdownStructure("第一行\n第二行\n第三行都是散文。"));
    // 单个 |、单个 `、单个 ** 都不够数。
    CHECK_FALSE(DetectMarkdownStructure("管道符 | 一个不算表格"));
    CHECK_FALSE(DetectMarkdownStructure("一个反引号 ` 不算行内码"));
    CHECK_FALSE(DetectMarkdownStructure("孤零零的 ** 不算粗体"));
    // #号后面没空格(比如 #include)不算标题。
    CHECK_FALSE(DetectMarkdownStructure("#include <vector> 这样的行"));
    // 单行 |xx| 不足两行,不算表格。
    CHECK_FALSE(DetectMarkdownStructure("|好像表格|\n但下一行不是"));
    // 减号开头但没跟空格,不算列表。
    CHECK_FALSE(DetectMarkdownStructure("-42 是个负数"));
}

// ---- 标题 -----------------------------------------------------------------

TEST_CASE("RenderMarkdown: 标题三级,一级最醒目(下划线),前后留空行") {
    const auto theme = BuiltinTheme("dark");
    const auto lines = RenderMarkdown("正文开头\n# 一级\n## 二级\n### 三级\n正文结尾", theme, 80);

    const int h1 = FindLine(lines, "一级");
    const int h2 = FindLine(lines, "二级");
    const int h3 = FindLine(lines, "三级");
    REQUIRE(h1 >= 0);
    REQUIRE(h2 >= 0);
    REQUIRE(h3 >= 0);

    // 一级:bold + 下划线 + 主题色;二级:bold + 主题色、无下划线;三级:bold。
    CHECK(Contains(lines[static_cast<std::size_t>(h1)], "\x1b[1m"));
    CHECK(Contains(lines[static_cast<std::size_t>(h1)], "\x1b[4m"));
    CHECK(Contains(lines[static_cast<std::size_t>(h1)], theme.banner));
    CHECK(Contains(lines[static_cast<std::size_t>(h2)], "\x1b[1m"));
    CHECK_FALSE(Contains(lines[static_cast<std::size_t>(h2)], "\x1b[4m"));
    CHECK(Contains(lines[static_cast<std::size_t>(h2)], theme.banner));
    CHECK(Contains(lines[static_cast<std::size_t>(h3)], "\x1b[1m"));
    CHECK_FALSE(Contains(lines[static_cast<std::size_t>(h3)], theme.banner));
    // # 号本身不再出现。
    CHECK_FALSE(Contains(StripAnsi(lines[static_cast<std::size_t>(h1)]), "#"));
    // 标题前后各留一空行(夹在正文中间的三个标题,彼此之间空行去重)。
    CHECK(lines[static_cast<std::size_t>(h1) - 1].empty());
    CHECK(lines[static_cast<std::size_t>(h1) + 1].empty());
    CHECK(lines[static_cast<std::size_t>(h3) + 1].empty());
}

TEST_CASE("RenderMarkdown: plain 主题标题只剥井号,不夹 ANSI") {
    const auto lines = RenderMarkdown("# 标题", BuiltinTheme("plain"), 80);
    REQUIRE(lines.size() == 1);
    CHECK(lines[0] == "标题");
}

// ---- 列表 -----------------------------------------------------------------

TEST_CASE("RenderMarkdown: 无序列表换圆点,嵌套按层加缩进") {
    const auto theme = BuiltinTheme("dark");
    const auto lines = RenderMarkdown("- 第一层\n  - 第二层\n    - 第三层\n* 星号也认", theme, 80);
    REQUIRE(lines.size() == 4);
    CHECK(lines[0] == std::string("  ") + kBullet + " \xE7\xAC\xAC\xE4\xB8\x80\xE5\xB1\x82");  // "  • 第一层"
    CHECK(lines[1].compare(0, 4, "    ") == 0);
    CHECK(Contains(lines[1], kBullet));
    CHECK(lines[2].compare(0, 6, "      ") == 0);
    CHECK(Contains(lines[2], kBullet));
    CHECK(lines[3] == std::string("  ") + kBullet + " \xE6\x98\x9F\xE5\x8F\xB7\xE4\xB9\x9F\xE8\xAE\xA4");
}

TEST_CASE("RenderMarkdown: 有序列表保留数字,两格缩进") {
    const auto lines = RenderMarkdown("1. 头一条\n2. 第二条\n10. 第十条", BuiltinTheme("dark"), 80);
    REQUIRE(lines.size() == 3);
    CHECK(lines[0].compare(0, 5, "  1. ") == 0);
    CHECK(lines[1].compare(0, 5, "  2. ") == 0);
    CHECK(lines[2].compare(0, 6, "  10. ") == 0);
}

// ---- 行内样式 -------------------------------------------------------------

TEST_CASE("RenderMarkdown: 粗斜体行内码混排") {
    const auto lines = RenderMarkdown("有 **粗体** 有 *斜体* 有 `code` 三样", BuiltinTheme("dark"), 120);
    REQUIRE(lines.size() == 1);
    const std::string& line = lines[0];
    CHECK(Contains(line, "\x1b[1m\xE7\xB2\x97\xE4\xBD\x93\x1b[22m"));  // bold 粗体
    CHECK(Contains(line, "\x1b[3m\xE6\x96\x9C\xE4\xBD\x93\x1b[23m"));  // italic 斜体
    CHECK(Contains(line, "\x1b[7mcode\x1b[27m"));                       // 反色行内码
    // 标记本身被剥掉。
    const std::string visible = StripAnsi(line);
    CHECK_FALSE(Contains(visible, "*"));
    CHECK_FALSE(Contains(visible, "`"));
}

TEST_CASE("RenderMarkdown: 配不上对的标记原样保留") {
    const auto lines = RenderMarkdown("孤 *星 没配对(后头是 3 * 4 这种)", BuiltinTheme("dark"), 120);
    REQUIRE(lines.size() == 1);
    // "*星 没配对(后头是 3 " 里的星号:闭合候选内容首尾带空格,斜体判定
    // 不认,原文原样过。
    CHECK(Contains(StripAnsi(lines[0]), "*"));
}

// ---- 代码块 ---------------------------------------------------------------

TEST_CASE("RenderMarkdown: 代码块三线表式,行首无前缀,块内 markdown 不解析") {
    const auto theme = BuiltinTheme("dark");
    const auto lines = RenderMarkdown("```cpp\nint main() { return 0; }\n# 不是标题\n**不是粗体**\n```", theme, 80);
    // 上线 + 语言 + 中线 + 三行裸码 + 下线 = 7 行。
    REQUIRE(lines.size() == 7);
    // 语言标签夹在头两线之间,淡色。
    CHECK(Contains(StripAnsi(lines[0]), "----"));
    CHECK(Contains(lines[1], theme.stats));
    CHECK(Contains(lines[1], "cpp"));
    CHECK(Contains(StripAnsi(lines[2]), "----"));
    // 内容行:裸奔无前缀,内容原样、不被解析。
    CHECK(Contains(lines[3], "int main() { return 0; }"));
    CHECK(StripAnsi(lines[3]).compare(0, 2, "  ") != 0);  // 不再有两格前缀
    CHECK_FALSE(Contains(StripAnsi(lines[3]), kVBar));    // 也没有竖线毛边
    CHECK(Contains(StripAnsi(lines[4]), "# 不是标题"));      // 井号原样在
    CHECK(Contains(StripAnsi(lines[5]), "**不是粗体**"));    // 星号原样在
    CHECK_FALSE(Contains(lines[5], "\x1b[1m\xE4\xB8\x8D"));  // 没被加粗
}

TEST_CASE("RenderMarkdown: 没有语言标记的代码块,两线夹裸码,不产出语言行") {
    const auto lines = RenderMarkdown("```\nx = 1\n```", BuiltinTheme("dark"), 80);
    REQUIRE(lines.size() == 3);
    CHECK(Contains(StripAnsi(lines[0]), "----"));
    CHECK(Contains(lines[1], "x = 1"));
    CHECK_FALSE(Contains(StripAnsi(lines[1]), kVBar));
    CHECK(Contains(StripAnsi(lines[2]), "----"));
}

// ---- 表格 -----------------------------------------------------------------

TEST_CASE("RenderMarkdown: 表格对齐(含 CJK 列),边线齐整,表头 bold") {
    const auto theme = BuiltinTheme("dark");
    const auto lines = RenderMarkdown(
        "| 名字 | 说明 |\n"
        "| --- | --- |\n"
        "| ab | 中文说明 |\n"
        "| 长一点的中文名字 | x |",
        theme, 80);
    // ┌ 边线 + 表头 + ├ 分隔 + 两行数据 + └ 边线 = 6 行。
    REQUIRE(lines.size() == 6);
    CHECK(Contains(lines[0], kHBar));
    CHECK(Contains(lines[2], kHBar));
    CHECK(Contains(lines[5], kHBar));
    // 表头 bold。
    CHECK(Contains(lines[1], "\x1b[1m"));
    // 所有行显示宽度一致(CJK 按 2 算,列对齐的硬标准)。
    const std::size_t w0 = DisplayWidthUtf8(StripAnsi(lines[0]));
    for (std::size_t i = 1; i < lines.size(); ++i) {
        CHECK(DisplayWidthUtf8(StripAnsi(lines[i])) == w0);
    }
    // 每行数据 3 根竖线(左、中、右)。
    int bars = 0;
    for (std::size_t pos = lines[3].find(kVBar); pos != std::string::npos; pos = lines[3].find(kVBar, pos + 3)) {
        ++bars;
    }
    CHECK(bars == 3);
    // 分隔行(| --- |)本身不渲染。
    CHECK(FindLine(lines, "---") == -1);
}

TEST_CASE("RenderMarkdown: 表格超宽按列截断加省略号,总宽不破 width-1") {
    const int width = 40;
    const auto lines = RenderMarkdown(
        "| 键 | 值 |\n"
        "| --- | --- |\n"
        "| 一个特别特别特别特别长的中文键名 | 一段特别特别特别特别长的中文值 |",
        BuiltinTheme("dark"), width);
    REQUIRE(lines.size() >= 4);
    bool truncated = false;
    for (const std::string& line : lines) {
        CHECK(DisplayWidthUtf8(StripAnsi(line)) <= static_cast<std::size_t>(width - 1));
        if (Contains(line, kEllipsis)) {
            truncated = true;
        }
    }
    CHECK(truncated);
}

TEST_CASE("RenderMarkdown: 凑不成表的孤行原样过") {
    const auto lines = RenderMarkdown("|只有一行|", BuiltinTheme("dark"), 80);
    REQUIRE(lines.size() == 1);
    CHECK(lines[0] == "|只有一行|");
}

// ---- 引用 -----------------------------------------------------------------

TEST_CASE("RenderMarkdown: 引用行淡色竖线前缀") {
    const auto theme = BuiltinTheme("dark");
    const auto lines = RenderMarkdown("> 引的一句话\n>", theme, 80);
    REQUIRE(lines.size() == 2);
    CHECK(Contains(lines[0], theme.stats + kVBar));
    CHECK(Contains(lines[0], "\xE5\xBC\x95\xE7\x9A\x84\xE4\xB8\x80\xE5\x8F\xA5\xE8\xAF\x9D"));
    CHECK(Contains(lines[1], kVBar));  // 空引用行也画竖线
}

// ---- 原样过 / 宽度铁律 ----------------------------------------------------

TEST_CASE("RenderMarkdown: 非 markdown 的散文一字不动") {
    const std::string text = "头一行散文。\n第二行还是散文,带个逗号。\n\n隔了空行的第三行。";
    const auto lines = RenderMarkdown(text, BuiltinTheme("dark"), 80);
    REQUIRE(lines.size() == 4);
    CHECK(lines[0] == "头一行散文。");
    CHECK(lines[1] == "第二行还是散文,带个逗号。");
    CHECK(lines[2].empty());
    CHECK(lines[3] == "隔了空行的第三行。");
}

TEST_CASE("RenderMarkdown: 所有输出行截到 width-1 以内(锚点铁律)") {
    const int width = 30;
    const std::string text =
        "# 一个特别特别特别特别特别特别长的标题\n"
        "- 一条特别特别特别特别特别特别长的列表项内容\n"
        "```\nauto extremely_long_identifier_name_for_test = some_function_call();\n```\n"
        "> 一句特别特别特别特别特别特别长的引用文字\n"
        "普通段落也一样特别特别特别特别特别特别长长长长长长。";
    for (const std::string& line : RenderMarkdown(text, BuiltinTheme("dark"), width)) {
        CHECK(DisplayWidthUtf8(StripAnsi(line)) <= static_cast<std::size_t>(width - 1));
    }
}

TEST_CASE("RenderMarkdown: 极窄终端(width=3/4/5)一切元素不破 width-1 铁律") {
    // 修复钉子:width<5 时表格压无可压仍画框线、代码块/引用/列表的固定
    // 前缀比 width-1 还宽,都会物理折行、毁掉重画的行数账。现在放不下就
    // 退回逐行裸截。
    const std::string text =
        "# 标题文字\n"
        "| 键 | 值 |\n"
        "| --- | --- |\n"
        "| 甲甲甲 | 乙乙乙 |\n"
        "- 无序列表项内容\n"
        "    - 嵌套两层的列表项\n"
        "1. 有序列表项内容\n"
        "> 引用的一句话\n"
        "```cpp\nint x = 42;\n```\n"
        "普通段落文字也来一行。";
    for (const int width : {3, 4, 5}) {
        CAPTURE(width);
        for (const std::string& line : RenderMarkdown(text, BuiltinTheme("dark"), width)) {
            CAPTURE(line);
            CHECK(DisplayWidthUtf8(StripAnsi(line)) <= static_cast<std::size_t>(width - 1));
        }
    }
}

TEST_CASE("RenderMarkdown: 窄到框线盛不下的表格退回逐行截断,不画边线") {
    // 一列的表框架底线就要 6 列;width=5(content_limit=4)画不下,退回
    // 原样行裸截——一根 ─ 都不该出现。
    const auto lines = RenderMarkdown("| 键 | 值 |\n| --- | --- |\n| 甲 | 乙 |", BuiltinTheme("dark"), 5);
    REQUIRE_FALSE(lines.empty());
    for (const std::string& line : lines) {
        CHECK_FALSE(Contains(line, kHBar));
        CHECK(DisplayWidthUtf8(StripAnsi(line)) <= 4);
    }
}

TEST_CASE("RenderMarkdown: 宽度刚够时表格照旧画框(退路不越界抢活)") {
    // 单列表:框架 = 2 竖线 + 列宽 + 两空格。width=20 绰绰有余,必须照常
    // 走框线渲染——退路只在压无可压时接手。
    const auto lines = RenderMarkdown("| 键 |\n| --- |\n| 甲 |", BuiltinTheme("dark"), 20);
    bool has_border = false;
    for (const std::string& line : lines) {
        if (Contains(line, kHBar)) {
            has_border = true;
        }
    }
    CHECK(has_border);
}

// ---- 混合文档 -------------------------------------------------------------

TEST_CASE("RenderMarkdown: 混合文档各元素齐活,顺序不乱") {
    const auto theme = BuiltinTheme("dark");
    const auto lines = RenderMarkdown(
        "## 自我介绍\n"
        "我是 **lubancode**,一个 `C++` 写的 CLI。\n"
        "| 项目 | 值 |\n"
        "| --- | --- |\n"
        "| 语言 | C++23 |\n"
        "- 会调工具\n"
        "- 会写代码\n"
        "```cpp\nint x = 42;\n```\n"
        "> 完。",
        theme, 100);

    const int heading = FindLine(lines, "自我介绍");
    const int intro = FindLine(lines, "lubancode");
    const int table_top = FindLine(lines, kHBar);
    const int item1 = FindLine(lines, "会调工具");
    const int code = FindLine(lines, "int x = 42;");
    const int quote = FindLine(lines, "完。");
    REQUIRE(heading >= 0);
    REQUIRE(intro >= 0);
    REQUIRE(table_top >= 0);
    REQUIRE(item1 >= 0);
    REQUIRE(code >= 0);
    REQUIRE(quote >= 0);
    CHECK(heading < intro);
    CHECK(intro < table_top);
    CHECK(table_top < item1);
    CHECK(item1 < code);
    CHECK(code < quote);
    // 抽查样式:标题 bold、粗体剥星号、列表圆点、代码竖线、引用竖线。
    CHECK(Contains(lines[static_cast<std::size_t>(heading)], "\x1b[1m"));
    CHECK(Contains(lines[static_cast<std::size_t>(intro)], "\x1b[1mlubancode\x1b[22m"));
    CHECK(Contains(lines[static_cast<std::size_t>(item1)], kBullet));
    CHECK(Contains(lines[static_cast<std::size_t>(code)], "int x = 42;"));  // 三线表式:裸码无竖线
    CHECK(Contains(lines[static_cast<std::size_t>(quote)], kVBar));
    // 整篇检测自然为真。
    CHECK(DetectMarkdownStructure("## 自我介绍\n- 会调工具"));
}

// ---- 软换行(视觉折行,不再截断丢内容) --------------------------------------

// 把渲染结果去掉 ANSI 后逐行拼回(不留分隔),用来核对内容是否被完整保留。
std::string JoinStripped(const std::vector<std::string>& lines) {
    std::string out;
    for (const std::string& line : lines) {
        out += StripAnsi(line);
    }
    return out;
}

TEST_CASE("RenderMarkdown: 长段落软换行铺满,内容一字不丢,各段不破 width-1") {
    const int width = 20;
    const std::string para = "abcdefghijklmnopqrstuvwxyz";  // 26 个 ASCII
    const auto lines = RenderMarkdown(para, BuiltinTheme("dark"), width);
    // 短宽必然折成多行(不会再是 1 行 + 省略号)。
    CHECK(lines.size() > 1);
    // 内容完整保留(拼回去等于原文)。
    CHECK(JoinStripped(lines) == para);
    // 没有省略号(软换行不截断)。
    for (const std::string& line : lines) {
        CHECK_FALSE(Contains(line, kEllipsis));
        CHECK(DisplayWidthUtf8(StripAnsi(line)) <= static_cast<std::size_t>(width - 1));
    }
}

TEST_CASE("RenderMarkdown: 长标题软换行,每段都带标题样式") {
    const int width = 12;
    const auto lines = RenderMarkdown("# 标题文字特别长要折行", BuiltinTheme("dark"), width);
    CHECK(lines.size() > 1);
    bool first = true;
    for (const std::string& line : lines) {
        if (first) {
            first = false;
            continue;  // 标题前留的空行
        }
        if (line.empty()) {
            continue;  // 标题后留的空行
        }
        // 每段标题都该带 bold。
        CHECK(Contains(line, "\x1b[1m"));
        CHECK(DisplayWidthUtf8(StripAnsi(line)) <= static_cast<std::size_t>(width - 1));
    }
}

TEST_CASE("RenderMarkdown: 长列表项软换行,续行对齐内容起点") {
    const int width = 14;
    const auto lines = RenderMarkdown("- 一条特别特别长的列表项内容在此", BuiltinTheme("dark"), width);
    CHECK(lines.size() > 1);
    // 第一行带圆点。
    CHECK(Contains(lines[0], kBullet));
    // 续行以两空格缩进开头(对齐内容),不带圆点。
    for (std::size_t i = 1; i < lines.size(); ++i) {
        CHECK(StripAnsi(lines[i]).substr(0, 2) == "  ");
        CHECK_FALSE(Contains(lines[i], kBullet));
        CHECK(DisplayWidthUtf8(StripAnsi(lines[i])) <= static_cast<std::size_t>(width - 1));
    }
}

TEST_CASE("RenderMarkdown: 长引用软换行,每段重复引用竖线") {
    const int width = 14;
    const auto lines = RenderMarkdown("> 一句特别特别长的引用文字在此", BuiltinTheme("dark"), width);
    CHECK(lines.size() > 1);
    // 折出来的每一段都该带引用竖线(贯通的引用观感)。
    for (const std::string& line : lines) {
        CHECK(Contains(line, kVBar));
        CHECK(DisplayWidthUtf8(StripAnsi(line)) <= static_cast<std::size_t>(width - 1));
    }
}

TEST_CASE("RenderMarkdown: 长代码块软换行,续行无前缀") {
    const int width = 16;
    const auto lines = RenderMarkdown("```\nauto very_long_identifier_name = 42;\n```", BuiltinTheme("dark"), width);
    CHECK(lines.size() > 2);  // 上下线 + 多行折行内容
    bool saw_code_line = false;
    for (const std::string& line : lines) {
        if (Contains(StripAnsi(line), "auto") || Contains(StripAnsi(line), "identifier")) {
            saw_code_line = true;
        }
        CHECK(DisplayWidthUtf8(StripAnsi(line)) <= static_cast<std::size_t>(width - 1));
    }
    CHECK(saw_code_line);
}

TEST_CASE("RenderMarkdown: 表格超宽仍按列截断加省略号,不软换行") {
    const int width = 30;
    const auto lines = RenderMarkdown(
        "| 键 | 值 |\n"
        "| --- | --- |\n"
        "| 一个特别特别特别长的键 | 一段特别特别特别长的值 |",
        BuiltinTheme("dark"), width);
    bool truncated = false;
    for (const std::string& line : lines) {
        CHECK(DisplayWidthUtf8(StripAnsi(line)) <= static_cast<std::size_t>(width - 1));
        if (Contains(line, kEllipsis)) {
            truncated = true;
        }
    }
    CHECK(truncated);  // 表格靠列对齐,折行会毁对齐——仍截断
}

