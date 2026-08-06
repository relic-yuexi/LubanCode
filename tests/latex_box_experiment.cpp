#include <iostream>
#include <string>
#include <utility>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#endif

#include "cli/latex_math.hpp"

int main() {
#ifdef _WIN32
    SetConsoleOutputCP(CP_UTF8);
#endif
    using Formula = std::pair<const char *, const char *>;
    const std::vector<Formula> formulas = {
        {"Euler", R"(e^{i\theta}=\cos\theta+i\sin\theta)"},
        {"products", R"(\left(x-1\right)\left(x+3\right))"},
        {"square root", R"(\sqrt{a^2+b^2})"},
        {"power of a fraction", R"(\left(\frac{a}{b}\right)^n=\frac{a^n}{b^n})"},
        {"fraction addition", R"(\frac{a}{b}\pm\frac{c}{d}=\frac{ad\pm bc}{bd})"},
        {"hyperbola", R"(\frac{x^2}{a^2}-\frac{y^2}{b^2}=1)"},
        {"rationalized root", R"(\frac{1}{\sqrt{a}}=\frac{\sqrt{a}}{a},\quad a>0)"},
        {"indexed root", R"(\sqrt[n]{a^n}=\left(\sqrt[n]{a}\right)^n)"},
        {"quadratic", R"(x={-b\pm\sqrt{b^2-4ac}\over 2a})"},
        {"point-slope", R"(y-y_1=k\left(x-x_1\right))"},
        {"system", R"(\left\{\begin{matrix}x=a+r\cos\theta\\y=b+r\sin\theta\end{matrix}\right.)"},
        {"geometry", R"(l\perp\beta,l\subset\alpha\Rightarrow\alpha\perp\beta)"},
        {"probability", R"(\begin{array}{c}A_i\cap A_j=\emptyset\quad(i\ne j)\\P\left(\bigcup\limits_{i=1}^{+\infty}A_i\right)=\sum\limits_{i=1}^{+\infty}P\left(A_i\right)\end{array})"},
        {"means", R"(\begin{array}{c}H_n=\frac{n}{\sum\limits_{i=1}^n\frac1{x_i}}=\frac{n}{\frac1{x_1}+\frac1{x_2}+\cdots+\frac1{x_n}}\\G_n=\sqrt[n]{\prod\limits_{i=1}^n x_i}=\sqrt[n]{x_1x_2\cdots x_n}\\A_n=\frac1n\sum\limits_{i=1}^n x_i=\frac{x_1+x_2+\cdots+x_n}{n}\\Q_n=\sqrt{\frac1n\sum\limits_{i=1}^n x_i^2}=\sqrt{\frac{x_1^2+x_2^2+\cdots+x_n^2}{n}}\\H_n\leq G_n\leq A_n\leq Q_n\quad(x_i>0)\end{array})"},
    };

    for (const auto &[title, latex] : formulas) {
        std::cout << title << '\n';
        const auto rendered = lubancode::cli::RenderLatexBlock(latex);
        if (!rendered) {
            std::cout << "  [parse failed]\n\n";
            continue;
        }
        for (const std::string &line : *rendered)
            std::cout << "  " << line << '\n';
        std::cout << '\n';
    }
}
