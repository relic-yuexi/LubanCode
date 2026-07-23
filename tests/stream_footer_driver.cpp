// 0.22.x 流式脚注框化专用的"刮屏驱动器":跟 tests/screen_driver.cpp 同一套
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
        const int composer_row = FindLastRow("排队下一条");
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
    // "排队下一条" 只出现在输入行的占位提示里(状态行是模式/模型/context 那
    // 一段,不含这句话),所以搜到的这一行本身就是输入行,不是状态行——
    // 版式:上横线(matched-1) / 输入行(matched) / 下横线(matched+1) / 状态行(matched+2)。
    int input_row = -1;
    Check(WaitForText("排队下一条", 60000, &input_row), "G1 流式期间:框出现(60s 内,靠占位提示定位输入行)");
    if (input_row >= 0) {
        const int top_rule_row = input_row - 1;
        const int bottom_rule_row = input_row + 1;
        const int status_row = input_row + 2;
        Check(IsRuleRow(top_rule_row), "G1 上横线在输入行上一行");
        Check(IsRuleRow(bottom_rule_row), "G1 下横线在输入行下一行(状态行上一行)");
        const std::string input_text = ReadRow(input_row);
        Check(!input_text.empty() && input_text[0] == '>', "G1 输入行以 '> ' 开头(跟 composer 一个样式)");
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
    Check(WaitForText("待发送消息 1 条", 5000), "G2 落队:常驻队列区显示 1 条待发送消息");
    Sleep(300);
    // 落队之后框应该重新出现在新位置(占位提示复位),再验一次上下横线还在。
    {
        int status_row2 = -1;
        Check(WaitForText("shift+tab", 10000, &status_row2), "G2 落队后:状态行仍在(框没被冲散)");
        if (status_row2 >= 0) {
            Check(IsRuleRow(status_row2 - 1), "G2 落队后:下横线还在状态行上一行");
            Check(IsRuleRow(status_row2 - 3), "G2 落队后:上横线还在(框结构完整,没有残影/错位)");
        }
    }

    // 用户反馈的原始现场:read_file 做完后，AgentLoop 会发起第二次模型请求。
    // 旧实现此时由新 Spinner 构造出的 SuspendScope 把整个 footer 藏掉，只剩
    // “思考中”。这里必须在第二个 Working 周期再次抓到 composer 与光标。
    Check(WaitForText("read_file(", 60000), "G2 工具续轮:read_file 已执行(60s 内)");
    bool post_tool_working_and_composer = false;
    bool post_tool_cursor_in_composer = false;
    const DWORD post_tool_deadline = GetTickCount() + 15000;
    while (GetTickCount() < post_tool_deadline) {
        const int tool_row = FindLastRow("read_file(");
        const int spinner_row = FindLastRow("思考中");
        const int composer_row = FindLastRow("排队下一条");
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

    // 等第一轮问答彻底收束(统计行落定),避免跟下面的 ESC 测试撞车。
    Check(WaitForTextCount("[tokens]", 2, 180000),
          "G1 主消息与排队消息:两轮统计行都出现(180s 内)");
    const int read_file_title_rows = CountRowsWithText("read_file(");
    Check(read_file_title_rows == 1,
          "G2 工具终态原位覆写:read_file 标题只剩一行(实际 " +
              std::to_string(read_file_title_rows) + " 行)");
    Sleep(1000);

    // ---- G3 ESC 打断:再发一句,几乎立刻按 ESC,验证打断提示 + 程序继续可用 ----
    {
        const int prev_tokens_row = FindLastRow("[tokens]");
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
            // 下横线(status-1) / 状态行(status)。"没有叠影残留"精确验成
            // "紧邻状态行的这四行严丝合缝,不多不少"——下横线正下方(status-1)
            // 是根线,它上面(status-2,输入行)不是线,再上面(status-3)又是
            // 根线;这三行的形状跟流式框叠一份出来的"线-线-输入-线-线"完全
            // 不同,足够分辨有没有叠影,不受"屏幕上方还有更早几轮遗留的框"
            // 干扰(那些是正常的历史记录,不是这一次打断留下的残留)。
            Check(IsRuleRow(post_status - 1), "G3 打断后:composer 下横线正常(没有残留的流式框横线叠加)");
            Check(!IsRuleRow(post_status - 2), "G3 打断后:下横线正上方是输入行,不是另一根线(没有双线叠影)");
            Check(IsRuleRow(post_status - 3), "G3 打断后:上横线在正确位置(框形状完整,不多不少)");
            for (int r = post_status - 6; r <= post_status; ++r) {
                const std::string row_text = ReadRow(r);
                if (!row_text.empty()) {
                    Log("INFO: G3 诊断 row[" + std::to_string(r) + "]=" + row_text);
                }
            }
        }
        (void)prev_tokens_row;
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
