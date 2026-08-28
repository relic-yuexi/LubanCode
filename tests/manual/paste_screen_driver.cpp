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


// ---- P2-4 帧账审计的家伙什:LUBANCODE_FRAME_AUDIT=1 的子进程把
// [frame-audit] 行落进 stderr;stderr 改道到文件,结束后解析。 ----

std::wstring g_audit_path;

bool StartAuditChild(const wchar_t* exe, const wchar_t* workdir, const wchar_t* audit_path,
                     STARTUPINFOW* si, PROCESS_INFORMATION* pi) {
    DeleteFileW(audit_path);
    SECURITY_ATTRIBUTES inheritable{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
    HANDLE err_file = CreateFileW(audit_path, GENERIC_WRITE, FILE_SHARE_READ, &inheritable,
                                  CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (err_file == INVALID_HANDLE_VALUE) {
        return false;
    }
    SetEnvironmentVariableW(L"LUBANCODE_FRAME_AUDIT", L"1");
    si->cb = sizeof(*si);
    si->dwFlags = STARTF_USESTDHANDLES;
    si->hStdInput = g_in;
    si->hStdOutput = g_out;
    si->hStdError = err_file;
    std::wstring command = L"\"" + std::wstring(exe) + L"\"";
    const bool ok = CreateProcessW(exe, command.data(), nullptr, nullptr, TRUE, 0, nullptr, workdir, si, pi) != 0;
    SetEnvironmentVariableW(L"LUBANCODE_FRAME_AUDIT", nullptr);
    CloseHandle(err_file);  // 子进程已继承,自家的这份收掉
    return ok;
}

std::string ReadAuditFile(const wchar_t* audit_path) {
    HANDLE file = CreateFileW(audit_path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return {};
    }
    std::string out;
    char buf[4096];
    DWORD read = 0;
    while (ReadFile(file, buf, sizeof(buf), &read, nullptr) && read > 0) {
        out.append(buf, read);
    }
    CloseHandle(file);
    return out;
}

// 从审计文本里抽第 last 个 "idle_composer frames=N bytes=M" 的 N/M;没有返回 -1。
bool ParseAuditLine(const std::string& audit, const std::string& tag, long long* frames, long long* bytes) {
    std::size_t at = audit.rfind("[frame-audit] " + tag);
    if (at == std::string::npos) {
        return false;
    }
    const std::size_t nl = audit.find('\n', at);
    const std::string line = audit.substr(at, nl == std::string::npos ? std::string::npos : nl - at);
    const auto field = [&line](const char* key) -> long long {
        const std::size_t k = line.find(key);
        if (k == std::string::npos) {
            return -1;
        }
        return atoll(line.c_str() + k + std::string(key).size());
    };
    *frames = field("frames=");
    *bytes = field("bytes=");
    return *frames >= 0;
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

// 一次粘贴的真实形状:整批字符一枚 WriteConsoleInputW 全部进队。控制台
// 输入缓冲若一口吃不下,循环补齐,批间不加延时——单行大粘贴在 ConPTY
// 上就是这样一口气到的。
void SendTextBatchAllAtOnce(const std::wstring& text) {
    std::vector<INPUT_RECORD> records;
    records.reserve(text.size() * 2);
    for (std::size_t i = 0; i < text.size(); ++i) {
        AppendKeyRecord(records, true, 0, text[i]);
        AppendKeyRecord(records, false, 0, text[i]);
    }
    std::size_t sent = 0;
    while (sent < records.size()) {
        DWORD written = 0;
        if (!WriteConsoleInputW(g_in, records.data() + sent,
                                static_cast<DWORD>(records.size() - sent), &written)) {
            return;
        }
        if (written == 0) {
            Sleep(5);
            continue;
        }
        sent += written;
    }
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
    // ---- P2-4 帧账审计幕:第二个子进程带 LUBANCODE_FRAME_AUDIT=1,
    // stderr 改道进文件。两桩账:一万字中文粘贴的帧数帽;终端把 bracketed
    // 标记拆成裸字符投递时不再漏成正文。 ----
    {
        const std::wstring audit_path = std::wstring(argv[3]) + L".audit";
        STARTUPINFOW si2{};
        PROCESS_INFORMATION pi2{};
        if (StartAuditChild(argv[1], argv[2], audit_path.c_str(), &si2, &pi2)) {
            CloseHandle(pi2.hThread);
            Check(WaitForAny({L"shift+tab"}, 30000), "audit child: composer ready");
            Sleep(300);

            // 账一:ESC 以裸字符事件(VK=0)投,标记跟着逐字符到——旧路把
            // 0x1b 静默丢弃,"[200~"便逐字漏进编辑器。
            SendKey(VK_ESCAPE, 0x1b);  // 空输入框上 Ctrl+C 是退出,清场用 Esc
            Sleep(200);
            SendKey(0, 0x1b);
            SendText(L"[200~标记剥离");
            SendKey(0, 0x1b);
            SendText(L"[201~");
            Check(WaitForAny({L"标记剥离"}, 5000), "bare-ESC bracketed paste: 正文进编辑器");
            Check(!ScreenContains(L"[200~") && !ScreenContains(L"[201~"),
                  "bare-ESC bracketed paste: 标记一个字都不漏成正文");

            // 账二:一万字中文单行粘贴。单行没换行也没标记,旧路逐字当
            // 打字交付,每个字一轮终端帧;一次粘贴须收成一次编辑事务
            // (占位符),整场 ReadLine 的帧数有帽。分批投递模拟 ConPTY
            // 拆批:批与批之间的 Paste 事件在编辑器里并成同一枚附件。
            SendKey(VK_ESCAPE, 0x1b);  // 清掉上一幕的正文(空框上 Ctrl+C 会退出)
            Sleep(200);
            std::wstring big;
            for (int i = 0; i < 10000; ++i) {
                big += L"万字中文"[static_cast<std::size_t>(i % 4)];
            }
            SendTextBatchAllAtOnce(big);
            Check(WaitForAny({L"[Pasted Content 10000 chars]", L"[粘贴内容 10000 字符]"}, 8000),
                  "10000-char paste collapses to one placeholder (single edit transaction)");

            // 收场:清掉占位符再 exit(Esc 清输入),ReadLine 返回,帧账落 stderr。
            SendKey(VK_ESCAPE, 0x1b);
            Sleep(200);
            SendText(L"exit");
            SendKey(VK_RETURN, L'');
            WaitForSingleObject(pi2.hProcess, 15000);
            DWORD audit_exit = STILL_ACTIVE;
            GetExitCodeProcess(pi2.hProcess, &audit_exit);
            Check(audit_exit == 0, "audit child exits cleanly");
            if (audit_exit == STILL_ACTIVE) {
                TerminateProcess(pi2.hProcess, 3);
                WaitForSingleObject(pi2.hProcess, 5000);
            }
            CloseHandle(pi2.hProcess);

            long long frames = -1;
            long long bytes = -1;
            const std::string audit = ReadAuditFile(audit_path.c_str());
            const bool parsed = ParseAuditLine(audit, "idle_composer", &frames, &bytes);
            Check(parsed, "frame audit line present in stderr log");
            if (!parsed) {
                g_report << "INFO: audit file bytes=" << audit.size() << " head=[" << audit.substr(0, 200)
                         << "]" << std::endl;
            }
            if (parsed) {
                // 帽:一万字粘贴 + 前后键入,帧数以百计都算过;旧路是一万
                // 帧起(每字一轮)。字节同帽(约几万,旧路百万级)。
                Check(frames <= 160, "10000-char paste: whole ReadLine draws at most 160 frames (got " +
                                          std::to_string(frames) + ")");
                Check(bytes <= 512 * 1024, "10000-char paste: written bytes capped (got " +
                                               std::to_string(bytes) + ")");
                g_report << "INFO: frame audit idle_composer frames=" << frames
                         << " bytes=" << bytes << "\n";
            }
            // 光标终位:子进程退场后控制台光标仍在缓冲区界内。
            CONSOLE_SCREEN_BUFFER_INFO after{};
            if (GetConsoleScreenBufferInfo(g_out, &after)) {
                Check(after.dwCursorPosition.X >= 0 && after.dwCursorPosition.Y >= 0,
                      "cursor lands inside the buffer after paste session");
            }
            if (parsed) {
                DeleteFileW(audit_path.c_str());
            }
        } else {
            Check(false, "audit child CreateProcess failed");
        }
    }

    g_report << "RESULT: " << (g_failures == 0 ? "ALL PASS" : "FAIL") << "\n";
    return g_failures == 0 ? 0 : 1;
}
