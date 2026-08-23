#include "tools/edit_file.hpp"

#include "hooks/hash.hpp"  // Sha256Hex:undo token 的 pre/post 摘要

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <system_error>
#include <utility>
#include <vector>

#include "tools/isolation.hpp"
#include "tools/path_utils.hpp"
#include "tools/tool_text.hpp"  // 模型可见文案(描述/参数说明)查表,源头 prompts/tools/

namespace lubancode::tools {

namespace {

std::size_t CountOccurrences(const std::string& text, const std::string& needle) {
    std::size_t count = 0;
    std::size_t pos = 0;
    while ((pos = text.find(needle, pos)) != std::string::npos) {
        ++count;
        pos += needle.size();
    }
    return count;
}

std::string ReplaceAllOccurrences(const std::string& text, const std::string& old_s, const std::string& new_s) {
    std::string result;
    result.reserve(text.size());
    std::size_t pos = 0;
    while (true) {
        const std::size_t found = text.find(old_s, pos);
        if (found == std::string::npos) {
            result.append(text, pos, std::string::npos);
            break;
        }
        result.append(text, pos, found - pos);
        result.append(new_s);
        pos = found + old_s.size();
    }
    return result;
}

struct NormalizedText {
    std::string text;
    // normalized 字符边界 -> 原文字节边界，大小恒为 text.size()+1。
    std::vector<std::size_t> original_offsets;
};

NormalizedText NormalizeLineEndingsWithOffsets(const std::string& input) {
    NormalizedText out;
    out.text.reserve(input.size());
    out.original_offsets.reserve(input.size() + 1);
    out.original_offsets.push_back(0);
    for (std::size_t i = 0; i < input.size();) {
        if (input[i] == '\r') {
            i += (i + 1 < input.size() && input[i + 1] == '\n') ? 2 : 1;
            out.text.push_back('\n');
            out.original_offsets.push_back(i);
            continue;
        }
        out.text.push_back(input[i]);
        ++i;
        out.original_offsets.push_back(i);
    }
    return out;
}

std::string NormalizeLineEndings(const std::string& input) {
    return NormalizeLineEndingsWithOffsets(input).text;
}

std::string FileLineEnding(const std::string& input) {
    if (input.find("\r\n") != std::string::npos) {
        return "\r\n";
    }
    return input.find('\r') != std::string::npos ? "\r" : "\n";
}

std::string UseLineEnding(const std::string& input, const std::string& line_ending) {
    const std::string normalized = NormalizeLineEndings(input);
    if (line_ending == "\n") {
        return normalized;
    }
    std::string out;
    out.reserve(normalized.size() + normalized.size() / 8);
    for (const char c : normalized) {
        out += c == '\n' ? line_ending : std::string(1, c);
    }
    return out;
}

struct LineSpan {
    std::size_t start = 0;
    std::size_t content_end = 0;
    std::size_t end = 0;
};

std::vector<LineSpan> SplitLines(const std::string& text) {
    std::vector<LineSpan> lines;
    for (std::size_t start = 0; start < text.size();) {
        const std::size_t newline = text.find('\n', start);
        if (newline == std::string::npos) {
            lines.push_back({start, text.size(), text.size()});
            break;
        }
        lines.push_back({start, newline, newline + 1});
        start = newline + 1;
    }
    return lines;
}

std::size_t LeadingWhitespace(const std::string& text, const LineSpan& line) {
    std::size_t pos = line.start;
    while (pos < line.content_end && (text[pos] == ' ' || text[pos] == '\t')) {
        ++pos;
    }
    return pos - line.start;
}

std::size_t TrimmedLineEnd(const std::string& text, const LineSpan& line) {
    std::size_t end = line.content_end;
    while (end > line.start && (text[end - 1] == ' ' || text[end - 1] == '\t')) {
        --end;
    }
    return end;
}

struct CanonicalBlock {
    std::string text;
    std::string indent;
};

CanonicalBlock CanonicalizeLines(const std::string& text, const std::vector<LineSpan>& lines,
                                  std::size_t first, std::size_t count) {
    std::size_t common_indent = std::string::npos;
    std::size_t indent_line = first;
    for (std::size_t i = first; i < first + count; ++i) {
        const std::size_t trimmed_end = TrimmedLineEnd(text, lines[i]);
        if (trimmed_end == lines[i].start) {
            continue;
        }
        const std::size_t indent = LeadingWhitespace(text, lines[i]);
        if (common_indent == std::string::npos || indent < common_indent) {
            common_indent = indent;
            indent_line = i;
        }
    }
    if (common_indent == std::string::npos) {
        common_indent = 0;
    }

    CanonicalBlock out;
    out.indent = text.substr(lines[indent_line].start, common_indent);
    for (std::size_t i = first; i < first + count; ++i) {
        if (i > first) {
            out.text.push_back('\n');
        }
        const std::size_t trimmed_end = TrimmedLineEnd(text, lines[i]);
        const std::size_t begin = (std::min)(lines[i].start + common_indent, trimmed_end);
        out.text.append(text, begin, trimmed_end - begin);
    }
    return out;
}

std::string ReindentReplacement(const std::string& replacement, const std::string& indent,
                                 const std::string& line_ending) {
    const std::string normalized = NormalizeLineEndings(replacement);
    const std::vector<LineSpan> lines = SplitLines(normalized);
    if (lines.empty()) {
        return replacement;
    }
    const CanonicalBlock canonical = CanonicalizeLines(normalized, lines, 0, lines.size());
    std::string out;
    for (std::size_t i = 0; i < lines.size(); ++i) {
        if (i > 0) {
            out += line_ending;
        }
        const std::size_t end = TrimmedLineEnd(normalized, lines[i]);
        const std::size_t own_indent = LeadingWhitespace(normalized, lines[i]);
        const std::size_t strip = (std::min)(own_indent, canonical.indent.size());
        if (end > lines[i].start + strip) {
            out += indent;
            out.append(normalized, lines[i].start + strip, end - lines[i].start - strip);
        }
    }
    if (!normalized.empty() && normalized.back() == '\n') {
        out += line_ending;
    }
    return out;
}

std::string ReplaceRanges(std::string text, const std::vector<std::pair<std::size_t, std::size_t>>& ranges,
                          const std::string& replacement) {
    for (auto it = ranges.rbegin(); it != ranges.rend(); ++it) {
        text.replace(it->first, it->second - it->first, replacement);
    }
    return text;
}

std::string ReplaceRanges(std::string text, const std::vector<std::pair<std::size_t, std::size_t>>& ranges,
                          const std::vector<std::string>& replacements) {
    for (std::size_t i = ranges.size(); i-- > 0;) {
        text.replace(ranges[i].first, ranges[i].second - ranges[i].first, replacements[i]);
    }
    return text;
}

std::string FirstMeaningfulLine(const std::string& text) {
    const std::string normalized = NormalizeLineEndings(text);
    for (const LineSpan& line : SplitLines(normalized)) {
        const std::size_t end = TrimmedLineEnd(normalized, line);
        const std::size_t begin = line.start + LeadingWhitespace(normalized, line);
        if (end > begin) {
            const std::string value = normalized.substr(begin, end - begin);
            return value.size() > 120 ? value.substr(0, 120) + "..." : value;
        }
    }
    return {};
}

}  // namespace

std::string EditFileTool::name() const {
    return "edit_file";
}

std::string EditFileTool::description() const {
    // 文案在 src/prompts/tools/<语言>/edit_file.md,兜底是迁移前的原文。
    return ToolText("edit_file", "description",
                    "对已有文件做字符串替换:先精确匹配,失败后会有限兼容 CRLF/LF、统一缩进和行尾空白。"
                    "old_string 仍须唯一出现(除非把 replace_all 设成 true),多处候选绝不猜。"
                    "适合小范围、精准的改动,不适合整篇重写(整篇重写用 write_file)。执行前需要用户确认。");
}

nlohmann::json EditFileTool::input_schema() const {
    nlohmann::json schema = nlohmann::json::object();
    schema["type"] = "object";

    nlohmann::json properties = nlohmann::json::object();

    nlohmann::json path_prop = nlohmann::json::object();
    path_prop["type"] = "string";
    path_prop["description"] = ToolText("edit_file", "param.path", "要修改的文件路径,相对或绝对均可");
    properties["path"] = path_prop;

    nlohmann::json old_prop = nlohmann::json::object();
    old_prop["type"] = "string";
    old_prop["description"] =
        ToolText("edit_file", "param.old_string",
                 "要被替换掉的原文。优先逐字匹配,必要时兼容换行、统一缩进与行尾空白;仍须唯一命中");
    properties["old_string"] = old_prop;

    nlohmann::json new_prop = nlohmann::json::object();
    new_prop["type"] = "string";
    new_prop["description"] = ToolText("edit_file", "param.new_string", "替换成的新内容");
    properties["new_string"] = new_prop;

    nlohmann::json replace_all_prop = nlohmann::json::object();
    replace_all_prop["type"] = "boolean";
    replace_all_prop["description"] =
        ToolText("edit_file", "param.replace_all", "true 就把所有出现的地方都替换掉,不填默认 false(要求唯一命中)");
    properties["replace_all"] = replace_all_prop;

    schema["properties"] = properties;
    schema["required"] = nlohmann::json::array({"path", "old_string", "new_string"});

    return schema;
}

Tool::Result EditFileTool::execute(const nlohmann::json& input) {
    if (!input.contains("path") || !input.at("path").is_string()) {
        return {"缺少必填参数 path(字符串)", true};
    }
    if (!input.contains("old_string") || !input.at("old_string").is_string()) {
        return {"缺少必填参数 old_string(字符串)", true};
    }
    if (!input.contains("new_string") || !input.at("new_string").is_string()) {
        return {"缺少必填参数 new_string(字符串)", true};
    }
    const std::string path_str = input.at("path").get<std::string>();
    if (path_str.empty()) {
        return {"path 不能是空字符串", true};
    }
    const std::string old_string = input.at("old_string").get<std::string>();
    const std::string new_string = input.at("new_string").get<std::string>();
    if (old_string.empty()) {
        return {"old_string 不能是空字符串", true};
    }

    bool replace_all = false;
    if (auto it = input.find("replace_all"); it != input.end() && !it->is_null()) {
        if (!it->is_boolean()) {
            return {"replace_all 得是布尔值(true/false)", true};
        }
        replace_all = it->get<bool>();
    }

    // 隔离文件闸:住在 worktree 房里的会话/子代理,写主 checkout 的文件一律拦。
    if (const IsolationScope* scope = IsolationGuard::Current();
        scope != nullptr && PathBlockedByIsolation(path_str, *scope)) {
        return {"[隔离] 会话正住在 worktree " + scope->name + " 里,不许改主 checkout 的文件: " + path_str +
                    "。请在房内操作,或先 worktree exit 出房。",
                true};
    }

    const std::filesystem::path path = Utf8ToPath(path_str);

    std::error_code ec;
    if (!std::filesystem::exists(path, ec)) {
        return {"文件不存在: " + path_str, true};
    }
    if (std::filesystem::is_directory(path, ec)) {
        return {"这是个目录,不是文件: " + path_str, true};
    }

    std::ifstream in(path, std::ios::binary);
    if (!in.is_open()) {
        return {"打不开文件(权限不够或者被占用): " + path_str, true};
    }
    std::ostringstream buf;
    buf << in.rdbuf();
    const std::string original = buf.str();
    in.close();

    std::size_t occurrences = CountOccurrences(original, old_string);
    std::string updated;
    std::string match_mode = "精确";
    bool used_fuzzy_match = false;

    if (occurrences == 0) {
        // 第一层容错:只统一 CRLF/LF/CR，字面内容仍须完全一致。
        const NormalizedText normalized_file = NormalizeLineEndingsWithOffsets(original);
        const std::string normalized_old = NormalizeLineEndings(old_string);
        std::vector<std::pair<std::size_t, std::size_t>> ranges;
        for (std::size_t pos = 0; !normalized_old.empty() &&
                                  (pos = normalized_file.text.find(normalized_old, pos)) != std::string::npos;) {
            ranges.emplace_back(normalized_file.original_offsets[pos],
                                normalized_file.original_offsets[pos + normalized_old.size()]);
            pos += normalized_old.size();
        }
        if (!ranges.empty()) {
            occurrences = ranges.size();
            if (!replace_all && occurrences > 1) {
                return {"old_string 统一换行后在文件里出现了 " + std::to_string(occurrences) +
                            " 次,仍不唯一。请带上更多前后文后再试。",
                        true};
            }
            if (!replace_all) {
                ranges.resize(1);
            }
            updated = ReplaceRanges(original, ranges, UseLineEnding(new_string, FileLineEnding(original)));
            match_mode = "换行归一";
            used_fuzzy_match = true;
        }
    }

    if (occurrences == 0) {
        // 第二层容错:按完整行块比对，忽略统一外层缩进与行尾空白。
        const NormalizedText normalized_file = NormalizeLineEndingsWithOffsets(original);
        const std::string normalized_old = NormalizeLineEndings(old_string);
        const std::vector<LineSpan> file_lines = SplitLines(normalized_file.text);
        const std::vector<LineSpan> old_lines = SplitLines(normalized_old);
        std::vector<std::pair<std::size_t, std::size_t>> ranges;
        std::vector<std::string> replacements;
        std::vector<std::size_t> line_numbers;
        if (!old_lines.empty() && old_lines.size() <= file_lines.size()) {
            const CanonicalBlock wanted = CanonicalizeLines(normalized_old, old_lines, 0, old_lines.size());
            const bool old_ends_newline = !normalized_old.empty() && normalized_old.back() == '\n';
            for (std::size_t i = 0; i + old_lines.size() <= file_lines.size();) {
                const CanonicalBlock candidate = CanonicalizeLines(normalized_file.text, file_lines, i, old_lines.size());
                if (candidate.text == wanted.text) {
                    const std::size_t normalized_end = old_ends_newline
                                                           ? file_lines[i + old_lines.size() - 1].end
                                                           : file_lines[i + old_lines.size() - 1].content_end;
                    ranges.emplace_back(normalized_file.original_offsets[file_lines[i].start],
                                        normalized_file.original_offsets[normalized_end]);
                    replacements.push_back(
                        ReindentReplacement(new_string, candidate.indent, FileLineEnding(original)));
                    line_numbers.push_back(i + 1);
                    i += old_lines.size();
                } else {
                    ++i;
                }
            }
        }
        occurrences = ranges.size();
        if (occurrences > 0) {
            if (!replace_all && occurrences > 1) {
                std::string where;
                for (std::size_t i = 0; i < line_numbers.size() && i < 5; ++i) {
                    where += (i == 0 ? "" : ", ") + std::to_string(line_numbers[i]);
                }
                return {"old_string 宽松匹配到 " + std::to_string(occurrences) + " 处(起始行 " + where +
                            "),不唯一。请补足前后文,不自动猜。",
                        true};
            }
            if (!replace_all) {
                ranges.resize(1);
                replacements.resize(1);
            }
            updated = ReplaceRanges(original, ranges, replacements);
            match_mode = "缩进/行尾空白归一";
            used_fuzzy_match = true;
        }
    }

    if (occurrences == 0) {
        const std::string first_line = FirstMeaningfulLine(old_string);
        return {"文件里找不到 old_string(精确、换行归一、缩进/行尾空白归一均未命中): " + path_str +
                    (first_line.empty() ? std::string() : "\nold_string 首行: " + first_line) +
                    "\n文件可能已经变化。请先 read_file 读取最新片段,再用最新原文重试；不要原样重复调用。",
                true};
    }
    if (!replace_all && occurrences > 1) {
        return {"old_string 在文件里出现了 " + std::to_string(occurrences) +
                     " 次,不唯一,没法确定该改哪一处。要么把 old_string 写得更具体(带上前后文),"
                     "要么显式传 replace_all=true 把所有出现的地方都换掉。",
                true};
    }

    std::size_t replaced_count = 0;
    if (used_fuzzy_match) {
        replaced_count = replace_all ? occurrences : 1;
    } else if (replace_all) {
        updated = ReplaceAllOccurrences(original, old_string, new_string);
        replaced_count = occurrences;
    } else {
        const std::size_t pos = original.find(old_string);
        updated = original;
        updated.replace(pos, old_string.size(), new_string);
        replaced_count = 1;
    }

    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out.is_open()) {
        return {"打不开文件写(权限不够或者被占用): " + path_str, true};
    }
    out.write(updated.data(), static_cast<std::streamsize>(updated.size()));
    if (!out) {
        return {"写回文件失败: " + path_str, true};
    }
    out.close();

    // 逐枚追踪单:undo token(original 是 preimage,updated 是 postimage;
    // 条件式撤销按这对哈希判"其后没人再改")。
    Tool::Result result;
    result.content = match_mode + "匹配,替换了 " + std::to_string(replaced_count) + " 处: " + path_str;
    result.undo_path = path_str;
    result.undo_preimage_sha256 = hooks::Sha256Hex(original);
    result.undo_postimage_sha256 = hooks::Sha256Hex(updated);
    result.undo_created_new_file = false;
    if (static_cast<std::uint64_t>(original.size()) <= Tool::kToolUndoPreimageCap) {
        result.undo_preimage = original;
    }
    result.effect_summary = "edit " + path_str + " (" + std::to_string(replaced_count) + " 处)";
    return result;
}

}  // namespace lubancode::tools
