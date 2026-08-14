#include "cli/line_editor.hpp"

#include <algorithm>
#include <cctype>

#include "cli/i18n.hpp"

namespace lubancode::cli {

namespace {

constexpr char32_t kPasteTokenBase = 0xF0000;

// 手写 UTF-8 -> UTF-32 解码已升为公开函数 Utf8ToUtf32(0.28.x 取回排队
// 消息装回编辑 buffer 也要用),本体搬去下面的公开区;非法起始字节、序列
// 被截断、续字节不是 10xxxxxx 一律跳过一个字节继续——这是给自己代码拼
// 出来的字符串用的,不是校验外部不可信输入的严格解码器。

std::u32string CurrentWord(const std::u32string& line) {
    const std::size_t space_pos = line.find(U' ');
    return space_pos == std::u32string::npos ? line : line.substr(0, space_pos);
}

// UI-A:多行 composer 的拼接/拆分。历史条目、draft、RenderState::line 都用
// "多行拼 '\n'"这一种存法,拆装只在这两个函数里做,不散落各处。
std::u32string JoinLines(const std::vector<std::u32string>& lines) {
    std::u32string out;
    for (std::size_t i = 0; i < lines.size(); ++i) {
        if (i > 0) {
            out.push_back(U'\n');
        }
        out += lines[i];
    }
    return out;
}

std::vector<std::u32string> SplitJoined(const std::u32string& joined) {
    std::vector<std::u32string> out(1);
    for (char32_t c : joined) {
        if (c == U'\n') {
            out.emplace_back();
        } else {
            out.back().push_back(c);
        }
    }
    return out;
}

// "空 composer(全空白)不发送"里的"全空白":空格、Tab、全角空格、回车
// 换行都算。别的不猜——判得太宽反而会把用户真想发的内容拦下来。
bool AllWhitespace(const std::u32string& text) {
    for (char32_t c : text) {
        if (c != U' ' && c != U'\t' && c != U'　' && c != U'\n' && c != U'\r') {
            return false;
        }
    }
    return true;
}

std::u32string AsciiToU32(const std::string& text) {
    std::u32string out;
    out.reserve(text.size());
    for (char c : text) {
        out.push_back(static_cast<char32_t>(static_cast<unsigned char>(c)));
    }
    return out;
}

// 只用来比较 ASCII 命令名(/help、/model……),忽略大小写——跟
// ParseSlashCommand 的大小写不敏感规则保持一致。
std::string ToLowerAscii(std::string text) {
    for (char& c : text) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return text;
}

std::string LongestCommonPrefix(const std::vector<std::string>& items) {
    if (items.empty()) {
        return {};
    }
    std::string prefix = items.front();
    for (const auto& item : items) {
        std::size_t i = 0;
        const std::size_t max_len = std::min(prefix.size(), item.size());
        while (i < max_len && prefix[i] == item[i]) {
            ++i;
        }
        prefix.resize(i);
        if (prefix.empty()) {
            break;
        }
    }
    return prefix;
}

}  // namespace

ConfirmMode NextConfirmMode(ConfirmMode mode) {
    switch (mode) {
        case ConfirmMode::Confirm:
            return ConfirmMode::Auto;
        case ConfirmMode::Auto:
            return ConfirmMode::Yolo;
        case ConfirmMode::Yolo:
            return ConfirmMode::Confirm;
    }
    return ConfirmMode::Confirm;
}

std::string ConfirmModeLabel(ConfirmMode mode) {
    switch (mode) {
        case ConfirmMode::Confirm:
            return tr("mode.confirm");
        case ConfirmMode::Auto:
            return "auto";
        case ConfirmMode::Yolo:
            return "yolo";
    }
    return tr("mode.confirm");
}

int CharDisplayWidth(char32_t cp) {
    // 见头文件注释:简易 East Asian Width 判定,不是完整的 Unicode 表,
    // 覆盖最常用的 CJK 统一表意文字、假名、韩文音节、全角标点这些区段。
    if (cp == 0) {
        return 0;
    }
    if (cp < 0x1100) {
        return 1;
    }
    const bool wide =
        (cp >= 0x1100 && cp <= 0x115F) ||   // 韩文字母(Hangul Jamo)
        cp == 0x2329 || cp == 0x232A ||
        (cp >= 0x2E80 && cp <= 0x303E) ||   // CJK 部首、CJK 标点
        (cp >= 0x3041 && cp <= 0x33FF) ||   // 平假名、片假名、CJK 兼容
        (cp >= 0x3400 && cp <= 0x4DBF) ||   // CJK 扩展 A
        (cp >= 0x4E00 && cp <= 0x9FFF) ||   // CJK 统一表意文字
        (cp >= 0xA000 && cp <= 0xA4CF) ||   // 彝文
        (cp >= 0xAC00 && cp <= 0xD7A3) ||   // 韩文音节
        (cp >= 0xF900 && cp <= 0xFAFF) ||   // CJK 兼容表意文字
        (cp >= 0xFF00 && cp <= 0xFF60) ||   // 全角字符
        (cp >= 0xFFE0 && cp <= 0xFFE6) ||
        (cp >= 0x1F300 && cp <= 0x1F64F) || // UI-A:emoji 常用区段(杂项符号和象形文字、表情),
        (cp >= 0x1F680 && cp <= 0x1F6FF) || // 交通与地图符号,
        (cp >= 0x1F900 && cp <= 0x1F9FF) || // 补充符号和象形文字,
        (cp >= 0x1FA70 && cp <= 0x1FAFF) || // 符号和象形文字扩展 A——终端都按两列画
        (cp >= 0x20000 && cp <= 0x3FFFD);   // CJK 扩展 B 及以上
    return wide ? 2 : 1;
}

std::size_t DisplayWidth(const std::u32string& text) {
    std::size_t width = 0;
    for (char32_t c : text) {
        width += static_cast<std::size_t>(CharDisplayWidth(c));
    }
    return width;
}

std::string Utf32ToUtf8(const std::u32string& text) {
    std::string out;
    out.reserve(text.size() * 3);
    for (char32_t cp : text) {
        if (cp <= 0x7F) {
            out.push_back(static_cast<char>(cp));
        } else if (cp <= 0x7FF) {
            out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        } else if (cp <= 0xFFFF) {
            out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        } else {
            out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        }
    }
    return out;
}

std::u32string Utf8ToUtf32(const std::string& text) {
    std::u32string out;
    std::size_t i = 0;
    const std::size_t n = text.size();
    while (i < n) {
        const unsigned char c0 = static_cast<unsigned char>(text[i]);
        char32_t cp = 0;
        std::size_t extra = 0;
        if (c0 < 0x80) {
            cp = c0;
        } else if ((c0 & 0xE0) == 0xC0) {
            cp = c0 & 0x1F;
            extra = 1;
        } else if ((c0 & 0xF0) == 0xE0) {
            cp = c0 & 0x0F;
            extra = 2;
        } else if ((c0 & 0xF8) == 0xF0) {
            cp = c0 & 0x07;
            extra = 3;
        } else {
            ++i;  // 非法首字节,跳过一个
            continue;
        }
        bool ok = true;
        for (std::size_t k = 0; k < extra; ++k) {
            if (i + 1 + k >= n) {
                ok = false;
                break;
            }
            const unsigned char ck = static_cast<unsigned char>(text[i + 1 + k]);
            if ((ck & 0xC0) != 0x80) {
                ok = false;
                break;
            }
            cp = (cp << 6) | (ck & 0x3F);
        }
        if (!ok) {
            ++i;
            continue;
        }
        out.push_back(cp);
        i += extra + 1;
    }
    return out;
}

std::u32string TruncateToDisplayWidth(const std::u32string& text, int max_width) {
    if (max_width <= 0) {
        return {};
    }
    std::u32string out;
    int width = 0;
    for (char32_t c : text) {
        const int w = CharDisplayWidth(c);
        if (width + w > max_width) {
            break;  // 下一个字符会超宽,整个不要它——不切半个字宽
        }
        out.push_back(c);
        width += w;
    }
    return out;
}

std::string TruncateUtf8ToDisplayWidth(const std::string& utf8, int max_width) {
    return Utf32ToUtf8(TruncateToDisplayWidth(Utf8ToUtf32(utf8), max_width));
}

std::vector<std::u32string> WrapToDisplayWidth(const std::u32string& text, int max_width) {
    std::vector<std::u32string> lines;
    if (max_width <= 0) {
        return lines;
    }
    std::u32string cur;
    int width = 0;
    const auto flush = [&] {
        lines.push_back(cur);
        cur.clear();
        width = 0;
    };
    for (char32_t c : text) {
        if (c == U'\n') {
            flush();  // 显式换行:原样尊重,断开一行
            continue;
        }
        const int w = CharDisplayWidth(c);
        // 下一个字加进来会超宽、且当前行非空:先把当前行交出去,再开新行装它。
        // 当前行为空时即便这个字比 max_width 还宽也照收(独占一行,不丢内容)。
        if (width + w > max_width && !cur.empty()) {
            flush();
        }
        cur.push_back(c);
        width += w;
    }
    if (!cur.empty()) {
        flush();
    }
    return lines;
}

std::vector<std::string> WrapUtf8ToDisplayWidth(const std::string& utf8, int max_width) {
    const std::vector<std::u32string> wrapped = WrapToDisplayWidth(Utf8ToUtf32(utf8), max_width);
    std::vector<std::string> out;
    out.reserve(wrapped.size());
    for (const std::u32string& line : wrapped) {
        out.push_back(Utf32ToUtf8(line));
    }
    return out;
}

std::size_t DisplayWidthUtf8(const std::string& utf8) { return DisplayWidth(Utf8ToUtf32(utf8)); }

EditLineWindow ComputeEditLineWindow(const std::u32string& line, std::size_t cursor, int content_width) {
    if (content_width <= 0) {
        return EditLineWindow{};
    }
    const std::size_t safe_cursor = std::min(cursor, line.size());
    const std::size_t cursor_col = DisplayWidth(line.substr(0, safe_cursor));
    const std::size_t total_width = DisplayWidth(line);
    const std::size_t cw = static_cast<std::size_t>(content_width);
    if (total_width <= cw) {
        return EditLineWindow{line, cursor_col};
    }

    // 整行放不下:窗口起点尽量让光标落在中间,不越过 [0, total_width - cw]。
    std::size_t desired_start = cursor_col > cw / 2 ? cursor_col - cw / 2 : 0;
    const std::size_t max_start = total_width - cw;
    if (desired_start > max_start) {
        desired_start = max_start;
    }

    // 把"窗口起点该在第几列"换算成"该从第几个码点开始"——不能砍在一个
    // 宽字符中间。
    std::size_t start_index = 0;
    std::size_t start_col = 0;
    while (start_index < line.size()) {
        const std::size_t w = static_cast<std::size_t>(CharDisplayWidth(line[start_index]));
        if (start_col + w > desired_start) {
            break;
        }
        start_col += w;
        ++start_index;
    }

    EditLineWindow window;
    window.text = TruncateToDisplayWidth(line.substr(start_index), content_width);
    window.cursor_display_col = cursor_col >= start_col ? cursor_col - start_col : 0;
    // 保险:极端取舍下(比如窗口起点因为宽字符对齐往前挪了一列)光标列可能
    // 比窗口文本本身的显示宽度还宽一点点,夹到窗口末尾,不越界。
    const std::size_t window_width = DisplayWidth(window.text);
    if (window.cursor_display_col > window_width) {
        window.cursor_display_col = window_width;
    }
    return window;
}

LineEditorCore::LineEditorCore(std::vector<CompletionCandidate> slash_candidates)
    : slash_candidates_(std::move(slash_candidates)) {}

void LineEditorCore::BeginLine(bool composer) {
    composer_ = composer;
    ClearBuffer();
    tab_cycle_.reset();
    menu_selection_.reset();
    focus_mode_ = false;  // 焦点态不跨读取——新一次 ReadLine 从编辑态起步
    ResetHistoryBrowsing();
}

void LineEditorCore::ResetHistoryBrowsing() {
    history_index_.reset();
    draft_.clear();
}

void LineEditorCore::ClearBuffer() {
    lines_.assign(1, std::u32string());
    row_ = 0;
    col_ = 0;
    pasted_contents_.clear();
    paste_run_.reset();
}

void LineEditorCore::LoadJoined(const std::u32string& joined) {
    paste_run_.reset();
    lines_ = SplitJoined(joined);
    row_ = lines_.size() - 1;
    col_ = lines_[row_].size();
}

void LineEditorCore::LoadText(const std::u32string& joined) {
    LoadJoined(joined);
    tab_cycle_.reset();
    menu_selection_.reset();
    ResetHistoryBrowsing();
}

void LineEditorCore::InsertChar(char32_t ch) {
    lines_[row_].insert(col_, 1, ch);
    ++col_;
    ResetHistoryBrowsing();
}

void LineEditorCore::InsertPaste(const std::string& text, std::size_t replace_before) {
    if (replace_before > 0) {
        // ConPTY 已把 paste 首行逐字送进编辑器，直到回车才露出它是粘贴。
        // 这些字都在当前行、紧贴光标；先原位撤下，再换成完整附件 token。
        const std::size_t erase_count = std::min(replace_before, col_);
        lines_[row_].erase(col_ - erase_count, erase_count);
        col_ -= erase_count;
    }
    std::string normalized;
    normalized.reserve(text.size());
    for (std::size_t i = 0; i < text.size(); ++i) {
        if (text[i] == '\r') {
            if (i + 1 < text.size() && text[i + 1] == '\n') {
                continue;
            }
            normalized += '\n';
        } else {
            normalized += text[i];
        }
    }
    const std::u32string decoded = Utf8ToUtf32(normalized);
    if (decoded.empty()) {
        return;
    }

    if (paste_run_.has_value()) {
        PasteRunState& run = *paste_run_;
        run.content += decoded;
        pasted_contents_[run.token_index] = run.content;
        ResetHistoryBrowsing();
        return;
    }

    if (decoded.size() <= kLargePasteCharThreshold || pasted_contents_.size() >= 0xFFFE) {
        for (const char32_t c : decoded) {
            if (c == U'\n') {
                InsertNewLine();
            } else {
                InsertChar(c);
            }
        }
        return;
    }
    const char32_t token = kPasteTokenBase + static_cast<char32_t>(pasted_contents_.size());
    const std::size_t token_index = pasted_contents_.size();
    pasted_contents_.push_back(decoded);
    InsertChar(token);
    paste_run_ = PasteRunState{token_index, decoded};
}

std::u32string LineEditorCore::ExpandPasteTokens(const std::u32string& text) const {
    std::u32string out;
    for (const char32_t c : text) {
        const std::size_t index = c >= kPasteTokenBase ? static_cast<std::size_t>(c - kPasteTokenBase)
                                                       : pasted_contents_.size();
        if (index < pasted_contents_.size()) {
            out += pasted_contents_[index];
        } else {
            out += c;
        }
    }
    return out;
}

std::u32string LineEditorCore::DisplayPasteTokens(const std::u32string& text) const {
    std::u32string out;
    for (const char32_t c : text) {
        const std::size_t index = c >= kPasteTokenBase ? static_cast<std::size_t>(c - kPasteTokenBase)
                                                       : pasted_contents_.size();
        if (index < pasted_contents_.size()) {
            out += Utf8ToUtf32(trf("input.pasted_content", pasted_contents_[index].size()));
        } else {
            out += c;
        }
    }
    return out;
}

void LineEditorCore::InsertNewLine() {
    std::u32string rest = lines_[row_].substr(col_);
    lines_[row_].resize(col_);
    lines_.insert(lines_.begin() + static_cast<std::ptrdiff_t>(row_) + 1, std::move(rest));
    ++row_;
    col_ = 0;
    ResetHistoryBrowsing();
}

void LineEditorCore::DeleteBackward() {
    if (col_ > 0) {
        lines_[row_].erase(col_ - 1, 1);
        --col_;
        ResetHistoryBrowsing();
        return;
    }
    if (row_ == 0) {
        return;  // 整个 composer 的开头,退无可退——跟单行时代行首退格一个样,什么都不发生
    }
    // 行首退格:把这一行整个并进上一行,光标落在缝合点上。
    col_ = lines_[row_ - 1].size();
    lines_[row_ - 1] += lines_[row_];
    lines_.erase(lines_.begin() + static_cast<std::ptrdiff_t>(row_));
    --row_;
    ResetHistoryBrowsing();
}

void LineEditorCore::MoveHistory(bool up) {
    if (up) {
        if (history_.empty()) {
            return;
        }
        if (!history_index_.has_value()) {
            draft_ = JoinLines(lines_);
            history_index_ = 0;
        } else if (*history_index_ + 1 < history_.size()) {
            ++(*history_index_);
        } else {
            return;  // 已经翻到最老的一条,到头了
        }
        LoadJoined(history_[history_.size() - 1 - *history_index_]);
    } else {
        if (!history_index_.has_value()) {
            return;  // 已经在底部,没什么好往下翻的
        }
        if (*history_index_ == 0) {
            LoadJoined(draft_);
            history_index_.reset();
        } else {
            --(*history_index_);
            LoadJoined(history_[history_.size() - 1 - *history_index_]);
        }
    }
}

std::vector<std::string> LineEditorCore::MatchingCandidateNames(const std::u32string& word) const {
    const std::string word_lower = ToLowerAscii(Utf32ToUtf8(word));
    std::vector<std::string> result;
    for (const auto& cand : slash_candidates_) {
        const std::string cand_lower = ToLowerAscii(cand.name);
        if (cand_lower.size() >= word_lower.size() && cand_lower.compare(0, word_lower.size(), word_lower) == 0) {
            result.push_back(cand.name);
        }
    }
    return result;
}

// 把整行内容换成"候选名 + 词后面原来剩下的那截(suffix)":suffix 非空
// 说明命令词后面本来就跟着别的内容(比如已经打的参数),原样保留、不吞掉;
// suffix 是空的话,按"唯一匹配直接补全整名 + 空格"的规矩自己补一个空格。
// 光标落到整行末尾。
//
// 之所以不能像第一次按 Tab 那样用 CurrentWord(line_) 现算词的边界:轮转
// 到第二个候选往后,line_ 已经被换成上一个候选的名字了,"当前词"早就不是
// 用户最初敲的那截前缀——suffix 得在第一次按 Tab、词边界还没被改写之前就
// 存进 TabCycleState 里,后面轮转直接复用这份存好的值。
void LineEditorCore::CompleteToCandidate(const std::string& name, const std::u32string& suffix) {
    // 补全只在"composer 恰好一行"时触发(HandleTab 顶部拦掉了多行),这里
    // 直接操作 lines_[0] 就是操作整个 composer。
    const std::u32string name32 = AsciiToU32(name);
    lines_[0] = suffix.empty() ? (name32 + U' ') : (name32 + suffix);
    col_ = lines_[0].size();
}

void LineEditorCore::HandleTab() {
    if (lines_.size() != 1) {
        return;  // UI-A:多行 composer 里 / 是正文不是命令,Tab 不补全
    }
    if (tab_cycle_.has_value()) {
        // 上一个按键就是 Tab(其余任何按键都会清空 tab_cycle_,见 HandleKey),
        // 这次接着轮转到下一个候选。
        if (tab_cycle_->matches.size() > 1) {
            tab_cycle_->index = (tab_cycle_->index + 1) % static_cast<int>(tab_cycle_->matches.size());
            CompleteToCandidate(tab_cycle_->matches[static_cast<std::size_t>(tab_cycle_->index)],
                                 tab_cycle_->suffix);
        }
        return;
    }

    if (lines_[0].empty() || lines_[0].front() != U'/') {
        return;  // 不是 slash 命令,Tab 什么都不做
    }
    const std::u32string word = CurrentWord(lines_[0]);
    if (col_ > word.size()) {
        return;  // 光标已经越过命令词、落在参数区了,不补全
    }
    const std::u32string suffix = lines_[0].substr(word.size());  // 词后面剩下的部分,原样保留

    const std::vector<std::string> matches = MatchingCandidateNames(word);
    if (matches.empty()) {
        return;
    }
    if (matches.size() == 1) {
        CompleteToCandidate(matches[0], suffix);
        return;
    }

    const std::u32string lcp = AsciiToU32(LongestCommonPrefix(matches));
    if (lcp.size() > word.size()) {
        // 先补到公共前缀,先不进入轮转(index = -1);下次 Tab 再轮转。
        lines_[0] = lcp + suffix;
        col_ = lcp.size();
        tab_cycle_ = TabCycleState{matches, -1, suffix};
    } else {
        // 已经在公共前缀上了,没法再往前补,直接开始轮转第一个候选。
        tab_cycle_ = TabCycleState{matches, 0, suffix};
        CompleteToCandidate(matches[0], suffix);
    }
}

RenderState LineEditorCore::BuildRenderState(bool submitted, bool cleared, bool eof_requested,
                                              bool mode_changed) const {
    RenderState state;
    state.lines.clear();
    state.lines.reserve(lines_.size());
    for (const std::u32string& line : lines_) {
        state.lines.push_back(DisplayPasteTokens(line));
    }
    state.cursor_row = row_;
    state.cursor_col = DisplayPasteTokens(lines_[row_].substr(0, col_)).size();
    state.line = ExpandPasteTokens(JoinLines(lines_));
    // 兼容字段 cursor:光标在拼接串里的码点位置——前面每行占"行长 + 1 个
    // '\n'",再加行内偏移。单行场景就是 col_,跟升级前一模一样。
    std::size_t joined_cursor = ExpandPasteTokens(lines_[row_].substr(0, col_)).size();
    for (std::size_t i = 0; i < row_; ++i) {
        joined_cursor += ExpandPasteTokens(lines_[i]).size() + 1;
    }
    state.cursor = joined_cursor;
    state.cursor_display_col = DisplayWidth(DisplayPasteTokens(lines_[row_].substr(0, col_)));
    state.submitted = submitted;
    state.cleared = cleared;
    state.eof_requested = eof_requested;
    state.mode_changed = mode_changed;
    state.mode = confirm_mode_;
    state.focus_active = focus_mode_;

    // UI-A:提示区只在"composer 恰好一行、且以 / 开头"时出现;多行时 / 是
    // 正文,不当命令,不出提示。
    if (lines_.size() == 1 && !lines_[0].empty() && lines_[0].front() == U'/') {
        // 菜单选择态(0.16.0 ↓↑ 直选)优先:候选名单和选中下标用进入菜单
        // 那一刻存下的那份。其次是正在轮转的 Tab 会话(tab_cycle_ 有值),
        // 同样不能拿当前行现算——轮转途中 line_ 已经被换成候选名本身,重新
        // 按 CurrentWord(line_) 去匹配只会匹配出它自己一个,候选名单会跟着
        // "塌缩成一个",连带选中标记也没地方标。
        std::vector<std::string> matches;
        int selected_index = -1;
        if (menu_selection_.has_value()) {
            matches = menu_selection_->matches;
            selected_index = menu_selection_->index;
        } else if (tab_cycle_.has_value()) {
            matches = tab_cycle_->matches;
            selected_index = tab_cycle_->index;
        } else {
            const std::u32string word = CurrentWord(lines_[0]);
            matches = MatchingCandidateNames(word);
        }
        state.hint_lines = BuildHintLines(matches, selected_index);
        state.selected_index = selected_index;
    }

    return state;
}

std::vector<std::string> LineEditorCore::BuildHintLines(const std::vector<std::string>& matches,
                                                          int selected_index) const {
    std::vector<std::string> lines;
    if (matches.empty()) {
        return lines;
    }
    constexpr std::size_t kMaxLines = 6;
    // 选中项落在前 6 个之外时,展示窗口往下挪到"选中项是窗口最后一行",
    // 保证 "> " 标记永远看得见(菜单选择态循环到第 7 个往后、Tab 轮转同理)。
    std::size_t begin = 0;
    if (selected_index >= static_cast<int>(kMaxLines)) {
        begin = static_cast<std::size_t>(selected_index) - kMaxLines + 1;
    }
    const std::size_t shown = std::min(matches.size() - begin, kMaxLines);
    for (std::size_t offset = 0; offset < shown; ++offset) {
        const std::size_t i = begin + offset;
        const std::string& name = matches[i];
        std::string description;
        for (const auto& cand : slash_candidates_) {
            if (cand.name == name) {
                description = cand.description;
                break;
            }
        }
        const bool selected = selected_index >= 0 && static_cast<std::size_t>(selected_index) == i;
        std::string line = selected ? "> " : "  ";
        line += name;
        line += "  ";
        line += description;
        lines.push_back(std::move(line));
    }
    if (matches.size() > kMaxLines) {
        lines.push_back(trf("ui.menu_more", matches.size()));
    }
    return lines;
}

RenderState LineEditorCore::CurrentRenderState() const {
    return BuildRenderState(false, false, false, false);
}

RenderState LineEditorCore::HandleKey(const KeyEvent& event) {
    // UI-A:非 composer 模式(确认提示、/model 选择、向导……)不认"插换行"
    // 这回事,NewLine 直接当 Enter 处理——单行读取场景语义跟升级前一字不差
    // (以前终端层遇到 VK_RETURN 不看修饰键,Shift+Enter 本来就是提交)。
    KeyEvent effective = event;
    if (effective.kind == KeyKind::NewLine && !composer_) {
        effective.kind = KeyKind::Enter;
    }

    // 连续 Paste 才能并成同一枚附件。任一真实编辑键都会截断这趟粘贴，
    // 免得用户先粘一段、移动光标后再粘一段，却被隔空并回旧附件。
    if (effective.kind != KeyKind::Paste) {
        paste_run_.reset();
    }

    if (effective.kind != KeyKind::Tab) {
        tab_cycle_.reset();
    }

    // 0.17.0 键位矫正:焦点态是显式模式(空 composer 按 Tab 进入,见下面
    // Tab 分支)。态内 Tab=焦点往旧走、Shift+Tab=往新走(这时不切档——
    // 焦点态里两个键就是一对方向键);ESC/Enter 退出焦点态回编辑,不清行、
    // 不提交(composer 本来就是空的);Ctrl+O/Ctrl+E 不退出,落到下面原语义
    // 继续转发(焦点态里看全文正是 Ctrl+E 的本职);其余按键(打字、退格、
    // 上下左右……)先退出焦点态,再按各自原语义处理——打字直接落进
    // composer,不用先按一下 ESC。
    if (focus_mode_) {
        switch (effective.kind) {
            case KeyKind::Tab: {
                RenderState state = BuildRenderState(false, false, false, false);
                state.focus_move = 1;
                return state;
            }
            case KeyKind::ShiftTab: {
                RenderState state = BuildRenderState(false, false, false, false);
                state.focus_move = -1;
                return state;
            }
            case KeyKind::Esc:
            case KeyKind::Enter:
                focus_mode_ = false;
                return BuildRenderState(false, false, false, false);
            case KeyKind::CtrlO:
            case KeyKind::CtrlE:
                break;  // 保持焦点态,落到下面的原语义(转发请求)
            default:
                focus_mode_ = false;
                break;  // 退出焦点态,按键原语义继续走下面的 switch
        }
    }

    // 0.16.0 slash 候选菜单直选:菜单选择态优先于一切旧语义。↓/↑ 循环移动
    // 选中标记;Enter 把整行换成"选中候选名 + 进入时存下的参数尾巴"再走
    // 下面正常的 Enter 提交路;ESC 只退出选择态(不清行,跟空闲态 ESC 清
    // composer 是两码事);其余任何按键(打字、退格、Tab、左右键……)先退出
    // 选择态,再按各自原语义继续处理——打字/退格回普通编辑后,候选提示会
    // 随新前缀自动刷新(BuildRenderState 现算)。
    if (menu_selection_.has_value()) {
        const int count = static_cast<int>(menu_selection_->matches.size());
        switch (effective.kind) {
            case KeyKind::Down:
                menu_selection_->index = (menu_selection_->index + 1) % count;
                return BuildRenderState(false, false, false, false);
            case KeyKind::Up:
                menu_selection_->index = (menu_selection_->index + count - 1) % count;
                return BuildRenderState(false, false, false, false);
            case KeyKind::Esc:
                menu_selection_.reset();
                return BuildRenderState(false, false, false, false);
            case KeyKind::Enter: {
                // 采纳:整行替换成选中命令名,用户已敲的参数尾巴(suffix)
                // 原样接在后面,然后落进下面的 Enter 分支正常提交。
                const std::string& name =
                    menu_selection_->matches[static_cast<std::size_t>(menu_selection_->index)];
                lines_.assign(1, AsciiToU32(name) + menu_selection_->suffix);
                row_ = 0;
                col_ = lines_[0].size();
                menu_selection_.reset();
                break;  // 不 return,交给下面 switch 的 Enter 分支提交
            }
            default:
                menu_selection_.reset();
                break;  // 退出选择态,按键本身的原语义继续走下面的 switch
        }
    }

    switch (effective.kind) {
        case KeyKind::Char:
            InsertChar(effective.ch);
            break;
        case KeyKind::Paste:
            InsertPaste(effective.text, effective.replace_before);
            break;
        case KeyKind::NewLine:
            InsertNewLine();
            break;
        case KeyKind::Backspace:
            DeleteBackward();
            break;
        case KeyKind::Left:
            if (col_ > 0) {
                --col_;
            } else if (row_ > 0) {
                // 行首再按左:跨过行边界,落到上一行行尾。
                --row_;
                col_ = lines_[row_].size();
            }
            break;
        case KeyKind::Right:
            if (col_ < lines_[row_].size()) {
                ++col_;
            } else if (row_ + 1 < lines_.size()) {
                // 行尾再按右:落到下一行行首。
                ++row_;
                col_ = 0;
            }
            break;
        case KeyKind::Home:
            col_ = 0;  // 当前行行首,不是整个 composer 开头
            break;
        case KeyKind::End:
            col_ = lines_[row_].size();  // 当前行行尾
            break;
        case KeyKind::Up:
            // UI-A 的取舍:光标不在第一行,就是行间移动;在第一行,才翻历史。
            // 单行 composer 恒在"第一行",跟升级前"上键=翻历史"完全一致;
            // 多行时规则统一("第一行再上=翻历史"),不用记"多行就翻不了
            // 历史"这种特例——翻走了 draft 也存着,Down 到底就回来,不丢字。
            if (row_ > 0) {
                --row_;
                col_ = std::min(col_, lines_[row_].size());
            } else {
                MoveHistory(true);
            }
            break;
        case KeyKind::Down:
            if (row_ + 1 < lines_.size()) {
                ++row_;
                col_ = std::min(col_, lines_[row_].size());
            } else if (lines_.size() == 1 && !lines_[0].empty() && lines_[0].front() == U'/') {
                // 0.16.0:单行、以 / 开头、候选非空——↓ 进入菜单选择态,选中
                // 第一条。候选为空(比如 /zzz)不进菜单,↓ 保持翻历史的老
                // 语义;多行 composer 里 / 是正文,走不到这个分支(上面的
                // 行间移动先接住了)。
                const std::u32string word = CurrentWord(lines_[0]);
                std::vector<std::string> matches = MatchingCandidateNames(word);
                if (!matches.empty()) {
                    menu_selection_ = MenuSelectionState{std::move(matches), 0, lines_[0].substr(word.size())};
                } else {
                    MoveHistory(false);
                }
            } else {
                MoveHistory(false);
            }
            break;
        case KeyKind::Tab:
            // 0.17.0 键位矫正:composer 整个为空时,Tab 进入焦点态、顺带发
            // 一个"选最近条目"的焦点请求(focus_move=+1,应用层起手落在最近
            // 一条上);之后的移动全在上面的焦点态分支里。有内容时补全/轮转
            // 职责一字不变。
            if (composer_ && lines_.size() == 1 && lines_[0].empty()) {
                focus_mode_ = true;
                RenderState state = BuildRenderState(false, false, false, false);
                state.focus_move = 1;
                return state;
            }
            HandleTab();
            break;
        case KeyKind::ShiftTab:
            // 0.17.0 键位矫正:Shift+Tab 任何时候都是切确认档(跟常驻状态行
            // 的 "shift+tab 切换" 提示一致)——UI-D 曾把空 composer 的这一下
            // 划给焦点导航,害得切档要先打个字,改回来。焦点态内的
            // Shift+Tab(往新走)在上面的焦点态分支里,走不到这儿。
            confirm_mode_ = NextConfirmMode(confirm_mode_);
            return BuildRenderState(false, false, false, true);
        case KeyKind::Enter: {
            const std::u32string joined = JoinLines(lines_);
            const std::u32string expanded = ExpandPasteTokens(joined);
            if (composer_ && AllWhitespace(expanded)) {
                break;  // UI-A:空 composer(全空白)不发送,原地不动
            }
            if (!expanded.empty()) {
                history_.push_back(expanded);
            }
            // Codex 的占位符只活在 composer：提交时展开，历史回显也留下
            // 全文。短 paste 本来就是明文；大 paste 到这里才从 token 还原。
            std::vector<std::u32string> submitted_lines = SplitJoined(expanded);
            ClearBuffer();
            ResetHistoryBrowsing();
            RenderState state = BuildRenderState(true, false, false, false);
            // 提交帧画的是"刚提交的完整内容"(终端层靠它把整段留在屏幕上),
            // 不是清空后的缓冲;光标摆到末行末尾,终端层从那儿换行接下文。
            state.lines = submitted_lines;
            state.cursor_row = submitted_lines.size() - 1;
            state.cursor_col = submitted_lines.back().size();
            state.line = expanded;
            state.cursor = expanded.size();
            state.cursor_display_col = DisplayWidth(submitted_lines.back());
            state.hint_lines.clear();
            return state;
        }
        case KeyKind::CtrlC: {
            if (lines_.size() == 1 && lines_[0].empty()) {
                return BuildRenderState(false, false, true, false);
            }
            // 非空:清空整个 composer(所有行),留在同一次 ReadLine 里继续。
            ClearBuffer();
            ResetHistoryBrowsing();
            return BuildRenderState(false, true, false, false);
        }
        case KeyKind::CtrlD:
            return BuildRenderState(false, false, true, false);
        case KeyKind::Delete:
            // Del 键:composer 编辑暂不理会(队列浏览的"删当前项"在
            // TurnInputListener 那条路上,不经过这里),显式空操作。
            return BuildRenderState(false, false, false, false);
        case KeyKind::Esc: {
            // M10:空闲编辑态清空整个 composer 和提示区,跟非空 Ctrl+C 是同
            // 一个效果(cleared=true,留在同一次 ReadLine 里继续等下一下按
            // 键),但空 composer 按 Esc 不像 Ctrl+C 那样触发 EOF——Esc 单纯
            // 是"清一下",不是"我要退出输入"。esc_pressed 额外标一下,终端层
            // 在"确认提示 [y/a/N]"那种读法下会看这个标志,把这一下直接当拒绝
            // 处理,不用非空清完还要用户再按一次 Enter。
            ClearBuffer();
            ResetHistoryBrowsing();
            RenderState state = BuildRenderState(false, true, false, false);
            state.esc_pressed = true;
            return state;
        }
        case KeyKind::CtrlO: {
            // UI-D:紧凑/详细全局切换请求。只在 composer(主提示符)下转发,
            // 确认提示、/model 选择这些单行读取不认这个键(什么都不发生)。
            RenderState state = BuildRenderState(false, false, false, false);
            if (composer_) {
                state.toggle_expand_requested = true;
            }
            return state;
        }
        case KeyKind::CtrlE: {
            // UI-D:聚焦查看请求,同上只在 composer 下转发。
            RenderState state = BuildRenderState(false, false, false, false);
            if (composer_) {
                state.focus_view_requested = true;
            }
            return state;
        }
    }

    return BuildRenderState(false, false, false, false);
}

std::vector<std::string> StreamSlashHintLines(const std::vector<CompletionCandidate>& candidates,
                                              const std::string& buffer_utf8) {
    // 出提示的门槛建在码点层:首字符是 '/'、整段没有空格。空串自然不过门。
    const std::u32string buffer = Utf8ToUtf32(buffer_utf8);
    if (buffer.empty() || buffer.front() != U'/' ||
        buffer.find(U' ') != std::u32string::npos) {
        return {};
    }

    // 匹配与 LineEditorCore::MatchingCandidateNames 同一规矩:ASCII 命令名
    // 大小写不敏感前缀匹配,命中按候选表原顺序摆。
    const std::string word_lower = ToLowerAscii(buffer_utf8);
    std::vector<std::string> matches;
    for (const auto& cand : candidates) {
        const std::string cand_lower = ToLowerAscii(cand.name);
        if (cand_lower.size() >= word_lower.size() &&
            cand_lower.compare(0, word_lower.size(), word_lower) == 0) {
            matches.push_back(cand.name);
        }
    }
    if (matches.empty()) {
        return {};
    }

    // 摆法跟 LineEditorCore::BuildHintLines(selected_index = -1) 一致:一行
    // "  /name  说明",封顶 6 行,超出加一行汇总;没有选中态,不出 "> " 行。
    constexpr std::size_t kMaxLines = 6;
    std::vector<std::string> lines;
    for (std::size_t i = 0; i < matches.size() && i < kMaxLines; ++i) {
        std::string line = "  ";
        line += matches[i];
        line += "  ";
        for (const auto& cand : candidates) {
            if (cand.name == matches[i]) {
                line += cand.description;
                break;
            }
        }
        lines.push_back(std::move(line));
    }
    if (matches.size() > kMaxLines) {
        lines.push_back(trf("ui.menu_more", matches.size()));
    }
    return lines;
}

}  // namespace lubancode::cli
