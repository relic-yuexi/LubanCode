// Windows 实现:控制台原语。全部搬自现有代码,逻辑一字未改——
//   StdinIsInteractive        <- cli/console_input.cpp 的 StdinIsRealConsole
//   ProbeStdoutConsole        <- cli/theme.cpp 的 DetectConsoleCapability(探测段)
//   ConsoleWidth              <- cli/console_input.cpp 的 DetectConsoleWidth
//   GetScreenInfo/SetCursorPos/ClearRowFrom
//                             <- console_input.cpp / main.cpp 里反复出现的
//                                GetConsoleScreenBufferInfo /
//                                SetConsoleCursorPosition /
//                                FillConsoleOutputCharacterW 三件套
//   RawInputScope             <- ReadLineKeyByKey 开头的 GetConsoleMode/
//                                SetConsoleMode + ModeGuard
//   KeyReader::ReadOne        <- ReadLineKeyByKey / TurnInputListener 里的
//                                ReadConsoleInputW + KEY_EVENT 翻译(含 UTF-16
//                                代理对配对;两处的映射合成这一份,超集)
//   ReadLineCooked            <- ReadLineFromConsole
//   SetupConsoleUtf8          <- wmain 开头的 SetConsoleOutputCP/SetConsoleCP
#include "platform/console.hpp"

#include "platform/paths.hpp"  // Utf8ToWide/WideToUtf8:输入侧宽窄转换共用 platform 那份(不许抛)

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <algorithm>
#include <iostream>
#include <iterator>
#include <deque>
#include <string_view>
#include <vector>

namespace lubancode::platform {

// 输入侧(剪贴板/按键)的宽窄转换直接用 paths_win 那两份(Utf8ToWide/
// WideToUtf8,经上面的 paths.hpp 引进来):同一份"坏字符替换 U+FFFD、
// 绝不抛"的合同,不在这只文件再养一份私有实现。

namespace {

std::deque<INPUT_RECORD>& PendingInputRecords() {
    static std::deque<INPUT_RECORD> records;
    return records;
}

bool ReadInputRecord(INPUT_RECORD& record, DWORD timeout_ms = INFINITE) {
    auto& pending = PendingInputRecords();
    if (!pending.empty()) {
        record = pending.front();
        pending.pop_front();
        return true;
    }
    const HANDLE input = GetStdHandle(STD_INPUT_HANDLE);
    if (timeout_ms != INFINITE && WaitForSingleObject(input, timeout_ms) != WAIT_OBJECT_0) {
        return false;
    }
    DWORD read = 0;
    return ReadConsoleInputW(input, &record, 1, &read) != 0 && read == 1;
}

void RestoreInputRecords(const std::vector<INPUT_RECORD>& records) {
    auto& pending = PendingInputRecords();
    for (auto it = records.rbegin(); it != records.rend(); ++it) {
        pending.push_front(*it);
    }
}

std::optional<wchar_t> ReadKeyChar(DWORD timeout_ms, std::vector<INPUT_RECORD>& consumed) {
    while (true) {
        INPUT_RECORD record{};
        if (!ReadInputRecord(record, timeout_ms)) {
            return std::nullopt;
        }
        consumed.push_back(record);
        if (record.EventType == KEY_EVENT && record.Event.KeyEvent.bKeyDown != FALSE &&
            record.Event.KeyEvent.uChar.UnicodeChar != 0) {
            return record.Event.KeyEvent.uChar.UnicodeChar;
        }
    }
}

std::wstring NormalizeNewlines(std::wstring_view text) {
    std::wstring normalized;
    normalized.reserve(text.size());
    for (std::size_t i = 0; i < text.size(); ++i) {
        if (text[i] == L'\r') {
            normalized.push_back(L'\n');
            if (i + 1 < text.size() && text[i + 1] == L'\n') {
                ++i;
            }
        } else {
            normalized.push_back(text[i]);
        }
    }
    return normalized;
}

std::optional<std::wstring> ReadClipboardText() {
    // VS Code 刚把正文写进剪贴板时，别的进程偶尔还攥着 clipboard 锁。
    // 略试几次便走；拿不到就退回原先的事件批探测，不耽误普通输入。
    bool opened = false;
    for (int attempt = 0; attempt < 5; ++attempt) {
        if (OpenClipboard(nullptr) != FALSE) {
            opened = true;
            break;
        }
        Sleep(5);
    }
    if (!opened) {
        return std::nullopt;
    }

    const HANDLE data = GetClipboardData(CF_UNICODETEXT);
    if (data == nullptr) {
        CloseClipboard();
        return std::nullopt;
    }
    const auto* chars = static_cast<const wchar_t*>(GlobalLock(data));
    if (chars == nullptr) {
        CloseClipboard();
        return std::nullopt;
    }
    const std::size_t capacity = GlobalSize(data) / sizeof(wchar_t);
    std::size_t length = 0;
    while (length < capacity && chars[length] != L'\0') {
        ++length;
    }
    std::wstring text(chars, length);
    GlobalUnlock(data);
    CloseClipboard();
    return text;
}

std::optional<std::wstring> MatchingMultilineClipboard(const std::wstring& prefix) {
    const std::optional<std::wstring> raw = ReadClipboardText();
    if (!raw.has_value()) {
        return std::nullopt;
    }
    std::wstring clipboard = NormalizeNewlines(*raw);
    if (clipboard.find(L'\n') == std::wstring::npos || prefix.size() > clipboard.size() ||
        clipboard.compare(0, prefix.size(), prefix) != 0) {
        return std::nullopt;
    }
    return clipboard;
}

std::optional<KeyInput> TryReadBracketedPaste() {
    constexpr std::wstring_view kStart = L"[200~";
    std::vector<INPUT_RECORD> probe;
    for (const wchar_t expected : kStart) {
        const std::optional<wchar_t> actual = ReadKeyChar(30, probe);
        if (!actual.has_value() || *actual != expected) {
            RestoreInputRecords(probe);
            return std::nullopt;
        }
    }

    constexpr std::wstring_view kEnd = L"\x1b[201~";
    std::wstring pasted;
    while (true) {
        INPUT_RECORD record{};
        if (!ReadInputRecord(record)) {
            break;
        }
        if (record.EventType != KEY_EVENT || record.Event.KeyEvent.bKeyDown == FALSE ||
            record.Event.KeyEvent.uChar.UnicodeChar == 0) {
            continue;
        }
        const KEY_EVENT_RECORD& key = record.Event.KeyEvent;
        const unsigned repeat = (std::max)(1U, static_cast<unsigned>(key.wRepeatCount));
        for (unsigned i = 0; i < repeat; ++i) {
            pasted.push_back(key.uChar.UnicodeChar);
        }
        if (pasted.size() >= kEnd.size() &&
            pasted.compare(pasted.size() - kEnd.size(), kEnd.size(), kEnd) == 0) {
            pasted.resize(pasted.size() - kEnd.size());
            break;
        }
    }

    KeyInput out;
    out.kind = KeyInput::Kind::Paste;
    out.text = WideToUtf8(NormalizeNewlines(pasted));
    return out;
}

bool AppendNativePasteKey(const INPUT_RECORD& record, std::wstring& text, bool& saw_newline,
                          bool& saw_text_after_newline) {
    if (record.EventType != KEY_EVENT) {
        return false;
    }
    const KEY_EVENT_RECORD& key = record.Event.KeyEvent;
    if (key.bKeyDown == FALSE) {
        return true;  // key-up 是同一批输入的一半，不进正文
    }
    const bool modifier_only = key.uChar.UnicodeChar == 0 &&
                               (key.wVirtualKeyCode == VK_SHIFT || key.wVirtualKeyCode == VK_LSHIFT ||
                                key.wVirtualKeyCode == VK_RSHIFT || key.wVirtualKeyCode == VK_CONTROL ||
                                key.wVirtualKeyCode == VK_LCONTROL || key.wVirtualKeyCode == VK_RCONTROL ||
                                key.wVirtualKeyCode == VK_MENU || key.wVirtualKeyCode == VK_LMENU ||
                                key.wVirtualKeyCode == VK_RMENU);
    if (modifier_only) {
        // Windows Terminal 把 paste 还原成一串逼真的 KEY_EVENT：括号、下划线
        // 前后会夹 Shift down/up，快捷键本身也会漏进 Ctrl/Shift down。它们
        // 没有正文字符，只是这趟 paste 的骨架，跳过即可。
        return true;
    }
    const bool ctrl = (key.dwControlKeyState & (LEFT_CTRL_PRESSED | RIGHT_CTRL_PRESSED)) != 0;
    const bool alt = (key.dwControlKeyState & (LEFT_ALT_PRESSED | RIGHT_ALT_PRESSED)) != 0;
    if (ctrl || alt) {
        return false;
    }

    wchar_t ch = key.uChar.UnicodeChar;
    if (key.wVirtualKeyCode == VK_RETURN || ch == L'\r' || ch == L'\n') {
        if (saw_newline) {
            saw_text_after_newline = true;  // 第二枚换行本身也说明这是多行内容
        }
        ch = L'\n';
        saw_newline = true;
    } else if (ch != L'\t' && ch < L' ') {
        return false;
    } else if (saw_newline) {
        saw_text_after_newline = true;
    }

    const unsigned repeat = (std::max)(1U, static_cast<unsigned>(key.wRepeatCount));
    text.append(repeat, ch);
    return true;
}

// VS Code/ConPTY 并不保证 bracketed paste 会以一条完整 VT 序列出现在
// ReadConsoleInputW 里；有些组合只把整段文字压成一批 KEY_EVENT。若照逐键
// 翻译，其中第一枚 VK_RETURN 会直接提交，余下各行便散成多条消息。
//
// 这里只认“一批连续文本里，换行后还有正文”的形状。ConPTY 可能把一次
// paste 拆成几批，第一批还偏生停在换行上；一见换行便留 60ms 的空闲窗口
// 续收，直到整批安静下来。普通打字只在按 Enter 时多等这一小拍，换行后
// 没正文仍按提交处理；方向键、Ctrl/Alt 等编辑键一混进来也整批原样放回。
std::optional<KeyInput> TryReadNativePasteBurst(const INPUT_RECORD& first) {
    std::vector<INPUT_RECORD> tail;
    INPUT_RECORD record{};
    while (ReadInputRecord(record, 0)) {
        tail.push_back(record);
    }

    std::wstring text;
    bool saw_newline = false;
    bool saw_text_after_newline = false;
    bool valid = AppendNativePasteKey(first, text, saw_newline, saw_text_after_newline);
    for (const INPUT_RECORD& item : tail) {
        if (!AppendNativePasteKey(item, text, saw_newline, saw_text_after_newline)) {
            valid = false;
            break;
        }
    }

    // ConPTY 有时按“每行一批”投递。一批之间能隔上百毫秒，靠空闲窗口
    // 猜边界终究会漏。首批若已含换行，就同 Windows 剪贴板逐字核对；
    // 对上后按剪贴板的确切长度收齐，既不误吞手敲 Enter，也不怕批间停顿。
    if (valid && saw_newline) {
        if (const std::optional<std::wstring> clipboard = MatchingMultilineClipboard(text);
            clipboard.has_value()) {
            constexpr ULONGLONG kClipboardCompletionMs = 2000;
            const ULONGLONG deadline = GetTickCount64() + kClipboardCompletionMs;
            while (valid && text.size() < clipboard->size()) {
                const ULONGLONG now = GetTickCount64();
                if (now >= deadline || !ReadInputRecord(record, static_cast<DWORD>(deadline - now))) {
                    break;
                }
                tail.push_back(record);
                if (record.EventType != KEY_EVENT || record.Event.KeyEvent.bKeyDown == FALSE) {
                    continue;
                }
                if (!AppendNativePasteKey(record, text, saw_newline, saw_text_after_newline) ||
                    text.size() > clipboard->size() || clipboard->compare(0, text.size(), text) != 0) {
                    valid = false;
                }
            }
            if (valid && text == *clipboard) {
                KeyInput out;
                out.kind = KeyInput::Kind::Paste;
                out.text = WideToUtf8(text);
                return out;
            }
        }
    }

    if (valid && saw_newline) {
        constexpr DWORD kContinuationIdleMs = 60;
        constexpr ULONGLONG kMaxContinuationMs = 1000;
        const ULONGLONG deadline = GetTickCount64() + kMaxContinuationMs;
        while (GetTickCount64() < deadline) {
            if (!ReadInputRecord(record, kContinuationIdleMs)) {
                break;  // 连续 60ms 没新字，这趟 paste 收口
            }
            tail.push_back(record);
            if (!AppendNativePasteKey(record, text, saw_newline, saw_text_after_newline)) {
                valid = false;
                break;
            }
            // 一枚到手后，把同一刻已经排队的也全吸进来；下一圈再等后续批。
            while (ReadInputRecord(record, 0)) {
                tail.push_back(record);
                if (!AppendNativePasteKey(record, text, saw_newline, saw_text_after_newline)) {
                    valid = false;
                    break;
                }
            }
            if (!valid) {
                break;
            }
        }
    }
    if (!valid || !saw_newline || !saw_text_after_newline) {
        RestoreInputRecords(tail);
        return std::nullopt;
    }

    KeyInput out;
    out.kind = KeyInput::Kind::Paste;
    out.text = WideToUtf8(text);
    return out;
}

std::optional<KeyInput> TryFinishClipboardPaste(std::wstring received, std::size_t replace_before) {
    const std::optional<std::wstring> clipboard = MatchingMultilineClipboard(received);
    if (!clipboard.has_value()) {
        return std::nullopt;
    }

    std::vector<INPUT_RECORD> consumed;
    bool saw_newline = received.find(L'\n') != std::wstring::npos;
    bool saw_text_after_newline = false;
    constexpr ULONGLONG kCompletionMs = 2000;
    const ULONGLONG deadline = GetTickCount64() + kCompletionMs;
    while (received.size() < clipboard->size()) {
        const ULONGLONG now = GetTickCount64();
        INPUT_RECORD record{};
        if (now >= deadline || !ReadInputRecord(record, static_cast<DWORD>(deadline - now))) {
            RestoreInputRecords(consumed);
            return std::nullopt;
        }
        consumed.push_back(record);
        if (record.EventType != KEY_EVENT || record.Event.KeyEvent.bKeyDown == FALSE) {
            continue;
        }
        if (!AppendNativePasteKey(record, received, saw_newline, saw_text_after_newline) ||
            received.size() > clipboard->size() || clipboard->compare(0, received.size(), received) != 0) {
            RestoreInputRecords(consumed);
            return std::nullopt;
        }
    }

    KeyInput out;
    out.kind = KeyInput::Kind::Paste;
    out.text = WideToUtf8(received);
    out.replace_before = replace_before;
    return out;
}

}  // namespace

bool StdinIsInteractive() {
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

StdoutConsoleProbe ProbeStdoutConsole() {
    StdoutConsoleProbe probe;
    const HANDLE h = GetStdHandle(STD_OUTPUT_HANDLE);
    if (h != nullptr && h != INVALID_HANDLE_VALUE && GetFileType(h) == FILE_TYPE_CHAR) {
        DWORD mode = 0;
        if (GetConsoleMode(h, &mode) != 0) {
            probe.is_console = true;
            if ((mode & ENABLE_VIRTUAL_TERMINAL_PROCESSING) != 0) {
                probe.vt_enabled = true;
            } else if (SetConsoleMode(h, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING) != 0) {
                probe.vt_enabled = true;
            }
        }
    }
    return probe;
}

std::optional<int> ConsoleWidth() {
    const HANDLE h_out = GetStdHandle(STD_OUTPUT_HANDLE);
    if (h_out == nullptr || h_out == INVALID_HANDLE_VALUE) {
        return std::nullopt;
    }
    CONSOLE_SCREEN_BUFFER_INFO info{};
    if (!GetConsoleScreenBufferInfo(h_out, &info)) {
        return std::nullopt;
    }
    return static_cast<int>(info.dwSize.X);
}

bool SupportsScreenRepaint() {
    // 真探测,跟 TranscriptPainter 实际用来定锚点的 API 同一把尺——之前
    // 写死 true,mintty/ConPTY 之类 GetConsoleScreenBufferInfo 靠不住的
    // 环境里 TranscriptPainter 会以为自己能原地改写,实际锚点全程拿不到
    // 准确坐标,子代理连打工具调用时会画出"一黄一绿"的重复行(旧状态没
    // 擦掉、新状态又在别处新起一行)。别再无条件 true,也别另立一套
    // is_console 标准。
    return GetScreenInfo().has_value();
}

std::optional<ScreenInfo> GetScreenInfo() {
    const HANDLE h_out = GetStdHandle(STD_OUTPUT_HANDLE);
    CONSOLE_SCREEN_BUFFER_INFO info{};
    if (!GetConsoleScreenBufferInfo(h_out, &info)) {
        return std::nullopt;
    }
    ScreenInfo out;
    out.width = static_cast<int>(info.dwSize.X);
    out.height = static_cast<int>(info.dwSize.Y);
    out.cursor_x = static_cast<int>(info.dwCursorPosition.X);
    out.cursor_y = static_cast<int>(info.dwCursorPosition.Y);
    out.viewport_x = static_cast<int>(info.srWindow.Left);
    out.viewport_y = static_cast<int>(info.srWindow.Top);
    out.viewport_height = static_cast<int>(info.srWindow.Bottom) - static_cast<int>(info.srWindow.Top) + 1;
    return out;
}

void SetCursorPos(int x, int y) {
    const HANDLE h_out = GetStdHandle(STD_OUTPUT_HANDLE);
    SetConsoleCursorPosition(h_out, COORD{static_cast<SHORT>(x), static_cast<SHORT>(y)});
}

void ClearScreen() {
    // 光标归位 + 清可见区 + 清回滚缓冲。VT 序列走 std::cout(main.cpp 那份
    // 调用点已经用 is_console 判断过要不要调这个函数),不碰 Win32
    // FillConsoleOutputCharacterW 那套——ENABLE_VIRTUAL_TERMINAL_PROCESSING
    // 在真控制台/现代终端下开着,ProbeStdoutConsole 已经保证过。
    std::cout << "\x1b[H\x1b[2J\x1b[3J" << std::flush;
}

void ClearRowFrom(int x, int y, int count) {
    if (count <= 0) {
        return;
    }
    const HANDLE h_out = GetStdHandle(STD_OUTPUT_HANDLE);
    DWORD written = 0;
    FillConsoleOutputCharacterW(h_out, L' ', static_cast<DWORD>(count),
                                 COORD{static_cast<SHORT>(x), static_cast<SHORT>(y)}, &written);
}

void ClearRowHardFrom(int x, int y, int count) {
    if (count <= 0) {
        return;
    }
    const HANDLE h_out = GetStdHandle(STD_OUTPUT_HANDLE);
    CONSOLE_SCREEN_BUFFER_INFO info{};
    DWORD written = 0;
    const COORD pos{static_cast<SHORT>(x), static_cast<SHORT>(y)};
    FillConsoleOutputCharacterW(h_out, L' ', static_cast<DWORD>(count), pos, &written);
    if (GetConsoleScreenBufferInfo(h_out, &info)) {
        FillConsoleOutputAttribute(h_out, info.wAttributes, static_cast<DWORD>(count), pos, &written);
    }
}

int PanViewportDown(int rows) {
    if (rows <= 0) {
        return 0;
    }
    const HANDLE h_out = GetStdHandle(STD_OUTPUT_HANDLE);
    CONSOLE_SCREEN_BUFFER_INFOEX info{};
    info.cbSize = sizeof(info);
    if (!GetConsoleScreenBufferInfoEx(h_out, &info)) {
        return 0;
    }
    const int buffer_bottom = static_cast<int>(info.dwSize.Y) - 1;
    const int viewport_bottom = static_cast<int>(info.srWindow.Bottom);
    const int room = buffer_bottom - viewport_bottom;
    if (room <= 0) {
        return 0;  // 窗口已贴缓冲区底:没有可平移的余地,调用方退回滚内容
    }
    const int pan = (std::min)(rows, room);
    info.srWindow.Top += static_cast<SHORT>(pan);
    info.srWindow.Bottom += static_cast<SHORT>(pan);
    // dwCursorPosition 原样带回(缓冲没滚,绝对坐标仍有效),光标一个不挪
    // ——挪了反而可能触发控制台"把光标带回视野"的反向滚动,平移白做。
    if (!SetConsoleScreenBufferInfoEx(h_out, &info)) {
        return 0;
    }
    return pan;
}

RawInputScope::RawInputScope() {
    const HANDLE h_in = GetStdHandle(STD_INPUT_HANDLE);
    DWORD original_mode = 0;
    if (!GetConsoleMode(h_in, &original_mode)) {
        return;
    }
    // 关掉行编辑、回显、Ctrl+C 自动处理(这三样都要自己接管):
    //   ENABLE_LINE_INPUT      —— 关掉之后 ReadConsoleInputW 才能逐键拿到,不用等回车
    //   ENABLE_ECHO_INPUT      —— 关掉控制台自带回显,回显交给我们自己按 RenderState 重画
    //   ENABLE_PROCESSED_INPUT —— 关掉之后 Ctrl+C 才会以 KEY_EVENT 形式读到,不会
    //                             被控制台直接当 SIGINT 处理掉、绕过我们的按键循环
    const DWORD new_mode = (original_mode & ~static_cast<DWORD>(ENABLE_LINE_INPUT) &
                             ~static_cast<DWORD>(ENABLE_ECHO_INPUT) & ~static_cast<DWORD>(ENABLE_PROCESSED_INPUT));
    if (!SetConsoleMode(h_in, new_mode)) {
        return;
    }
    original_mode_ = original_mode;
    ok_ = true;
}

RawInputScope::~RawInputScope() {
    if (ok_) {
        SetConsoleMode(GetStdHandle(STD_INPUT_HANDLE), static_cast<DWORD>(original_mode_));
    }
}

std::optional<KeyInput> KeyReader::ReadOne() {
    const auto reset_text_run = [this]() {
        rapid_text_run_.clear();
        rapid_char_count_ = 0;
        last_text_tick_ = 0;
    };
    INPUT_RECORD record{};
    if (!ReadInputRecord(record)) {
        return std::nullopt;
    }
    if (record.EventType != KEY_EVENT || record.Event.KeyEvent.bKeyDown == FALSE) {
        return KeyInput{};  // None
    }
    if (auto paste = TryReadNativePasteBurst(record); paste.has_value()) {
        if (!rapid_text_run_.empty()) {
            std::wstring combined = rapid_text_run_ + Utf8ToWide(paste->text);
            if (combined.find(L'\n') != std::wstring::npos) {
                if (auto completed = TryFinishClipboardPaste(std::move(combined), rapid_char_count_);
                    completed.has_value()) {
                    reset_text_run();
                    return completed;
                }
            }
        }
        reset_text_run();
        return paste;
    }
    const KEY_EVENT_RECORD& ke = record.Event.KeyEvent;
    const bool ctrl = (ke.dwControlKeyState & (LEFT_CTRL_PRESSED | RIGHT_CTRL_PRESSED)) != 0;
    const bool shift = (ke.dwControlKeyState & SHIFT_PRESSED) != 0;
    const bool alt = (ke.dwControlKeyState & (LEFT_ALT_PRESSED | RIGHT_ALT_PRESSED)) != 0;

    KeyInput out;
    if (ctrl && ke.wVirtualKeyCode == 'C') {
        reset_text_run();
        out.kind = KeyInput::Kind::CtrlC;
    } else if (ctrl && ke.wVirtualKeyCode == 'D') {
        reset_text_run();
        out.kind = KeyInput::Kind::CtrlD;
    } else if (ctrl && ke.wVirtualKeyCode == 'O') {
        reset_text_run();
        out.kind = KeyInput::Kind::CtrlO;  // UI-D:紧凑/详细切换
    } else if (ctrl && ke.wVirtualKeyCode == 'E') {
        reset_text_run();
        out.kind = KeyInput::Kind::CtrlE;  // UI-D:聚焦查看
    } else if (ctrl && ke.wVirtualKeyCode == 'X') {
        reset_text_run();
        out.kind = KeyInput::Kind::CtrlX;  // 子代理面板:停止全部(两段确认第一段)
    } else if (ctrl && ke.wVirtualKeyCode == 'K') {
        reset_text_run();
        out.kind = KeyInput::Kind::CtrlK;  // 子代理面板:停止全部(两段确认第二段)
    } else if (ctrl && ke.wVirtualKeyCode == 'L') {
        reset_text_run();
        out.kind = KeyInput::Kind::CtrlL;  // 底栏自救:整屏重画
    } else if (ctrl && ke.wVirtualKeyCode == 'P') {
        reset_text_run();
        out.kind = KeyInput::Kind::CtrlP;  // 历史:上一条(明确别名,不受多行位置影响)
    } else if (ctrl && ke.wVirtualKeyCode == 'N') {
        reset_text_run();
        out.kind = KeyInput::Kind::CtrlN;  // 历史:下一条
    } else if (ke.wVirtualKeyCode == VK_BACK) {
        reset_text_run();
        out.kind = KeyInput::Kind::Backspace;
    } else if (ke.wVirtualKeyCode == VK_LEFT) {
        // Shift+Left / Ctrl+Left 单独翻出来(排队消息取回键与可配置备用键)。
        // 纯 Shift/Ctrl 不叠别的修饰才认;Alt+Left 这类"历史后退"键照旧当 Left。
        reset_text_run();
        if (shift && !ctrl && !alt) {
            out.kind = KeyInput::Kind::ShiftLeft;
        } else if (ctrl && !shift && !alt) {
            out.kind = KeyInput::Kind::CtrlLeft;
        } else {
            out.kind = KeyInput::Kind::Left;
        }
    } else if (ke.wVirtualKeyCode == VK_RIGHT) {
        reset_text_run();
        out.kind = KeyInput::Kind::Right;
    } else if (ke.wVirtualKeyCode == VK_HOME) {
        reset_text_run();
        out.kind = KeyInput::Kind::Home;
    } else if (ke.wVirtualKeyCode == VK_END) {
        reset_text_run();
        out.kind = KeyInput::Kind::End;
    } else if (ke.wVirtualKeyCode == VK_UP) {
        reset_text_run();
        out.kind = KeyInput::Kind::Up;
    } else if (ke.wVirtualKeyCode == VK_DOWN) {
        reset_text_run();
        out.kind = KeyInput::Kind::Down;
    } else if (ke.wVirtualKeyCode == VK_DELETE) {
        reset_text_run();
        out.kind = KeyInput::Kind::Delete;
    } else if (ke.wVirtualKeyCode == VK_TAB) {
        reset_text_run();
        out.kind = shift ? KeyInput::Kind::ShiftTab : KeyInput::Kind::Tab;
    } else if (ke.wVirtualKeyCode == VK_RETURN) {
        // UI-A:Alt+Enter / Shift+Enter 都翻成 NewLine(插换行)。实测这台
        // 机器:Windows Terminal 把 Alt+Enter 绑成了全屏切换,keydown 根本
        // 进不了输入缓冲(只漏一个 keyup,bKeyDown==FALSE 早被上面滤掉),
        // 等于天然收不到;conhost 下两个组合都完好。所以两个都认,文档里
        // 推荐 Shift+Enter。非 composer 读取里 NewLine 会被核心层当 Enter,
        // 确认提示那些单行场景语义不变。
        if (!shift && !alt && !rapid_text_run_.empty()) {
            std::wstring received = rapid_text_run_;
            received.push_back(L'\n');  // 当前 VK_RETURN 已从输入队列取走
            if (auto paste = TryFinishClipboardPaste(std::move(received), rapid_char_count_);
                paste.has_value()) {
                reset_text_run();
                return paste;
            }
        }
        reset_text_run();
        out.kind = (shift || alt) ? KeyInput::Kind::NewLine : KeyInput::Kind::Enter;
    } else if (ke.wVirtualKeyCode == VK_ESCAPE) {
        if (auto paste = TryReadBracketedPaste(); paste.has_value()) {
            reset_text_run();
            return paste;
        }
        reset_text_run();
        out.kind = KeyInput::Kind::Esc;
    } else if (ke.uChar.UnicodeChar != 0 && ctrl && !alt &&
               ke.wVirtualKeyCode >= 'A' && ke.wVirtualKeyCode <= 'Z') {
        // Ctrl+字母(未列专枚举的那批):按 Char 送出并置 ctrl,交给
        // cli/keymap 和弦层分派(编辑器核心对带修饰的 Char 不当正文插)。
        // 输入法/死键不会走到这——它们不带 VK_ 字母码。
        reset_text_run();
        out.kind = KeyInput::Kind::Char;
        out.ch = static_cast<char32_t>(ke.wVirtualKeyCode - 'A' + 'a');
        out.ctrl = true;
    } else if (ke.uChar.UnicodeChar != 0 && !ctrl && alt &&
               ((ke.wVirtualKeyCode >= 'A' && ke.wVirtualKeyCode <= 'Z') ||
                (ke.wVirtualKeyCode >= '0' && ke.wVirtualKeyCode <= '9'))) {
        // Alt+字母/数字:同上,作和弦送出(Alt+V 贴图这类)。原先是当
        // 普通字符插进编辑器——按住 Alt 打字不是正常输入姿势,改判和弦。
        reset_text_run();
        out.kind = KeyInput::Kind::Char;
        out.ch = static_cast<char32_t>(ke.wVirtualKeyCode >= 'A' && ke.wVirtualKeyCode <= 'Z'
                                           ? ke.wVirtualKeyCode - 'A' + 'a'
                                           : ke.wVirtualKeyCode);
        out.alt = true;
    } else if (ke.uChar.UnicodeChar != 0 && !ctrl) {
        const wchar_t wc = ke.uChar.UnicodeChar;
        constexpr ULONGLONG kRapidTextGapMs = 50;
        const ULONGLONG now = GetTickCount64();
        if (!rapid_text_run_.empty() && now - last_text_tick_ > kRapidTextGapMs) {
            reset_text_run();
        }
        rapid_text_run_.push_back(wc);
        last_text_tick_ = now;
        if (wc >= 0xD800 && wc <= 0xDBFF) {
            pending_high_surrogate_ = static_cast<char32_t>(wc);
            return KeyInput{};  // 高代理项,等低代理项凑成一个完整码点再交
        }
        char32_t cp = static_cast<char32_t>(wc);
        if (wc >= 0xDC00 && wc <= 0xDFFF && pending_high_surrogate_.has_value()) {
            const char32_t high = *pending_high_surrogate_;
            pending_high_surrogate_.reset();
            cp = 0x10000 + ((high - 0xD800) << 10) + (static_cast<char32_t>(wc) - 0xDC00);
        }
        if (cp >= 0x20) {  // 过滤掉控制字符(Esc、独立的 Tab 已经在上面单独处理)
            ++rapid_char_count_;
            out.kind = KeyInput::Kind::Char;
            out.ch = cp;
        }
    }
    return out;
}

bool WaitForKeyEvent(int timeout_ms) {
    // 粘贴探测把事件吸干又还进了内部缓冲，这些事件不再让控制台句柄有信号。
    // 缓冲里还有货就算有键，别去干等句柄（输入法整词提交/快打连击会中招）。
    if (!PendingInputRecords().empty()) {
        return true;
    }
    const HANDLE h_in = GetStdHandle(STD_INPUT_HANDLE);
    return WaitForSingleObject(h_in, timeout_ms > 0 ? static_cast<DWORD>(timeout_ms) : 0) == WAIT_OBJECT_0;
}

KeyListenScope::KeyListenScope() = default;
KeyListenScope::~KeyListenScope() = default;

std::optional<std::string> ReadLineCooked() {
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
        return std::nullopt;  // 单独一个 Ctrl+Z:EOF
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

void SetupConsoleUtf8() {
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);
}

}  // namespace lubancode::platform
