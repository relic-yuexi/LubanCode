// mention_menu.hpp 的实现。

#include "cli/mention_menu.hpp"

#include <algorithm>
#include <map>

#include "cli/i18n.hpp"
#include "cli/line_editor.hpp"  // TruncateUtf8ToDisplayWidth / Utf8ToUtf32 / Utf32ToUtf8

namespace lubancode::cli {

namespace {

bool IsSpaceChar(char32_t c) {
    return c == U' ' || c == U'\t' || c == U'\n' || c == U'\r';
}

// 词元的非空白终点:路径里不会出现、口语里紧跟提及的标点(逗号顿号冒号
// 叹问、闭括号引号、CJK 同类)。"看 @src/main.cpp,然后…" 的词元不该把
// ",然后…" 吞进去。'.' 不在列——路径自己就带点。
bool IsMentionBoundary(char32_t c) {
    switch (c) {
        case U',':
        case U';':
        case U':':
        case U'!':
        case U'?':
        case U')':
        case U']':
        case U'}':
        case U'"':
        case U'\'':
        case U'，':
        case U'、':
        case U'；':
        case U'。':
        case U'：':
        case U'！':
        case U'？':
        case U'）':
        case U'】':
        case U'》':
        case U'」':
            return true;
        default:
            return false;
    }
}

std::string ToLowerAscii(std::string text) {
    for (char& c : text) {
        if (c >= 'A' && c <= 'Z') {
            c = static_cast<char>(c - 'A' + 'a');
        }
    }
    return text;
}

// 子序列匹配带分:query 的字符按序出现在 path 里即命中;连续命中的段越长、
// 命中落在路径段边界('/' 后首字符或整个路径首字符)加权越高。返回
// nullopt = 不命中。
std::optional<int> FuzzyScore(const std::string& path_lower, const std::string& query_lower) {
    if (query_lower.empty()) {
        return 0;
    }
    int score = 0;
    int run = 0;
    std::size_t qi = 0;
    for (std::size_t pi = 0; pi < path_lower.size() && qi < query_lower.size(); ++pi) {
        if (path_lower[pi] != query_lower[qi]) {
            run = 0;
            continue;
        }
        ++run;
        // 连越长越值钱;段边界(前一字符是 '/' 或位置 0)再加分。
        score += 1 + (run > 1 ? run : 0);
        if (pi == 0 || path_lower[pi - 1] == '/') {
            score += 3;
        }
        ++qi;
    }
    if (qi < query_lower.size()) {
        return std::nullopt;  // 没全用上,不命中
    }
    return score;
}

bool PathHasWhitespaceOrBracket(const std::string& path) {
    return path.find_first_of(" \t<>") != std::string::npos;
}

}  // namespace

std::optional<MentionToken> FindMentionToken(const std::u32string& line, std::size_t cursor) {
    if (cursor > line.size()) {
        cursor = line.size();
    }
    // 前向解析整行,拿全部 @ 词元的区间;光标落在谁的 [start,end] 里
    // (end 半开,光标在词元尾也算)谁就是"正在输入"的那个词元。回溯法
    // 在 @<带 空格> 形式上会断(词元中间就有空白),不用。
    for (std::size_t at = 0; at < line.size(); ++at) {
        if (line[at] != U'@') {
            continue;
        }
        // '@' 必须在行首或前面是空白/闭角,不然是邮箱一类的正文。
        if (at > 0 && !IsSpaceChar(line[at - 1]) && line[at - 1] != U'>') {
            continue;
        }
        std::size_t end = at + 1;
        bool bracketed = false;
        if (end < line.size() && line[end] == U'<') {
            bracketed = true;
            const std::size_t close = line.find(U'>', end + 1);
            end = close == std::u32string::npos ? line.size() : close + 1;
        } else {
            while (end < line.size() && !IsSpaceChar(line[end]) && !IsMentionBoundary(line[end])) {
                ++end;
            }
        }
        if (cursor < at || cursor > end) {
            continue;  // 光标不在(含词尾的)这个词元里
        }
        MentionToken token;
        token.start = at;
        token.end = end;
        token.bracketed = bracketed;
        if (bracketed) {
            const std::size_t open = at + 2;  // 跳过 '@' 与 '<'
            const std::size_t close = line.find(U'>', open);
            const std::size_t stop = close == std::u32string::npos ? line.size() : close;
            token.query = Utf32ToUtf8(line.substr(open, stop - open));
        } else {
            token.query = Utf32ToUtf8(line.substr(at + 1, end - (at + 1)));
        }
        // 正在编辑的词元:光标在词元里(不是后面已完成的词元尾巴)才给
        // 菜单——光标正好落在 end 且下一个字符是空白/行尾也算"刚敲完"。
        return token;
    }
    return std::nullopt;
}

std::vector<std::size_t> FuzzyMatchMentions(const std::vector<FileMentionEntry>& entries,
                                            const std::string& query, std::size_t limit) {
    const std::string query_lower = ToLowerAscii(query);
    // (score, 字典序 path, index)
    std::vector<std::tuple<int, std::string, std::size_t>> ranked;
    ranked.reserve(entries.size());
    for (std::size_t i = 0; i < entries.size(); ++i) {
        const auto score = FuzzyScore(ToLowerAscii(entries[i].relative_path), query_lower);
        if (score.has_value()) {
            ranked.emplace_back(*score, entries[i].relative_path, i);
        }
    }
    std::sort(ranked.begin(), ranked.end(), [](const auto& a, const auto& b) {
        if (std::get<0>(a) != std::get<0>(b)) {
            return std::get<0>(a) > std::get<0>(b);  // 分高在前
        }
        if (std::get<1>(a) != std::get<1>(b)) {
            return std::get<1>(a) < std::get<1>(b);
        }
        return std::get<2>(a) < std::get<2>(b);
    });
    std::vector<std::size_t> out;
    for (const auto& [score, path, index] : ranked) {
        if (out.size() >= limit) {
            break;
        }
        (void)score;
        (void)path;
        out.push_back(index);
    }
    return out;
}

std::vector<std::string> BuildMentionMenuLines(const std::vector<FileMentionEntry>& entries,
                                               const std::vector<std::size_t>& matches, int selected,
                                               int width) {
    std::vector<std::string> lines;
    lines.push_back(std::string(tr("mention.header")) + " · " + tr("mention.keys_hint"));
    if (matches.empty()) {
        lines.push_back("  " + tr("mention.no_match"));
        return lines;
    }
    for (std::size_t i = 0; i < matches.size(); ++i) {
        const FileMentionEntry& entry = entries[matches[i]];
        const bool is_selected = static_cast<int>(i) == selected;
        std::string line = is_selected ? "❯ " : "  ";
        line += entry.is_dir ? tr("mention.dir_icon") : tr("mention.file_icon");
        line += " ";
        line += entry.relative_path;
        if (entry.is_dir) {
            line += "/";
        }
        lines.push_back(TruncateUtf8ToDisplayWidth(line, (std::max)(1, width - 1)));
    }
    return lines;
}

std::string MentionInsertionString(const FileMentionEntry& entry) {
    std::string path = entry.relative_path;
    if (entry.is_dir && !path.empty() && path.back() != '/') {
        path += '/';
    }
    if (PathHasWhitespaceOrBracket(path)) {
        return "@<" + path + ">";
    }
    return "@" + path;
}

std::u32string ReplaceMentionToken(const std::u32string& line, const MentionToken& token,
                                   const std::string& insertion) {
    std::u32string out = line.substr(0, token.start);
    out += Utf8ToUtf32(insertion);
    if (token.end < line.size()) {
        out += line.substr(token.end);
    }
    return out;
}

std::vector<std::string> ExtractTextMentions(std::string_view submitted) {
    std::vector<std::string> out;
    std::string current;
    bool in_mention = false;
    bool bracketed = false;
    auto flush = [&] {
        if (in_mention && !current.empty()) {
            if (std::find(out.begin(), out.end(), current) == out.end()) {
                out.push_back(current);
            }
        }
        in_mention = false;
        bracketed = false;
        current.clear();
    };
    for (std::size_t i = 0; i < submitted.size(); ++i) {
        const char c = submitted[i];
        if (!in_mention) {
            if (c == '@' && (i == 0 || submitted[i - 1] == ' ' || submitted[i - 1] == '\t' ||
                             submitted[i - 1] == '\n')) {
                in_mention = true;
                bracketed = false;
                current.clear();
            }
            continue;
        }
        if (bracketed) {
            if (c == '>') {
                flush();
            } else {
                current.push_back(c);
            }
            continue;
        }
        if (current.empty() && c == '<') {
            bracketed = true;
            continue;
        }
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r' || IsMentionBoundary(static_cast<unsigned char>(c))) {
            flush();
        } else {
            current.push_back(c);
        }
    }
    flush();
    return out;
}

}  // namespace lubancode::cli
