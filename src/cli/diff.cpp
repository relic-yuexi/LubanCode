#include "cli/diff.hpp"

#include <cstdio>

#include "cli/diff_syntax.hpp"
#include "cli/line_editor.hpp"  // TruncateUtf8ToDisplayWidth

namespace lubancode::cli {

namespace {

// 中段 LCS DP 的规模上限(行数乘积)。1000×1000 级别的中段(int 表也就
// 4MB 一瞬)朴素 DP 毫无压力;真超了说明是超大文件的整篇重写,退化成
// 整删整增,反正 FormatDiff 400 行就截了,追求最小 diff 没有意义。
constexpr std::size_t kDpCellCap = 4'000'000;

// 变更行前后各留几行上下文,更远的未变段折叠。
constexpr int kContextRadius = 3;

// 超宽截断 + "..."(跟 transcript.cpp 同一套手艺,那份在匿名空间里拿
// 不到,这里自备一份)。
std::string TruncateWithEllipsis(const std::string& utf8, int max_width) {
    if (max_width <= 0) {
        return std::string();
    }
    const std::string fit = TruncateUtf8ToDisplayWidth(utf8, max_width);
    if (fit == utf8) {
        return utf8;
    }
    if (max_width <= 3) {
        return std::string("...").substr(0, static_cast<std::size_t>(max_width));
    }
    return TruncateUtf8ToDisplayWidth(utf8, max_width - 3) + "...";
}

std::size_t CountOccurrences(const std::string& text, const std::string& needle) {
    if (needle.empty()) {
        return 0;
    }
    std::size_t count = 0;
    std::size_t pos = 0;
    while ((pos = text.find(needle, pos)) != std::string::npos) {
        ++count;
        pos += needle.size();
    }
    return count;
}

std::string ReplaceOccurrences(const std::string& text, const std::string& old_s, const std::string& new_s,
                                bool replace_all) {
    std::string result;
    result.reserve(text.size());
    std::size_t pos = 0;
    bool replaced_once = false;
    while (true) {
        const std::size_t found = (!replace_all && replaced_once) ? std::string::npos : text.find(old_s, pos);
        if (found == std::string::npos) {
            result.append(text, pos, std::string::npos);
            break;
        }
        result.append(text, pos, found - pos);
        result.append(new_s);
        pos = found + old_s.size();
        replaced_once = true;
    }
    return result;
}

int DigitCount(int value) {
    int digits = 1;
    while (value >= 10) {
        value /= 10;
        ++digits;
    }
    return digits;
}

}  // namespace

std::vector<std::string> SplitDiffLines(const std::string& text) {
    std::vector<std::string> lines;
    std::size_t pos = 0;
    while (pos < text.size()) {
        const std::size_t nl = text.find('\n', pos);
        std::string line =
            nl == std::string::npos ? text.substr(pos) : text.substr(pos, nl - pos);
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        lines.push_back(std::move(line));
        if (nl == std::string::npos) {
            break;
        }
        pos = nl + 1;
    }
    return lines;
}

std::vector<DiffLine> ComputeLineDiff(const std::vector<std::string>& old_lines,
                                       const std::vector<std::string>& new_lines) {
    const std::size_t na = old_lines.size();
    const std::size_t nb = new_lines.size();
    std::vector<DiffLine> out;
    out.reserve(na + nb);

    // 公共前缀。
    std::size_t pre = 0;
    while (pre < na && pre < nb && old_lines[pre] == new_lines[pre]) {
        out.push_back(DiffLine{DiffLineKind::Context, old_lines[pre], static_cast<int>(pre) + 1,
                               static_cast<int>(pre) + 1});
        ++pre;
    }
    // 公共后缀(别跟前缀重叠)。
    std::size_t suf = 0;
    while (suf < na - pre && suf < nb - pre && old_lines[na - 1 - suf] == new_lines[nb - 1 - suf]) {
        ++suf;
    }

    const std::size_t ma = na - pre - suf;
    const std::size_t mb = nb - pre - suf;

    if (ma > 0 && mb > 0 && ma * mb <= kDpCellCap) {
        // 朴素 LCS DP:dp[i][j] = old[pre+i..) 与 new[pre+j..) 的 LCS 长度。
        const std::size_t cols = mb + 1;
        std::vector<int> dp((ma + 1) * (mb + 1), 0);
        for (std::size_t i = ma; i-- > 0;) {
            for (std::size_t j = mb; j-- > 0;) {
                if (old_lines[pre + i] == new_lines[pre + j]) {
                    dp[i * cols + j] = dp[(i + 1) * cols + (j + 1)] + 1;
                } else {
                    dp[i * cols + j] =
                        dp[(i + 1) * cols + j] > dp[i * cols + (j + 1)] ? dp[(i + 1) * cols + j]
                                                                         : dp[i * cols + (j + 1)];
                }
            }
        }
        // 回溯:相等走 Context;否则删优先(>=),让每个变更块先 - 后 +。
        std::size_t i = 0;
        std::size_t j = 0;
        while (i < ma && j < mb) {
            if (old_lines[pre + i] == new_lines[pre + j]) {
                out.push_back(DiffLine{DiffLineKind::Context, old_lines[pre + i],
                                       static_cast<int>(pre + i) + 1, static_cast<int>(pre + j) + 1});
                ++i;
                ++j;
            } else if (dp[(i + 1) * cols + j] >= dp[i * cols + (j + 1)]) {
                out.push_back(
                    DiffLine{DiffLineKind::Del, old_lines[pre + i], static_cast<int>(pre + i) + 1, 0});
                ++i;
            } else {
                out.push_back(
                    DiffLine{DiffLineKind::Add, new_lines[pre + j], 0, static_cast<int>(pre + j) + 1});
                ++j;
            }
        }
        for (; i < ma; ++i) {
            out.push_back(DiffLine{DiffLineKind::Del, old_lines[pre + i], static_cast<int>(pre + i) + 1, 0});
        }
        for (; j < mb; ++j) {
            out.push_back(DiffLine{DiffLineKind::Add, new_lines[pre + j], 0, static_cast<int>(pre + j) + 1});
        }
    } else {
        // 一边是空(纯增/纯删),或规模超限——整删整增。
        for (std::size_t i = 0; i < ma; ++i) {
            out.push_back(DiffLine{DiffLineKind::Del, old_lines[pre + i], static_cast<int>(pre + i) + 1, 0});
        }
        for (std::size_t j = 0; j < mb; ++j) {
            out.push_back(DiffLine{DiffLineKind::Add, new_lines[pre + j], 0, static_cast<int>(pre + j) + 1});
        }
    }

    // 公共后缀。
    for (std::size_t k = 0; k < suf; ++k) {
        out.push_back(DiffLine{DiffLineKind::Context, old_lines[na - suf + k],
                               static_cast<int>(na - suf + k) + 1, static_cast<int>(nb - suf + k) + 1});
    }
    return out;
}

std::string FormatDiff(const std::vector<DiffLine>& diff, const Theme& theme, int width, int max_lines,
                        std::size_t max_bytes, std::string_view file_path) {
    bool has_change = false;
    for (const auto& line : diff) {
        if (line.kind != DiffLineKind::Context) {
            has_change = true;
            break;
        }
    }
    if (!has_change) {
        return "(无变化)\n";
    }

    // 变更行 ±kContextRadius 行保留,其余上下文折叠。
    const std::size_t n = diff.size();
    std::vector<char> keep(n, 0);
    for (std::size_t i = 0; i < n; ++i) {
        if (diff[i].kind == DiffLineKind::Context) {
            continue;
        }
        const std::size_t lo = i >= static_cast<std::size_t>(kContextRadius) ? i - kContextRadius : 0;
        const std::size_t hi =
            i + kContextRadius < n ? i + kContextRadius : n - 1;
        for (std::size_t j = lo; j <= hi; ++j) {
            keep[j] = 1;
        }
    }

    // 行号栏宽度:保留行里最大的行号说了算,至少 3 格。
    int max_no = 0;
    for (std::size_t i = 0; i < n; ++i) {
        if (!keep[i]) {
            continue;
        }
        const int no = diff[i].kind == DiffLineKind::Del ? diff[i].old_no : diff[i].new_no;
        if (no > max_no) {
            max_no = no;
        }
    }
    int num_w = DigitCount(max_no);
    if (num_w < 3) {
        num_w = 3;
    }

    // 先把"显示行"(变更/上下文/折叠标注)全生成成 plain 文本,再统一
    // 截宽、限行限字节、上色。
    struct Row {
        std::string plain;
        DiffLineKind kind = DiffLineKind::Context;
        bool folded = false;
    };
    std::vector<Row> rows;
    std::size_t i = 0;
    while (i < n) {
        if (!keep[i]) {
            std::size_t j = i;
            while (j < n && !keep[j]) {
                ++j;
            }
            Row row;
            row.plain = std::string(static_cast<std::size_t>(num_w) + 3, ' ') + "\xE2\x80\xA6(" +
                        std::to_string(j - i) + " 行未变)";
            row.folded = true;
            rows.push_back(std::move(row));
            i = j;
            continue;
        }
        const DiffLine& d = diff[i];
        const char sign = d.kind == DiffLineKind::Del ? '-' : (d.kind == DiffLineKind::Add ? '+' : ' ');
        const int no = d.kind == DiffLineKind::Del ? d.old_no : d.new_no;
        char gutter[32];
        std::snprintf(gutter, sizeof(gutter), "%*d %c ", num_w, no, sign);
        Row row;
        row.plain = std::string(gutter) + d.text;
        row.kind = d.kind;
        rows.push_back(std::move(row));
        ++i;
    }

    const bool limit_lines = max_lines > 0;
    const bool limit_bytes = max_bytes > 0;
    std::string out;
    std::size_t bytes = 0;
    int emitted = 0;
    const DiffSyntaxLanguage syntax_language = DetectDiffSyntaxLanguage(file_path);
    for (std::size_t r = 0; r < rows.size(); ++r) {
        std::string plain = rows[r].plain;
        if (width > 0) {
            plain = TruncateWithEllipsis(plain, width);
        }
        const std::size_t line_bytes = plain.size() + 1;
        if ((limit_lines && emitted >= max_lines) || (limit_bytes && bytes + line_bytes > max_bytes)) {
            out += "\xE2\x80\xA6 已省略 " + std::to_string(rows.size() - r) +
                   " 行(Ctrl+E 查看完整)\n";
            return out;
        }
        // 上色(Claude Code Update 样式):删除行整行(行号 + '- ' + 内容)
        // 套红底、新增行套绿底,行尾紧跟 reset——背景只铺到内容实际结尾,
        // 不填充到终端宽,拖选复制不带大片色块;截宽在上头已做完,reset
        // 永远落在行尾。上下文行行号栏染淡色、正文原色;折叠标注走淡色,
        // 不上底。plain 主题各字段全是空串,天然退化成纯文本,靠前缀辨认。
        if (rows[r].folded) {
            out += theme.stats + plain + theme.reset;
        } else {
            // 行号栏(数字 + 空格 + 符号位 + 空格)全是 ASCII,按字节切安全;
            // 宽度掐得极窄时行可能比栏还短,取短的那头。
            const std::size_t gutter = static_cast<std::size_t>(num_w) + 3 < plain.size()
                                            ? static_cast<std::size_t>(num_w) + 3
                                            : plain.size();
            if (rows[r].kind == DiffLineKind::Del) {
                out += theme.diff_del_bg;
            } else if (rows[r].kind == DiffLineKind::Add) {
                out += theme.diff_add_bg;
            }
            if (!theme.diff_line_no.empty()) {
                // diff_syntax_plain 只复原前景/dim,不复原背景;变更行的底色
                // 因而不会在行号与代码交界处断开。
                out += theme.diff_line_no + plain.substr(0, gutter) + theme.diff_syntax_plain;
            } else {
                out += plain.substr(0, gutter);
            }
            out += HighlightDiffCodeLine(plain.substr(gutter), syntax_language, theme);
            if (rows[r].kind != DiffLineKind::Context || !theme.reset.empty()) {
                out += theme.reset;
            }
        }
        out += "\n";
        bytes += line_bytes;
        ++emitted;
    }
    return out;
}

EditDiffPreview BuildEditDiff(const std::string& file_content, const std::string& old_string,
                               const std::string& new_string, bool replace_all) {
    EditDiffPreview out;
    const std::size_t occurrences = CountOccurrences(file_content, old_string);
    if (occurrences == 0) {
        // 定位失败(文件读不出来 / old_string 压根不在里头):只对比新旧
        // 两段,行号是段内行号,聊胜于无——真执行时工具自己会报错。
        out.located = false;
        out.replaced_count = 0;
        out.lines = ComputeLineDiff(SplitDiffLines(old_string), SplitDiffLines(new_string));
        return out;
    }
    out.located = true;
    out.replaced_count = replace_all ? occurrences : 1;
    const std::string updated = ReplaceOccurrences(file_content, old_string, new_string, replace_all);
    // 整份旧文 vs 替换后全文:公共前后缀一剥,剩下的正是替换点;FormatDiff
    // 折叠后每处变更自带 ±3 行真实上下文和真实行号,replace_all 的每一处
    // 都各自成块。
    out.lines = ComputeLineDiff(SplitDiffLines(file_content), SplitDiffLines(updated));
    return out;
}

std::vector<DiffLine> BuildWriteDiff(const std::optional<std::string>& old_content,
                                      const std::string& new_content) {
    const std::vector<std::string> new_lines = SplitDiffLines(new_content);
    if (!old_content.has_value()) {
        // 新文件:全部新增。
        std::vector<DiffLine> out;
        out.reserve(new_lines.size());
        for (std::size_t i = 0; i < new_lines.size(); ++i) {
            out.push_back(DiffLine{DiffLineKind::Add, new_lines[i], 0, static_cast<int>(i) + 1});
        }
        return out;
    }
    return ComputeLineDiff(SplitDiffLines(*old_content), new_lines);
}

}  // namespace lubancode::cli
