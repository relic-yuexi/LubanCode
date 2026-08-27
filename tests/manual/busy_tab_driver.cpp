// 忙碌排队输入框 Tab 补全(0.30.x)专用的"刮屏驱动器":跟
// tests/manual/stream_footer_driver.cpp 同一套手艺(AllocConsole + WriteConsoleInputW
// + ReadConsoleOutputW),验的是模型流式期间底部排队输入框的 Tab 补全链路——
// 候选可见、Tab 当场补全、提示随完成收起、连按轮转带 "> " 标记、空正文 Tab
// 是明确 no-op(不留暗焦点态)、排队消息由轮末队列泵按 slash 命令分派(不当
// 普通正文发给模型)、SharedEditor 历史不多一条 "/effort"。不进 ctest,集成
// 验证时手动跑(要真钥匙,环境变量喂):
//   busy_tab_driver <lubancode.exe 路径> <子进程工作目录> <报告文件路径>

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
    std::vector<CHAR_INFO> cells(static_cast<std::size_t>(width));
    SMALL_RECT region{0, static_cast<SHORT>(row), static_cast<SHORT>(width - 1), static_cast<SHORT>(row)};
    if (row < 0 ||
        !ReadConsoleOutputW(g_conout, cells.data(), COORD{static_cast<SHORT>(width), 1}, COORD{0, 0}, &region)) {
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

void Check(bool ok, const std::string& what) {
    if (ok) {
        Log("PASS: " + what);
        return;
    }
    Log("FAIL: " + what);
    ++g_failures;
    // 失败即转储此刻缓冲区最后 30 个非空行原文,盲猜不如看屏。
    int last = -1;
    for (int r = 399; r >= 0; --r) {
        if (!ReadRow(r).empty()) {
            last = r;
            break;
        }
    }
    Log("---- screen dump (cursor " + std::to_string(CursorColumn()) + "," + std::to_string(CursorRow()) +
        ", last row " + std::to_string(last) + ") ----");
    for (int r = last >= 30 ? last - 30 : 0; r <= last; ++r) {
        const std::string t = ReadRow(r);
        if (!t.empty()) {
            Log("DUMP[" + std::to_string(r) + "] " + t);
        }
    }
    Log("---- dump end ----");
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
        if (FindLastRow(needle) == -1) {
            return true;
        }
        Sleep(100);
    }
    return false;
}

// 可视区口径找行(鬼影不计):footer 随流式正文滚动后,滚出窗口的旧帧行
// 会在回滚缓冲里留影,整缓冲扫会把这种用户看不见的瞬影当残帧。
int FindLastRowInViewport(const std::string& needle) {
    CONSOLE_SCREEN_BUFFER_INFO info{};
    if (!GetConsoleScreenBufferInfo(g_conout, &info)) {
        return -1;
    }
    for (int row = info.srWindow.Bottom; row >= info.srWindow.Top; --row) {
        if (ReadRow(row).find(needle) != std::string::npos) {
            return row;
        }
    }
    return -1;
}

bool WaitForTextGoneInViewport(const std::string& needle, int timeout_ms) {
    const DWORD deadline = GetTickCount() + static_cast<DWORD>(timeout_ms);
    while (GetTickCount() < deadline) {
        if (FindLastRowInViewport(needle) == -1) {
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

// 结构化找 footer/composer 的输入行:Composer 合流(P1)后框随内容长高——
// 上横线、上留白、以 '>' 起的输入区、下补空、下横线、状态行,不再假定
// 输入行紧贴横线。认法:输入行上头 4 行内有一根横线、下头 6 行内有一根
// 横线且横线下不是横线(状态行)。从底往上扫,命中最近的一个(流式期间
// 最底下那个框就是 footer;空闲后是主 composer;待发队列的 "> 消息" 行
// 虽然也以 '>' 起,却在更上方,扫不到前就被真输入行截住)。
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

// 输入行上方的第一根横线(上横线):队列区就在它上头(标题 rule-2、消息
// rule-1,BuildSteeringQueueRows 的真序)。
int FindRuleAboveInput(int input_row) {
    for (int r = input_row - 1; r >= input_row - 4 && r >= 0; --r) {
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

// 输入行之下的 slash 提示区里,以 "> " 起头的那一行(轮转选中标记)。
// 返回内容;没有选中标记给空串。
std::string FindMarkedHintBelow(int input_row) {
    for (int r = input_row + 3; r < input_row + 12 && r < 400; ++r) {
        const std::string t = ReadRow(r);
        if (t.size() >= 2 && t[0] == '>' && t[1] == ' ') {
            return t;
        }
    }
    return {};
}

int CountHintRowsBelow(int input_row) {
    int count = 0;
    for (int r = input_row + 3; r < input_row + 12 && r < 400; ++r) {
        const std::string t = ReadRow(r);
        if (t.size() >= 3 && t[0] == ' ' && t[1] == ' ' && t[2] == '/') {
            ++count;
        }
    }
    return count;
}

// 流式 footer 还挂着吗:状态行带"打断"提示是流式 footer 独有的(空闲
// composer 的状态行没有它)。轮末/出错后键就归空闲 composer,再按回车会把
// 草稿真发给模型——落队前的硬前提。
bool StreamFooterAlive() {
    // 流式 footer 活着 = 输入行在 + turn 级活动条("• 思考中 (Ns)",整轮不
    // 熄)还在。旧锚"状态行带'打断'"已失效:Esc 打断提示如今只写在队列
    // 标题里,空队列时全屏没有"打断"字样。
    const int row = FindFooterInputRow();
    return row >= 0 && FindLastRow("\xE6\x80\x9D\xE8\x80\x83\xE4\xB8\xAD") >= 0;  // "思考中"
}

// 等输入行(以 '>' 起)呈现出指定内容。ReadRow 会掐掉行尾空格,所以
// "/effort "(带尾空格的补全态)只能匹配到 "/effort" 这一截。
bool WaitForInputRowText(const std::string& needle, int timeout_ms) {
    const DWORD deadline = GetTickCount() + static_cast<DWORD>(timeout_ms);
    while (GetTickCount() < deadline) {
        const int row = FindFooterInputRow();
        if (row >= 0 && ReadRow(row).find(needle) != std::string::npos) {
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
    SetConsoleScreenBufferSize(g_conout, COORD{120, 400});
    SMALL_RECT window{0, 0, 119, 29};
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

    // ---- T0 开场:composer 起来了 ----
    Check(WaitForText("shift+tab", 30000), "T0 开场:composer 状态行出现(30s 内)");
    Sleep(300);

    // ---- T1 发起一轮够长的纯文本流式(明确"不调用工具"):整轮只有一段长
    //      流,排队消息没有工具边界可钻,会稳稳挂到轮末队列泵——T6/T7/T9 才
    //      是确定性的。(让模型自由发挥去读文件的话,工具边界会把队列瞬间
    //      送达;让模型数数又太快收工——2500 字散文的流式窗口最稳。)
    SendText("请写一篇大约 2500 字的中文散文,主题是山中的四季,分四段,不要调用任何工具,也不要标题以外的解释。");
    SendKey(VK_RETURN, L'\r', 0);

    // ---- T2 流式 footer 出现:必须等到 Working 动画 + 输入框同帧可见,证明
    //      轮次真的开跑、空闲 composer 已退场,后面的按键才落在排队输入框里。
    //      (只等"框出现"会撞上空闲 composer 还没退休的窗口,按键全喂错地方。)
    bool streaming_up = false;
    int input_row = -1;
    {
        const DWORD deadline = GetTickCount() + 60000;
        while (GetTickCount() < deadline) {
            const int spinner_row = FindLastRow("思考中");
            const int row = FindFooterInputRow();
            if (spinner_row >= 0 && row > spinner_row) {
                streaming_up = true;
                input_row = row;
                break;
            }
            Sleep(100);
        }
    }
    Check(streaming_up, "T2 流式期间:Working 与 footer 输入框同帧可见(60s 内)");
    Sleep(500);

    // ---- T3 空正文 Tab:明确 no-op(不进焦点态、不闪、框还在) ----
    {
        SendKey(VK_TAB, L'\t', 0);
        Sleep(400);
        const int row = FindFooterInputRow();
        Check(row >= 0, "T3 空正文 Tab no-op:footer 框还在,没有暗状态把画面弄丢");
        if (row >= 0) {
            const std::string t = ReadRow(row);
            // 占位提示文案自带 "/help" 字样,判据只能卡"不得以 '> /' 起头"
            // (打进去的 slash 草稿才这个样子),不能要求整行无 '/'。
            Check(t.rfind("> /", 0) != 0, "T3 空正文 Tab no-op:输入行没有冒出 slash 草稿");
            Check(FindMarkedHintBelow(row).empty(), "T3 空正文 Tab no-op:没有选中标记行");
        }
    }

    // ---- T4 /eff 唯一命中:候选先看得见,Tab 当场补 /effort ,提示收起 ----
    {
        SendText("/eff");
        Check(WaitForText("同 /think", 10000), "T4 键入 /eff:候选提示出现('/effort 同 /think')");
        SendKey(VK_TAB, L'\t', 0);
        Check(WaitForInputRowText("/effort", 8000), "T4 Tab:输入行当场变成 /effort(不闪、不铺新行)");
        // 收起断言走可视区口径:旧帧的候选行滚出窗口后会在回滚缓冲留影,
        // 整缓冲扫会误报"没收起"。
        Check(WaitForTextGoneInViewport("同 /think", 6000),
              "T4 补成唯一命令带空格后:候选提示收起,不留小尾巴(可视区)");
    }

    // ---- T5 补全后继续输入参数 ----
    SendText("xhigh");
    Check(WaitForInputRowText("/effort xhigh", 8000), "T5 补全后继续输入:输入行 /effort xhigh");

    // ---- T6 回车落队 ----
    const bool queue_flow = StreamFooterAlive();
    if (!queue_flow) {
        Log("SKIP: T6/T7/T9 流式已提前结束(模型数得太快),落队/取回/轮末泵这轮不验");
    } else {
        SendKey(VK_RETURN, L'\r', 0);
        bool queued = false;
        const DWORD deadline = GetTickCount() + 8000;
        while (GetTickCount() < deadline) {
            const int row = FindFooterInputRow();
            const int rule = row >= 0 ? FindRuleAboveInput(row) : -1;
            if (rule >= 2 && ReadRow(rule - 1).find("/effort xhigh") != std::string::npos &&
                ReadRow(rule - 2).find("送出") != std::string::npos &&
                ReadRow(row).find("/effort") == std::string::npos) {
                queued = true;
                break;
            }
            Sleep(100);
        }
        Check(queued, "T6 回车落队:正文挪进队列区(标题带'送出'),输入行清空");
    }

    // ---- T7 Shift+← 取回编辑:删掉 xhigh 再原位替换 ----
    if (!queue_flow) {
        Log("SKIP: T7 未落队,无可取回");
    } else {
        SendKey(VK_LEFT, 0, SHIFT_PRESSED);
        bool recalled = false;
        const DWORD deadline = GetTickCount() + 6000;
        while (GetTickCount() < deadline) {
            const int row = FindFooterInputRow();
            const int rule = row >= 0 ? FindRuleAboveInput(row) : -1;
            // 取回态形状(queue_model.cpp):rule-1 是消息行 "  ↳ [编辑中]
            // /effort xhigh"(标记挂消息行),rule-2 是标题行 "正在编辑排队
            // 消息 · …"。两行任一见编辑态即算取回。
            if (rule >= 2 && ReadRow(row).find("/effort xhigh") != std::string::npos &&
                (ReadRow(rule - 1).find("编辑中") != std::string::npos ||
                 ReadRow(rule - 2).find("正在编辑") != std::string::npos)) {
                recalled = true;
                break;
            }
            Sleep(100);
        }
        Check(recalled, "T7 Shift+← 取回:正文进输入行,队列条目挂'[编辑中]'标记");
        for (int i = 0; i < 4; ++i) {
            SendKey(VK_BACK, L'\b', 0);  // 删掉 "xhigh"
            Sleep(30);
        }
        Check(WaitForInputRowText("/effort", 6000), "T7 取回编辑:删掉 xhigh 后输入行回到 /effort");
        SendKey(VK_RETURN, L'\r', 0);
        bool replaced = false;
        const DWORD replace_deadline = GetTickCount() + 8000;
        while (GetTickCount() < replace_deadline) {
            const int row = FindFooterInputRow();
            const int rule = row >= 0 ? FindRuleAboveInput(row) : -1;
            if (rule >= 2 && ReadRow(rule - 1).find("/effort") != std::string::npos &&
                ReadRow(rule - 1).find("xhigh") == std::string::npos &&
                ReadRow(rule - 1).find("编辑中") == std::string::npos &&
                ReadRow(rule - 2).find("正在编辑") == std::string::npos &&
                ReadRow(row).find("/effort") == std::string::npos) {
                replaced = true;
                break;
            }
            Sleep(100);
        }
        Check(replaced, "T7 Enter 原位替换:队列行换成 /effort,输入行清空");
    }

    // ---- T8 多候选轮转:/se 命中 /sessions /send(命令表序,谁在前不打死:
    //      断言只看"正文与 '> ' 标记同走、且真的换了一枚候选") ----
    {
        SendText("/se");
        bool two_hints = false;
        const DWORD deadline = GetTickCount() + 10000;
        while (GetTickCount() < deadline) {
            const int row = FindFooterInputRow();
            if (row >= 0) {
                const std::string below =
                    ReadRow(row + 3) + " " + ReadRow(row + 4) + " " + ReadRow(row + 5);
                if (below.find("/send") != std::string::npos && below.find("/sessions") != std::string::npos) {
                    two_hints = true;
                    break;
                }
            }
            Sleep(100);
        }
        Check(two_hints, "T8 键入 /se:两个同前缀候选都列出来(/sessions /send)");

        // 轮转一步:正文换成两枚候选之一,"> " 标记落在同一枚上。
        const auto rotation_step = [&](const std::string& exclude_input) -> std::string {
            const DWORD step_deadline = GetTickCount() + 8000;
            while (GetTickCount() < step_deadline) {
                const int row = FindFooterInputRow();
                if (row >= 0) {
                    const std::string input = ReadRow(row);
                    const std::string marked = FindMarkedHintBelow(row);
                    if (!marked.empty() && input.size() > 2 && input.rfind("> /", 0) == 0) {
                        // 输入行形如 "> /xxx",标记行 "> /xxx  说明":取候选名对齐。
                        if (exclude_input.empty() || input != exclude_input) {
                            if (input.find("/send") != std::string::npos ||
                                input.find("/sessions") != std::string::npos) {
                                if ((input.find("/send") != std::string::npos &&
                                     marked.find("/send") != std::string::npos) ||
                                    (input.find("/sessions") != std::string::npos &&
                                     marked.find("/sessions") != std::string::npos)) {
                                    return input;
                                }
                            }
                        }
                    }
                }
                Sleep(100);
            }
            return {};
        };

        SendKey(VK_TAB, L'\t', 0);
        const std::string first_input = rotation_step("");
        Check(!first_input.empty(), "T8 第一下 Tab:正文轮到某一枚候选,标记 '> ' 落在同一枚");

        SendKey(VK_TAB, L'\t', 0);
        const std::string second_input = rotation_step(first_input);
        Check(!second_input.empty() && second_input != first_input,
              "T8 第二下 Tab:正文与标记一起换到另一枚候选");

        for (int i = 0; i < 11; ++i) {  // "/sessions "/"/send " 最长 10 字符,退格清干净
            SendKey(VK_BACK, L'\b', 0);
            Sleep(30);
        }
        Sleep(400);
        const int row = FindFooterInputRow();
        Check(row < 0 || (ReadRow(row).find("/sessions") == std::string::npos &&
                          ReadRow(row).find("/send") == std::string::npos),
              "T8 退格清空:草稿清掉(不落队)");
    }

    // ---- T9 轮末队列泵:/effort 按 slash 命令分派,不是发给模型的正文 ----
    if (!queue_flow) {
        Log("SKIP: T9 未落队,无轮末泵可验");
    } else {
        const bool pumped = WaitForText("当前推理强度", 240000) || WaitForText("推理强度已切到", 10000);
        Check(pumped, "T9 轮末队列泵:/effort 按 slash 分派执行,出现推理强度信息(240s 内)");
        {
            // 队列应已清空:等 footer/composer 输入行回来,且队列区没有 /effort 残留。
            int idle_row = -1;
            Check(WaitForFooterInputRow(30000, &idle_row), "T9 轮末:输入框回到可用状态");
            const int idle_rule = idle_row >= 0 ? FindRuleAboveInput(idle_row) : -1;
            if (idle_rule >= 2) {
                // 泵完队列该整个退场:队列条目的签名行首 "  ↳ " 在可视区一行
                // 都不许剩。泵自己的回显("> /effort")与推理强度输出是合法
                // 正文,不拿 "/effort" 字样当残留判据。
                int queue_item_rows = 0;
                CONSOLE_SCREEN_BUFFER_INFO info{};
                if (GetConsoleScreenBufferInfo(g_conout, &info)) {
                    for (int r = info.srWindow.Top; r <= info.srWindow.Bottom; ++r) {
                        if (ReadRow(r).rfind("  \xE2\x86\xB3 ", 0) == 0) {  // "  ↳ "
                            ++queue_item_rows;
                        }
                    }
                }
                Check(queue_item_rows == 0,
                      "T9 队列已清空,可视区无 '  ↳ ' 队列条目行(实际 " +
                          std::to_string(queue_item_rows) + " 行)");
            }
        }
    }

    // ---- T10 空闲历史:上键翻到的是真提问,不是 "/effort"(排队路不进
    //      SharedEditor 历史) ----
    {
        Sleep(800);
        SendKey(VK_UP, 0, 0);
        Sleep(600);
        const int row = FindFooterInputRow();
        bool ok = false;
        if (row >= 0) {
            const std::string draft = ReadRow(row);
            ok = draft.find("山中的四季") != std::string::npos && draft.find("/effort") == std::string::npos;
        }
        Check(ok, "T10 空闲历史上翻:取回的是真提问(山中的四季),没有多出一条 /effort");
        SendKey(VK_DOWN, 0, 0);  // 翻回空草稿,别把长文留在框里
        Sleep(300);
    }

    // ---- T11 收尾:正常退出 ----
    SendText("/exit");
    SendKey(VK_RETURN, L'\r', 0);
    bool exited = false;
    if (WaitForSingleObject(pi.hProcess, 30000) == WAIT_OBJECT_0) {
        exited = true;
        DWORD code = 0;
        GetExitCodeProcess(pi.hProcess, &code);
        Check(code == 0, "T11 /exit:进程退出码 0(实际 " + std::to_string(code) + ")");
    }
    if (!exited) {
        TerminateProcess(pi.hProcess, 1);
        Check(false, "T11 /exit:进程 30s 内退出");
    }
    CloseHandle(pi.hProcess);

    Log(g_failures == 0 ? "ALL PASS" : ("FAILURES: " + std::to_string(g_failures)));
    FreeConsole();
    return g_failures == 0 ? 0 : 1;
}
