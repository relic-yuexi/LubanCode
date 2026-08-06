#include "cli/latex_math.hpp"

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string_view>
#include <utility>

#include "cli/line_editor.hpp" // DisplayWidthUtf8

namespace lubancode::cli {

namespace {

std::string Trim(std::string_view text) {
    std::size_t first = 0;
    while (first < text.size() && std::isspace(static_cast<unsigned char>(text[first])))
        ++first;
    std::size_t last = text.size();
    while (last > first && std::isspace(static_cast<unsigned char>(text[last - 1])))
        --last;
    return std::string(text.substr(first, last - first));
}

bool IsAsciiLetter(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }

std::size_t Utf8CharLength(unsigned char first) {
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

std::uint32_t NextCodePoint(std::string_view text, std::size_t &pos) {
    const auto first = static_cast<unsigned char>(text[pos++]);
    if ((first & 0x80U) == 0)
        return first;

    int tails = 0;
    std::uint32_t value = 0;
    if ((first & 0xe0U) == 0xc0U) {
        tails = 1;
        value = first & 0x1fU;
    } else if ((first & 0xf0U) == 0xe0U) {
        tails = 2;
        value = first & 0x0fU;
    } else if ((first & 0xf8U) == 0xf0U) {
        tails = 3;
        value = first & 0x07U;
    } else {
        return first;
    }
    while (tails-- > 0 && pos < text.size())
        value = (value << 6U) | (static_cast<unsigned char>(text[pos++]) & 0x3fU);
    return value;
}

std::string Utf8(std::uint32_t cp) {
    std::string out;
    if (cp <= 0x7f) {
        out.push_back(static_cast<char>(cp));
    } else if (cp <= 0x7ff) {
        out.push_back(static_cast<char>(0xc0U | (cp >> 6U)));
        out.push_back(static_cast<char>(0x80U | (cp & 0x3fU)));
    } else if (cp <= 0xffff) {
        out.push_back(static_cast<char>(0xe0U | (cp >> 12U)));
        out.push_back(static_cast<char>(0x80U | ((cp >> 6U) & 0x3fU)));
        out.push_back(static_cast<char>(0x80U | (cp & 0x3fU)));
    } else {
        out.push_back(static_cast<char>(0xf0U | (cp >> 18U)));
        out.push_back(static_cast<char>(0x80U | ((cp >> 12U) & 0x3fU)));
        out.push_back(static_cast<char>(0x80U | ((cp >> 6U) & 0x3fU)));
        out.push_back(static_cast<char>(0x80U | (cp & 0x3fU)));
    }
    return out;
}

std::uint32_t MathItalic(std::uint32_t cp) {
    if (cp >= 'A' && cp <= 'Z')
        return 0x1d434 + cp - 'A';
    if (cp >= 'a' && cp <= 'z')
        return cp == 'h' ? 0x210e : 0x1d44e + cp - 'a';

    static const std::pair<std::uint32_t, std::uint32_t> kGreek[] = {
        {0x0391, 0x1d6e2}, {0x0392, 0x1d6e3}, {0x0393, 0x1d6e4},
        {0x0394, 0x1d6e5}, {0x0395, 0x1d6e6}, {0x0396, 0x1d6e7},
        {0x0397, 0x1d6e8}, {0x0398, 0x1d6e9}, {0x0399, 0x1d6ea},
        {0x039a, 0x1d6eb}, {0x039b, 0x1d6ec}, {0x039c, 0x1d6ed},
        {0x039d, 0x1d6ee}, {0x039e, 0x1d6ef}, {0x039f, 0x1d6f0},
        {0x03a0, 0x1d6f1}, {0x03a1, 0x1d6f2}, {0x03a3, 0x1d6f4},
        {0x03a4, 0x1d6f5}, {0x03a5, 0x1d6f6}, {0x03a6, 0x1d6f7},
        {0x03a7, 0x1d6f8}, {0x03a8, 0x1d6f9}, {0x03a9, 0x1d6fa},
        {0x03b1, 0x1d6fc}, {0x03b2, 0x1d6fd}, {0x03b3, 0x1d6fe},
        {0x03b4, 0x1d6ff}, {0x03b5, 0x1d700}, {0x03b6, 0x1d701},
        {0x03b7, 0x1d702}, {0x03b8, 0x1d703}, {0x03b9, 0x1d704},
        {0x03ba, 0x1d705}, {0x03bb, 0x1d706}, {0x03bc, 0x1d707},
        {0x03bd, 0x1d708}, {0x03be, 0x1d709}, {0x03bf, 0x1d70a},
        {0x03c0, 0x1d70b}, {0x03c1, 0x1d70c}, {0x03c2, 0x1d70d},
        {0x03c3, 0x1d70e}, {0x03c4, 0x1d70f}, {0x03c5, 0x1d710},
        {0x03c6, 0x1d711}, {0x03c7, 0x1d712}, {0x03c8, 0x1d713},
        {0x03c9, 0x1d714}, {0x2202, 0x1d715}, {0x03f5, 0x1d716},
        {0x03d1, 0x1d717}, {0x03f0, 0x1d718}, {0x03d5, 0x1d719},
        {0x03f1, 0x1d71a}, {0x03d6, 0x1d71b},
    };
    for (const auto &[plain, italic] : kGreek) {
        if (plain == cp)
            return italic;
    }
    return cp;
}

std::string StyleVariable(std::string_view text) {
    std::string out;
    for (std::size_t pos = 0; pos < text.size();)
        out += Utf8(MathItalic(NextCodePoint(text, pos)));
    return out;
}

struct SymbolInfo {
    const char *text;
    bool variable = false;
    bool large_operator = false;
};

std::optional<SymbolInfo> CommandSymbol(std::string_view name) {
    static const std::pair<std::string_view, const char *> kGreek[] = {
        {"alpha", "α"},       {"beta", "β"},       {"gamma", "γ"},
        {"delta", "δ"},       {"epsilon", "ε"},    {"varepsilon", "ϵ"},
        {"zeta", "ζ"},        {"eta", "η"},        {"theta", "θ"},
        {"vartheta", "ϑ"},    {"iota", "ι"},       {"kappa", "κ"},
        {"lambda", "λ"},      {"mu", "μ"},         {"nu", "ν"},
        {"xi", "ξ"},          {"omicron", "ο"},    {"pi", "π"},
        {"rho", "ρ"},         {"varrho", "ϱ"},     {"sigma", "σ"},
        {"varsigma", "ς"},    {"tau", "τ"},        {"upsilon", "υ"},
        {"phi", "φ"},         {"varphi", "ϕ"},     {"chi", "χ"},
        {"psi", "ψ"},         {"omega", "ω"},      {"Alpha", "Α"},
        {"Beta", "Β"},        {"Gamma", "Γ"},      {"Delta", "Δ"},
        {"Epsilon", "Ε"},     {"Zeta", "Ζ"},       {"Eta", "Η"},
        {"Theta", "Θ"},       {"Iota", "Ι"},       {"Kappa", "Κ"},
        {"Lambda", "Λ"},      {"Mu", "Μ"},         {"Nu", "Ν"},
        {"Xi", "Ξ"},          {"Omicron", "Ο"},    {"Pi", "Π"},
        {"Rho", "Ρ"},         {"Sigma", "Σ"},      {"Tau", "Τ"},
        {"Upsilon", "Υ"},     {"Phi", "Φ"},        {"Chi", "Χ"},
        {"Psi", "Ψ"},         {"Omega", "Ω"},
    };
    for (const auto &[key, value] : kGreek) {
        if (key == name)
            return SymbolInfo{value, true, false};
    }

    static const std::pair<std::string_view, const char *> kSymbols[] = {
        {"le", "≤"},          {"leq", "≤"},          {"ge", "≥"},
        {"geq", "≥"},         {"to", "→"},           {"rightarrow", "→"},
        {"leftarrow", "←"},   {"leftrightarrow", "↔"}, {"Rightarrow", "⇒"},
        {"Leftarrow", "⇐"},   {"Leftrightarrow", "⇔"}, {"times", "×"},
        {"div", "÷"},         {"neq", "≠"},          {"ne", "≠"},
        {"cdot", "·"},        {"pm", "±"},           {"mp", "∓"},
        {"approx", "≈"},      {"sim", "∼"},          {"equiv", "≡"},
        {"propto", "∝"},      {"in", "∈"},           {"notin", "∉"},
        {"subset", "⊂"},      {"subseteq", "⊆"},     {"supset", "⊃"},
        {"supseteq", "⊇"},    {"cap", "∩"},          {"cup", "∪"},
        {"setminus", "∖"},    {"emptyset", "∅"},     {"varnothing", "∅"},
        {"forall", "∀"},      {"exists", "∃"},       {"land", "∧"},
        {"lor", "∨"},         {"perp", "⟂"},         {"parallel", "∥"},
        {"infty", "∞"},       {"nabla", "∇"},        {"dots", "…"},
        {"ldots", "…"},       {"cdots", "⋯"},        {"vdots", "⋮"},
        {"ddots", "⋱"},
    };
    for (const auto &[key, value] : kSymbols) {
        if (key == name)
            return SymbolInfo{value, false, false};
    }

    static const std::pair<std::string_view, const char *> kLargeOperators[] = {
        {"int", "∫"}, {"oint", "∮"}, {"sum", "∑"}, {"prod", "∏"},
        {"bigcup", "⋃"}, {"bigcap", "⋂"},
    };
    for (const auto &[key, value] : kLargeOperators) {
        if (key == name)
            return SymbolInfo{value, false, true};
    }
    if (name == "partial")
        return SymbolInfo{"∂", true, false};
    return std::nullopt;
}

enum class NodeKind {
    Sequence,
    Literal,
    Variable,
    Upright,
    Fraction,
    Radical,
    Scripts,
    Delimited,
    Matrix,
    Accent,
};

struct Node {
    NodeKind kind = NodeKind::Sequence;
    std::string text;
    std::vector<Node> children;
    std::vector<std::vector<Node>> rows;
    char left = '.';
    char right = '.';
    bool large_operator = false;
    bool has_upper = false;
    bool has_lower = false;
    bool force_limits = false;
};

Node Leaf(NodeKind kind, std::string text, bool large_operator = false) {
    Node node;
    node.kind = kind;
    node.text = std::move(text);
    node.large_operator = large_operator;
    return node;
}

Node Sequence(std::vector<Node> children) {
    if (children.size() == 1)
        return std::move(children.front());
    Node node;
    node.kind = NodeKind::Sequence;
    node.children = std::move(children);
    return node;
}

Node Binary(NodeKind kind, Node first, Node second) {
    Node node;
    node.kind = kind;
    node.children.push_back(std::move(first));
    node.children.push_back(std::move(second));
    return node;
}

Node WithScripts(Node base, std::optional<Node> upper, std::optional<Node> lower,
                 bool force_limits) {
    Node node;
    node.kind = NodeKind::Scripts;
    node.force_limits = force_limits || base.large_operator;
    node.has_upper = upper.has_value();
    node.has_lower = lower.has_value();
    node.children.push_back(std::move(base));
    if (upper)
        node.children.push_back(std::move(*upper));
    if (lower)
        node.children.push_back(std::move(*lower));
    return node;
}

const Node *UpperScript(const Node &node) {
    return node.has_upper ? &node.children[1] : nullptr;
}

const Node *LowerScript(const Node &node) {
    if (!node.has_lower)
        return nullptr;
    return &node.children[node.has_upper ? 2 : 1];
}

class MathParser {
  public:
    explicit MathParser(std::string_view source) : source_(source) {}

    std::optional<Node> Parse() {
        auto node = ParseSequence(false, false);
        return node && pos_ == source_.size() ? node : std::nullopt;
    }

  private:
    bool CommandAt(std::string_view name) const {
        if (pos_ >= source_.size() || source_[pos_] != '\\')
            return false;
        const std::size_t begin = pos_ + 1;
        if (source_.substr(begin, name.size()) != name)
            return false;
        const std::size_t end = begin + name.size();
        return end == source_.size() || !IsAsciiLetter(source_[end]);
    }

    void ConsumeCommand(std::string_view name) { pos_ += name.size() + 1; }

    void SkipWhitespace() {
        while (pos_ < source_.size() && std::isspace(static_cast<unsigned char>(source_[pos_])))
            ++pos_;
    }

    std::optional<Node> ParseSequence(bool stops_at_brace, bool stops_at_right) {
        std::vector<Node> parts;
        while (pos_ < source_.size()) {
            if (source_[pos_] == '}') {
                if (!stops_at_brace)
                    return std::nullopt;
                ++pos_;
                return Sequence(std::move(parts));
            }
            if (stops_at_right && CommandAt("right"))
                return Sequence(std::move(parts));
            if (CommandAt("over")) {
                if (!stops_at_brace || parts.empty())
                    return std::nullopt;
                ConsumeCommand("over");
                auto denominator = ParseSequence(true, false);
                if (!denominator)
                    return std::nullopt;
                return Binary(NodeKind::Fraction, Sequence(std::move(parts)),
                              std::move(*denominator));
            }

            auto atom = ParseAtom();
            if (!atom)
                return std::nullopt;

            bool explicit_limits = false;
            const std::size_t before_limits = pos_;
            SkipWhitespace();
            if (CommandAt("limits")) {
                ConsumeCommand("limits");
                SkipWhitespace();
                explicit_limits = true;
            } else {
                pos_ = before_limits;
            }

            std::optional<Node> upper;
            std::optional<Node> lower;
            while (pos_ < source_.size() && (source_[pos_] == '^' || source_[pos_] == '_')) {
                const bool is_upper = source_[pos_++] == '^';
                auto script = ParseScriptAtom();
                if (!script || (is_upper ? upper.has_value() : lower.has_value()))
                    return std::nullopt;
                if (is_upper)
                    upper = std::move(*script);
                else
                    lower = std::move(*script);
            }
            if (upper || lower)
                *atom = WithScripts(std::move(*atom), std::move(upper), std::move(lower),
                                    explicit_limits);
            else if (explicit_limits)
                return std::nullopt;
            parts.push_back(std::move(*atom));
        }
        return stops_at_brace || stops_at_right ? std::nullopt
                                                : std::optional<Node>(Sequence(std::move(parts)));
    }

    std::optional<Node> ParseAtom() {
        if (pos_ >= source_.size())
            return std::nullopt;
        const char c = source_[pos_];
        if (c == '{') {
            ++pos_;
            return ParseSequence(true, false);
        }
        if (c == '\\') {
            if (CommandAt("left"))
                return ParseDelimited();
            return ParseCommandNode();
        }
        if (c == '^' || c == '_' || c == '&')
            return std::nullopt;
        if (c == '\r' || c == '\n') {
            while (pos_ < source_.size() &&
                   (source_[pos_] == '\r' || source_[pos_] == '\n'))
                ++pos_;
            return Leaf(NodeKind::Literal, " ");
        }
        if (IsAsciiLetter(c)) {
            ++pos_;
            return Leaf(NodeKind::Variable, std::string(1, c));
        }
        const auto first = static_cast<unsigned char>(c);
        const std::size_t length = Utf8CharLength(first);
        if (length == 0 || pos_ + length > source_.size())
            return std::nullopt;
        const std::string one(source_.substr(pos_, length));
        pos_ += length;
        return Leaf(first >= 0x80U ? NodeKind::Variable : NodeKind::Literal, one);
    }

    std::optional<Node> ParseScriptAtom() {
        if (pos_ >= source_.size())
            return std::nullopt;
        if (source_[pos_] == '{') {
            ++pos_;
            return ParseSequence(true, false);
        }
        if (std::isdigit(static_cast<unsigned char>(source_[pos_]))) {
            const std::size_t begin = pos_;
            while (pos_ < source_.size() &&
                   std::isdigit(static_cast<unsigned char>(source_[pos_])))
                ++pos_;
            return Leaf(NodeKind::Literal, std::string(source_.substr(begin, pos_ - begin)));
        }
        return ParseAtom();
    }

    std::optional<std::string> ParseRawGroup() {
        SkipWhitespace();
        if (pos_ >= source_.size() || source_[pos_] != '{')
            return std::nullopt;
        ++pos_;
        const std::size_t begin = pos_;
        int depth = 1;
        while (pos_ < source_.size()) {
            if (source_[pos_] == '\\' && pos_ + 1 < source_.size() &&
                (source_[pos_ + 1] == '{' || source_[pos_ + 1] == '}')) {
                pos_ += 2;
                continue;
            }
            if (source_[pos_] == '{')
                ++depth;
            if (source_[pos_] == '}' && --depth == 0) {
                const std::string value(source_.substr(begin, pos_ - begin));
                ++pos_;
                return value;
            }
            ++pos_;
        }
        return std::nullopt;
    }

    std::optional<Node> ParseRequiredArgument() {
        SkipWhitespace();
        if (pos_ >= source_.size())
            return std::nullopt;
        if (source_[pos_] == '{') {
            ++pos_;
            return ParseSequence(true, false);
        }
        return ParseAtom();
    }

    std::optional<Node> ParseOptionalIndex() {
        if (pos_ >= source_.size() || source_[pos_] != '[')
            return std::nullopt;
        ++pos_;
        const std::size_t begin = pos_;
        int brace_depth = 0;
        while (pos_ < source_.size()) {
            if (source_[pos_] == '{')
                ++brace_depth;
            else if (source_[pos_] == '}')
                --brace_depth;
            else if (source_[pos_] == ']' && brace_depth == 0) {
                const std::string_view body = source_.substr(begin, pos_ - begin);
                ++pos_;
                MathParser parser(body);
                return parser.Parse();
            }
            ++pos_;
        }
        return std::nullopt;
    }

    std::optional<char> ParseDelimiterChar() {
        SkipWhitespace();
        if (pos_ >= source_.size())
            return std::nullopt;
        if (source_[pos_] != '\\')
            return source_[pos_++];
        ++pos_;
        if (pos_ >= source_.size())
            return std::nullopt;
        if (!IsAsciiLetter(source_[pos_]))
            return source_[pos_++];
        const std::size_t begin = pos_;
        while (pos_ < source_.size() && IsAsciiLetter(source_[pos_]))
            ++pos_;
        const std::string_view name = source_.substr(begin, pos_ - begin);
        if (name == "lbrace")
            return '{';
        if (name == "rbrace")
            return '}';
        if (name == "lvert" || name == "rvert" || name == "vert")
            return '|';
        return std::nullopt;
    }

    std::optional<Node> ParseDelimited() {
        ConsumeCommand("left");
        auto left = ParseDelimiterChar();
        if (!left)
            return std::nullopt;
        auto inner = ParseSequence(false, true);
        if (!inner || !CommandAt("right"))
            return std::nullopt;
        ConsumeCommand("right");
        auto right = ParseDelimiterChar();
        if (!right)
            return std::nullopt;
        Node node;
        node.kind = NodeKind::Delimited;
        node.left = *left;
        node.right = *right;
        node.children.push_back(std::move(*inner));
        return node;
    }

    std::optional<Node> ParseEnvironment() {
        auto environment = ParseRawGroup();
        if (!environment || (*environment != "matrix" && *environment != "pmatrix" &&
                             *environment != "bmatrix" && *environment != "array"))
            return std::nullopt;
        if (*environment == "array" && !ParseRawGroup())
            return std::nullopt;

        const std::string ending = "\\end{" + *environment + "}";
        const std::size_t end = source_.find(ending, pos_);
        if (end == std::string_view::npos)
            return std::nullopt;
        const std::string_view body = source_.substr(pos_, end - pos_);
        pos_ = end + ending.size();

        std::vector<std::vector<std::string>> raw_rows(1, std::vector<std::string>(1));
        int brace_depth = 0;
        for (std::size_t i = 0; i < body.size();) {
            if (body[i] == '\\' && i + 1 < body.size()) {
                if (body[i + 1] == '\\' && brace_depth == 0) {
                    raw_rows.emplace_back(1);
                    i += 2;
                    continue;
                }
                if (body[i + 1] == '{' || body[i + 1] == '}' || body[i + 1] == '&') {
                    raw_rows.back().back().append(body.substr(i, 2));
                    i += 2;
                    continue;
                }
            }
            if (body[i] == '{')
                ++brace_depth;
            else if (body[i] == '}') {
                if (brace_depth == 0)
                    return std::nullopt;
                --brace_depth;
            }
            if (body[i] == '&' && brace_depth == 0) {
                raw_rows.back().emplace_back();
                ++i;
                continue;
            }
            raw_rows.back().back() += body[i++];
        }
        if (brace_depth != 0)
            return std::nullopt;
        if (!raw_rows.empty() && raw_rows.back().size() == 1 &&
            Trim(raw_rows.back().front()).empty())
            raw_rows.pop_back();
        if (raw_rows.empty())
            return std::nullopt;

        const std::size_t columns = raw_rows.front().size();
        Node matrix;
        matrix.kind = NodeKind::Matrix;
        matrix.text = *environment;
        for (const auto &raw_row : raw_rows) {
            if (raw_row.size() != columns)
                return std::nullopt;
            std::vector<Node> row;
            for (const std::string &raw_cell : raw_row) {
                const std::string cell = Trim(raw_cell);
                MathParser parser(cell);
                auto parsed = parser.Parse();
                if (!parsed)
                    return std::nullopt;
                row.push_back(std::move(*parsed));
            }
            matrix.rows.push_back(std::move(row));
        }
        return matrix;
    }

    std::optional<Node> ParseCommandNode() {
        ++pos_;
        if (pos_ >= source_.size())
            return std::nullopt;
        if (!IsAsciiLetter(source_[pos_])) {
            const char escaped = source_[pos_++];
            switch (escaped) {
            case '{':
            case '}':
            case '_':
            case '^':
            case '%':
            case '$':
                return Leaf(NodeKind::Literal, std::string(1, escaped));
            case ' ':
            case ',':
            case ':':
            case ';':
                return Leaf(NodeKind::Literal, " ");
            case '!':
                return Leaf(NodeKind::Literal, "");
            case '|':
                return Leaf(NodeKind::Literal, "‖");
            default:
                return std::nullopt;
            }
        }

        const std::size_t begin = pos_;
        while (pos_ < source_.size() && IsAsciiLetter(source_[pos_]))
            ++pos_;
        const std::string_view name = source_.substr(begin, pos_ - begin);
        if (name == "frac" || name == "dfrac" || name == "tfrac") {
            auto numerator = ParseRequiredArgument();
            auto denominator = ParseRequiredArgument();
            if (!numerator || !denominator)
                return std::nullopt;
            return Binary(NodeKind::Fraction, std::move(*numerator), std::move(*denominator));
        }
        if (name == "sqrt") {
            SkipWhitespace();
            std::optional<Node> index;
            if (pos_ < source_.size() && source_[pos_] == '[') {
                index = ParseOptionalIndex();
                if (!index)
                    return std::nullopt;
            }
            auto radicand = ParseRequiredArgument();
            if (!radicand)
                return std::nullopt;
            Node node;
            node.kind = NodeKind::Radical;
            node.children.push_back(std::move(*radicand));
            if (index)
                node.children.push_back(std::move(*index));
            return node;
        }
        if (name == "operatorname" || name == "mathrm" || name == "text") {
            auto body = ParseRawGroup();
            return body ? std::optional<Node>(Leaf(NodeKind::Upright, std::move(*body)))
                        : std::nullopt;
        }
        if (name == "hat" || name == "widehat") {
            auto body = ParseRequiredArgument();
            if (!body)
                return std::nullopt;
            Node node;
            node.kind = NodeKind::Accent;
            node.children.push_back(std::move(*body));
            return node;
        }
        if (name == "mathbb") {
            auto body = ParseRawGroup();
            if (!body)
                return std::nullopt;
            std::string rendered;
            for (char c : *body) {
                switch (c) {
                case 'C': rendered += "ℂ"; break;
                case 'E': rendered += "𝔼"; break;
                case 'N': rendered += "ℕ"; break;
                case 'Q': rendered += "ℚ"; break;
                case 'R': rendered += "ℝ"; break;
                case 'Z': rendered += "ℤ"; break;
                default: rendered += c; break;
                }
            }
            return Leaf(NodeKind::Upright, std::move(rendered));
        }
        if (name == "begin")
            return ParseEnvironment();
        if (name == "quad" || name == "qquad") {
            if (pos_ < source_.size() && source_[pos_] == ' ')
                ++pos_;
            return Leaf(NodeKind::Literal, name == "quad" ? "  " : "    ");
        }
        if (name == "cos" || name == "sin" || name == "tan" || name == "cot" ||
            name == "sec" || name == "csc" || name == "log" || name == "ln" ||
            name == "min" || name == "max" || name == "mean" || name == "std" ||
            name == "exp" || name == "det" || name == "gcd" || name == "lim")
            return Leaf(NodeKind::Upright, std::string(name));
        if (name == "right" || name == "left" || name == "limits" || name == "over" ||
            name == "end")
            return std::nullopt;
        if (auto symbol = CommandSymbol(name))
            return Leaf(symbol->variable ? NodeKind::Variable : NodeKind::Literal, symbol->text,
                        symbol->large_operator);
        return std::nullopt;
    }

    std::string_view source_;
    std::size_t pos_ = 0;
};

const char *ScriptGlyph(std::uint32_t value, bool upper) {
    if (upper) {
        switch (value) {
        case '0': return "⁰";
        case '1': return "¹";
        case '2': return "²";
        case '3': return "³";
        case '4': return "⁴";
        case '5': return "⁵";
        case '6': return "⁶";
        case '7': return "⁷";
        case '8': return "⁸";
        case '9': return "⁹";
        case '+': return "⁺";
        case '-': return "⁻";
        case '=': return "⁼";
        case '(': return "⁽";
        case ')': return "⁾";
        case 'a': return "ᵃ";
        case 'b': return "ᵇ";
        case 'c': return "ᶜ";
        case 'd': return "ᵈ";
        case 'e': return "ᵉ";
        case 'f': return "ᶠ";
        case 'g': return "ᵍ";
        case 'h': return "ʰ";
        case 'i': return "ⁱ";
        case 'j': return "ʲ";
        case 'k': return "ᵏ";
        case 'l': return "ˡ";
        case 'm': return "ᵐ";
        case 'n': return "ⁿ";
        case 'o': return "ᵒ";
        case 'p': return "ᵖ";
        case 'r': return "ʳ";
        case 's': return "ˢ";
        case 't': return "ᵗ";
        case 'u': return "ᵘ";
        case 'v': return "ᵛ";
        case 'w': return "ʷ";
        case 'x': return "ˣ";
        case 'y': return "ʸ";
        case 'z': return "ᶻ";
        case 'A': return "ᴬ";
        case 'B': return "ᴮ";
        case 'D': return "ᴰ";
        case 'E': return "ᴱ";
        case 'G': return "ᴳ";
        case 'H': return "ᴴ";
        case 'I': return "ᴵ";
        case 'J': return "ᴶ";
        case 'K': return "ᴷ";
        case 'L': return "ᴸ";
        case 'M': return "ᴹ";
        case 'N': return "ᴺ";
        case 'O': return "ᴼ";
        case 'P': return "ᴾ";
        case 'R': return "ᴿ";
        case 'T': return "ᵀ";
        case 'U': return "ᵁ";
        case 'V': return "ⱽ";
        case 'W': return "ᵂ";
        case 0x03b2: return "ᵝ";
        case 0x03b3: return "ᵞ";
        case 0x03b4: return "ᵟ";
        case 0x03b8: return "ᶿ";
        case 0x03c6: return "ᵠ";
        case 0x03c7: return "ᵡ";
        default: return nullptr;
        }
    }
    switch (value) {
    case '0': return "₀";
    case '1': return "₁";
    case '2': return "₂";
    case '3': return "₃";
    case '4': return "₄";
    case '5': return "₅";
    case '6': return "₆";
    case '7': return "₇";
    case '8': return "₈";
    case '9': return "₉";
    case '+': return "₊";
    case '-': return "₋";
    case '=': return "₌";
    case '(': return "₍";
    case ')': return "₎";
    case 'a': return "ₐ";
    case 'e': return "ₑ";
    case 'h': return "ₕ";
    case 'i': return "ᵢ";
    case 'j': return "ⱼ";
    case 'k': return "ₖ";
    case 'l': return "ₗ";
    case 'm': return "ₘ";
    case 'n': return "ₙ";
    case 'o': return "ₒ";
    case 'p': return "ₚ";
    case 'r': return "ᵣ";
    case 's': return "ₛ";
    case 't': return "ₜ";
    case 'u': return "ᵤ";
    case 'v': return "ᵥ";
    case 'x': return "ₓ";
    default: return nullptr;
    }
}

std::optional<std::string> CompactScriptText(std::string_view text, bool upper) {
    std::string out;
    for (std::size_t pos = 0; pos < text.size();) {
        const char *glyph = ScriptGlyph(NextCodePoint(text, pos), upper);
        if (!glyph)
            return std::nullopt;
        out += glyph;
    }
    return out;
}

std::optional<std::string> CompactScript(const Node &node, bool upper) {
    if (node.kind == NodeKind::Literal || node.kind == NodeKind::Variable ||
        node.kind == NodeKind::Upright)
        return CompactScriptText(node.text, upper);
    if (node.kind != NodeKind::Sequence)
        return std::nullopt;
    std::string out;
    for (const Node &child : node.children) {
        auto part = CompactScript(child, upper);
        if (!part)
            return std::nullopt;
        out += *part;
    }
    return out;
}

std::size_t Utf8CodePointCount(std::string_view text) {
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

std::string RenderInlineNode(const Node &node) {
    switch (node.kind) {
    case NodeKind::Literal:
    case NodeKind::Upright:
        return node.text;
    case NodeKind::Variable:
        return StyleVariable(node.text);
    case NodeKind::Sequence: {
        std::string out;
        for (const Node &child : node.children)
            out += RenderInlineNode(child);
        return out;
    }
    case NodeKind::Fraction:
        return ParenthesizeIfNeeded(RenderInlineNode(node.children[0])) + "/" +
               ParenthesizeIfNeeded(RenderInlineNode(node.children[1]));
    case NodeKind::Radical: {
        std::string prefix;
        if (node.children.size() > 1) {
            if (auto compact = CompactScript(node.children[1], true))
                prefix = *compact;
            else
                prefix = "root(" + RenderInlineNode(node.children[1]) + ",";
        }
        const std::string body = ParenthesizeIfNeeded(RenderInlineNode(node.children[0]));
        return prefix.starts_with("root(") ? prefix + body + ")" : prefix + "√" + body;
    }
    case NodeKind::Scripts: {
        std::string out = RenderInlineNode(node.children[0]);
        if (const Node *lower = LowerScript(node)) {
            if (auto compact = CompactScript(*lower, false))
                out += *compact;
            else
                out += "_(" + RenderInlineNode(*lower) + ")";
        }
        if (const Node *upper = UpperScript(node)) {
            if (auto compact = CompactScript(*upper, true))
                out += *compact;
            else
                out += "^(" + RenderInlineNode(*upper) + ")";
        }
        return out;
    }
    case NodeKind::Delimited: {
        const std::string left = node.left == '.' ? "" : std::string(1, node.left);
        const std::string right = node.right == '.' ? "" : std::string(1, node.right);
        return left + RenderInlineNode(node.children[0]) + right;
    }
    case NodeKind::Matrix: {
        std::string out = "[";
        for (std::size_t row = 0; row < node.rows.size(); ++row) {
            out += row == 0 ? "[" : "; [";
            for (std::size_t col = 0; col < node.rows[row].size(); ++col) {
                if (col > 0)
                    out += ", ";
                out += RenderInlineNode(node.rows[row][col]);
            }
            out += ']';
        }
        return out + ']';
    }
    case NodeKind::Accent:
        return RenderInlineNode(node.children[0]) + "̂";
    }
    return {};
}

struct Box {
    std::vector<std::string> rows;
    int width = 0;
    int baseline = 0;
};

std::string PadRight(std::string text, int width) {
    text.append(static_cast<std::size_t>((std::max)(0, width -
                                                       static_cast<int>(DisplayWidthUtf8(text)))),
                ' ');
    return text;
}

Box TextBox(std::string text) {
    return {{text}, static_cast<int>(DisplayWidthUtf8(text)), 0};
}

Box HBox(std::vector<Box> parts) {
    if (parts.empty())
        return TextBox("");
    int top = 0;
    int bottom = 0;
    int width = 0;
    for (const Box &part : parts) {
        top = (std::max)(top, part.baseline);
        bottom = (std::max)(bottom, static_cast<int>(part.rows.size()) - part.baseline - 1);
        width += part.width;
    }
    Box out{{}, width, top};
    out.rows.resize(static_cast<std::size_t>(top + bottom + 1));
    for (int row = 0; row <= top + bottom; ++row) {
        for (const Box &part : parts) {
            const int local_row = row - (top - part.baseline);
            out.rows[static_cast<std::size_t>(row)] +=
                local_row >= 0 && local_row < static_cast<int>(part.rows.size())
                    ? PadRight(part.rows[static_cast<std::size_t>(local_row)], part.width)
                    : std::string(static_cast<std::size_t>(part.width), ' ');
        }
    }
    return out;
}

std::string Rule(int width) {
    std::string out;
    for (int column = 0; column < width; ++column)
        out += "─";
    return out;
}

Box FractionBox(Box numerator, Box denominator) {
    const int width = (std::max)(numerator.width, denominator.width) + 2;
    const auto center = [width](const Box &box, const std::string &row) {
        const int left = (width - box.width) / 2;
        return std::string(static_cast<std::size_t>(left), ' ') + PadRight(row, box.width) +
               std::string(static_cast<std::size_t>(width - left - box.width), ' ');
    };
    Box out{{}, width, static_cast<int>(numerator.rows.size())};
    for (const std::string &row : numerator.rows)
        out.rows.push_back(center(numerator, row));
    out.rows.push_back(Rule(width));
    for (const std::string &row : denominator.rows)
        out.rows.push_back(center(denominator, row));
    return out;
}

Box SideScriptsBox(Box base, std::optional<Box> upper, std::optional<Box> lower) {
    const int script_width = (std::max)(upper ? upper->width : 0, lower ? lower->width : 0);
    const int upper_height = upper ? static_cast<int>(upper->rows.size()) : 0;
    const int lower_height = lower ? static_cast<int>(lower->rows.size()) : 0;
    const int ascent = (std::max)(base.baseline, upper_height);
    const int descent =
        (std::max)(static_cast<int>(base.rows.size()) - base.baseline - 1, lower_height);
    Box out{{}, base.width + script_width, ascent};
    out.rows.resize(static_cast<std::size_t>(ascent + descent + 1));
    for (int row = 0; row <= ascent + descent; ++row) {
        const int base_row = row - (ascent - base.baseline);
        out.rows[static_cast<std::size_t>(row)] =
            base_row >= 0 && base_row < static_cast<int>(base.rows.size())
                ? PadRight(base.rows[static_cast<std::size_t>(base_row)], base.width)
                : std::string(static_cast<std::size_t>(base.width), ' ');

        std::string script(static_cast<std::size_t>(script_width), ' ');
        const int upper_row = row - (ascent - upper_height);
        const int lower_row = row - (ascent + 1);
        if (upper && upper_row >= 0 && upper_row < upper_height)
            script = PadRight(upper->rows[static_cast<std::size_t>(upper_row)], script_width);
        else if (lower && lower_row >= 0 && lower_row < lower_height)
            script = PadRight(lower->rows[static_cast<std::size_t>(lower_row)], script_width);
        out.rows[static_cast<std::size_t>(row)] += script;
    }
    return out;
}

std::string CenterRow(const std::string &row, int row_width, int width) {
    const int left = (width - row_width) / 2;
    return std::string(static_cast<std::size_t>(left), ' ') + PadRight(row, row_width) +
           std::string(static_cast<std::size_t>(width - left - row_width), ' ');
}

Box LimitsBox(Box base, std::optional<Box> upper, std::optional<Box> lower) {
    const int width = (std::max)({base.width, upper ? upper->width : 0, lower ? lower->width : 0});
    Box out{{}, width, (upper ? static_cast<int>(upper->rows.size()) : 0) + base.baseline};
    if (upper) {
        for (const std::string &row : upper->rows)
            out.rows.push_back(CenterRow(row, upper->width, width));
    }
    for (const std::string &row : base.rows)
        out.rows.push_back(CenterRow(row, base.width, width));
    if (lower) {
        for (const std::string &row : lower->rows)
            out.rows.push_back(CenterRow(row, lower->width, width));
    }
    return out;
}

Box DelimiterBox(char delimiter, bool left, int height, int baseline) {
    if (delimiter == '.')
        return {std::vector<std::string>(static_cast<std::size_t>(height)), 0, baseline};
    if (height == 1)
        return TextBox(std::string(1, delimiter));
    Box out{{}, 1, baseline};
    for (int row = 0; row < height; ++row) {
        if (delimiter == '(' || delimiter == ')') {
            out.rows.push_back(left ? (row == 0 ? "⎛" : (row + 1 == height ? "⎝" : "⎜"))
                                    : (row == 0 ? "⎞" : (row + 1 == height ? "⎠" : "⎟")));
        } else if (delimiter == '[' || delimiter == ']') {
            out.rows.push_back(left ? (row == 0 ? "⎡" : (row + 1 == height ? "⎣" : "⎢"))
                                    : (row == 0 ? "⎤" : (row + 1 == height ? "⎦" : "⎥")));
        } else if (delimiter == '{' || delimiter == '}') {
            if (left) {
                out.rows.push_back(row == 0 ? "⎧"
                                            : (row + 1 == height ? "⎩"
                                                                 : (row == height / 2 ? "⎨" : "⎪")));
            } else {
                out.rows.push_back(row == 0 ? "⎫"
                                            : (row + 1 == height ? "⎭"
                                                                 : (row == height / 2 ? "⎬" : "⎪")));
            }
        } else {
            out.rows.push_back(std::string(1, delimiter));
        }
    }
    return out;
}

Box DelimitedBox(Box inner, char left, char right) {
    const int height = static_cast<int>(inner.rows.size());
    return HBox({DelimiterBox(left, true, height, inner.baseline), std::move(inner),
                 DelimiterBox(right, false, height, inner.baseline)});
}

Box RadicalBox(Box radicand, std::optional<Box> index) {
    const int lead = (std::max)(2, index ? index->width : 0);
    Box out{{}, lead + radicand.width, radicand.baseline + 1};
    std::string top;
    if (index)
        top = PadRight(index->rows.front(), lead);
    else
        top.assign(static_cast<std::size_t>(lead), ' ');
    out.rows.push_back(top + Rule(radicand.width));
    for (int row = 0; row < static_cast<int>(radicand.rows.size()); ++row) {
        const std::string prefix =
            std::string(static_cast<std::size_t>(lead - 2), ' ') +
            (row == radicand.baseline ? "√ " : "  ");
        out.rows.push_back(prefix + PadRight(radicand.rows[static_cast<std::size_t>(row)],
                                             radicand.width));
    }
    return out;
}

Box MatrixBox(const Node &node, const std::function<Box(const Node &)> &render) {
    const std::size_t columns = node.rows.front().size();
    std::vector<std::vector<Box>> cells;
    std::vector<int> widths(columns, 0);
    for (const auto &node_row : node.rows) {
        std::vector<Box> row;
        for (std::size_t col = 0; col < columns; ++col) {
            row.push_back(render(node_row[col]));
            widths[col] = (std::max)(widths[col], row.back().width);
        }
        cells.push_back(std::move(row));
    }

    Box matrix{{}, 0, 0};
    for (std::size_t col = 0; col < columns; ++col)
        matrix.width += widths[col] + (col > 0 ? 2 : 0);
    for (const auto &row : cells) {
        int top = 0;
        int bottom = 0;
        for (const Box &cell : row) {
            top = (std::max)(top, cell.baseline);
            bottom = (std::max)(bottom,
                                static_cast<int>(cell.rows.size()) - cell.baseline - 1);
        }
        for (int physical = 0; physical <= top + bottom; ++physical) {
            std::string line;
            for (std::size_t col = 0; col < columns; ++col) {
                if (col > 0)
                    line += "  ";
                const Box &cell = row[col];
                const int local = physical - (top - cell.baseline);
                if (local < 0 || local >= static_cast<int>(cell.rows.size())) {
                    line += std::string(static_cast<std::size_t>(widths[col]), ' ');
                } else {
                    const int left = (widths[col] - cell.width) / 2;
                    line += std::string(static_cast<std::size_t>(left), ' ');
                    line += PadRight(cell.rows[static_cast<std::size_t>(local)], cell.width);
                    line += std::string(
                        static_cast<std::size_t>(widths[col] - left - cell.width), ' ');
                }
            }
            matrix.rows.push_back(std::move(line));
        }
    }
    matrix.baseline = static_cast<int>(matrix.rows.size()) / 2;
    if (node.text == "pmatrix")
        return DelimitedBox(std::move(matrix), '(', ')');
    if (node.text == "bmatrix")
        return DelimitedBox(std::move(matrix), '[', ']');
    return matrix;
}

Box RenderBox(const Node &node) {
    switch (node.kind) {
    case NodeKind::Literal:
    case NodeKind::Upright:
        return TextBox(node.text);
    case NodeKind::Variable:
        return TextBox(StyleVariable(node.text));
    case NodeKind::Sequence: {
        std::vector<Box> parts;
        for (const Node &child : node.children)
            parts.push_back(RenderBox(child));
        return HBox(std::move(parts));
    }
    case NodeKind::Fraction:
        return FractionBox(RenderBox(node.children[0]), RenderBox(node.children[1]));
    case NodeKind::Radical: {
        std::optional<Box> index;
        if (node.children.size() > 1)
            index = TextBox(RenderInlineNode(node.children[1]));
        return RadicalBox(RenderBox(node.children[0]), std::move(index));
    }
    case NodeKind::Scripts: {
        Box base = RenderBox(node.children[0]);
        std::optional<Box> upper;
        std::optional<Box> lower;
        const Node *upper_node = UpperScript(node);
        const Node *lower_node = LowerScript(node);
        if (node.force_limits) {
            if (upper_node)
                upper = RenderBox(*upper_node);
            if (lower_node)
                lower = RenderBox(*lower_node);
            return LimitsBox(std::move(base), std::move(upper), std::move(lower));
        }
        const auto compact_upper = upper_node ? CompactScript(*upper_node, true)
                                              : std::optional<std::string>(std::string());
        const auto compact_lower = lower_node ? CompactScript(*lower_node, false)
                                              : std::optional<std::string>(std::string());
        if (base.rows.size() == 1 && compact_upper && compact_lower)
            return TextBox(base.rows.front() + *compact_lower + *compact_upper);
        if (upper_node)
            upper = compact_upper ? TextBox(*compact_upper) : RenderBox(*upper_node);
        if (lower_node)
            lower = compact_lower ? TextBox(*compact_lower) : RenderBox(*lower_node);
        return SideScriptsBox(std::move(base), std::move(upper), std::move(lower));
    }
    case NodeKind::Delimited:
        return DelimitedBox(RenderBox(node.children[0]), node.left, node.right);
    case NodeKind::Matrix:
        return MatrixBox(node, RenderBox);
    case NodeKind::Accent:
        return TextBox(RenderInlineNode(node.children[0]) + "̂");
    }
    return TextBox("");
}

void TrimRight(std::string &text) {
    while (!text.empty() && text.back() == ' ')
        text.pop_back();
}

} // namespace

std::optional<std::string> RenderLatexInline(const std::string &latex) {
    MathParser parser(latex);
    auto node = parser.Parse();
    return node ? std::optional<std::string>(RenderInlineNode(*node)) : std::nullopt;
}

std::optional<std::vector<std::string>> RenderLatexBlock(const std::string &latex) {
    const std::string source = Trim(latex);
    MathParser parser(source);
    auto node = parser.Parse();
    if (!node)
        return std::nullopt;
    Box box = RenderBox(*node);
    // 早先的终端语法把单独出现的 matrix 当方阵画；嵌在 \left...\right
    // 里的 matrix 仍保持无框，免得方程组套上两层括号。
    if (node->kind == NodeKind::Matrix && node->text == "matrix")
        box = DelimitedBox(std::move(box), '[', ']');
    for (std::string &row : box.rows)
        TrimRight(row);
    return box.rows;
}

} // namespace lubancode::cli
