// Esc 取消真机 P50/P95 复测专用"刮屏/注键驱动器"(MiniCPM5-1B 真机巡检单
// P1):跟 busy_tab_driver 同一套手艺(AllocConsole + WriteConsoleInputW +
// ReadConsoleOutputW),验的是"真 vLLM 长答流式期间按 Esc,多久断开并回
// 输入态"。先前两回测不准:真机一回拖 8.4 秒,终端注键又受输出排队干扰,
// 按键落点不明——这只驱动把键直接写进子进程控制台输入队列,刮屏认
// "[已打断]"与"Stopped after"落点,不拿排队数据充数。
//
// 每轮:发一条长散文题 → 等流式 footer 与输入框同帧 → 流稳后按 Esc →
// 量 Esc→"Stopped after"(断开回输入态)与 Esc→"[已打断]"(即时回执)
// 两笔时延。轮末再做一轮"下一轮可续"(CANCEL_RECOVER_OK 路数)。若正文
// 锚句("雨从夜里下起")在 Esc 前已上屏,断言打断后它还在——半截正文保留。
//
// 不进 ctest,集成验证手动跑(真 vLLM 端点用环境变量喂,见巡检单"现场
// 配置"节):
//   esc_cancel_driver <lubancode.exe 路径> <子进程工作目录> <报告文件路径> [轮数 1-5]
//
// 报告只记时延毫秒、每轮判据与屏行摘要,不记 API key、不记模型正文全文。

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <algorithm>
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

// 高精度计时:QueryPerformanceCounter,毫秒。
double NowMs() {
    static LARGE_INTEGER freq = [] {
        LARGE_INTEGER f{};
        QueryPerformanceFrequency(&f);
        return f;
    }();
    LARGE_INTEGER counter{};
    QueryPerformanceCounter(&counter);
    return static_cast<double>(counter.QuadPart) * 1000.0 / static_cast<double>(freq.QuadPart);
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
    if (row < 0 || row > 399 ||
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

int FindLastRow(const std::string& needle, int max_rows = 400) {
    for (int row = max_rows - 1; row >= 0; --row) {
        if (ReadRow(row).find(needle) != std::string::npos) {
            return row;
        }
    }
    return -1;
}

bool WaitForText(const std::string& needle, int timeout_ms) {
    const DWORD deadline = GetTickCount() + static_cast<DWORD>(timeout_ms);
    while (GetTickCount() < deadline) {
        if (FindLastRow(needle) >= 0) {
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
        Sleep(15);
    }
}

// 一行里是不是一根框横线(至少 40 个 '─' 或 '-' 连排)——认 composer 框用,
// 写法与 busy_tab_driver 同源。
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

// composer/footer 的输入行(以 '>' 起,上下各有横线)。
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

bool WaitForFooterInputRow(int timeout_ms) {
    const DWORD deadline = GetTickCount() + static_cast<DWORD>(timeout_ms);
    while (GetTickCount() < deadline) {
        if (FindFooterInputRow() >= 0) {
            return true;
        }
        Sleep(50);
    }
    return false;
}

// 流式 footer 还活着:turn 级活动条("思考中")在,输入框也在。
bool StreamFooterAlive() {
    return FindLastRow("\xE6\x80\x9D\xE8\x80\x83\xE4\xB8\xAD") >= 0 && FindFooterInputRow() >= 0;  // "思考中"
}

// 等"流式 footer 与输入框同帧"(busy_tab T2 同款判据)。
bool WaitForStreamingUp(int timeout_ms) {
    const DWORD deadline = GetTickCount() + static_cast<DWORD>(timeout_ms);
    while (GetTickCount() < deadline) {
        const int spinner_row = FindLastRow("思考中");
        const int row = FindFooterInputRow();
        if (spinner_row >= 0 && row > spinner_row) {
            return true;
        }
        Sleep(50);
    }
    return false;
}

void Check(bool ok, const std::string& what) {
    if (ok) {
        Log("PASS: " + what);
        return;
    }
    Log("FAIL: " + what);
    ++g_failures;
    int last = -1;
    for (int r = 399; r >= 0; --r) {
        if (!ReadRow(r).empty()) {
            last = r;
            break;
        }
    }
    Log("---- screen dump (cursor " + std::to_string(CursorColumn()) + "," + std::to_string(CursorRow()) +
        ", last row " + std::to_string(last) + ") ----");
    for (int r = last >= 24 ? last - 24 : 0; r <= last; ++r) {
        const std::string t = ReadRow(r);
        if (!t.empty()) {
            Log("DUMP[" + std::to_string(r) + "] " + t);
        }
    }
    Log("---- dump end ----");
}

// 近位秩百分位(nearest-rank):P50 = 升序第 ceil(0.5n) 个,P95 同法。
double Percentile(std::vector<double> sorted, double p) {
    if (sorted.empty()) {
        return -1;
    }
    std::sort(sorted.begin(), sorted.end());
    const std::size_t rank = static_cast<std::size_t>(
        std::ceil(p / 100.0 * static_cast<double>(sorted.size())));
    const std::size_t index = (std::min)(rank, sorted.size()) - 1;
    return sorted[index];
}

std::string FormatMs(double ms) {
    char buf[32];
    sprintf_s(buf, "%.0f", ms);
    return buf;
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
    int rounds = 5;
    if (argc >= 5) {
        rounds = _wtoi(argv[4]);
    }
    if (rounds < 1) {
        rounds = 1;
    }
    if (rounds > 5) {
        rounds = 5;  // 巡检单上限:Repeats 不超 5
    }
    Log("INFO: rounds = " + std::to_string(rounds));

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

    // ---- 开场:composer 起来了 ----
    Check(WaitForText("shift+tab", 30000), "T0 开场:composer 状态行出现(30s 内)");
    Sleep(300);

    // 正文锚句:题面要求第一句固定,流式正文一旦开写就能刮到。1B 模型服从
    // 度不保证,锚句没等到就不拿"半截正文保留"当判据(只记观察)。
    const std::string anchor = "\xE9\x9B\xA8\xE4\xBB\x8E\xE5\xA4\x9C\xE9\x87\x8C\xE4\xB8\x8B\xE8\xB5\xB7";  // 雨从夜里下起
    const std::string prompt =
        "请写一篇大约 4000 字的中文散文,主题是江南的雨季,分六段,不要调用任何工具,"
        "第一句必须是:雨从夜里下起。";

    std::vector<double> stop_ms;   // Esc → "Stopped after"(断开并回输入态)
    std::vector<double> echo_ms;   // Esc → "[已打断]"(即时回执)
    int body_anchor_rounds = 0;
    int body_kept_rounds = 0;

    int attempts = 0;
    while (static_cast<int>(stop_ms.size()) < rounds && attempts < rounds + 3) {
        ++attempts;
        const int round_no = static_cast<int>(stop_ms.size()) + 1;
        Log("---- 第 " + std::to_string(round_no) + " 轮(尝试 " + std::to_string(attempts) + ")----");
        if (!WaitForFooterInputRow(15000)) {
            Check(false, "R" + std::to_string(round_no) + " 起步:输入框可用(15s 内)");
            break;
        }
        SendText(prompt);
        SendKey(VK_RETURN, L'\r', 0);
        if (!WaitForStreamingUp(60000)) {
            Log("SKIP: R" + std::to_string(round_no) + " 60s 内没等到流式 footer(端点慢或已答完),重试");
            WaitForFooterInputRow(120000);
            continue;
        }
        // 流稳了再等一小会儿:给思考/正文起跑的时间,Esc 落在长答中途。
        const DWORD settle_deadline = GetTickCount() + 8000;
        bool anchor_seen = false;
        while (GetTickCount() < settle_deadline) {
            if (FindLastRow(anchor) >= 0) {
                anchor_seen = true;
                break;
            }
            if (!StreamFooterAlive()) {
                break;  // 流提前结束:这轮作废,重试
            }
            Sleep(100);
        }
        if (!StreamFooterAlive()) {
            Log("SKIP: R" + std::to_string(round_no) + " 模型答得太快,流已结束,重试");
            WaitForText("Worked for", 30000);
            WaitForFooterInputRow(30000);
            continue;
        }
        if (anchor_seen) {
            ++body_anchor_rounds;
        }
        Log("INFO: R" + std::to_string(round_no) +
            (anchor_seen ? " 正文锚句已上屏" : " 正文锚句未见(不判半截保留)"));

        // 按 Esc,量两笔时延。轮询 25ms,比 1 秒目标细一档。
        const double t0 = NowMs();
        SendKey(VK_ESCAPE, 0, 0);
        bool echoed = false;
        double echo_at = -1;
        double stop_at = -1;
        const DWORD deadline = GetTickCount() + 30000;
        while (GetTickCount() < deadline) {
            if (!echoed && FindLastRow("[已打断]") >= 0) {
                echoed = true;
                echo_at = NowMs();
            }
            if (FindLastRow("Stopped after") >= 0) {
                stop_at = NowMs();
                break;
            }
            Sleep(25);
        }
        if (echo_at < 0 && stop_at >= 0) {
            // 收尾前再核一遍(轮询窗口可能错过)
            echoed = FindLastRow("[已打断]") >= 0;
            if (echoed) {
                echo_at = stop_at;
            }
        }
        if (stop_at < 0) {
            Check(false, "R" + std::to_string(round_no) + " Esc 后 30s 内未见 'Stopped after' 收口");
            break;
        }
        const double stop_delta = stop_at - t0;
        stop_ms.push_back(stop_delta);
        if (echo_at >= 0) {
            echo_ms.push_back(echo_at - t0);
        }
        Log("INFO: R" + std::to_string(round_no) + " Esc→[已打断] " + FormatMs(echo_at - t0) +
            "ms  Esc→Stopped after " + FormatMs(stop_delta) + "ms");
        Check(echoed, "R" + std::to_string(round_no) + " Esc 回执:'[已打断]' 上屏");
        Check(stop_delta <= 2000.0, "R" + std::to_string(round_no) + " Esc→回输入态在 2 秒门内(实际 " +
                                        FormatMs(stop_delta) + "ms)");
        // 半截正文保留:锚句上屏过的轮,打断后锚句还得在(整缓冲找,正文可能滚上去)。
        if (anchor_seen) {
            const bool kept = FindLastRow(anchor) >= 0;
            if (kept) {
                ++body_kept_rounds;
            }
            Check(kept, "R" + std::to_string(round_no) + " 半截正文保留:锚句打断后仍在屏账");
        }
        // 回输入态:收口行之后 composer 输入框回来。
        Check(WaitForFooterInputRow(15000), "R" + std::to_string(round_no) + " 收口后输入框回位(15s 内)");
        Sleep(300);
    }

    // ---- 汇总:P50/P95 ----
    if (!stop_ms.empty()) {
        std::string all;
        for (double v : stop_ms) {
            if (!all.empty()) {
                all += " ";
            }
            all += FormatMs(v);
        }
        Log("INFO: Esc→Stopped after 全样本(ms): " + all);
        Log("INFO: P50 = " + FormatMs(Percentile(stop_ms, 50)) + "ms  P95 = " +
            FormatMs(Percentile(stop_ms, 95)) + "ms(目标 1 秒内)");
        if (!echo_ms.empty()) {
            std::string echoes;
            for (double v : echo_ms) {
                if (!echoes.empty()) {
                    echoes += " ";
                }
                echoes += FormatMs(v);
            }
            Log("INFO: Esc→[已打断] 全样本(ms): " + echoes + "  P95 = " +
                FormatMs(Percentile(echo_ms, 95)) + "ms");
        }
        Log("INFO: 正文锚句上屏 " + std::to_string(body_anchor_rounds) + " 轮,其中打断后保留 " +
            std::to_string(body_kept_rounds) + " 轮");
        Check(Percentile(stop_ms, 95) <= 1000.0,
              "P95(Esc→断开回输入态)在 1 秒内(实际 " + FormatMs(Percentile(stop_ms, 95)) + "ms)");
        Check(static_cast<int>(stop_ms.size()) >= 3, "有效轮数至少 3(实际 " + std::to_string(stop_ms.size()) + ")");
    } else {
        Check(false, "一轮有效时延都没量到");
    }

    // ---- 下一轮可续:打断后再问一句,答得上来就是没砖死 ----
    {
        if (!WaitForFooterInputRow(15000)) {
            Check(false, "续问前输入框可用");
        } else {
            SendText("只回复四个字:一切正常");
            SendKey(VK_RETURN, L'\r', 0);
            const DWORD deadline = GetTickCount() + 120000;
            bool answered = false;
            while (GetTickCount() < deadline) {
                if (FindLastRow("Worked for") >= 0) {
                    answered = true;
                    break;
                }
                Sleep(200);
            }
            Check(answered, "打断后下一轮可续:续问正常收口(Worked for)");
        }
    }

    // ---- 收尾:正常退出 ----
    SendText("/exit");
    SendKey(VK_RETURN, L'\r', 0);
    bool exited = false;
    if (WaitForSingleObject(pi.hProcess, 30000) == WAIT_OBJECT_0) {
        exited = true;
        DWORD code = 0;
        GetExitCodeProcess(pi.hProcess, &code);
        Check(code == 0, "/exit:进程退出码 0(实际 " + std::to_string(code) + ")");
    }
    if (!exited) {
        TerminateProcess(pi.hProcess, 1);
        Check(false, "/exit:进程 30s 内退出");
    }
    CloseHandle(pi.hProcess);

    Log(g_failures == 0 ? "ALL PASS" : ("FAILURES: " + std::to_string(g_failures)));
    FreeConsole();
    return g_failures == 0 ? 0 : 1;
}
