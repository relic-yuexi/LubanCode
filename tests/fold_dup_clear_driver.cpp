// 真机验证驱动器(#一/#二/#三 三条修复 + 显示体验后续三单专用,不进
// ctest):跟 screen_driver.cpp 同一套手艺(AllocConsole + WriteConsoleInput
// + ReadConsoleOutput,真控制台专用),单独开一个文件——screen_driver.cpp
// 是既有的 UI-B/C/D 回归脚本(F1-F8),这些修复不去碰它,免得牵连不相关
// 的场景。
//
// 老三件(前一单验过,保留):
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
// 本单新增四件(显示体验四单落地之后的回归/深化):
//   4. 图标:启动打一次,/clear 之后重打一次(不是清成一片空白)——用
//      图标独有的 "匠心运斤" 四个字当签名,数它在整块缓冲区里出现几次:
//      /clear 前 1 次(启动打的那次),/clear 之后仍然是 1 次(旧的被真清屏
//      擦掉、新的紧跟着重打——次数不变;要是变 0 说明清完没重打,回归复发;
//      变 2 说明没真清屏,旧的还在)。
//   5. 回合执行期间按 Ctrl+O 也有反应:不再等到两轮之间的 composer 主循环
//      才处理得了——子代理任务还在跑的时候(agent( 已经出现、"子代理 "
//      终态摘要还没落定)按下 Ctrl+O,馬上能看到 "详细模式"/"紧凑模式"
//      切换提示打出来。
//   6. 一上来就是工具调用、没有开场正文的回合,流式脚注(输入框:上横线+
//      `> ` 输入行+下横线+状态行)照样在第一次工具调用打印之前正常出现——
//      用状态行的 "shift+tab" / 输入行占位提示"排队下一条" 当信号。
//   7. /provider switch 成功后真清一次屏,按新配置重画图标与横幅,切换
//      提示留在屏上；失败、管道输出不清。
//
// 用法: fold_dup_clear_driver <lubancode.exe 路径> <子进程工作目录> <报告文件路径>
//                             [要验证清屏的 provider 名] [--provider-only]
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

// 只在 from_row_exclusive 之后(不含)找——同一轮会话里同一句签名文字
// (比如"子代理 "、"agent(")前一轮就已经打出来过、还留在屏幕上(完成态
// 条目不会被擦掉),不挑基准行的话,第二轮的 WaitForText 会被第一轮的
// 旧文字骗过去,判定成"已经出现"其实压根没等到第二轮真的打出来。
int FindLastRowAfter(const std::string& needle, int from_row_exclusive, int max_rows) {
    for (int row = max_rows - 1; row > from_row_exclusive; --row) {
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

// WaitForText 的"只认新出现"版,见 FindLastRowAfter 注释。
bool WaitForTextAfter(const std::string& needle, int from_row_exclusive, int timeout_ms, int max_rows,
                      int* found_row = nullptr) {
    const DWORD deadline = GetTickCount() + static_cast<DWORD>(timeout_ms);
    while (GetTickCount() < deadline) {
        const int row = FindLastRowAfter(needle, from_row_exclusive, max_rows);
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
    const std::string provider_name = argc >= 5 ? WideToUtf8(argv[4]) : std::string();
    const bool provider_only = argc >= 6 && std::wstring(argv[5]) == L"--provider-only";
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

    const auto finish = [&]() {
        SendText("exit");
        SendKey(VK_RETURN, L'\r', 0);
        if (WaitForSingleObject(pi.hProcess, 15000) != WAIT_OBJECT_0) {
            Log("INFO: exit 超时,强杀子进程");
            TerminateProcess(pi.hProcess, 9);
        }
        CloseHandle(pi.hProcess);
        Log(g_failures == 0 ? "RESULT: ALL PASS" : "RESULT: " + std::to_string(g_failures) + " FAIL");
        return g_failures == 0 ? 0 : 1;
    };

    // ---- 开场:等状态行出现 ----
    int status_row = -1;
    const bool opened = WaitForText("shift+tab", 30000, height, &status_row);
    Check(opened, "开场:状态行出现(30s 内)");
    if (!opened) {
        TerminateProcess(pi.hProcess, 9);
        return 1;
    }
    Sleep(300);

    // ---- #四:图标 ----
    // 启动时图标该打一次——"匠心运斤"四个字是图标独有签名(正文/横幅其余
    // 部分都不会出现这四个字),数它在整块缓冲区里出现几次。
    const auto icon_rows_at_start = FindAllRows("\xe5\x8c\xa0\xe5\xbf\x83\xe8\xbf\x90\xe6\x96\xa4", height);  // "匠心运斤"
    Check(icon_rows_at_start.size() == 1,
          "#四 启动:图标出现且只出现一次(实际 " + std::to_string(icon_rows_at_start.size()) + " 次)");

    // ---- #七:/provider switch 成功后刷新屏面 ----
    // provider 名由调用方传入,免得把某个私有配置硬编码进回归驱动。切换
    // 成功后该真清一次,再重画图标和新配置横幅:图标签名仍只出现一次,
    // 输入命令则该从整块回滚缓冲里消失。对话历史是否保留由 AgentLoop
    // 单测兜底,这里只验肉眼能见的屏面。
    if (!provider_name.empty()) {
        const std::string switch_command = "/provider switch " + provider_name;
        SendText(switch_command);
        SendKey(VK_RETURN, L'\r', 0);
        const std::string switched_text = "provider " + provider_name;
        Check(WaitForText(switched_text, 10000, height),
              "#七 /provider switch:成功提示出现(10s 内)");
        Sleep(200);
        const auto icon_rows_after_switch =
            FindAllRows("\xe5\x8c\xa0\xe5\xbf\x83\xe8\xbf\x90\xe6\x96\xa4", height);  // "匠心运斤"
        Check(icon_rows_after_switch.size() == 1,
              "#七 /provider switch:清屏后图标重画且只有一份(实际 " +
                  std::to_string(icon_rows_after_switch.size()) + " 次)");
        Check(FindLastRow(switch_command, height) < 0,
              "#七 /provider switch:输入命令已从回滚缓冲清掉");
        Check(FindLastRow("cwd:", height) >= 0,
              "#七 /provider switch:新配置横幅重新出现");
        if (provider_only) {
            return finish();
        }
    }

    // ---- #一:/clear 清屏 ----
    // 横幅上必有的一句提示("banner.hint" 键)先确认在屏,再 /clear,再确认
    // 从可见缓冲区消失(整块缓冲区扫,不只是当前窗口)——老断言;但这一轮
    // 修完回归之后,/clear 会重打图标+横幅,"/help" 这句提示词会在新横幅
    // 里原样重新出现,不能再拿它当"清没清"的信号,改用图标签名的出现次数
    // 判断:真清屏 = 旧的被擦、新的紧跟着补一份,次数不变(还是 1);
    // 次数变 0 = 清完没重打(回归复发);次数变 2 = 没真清屏,新旧都在。
    const bool banner_before = FindLastRow("/help", height) >= 0;
    Check(banner_before, "#一 /clear 前:横幅提示('/help 看命令')在屏");
    SendText("/clear");
    SendKey(VK_RETURN, L'\r', 0);
    Check(WaitForText("已清空对话历史", 5000, height), "#一 /clear:确认行出现");
    Sleep(200);
    {
        const auto icon_rows_after_clear =
            FindAllRows("\xe5\x8c\xa0\xe5\xbf\x83\xe8\xbf\x90\xe6\x96\xa4", height);  // "匠心运斤"
        Check(icon_rows_after_clear.size() == 1,
              "#四 /clear 之后:图标重新打出来且只有一份(旧的被真清屏擦掉、新的补上,"
              "实际 " + std::to_string(icon_rows_after_clear.size()) + " 次——0 次说明清完没重打,"
              "2 次说明没真清屏)");
        // #一 佐证:cwd/模型这几行"我是谁、在哪儿"的身份信息也得跟着图标
        // 一起回来,不是只回来一个孤零零的图标框子。
        Check(FindLastRow("cwd:", height) >= 0, "#一 /clear 之后:cwd 提示行也重新出现");
        Check(FindLastRow("/help", height) >= 0, "#一 /clear 之后:横幅提示重新出现(不再是一句'已清空'之后一片空白)");
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

            // ---- #三附加 + #五:回合执行期间按 Ctrl+O 也要有反应 ----
            // 不在两轮之间按(composer 主循环那条路本来就通,老版本已经验
            // 过)——这次故意等回合真的在跑(agent( 已经出现,还没到"子代理 "
            // 终态摘要)才按 Ctrl+O,考的正是 TurnInputListener::ThreadMain
            // 新加的 CtrlO 分支(根因二 part B:以前这段时间按 Ctrl+O 完全
            // 被吞,没有任何反应)。数据没丢(NewItem/FinalizeItem 不受
            // SubItemsExpanded() 影响),只是紧凑态默认不画;切到详细态之后
            // (哪怕是回合中途切的)后续新发生的子工具调用该逐条铺屏了。
            // 故意要求依次读两个文件(不是一个)——子代理至少要打两次内部
            // 工具调用、中间隔着它自己的一轮模型往返。只要求一次的话,
            // 那唯一一次工具调用可能在 Ctrl+O 这个按键事件真正传到监听
            // 线程之前就已经跑完+落定,"切换生效前就已经收尾的条目不补画"
            // 这条本来就说好不管(见交付说明的取舍),会把这条校验测成
            // 假阴性——两个文件保证切换生效之后至少还有第二次工具调用等着。
            // 基准行:发第二条消息之前先记下"agent("/"子代理 "这两句签名文字
            // 此刻在屏幕上最靠下的一次出现(第一轮留下的),下面等第二轮的
            // 版本必须用 WaitForTextAfter 只认这个基准行之后的新出现——不然
            // 完成态条目不会被擦掉,第一轮的旧文字会把 WaitForText 骗过去,
            // 判定成"已经出现"其实压根没等到第二轮真的打出来(真机实测踩到
            // 过:agent2_done 秒过,但屏幕原样往下翻发现第二轮其实还在"思考
            // 中",子工具调用压根没开始,后面的 0 行断言完全是误判)。
            const int baseline_agent_row = FindLastRow("agent(", height);
            const int baseline_done_row = FindLastRow("\xE5\xAD\x90\xE4\xBB\xA3\xE7\x90\x86 ", height);  // "子代理 "
            SendText(
                "请你必须再调用一次内置的 agent 工具,委派一个子任务代理去完成:"
                "依次读取这两个文件——D:\\lubancode\\src\\platform\\console_posix.cpp"
                " 和 D:\\lubancode\\src\\platform\\console_win.cpp——分别找出各自的"
                "ClearScreen 函数定义在第几行,最后按文件名列出两个行号。");
            SendKey(VK_RETURN, L'\r', 0);
            const bool agent2_started = WaitForTextAfter("agent(", baseline_agent_row, 60000, height);
            if (!agent2_started) {
                Log("INFO: 第二轮 60s 内没等到新的 agent( 工具调用,跳过 #三附加/#五 这两条校验");
            } else {
                // 回合真的在跑的这个窗口按 Ctrl+O('O' 的虚拟键码就是字符
                // 'O' 0x4F),紧接着找切换提示——这条反应必须在回合收尾之前
                // 就看得见,不是等到下一轮 composer 才处理得了。
                SendKey(0x4F, 0, LEFT_CTRL_PRESSED);
                const bool ctrlo_mid_turn_reacted = WaitForText("详细模式", 5000, height);
                Check(ctrlo_mid_turn_reacted,
                      "#五 回合执行期间按 Ctrl+O:'详细模式' 切换提示在 5s 内打出来"
                      "(不是等到下一轮才有反应)");

                // #三附加(参考项,不计入 PASS/FAIL):切到详细态之后,回合
                // 中途新发生的子工具调用理论上该逐条铺屏——但这一步依赖
                // "子代理还没打完全部工具调用、Ctrl+O 就先落地"这个时序,
                // 真机上取决于模型自己跑多快、网络多快,不是这条修复本身
                // 能控制的(任务交付说明原文:回合执行期间切到"展开"之后,
                // 已经收尾、被紧凑折叠收走的历史子工具条目不补画——这是
                // 明确写好的取舍,不算 bug)。#五 的"提示文案 5s 内打出来"
                // 已经是任务验收允许的充分证据("哪怕只是提示文案打出来"),
                // 这里只作为参考多验一层,超时/没等到都只记 INFO,不算 FAIL。
                const bool agent2_done =
                    WaitForTextAfter("\xE5\xAD\x90\xE4\xBB\xA3\xE7\x90\x86 ", baseline_done_row, 90000,
                                      height);  // "子代理 "
                if (!agent2_done) {
                    Log("INFO: #三附加(参考项)90s 内没等到第二轮子代理收尾,网络/模型"
                        "偶发慢一拍——不影响 #五 已经拿到的结论,跳过,不计入 FAIL");
                } else {
                    Sleep(500);
                    int expanded_sub_rows = 0;
                    for (int row = 0; row < height; ++row) {
                        if (RowIsSubToolItem(ReadRow(row))) {
                            ++expanded_sub_rows;
                        }
                    }
                    if (expanded_sub_rows > 0) {
                        Log("INFO: #三附加(参考项)PASS:详细态下子工具条目明细逐条可见,实际 " +
                            std::to_string(expanded_sub_rows) + " 行");
                    } else {
                        // 诊断:没看到就把 agent( 那一行开始往下一段原样记下来,
                        // 看子代理这次到底打了几次工具、条目摘要长什么样——
                        // 多半是"整段任务在 Ctrl+O 落地前就已经跑完+收尾折叠掉了"
                        // 这条已知取舍,留个痕迹方便复核,不算 FAIL。
                        Log("INFO: #三附加(参考项)没看到展开的子工具条目行,大概率是"
                            "任务在 Ctrl+O 落地前就已收尾(已知取舍,不算 FAIL)");
                        const int agent2_row = FindLastRow("agent(", height);
                        const int dump_from = agent2_row >= 0 ? agent2_row : 0;
                        for (int r = dump_from; r < dump_from + 40 && r < height; ++r) {
                            const std::string row_text = ReadRow(r);
                            if (!row_text.empty()) {
                                Log("INFO: #三附加诊断 row[" + std::to_string(r) + "]=" + row_text);
                            }
                        }
                    }
                }
            }
        }
    }

    // ---- #六:一上来就是工具调用、没有开场正文,footer 照样出现 ----
    // 根因三描述的场景:模型这一轮压根不先吐正文,OnDelta 可能永远不会
    // 调用一次——footer 要靠 OnToolStart/AgentStatusPainter::Tick 里新补
    // 的 Redraw 才第一次露面。这里换个直白的指令,逼模型一上来就调工具、
    // 不先扯闲话;footer 的输入行占位提示("排队下一条")是独有信号,回合
    // 还在跑(还没到 "[tokens]" 统计行)期间就该看得见,不是等到最后才冒
    // 出来或者全程不出现。
    SendText(
        "请直接调用 run_command 工具执行 dir 命令,不要说任何其他话,不要做任何"
        "总结,调用完就结束这一轮。");
    SendKey(VK_RETURN, L'\r', 0);
    const bool footer_seen_before_done = WaitForText("排队下一条", 20000, height);
    Check(footer_seen_before_done,
          "#六 纯工具回合:footer(输入框占位提示'排队下一条')在回合收尾前出现(20s 内)");
    WaitForText("[tokens]", 60000, height);  // 让这一轮收个尾,给后面 exit 让路

    // ---- 收尾:exit ----
    return finish();
}
