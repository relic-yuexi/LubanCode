#include "cli/line_editor.hpp"

#include <algorithm>
#include <cctype>

namespace lubancode::cli {

namespace {

// 手写 UTF-8 -> UTF-32 解码,只给 TruncateUtf8ToDisplayWidth 内部用。非法
// 起始字节、序列被截断、续字节不是 10xxxxxx 这几种情况一律跳过一个字节
// 继续——这是给自己代码拼出来的字符串(候选名 + 中文说明)截断用,不是
// 拿来校验外部不可信输入的严格解码器。
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
            extra = 0;
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
            ++i;
            continue;
        }
        if (i + extra >= n) {
            break;  // 序列被截断,到此为止
        }
        bool ok = true;
        char32_t decoded = cp;
        for (std::size_t k = 1; k <= extra; ++k) {
            const unsigned char ck = static_cast<unsigned char>(text[i + k]);
            if ((ck & 0xC0) != 0x80) {
                ok = false;
                break;
            }
            decoded = (decoded << 6) | (ck & 0x3F);
        }
        if (!ok) {
            ++i;
            continue;
        }
        out.push_back(decoded);
        i += extra + 1;
    }
    return out;
}

std::u32string CurrentWord(const std::u32string& line) {
    const std::size_t space_pos = line.find(U' ');
    return space_pos == std::u32string::npos ? line : line.substr(0, space_pos);
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
            return "确认";
        case ConfirmMode::Auto:
            return "auto";
        case ConfirmMode::Yolo:
            return "yolo";
    }
    return "确认";
}

std::string ConfirmModePromptPrefix(ConfirmMode mode) {
    switch (mode) {
        case ConfirmMode::Confirm:
            return "";
        case ConfirmMode::Auto:
            return "[auto] ";
        case ConfirmMode::Yolo:
            return "[yolo] ";
    }
    return "";
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

void LineEditorCore::BeginLine() {
    line_.clear();
    cursor_ = 0;
    tab_cycle_.reset();
    ResetHistoryBrowsing();
}

void LineEditorCore::ResetHistoryBrowsing() {
    history_index_.reset();
    draft_.clear();
}

void LineEditorCore::InsertChar(char32_t ch) {
    line_.insert(line_.begin() + static_cast<std::ptrdiff_t>(cursor_), ch);
    ++cursor_;
    ResetHistoryBrowsing();
}

void LineEditorCore::DeleteBackward() {
    if (cursor_ == 0) {
        return;
    }
    line_.erase(line_.begin() + static_cast<std::ptrdiff_t>(cursor_ - 1));
    --cursor_;
    ResetHistoryBrowsing();
}

void LineEditorCore::MoveHistory(bool up) {
    if (up) {
        if (history_.empty()) {
            return;
        }
        if (!history_index_.has_value()) {
            draft_ = line_;
            history_index_ = 0;
        } else if (*history_index_ + 1 < history_.size()) {
            ++(*history_index_);
        } else {
            return;  // 已经翻到最老的一条,到头了
        }
        line_ = history_[history_.size() - 1 - *history_index_];
        cursor_ = line_.size();
    } else {
        if (!history_index_.has_value()) {
            return;  // 已经在底部,没什么好往下翻的
        }
        if (*history_index_ == 0) {
            line_ = draft_;
            history_index_.reset();
        } else {
            --(*history_index_);
            line_ = history_[history_.size() - 1 - *history_index_];
        }
        cursor_ = line_.size();
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
    const std::u32string name32 = AsciiToU32(name);
    line_ = suffix.empty() ? (name32 + U' ') : (name32 + suffix);
    cursor_ = line_.size();
}

void LineEditorCore::HandleTab() {
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

    if (line_.empty() || line_.front() != U'/') {
        return;  // 不是 slash 命令,Tab 什么都不做
    }
    const std::u32string word = CurrentWord(line_);
    if (cursor_ > word.size()) {
        return;  // 光标已经越过命令词、落在参数区了,不补全
    }
    const std::u32string suffix = line_.substr(word.size());  // 词后面剩下的部分,原样保留

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
        line_ = lcp + suffix;
        cursor_ = lcp.size();
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
    state.line = line_;
    state.cursor = cursor_;
    state.cursor_display_col = DisplayWidth(line_.substr(0, cursor_));
    state.submitted = submitted;
    state.cleared = cleared;
    state.eof_requested = eof_requested;
    state.mode_changed = mode_changed;
    state.mode = confirm_mode_;

    if (!line_.empty() && line_.front() == U'/') {
        // 正在轮转(tab_cycle_ 有值)时,候选名单和"选中第几个"直接用这次
        // Tab 会话存下来的那份,不能拿当前行现算——轮转途中 line_ 已经被
        // 换成候选名本身,重新按 CurrentWord(line_) 去匹配只会匹配出它
        // 自己一个,候选名单会跟着"塌缩成一个",连带轮转标记也没地方标。
        std::vector<std::string> matches;
        int selected_index = -1;
        if (tab_cycle_.has_value()) {
            matches = tab_cycle_->matches;
            selected_index = tab_cycle_->index;
        } else {
            const std::u32string word = CurrentWord(line_);
            matches = MatchingCandidateNames(word);
        }
        state.hint_lines = BuildHintLines(matches, selected_index);
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
    const std::size_t shown = std::min(matches.size(), kMaxLines);
    for (std::size_t i = 0; i < shown; ++i) {
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
        lines.push_back("  … 共 " + std::to_string(matches.size()) + " 个命令");
    }
    return lines;
}

RenderState LineEditorCore::CurrentRenderState() const {
    return BuildRenderState(false, false, false, false);
}

RenderState LineEditorCore::HandleKey(const KeyEvent& event) {
    if (event.kind != KeyKind::Tab) {
        tab_cycle_.reset();
    }

    switch (event.kind) {
        case KeyKind::Char:
            InsertChar(event.ch);
            break;
        case KeyKind::Backspace:
            DeleteBackward();
            break;
        case KeyKind::Left:
            if (cursor_ > 0) {
                --cursor_;
            }
            break;
        case KeyKind::Right:
            if (cursor_ < line_.size()) {
                ++cursor_;
            }
            break;
        case KeyKind::Home:
            cursor_ = 0;
            break;
        case KeyKind::End:
            cursor_ = line_.size();
            break;
        case KeyKind::Up:
            MoveHistory(true);
            break;
        case KeyKind::Down:
            MoveHistory(false);
            break;
        case KeyKind::Tab:
            HandleTab();
            break;
        case KeyKind::ShiftTab:
            confirm_mode_ = NextConfirmMode(confirm_mode_);
            return BuildRenderState(false, false, false, true);
        case KeyKind::Enter: {
            const std::u32string submitted_line = line_;
            if (!submitted_line.empty()) {
                history_.push_back(submitted_line);
            }
            line_.clear();
            cursor_ = 0;
            ResetHistoryBrowsing();
            RenderState state = BuildRenderState(true, false, false, false);
            state.line = submitted_line;
            state.cursor = submitted_line.size();
            state.cursor_display_col = DisplayWidth(submitted_line);
            state.hint_lines.clear();
            return state;
        }
        case KeyKind::CtrlC: {
            if (line_.empty()) {
                return BuildRenderState(false, false, true, false);
            }
            line_.clear();
            cursor_ = 0;
            ResetHistoryBrowsing();
            return BuildRenderState(false, true, false, false);
        }
        case KeyKind::CtrlD:
            return BuildRenderState(false, false, true, false);
    }

    return BuildRenderState(false, false, false, false);
}

}  // namespace lubancode::cli
