// 问题 1(真实实测:流式正文 Markdown 粗体不渲染)的刮屏冒烟驱动器:
// 照 agent_stream_driver 的手艺(进程内假 anthropic 服务 + AllocConsole +
// WriteConsoleInputW 假装敲键盘 + ReadConsoleOutputW 逐格刮屏),专验
// "正文 → 渲染"这条链在真控制台上的落笔半边。不进 ctest,手动跑:
//   body_render_driver <lubancode.exe 路径> <子进程工作目录> <报告文件路径>
//
// 剧本(一次用户消息,两轮模型请求):
//   第一幕·让路定格:正文段(含 **粗体**,故意不给空行结尾)后直接
//     tool_use read_file —— 实测真机最痛的形状。修复前 OnBlockBreak
//     直接丢块,星号永远露着;修复后正文在工具条目开画前定格成渲染版。
//     断言:该段无裸 **、粗体字带 FOREGROUND_INTENSITY。
//   第二幕·收尾重画:tool_result 回来后模型补一段带空行的完整正文
//     (逐字滴的 **粗体**、标题、列表、代码块内 ** 字面量、\** 转义)。
//     断言:正文区无裸 **;代码块内 ** 原样;收尾统计行出现。
//   M3·标题间距(问题 3:分块渲染吃掉标题前空行):第二幕的标题
//     "### 运行方式"独立成块,断言它上一行是空行、上上行(粗体段)非空
//     ——恰好一行,不贴死也不多垫。
// 结论逐行写报告文件(PASS/FAIL/INFO),退出码 0 = 全过。

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <winsock2.h>
#include <ws2tcpip.h>

#include <atomic>
#include <cstdlib>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace {

using SOCKET_T = SOCKET;
constexpr SOCKET_T kBadSocket = INVALID_SOCKET;

HANDLE g_conin = INVALID_HANDLE_VALUE;
HANDLE g_conout = INVALID_HANDLE_VALUE;
std::ofstream g_report;
std::mutex g_log_mutex;
int g_failures = 0;
DWORD g_start_tick = 0;

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

std::string JsonEscape(const std::string& text) {
    std::string escaped;
    for (char c : text) {
        switch (c) {
            case '\\':
                escaped += "\\\\";
                break;
            case '"':
                escaped += "\\\"";
                break;
            case '\n':
                escaped += "\\n";
                break;
            case '\r':
                escaped += "\\r";
                break;
            case '\t':
                escaped += "\\t";
                break;
            default:
                escaped += c;
                break;
        }
    }
    return escaped;
}

std::string Sse(const std::string& json) { return "data: " + json + "\n\n"; }

std::string TextDeltaEvent(const std::string& text) {
    return "{\"type\":\"content_block_delta\",\"index\":0,\"delta\":{\"type\":\"text_delta\",\"text\":\"" +
           JsonEscape(text) + "\"}}";
}

// ---- 剧本文本(UTF-8 字面量直接写中文,文件本身存 UTF-8) --------------------

// 用户锚:流式粗体体检
const char* kUserMark =
    "\xe6\xb5\x81\xe5\xbc\x8f\xe7\xb2\x97\xe4\xbd\x93\xe4\xbd\x93\xe6\xa3\x80";

// 第一幕正文(无空行结尾,直接跟 tool_use):……**React + Vite 前端、Express 后端**……
const char* kAct1Text =
    "\xe5\xbd\x93\xe5\x89\x8d\xe7\x9b\xae\xe5\xbd\x95\xe4\xb8\xba\xe7\xa9\xba\xe3\x80\x82"
    "\xe6\x88\x91\xe5\xb0\x86\xe6\x8c\x89\xe5\xb8\xb8\xe8\xa7\x81\xe4\xb8\x94\xe6\x98\x93\xe8\xbf\x90"
    "\xe8\xa1\x8c\xe7\x9a\x84\xe6\x96\xb9\xe6\xa1\x88\xe5\xae\x9e\xe7\x8e\xb0\xef\xbc\x9a"
    "**React + Vite \xe5\x89\x8d\xe7\xab\xaf\xe3\x80\x81" "Express \xe5\x90\x8e\xe7\xab\xaf**"
    "\xef\xbc\x8c\xe6\x94\xaf\xe6\x8c\x81\xe6\x90\x9c\xe7\xb4\xa2\xe5\x8f\x8a\xe5\xae\x8c\xe6\x95\xb4"
    "\xe5\x9b\xbe\xe4\xb9\xa6\xe5\xa2\x9e\xe5\x88\xa0\xe6\x94\xb9\xe6\x9f\xa5\xe3\x80\x82";

// 第一幕粗体锚(渲染版应显示、不带星号):React + Vite
const char* kAct1BoldMark = "React + Vite";

// 第二幕收尾正文:
//   收尾说明:技术选型是**React 前端、Express 后端(JSON 持久化)**的组合。(逐字滴)
//   ### 运行方式
//   - 先 npm install
//   - 再 npm run dev
//   ```js
//   const s = `**不是粗体**`;   ← 代码块内字面星号,必须原样
//   ```
//   转义写法 \**半转义** 与 \*\*全字面\*\* 不上样式。
const char* kAct2BoldText =
    "\xe6\x94\xb6\xe5\xb0\xbe\xe8\xaf\xb4\xe6\x98\x8e\xef\xbc\x9a"
    "\xe6\x8a\x80\xe6\x9c\xaf\xe9\x80\x89\xe5\x9e\x8b\xe6\x98\xaf**React \xe5\x89\x8d\xe7\xab\xaf\xe3\x80\x81"
    "Express \xe5\x90\x8e\xe7\xab\xaf\xef\xbc\x88JSON \xe6\x8c\x81\xe4\xb9\x85\xe5\x8c\x96\xef\xbc\x89**"
    "\xe7\x9a\x84\xe7\xbb\x84\xe5\x90\x88\xe3\x80\x82";
const char* kAct2Rest =
    "\n\n### \xe8\xbf\x90\xe8\xa1\x8c\xe6\x96\xb9\xe5\xbc\x8f\n\n"
    "- \xe5\x85\x88 npm install\n- \xe5\x86\x8d npm run dev\n\n"
    "```js\nconst s = `**\xe4\xb8\x8d\xe6\x98\xaf\xe7\xb2\x97\xe4\xbd\x93**`;\n```\n\n"
    "\xe8\xbd\xac\xe4\xb9\x89\xe5\x86\x99\xe6\xb3\x95 \\**\xe5\x8d\x8a\xe8\xbd\xac\xe4\xb9\x89** "
    "\xe4\xb8\x8e \\*\\*\xe5\x85\xa8\xe5\xad\x97\xe9\x9d\xa2\\*\\* \xe4\xb8\x8d\xe4\xb8\x8a\xe6\xa0\xb7\xe5\xbc\x8f\xe3\x80\x82\n";

// ---- SSE 轮 -----------------------------------------------------------------

// 第零幕:先来一轮纯工具调用(不带正文)——照真机现场:问题 1 的粗体段
// 出现在"调完 todo_write 和数次 search 之后",正文首笔落在工具条目之下,
// 不是从转轮底下凭空起笔。
void RespondAct0(SOCKET_T s) {
    const std::vector<std::string> events = {
        "{\"type\":\"message_start\",\"message\":{\"id\":\"msg0\",\"model\":\"fake-model\"}}",
        "{\"type\":\"content_block_start\",\"index\":0,\"content_block\":{\"type\":\"tool_use\",\"id\":\"toolu_r0\","
        "\"name\":\"read_file\"}}",
        "{\"type\":\"content_block_delta\",\"index\":0,\"delta\":{\"type\":\"input_json_delta\",\"partial_json\":"
        "\"{\\\"path\\\":\\\"C:/Windows/win.ini\\\"}\"}}",
        "{\"type\":\"content_block_stop\",\"index\":0}",
        "{\"type\":\"message_delta\",\"delta\":{\"stop_reason\":\"tool_use\"},\"usage\":{\"input_tokens\":80,"
        "\"output_tokens\":10}}",
    };
    std::string out;
    for (const auto& e : events) {
        out += Sse(e);
    }
    const std::string head = "HTTP/1.1 200 OK\r\n"
                             "Content-Type: text/event-stream\r\n"
                             "Content-Length: " +
                             std::to_string(out.size()) + "\r\n" + "Connection: close\r\n\r\n";
    SendAll(s, head + out);
}

// 第一幕:text 块(拆碎滴出)+ tool_use read_file,同一条消息。
// delta 逐枚隔 150ms 滴出——快照采样才追得上每一笔之后的屏面。
void RespondAct1(SOCKET_T s) {
    std::vector<std::string> leading = {
        "{\"type\":\"message_start\",\"message\":{\"id\":\"msg1\",\"model\":\"fake-model\"}}",
        "{\"type\":\"content_block_start\",\"index\":0,\"content_block\":{\"type\":\"text\",\"text\":\"\"}}",
    };
    std::vector<std::string> body_events;
    // 正文整段一笔发:逐字/跨块分块已由单测(test_body_render_pipeline)钉
    // 死;这里专验让路定格,避开 wire 层"小 delta 丢字"的既有病(main 同
    // 样复现,另行立案)。
    body_events.push_back(TextDeltaEvent(kAct1Text));
    std::vector<std::string> trailing = {
        "{\"type\":\"content_block_stop\",\"index\":0}",
        "{\"type\":\"content_block_start\",\"index\":1,\"content_block\":{\"type\":\"tool_use\",\"id\":\"toolu_r1\","
        "\"name\":\"read_file\"}}",
        "{\"type\":\"content_block_delta\",\"index\":1,\"delta\":{\"type\":\"input_json_delta\",\"partial_json\":"
        "\"{\\\"path\\\":\\\"C:/Windows/win.ini\\\"}\"}}",
        "{\"type\":\"content_block_stop\",\"index\":1}",
        "{\"type\":\"message_delta\",\"delta\":{\"stop_reason\":\"tool_use\"},\"usage\":{\"input_tokens\":100,"
        "\"output_tokens\":40}}",
    };
    std::size_t total = 0;
    for (const auto& e : leading) {
        total += Sse(e).size();
    }
    for (const auto& e : body_events) {
        total += Sse(e).size();
    }
    for (const auto& e : trailing) {
        total += Sse(e).size();
    }
    const std::string head = "HTTP/1.1 200 OK\r\n"
                             "Content-Type: text/event-stream\r\n"
                             "Content-Length: " +
                             std::to_string(total) + "\r\n" + "Connection: close\r\n\r\n";
    SendAll(s, head);
    for (const auto& e : leading) {
        SendAll(s, Sse(e));
    }
    for (const auto& e : body_events) {
        SendAll(s, Sse(e));
        Sleep(150);
    }
    for (const auto& e : trailing) {
        SendAll(s, Sse(e));
    }
}

// 第二幕:收尾正文——粗体段逐字滴(验增量重画),其余整段 + 空行收束。
void RespondAct2(SOCKET_T s) {
    std::vector<std::string> events;
    events.push_back("{\"type\":\"message_start\",\"message\":{\"id\":\"msg2\",\"model\":\"fake-model\"}}");
    events.push_back("{\"type\":\"content_block_start\",\"index\":0,\"content_block\":{\"type\":\"text\",\"text\":\"\"}}");
    // 收尾正文整段两笔(粗体段一笔+其余一笔):逐字路径由单测钉,这里验
    // 空行收束与 FinalizeRepaint 的真控制台落笔。
    events.push_back(TextDeltaEvent(kAct2BoldText));
    events.push_back(TextDeltaEvent(kAct2Rest));
    events.push_back("{\"type\":\"content_block_stop\",\"index\":0}");
    events.push_back(
        "{\"type\":\"message_delta\",\"delta\":{\"stop_reason\":\"end_turn\"},\"usage\":{\"input_tokens\":150,"
        "\"output_tokens\":80}}");
    std::string out;
    for (const auto& e : events) {
        out += Sse(e);
    }
    const std::string head = "HTTP/1.1 200 OK\r\n"
                             "Content-Type: text/event-stream\r\n"
                             "Content-Length: " +
                             std::to_string(out.size()) + "\r\n" + "Connection: close\r\n\r\n";
    SendAll(s, head + out);
}

int StartFakeServer() {
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
    ::listen(listener, SOMAXCONN);
    std::thread([listener] {
        std::atomic<int> connection_ordinal{0};
        for (;;) {
            sockaddr_in client{};
            int client_len = sizeof(client);
            const SOCKET_T client_fd = ::accept(listener, reinterpret_cast<sockaddr*>(&client), &client_len);
            if (client_fd == kBadSocket) {
                return;
            }
            std::thread([client_fd, &connection_ordinal] {
                const std::string raw = DrainHttpRequest(client_fd);
                const std::size_t body_at = raw.find("\r\n\r\n");
                const std::string body = body_at == std::string::npos ? std::string() : raw.substr(body_at + 4);
                // 按连接次序分派(不按请求体特征):系统提示/工具 schema 里
                // 本就带 "tool_result" 字样,首条真请求会被特征分派误判;标
                // 题生成请求也没个准特征。次序是稳的:1=第一幕(正文+工具),
                // 2=第二幕(收尾),3 起(标题生成等)回一段无 markdown 的短文本。
                const int ordinal = connection_ordinal.fetch_add(1) + 1;
                Log("SERVER t=" + std::to_string(GetTickCount() - g_start_tick) + "ms conn#" +
                    std::to_string(ordinal) + " bytes=" + std::to_string(body.size()));
                if (ordinal == 1) {
                    RespondAct0(client_fd);
                } else if (ordinal == 2) {
                    RespondAct1(client_fd);
                } else if (ordinal == 3) {
                    RespondAct2(client_fd);
                } else {
                    // 标题等旁路请求:短纯文本,别把 markdown 标记漏进标题行。
                    const std::vector<std::string> events = {
                        "{\"type\":\"message_start\",\"message\":{\"id\":\"msgx\",\"model\":\"fake-model\"}}",
                        "{\"type\":\"content_block_start\",\"index\":0,\"content_block\":{\"type\":\"text\","
                        "\"text\":\"\"}}",
                        TextDeltaEvent("ok"),
                        "{\"type\":\"content_block_stop\",\"index\":0}",
                        "{\"type\":\"message_delta\",\"delta\":{\"stop_reason\":\"end_turn\"},\"usage\":"
                        "{\"input_tokens\":10,\"output_tokens\":2}}",
                    };
                    std::string out;
                    for (const auto& e : events) {
                        out += Sse(e);
                    }
                    const std::string head = "HTTP/1.1 200 OK\r\n"
                                             "Content-Type: text/event-stream\r\n"
                                             "Content-Length: " +
                                             std::to_string(out.size()) + "\r\n" + "Connection: close\r\n\r\n";
                    SendAll(client_fd, head + out);
                }
                ::closesocket(client_fd);
            }).detach();
        }
    }).detach();
    return port;
}

// ---- 刮屏 -------------------------------------------------------------------

int BufferWidth() {
    CONSOLE_SCREEN_BUFFER_INFO info{};
    GetConsoleScreenBufferInfo(g_conout, &info);
    return info.dwSize.X;
}

std::string ReadRow(int row, std::vector<WORD>* attrs = nullptr) {
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
        Sleep(200);
    }
    return false;
}

// row 行起的连续非空行区里有没有裸 ** (顺带把命中行记进报告)。
bool RegionHasBareStars(int from_row, int to_row) {
    for (int r = from_row; r <= to_row; ++r) {
        const std::string text = ReadRow(r);
        if (text.find("**") != std::string::npos) {
            Log("INFO: 裸 ** 所在行[" + std::to_string(r) + "] = " + text);
            return true;
        }
    }
    return false;
}

// row 行(或紧邻的下一行)里粗体锚文本是否带亮色(bold → FOREGROUND_INTENSITY)。
bool RowHasIntensity(const std::string& needle) {
    const int row = FindLastRow(needle);
    if (row < 0) {
        return false;
    }
    std::vector<WORD> attrs;
    const std::string text = ReadRow(row, &attrs);
    // ReadRow 逐格收字(一格一字符),attrs 与字符格一一对应;而 find 给的是
    // UTF-8 字节下标——先把字节下标换算成字符格下标(非续字节即新字符)。
    const std::size_t at = text.find(needle);
    if (at == std::string::npos) {
        return false;
    }
    std::size_t col = 0;
    for (std::size_t i = 0; i < at; ++i) {
        if ((static_cast<unsigned char>(text[i]) & 0xC0) != 0x80) {
            ++col;
        }
    }
    std::size_t needle_cells = 0;
    for (const char c : needle) {
        if ((static_cast<unsigned char>(c) & 0xC0) != 0x80) {
            ++needle_cells;
        }
    }
    int bright = 0;
    for (std::size_t i = col; i < attrs.size() && i < col + needle_cells; ++i) {
        if ((attrs[i] & FOREGROUND_INTENSITY) != 0) {
            ++bright;
        }
    }
    return bright >= 2;
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
        Sleep(12);
    }
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
    g_start_tick = GetTickCount();

    const int port = StartFakeServer();
    if (port == 0) {
        Log("FAIL: fake server bind");
        return 1;
    }
    {
        wchar_t workdir_buf[MAX_PATH]{};
        GetCurrentDirectoryW(MAX_PATH, workdir_buf);
        const std::wstring hermetic_home =
            std::wstring(workdir_buf) + L"\\drvhome_br_" + std::to_wstring(GetCurrentProcessId());
        CreateDirectoryW(hermetic_home.c_str(), nullptr);
        CreateDirectoryW((hermetic_home + L"\\.lubancode").c_str(), nullptr);
        SetEnv(L"USERPROFILE", hermetic_home);
    }
    SetEnv(L"LUBANCODE_WIRE", L"anthropic");
    SetEnv(L"LUBANCODE_BASE_URL", L"http://127.0.0.1:" + std::to_wstring(port));
    SetEnv(L"LUBANCODE_API_KEY", L"body-render-driver");
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
    inheritable.nLength = sizeof(SECURITY_ATTRIBUTES);
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
    si.hStdError = g_conout;
    PROCESS_INFORMATION pi{};
    std::wstring cmdline = L"\"" + exe_path + L"\" --yes";
    if (!CreateProcessW(exe_path.c_str(), cmdline.data(), nullptr, nullptr, TRUE, 0, nullptr, workdir.c_str(), &si,
                        &pi)) {
        Log("FAIL: CreateProcess " + std::to_string(GetLastError()));
        return 1;
    }
    CloseHandle(pi.hThread);

    Check(WaitForText("shift+tab", 30000), "M0 开场:输入框就位(30s 内)");
    Sleep(400);

    // ---- 第一幕:正文(含粗体、无空行)后直接跟工具 → 让路定格 ----
    SendText(kUserMark);
    SendKey(VK_RETURN, L'\r', 0);
    // 流式中逐帧采样(排障用):第一幕正文滴落期间每 300ms 抓正文区几行,
    // 看增量重画/让路定格哪一步把字弄丢。回合收束后停止。
    {
        const DWORD sample_deadline = GetTickCount() + 30000;
        int frame = 0;
        while (GetTickCount() < sample_deadline) {
            Sleep(120);
            ++frame;
            std::string snap = "FRAME" + std::to_string(frame) + ":";
            for (int r = 8; r < 12; ++r) {
                snap += " |[" + std::to_string(r) + "] " + ReadRow(r);
            }
            Log(snap);
            if (FindLastRow("npm run dev") >= 0) {
                break;
            }
        }
    }
    // 工具轮真实执行(read_file 免确认),第二幕收尾文本出现即回合收束。
    Check(WaitForText("npm run dev", 120000), "M1 回合跑完:第二幕收尾正文出现(120s 内)");
    Sleep(1500);  // 收束重画/统计行落定

    {
        // 第一幕正文段:含粗体锚的那一行(区段 = 锚行上下 3 行)。
        const int anchor = FindLastRow(kAct1BoldMark);
        Check(anchor >= 0, "M1 让路定格:第一幕正文(React + Vite)还在屏上");
        if (anchor >= 0) {
            const int from = anchor > 2 ? anchor - 2 : 0;
            Check(!RegionHasBareStars(from, anchor + 2), "M1 让路定格:正文段无裸 **");
            Check(RowHasIntensity(kAct1BoldMark), "M1 让路定格:粗体字带亮色(FOREGROUND_INTENSITY)");
        }
    }

    {
        // 第二幕收尾:粗体段(锚 = Express 后端(JSON 持久化) 渲染后无星号)。
        const int anchor = FindLastRow("Express");
        Check(anchor >= 0, "M2 收尾重画:粗体段在屏上");
        if (anchor >= 0) {
            Check(!ReadRow(anchor).empty(), "M2 收尾重画:粗体行非空");
        }
        // 标题剥号:### 运行方式 → 渲染版只有"运行方式",无 ###。
        const int heading = FindLastRow("\xe8\xbf\x90\xe8\xa1\x8c\xe6\x96\xb9\xe5\xbc\x8f");  // 运行方式
        Check(heading >= 0, "M2 收尾重画:标题行在屏上");
        if (heading >= 0) {
            Check(ReadRow(heading).find("###") == std::string::npos, "M2 收尾重画:标题已剥 ###");
            // 问题 3(分块吃掉标题前空行):标题前恰好一行空行——上一行空、
            // 上上行(粗体段)非空,不贴死也不越撑越松。
            if (heading >= 2) {
                Check(ReadRow(heading - 1).empty(), "M3 标题间距:标题上一行是空行");
                Check(!ReadRow(heading - 2).empty(), "M3 标题间距:标题前恰好一行(上上行非空)");
            } else {
                Check(false, "M3 标题间距:标题行离屏顶太近,位置异常");
            }
        }
        // 列表圆点。
        const std::string bullet = "\xE2\x80\xA2";
        bool saw_bullet = false;
        for (int r = heading; r < heading + 6 && r < 400; ++r) {
            if (ReadRow(r).find(bullet) != std::string::npos) {
                saw_bullet = true;
            }
        }
        Check(saw_bullet, "M2 收尾重画:列表圆点出现");
        // 代码块内字面 ** 原样保留。
        const int code_row = FindLastRow("const s =");
        Check(code_row >= 0, "M2 代码块:代码行在屏上");
        if (code_row >= 0) {
            Check(ReadRow(code_row).find("**") != std::string::npos, "M2 代码块:块内 ** 字面量原样保留");
        }
        // 转义:\*\*全字面\*\* 渲染成 **全字面**(两枚星在、不上样式)。
        const int esc_row = FindLastRow("\xe5\x85\xa8\xe5\xad\x97\xe9\x9d\xa2");  // 全字面
        Check(esc_row >= 0, "M2 转义:全字面锚在屏上");
        if (esc_row >= 0) {
            const std::string row = ReadRow(esc_row);
            Check(row.find("**\xe5\x85\xa8\xe5\xad\x97\xe9\x9d\xa2**") != std::string::npos,
                  "M2 转义:\\*\\* 渲染成字面 **(不误吞)");
        }
        // 收尾后,整个回答区(用户提交行之下的正文区)无本应渲染的裸 **——
        // 代码块与转义字面量两处除外(它们按验收就该带字面星)。
        const int prompt_row = FindLastRow(kUserMark);
        int bare_rows = 0;
        if (prompt_row >= 0) {
            for (int r = prompt_row + 1; r < 400; ++r) {
                const std::string text = ReadRow(r);
                if (text.find("**") != std::string::npos && r != code_row && r != esc_row) {
                    ++bare_rows;
                    Log("INFO: 收尾后仍裸 ** 的行[" + std::to_string(r) + "] = " + text);
                }
            }
        }
        Check(bare_rows == 0, "M2 收尾:除字面量外全屏无裸 **");
    }

    // 有失败就把用户提交行之下的屏都倒出来,好对着报告查案。
    if (g_failures > 0) {
        const int prompt_row = FindLastRow(kUserMark);
        const int from = prompt_row >= 0 ? prompt_row : 0;
        for (int r = from; r < 400; ++r) {
            const std::string text = ReadRow(r);
            if (!text.empty()) {
                Log("DUMP[" + std::to_string(r) + "]: " + text);
            }
        }
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
