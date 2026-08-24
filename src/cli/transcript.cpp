#include "cli/transcript.hpp"

#include <cstdio>
#include <map>
#include <string_view>

#include "cli/i18n.hpp"
#include "cli/line_editor.hpp"  // TruncateUtf8ToDisplayWidth / WrapToDisplayWidth / DisplayWidth
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
        case TranscriptStatus::Blocked:
            return theme.error;  // 拦下用失败色,但措辞是"未执行",不冒充跑过
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

// read_file 的正文行固定是“右对齐行号<Tab>正文”。结果末尾还可能跟一行
// 截断提示，不能拿普通文本总行数冒充真正读到的源码行数。
std::size_t CountReadFileSourceLines(const std::string& content) {
    std::size_t count = 0;
    for (const std::string& line : SplitLines(content)) {
        std::size_t pos = 0;
        while (pos < line.size() && line[pos] == ' ') {
            ++pos;
        }
        const std::size_t digits_begin = pos;
        while (pos < line.size() && line[pos] >= '0' && line[pos] <= '9') {
            ++pos;
        }
        if (pos > digits_begin && pos < line.size() && line[pos] == '\t') {
            ++count;
        }
    }
    return count;
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

}  // namespace

std::string FormatSeconds(double seconds) {
    if (seconds < 0.0) {
        seconds = 0.0;
    }
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.1fs", seconds);
    return std::string(buf);
}

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
        case TranscriptStatus::Blocked:
            return "[BLOCKED]";
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
        // 思考条目展开档:标题补「· N 字」,正文多长一眼有数。紧凑档不
        // 加,收定时保持一行「思考 Xs」。
        if (expanded && item.kind == TranscriptKind::Thinking && !item.full_output.empty()) {
            title += trf("transcript.thinking_chars", CountUtf8Codepoints(item.full_output));
        }
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
        // 思考进行中(Running):已到正文随 Ctrl+O 快照摊开,但只铺约一屏,
        // 超了截断收口「共 N 行,结束后 Ctrl+O 看全文」——思考收定后条目
        // 变非 Running,再 Ctrl+O 就是全文。正文一个字没到时连占位行都
        // 不铺,免得刚起头的思考块顶着一行"(无完整输出)"。
        const bool thinking_live =
            item.kind == TranscriptKind::Thinking && item.status == TranscriptStatus::Running;
        if (thinking_live && item.full_output.empty()) {
            // 什么都不铺:标题行("思考中…")就是这块的全部。
        } else if (item.full_output.empty()) {
            out += body_indent + tr("transcript.no_full_output") + "\n";
        } else {
            const std::vector<std::string> lines = SplitLines(item.full_output);
            const bool truncated =
                thinking_live && static_cast<int>(lines.size()) > kThinkingStreamExpandedMaxLines;
            const std::size_t print_upto =
                truncated ? static_cast<std::size_t>(kThinkingStreamExpandedMaxLines) : lines.size();
            out += body_indent + trf("transcript.full_output_header", CountLines(item.full_output)) + "\n";
            for (std::size_t i = 0; i < print_upto; ++i) {
                std::string line = lines[i];
                if (width > 0 && line.find('\x1b') == std::string::npos) {
                    line = TruncateWithEllipsis(line, width - body_cols - 1);
                }
                out += body_indent + line + "\n";
            }
            if (truncated) {
                out += body_indent + trf("transcript.thinking_stream_more",
                                         static_cast<int>(lines.size())) +
                       "\n";
            }
        }
    }

    return out;
}

std::string FormatTranscriptItems(const std::vector<TranscriptItem>& items, const Theme& theme,
                                  int width, bool expanded, int focus_index, int expanded_index) {
    std::string out;
    for (std::size_t i = 0; i < items.size(); ++i) {
        const bool item_expanded = expanded || static_cast<int>(i) == expanded_index;
        if (!item_expanded && items[i].kind == TranscriptKind::SubTool) {
            continue;
        }
        out += FormatTranscriptItem(items[i], theme, width, item_expanded,
                                    static_cast<int>(i) == focus_index);
    }
    return out;
}

std::string BuildToolTitle(const std::string& name, const nlohmann::json& input) {
    std::string arg;
    if (name == "run_command") {
        // 回合视觉收束单第四节:多行命令只取首个非空逻辑行,末尾加
        // "+N lines"——不能把整段 PowerShell 横铺一百多列。N 是"首行之
        // 外还有几行"(空行计入,如实报)。完整脚本在 Ctrl+O 展开版与
        // input_json 里,一个字不丢。
        const std::string command = input.value("command", std::string());
        const std::vector<std::string> lines = SplitLines(command);
        std::size_t first = 0;
        while (first < lines.size() &&
               lines[first].find_first_not_of(" \t\r") == std::string::npos) {
            ++first;  // 跳过前导空行/纯空白行(首行是"逻辑行",空行不算数)
        }
        if (first < lines.size()) {
            arg = lines[first];
            const std::size_t rest = lines.size() - first - 1;
            if (rest > 0) {
                arg += trf("transcript.more_lines", static_cast<int>(rest));
            }
        }
    } else if (name == "read_file") {
        arg = input.value("path", std::string());
        for (const char* key : {"offset", "limit"}) {
            const auto value = input.find(key);
            if (value != input.end() && value->is_number_integer()) {
                arg += ", " + std::string(key) + "=" + std::to_string(value->get<long long>());
            }
        }
    } else if (name == "write_file" || name == "edit_file") {
        arg = input.value("path", std::string());
    } else if (name == "agent") {
        // 工具条目标题只认真正短 title(入参必填);不拿 prompt 片段冒充。
        arg = input.value("title", std::string());
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
        if (assistant) {
            out += theme.banner + "● " + tr("cmd.resume.history.assistant") + theme.reset + "\n";
        } else {
            // 用户消息:不再印一行 "> 你" 标头,正文直接铺成背景块——与
            // live 提交、Ctrl+L 重画同一颗 formatter(单子"live、恢复、重画
            // 必须同源")。只含工具结果的 user 消息不会走到这里(上面
            // has_visible_content 已挡);真用户文本逐块铺,多块之间照旧
            // 由 Markdown 自己排。
            for (const auto& block : message.content) {
                if (const auto* text = std::get_if<api::TextBlock>(&block)) {
                    out += FormatUserPromptBlock(text->text, theme, width);
                }
            }
        }
        for (const auto& block : message.content) {
            if (const auto* text = std::get_if<api::TextBlock>(&block)) {
                if (!assistant) {
                    continue;  // 用户文本已在上面铺成背景块,不另走 Markdown
                }
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

UserPromptLayout LayoutUserPromptBlock(const std::string& text, const Theme& theme, int width) {
    UserPromptLayout layout;
    const int safe_width = width > 0 ? width : 80;
    // 窄屏(<20 列)先保提示符与正文,舍左右 padding(单子 corner case 节)。
    const int padding = safe_width < 20 ? 0 : kUserPromptPadding;
    // 色面铺到安全宽:末列留一格,不在最后一列写字符再触发隐式 wrap、把
    // 背景漏到下一行(单子:最后一列不能写字符后再触发隐式 wrap)。
    const int block_width = safe_width - 1;
    if (block_width <= 0) {
        return layout;
    }
    // 空白 prompt 不生成空色块:只有空白(全空白算没提交)就一片 rows 都不给。
    bool has_content = false;
    for (const char c : text) {
        if (c != ' ' && c != '\t' && c != '\n' && c != '\r') {
            has_content = true;
            break;
        }
    }
    if (!has_content) {
        return layout;
    }
    const int content_width = (std::max)(1, block_width - padding * 2 - kUserPromptMarkerWidth);

    // 逻辑行 -> 折行:首行容提示符,续行缩进(与 composer 续行同款两格)。
    // WrapUtf8ToDisplayWidth 按码点宽度断(CJK/emoji 友好,不切半个宽字)。
    std::vector<std::u32string> logical;
    {
        std::size_t pos = 0;
        while (pos <= text.size()) {
            const std::size_t nl = text.find('\n', pos);
            const std::size_t end = nl == std::string::npos ? text.size() : nl;
            logical.push_back(Utf8ToUtf32(text.substr(pos, end - pos)));
            if (nl == std::string::npos) {
                break;
            }
            pos = nl + 1;
        }
    }

    const std::string pad(padding, ' ');
    const std::string indent(kUserPromptIndent, ' ');
    auto push_row = [&](const std::string& body, int body_cols, bool first) {
        // 每行:bg + 左 padding + 提示符/缩进 + 正文 + 右侧补到 block_width。
        // 背景 ANSI 每行开、每行关——不靠软换行把背景带到下一行。
        // plain 主题(surface_* 全空串)自动退化成 "> 正文" / "  正文" 的裸
        // 文本,右侧不补空格(尾部空格只会给重定向文件添噪音)。
        const bool plain = theme.surface_user_bg.empty() && theme.surface_padding.empty();
        const int lead_cols = padding + (first ? kUserPromptMarkerWidth : kUserPromptIndent);
        const int fill_cols = (std::max)(0, block_width - lead_cols - body_cols - padding);
        std::string line;
        if (!plain) {
            line = theme.surface_user_bg + pad;
        }
        if (first) {
            line += theme.surface_user_marker + "> " + theme.reset;
            if (!plain) {
                line += theme.surface_user_bg;
            }
        } else {
            line += indent;
        }
        line += theme.surface_user_fg + body + theme.reset;
        if (!plain) {
            line += theme.surface_padding + std::string(fill_cols + padding, ' ') + theme.reset;
        }
        layout.rows.push_back(UserPromptRow{std::move(line), block_width});
    };

    bool first_logical = true;
    for (const std::u32string& logical_line : logical) {
        // 空逻辑行也占一行色面(用户自己敲的空行,不是块外 gap)。
        if (logical_line.empty()) {
            push_row(std::string(), 0, first_logical);
            first_logical = false;
            continue;
        }
        // 首行容提示符("> " 两列),续行容缩进(同为两列)——两条容量一致,
        // 折行宽度统一走 content_width,与 composer 的首行/续行同宽规矩对齐。
        const std::vector<std::u32string> wrapped = WrapToDisplayWidth(logical_line, content_width);
        for (std::size_t i = 0; i < wrapped.size(); ++i) {
            push_row(Utf32ToUtf8(wrapped[i]),
                     static_cast<int>(DisplayWidth(wrapped[i])), first_logical && i == 0);
        }
        first_logical = false;
    }
    layout.block_width = block_width;
    layout.content_width = content_width;
    return layout;
}

std::string FormatUserPromptBlock(const std::string& text, const Theme& theme, int width) {
    const UserPromptLayout layout = LayoutUserPromptBlock(text, theme, width);
    std::string out;
    for (const UserPromptRow& row : layout.rows) {
        out += row.text;
        out += "\n";
    }
    return out;
}

int GapBetween(BlockRole before, BlockRole after) {
    // 子项贴父项、同父批次紧排:0。
    if (before == BlockRole::Tool && after == BlockRole::SubTool) {
        return 0;
    }
    if (before == BlockRole::SubTool && after == BlockRole::SubTool) {
        return 0;
    }
    // 表外一律 1:UserPrompt -> 任意、Tool -> Tool、Tool -> AssistantText、
    // AssistantText -> Tool、Warning/Error 不黏正文、任意 -> TurnFooter。
    (void)before;
    (void)after;
    return 1;
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
    return trf("transcript.read_lines", CountReadFileSourceLines(content));
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

std::string AgentDoneSummary(int step_count, int tool_call_count) {
    return trf("transcript.agent", step_count, tool_call_count);
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

int CountUtf8Codepoints(const std::string& text) {
    int count = 0;
    std::size_t pos = 0;
    while (pos < text.size()) {
        const unsigned char byte = static_cast<unsigned char>(text[pos]);
        if ((byte & 0xF8) == 0xF0) {
            pos += 4;
        } else if ((byte & 0xF0) == 0xE0) {
            pos += 3;
        } else if ((byte & 0xE0) == 0xC0) {
            pos += 2;
        } else {
            pos += 1;
        }
        ++count;
    }
    return count;
}

}  // namespace lubancode::cli
