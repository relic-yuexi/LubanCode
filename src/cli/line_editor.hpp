// 逐键输入编辑器 —— 核心层:纯逻辑,吃抽象 KeyEvent、吐 RenderState,不碰
// 任何 Win32 API / 真实控制台,可以脱离终端单测。真正读键盘、按 RenderState
// 重画屏幕的活儿在终端层(console_input.cpp)。
//
// 设计成一个"跨整条交互会话存活"的对象:main.cpp 那条 `> ` 主循环、工具
// 确认提示、/model 选择、初次配置向导,全部经同一个 cli::ReadLine 入口,
// 底下共用同一个 LineEditorCore 实例(见 console_input.cpp 里的
// SharedEditor())。这样两件"会话级"的事才有地方放:
//   - 历史列表跨多轮 ReadLine 保留,上下键翻;翻到一半又开始敲字符/退格,
//     视为"就地编辑这条历史记录",历史索引落回"底部"(下次 Up 键重新从
//     最新一条历史开始翻,不接着刚才那条继续走)。
//   - 确认模式(ConfirmMode)是会话级状态,Shift+Tab 循环切换,同样跨多轮
//     ReadLine 保留。核心层完全不知道"确认模式"在应用层被用来干什么
//     (自动放行哪些工具),只管把这个三态开关轮转好、在 RenderState 里
//     告诉上层"这一下是不是切换动作、切到哪一档了"。

#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace lubancode::cli {

// 抽象按键事件,终端层负责把真实的 ReadConsoleInputW KEY_EVENT_RECORD
// 翻译成这个,核心层只认这个,不知道底下是 Win32 API。
enum class KeyKind {
    Char,  // 可打印字符,ch 有效(按 Unicode 码点算,不是字节、也不是 UTF-16 code unit)
    Backspace,
    Left,
    Right,
    Home,
    End,
    Up,
    Down,
    Tab,
    ShiftTab,
    Enter,
    CtrlC,
    CtrlD,
    Esc,  // M10:ESC 打断/清行。终端层只在真控制台下(VK_ESCAPE)产出这个
};

struct KeyEvent {
    KeyKind kind;
    char32_t ch = 0;  // 只有 kind == Char 时有意义

    static KeyEvent Char(char32_t c) { return KeyEvent{KeyKind::Char, c}; }
    static KeyEvent Simple(KeyKind k) { return KeyEvent{k, 0}; }
};

// Shift+Tab 循环切换的三档会话级确认模式,main.cpp 的工具确认回调按这个
// 分流:
//   Confirm(默认) —— needs_confirm 的工具逐个问(现状)
//   Auto           —— write_file/edit_file 自动放行,run_command 仍问
//   Yolo           —— 全自动(等价 --yes)
enum class ConfirmMode { Confirm, Auto, Yolo };

// 滚动切到下一档:Confirm -> Auto -> Yolo -> Confirm。单独导出成纯函数,
// 方便不经过 LineEditorCore 也能测这一条轮转规则。
ConfirmMode NextConfirmMode(ConfirmMode mode);

// 中文说法(切换通知用)、提示符前缀(`> ` 前面加的那截)。都是纯函数,
// 终端层拼提示符/通知文字用,不用各处各写一份。
std::string ConfirmModeLabel(ConfirmMode mode);
std::string ConfirmModePromptPrefix(ConfirmMode mode);

// 补全候选:slash 命令名 + 一句话说明,由调用方(console_input.cpp)从
// cli::slash_commands 现有的命令定义转换而来,核心层自己不认得任何具体
// 命令名字,不写第二份命令清单。
struct CompletionCandidate {
    std::string name;         // 完整命令词,比如 "/model"
    std::string description;  // 一句话说明,提示行展示用
};

// 简易 East Asian Width 判定:>= 0x1100 起,常用的 CJK 统一表意文字、
// 假名、韩文音节、全角标点这些区段按显示宽度 2 算,其余按 1 算。不是一份
// 完整的 Unicode East Asian Width 表(生僻扩展区、emoji 宽字符没覆盖),
// 对日常中文/日文/韩文终端交互场景够用——这是刻意的取舍:真要做全,该去
// 啃 Unicode UAX #11 那张完整宽度表,对一个 CLI 的光标定位需求性价比不高。
int CharDisplayWidth(char32_t codepoint);
std::size_t DisplayWidth(const std::u32string& text);

// 手写 UTF-32 -> UTF-8 编码,不依赖 Win32 API,核心层和终端层共用
// (核心层的 RenderState::line 按码点存,真要写到控制台/拼回 std::string
// 结果时需要转回 UTF-8)。
std::string Utf32ToUtf8(const std::u32string& text);

// 按显示宽度截断:从头开始累加每个字符的显示宽度,一旦下一个字符会让
// 累计宽度超过 max_width 就整个不要那个字符——绝不会把一个占 2 列的宽字符
// 切成半个字宽。max_width <= 0 给空串。终端层"保证物理上永不折行"的截断
// 落在这个纯函数上,可以脱离控制台单测。
std::u32string TruncateToDisplayWidth(const std::u32string& text, int max_width);

// UTF-8 版本:内部解码成码点、按上面那个函数截断、再编码回 UTF-8。专给
// hint_lines(本来就是 UTF-8 std::string,可能夹汉字)截断用,不用另写一份
// 逻辑。遇到非法字节序列按"跳过这一个字节"处理,不中断——对内部自己拼出来
// 的字符串够用,不是给外部不可信输入准备的严格校验器。
std::string TruncateUtf8ToDisplayWidth(const std::string& utf8, int max_width);

// 编辑行超宽时的可视窗口:保证光标所在列落在窗口内,绝不让编辑行折行。
// content_width <= 0 时给空窗口。取舍见 console_input.cpp 里调用处的注释——
// 每帧独立按当前光标位置重算窗口,不跨帧持久化滚动偏移。
struct EditLineWindow {
    std::u32string text;                 // 窗口内的可见文本
    std::size_t cursor_display_col = 0;  // 光标在窗口内(不是整行里)的显示列
};
EditLineWindow ComputeEditLineWindow(const std::u32string& line, std::size_t cursor, int content_width);

// 每次 HandleKey()/CurrentRenderState() 之后吐出来的"这一刻该怎么画"。
// 终端层拿这个重画:定位到编辑区域起始行、清行、按 line 重写、把光标定位
// 到 cursor_display_col 那一列、hint_lines 非空就在下面逐行展示(每个元素
// 一个逻辑行,终端层负责按控制台实际宽度截断,核心层不折行、不猜宽度)。
struct RenderState {
    std::u32string line;                 // 当前行内容,按码点存
    std::size_t cursor = 0;              // 光标在 line 里的码点位置(不是显示列)
    std::size_t cursor_display_col = 0;  // 光标对应的显示列宽(CJK 按 2 算),终端层定位光标直接用这个
    std::vector<std::string> hint_lines;  // line 以 / 开头时,匹配的命令逐行列出;不需要显示就是空 vector
    bool submitted = false;              // Enter:line 是这一行最终提交的内容
    bool cleared = false;                // Ctrl+C 清空了非空行,留在同一次 ReadLine 里继续编辑
    bool eof_requested = false;          // Ctrl+D,或 Ctrl+C 在空行按下:整个读取应该当 EOF 处理
    bool mode_changed = false;           // 这一次事件是不是 ShiftTab 触发的模式切换
    bool esc_pressed = false;            // M10:这一下是不是 Esc 触发的(空闲态清行;终端层若在"确认
                                          // 提示"模式下读,看到这个直接把整次 ReadLine 当拒绝处理)
    ConfirmMode mode = ConfirmMode::Confirm;
};

class LineEditorCore {
public:
    // slash_candidates 是全部 slash 命令的补全候选,构造时传入一次
    // (调用方从 slash_commands 现有定义转换过来,核心层不重复定义)。
    explicit LineEditorCore(std::vector<CompletionCandidate> slash_candidates = {});

    // 开始读新的一行:清空行缓冲、光标、Tab 补全会话状态、历史浏览位置
    // (回到"底部");历史列表本身和确认模式是会话级的,跨多轮 BeginLine()
    // 保留,这里不碰。
    void BeginLine();

    // 喂一个按键事件,返回喂完这一下之后该怎么画。
    RenderState HandleKey(const KeyEvent& event);

    // 不喂新事件,只是想拿"当前状态该怎么画"(BeginLine() 之后、还没敲任何键
    // 时,终端层用这个画出空行 + 提示符)。
    RenderState CurrentRenderState() const;

    ConfirmMode confirm_mode() const { return confirm_mode_; }
    // 外部强制设定确认模式(比如 --yes 等价于起手切到 Yolo)。不算"切换",
    // 不会在下一次渲染里置 mode_changed。
    void set_confirm_mode(ConfirmMode mode) { confirm_mode_ = mode; }

    // 历史列表当前有多少条,单测用来断言"Enter 提交后历史真的记了一条"。
    std::size_t history_size() const { return history_.size(); }

private:
    struct TabCycleState {
        std::vector<std::string> matches;  // 命中的候选全名
        int index = -1;                    // -1: 还没真正开始轮转(刚补了公共前缀);>=0: 轮转到第几个
        std::u32string suffix;             // 触发这次补全会话时,命令词后面剩下的内容(比如已经打的参数),
                                            // 补全/轮转时原样接在候选名后面,不吞用户打的东西
    };

    std::vector<CompletionCandidate> slash_candidates_;

    std::u32string line_;
    std::size_t cursor_ = 0;

    std::vector<std::u32string> history_;
    std::optional<std::size_t> history_index_;  // nullopt = 在"底部"(没有在翻历史)
    std::u32string draft_;                      // 开始翻历史之前正在编辑的那一行,翻回底部时恢复

    std::optional<TabCycleState> tab_cycle_;
    ConfirmMode confirm_mode_ = ConfirmMode::Confirm;

    void ResetHistoryBrowsing();  // "翻到一半又编辑" -> 落回底部,但保留当前(已编辑的)内容
    void InsertChar(char32_t ch);
    void DeleteBackward();
    void MoveHistory(bool up);
    void HandleTab();
    void CompleteToCandidate(const std::string& name, const std::u32string& suffix);
    RenderState BuildRenderState(bool submitted, bool cleared, bool eof_requested, bool mode_changed) const;
    std::vector<std::string> MatchingCandidateNames(const std::u32string& word) const;

    // 把匹配到的候选名单排成 hint_lines:一行一个 `  /name  说明`,最多 6 行,
    // 超出加一行 "  … 共 N 个命令";selected_index >= 0 时,那一行的前缀
    // 换成 "> ",标出 Tab 轮转当前选中的是谁(-1 表示还没真正选中任何一个,
    // 比如刚补完公共前缀、下一下 Tab 才开始轮转)。
    std::vector<std::string> BuildHintLines(const std::vector<std::string>& matches, int selected_index) const;
};

}  // namespace lubancode::cli
