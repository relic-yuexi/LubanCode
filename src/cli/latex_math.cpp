#include "cli/latex_math.hpp"

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <string_view>
#include <utility>

#include "cli/line_editor.hpp" // DisplayWidthUtf8

namespace lubancode::cli {

namespace {

std::string Trim(const std::string &text) {
    std::size_t first = 0;
    while (first < text.size() && (text[first] == ' ' || text[first] == '\t' ||
                                   text[first] == '\n' || text[first] == '\r')) {
        ++first;
    }
    std::size_t last = text.size();
    while (last > first && (text[last - 1] == ' ' || text[last - 1] == '\t' ||
                            text[last - 1] == '\n' || text[last - 1] == '\r')) {
        --last;
    }
    return text.substr(first, last - first);
}

bool IsAsciiLetter(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }

const char *CommandSymbol(std::string_view name) {
    // 大写 Alpha/Beta 也照收，虽不是标准 LaTeX 命令，终端里仍能明白。
    static const std::pair<std::string_view, const char *> kSymbols[] = {
        {"alpha", "α"},
        {"beta", "β"},
        {"gamma", "γ"},
        {"delta", "δ"},
        {"epsilon", "ε"},
        {"varepsilon", "ϵ"},
        {"zeta", "ζ"},
        {"eta", "η"},
        {"theta", "θ"},
        {"vartheta", "ϑ"},
        {"iota", "ι"},
        {"kappa", "κ"},
        {"lambda", "λ"},
        {"mu", "μ"},
        {"nu", "ν"},
        {"xi", "ξ"},
        {"omicron", "ο"},
        {"pi", "π"},
        {"rho", "ρ"},
        {"varrho", "ϱ"},
        {"sigma", "σ"},
        {"varsigma", "ς"},
        {"tau", "τ"},
        {"upsilon", "υ"},
        {"phi", "φ"},
        {"varphi", "ϕ"},
        {"chi", "χ"},
        {"psi", "ψ"},
        {"omega", "ω"},
        {"Alpha", "Α"},
        {"Beta", "Β"},
        {"Gamma", "Γ"},
        {"Delta", "Δ"},
        {"Epsilon", "Ε"},
        {"Zeta", "Ζ"},
        {"Eta", "Η"},
        {"Theta", "Θ"},
        {"Iota", "Ι"},
        {"Kappa", "Κ"},
        {"Lambda", "Λ"},
        {"Mu", "Μ"},
        {"Nu", "Ν"},
        {"Xi", "Ξ"},
        {"Omicron", "Ο"},
        {"Pi", "Π"},
        {"Rho", "Ρ"},
        {"Sigma", "Σ"},
        {"Tau", "Τ"},
        {"Upsilon", "Υ"},
        {"Phi", "Φ"},
        {"Chi", "Χ"},
        {"Psi", "Ψ"},
        {"Omega", "Ω"},
        {"le", "≤"},
        {"leq", "≤"},
        {"ge", "≥"},
        {"geq", "≥"},
        {"to", "→"},
        {"rightarrow", "→"},
        {"leftarrow", "←"},
        {"leftrightarrow", "↔"},
        {"times", "×"},
        {"div", "÷"},
        {"neq", "≠"},
        {"ne", "≠"},
        {"cdot", "·"},
        {"pm", "±"},
        {"mp", "∓"},
        {"approx", "≈"},
        {"sim", "∼"},
        {"equiv", "≡"},
        {"propto", "∝"},
        {"in", "∈"},
        {"notin", "∉"},
        {"subset", "⊂"},
        {"subseteq", "⊆"},
        {"supset", "⊃"},
        {"supseteq", "⊇"},
        {"forall", "∀"},
        {"exists", "∃"},
        {"land", "∧"},
        {"lor", "∨"},
        {"int", "∫"},
        {"oint", "∮"},
        {"sum", "∑"},
        {"prod", "∏"},
        {"infty", "∞"},
        {"partial", "∂"},
        {"nabla", "∇"},
    };
    for (const auto &[key, value] : kSymbols) {
        if (key == name)
            return value;
    }
    return nullptr;
}

std::size_t Utf8CodePointCount(const std::string &text) {
    std::size_t count = 0;
    for (unsigned char c : text) {
        if ((c & 0xc0U) != 0x80U)
            ++count;
    }
    return count;
}

std::string ParenthesizeIfNeeded(const std::string &text) {
    return Utf8CodePointCount(text) == 1 ? text : "(" + text + ")";
}

const char *Superscript(char c) {
    switch (c) {
    case '0':
        return "⁰";
    case '1':
        return "¹";
    case '2':
        return "²";
    case '3':
        return "³";
    case '4':
        return "⁴";
    case '5':
        return "⁵";
    case '6':
        return "⁶";
    case '7':
        return "⁷";
    case '8':
        return "⁸";
    case '9':
        return "⁹";
    case '+':
        return "⁺";
    case '-':
        return "⁻";
    case '=':
        return "⁼";
    case '(':
        return "⁽";
    case ')':
        return "⁾";
    case 'a':
        return "ᵃ";
    case 'b':
        return "ᵇ";
    case 'c':
        return "ᶜ";
    case 'd':
        return "ᵈ";
    case 'e':
        return "ᵉ";
    case 'f':
        return "ᶠ";
    case 'g':
        return "ᵍ";
    case 'h':
        return "ʰ";
    case 'i':
        return "ⁱ";
    case 'j':
        return "ʲ";
    case 'k':
        return "ᵏ";
    case 'l':
        return "ˡ";
    case 'm':
        return "ᵐ";
    case 'n':
        return "ⁿ";
    case 'o':
        return "ᵒ";
    case 'p':
        return "ᵖ";
    case 'r':
        return "ʳ";
    case 's':
        return "ˢ";
    case 't':
        return "ᵗ";
    case 'u':
        return "ᵘ";
    case 'v':
        return "ᵛ";
    case 'w':
        return "ʷ";
    case 'x':
        return "ˣ";
    case 'y':
        return "ʸ";
    case 'z':
        return "ᶻ";
    case 'A':
        return "ᴬ";
    case 'B':
        return "ᴮ";
    case 'D':
        return "ᴰ";
    case 'E':
        return "ᴱ";
    case 'G':
        return "ᴳ";
    case 'H':
        return "ᴴ";
    case 'I':
        return "ᴵ";
    case 'J':
        return "ᴶ";
    case 'K':
        return "ᴷ";
    case 'L':
        return "ᴸ";
    case 'M':
        return "ᴹ";
    case 'N':
        return "ᴺ";
    case 'O':
        return "ᴼ";
    case 'P':
        return "ᴾ";
    case 'R':
        return "ᴿ";
    case 'T':
        return "ᵀ";
    case 'U':
        return "ᵁ";
    case 'V':
        return "ⱽ";
    case 'W':
        return "ᵂ";
    default:
        return nullptr;
    }
}

const char *Subscript(char c) {
    switch (c) {
    case '0':
        return "₀";
    case '1':
        return "₁";
    case '2':
        return "₂";
    case '3':
        return "₃";
    case '4':
        return "₄";
    case '5':
        return "₅";
    case '6':
        return "₆";
    case '7':
        return "₇";
    case '8':
        return "₈";
    case '9':
        return "₉";
    case '+':
        return "₊";
    case '-':
        return "₋";
    case '=':
        return "₌";
    case '(':
        return "₍";
    case ')':
        return "₎";
    case 'a':
        return "ₐ";
    case 'e':
        return "ₑ";
    case 'h':
        return "ₕ";
    case 'i':
        return "ᵢ";
    case 'j':
        return "ⱼ";
    case 'k':
        return "ₖ";
    case 'l':
        return "ₗ";
    case 'm':
        return "ₘ";
    case 'n':
        return "ₙ";
    case 'o':
        return "ₒ";
    case 'p':
        return "ₚ";
    case 'r':
        return "ᵣ";
    case 's':
        return "ₛ";
    case 't':
        return "ₜ";
    case 'u':
        return "ᵤ";
    case 'v':
        return "ᵥ";
    case 'x':
        return "ₓ";
    default:
        return nullptr;
    }
}

std::optional<std::string> ConvertScript(const std::string &text, bool superscript) {
    std::string out;
    for (unsigned char c : text) {
        const char *mapped =
            superscript ? Superscript(static_cast<char>(c)) : Subscript(static_cast<char>(c));
        if (mapped == nullptr)
            return std::nullopt;
        out += mapped;
    }
    return out;
}

class ExpressionParser {
  public:
    explicit ExpressionParser(std::string_view source) : source_(source) {}

    std::optional<std::string> Parse() {
        auto text = ParseSequence(false);
        return text && pos_ == source_.size() ? text : std::nullopt;
    }

  private:
    std::optional<std::string> ParseSequence(bool stops_at_brace) {
        std::string out;
        while (pos_ < source_.size()) {
            const char c = source_[pos_];
            if (c == '}') {
                if (stops_at_brace) {
                    ++pos_;
                    return out;
                }
                return std::nullopt;
            }
            if (c == '{') {
                ++pos_;
                auto group = ParseSequence(true);
                if (!group)
                    return std::nullopt;
                out += *group;
                continue;
            }
            if (c == '\\') {
                auto command = ParseCommand();
                if (!command)
                    return std::nullopt;
                out += *command;
                continue;
            }
            if (c == '^' || c == '_') {
                const bool superscript = c == '^';
                ++pos_;
                auto body = ParseScriptBody();
                if (!body)
                    return std::nullopt;
                if (const auto converted = ConvertScript(*body, superscript)) {
                    out += *converted;
                } else {
                    out += superscript ? "^(" : "_(";
                    out += *body;
                    out += ')';
                }
                continue;
            }
            if (c == '&')
                return std::nullopt; // 只准矩阵切列。
            if (c == '\n' || c == '\r') {
                if (!out.empty() && out.back() != ' ')
                    out += ' ';
                ++pos_;
                continue;
            }
            out += c;
            ++pos_;
        }
        return stops_at_brace ? std::nullopt : std::optional<std::string>(out);
    }

    std::optional<std::string> ParseGroup() {
        if (pos_ >= source_.size() || source_[pos_] != '{')
            return std::nullopt;
        ++pos_;
        return ParseSequence(true);
    }

    std::optional<std::string> ParseScriptBody() {
        if (pos_ >= source_.size())
            return std::nullopt;
        if (source_[pos_] == '{')
            return ParseGroup();
        const unsigned char first = static_cast<unsigned char>(source_[pos_]);
        if (std::isdigit(first)) {
            const std::size_t begin = pos_;
            while (pos_ < source_.size() && std::isdigit(static_cast<unsigned char>(source_[pos_])))
                ++pos_;
            return std::string(source_.substr(begin, pos_ - begin));
        }
        if (source_[pos_] == '\\')
            return std::nullopt;
        const std::size_t len = Utf8CharLength(first);
        if (len == 0 || pos_ + len > source_.size())
            return std::nullopt;
        const std::string one(source_.substr(pos_, len));
        pos_ += len;
        return one;
    }

    std::optional<std::string> ParseCommand() {
        ++pos_;
        if (pos_ >= source_.size())
            return std::nullopt;
        if (!IsAsciiLetter(source_[pos_])) {
            const char escaped = source_[pos_++];
            switch (escaped) {
            case '{':
                return std::string("{");
            case '}':
                return std::string("}");
            case '_':
                return std::string("_");
            case '^':
                return std::string("^");
            case '%':
                return std::string("%");
            case '$':
                return std::string("$");
            case ' ':
                return std::string(" ");
            default:
                return std::nullopt;
            }
        }
        const std::size_t begin = pos_;
        while (pos_ < source_.size() && IsAsciiLetter(source_[pos_]))
            ++pos_;
        const std::string_view name = source_.substr(begin, pos_ - begin);
        if (name == "frac") {
            auto numerator = ParseGroup();
            auto denominator = ParseGroup();
            if (!numerator || !denominator)
                return std::nullopt;
            return ParenthesizeIfNeeded(*numerator) + "/" + ParenthesizeIfNeeded(*denominator);
        }
        if (name == "sqrt") {
            auto radicand = ParseGroup();
            return radicand ? std::optional<std::string>("√" + ParenthesizeIfNeeded(*radicand))
                            : std::nullopt;
        }
        if (name == "left" || name == "right")
            return std::string();
        if (const char *symbol = CommandSymbol(name))
            return std::string(symbol);
        return std::nullopt;
    }

    static std::size_t Utf8CharLength(unsigned char first) {
        if ((first & 0x80U) == 0)
            return 1;
        if ((first & 0xe0U) == 0xc0U)
            return 2;
        if ((first & 0xf0U) == 0xe0U)
            return 3;
        if ((first & 0xf8U) == 0xf0U)
            return 4;
        return 0;
    }

    std::string_view source_;
    std::size_t pos_ = 0;
};

struct Matrix {
    std::string environment;
    std::vector<std::vector<std::string>> rows;
};

std::optional<std::string> ParseRawGroup(std::string_view text, std::size_t &pos) {
    if (pos >= text.size() || text[pos] != '{')
        return std::nullopt;
    ++pos;
    const std::size_t begin = pos;
    int depth = 1;
    while (pos < text.size()) {
        if (text[pos] == '{')
            ++depth;
        if (text[pos] == '}' && --depth == 0) {
            const std::string value(text.substr(begin, pos - begin));
            ++pos;
            return value;
        }
        ++pos;
    }
    return std::nullopt;
}

std::optional<Matrix> ParseMatrix(const std::string &source) {
    const std::string trimmed = Trim(source);
    constexpr std::string_view kBegin = "\\begin";
    if (!trimmed.starts_with(kBegin))
        return std::nullopt;
    std::size_t pos = kBegin.size();
    auto environment = ParseRawGroup(trimmed, pos);
    if (!environment ||
        (*environment != "matrix" && *environment != "pmatrix" && *environment != "bmatrix")) {
        return std::nullopt;
    }
    const std::string ending = "\\end{" + *environment + "}";
    if (trimmed.size() < pos + ending.size() ||
        trimmed.compare(trimmed.size() - ending.size(), ending.size(), ending) != 0)
        return std::nullopt;
    const std::string body = trimmed.substr(pos, trimmed.size() - pos - ending.size());

    std::vector<std::vector<std::string>> raw_rows(1, std::vector<std::string>(1));
    int brace_depth = 0;
    for (std::size_t i = 0; i < body.size();) {
        if (body[i] == '{')
            ++brace_depth;
        if (body[i] == '}') {
            if (brace_depth == 0)
                return std::nullopt;
            --brace_depth;
        }
        if (brace_depth == 0 && body[i] == '&') {
            raw_rows.back().push_back(std::string());
            ++i;
            continue;
        }
        if (brace_depth == 0 && body[i] == '\\' &&
            (i + 1 == body.size() || body[i + 1] == '\\' ||
             std::isspace(static_cast<unsigned char>(body[i + 1])))) {
            raw_rows.emplace_back(1);
            i += i + 1 < body.size() && body[i + 1] == '\\' ? 2 : 1;
            continue;
        }
        raw_rows.back().back() += body[i++];
    }
    if (brace_depth != 0 || raw_rows.back().size() == 1 && Trim(raw_rows.back().front()).empty())
        return std::nullopt;

    const std::size_t columns = raw_rows.front().size();
    Matrix matrix{*environment, {}};
    for (const auto &raw_row : raw_rows) {
        if (raw_row.size() != columns)
            return std::nullopt;
        std::vector<std::string> row;
        for (const std::string &raw_cell : raw_row) {
            ExpressionParser parser(Trim(raw_cell));
            auto rendered = parser.Parse();
            if (!rendered)
                return std::nullopt;
            row.push_back(std::move(*rendered));
        }
        matrix.rows.push_back(std::move(row));
    }
    return matrix;
}

std::string RenderMatrixInline(const Matrix &matrix) {
    std::string out = "[[";
    for (std::size_t row = 0; row < matrix.rows.size(); ++row) {
        if (row > 0)
            out += "; [";
        for (std::size_t col = 0; col < matrix.rows[row].size(); ++col) {
            if (col > 0)
                out += ", ";
            out += matrix.rows[row][col];
        }
        out += ']';
    }
    return out;
}

std::vector<std::string> RenderMatrixBlock(const Matrix &matrix) {
    std::vector<int> widths(matrix.rows.front().size(), 0);
    for (const auto &row : matrix.rows) {
        for (std::size_t col = 0; col < row.size(); ++col) {
            widths[col] = (std::max)(widths[col], static_cast<int>(DisplayWidthUtf8(row[col])));
        }
    }
    std::vector<std::string> lines;
    for (std::size_t row = 0; row < matrix.rows.size(); ++row) {
        const bool first = row == 0;
        const bool last = row + 1 == matrix.rows.size();
        const std::string left = matrix.environment == "pmatrix"
                                     ? (first ? "⎛" : (last ? "⎝" : "⎜"))
                                     : (first ? "⎡" : (last ? "⎣" : "⎢"));
        const std::string right = matrix.environment == "pmatrix"
                                      ? (first ? "⎞" : (last ? "⎠" : "⎟"))
                                      : (first ? "⎤" : (last ? "⎦" : "⎥"));
        std::string line = left + " ";
        for (std::size_t col = 0; col < matrix.rows[row].size(); ++col) {
            if (col > 0)
                line += "  ";
            line += matrix.rows[row][col];
            line += std::string(
                static_cast<std::size_t>(widths[col] -
                                         static_cast<int>(DisplayWidthUtf8(matrix.rows[row][col]))),
                ' ');
        }
        lines.push_back(line + " " + right);
    }
    return lines;
}

} // namespace

std::optional<std::string> RenderLatexInline(const std::string &latex) {
    if (auto matrix = ParseMatrix(latex))
        return RenderMatrixInline(*matrix);
    ExpressionParser parser(latex);
    return parser.Parse();
}

std::optional<std::vector<std::string>> RenderLatexBlock(const std::string &latex) {
    if (auto matrix = ParseMatrix(latex))
        return RenderMatrixBlock(*matrix);
    const std::string trimmed = Trim(latex);
    ExpressionParser parser(trimmed);
    auto rendered = parser.Parse();
    if (!rendered)
        return std::nullopt;
    return std::vector<std::string>{std::move(*rendered)};
}

} // namespace lubancode::cli
