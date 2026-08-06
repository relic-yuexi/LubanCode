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

bool Contains(const std::vector<std::string> &lines, const std::string &needle) {
    for (const std::string &line : lines) {
        if (line.find(needle) != std::string::npos)
            return true;
    }
    return false;
}

} // namespace

TEST_CASE("LaTeX math: 希腊字母白名单") {
    CHECK(Inline(R"(\alpha \beta \Gamma \Omega)") == "𝛼 𝛽 𝛤 𝛺");
    CHECK(Inline(R"(\varepsilon \vartheta \varphi \varsigma)") == "𝜖 𝜗 𝜙 𝜍");
    CHECK(Inline(R"(\Alpha \Beta \Delta \Pi)") == "𝛢 𝛣 𝛥 𝛱");
}

TEST_CASE("LaTeX math: 关系和运算白名单") {
    CHECK(Inline(R"(a \leq b \ge c \to d)") == "𝑎 ≤ 𝑏 ≥ 𝑐 → 𝑑");
    CHECK(Inline(R"(a \times b \neq c \cdot d)") == "𝑎 × 𝑏 ≠ 𝑐 · 𝑑");
    CHECK(Inline(R"(x \pm y \approx z \in S)") == "𝑥 ± 𝑦 ≈ 𝑧 ∈ 𝑆");
    CHECK(Inline(R"(A \subseteq B \land \forall x)") == "𝐴 ⊆ 𝐵 ∧ ∀ 𝑥");
}

TEST_CASE("LaTeX math: 大符号白名单") {
    CHECK(Inline(R"(\int_0^1)") == "∫₀¹");
    CHECK(Inline(R"(\sum_i \prod_j)") == "∑ᵢ ∏ⱼ");
    CHECK(Inline(R"(\infty \partial \nabla)") == "∞ 𝜕 ∇");
}

TEST_CASE("LaTeX math: 上下标整段映射或整段退") {
    CHECK(Inline("H_2O") == "𝐻₂𝑂");
    CHECK(Inline("x^2") == "𝑥²");
    CHECK(Inline("x^12") == "𝑥¹²");
    CHECK(Inline("x^{n+1}") == "𝑥ⁿ⁺¹");
    CHECK(Inline("x^{q+1}") == "𝑥^(𝑞+1)");
    CHECK(Inline("a_{q+1}") == "𝑎_(𝑞+1)");
    CHECK(Inline("x^{abq}") == "𝑥^(𝑎𝑏𝑞)");
    CHECK(Inline(R"(e^{i\theta})") == "𝑒ⁱᶿ");
    CHECK(Inline(R"(g^{\beta\gamma\delta\theta\phi\chi})") == "𝑔ᵝᵞᵟᶿᵠᵡ");
    CHECK(Inline("ahx") == "𝑎ℎ𝑥");
}

TEST_CASE("LaTeX math: 分式和根式递归拆组") {
    CHECK(Inline(R"(\frac{a}{b})") == "𝑎/𝑏");
    CHECK(Inline(R"(\frac{a+b}{c+d})") == "(𝑎+𝑏)/(𝑐+𝑑)");
    CHECK(Inline(R"(\frac{\frac{a}{b}}{c})") == "(𝑎/𝑏)/𝑐");
    CHECK(Inline(R"(\sqrt{x})") == "√𝑥");
    CHECK(Inline(R"(\sqrt{x+1})") == "√(𝑥+1)");
    CHECK(Inline(R"(\sqrt{\frac{a}{b}})") == "√(𝑎/𝑏)");
    CHECK(Inline(R"(\frac12)") == "1/2");
}

TEST_CASE("LaTeX math: GRPO 常用函数、字体和间距命令") {
    CHECK(Inline(R"(\operatorname{mean}(r_i) + \operatorname{std}(r_i))") ==
          "mean(𝑟ᵢ) + std(𝑟ᵢ)");
    CHECK(Inline(R"(\mathbb{E}_{q\sim P(Q)} \min(\rho_i,\,1+\varepsilon))") ==
          "𝔼_(𝑞∼ 𝑃(𝑄)) min(𝜌ᵢ, 1+𝜖)");
    CHECK(Inline(R"(D_{KL}\!\left[\pi_\theta\,\|\,\pi_{ref}\right])") ==
          "𝐷_(𝐾𝐿)[𝜋_(𝜃) ‖ 𝜋_(𝑟𝑒𝑓)]");
    CHECK(Inline(R"(\dfrac{\pi_{ref}}{\pi_\theta}-\log\dfrac{\pi_{ref}}{\pi_\theta}-1)") ==
          "(𝜋_(𝑟𝑒𝑓))/(𝜋_(𝜃))-log(𝜋_(𝑟𝑒𝑓))/(𝜋_(𝜃))-1");
    CHECK(Inline(R"(\{o_1,\dots,o_G\})") == "{𝑜₁,…,𝑜_(𝐺)}");
}

TEST_CASE("LaTeX math: PPO 常用帽号、文本和宽空格命令") {
    CHECK(Inline(R"(\hat{A}_t + \hat{\mathbb{E}}_t)") == "𝐴̂ₜ + 𝔼̂ₜ");
    CHECK(Inline(R"(\text{mean}(r_i) + \text{std}(r_i))") ==
          "mean(𝑟ᵢ) + std(𝑟ᵢ)");
    CHECK(Inline(R"(a\quad b\qquad c)") == "𝑎  𝑏    𝑐");
}

TEST_CASE("LaTeX math: 未知命令和坏花括号都失败") {
    CHECK_FALSE(RenderLatexInline(R"(\unknown{hello})").has_value());
    CHECK_FALSE(RenderLatexInline(R"(\frac{a}{b)").has_value());
    CHECK_FALSE(RenderLatexInline("x^").has_value());
    CHECK_FALSE(RenderLatexBlock(R"(\unknown{x})").has_value());
}

TEST_CASE("LaTeX math: 行内矩阵退成紧凑单行") {
    CHECK(Inline(R"(\begin{matrix}a & b \\ c & d\end{matrix})") ==
          "[[𝑎, 𝑏]; [𝑐, 𝑑]]");
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

TEST_CASE("LaTeX math: 块公式铺开分式、根式和伸缩括号") {
    const auto fraction = Block(R"(\frac{a+b}{c+d})");
    REQUIRE(fraction.size() == 3);
    CHECK(fraction[0].find("𝑎+𝑏") != std::string::npos);
    CHECK(fraction[1] == "─────");
    CHECK(fraction[2].find("𝑐+𝑑") != std::string::npos);

    const auto radical = Block(R"(\sqrt{a^2+b^2})");
    REQUIRE(radical.size() == 2);
    CHECK(radical[0].find("─────") != std::string::npos);
    CHECK(radical[1].find("√ 𝑎²+𝑏²") != std::string::npos);

    const auto delimited = Block(R"(\left(\frac{a}{b}\right))");
    REQUIRE(delimited.size() == 3);
    CHECK(delimited[0].starts_with("⎛"));
    CHECK(delimited[1].starts_with("⎜"));
    CHECK(delimited[2].starts_with("⎝"));
}

TEST_CASE("LaTeX math: 方程组、数组和上下限走多行盒子") {
    const auto system = Block(
        R"(\left\{\begin{matrix}x=a+r\cos\theta\\y=b+r\sin\theta\end{matrix}\right.)");
    REQUIRE(system.size() == 2);
    CHECK(system[0].starts_with("⎧"));
    CHECK(system[1].starts_with("⎩"));
    CHECK(Contains(system, "cos"));
    CHECK(Contains(system, "sin"));

    const auto limits = Block(
        R"(\sum\limits_{i=1}^{+\infty}P\left(A_i\right))");
    REQUIRE(limits.size() == 3);
    CHECK(Contains(limits, "+∞"));
    CHECK(Contains(limits, "∑"));
    CHECK(Contains(limits, "𝑖=1"));

    const auto array = Block(
        R"(\begin{array}{c}A_i\cap A_j=\emptyset\\P(A_i)\geq0\end{array})");
    REQUIRE(array.size() == 2);
    CHECK(Contains(array, "∩"));
    CHECK(Contains(array, "𝑃(𝐴ᵢ)≥0"));
}

TEST_CASE("LaTeX math: 二次公式和均值链常用语法都能收") {
    const auto quadratic = Block(R"(x={-b\pm\sqrt{b^2-4ac}\over 2a})");
    REQUIRE(quadratic.size() == 4);
    CHECK(Contains(quadratic, "────"));
    CHECK(Contains(quadratic, "2𝑎"));

    const auto means = Block(
        R"(\begin{array}{c}H_n=\frac{n}{\sum\limits_{i=1}^n\frac1{x_i}}\\G_n=\sqrt[n]{\prod\limits_{i=1}^n x_i}\\H_n\leq G_n\end{array})");
    CHECK(means.size() > 5);
    CHECK(Contains(means, "𝐻ₙ"));
    CHECK(Contains(means, "𝐺ₙ"));
    CHECK(Contains(means, "∏"));
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
    CHECK(lines[0] == "水是 𝐻₂𝑂，幂是 𝑥ⁿ⁺¹。");
    const auto bold = RenderMarkdown("**$x^2$**", BuiltinTheme("plain"), 80);
    REQUIRE(bold.size() == 1);
    CHECK(bold[0] == "𝑥²");
    CHECK(DetectMarkdownStructure("只有 $\\alpha + \\beta$ 一句"));
}

TEST_CASE("LaTeX math Markdown: 块级定界符先拆成公式行") {
    const auto lines =
        RenderMarkdown("前文\n$$\n\\frac{a+b}{c+d}\n$$\n后文", BuiltinTheme("plain"), 80);
    REQUIRE(lines.size() == 5);
    CHECK(lines[0] == "前文");
    CHECK(lines[1].find("𝑎+𝑏") != std::string::npos);
    CHECK(lines[2] == "─────");
    CHECK(lines[3].find("𝑐+𝑑") != std::string::npos);
    CHECK(lines[4] == "后文");
}

TEST_CASE("LaTeX math Markdown: GRPO 公式不因常用排版命令整块退回") {
    const auto advantage = RenderMarkdown(
        "$$\nA_i = \\frac{r_i - \\operatorname{mean}(\\{r_1,\\dots,r_G\\})}"
        "{\\operatorname{std}(\\{r_1,\\dots,r_G\\})}\n$$",
        BuiltinTheme("plain"), 200);
    CHECK(advantage.size() >= 3);
    CHECK_FALSE(Contains(advantage, "$$"));
    CHECK(Contains(advantage, "mean"));
    CHECK(Contains(advantage, "std"));

    const auto objective = RenderMarkdown(
        "$$\nJ_{GRPO}(\\theta)=\\mathbb{E}_{q\\sim P(Q),\\,\\{o_i\\}\\sim "
        "\\pi_{\\theta_{old}}(O|q)}[\\min(\\rho_i A_i,\\,"
        "\\operatorname{clip}(\\rho_i,\\,1-\\varepsilon,\\,1+\\varepsilon) A_i)"
        "-\\beta D_{KL}\\!\\left[\\pi_\\theta\\,\\|\\,\\pi_{ref}\\right]]\n$$",
        BuiltinTheme("plain"), 300);
    CHECK(objective.size() >= 2);
    CHECK_FALSE(Contains(objective, "$$"));
    CHECK(Contains(objective, "clip"));
    CHECK(Contains(objective, "𝔼"));

    const auto kl = RenderMarkdown(
        "$$\nD_{KL}\\!\\left[\\pi_\\theta\\,\\|\\,\\pi_{ref}\\right]="
        "\\frac{\\pi_{ref}(o_i|q)}{\\pi_\\theta(o_i|q)}"
        "-\\log\\frac{\\pi_{ref}(o_i|q)}{\\pi_\\theta(o_i|q)}-1\n$$",
        BuiltinTheme("plain"), 300);
    CHECK(kl.size() >= 3);
    CHECK_FALSE(Contains(kl, "$$"));
    CHECK(Contains(kl, "log"));
}

TEST_CASE("LaTeX math Markdown: PPO 目标和 GAE 公式不整块退回") {
    const auto clip = RenderMarkdown(
        "$$L^{CLIP}(\\theta)=\\hat{\\mathbb{E}}_t\\left[\\min\\left("
        "r_t(\\theta)\\,\\hat{A}_t,\\;\\operatorname{clip}(r_t(\\theta),\\,"
        "1-\\varepsilon,\\,1+\\varepsilon)\\hat{A}_t\\right)\\right]$$",
        BuiltinTheme("plain"), 300);
    CHECK(clip.size() >= 2);
    CHECK_FALSE(Contains(clip, "$$"));
    CHECK(Contains(clip, "clip"));
    CHECK(Contains(clip, "𝔼̂"));

    const auto combined = RenderMarkdown(
        "$$L^{CLIP+VF+S}_t(\\theta)=\\hat{\\mathbb{E}}_t\\left["
        "L^{CLIP}_t(\\theta)-c_1\\,L^{VF}_t(\\theta)+c_2\\,"
        "S[\\pi_\\theta](s_t)\\right]$$",
        BuiltinTheme("plain"), 300);
    CHECK(combined.size() >= 2);
    CHECK_FALSE(Contains(combined, "$$"));

    const auto gae = RenderMarkdown(
        "$$\\hat{A}_t=\\sum_{l=0}^{\\infty}(\\gamma\\lambda)^l\\,"
        "\\delta_{t+l},\\qquad\\delta_t=r_t+\\gamma V(s_{t+1})-V(s_t)$$",
        BuiltinTheme("plain"), 300);
    CHECK(gae.size() >= 3);
    CHECK_FALSE(Contains(gae, "$$"));
    CHECK(Contains(gae, "∞"));
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
    // \$ 只转义开头那一个美元号(变普通 "$"字符),前面的"价签是 "原样留着;
    // 后头单落的 "$" 找不到配对的闭合定界符,也原样留着,不触发公式转换。
    CHECK(lines[0] == "价签是 $x^2$，不是公式。");
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
