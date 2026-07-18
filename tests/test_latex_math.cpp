#include <doctest/doctest.h>

#include <string>
#include <vector>

#include "cli/latex_math.hpp"
#include "cli/line_editor.hpp"
#include "cli/markdown.hpp"
#include "cli/theme.hpp"

using lubancode::cli::DisplayWidthUtf8;
using lubancode::cli::RenderLatexBlock;
using lubancode::cli::RenderLatexInline;

namespace {

std::string Inline(const std::string &latex) {
    const auto rendered = RenderLatexInline(latex);
    REQUIRE(rendered.has_value());
    return *rendered;
}

std::vector<std::string> Block(const std::string &latex) {
    const auto rendered = RenderLatexBlock(latex);
    REQUIRE(rendered.has_value());
    return *rendered;
}

} // namespace

TEST_CASE("LaTeX math: 希腊字母白名单") {
    CHECK(Inline(R"(\alpha \beta \Gamma \Omega)") == "α β Γ Ω");
    CHECK(Inline(R"(\varepsilon \vartheta \varphi \varsigma)") == "ϵ ϑ ϕ ς");
    CHECK(Inline(R"(\Alpha \Beta \Delta \Pi)") == "Α Β Δ Π");
}

TEST_CASE("LaTeX math: 关系和运算白名单") {
    CHECK(Inline(R"(a \leq b \ge c \to d)") == "a ≤ b ≥ c → d");
    CHECK(Inline(R"(a \times b \neq c \cdot d)") == "a × b ≠ c · d");
    CHECK(Inline(R"(x \pm y \approx z \in S)") == "x ± y ≈ z ∈ S");
    CHECK(Inline(R"(A \subseteq B \land \forall x)") == "A ⊆ B ∧ ∀ x");
}

TEST_CASE("LaTeX math: 大符号白名单") {
    CHECK(Inline(R"(\int_0^1)") == "∫₀¹");
    CHECK(Inline(R"(\sum_i \prod_j)") == "∑ᵢ ∏ⱼ");
    CHECK(Inline(R"(\infty \partial \nabla)") == "∞ ∂ ∇");
}

TEST_CASE("LaTeX math: 上下标整段映射或整段退") {
    CHECK(Inline("H_2O") == "H₂O");
    CHECK(Inline("x^2") == "x²");
    CHECK(Inline("x^12") == "x¹²");
    CHECK(Inline("x^{n+1}") == "xⁿ⁺¹");
    CHECK(Inline("x^{q+1}") == "x^(q+1)");
    CHECK(Inline("a_{q+1}") == "a_(q+1)");
    CHECK(Inline("x^{abq}") == "x^(abq)");
}

TEST_CASE("LaTeX math: 分式和根式递归拆组") {
    CHECK(Inline(R"(\frac{a}{b})") == "a/b");
    CHECK(Inline(R"(\frac{a+b}{c+d})") == "(a+b)/(c+d)");
    CHECK(Inline(R"(\frac{\frac{a}{b}}{c})") == "(a/b)/c");
    CHECK(Inline(R"(\sqrt{x})") == "√x");
    CHECK(Inline(R"(\sqrt{x+1})") == "√(x+1)");
    CHECK(Inline(R"(\sqrt{\frac{a}{b}})") == "√(a/b)");
}

TEST_CASE("LaTeX math: 未知命令和坏花括号都失败") {
    CHECK_FALSE(RenderLatexInline(R"(\text{hello})").has_value());
    CHECK_FALSE(RenderLatexInline(R"(\frac{a}{b)").has_value());
    CHECK_FALSE(RenderLatexInline("x^").has_value());
    CHECK_FALSE(RenderLatexBlock(R"(\unknown{x})").has_value());
}

TEST_CASE("LaTeX math: 行内矩阵退成紧凑单行") {
    CHECK(Inline(R"(\begin{matrix}a & b \\ c & d\end{matrix})") == "[[a, b]; [c, d]]");
    CHECK(Inline(R"(\begin{pmatrix}1 & 2\end{pmatrix})") == "[[1, 2]]");
}

TEST_CASE("LaTeX math: 块矩阵按显示宽度对齐") {
    const auto lines = Block(R"(\begin{matrix}a & 中文 \\ 长字 & x\end{matrix})");
    REQUIRE(lines.size() == 2);
    CHECK(lines[0].starts_with("⎡"));
    CHECK(lines[1].starts_with("⎣"));
    CHECK(DisplayWidthUtf8(lines[0]) == DisplayWidthUtf8(lines[1]));
    CHECK(lines[0].find("中文") != std::string::npos);
}

TEST_CASE("LaTeX math: pmatrix 与 bmatrix 也走块矩阵") {
    const auto paren = Block(R"(\begin{pmatrix}1 & 22 \\ 333 & 4\end{pmatrix})");
    REQUIRE(paren.size() == 2);
    CHECK(paren[0].starts_with("⎛"));
    CHECK(paren[1].starts_with("⎝"));
    CHECK(DisplayWidthUtf8(paren[0]) == DisplayWidthUtf8(paren[1]));

    const auto square = Block(R"(\begin{bmatrix}甲 & b \\ c & 丁丁\end{bmatrix})");
    REQUIRE(square.size() == 2);
    CHECK(square[0].starts_with("⎡"));
    CHECK(square[1].starts_with("⎣"));
    CHECK(DisplayWidthUtf8(square[0]) == DisplayWidthUtf8(square[1]));
}

using lubancode::cli::BuiltinTheme;
using lubancode::cli::DetectMarkdownStructure;
using lubancode::cli::RenderMarkdown;

TEST_CASE("LaTeX math Markdown: 行内定界符在 plain 主题也翻译") {
    const auto lines = RenderMarkdown("水是 $H_2O$，幂是 $x^{n+1}$。", BuiltinTheme("plain"), 80);
    REQUIRE(lines.size() == 1);
    CHECK(lines[0] == "水是 H₂O，幂是 xⁿ⁺¹。");
    const auto bold = RenderMarkdown("**$x^2$**", BuiltinTheme("plain"), 80);
    REQUIRE(bold.size() == 1);
    CHECK(bold[0] == "x²");
    CHECK(DetectMarkdownStructure("只有 $\\alpha + \\beta$ 一句"));
}

TEST_CASE("LaTeX math Markdown: 块级定界符先拆成公式行") {
    const auto lines =
        RenderMarkdown("前文\n$$\n\\frac{a+b}{c+d}\n$$\n后文", BuiltinTheme("plain"), 80);
    REQUIRE(lines.size() == 3);
    CHECK(lines[0] == "前文");
    CHECK(lines[1] == "(a+b)/(c+d)");
    CHECK(lines[2] == "后文");
}

TEST_CASE("LaTeX math Markdown: 未知命令原样保留定界符") {
    const auto inline_lines = RenderMarkdown("留着 $\\unknown{x}$ 别动", BuiltinTheme("plain"), 80);
    REQUIRE(inline_lines.size() == 1);
    CHECK(inline_lines[0] == "留着 $\\unknown{x}$ 别动");

    const auto block_lines = RenderMarkdown("$$\n\\unknown{x}\n$$", BuiltinTheme("plain"), 80);
    REQUIRE(block_lines.size() == 3);
    CHECK(block_lines[0] == "$$");
    CHECK(block_lines[1] == "\\unknown{x}");
    CHECK(block_lines[2] == "$$");
}

TEST_CASE("LaTeX math Markdown: 围栏和行内代码都不认美元号") {
    const auto fenced = RenderMarkdown("```\n$x^2$\n```", BuiltinTheme("plain"), 80);
    REQUIRE(fenced.size() == 1);
    CHECK(fenced[0].find("$x^2$") != std::string::npos);
    CHECK(fenced[0].find("x²") == std::string::npos);

    const auto inline_code = RenderMarkdown("`$x^2$`", BuiltinTheme("plain"), 80);
    REQUIRE(inline_code.size() == 1);
    CHECK(inline_code[0] == "$x^2$");
}

TEST_CASE("LaTeX math Markdown: 转义美元号不触发公式") {
    const auto lines = RenderMarkdown(R"(价签是 \$x^2$，不是公式。)", BuiltinTheme("plain"), 80);
    REQUIRE(lines.size() == 1);
    CHECK(lines[0] == "$x^2$，不是公式。");
    CHECK(lines[0].find("x²") == std::string::npos);
}

TEST_CASE("LaTeX math Markdown: 块矩阵和普通行都守终端宽度") {
    const int width = 18;
    const auto lines =
        RenderMarkdown("$$\n\\begin{bmatrix}中文 & a \\\\ b & 丁丁\\end{bmatrix}\n$$",
                       BuiltinTheme("plain"), width);
    REQUIRE(lines.size() == 2);
    for (const std::string &line : lines) {
        CHECK(DisplayWidthUtf8(line) <= static_cast<std::size_t>(width - 1));
    }
}
