// markdown(0.18.x)集成验证用的刮屏驱动器:照 screen_driver.cpp 的手艺
// (AllocConsole + WriteConsoleInputW 假装敲键盘 + ReadConsoleOutputW 逐格
// 刮屏),专验"回合收束后 markdown 重画"这一件事,不进 ctest:
//   M1 问一段强制 markdown 的自我介绍,收束后帧上要有:亮色(bold)标题、
//      ┌─┬─┐/└─┴─┘ 对齐的表格边线、"  │" 前缀的代码块、• 圆点列表;
//      原样正文的 ``` 围栏该被擦掉重画,不许残留。
//   M2 再问一句纯文本,确认不触发重画——圆点/边线的数量一个不多。
// 结论逐行写报告文件(PASS/FAIL/INFO),退出码 0 = 全过。
//
// 用法: markdown_screen_driver <lubancode.exe 路径> <子进程工作目录> <报告文件路径>
// 环境变量(LUBANCODE_BASE_URL/API_KEY/MODEL、USERPROFILE)由调用方设好。

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

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
    if (ok) {
        Log("PASS: " + what);
    } else {
        Log("FAIL: " + what);
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

std::string ReadRow(int row, std::vector<WORD>* attrs = nullptr) {
    const int width = BufferWidth();
    std::vector<CHAR_INFO> cells(static_cast<std::size_t>(width));
    SMALL_RECT region{0, static_cast<SHORT>(row), static_cast<SHORT>(width - 1), static_cast<SHORT>(row)};
    if (!ReadConsoleOutputW(g_conout, cells.data(), COORD{static_cast<SHORT>(width), 1}, COORD{0, 0}, &region)) {
        return {};
    }
    std::wstring text;
    if (attrs != nullptr) {
        attrs->clear();
    }
    for (const CHAR_INFO& cell : cells) {
        if (cell.Attributes & COMMON_LVB_TRAILING_BYTE) {
            continue;
        }
        text.push_back(cell.Char.UnicodeChar);
        if (attrs != nullptr) {
            attrs->push_back(cell.Attributes);
        }
    }
    while (!text.empty() && (text.back() == L' ' || text.back() == L'\0')) {
        text.pop_back();
        if (attrs != nullptr && !attrs->empty()) {
            attrs->pop_back();
        }
    }
    return WideToUtf8(text);
}

int FindLastRow(const std::string& needle, int max_rows = 1000) {
    for (int row = max_rows - 1; row >= 0; --row) {
        if (ReadRow(row).find(needle) != std::string::npos) {
            return row;
        }
    }
    return -1;
}

// 全缓冲里含 needle 的行数(帧快照比对用)。
int CountRows(const std::string& needle, int max_rows = 1000) {
    int count = 0;
    for (int row = 0; row < max_rows; ++row) {
        if (ReadRow(row).find(needle) != std::string::npos) {
            ++count;
        }
    }
    return count;
}

bool WaitForText(const std::string& needle, int timeout_ms) {
    const DWORD deadline = GetTickCount() + static_cast<DWORD>(timeout_ms);
    while (GetTickCount() < deadline) {
        if (FindLastRow(needle) >= 0) {
            return true;
        }
        Sleep(200);
    }
    return false;
}

bool WaitForCount(const std::string& needle, int want, int timeout_ms) {
    const DWORD deadline = GetTickCount() + static_cast<DWORD>(timeout_ms);
    while (GetTickCount() < deadline) {
        if (CountRows(needle) >= want) {
            return true;
        }
        Sleep(300);
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
    SetConsoleScreenBufferSize(g_conout, COORD{120, 1000});
    SMALL_RECT window{0, 0, 119, 40};
    SetConsoleWindowInfo(g_conout, TRUE, &window);
    FlushConsoleInputBuffer(g_conin);
    Log("INFO: console buffer " + std::to_string(BufferWidth()) + " cols");

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

    Check(WaitForText("shift+tab", 30000), "M0 开场:输入框就位(30s 内)");
    Sleep(500);

    // ---- M1 markdown 回合:标题/表格/代码块/列表,收束后重画 ----
    // 提示词里刻意不写井号/围栏/竖线这些字面标记——提交行会留在屏上,
    // 别跟"原样正文已被擦掉"的断言撞车。
    SendText("请用 markdown 介绍你自己,必须包含:一个二级标题;一个三行表格(表头加两行数据,要有中文);"
             "一段 cpp 代码块;一个无序列表(短横线开头,三项)。不要调用任何工具,直接回答。");
    SendKey(VK_RETURN, L'\r', 0);
    Check(WaitForText("[tokens]", 300000), "M1 一轮问答:统计行出现(300s 内)");
    Sleep(1500);

    const char* kTopLeft = "\xe2\x94\x8c";      // ┌
    const char* kBottomLeft = "\xe2\x94\x94";   // └
    const char* kVBar = "\xe2\x94\x82";         // │
    const char* kBullet = "\xe2\x80\xa2";       // •
    {
        // 表格边线:┌ 行、└ 行各至少一根,且首尾边线同宽(逐格刮出来的
        // 文本行尾空白已剪,字符数一致 = 列对齐没歪)。
        const int top = FindLastRow(kTopLeft);
        const int bottom = FindLastRow(kBottomLeft);
        Check(top >= 0, "M1 表格:┌ 顶边线出现");
        Check(bottom > top, "M1 表格:└ 底边线在顶边线之下");
        if (top >= 0 && bottom > top) {
            const std::string top_line = ReadRow(top);
            const std::string bottom_line = ReadRow(bottom);
            Log("INFO: 表格顶边线 = " + top_line);
            Check(top_line.size() == bottom_line.size(), "M1 表格:顶/底边线等宽(列对齐)");
            int data_rows = 0;
            for (int r = top + 1; r < bottom; ++r) {
                if (ReadRow(r).find(kVBar) != std::string::npos) {
                    ++data_rows;
                    Log("INFO: 表格行 = " + ReadRow(r));
                }
            }
            Check(data_rows >= 3, "M1 表格:边线之间至少三行(表头+分隔+两行数据)");
        }

        // 代码块:两格缩进 + │ 前缀的行。
        int code_rows = 0;
        for (int r = 0; r < 1000; ++r) {
            const std::string text = ReadRow(r);
            if (text.compare(0, 5, std::string("  ") + kVBar) == 0) {
                ++code_rows;
                if (code_rows == 1) {
                    Log("INFO: 代码块行 = " + text);
                }
            }
        }
        Check(code_rows >= 1, "M1 代码块:两格缩进 + │ 前缀的行出现");

        // 列表圆点。
        Check(CountRows(kBullet) >= 3, "M1 列表:• 圆点至少三行");

        // 标题:提交行之下(启动横幅那行也是亮色,不许蹭)某一行带亮色
        // (bold -> FOREGROUND_INTENSITY)的非空文字。
        const int prompt_row = FindLastRow("> \xe8\xaf\xb7\xe7\x94\xa8 markdown");  // "> 请用 markdown"
        Check(prompt_row >= 0, "M1 提交行还在屏上(重画没伤到它)");
        bool bold_heading = false;
        for (int r = prompt_row + 1; r < 1000 && !bold_heading; ++r) {
            std::vector<WORD> attrs;
            const std::string text = ReadRow(r, &attrs);
            if (text.empty() || text.find("> ") != std::string::npos || text.find("[tokens]") != std::string::npos ||
                text.find("lubancode 0.18") != std::string::npos) {
                continue;
            }
            if (text.find(kVBar) != std::string::npos || text.find(kBullet) != std::string::npos) {
                continue;  // 表格/代码/列表行,不算标题
            }
            int bright = 0;
            for (const WORD a : attrs) {
                if ((a & FOREGROUND_INTENSITY) != 0) {
                    ++bright;
                }
            }
            if (bright >= 2) {
                bold_heading = true;
                Log("INFO: 标题行 = " + text);
            }
        }
        Check(bold_heading, "M1 标题:提交行之下出现 bold(亮色)标题行");

        // 原样正文的 ``` 围栏被擦掉重画,屏上不许残留。
        Check(CountRows("```") == 0, "M1 重画:原样 ``` 围栏已被擦掉");

        // 有失败就把屏尾几十行倒出来,好对着报告查案。
        if (g_failures > 0) {
            const int tail_from = FindLastRow("[tokens]");
            for (int r = (tail_from > 60 ? tail_from - 60 : 0); r <= tail_from && r >= 0; ++r) {
                Log("DUMP[" + std::to_string(r) + "]: " + ReadRow(r));
            }
        }
    }

    // ---- M2 纯文本回合:不触发重画,圆点/边线数量一个不多 ----
    const int bullets_before = CountRows(kBullet);
    const int borders_before = CountRows(kTopLeft);
    SendText("再用一句纯文本回答:你最喜欢哪种编程语言?不要用任何 markdown 标记,不要列表,不要表格。");
    SendKey(VK_RETURN, L'\r', 0);
    Check(WaitForCount("[tokens]", 2, 300000), "M2 第二轮问答:第二条统计行出现(300s 内)");
    Sleep(1500);
    {
        const int bullets_after = CountRows(kBullet);
        const int borders_after = CountRows(kTopLeft);
        Log("INFO: 圆点行 " + std::to_string(bullets_before) + " -> " + std::to_string(bullets_after) +
            ",顶边线 " + std::to_string(borders_before) + " -> " + std::to_string(borders_after));
        Check(bullets_after == bullets_before, "M2 纯文本:圆点行数不变(没重画)");
        Check(borders_after == borders_before, "M2 纯文本:表格边线数不变(没重画)");
        const int tokens_row = FindLastRow("[tokens]");
        // 第二轮的回答正文在第二条分界线之后、统计行之前,应当是原样纯文本。
        Log("INFO: 第二轮统计行 = " + ReadRow(tokens_row));
    }

    // ---- 收尾 ----
    SendText("exit");
    SendKey(VK_RETURN, L'\r', 0);
    if (WaitForSingleObject(pi.hProcess, 15000) != WAIT_OBJECT_0) {
        Log("INFO: exit 超时,强杀子进程");
        TerminateProcess(pi.hProcess, 9);
    }
    CloseHandle(pi.hProcess);

    Log(g_failures == 0 ? "RESULT: ALL PASS" : "RESULT: " + std::to_string(g_failures) + " FAIL");
    return g_failures == 0 ? 0 : 1;
}
