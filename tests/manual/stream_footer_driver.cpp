// 0.22.x 流式脚注框化专用的"刮屏驱动器":跟 tests/manual/screen_driver.cpp 同一套
// 手艺(AllocConsole + WriteConsoleInputW + ReadConsoleOutputW),验的是这轮
// 新加的东西——流式期间正文下方常驻的框(上横线/输入行/下横线/状态行)、
// Working 动态着色、输入光标、常驻队列、ESC 打断、长输出滚屏时框还贴得住。
// 不进 ctest,集成验证时手动跑:
//   stream_footer_driver <lubancode.exe 路径> <子进程工作目录> <报告文件路径>

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <fstream>
#include <set>
#include <string>
#include <vector>

namespace {

HANDLE g_conin = INVALID_HANDLE_VALUE;
HANDLE g_conout = INVALID_HANDLE_VALUE;
std::ofstream g_report;
int g_failures = 0;
HANDLE g_child = nullptr;

std::string ReadRow(int row);

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

bool ChildStillRunning(const std::string& stage) {
    DWORD exit_code = 0;
    if (g_child == nullptr || !GetExitCodeProcess(g_child, &exit_code)) {
        Log("FAIL: " + stage + " 无法读取子进程状态,error=" + std::to_string(GetLastError()));
        ++g_failures;
        return false;
    }
    if (exit_code == STILL_ACTIVE) {
        return true;
    }
    Log("FAIL: " + stage + " 子进程已退出,exit=" + std::to_string(exit_code));
    for (int row = 0; row < 30; ++row) {
        const std::string text = ReadRow(row);
        if (!text.empty()) {
            Log("INFO: " + stage + " 退出现场 row[" + std::to_string(row) + "]=" + text);
        }
    }
    ++g_failures;
    return false;
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

int CursorRow() {
    CONSOLE_SCREEN_BUFFER_INFO info{};
    GetConsoleScreenBufferInfo(g_conout, &info);
    return info.dwCursorPosition.Y;
}

int CursorColumn() {
    CONSOLE_SCREEN_BUFFER_INFO info{};
    GetConsoleScreenBufferInfo(g_conout, &info);
    return info.dwCursorPosition.X;
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

std::string ReadAttributeSignature(int row) {
    const int width = BufferWidth();
    std::vector<CHAR_INFO> cells(static_cast<std::size_t>(width));
    SMALL_RECT region{0, static_cast<SHORT>(row), static_cast<SHORT>(width - 1), static_cast<SHORT>(row)};
    if (row < 0 ||
        !ReadConsoleOutputW(g_conout, cells.data(), COORD{static_cast<SHORT>(width), 1}, COORD{0, 0}, &region)) {
        return {};
    }
    std::string signature;
    signature.reserve(cells.size() * 2);
    for (const CHAR_INFO& cell : cells) {
        signature.push_back(static_cast<char>(cell.Attributes & 0xff));
        signature.push_back(static_cast<char>((cell.Attributes >> 8) & 0xff));
    }
    return signature;
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

int CountRowsWithText(const std::string& needle, int max_rows = 400) {
    int count = 0;
    for (int row = 0; row < max_rows; ++row) {
        if (ReadRow(row).find(needle) != std::string::npos) {
            ++count;
        }
    }
    return count;
}

bool WaitForTextCount(const std::string& needle, int wanted, int timeout_ms) {
    const DWORD deadline = GetTickCount() + static_cast<DWORD>(timeout_ms);
    while (GetTickCount() < deadline) {
        if (CountRowsWithText(needle) >= wanted) {
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

// "裸横线"行:整行只有横线字符(─/-)与空白。turn 尾分界线(── Worked
// for 2.1s ──)夹着文字,不算裸线;叠影残留是清一色的纯横线行,才算。
bool IsBareRuleRow(int row) {
    if (!IsRuleRow(row)) {
        return false;
    }
    const std::string text = ReadRow(row);
    for (std::size_t i = 0; i < text.size();) {
        if (text.compare(i, 3, "\xe2\x94\x80") == 0) {  // ─
            i += 3;
            continue;
        }
        if (text[i] == '-' || text[i] == ' ') {
            ++i;
            continue;
        }
        return false;
    }
    return true;
}

// 结构化找 footer 的输入行(0.25.x 起排队界面定位不靠文案——文案一改便全
// 倒,规矩是"按结构认框")。Composer 合流(P1)后框随内容长高:上横线、
// 以 '>' 起的输入区、下横线、状态行。自定义 padding 可以插空行,故不写死
// 相邻关系。认法:输入行上头 4 行内有一根横线、下头 6 行内有一根横线且横线下
// 不是横线(状态行)。从底往上扫,命中最近的一个(流式期间最底下那个框就
// 是 footer;待发队列的 "> 消息" 行虽然也以 '>' 起,却在更上方,扫不到
// 前就被真输入行截住)。
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
        for (int b = i + 1; b <= i + 6 && b + 1 < 400; ++b) {
            if (IsRuleRow(b) && !IsRuleRow(b + 1)) {
                return i;
            }
        }
    }
    return -1;
}

// 输入行上方的第一根横线(上横线):队列区就在它上头。下方的第一根横线是
// 下横线,再下一行是状态行。
int FindRuleAboveInput(int input_row) {
    for (int r = input_row - 1; r >= input_row - 4 && r >= 0; --r) {
        if (IsRuleRow(r)) {
            return r;
        }
    }
    return -1;
}

int FindRuleBelowInput(int input_row) {
    for (int r = input_row + 1; r <= input_row + 6 && r < 400; ++r) {
        if (IsRuleRow(r)) {
            return r;
        }
    }
    return -1;
}

bool WaitForFooterInputRow(int timeout_ms, int* found_row = nullptr) {
    const DWORD deadline = GetTickCount() + static_cast<DWORD>(timeout_ms);
    while (GetTickCount() < deadline) {
        const int row = FindFooterInputRow();
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
    // 120 列宽、缓冲 400 行,可视窗口只给 30 行——比 screen_driver.cpp 的 40
    // 行窗口更矮,更容易在一轮长回复里触发可视区域的自然滚屏,好验框子
    // "贴着正文底部走"这条,不用真把 400 行缓冲区喂满(那不现实)。
    SetConsoleScreenBufferSize(g_conout, COORD{120, 400});
    SMALL_RECT window{0, 0, 119, 29};
    SetConsoleWindowInfo(g_conout, TRUE, &window);
    FlushConsoleInputBuffer(g_conin);
    Log("INFO: console buffer " + std::to_string(BufferWidth()) + " cols, window 30 rows");

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
    g_child = pi.hProcess;

    // ---- 开场帧:composer 的框先出来,确认程序正常起来了 ----
    Check(WaitForText("shift+tab", 30000), "开场:composer 状态行出现(30s 内)");
    Sleep(300);

    // ---- G1 流式框化:发一句会触发较长回复的问题,等流式脚注框出现 ----
    SendText("请务必先调用 read_file 读取 README.md 的前 20 行,再用大约 200 字介绍一下这个项目,"
             "分两三段说,不用标题。");
    SendKey(VK_RETURN, L'\r', 0);
    // Working/思考中:首个流事件前亮色须沿文字移动。刮同一行的字符属性
    // 签名，至少见到两帧不同分布才算真动画，不只换了文案。
    std::set<std::string> spinner_frames;
    bool working_and_composer_visible = false;
    bool working_cursor_in_composer = false;
    const DWORD spinner_deadline = GetTickCount() + 10000;
    while (GetTickCount() < spinner_deadline) {
        const int spinner_row = FindLastRow("思考中");
        const int composer_row = FindFooterInputRow();
        if (spinner_row >= 0) {
            spinner_frames.insert(ReadAttributeSignature(spinner_row));
        }
        if (spinner_row >= 0 && composer_row > spinner_row) {
            working_and_composer_visible = true;
            working_cursor_in_composer = CursorRow() == composer_row && CursorColumn() >= 2;
        }
        if (spinner_frames.size() >= 2 && working_and_composer_visible && working_cursor_in_composer) {
            break;
        }
        Sleep(50);
    }
    Check(spinner_frames.size() >= 2,
          "G0 Working:亮色扫过文字,捕获到 " + std::to_string(spinner_frames.size()) + " 帧不同配色");
    Check(working_and_composer_visible,
          "G0 Working 与输入框同帧可见,输入框不再被 spinner 挂起");
    Check(working_cursor_in_composer,
          "G0 Working 动画期间物理光标仍停在输入框");
    ChildStillRunning("G0 Working");
    // 输入行靠结构定位(上横线/`> `输入行/下横线/状态行四连),不靠占位
    // 提示文案——文案改版不再连坐。
    int input_row = -1;
    Check(WaitForFooterInputRow(60000, &input_row), "G1 流式期间:框出现(60s 内,结构定位输入行)");
    if (input_row >= 0) {
        const int top_rule_row = FindRuleAboveInput(input_row);
        const int bottom_rule_row = FindRuleBelowInput(input_row);
        const int status_row = bottom_rule_row + 1;
        // Composer 合流(35c621d)后主输入区只占一行正文、紧贴上下横线
        // (kComposerTopPaddingRows=0),旧"隔一行留白"形状已撤。
        Check(top_rule_row == input_row - 1, "G1 上横线紧贴输入行上方(合流后紧排)");
        Check(bottom_rule_row >= 0, "G1 下横线在输入区之下");
        Check(IsRuleRow(top_rule_row), "G1 上横线在输入行上一行");
        Check(IsRuleRow(bottom_rule_row), "G1 下横线在输入行下一行(状态行上一行)");
        const std::string input_text = ReadRow(input_row);
        Check(!input_text.empty() && input_text[0] == '>', "G1 输入行以 '> ' 开头(跟 composer 一个样式)");
        // 忙时空草稿的占位提示自带 Esc 打断(旧状态行尾巴撤了之后,"打断"
        // 可发现性的新家;空队列时全屏只有这里写)。
        Check(input_text.find("Esc") != std::string::npos && input_text.find("打断") != std::string::npos,
              "G1 忙时空草稿:占位提示写明 Esc 打断");
        Check(ReadRow(status_row).find("shift+tab") != std::string::npos, "G1 状态行有 shift+tab 提示(复用 PrintStatusLine)");
        Check(CursorRow() == input_row && CursorColumn() >= 2,
              "G1 物理光标停在输入行 '> ' 后面,不再钉在正文末尾");
        Log("INFO: G1 框位置 top_rule=" + std::to_string(top_rule_row) + " input=" + std::to_string(input_row) +
            " bottom_rule=" + std::to_string(bottom_rule_row) + " status=" + std::to_string(status_row));
    } else {
        Check(false, "G1 框位置:没找到占位提示行,后续 G2/G3 跳过");
    }

    // ---- G2 排队回显:流式还没完时敲几个字,输入行要实时显示 "> 你好排队" ----
    SendText("你好排队");
    const bool g2_echo_seen = WaitForText("你好排队", 8000);
    Check(g2_echo_seen, "G2 排队回显:输入行实时显示已键入内容(8s 内出现 '你好排队')");
    ChildStillRunning("G2 排队回显");
    if (g2_echo_seen) {
        const int echo_row = FindLastRow("你好排队");
        Check(CursorRow() == echo_row && CursorColumn() > 2,
              "G2 键入时:光标跟在输入文字末尾");
    }
    if (!g2_echo_seen) {
        // 诊断:没等到就把此刻缓冲区靠下的十几行原样记下来,看文字到底
        // 落在哪儿(比如落进了下一轮 composer 的编辑行,说明流式已经先
        // 结束了,不是框本身的问题)。
        int probe_row = FindLastRow("");  // 兜底找不到就跳过
        (void)probe_row;
        for (int r = 0; r < 30; ++r) {
            const std::string row_text = ReadRow(r);
            if (!row_text.empty()) {
                Log("INFO: G2 诊断 row[" + std::to_string(r) + "]=" + row_text);
            }
        }
    }
    SendKey(VK_RETURN, L'\r', 0);
    // 落队后:正文挪进输入框上方的队列区。0.28.x 起队列区带标题行,行序
    // (自上而下)= 标题行、消息行、上横线、(留白)、输入行(BuildSteering
    // QueueRows 的真序:标题在前);输入行清空回占位提示。只有一条时不写
    // "另有 N 条"。位置全按上横线相对算(Composer 合流后输入行与横线之间
    // 隔着留白行,不再紧贴)。
    bool g2_queued_row_seen = false;
    const DWORD g2_queue_deadline = GetTickCount() + 8000;
    while (GetTickCount() < g2_queue_deadline) {
        const int row = FindFooterInputRow();
        const int rule = row >= 0 ? FindRuleAboveInput(row) : -1;
        if (rule >= 2 && ReadRow(rule - 1).find("你好排队") != std::string::npos &&
            ReadRow(rule - 2).find("送出") != std::string::npos &&
            ReadRow(row).find("你好排队") == std::string::npos) {
            g2_queued_row_seen = true;
            break;
        }
        Sleep(100);
    }
    Check(g2_queued_row_seen, "G2 落队:正文挪到输入框上方队列区(消息行在标题行之上),输入行清空");
    Check(FindLastRow("待发送消息") == -1, "G2 落队:一条不写'待发送消息 1 条'汇总头");
    Check(FindLastRow("另有") == -1, "G2 落队:未超可见上限,不出现'另有 N 条'");
    if (g2_queued_row_seen) {
        // 标题说清送达时机与操作(规格"标题"节):工具边界送出、Esc 立即送、
        // Shift+← 取回编辑,三样都得在。
        const std::string title = ReadRow(FindLastRow("送出"));
        Check(title.find("Esc") != std::string::npos, "G2 队列标题:写明 Esc 打断并立即送");
        Check(title.find("Shift") != std::string::npos, "G2 队列标题:写明取回编辑键");
    }

    // ---- G4 Shift+← 取回编辑:正文空 + 队列非空,按 Shift+Left 取回最新
    // 一条;队列区该条标"编辑中",正文装进真编辑器(可挪光标/改字),Enter
    // 原位替换。 ----
    {
        SendKey(VK_LEFT, 0, SHIFT_PRESSED);
        bool recalled = false;
        const DWORD recall_deadline = GetTickCount() + 6000;
        while (GetTickCount() < recall_deadline) {
            const int row = FindFooterInputRow();
            const int rule = row >= 0 ? FindRuleAboveInput(row) : -1;
            // 取回态形状(queue_model.cpp):rule-1 是消息行 "  ↳ [编辑中] 你好
            // 排队"(标记挂在消息行前缀),rule-2 是标题行 "正在编辑排队消
            // 息 · …"(没有"编辑中"三字连写)。两行任一见到编辑态即算取回。
            if (rule >= 2 && ReadRow(row).find("你好排队") != std::string::npos &&
                (ReadRow(rule - 1).find("编辑中") != std::string::npos ||
                 ReadRow(rule - 2).find("正在编辑") != std::string::npos)) {
                recalled = true;
                break;
            }
            Sleep(100);
        }
        Check(recalled, "G4 Shift+← 取回:正文进输入行,队列条目挂'[编辑中]'标记");
        if (recalled) {
            // 光标落在末尾:补一个字再 Enter 原位替换(编辑态不打字即同步,
            // 队列行换新全靠 Enter 提交),位置不动。
            SendText("过");
            Sleep(200);
            SendKey(VK_RETURN, L'\r', 0);
            bool replaced = false;
            const DWORD replace_deadline = GetTickCount() + 6000;
            while (GetTickCount() < replace_deadline) {
                const int row = FindFooterInputRow();
                const int rule = row >= 0 ? FindRuleAboveInput(row) : -1;
                if (rule >= 2 && ReadRow(rule - 1).find("你好排队过") != std::string::npos &&
                    ReadRow(rule - 1).find("编辑中") == std::string::npos &&
                    ReadRow(rule - 2).find("正在编辑") == std::string::npos &&
                    ReadRow(row).find("你好排队过") == std::string::npos) {
                    replaced = true;
                    break;
                }
                Sleep(100);
            }
            Check(replaced, "G4 Enter 原位替换:队列行换新文,id/位置不动,输入行清空");
            if (!replaced) {
                for (int r = 0; r < 60; ++r) {
                    const std::string row_text = ReadRow(r);
                    if (!row_text.empty()) {
                        Log("INFO: G4 诊断 row[" + std::to_string(r) + "]=" + row_text);
                    }
                }
            }
        }
    }
    Sleep(300);
    // 落队之后框应该重新出现在新位置(占位提示复位),再验一次上下横线还在。
    {
        int status_row2 = -1;
        Check(WaitForText("shift+tab", 10000, &status_row2), "G2 落队后:状态行仍在(框没被冲散)");
        if (status_row2 >= 0) {
            // 合流后紧排版式(自上而下):上横线(status-3)/ 输入行
            // (status-2)/ 下横线(status-1)/ 状态行(status)。
            Check(IsRuleRow(status_row2 - 1), "G2 落队后:下横线还在状态行上一行");
            Check(!IsRuleRow(status_row2 - 2), "G2 落队后:下横线正上方是输入行(非横线)");
            Check(IsRuleRow(status_row2 - 3), "G2 落队后:上横线还在(框结构完整,没有残影/错位)");
            // 占位提示复位:落队清空正文后,"Esc 打断"提示跟着回到输入行
            // (打的是输入行这一行,队列标题那处"打断"不算数)。
            Check(ReadRow(status_row2 - 2).find("打断") != std::string::npos,
                  "G2 落队后:占位提示复位,输入行重新带 Esc 打断");
        }
    }

    // ---- G2b 流式 slash 提示:流式期间在输入行打 '/',状态行之下实时列出
    // 匹配命令(跟空闲 composer 一致)。判据不靠文案:结构上认"待发行(上
    // 横线上方) + 输入行'/' + 状态行之下出现若干以两空格起头的提示行"——
    // 这套组合只有流式 footer 才有(空闲 composer 没有待发区),不会误认。
    // 退格清掉 '/' 后提示行须跟着消失。
    {
        SendText("/");  // 流式输入行打一个 '/',提示应实时出现
        bool hint_seen = false;
        int hint_count_seen = 0;
        const DWORD hint_deadline = GetTickCount() + 15000;
        while (GetTickCount() < hint_deadline) {
            const int row = FindFooterInputRow();
            const int rule = row >= 0 ? FindRuleAboveInput(row) : -1;
            if (rule >= 2 && ReadRow(rule - 1).find("你好排队") != std::string::npos) {
                const std::string input_text = ReadRow(row);  // 输入行 "> /"(打了一个 '/')
                int hint_count = 0;
                for (int r = row + 3; r < row + 11 && r < 400; ++r) {
                    const std::string t = ReadRow(r);
                    if (t.size() >= 3 && t[0] == ' ' && t[1] == ' ' && t[2] == '/') {
                        ++hint_count;
                    }
                }
                if (input_text.size() >= 3 && input_text[0] == '>' && input_text[2] == '/' &&
                    hint_count >= 3) {
                    hint_seen = true;
                    hint_count_seen = hint_count;
                    break;
                }
            }
            Sleep(100);
        }
        Check(hint_seen, "G2b 流式 slash 提示:'/' 实时列出命令(状态行之下),队列区同屏共存");
        Log("INFO: G2b 提示行数 " + std::to_string(hint_count_seen));
        SendKey(VK_BACK, L'\b', 0);
        bool hint_gone = false;
        const DWORD hint_gone_deadline = GetTickCount() + 8000;
        while (GetTickCount() < hint_gone_deadline) {
            const int row = FindFooterInputRow();
            if (row >= 0) {
                int hint_count = 0;
                for (int r = row + 3; r < row + 11 && r < 400; ++r) {
                    const std::string t = ReadRow(r);
                    if (t.size() >= 3 && t[0] == ' ' && t[1] == ' ' && t[2] == '/') {
                        ++hint_count;
                    }
                }
                if (hint_count == 0) {
                    hint_gone = true;
                    break;
                }
            }
            Sleep(100);
        }
        Check(hint_gone, "G2b 退格清空后,提示行跟着消失(高度账对平,无残影)");
    }

    // 用户反馈的原始现场:read_file 做完后，AgentLoop 会发起第二次模型请求。
    // 旧实现此时由新 Spinner 构造出的 SuspendScope 把整个 footer 藏掉，只剩
    // “思考中”。这里必须在第二个 Working 周期再次抓到 composer 与光标。
    Check(WaitForText("read_file(", 60000), "G2 工具续轮:read_file 已执行(60s 内)");
    if (FindLastRow("read_file(") < 0) {
        for (int r = 0; r < 80; ++r) {
            const std::string row_text = ReadRow(r);
            if (!row_text.empty()) {
                Log("INFO: G2 诊断 row[" + std::to_string(r) + "]=" + row_text);
            }
        }
    }
    bool post_tool_working_and_composer = false;
    bool post_tool_cursor_in_composer = false;
    const DWORD post_tool_deadline = GetTickCount() + 15000;
    while (GetTickCount() < post_tool_deadline) {
        const int tool_row = FindLastRow("read_file(");
        const int spinner_row = FindLastRow("思考中");
        const int composer_row = FindFooterInputRow();
        if (tool_row >= 0 && spinner_row > tool_row && composer_row > spinner_row) {
            post_tool_working_and_composer = true;
            post_tool_cursor_in_composer = CursorRow() == composer_row && CursorColumn() >= 2;
            if (post_tool_cursor_in_composer) {
                break;
            }
        }
        Sleep(50);
    }
    Check(post_tool_working_and_composer,
          "G2 工具续轮:第二个 Working 与输入框同帧可见");
    Check(post_tool_cursor_in_composer,
          "G2 工具续轮:第二个 Working 期间光标仍在输入框");

    // 等第一轮问答彻底收束。统计降噪(0.26.x)后真控制台紧凑态不打
    // [tokens] 长行(30 行窄窗也盛不下详细态),回合收尾的锚改用 turn 尾
    // 分界线 "Worked for"(两轮各一条,打断轮才是 Stopped after)。
    Check(WaitForTextCount("Worked for", 2, 180000),
          "G1 主消息与排队消息:两轮 turn 尾分界线都出现(180s 内)");
    const int read_file_title_rows = CountRowsWithText("read_file(");
    Check(read_file_title_rows == 1,
          "G2 工具终态原位覆写:read_file 标题只剩一行(实际 " +
              std::to_string(read_file_title_rows) + " 行)");
    Sleep(1000);

    // ---- G3 ESC 打断:再发一句,几乎立刻按 ESC,验证打断提示 + 程序继续可用 ----
    {
        const int prev_tokens_row = FindLastRow("Worked for");
        SendText("请用大约 500 字详细介绍一下 C++23 的新特性。");
        SendKey(VK_RETURN, L'\r', 0);
        // 给流式一点点时间起步(先看到框子/开始吐字),再打断——纯秒按容易
        // 打在请求还没发出去之前,反而测不到"框子被正确擦掉"这条。
        Sleep(1500);
        SendKey(VK_ESCAPE, 0, 0);
        Check(WaitForText("[\xe5\xb7\xb2\xe6\x89\x93\xe6\x96\xad]", 15000), "G3 ESC 打断:出现 '[已打断]' 提示");
        Sleep(500);
        // 打断后回到常驻提示符,composer 的框(不是流式框,但视觉同款)得
        // 干干净净地出现,没有跟旧流式框叠在一起变成一堆重复横线。
        Check(WaitForText("shift+tab", 15000), "G3 打断后:composer 状态行重新出现");
        Sleep(300);
        int post_status = FindLastRow("shift+tab");
        if (post_status >= 0) {
            // composer 新框的标准版式:上横线(status-3) / 输入行(status-2) /
            // 下横线(status-1) / 状态行(status)。"没有叠影残留"精确验成这
            // 四行严丝合缝——上下横线各只有一根;叠一份旧框出来的"线贴线"形状跟它完全不同,
            // 足够分辨有没有叠影,不受"屏幕上方还有更早几轮遗留的框"干扰
            //(那些是正常的历史记录,不是这一次打断留下的残留)。
            Check(IsRuleRow(post_status - 1), "G3 打断后:composer 下横线正常(没有残留的流式框横线叠加)");
            Check(!ReadRow(post_status - 2).empty() && ReadRow(post_status - 2)[0] == '>',
                  "G3 打断后:下横线正上方是 '> ' 输入行(框形状完整,不多不少)");
            Check(IsRuleRow(post_status - 3), "G3 打断后:上横线紧贴输入行");
            // 上横线正上方那一行:turn 尾分界线(── Stopped after … ──)夹着
            // 文字,是合法邻居;只有清一色纯横线的"裸线"才是叠影残留。
            Check(!IsBareRuleRow(post_status - 4), "G3 打断后:上横线之上无裸横线叠影(文字分界线除外)");
            for (int r = post_status - 4; r <= post_status; ++r) {
                const std::string row_text = ReadRow(r);
                if (!row_text.empty()) {
                    Log("INFO: G3 诊断 row[" + std::to_string(r) + "]=" + row_text);
                }
            }
        }
        (void)prev_tokens_row;
    }

    // ---- G5 Esc 打断并立即送:队列非空时 Esc 不再只打断——收场后队列消息
    // 立即自动发出(pump 打一行 "> 内容");多条同批、超可见上限时队列区
    // 出现"另有 N 条"(30 行窄窗下输入框与状态行仍看得见,前面各步已经顺
    // 带验过窄窗共存)。 ----
    {
        SendText("再讲讲 C++ 模块的历史。");
        SendKey(VK_RETURN, L'\r', 0);
        Sleep(1500);  // 让流式起步,确认排队的消息真落在"流式期间"
        for (const char* line : {"第一条:先讲 export", "第二条:再讲 import", "第三条:收个尾", "第四条:别超时"}) {
            SendText(line);
            SendKey(VK_RETURN, L'\r', 0);
            Sleep(120);
        }
        // 四条排队(可见上限 3):队列区出现"另有 1 条"。
        Check(WaitForText("另有", 8000), "G5 多条排队:超可见上限出现'另有 N 条'");
        SendKey(VK_ESCAPE, 0, 0);
        Check(WaitForText("[\xe5\xb7\xb2\xe6\x89\x93\xe6\x96\xad]", 15000), "G5 Esc:出现 '[已打断]'");
        // 打断收场后,会话泵立即把排队消息发出:屏上出现 pump 打的
        // "> 第一条:先讲 export"(流式期间排队,收场即送,不等用户再敲)。
        Check(WaitForText("> \xe7\xac\xac\xe4\xb8\x80\xe6\x9d\xa1", 30000),
              "G5 Esc 立即送:收场后第一条排队消息自动发出('> 第一条')");
        // 剩余三条也逐条跟上(会话泵一条一轮),给足时间;不逐条断言文案,
        // 只验第四条最终也发了出去——四条一个不丢。
        Check(WaitForText("> \xe7\xac\xac\xe5\x9b\x9b\xe6\x9d\xa1", 300000),
              "G5 Esc 立即送:四条排队消息逐条全部送出");
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
    g_child = nullptr;

    Log(g_failures == 0 ? "RESULT: ALL PASS" : "RESULT: " + std::to_string(g_failures) + " FAIL");
    return g_failures == 0 ? 0 : 1;
}
