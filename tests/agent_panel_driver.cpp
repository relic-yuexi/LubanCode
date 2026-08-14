// 子代理面板移到输入框上方(0.28.x)专用的"刮屏驱动器":跟
// tests/stream_footer_driver.cpp 同一套手艺(AllocConsole + WriteConsoleInputW
// + ReadConsoleOutputW),验的是这轮新加的东西——空闲 composer 的代理面板
// 画在输入框上横线之上、输入框与状态行钉在视口下部、焦点/查看态/两段确认
// 的屏面行为。不进 ctest,集成验证时手动跑:
//   agent_panel_driver <lubancode.exe 路径> <子进程工作目录> <报告文件路径>
// 面板数据走 LUBANCODE_AGENT_PANEL_DEMO 假代理钩子(InteractiveSession 构造
// 时认这个环境变量),不用真起子代理、不碰网络。

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <cstdlib>
#include <fstream>
#include <string>
#include <vector>

namespace {

HANDLE g_conin = INVALID_HANDLE_VALUE;
HANDLE g_conout = INVALID_HANDLE_VALUE;
std::ofstream g_report;
int g_failures = 0;

void Log(const std::string& line) {
    g_report << line << "\n";
    g_report.flush();
}

void Check(bool ok, const std::string& what) {
    Log(std::string(ok ? "PASS: " : "FAIL: ") + what);
    if (!ok) {
        ++g_failures;
    }
}

std::string WideToUtf8(const std::wstring& w) {
    if (w.empty()) {
        return {};
    }
    const int len = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    std::string out(static_cast<std::size_t>(len), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), out.data(), len, nullptr, nullptr);
    return out;
}

std::wstring Utf8ToWide(const std::string& s) {
    if (s.empty()) {
        return {};
    }
    const int len = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring out(static_cast<std::size_t>(len), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), out.data(), len);
    return out;
}

int BufferWidth() {
    CONSOLE_SCREEN_BUFFER_INFO info{};
    GetConsoleScreenBufferInfo(g_conout, &info);
    return info.dwSize.X;
}

std::string ReadRow(int row) {
    const int width = BufferWidth();
    if (row < 0) {
        return {};
    }
    std::vector<CHAR_INFO> cells(static_cast<std::size_t>(width));
    SMALL_RECT region{0, static_cast<SHORT>(row), static_cast<SHORT>(width - 1), static_cast<SHORT>(row)};
    if (!ReadConsoleOutputW(g_conout, cells.data(), COORD{static_cast<SHORT>(width), 1}, COORD{0, 0}, &region)) {
        return {};
    }
    std::wstring text;
    for (const CHAR_INFO& cell : cells) {
        if (cell.Attributes & COMMON_LVB_TRAILING_BYTE) {
            continue;
        }
        text.push_back(cell.Char.UnicodeChar);
    }
    while (!text.empty() && (text.back() == L' ' || text.back() == L'\0')) {
        text.pop_back();
    }
    return WideToUtf8(text);
}

int FindLastRow(const std::string& needle, int max_rows = 400) {
    for (int row = max_rows - 1; row >= 0; --row) {
        if (ReadRow(row).find(needle) != std::string::npos) {
            return row;
        }
    }
    return -1;
}

bool WaitForText(const std::string& needle, int timeout_ms, int* found_row = nullptr) {
    const DWORD deadline = GetTickCount() + static_cast<DWORD>(timeout_ms);
    while (GetTickCount() < deadline) {
        const int row = FindLastRow(needle);
        if (row >= 0) {
            if (found_row != nullptr) {
                *found_row = row;
            }
            return true;
        }
        Sleep(100);
    }
    return false;
}

bool WaitForTextGone(const std::string& needle, int timeout_ms) {
    const DWORD deadline = GetTickCount() + static_cast<DWORD>(timeout_ms);
    while (GetTickCount() < deadline) {
        if (FindLastRow(needle) < 0) {
            return true;
        }
        Sleep(100);
    }
    return false;
}

void SendKey(WORD vk, wchar_t ch, DWORD control_state) {
    INPUT_RECORD records[2]{};
    for (int i = 0; i < 2; ++i) {
        records[i].EventType = KEY_EVENT;
        records[i].Event.KeyEvent.bKeyDown = i == 0 ? TRUE : FALSE;
        records[i].Event.KeyEvent.wRepeatCount = 1;
        records[i].Event.KeyEvent.wVirtualKeyCode = vk;
        records[i].Event.KeyEvent.uChar.UnicodeChar = ch;
        records[i].Event.KeyEvent.dwControlKeyState = control_state;
    }
    DWORD written = 0;
    WriteConsoleInputW(g_conin, records, 2, &written);
}

void SendText(const std::string& utf8) {
    for (wchar_t wc : Utf8ToWide(utf8)) {
        SendKey(0, wc, 0);
        Sleep(15);
    }
}

// 一行里是不是一根框横线(至少 40 个 '─' 或 '-' 连排)。
bool IsRuleRow(int row) {
    const std::string text = ReadRow(row);
    int run = 0;
    for (std::size_t i = 0; i < text.size();) {
        const bool box_char = text.compare(i, 3, "\xe2\x94\x80") == 0;
        if (box_char || text[i] == '-') {
            ++run;
            if (run >= 40) {
                return true;
            }
            i += box_char ? 3 : 1;
        } else {
            run = 0;
            ++i;
        }
    }
    return false;
}

// 按结构认 composer 框(不靠文案):上横线(r) / '>' 起的输入行(r+1) /
// 下横线(r+2) / 非横线状态行(r+3)。从底往上扫,最近的一个就是当前框。
int FindComposerInputRow(int max_rows = 400) {
    for (int r = max_rows - 5; r >= 0; --r) {
        const std::string input_text = ReadRow(r + 1);
        if (IsRuleRow(r) && !input_text.empty() && input_text[0] == '>' && IsRuleRow(r + 2) && !IsRuleRow(r + 3)) {
            return r + 1;
        }
    }
    return -1;
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    if (argc < 4) {
        return 2;
    }
    const std::wstring exe_path = argv[1];
    const std::wstring workdir = argv[2];
    g_report.open(argv[3], std::ios::binary | std::ios::trunc);
    if (!g_report.is_open()) {
        return 2;
    }

    // 假代理面板钩子:6 只演示任务,只喂给这一场子进程。
    _wputenv(L"LUBANCODE_AGENT_PANEL_DEMO=6");

    FreeConsole();
    if (!AllocConsole()) {
        Log("FAIL: AllocConsole");
        return 1;
    }
    SECURITY_ATTRIBUTES inheritable{};
    inheritable.nLength = sizeof(inheritable);
    inheritable.bInheritHandle = TRUE;
    g_conin = CreateFileW(L"CONIN$", GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, &inheritable,
                          OPEN_EXISTING, 0, nullptr);
    g_conout = CreateFileW(L"CONOUT$", GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, &inheritable,
                           OPEN_EXISTING, 0, nullptr);
    if (g_conin == INVALID_HANDLE_VALUE || g_conout == INVALID_HANDLE_VALUE) {
        Log("FAIL: open CONIN$/CONOUT$");
        return 1;
    }
    SMALL_RECT small{0, 0, 1, 1};
    SetConsoleWindowInfo(g_conout, TRUE, &small);
    SetConsoleScreenBufferSize(g_conout, COORD{120, 400});
    SMALL_RECT window{0, 0, 119, 29};  // 矮窗口:面板要向上长,不能把输入框顶出去
    SetConsoleWindowInfo(g_conout, TRUE, &window);
    FlushConsoleInputBuffer(g_conin);

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = g_conin;
    si.hStdOutput = g_conout;
    si.hStdError = g_conout;
    PROCESS_INFORMATION pi{};
    std::wstring cmdline = L"\"" + exe_path + L"\"";
    if (!CreateProcessW(exe_path.c_str(), cmdline.data(), nullptr, nullptr, TRUE, 0, nullptr, workdir.c_str(), &si,
                        &pi)) {
        Log("FAIL: CreateProcess " + std::to_string(GetLastError()));
        return 1;
    }
    CloseHandle(pi.hThread);

    // ---- 开场帧:面板在输入框上方 ----
    int hint_row = -1;
    Check(WaitForText("\xe2\x86\x91/\xe2\x86\x93", 30000, &hint_row), "开场:面板操作提示行出现(30s 内)");
    Sleep(500);  // 等首帧整帧画稳
    int input_row = FindComposerInputRow();
    Check(input_row > 0, "开场:composer 输入行按结构找到");
    const int rule_row = input_row - 1;
    const int agent_row = FindLastRow("general-purpose #6");
    Check(agent_row >= 0 && agent_row < rule_row, "代理行在上横线之上(不在输入框下面)");
    const int main_row = FindLastRow("main");
    Check(main_row >= 0 && main_row < rule_row && main_row > hint_row, "main 行在面板列表里");
    Check(FindLastRow("演示任务 3") >= 0, "6 只演示代理全量在列(没有无声截断)");
    Check(!IsRuleRow(input_row + 2) ? true : IsRuleRow(input_row + 1), "输入框下横线与状态行仍钉在框底");

    // ---- 焦点:空 composer 按下键进代理焦点,选中标记出现 ----
    SendKey(VK_DOWN, 0, 0);
    int marker_row = -1;
    Check(WaitForText("\xe2\x9d\xaf", 3000, &marker_row), "按下键:焦点标记 ❯ 出现");
    Check(marker_row < rule_row, "焦点标记落在代理行,不在 composer 里");

    // ---- 查看态:Enter 展开,上横线右端挂短标题 ----
    SendKey(VK_RETURN, L'\r', 0);
    Check(WaitForText("Enter \xe6\x94\xb6\xe8\xb5\xb7", 3000), "Enter:查看态展开(详情提示行出现)");
    Check(WaitForText("\xe6\xbc\x94\xe7\xa4\xba\xe4\xbb\xbb\xe5\x8a\xa1 1", 3000),
          "查看态:上横线右端挂代理短标题");
    // Esc 先退查看态(标签摘掉——看 composer 上横线那一行,不再挂短标题),
    // 再 Esc 退焦点。
    SendKey(VK_ESCAPE, 0, 0);
    Check(WaitForTextGone("Enter \xe6\x94\xb6\xe8\xb5\xb7", 3000), "Esc:退查看态,详情行收起");
    {
        // 退查看态后面板收了详情行,框顶挪位:重新按结构找框。
        int rule_after = -1;
        for (int r = 398; r >= 0; --r) {
            const std::string input_text = ReadRow(r + 1);
            if (IsRuleRow(r) && !input_text.empty() && input_text[0] == '>' && IsRuleRow(r + 2) &&
                !IsRuleRow(r + 3)) {
                rule_after = r;
                break;
            }
        }
        Check(rule_after >= 0 && ReadRow(rule_after).find("\xe6\xbc\x94\xe7\xa4\xba\xe4\xbb\xbb\xe5\x8a\xa1 1") ==
                  std::string::npos,
              "Esc:退查看态,上横线右端短标题摘掉");
    }
    SendKey(VK_ESCAPE, 0, 0);
    Check(WaitForTextGone("\xe2\x9d\xaf", 3000), "再 Esc:退出代理焦点");

    // ---- 两段确认:Ctrl+X 亮确认提示,Esc 撤销,不误杀 ----
    SendKey('X', 0, LEFT_CTRL_PRESSED);
    Check(WaitForText("\xe5\x86\x8d\xe6\x8c\x89", 3000), "Ctrl+X:出现'再按 Ctrl+K 确认'提示");
    SendKey(VK_ESCAPE, 0, 0);
    Check(WaitForTextGone("\xe5\x86\x8d\xe6\x8c\x89", 3000), "Esc:两段确认撤销,提示收走");
    Check(FindLastRow("general-purpose #6") >= 0, "误按第一段不动任务:面板原样");

    // ---- 退出子进程 ----
    SendText("/exit");
    SendKey(VK_RETURN, L'\r', 0);
    WaitForTextGone("Ctrl+X Ctrl+K", 5000);
    WaitForSingleObject(pi.hProcess, 15000);
    CloseHandle(pi.hProcess);

    Log(g_failures == 0 ? "ALL PASS" : ("FAILURES: " + std::to_string(g_failures)));
    FreeConsole();
    return g_failures == 0 ? 0 : 1;
}
