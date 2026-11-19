#include "tools/terminal_screen.hpp"

#include <algorithm>
#include <array>
#include <utility>

namespace lubancode::tools {

namespace {

constexpr std::size_t kMaxCsiParamBytes = 64;  // 参数串上限:超了整条序列作废
constexpr int kMaxParamValue = 9999;           // 单个参数夹到这里,防 CSI 99999999A 之类
constexpr char32_t kReplacementChar = 0xFFFD;  // U+FFFD:坏 UTF-8 的替换符

struct Range {
    char32_t lo;
    char32_t hi;
};

// 零宽:组合附加符、零宽空格/连接符、变体选择符、肤色修饰符等。
constexpr std::array<Range, 14> kZeroWidth{{
    {0x0300, 0x036F}, {0x0483, 0x0489}, {0x0591, 0x05BD}, {0x0610, 0x061A}, {0x064B, 0x065F},
    {0x200B, 0x200F}, {0x2028, 0x202E}, {0x2060, 0x2064}, {0x20D0, 0x20FF}, {0xFE00, 0xFE0F},
    {0xFE20, 0xFE2F}, {0xFEFF, 0xFEFF}, {0x1F3FB, 0x1F3FF}, {0xE0100, 0xE01EF},
}};

// 双宽:东亚宽字符与常见 emoji 区(粗粒度,取舍见头文件)。
constexpr std::array<Range, 30> kWide{{
    {0x1100, 0x115F},   {0x231A, 0x231B},   {0x2329, 0x232A},   {0x23E9, 0x23EC},   {0x23F0, 0x23F3},
    {0x25FD, 0x25FE},   {0x2614, 0x2615},   {0x2648, 0x2653},   {0x26A1, 0x26A1},   {0x26BD, 0x26BE},
    {0x2705, 0x2705},   {0x274C, 0x274C},   {0x2B50, 0x2B55},   {0x2E80, 0x303E},   {0x3041, 0x33FF},
    {0x3400, 0x4DBF},   {0x4E00, 0x9FFF},   {0xA000, 0xA4CF},   {0xA960, 0xA97F},   {0xAC00, 0xD7A3},
    {0xF900, 0xFAFF},   {0xFE10, 0xFE19},   {0xFE30, 0xFE6F},   {0xFF00, 0xFF60},   {0xFFE0, 0xFFE6},
    {0x1F300, 0x1F64F}, {0x1F680, 0x1F6FF}, {0x1F900, 0x1F9FF}, {0x1FA70, 0x1FAFF}, {0x20000, 0x3FFFD},
}};

template <std::size_t N>
bool InRanges(const std::array<Range, N>& ranges, char32_t ch) {
    return std::any_of(ranges.begin(), ranges.end(), [ch](const Range& r) { return ch >= r.lo && ch <= r.hi; });
}

void AppendUtf8(std::string& out, char32_t ch) {
    if (ch < 0x80) {
        out.push_back(static_cast<char>(ch));
    } else if (ch < 0x800) {
        out.push_back(static_cast<char>(0xC0 | (ch >> 6)));
        out.push_back(static_cast<char>(0x80 | (ch & 0x3F)));
    } else if (ch < 0x10000) {
        out.push_back(static_cast<char>(0xE0 | (ch >> 12)));
        out.push_back(static_cast<char>(0x80 | ((ch >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (ch & 0x3F)));
    } else {
        out.push_back(static_cast<char>(0xF0 | (ch >> 18)));
        out.push_back(static_cast<char>(0x80 | ((ch >> 12) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | ((ch >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (ch & 0x3F)));
    }
}

}  // namespace

int TerminalCharWidth(char32_t ch) {
    if (ch < 0x20 || (ch >= 0x7F && ch < 0xA0)) {
        return 0;
    }
    if (InRanges(kZeroWidth, ch)) {
        return 0;
    }
    if (InRanges(kWide, ch)) {
        return 2;
    }
    return 1;
}

TerminalScreen::TerminalScreen(int rows, int cols, std::size_t scrollback_limit)
    : rows_(std::clamp(rows, 1, 1000)), cols_(std::clamp(cols, 1, 1000)), scrollback_limit_(scrollback_limit) {
    Reset();
}

TerminalScreen::Row TerminalScreen::BlankRow() const {
    return Row(static_cast<std::size_t>(cols_));
}

void TerminalScreen::Reset() {
    main_.grid.assign(static_cast<std::size_t>(rows_), BlankRow());
    alt_.grid.assign(static_cast<std::size_t>(rows_), BlankRow());
    alt_active_ = false;
    cur_row_ = 0;
    cur_col_ = 0;
    pending_wrap_ = false;
    autowrap_ = true;
    app_cursor_keys_ = false;
    scroll_top_ = 0;
    scroll_bottom_ = rows_ - 1;
    saved_ = {};
    alt_saved_ = {};
}

void TerminalScreen::Feed(std::string_view bytes) {
    for (const char raw : bytes) {
        const auto b = static_cast<unsigned char>(raw);
        // 用小循环支持"重新处理当前字节"(半截 UTF-8 被打断、串里出现新 ESC)。
        bool reprocess = true;
        while (reprocess) {
            reprocess = false;
            switch (state_) {
                case State::Ground: {
                    if (utf8_need_ > 0) {
                        if ((b & 0xC0) == 0x80) {
                            utf8_acc_ = (utf8_acc_ << 6) | (b & 0x3F);
                            if (--utf8_need_ == 0) {
                                const bool valid = utf8_acc_ >= utf8_min_ && utf8_acc_ <= 0x10FFFF &&
                                                   !(utf8_acc_ >= 0xD800 && utf8_acc_ <= 0xDFFF);
                                PutGlyph(valid ? utf8_acc_ : kReplacementChar);
                            }
                            break;
                        }
                        // 序列被打断:前半截记一个替换符,当前字节重新从头处理。
                        utf8_need_ = 0;
                        PutGlyph(kReplacementChar);
                    }
                    if (b < 0x20) {
                        ExecuteControl(b);
                    } else if (b == 0x7F) {
                        // DEL:忽略
                    } else if (b < 0x80) {
                        PutGlyph(b);
                    } else if (b >= 0xC2 && b <= 0xDF) {
                        utf8_need_ = 1;
                        utf8_acc_ = b & 0x1F;
                        utf8_min_ = 0x80;
                    } else if (b >= 0xE0 && b <= 0xEF) {
                        utf8_need_ = 2;
                        utf8_acc_ = b & 0x0F;
                        utf8_min_ = 0x800;
                    } else if (b >= 0xF0 && b <= 0xF4) {
                        utf8_need_ = 3;
                        utf8_acc_ = b & 0x07;
                        utf8_min_ = 0x10000;
                    } else {
                        PutGlyph(kReplacementChar);
                    }
                    break;
                }
                case State::Escape:
                    state_ = State::Ground;
                    if (b == '[') {
                        state_ = State::Csi;
                        csi_params_.clear();
                        csi_private_ = false;
                        csi_marker_ = 0;
                        csi_overflow_ = false;
                        csi_intermediate_ = false;
                    } else if (b == ']' || b == 'P' || b == '_' || b == '^' || b == 'X') {
                        state_ = State::String;  // OSC/DCS/APC/PM/SOS:吞到 BEL 或 ST
                    } else if (b == '(' || b == ')' || b == '*' || b == '+' || b == '-' || b == '.' || b == '/' ||
                               b == '#' || b == '%' || b == ' ') {
                        state_ = State::EscapeSkipOne;  // 字符集/行属性选择:再吞一个字节
                    } else if (b == 0x1B) {
                        state_ = State::Escape;
                    } else if (b < 0x20) {
                        ExecuteControl(b);  // ESC 后跟 C0:照常执行,ESC 作废
                    } else {
                        HandleEscape(b);
                    }
                    break;
                case State::EscapeSkipOne:
                    state_ = State::Ground;
                    break;
                case State::Csi:
                    if (b == 0x1B) {
                        state_ = State::Escape;  // 序列中途来新 ESC:旧的作废
                    } else if (b < 0x20) {
                        ExecuteControl(b);  // VT 语义:CSI 里夹的 C0 照常执行
                    } else if (b >= 0x30 && b <= 0x3F) {
                        if (csi_params_.empty() && !csi_private_ && (b == '?' || b == '>' || b == '<' || b == '=')) {
                            csi_private_ = true;
                            csi_marker_ = static_cast<char>(b);
                        } else if (csi_params_.size() < kMaxCsiParamBytes) {
                            csi_params_.push_back(static_cast<char>(b));
                        } else {
                            csi_overflow_ = true;
                        }
                    } else if (b >= 0x20 && b <= 0x2F) {
                        csi_intermediate_ = true;  // 带中间字节的(CSI SP q 光标形状等)整条忽略
                    } else if (b >= 0x40 && b <= 0x7E) {
                        state_ = State::Ground;
                        if (!csi_overflow_ && !csi_intermediate_) {
                            HandleCsi(b);
                        }
                    }
                    break;
                case State::String:
                    if (b == 0x07) {
                        state_ = State::Ground;
                    } else if (b == 0x1B) {
                        state_ = State::StringEsc;
                    }
                    break;
                case State::StringEsc:
                    if (b == '\\') {
                        state_ = State::Ground;  // ST
                    } else {
                        state_ = State::Escape;  // 不是 ST:串结束,ESC 另起一条序列
                        reprocess = true;
                    }
                    break;
            }
        }
    }
}

void TerminalScreen::ExecuteControl(unsigned char c) {
    switch (c) {
        case 0x08:  // BS
            pending_wrap_ = false;
            if (cur_col_ > 0) {
                --cur_col_;
            }
            break;
        case 0x09:  // TAB:下一个 8 列制表位
            pending_wrap_ = false;
            cur_col_ = std::min(cols_ - 1, (cur_col_ / 8 + 1) * 8);
            break;
        case 0x0A:
        case 0x0B:
        case 0x0C:
            LineFeed();
            break;
        case 0x0D:
            pending_wrap_ = false;
            cur_col_ = 0;
            break;
        case 0x1B:
            state_ = State::Escape;
            break;
        default:
            break;  // BEL、SO/SI 等:忽略
    }
}

void TerminalScreen::HandleEscape(unsigned char c) {
    switch (c) {
        case '7':
            saved_ = {cur_row_, cur_col_};
            break;
        case '8':
            cur_row_ = std::clamp(saved_.row, 0, rows_ - 1);
            cur_col_ = std::clamp(saved_.col, 0, cols_ - 1);
            pending_wrap_ = false;
            break;
        case 'D':
            LineFeed();
            break;
        case 'E':
            cur_col_ = 0;
            LineFeed();
            break;
        case 'M':
            ReverseIndex();
            break;
        case 'c':
            Reset();
            break;
        default:
            break;  // ESC = / ESC > 小键盘模式、ESC H 设制表位等:忽略
    }
}

int TerminalScreen::Param(std::size_t index, int fallback) const {
    if (index >= params_.size() || params_[index] < 0) {
        return fallback;
    }
    return params_[index];
}

void TerminalScreen::HandleCsi(unsigned char final_byte) {
    // 解析参数:';' 分隔,':' 子参数只取首段,空位记 -1(=缺省)。
    params_.clear();
    {
        int value = -1;
        bool in_sub = false;
        for (const char ch : csi_params_) {
            if (ch == ';') {
                params_.push_back(value);
                value = -1;
                in_sub = false;
            } else if (ch == ':') {
                in_sub = true;
            } else if (ch >= '0' && ch <= '9' && !in_sub) {
                value = std::min(kMaxParamValue, (value < 0 ? 0 : value) * 10 + (ch - '0'));
            }
            if (params_.size() > 32) {
                return;  // 参数个数离谱:整条忽略
            }
        }
        params_.push_back(value);
    }

    if (csi_private_) {
        if (csi_marker_ == '?' && (final_byte == 'h' || final_byte == 'l')) {
            SetMode(true, final_byte == 'h');
        }
        return;  // CSI > c(DA2)、CSI ? u 等私有查询:不答
    }

    const auto count = [this](std::size_t i) { return std::max(1, Param(i, 1)); };
    if (final_byte != 'm' && final_byte != 'n' && final_byte != 'c' && final_byte != 'h' && final_byte != 'l') {
        pending_wrap_ = false;
    }
    Row& row = Active().grid[static_cast<std::size_t>(cur_row_)];
    switch (final_byte) {
        case 'A': {  // CUU
            const int lo = cur_row_ >= scroll_top_ ? scroll_top_ : 0;
            cur_row_ = std::max(lo, cur_row_ - count(0));
            break;
        }
        case 'B':
        case 'e': {  // CUD / VPR
            const int hi = cur_row_ <= scroll_bottom_ ? scroll_bottom_ : rows_ - 1;
            cur_row_ = std::min(hi, cur_row_ + count(0));
            break;
        }
        case 'C':
        case 'a':  // CUF / HPR
            cur_col_ = std::min(cols_ - 1, cur_col_ + count(0));
            break;
        case 'D':  // CUB
            cur_col_ = std::max(0, cur_col_ - count(0));
            break;
        case 'E': {  // CNL
            const int hi = cur_row_ <= scroll_bottom_ ? scroll_bottom_ : rows_ - 1;
            cur_row_ = std::min(hi, cur_row_ + count(0));
            cur_col_ = 0;
            break;
        }
        case 'F': {  // CPL
            const int lo = cur_row_ >= scroll_top_ ? scroll_top_ : 0;
            cur_row_ = std::max(lo, cur_row_ - count(0));
            cur_col_ = 0;
            break;
        }
        case 'G':
        case '`':  // CHA / HPA
            cur_col_ = std::clamp(count(0) - 1, 0, cols_ - 1);
            break;
        case 'd':  // VPA
            cur_row_ = std::clamp(count(0) - 1, 0, rows_ - 1);
            break;
        case 'H':
        case 'f':  // CUP / HVP
            cur_row_ = std::clamp(count(0) - 1, 0, rows_ - 1);
            cur_col_ = std::clamp(count(1) - 1, 0, cols_ - 1);
            break;
        case 'J': {  // ED
            auto& grid = Active().grid;
            const int mode = Param(0, 0);
            if (mode == 0) {
                ClearRow(row, cur_col_, cols_);
                for (int r = cur_row_ + 1; r < rows_; ++r) {
                    ClearRow(grid[static_cast<std::size_t>(r)], 0, cols_);
                }
            } else if (mode == 1) {
                for (int r = 0; r < cur_row_; ++r) {
                    ClearRow(grid[static_cast<std::size_t>(r)], 0, cols_);
                }
                ClearRow(row, 0, cur_col_ + 1);
            } else if (mode == 2 || mode == 3) {
                for (auto& r : grid) {
                    ClearRow(r, 0, cols_);
                }
                if (mode == 3 && !alt_active_) {
                    scrollback_.clear();
                }
            }
            break;
        }
        case 'K': {  // EL
            const int mode = Param(0, 0);
            if (mode == 0) {
                ClearRow(row, cur_col_, cols_);
            } else if (mode == 1) {
                ClearRow(row, 0, cur_col_ + 1);
            } else if (mode == 2) {
                ClearRow(row, 0, cols_);
            }
            break;
        }
        case '@': {  // ICH:光标处插空格,右侧顺移,挤出行尾的丢弃
            const int n = std::min(count(0), cols_ - cur_col_);
            row.insert(row.begin() + cur_col_, static_cast<std::size_t>(n), Cell{});
            row.resize(static_cast<std::size_t>(cols_));
            break;
        }
        case 'P': {  // DCH:删光标处字符,左移,行尾补空
            const int n = std::min(count(0), cols_ - cur_col_);
            row.erase(row.begin() + cur_col_, row.begin() + cur_col_ + n);
            row.resize(static_cast<std::size_t>(cols_));
            break;
        }
        case 'X':  // ECH
            ClearRow(row, cur_col_, std::min(cols_, cur_col_ + count(0)));
            break;
        case 'L':  // IL
            if (cur_row_ >= scroll_top_ && cur_row_ <= scroll_bottom_) {
                ScrollDown(cur_row_, scroll_bottom_, count(0));
                cur_col_ = 0;
            }
            break;
        case 'M':  // DL
            if (cur_row_ >= scroll_top_ && cur_row_ <= scroll_bottom_) {
                ScrollUp(cur_row_, scroll_bottom_, count(0));
                cur_col_ = 0;
            }
            break;
        case 'S':  // SU
            ScrollUp(scroll_top_, scroll_bottom_, count(0));
            break;
        case 'T':  // SD(五参数版是鼠标高亮跟踪,不理)
            if (params_.size() <= 1) {
                ScrollDown(scroll_top_, scroll_bottom_, count(0));
            }
            break;
        case 'r': {  // DECSTBM
            const int top = count(0) - 1;
            const int bottom = Param(1, rows_) <= 0 ? rows_ - 1 : std::min(rows_, Param(1, rows_)) - 1;
            if (top < bottom) {
                scroll_top_ = top;
                scroll_bottom_ = bottom;
                cur_row_ = 0;
                cur_col_ = 0;
            }
            break;
        }
        case 's':
            saved_ = {cur_row_, cur_col_};
            break;
        case 'u':
            cur_row_ = std::clamp(saved_.row, 0, rows_ - 1);
            cur_col_ = std::clamp(saved_.col, 0, cols_ - 1);
            break;
        case 'n':  // DSR
            if (Param(0, 0) == 5) {
                replies_ += "\x1b[0n";
            } else if (Param(0, 0) == 6) {
                replies_ += "\x1b[" + std::to_string(cur_row_ + 1) + ";" + std::to_string(cur_col_ + 1) + "R";
            }
            break;
        case 'c':  // DA1:报 VT100 带 AVO
            if (Param(0, 0) == 0) {
                replies_ += "\x1b[?1;2c";
            }
            break;
        default:
            break;  // SGR(m)、IRM(4h)、制表、窗口操作等:忽略
    }
}

void TerminalScreen::SetMode(bool private_marker, bool enable) {
    if (!private_marker) {
        return;
    }
    for (const int p : params_) {
        switch (p) {
            case 1049:
                if (enable) {
                    EnterAlternate(true, true);
                } else {
                    LeaveAlternate(true);
                }
                break;
            case 1047:
            case 47:
                if (enable) {
                    EnterAlternate(false, p == 1047);
                } else {
                    LeaveAlternate(false);
                }
                break;
            case 7:
                autowrap_ = enable;
                if (!enable) {
                    pending_wrap_ = false;
                }
                break;
            case 1:
                app_cursor_keys_ = enable;
                break;
            default:
                break;  // ?25 光标显隐、?2004 括号粘贴、?1000 鼠标等:忽略
        }
    }
}

void TerminalScreen::EnterAlternate(bool save_cursor, bool clear) {
    if (alt_active_) {
        return;
    }
    if (save_cursor) {
        alt_saved_ = {cur_row_, cur_col_};
    }
    alt_active_ = true;
    if (clear) {
        alt_.grid.assign(static_cast<std::size_t>(rows_), BlankRow());
    }
    pending_wrap_ = false;
}

void TerminalScreen::LeaveAlternate(bool restore_cursor) {
    if (!alt_active_) {
        return;
    }
    alt_active_ = false;
    if (restore_cursor) {
        cur_row_ = std::clamp(alt_saved_.row, 0, rows_ - 1);
        cur_col_ = std::clamp(alt_saved_.col, 0, cols_ - 1);
    }
    pending_wrap_ = false;
}

void TerminalScreen::ClearRow(Row& row, int from, int to) {
    from = std::clamp(from, 0, cols_);
    to = std::clamp(to, 0, cols_);
    if (from >= to) {
        return;
    }
    // 宽字符被劈开半边:另一半也清掉,免得快照里留孤儿。
    if (from > 0 && row[static_cast<std::size_t>(from)].continuation) {
        row[static_cast<std::size_t>(from - 1)] = Cell{};
    }
    if (to < cols_ && row[static_cast<std::size_t>(to)].continuation) {
        row[static_cast<std::size_t>(to)] = Cell{};
    }
    for (int c = from; c < to; ++c) {
        row[static_cast<std::size_t>(c)] = Cell{};
    }
}

void TerminalScreen::LineFeed() {
    pending_wrap_ = false;
    if (cur_row_ == scroll_bottom_) {
        // 主屏整屏滚动时,顶行进历史;备用屏/局部滚动区不进。
        if (!alt_active_ && scroll_top_ == 0) {
            scrollback_.push_back(RowText(main_.grid[0]));
            while (scrollback_.size() > scrollback_limit_) {
                scrollback_.pop_front();
            }
        }
        ScrollUp(scroll_top_, scroll_bottom_, 1);
    } else if (cur_row_ < rows_ - 1) {
        ++cur_row_;
    }
}

void TerminalScreen::ReverseIndex() {
    pending_wrap_ = false;
    if (cur_row_ == scroll_top_) {
        ScrollDown(scroll_top_, scroll_bottom_, 1);
    } else if (cur_row_ > 0) {
        --cur_row_;
    }
}

void TerminalScreen::ScrollUp(int top, int bottom, int n) {
    auto& grid = Active().grid;
    n = std::min(n, bottom - top + 1);
    for (int i = 0; i < n; ++i) {
        grid.erase(grid.begin() + top);
        grid.insert(grid.begin() + bottom, BlankRow());
    }
}

void TerminalScreen::ScrollDown(int top, int bottom, int n) {
    auto& grid = Active().grid;
    n = std::min(n, bottom - top + 1);
    for (int i = 0; i < n; ++i) {
        grid.erase(grid.begin() + bottom);
        grid.insert(grid.begin() + top, BlankRow());
    }
}

void TerminalScreen::PutGlyph(char32_t ch) {
    int width = TerminalCharWidth(ch);
    if (width == 0) {
        return;  // 组合符/零宽:丢弃(取舍见头文件)
    }
    if (width == 2 && cols_ < 2) {
        width = 1;
    }
    if (pending_wrap_) {
        pending_wrap_ = false;
        cur_col_ = 0;
        LineFeed();
    }
    if (width == 2 && cur_col_ == cols_ - 1) {
        // 末列放不下宽字符:能折就折到下一行,不能折就退一格硬塞。
        if (autowrap_) {
            Active().grid[static_cast<std::size_t>(cur_row_)][static_cast<std::size_t>(cur_col_)] = Cell{};
            cur_col_ = 0;
            LineFeed();
        } else {
            cur_col_ = cols_ - 2;
        }
    }
    Row& row = Active().grid[static_cast<std::size_t>(cur_row_)];
    const auto col = static_cast<std::size_t>(cur_col_);
    // 覆盖宽字符的任一半:把另一半也清成空格。
    if (row[col].continuation && col > 0) {
        row[col - 1] = Cell{};
    }
    const std::size_t span_end = col + static_cast<std::size_t>(width);
    if (span_end < row.size() && row[span_end].continuation) {
        row[span_end] = Cell{};
    }
    row[col] = Cell{ch, false};
    if (width == 2) {
        row[col + 1] = Cell{U' ', true};
    }
    cur_col_ += width;
    if (cur_col_ >= cols_) {
        cur_col_ = cols_ - 1;
        pending_wrap_ = autowrap_;
    }
}

std::string TerminalScreen::RowText(const Row& row) {
    std::string out;
    for (const Cell& cell : row) {
        if (cell.continuation) {
            continue;
        }
        AppendUtf8(out, cell.ch);
    }
    while (!out.empty() && out.back() == ' ') {
        out.pop_back();
    }
    return out;
}

std::vector<std::string> TerminalScreen::Lines() const {
    std::vector<std::string> out;
    out.reserve(static_cast<std::size_t>(rows_));
    for (const Row& row : Active().grid) {
        out.push_back(RowText(row));
    }
    return out;
}

std::string TerminalScreen::Snapshot() const {
    std::vector<std::string> lines = Lines();
    while (!lines.empty() && lines.back().empty()) {
        lines.pop_back();
    }
    std::string out;
    for (std::size_t i = 0; i < lines.size(); ++i) {
        if (i > 0) {
            out.push_back('\n');
        }
        out += lines[i];
    }
    return out;
}

std::vector<std::string> TerminalScreen::Scrollback(std::size_t n) const {
    const std::size_t take = std::min(n, scrollback_.size());
    return std::vector<std::string>(scrollback_.end() - static_cast<std::ptrdiff_t>(take), scrollback_.end());
}

std::string TerminalScreen::TakeReplies() {
    return std::exchange(replies_, std::string());
}

}  // namespace lubancode::tools
