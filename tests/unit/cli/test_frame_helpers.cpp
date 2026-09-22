// TUI 排版基件批 0(TUI 界面美化单):cli::frame::* 三助手 + divider::line
// + format::AlignLeft/AlignRight 的合同册。
//   - 三助手钉输出形状:边框三档字形、标题嵌上边框、列对齐(剥掉 ANSI 后
//     按位置比)、主题色注入(行里真出现该字段的 ANSI 前后缀);
//   - plain 主题(--no-color/T3 降级路径)钉"零转义字节、隐藏装饰":
//     无边框字形、项目符退 "-"、表头垫 "-" 横线;
//   - /usage 一段 mock 数据走三助手的 demo 以测试形式落(单子验收调整:
//     真机截屏留给后续批次收口);
//   - 旧主题兼容在 test_theme.cpp(11 枚新字段带缺省值、plain 全空)。

#include <doctest/doctest.h>

#include <string>
#include <vector>

#include "cli/divider.hpp"
#include "cli/format_utils.hpp"
#include "cli/line_editor.hpp"
#include "cli/terminal_frame.hpp"
#include "cli/theme.hpp"

using namespace lubancode::cli;
using lubancode::cli::divider::line;
using lubancode::cli::frame::Ascii;
using lubancode::cli::frame::BoxStyle;
using lubancode::cli::frame::Bullet;
using lubancode::cli::frame::CellTone;
using lubancode::cli::frame::Double;
using lubancode::cli::frame::Field;
using lubancode::cli::frame::FieldAccent;
using lubancode::cli::frame::ListRow;
using lubancode::cli::frame::Light;
using lubancode::cli::frame::RenderKeyValues;
using lubancode::cli::frame::RenderList;
using lubancode::cli::frame::RenderTable;
using lubancode::cli::frame::TableColumn;
using lubancode::cli::frame::TableRow;

namespace {

// 剥掉 CSI 序列(与 test_body_render_pipeline 同一把手写的尺)。
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

bool HasEscape(const std::string& text) {
    return text.find("\x1b") != std::string::npos;
}

// UTF-8 字节串常量(手写,免在测试里拼转义看花眼)。
constexpr const char* kBoxLightTopLeft = "\xe2\x94\x8c";      // ┌
constexpr const char* kBoxLightHoriz = "\xe2\x94\x80";        // ─
constexpr const char* kBoxLightBottomRight = "\xe2\x94\x98";  // ┘
constexpr const char* kBoxDoubleTopLeft = "\xe2\x95\x94";     // ╔
constexpr const char* kBoxDoubleVert = "\xe2\x95\x91";        // ║
constexpr const char* kBullet = "\xe2\x80\xa2";               // •

}  // namespace

// ---- BoxStyle 三档字形 ------------------------------------------------------

TEST_CASE("frame: 默认 BoxStyle 是 Light 档,三工厂各选各档") {
    constexpr BoxStyle style{};
    static_assert(style.flavor == BoxStyle::Flavor::Light);
    CHECK(Light().flavor == BoxStyle::Flavor::Light);
    CHECK(Ascii().flavor == BoxStyle::Flavor::Ascii);
    CHECK(Double().flavor == BoxStyle::Flavor::Double);
}

TEST_CASE("frame: RenderKeyValues 边框三档字形随 BoxStyle 切换") {
    const Theme dark = BuiltinTheme("dark");
    const std::vector<Field> fields{Field{"k", "v"}};

    const std::vector<std::string> light = RenderKeyValues("T", fields, dark, Light());
    REQUIRE(light.size() == 3);
    CHECK(Contains(light.front(), kBoxLightTopLeft));
    CHECK(Contains(light.front(), kBoxLightHoriz));
    CHECK(Contains(light.back(), kBoxLightBottomRight));

    const std::vector<std::string> dbl = RenderKeyValues("T", fields, dark, Double());
    REQUIRE(dbl.size() == 3);
    CHECK(Contains(dbl.front(), kBoxDoubleTopLeft));
    CHECK(Contains(dbl[1], kBoxDoubleVert));

    const std::vector<std::string> ascii = RenderKeyValues("T", fields, dark, Ascii());
    REQUIRE(ascii.size() == 3);
    CHECK(Contains(ascii.front(), "+-"));
    CHECK(Contains(ascii[1], "|"));
    // Ascii 档只在字形上降级,颜色照走主题。
    CHECK(HasEscape(ascii[1]));
}

// ---- 键值对助手 --------------------------------------------------------------

TEST_CASE("frame: RenderKeyValues 两列对齐,标题嵌上边框,key 走 row_label 色") {
    const Theme dark = BuiltinTheme("dark");
    const std::vector<Field> fields{
        Field{"model", "glm-5.3"},
        Field{"context", "48%"},
    };
    const std::vector<std::string> lines = RenderKeyValues("Session", fields, dark, Light());
    REQUIRE(lines.size() == 4);

    // 标题嵌进上边框,带 frame_title 色。
    CHECK(Contains(lines[0], "Session"));
    CHECK(Contains(lines[0], dark.frame_title + "Session" + dark.reset));

    // 两行内容:key 各自包 row_label 色;key 列按最宽者(7 列)补齐后,
    // value 起点同一字节位(剥 ANSI 后比位置)。
    CHECK(Contains(lines[1], dark.row_label));
    CHECK(Contains(lines[2], dark.row_label));
    const std::string row1 = StripAnsi(lines[1]);
    const std::string row2 = StripAnsi(lines[2]);
    CHECK(Contains(row1, "model"));
    CHECK(Contains(row2, "context"));
    const std::size_t v1 = row1.find("glm-5.3");
    const std::size_t v2 = row2.find("48%");
    REQUIRE(v1 != std::string::npos);
    REQUIRE(v2 != std::string::npos);
    CHECK(v1 == v2);
}

TEST_CASE("frame: RenderKeyValues 的 accent 各档走既有语义色") {
    const Theme dark = BuiltinTheme("dark");
    const std::vector<Field> fields{
        Field{"error", "boom", FieldAccent::Error},
        Field{"muted", "note", FieldAccent::Muted},
        Field{"pass", "ok", FieldAccent::Pass},
        Field{"skip", "later", FieldAccent::Skip},
        Field{"stats", "12k", FieldAccent::Stats},
    };
    const std::vector<std::string> lines = RenderKeyValues("T", fields, dark, Light());
    REQUIRE(lines.size() == 7);
    CHECK(Contains(lines[1], dark.error + "boom" + dark.reset));
    CHECK(Contains(lines[2], dark.row_muted + "note" + dark.reset));
    CHECK(Contains(lines[3], dark.table_pass + "ok" + dark.reset));
    CHECK(Contains(lines[4], dark.table_skip + "later" + dark.reset));
    CHECK(Contains(lines[5], dark.stats + "12k" + dark.reset));
}

TEST_CASE("frame: RenderKeyValues 宽预算超了截 value,整行不破预算") {
    const Theme dark = BuiltinTheme("dark");
    const std::vector<Field> fields{Field{"key", "a-very-long-value-that-will-not-fit-at-all"}};
    const std::vector<std::string> lines = RenderKeyValues("T", fields, dark, Light(), /*width=*/14);
    REQUIRE(lines.size() == 3);
    // 预算 14 = 边框 4 + 内容 10;key 3 列 + 2 格,value 截到 5 列("a-ver")。
    CHECK(Contains(StripAnsi(lines[1]), "a-ver"));
    CHECK(static_cast<int>(DisplayWidthUtf8(StripAnsi(lines[1]))) == 14);
    CHECK(Contains(StripAnsi(lines[1]), "key"));
}

TEST_CASE("frame: 空输入给空输出,不画空框") {
    const Theme dark = BuiltinTheme("dark");
    CHECK(RenderKeyValues("T", {}, dark, Light()).empty());
    CHECK(RenderList("T", {}, dark, Light()).empty());
    CHECK(RenderTable("T", {}, {}, dark, Light()).empty());
    CHECK(RenderTable("T", {TableColumn{"h"}}, {}, dark, Light()).empty());
}

// ---- 列表助手 ----------------------------------------------------------------

TEST_CASE("frame: RenderList 行形与项目符两档色") {
    const Theme dark = BuiltinTheme("dark");
    const std::vector<ListRow> rows{
        ListRow{"mem-1", "架构观", "(global)", Bullet::User},
        ListRow{"mem-2", "工作流", "", Bullet::Project},
        ListRow{"plain-row", "无项目符行", "hint"},
    };
    const std::vector<std::string> lines = RenderList("Memory", rows, dark, Light());
    REQUIRE(lines.size() == 5);  // 上边框 + 3 行 + 下边框
    CHECK(Contains(lines[0], "Memory"));

    // 项目符色分档:user 一档、project 一档,行里各自出现。
    CHECK(Contains(lines[1], dark.list_bullet_user + kBullet + dark.reset));
    CHECK(Contains(lines[2], dark.list_bullet_project + kBullet + dark.reset));

    // label 列按最宽者(9 列)补齐:三行 value 起点同字节位(中文等宽,
    // 剥 ANSI 后可比)。
    const std::string r1 = StripAnsi(lines[1]);
    const std::string r2 = StripAnsi(lines[2]);
    const std::string r3 = StripAnsi(lines[3]);
    const std::size_t v1 = r1.find("架构观");
    const std::size_t v2 = r2.find("工作流");
    const std::size_t v3 = r3.find("无项目符行");
    REQUIRE(v1 != std::string::npos);
    REQUIRE(v2 != std::string::npos);
    REQUIRE(v3 != std::string::npos);
    CHECK(v1 == v2);
    CHECK(v1 == v3);

    // hint 走 key_hint 色。
    CHECK(Contains(lines[1], dark.key_hint + "(global)" + dark.reset));
    CHECK(Contains(lines[3], dark.key_hint + "hint" + dark.reset));
}

TEST_CASE("frame: RenderList 宽预算下先丢 hint 再截 value,整行不破预算") {
    const Theme dark = BuiltinTheme("dark");
    const std::vector<ListRow> rows{
        ListRow{"label", "0123456789abcdef", "(hint)"},
    };
    const auto lines = RenderList("T", rows, dark, Light(), /*width=*/18);
    REQUIRE(lines.size() == 3);
    const std::string row = StripAnsi(lines[1]);
    // 预算 18 = 边框 4 + 内容 14:bullet 2 + label 5 + 2 格,value 只剩 5 列;
    // value16+hint 装不下,hint 先下车。
    CHECK(Contains(row, "01234"));
    CHECK_FALSE(Contains(row, "(hint)"));
    CHECK(static_cast<int>(DisplayWidthUtf8(row)) == 18);
}

// ---- 表格助手 ----------------------------------------------------------------

TEST_CASE("frame: RenderTable 列宽自适应、首列 row_label、tone 走主题色") {
    const Theme dark = BuiltinTheme("dark");
    const std::vector<TableColumn> columns{
        TableColumn{"NAME"},
        TableColumn{"TOKENS", /*min_width=*/8, /*align_right=*/true},
        TableColumn{"STATE"},
    };
    const std::vector<TableRow> rows{
        TableRow{{"glm-5.3", "12.3k", "pass"}, {CellTone::Normal, CellTone::Normal, CellTone::Pass}},
        TableRow{{"gpt-5x", "1.1M", "skip"}, {CellTone::Normal, CellTone::Normal, CellTone::Skip}},
        TableRow{{"kimi", "999", "fail"}, {CellTone::Normal, CellTone::Normal, CellTone::Fail}},
    };
    const std::vector<std::string> lines = RenderTable("Usage", columns, rows, dark, Light());
    REQUIRE(lines.size() == 5);  // 上边框 + 表头 + 3 行 + 下边框
    CHECK(Contains(lines[0], "Usage"));

    // 表头整行 table_header 色;列宽 NAME=7(数据定宽)、TOKENS=8(min_width)、
    // STATE=5(表头定宽)。
    CHECK(Contains(lines[1], dark.table_header));
    const std::string header = StripAnsi(lines[1]);
    CHECK(Contains(header, "NAME     TOKENS    STATE"));

    const std::string row1 = StripAnsi(lines[2]);
    const std::string row2 = StripAnsi(lines[3]);
    const std::string row3 = StripAnsi(lines[4]);
    // 首列左对齐:三行首列同列起。
    CHECK(row1.find("glm-5.3") == row2.find("gpt-5x"));
    CHECK(row1.find("glm-5.3") == row3.find("kimi"));
    // 数值列右对齐:右端跨行对齐(列宽 8),起点随字宽浮动。
    CHECK(row1.find("12.3k") + std::string("12.3k").size() ==
          row2.find("1.1M") + std::string("1.1M").size());
    CHECK(row1.find("12.3k") + std::string("12.3k").size() ==
          row3.find("999") + std::string("999").size());
    // 状态列左对齐同列起(末列不补尾随空格)。
    CHECK(row1.find("pass") == row2.find("skip"));
    CHECK(row1.find("pass") == row3.find("fail"));

    // 首列加粗档(row_label)、tone 三档色:Pass/Skip 走新字段,Fail 走 error。
    CHECK(Contains(lines[2], dark.row_label + "glm-5.3" + dark.reset));
    CHECK(Contains(lines[2], dark.table_pass + "pass" + dark.reset));
    CHECK(Contains(lines[3], dark.table_skip + "skip" + dark.reset));
    CHECK(Contains(lines[4], dark.error + "fail" + dark.reset));
}

TEST_CASE("frame: RenderTable 窄预算从最宽列起削,整行不破预算") {
    const Theme dark = BuiltinTheme("dark");
    const std::vector<TableColumn> columns{
        TableColumn{"A"},
        TableColumn{"BBBBBBBBBB"},
    };
    const std::vector<TableRow> rows{
        TableRow{{"x", "0123456789"}},
    };
    const auto lines = RenderTable("T", columns, rows, dark, Light(), /*width=*/12);
    REQUIRE(lines.size() == 4);
    for (std::size_t i = 0; i < lines.size(); ++i) {
        CHECK(static_cast<int>(DisplayWidthUtf8(StripAnsi(lines[i]))) <= 12);
    }
}

// ---- plain 降级路径(单子合同第 3 条) ---------------------------------------

TEST_CASE("frame: plain 主题零转义字节、隐藏装饰(无边框字形)、标题独立成行") {
    const Theme plain = BuiltinTheme("plain");
    const std::vector<Field> fields{Field{"k", "v", FieldAccent::Error}};
    const std::vector<ListRow> list_rows{ListRow{"l", "v", "h", Bullet::User}};
    const std::vector<TableColumn> columns{TableColumn{"H1"}, TableColumn{"H2"}};
    const std::vector<TableRow> table_rows{TableRow{{"a", "b"}, {CellTone::Pass, CellTone::Fail}}};

    // 键值对 plain 形:标题一行 + 内容一行,零转义。
    const auto kv = RenderKeyValues("Title", fields, plain, Light());
    REQUIRE(kv.size() == 2);
    CHECK(kv[0] == "Title");
    CHECK(kv[1] == "k  v");

    // 列表 plain 形:项目符退 "-",hint 原样跟排。
    const auto list = RenderList("Title", list_rows, plain, Light());
    REQUIRE(list.size() == 2);
    CHECK(list[0] == "Title");
    CHECK(list[1] == "- l  v  h");

    // 表格 plain 形:表头下垫一条 "-" 横线顶替颜色分隔,数据行照常对齐。
    const auto table = RenderTable("Title", columns, table_rows, plain, Light());
    REQUIRE(table.size() == 4);
    CHECK(table[0] == "Title");
    CHECK(table[1] == "H1  H2");
    CHECK(table[2] == "------");  // 各列宽(2+2)+ 两格分隔 = 6 列横线
    CHECK(table[3] == "a   b");

    for (const std::string& text : kv) {
        CHECK_FALSE(HasEscape(text));
        CHECK_FALSE(Contains(text, kBoxLightTopLeft));
    }
    for (const std::string& text : list) {
        CHECK_FALSE(HasEscape(text));
    }
    for (const std::string& text : table) {
        CHECK_FALSE(HasEscape(text));
    }
}

TEST_CASE("frame: ResolveTheme 降级(no-color)下的助手输出同样零转义") {
    // --no-color 的正路:ResolveTheme(name, false) 强制 plain。三助手在该
    // 主题下不管传哪档 BoxStyle,输出都不夹一个转义字节、不出边框字形。
    const Theme degraded = ResolveTheme("dark", /*enable_colors=*/false);
    const std::vector<Field> fields{Field{"k", "v"}};
    for (const BoxStyle style : {Light(), Double(), Ascii()}) {
        for (const std::string& text : RenderKeyValues("T", fields, degraded, style)) {
            CHECK_FALSE(HasEscape(text));
            CHECK_FALSE(Contains(text, kBoxLightTopLeft));
        }
    }
}

// ---- /usage demo:mock 数据走三助手(验收以测试承载) -------------------------

TEST_CASE("frame: /usage mock 数据走三助手的 demo 形状") {
    // 对应 /usage 人看分支的骨架:汇总键值对 + 按用途表格 + 按模型列表。
    // 文案是 mock 的(真命令文案走 i18n,批 5 才接)。
    const Theme dark = BuiltinTheme("dark");
    const std::vector<Field> summary{
        Field{"session", "today-42"},
        Field{"requests", "37"},
        Field{"total", "$1.250000"},
    };
    const std::vector<TableColumn> purpose_columns{
        TableColumn{"PURPOSE"},
        TableColumn{"TOKENS", 8, true},
        TableColumn{"STATE"},
    };
    const std::vector<TableRow> purpose_rows{
        TableRow{{"chat", "890k", "pass"}, {CellTone::Normal, CellTone::Normal, CellTone::Pass}},
        TableRow{{"summary", "140k", "skip"}, {CellTone::Normal, CellTone::Normal, CellTone::Skip}},
    };
    const std::vector<ListRow> model_rows{
        ListRow{"glm-5.3", "62% tokens", "(current)", Bullet::Project},
        ListRow{"gpt-5x", "38% tokens", "", Bullet::None},
    };

    const auto summary_lines = RenderKeyValues("Usage summary", summary, dark, Light());
    const auto purpose_lines = RenderTable("By purpose", purpose_columns, purpose_rows, dark, Light());
    const auto model_lines = RenderList("By model", model_rows, dark, Light());

    REQUIRE(summary_lines.size() == 5);
    REQUIRE(purpose_lines.size() == 5);
    REQUIRE(model_lines.size() == 4);

    // 三段各有 frame 标题,拼起来是一份可读报告;逐行走 TermOut 是调用方
    // 的事,这里只钉形状。
    std::vector<std::string> report;
    report.insert(report.end(), summary_lines.begin(), summary_lines.end());
    report.insert(report.end(), purpose_lines.begin(), purpose_lines.end());
    report.insert(report.end(), model_lines.begin(), model_lines.end());
    std::size_t escapes = 0;
    std::size_t titled = 0;
    for (const std::string& text : report) {
        if (HasEscape(text)) {
            ++escapes;
        }
        if (Contains(text, "Usage summary") || Contains(text, "By purpose") ||
            Contains(text, "By model")) {
            ++titled;
        }
    }
    CHECK(titled == 3);
    CHECK(escapes > 0);  // dark 主题下确有主题色注入

    // 表格行:数值列右对齐(右端跨行齐)、状态列三档色都在。
    const std::string p1 = StripAnsi(purpose_lines[2]);
    const std::string p2 = StripAnsi(purpose_lines[3]);
    CHECK(p1.find("890k") + 4 == p2.find("140k") + 5);
    CHECK(p1.find("pass") == p2.find("skip"));
    CHECK(Contains(p1, "pass"));
    CHECK(Contains(p2, "skip"));
    CHECK(Contains(purpose_lines[2], dark.table_pass + "pass" + dark.reset));
    CHECK(Contains(purpose_lines[3], dark.table_skip + "skip" + dark.reset));

    // 列表行:bullet 走 project 档色。
    CHECK(Contains(model_lines[1], dark.list_bullet_project));
}

// ---- divider::line 统一接口 ---------------------------------------------------

TEST_CASE("divider::line: 三档字符与宽度账") {
    CHECK(line(divider::Style::Ascii, 5) == "-----");
    // "─" U+2500 三字节一枚。
    CHECK(line(divider::Style::Light, 3).size() == 9);
    CHECK(line(divider::Style::Light, 1) == "\xe2\x94\x80");
    // "━" U+2501 同为三字节一枚,且与 Light 档不同字符。
    const std::string heavy = line(divider::Style::Heavy, 2);
    CHECK(heavy.size() == 6);
    CHECK(heavy != line(divider::Style::Light, 2));
    // width <= 0 空串,与 BuildDividerLine 同规。
    CHECK(line(divider::Style::Light, 0).empty());
    CHECK(line(divider::Style::Ascii, -3).empty());
}

// ---- format::AlignLeft / AlignRight -------------------------------------------

TEST_CASE("format: AlignLeft/AlignRight 半角与中文宽度账") {
    CHECK(format::AlignLeft("ab", 5) == "ab   ");
    CHECK(format::AlignRight("ab", 5) == "   ab");
    // 中文占两列:两字 4 列,补 1 列。
    CHECK(format::AlignLeft("工作", 5) == "工作 ");
    CHECK(format::AlignRight("工作", 5) == " 工作");
    CHECK(format::AlignLeft("", 3) == "   ");
    CHECK(format::AlignRight("", 3) == "   ");
    // width <= 0 给空串。
    CHECK(format::AlignLeft("abc", 0).empty());
    CHECK(format::AlignRight("abc", -1).empty());
}

TEST_CASE("format: AlignLeft/AlignRight 超宽先截断(保字头,不劈宽字)") {
    CHECK(format::AlignLeft("abcdef", 4) == "abcd");
    CHECK(format::AlignRight("abcdef", 4) == "abcd");
    // 5 列装"工作流"(6 列):截到前两字,不劈第三个字的一半。
    CHECK(format::AlignLeft("工作流", 5) == "工作");
    // 恰好等宽:原样。
    CHECK(format::AlignLeft("工作", 4) == "工作");
    CHECK(format::AlignRight("abcd", 4) == "abcd");
}
