// 可视区重排驱动器(多智能体真机回归单):真控制台(AllocConsole +
// WriteConsoleInputW 假键盘 + ReadConsoleOutputW 逐格刮屏)里跑一场
// "主回合派两只后台代理随即收口"的完整回合,验的是规格《多智能体真机回归_
// 可视区重排与查看态》的主病灶——回合收口后 composer 顶边、代理坞、状态栏
// 必须整体落在可视窗口里,不许靠改字号/滚轮/Ctrl+End 救场。不进 ctest,
// 集成验证时手动跑:
//   viewport_driver <lubancode.exe 路径> <子进程工作目录> <报告文件路径>
//
// 五种现场(规格"实现落点"第四条)各一幕,每幕起一只全新子进程:
//   1) 80×24 窄矮窗     2) 120×35 常规窗    3) 全屏(最大窗口,缓冲贴窗口)
//   4) 改字号:启动中/运行中/完成回流时三时机各十轮,进程不退、任务不丢、
//      main 可回(疑案"code 1"的自动化复现尝试)
//   5) 滚动缓冲区非底部:回合收口后把窗口上滚数行再等回流,composer 仍须
//      无补键可见
// 每幕把 viewport 快照(窗口内逐行)写进报告——"截图回归"的留档。
//
// 模型走进程内假 anthropic SSE 服务(与 agent_stream_driver 同一套手艺),
// 主回合派两只后台代理后交一段长正文收口;后台代理跑若干轮慢工具后交卷,
// 触发空闲回流轮。不碰真网络。

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <winsock2.h>
#include <ws2tcpip.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#pragma comment(lib, "ws2_32.lib")

namespace {

HANDLE g_conin = INVALID_HANDLE_VALUE;
HANDLE g_conout = INVALID_HANDLE_VALUE;
std::ofstream g_report;
int g_failures = 0;
// 幕间重置假服务的剧本状态:同一服务跑七幕,每幕的子进程都当"第一场"派
// 甲派乙(不重置的话第二幕起主回合直接收长正文,坞行永远等不来)。
std::function<void()> g_reset_server_state;

void Log(const std::string& line) {
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

// ---- 刮屏原语 ----

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

// 可视窗口范围(缓冲绝对行号)。
int WindowTop() {
    CONSOLE_SCREEN_BUFFER_INFO info{};
    GetConsoleScreenBufferInfo(g_conout, &info);
    return info.srWindow.Top;
}

int WindowBottom() {
    CONSOLE_SCREEN_BUFFER_INFO info{};
    GetConsoleScreenBufferInfo(g_conout, &info);
    return info.srWindow.Bottom;
}

std::string ReadRow(int row) {
    const int width = BufferWidth();
    if (row < 0 || row >= BufferHeight()) {
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
    for (int row = (std::min)(max_rows, BufferHeight()) - 1; row >= 0; --row) {
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
            if (run >= 30) {
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

// 从底往上按结构认 composer 框(上横线 r / '>' 起输入行 r+1 / 下横线 r+2 /
// 非横线状态行 r+3),返回输入行行号;找不到 -1。
int FindComposerInputRow() {
    for (int r = BufferHeight() - 5; r >= 0; --r) {
        const std::string input_text = ReadRow(r + 1);
        if (IsRuleRow(r) && !input_text.empty() && input_text[0] == '>' && IsRuleRow(r + 2) && !IsRuleRow(r + 3)) {
            return r + 1;
        }
    }
    return -1;
}

// 一次性判定:composer 上横线到坞区末行整体落在可视窗口里。坞末行按
// "上横线之下最后一条非空行"认(状态栏、导航坞、提示行都在其中)。
bool TryComposerFrameInViewport(int* rule_row_out, int* frame_bottom_out, std::string* why) {
    const int input_row = FindComposerInputRow();
    if (input_row <= 0) {
        *why = "按结构找不到 composer 框";
        return false;
    }
    const int rule_row = input_row - 1;
    int bottom = rule_row;
    for (int r = rule_row + 1; r < BufferHeight(); ++r) {
        if (!ReadRow(r).empty()) {
            bottom = r;
        }
    }
    *rule_row_out = rule_row;
    *frame_bottom_out = bottom;
    const int top = WindowTop();
    const int bot = WindowBottom();
    char buf[160];
    if (rule_row < top) {
        std::snprintf(buf, sizeof(buf), "composer 上横线(%d)在窗口顶(%d)之上", rule_row, top);
        *why = buf;
        return false;
    }
    if (bottom > bot) {
        std::snprintf(buf, sizeof(buf), "坞区末行(%d)伸出窗口底(%d)", bottom, bot);
        *why = buf;
        return false;
    }
    std::snprintf(buf, sizeof(buf), "composer 帧 %d..%d 全在窗口 %d..%d 内", rule_row, bottom, top, bot);
    *why = buf;
    return true;
}

// 主断言(带重试):composer 框 10 秒内出现且整体落在可视窗口里——收口到
// 下一次 ReadLine 重画之间有空窗,重试只等"框还没画",范围问题立刻判。
bool ComposerFrameInViewport(int timeout_ms, int* rule_row_out, int* frame_bottom_out, std::string* why) {
    const DWORD deadline = GetTickCount() + static_cast<DWORD>(timeout_ms);
    while (true) {
        if (TryComposerFrameInViewport(rule_row_out, frame_bottom_out, why)) {
            return true;
        }
        if (GetTickCount() >= deadline) {
            return false;
        }
        Sleep(200);
    }
}

// 把当前可视窗口逐行倒进报告(截图留档)。
void DumpViewport(const std::string& tag) {
    Log("--- VIEWPORT " + tag + " (window " + std::to_string(WindowTop()) + ".." + std::to_string(WindowBottom()) +
        ") ---");
    for (int r = WindowTop(); r <= WindowBottom(); ++r) {
        const std::string row = ReadRow(r);
        Log("  VP" + std::to_string(r) + ": " + row);
    }
}

// ---- 进程内假 anthropic 服务 ----

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

// 剧本锚文本(UTF-8 字面量,与 agent_stream_driver 同一套写法)。
const char* kUserPromptReal =
    "\xe5\x9b\x9e\xe5\xbd\x92\xe5\x8f\xaf\xe8\xa7\x86\xe5\x8c\xba\xe4\xb8\x80\xe6\x9c\xac\xe8\xb4\xa6";  // 回归可视区一本账
const char* kAgentA = "\xe6\x85\xa2\xe6\x9f\xa5\xe7\x94\xb2\xe8\xa7\x86\xe5\x8f\xa3\xe8\xb4\xa6";  // 慢查甲视口账
const char* kAgentB = "\xe6\x85\xa2\xe6\x9f\xa5\xe4\xb9\x99\xe8\xa7\x86\xe5\x8f\xa3\xe8\xb4\xa6";  // 慢查乙视口账
const char* kTitleA = "\xe7\x94\xb2\xe7\x9a\x84\xe8\xa7\x86\xe5\x8f\xa3\xe8\xb4\xa6";  // 甲的视口账
const char* kTitleB = "\xe4\xb9\x99\xe7\x9a\x84\xe8\xa7\x86\xe5\x8f\xa3\xe8\xb4\xa6";  // 乙的视口账
const char* kDoneA =
    "\xe7\x94\xb2\xe4\xba\xa4\xe5\x8d\xb7\xef\xbc\x9a\xe8\xa7\x86\xe5\x8f\xa3\xe8\xb4\xa6\xe5\xb7\xb2\xe6\xb8\x85";  // 甲交卷:视口账已清
const char* kDoneB =
    "\xe4\xb9\x99\xe4\xba\xa4\xe5\x8d\xb7\xef\xbc\x9a\xe8\xa7\x86\xe5\x8f\xa3\xe8\xb4\xa6\xe5\xb7\xb2\xe6\xb8\x85";  // 乙交卷:视口账已清
const char* kBodyTail =
    "\xe6\x94\xb6\xe5\x8f\xa3\xe5\x8f\xa5\xe5\x91\x8a\xe5\xae\x8c\xe6\xaf\x95";  // 收口句号完毕
const char* kReflowDone =
    "\xe5\x9b\x9e\xe6\xb5\x81\xe6\x94\xb6\xe5\x8f\xa3\xe5\xae\x8c\xe6\xaf\x95";  // 回流收口完毕

// 长正文:30 行,逼着回合把缓冲滚起来(窄窗也要滚)。
std::string LongBody() {
    std::string text;
    for (int i = 1; i <= 30; ++i) {
        text += "\xe6\xad\xa3\xe6\x96\x87\xe7\xac\xac" + std::to_string(i) +
                "\xe8\xa1\x8c\xef\xbc\x8c\xe6\x85\xa2\xe6\x85\xa2\xe9\x93\xba\xe5\xbc\x80\xe3\x80\x82\n";  // 正文第N行,慢慢铺开。
    }
    text += kBodyTail;
    return text;
}

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

    // 剧本(按请求体特征派发):
    //   主回合:没派过派甲(后台)-> 派过甲派乙(后台)-> 都派过长正文收口。
    //   后台子代理:三轮"睡 1 秒 + 读文件"后交卷(总时长 ~4s,给改字号与
    //   上滚留窗口)。
    //   回流轮(最新 user 是回流 prompt):先 ping ~3 秒再收口——"完成回流时"
    //   改字号的时机窗口。
    struct ServerState {
        std::mutex mutex;
        bool dispatched_a = false;
        bool dispatched_b = false;
        int a_reads = 0;
        int b_reads = 0;
        bool reflow_tool_done = false;
    };
    const auto state = std::make_shared<ServerState>();
    g_reset_server_state = [state] {
        std::lock_guard<std::mutex> lock(state->mutex);
        state->dispatched_a = false;
        state->dispatched_b = false;
        state->a_reads = 0;
        state->b_reads = 0;
        state->reflow_tool_done = false;
    };
    const auto serve_connection = [state](SOCKET_T client_fd) {
        const std::string raw = DrainHttpRequest(client_fd);
        const std::size_t body_at = raw.find("\r\n\r\n");
        const std::string body = body_at == std::string::npos ? std::string() : raw.substr(body_at + 4);
        const auto has = [&body](const char* needle) { return body.find(needle) != std::string::npos; };
        // 子代理请求的 system 带专用 persona(SubAgentPersona),主回合不带
        // ——凭这个分账,主回合历史里的 agent 工具入参不会误命中(与
        // agent_stream_driver 同一判法)。
        const bool sub_agent_request = has("\xe8\x83\xbd\xe6\x90\x9c\xe7\xb4\xa2"
                                           "\xe3\x80\x81\xe5\x88\x86\xe6\x9e\x90"
                                           "\xe5\xb9\xb6\xe5\xae\x8c\xe6\x88\x90"
                                           "\xe5\xa4\x9a\xe6\xad\xa5\xe4\xbb\xbb"
                                           "\xe5\x8a\xa1");  // 能搜索、分析并完成多步任务
        // 最新一条 user 消息(回流轮分账只认它,历史旧标记不劫持)。
        const auto newest_has = [&body](const char* needle) {
            const std::size_t last_user = body.rfind("\"role\":\"user\"");
            return last_user != std::string::npos && body.find(needle, last_user) != std::string::npos;
        };
        if (sub_agent_request) {
            // 后台子代理自己的来回:三轮"睡 1 秒 + 读文件"后交卷。
            bool is_a = has(kAgentA);
            int reads = 0;
            {
                std::lock_guard<std::mutex> lock(state->mutex);
                reads = is_a ? ++state->a_reads : ++state->b_reads;
            }
            Sleep(1000);
            const bool done = reads >= 3;
            if (done) {
                RespondSse(client_fd, TextTurn(is_a ? kDoneA : kDoneB));
            } else {
                RespondSse(client_fd, ToolUseTurn(is_a ? "toolu_a" : "toolu_b", "read_file",
                                                  "{\"path\":\"C:/Windows/win.ini\"}"));
            }
        } else if (newest_has("\xe5\x90\x8e\xe5\x8f\xb0\xe5\xad\x90\xe4\xbb\xa3\xe7\x90\x86\xe6\x9c\x89\xe6\x96\xb0\xe7\xbb\x93"
                              "\xe6\x9e\x9c")) {  // 后台子代理有新结果
            std::lock_guard<std::mutex> lock(state->mutex);
            if (state->reflow_tool_done) {
                RespondSse(client_fd, TextTurn(kReflowDone));
            } else {
                state->reflow_tool_done = true;
                RespondSse(client_fd, ToolUseTurn("toolu_reflow", "run_command",
                                                  "{\"command\":\"ping -n 4 127.0.0.1\",\"shell\":\"cmd\"}"));
            }
        } else {
            // 主回合:没派过派甲(后台)-> 派过甲派乙(后台)-> 都派过长正文收口。
            std::lock_guard<std::mutex> lock(state->mutex);
            if (!state->dispatched_a) {
                state->dispatched_a = true;
                RespondSse(client_fd, ToolUseTurn("toolu_agent_a", "agent",
                                                  "{\"title\":\"" + std::string(kTitleA) + "\",\"prompt\":\"" +
                                                      std::string(kAgentA) +
                                                      "\",\"execution_mode\":\"background\"}"));
            } else if (!state->dispatched_b) {
                state->dispatched_b = true;
                RespondSse(client_fd, ToolUseTurn("toolu_agent_b", "agent",
                                                  "{\"title\":\"" + std::string(kTitleB) + "\",\"prompt\":\"" +
                                                      std::string(kAgentB) +
                                                      "\",\"execution_mode\":\"background\"}"));
            } else {
                RespondSse(client_fd, TextTurn(LongBody()));
            }
        }
        closesocket(client_fd);
    };
    std::thread([listener, state, serve_connection]() {
        while (true) {
            sockaddr_in client{};
            int client_len = sizeof(client);
            const SOCKET_T client_fd = ::accept(listener, reinterpret_cast<sockaddr*>(&client), &client_len);
            if (client_fd == kBadSocket) {
                return;
            }
            std::thread(serve_connection, client_fd).detach();
        }
    }).detach();
    return port;
}

void SetEnv(const std::wstring& name, const std::wstring& value) {
    _wputenv((name + L"=" + value).c_str());
}

// ---- 控制台布景与改字号 ----

void SetSceneSize(int width, int window_rows, int buffer_rows) {
    SMALL_RECT small{0, 0, 1, 1};
    SetConsoleWindowInfo(g_conout, TRUE, &small);
    SetConsoleScreenBufferSize(g_conout, COORD{static_cast<SHORT>(width), static_cast<SHORT>(buffer_rows)});
    SMALL_RECT window{0, 0, static_cast<SHORT>(width - 1), static_cast<SHORT>(window_rows - 1)};
    SetConsoleWindowInfo(g_conout, TRUE, &window);
}

// 十轮改字号(疑案复现尝试):原字号与大小字号来回切,末了还原。
void FontHammer(int rounds, const char* tag) {
    CONSOLE_FONT_INFOEX original{};
    original.cbSize = sizeof(original);
    GetCurrentConsoleFontEx(g_conout, FALSE, &original);
    bool big = false;
    for (int i = 0; i < rounds; ++i) {
        CONSOLE_FONT_INFOEX fx{};
        fx.cbSize = sizeof(fx);
        fx.dwFontSize.X = 0;
        fx.dwFontSize.Y = static_cast<SHORT>(big ? 20 : 12);
        fx.FontFamily = original.FontFamily;
        fx.FontWeight = original.FontWeight;
        SetCurrentConsoleFontEx(g_conout, FALSE, &fx);
        big = !big;
        Sleep(150);
    }
    SetCurrentConsoleFontEx(g_conout, FALSE, &original);
    Log(std::string("INFO: 改字号 ") + tag + " ×" + std::to_string(rounds) + " 完成(已还原原字号)");
}

struct ChildGuard {
    PROCESS_INFORMATION pi{};
    ~ChildGuard() {
        if (pi.hProcess != nullptr) {
            TerminateProcess(pi.hProcess, 0);
            WaitForSingleObject(pi.hProcess, 3000);
            CloseHandle(pi.hProcess);
        }
    }
};

bool SpawnChild(const std::wstring& exe_path, const std::wstring& workdir, ChildGuard& guard) {
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = g_conin;
    si.hStdOutput = g_conout;
    si.hStdError = g_conout;
    std::wstring cmdline = L"\"" + exe_path + L"\" --yes";
    if (!CreateProcessW(exe_path.c_str(), cmdline.data(), nullptr, nullptr, TRUE, 0, nullptr, workdir.c_str(), &si,
                        &guard.pi)) {
        Check(false, "CreateProcess " + std::to_string(GetLastError()) + "(工作目录要用 Windows 式路径)");
        return false;
    }
    CloseHandle(guard.pi.hThread);
    FlushConsoleInputBuffer(g_conin);
    return true;
}

bool ChildAlive(const ChildGuard& guard) {
    DWORD code = STILL_ACTIVE;
    GetExitCodeProcess(guard.pi.hProcess, &code);
    return code == STILL_ACTIVE;
}

// ---- 一幕的标准流程 ----
// 布景(setup)由各幕自带;这里跑:起进程 -> 开场 composer -> 发 prompt ->
// 等收口 -> 断言 composer 帧在可视区 ->(可选 hook_after_turn)-> 等回流 ->
// 再断言 -> 干净退出(验退出码 0)。
struct ScenarioHooks {
    bool font_hammer_startup = false;   // 启动中改字号十轮
    bool font_hammer_running = false;   // 运行中改字号十轮
    bool font_hammer_reflow = false;    // 完成回流时改字号十轮
    bool scroll_up_before_reflow = false;  // 收口后把窗口上滚,等回流无补键回可见
};

void RunScenario(const std::string& name, const std::wstring& exe_path, const std::wstring& workdir,
                 const ScenarioHooks& hooks) {
    Log("==== 幕:" + name + " ====");
    if (g_reset_server_state) {
        g_reset_server_state();
    }
    ChildGuard guard;
    if (!SpawnChild(exe_path, workdir, guard)) {
        return;
    }
    if (hooks.font_hammer_startup) {
        FontHammer(10, "启动中");
    }
    Check(WaitForText("\xe9\x94\xae\xe5\x85\xa5\xe5\xb9\xb6\xe5\x9b\x9e\xe8\xbd\xa6", 30000) ||
              FindComposerInputRow() > 0,
          name + ": 开场空闲 composer 出现");  // 键入并回车
    Sleep(400);

    // 发一句正文:主模型派两只后台代理,随后长正文收口(主病灶现场)。
    SendText(kUserPromptReal);
    SendKey(VK_RETURN, L'\r', 0);
    if (hooks.font_hammer_running) {
        Sleep(800);  // 等它真的跑进流式
        FontHammer(10, "运行中");
    }

    // 两只后台代理的坞行出现。
    Check(WaitForText(kTitleA, 20000), name + ": 后台代理甲坞行出现");
    Check(WaitForText(kTitleB, 20000), name + ": 后台代理乙坞行出现");
    // 回合收口:长正文最后一行 + 统计行。
    Check(WaitForText(kBodyTail, 30000), name + ": 长正文收口句出现");
    Sleep(1200);  // 收口到 ReadLine 重画之间的空窗

    {
        int rule_row = -1;
        int frame_bottom = -1;
        std::string why;
        const bool ok = ComposerFrameInViewport(10000, &rule_row, &frame_bottom, &why);
        Check(ok, name + ": 回合收口后 composer 帧(顶边/输入框/状态栏/代理坞)全在可视窗口——" + why);
        DumpViewport(name + "-after-turn");
    }
    Check(ChildAlive(guard), name + ": 收口后进程仍活");

    if (hooks.scroll_up_before_reflow) {
        // 滚动缓冲区非底部:窗口上滚 8 行(不碰内容),等回流轮自己收口后
        // composer 必须无补键重新可见。
        CONSOLE_SCREEN_BUFFER_INFO info{};
        if (GetConsoleScreenBufferInfo(g_conout, &info) && info.srWindow.Top >= 8) {
            SMALL_RECT up = info.srWindow;
            up.Top -= 8;
            up.Bottom -= 8;
            SetConsoleWindowInfo(g_conout, TRUE, &up);
            Log("INFO: 窗口上滚 8 行(现 " + std::to_string(WindowTop()) + ".." + std::to_string(WindowBottom()) +
                ")");
        } else {
            Log("INFO: 窗口顶不足 8 行,上滚改为尽力 1 行");
            SMALL_RECT up = info.srWindow;
            if (up.Top >= 1) {
                up.Top -= 1;
                up.Bottom -= 1;
                SetConsoleWindowInfo(g_conout, TRUE, &up);
            }
        }
    }

    // 等后台代理交卷 + 回流轮收口。
    if (hooks.font_hammer_reflow) {
        // 回流触发点:等甲乙交卷的通知出现,随即在回流轮里改字号。
        if (WaitForText("\xe5\x90\x8e\xe5\x8f\xb0\xe5\xad\x90\xe4\xbb\xa3\xe7\x90\x86\xe5\xae\x8c\xe6\x88\x90", 30000)) {
            FontHammer(10, "完成回流时");
        }
    }
    Check(WaitForText(kReflowDone, 60000), name + ": 回流轮收口正文出现");
    Sleep(1200);
    {
        int rule_row = -1;
        int frame_bottom = -1;
        std::string why;
        const bool ok = ComposerFrameInViewport(10000, &rule_row, &frame_bottom, &why);
        Check(ok, name + ": 回流收口后 composer 帧全在可视窗口——" + why);
    }
    Check(ChildAlive(guard), name + ": 回流后进程仍活");
    if (hooks.font_hammer_startup || hooks.font_hammer_running || hooks.font_hammer_reflow) {
        // 改字号幕的完成标准:任务不丢——字号来回切过后,两只代理的完成
        // 通知与回流正文都在(账没丢;已完成+已交付的坞行按规矩退场,不再
        // 数坞里的行),main 可回(敲一个回车空行,composer 还应答)。
        Check(FindLastRow(kTitleA) >= 0 && FindLastRow(kTitleB) >= 0,
              name + ": 改字号后两只后台代理的账都在(完成通知可寻,任务不丢)");
        SendKey(VK_RETURN, L'\r', 0);
        Sleep(600);
        Check(FindComposerInputRow() > 0, name + ": 改字号后 main 仍可回(composer 应答)");
    }

    // 干净退出:exit 一行,等退出码 0(疑案"code 1"的自动对账)。
    SendText("exit");
    SendKey(VK_RETURN, L'\r', 0);
    if (WaitForSingleObject(guard.pi.hProcess, 15000) == WAIT_OBJECT_0) {
        DWORD code = 0;
        GetExitCodeProcess(guard.pi.hProcess, &code);
        Check(code == 0, name + ": 干净退出码 0(实得 " + std::to_string(code) + ")");
        CloseHandle(guard.pi.hProcess);
        guard.pi.hProcess = nullptr;
    } else {
        Check(false, name + ": 15 秒内没退出");
    }
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

    const int port = StartFakeAnthropicServer();
    if (port == 0) {
        Log("FAIL: fake server bind");
        return 1;
    }
    SetEnv(L"LUBANCODE_WIRE", L"anthropic");
    SetEnv(L"LUBANCODE_BASE_URL", L"http://127.0.0.1:" + std::to_wstring(port));
    SetEnv(L"LUBANCODE_API_KEY", L"viewport-driver");
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

    // 幕一:80×24 窄矮窗(缓冲 80×400,长缓冲 + 绝对定位画帧的老病灶现场)。
    SetSceneSize(80, 24, 400);
    RunScenario("80x24", exe_path, workdir, ScenarioHooks{});

    // 幕二:120×35 常规窗。
    SetSceneSize(120, 35, 400);
    RunScenario("120x35", exe_path, workdir, ScenarioHooks{});

    // 幕三:全屏(最大窗口;缓冲高度贴窗口——ConPTY/贴底滚内容的形态)。
    // 最大窗口尺寸按显示上限报,封顶 200×56——无人值守跑驱动器时弹出的
    // 控制台盖不住半个桌面,也别让超大窗把断言变成马拉松。
    {
        const COORD largest = GetLargestConsoleWindowSize(g_conout);
        const int w = largest.X > 40 ? (std::min)(static_cast<int>(largest.X), 200) : 120;
        const int h = largest.Y > 20 ? (std::min)(static_cast<int>(largest.Y), 56) : 40;
        SetSceneSize(w, h, h);
        Log("INFO: 全屏幕 " + std::to_string(w) + "x" + std::to_string(h));
    }
    RunScenario("fullscreen", exe_path, workdir, ScenarioHooks{});

    // 幕四:改字号三时机各十轮(启动中/运行中/完成回流时)。
    SetSceneSize(120, 30, 400);
    {
        ScenarioHooks hooks;
        hooks.font_hammer_startup = true;
        RunScenario("font-startup", exe_path, workdir, hooks);
    }
    SetSceneSize(120, 30, 400);
    {
        ScenarioHooks hooks;
        hooks.font_hammer_running = true;
        RunScenario("font-running", exe_path, workdir, hooks);
    }
    SetSceneSize(120, 30, 400);
    {
        ScenarioHooks hooks;
        hooks.font_hammer_reflow = true;
        RunScenario("font-reflow", exe_path, workdir, hooks);
    }

    // 幕五:滚动缓冲区非底部(收口后上滚,回流轮无补键带回 composer)。
    SetSceneSize(120, 30, 400);
    {
        ScenarioHooks hooks;
        hooks.scroll_up_before_reflow = true;
        RunScenario("scroll-nonbottom", exe_path, workdir, hooks);
    }

    Log(g_failures == 0 ? "ALL PASS" : ("FAILURES: " + std::to_string(g_failures)));
    FreeConsole();
    return g_failures == 0 ? 0 : 1;
}
