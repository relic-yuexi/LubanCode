// 真机验证驱动器(#一/#二/#三 三条修复专用,不进 ctest):跟 screen_driver.cpp
// 同一套手艺(AllocConsole + WriteConsoleInput + ReadConsoleOutput,真控制台
// 专用),单独开一个文件——screen_driver.cpp 是既有的 UI-B/C/D 回归脚本
// (F1-F8),这三条修复不去碰它,免得牵连不相关的场景。
//
// 验的三件事:
//   1. /clear 之后旧对话确实从可见屏幕消失(整块缓冲区扫一遍,横幅原文
//      找不到了)。
//   2. 连续触发子代理多次内部工具调用,不再有"一黄一绿"的重复/滞留行——
//      具体检验:一轮问答收尾之后,全屏找不到残留的字面 "Running..."
//      (NewItem 建条目时硬编码的执行中摘要,不受主题/语言影响;锚点跟
//      ticker 打架的旧 bug 会让某个条目的执行中行永远被晾在原地擦不掉,
//      这个残留字符串就是那次 bug 的铁证)。
//   3. 紧凑模式(默认)下子代理内层工具明细不再逐条铺屏——收尾之后全屏
//      找不到 4 空格缩进 + ● 圆点的子工具条目行(SubTool 专属缩进规矩)。
//
// 用法: fold_dup_clear_driver <lubancode.exe 路径> <子进程工作目录> <报告文件路径>
// 环境变量(LUBANCODE_BASE_URL/LUBANCODE_API_KEY/LUBANCODE_MODEL 或者走
// 子进程工作目录/USERPROFILE 下现成的 config.json)由调用方设好,子进程
// 原样继承。

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

std::wstring Utf8ToWide(const std::string& s) {
    if (s.empty()) {
        return {};
    }
    const int len = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring out(static_cast<std::size_t>(len), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), out.data(), len);
    return out;
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

int BufferWidth() {
    CONSOLE_SCREEN_BUFFER_INFO info{};
    GetConsoleScreenBufferInfo(g_conout, &info);
    return info.dwSize.X;
}

int BufferHeight() {
    CONSOLE_SCREEN_BUFFER_INFO info{};
    GetConsoleScreenBufferInfo(g_conout, &info);
    return info.dwSize.Y;
}

std::string ReadRow(int row) {
    const int width = BufferWidth();
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

int FindLastRow(const std::string& needle, int max_rows) {
    for (int row = max_rows - 1; row >= 0; --row) {
        if (ReadRow(row).find(needle) != std::string::npos) {
            return row;
        }
    }
    return -1;
}

bool WaitForText(const std::string& needle, int timeout_ms, int max_rows, int* found_row = nullptr) {
    const DWORD deadline = GetTickCount() + static_cast<DWORD>(timeout_ms);
    while (GetTickCount() < deadline) {
        const int row = FindLastRow(needle, max_rows);
        if (row >= 0) {
            if (found_row != nullptr) {
                *found_row = row;
            }
            return true;
        }
        Sleep(200);
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

// 全屏(缓冲区全高)扫一遍某个 needle 出现在哪些行,返回行号列表。
std::vector<int> FindAllRows(const std::string& needle, int max_rows) {
    std::vector<int> rows;
    for (int row = 0; row < max_rows; ++row) {
        if (ReadRow(row).find(needle) != std::string::npos) {
            rows.push_back(row);
        }
    }
    return rows;
}

// 子工具条目专属签名:4 空格缩进 + ● 圆点(transcript.cpp 的 SubTool 缩进
// 规矩)。ReadConsoleOutputW 拿到的是终端渲染完的可见字符,ANSI 转义序列
// 早被消化成属性,不会混进文本——直接找这个前缀就行,不用管颜色。
bool RowIsSubToolItem(const std::string& text) {
    constexpr const char* kDot = "\xE2\x97\x8F";  // ● U+25CF
    return text.compare(0, 4, "    ") == 0 && text.compare(4, 3, kDot) == 0;
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
    // 缓冲区跟 screen_driver.cpp 已验证过的配置一致(120x400)——先用这个
    // 已知能跑通的尺寸把三条修复的基本功能过一遍,小缓冲区(贴近 ConPTY
    // 那种"缓冲区约等于可见窗口"的场景,更容易触发 #二 的滚屏根因)留到
    // 基本功能确认没问题之后再单独加场景验。
    SetConsoleScreenBufferSize(g_conout, COORD{120, 400});
    SMALL_RECT window{0, 0, 119, 39};
    SetConsoleWindowInfo(g_conout, TRUE, &window);
    FlushConsoleInputBuffer(g_conin);
    const int height = BufferHeight();
    Log("INFO: console buffer " + std::to_string(BufferWidth()) + "x" + std::to_string(height));

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

    // ---- 开场:等状态行出现 ----
    int status_row = -1;
    const bool opened = WaitForText("shift+tab", 30000, height, &status_row);
    Check(opened, "开场:状态行出现(30s 内)");
    if (!opened) {
        TerminateProcess(pi.hProcess, 9);
        return 1;
    }
    Sleep(300);

    // ---- #一:/clear 清屏 ----
    // 横幅上必有的一句提示("banner.hint" 键)先确认在屏,再 /clear,再确认
    // 从可见缓冲区消失(整块缓冲区扫,不只是当前窗口)。
    const bool banner_before = FindLastRow("/help", height) >= 0;
    Check(banner_before, "#一 /clear 前:横幅提示('/help 看命令')在屏");
    SendText("/clear");
    SendKey(VK_RETURN, L'\r', 0);
    Check(WaitForText("已清空对话历史", 5000, height), "#一 /clear:确认行出现");
    Sleep(200);
    {
        const bool banner_gone = FindLastRow("/help", height) < 0;
        Check(banner_gone, "#一 /clear:横幅提示从整块缓冲区消失(真清屏,不是滚走)");
    }

    // ---- #二/#三:委派子代理,连打多次内部工具调用 ----
    // 明确要求走 agent 工具委派(不是让主循环自己直接调 search/read_file)——
    // #二/#三 的 bug 都长在 OnSubToolStart/OnSubToolResult 这条子代理内层
    // 工具的展示链路上,得真的经过子代理才踩得到。任务故意收窄到单个文件、
    // 单个函数名,让子代理一两轮工具调用就能收工,别让真机验证卡在模型
    // 自己的思考耗时上。
    SendText(
        "请你必须调用内置的 agent 工具,委派一个子任务代理去完成,不要自己直接调用"
        "search/read_file:读取 D:\\lubancode\\src\\platform\\console_win.cpp 这一"
        "个文件,找出 ClearScreen 函数定义在第几行,最后只回答行号。");
    SendKey(VK_RETURN, L'\r', 0);

    int agent_row = -1;
    const bool agent_started = WaitForText("agent(", 60000, height, &agent_row);
    if (!agent_started) {
        Log("INFO: 60s 内没等到 agent( 工具调用,模型这次没选择委派子任务——"
            "跳过 #二/#三 的屏面校验,不计入 PASS/FAIL(不是这两条修复本身的问题)");
    } else {
        Log("INFO: 子代理已起,行号=" + std::to_string(agent_row));
        // 等一轮问答彻底收尾:先等 agent 条目摘要落定("子代理 N 轮 · M 次
        // 工具",transcript.cpp::AgentDoneSummary 独有的措辞),这只代表子
        // 代理那一步完了,主循环收到结果后通常还要再吐一段总结文字、才会
        // 打统计行"[tokens]"——两个都等到,才是"这一轮真正结束",提前扫屏
        // 找残留只会把"还没轮到它"误判成"卡住了"。注意不能拿"次工具"当
        // 关键字——AgentStatusBoard 那条 ticker 摘要行(agent_status.cpp)
        // 跑动中就会写"N 次工具调用",跟这里要等的终态摘要撞了子串,得挑
        // 只有终态才会出现的"子代理 "开头。超时给得宽松(5 分钟),真机
        // 模型响应慢不算这两条修复的账。
        const bool agent_done = WaitForText("\xE5\xAD\x90\xE4\xBB\xA3\xE7\x90\x86 ", 120000, height);  // "子代理 "
        Check(agent_done, "#二/#三 子代理收尾:agent 条目摘要落定('子代理 N 轮',120s 内)");
        const bool turn_done = agent_done && WaitForText("[tokens]", 300000, height);
        Check(turn_done, "#二/#三 整轮收尾:统计行出现(300s 内)");

        if (!turn_done) {
            Log("INFO: 整轮没能在超时内收尾,#二/#三 的屏面校验没有可靠的\"已经\n"
                "落定终态\"基准点,跳过(不计入额外 FAIL,避免把\"还没跑完\"\n"
                "误判成\"卡住了\")。");
        } else {
            Sleep(1000);  // 给 AgentStatusPainter 的 400ms ticker 留够时间收场擦干净

            // #二:全屏找残留的字面 "Running..."——NewItem 建条目时硬编码的
            // 执行中摘要,任何条目(主/子)只要走到这个字符串就说明它卡在
            // 执行中态没被正常改写成终态;旧版的锚点/ticker 打架 bug 会让
            // 某个子工具条目的这一行永远擦不掉(新内容画到了错位的行,旧
            // 行原样留着)。
            const auto stuck_running = FindAllRows("Running...", height);
            Check(stuck_running.empty(),
                  "#二 收尾后全屏无残留 'Running...'(锚点/ticker 不再打架,共 " +
                      std::to_string(stuck_running.size()) + " 处残留)");

            // #三:紧凑模式(默认,没按过 Ctrl+O)下,子代理内层工具条目不
            // 逐条铺屏——全屏扫一遍,找不到任何一行是"4 空格缩进 + ● 圆点"
            // 的子工具条目签名。
            int sub_item_rows = 0;
            for (int row = 0; row < height; ++row) {
                if (RowIsSubToolItem(ReadRow(row))) {
                    ++sub_item_rows;
                }
            }
            Check(sub_item_rows == 0,
                  "#三 紧凑模式:全屏无子工具条目行(4 空格缩进+●),实际 " +
                      std::to_string(sub_item_rows) + " 行");

            // 反向抽样:agent 条目本身(顶层,不缩进)理应看得见,顺手确认
            // 没有连带把主区也一起吞了——只是子代理内层被折叠。
            Check(FindLastRow("agent(", height) >= 0, "#三 佐证:顶层 agent(...) 条目仍然可见(只折叠子层)");

            // ---- #三附加:Ctrl+O 切到详细态,子代理内层明细能看见 ----
            // 数据没丢(NewItem/FinalizeItem 不受 SubItemsExpanded() 影响),
            // 只是紧凑态默认不画;切到详细态之后再让子代理跑一次,这次该
            // 逐条铺屏了。
            SendKey(0x4F, 0, LEFT_CTRL_PRESSED);  // Ctrl+O('O' 的虚拟键码就是字符 'O' 0x4F)
            Sleep(300);
            SendText(
                "请你必须再调用一次内置的 agent 工具,委派一个子任务代理去完成:"
                "读取 D:\\lubancode\\src\\platform\\console_posix.cpp 这一个文件,"
                "找出 ClearScreen 函数定义在第几行,最后只回答行号。");
            SendKey(VK_RETURN, L'\r', 0);
            const bool agent2_started = WaitForText("agent(", 60000, height);
            if (!agent2_started) {
                Log("INFO: 详细态第二轮 60s 内没等到 agent( 工具调用,跳过这条附加校验");
            } else {
                const bool agent2_done =
                    WaitForText("\xE5\xAD\x90\xE4\xBB\xA3\xE7\x90\x86 ", 120000, height);  // "子代理 "
                Check(agent2_done, "#三附加 详细态子代理收尾:agent 条目摘要落定(120s 内)");
                if (agent2_done) {
                    Sleep(500);
                    int expanded_sub_rows = 0;
                    for (int row = 0; row < height; ++row) {
                        if (RowIsSubToolItem(ReadRow(row))) {
                            ++expanded_sub_rows;
                        }
                    }
                    Check(expanded_sub_rows > 0,
                          "#三附加 详细态(ctrl+o 之后):子工具条目明细逐条可见,实际 " +
                              std::to_string(expanded_sub_rows) + " 行");
                }
            }
        }
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
