// Ctrl+R 提问历史搜索的真控制台驱动器(实测问题 8:同一条提问列两遍):
// 与 agent_stream_driver 同一套手法——进程内起假 anthropic SSE 服务,真
// lubancode.exe 连上去跑真回合、真落盘,再按真 Ctrl+R 键刮屏断言搜索面
// 板"不重不漏"。不进 ctest,集成验证时手动跑:
//   history_search_driver <lubancode.exe 路径> <子进程工作目录> <报告文件路径> [full|noflush]
//
// full 模式(缺省,验收五景之"仅磁盘/完全重叠/落盘后不重/同文两次"):
//   一、发"一号提问"收答(回合收尾落盘);
//   二、发"二号提问"收答(落盘);Ctrl+R:面板恰好两条,各一次;
//   三、发"三号提问"收答(落盘),Ctrl+R:仍恰好三条(落盘后不变成两条);
//   四、同一句"原句重发一遍"真发两次(各收一次答),Ctrl+R:该句在面板
//       恰好两行(同文不同事件都留)。
// noflush 模式(验收五景之"仅内存"):.lubancode 造成一枚文件,建档必败,
// 提问只活在活历史——Ctrl+R 本会话范围显"没有命中"(没建上档没有场次
// id,口径如此),本项目范围恰显一条未落盘尾巴,sessions 目录一字未落。
// 注:不做"流式中开面板"一幕——流式监听线程不分派 Ctrl+R,轮内(未落盘)
// 的空闲路只有建档失败一种;其余"尾部部分重叠"景由单测钉
// (tests/unit/sessions/test_session_store.cpp 的 ExtractLivePromptTail)。
// 报告逐条 PASS/FAIL,退出码按失败数。env 全量指到假服务,不碰真网络。

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#include <cstdlib>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace {

HANDLE g_conin = INVALID_HANDLE_VALUE;
HANDLE g_conout = INVALID_HANDLE_VALUE;
std::ofstream g_report;
std::mutex g_log_mutex;
int g_failures = 0;

void Log(const std::string& line) {
    std::lock_guard<std::mutex> lock(g_log_mutex);
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
    const int len =
        WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
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

bool WaitForText(const std::string& needle, int timeout_ms) {
    const DWORD deadline = GetTickCount() + static_cast<DWORD>(timeout_ms);
    while (GetTickCount() < deadline) {
        if (FindLastRow(needle) >= 0) {
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

void SendCtrlR() {
    // Ctrl+R:VK_R + 控制字符 0x12 + LEFT_CTRL_PRESSED(console_win 的
    // "Ctrl+字母作和弦送出"认这个形状)。
    SendKey(0x52, L'\x12', LEFT_CTRL_PRESSED);
    Sleep(120);
}

void SendCtrlS() {
    SendKey(0x53, L'\x13', LEFT_CTRL_PRESSED);
    Sleep(300);
}

void SendCtrlC() {
    // 面板收起用 SearchCancel(Ctrl+C):Esc 是 SearchAccept——把选中条目取回
    // composer,会污染下一幕的输入。
    SendKey(0x43, L'\x03', LEFT_CTRL_PRESSED);
    Sleep(300);
}

// 搜索面板扫描:从最底一个"历史搜索"表头行起,收其下的结果行(❯ 或两
// 空格起头),撞上空行/横线/composer 即停。历史面板只画在底部 hint 区,
// 从最后一个表头数起,旧帧滚上去的残影不进来。
struct PanelScan {
    int header_row = -1;
    std::vector<std::string> rows;
};

PanelScan ScanSearchPanel() {
    PanelScan out;
    out.header_row = FindLastRow("历史搜索");
    if (out.header_row < 0) {
        return out;
    }
    for (int row = out.header_row + 1; row < out.header_row + 20; ++row) {
        const std::string text = ReadRow(row);
        if (text.empty()) {
            break;
        }
        if (text.rfind("❯ ", 0) == 0 || text.rfind("  ", 0) == 0) {
            out.rows.push_back(text);
        } else {
            break;  // 横线/composer/状态行:面板到头
        }
    }
    return out;
}

// 屏上的探针行(被测 exe 打到 stderr=控制台的诊断行)全部拓进报告。
void DumpProbeRows() {
    for (int row = 0; row < 400; ++row) {
        const std::string text = ReadRow(row);
        if (text.find("hsearch-probe") != std::string::npos) {
            Log("PROBE " + text);
        }
    }
}

// 子进程 stderr 文件(驱动器把 hStdError 指过去的)逐行归档(至多 80 行,
// 诊断之用:被测 exe 往 stderr 打什么,报告里就看得见什么)。
void ArchiveProbeFile(const std::wstring& path) {
    std::ifstream in(WideToUtf8(path), std::ios::binary);
    if (!in) {
        return;
    }
    std::string line;
    int archived = 0;
    while (std::getline(in, line) && archived < 80) {
        if (!line.empty()) {
            Log("STDERR " + line);
            ++archived;
        }
    }
}

// 现场拓片:表头下 8 行原样进报告(排查面板行形之用)。
void DumpPanelRows(const PanelScan& panel) {
    if (panel.header_row < 0) {
        Log("DUMP: header not found");
        return;
    }
    for (int row = panel.header_row; row < panel.header_row + 8; ++row) {
        Log("DUMP row" + std::to_string(row) + ": [" + ReadRow(row) + "]");
    }
}

int CountRowsWith(const PanelScan& panel, const std::string& needle) {
    int count = 0;
    for (const auto& row : panel.rows) {
        if (row.find(needle) != std::string::npos) {
            ++count;
        }
    }
    return count;
}

void SetEnv(const wchar_t* name, const std::wstring& value) {
    SetEnvironmentVariableW(name, value.c_str());
}

// ------------------------- 进程内假 anthropic 服务 -------------------------

using SOCKET_T = SOCKET;

void SendAll(SOCKET_T s, const std::string& data) {
    std::size_t sent = 0;
    while (sent < data.size()) {
        const int n = ::send(s, data.data() + sent, static_cast<int>(data.size() - sent), 0);
        if (n <= 0) {
            return;
        }
        sent += static_cast<std::size_t>(n);
    }
}

std::string DrainHttpRequest(SOCKET_T s) {
    std::string raw;
    char buf[4096];
    std::size_t header_end = std::string::npos;
    while (header_end == std::string::npos) {
        const int n = ::recv(s, buf, sizeof(buf), 0);
        if (n <= 0) {
            return raw;
        }
        raw.append(buf, static_cast<std::size_t>(n));
        header_end = raw.find("\r\n\r\n");
    }
    std::size_t content_length = 0;
    const std::string header = raw.substr(0, header_end);
    if (const std::size_t cl_pos = header.find("Content-Length"); cl_pos != std::string::npos) {
        content_length = static_cast<std::size_t>(atol(header.c_str() + cl_pos + 15));
    }
    const std::size_t total = header_end + 4 + content_length;
    while (raw.size() < total) {
        const int n = ::recv(s, buf, sizeof(buf), 0);
        if (n <= 0) {
            break;
        }
        raw.append(buf, static_cast<std::size_t>(n));
    }
    return raw;
}

std::string Sse(const std::string& json) { return "data: " + json + "\n\n"; }

void RespondSse(SOCKET_T s, const std::vector<std::string>& events) {
    std::string body;
    for (const auto& event : events) {
        body += Sse(event);
    }
    const std::string head = "HTTP/1.1 200 OK\r\n"
                             "Content-Type: text/event-stream\r\n"
                             "Content-Length: " +
                             std::to_string(body.size()) + "\r\n" + "Connection: close\r\n\r\n";
    SendAll(s, head + body);
}

std::string JsonEscape(const std::string& text) {
    std::string escaped;
    for (char c : text) {
        switch (c) {
            case '\\': escaped += "\\\\"; break;
            case '"': escaped += "\\\""; break;
            case '\n': escaped += "\\n"; break;
            case '\r': escaped += "\\r"; break;
            case '\t': escaped += "\\t"; break;
            default: escaped += c; break;
        }
    }
    return escaped;
}

std::string TextDeltaEvent(const std::string& text) {
    return "{\"type\":\"content_block_delta\",\"index\":0,\"delta\":{\"type\":\"text_delta\",\"text\":\"" +
           JsonEscape(text) + "\"}}";
}

std::vector<std::string> TextTurn(const std::string& text) {
    return {
        "{\"type\":\"message_start\",\"message\":{\"id\":\"msg\",\"model\":\"fake-model\"}}",
        "{\"type\":\"content_block_start\",\"index\":0,\"content_block\":{\"type\":\"text\",\"text\":\"\"}}",
        TextDeltaEvent(text),
        "{\"type\":\"content_block_stop\",\"index\":0}",
        "{\"type\":\"message_delta\",\"delta\":{\"stop_reason\":\"end_turn\"},\"usage\":{\"output_tokens\":8}}",
        "{\"type\":\"message_stop\"}",
    };
}

// 五幕的锚文本(提问 / 回答标记:回答标记只在屏幕出现一次,作回合收口门闩)。
const char* kPrompt1 = "一号提问讲讲代理";
const char* kReply1 = "答一号完毕";
const char* kPrompt2 = "二号提问讲讲压缩";
const char* kReply2 = "答二号完毕";
const char* kPrompt3 = "三号提问讲讲落盘";
const char* kReply3 = "答三号完毕";
const char* kPromptRepeat = "原句重发一遍";
const char* kReplyRepeatFirst = "答重发甲完毕";
const char* kReplyRepeatSecond = "答重发乙完毕";

u_short StartFakeServer() {
    WSADATA wsa{};
    WSAStartup(MAKEWORD(2, 2), &wsa);
    const SOCKET_T listener = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    int yes = 1;
    setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&yes), sizeof(yes));
    sockaddr_in bind_addr{};
    bind_addr.sin_family = AF_INET;
    bind_addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    bind_addr.sin_port = 0;
    if (::bind(listener, reinterpret_cast<const sockaddr*>(&bind_addr), sizeof(bind_addr)) != 0 ||
        ::listen(listener, 16) != 0) {
        Log("FAIL: fake server bind/listen");
        return 0;
    }
    int len = sizeof(bind_addr);
    getsockname(listener, reinterpret_cast<sockaddr*>(&bind_addr), &len);
    const u_short port = ntohs(bind_addr.sin_port);

    const auto serve_connection = [](SOCKET_T client_fd) {
        const std::string body = DrainHttpRequest(client_fd);
        // 最新一条真用户话(历史旧消息会随每次请求重复出现,只认尾部区间)。
        const std::size_t last_user_at = body.rfind("\"role\":\"user\"");
        const std::size_t last_assistant_at = body.rfind("\"role\":\"assistant\"");
        const std::size_t newest_from = last_assistant_at == std::string::npos ? 0 : last_assistant_at;
        const std::size_t newest_to = last_user_at == std::string::npos ? body.size() : last_user_at;
        const std::string newest_user_text =
            newest_from < newest_to ? body.substr(newest_from, newest_to - newest_from) : std::string();
        const auto newest_has = [&newest_user_text](const char* needle) {
            return newest_user_text.find(needle) != std::string::npos;
        };
        // 会话起名/记忆抽取等旁路请求:回个 ok,不掺和剧本。重发幕不数
        // 连接(旁路请求会夹在中间),按"历史里有没有甲答"判第一/第二遍,
        // 幂等,谁先谁后都不吃错剧本。
        if (newest_has(kPrompt3)) {
            Sleep(400);  // 稍压一拍,让"回合进行中"的画面站稳再收口
            RespondSse(client_fd, TextTurn(kReply3));
        } else if (newest_has(kPrompt1)) {
            RespondSse(client_fd, TextTurn(kReply1));
        } else if (newest_has(kPrompt2)) {
            RespondSse(client_fd, TextTurn(kReply2));
        } else if (newest_has(kPromptRepeat)) {
            const bool answered_first = body.find(kReplyRepeatFirst) != std::string::npos;
            RespondSse(client_fd, TextTurn(answered_first ? kReplyRepeatSecond : kReplyRepeatFirst));
        } else {
            RespondSse(client_fd, TextTurn("ok"));
        }
        shutdown(client_fd, SD_SEND);
        {
            char drain[512];
            for (int i = 0; i < 64; ++i) {
                const int n = ::recv(client_fd, drain, sizeof(drain), 0);
                if (n <= 0) {
                    break;
                }
            }
        }
        closesocket(client_fd);
    };
    std::thread([listener, serve_connection]() {
        while (true) {
            sockaddr_in client{};
            int client_len = sizeof(client);
            const SOCKET_T client_fd =
                ::accept(listener, reinterpret_cast<sockaddr*>(&client), &client_len);
            if (client_fd == INVALID_SOCKET) {
                return;
            }
            std::thread(serve_connection, client_fd).detach();
        }
    }).detach();
    return port;
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    if (argc < 4) {
        std::fprintf(stderr, "usage: history_search_driver <lubancode.exe> <workdir> <report> [full|noflush]\n");
        return 2;
    }
    const std::wstring exe_path = argv[1];
    const std::wstring workdir = argv[2];
    const bool noflush = argc >= 5 && std::wstring(argv[4]) == L"noflush";
    g_report.open(WideToUtf8(argv[3]), std::ios::binary | std::ios::trunc);
    if (!g_report) {
        return 2;
    }
    CreateDirectoryW(workdir.c_str(), nullptr);

    const u_short port = StartFakeServer();
    if (port == 0) {
        return 2;
    }
    Log("fake anthropic server on 127.0.0.1:" + std::to_string(port));

    // 隔离家底:USERPROFILE 指到本进程私目录,sessions 落在这底下,验收
    // 后现场核档。noflush 模式把 .lubancode 造成一枚**文件**——建档必然
    // 失败(session_store_broken),提问只活在内存里,验"仅内存尾巴"一景。
    std::wstring hermetic_home;
    {
        wchar_t workdir_buf[MAX_PATH]{};
        GetCurrentDirectoryW(MAX_PATH, workdir_buf);
        hermetic_home = std::wstring(workdir_buf) + L"\\hsearch_home_" + std::to_wstring(GetCurrentProcessId());
        CreateDirectoryW(hermetic_home.c_str(), nullptr);
        if (noflush) {
            const HANDLE block = CreateFileW((hermetic_home + L"\\.lubancode").c_str(), GENERIC_WRITE, 0, nullptr,
                                             CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (block != INVALID_HANDLE_VALUE) {
                CloseHandle(block);
            }
        } else {
            CreateDirectoryW((hermetic_home + L"\\.lubancode").c_str(), nullptr);
            CreateDirectoryW((hermetic_home + L"\\.lubancode\\sessions").c_str(), nullptr);
        }
        SetEnv(L"USERPROFILE", hermetic_home);
    }
    SetEnv(L"LUBANCODE_WIRE", L"anthropic");
    SetEnv(L"LUBANCODE_BASE_URL", L"http://127.0.0.1:" + std::to_wstring(port));
    SetEnv(L"LUBANCODE_API_KEY", L"history-search-driver");
    SetEnv(L"LUBANCODE_MODEL", L"fake-model");
    SetEnv(L"ANTHROPIC_BASE_URL", L"");
    SetEnv(L"ANTHROPIC_AUTH_TOKEN", L"");
    SetEnv(L"ANTHROPIC_MODEL", L"");
    SetEnv(L"NO_PROXY", L"127.0.0.1,localhost");
    SetEnv(L"http_proxy", L"");
    SetEnv(L"https_proxy", L"");

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
    SetConsoleScreenBufferSize(g_conout, COORD{120, 400});
    FlushConsoleInputBuffer(g_conin);

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = g_conin;
    si.hStdOutput = g_conout;
    // stderr 单走文件:被测 exe 的诊断探针(如 [hsearch-probe])不搅画面,
    // 收场后原样归档到报告。
    SECURITY_ATTRIBUTES inheritable_file{};
    inheritable_file.nLength = sizeof(inheritable_file);
    inheritable_file.bInheritHandle = TRUE;
    const std::wstring probe_path = hermetic_home + L"\\probe_stderr.txt";
    HANDLE probe_file = CreateFileW(probe_path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, &inheritable_file,
                                    CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    si.hStdError = probe_file != INVALID_HANDLE_VALUE ? probe_file : g_conout;
    PROCESS_INFORMATION pi{};
    std::wstring cmdline = L"\"" + exe_path + L"\" --yes";
    if (!CreateProcessW(exe_path.c_str(), cmdline.data(), nullptr, nullptr, TRUE, 0, nullptr, workdir.c_str(), &si,
                        &pi)) {
        Log("FAIL: CreateProcess " + std::to_string(GetLastError()));
        return 1;
    }
    CloseHandle(pi.hThread);

    Check(WaitForText("fake-model", 30000), "开场:状态栏出现(程序活着)");
    Sleep(500);

    if (noflush) {
        // ---- 仅内存一景:建档失败,提问只活在活历史里 ----
        SendText(kPrompt1);
        SendKey(VK_RETURN, L'\r', 0);
        Check(WaitForText(kReply1, 30000), "仅内存:答一号上屏");
        Sleep(800);
        SendCtrlR();
        Check(WaitForText("历史搜索 [本会话]", 10000), "仅内存:面板打开");
        {
            // 没建上档:current_session_id 空,本会话范围按口径显"没有命中"。
            const PanelScan panel = ScanSearchPanel();
            Check(panel.header_row >= 0 && CountRowsWith(panel, "没有命中") == 1,
                  "仅内存:未建档的本会话范围显没有命中(got " + std::to_string(panel.rows.size()) + ")");
        }
        SendCtrlS();  // -> 本项目
        {
            const PanelScan panel = ScanSearchPanel();
            Check(panel.header_row >= 0 && panel.rows.size() == 1,
                  "仅内存:本项目范围恰显一条尾巴(got " + std::to_string(panel.rows.size()) + ")");
            Check(CountRowsWith(panel, kPrompt1) == 1, "仅内存:未落盘的一号恰一次");
        }
        SendCtrlC();
        Sleep(400);
        // 档上确实什么都没落。
        {
            WIN32_FIND_DATAW fd{};
            const HANDLE find = FindFirstFileW((hermetic_home + L"\\.lubancode\\sessions\\*.jsonl").c_str(), &fd);
            Check(find == INVALID_HANDLE_VALUE, "仅内存:sessions 里没有档(一字未落)");
            if (find != INVALID_HANDLE_VALUE) {
                FindClose(find);
            }
        }
        SendText("/exit");
        SendKey(VK_RETURN, L'\r', 0);
        if (WaitForSingleObject(pi.hProcess, 10000) != WAIT_OBJECT_0) {
            TerminateProcess(pi.hProcess, 0);
        }
        CloseHandle(pi.hProcess);
        Log(std::string(g_failures == 0 ? "ALL PASS" : "FAILURES: ") + std::to_string(g_failures));
        if (probe_file != INVALID_HANDLE_VALUE) {
            CloseHandle(probe_file);
        }
        ArchiveProbeFile(probe_path);
        FreeConsole();
        return g_failures == 0 ? 0 : 1;
    }

    // ---- 一、二号:各发一问各收一答,回合收尾即落盘 ----
    SendText(kPrompt1);
    SendKey(VK_RETURN, L'\r', 0);
    Check(WaitForText(kReply1, 30000), "一:答一号上屏(回合收尾)");
    Sleep(800);  // 落盘 + 起名旁路收口

    SendText(kPrompt2);
    SendKey(VK_RETURN, L'\r', 0);
    Check(WaitForText(kReply2, 30000), "二:答二号上屏");
    Sleep(800);

    // ---- 三:两问已落盘,Ctrl+R 只显两条,各一次 ----
    SendCtrlR();
    Check(WaitForText("历史搜索 [本会话]", 10000), "三:面板打开");
    {
        const PanelScan panel = ScanSearchPanel();
        DumpPanelRows(panel);
        // 分层探针:本会话空就换本项目再扫一枪,分辨"场次 id 对不上"与
        // "数据集整空"。
        if (panel.rows.size() != 2) {
            SendCtrlS();  // -> 本项目
            const PanelScan project_panel = ScanSearchPanel();
            Log("PROBE project rows=" + std::to_string(project_panel.rows.size()));
            DumpPanelRows(project_panel);
            SendCtrlS();  // -> 全部
            const PanelScan all_panel = ScanSearchPanel();
            Log("PROBE all rows=" + std::to_string(all_panel.rows.size()));
            DumpPanelRows(all_panel);
            SendCtrlS();  // 回本会话
        }
        Check(panel.header_row >= 0 && panel.rows.size() == 2,
              "三:两条已落盘提问只显两条(got " + std::to_string(panel.rows.size()) + ")");
        Check(CountRowsWith(panel, kPrompt2) == 1, "三:二号恰一次");
        Check(CountRowsWith(panel, kPrompt1) == 1, "三:一号恰一次");
        if (!panel.rows.empty()) {
            Check(panel.rows[0].find(kPrompt2) != std::string::npos, "三:最新(二号)在最上");
        }
    }
    SendCtrlC();
    Sleep(400);

    // 注:不做"流式中开面板"一幕——流式监听线程只分派 Esc/Shift+Tab/方向键
    // 与正文,不分派 Ctrl+R,轮内(未落盘)开不了搜索面板;未落盘尾巴的
    // 交互可达路只剩"建档失败"(见 noflush 模式),其余由单测钉。

    // ---- 四:三号发出去收答落盘;再 Ctrl+R:仍三条,不多一条 ----
    SendText(kPrompt3);
    SendKey(VK_RETURN, L'\r', 0);
    Check(WaitForText(kReply3, 30000), "四:答三号上屏(落盘)");
    Sleep(800);
    SendCtrlR();
    Check(WaitForText("历史搜索 [本会话]", 10000), "四:面板再开");
    {
        const PanelScan panel = ScanSearchPanel();
        Check(panel.header_row >= 0 && panel.rows.size() == 3,
              "四:三号也落盘后仍恰好三条(got " + std::to_string(panel.rows.size()) + ")");
        Check(CountRowsWith(panel, kPrompt3) == 1, "四:三号恰一次");
    }
    SendCtrlC();
    Sleep(400);

    // ---- 六:同一句真发两次,两条都留 ----
    SendText(kPromptRepeat);
    SendKey(VK_RETURN, L'\r', 0);
    Check(WaitForText(kReplyRepeatFirst, 30000), "六:重发甲答上屏");
    Sleep(800);
    SendText(kPromptRepeat);
    SendKey(VK_RETURN, L'\r', 0);
    Check(WaitForText(kReplyRepeatSecond, 30000), "六:重发乙答上屏");
    Sleep(800);
    SendCtrlR();
    Check(WaitForText("历史搜索 [本会话]", 10000), "六:面板三开");
    {
        const PanelScan panel = ScanSearchPanel();
        Check(panel.header_row >= 0 && panel.rows.size() == 5,
              "六:全场五条提问恰显五行(got " + std::to_string(panel.rows.size()) + ")");
        Check(CountRowsWith(panel, kPromptRepeat) == 2, "六:同文两次真发恰两行");
    }
    SendCtrlC();
    Sleep(400);

    // ---- 收场:核档(sessions 下恰一场,档上用户提问行数与面板口径对齐)----
    {
        std::wstring find_pattern = hermetic_home + L"\\.lubancode\\sessions\\*.jsonl";
        WIN32_FIND_DATAW fd{};
        const HANDLE find = FindFirstFileW(find_pattern.c_str(), &fd);
        int files = 0;
        std::wstring first_file;
        if (find != INVALID_HANDLE_VALUE) {
            do {
                ++files;
                if (first_file.empty()) {
                    first_file = fd.cFileName;
                }
            } while (FindNextFileW(find, &fd));
            FindClose(find);
        }
        Check(files == 1, "档:一场会话一个文件(got " + std::to_string(files) + ")");
    }

    SendText("/exit");
    SendKey(VK_RETURN, L'\r', 0);
    if (WaitForSingleObject(pi.hProcess, 10000) != WAIT_OBJECT_0) {
        TerminateProcess(pi.hProcess, 0);
    }
    CloseHandle(pi.hProcess);

    Log(std::string(g_failures == 0 ? "ALL PASS" : "FAILURES: ") + std::to_string(g_failures));
    DumpProbeRows();
    if (probe_file != INVALID_HANDLE_VALUE) {
        CloseHandle(probe_file);
    }
    ArchiveProbeFile(probe_path);
    FreeConsole();
    return g_failures == 0 ? 0 : 1;
}
