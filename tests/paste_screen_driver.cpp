#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <algorithm>
#include <fstream>
#include <optional>
#include <string>
#include <vector>

namespace {

HANDLE g_in = INVALID_HANDLE_VALUE;
HANDLE g_out = INVALID_HANDLE_VALUE;
std::ofstream g_report;
int g_failures = 0;

void Check(bool ok, const std::string& message) {
    g_report << (ok ? "PASS: " : "FAIL: ") << message << "\n";
    g_report.flush();
    g_failures += ok ? 0 : 1;
}

std::wstring ReadRow(int row) {
    CONSOLE_SCREEN_BUFFER_INFO info{};
    GetConsoleScreenBufferInfo(g_out, &info);
    const int width = info.dwSize.X;
    std::vector<CHAR_INFO> cells(static_cast<std::size_t>(width));
    SMALL_RECT region{0, static_cast<SHORT>(row), static_cast<SHORT>(width - 1), static_cast<SHORT>(row)};
    if (!ReadConsoleOutputW(g_out, cells.data(), COORD{static_cast<SHORT>(width), 1}, COORD{0, 0}, &region)) {
        return {};
    }
    std::wstring text;
    for (const CHAR_INFO& cell : cells) {
        if ((cell.Attributes & COMMON_LVB_TRAILING_BYTE) == 0) {
            text.push_back(cell.Char.UnicodeChar);
        }
    }
    return text;
}

bool ScreenContains(const std::wstring& needle) {
    for (int row = 0; row < 200; ++row) {
        if (ReadRow(row).find(needle) != std::wstring::npos) {
            return true;
        }
    }
    return false;
}

bool WaitForAny(const std::vector<std::wstring>& needles, int timeout_ms) {
    const DWORD deadline = GetTickCount() + static_cast<DWORD>(timeout_ms);
    while (GetTickCount() < deadline) {
        for (const auto& needle : needles) {
            if (ScreenContains(needle)) {
                return true;
            }
        }
        Sleep(50);
    }
    return false;
}

bool WaitForStatusPanel(const std::wstring& cwd, const std::wstring& branch, int timeout_ms) {
    const DWORD deadline = GetTickCount() + static_cast<DWORD>(timeout_ms);
    while (GetTickCount() < deadline) {
        for (int row = 0; row < 200; ++row) {
            const std::wstring line = ReadRow(row);
            if (line.find(L"shift+tab") == std::wstring::npos) {
                continue;
            }
            if (line.find(cwd) != std::wstring::npos &&
                (branch.empty() || line.find(branch) != std::wstring::npos)) {
                return true;
            }
        }
        Sleep(50);
    }
    return false;
}

void SendKey(WORD vk, wchar_t ch, DWORD state = 0) {
    INPUT_RECORD records[2]{};
    for (int i = 0; i < 2; ++i) {
        records[i].EventType = KEY_EVENT;
        records[i].Event.KeyEvent.bKeyDown = i == 0 ? TRUE : FALSE;
        records[i].Event.KeyEvent.wRepeatCount = 1;
        records[i].Event.KeyEvent.wVirtualKeyCode = vk;
        records[i].Event.KeyEvent.uChar.UnicodeChar = ch;
        records[i].Event.KeyEvent.dwControlKeyState = state;
    }
    DWORD written = 0;
    WriteConsoleInputW(g_in, records, 2, &written);
}

void SendText(const std::wstring& text) {
    for (const wchar_t ch : text) {
        SendKey(0, ch);
    }
}

void SendTextBatch(const std::wstring& text) {
    std::vector<INPUT_RECORD> records;
    records.reserve(text.size() * 2);
    for (const wchar_t ch : text) {
        INPUT_RECORD down{};
        down.EventType = KEY_EVENT;
        down.Event.KeyEvent.bKeyDown = TRUE;
        down.Event.KeyEvent.wRepeatCount = 1;
        down.Event.KeyEvent.wVirtualKeyCode = ch == L'\n' ? VK_RETURN : 0;
        down.Event.KeyEvent.uChar.UnicodeChar = ch == L'\n' ? L'\r' : ch;
        records.push_back(down);
        down.Event.KeyEvent.bKeyDown = FALSE;
        records.push_back(down);
    }
    DWORD written = 0;
    WriteConsoleInputW(g_in, records.data(), static_cast<DWORD>(records.size()), &written);
}

void AppendKeyRecord(std::vector<INPUT_RECORD>& records, bool down, WORD vk, wchar_t ch, DWORD state = 0) {
    INPUT_RECORD record{};
    record.EventType = KEY_EVENT;
    record.Event.KeyEvent.bKeyDown = down ? TRUE : FALSE;
    record.Event.KeyEvent.wRepeatCount = 1;
    record.Event.KeyEvent.wVirtualKeyCode = vk;
    record.Event.KeyEvent.uChar.UnicodeChar = ch;
    record.Event.KeyEvent.dwControlKeyState = state;
    records.push_back(record);
}

void SendTerminalPasteBatch(const std::wstring& text) {
    // Windows Terminal 的真事件形状：快捷键 Ctrl/Shift down 会漏进输入流；
    // 正文里为符号合成出的 Shift down/up 也夹在字符之间。旧解析器正是
    // 碰到这类“有修饰键、没正文字符”的记录后，把整批 paste 判废。
    std::vector<INPUT_RECORD> records;
    records.reserve(text.size() * 2 + 8);
    AppendKeyRecord(records, true, VK_CONTROL, 0, LEFT_CTRL_PRESSED);
    AppendKeyRecord(records, true, VK_SHIFT, 0, LEFT_CTRL_PRESSED | SHIFT_PRESSED);
    for (std::size_t i = 0; i < text.size(); ++i) {
        if (i == 3) {
            AppendKeyRecord(records, true, VK_SHIFT, 0, SHIFT_PRESSED);
            AppendKeyRecord(records, false, VK_SHIFT, 0, 0);
        }
        const wchar_t ch = text[i];
        const WORD vk = ch == L'\n' ? VK_RETURN : (ch == L' ' ? VK_SPACE : L'A');
        const wchar_t emitted = ch == L'\n' ? L'\r' : ch;
        AppendKeyRecord(records, true, vk, emitted);
        AppendKeyRecord(records, false, vk, emitted);
    }
    AppendKeyRecord(records, false, 'V', 0x16, LEFT_CTRL_PRESSED | SHIFT_PRESSED);
    AppendKeyRecord(records, false, VK_SHIFT, 0, LEFT_CTRL_PRESSED);
    AppendKeyRecord(records, false, VK_CONTROL, 0, 0);
    DWORD written = 0;
    WriteConsoleInputW(g_in, records.data(), static_cast<DWORD>(records.size()), &written);
}

std::optional<std::wstring> ReadClipboardText() {
    if (OpenClipboard(nullptr) == FALSE) {
        return std::nullopt;
    }
    const HANDLE data = GetClipboardData(CF_UNICODETEXT);
    const auto* chars = data == nullptr ? nullptr : static_cast<const wchar_t*>(GlobalLock(data));
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

bool SetClipboardText(const std::wstring& text) {
    if (OpenClipboard(nullptr) == FALSE) {
        return false;
    }
    if (EmptyClipboard() == FALSE) {
        CloseClipboard();
        return false;
    }
    const std::size_t bytes = (text.size() + 1) * sizeof(wchar_t);
    HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE, bytes);
    auto* chars = memory == nullptr ? nullptr : static_cast<wchar_t*>(GlobalLock(memory));
    if (chars == nullptr) {
        if (memory != nullptr) {
            GlobalFree(memory);
        }
        CloseClipboard();
        return false;
    }
    std::copy(text.c_str(), text.c_str() + text.size() + 1, chars);
    GlobalUnlock(memory);
    if (SetClipboardData(CF_UNICODETEXT, memory) == nullptr) {
        GlobalFree(memory);
        CloseClipboard();
        return false;
    }
    CloseClipboard();
    return true;  // SetClipboardData 成功后由系统接管 memory
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    if (argc < 4) {
        return 2;
    }
    g_report.open(argv[3], std::ios::binary | std::ios::trunc);
    FreeConsole();
    if (!AllocConsole()) {
        return 2;
    }
    SECURITY_ATTRIBUTES attributes{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
    g_in = CreateFileW(L"CONIN$", GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, &attributes,
                       OPEN_EXISTING, 0, nullptr);
    g_out = CreateFileW(L"CONOUT$", GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, &attributes,
                        OPEN_EXISTING, 0, nullptr);
    SMALL_RECT tiny{0, 0, 1, 1};
    SetConsoleWindowInfo(g_out, TRUE, &tiny);
    SetConsoleScreenBufferSize(g_out, COORD{100, 200});
    SMALL_RECT window{0, 0, 99, 35};
    SetConsoleWindowInfo(g_out, TRUE, &window);
    FlushConsoleInputBuffer(g_in);

    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdInput = g_in;
    startup.hStdOutput = g_out;
    startup.hStdError = g_out;
    PROCESS_INFORMATION process{};
    std::wstring command = L"\"" + std::wstring(argv[1]) + L"\"";
    if (!CreateProcessW(argv[1], command.data(), nullptr, nullptr, TRUE, 0, nullptr, argv[2], &startup, &process)) {
        return 2;
    }
    CloseHandle(process.hThread);

    Check(WaitForAny({L"shift+tab"}, 30000), "composer ready");
    if (argc >= 5) {
        Check(WaitForStatusPanel(argv[2], argv[4], 5000),
              "status panel shows current working directory and git branch on one row");
    }
    SendKey(VK_ESCAPE, 0x1b);
    SendText(L"[200~alpha\nbeta\n");
    SendKey(VK_ESCAPE, 0x1b);
    SendText(L"[201~");

    Check(WaitForAny({L"beta"}, 5000) && ScreenContains(L"alpha"),
          "short multiline paste stays visible in the composer");
    Check(!ScreenContains(L"[Pasted Content 11 chars]") && !ScreenContains(L"[粘贴内容 11 字符]"),
          "short multiline paste does not use a placeholder");

    SendKey('C', 0x03, LEFT_CTRL_PRESSED);
    Sleep(200);
    SendTerminalPasteBatch(L"native\nburst");
    Check(WaitForAny({L"burst"}, 5000) && ScreenContains(L"native"),
          "modifier-wrapped Windows paste stays visible when short");
    Check(!ScreenContains(L"[Pasted Content 12 chars]") && !ScreenContains(L"[粘贴内容 12 字符]"),
          "short Windows paste burst does not use a placeholder");

    // VS Code/ConPTY 会把一次 paste 拆成逐行批次，批间停顿可远超旧实现的
    // 60ms。首行用 /help，旧实现若误提交也只打印帮助，不会请求真实模型；
    // 其余三行照用户报告里的 Python 形状投递。
    SendKey('C', 0x03, LEFT_CTRL_PRESSED);
    Sleep(200);
    const std::optional<std::wstring> saved_clipboard = ReadClipboardText();
    const std::wstring slow_paste =
        L"/help\n"
        L"    for j in range(i+1,len(nums)):\n"
        L"        if nums[i] + nums[j] == target:\n"
        L"            return [i,j]";
    Check(SetClipboardText(slow_paste), "test multiline content is present in the Windows clipboard");
    // 首行逐字投递，保证子进程在回车前已经把它画进 composer；这正是
    // VS Code 实机里旧回归没罩住的路径。虚拟键码故意填非零，兼测
    // ConPTY 不采用“合成键码 0”时仍能辨认。
    for (const wchar_t ch : std::wstring(L"/help")) {
        SendKey('A', ch);
        Sleep(15);
    }
    SendKey(VK_RETURN, L'\r');
    Sleep(150);
    SendTextBatch(L"    for j in range(i+1,len(nums)):\n");
    Sleep(150);
    SendTextBatch(L"        if nums[i] + nums[j] == target:\n");
    Sleep(150);
    SendTextBatch(L"            return [i,j]");
    Check(WaitForAny({L"return [i,j]"}, 5000) && ScreenContains(L"for j in range"),
          "slow line-split Windows paste is recovered as visible multiline text");
    const std::wstring pasted_chars = std::to_wstring(slow_paste.size());
    Check(!ScreenContains(L"[Pasted Content " + pasted_chars + L" chars]") &&
              !ScreenContains(L"[粘贴内容 " + pasted_chars + L" 字符]"),
          "recovered short Windows paste does not use a placeholder");
    if (saved_clipboard.has_value()) {
        Check(SetClipboardText(*saved_clipboard), "test restores the previous Windows clipboard text");
    }

    SendKey('C', 0x03, LEFT_CTRL_PRESSED);
    Sleep(200);
    const std::wstring large_paste(1001, L'x');
    SendKey(VK_ESCAPE, 0x1b);
    SendText(L"[200~");
    SendText(large_paste);
    SendKey(VK_ESCAPE, 0x1b);
    SendText(L"[201~");
    Check(WaitForAny({L"[Pasted Content 1001 chars]", L"[粘贴内容 1001 字符]"}, 5000),
          "paste over 1000 characters collapses to one placeholder");

    SendKey('C', 0x03, LEFT_CTRL_PRESSED);
    Sleep(200);
    SendText(L"exit");
    SendKey(VK_RETURN, L'\r');
    WaitForSingleObject(process.hProcess, 10000);
    DWORD exit_code = STILL_ACTIVE;
    GetExitCodeProcess(process.hProcess, &exit_code);
    Check(exit_code == 0, "process exits cleanly after clearing the paste");
    if (exit_code == STILL_ACTIVE) {
        TerminateProcess(process.hProcess, 3);
        WaitForSingleObject(process.hProcess, 5000);
    }
    CloseHandle(process.hProcess);
    g_report << "RESULT: " << (g_failures == 0 ? "ALL PASS" : "FAIL") << "\n";
    return g_failures == 0 ? 0 : 1;
}
