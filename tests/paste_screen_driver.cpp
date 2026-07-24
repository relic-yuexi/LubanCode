#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <algorithm>
#include <fstream>
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

    const bool placeholder = WaitForAny({L"[Pasted Content 11 chars]", L"[粘贴内容 11 字符]"}, 5000);
    Check(placeholder, "multiline paste is collapsed to one placeholder");
    Check(!ScreenContains(L"alpha") && !ScreenContains(L"beta"), "raw pasted lines stay out of the composer");

    SendKey('C', 0x03, LEFT_CTRL_PRESSED);
    Sleep(200);
    SendTextBatch(L"native\nburst");
    const bool native_placeholder =
        WaitForAny({L"[Pasted Content 12 chars]", L"[粘贴内容 12 字符]"}, 5000);
    Check(native_placeholder, "unmarked Windows KEY_EVENT paste burst is collapsed to one placeholder");
    Check(!ScreenContains(L"native") && !ScreenContains(L"burst"),
          "unmarked paste burst does not leak raw lines into the composer");

    SendKey('C', 0x03, LEFT_CTRL_PRESSED);
    Sleep(200);
    SendText(L"exit");
    SendKey(VK_RETURN, L'\r');
    WaitForSingleObject(process.hProcess, 10000);
    DWORD exit_code = STILL_ACTIVE;
    GetExitCodeProcess(process.hProcess, &exit_code);
    Check(exit_code == 0, "process exits cleanly after clearing the paste");
    CloseHandle(process.hProcess);
    g_report << "RESULT: " << (g_failures == 0 ? "ALL PASS" : "FAIL") << "\n";
    return g_failures == 0 ? 0 : 1;
}
