#include "cli/transcript.hpp"

#include <cstdio>
#include <map>
#include <string_view>

#include "cli/i18n.hpp"
#include "cli/line_editor.hpp"  // TruncateUtf8ToDisplayWidth
#include "cli/markdown.hpp"

namespace lubancode::cli {

namespace {

constexpr const char* kElbow = "\xE2\x8E\xBF";  // ⎿ U+23BF
constexpr const char* kDot = "\xE2\x97\x8F";    // ● U+25CF

// 状态灯颜色:只染灯,不染正文——Running/Pending 黄(tool_line),成功绿
// (prompt),失败红(error),拒绝灰(stats),打断灰黄(dim + tool_line)。
std::string StatusColor(TranscriptStatus status, const Theme& theme) {
    switch (status) {
        case TranscriptStatus::Pending:
        case TranscriptStatus::Running:
            return theme.tool_line;
        case TranscriptStatus::Ok:
            return theme.prompt;
        case TranscriptStatus::Error:
            return theme.error;
        case TranscriptStatus::Cancelled:
            return theme.stats;
        case TranscriptStatus::Interrupted:
            return "\x1b[2m" + theme.tool_line;
    }
    return std::string();
}

// 把文本按 \n 拆成行(丢掉行尾 \n;末尾没有 \n 的最后一截也算一行)。
std::vector<std::string> SplitLines(const std::string& text) {
    std::vector<std::string> lines;
    std::size_t pos = 0;
    while (pos < text.size()) {
        const std::size_t nl = text.find('\n', pos);
        if (nl == std::string::npos) {
            lines.push_back(text.substr(pos));
            break;
        }
        lines.push_back(text.substr(pos, nl - pos));
        pos = nl + 1;
    }
    return lines;
}

// 超宽截断 + "..."。已在宽度内就原样返回;max_width 太小放不下省略号时
// 直接给 "..." 的能放下的部分。
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

std::string FormatSeconds(double seconds) {
    if (seconds < 0.0) {
        seconds = 0.0;
    }
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.1fs", seconds);
    return std::string(buf);
}

}  // namespace

std::string TranscriptStatusWord(TranscriptStatus status) {
    switch (status) {
        case TranscriptStatus::Pending:
            return "[CONFIRM]";
        case TranscriptStatus::Running:
            return "[RUNNING]";
        case TranscriptStatus::Ok:
            return "[OK]";
        case TranscriptStatus::Error:
            return "[ERROR]";
        case TranscriptStatus::Cancelled:
            return "[CANCELLED]";
        case TranscriptStatus::Interrupted:
            return "[INTERRUPTED]";
    }
    return "[?]";
}

std::string FormatTranscriptItem(const TranscriptItem& item, const Theme& theme, int width,
                                  bool expanded, bool focused) {
    const bool plain = theme.reset.empty();
    const std::string indent = item.kind == TranscriptKind::SubTool ? "    " : "";
    const int indent_cols = static_cast<int>(indent.size());
    // UI-D:焦点标记 "► "(U+25BA + 空格,显示宽度按 2 算——► 在多数终端
    // 占一列,加空格共两列),只加在首行行首、缩进之前,一眼扫得到。
    const std::string focus_mark = focused ? "\xE2\x96\xBA " : "";
    const int focus_cols = focused ? 2 : 0;

    std::string out;

    // 首行:[焦点标记 +] 状态灯 + 工具名(参数摘要),超宽按显示宽度截断加 "..."。
    {
        std::string prefix;
        int prefix_cols = indent_cols + focus_cols;
        if (plain) {
            const std::string word = TranscriptStatusWord(item.status);
            prefix = focus_mark + indent + word + " ";
            prefix_cols += static_cast<int>(word.size()) + 1;
        } else {
            prefix = focus_mark + indent + StatusColor(item.status, theme) + kDot + theme.reset + " ";
            prefix_cols += 2;  // ● 一列 + 空格一列
        }
        std::string title = item.title;
        if (width > 0) {
            title = TruncateWithEllipsis(title, width - prefix_cols - 1);
        }
        out += prefix + title + "\n";
    }

    // 次行起:缩进两空格 "⎿ " 开头,续行再缩两空格。摘要行也按宽度截断
    // (物理折行会毁掉原地改写的行数记账),但夹着 ANSI 序列的行(todo
    // 清单自带颜色)不截——按显示宽截会把转义序列剪碎,这类行本来就短。
    for (std::size_t i = 0; i < item.summary_lines.size(); ++i) {
        const std::string prefix = indent + (i == 0 ? std::string("  ") + kElbow + " " : std::string("    "));
        const int prefix_cols = indent_cols + 4;
        std::string line = item.summary_lines[i];
        if (width > 0 && line.find('\x1b') == std::string::npos) {
            line = TruncateWithEllipsis(line, width - prefix_cols - 1);
        }
        out += prefix + line + "\n";
    }

    // UI-D 展开版:完整入参 JSON 一行 + full_output 全文(标题行 + 每行缩进)。
    // 截断规则跟摘要行一致:width > 0 且不夹 ANSI 才截——展开版可能被
    // TranscriptPainter 原地重画,物理折行同样会毁掉行数记账。
    if (expanded) {
        const std::string body_indent = indent + "  ";
        const int body_cols = indent_cols + 2;
        if (!item.input_json.empty()) {
            std::string param_line = tr("transcript.params_prefix") + item.input_json;
            if (width > 0) {
                param_line = TruncateWithEllipsis(param_line, width - body_cols - 1);
            }
            out += body_indent + param_line + "\n";
        }
        if (item.full_output.empty()) {
            out += body_indent + tr("transcript.no_full_output") + "\n";
        } else {
            out += body_indent + trf("transcript.full_output_header", CountLines(item.full_output)) + "\n";
            for (std::string& line : SplitLines(item.full_output)) {
                if (width > 0 && line.find('\x1b') == std::string::npos) {
                    line = TruncateWithEllipsis(line, width - body_cols - 1);
                }
                out += body_indent + line + "\n";
            }
        }
    }

    return out;
}

std::string FormatTranscriptItems(const std::vector<TranscriptItem>& items, const Theme& theme,
                                  int width, bool expanded, int focus_index) {
    std::string out;
    for (std::size_t i = 0; i < items.size(); ++i) {
        if (!expanded && items[i].kind == TranscriptKind::SubTool) {
            continue;
        }
        out += FormatTranscriptItem(items[i], theme, width, expanded,
                                    static_cast<int>(i) == focus_index);
    }
    return out;
}

std::string BuildToolTitle(const std::string& name, const nlohmann::json& input) {
    std::string arg;
    if (name == "run_command") {
        arg = input.value("command", std::string());
    } else if (name == "read_file" || name == "write_file" || name == "edit_file") {
        arg = input.value("path", std::string());
    } else if (name == "agent") {
        arg = TruncateUtf8Codepoints(input.value("prompt", std::string()), 40);
    } else if (name == "ask_user") {
        if (const auto questions = input.find("questions"); questions != input.end() && questions->is_array() &&
            !questions->empty() && (*questions)[0].is_object()) {
            arg = TruncateUtf8Codepoints((*questions)[0].value("question", std::string()), 40);
        }
    } else if (name == "web_search") {
        arg = input.value("query", std::string());
        if (arg.empty()) {
            if (const auto queries = input.find("queries"); queries != input.end() && queries->is_array()) {
                arg = std::to_string(queries->size()) + " queries";
            }
        }
    } else if (name == "todo_write" || name == "todo_update") {
        std::size_t count = 0;
        if (const auto it = input.find("items"); it != input.end() && it->is_array()) {
            count = it->size();
        }
        arg = trf("transcript.todo_count", count);
    } else {
        // MCP 工具(mcp__server__tool)和其余工具:入参紧凑 JSON。
        arg = input.is_null() ? std::string() : input.dump();
    }
    // 参数里的换行会把首行拆成多行、毁掉行数记账,压成空格。
    for (char& c : arg) {
        if (c == '\n' || c == '\r' || c == '\t') {
            c = ' ';
        }
    }
    return name + "(" + arg + ")";
}

std::string FormatRestoredHistory(const std::vector<api::Message>& messages, const Theme& theme,
                                  int width, const std::vector<std::size_t>& compact_positions) {
    std::map<std::string, const api::ToolResultBlock*> results;
    for (const auto& message : messages) {
        for (const auto& block : message.content) {
            if (const auto* result = std::get_if<api::ToolResultBlock>(&block)) {
                results[result->tool_use_id] = result;
            }
        }
    }

    std::string out;
    const auto separate = [&out] {
        if (!out.empty() && out.back() != '\n') {
            out += '\n';
        }
        if (!out.empty() && (out.size() < 2 || out[out.size() - 2] != '\n')) {
            out += '\n';
        }
    };
    const auto append_markdown = [&](const std::string& text) {
        const auto lines = RenderMarkdown(text, theme, width);
        for (const std::string& line : lines) {
            out += line + "\n";
        }
    };

    std::size_t next_compact = 0;
    const auto emit_compact_notes = [&](std::size_t message_index) {
        while (next_compact < compact_positions.size() && compact_positions[next_compact] <= message_index) {
            separate();
            out += theme.stats + tr("cmd.resume.history.compact") + theme.reset + "\n";
            ++next_compact;
        }
    };

    for (std::size_t mi = 0; mi < messages.size(); ++mi) {
        const auto& message = messages[mi];
        emit_compact_notes(mi);
        bool has_visible_content = false;
        for (const auto& block : message.content) {
            has_visible_content = has_visible_content || std::holds_alternative<api::TextBlock>(block) ||
                                  std::holds_alternative<api::ImageBlock>(block) ||
                                  std::holds_alternative<api::ToolUseBlock>(block);
        }
        if (!has_visible_content) {
            continue;
        }

        separate();
        const bool assistant = message.role == api::Role::Assistant;
        out += (assistant ? theme.banner + "● " + tr("cmd.resume.history.assistant")
                          : theme.confirm + "> " + tr("cmd.resume.history.user")) +
               theme.reset + "\n";
        for (const auto& block : message.content) {
            if (const auto* text = std::get_if<api::TextBlock>(&block)) {
                append_markdown(text->text);
                continue;
            }
            if (const auto* image = std::get_if<api::ImageBlock>(&block)) {
                out += trf("cmd.resume.history.image", image->filename, image->width, image->height) + "\n";
                continue;
            }
            const auto* use = std::get_if<api::ToolUseBlock>(&block);
            if (use == nullptr) {
                continue;
            }
            TranscriptItem item;
            item.tool_name = use->name;
            item.title = BuildToolTitle(use->name, use->input);
            item.input_json = use->input.dump();
            const auto found = results.find(use->id);
            if (found == results.end()) {
                item.status = TranscriptStatus::Error;
                item.summary_lines = {tr("cmd.resume.history.tool_missing")};
            } else {
                const api::ToolResultBlock& result = *found->second;
                item.status = result.is_error ? TranscriptStatus::Error : TranscriptStatus::Ok;
                std::string first_line = result.content.substr(0, result.content.find('\n'));
                if (first_line.empty()) {
                    first_line = result.is_error ? tr("cmd.resume.history.tool_error")
                                                 : tr("cmd.resume.history.tool_done");
                }
                const int line_count = CountLines(result.content);
                if (line_count > 1) {
                    first_line += trf("cmd.resume.history.tool_more", line_count - 1);
                }
                item.summary_lines = {std::move(first_line)};
                item.full_output = TruncateUtf8Bytes(result.content, kFullOutputCapBytes);
            }
            out += FormatTranscriptItem(item, theme, width);
        }
    }
    emit_compact_notes(messages.size());
    return out;
}

int CountLines(const std::string& text) {
    if (text.empty()) {
        return 0;
    }
    int count = 0;
    for (const char c : text) {
        if (c == '\n') {
            ++count;
        }
    }
    if (text.back() != '\n') {
        ++count;
    }
    return count;
}

std::optional<int> ParseRunCommandExitCode(const std::string& content) {
    constexpr std::string_view prefix = "[退出码 ";
    if (content.compare(0, prefix.size(), prefix) != 0) {
        return std::nullopt;
    }
    std::size_t pos = prefix.size();
    bool negative = false;
    if (pos < content.size() && content[pos] == '-') {
        negative = true;
        ++pos;
    }
    if (pos >= content.size() || content[pos] < '0' || content[pos] > '9') {
        return std::nullopt;
    }
    long long value = 0;
    while (pos < content.size() && content[pos] >= '0' && content[pos] <= '9') {
        value = value * 10 + (content[pos] - '0');
        ++pos;
    }
    if (pos >= content.size() || content[pos] != ']') {
        return std::nullopt;
    }
    return static_cast<int>(negative ? -value : value);
}

std::string RunCommandDoneSummary(const std::string& content, double seconds) {
    std::string out = "Done";
    if (const auto code = ParseRunCommandExitCode(content); code.has_value()) {
        out += " · " + trf("transcript.exit_code", *code);
    }
    out += " · " + FormatSeconds(seconds);
    return out;
}

std::string ReadFileDoneSummary(const std::string& content) {
    constexpr std::string_view kEmptyFile = "(空文件)";  // 跟 read_file.cpp 的原文逐字节一致
    if (content.compare(0, kEmptyFile.size(), kEmptyFile) == 0) {
        return trf("transcript.read_lines", 0);
    }
    return trf("transcript.read_lines", CountLines(content));
}

std::string WriteDiffSummary(int added_lines, std::optional<int> removed_lines) {
    if (removed_lines.has_value()) {
        return trf("transcript.added_removed", added_lines, *removed_lines);
    }
    return trf("transcript.added", added_lines);
}

std::string SearchDoneSummary(const std::string& content) {
    int hits = 0;
    if (content != "没搜到匹配的内容" && content != "没找到匹配的文件") {
        for (const std::string& line : SplitLines(content)) {
            if (line.compare(0, 6, "\xE2\x80\xA6\xE2\x80\xA6") == 0) {
                continue;  // "……(结果超过 N 条,已截断……)" 提示行不算命中
            }
            if (!line.empty()) {
                ++hits;
            }
        }
    }
    return trf("transcript.hits", hits);
}

std::string AgentDoneSummary(int rounds, int sub_tool_calls) {
    return trf("transcript.agent", rounds, sub_tool_calls);
}

std::vector<std::string> ErrorSummaryLines(const std::string& tool_name, const std::string& content) {
    std::vector<std::string> lines = SplitLines(content);
    const int total = static_cast<int>(lines.size());
    std::vector<std::string> out;

    if (lines.empty()) {
        out.push_back(tr("transcript.error_no_output"));
        return out;
    }

    // run_command 的失败结果开头是 "[退出码 N]",首行改写成人话。
    if (tool_name == "run_command") {
        if (const auto code = ParseRunCommandExitCode(content); code.has_value()) {
            out.push_back(trf("transcript.error_exit_code", *code));
        } else {
            out.push_back("Error: " + lines[0]);
        }
    } else {
        out.push_back("Error: " + lines[0]);
    }

    for (std::size_t i = 1; i < lines.size() && out.size() < 5; ++i) {
        out.push_back(lines[i]);
    }
    if (total > 5) {
        out.push_back(trf("transcript.error_truncated", total));
    }
    return out;
}

std::string TruncateUtf8Bytes(const std::string& text, std::size_t max_bytes) {
    if (text.size() <= max_bytes) {
        return text;
    }
    std::size_t cut = max_bytes;
    // 别劈开多字节字符:落在续字节(10xxxxxx)上就往前退。
    while (cut > 0 && (static_cast<unsigned char>(text[cut]) & 0xC0) == 0x80) {
        --cut;
    }
    return text.substr(0, cut);
}

std::string TruncateUtf8Codepoints(const std::string& text, std::size_t max_codepoints) {
    std::size_t count = 0;
    std::size_t pos = 0;
    while (pos < text.size() && count < max_codepoints) {
        const unsigned char byte = static_cast<unsigned char>(text[pos]);
        std::size_t len = 1;
        if ((byte & 0xF8) == 0xF0) {
            len = 4;
        } else if ((byte & 0xF0) == 0xE0) {
            len = 3;
        } else if ((byte & 0xE0) == 0xC0) {
            len = 2;
        }
        pos += len;
        ++count;
    }
    if (pos >= text.size()) {
        return text;
    }
    return text.substr(0, pos) + "...";
}

}  // namespace lubancode::cli
