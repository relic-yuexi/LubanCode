// 0.17.0 集成验证用的"刮屏驱动器":不是单测,不进 ctest。自己开一个全新
// 控制台(AllocConsole),把 lubancode.exe 当子进程挂进同一个控制台,用
// WriteConsoleInputW 假装敲键盘、用 ReadConsoleOutputW 逐格刮屏幕缓冲区,
// 验证输入框(上横线/提示行/下横线/状态行)、切档零新增行、多行长高、
// 提交收尾、焦点键位这些"只有真控制台才看得见"的行为。结论逐行写进
// 报告文件(PASS/FAIL/SKIP/INFO),退出码 0 = 全过。
//
// 用法: screen_driver <lubancode.exe 路径> <子进程工作目录> <报告文件路径>
// 环境变量(LUBANCODE_BASE_URL/API_KEY/MODEL、USERPROFILE)由调用方设好,
// 子进程原样继承。

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <filesystem>
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

COORD CursorPosition() {
    CONSOLE_SCREEN_BUFFER_INFO info{};
    GetConsoleScreenBufferInfo(g_conout, &info);
    return info.dwCursorPosition;
}

// 刮一行:CHAR_INFO 逐格读,宽字符的 TRAILING 半格跳过,行尾空白剪掉。
// attrs 非空时顺带带出每一格的属性(按可见字符的顺序,不含跳过的半格)。
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

// 从底往上找最后一个含 needle 的行号,找不到给 -1。
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

// 等"某一行的内容变成含 needle"(盯着固定行号,不全屏扫)。
bool WaitForRowText(int row, const std::string& needle, int timeout_ms) {
    const DWORD deadline = GetTickCount() + static_cast<DWORD>(timeout_ms);
    while (GetTickCount() < deadline) {
        if (ReadRow(row).find(needle) != std::string::npos) {
            return true;
        }
        Sleep(50);
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
        Sleep(15);  // 逐键小睡,别把编辑器的重画节奏挤爆
    }
}

// UTF-8 字节偏移换算成字符序号(attrs 是按可见字符排的)。
std::size_t CharIndexOfBytePos(const std::string& utf8, std::size_t byte_pos) {
    std::size_t chars = 0;
    for (std::size_t i = 0; i < byte_pos && i < utf8.size();) {
        const unsigned char c = static_cast<unsigned char>(utf8[i]);
        i += c < 0x80 ? 1 : (c & 0xE0) == 0xC0 ? 2 : (c & 0xF0) == 0xE0 ? 3 : 4;
        ++chars;
    }
    return chars;
}

// 验一行 diff 的背景属性:needle 覆盖的格子背景要 red/green 如愿(256 色
// 深红底/深绿底被 conhost 映到 BACKGROUND_RED/BACKGROUND_GREEN),行末
// 最后一格(内容之外)不许带底色——背景只铺到内容实际结尾,不填充终端宽。
void CheckDiffRowBg(int row, const std::string& needle, bool want_red, bool want_green, const std::string& what) {
    std::vector<WORD> attrs;
    const std::string text = ReadRow(row, &attrs);
    const std::size_t pos = text.find(needle);
    if (pos == std::string::npos) {
        Check(false, what + "(行里没找到 \"" + needle + "\")");
        return;
    }
    const std::size_t first = CharIndexOfBytePos(text, pos);
    const std::size_t last = CharIndexOfBytePos(text, pos + needle.size() - 1);
    bool ok = last < attrs.size();
    for (std::size_t i = first; ok && i <= last; ++i) {
        const bool red = (attrs[i] & BACKGROUND_RED) != 0;
        const bool green = (attrs[i] & BACKGROUND_GREEN) != 0;
        ok = red == want_red && green == want_green;
    }
    Check(ok, what);

    // 行尾格子(ReadRow 已剪掉的行尾空白之外)原始属性:直接刮最后一格。
    const int width = BufferWidth();
    CHAR_INFO cell{};
    SMALL_RECT region{static_cast<SHORT>(width - 1), static_cast<SHORT>(row), static_cast<SHORT>(width - 1),
                       static_cast<SHORT>(row)};
    if (ReadConsoleOutputW(g_conout, &cell, COORD{1, 1}, COORD{0, 0}, &region)) {
        Check((cell.Attributes & (BACKGROUND_RED | BACKGROUND_GREEN | BACKGROUND_BLUE)) == 0,
              what + ":底色没铺到终端宽(行尾格无背景)");
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

// 结构化找流式 footer 的输入行,不靠占位文案(0.25.x 起文案改版不再连坐)。
// Composer 合流(P1)后框随内容长高:上横线、'>' 起输入区、下横线、状态行。
// 自定义 padding 仍可插空行,故定位不写死相邻关系:认"输入区上下各有一根
// 横线、下横线之下不是横线(状态行)"。从底往上扫,命中最近的一个。
int FindFooterInputRow(int max_rows = 400) {
    for (int i = max_rows - 2; i >= 0; --i) {
        const std::string input_text = ReadRow(i);
        if (input_text.empty() || input_text[0] != '>') {
            continue;
        }
        bool rule_above = false;
        for (int r = i - 1; r >= i - 4 && r >= 0; --r) {
            rule_above = rule_above || IsRuleRow(r);
        }
        if (!rule_above) {
            continue;
        }
        for (int b = i + 1; b <= i + 6 && b + 1 < max_rows; ++b) {
            if (IsRuleRow(b) && !IsRuleRow(b + 1)) {
                return i;
            }
        }
    }
    return -1;
}

// 一行横线的可视列宽(─ 记 1 列、- 记 1 列;ReadRow 已剪掉行尾空白)。
// 0.21.x 验"横线满终端宽":满宽 = BufferWidth() - 1。
int RuleGlyphWidth(int row) {
    const std::string text = ReadRow(row);
    int cols = 0;
    for (std::size_t i = 0; i < text.size();) {
        if (text.compare(i, 3, "\xe2\x94\x80") == 0) {
            ++cols;
            i += 3;
        } else if (text[i] == '-') {
            ++cols;
            ++i;
        } else {
            ++i;  // 混入的别的字符不计(正常满宽横线整行只有 ─)
        }
    }
    return cols;
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    if (argc < 4) {
        return 2;
    }
    const std::wstring exe_path = argv[1];
    const std::wstring workdir = argv[2];
    const bool composer_only = argc >= 5 && std::wstring(argv[4]) == L"--composer-only";
    g_report.open(argv[3], std::ios::binary | std::ios::trunc);
    if (!g_report.is_open()) {
        return 2;
    }

    // 全新控制台:120 列宽(框横线该取 min(119, 100) = 100 列),缓冲 400 行。
    FreeConsole();
    if (!AllocConsole()) {
        Log("FAIL: AllocConsole");
        return 1;
    }
    // 句柄开成可继承的:CreateProcess 得用 STARTF_USESTDHANDLES 把这两个
    // 控制台句柄显式塞给子进程当 stdin/stdout/stderr——驱动器自己是被
    // bash 用管道句柄起的,不显式指定的话 CreateProcess 会把父进程的管道
    // std 句柄复制给子进程(Vista 起的 std 句柄复制怪癖),子进程一探测
    // stdin 是管道,整条逐键/输入框路径就走不进去(实测踩过)。
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

    // ---- F1 开场帧:上横线 / 提示行 / 下横线 / 状态行 四件套就位 ----
    int status_row = -1;
    Check(WaitForText("shift+tab", 30000, &status_row), "F1 开场:状态行出现(30s 内)");
    Sleep(300);
    status_row = FindLastRow("shift+tab");
    const int prompt_row = status_row - 2;  // 单行正文区:提示行紧贴上下横线
    {
        const std::string status = ReadRow(status_row);
        Check(status.find("\xe2\x8f\xb5\xe2\x8f\xb5") != std::string::npos, "F1 状态行有 ⏵⏵ 前缀");
        Check(status.find("确认模式") != std::string::npos, "F1 状态行显示确认档");
        if (!composer_only) {
            Check(status.find("MiniMax-M3") != std::string::npos, "F1 状态行显示模型名");
        }
        Check(status.find("context ") != std::string::npos, "F1 状态行显示 context 占比");
        Check(IsRuleRow(prompt_row - 1), "F1 上横线紧贴提示行");
        Check(IsRuleRow(prompt_row + 1), "F1 下横线紧贴提示行");
        // 0.21.x:框线满终端宽(BufferWidth - 1),不再卡 100 列上限。
        const int full = BufferWidth() - 1;
        Check(RuleGlyphWidth(prompt_row - 1) == full,
              "F1 上横线满终端宽(" + std::to_string(RuleGlyphWidth(prompt_row - 1)) + "==" +
                  std::to_string(full) + ")");
        Check(RuleGlyphWidth(prompt_row + 1) == full, "F1 下横线满终端宽");
        // 空 composer 的提示行只剩 "> ",行尾空白被 ReadRow 剪掉,按裸 ">" 认。
        const std::string prompt_text = ReadRow(prompt_row);
        Check(!prompt_text.empty() && prompt_text.back() == '>', "F1 提示行有 '> '");
        Log("INFO: F1 屏面 prompt_row=" + std::to_string(prompt_row) + " status_row=" + std::to_string(status_row));
    }

    // ---- F2 打字帧:框内容更新,状态行原地不动 ----
    SendText("abc");
    Check(WaitForRowText(prompt_row, "> abc", 5000), "F2 打字:提示行变成 '> abc'");
    Check(FindLastRow("shift+tab") == status_row, "F2 打字:状态行行号不动");
    {
        const COORD cursor = CursorPosition();
        Check(cursor.Y == prompt_row && cursor.X == 5,
              "F2 打字:彩色提示符不占虚列,光标紧跟 abc");
    }

    // ---- F3 切档帧:Shift+Tab 三连,状态行原地变档、屏上零新增行 ----
    SendKey(VK_TAB, 0, SHIFT_PRESSED);
    Check(WaitForRowText(status_row, "auto", 5000), "F3 切档:状态行变 auto");
    // 0.21.x:提示符不再冠 [auto]/[yolo] 前缀,档位只在状态行体现;提示行
    // 恒是 "> abc"(F2 打的内容),既不含 [auto] 前缀,也不被切档动过。
    {
        const std::string prompt_after = ReadRow(prompt_row);
        Check(prompt_after.find("[auto]") == std::string::npos, "F3 切档:提示符不带 [auto] 前缀");
        Check(prompt_after.find("> abc") != std::string::npos, "F3 切档:提示行恒 '> abc'");
    }
    Check(FindLastRow("shift+tab") == status_row, "F3 切档:auto 档零新增行");
    SendKey(VK_TAB, 0, SHIFT_PRESSED);
    Check(WaitForRowText(status_row, "yolo", 5000), "F3 切档:状态行变 yolo");
    {
        // 配色属性:yolo 段是亮红(FOREGROUND_RED 有、GREEN 无)。
        std::vector<WORD> attrs;
        const std::string status = ReadRow(status_row, &attrs);
        const std::size_t pos = status.find("yolo");
        bool red = false;
        if (pos != std::string::npos) {
            // pos 是 UTF-8 字节偏移,attrs 是按字符排的——数一下 yolo 前面
            // 有几个字符。
            std::size_t chars = 0;
            for (std::size_t i = 0; i < pos;) {
                const unsigned char c = static_cast<unsigned char>(status[i]);
                i += c < 0x80 ? 1 : (c & 0xE0) == 0xC0 ? 2 : (c & 0xF0) == 0xE0 ? 3 : 4;
                ++chars;
            }
            if (chars < attrs.size()) {
                const WORD a = attrs[chars];
                red = (a & FOREGROUND_RED) != 0 && (a & FOREGROUND_GREEN) == 0;
            }
        }
        Check(red, "F3 切档:yolo 段是红色属性");
    }
    Check(FindLastRow("shift+tab") == status_row, "F3 切档:yolo 档零新增行");
    SendKey(VK_TAB, 0, SHIFT_PRESSED);
    Check(WaitForRowText(status_row, "确认模式", 5000), "F3 切档:转回确认档");

    // ---- F4 多行帧:正文添一行,下横线与状态栏一同下移 ----
    SendKey(VK_RETURN, L'\r', SHIFT_PRESSED);
    Sleep(400);
    Check(IsRuleRow(prompt_row + 2), "F4 多行:下横线随第二行下移");
    Check(FindLastRow("shift+tab") == status_row + 1, "F4 多行:状态栏随第二行下移");
    SendText("def");
    Check(WaitForRowText(prompt_row + 1, "def", 5000), "F4 多行:续行显示 def(两空格缩进)");
    {
        const COORD cursor = CursorPosition();
        Check(cursor.Y == prompt_row + 1 && cursor.X == 5,
              "F4 多行:Shift+Enter 后光标落在第二行末尾");
    }

    // F4a 截图回归:续行里混排 ASCII/CJK。11 枚汉字各占两格，连同 j 与
    // 两格续行缩进，光标应落在第 25 列的空白格，不能压住末字“有”。
    SendKey('C', 3, LEFT_CTRL_PRESSED);
    SendKey(VK_RETURN, L'\r', SHIFT_PRESSED);
    SendText("j就你看看人家的那个会有");
    Check(WaitForRowText(prompt_row + 1, "j就你看看人家的那个会有", 5000),
          "F4a 多行中英混排:续行文字完整");
    {
        const COORD cursor = CursorPosition();
        Check(cursor.Y == prompt_row + 1 && cursor.X == 25,
              "F4a 多行中英混排:光标落在末字后一格");
    }

    // ---- F4b 软换行:长逻辑行铺成两行，光标、下横线一同下移 ----
    SendKey('C', 3, LEFT_CTRL_PRESSED);
    Sleep(400);
    const std::string long_line(140, 'w');
    SendText(long_line);
    Sleep(400);
    Check(ReadRow(prompt_row).size() == 119, "F4b 软换行:首行吃满提示符后的 117 列");
    const std::string wrapped_remainder = ReadRow(prompt_row + 1);
    Check(wrapped_remainder == "  " + std::string(23, 'w'),
          "F4b 软换行:余下 23 字落到带两格缩进的续行");
    Check(IsRuleRow(prompt_row + 2), "F4b 软换行:下横线随物理行下移");
    const COORD wrapped_cursor = CursorPosition();
    Check(wrapped_cursor.Y == prompt_row + 1 && wrapped_cursor.X == 25,
          "F4b 软换行:光标落在续行末尾");

    // 再开一行,框与状态栏再向下长一行。
    SendKey(VK_RETURN, L'\r', SHIFT_PRESSED);
    Sleep(400);
    {
        const COORD cursor = CursorPosition();
        Check(cursor.Y == prompt_row + 2 && cursor.X == 2,
              "F4c 三行:光标落在第三行两格缩进后");
    }
    Check(IsRuleRow(prompt_row + 3), "F4c 三行:下横线向下长一行");
    Check(FindLastRow("shift+tab") == status_row + 2, "F4c 三行:状态行共向下长两行");

    // ---- F5 键位矫正:Ctrl+C 清空,Tab(空框)进焦点态无条目退回,Shift+Tab 仍切档 ----
    SendKey('C', 3, LEFT_CTRL_PRESSED);
    Sleep(400);
    Check(IsRuleRow(prompt_row + 1), "F5 清空:框缩回单行正文区");
    SendKey(VK_TAB, 0, 0);  // 空框 Tab:transcript 还是空的,焦点请求没人接,状态机得退回来
    Sleep(300);
    SendKey(VK_TAB, 0, SHIFT_PRESSED);
    Check(WaitForRowText(status_row, "auto", 5000), "F5 键位:无条目时 Tab 后 Shift+Tab 仍切档(焦点态已退)");
    SendKey(VK_TAB, 0, SHIFT_PRESSED);
    Check(WaitForRowText(status_row, "yolo", 5000), "F5 键位:切到 yolo 档");
    SendKey(VK_TAB, 0, SHIFT_PRESSED);
    Check(WaitForRowText(status_row, "确认模式", 5000), "F5 键位:切回确认档");

    if (composer_only) {
        SendText("exit");
        SendKey(VK_RETURN, L'\r', 0);
        if (WaitForSingleObject(pi.hProcess, 15000) != WAIT_OBJECT_0) {
            Log("INFO: composer-only exit 超时,强杀子进程");
            TerminateProcess(pi.hProcess, 9);
        }
        CloseHandle(pi.hProcess);
        Log(g_failures == 0 ? "RESULT: ALL PASS" :
                              "RESULT: " + std::to_string(g_failures) + " FAIL");
        return g_failures == 0 ? 0 : 1;
    }

    // ---- F6 提交帧:横线擦掉、提交行保留;一轮问答后统计行 + 新框 ----
    SendText("请用 read_file 工具读取 hello.txt,然后原样告诉我文件内容。");
    SendKey(VK_RETURN, L'\r', 0);
    // 0.21.x:流式期间正文下方常驻 footer 框(上横线/`> ` 输入行/下横线/
    // 状态行)。整段流式里都在,收束才擦——180s 内应能刮到一帧;定位靠
    // 框的结构,不靠占位提示文案。
    {
        const DWORD footer_deadline = GetTickCount() + 180000;
        bool footer_seen = false;
        while (GetTickCount() < footer_deadline) {
            if (FindFooterInputRow() >= 0) {
                footer_seen = true;
                break;
            }
            Sleep(100);
        }
        Check(footer_seen, "F6 流式期间:输出下方出现 footer 输入框(结构定位)");
    }
    Sleep(600);
    {
        // 收尾后 "> 请用..." 上移到原上横线那一行,原提示行现在是别的内容。
        const int submitted_row = FindLastRow("> 请用");
        Check(submitted_row == prompt_row - 1, "F6 提交:横线擦掉,提交行上移一行保留");
        Check(!IsRuleRow(prompt_row + 1), "F6 提交:下横线已擦");
    }
    Check(WaitForText("[tokens]", 180000), "F6 一轮问答:统计行出现(180s 内)");
    Sleep(1500);
    {
        const int tokens_row = FindLastRow("[tokens]");
        const std::string tokens_line = ReadRow(tokens_row);
        Log("INFO: 统计行 = " + tokens_line);
        Check(tokens_line.find("输入 ") != std::string::npos, "F6 统计行有输入 token 数(FormatTokenCount 接线)");
        const int new_status = FindLastRow("shift+tab");
        const std::string status = ReadRow(new_status);
        Log("INFO: 新状态行 = " + status);
        Check(new_status > tokens_row, "F6 新一帧框在统计行之下");
        Check(status.find("context ") != std::string::npos && status.find("context 0%") == std::string::npos,
              "F6 状态行 context 占比已刷新(非 0%)");
        Check(status.find("(") != std::string::npos, "F6 状态行 context 旁带 token 数字(k 化接线)");
    }

    // ---- F7 焦点态:Tab 进焦点态(现在有条目了),ESC 退出 ----
    {
        const int status_row2 = FindLastRow("shift+tab");
        SendKey(VK_TAB, 0, 0);
        const bool focused = WaitForText("[焦点 ", 5000);
        Check(focused, "F7 焦点:空框 Tab 进焦点态选最近条目");
        Sleep(300);
        SendKey(VK_ESCAPE, 0, 0);
        Sleep(500);
        // ESC 退出焦点态回编辑:接着 Shift+Tab 得是切档。
        const int status_row3 = FindLastRow("shift+tab");
        SendKey(VK_TAB, 0, SHIFT_PRESSED);
        Check(WaitForRowText(status_row3, "auto", 5000), "F7 焦点:ESC 退出后 Shift+Tab 恢复切档");
        SendKey(VK_TAB, 0, SHIFT_PRESSED);
        SendKey(VK_TAB, 0, SHIFT_PRESSED);
        WaitForRowText(status_row3, "确认模式", 5000);
        (void)status_row2;
    }

    // ---- F8 diff 帧(0.18.0):edit_file 预览删除行红底、新增行绿底、
    // 上下文无底;确认放行后终态留存同验,摘要是 "新增 N 行,删除 M 行"。----
    {
        // 素材文件驱动器自己写,别劳模型的驾。
        std::ofstream demo(std::filesystem::path(workdir) / L"diffdemo.txt", std::ios::binary | std::ios::trunc);
        demo << "alpha\nbeta\ngamma\ndelta\nepsilon\n";
        demo.close();
        const int prev_tokens_row = FindLastRow("[tokens]");
        SendText("请用 edit_file 工具把 diffdemo.txt 里的 gamma 这一行替换成 GAMMA,其余行一个字不动,替换完不用解释。");
        SendKey(VK_RETURN, L'\r', 0);
        // 确认档下 edit_file 要问 [y/a/N],确认块上方垫着 diff 预览。
        Check(WaitForText("- gamma", 180000), "F8 预览:diff 里出现删除行 '- gamma'(180s 内)");
        Sleep(800);
        {
            const int del_row = FindLastRow("- gamma");
            const int add_row = FindLastRow("+ GAMMA");
            const int ctx_row = FindLastRow("beta");
            Check(del_row >= 0 && add_row >= 0 && ctx_row >= 0, "F8 预览:删/增/上下文三种行都在屏上");
            if (del_row >= 0) {
                CheckDiffRowBg(del_row, "- gamma", /*want_red=*/true, /*want_green=*/false,
                                "F8 预览:删除行整行红底属性");
            }
            if (add_row >= 0) {
                CheckDiffRowBg(add_row, "+ GAMMA", /*want_red=*/false, /*want_green=*/true,
                                "F8 预览:新增行整行绿底属性");
            }
            if (ctx_row >= 0) {
                CheckDiffRowBg(ctx_row, "beta", /*want_red=*/false, /*want_green=*/false,
                                "F8 预览:上下文行无底色");
            }
        }
        // 放行,等终态摘要 + 留存 diff。
        SendText("y");
        SendKey(VK_RETURN, L'\r', 0);
        Check(WaitForText("新增 1 行,删除 1 行", 60000), "F8 终态:摘要新文案 '新增 1 行,删除 1 行'(60s 内)");
        Sleep(1000);
        {
            const int del_row = FindLastRow("- gamma");
            const int add_row = FindLastRow("+ GAMMA");
            Check(del_row >= 0 && add_row >= 0, "F8 终态:条目里留存 diff(删/增行都在)");
            if (del_row >= 0) {
                CheckDiffRowBg(del_row, "- gamma", /*want_red=*/true, /*want_green=*/false,
                                "F8 终态:留存删除行红底属性");
            }
            if (add_row >= 0) {
                CheckDiffRowBg(add_row, "+ GAMMA", /*want_red=*/false, /*want_green=*/true,
                                "F8 终态:留存新增行绿底属性");
            }
        }
        // 等这一轮新的统计行落定(F6 那行还在屏上,得盯"更靠下的一行"),
        // 别让收尾 exit 跟工具轮次撞车。
        {
            const DWORD deadline = GetTickCount() + 60000;
            while (GetTickCount() < deadline && FindLastRow("[tokens]") <= prev_tokens_row) {
                Sleep(200);
            }
        }
        Sleep(1000);
    }

    // ---- 收尾:exit ----
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
