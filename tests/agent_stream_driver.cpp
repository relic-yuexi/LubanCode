// "真前台任务 + 真流式监听"专用刮屏驱动器:与 agent_panel_driver 的差别
// ——那只验空闲画面(假 provider),这只把真 lubancode.exe 连上进程内的
// 假 anthropic 服务,跑一条真的前台 agent 工具调用,流式期间按 Up/Down/
// Enter/Esc 逐帧断言代理导航坞:贴底层级(状态栏之下)、残帧计数(提示/
// main/title 至多一份)、Ctrl+C 有字先清字。不进 ctest,集成验证时手动跑:
//   agent_stream_driver <lubancode.exe 路径> <子进程工作目录> <报告文件路径>
//
// 剧本(请求按到达次序,假服务一次只收一条连接):
//   1. 主模型:tool_use agent{title:"项目记忆升级一期", prompt:"你在一个
//      C++ 项目的隔离 git worktree 里实施项目记忆系统升级……", run_in_background:false}
//   2. 子代理:tool_use run_command(ping -n 8 127.0.0.1)——真工具、真耗时
//      (~7s),坞的 Running 灯/工时/工具计数有东西可画;
//   3. 子代理:文本结论"子代理干完了:检索阈值回归全绿";
//   4. 主模型:文本收尾"主代理汇总完毕"。
// 子进程用 --yes 起跑(run_command 不弹确认),env 全量指到假服务
// (LUBANCODE_WIRE/BASE_URL/API_KEY/MODEL + NO_PROXY),不碰真网络。

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <winsock2.h>
#include <ws2tcpip.h>

#include <atomic>
#include <cstdlib>
#include <fstream>
#include <functional>
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

// 残帧计数(规格"测试"四):数遍整屏,不认"最后一次找到"。
int CountRowsWith(const std::string& needle, int max_rows = 400) {
    int count = 0;
    for (int row = 0; row < max_rows; ++row) {
        if (ReadRow(row).find(needle) != std::string::npos) {
            ++count;
        }
    }
    return count;
}

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

// 按结构认 footer 的 composer 框(流式期间最底下那个):上横线(r)/ '>' 起的
// 输入行(r+1)/ 下横线(r+2)/ 非横线状态行(r+3)。
int FindFooterInputRow(int max_rows = 400) {
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
        if (text.find("general-purpose") != std::string::npos) {
            return false;
        }
        if (text.find("\xe2\x97\x8f main") != std::string::npos) {
            return false;
        }
    }
    return true;
}

// ------------------------- 进程内假 anthropic 服务 -------------------------

using SOCKET_T = SOCKET;
constexpr SOCKET_T kBadSocket = INVALID_SOCKET;

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

// 读掉一条 HTTP 请求(头 + Content-Length 定长的体),返回体内容(给剧本
// 判断用,这里其实只按连接次序派发,不看体)。
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

std::string SseBody(const std::vector<std::string>& events) {
    std::string body;
    for (const auto& event : events) {
        body += Sse(event);
    }
    return body;
}

void RespondSse(SOCKET_T s, const std::vector<std::string>& events) {
    const std::string body = SseBody(events);
    const std::string head = "HTTP/1.1 200 OK\r\n"
                             "Content-Type: text/event-stream\r\n"
                             "Content-Length: " +
                             std::to_string(body.size()) + "\r\n" + "Connection: close\r\n\r\n";
    SendAll(s, head + body);
}

std::vector<std::string> TextTurn(const std::string& text) {
    return {
        "{\"type\":\"message_start\",\"message\":{\"id\":\"msg\",\"model\":\"fake-model\"}}",
        "{\"type\":\"content_block_start\",\"index\":0,\"content_block\":{\"type\":\"text\",\"text\":\"\"}}",
        "{\"type\":\"content_block_delta\",\"index\":0,\"delta\":{\"type\":\"text_delta\",\"text\":\"" + text +
            "\"}}",
        "{\"type\":\"content_block_stop\",\"index\":0}",
        "{\"type\":\"message_delta\",\"delta\":{\"stop_reason\":\"end_turn\"},\"usage\":{\"input_tokens\":120,"
        "\"output_tokens\":30}}",
    };
}

std::vector<std::string> ToolUseTurn(const std::string& tool_id, const std::string& name,
                                     const std::string& input_json) {
    // partial_json 按协议是字符串:整份入参 JSON 转义后塞进去。
    std::string escaped;
    for (char c : input_json) {
        switch (c) {
            case '\\':
                escaped += "\\\\";
                break;
            case '"':
                escaped += "\\\"";
                break;
            default:
                escaped += c;
                break;
        }
    }
    return {
        "{\"type\":\"message_start\",\"message\":{\"id\":\"msg\",\"model\":\"fake-model\"}}",
        "{\"type\":\"content_block_start\",\"index\":0,\"content_block\":{\"type\":\"tool_use\",\"id\":\"" +
            tool_id + "\",\"name\":\"" + name + "\"}}",
        "{\"type\":\"content_block_delta\",\"index\":0,\"delta\":{\"type\":\"input_json_delta\",\"partial_json\":\"" +
            escaped + "\"}}",
        "{\"type\":\"content_block_stop\",\"index\":0}",
        "{\"type\":\"message_delta\",\"delta\":{\"stop_reason\":\"tool_use\"},\"usage\":{\"input_tokens\":100,"
        "\"output_tokens\":20}}",
    };
}

const char* kTitle = "\xe9\xa1\xb9\xe7\x9b\xae\xe8\xae\xb0\xe5\xbf\x86\xe5\x8d\x87\xe7\xba\xa7\xe4\xb8\x80\xe6\x9c\x9f";  // 项目记忆升级一期
const char* kPromptHead =
    "\xe4\xbd\xa0\xe5\x9c\xa8\xe4\xb8\x80\xe4\xb8\xaa C++ \xe9\xa1\xb9\xe7\x9b\xae\xe7\x9a\x84\xe9\x9a\x94"
    "\xe7\xa6\xbb";  // 你在一个 C++ 项目的隔离

int StartFakeAnthropicServer() {
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
    SOCKET_T listener = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listener == kBadSocket) {
        return 0;
    }
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;
    if (::bind(listener, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        return 0;
    }
    sockaddr_in bound{};
    int bound_len = sizeof(bound);
    ::getsockname(listener, reinterpret_cast<sockaddr*>(&bound), &bound_len);
    const int port = ntohs(bound.sin_port);
    ::listen(listener, 8);

    // 剧本按连接次序派发:1 主(tool_use agent)-> 2 子(run_command 卡 ~7s)->
    // 3 子(结论)-> 4 主(收尾)。之后再来连接一律给一句文本兜底。
    std::thread([listener]() {
        int connection = 0;
        while (true) {
            sockaddr_in client{};
            int client_len = sizeof(client);
            const SOCKET_T client_fd =
                ::accept(listener, reinterpret_cast<sockaddr*>(&client), &client_len);
            if (client_fd == kBadSocket) {
                return;
            }
            DrainHttpRequest(client_fd);
            ++connection;
            Log("SERVER connection #" + std::to_string(connection));
            switch (connection) {
                case 1:
                    RespondSse(client_fd,
                               ToolUseTurn("toolu_agent", "agent",
                                           "{\"title\":\"" + std::string(kTitle) + "\",\"prompt\":\"" +
                                               std::string(kPromptHead) +
                                               " git worktree "
                                               "\xe9\x87\x8c\xe5\xae\x9e\xe6\x96\xbd\xe9\xa1\xb9\xe7\x9b\xae"
                                               "\xe8\xae\xb0\xe5\xbf\x86\xe7\xb3\xbb\xe7\xbb\x9f\xe5\x8d\x87"
                                               "\xe7\xba\xa7\xef\xbc\x8c\xe8\xbf\x99\xe6\xae\xb5\xe8\xaf\xb4"
                                               "\xe6\x98\x8e\xe5\xbe\x88\xe9\x95\xbf\xe5\xbe\x88\xe9\x95\xbf"
                                               "\xe3\x80\x82\",\"run_in_background\":false}"));
                    break;
                case 2:
                    // 真工具、真耗时:ping -n 8 约 7 秒,坞的 Running 灯有
                    // 东西可画;随后子代理带着工具结果回来。
                    RespondSse(client_fd,
                               ToolUseTurn("toolu_sub", "run_command",
                                           "{\"command\":\"ping -n 8 127.0.0.1\",\"shell\":\"cmd\"}"));
                    break;
                case 3:
                    RespondSse(client_fd, TextTurn("\xe5\xad\x90\xe4\xbb\xa3\xe7\x90\x86\xe5\xb9\xb2"
                                                   "\xe5\xae\x8c\xe4\xba\x86\xef\xbc\x9a\xe6\xa3\x80"
                                                   "\xe7\xb4\xa2\xe9\x98\x88\xe5\x80\xbc\xe5\x9b\x9e"
                                                   "\xe5\xbd\x92\xe5\x85\xa8\xe7\xbb\xbf"));  // 子代理干完了:检索阈值回归全绿
                    break;
                case 4:
                    RespondSse(client_fd, TextTurn("\xe4\xb8\xbb\xe4\xbb\xa3\xe7\x90\x86\xe6\xb1\x87"
                                                   "\xe6\x80\xbb\xe5\xae\x8c\xe6\xaf\x95"));  // 主代理汇总完毕
                    break;
                default:
                    RespondSse(client_fd, TextTurn("ok"));
                    break;
            }
            closesocket(client_fd);
        }
    }).detach();
    return port;
}

void SetEnv(const std::wstring& name, const std::wstring& value) {
    _wputenv((name + L"=" + value).c_str());
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

    // 假 anthropic 服务 + 子进程环境:全量指到本地,不碰真网络,也不被
    // 会话中转/代理截胡(NO_PROXY 盖掉 http_proxy)。
    const int port = StartFakeAnthropicServer();
    if (port == 0) {
        Log("FAIL: fake server bind");
        return 1;
    }
    SetEnv(L"LUBANCODE_WIRE", L"anthropic");
    SetEnv(L"LUBANCODE_BASE_URL", L"http://127.0.0.1:" + std::to_wstring(port));
    SetEnv(L"LUBANCODE_API_KEY", L"agent-stream-driver");
    SetEnv(L"LUBANCODE_MODEL", L"fake-model");
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
    FlushConsoleInputBuffer(g_conin);

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = g_conin;
    si.hStdOutput = g_conout;
    si.hStdError = g_conout;
    PROCESS_INFORMATION pi{};
    std::wstring cmdline = L"\"" + exe_path + L"\" --yes";
    if (!CreateProcessW(exe_path.c_str(), cmdline.data(), nullptr, nullptr, TRUE, 0, nullptr, workdir.c_str(), &si,
                        &pi)) {
        Log("FAIL: CreateProcess " + std::to_string(GetLastError()));
        return 1;
    }
    CloseHandle(pi.hThread);

    // ---- 开场:空闲 composer 出来,程序活着 ----
    Check(WaitForText("\xe9\x94\xae\xe5\x85\xa5\xe5\xb9\xb6\xe5\x9b\x9e\xe8\xbd\xa6", 30000) ||
              FindFooterInputRow() >= 0,
          "开场:空闲 composer(30s 内)出现");  // 键入并回车
    Sleep(500);

    // ---- 发一句正文:主模型(假)回 agent tool_use,前台子代理跑起来 ----
    SendText("\xe6\xb4\xbe\xe4\xb8\x80\xe5\x8f\xaa\xe5\x89\x8d\xe5\x8f\xb0\xe5\xad\x90\xe4\xbb\xa3"
             "\xe7\x90\x86\xe5\x8e\xbb\xe5\xb9\xb2\xe6\xb4\xbb");  // 派一只前台子代理去干活
    SendKey(VK_RETURN, L'\r', 0);

    // 流式 footer 的输入框出现(分界线之后最底下的那个框)。
    Check(WaitForText("\xe9\x94\xae\xe5\x85\xa5\xe5\xb9\xb6\xe5\x9b\x9e\xe8\xbd\xa6", 30000),
          "流式:footer 输入行占位提示出现");  // 键入并回车
    int footer_input = FindFooterInputRow();
    Check(footer_input > 0, "流式:按结构找到 footer 输入行");

    // ---- 导航坞在 footer 输入框与状态栏之下贴底(层级反转) ----
    int title_row = -1;
    Check(WaitForText(kTitle, 15000, &title_row), "流式:坞行出现真正短 title");
    int rule_row = -1;
    for (int r = 398; r >= 0; --r) {
        const std::string input_text = ReadRow(r + 1);
        if (IsRuleRow(r) && !input_text.empty() && input_text[0] == '>' && IsRuleRow(r + 2) && !IsRuleRow(r + 3)) {
            rule_row = r;
            break;
        }
    }
    Check(rule_row > 0, "流式:composer 上横线定位到");
    Check(title_row >= 0 && title_row > rule_row + 3, "流式:title 行在状态栏之下(导航坞贴底)");
    Check(NoDockTextAboveComposer(rule_row), "流式:composer 上横线之上没有任何导航文本");
    Check(FindLastRow(kPromptHead) < 0, "流式:prompt 开头整屏不出现(不冒充标题)");
    Check(FindLastRow("ctrl+o \xe5\xb1\x95\xe5\xbc\x80\xe6\x98\x8e\xe7\xbb\x86") < 0,
          "流式:旧三行状态块(ctrl+o 展开明细)不再出现");

    // ---- 残帧计数:工具跑着、耗时/tokens 跳动,导航不复制 ----
    Check(CountRowsWith("\xe2\x86\x91/\xe2\x86\x93") == 1, "流式:操作提示恰好一份");
    Check(CountMainRows() == 1, "流式:main 行恰好一份");
    for (int sample = 0; sample < 3; ++sample) {
        Sleep(1500);  // 耗时 ~1s 一跳,采样跨多拍
        Check(CountRowsWith("\xe2\x86\x91/\xe2\x86\x93") == 1,
              "流式:耗时/token 刷新后操作提示仍恰好一份(第 " + std::to_string(sample + 1) + " 次采样)");
        Check(CountRowsWith("general-purpose") <= 1,
              "流式:坞行不随刷新复制(第 " + std::to_string(sample + 1) + " 次采样)");
    }

    // ---- Up 进坞焦点:选中标记出现在状态栏之下 ----
    SendKey(VK_UP, 0, 0);
    int marker_row = -1;
    Check(WaitForText("\xe2\x9d\xaf", 3000, &marker_row), "流式:空输入按上键,焦点标记出现");
    Check(marker_row >= 0 && marker_row > rule_row + 3, "流式:焦点标记在导航坞里(状态栏之下)");

    // ---- Enter 进详情:上横线右端挂 title,完整 prompt 只在详情里;
    //      Enter 被导航消费,不顺手把 composer 的字提交落队 ----
    SendKey(VK_RETURN, L'\r', 0);
    Check(WaitForText("\xe4\xbb\xbb\xe5\x8a\xa1\xe6\xa0\x87\xe9\xa2\x98", 3000),
          "流式:Enter 展开,详情出现\"任务标题\"");  // 任务标题
    {
        int rule_with_tag = -1;
        for (int r = 398; r >= 0; --r) {
            const std::string input_text = ReadRow(r + 1);
            if (IsRuleRow(r) && !input_text.empty() && input_text[0] == '>' && IsRuleRow(r + 2) &&
                !IsRuleRow(r + 3)) {
                rule_with_tag = r;
                break;
            }
        }
        Check(rule_with_tag > 0 && ReadRow(rule_with_tag).find(kTitle) != std::string::npos,
              "流式:详情态输入框上横线右端挂 title");
        Check(FindLastRow(kPromptHead) >= 0, "流式:详情里能看到完整 prompt(只有详情能看)");
    }

    // ---- Ctrl+C 有字先清字:敲半句,Ctrl+C 只清草稿,不打断、不退出 ----
    SendText("\xe5\x8d\x8a\xe5\x8f\xa5\xe8\xaf\x9d");  // 半句话
    Sleep(400);
    SendKey('C', 0, LEFT_CTRL_PRESSED);
    Check(WaitForTextGone("\xe5\x8d\x8a\xe5\x8f\xa5\xe8\xaf\x9d", 3000), "流式 Ctrl+C:footer 草稿被清空");
    Check(FindLastRow("\xe5\xb7\xb2\xe6\x89\x93\xe6\x96\xad") < 0,
          "流式 Ctrl+C 清字:不打断当前轮(没有'[已打断]')");
    Check(FindLastRow(kTitle) >= 0, "流式 Ctrl+C 清字:子代理还在跑(坞原样)");
    Check(WaitForText("\xe9\x94\xae\xe5\x85\xa5\xe5\xb9\xb6\xe5\x9b\x9e\xe8\xbd\xa6", 3000),
          "流式 Ctrl+C 清字:footer 占位提示回位");
    DWORD alive = STILL_ACTIVE;
    GetExitCodeProcess(pi.hProcess, &alive);
    Check(alive == STILL_ACTIVE, "流式 Ctrl+C 清字:进程仍活(没进双击退出)");

    // ---- Esc 逐层退:先详情,再焦点;两下都不打断整轮 ----
    SendKey(VK_ESCAPE, 0, 0);
    Check(WaitForTextGone("\xe4\xbb\xbb\xe5\x8a\xa1\xe6\xa0\x87\xe9\xa2\x98", 3000), "流式:Esc 先退详情");
    SendKey(VK_ESCAPE, 0, 0);
    Check(WaitForTextGone("\xe2\x9d\xaf", 3000), "流式:再 Esc 退代理焦点");
    Check(FindLastRow(kTitle) >= 0 && FindLastRow("\xe5\xb7\xb2\xe6\x89\x93\xe6\x96\xad") < 0,
          "流式:两下 Esc 都没有打断整轮(坞还在)");
    Check(CountRowsWith("general-purpose") <= 1, "流式:退详情后坞行至多一份(标签已摘)");

    // ---- 放开子代理:ping 跑完,Running 原地变完成,回合收场回空闲 ----
    // 注:子代理结论文本只在工具结果里,屏上摘要行是"子代理 N 轮 · M 次工具";
    // 主代理收尾正文紧跟着被 footer 的整帧重画顶走——所以这两条按"终态与
    // 回合收口"断言,不赌瞬时正文。
    Check(WaitForText("\xe6\xac\xa1\xe5\xb7\xa5\xe5\x85\xb7", 30000),
          "收尾:agent 条目终态摘要出现");  // "次工具"(⎿ 子代理 N 轮 · M 次工具)
    Check(WaitForText("\xe5\xae\x8c\xe6\x88\x90(", 15000), "收尾:坞 Running 原地变完成");
    // 回到空闲后:坞还挂着这条任务的终态,title 不跳、不重复、仍在下方。
    Check(WaitForText(kTitle, 10000), "空闲:坞保住终态任务的 title");
    Check(CountRowsWith("general-purpose") == 1, "空闲:坞行恰好一份(残帧归零)");
    Check(CountMainRows() == 1, "空闲:main 恰好一份");
    {
        const int idle_input = FindFooterInputRow();
        Check(idle_input > 0 && FindLastRow(kTitle) > idle_input + 3, "空闲:终态 title 仍在状态栏之下贴底");
    }

    // ---- 排查/留档:把当前屏面非空行倒进报告(不判定,只 INFO) ----
    {
        DWORD exit_code = STILL_ACTIVE;
        GetExitCodeProcess(pi.hProcess, &exit_code);
        Log(std::string("CHILD exit code: ") + (exit_code == STILL_ACTIVE ? "STILL_ACTIVE" : std::to_string(exit_code)));
    }
    for (int r = 0; r < 400; ++r) {
        const std::string row = ReadRow(r);
        if (!row.empty()) {
            Log("SCREEN " + std::to_string(r) + ": " + row);
        }
    }

    // ---- 退出子进程 ----
    SendText("/exit");
    SendKey(VK_RETURN, L'\r', 0);
    WaitForSingleObject(pi.hProcess, 15000);
    CloseHandle(pi.hProcess);

    Log(g_failures == 0 ? "ALL PASS" : ("FAILURES: " + std::to_string(g_failures)));
    FreeConsole();
    return g_failures == 0 ? 0 : 1;
}
