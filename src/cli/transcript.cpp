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

// 思考露尾排版的家伙什(见 ThinkingPreviewRows)。
// 预览正文的一个码点要不要留:C0 控制字符(含 ESC)与 DEL 全剔——
// provider 传来的转义不能改色、挪光标、清屏;TAB 折成一空格,其余可见
// 码点照留。ESC 序列整段跳过由 SanitizePreviewLine 管(这里只看单点)。
inline bool PreviewKeepsCodepoint(char32_t cp) {
    if (cp == U'\t') {
        return true;
    }
    return cp >= 0x20 && cp != 0x7F;
}

// 剥 ANSI/控制字符:ESC 起的转义序列(CSI 参数串到终结字节为止)整段丢,
// 其余控制码点剔除、TAB 折空格。产出可直接折行的纯可见文本。
inline std::u32string SanitizePreviewLine(const std::u32string& line) {
    std::u32string out;
    out.reserve(line.size());
    bool in_escape = false;
    for (const char32_t cp : line) {
        if (in_escape) {
            // CSI/简单转义的终结字节是 0x40~0x7E(@..~);到了就收班。
            if (cp >= 0x40 && cp <= 0x7E) {
                in_escape = false;
            }
            continue;
        }
        if (cp == 0x1B) {
            in_escape = true;
            continue;
        }
        if (!PreviewKeepsCodepoint(cp)) {
            continue;
        }
        out.push_back(cp == U'\t' ? U' ' : cp);
    }
    return out;
}

// 一条(已清洗的)逻辑行折成视觉行;巨长行先掐头(露尾只要末几行,头
// 部折了也是白折):显示宽超过 reserve_cols 就从头上削到剩余约
// reserve_cols 列。削在码点边界(u32string 天然安全),O(行长) 一次。
inline std::vector<std::u32string> WrapPreviewLine(const std::u32string& line, int wrap_width, int reserve_cols) {
    if (static_cast<int>(DisplayWidth(line)) <= reserve_cols) {
        return WrapToDisplayWidth(line, wrap_width);
    }
    const std::size_t total = DisplayWidth(line);
    const std::size_t keep = static_cast<std::size_t>(reserve_cols);
    std::size_t seen = 0;
    std::size_t cut = 0;
    while (cut < line.size() && seen + keep < total) {
        seen += static_cast<std::size_t>(CharDisplayWidth(line[cut]));
        ++cut;
    }
    return WrapToDisplayWidth(line.substr(cut), wrap_width);
}

}  // namespace

// ---- 思考流中预览:状态机转移(纯函数,单测主战场) ----------------------
//
// 转移表照单上"状态机"一节逐条落;表外的组合原样返回——状态机不许有
// 第二条隐路,画错了宁可停在原地让人看见。
ThinkingPhase NextThinkingPhase(ThinkingPhase phase, ThinkingSignal signal) {
    switch (phase) {
        case ThinkingPhase::Hidden:
            return signal == ThinkingSignal::FirstDelta ? ThinkingPhase::AutoPreviewRunning : phase;
        case ThinkingPhase::AutoPreviewRunning:
            switch (signal) {
                case ThinkingSignal::Done:
                    return ThinkingPhase::CollapsedDone;
                case ThinkingSignal::ToggleExpand:
                    return ThinkingPhase::ExplicitExpandedRunning;
                default:
                    return phase;
            }
        case ThinkingPhase::ExplicitExpandedRunning:
            switch (signal) {
                case ThinkingSignal::Done:
                    return ThinkingPhase::ExplicitExpandedDone;
                case ThinkingSignal::ToggleCollapse:
                    return ThinkingPhase::CollapsedRunning;
                default:
                    return phase;
            }
        case ThinkingPhase::CollapsedRunning:
            return signal == ThinkingSignal::ToggleExpand ? ThinkingPhase::ExplicitExpandedRunning : phase;
        case ThinkingPhase::CollapsedDone:
            return signal == ThinkingSignal::ToggleExpand ? ThinkingPhase::ExplicitExpandedDone : phase;
        case ThinkingPhase::ExplicitExpandedDone:
            return signal == ThinkingSignal::ToggleCollapse ? ThinkingPhase::CollapsedDone : phase;
    }
    return phase;
}

bool ThinkingHasVisibleText(const std::string& text) {
    for (const char c : text) {
        if (c != ' ' && c != '\t' && c != '\n' && c != '\r') {
            return true;
        }
    }
    return false;
}

std::vector<std::string> ThinkingPreviewRows(const std::string& text, int width, int max_rows) {
    std::vector<std::string> rows;
    if (text.empty() || max_rows <= 0) {
        return rows;
    }
    const int safe_width = width > 0 ? width : 80;
    // 正文挂两空格缩进,预览行自己的宽度让出这两列,再留末列一格防隐式折行。
    const int wrap_width = (std::max)(8, safe_width - 2 - 1);
    const int reserve_cols = (max_rows + 2) * wrap_width;

    const std::vector<std::string> logical = SplitLines(text);
    // 从最后一条逻辑行往前收,收满 max_rows 条视觉行就停——"取末尾,
    // 不取开头"。
    std::vector<std::string> tail;
    for (auto it = logical.rbegin(); it != logical.rend() && static_cast<int>(tail.size()) < max_rows; ++it) {
        const std::u32string clean = SanitizePreviewLine(Utf8ToUtf32(*it));
        if (clean.empty()) {
            tail.insert(tail.begin(), std::string());
            continue;
        }
        const std::vector<std::u32string> wrapped = WrapPreviewLine(clean, wrap_width, reserve_cols);
        for (auto w = wrapped.rbegin(); w != wrapped.rend() && static_cast<int>(tail.size()) < max_rows; ++w) {
            tail.insert(tail.begin(), Utf32ToUtf8(*w));
        }
    }
    return tail;
}

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
    // 思考条目的有效档:全局展开开关,或本条自己的用户展开态(运行中
    // ExplicitExpandedRunning 优先于全局紧凑——用户刚伸手打开,不能等
    // 下一帧全局重打才看见)。收定后的展开交给全局开关(整组重打路)。
    const bool thinking_explicit_expanded = item.kind == TranscriptKind::Thinking &&
                                            item.thinking_phase == ThinkingPhase::ExplicitExpandedRunning;
    const bool effective_expanded = expanded || thinking_explicit_expanded;

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
        if (effective_expanded && item.kind == TranscriptKind::Thinking && !item.full_output.empty()) {
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

    // 思考自动预览(逐帧露尾):默认档运行中,标题底下露正文末尾最多
    // kThinkingPreviewMaxRows 条视觉行,弱色、不抢正文层级、不跑
    // Markdown。正文没有可见内容(空/纯空白/全控制字符)时一行也不铺,
    // 不露空框。宽度/清洗都在 ThinkingPreviewRows 里办好,这里只加缩进
    // 与弱色。
    if (item.kind == TranscriptKind::Thinking && item.thinking_phase == ThinkingPhase::AutoPreviewRunning &&
        !effective_expanded && item.status == TranscriptStatus::Running &&
        ThinkingHasVisibleText(item.full_output)) {
        const std::vector<std::string> rows = ThinkingPreviewRows(item.full_output, width, kThinkingPreviewMaxRows);
        for (const std::string& row : rows) {
            out += indent + "  " + theme.stats + row + theme.reset + "\n";
        }
    }

    // UI-D 展开版:完整入参 JSON 一行 + full_output 全文(标题行 + 每行缩进)。
    // 截断规则跟摘要行一致:width > 0 且不夹 ANSI 才截——展开版可能被
    // TranscriptPainter 原地重画,物理折行同样会毁掉行数记账。
    if (effective_expanded) {
        const std::string body_indent = indent + "  ";
        const int body_cols = indent_cols + 2;
        if (!item.input_json.empty()) {
            std::string param_line = tr("transcript.params_prefix") + item.input_json;
            if (width > 0) {
                param_line = TruncateWithEllipsis(param_line, width - body_cols - 1);
            }
            out += body_indent + param_line + "\n";
        }
        // 思考条目正文一个字没到(空 thinking/仅 signature/redacted):连
        // 占位行都不铺,不露空框——运行中如此,收定后展开档也如此。正文
        // 到了就全文随流续画(用户展开不设行帽,完毕也不自动收折);收定
        // 的思考同工具规矩全文铺。
        const bool thinking_item = item.kind == TranscriptKind::Thinking;
        if (thinking_item && !ThinkingHasVisibleText(item.full_output)) {
            // 什么都不铺:标题行("思考 Xs(未提供摘要)")就是这块的全部。
        } else if (item.full_output.empty()) {
            out += body_indent + tr("transcript.no_full_output") + "\n";
        } else {
            const std::vector<std::string> lines = SplitLines(item.full_output);
            out += body_indent + trf("transcript.full_output_header", CountLines(item.full_output)) + "\n";
            for (std::size_t i = 0; i < lines.size(); ++i) {
                std::string line = lines[i];
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
                                  int width, bool expanded, int focus_index, int expanded_index) {
    std::string out;
    // 相邻两枚的间距由间距表唯一决定(同构渲染单 P0):只对"实际打印出来"
    // 的条目记账——紧凑档跳过的 SubTool 不参与间距计算,免得被隐藏的子项
    // 把后一枚顶层 Tool 的间隔吃掉。
    bool printed_any = false;
    BlockRole previous_role = BlockRole::Tool;
    const auto role_of = [](const TranscriptItem& item) {
        switch (item.kind) {
            case TranscriptKind::Tool:
                return BlockRole::Tool;
            case TranscriptKind::SubTool:
                return BlockRole::SubTool;
            case TranscriptKind::Thinking:
                return BlockRole::Thinking;
        }
        return BlockRole::Tool;
    };
    for (std::size_t i = 0; i < items.size(); ++i) {
        const bool item_expanded = expanded || static_cast<int>(i) == expanded_index;
        if (!item_expanded && items[i].kind == TranscriptKind::SubTool) {
            continue;
        }
        if (printed_any) {
            for (int gap = 0; gap < GapBetween(previous_role, role_of(items[i])); ++gap) {
                out += "\n";
            }
        }
        out += FormatTranscriptItem(items[i], theme, width, item_expanded,
                                    static_cast<int>(i) == focus_index);
        previous_role = role_of(items[i]);
        printed_any = true;
    }
    return out;
}

std::vector<std::string> RenderSessionBlocks(const std::vector<SessionBlock>& blocks, const Theme& theme,
                                             int width, bool expanded) {
    std::vector<std::string> lines;
    const auto append_lines = [&lines](const std::string& text) {
        std::size_t pos = 0;
        while (pos < text.size()) {
            const std::size_t nl = text.find('\n', pos);
            const std::size_t end = nl == std::string::npos ? text.size() : nl;
            lines.push_back(text.substr(pos, end - pos));
            if (nl == std::string::npos) {
                break;
            }
            pos = nl + 1;
        }
    };
    bool printed_any = false;
    BlockRole previous_role = BlockRole::Tool;
    for (const SessionBlock& block : blocks) {
        std::string rendered;
        switch (block.kind) {
            case SessionBlock::Kind::Items:
                if (block.items.empty()) {
                    continue;
                }
                rendered = FormatTranscriptItems(block.items, theme, width, expanded);
                break;
            case SessionBlock::Kind::Markdown: {
                if (block.header.empty() && block.body.empty()) {
                    continue;
                }
                rendered = block.header.empty() ? std::string() : block.header + "\n";
                for (const std::string& line : RenderMarkdown(block.body, theme, width)) {
                    rendered += line + "\n";
                }
                break;
            }
            case SessionBlock::Kind::Notice:
                if (block.line.empty()) {
                    continue;
                }
                rendered = block.line + "\n";
                break;
        }
        if (rendered.empty()) {
            continue;
        }
        if (printed_any) {
            for (int gap = 0; gap < GapBetween(previous_role, block.role); ++gap) {
                lines.push_back(std::string());
            }
        }
        append_lines(rendered);
        previous_role = block.role;
        printed_any = true;
    }
    return lines;
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

// ---- 条目工厂(终端接线收尾单):实现与原先各处手工拼的代码逐字对齐,
// 行为零变——只是把"怎么拼"收进一处。 -------------------------------

TranscriptItem MakeNoticeItem(int id, const std::string& title, TranscriptStatus status,
                              std::vector<std::string> summary_lines) {
    TranscriptItem item;
    item.id = id;
    item.kind = TranscriptKind::Tool;
    item.tool_name = "agent_notice";
    item.title = title;
    item.status = status;
    item.start_time = item.end_time = std::chrono::steady_clock::now();
    item.summary_lines = std::move(summary_lines);
    return item;
}

std::string BackgroundNoticeTitle(bool permission_denial) {
    return permission_denial ? tr("agent_panel.denial_notice_title")
                             : tr("agent_panel.supervisor_notice_title");
}

TranscriptItem MakeAssistantArchiveItem(int id, std::string body, TranscriptStatus status) {
    TranscriptItem item;
    item.id = id;
    item.kind = TranscriptKind::Tool;
    item.tool_name = "assistant";
    item.title = tr("transcript.assistant_bg_title");
    item.status = status;
    item.start_time = item.end_time = std::chrono::steady_clock::now();
    // 紧凑档摘要:正文头两行,每行掐 120 码点——渲染层还会按终端宽截,
    // 这里先兜住 Ctrl+E(不截宽)那一路。
    std::size_t cursor = 0;
    for (int taken = 0; taken < 2 && cursor < body.size(); ++taken) {
        std::size_t cut = body.find('\n', cursor);
        const std::string line =
            body.substr(cursor, cut == std::string::npos ? std::string::npos : cut - cursor);
        if (!line.empty()) {
            item.summary_lines.push_back(TruncateUtf8Codepoints(line, 120));
        }
        if (cut == std::string::npos) {
            break;
        }
        cursor = cut + 1;
    }
    item.full_output = std::move(body);
    return item;
}

TranscriptItem MakeAgentTaskToolItem(int id, const std::string& tool_name, const std::string& input_json,
                                     bool done, bool is_error, const std::string& result,
                                     TranscriptKind kind) {
    TranscriptItem item;
    item.id = id;
    // 投影坐标归调用方(同构渲染单 P0):kind 是"相对当前查看根"的层级,
    // 不在工厂里写死——Main 面板的子代理内层工具给 SubTool,Subagent 查看
    // 页给该代理自己的 Tool。
    item.kind = kind;
    item.tool_name = tool_name;
    item.input_json = input_json;
    // 解析不出对象(空串/坏 JSON,比如只有结果的孤儿卡)退空对象——
    // BuildToolTitle 拿空对象给出 "名字(...)" 的老兜底,不在 null 上炸。
    nlohmann::json parsed = nlohmann::json::object();
    try {
        parsed = nlohmann::json::parse(input_json);
    } catch (...) {
        parsed = nlohmann::json::object();
    }
    item.title = BuildToolTitle(tool_name, parsed);
    if (done) {
        item.status = is_error ? TranscriptStatus::Error : TranscriptStatus::Ok;
        item.full_output = result;
        // ⎿ 摘要(Main 重放同款口径,同构渲染单 §7.3):结果首行当摘要,
        // 多行补 "+N 行";空结果按成败给一句兜底,不露空 ⎿。
        std::string first_line = result.substr(0, result.find('\n'));
        if (first_line.empty()) {
            first_line = is_error ? tr("cmd.resume.history.tool_error") : tr("cmd.resume.history.tool_done");
        }
        const int line_count = CountLines(result);
        if (line_count > 1) {
            first_line += trf("cmd.resume.history.tool_more", line_count - 1);
        }
        item.summary_lines = {std::move(first_line)};
        item.end_time = std::chrono::steady_clock::now();
    } else {
        item.status = TranscriptStatus::Running;  // 还在跑
    }
    item.start_time = std::chrono::steady_clock::now();
    return item;
}

TranscriptItem MakeAgentTaskThinkingItem(int id, const std::string& text, bool streaming) {
    TranscriptItem item;
    item.id = id;
    item.kind = TranscriptKind::Thinking;
    item.tool_name = "thinking";
    item.full_output = text;
    item.thinking_text_bytes = static_cast<int>(text.size());
    // 查看态(代理面板)的思考卡不掺 main 流的自动露尾——保持"标题一行,
    // 展开看全文"的老档位;这里 Collapsed* 只表画法,数据照全量持有。
    if (streaming) {
        item.status = TranscriptStatus::Running;
        item.thinking_phase = ThinkingPhase::CollapsedRunning;
        // 流式思考尾巴:与 main 流式思考同款折叠规矩——Running 条目,头行
        // 「思考中 · N 字」随重铺拍跳动;Ctrl+O 展开看长文。
        item.title = trf("agent_activity.thinking", CountUtf8Codepoints(text));
    } else {
        item.status = TranscriptStatus::Ok;
        item.thinking_phase = ThinkingPhase::CollapsedDone;
        item.title = tr("agent_panel.event_thinking");
    }
    item.start_time = std::chrono::steady_clock::now();
    return item;
}

}  // namespace lubancode::cli
