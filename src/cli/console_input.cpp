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

#include "cli/console_input.hpp"

#include <iostream>

#include "cli/slash_commands.hpp"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
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

// 按 RenderState 重画"编辑区域":第一行是 提示符前缀 + prompt + 当前行
// 内容,紧接着第二行是 hint_line(非空时才画)。start_row/prompt_end_col
// 是这次 ReadLine() 调用一开始就定死的(打完 prompt 那一刻测出来的),
// 之后的每次重画都锚定在这个位置上——已知限制:如果这行离控制台可视窗口
// 底部只剩一两行,多占的提示行会触发控制台自动向上滚屏,这两个坐标就跟着
// 失效,真机上如果撞见"光标乱跳"大概率是这个,当前 headless 环境没法
// 验证,如实记在这里。
void RedrawEditArea(HANDLE h_out, SHORT start_row, SHORT buffer_width, SHORT prompt_end_col,
                     const RenderState& state, bool& hint_shown_last_time) {
    DWORD written = 0;
    const COORD line_pos{0, start_row};
    SetConsoleCursorPosition(h_out, line_pos);
    FillConsoleOutputCharacterW(h_out, L' ', static_cast<DWORD>(buffer_width), line_pos, &written);
    if (hint_shown_last_time) {
        const COORD hint_pos{0, static_cast<SHORT>(start_row + 1)};
        FillConsoleOutputCharacterW(h_out, L' ', static_cast<DWORD>(buffer_width), hint_pos, &written);
    }

    SetConsoleCursorPosition(h_out, COORD{prompt_end_col, start_row});
    std::cout << Utf32ToUtf8(state.line);
    std::cout.flush();

    if (!state.hint_line.empty()) {
        SetConsoleCursorPosition(h_out, COORD{0, static_cast<SHORT>(start_row + 1)});
        std::cout << "  " << state.hint_line;
        std::cout.flush();
        hint_shown_last_time = true;
    } else {
        hint_shown_last_time = false;
    }

    const SHORT target_col = static_cast<SHORT>(prompt_end_col + static_cast<SHORT>(state.cursor_display_col));
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
    const SHORT prompt_end_col = info.dwCursorPosition.X;
    const SHORT buffer_width = info.dwSize.X;
    bool hint_shown_last_time = false;

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
            std::cout << ConfirmModePromptPrefix(state.mode) << prompt;
            std::cout.flush();
            CONSOLE_SCREEN_BUFFER_INFO after_notice_info{};
            if (GetConsoleScreenBufferInfo(h_out, &after_notice_info)) {
                start_row = after_notice_info.dwCursorPosition.Y;
            }
            hint_shown_last_time = false;
        }

        RedrawEditArea(h_out, start_row, buffer_width, prompt_end_col, state, hint_shown_last_time);

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
