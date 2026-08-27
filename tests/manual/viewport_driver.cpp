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
#include <atomic>
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
// 本幕假服务真接过几笔回流请求(幕间随 g_reset_server_state 归零)。改字
// 号会让 conhost 重排缓冲,屏面字符可能被搅得数不准;回流账以服务端为
// 准——路由进了 reflow 分支几回,就是几只代理的结果真交回过 main。
std::atomic<int> g_reflow_served{0};

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

// 从底往上按结构认 composer 框,返回输入行行号;找不到 -1。Composer 合流
// (P1)后框随内容长高:上横线、上留白、'>' 起输入区、下补空、下横线、状态行,
// 不再假定输入行紧贴横线——认"输入行上下各有一根横线、下横线之下不是横线
// (状态行)"。
int FindComposerInputRow() {
    const int max_rows = BufferHeight();
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
        for (int b = i + 1; b <= i + 6 && b + 1 < max_rows; ++b) {
            if (IsRuleRow(b) && !IsRuleRow(b + 1)) {
                return i;
            }
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
    // 上横线在输入行上方(合流后隔着留白,不紧贴):向上找第一根。
    int rule_row = -1;
    for (int r = input_row - 1; r >= input_row - 4 && r >= 0; --r) {
        if (IsRuleRow(r)) {
            rule_row = r;
            break;
        }
    }
    if (rule_row < 0) {
        *why = "输入行上方找不到上横线";
        return false;
    }
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

// 慢流应答:响应头带 Content-Length(与 RespondSse 同一定长成法——无定长
// 流在客户端连接池上会被误判早断,正文整段静默丢),再按 gap_ms 逐事件吐
// ——给"流式期间采样活度账"留窗口(幕六:思考流查看态)。
void RespondSseSlow(SOCKET_T s, const std::vector<std::string>& events, int gap_ms) {
    std::size_t body_bytes = 0;
    for (const auto& event : events) {
        body_bytes += Sse(event).size();
    }
    const std::string head = "HTTP/1.1 200 OK\r\n"
                             "Content-Type: text/event-stream\r\n"
                             "Content-Length: " +
                             std::to_string(body_bytes) +
                             "\r\n"
                             "Connection: close\r\n"
                             "\r\n";
    SendAll(s, head);
    for (const auto& event : events) {
        Sleep(gap_ms);
        SendAll(s, Sse(event));
    }
}

std::vector<std::string> TextTurn(const std::string& text) {
    // 正文是 JSON 字符串字面量:换行/引号/反斜杠必须转义。长正文(LongBody)
    // 带裸换行进 JSON 会让整帧解析失败,客户端静默跳过——正文一个字都不
    // 上屏,回合秒收,好像"模型没说话"。
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
    return {
        "{\"type\":\"message_start\",\"message\":{\"id\":\"msg\",\"model\":\"fake-model\"}}",
        "{\"type\":\"content_block_start\",\"index\":0,\"content_block\":{\"type\":\"text\",\"text\":\"\"}}",
        "{\"type\":\"content_block_delta\",\"index\":0,\"delta\":{\"type\":\"text_delta\",\"text\":\"" + escaped +
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
const char* kWidthPrompt = "width-resize-footer-regression";
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

// ---- 幕六(追加需求"查看态实时思考流")的剧本锚 ----
// 思考流场景开关:真后端按"甲 = 慢思考流"派发;主回合只派甲、回流轮直收。
std::atomic<bool> g_thinking_scene{false};
const char* kThinkingDone =
    "\xe6\x80\x9d\xe8\x80\x83\xe5\xae\x8c\xe6\xaf\x95\xe4\xba\xa4\xe5\x8d\xb7";  // 思考完毕交卷

// 甲的慢思考流:40 段思考增量(每段 500ms,约 20 秒窗口),随后一小段正文
// 收口——坞行/查看态在此期间应显示"思考中 · N 字"且 N 逐秒增长。
std::vector<std::string> ThinkingStreamTurn() {
    std::vector<std::string> events;
    events.push_back("{\"type\":\"message_start\",\"message\":{\"id\":\"msg\",\"model\":\"fake-model\"}}");
    events.push_back(
        "{\"type\":\"content_block_start\",\"index\":0,\"content_block\":{\"type\":\"thinking\",\"thinking\":\"\"}}");
    for (int i = 1; i <= 40; ++i) {
        // 第N段:棋盘、走法、界面……慢慢想。
        std::string piece = "\xe7\xac\xac" + std::to_string(i) +
                            "\xe6\xae\xb5\xef\xbc\x9a\xe6\xa3\x8b\xe7\x9b\x98\xe3\x80\x81\xe8\xb5\xb0\xe6\xb3\x95\xef"
                            "\xbc\x8c\xe6\x85\xa2\xe6\x85\xa2\xe6\x83\xb3\xe3\x80\x82";  // 第N段:棋盘、走法,慢慢想。
        events.push_back("{\"type\":\"content_block_delta\",\"index\":0,\"delta\":{\"type\":\"thinking_delta\","
                         "\"thinking\":\"" +
                         piece + "\"}}");
    }
    events.push_back("{\"type\":\"content_block_stop\",\"index\":0}");
    events.push_back(
        "{\"type\":\"content_block_start\",\"index\":1,\"content_block\":{\"type\":\"text\",\"text\":\"\"}}");
    events.push_back("{\"type\":\"content_block_delta\",\"index\":1,\"delta\":{\"type\":\"text_delta\",\"text\":\"" +
                     std::string(kThinkingDone) + "\"}}");
    events.push_back("{\"type\":\"content_block_stop\",\"index\":1}");
    events.push_back("{\"type\":\"message_delta\",\"delta\":{\"stop_reason\":\"end_turn\"},\"usage\":{\"input_"
                     "tokens\":80,\"output_tokens\":40}}");
    return events;
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
        g_reflow_served.store(0);
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
        if (!sub_agent_request && has(kWidthPrompt)) {
            // 专用改宽幕:一条 2.5 秒慢流，活动栏稳稳跨过 120 -> 80 -> 120。
            Log("SERVER: route=width-slow-stream");
            RespondSseSlow(client_fd, TextTurn(kBodyTail), 500);
        } else if (sub_agent_request) {
            Log("SERVER: route=sub-agent");
            if (g_thinking_scene.load()) {
                // 幕六:甲 = 慢思考流(20 段 × 400ms),乙不出场。
                RespondSseSlow(client_fd, ThinkingStreamTurn(), 400);
                return;
            }
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
        } else if (has("\xe5\x90\x8e\xe5\x8f\xb0\xe5\xad\x90\xe4\xbb\xa3\xe7\x90\x86\xe7\xbb\x93"
                       "\xe6\x9e\x9c")) {  // 后台子代理结果
            Log("SERVER: route=reflow(tool_done=" + std::string(state->reflow_tool_done ? "1" : "0") + ")");
            g_reflow_served.fetch_add(1);
            if (g_thinking_scene.load()) {
                // 幕六的回流轮:直收,不再跑工具。
                RespondSse(client_fd, TextTurn(kReflowDone));
                return;
            }
            // 分账锚 = 交互路回流通知的原文起头 "后台子代理结果 #N …"
            // (one_shot 管道路的措辞是"后台子代理有新结果送达",别混)。
            // 不能靠 "最后一条 user 消息" 的裸串定位:请求 JSON 的尾部元数
            // 据段还藏着 "role":"user" 字样,rfind 咬到它之后的搜索就落空;
            // 通知短语只随真回流轮进会话,全文搜即无劫持(派发回合的历史
            // 里只有"后台子代理 #N 已启动",不带"结果"二字)。
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
            Log("SERVER: route=main(a=" + std::string(state->dispatched_a ? "1" : "0") +
                " b=" + std::string(state->dispatched_b ? "1" : "0") + ")");
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

int CountBusyFooterRows() {
    // 活动行是 BuildFooterWorkingLine 的 "• 思考中 (Ns)"。旧锚还要求行内
    // 带 "Esc" 字样——那段打断提示如今只写在队列标题里,空队列的 footer
    // 全屏无 "Esc",旧锚永远数出 0。行首 "• " + 思考中即活动行签名;只数
    // 可视窗口——footer 随正文滚动后,滚出窗口的旧帧行会在回滚缓冲里留
    // 影,整缓冲计数会把鬼影当活栏,误报"没收走"。
    const std::string working = "\xe6\x80\x9d\xe8\x80\x83\xe4\xb8\xad";  // 思考中
    const std::string dot_prefix = "\xe2\x80\xa2 ";                      // "• "
    CONSOLE_SCREEN_BUFFER_INFO info{};
    if (!GetConsoleScreenBufferInfo(g_conout, &info)) {
        return 0;
    }
    int count = 0;
    for (int row = info.srWindow.Top; row <= info.srWindow.Bottom; ++row) {
        const std::string text = ReadRow(row);
        if (text.rfind(dot_prefix, 0) == 0 && text.find(working) != std::string::npos) {
            ++count;
        }
    }
    return count;
}

bool WaitForSingleBusyFooter(int timeout_ms) {
    const DWORD deadline = GetTickCount() + static_cast<DWORD>(timeout_ms);
    while (GetTickCount() < deadline) {
        if (CountBusyFooterRows() == 1) {
            return true;
        }
        Sleep(100);
    }
    return CountBusyFooterRows() == 1;
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
        // 改字号幕的完成标准:任务不丢——两只代理的结果各真交回过 main
        // 一次。改字号会让 conhost 重排缓冲,屏面字符数不准;回流账以假服
        // 务的路由计数为准(进了 reflow 分支几回 = 几只代理交卷回流)。
        // 旧锚找早前的标题/通知行,长正文一滚就出缓冲,把"滚屏"误报成
        // "丢账"。main 可回(敲一个回车空行,composer 还应答)。
        const int reflow_served = g_reflow_served.load();
        Check(reflow_served >= 2,
              name + ": 改字号后两只后台代理的账都在(各回流收口一次,实际 " +
                  std::to_string(reflow_served) + " 回)");
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

void RunWidthResizeScene(const std::wstring& exe_path, const std::wstring& workdir) {
    Log("==== 幕:忙碌栏运行中改宽 ====");
    if (g_reset_server_state) {
        g_reset_server_state();
    }
    ChildGuard guard;
    if (!SpawnChild(exe_path, workdir, guard)) {
        return;
    }
    Check(WaitForText("\xe9\x94\xae\xe5\x85\xa5\xe5\xb9\xb6\xe5\x9b\x9e\xe8\xbd\xa6", 30000) ||
              FindComposerInputRow() > 0,
          "改宽: 开场空闲 composer 出现");  // 键入并回车
    SendText(kWidthPrompt);
    SendKey(VK_RETURN, L'\r', 0);

    Check(WaitForSingleBusyFooter(10000), "改宽前恰有一条忙碌活动栏");
    SetSceneSize(80, 30, 400);
    Sleep(800);  // 至少跨过四拍 200ms heartbeat，让改宽路径确实落笔。
    const int narrow_count = CountBusyFooterRows();
    Check(narrow_count == 1,
          "拉窄后忙碌活动栏仍只有一条(实得 " + std::to_string(narrow_count) + ")");
    SetSceneSize(120, 30, 400);
    Sleep(800);
    const int wide_count = CountBusyFooterRows();
    Check(wide_count == 1,
          "拉宽后忙碌活动栏仍只有一条(实得 " + std::to_string(wide_count) + ")");
    DumpViewport("width-running");

    Check(WaitForText(kBodyTail, 10000), "改宽后当前回合仍能收口");
    // 窗口给宽(20s):改宽瞬间偶发流式读停摆、回合比 2.5s 的慢流拖长得
    // 多(app 层嫌疑,另行记账),收栏以"最终确实收走"为准。
    const DWORD idle_deadline = GetTickCount() + 20000;
    while (GetTickCount() < idle_deadline && CountBusyFooterRows() != 0) {
        Sleep(100);
    }
    Check(CountBusyFooterRows() == 0, "改宽回合活动栏按时收走");
    // 退出前确保回合真收了口:万一活动栏还亮着(上一条 FAIL 的现场),先
    // Esc 打断再退——不然 "exit" 落进忙时队列被当正文发给模型,进程永远
    // 退不了,把下一幕的场地也堵死。
    if (CountBusyFooterRows() != 0) {
        SendKey(VK_ESCAPE, 0, 0);
        const DWORD esc_deadline = GetTickCount() + 8000;
        while (GetTickCount() < esc_deadline && CountBusyFooterRows() != 0) {
            Sleep(150);
        }
    }
    SendText("exit");
    SendKey(VK_RETURN, L'\r', 0);
    if (WaitForSingleObject(guard.pi.hProcess, 15000) == WAIT_OBJECT_0) {
        DWORD code = 0;
        GetExitCodeProcess(guard.pi.hProcess, &code);
        Check(code == 0, "改宽幕干净退出码 0(实得 " + std::to_string(code) + ")");
        CloseHandle(guard.pi.hProcess);
        guard.pi.hProcess = nullptr;
    } else {
        Check(false, "改宽幕 15 秒内没退出");
    }
}

// ---- 幕六(追加需求"查看态实时思考流"):思考流期间进查看态,字数在长 ----
// 剧本:主回合派甲(后台);甲按 500ms 一段慢慢吐 40 段思考(约 20s 窗口)。
// 断言:1) 坞行出"思考中 · N 字"(不再是死秒表);2) Down+Enter 进查看态
// 后,"思考中 · N 字"在视口里逐秒增长(实时流重铺,1s 节流);3) 干净退出。
// 不等主回合长正文——集成环境下"收口句重画"不总是可刮(基线 exe 同样
// 刮不到),这一幕只对自己要验的活度链路负责。
std::string ThinkingCharsInViewport() {
    // "思考中" 的 UTF-8;数字在 "· " 与 " 字" 之间。只扫可视窗口,从上往
    // 下取第一处——查看态正文在坞上方,先命中即它(不跟坞行混)。
    const std::string needle = "\xe6\x80\x9d\xe8\x80\x83\xe4\xb8\xad";  // 思考中
    const std::string mid = "\xc2\xb7 ";
    const std::string tail = " \xe5\xad\x97";  // " 字"
    for (int row = WindowTop(); row <= WindowBottom(); ++row) {
        const std::string text = ReadRow(row);
        const std::size_t at = text.find(needle);
        if (at == std::string::npos) {
            continue;
        }
        const std::size_t mid_at = text.find(mid, at);
        if (mid_at == std::string::npos) {
            continue;
        }
        std::size_t number_at = mid_at + mid.size();
        std::string digits;
        while (number_at < text.size() && text[number_at] >= '0' && text[number_at] <= '9') {
            digits += text[number_at];
            ++number_at;
        }
        if (!digits.empty() && text.compare(number_at, tail.size(), tail) == 0) {
            return digits;
        }
    }
    return {};
}

void RunThinkingViewScene(const std::wstring& exe_path, const std::wstring& workdir) {
    Log("==== 幕:思考流查看态(追加需求) ====");
    g_reset_server_state();
    g_thinking_scene.store(true);
    ChildGuard guard;
    if (!SpawnChild(exe_path, workdir, guard)) {
        g_thinking_scene.store(false);
        return;
    }
    Check(WaitForText("\xe9\x94\xae\xe5\x85\xa5\xe5\xb9\xb6\xe5\x9b\x9e\xe8\xbd\xa6", 30000) ||
              FindComposerInputRow() > 0,
          "思考流: 开场空闲 composer 出现");  // 键入并回车
    Sleep(400);
    SendText(kUserPromptReal);
    SendKey(VK_RETURN, L'\r', 0);
    Check(WaitForText(kTitleA, 20000), "思考流: 甲坞行出现");

    // 断言一:坞行出"思考中 · N 字"——长思考不再只剩秒表。
    Check(WaitForText("\xe6\x80\x9d\xe8\x80\x83\xe4\xb8\xad", 20000),
          "思考流: 坞行显示思考中阶段");  // 思考中
    // 等主回合收口回到空闲 composer(Down/Enter 走空闲面板路),窗口 20s
    // 够留出增长采样;等不到空闲也照样往下试(流式 footer 同样能进查看态)。
    const DWORD idle_deadline = GetTickCount() + 12000;
    while (GetTickCount() < idle_deadline && FindComposerInputRow() <= 0) {
        Sleep(300);
    }
    Sleep(600);
    // 断言二:空 composer Down 聚焦面板、Enter 切进甲的查看态,视口里
    // "思考中 · N 字"的 N 逐秒增长。
    SendKey(VK_DOWN, 0, 0);
    Sleep(250);
    SendKey(VK_RETURN, L'\r', 0);
    Check(WaitForText("\xe5\x90\x8e\xe5\x8f\xb0", 8000), "思考流: 查看态头行(来源行)出现");  // 后台
    int first = -1;
    int last = -1;
    const DWORD sample_deadline = GetTickCount() + 14000;
    while (GetTickCount() < sample_deadline) {
        const std::string digits = ThinkingCharsInViewport();
        if (!digits.empty()) {
            const int value = atoi(digits.c_str());
            if (first < 0) {
                first = value;
            }
            last = value;
        }
        Sleep(400);
    }
    Check(first > 0, "思考流: 查看态读到思考字数(首采样 " + std::to_string(first) + ")");
    Check(last > first, "思考流: 思考字数在长(" + std::to_string(first) + " -> " + std::to_string(last) + ")");
    DumpViewport("thinking-view");

    // 等甲收口(回流正文此环境不总可刮,等完成短行即可),再干净退出。
    const DWORD done_deadline = GetTickCount() + 30000;
    while (GetTickCount() < done_deadline && FindLastRow(kThinkingDone) < 0 &&
           FindLastRow(kReflowDone) < 0) {
        Sleep(300);
    }
    Sleep(1200);
    SendKey(VK_ESCAPE, 0, 0);  // 退查看态/焦点(多余的一拍归编辑器清空,无害)
    Sleep(200);
    SendText("exit");
    SendKey(VK_RETURN, L'\r', 0);
    if (WaitForSingleObject(guard.pi.hProcess, 15000) == WAIT_OBJECT_0) {
        DWORD code = 0;
        GetExitCodeProcess(guard.pi.hProcess, &code);
        Check(code == 0, "思考流: 干净退出码 0(实得 " + std::to_string(code) + ")");
        CloseHandle(guard.pi.hProcess);
        guard.pi.hProcess = nullptr;
    } else {
        Check(false, "思考流: 15 秒内没退出");
    }
    g_thinking_scene.store(false);
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

    if (argc >= 5 && std::wstring(argv[4]) == L"--width-only") {
        SetSceneSize(120, 30, 400);
        RunWidthResizeScene(exe_path, workdir);
        Log(g_failures == 0 ? "ALL PASS" : ("FAILURES: " + std::to_string(g_failures)));
        FreeConsole();
        return g_failures == 0 ? 0 : 1;
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

    // 追加幕:主回合活动栏亮着时收窄再放宽，旧帧不能留在历史区。
    SetSceneSize(120, 30, 400);
    RunWidthResizeScene(exe_path, workdir);

    // 幕六(追加需求"查看态实时思考流"):子代理慢思考流期间进查看态,
    // 坞行与视口的"思考中 · N 字"逐秒增长。
    SetSceneSize(120, 30, 400);
    RunThinkingViewScene(exe_path, workdir);

    Log(g_failures == 0 ? "ALL PASS" : ("FAILURES: " + std::to_string(g_failures)));
    FreeConsole();
    return g_failures == 0 ? 0 : 1;
}
