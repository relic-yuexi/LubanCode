// 子代理导航贴底(0.29.x)专用的"刮屏驱动器":跟 tests/stream_footer_driver.cpp
// 同一套手艺(AllocConsole + WriteConsoleInputW + ReadConsoleOutputW),验的
// 是这轮的新东西——空闲 composer 的代理导航坞画在 composer 下横线与状态行
// 之后贴底、任意状态转换后每种导航行至多一份(残帧计数)、闲置汇总折叠、
// Ctrl+L 整屏重画。不进 ctest,集成验证时手动跑:
//   agent_panel_driver <lubancode.exe 路径> <子进程工作目录> <报告文件路径>
// 面板数据走 LUBANCODE_AGENT_PANEL_DEMO 假代理钩子(InteractiveSession 构造
// 时认这个环境变量),不用真起子代理、不碰网络。前 5 只演示代理设为完成态
// (DEMO_IDLE),驱动闲置折叠;其余运行中。

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

// ---- 残帧扫描(规格"测试"四):不能只查"最后一次 FindRow 找到了",
//      要数遍整屏,前面堆着的旧副本一票否决。 ----
int CountRowsWith(const std::string& needle, int max_rows = 400) {
    int count = 0;
    for (int row = 0; row < max_rows; ++row) {
        if (ReadRow(row).find(needle) != std::string::npos) {
            ++count;
        }
    }
    return count;
}

// main 导航行:状态灯 ● + "main"(身份列)。状态行/正文里的 "main" 不算。
int CountMainRows() {
    int count = 0;
    for (int row = 0; row < 400; ++row) {
        const std::string text = ReadRow(row);
        if (text.find("\xe2\x97\x8f main") != std::string::npos ||
            text.find("\xe2\x97\x89 main") != std::string::npos) {
            ++count;
        }
    }
    return count;
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

// 导航文本(操作提示/代理行)绝不许出现在 composer 上横线之上。
bool NoDockTextAboveComposer(int rule_row) {
    for (int r = 0; r < rule_row; ++r) {
        const std::string text = ReadRow(r);
        if (text.find("\xe2\x86\x91/\xe2\x86\x93") != std::string::npos) {  // ↑/↓
            return false;
        }
        if (text.find("general-purpose #") != std::string::npos) {
            return false;
        }
        if (text.find("\xe2\x97\x8f main") != std::string::npos) {
            return false;
        }
    }
    return true;
}

// 当前屏面快照的残帧总账:提示 1、main 1、每只代理 <=1、闲置汇总 <=1,
// 且都在 composer 上横线之下。返回 true 表示账面干净。
bool DockLedgerClean(int rule_row) {
    // 提示行文案随焦点收放(聚焦版没有"↑/↓"),按各版共性认:恰好一行。
    int hint_rows = 0;
    for (int row = 0; row < 400; ++row) {
        const std::string text = ReadRow(row);
        if (text.find("\xe2\x86\x91/\xe2\x86\x93") != std::string::npos ||       // ↑/↓
            text.find("Enter \xe6\x9f\xa5\xe7\x9c\x8b") != std::string::npos ||  // Enter 查看
            text.find("\xe5\x86\x8d\xe6\x8c\x89 Ctrl+K") != std::string::npos) { // 再按 Ctrl+K(两段确认)
            ++hint_rows;
        }
    }
    if (hint_rows != 1) {
        return false;
    }
    if (CountMainRows() != 1) {
        return false;
    }
    for (int i = 1; i <= 8; ++i) {
        const std::string title = "\xe6\xbc\x94\xe7\xa4\xba\xe4\xbb\xbb\xe5\x8a\xa1 " +
                                  std::to_string(i);  // 演示任务 N
        if (CountRowsWith(title) > 1) {
            return false;
        }
    }
    if (CountRowsWith("\xe9\x97\xb2\xe7\xbd\xae\xe4\xbb\xa3\xe7\x90\x86") > 1) {  // 闲置代理(汇总行)
        return false;
    }
    return NoDockTextAboveComposer(rule_row);
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

    // 假代理面板钩子:8 只演示任务,前 5 只完成态(驱动闲置折叠),后 3 只
    // 运行中。只喂给这一场子进程。
    _wputenv(L"LUBANCODE_AGENT_PANEL_DEMO=8");
    _wputenv(L"LUBANCODE_AGENT_PANEL_DEMO_IDLE=5");

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
    SMALL_RECT window{0, 0, 119, 29};  // 矮窗口:导航坞贴底长,不能把 composer 挤出屏
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

    // ---- 开场帧:导航坞在 composer 下横线与状态行之后 ----
    Check(WaitForText("\xe2\x86\x91/\xe2\x86\x93", 30000), "开场:导航操作提示行出现(30s 内)");
    Sleep(500);  // 等首帧整帧画稳
    int hint_row = FindLastRow("\xe2\x86\x91/\xe2\x86\x93");
    int input_row = FindComposerInputRow();
    Check(input_row > 0, "开场:composer 输入行按结构找到");
    const int rule_row = input_row - 1;
    const int status_row = input_row + 2;  // 上横线(输入行-1)/输入/下横线/状态
    Check(hint_row > status_row, "导航提示在状态行之下(composer 下横线之后)");
    const int main_row = FindLastRow("\xe2\x97\x8f main");
    Check(main_row >= 0 && main_row > status_row, "main 行在状态行之下,不插进正文");
    const int agent_row = FindLastRow("general-purpose #1");
    Check(agent_row >= 0 && agent_row > status_row, "代理行在状态行之下(导航坞贴底)");
    Check(NoDockTextAboveComposer(rule_row), "composer 上横线之上没有任何导航文本");
    Check(DockLedgerClean(rule_row), "开场残帧账:提示/main/代理/汇总各至多一份");

    // ---- 闲置折叠:前 5 只完成态,只列前三只与一行汇总 ----
    Check(FindLastRow("\xe6\xbc\x94\xe7\xa4\xba\xe4\xbb\xbb\xe5\x8a\xa1 3") >= 0, "闲置折叠:第 1~3 只完成态在列");
    Check(FindLastRow("\xe6\xbc\x94\xe7\xa4\xba\xe4\xbb\xbb\xe5\x8a\xa1 4") < 0,
          "闲置折叠:第 4 只完成态被折起(屏上不见)");
    int summary_row = -1;
    Check(WaitForText("\xe9\x97\xb2\xe7\xbd\xae\xe4\xbb\xa3\xe7\x90\x86", 3000, &summary_row),
          "闲置折叠:汇总行出现");
    Check(summary_row > status_row, "闲置折叠:汇总行在状态行之下");
    Check(CountRowsWith("\xe9\x97\xb2\xe7\xbd\xae\xe4\xbb\xa3\xe7\x90\x86") == 1, "闲置汇总至多一份");

    // ---- 汇总哨兵可导航:Down×4(1,2,3,哨兵)后 Enter 展开 ----
    for (int i = 0; i < 4; ++i) {
        SendKey(VK_DOWN, 0, 0);
        Sleep(120);
    }
    Check(WaitForText("\xe2\x9d\xaf", 3000), "导航:连按 4 下 Down,焦点标记出现");
    Check(FindLastRow("\xe5\x8f\xa6\xe6\x9c\x89") >= 0 && WaitForText("\xe5\x8f\xa6\xe6\x9c\x89", 1000),
          "导航:选中落在汇总哨兵上(另有 N 只)");
    SendKey(VK_RETURN, L'\r', 0);
    Check(WaitForText("\xe6\xbc\x94\xe7\xa4\xba\xe4\xbb\xbb\xe5\x8a\xa1 4", 3000),
          "Enter 展开闲置:第 4 只完成态回到列表");
    Check(CountRowsWith("\xe9\x97\xb2\xe7\xbd\xae\xe4\xbb\xa3\xe7\x90\x86") == 0,
          "Enter 展开后:汇总行收走,没有第二份");
    Check(DockLedgerClean(FindComposerInputRow() - 1), "展开后残帧账干净");
    for (int r = 0; r < 44; ++r) {
        const std::string row = ReadRow(r);
        if (!row.empty()) {
            Log("EXPAND " + std::to_string(r) + ": " + row);
        }
    }
    SendKey(VK_ESCAPE, 0, 0);
    Check(WaitForTextGone("\xe6\xbc\x94\xe7\xa4\xba\xe4\xbb\xbb\xe5\x8a\xa1 4", 3000), "Esc 收起闲置汇总");
    Check(CountRowsWith("\xe9\x97\xb2\xe7\xbd\xae\xe4\xbb\xa3\xe7\x90\x86") == 1, "收起后汇总恰好一份");
    SendKey(VK_ESCAPE, 0, 0);
    Check(WaitForTextGone("\xe2\x9d\xaf", 3000), "再 Esc:退出代理焦点");

    // ---- 连按 20 次上下:不多出第二份提示或 main ----
    for (int i = 0; i < 20; ++i) {
        SendKey(i % 2 == 0 ? VK_DOWN : VK_UP, 0, 0);
        Sleep(60);
    }
    Sleep(400);
    Check(DockLedgerClean(FindComposerInputRow() - 1), "连按 20 次上下后残帧账干净(提示/main 各一份)");

    // ---- Enter/Esc 往返 20 次:查看态详情不留残骸 ----
    SendKey(VK_DOWN, 0, 0);
    WaitForText("\xe2\x9d\xaf", 3000);
    for (int i = 0; i < 20; ++i) {
        SendKey(VK_RETURN, L'\r', 0);
        Sleep(120);
        SendKey(VK_ESCAPE, 0, 0);
        Sleep(120);
    }
    Check(FindLastRow("\xe4\xbb\xbb\xe5\x8a\xa1\xe6\xa0\x87\xe9\xa2\x98") < 0,
          "Enter/Esc 往返 20 次:详情('任务标题')不留残骸");
    Check(DockLedgerClean(FindComposerInputRow() - 1), "往返 20 次后残帧账干净");
    SendKey(VK_ESCAPE, 0, 0);
    WaitForTextGone("\xe2\x9d\xaf", 3000);

    // ---- Ctrl+L:草稿/选择保住,重复行归零 ----
    SendKey(VK_DOWN, 0, 0);
    WaitForText("\xe2\x9d\xaf", 3000);
    SendText("\xe8\x8d\x89\xe7\xa8\xbf");  // 草稿
    Sleep(300);
    // 打了字,面板焦点让位(既有规矩);Ctrl+L 只重画,不吞输入。
    SendKey('L', 0, LEFT_CTRL_PRESSED);
    Sleep(600);
    int input_after = FindComposerInputRow();
    Check(input_after > 0 && ReadRow(input_after).find("\xe8\x8d\x89\xe7\xa8\xbf") != std::string::npos,
          "Ctrl+L:composer 草稿还在(不吞输入)");
    Check(DockLedgerClean(FindComposerInputRow() - 1), "Ctrl+L:重复行归零,残帧账干净");
    // 清草稿(Ctrl+C 有字先清字,空闲路已有实现,顺手回归)。
    SendKey('C', 0, LEFT_CTRL_PRESSED);
    Sleep(400);
    Check(FindComposerInputRow() > 0 && ReadRow(FindComposerInputRow()).find("\xe8\x8d\x89\xe7\xa8\xbf") ==
              std::string::npos,
          "Ctrl+C:空闲草稿清空,框还在");
    DWORD exit_code = STILL_ACTIVE;
    GetExitCodeProcess(pi.hProcess, &exit_code);
    Check(exit_code == STILL_ACTIVE, "Ctrl+C 清草稿:进程仍活");

    // ---- resize:拉窄再拉宽,旧宽度字符清空、账面仍干净 ----
    SetConsoleScreenBufferSize(g_conout, COORD{80, 400});
    SMALL_RECT narrow{0, 0, 79, 29};
    SetConsoleWindowInfo(g_conout, TRUE, &narrow);
    Sleep(800);
    for (int r = 0; r < 44; ++r) {
        const std::string row = ReadRow(r);
        if (!row.empty()) {
            Log("NARROW " + std::to_string(r) + ": " + row);
        }
    }
    Check(DockLedgerClean(FindComposerInputRow() - 1), "resize 拉窄后残帧账干净");
    SetConsoleScreenBufferSize(g_conout, COORD{120, 400});
    SMALL_RECT wide{0, 0, 119, 29};
    SetConsoleWindowInfo(g_conout, TRUE, &wide);
    Sleep(800);
    Check(DockLedgerClean(FindComposerInputRow() - 1), "resize 拉宽后残帧账干净");

    // ---- 两段确认:Ctrl+X 亮确认提示,Esc 撤销,不误杀 ----
    SendKey('X', 0, LEFT_CTRL_PRESSED);
    Check(WaitForText("\xe5\x86\x8d\xe6\x8c\x89", 3000), "Ctrl+X:出现'再按 Ctrl+K 确认'提示");
    SendKey(VK_ESCAPE, 0, 0);
    Check(WaitForTextGone("\xe5\x86\x8d\xe6\x8c\x89", 3000), "Esc:两段确认撤销,提示收走");
    Check(FindLastRow("general-purpose #1") >= 0, "误按第一段不动任务:坞原样");

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
