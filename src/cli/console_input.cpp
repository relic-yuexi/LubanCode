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
#include <iostream>

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

// 按 RenderState 重画"编辑区域":第一行是 提示符前缀 + prompt + 当前行
// 内容,后面紧跟 0~N 行 hint_lines(每个元素一个逻辑行,列出匹配的 slash
// 命令)。start_row/prompt_end_col 是这次 ReadLine() 调用一开始(或者
// Shift+Tab 切模式重打提示符之后)测出来的,之后的每次重画都锚定在这个
// 位置上。
//
// 相比升级前的版本,这里补上两条账,对应"提示堆残骸"这个 bug 的两个病根:
//   1. 每行落笔之前按控制台此刻的实际宽度(dwSize.X)截断,物理上绝不
//      折行——折行会让"起始行/光标位"这两个锚点失效,是清不干净的根子。
//      编辑行本身超宽同样按可视窗口截断(见 ComputeEditLineWindow),不
//      让编辑行折行破坏锚点。
//   2. 上一帧到底画了几行提示得记账(prev_hint_line_count):这一帧提示
//      行数比上一帧少,多出来的旧行要显式清空,不能只清"新内容覆盖到的
//      那部分"——这是"旧提示不擦、越敲越堆"的根子。
// 顺带用 EnsureRoomForRows 把"离缓冲区底部太近导致自动滚屏、坐标跟着失效"
// 这条已知限制也堵上了。
void RedrawEditArea(HANDLE h_out, SHORT& start_row, SHORT& prompt_end_col, const RenderState& state,
                     int& prev_hint_line_count) {
    CONSOLE_SCREEN_BUFFER_INFO info{};
    if (!GetConsoleScreenBufferInfo(h_out, &info)) {
        return;  // 拿不到屏幕信息就没法定位,这一帧放弃重画,下一帧再试
    }
    const SHORT buffer_width = info.dwSize.X;
    const SHORT buffer_height = info.dwSize.Y;
    const int new_hint_count = static_cast<int>(state.hint_lines.size());
    // 这一帧要碰(清或画)的提示行数上限:取新旧两帧行数的较大值,保证
    // 旧帧多出来的行不会被漏清。
    const int rows_to_touch = std::max(new_hint_count, prev_hint_line_count);

    EnsureRoomForRows(h_out, start_row, buffer_height, static_cast<SHORT>(1 + rows_to_touch));

    DWORD written = 0;

    // 编辑行:按"width - 1"留一列安全边界,窗口式截断保证光标可见、绝不
    // 折行(取舍见 ComputeEditLineWindow 头文件注释——每帧独立按光标位置
    // 重算窗口,不跨帧持久化滚动偏移)。
    const int content_width = static_cast<int>(buffer_width) - static_cast<int>(prompt_end_col) - 1;
    const EditLineWindow window = ComputeEditLineWindow(state.line, state.cursor, content_width);

    const COORD line_pos{0, start_row};
    SetConsoleCursorPosition(h_out, line_pos);
    FillConsoleOutputCharacterW(h_out, L' ', static_cast<DWORD>(buffer_width), line_pos, &written);
    SetConsoleCursorPosition(h_out, COORD{prompt_end_col, start_row});
    std::cout << Utf32ToUtf8(window.text);
    std::cout.flush();

    // 提示行:逐行先清后画。i >= new_hint_count 但 i < rows_to_touch 的行
    // 是上一帧画过、这一帧不再需要的,只清不画。
    for (int i = 0; i < rows_to_touch; ++i) {
        const COORD hint_pos{0, static_cast<SHORT>(start_row + 1 + i)};
        SetConsoleCursorPosition(h_out, hint_pos);
        FillConsoleOutputCharacterW(h_out, L' ', static_cast<DWORD>(buffer_width), hint_pos, &written);
        if (i < new_hint_count) {
            const int hint_width = static_cast<int>(buffer_width) - 1;
            const std::string truncated =
                TruncateUtf8ToDisplayWidth(state.hint_lines[static_cast<std::size_t>(i)], hint_width);
            SetConsoleCursorPosition(h_out, hint_pos);
            std::cout << truncated;
            std::cout.flush();
        }
    }
    prev_hint_line_count = new_hint_count;

    // 提示行画完,光标要挪回编辑行、落在窗口内换算过的那一列——不管画没画
    // 提示行,这一步都得做,不然光标就停在最后一行提示上了。
    const SHORT target_col = static_cast<SHORT>(prompt_end_col + static_cast<SHORT>(window.cursor_display_col));
    SetConsoleCursorPosition(h_out, COORD{target_col, start_row});
}

// 逐键读入这一行,真控制台专用。ReadConsoleInputW 逐个读键盘事件,翻译成
// cli::KeyEvent 喂 SharedEditor(),按吐出来的 RenderState 重画。
std::optional<std::string> ReadLineKeyByKey(const std::string& prompt, const Theme& theme) {
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
    editor.BeginLine();

    const std::string full_prompt = ConfirmModePromptPrefix(editor.confirm_mode()) + prompt;
    std::cout << full_prompt;
    std::cout.flush();

    CONSOLE_SCREEN_BUFFER_INFO info{};
    if (!GetConsoleScreenBufferInfo(h_out, &info)) {
        return ReadLineFromConsole();  // 拿不到屏幕信息就没法定位光标,退回整行读入
    }
    SHORT start_row = info.dwCursorPosition.Y;
    SHORT prompt_end_col = info.dwCursorPosition.X;
    int prev_hint_line_count = 0;

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

        std::optional<KeyEvent> mapped;
        if (ctrl && ke.wVirtualKeyCode == 'C') {
            mapped = KeyEvent::Simple(KeyKind::CtrlC);
        } else if (ctrl && ke.wVirtualKeyCode == 'D') {
            mapped = KeyEvent::Simple(KeyKind::CtrlD);
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
            mapped = KeyEvent::Simple(KeyKind::Enter);
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

        if (state.mode_changed) {
            std::cout << "\n" << theme.stats << "已切换到 " << ConfirmModeLabel(state.mode) << " 模式" << theme.reset
                       << "\n";
            const std::string new_full_prompt = ConfirmModePromptPrefix(state.mode) + prompt;
            std::cout << new_full_prompt;
            std::cout.flush();
            // 重打提示符之后,行的起始行号和"提示符打完光标停在哪一列"都
            // 可能变了(比如 "" -> "[auto] " 前缀变长了),两个锚点一起
            // 重新测,不然后面重画会画歪。
            CONSOLE_SCREEN_BUFFER_INFO after_notice_info{};
            if (GetConsoleScreenBufferInfo(h_out, &after_notice_info)) {
                start_row = after_notice_info.dwCursorPosition.Y;
                prompt_end_col = after_notice_info.dwCursorPosition.X;
            }
            prev_hint_line_count = 0;
        }

        RedrawEditArea(h_out, start_row, prompt_end_col, state, prev_hint_line_count);

        if (state.eof_requested) {
            std::cout << "\n";
            return std::nullopt;
        }
        if (state.cleared) {
            continue;  // 行已经清空,继续在同一次调用里编辑
        }
        if (state.submitted) {
            std::cout << "\n";
            return Utf32ToUtf8(state.line);
        }
    }
}

#endif  // _WIN32

}  // namespace

std::optional<std::string> ReadLine(const std::string& prompt, const Theme& theme) {
#ifdef _WIN32
    if (StdinIsRealConsole()) {
        return ReadLineKeyByKey(prompt, theme);
    }
#else
    (void)theme;
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

}  // namespace lubancode::cli
