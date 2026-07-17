// 实测结论(诊断交互模式"答完一轮就自动退出"的 bug 时留下的记录):
//
// 现象:交互模式里过了两次工具确认([y]/[a]/[N] 各读一行),模型答完回到
// `> ` 主提示符,用户接着输中文,程序却直接退出了——跟"空行退出"的规则对上了,
// 说明那次 std::getline 读到的是空串,不是用户真敲的内容。
//
// 复现条件:这台机器上的 shell 工具(git-bash 起子进程)拿不到真控制台句柄
// (GetFileType(stdin) 恒为 FILE_TYPE_PIPE),没法用自动化脚本直接敲键盘去
// 触发 conhost 的 bug,已用 GetFileType/GetConsoleMode 探测程序实测确认了
// 这一点。管道场景下 CP_UTF8 这条坑本来就不会踩上(SetConsoleCP 只影响
// "真控制台"的 ReadFile 语义,对管道没有任何编码转换),所以自动化管道测试
// 天然复现不出这个 bug——这跟用户反馈"部分场景正常、部分场景炸"的说法也对得上。
//
// 病根按代码走查 + 已知问题排查确认为疑点 1:
//   Windows 的 conhost 在输入代码页设为 CP_UTF8(65001)时,narrow 版
//   ReadFile/ReadConsoleA 读多字节字符有年头的已知 bug——多次 ReadFile 交替
//   调用之后,内部对"上一次没读完的多字节序列"的状态会跟丢,后续一次读到
//   空串或半个字符。
//
// 修法:交互模式全程只留这一个 stdin 入口——真控制台就用宽字符 API 读,
// 彻底不走窄字符 CP_UTF8 那条路;stdin 是管道/重定向时走原来的
// std::getline,不影响 `echo "x" | lubancode.exe` 这种自动化用法。
//
// M6.5 补充:真控制台这条路从"ReadConsoleW 整行读入"升级成"逐键输入编辑器"
// (ReadConsoleInputW 逐个键盘事件读,翻成 cli::KeyEvent 喂
// LineEditorCore,按吐出来的 RenderState 用 Win32 控制台 API 重画)。这一步
// 没法在当前 headless 环境里自动化敲键盘验证(见上面"复现条件"),已经过
// 编译告警检查(/W4 /permissive- 无告警)、逐行代码走查、以及
// LineEditorCore 纯逻辑部分的完整单测(见 tests/test_line_editor.cpp)。
// SetConsoleMode 失败(极少见,比如某些非标准终端模拟器)时,退回到老的
// ReadLineFromConsole()(整行读入,没有补全/历史/模式切换,但至少能用)。
//
// 补丁记录:用户实测报过 slash 补全提示"越敲越堆、清不干净"——病根有二:
//   1. 老版本提示是单行大拼串,超过控制台宽度被自动折行,重画逻辑记的
//      起始行/光标位这两个锚点没算上折出去的那些物理行,清不到。
//   2. 上一帧画了几行提示没记账,新帧不知道该擦几行。
// 修法:核心层 RenderState 把提示从单行字符串(hint_line)改成
// std::vector<std::string> hint_lines(一元素一逻辑行,核心层保证不折行由
// 终端层截断兜底);终端层每行落笔前按 dwSize.X 截断、记住上一帧画了几行
// 提示、多退少补地清。顺带把"离缓冲区底部太近导致自动滚屏、锚点跟着失效"
// 这条以前记在案但没堵上的已知限制也一并处理了(见 RedrawEditArea/
// EnsureRoomForRows)。

#include "cli/console_input.hpp"

#include <algorithm>
#include <chrono>
#include <iostream>
#include <mutex>

#include "cli/divider.hpp"
#include "cli/format_utils.hpp"
#include "cli/i18n.hpp"
#include "cli/slash_commands.hpp"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
// windows.h 自己定义的 max/min 宏会跟 <algorithm> 的 std::max/std::min 撞车
// (std::max(a, b) 被预处理器拆成 std::max(a, b) 的 max(...) 部分,展开成
// 一坨语法错误)。NOMINMAX 关掉这两个宏,规矩做法。
#define NOMINMAX
#include <windows.h>

#include <cstddef>
#endif

namespace lubancode::cli {

namespace {

void StripTrailingCrLf(std::string& s) {
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) {
        s.pop_back();
    }
}

// 贯穿整条交互会话存活的编辑器实例:main.cpp 里 `> ` 主循环、工具确认
// 提示、/model 选择、初次配置向导,全部经这一个 ReadLine() 入口,底下共用
// 这一份 LineEditorCore——历史列表、确认模式才有地方跨多轮读取存住。
// 补全候选从 cli::slash_commands 现有定义转过来,不重复写一份命令清单。
LineEditorCore& SharedEditor() {
    static LineEditorCore editor = [] {
        std::vector<CompletionCandidate> candidates;
        for (const auto& cmd : AllSlashCommands()) {
            candidates.push_back(CompletionCandidate{cmd.name, cmd.description});
        }
        return LineEditorCore(std::move(candidates));
    }();
    return editor;
}

// UI-D(0.16.0):会话级 UI 按键回调(Ctrl+O/Ctrl+E/焦点导航)。存这儿、
// 由 SetTranscriptUiHandler 装卸;ReadLineKeyByKey 只在 composer 读取里查它。
TranscriptUiHandler& UiHandlerSlot() {
    static TranscriptUiHandler handler;
    return handler;
}

// 0.17.0:常驻状态行数据(模型名/context 百分比/token 数)。main.cpp 每轮
// 更新,ReadLineKeyByKey 每帧重画状态行时读——都在主线程上,不用加锁
// (监听线程从不碰状态行)。
struct StatusLineData {
    std::string model;
    int percent = 0;
    long long used_tokens = 0;
    long long window_tokens = 0;
};
StatusLineData& StatusDataSlot() {
    static StatusLineData data;
    return data;
}

// M10:谁在真的调 ReadConsoleInputW 读键盘,谁就得先拿到这把锁——
// ReadLineKeyByKey() 整个调用期间(从进函数到返回)一直攥着它,
// TurnInputListener 的监听线程只在抢到锁的间隙才读一次。这样"监听只活在
// 编辑器不在读的窗口期"这条要求不用靠回调层层传参去手动维护,两边天然靠锁
// 互斥错开——工具确认提示 [y/a/N] 走的也是 ReadLineKeyByKey(),天然一并
// 受益,不用另外接管。
std::mutex& ConsoleReadMutex() {
    static std::mutex m;
    return m;
}

#ifdef _WIN32

// stdin 是不是挂着一个真控制台(不是管道、不是重定向的磁盘文件)。
bool StdinIsRealConsole() {
    const HANDLE h = GetStdHandle(STD_INPUT_HANDLE);
    if (h == nullptr || h == INVALID_HANDLE_VALUE) {
        return false;
    }
    if (GetFileType(h) != FILE_TYPE_CHAR) {
        return false;
    }
    DWORD mode = 0;
    return GetConsoleMode(h, &mode) != 0;
}

// 用 ReadConsoleW 读一整行宽字符(conhost 的行编辑器——退格、方向键、
// 输入法——都在这一步之前处理好了,交出来的是敲完回车的完整一行),
// 再转 UTF-8。逐键编辑器初始化失败(SetConsoleMode 不认新模式)时的兜底。
std::optional<std::string> ReadLineFromConsole() {
    const HANDLE h = GetStdHandle(STD_INPUT_HANDLE);
    std::wstring wide;
    wchar_t buf[1024];
    while (true) {
        DWORD read = 0;
        const BOOL ok = ReadConsoleW(h, buf, static_cast<DWORD>(std::size(buf)), &read, nullptr);
        if (!ok || read == 0) {
            return std::nullopt;
        }
        wide.append(buf, read);
        if (!wide.empty() && wide.back() == L'\n') {
            break;
        }
    }
    while (!wide.empty() && (wide.back() == L'\n' || wide.back() == L'\r')) {
        wide.pop_back();
    }
    if (wide.size() == 1 && wide[0] == 0x1A) {
        return std::nullopt;
    }
    const int utf8_len =
        WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), nullptr, 0, nullptr, nullptr);
    std::string utf8;
    if (utf8_len > 0) {
        utf8.resize(static_cast<std::size_t>(utf8_len));
        WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), utf8.data(), utf8_len, nullptr,
                             nullptr);
    }
    return utf8;
}

// 探一下从 start_row 起要画 total_rows 行(编辑行 + 提示行)会不会撞到
// 控制台缓冲区最后一行(GetConsoleScreenBufferInfo 的 dwSize.Y)——真撞上
// 了,后面往下写东西会让缓冲区内容整体往上滚,start_row 记的那个绝对行号
// 就跟着报废。这里主动先滚够所需的行数(挪到缓冲区最后一行、写换行逼滚屏)、
// 同步把 start_row 往上修正相同的行数,账目对平之后再画,不留"锚点失效"
// 的口子——这是修复"提示行堆残骸"时确认到的另一个锚点失效来源,原地一并
// 堵上。
void EnsureRoomForRows(HANDLE h_out, SHORT& start_row, SHORT buffer_height, SHORT total_rows) {
    const SHORT needed_bottom_row = static_cast<SHORT>(start_row + total_rows - 1);
    if (needed_bottom_row < buffer_height) {
        return;  // 缓冲区里本来就放得下,不用滚
    }
    const SHORT overflow = static_cast<SHORT>(needed_bottom_row - buffer_height + 1);
    SetConsoleCursorPosition(h_out, COORD{0, static_cast<SHORT>(buffer_height - 1)});
    for (SHORT i = 0; i < overflow; ++i) {
        std::cout << "\n";
    }
    std::cout.flush();
    start_row = static_cast<SHORT>(start_row - overflow);
    if (start_row < 0) {
        start_row = 0;
    }
}

// -----------------------------------------------------------------------
// 0.17.0 输入框化(Claude Code 版式):composer 主提示符的编辑区装进两根
// 长横线之间——上横线、`> ` 输入行(多行 composer 随内容长高)、下横线,
// 下横线之下再常驻一行状态行(确认档/模型名/context 占比),slash 候选
// 提示行挪到状态行之下。只有 composer 读取开框;确认提示、/model 编号
// 选择、向导这些单行读取跟从前一个样。管道/重定向走不到逐键路径,天然
// 无框无状态行。
// -----------------------------------------------------------------------

// 输入框上下横线的宽度上限:min(控制台宽 - 1, 100)。比输入/输出分界线的
// 80 宽——框是常驻画面的主角,略宽一点撑住版式;分界线那条老规矩不动。
constexpr int kComposerBoxMaxWidth = 100;

struct BoxChrome {
    bool enabled = false;
    const Theme* theme = nullptr;
    ConfirmMode mode = ConfirmMode::Confirm;
};

// 一根框线(带主题淡色;plain 主题 theme.stats/reset 都是空串,自动退化成
// 无色 '-' 线,不用另判断)。
std::string BoxRuleLine(const Theme& theme, int console_width) {
    const bool plain = theme.reset.empty();
    return theme.stats + BuildDividerLine(console_width, plain, kComposerBoxMaxWidth) + theme.reset;
}

// 状态行:模式段按档配色(确认=默认色、auto=stats、yolo=error,跟提示符
// 前缀 ColoredConfirmPrefix 同一套语义),信息段恒 stats 淡色。文本拼装是
// cli/format_utils 的纯函数,这里只管配色和按控制台宽度分段截断(截断得
// 按段做——夹着 ANSI 的整行没法安全截)。
void PrintStatusLine(const BoxChrome& chrome, int max_width) {
    const Theme& theme = *chrome.theme;
    const StatusLineData& data = StatusDataSlot();
    std::string seg_mode = StatusLineModeSegment(chrome.mode);
    std::string seg_info =
        StatusLineInfoSegment(data.model, data.percent, data.used_tokens, data.window_tokens);
    seg_mode = TruncateUtf8ToDisplayWidth(seg_mode, max_width);
    const int rest = max_width - static_cast<int>(DisplayWidthUtf8(seg_mode));
    seg_info = rest > 0 ? TruncateUtf8ToDisplayWidth(seg_info, rest) : std::string();

    std::string mode_color;
    switch (chrome.mode) {
        case ConfirmMode::Auto:
            mode_color = theme.stats;
            break;
        case ConfirmMode::Yolo:
            mode_color = theme.error;
            break;
        case ConfirmMode::Confirm:
            break;  // 默认色
    }
    std::cout << mode_color << seg_mode << (mode_color.empty() ? std::string() : theme.reset)
              << theme.stats << seg_info << theme.reset;
}

// 按 RenderState 重画"编辑区域"。UI-A(0.11.0)起编辑区不止一行:第一行是
// 提示符前缀 + prompt + composer 第一行,composer 后续行各占一个物理行、
// 行首两空格续行缩进(提示符只在第一行),再往下紧跟 0~N 行 hint_lines
// (每个元素一个逻辑行,列出匹配的 slash 命令——多行 composer 下核心层
// 保证 hint_lines 恒空)。start_row/prompt_end_col 是这次 ReadLine() 调用
// 一开始(或者 Shift+Tab 切模式重打提示符之后)测出来的,之后的每次重画
// 都锚定在这个位置上。
//
// 记账规则沿用修"提示堆残骸"那次立下的两条,只是从"提示行数"扩成"第一行
// 之外的全部行数"(composer 续行 + 提示行统一算):
//   1. 每行落笔之前按控制台此刻的实际宽度(dwSize.X)截断,物理上绝不
//      折行——折行会让"起始行/光标位"这两个锚点失效,是清不干净的根子。
//      每一行 composer 内容都各自按可视窗口截断(ComputeEditLineWindow
//      按行应用,光标所在行给真光标位、其余行从行首截),绝不折行。
//   2. 上一帧第一行之外到底画了几行得记账(prev_body_row_count):这一帧
//      行数比上一帧少(删行、清空、提示消失),多出来的旧行要显式清空,
//      不能只清"新内容覆盖到的那部分"。
// 顺带用 EnsureRoomForRows 按总行数探底滚屏,把"离缓冲区底部太近导致自动
// 滚屏、坐标跟着失效"这条已知限制堵住(增删行时锚点账目同步修正)。
void RedrawEditArea(HANDLE h_out, SHORT& start_row, SHORT& prompt_end_col, const RenderState& state,
                     int& prev_body_row_count, const BoxChrome& chrome = BoxChrome{}) {
    CONSOLE_SCREEN_BUFFER_INFO info{};
    if (!GetConsoleScreenBufferInfo(h_out, &info)) {
        return;  // 拿不到屏幕信息就没法定位,这一帧放弃重画,下一帧再试
    }
    const SHORT buffer_width = info.dwSize.X;
    const SHORT buffer_height = info.dwSize.Y;

    constexpr SHORT kContinuationIndent = 2;  // composer 续行的两空格缩进

    // 核心层保证 state.lines 至少一个元素,这里再兜一手,免得空 vector 把
    // 下标访问带崩。
    const std::vector<std::u32string> fallback_lines{std::u32string()};
    const std::vector<std::u32string>& edit_lines = state.lines.empty() ? fallback_lines : state.lines;
    const int edit_row_count = static_cast<int>(edit_lines.size());
    const int hint_count = static_cast<int>(state.hint_lines.size());
    // 0.17.0 输入框化:开框时最后一个编辑行下面再垫两行——下横线 + 状态行,
    // 提示行(hint_lines)排在状态行之下。锚点记账原样扩展:这两行也算进
    // body_rows,多退少补、EnsureRoomForRows 探底,全套现成机制照走。
    const int box_rows = chrome.enabled ? 2 : 0;
    // "第一行之外"这一帧要占的行数(composer 续行 + 框线/状态行 + 提示行),
    // 跟上一帧取较大值,保证旧帧多出来的行(删行、清 composer、提示消失)
    // 不漏清。
    const int body_rows = (edit_row_count - 1) + box_rows + hint_count;
    const int rows_to_touch = std::max(body_rows, prev_body_row_count);

    EnsureRoomForRows(h_out, start_row, buffer_height, static_cast<SHORT>(1 + rows_to_touch));

    DWORD written = 0;

    // 光标最终该落的位置,画完统一挪过去(先给个兜底值:第一行提示符后)。
    COORD final_cursor{prompt_end_col, start_row};

    // 第一行:只清"提示符之后"这一段(从 prompt_end_col 到行尾),提示符
    // 本身那几列一个字符都不碰——清整行会把 "> "/"[auto] > " 盖没,是修过
    // 的老 bug,别再犯。
    {
        const int content_width = static_cast<int>(buffer_width) - static_cast<int>(prompt_end_col) - 1;
        const std::size_t cursor_in_row = state.cursor_row == 0 ? state.cursor_col : 0;
        const EditLineWindow window = ComputeEditLineWindow(edit_lines[0], cursor_in_row, content_width);
        const int clear_width = static_cast<int>(buffer_width) - static_cast<int>(prompt_end_col);
        if (clear_width > 0) {
            const COORD line_pos{prompt_end_col, start_row};
            FillConsoleOutputCharacterW(h_out, L' ', static_cast<DWORD>(clear_width), line_pos, &written);
        }
        SetConsoleCursorPosition(h_out, COORD{prompt_end_col, start_row});
        std::cout << Utf32ToUtf8(window.text);
        std::cout.flush();
        if (state.cursor_row == 0) {
            final_cursor = COORD{static_cast<SHORT>(prompt_end_col + static_cast<SHORT>(window.cursor_display_col)),
                                  start_row};
        }
    }

    // 第一行之外:逐行先整行清、再按行的身份画——composer 续行(两空格
    // 缩进 + 窗口截断)、下横线/状态行(开框时)、提示行(截宽),或者
    // 旧帧残行(只清不画)。
    for (int i = 1; i <= rows_to_touch; ++i) {
        const COORD row_pos{0, static_cast<SHORT>(start_row + i)};
        FillConsoleOutputCharacterW(h_out, L' ', static_cast<DWORD>(buffer_width), row_pos, &written);
        if (i < edit_row_count) {
            const int content_width = static_cast<int>(buffer_width) - kContinuationIndent - 1;
            const std::size_t row_index = static_cast<std::size_t>(i);
            const std::size_t cursor_in_row = state.cursor_row == row_index ? state.cursor_col : 0;
            const EditLineWindow window = ComputeEditLineWindow(edit_lines[row_index], cursor_in_row, content_width);
            SetConsoleCursorPosition(h_out, COORD{kContinuationIndent, row_pos.Y});
            std::cout << Utf32ToUtf8(window.text);
            std::cout.flush();
            if (state.cursor_row == row_index) {
                final_cursor =
                    COORD{static_cast<SHORT>(kContinuationIndent + static_cast<SHORT>(window.cursor_display_col)),
                           row_pos.Y};
            }
        } else if (chrome.enabled && i == edit_row_count) {
            // 下横线,紧贴最后一个编辑行。
            SetConsoleCursorPosition(h_out, row_pos);
            std::cout << BoxRuleLine(*chrome.theme, buffer_width);
            std::cout.flush();
        } else if (chrome.enabled && i == edit_row_count + 1) {
            // 常驻状态行。
            SetConsoleCursorPosition(h_out, row_pos);
            PrintStatusLine(chrome, static_cast<int>(buffer_width) - 1);
            std::cout.flush();
        } else if (i - edit_row_count - box_rows >= 0 && i - edit_row_count - box_rows < hint_count) {
            const int hint_width = static_cast<int>(buffer_width) - 1;
            const std::string truncated = TruncateUtf8ToDisplayWidth(
                state.hint_lines[static_cast<std::size_t>(i - edit_row_count - box_rows)], hint_width);
            SetConsoleCursorPosition(h_out, row_pos);
            std::cout << truncated;
            std::cout.flush();
        }
    }
    prev_body_row_count = body_rows;

    // 画完把光标挪回它该在的那一行那一列——不管画没画续行/提示行,这一步
    // 都得做,不然光标就停在最后一行上了。
    SetConsoleCursorPosition(h_out, final_cursor);
}

// M11(0.10.0):Shift+Tab 切确认档位时提示符前缀该套什么颜色——auto 用
// theme.stats(跟别的淡色提示一致),yolo 用 theme.error(全放行,得扎眼
// 一点),confirm(默认档)不着色。ConfirmModePromptPrefix() 本身保持纯
// 文本、不夹 ANSI(line_editor 的单测直接断言它的返回值是 "" / "[auto] " /
// "[yolo] ",不能把颜色码焊死在里头),颜色只在这个终端层的打印点包一层。
std::string ColoredConfirmPrefix(ConfirmMode mode, const Theme& theme) {
    const std::string prefix = ConfirmModePromptPrefix(mode);
    if (prefix.empty()) {
        return prefix;
    }
    switch (mode) {
        case ConfirmMode::Auto:
            return theme.stats + prefix + theme.reset;
        case ConfirmMode::Yolo:
            return theme.error + prefix + theme.reset;
        case ConfirmMode::Confirm:
            return prefix;
    }
    return prefix;
}

// 0.17.0 输入框化的提交收尾:横线擦掉、提交行保留。取舍:两个方案里选了
// "只留 `> 内容`"这条——框连横线留在滚动历史的话,每一问上下各一根 100 列
// 横线,再加 RunTurn 紧接着打的输入/输出分界线,三根线叠一块,滚动历史
// 全是线;擦掉横线后历史里就是干干净净的 `> 问题` + 分界线 + 回答,跟
// 0.16.0 的历史观感一致,框只属于"正在输入"这一刻。
//
// 做法:把整个框(上横线在 start_row - 1,下横线/状态行/提示行在编辑行
// 之下)统统清掉,从上横线那一行起重打提交内容(`> ` 第一行、两空格续行,
// 每行按宽截断防折行——完整内容反正在历史/存档里),光标停在末行末尾,
// 调用方接着换行。内容整体上移一行,不留空行。
void CollapseBoxOnSubmit(HANDLE h_out, SHORT start_row, int prev_body_row_count, const RenderState& state,
                          const Theme& theme, const std::string& prompt) {
    CONSOLE_SCREEN_BUFFER_INFO info{};
    if (!GetConsoleScreenBufferInfo(h_out, &info)) {
        return;  // 拿不到屏幕信息就不收尾了,提交帧画面已经在屏上,不算坏
    }
    const SHORT buffer_width = info.dwSize.X;
    const SHORT top = start_row > 0 ? static_cast<SHORT>(start_row - 1) : start_row;
    SHORT bottom = static_cast<SHORT>(start_row + prev_body_row_count);
    if (bottom >= info.dwSize.Y) {
        bottom = static_cast<SHORT>(info.dwSize.Y - 1);
    }
    DWORD written = 0;
    for (SHORT r = top; r <= bottom; ++r) {
        const COORD row_pos{0, r};
        FillConsoleOutputCharacterW(h_out, L' ', static_cast<DWORD>(buffer_width), row_pos, &written);
        FillConsoleOutputAttribute(h_out, info.wAttributes, static_cast<DWORD>(buffer_width), row_pos, &written);
    }
    SetConsoleCursorPosition(h_out, COORD{0, top});
    std::cout << ColoredConfirmPrefix(state.mode, theme) << prompt;
    std::cout.flush();
    CONSOLE_SCREEN_BUFFER_INFO after{};
    int first_width = static_cast<int>(buffer_width) - 3;
    if (GetConsoleScreenBufferInfo(h_out, &after)) {
        first_width = static_cast<int>(buffer_width) - static_cast<int>(after.dwCursorPosition.X) - 1;
    }
    if (!state.lines.empty()) {
        std::cout << Utf32ToUtf8(TruncateToDisplayWidth(state.lines[0], first_width));
        for (std::size_t i = 1; i < state.lines.size(); ++i) {
            std::cout << "\n  "
                      << Utf32ToUtf8(TruncateToDisplayWidth(state.lines[i], static_cast<int>(buffer_width) - 3));
        }
    }
    std::cout.flush();
}

// 逐键读入这一次输入(UI-A 起,composer 模式下可能是多行),真控制台专用。
// ReadConsoleInputW 逐个读键盘事件,翻译成 cli::KeyEvent 喂 SharedEditor(),
// 按吐出来的 RenderState 重画。
//
// esc_rejects/composer:见 console_input.hpp 里 ReadLine() 的同名参数注释。
std::optional<std::string> ReadLineKeyByKey(const std::string& prompt, const Theme& theme, bool esc_rejects,
                                             bool composer) {
    // 整个函数体都攥着这把锁:M10 的 TurnInputListener 监听线程只在抢到锁
    // 的间隙才读控制台输入,这一行锁一上,就等于宣布"编辑器正在读",监听
    // 线程会自动让出、不跟这里抢同一份 ReadConsoleInputW 输入。
    std::lock_guard<std::mutex> console_read_lock(ConsoleReadMutex());

    const HANDLE h_in = GetStdHandle(STD_INPUT_HANDLE);
    const HANDLE h_out = GetStdHandle(STD_OUTPUT_HANDLE);

    DWORD original_mode = 0;
    if (!GetConsoleMode(h_in, &original_mode)) {
        return ReadLineFromConsole();
    }
    // 关掉行编辑、回显、Ctrl+C 自动处理(这三样都要自己接管):
    //   ENABLE_LINE_INPUT      —— 关掉之后 ReadConsoleInputW 才能逐键拿到,不用等回车
    //   ENABLE_ECHO_INPUT      —— 关掉控制台自带回显,回显交给我们自己按 RenderState 重画
    //   ENABLE_PROCESSED_INPUT —— 关掉之后 Ctrl+C 才会以 KEY_EVENT 形式读到,不会
    //                             被控制台直接当 SIGINT 处理掉、绕过我们的按键循环
    const DWORD new_mode = (original_mode & ~static_cast<DWORD>(ENABLE_LINE_INPUT) &
                             ~static_cast<DWORD>(ENABLE_ECHO_INPUT) & ~static_cast<DWORD>(ENABLE_PROCESSED_INPUT));
    if (!SetConsoleMode(h_in, new_mode)) {
        return ReadLineFromConsole();
    }
    struct ModeGuard {
        HANDLE h;
        DWORD original;
        ~ModeGuard() { SetConsoleMode(h, original); }
    } mode_guard{h_in, original_mode};

    LineEditorCore& editor = SharedEditor();
    editor.BeginLine(composer);

    // 0.17.0:composer 读取开输入框(上横线 + `> ` 输入行 + 下横线 + 状态
    // 行)。上横线打在提示符上一行,不进重画账(锚点 start_row 是提示符
    // 行,横线在 start_row - 1,重画从不碰它;EnsureRoomForRows 逼滚屏时
    // 整个缓冲区内容连横线一起上移,start_row 同步修正,账目还是平的)。
    const bool box = composer;
    BoxChrome chrome{box, &theme, editor.confirm_mode()};
    if (box) {
        CONSOLE_SCREEN_BUFFER_INFO pre_info{};
        const int console_width = GetConsoleScreenBufferInfo(h_out, &pre_info) ? pre_info.dwSize.X : 80;
        std::cout << BoxRuleLine(theme, console_width) << "\n";
    }

    const std::string full_prompt = ColoredConfirmPrefix(editor.confirm_mode(), theme) + prompt;
    std::cout << full_prompt;
    std::cout.flush();

    CONSOLE_SCREEN_BUFFER_INFO info{};
    if (!GetConsoleScreenBufferInfo(h_out, &info)) {
        return ReadLineFromConsole();  // 拿不到屏幕信息就没法定位光标,退回整行读入
    }
    SHORT start_row = info.dwCursorPosition.Y;
    SHORT prompt_end_col = info.dwCursorPosition.X;
    int prev_body_row_count = 0;  // 上一帧第一行之外画了几行(composer 续行 + 框线/状态行 + 提示行)

    if (box) {
        // 开场帧:下横线 + 状态行立刻就位,不等第一个按键。
        RedrawEditArea(h_out, start_row, prompt_end_col, editor.CurrentRenderState(), prev_body_row_count,
                        chrome);
    }

    std::optional<char32_t> pending_high_surrogate;

    while (true) {
        INPUT_RECORD record{};
        DWORD read = 0;
        if (!ReadConsoleInputW(h_in, &record, 1, &read) || read == 0) {
            return std::nullopt;
        }
        if (record.EventType != KEY_EVENT || record.Event.KeyEvent.bKeyDown == FALSE) {
            continue;
        }
        const KEY_EVENT_RECORD& ke = record.Event.KeyEvent;
        const bool ctrl = (ke.dwControlKeyState & (LEFT_CTRL_PRESSED | RIGHT_CTRL_PRESSED)) != 0;
        const bool shift = (ke.dwControlKeyState & SHIFT_PRESSED) != 0;
        const bool alt = (ke.dwControlKeyState & (LEFT_ALT_PRESSED | RIGHT_ALT_PRESSED)) != 0;

        std::optional<KeyEvent> mapped;
        if (ctrl && ke.wVirtualKeyCode == 'C') {
            mapped = KeyEvent::Simple(KeyKind::CtrlC);
        } else if (ctrl && ke.wVirtualKeyCode == 'D') {
            mapped = KeyEvent::Simple(KeyKind::CtrlD);
        } else if (ctrl && ke.wVirtualKeyCode == 'O') {
            mapped = KeyEvent::Simple(KeyKind::CtrlO);  // UI-D:紧凑/详细切换
        } else if (ctrl && ke.wVirtualKeyCode == 'E') {
            mapped = KeyEvent::Simple(KeyKind::CtrlE);  // UI-D:聚焦查看
        } else if (ke.wVirtualKeyCode == VK_BACK) {
            mapped = KeyEvent::Simple(KeyKind::Backspace);
        } else if (ke.wVirtualKeyCode == VK_LEFT) {
            mapped = KeyEvent::Simple(KeyKind::Left);
        } else if (ke.wVirtualKeyCode == VK_RIGHT) {
            mapped = KeyEvent::Simple(KeyKind::Right);
        } else if (ke.wVirtualKeyCode == VK_HOME) {
            mapped = KeyEvent::Simple(KeyKind::Home);
        } else if (ke.wVirtualKeyCode == VK_END) {
            mapped = KeyEvent::Simple(KeyKind::End);
        } else if (ke.wVirtualKeyCode == VK_UP) {
            mapped = KeyEvent::Simple(KeyKind::Up);
        } else if (ke.wVirtualKeyCode == VK_DOWN) {
            mapped = KeyEvent::Simple(KeyKind::Down);
        } else if (ke.wVirtualKeyCode == VK_TAB) {
            mapped = KeyEvent::Simple(shift ? KeyKind::ShiftTab : KeyKind::Tab);
        } else if (ke.wVirtualKeyCode == VK_RETURN) {
            // UI-A:Alt+Enter / Shift+Enter 都翻成 NewLine(插换行)。实测这台
            // 机器:Windows Terminal 把 Alt+Enter 绑成了全屏切换,keydown 根本
            // 进不了输入缓冲(只漏一个 keyup,bKeyDown==FALSE 早被上面滤掉),
            // 等于天然收不到;conhost 下两个组合都完好。所以两个都认,文档里
            // 推荐 Shift+Enter。非 composer 读取里 NewLine 会被核心层当 Enter,
            // 确认提示那些单行场景语义不变。
            mapped = KeyEvent::Simple((shift || alt) ? KeyKind::NewLine : KeyKind::Enter);
        } else if (ke.wVirtualKeyCode == VK_ESCAPE) {
            mapped = KeyEvent::Simple(KeyKind::Esc);
        } else if (ke.uChar.UnicodeChar != 0 && !ctrl) {
            const wchar_t wc = ke.uChar.UnicodeChar;
            if (wc >= 0xD800 && wc <= 0xDBFF) {
                pending_high_surrogate = static_cast<char32_t>(wc);
                continue;  // 高代理项,等低代理项凑成一个完整码点再喂
            }
            char32_t cp = static_cast<char32_t>(wc);
            if (wc >= 0xDC00 && wc <= 0xDFFF && pending_high_surrogate.has_value()) {
                const char32_t high = *pending_high_surrogate;
                pending_high_surrogate.reset();
                cp = 0x10000 + ((high - 0xD800) << 10) + (static_cast<char32_t>(wc) - 0xDC00);
            }
            if (cp >= 0x20) {  // 过滤掉控制字符(Esc、独立的 Tab 已经在上面单独处理)
                mapped = KeyEvent::Char(cp);
            }
        }

        if (!mapped.has_value()) {
            continue;  // 单按 Shift/Ctrl/Alt 之类的修饰键,或者没映射到的键,跳过
        }

        const RenderState state = editor.HandleKey(*mapped);

        // UI-D(0.16.0):composer 读取里,把核心层翻好的 UI 按键语义转发给
        // 应用层回调。流程:光标先挪到编辑区最后一行、换行(回调的输出从
        // 编辑区下面开始铺,旧编辑区画面留在上面,滚动历史自然带走);回调
        // 返回 true 表示真铺了内容——重打提示符、重测锚点、按当前编辑内容
        // 原样重画,接着等键;返回 false 表示这个键没被消费(比如焦点导航
        // 时 transcript 是空的、ESC 不在聚焦查看态),落回下面的原有处理
        // (RedrawEditArea 会把光标摆回去,挪动无痕)。
        if (composer && UiHandlerSlot()) {
            std::optional<UiKeyAction> action;
            if (state.toggle_expand_requested) {
                action = UiKeyAction::ToggleExpand;
            } else if (state.focus_view_requested) {
                action = UiKeyAction::FocusView;
            } else if (state.focus_move > 0) {
                action = UiKeyAction::FocusOlder;
            } else if (state.focus_move < 0) {
                action = UiKeyAction::FocusNewer;
            } else if (state.esc_pressed) {
                action = UiKeyAction::Escape;
            }
            if (action.has_value()) {
                CONSOLE_SCREEN_BUFFER_INFO before_info{};
                if (GetConsoleScreenBufferInfo(h_out, &before_info)) {
                    SHORT last_row = static_cast<SHORT>(start_row + prev_body_row_count);
                    if (last_row >= before_info.dwSize.Y) {
                        last_row = static_cast<SHORT>(before_info.dwSize.Y - 1);
                    }
                    SetConsoleCursorPosition(h_out, COORD{0, last_row});
                }
                const bool handled = UiHandlerSlot()(*action);
                if (handled) {
                    // 回调铺完内容,整个框(上横线起)重打一遍、重测锚点。
                    if (box) {
                        CONSOLE_SCREEN_BUFFER_INFO rule_info{};
                        const int console_width =
                            GetConsoleScreenBufferInfo(h_out, &rule_info) ? rule_info.dwSize.X : 80;
                        std::cout << BoxRuleLine(theme, console_width) << "\n";
                    }
                    std::cout << ColoredConfirmPrefix(editor.confirm_mode(), theme) << prompt;
                    std::cout.flush();
                    CONSOLE_SCREEN_BUFFER_INFO after_info{};
                    if (GetConsoleScreenBufferInfo(h_out, &after_info)) {
                        start_row = after_info.dwCursorPosition.Y;
                        prompt_end_col = after_info.dwCursorPosition.X;
                    }
                    prev_body_row_count = 0;
                    chrome.mode = editor.confirm_mode();
                    RedrawEditArea(h_out, start_row, prompt_end_col, state, prev_body_row_count, chrome);
                    continue;
                }
                // 0.17.0:焦点导航请求没被消费(比如 transcript 还是空的)——
                // 把核心层的焦点态退掉,不然屏上什么都没发生,下一下
                // Shift+Tab 却被当成"焦点往新走"吞掉,切不了档。
                if (*action == UiKeyAction::FocusOlder || *action == UiKeyAction::FocusNewer) {
                    editor.ExitFocusMode();
                }
            }
        }

        // M11(0.10.0):切档原地更新,不打印任何新行——用户反馈过"连按几轮
        // Shift+Tab,'已切换到 X 模式' 通知行滚出一屏残骸"。档位只体现在
        // 提示符前缀的颜色/文字上(ColoredConfirmPrefix)和常驻状态行上
        // (0.17.0,紧接着的 RedrawEditArea 每帧都重画状态行,chrome.mode
        // 下面刚同步过,自然带上新档),这里原地清掉 start_row 这一整行、
        // 换上新前缀,起始行号不变,不往下挪一行——切档只动状态行和提示符
        // 前缀,屏上零新增行。
        if (state.mode_changed) {
            CONSOLE_SCREEN_BUFFER_INFO row_info{};
            if (GetConsoleScreenBufferInfo(h_out, &row_info)) {
                const SHORT buffer_width = row_info.dwSize.X;
                DWORD written = 0;
                const COORD row_pos{0, start_row};
                FillConsoleOutputCharacterW(h_out, L' ', static_cast<DWORD>(buffer_width), row_pos, &written);
                SetConsoleCursorPosition(h_out, row_pos);
                std::cout << ColoredConfirmPrefix(state.mode, theme) << prompt;
                std::cout.flush();
                CONSOLE_SCREEN_BUFFER_INFO after_info{};
                if (GetConsoleScreenBufferInfo(h_out, &after_info)) {
                    prompt_end_col = after_info.dwCursorPosition.X;
                }
            }
        }

        if (box && state.submitted) {
            // 0.17.0 输入框化的提交收尾:横线/状态行擦掉,只留 `> 内容`,
            // 换行收尾——取舍见 CollapseBoxOnSubmit 注释。
            CollapseBoxOnSubmit(h_out, start_row, prev_body_row_count, state, theme, prompt);
            std::cout << "\n";
            return Utf32ToUtf8(state.line);
        }

        chrome.mode = editor.confirm_mode();
        RedrawEditArea(h_out, start_row, prompt_end_col, state, prev_body_row_count, chrome);

        if (state.esc_pressed && esc_rejects) {
            // 确认提示 [y/a/N] 场景:Esc 不留在循环里继续等,直接当这次
            // 读取交了个空串——main.cpp 的确认回调本来就把"不是 y/Y/a/A"
            // 都当拒绝,空串天然就是拒绝。
            std::cout << "\n";
            return std::string();
        }
        if (state.eof_requested) {
            // Ctrl+D/Ctrl+Z 可能按在多行 composer 中间某一行,框下面还垫着
            // 横线/状态行:先把光标挪到编辑区(含框)最下面一行再换行,免得
            // 接下来的输出打在残留画面身上。prev_body_row_count 刚被
            // RedrawEditArea 更新过,就是"第一行之外"的总行数。
            if (prev_body_row_count > 0) {
                SetConsoleCursorPosition(
                    h_out, COORD{0, static_cast<SHORT>(start_row + static_cast<SHORT>(prev_body_row_count))});
            }
            std::cout << "\n";
            return std::nullopt;
        }
        if (state.cleared) {
            continue;  // composer 已经清空,继续在同一次调用里编辑
        }
        if (state.submitted) {
            // 非框读取的提交:RedrawEditArea 刚按提交帧把完整多行内容画在
            // 屏幕上、光标停在末行末尾,这里换行收尾。
            std::cout << "\n";
            return Utf32ToUtf8(state.line);
        }
    }
}

#endif  // _WIN32

}  // namespace

std::optional<std::string> ReadLine(const std::string& prompt, const Theme& theme, bool esc_rejects, bool composer) {
#ifdef _WIN32
    if (StdinIsRealConsole()) {
        return ReadLineKeyByKey(prompt, theme, esc_rejects, composer);
    }
    (void)composer;  // 管道/重定向:没有 composer 概念,照旧逐行 getline
#else
    (void)theme;
    (void)esc_rejects;
    (void)composer;
#endif

    if (!prompt.empty()) {
        std::cout << prompt;
        std::cout.flush();
    }
    std::string line;
    if (!std::getline(std::cin, line)) {
        return std::nullopt;
    }
    StripTrailingCrLf(line);
    return line;
}

ConfirmMode CurrentConfirmMode() { return SharedEditor().confirm_mode(); }

void SetConfirmMode(ConfirmMode mode) { SharedEditor().set_confirm_mode(mode); }

void SetTranscriptUiHandler(TranscriptUiHandler handler) { UiHandlerSlot() = std::move(handler); }

void SetStatusLineData(const std::string& model, int context_percent, long long used_tokens,
                        long long window_tokens) {
    StatusLineData& data = StatusDataSlot();
    data.model = model;
    data.percent = context_percent;
    data.used_tokens = used_tokens;
    data.window_tokens = window_tokens;
}

std::optional<int> DetectConsoleWidth() {
#ifdef _WIN32
    const HANDLE h_out = GetStdHandle(STD_OUTPUT_HANDLE);
    if (h_out == nullptr || h_out == INVALID_HANDLE_VALUE) {
        return std::nullopt;
    }
    CONSOLE_SCREEN_BUFFER_INFO info{};
    if (!GetConsoleScreenBufferInfo(h_out, &info)) {
        return std::nullopt;
    }
    return static_cast<int>(info.dwSize.X);
#else
    return std::nullopt;
#endif
}

std::mutex& StdoutWriteMutex() {
    static std::mutex m;
    return m;
}

TurnInputListener::TurnInputListener(std::atomic<bool>& cancel_flag, const Theme& theme)
    : cancel_flag_(cancel_flag), theme_(theme) {
#ifdef _WIN32
    if (StdinIsRealConsole()) {
        enabled_ = true;
        thread_ = std::thread([this] { ThreadMain(); });
    }
#endif
}

TurnInputListener::~TurnInputListener() { Stop(); }

void TurnInputListener::Stop() {
    if (!enabled_) {
        return;
    }
    stop_requested_.store(true);
    if (thread_.joinable()) {
        thread_.join();
    }
    enabled_ = false;  // 幂等:重复调用/析构再调都不会再 join 一次已经空了的 thread_
}

std::vector<std::string> TurnInputListener::TakeQueuedLines() {
    std::lock_guard<std::mutex> lock(queue_mutex_);
    std::vector<std::string> out = std::move(queued_lines_);
    queued_lines_.clear();
    return out;
}

#ifdef _WIN32

void TurnInputListener::ThreadMain() {
    const HANDLE h_in = GetStdHandle(STD_INPUT_HANDLE);
    // 排队缓冲按码点存(不是 UTF-8 字节),Backspace 才不会把一个多字节字符
    // 切成半个、写出乱码——跟 line_editor 那套一个思路,只是这里不需要
    // 完整编辑器的光标移动/历史/补全,故意从简。
    std::u32string buffer;
    std::optional<char32_t> pending_high_surrogate;

    while (!stop_requested_.load()) {
        // try_lock:抢不到就说明编辑器(ReadLineKeyByKey,含工具确认提示)
        // 正在读,乖乖让出、睡一下再抢,绝不跟前台读键盘的那次调用抢同一份
        // 控制台输入。
        std::unique_lock<std::mutex> read_lock(ConsoleReadMutex(), std::try_to_lock);
        if (!read_lock.owns_lock()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
            continue;
        }
        // WaitForSingleObject 只是问"有没有事件可读",不消费——真正消费在
        // 下面的 ReadConsoleInputW。超时就松开锁、回到循环顶部重新抢,好
        // 让出机会给这段时间可能开始的前台读取。
        const DWORD wait_result = WaitForSingleObject(h_in, 50);
        if (wait_result != WAIT_OBJECT_0) {
            continue;
        }
        if (stop_requested_.load()) {
            break;
        }
        INPUT_RECORD record{};
        DWORD read = 0;
        if (!ReadConsoleInputW(h_in, &record, 1, &read) || read == 0) {
            continue;
        }
        read_lock.unlock();  // 这一个事件读完了,处理逻辑不用再攥着锁

        if (record.EventType != KEY_EVENT || record.Event.KeyEvent.bKeyDown == FALSE) {
            continue;
        }
        const KEY_EVENT_RECORD& ke = record.Event.KeyEvent;
        const bool ctrl = (ke.dwControlKeyState & (LEFT_CTRL_PRESSED | RIGHT_CTRL_PRESSED)) != 0;

        if (ke.wVirtualKeyCode == VK_ESCAPE) {
            cancel_flag_.store(true);
            std::lock_guard<std::mutex> stdout_lock(StdoutWriteMutex());
            std::cout << "\n" << theme_.stats << tr("input.interrupted") << theme_.reset << "\n";
            std::cout.flush();
            continue;
        }
        if (ke.wVirtualKeyCode == VK_RETURN) {
            if (!buffer.empty()) {
                const std::string line = Utf32ToUtf8(buffer);
                {
                    std::lock_guard<std::mutex> stdout_lock(StdoutWriteMutex());
                    std::cout << "\n" << theme_.stats << tr("input.queued") << theme_.reset << line << "\n";
                    std::cout.flush();
                }
                {
                    std::lock_guard<std::mutex> lock(queue_mutex_);
                    queued_lines_.push_back(line);
                }
                buffer.clear();
            }
            continue;
        }
        if (ke.wVirtualKeyCode == VK_BACK) {
            if (!buffer.empty()) {
                buffer.pop_back();
            }
            continue;
        }
        if (ke.uChar.UnicodeChar != 0 && !ctrl) {
            const wchar_t wc = ke.uChar.UnicodeChar;
            if (wc >= 0xD800 && wc <= 0xDBFF) {
                pending_high_surrogate = static_cast<char32_t>(wc);
                continue;
            }
            char32_t cp = static_cast<char32_t>(wc);
            if (wc >= 0xDC00 && wc <= 0xDFFF && pending_high_surrogate.has_value()) {
                const char32_t high = *pending_high_surrogate;
                pending_high_surrogate.reset();
                cp = 0x10000 + ((high - 0xD800) << 10) + (static_cast<char32_t>(wc) - 0xDC00);
            }
            if (cp >= 0x20) {
                buffer.push_back(cp);
            }
        }
    }
}

#else

void TurnInputListener::ThreadMain() {}

#endif  // _WIN32

}  // namespace lubancode::cli
