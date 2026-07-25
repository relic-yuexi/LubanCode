#include <doctest/doctest.h>

#include <string>

#include "cli/diff.hpp"
#include "cli/diff_syntax.hpp"
#include "cli/theme.hpp"

using namespace lubancode::cli;

namespace {

std::string StripAnsi(const std::string& text) {
    std::string out;
    for (std::size_t i = 0; i < text.size();) {
        if (text[i] == '\x1b' && i + 1 < text.size() && text[i + 1] == '[') {
            i += 2;
            while (i < text.size() && text[i] != 'm') {
                ++i;
            }
            if (i < text.size()) {
                ++i;
            }
            continue;
        }
        out.push_back(text[i++]);
    }
    return out;
}

}  // namespace

TEST_CASE("diff syntax: 文件扩展名选择词法器,未知文本不乱猜") {
    CHECK(DetectDiffSyntaxLanguage("src/main.CPP") == DiffSyntaxLanguage::Cpp);
    CHECK(DetectDiffSyntaxLanguage("C:\\work\\demo.py") == DiffSyntaxLanguage::Python);
    CHECK(DetectDiffSyntaxLanguage("app.tsx") == DiffSyntaxLanguage::JavaScript);
    CHECK(DetectDiffSyntaxLanguage("data.json") == DiffSyntaxLanguage::Json);
    CHECK(DetectDiffSyntaxLanguage("Dockerfile") == DiffSyntaxLanguage::Shell);
    CHECK(DetectDiffSyntaxLanguage("lib.rs") == DiffSyntaxLanguage::Rust);
    CHECK(DetectDiffSyntaxLanguage("main.go") == DiffSyntaxLanguage::Go);
    CHECK(DetectDiffSyntaxLanguage("notes.txt") == DiffSyntaxLanguage::Unknown);
}

TEST_CASE("diff syntax: C++ 关键字、类型、函数、字符串、数字、注释分层着色") {
    const Theme theme = BuiltinTheme("dark");
    const std::string code =
        "const std::string value = make_name(42, \"ok\"); // note";
    const std::string colored = HighlightDiffCodeLine(code, DiffSyntaxLanguage::Cpp, theme);

    CHECK(StripAnsi(colored) == code);
    CHECK(colored.find(theme.diff_syntax_keyword + "const") != std::string::npos);
    CHECK(colored.find(theme.diff_syntax_type + "std") != std::string::npos);
    CHECK(colored.find(theme.diff_syntax_type + "string") != std::string::npos);
    CHECK(colored.find(theme.diff_syntax_function + "make_name") != std::string::npos);
    CHECK(colored.find(theme.diff_syntax_number + "42") != std::string::npos);
    CHECK(colored.find(theme.diff_syntax_string + "\"ok\"") != std::string::npos);
    CHECK(colored.find(theme.diff_syntax_comment + "// note") != std::string::npos);
}

TEST_CASE("diff syntax: Python 识别 def、函数、内置类型、数字、字符串与井号注释") {
    const Theme theme = BuiltinTheme("light");
    const std::string code = "def solve(nums: list[int]) -> int: return len(nums) + 1  # answer";
    const std::string colored = HighlightDiffCodeLine(code, DiffSyntaxLanguage::Python, theme);

    CHECK(StripAnsi(colored) == code);
    CHECK(colored.find(theme.diff_syntax_keyword + "def") != std::string::npos);
    CHECK(colored.find(theme.diff_syntax_function + "solve") != std::string::npos);
    CHECK(colored.find(theme.diff_syntax_type + "list") != std::string::npos);
    CHECK(colored.find(theme.diff_syntax_type + "int") != std::string::npos);
    CHECK(colored.find(theme.diff_syntax_keyword + "return") != std::string::npos);
    CHECK(colored.find(theme.diff_syntax_function + "len") != std::string::npos);
    CHECK(colored.find(theme.diff_syntax_number + "1") != std::string::npos);
    CHECK(colored.find(theme.diff_syntax_comment + "# answer") != std::string::npos);
}

TEST_CASE("diff syntax: JS JSON Rust Go Shell 都有各自关键字或注释规则") {
    const Theme theme = BuiltinTheme("dark");
    CHECK(HighlightDiffCodeLine("const n = 1;", DiffSyntaxLanguage::JavaScript, theme)
              .find(theme.diff_syntax_keyword + "const") != std::string::npos);
    CHECK(HighlightDiffCodeLine("\"name\": true", DiffSyntaxLanguage::Json, theme)
              .find(theme.diff_syntax_function + "\"name\"") != std::string::npos);
    CHECK(HighlightDiffCodeLine("pub fn run() {}", DiffSyntaxLanguage::Rust, theme)
              .find(theme.diff_syntax_keyword + "pub") != std::string::npos);
    CHECK(HighlightDiffCodeLine("func run() int {}", DiffSyntaxLanguage::Go, theme)
              .find(theme.diff_syntax_keyword + "func") != std::string::npos);
    CHECK(HighlightDiffCodeLine("echo ok # note", DiffSyntaxLanguage::Shell, theme)
              .find(theme.diff_syntax_comment + "# note") != std::string::npos);
}

TEST_CASE("diff syntax: plain 主题逐字返回,没有 ANSI") {
    const Theme theme = BuiltinTheme("plain");
    const std::string code = "def f() -> int: return 1";
    const std::string out = HighlightDiffCodeLine(code, DiffSyntaxLanguage::Python, theme);
    CHECK(out == code);
    CHECK(out.find('\x1b') == std::string::npos);
}

TEST_CASE("FormatDiff: 语法前景叠在新增背景上,只在行末复位背景") {
    const Theme theme = BuiltinTheme("dark");
    const std::string code = "def answer() -> int: return 42";
    const std::string out =
        FormatDiff(ComputeLineDiff({}, {code}), theme, 0, 0, 0, "answer.py");

    CHECK(StripAnsi(out) == "  1 + " + code + "\n");
    CHECK(out.find(theme.diff_add_bg) == 0);
    CHECK(out.find(theme.diff_syntax_keyword + "def") != std::string::npos);
    CHECK(out.find(theme.diff_syntax_function + "answer") != std::string::npos);
    CHECK(out.find(theme.diff_syntax_type + "int") != std::string::npos);
    CHECK(out.find(theme.diff_syntax_number + "42") != std::string::npos);

    // token 间只许用 22;39m 恢复前景。全复位会顺手清掉绿底,只能落在行尾。
    const std::size_t first_reset = out.find(theme.reset);
    REQUIRE(first_reset != std::string::npos);
    CHECK(first_reset == out.size() - theme.reset.size() - 1);
}

TEST_CASE("FormatDiff: 未知扩展名维持原式,不插语法 token 色") {
    const Theme theme = BuiltinTheme("dark");
    const std::string out =
        FormatDiff(ComputeLineDiff({}, {"const value = 1"}), theme, 0, 0, 0, "notes.txt");
    CHECK(out.find(theme.diff_syntax_keyword) == std::string::npos);
    CHECK(StripAnsi(out) == "  1 + const value = 1\n");
}
