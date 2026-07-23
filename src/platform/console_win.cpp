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

std::string WideToUtf8(const std::wstring& wide) {
    const int size = WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), nullptr, 0,
                                         nullptr, nullptr);
    std::string out;
    if (size > 0) {
        out.resize(static_cast<std::size_t>(size));
        WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), out.data(), size, nullptr,
                            nullptr);
    }
    return out;
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

    std::wstring normalized;
    normalized.reserve(pasted.size());
    for (std::size_t i = 0; i < pasted.size(); ++i) {
        if (pasted[i] == L'\r') {
            normalized.push_back(L'\n');
            if (i + 1 < pasted.size() && pasted[i + 1] == L'\n') {
                ++i;
            }
        } else {
            normalized.push_back(pasted[i]);
        }
    }
    KeyInput out;
    out.kind = KeyInput::Kind::Paste;
    out.text = WideToUtf8(normalized);
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
    INPUT_RECORD record{};
    if (!ReadInputRecord(record)) {
        return std::nullopt;
    }
    if (record.EventType != KEY_EVENT || record.Event.KeyEvent.bKeyDown == FALSE) {
        return KeyInput{};  // None
    }
    const KEY_EVENT_RECORD& ke = record.Event.KeyEvent;
    const bool ctrl = (ke.dwControlKeyState & (LEFT_CTRL_PRESSED | RIGHT_CTRL_PRESSED)) != 0;
    const bool shift = (ke.dwControlKeyState & SHIFT_PRESSED) != 0;
    const bool alt = (ke.dwControlKeyState & (LEFT_ALT_PRESSED | RIGHT_ALT_PRESSED)) != 0;

    KeyInput out;
    if (ctrl && ke.wVirtualKeyCode == 'C') {
        out.kind = KeyInput::Kind::CtrlC;
    } else if (ctrl && ke.wVirtualKeyCode == 'D') {
        out.kind = KeyInput::Kind::CtrlD;
    } else if (ctrl && ke.wVirtualKeyCode == 'O') {
        out.kind = KeyInput::Kind::CtrlO;  // UI-D:紧凑/详细切换
    } else if (ctrl && ke.wVirtualKeyCode == 'E') {
        out.kind = KeyInput::Kind::CtrlE;  // UI-D:聚焦查看
    } else if (ke.wVirtualKeyCode == VK_BACK) {
        out.kind = KeyInput::Kind::Backspace;
    } else if (ke.wVirtualKeyCode == VK_LEFT) {
        out.kind = KeyInput::Kind::Left;
    } else if (ke.wVirtualKeyCode == VK_RIGHT) {
        out.kind = KeyInput::Kind::Right;
    } else if (ke.wVirtualKeyCode == VK_HOME) {
        out.kind = KeyInput::Kind::Home;
    } else if (ke.wVirtualKeyCode == VK_END) {
        out.kind = KeyInput::Kind::End;
    } else if (ke.wVirtualKeyCode == VK_UP) {
        out.kind = KeyInput::Kind::Up;
    } else if (ke.wVirtualKeyCode == VK_DOWN) {
        out.kind = KeyInput::Kind::Down;
    } else if (ke.wVirtualKeyCode == VK_TAB) {
        out.kind = shift ? KeyInput::Kind::ShiftTab : KeyInput::Kind::Tab;
    } else if (ke.wVirtualKeyCode == VK_RETURN) {
        // UI-A:Alt+Enter / Shift+Enter 都翻成 NewLine(插换行)。实测这台
        // 机器:Windows Terminal 把 Alt+Enter 绑成了全屏切换,keydown 根本
        // 进不了输入缓冲(只漏一个 keyup,bKeyDown==FALSE 早被上面滤掉),
        // 等于天然收不到;conhost 下两个组合都完好。所以两个都认,文档里
        // 推荐 Shift+Enter。非 composer 读取里 NewLine 会被核心层当 Enter,
        // 确认提示那些单行场景语义不变。
        out.kind = (shift || alt) ? KeyInput::Kind::NewLine : KeyInput::Kind::Enter;
    } else if (ke.wVirtualKeyCode == VK_ESCAPE) {
        if (auto paste = TryReadBracketedPaste(); paste.has_value()) {
            return paste;
        }
        out.kind = KeyInput::Kind::Esc;
    } else if (ke.uChar.UnicodeChar != 0 && !ctrl) {
        const wchar_t wc = ke.uChar.UnicodeChar;
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
            out.kind = KeyInput::Kind::Char;
            out.ch = cp;
        }
    }
    return out;
}

bool WaitForKeyEvent(int timeout_ms) {
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
