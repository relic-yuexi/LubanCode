// terminal 工具的最小终端仿真器(纯类,不碰任何 IO,两平台都编都测)。
//
// 吃 PTY 主端读出的字节流,维护一张 rows x cols 的字符网格,随时吐出
// "当前屏幕"的纯文本快照给模型看。目标只是让 bash 提示符、vim、less、
// python REPL、top 的画面看起来对,不追求完整 xterm:
//   - C0:CR / LF(VT、FF 同 LF)/ BS / TAB(8 列制表位)/ BEL 忽略;
//   - ESC:7/8 存取光标、D/E/M(IND/NEL/RI)、c 复位;字符集选择
//     ESC ( x 之类吞掉一个字节;OSC/DCS/APC/PM 串吞到 BEL 或 ST;
//   - CSI:CUU/CUD/CUF/CUB/CNL/CPL/CHA/HPA/VPA/CUP/HVP 光标移动,ED/EL
//     清屏清行,ICH/DCH/ECH/IL/DL 插删,SU/SD 滚屏,DECSTBM 滚动区,
//     s/u 存取光标,SGR 整个忽略(快照是纯文本);
//   - DECSET/DECRST:?1049/?1047/?47 备用屏,?7 自动换行,?1 光标键
//     应用模式(按键编码要用),其余忽略;
//   - 应答:DSR(CSI 5n / 6n)与 DA1(CSI c)生成回复字节,由会话写回
//     PTY——不答的话个别程序会傻等一会儿;DA2 等故意不答。
//   - UTF-8:跨 Feed 调用续接半截序列;坏字节记 U+FFFD。宽度取舍:CJK
//     统一表意、谚文、全角形式、常见 emoji 区按 2 列,组合附加符与零宽
//     字符按 0 列直接丢弃(不叠在前一格上),其余按 1 列。与真实 wcwidth
//     在边角码点上会有出入,快照里大不了错一两格,不影响读懂画面。
// 未知序列一律忽略,绝不抛异常;参数个数与数值都有上限,防恶意长串。
#pragma once

#include <cstddef>
#include <deque>
#include <string>
#include <string_view>
#include <vector>

namespace lubancode::tools {

class TerminalScreen {
public:
    // rows/cols 夹在 [1, 1000];scrollback_limit = 主屏滚出顶端的行最多留几行。
    TerminalScreen(int rows, int cols, std::size_t scrollback_limit = 2000);

    // 喂一段输出字节(可以在任意字节处切开,多字节/转义序列跨调用续接)。
    void Feed(std::string_view bytes);

    // 当前屏幕:每行去尾空格,去掉末尾整片空行,行间 '\n'。
    std::string Snapshot() const;
    // 当前屏幕逐行(去尾空格,不去空行,恰好 rows 行)。
    std::vector<std::string> Lines() const;
    // 主屏滚动历史最近 n 行(旧在前)。备用屏不进历史。
    std::vector<std::string> Scrollback(std::size_t n) const;

    // 取走并清空待回写 PTY 的应答字节(DSR/DA)。
    std::string TakeReplies();

    int rows() const { return rows_; }
    int cols() const { return cols_; }
    int cursor_row() const { return cur_row_; }  // 0 基
    int cursor_col() const { return cur_col_; }  // 0 基
    bool alternate_screen() const { return alt_active_; }
    bool app_cursor_keys() const { return app_cursor_keys_; }

private:
    struct Cell {
        char32_t ch = U' ';
        bool continuation = false;  // 宽字符右半格:快照时跳过
    };
    using Row = std::vector<Cell>;
    struct Buffer {
        std::vector<Row> grid;
    };
    struct SavedCursor {
        int row = 0;
        int col = 0;
    };
    enum class State { Ground, Escape, EscapeSkipOne, Csi, String, StringEsc };

    void PutGlyph(char32_t ch);
    void ExecuteControl(unsigned char c);
    void HandleEscape(unsigned char c);
    void HandleCsi(unsigned char final_byte);
    void SetMode(bool private_marker, bool enable);

    void LineFeed();
    void ReverseIndex();
    void ScrollUp(int top, int bottom, int n);
    void ScrollDown(int top, int bottom, int n);
    void ClearRow(Row& row, int from, int to);  // [from, to)
    void EnterAlternate(bool save_cursor, bool clear);
    void LeaveAlternate(bool restore_cursor);
    void Reset();
    Row BlankRow() const;
    Buffer& Active() { return alt_active_ ? alt_ : main_; }
    const Buffer& Active() const { return alt_active_ ? alt_ : main_; }
    int Param(std::size_t index, int fallback) const;
    static std::string RowText(const Row& row);

    int rows_;
    int cols_;
    std::size_t scrollback_limit_;
    Buffer main_;
    Buffer alt_;
    bool alt_active_ = false;
    std::deque<std::string> scrollback_;

    int cur_row_ = 0;
    int cur_col_ = 0;
    bool pending_wrap_ = false;  // 写满末列后延迟换行(xterm 语义)
    bool autowrap_ = true;
    bool app_cursor_keys_ = false;
    int scroll_top_ = 0;
    int scroll_bottom_ = 0;  // 含
    SavedCursor saved_{};
    SavedCursor alt_saved_{};

    State state_ = State::Ground;
    std::string csi_params_;
    bool csi_private_ = false;  // 参数串以 ? > < = 起头
    char csi_marker_ = 0;
    bool csi_overflow_ = false;
    bool csi_intermediate_ = false;  // 见过 0x20-0x2F 中间字节
    std::vector<int> params_;

    // UTF-8 续接
    char32_t utf8_acc_ = 0;
    int utf8_need_ = 0;
    char32_t utf8_min_ = 0;

    std::string replies_;
};

// 码点显示宽度(0/1/2),取舍见文件头。
int TerminalCharWidth(char32_t ch);

}  // namespace lubancode::tools
