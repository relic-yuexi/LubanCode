#include "cli/markdown.hpp"

#include <algorithm>
#include <cstddef>

#include "cli/latex_math.hpp"
#include "cli/line_editor.hpp"  // DisplayWidthUtf8 / TruncateUtf8ToDisplayWidth / WrapUtf8ToDisplayWidth

namespace lubancode::cli {

namespace {

// ---- 通用小工具 -----------------------------------------------------------

// 按 \n 拆行,行尾 \r 顺手剥掉(SSE 正文偶见 \r\n)。
std::vector<std::string> SplitLines(const std::string& text) {
    std::vector<std::string> lines;
    std::size_t pos = 0;
    while (pos <= text.size()) {
        std::size_t nl = text.find('\n', pos);
        if (nl == std::string::npos) {
            if (pos < text.size()) {
                lines.push_back(text.substr(pos));
            }
            break;
        }
        std::string line = text.substr(pos, nl - pos);
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        lines.push_back(std::move(line));
        pos = nl + 1;
    }
    return lines;
}

std::size_t LeadingSpaces(const std::string& line) {
    std::size_t i = 0;
    while (i < line.size() && line[i] == ' ') {
        ++i;
    }
    return i;
}

std::string Trim(const std::string& s) {
    std::size_t b = 0;
    std::size_t e = s.size();
    while (b < e && (s[b] == ' ' || s[b] == '\t')) {
        ++b;
    }
    while (e > b && (s[e - 1] == ' ' || s[e - 1] == '\t')) {
        --e;
    }
    return s.substr(b, e - b);
}

// $ 定界符若先带反斜线，便只是正文里的美元号。找闭合符时也跳过转义。
std::size_t FindClosingDollar(const std::string& text, std::size_t opening, bool block) {
    const std::size_t step = block ? 2 : 1;
    for (std::size_t pos = opening + step; pos < text.size();) {
        if (text[pos] == '\\' && pos + 1 < text.size()) {
            pos += 2;
            continue;
        }
        if (text[pos] == '$' && (!block || pos + 1 < text.size() && text[pos + 1] == '$')) {
            return pos;
        }
        ++pos;
    }
    return std::string::npos;
}

std::size_t FindBlockClose(const std::string& text, std::size_t first) {
    for (std::size_t pos = first; pos + 1 < text.size();) {
        if (text[pos] == '\\' && pos + 1 < text.size()) {
            pos += 2;
            continue;
        }
        if (text[pos] == '$' && text[pos + 1] == '$') {
            return pos;
        }
        ++pos;
    }
    return std::string::npos;
}

bool IsInlineDollar(const std::string& text, std::size_t pos) {
    return text[pos] == '$' && (pos == 0 || text[pos - 1] != '$') &&
           (pos + 1 == text.size() || text[pos + 1] != '$');
}

std::string RenderInlineMathOnly(const std::string& text) {
    std::string out;
    std::size_t pos = 0;
    while (pos < text.size()) {
        if (text[pos] == '\\' && pos + 1 < text.size() && text[pos + 1] == '$') {
            out += '$';
            pos += 2;
            continue;
        }
        // 行内码里的字样原样放回，不能在其中认数学。
        if (text[pos] == '`') {
            const std::size_t close = text.find('`', pos + 1);
            if (close != std::string::npos) {
                out += text.substr(pos, close - pos + 1);
                pos = close + 1;
                continue;
            }
        }
        if (IsInlineDollar(text, pos)) {
            const std::size_t close = FindClosingDollar(text, pos, false);
            if (close != std::string::npos && close > pos + 1) {
                const auto rendered = RenderLatexInline(text.substr(pos + 1, close - pos - 1));
                if (rendered) {
                    out += *rendered;
                    pos = close + 1;
                    continue;
                }
            }
        }
        out += text[pos++];
    }
    return out;
}

struct MathBlock {
    std::string latex;
    std::vector<std::string> raw_lines;
    std::size_t last_line = 0;
};

// 块级 $$ 只认从一行第一个非空白处起笔的写法；这是 Markdown 的常见形状，
// 也免得把行中两枚美元号错当成整块公式。
std::optional<MathBlock> ReadMathBlock(const std::vector<std::string>& lines, std::size_t first_line) {
    const std::string& first = lines[first_line];
    const std::size_t indent = LeadingSpaces(first);
    if (first.compare(indent, 2, "$$") != 0) {
        return std::nullopt;
    }

    MathBlock block;
    block.raw_lines.push_back(first);
    std::size_t content = indent + 2;
    for (std::size_t line = first_line; line < lines.size(); ++line) {
        const std::string& raw = lines[line];
        if (line != first_line) {
            block.raw_lines.push_back(raw);
            content = 0;
        }
        const std::size_t close = FindBlockClose(raw, content);
        if (close != std::string::npos) {
            if (!Trim(raw.substr(close + 2)).empty()) {
                return std::nullopt;
            }
            block.latex += raw.substr(content, close - content);
            block.last_line = line;
            return block;
        }
        block.latex += raw.substr(content);
        block.latex += '\n';
    }
    return std::nullopt;
}

bool HasLatexDelimiter(const std::string& text) {
    for (std::size_t pos = 0; pos < text.size(); ++pos) {
        if (text[pos] == '\\') {
            ++pos;
            continue;
        }
        if (text[pos] != '$') {
            continue;
        }
        const bool block = pos + 1 < text.size() && text[pos + 1] == '$';
        const std::size_t close = FindClosingDollar(text, pos, block);
        if (close != std::string::npos && close > pos + (block ? 2U : 1U)) {
            return true;
        }
    }
    return false;
}
// ---- 行首结构判定(检测与渲染共用一份,不写两遍) -------------------------

// "# 标题":1~6 个 # 后面跟一个空格。返回级数,不是标题给 0。
int HeadingLevel(const std::string& trimmed) {
    std::size_t hashes = 0;
    while (hashes < trimmed.size() && trimmed[hashes] == '#') {
        ++hashes;
    }
    if (hashes < 1 || hashes > 6) {
        return 0;
    }
    if (hashes >= trimmed.size() || trimmed[hashes] != ' ') {
        return 0;
    }
    return static_cast<int>(hashes);
}

// "- 内容" / "* 内容"。命中给 marker 后内容的起始下标,不命中给 npos。
std::size_t BulletContentPos(const std::string& trimmed) {
    if (trimmed.size() >= 2 && (trimmed[0] == '-' || trimmed[0] == '*') && trimmed[1] == ' ') {
        return 2;
    }
    return std::string::npos;
}

// "12. 内容"。命中给 {数字串长度, 内容起始下标},不命中给 {0, npos}。
struct OrderedMarker {
    std::size_t digits = 0;
    std::size_t content_pos = std::string::npos;
};
OrderedMarker OrderedListMarker(const std::string& trimmed) {
    std::size_t d = 0;
    while (d < trimmed.size() && trimmed[d] >= '0' && trimmed[d] <= '9') {
        ++d;
    }
    if (d == 0 || d > 3) {
        return {};
    }
    if (d + 1 < trimmed.size() && trimmed[d] == '.' && trimmed[d + 1] == ' ') {
        return {d, d + 2};
    }
    return {};
}

bool IsFenceLine(const std::string& trimmed) { return trimmed.compare(0, 3, "```") == 0; }

bool IsQuoteLine(const std::string& trimmed) {
    return trimmed == ">" || trimmed.compare(0, 2, "> ") == 0;
}

bool IsTableLine(const std::string& trimmed) {
    return trimmed.size() >= 2 && trimmed[0] == '|' && trimmed.find('|', 1) != std::string::npos;
}

// 表格分隔行的一格:只由 -、: 组成且至少一个 -(":---:" 那种对齐标记也认)。
bool IsSeparatorCell(const std::string& cell) {
    bool has_dash = false;
    for (const char c : cell) {
        if (c == '-') {
            has_dash = true;
        } else if (c != ':') {
            return false;
        }
    }
    return has_dash && !cell.empty();
}

// ---- 行内样式:**粗体** / *斜体* / `行内码` --------------------------------

// 一段带样式的连续文本。prefix/suffix 是 ANSI 开关(plain 主题下都是空串,
// 只剥标记不上色)。
struct Run {
    std::string prefix;
    std::string text;
    std::string suffix;
};

std::vector<Run> ParseInline(const std::string& s, bool plain) {
    std::vector<Run> runs;
    std::string cur;
    const auto flush_cur = [&]() {
        if (!cur.empty()) {
            runs.push_back({"", cur, ""});
            cur.clear();
        }
    };
    std::size_t i = 0;
    while (i < s.size()) {
        if (s[i] == '\\' && i + 1 < s.size() && s[i + 1] == '$') {
            cur += '$';
            i += 2;
            continue;
        }
        if (IsInlineDollar(s, i)) {
            const std::size_t close = FindClosingDollar(s, i, false);
            if (close != std::string::npos && close > i + 1) {
                const auto rendered = RenderLatexInline(s.substr(i + 1, close - i - 1));
                if (rendered) {
                    cur += *rendered;
                    i = close + 1;
                    continue;
                }
            }
        }
        if (s[i] == '`') {
            const std::size_t close = s.find('`', i + 1);
            if (close != std::string::npos && close > i + 1) {
                flush_cur();
                const std::string inner = s.substr(i + 1, close - i - 1);
                runs.push_back(plain ? Run{"", inner, ""} : Run{"\x1b[7m", inner, "\x1b[27m"});
                i = close + 1;
                continue;
            }
        } else if (s.compare(i, 2, "**") == 0) {
            const std::size_t close = s.find("**", i + 2);
            if (close != std::string::npos && close > i + 2) {
                flush_cur();
                const std::string inner = RenderInlineMathOnly(s.substr(i + 2, close - i - 2));
                runs.push_back(plain ? Run{"", inner, ""} : Run{"\x1b[1m", inner, "\x1b[22m"});
                i = close + 2;
                continue;
            }
        } else if (s[i] == '*') {
            const std::size_t close = s.find('*', i + 1);
            // 斜体的门槛收紧一点:内容非空、首尾不是空格——"3 * 4 * 5" 这种
            // 乘号不该被吞成斜体(宁可漏渲染,不可错渲染)。
            if (close != std::string::npos && close > i + 1) {
                const std::string inner = RenderInlineMathOnly(s.substr(i + 1, close - i - 1));
                if (inner.front() != ' ' && inner.back() != ' ') {
                    flush_cur();
                    runs.push_back(plain ? Run{"", inner, ""} : Run{"\x1b[3m", inner, "\x1b[23m"});
                    i = close + 1;
                    continue;
                }
            }
        }
        cur += s[i];
        ++i;
    }
    flush_cur();
    return runs;
}

// runs 拼成一行,按显示宽度截到 max_width 以内,截了补 …(占一列)。
struct Assembled {
    std::string text;
    int width = 0;
};

Assembled AssembleRuns(const std::vector<Run>& runs, int max_width) {
    int total = 0;
    for (const Run& run : runs) {
        total += static_cast<int>(DisplayWidthUtf8(run.text));
    }
    Assembled out;
    if (max_width <= 0) {
        return out;
    }
    if (total <= max_width) {
        for (const Run& run : runs) {
            out.text += run.prefix + run.text + run.suffix;
        }
        out.width = total;
        return out;
    }
    int budget = max_width - 1;  // 给 … 留一列
    for (const Run& run : runs) {
        if (budget <= 0) {
            break;
        }
        const std::string fit = TruncateUtf8ToDisplayWidth(run.text, budget);
        if (!fit.empty()) {
            out.text += run.prefix + fit + run.suffix;
            const int w = static_cast<int>(DisplayWidthUtf8(fit));
            budget -= w;
            out.width += w;
        } else {
            break;
        }
    }
    out.text += "\xE2\x80\xA6";  // …
    out.width += 1;
    return out;
}

// 纯文本按显示宽截断,截了补 …(代码块内容、标题这类不做行内解析的行用)。
std::string TruncatePlain(const std::string& text, int max_width) {
    if (max_width <= 0) {
        return std::string();
    }
    if (static_cast<int>(DisplayWidthUtf8(text)) <= max_width) {
        return text;
    }
    return TruncateUtf8ToDisplayWidth(text, max_width - 1) + "\xE2\x80\xA6";
}

// 把一段 UTF-8 切成逐码点子串(同 console_input 里 FooterUtf8Glyphs 的思路):
// 按首字节判定序列长,不校验续字节——给内部自己拼出来的字符串量宽/折行用,
// 不是严格解码器。折行要逐字量宽,得先把多字节字拆成独立小块。
std::vector<std::string> SplitUtf8Glyphs(const std::string& text) {
    std::vector<std::string> out;
    for (std::size_t i = 0; i < text.size();) {
        const unsigned char lead = static_cast<unsigned char>(text[i]);
        std::size_t bytes = 1;
        if ((lead & 0xE0U) == 0xC0U) {
            bytes = 2;
        } else if ((lead & 0xF0U) == 0xE0U) {
            bytes = 3;
        } else if ((lead & 0xF8U) == 0xF0U) {
            bytes = 4;
        }
        bytes = (std::min)(bytes, text.size() - i);
        out.push_back(text.substr(i, bytes));
        i += bytes;
    }
    return out;
}

// 把带行内样式(粗体/斜体/行内码)的一行 runs 按显示宽度折成多行。逐字
// 累加,下一个字超 max_width 就断;断点落在一个 run 中间时,折出来的各段
// 各自重新包好该 run 的 ANSI 开关(同一行内独立上色,跨行样式不串)。当前
// 行空时即便单字比 max_width 宽也照收(独占一行,不丢内容)。max_width<=0
// 或 runs 全空给空 vector。
std::vector<std::string> WrapRuns(const std::vector<Run>& runs, int max_width) {
    std::vector<std::string> lines;
    if (max_width <= 0) {
        return lines;
    }
    // 当前折行里已经攒下的各 run 片段:每段记它属哪个 run、该 run 的子串。
    // flush 时按顺序把每段 prefix+text+suffix 拼起来。
    struct Seg {
        std::size_t run;
        std::string text;
    };
    std::vector<Seg> segs;
    int width = 0;
    const auto flush = [&] {
        std::string line;
        for (const Seg& seg : segs) {
            line += runs[seg.run].prefix + seg.text + runs[seg.run].suffix;
        }
        lines.push_back(std::move(line));
        segs.clear();
        width = 0;
    };
    for (std::size_t r = 0; r < runs.size(); ++r) {
        for (const std::string& g : SplitUtf8Glyphs(runs[r].text)) {
            const int gw = static_cast<int>(DisplayWidthUtf8(g));
            if (width + gw > max_width && !segs.empty()) {
                flush();
            }
            if (!segs.empty() && segs.back().run == r) {
                segs.back().text += g;
            } else {
                segs.push_back({r, g});
            }
            width += gw;
        }
    }
    if (!segs.empty()) {
        flush();
    }
    return lines;
}

// ---- 表格 -----------------------------------------------------------------

std::vector<std::string> SplitTableCells(const std::string& trimmed) {
    // 掐头去尾的 |,中间按 | 劈开,每格 Trim。
    std::string body = trimmed;
    if (!body.empty() && body.front() == '|') {
        body.erase(0, 1);
    }
    if (!body.empty() && body.back() == '|') {
        body.pop_back();
    }
    std::vector<std::string> cells;
    std::size_t pos = 0;
    while (pos <= body.size()) {
        const std::size_t bar = body.find('|', pos);
        if (bar == std::string::npos) {
            cells.push_back(Trim(body.substr(pos)));
            break;
        }
        cells.push_back(Trim(body.substr(pos, bar - pos)));
        pos = bar + 1;
    }
    return cells;
}

bool IsSeparatorRow(const std::vector<std::string>& cells) {
    if (cells.empty()) {
        return false;
    }
    for (const std::string& cell : cells) {
        if (!IsSeparatorCell(cell)) {
            return false;
        }
    }
    return true;
}

// 一根边线:┌─┬─┐ / ├─┼─┤ / └─┴─┘,整根淡色。
std::string BorderLine(const std::vector<int>& widths, const char* left, const char* mid, const char* right,
                       const Theme& theme) {
    std::string line = theme.stats;
    line += left;
    for (std::size_t i = 0; i < widths.size(); ++i) {
        for (int c = 0; c < widths[i] + 2; ++c) {
            line += "\xE2\x94\x80";  // ─
        }
        line += i + 1 < widths.size() ? mid : right;
    }
    line += theme.reset;
    return line;
}

// 把攒下的 |…| 行渲染成对齐表格,追加进 out。攒的行凑不成表(不足两行、
// 剥掉分隔行后没内容)就原样放回去——宁可漏渲染。
void FlushTable(std::vector<std::string>& table_buf, std::vector<std::string>& out, const Theme& theme,
                int content_limit) {
    if (table_buf.empty()) {
        return;
    }
    const bool plain = theme.reset.empty();
    if (table_buf.size() < 2) {
        for (const std::string& raw : table_buf) {
            out.push_back(TruncatePlain(raw, content_limit));
        }
        table_buf.clear();
        return;
    }

    std::vector<std::vector<std::string>> rows;
    bool header_present = false;
    for (std::size_t i = 0; i < table_buf.size(); ++i) {
        std::vector<std::string> cells = SplitTableCells(Trim(table_buf[i]));
        if (IsSeparatorRow(cells)) {
            if (i == 1) {
                header_present = true;  // 首行之后紧跟分隔行 = 首行是表头
            }
            continue;  // 分隔行本身不渲染
        }
        rows.push_back(std::move(cells));
    }
    if (rows.empty()) {
        for (const std::string& raw : table_buf) {
            out.push_back(TruncatePlain(raw, content_limit));
        }
        table_buf.clear();
        return;
    }
    // 注意:table_buf 先不清——底下压宽压到底仍放不下时,还要拿原始行走
    // 逐行截断的退路。

    std::size_t ncols = 0;
    for (const auto& row : rows) {
        ncols = (std::max)(ncols, row.size());
    }

    // 各列取最大显示宽度(格内容先剥行内标记再量宽,CJK 按 2 算)。
    std::vector<int> widths(ncols, 1);
    std::vector<std::vector<std::vector<Run>>> parsed(rows.size());
    for (std::size_t r = 0; r < rows.size(); ++r) {
        parsed[r].resize(ncols);
        for (std::size_t c = 0; c < ncols; ++c) {
            const std::string cell = c < rows[r].size() ? rows[r][c] : std::string();
            parsed[r][c] = ParseInline(cell, plain);
            int w = 0;
            for (const Run& run : parsed[r][c]) {
                w += static_cast<int>(DisplayWidthUtf8(run.text));
            }
            widths[c] = (std::max)(widths[c], w);
        }
    }

    // 总宽 = 竖线 (ncols+1) + 每列 (宽+两侧空格)。超过终端宽就一格格压
    // 最宽的列(每列至少 2),压到放得下或者压无可压为止。
    int total = static_cast<int>(ncols) + 1;
    for (const int w : widths) {
        total += w + 2;
    }
    while (total > content_limit) {
        const auto widest = std::max_element(widths.begin(), widths.end());
        if (*widest <= 2) {
            break;
        }
        --*widest;
        --total;
    }
    // 压无可压还是放不下(极窄终端:一列的框架底线就要 6 列)——框线一画
    // 必物理折行,重画的行数账立刻失准。铁律优先:退回逐行截断原样过,
    // 宁可不画框,不破 width-1。
    if (total > content_limit) {
        for (const std::string& raw : table_buf) {
            out.push_back(TruncatePlain(raw, content_limit));
        }
        table_buf.clear();
        return;
    }
    table_buf.clear();

    out.push_back(BorderLine(widths, "\xE2\x94\x8C", "\xE2\x94\xAC", "\xE2\x94\x90", theme));  // ┌ ┬ ┐
    for (std::size_t r = 0; r < rows.size(); ++r) {
        std::string line = theme.stats + "\xE2\x94\x82" + theme.reset;  // │
        for (std::size_t c = 0; c < ncols; ++c) {
            Assembled cell = AssembleRuns(parsed[r][c], widths[c]);
            std::string cell_text = std::move(cell.text);
            if (!plain && header_present && r == 0 && !cell_text.empty()) {
                cell_text = "\x1b[1m" + cell_text + "\x1b[22m";  // 表头 bold
            }
            line += " " + cell_text + std::string(static_cast<std::size_t>((std::max)(0, widths[c] - cell.width)), ' ') +
                    " " + theme.stats + "\xE2\x94\x82" + theme.reset;
        }
        out.push_back(std::move(line));
        if (header_present && r == 0 && rows.size() > 1) {
            out.push_back(BorderLine(widths, "\xE2\x94\x9C", "\xE2\x94\xBC", "\xE2\x94\xA4", theme));  // ├ ┼ ┤
        }
    }
    out.push_back(BorderLine(widths, "\xE2\x94\x94", "\xE2\x94\xB4", "\xE2\x94\x98", theme));  // └ ┴ ┘
}

}  // namespace

// ---- 结构检测 -------------------------------------------------------------

bool DetectMarkdownStructure(const std::string& text) {
    if (HasLatexDelimiter(text)) {
        return true;
    }
    int consecutive_table = 0;
    for (const std::string& line : SplitLines(text)) {
        const std::string trimmed = line.substr(LeadingSpaces(line));
        if (IsFenceLine(trimmed)) {
            return true;
        }
        if (HeadingLevel(trimmed) > 0) {
            return true;
        }
        if (BulletContentPos(trimmed) != std::string::npos) {
            return true;
        }
        if (OrderedListMarker(trimmed).content_pos != std::string::npos) {
            return true;
        }
        if (IsQuoteLine(trimmed)) {
            return true;
        }
        if (IsTableLine(trimmed)) {
            if (++consecutive_table >= 2) {
                return true;
            }
        } else {
            consecutive_table = 0;
        }
    }
    // 行内结构:成对的 ** 或成对的 `(整篇里数,不必同一行)。
    std::size_t bold_marks = 0;
    for (std::size_t pos = text.find("**"); pos != std::string::npos; pos = text.find("**", pos + 2)) {
        ++bold_marks;
    }
    if (bold_marks >= 2) {
        return true;
    }
    if (std::count(text.begin(), text.end(), '`') >= 2) {
        return true;
    }
    return false;
}

// ---- 渲染 -----------------------------------------------------------------

std::vector<std::string> RenderMarkdown(const std::string& text, const Theme& theme, int width) {
    const bool plain = theme.reset.empty();
    const int content_limit = width > 0 ? width - 1 : 1000000;

    std::vector<std::string> out;
    std::vector<std::string> table_buf;
    bool in_code = false;

    const auto push_blank = [&out]() {
        if (!out.empty() && !out.back().empty()) {
            out.push_back(std::string());
        }
    };

    const std::vector<std::string> source_lines = SplitLines(text);
    for (std::size_t line_index = 0; line_index < source_lines.size(); ++line_index) {
        const std::string& line = source_lines[line_index];
        const std::size_t indent_spaces = LeadingSpaces(line);
        const std::string trimmed = line.substr(indent_spaces);

        // 代码块内:除了收尾围栏,一切原样(不解析、不上样式),只挂前缀。
        if (in_code) {
            if (IsFenceLine(trimmed)) {
                in_code = false;
                continue;
            }
            // "  │ " 前缀占 4 列。前缀 + 续行缩进各自都吃列;内容区(content_limit-4)
            // 不足 2 列时连一个宽字都盛不下,挂前缀必让该字撑破 width-1——退回裸折。
            const std::string prefix = "  " + theme.stats + "\xE2\x94\x82" + theme.reset + " ";
            if (content_limit < 4 + 2) {
                for (const std::string& w : WrapUtf8ToDisplayWidth(line, content_limit)) {
                    out.push_back(w);
                }
            } else {
                const std::vector<std::string> wraps = WrapUtf8ToDisplayWidth(line, content_limit - 4);
                if (wraps.empty()) {
                    out.push_back(prefix);
                } else {
                    out.push_back(prefix + wraps[0]);
                    for (std::size_t i = 1; i < wraps.size(); ++i) {
                        out.push_back("    " + wraps[i]);  // 续行缩进 4 列,对齐前缀后的内容起点
                    }
                }
            }
            continue;
        }

        if (IsFenceLine(trimmed)) {
            FlushTable(table_buf, out, theme, content_limit);
            in_code = true;
            const std::string lang = Trim(trimmed.substr(3));
            if (!lang.empty()) {
                if (content_limit < 4) {  // 同上:前缀 4 列放不下就裸截
                    out.push_back(theme.stats + TruncatePlain(lang, content_limit) + theme.reset);
                } else {
                    out.push_back("  " + theme.stats + "\xE2\x94\x82 " + TruncatePlain(lang, content_limit - 4) +
                                  theme.reset);
                }
            }
            continue;
        }

        if (const auto math = ReadMathBlock(source_lines, line_index)) {
            FlushTable(table_buf, out, theme, content_limit);
            const auto rendered = RenderLatexBlock(math->latex);
            if (rendered) {
                for (const std::string& math_line : *rendered) {
                    for (const std::string& w : WrapUtf8ToDisplayWidth(math_line, content_limit)) {
                        out.push_back(w);
                    }
                }
            } else {
                for (const std::string& raw : math->raw_lines) {
                    for (const std::string& w : WrapUtf8ToDisplayWidth(raw, content_limit)) {
                        out.push_back(w);
                    }
                }
            }
            line_index = math->last_line;
            continue;
        }

        if (IsTableLine(trimmed)) {
            table_buf.push_back(line);
            continue;
        }
        FlushTable(table_buf, out, theme, content_limit);

        if (const int level = HeadingLevel(trimmed); level > 0) {
            const std::string content = Trim(trimmed.substr(static_cast<std::size_t>(level) + 1));
            push_blank();
            std::string prefix;
            if (!plain) {
                prefix = "\x1b[1m";
                if (level <= 2) {
                    prefix = theme.banner + prefix;
                }
                if (level == 1) {
                    prefix += "\x1b[4m";
                }
            }
            const std::vector<std::string> wraps = WrapUtf8ToDisplayWidth(RenderInlineMathOnly(content), content_limit);
            if (wraps.empty()) {
                out.push_back(prefix + (plain ? std::string() : theme.reset));
            } else {
                for (const std::string& w : wraps) {
                    out.push_back(prefix + w + (plain ? std::string() : theme.reset));
                }
            }
            out.push_back(std::string());
            continue;
        }

        if (IsQuoteLine(trimmed)) {
            const std::string content = trimmed.size() > 2 ? trimmed.substr(2) : std::string();
            if (content_limit < 2 + 2) {  // "│ " 前缀后内容区不足 2 列:裸折保铁律
                for (const std::string& w : WrapUtf8ToDisplayWidth(trimmed, content_limit)) {
                    out.push_back(w);
                }
                continue;
            }
            // 折出来的每一段都重复 "│ " 前缀——视觉上是一根贯通的引用竖线,
            // 跟 markdown 里多行 "> " 引用块一个观感。
            const std::vector<std::string> wraps = WrapRuns(ParseInline(content, plain), content_limit - 2);
            if (wraps.empty()) {
                out.push_back(theme.stats + "\xE2\x94\x82" + theme.reset + " ");
            } else {
                for (const std::string& w : wraps) {
                    out.push_back(theme.stats + "\xE2\x94\x82" + theme.reset + " " + w);
                }
            }
            continue;
        }

        if (const std::size_t pos = BulletContentPos(trimmed); pos != std::string::npos) {
            const std::size_t level = indent_spaces / 2;
            const std::string pad(2 * (level + 1), ' ');
            const int prefix_cols = static_cast<int>(pad.size()) + 2;
            const std::string marker = pad + "\xE2\x80\xA2 ";  // •
            if (content_limit < prefix_cols + 2) {  // 内容区不足 2 列(极窄/嵌套过深):裸折
                for (const std::string& w : WrapUtf8ToDisplayWidth(trimmed, content_limit)) {
                    out.push_back(w);
                }
                continue;
            }
            const std::vector<std::string> wraps = WrapRuns(ParseInline(trimmed.substr(pos), plain),
                                                             content_limit - prefix_cols);
            if (wraps.empty()) {
                out.push_back(marker);
            } else {
                out.push_back(marker + wraps[0]);
                for (std::size_t i = 1; i < wraps.size(); ++i) {
                    out.push_back(std::string(prefix_cols, ' ') + wraps[i]);  // 续行对齐内容起点
                }
            }
            continue;
        }
        if (const OrderedMarker marker = OrderedListMarker(trimmed); marker.content_pos != std::string::npos) {
            const std::size_t level = indent_spaces / 2;
            const std::string pad(2 * (level + 1), ' ');
            const std::string number = trimmed.substr(0, marker.digits) + ". ";
            const int prefix_cols = static_cast<int>(pad.size() + number.size());
            const std::string prefix = pad + number;
            if (content_limit < prefix_cols + 2) {  // 同上:内容区不足 2 列就裸折
                for (const std::string& w : WrapUtf8ToDisplayWidth(trimmed, content_limit)) {
                    out.push_back(w);
                }
                continue;
            }
            const std::vector<std::string> wraps = WrapRuns(ParseInline(trimmed.substr(marker.content_pos), plain),
                                                             content_limit - prefix_cols);
            if (wraps.empty()) {
                out.push_back(prefix);
            } else {
                out.push_back(prefix + wraps[0]);
                for (std::size_t i = 1; i < wraps.size(); ++i) {
                    out.push_back(std::string(prefix_cols, ' ') + wraps[i]);  // 续行对齐内容起点
                }
            }
            continue;
        }

        // 空行:照放,连续多空行收成一个(标题前后补的空行也靠这条去重)。
        if (Trim(line).empty()) {
            push_blank();
            continue;
        }

        // 普通段落:做行内样式后按显示宽度折行;没有任何行内标记时,WrapRuns
        // 给回原文分段(宽以内原样、超宽折),原样过。短行不折,一字不动。
        for (const std::string& w : WrapRuns(ParseInline(line, plain), content_limit)) {
            out.push_back(w);
        }
    }
    FlushTable(table_buf, out, theme, content_limit);

    // 收尾:头尾的空行剪掉(重画时上下文自带间隔,不用自己再垫)。
    while (!out.empty() && out.back().empty()) {
        out.pop_back();
    }
    std::size_t first = 0;
    while (first < out.size() && out[first].empty()) {
        ++first;
    }
    if (first > 0) {
        out.erase(out.begin(), out.begin() + static_cast<std::ptrdiff_t>(first));
    }
    return out;
}

}  // namespace lubancode::cli
