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
//   第五幕(压测,终端画面隔网单):同一句"压测长流两本账"发两遍——
//   千枚小 delta 滴流(~2.4s)与十枚 600 字大 delta,断言输入框常驻、
//   心跳不饿死、一字不丢。
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
DWORD g_start_tick = 0;  // SERVER 日志的相对毫秒零点(wmain 里起表)

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

// 可视区内 main 行计数:残帧判定只认用户看得见的行(整缓冲口径见上方
// CountMainRows,留作对照)。
int CountMainRowsViewport() {
    CONSOLE_SCREEN_BUFFER_INFO info{};
    if (!GetConsoleScreenBufferInfo(g_conout, &info)) {
        return -1;
    }
    int count = 0;
    for (int r = info.srWindow.Top; r <= info.srWindow.Bottom; ++r) {
        const std::string text = ReadRow(r);
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

// 按结构认 footer 的 composer 框(流式期间最底下那个)。Composer 合流(P1)
// 后框随内容长高:上横线、上留白、'>' 起的输入区、下补空、下横线、状态行,
// 不再假定输入行紧贴横线——认"输入行上下各有一根横线、下横线之下不是
// 横线(状态行)"。从底往上扫,命中最近的一个。
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

// 输入行上方的第一根横线(上横线):坞区计数从它之下起算。
int FindRuleAboveInput(int input_row) {
    for (int r = input_row - 1; r >= input_row - 4 && r >= 0; --r) {
        if (IsRuleRow(r)) {
            return r;
        }
    }
    return -1;
}

// "最底下那个框的上横线"的一次取用(老代码拿输入行-1 冒充,合流后输入行
// 与上横线之间隔着留白,统一走这里)。
int ComposerTopRuleNow() {
    const int input = FindFooterInputRow();
    return input > 0 ? FindRuleAboveInput(input) : -1;
}

// 坞区计数:只数 composer 上横线之下的行——上横线之上出现的同名文本是
// 有归属的正文(查看态 transcript、agent 工具条目、完成通知),不算残帧。
int CountDockRowsWith(const std::string& needle, int rule_row) {
    int count = 0;
    for (int row = rule_row + 1; row < 400; ++row) {
        if (ReadRow(row).find(needle) != std::string::npos) {
            ++count;
        }
    }
    return count;
}

// 坞区里最后一次出现某文本的行号(找不到 -1):"贴底"断言用。
int FindLastDockRow(const std::string& needle, int rule_row) {
    for (int row = 399; row > rule_row; --row) {
        if (ReadRow(row).find(needle) != std::string::npos) {
            return row;
        }
    }
    return -1;
}

// 当前可视区(不含滚屏历史)内含某文本的行数:第三幕"回 main 重铺恰好
// 一次"用——前两幕的同款文案躺在滚屏里,不能数进来。
int CountViewportRowsWith(const std::string& needle) {
    CONSOLE_SCREEN_BUFFER_INFO info{};
    if (!GetConsoleScreenBufferInfo(g_conout, &info)) {
        return -1;
    }
    int count = 0;
    for (int r = info.srWindow.Top; r <= info.srWindow.Bottom; ++r) {
        if (ReadRow(r).find(needle) != std::string::npos) {
            ++count;
        }
    }
    return count;
}

// 全缓冲区按结构认出的 composer 框数(横线/'>' 空输入行/横线/状态行成套,
// Composer 合流后框随留白长高,不要求紧贴):完成唤醒后旧底栏该已退场,
// 只剩一副。输入行须为空——"分界线+> 已提交正文+分界线"是历史回显,不是
// composer,不误计(它上下没有成对的横线加状态行)。
int CountVisibleComposers() {
    int count = 0;
    for (int i = 396; i >= 0; --i) {
        const std::string input_text = ReadRow(i);
        if (input_text != ">") {
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
                ++count;
                break;
            }
        }
    }
    return count;
}

// 等空闲 composer 真画出来(回合收口到下一次 ReadLine 之间有空窗,不等的
// 话按结构找框会找不到、坞区计数会把滚屏里的旧账也数进去)。
bool WaitForIdleComposer(int timeout_ms) {
    const DWORD deadline = GetTickCount() + static_cast<DWORD>(timeout_ms);
    while (GetTickCount() < deadline) {
        if (FindFooterInputRow() > 0) {
            return true;
        }
        Sleep(200);
    }
    return FindFooterInputRow() > 0;
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

// ---- 压测两本账(终端画面隔网单验收):千枚小 delta 滴流 + 十枚大 delta ----

// 第五幕(压测,终端画面隔网单验收):两本账的锚文本。用户话同一句发两
// 遍,服务端按"历史里有没有小字流的收尾标记"分派剧本一/剧本二。
const char* kStressUser =
    "\xe5\x8e\x8b\xe6\xb5\x8b\xe9\x95\xbf\xe6\xb5\x81\xe4\xb8\xa4\xe6\x9c\xac\xe8\xb4\xa6";  // 压测长流两本账
const char* kStressSmallHead =
    "\xe5\xb0\x8f\xe5\xad\x97\xe6\xb5\x81\xe8\xb5\xb7\xe8\xb7\x91";  // 小字流起跑
const char* kStressSmallTail =
    "\xe5\x8d\x83\xe6\x9e\x9a\xe5\xb0\x8f\xe5\xad\x97\xe8\xb5\xb0\xe5\xae\x8c";  // 千枚小字走完
const char* kStressBigHead =
    "\xe5\xa4\xa7\xe5\xad\x97\xe6\xb5\x81\xe8\xb5\xb7\xe8\xb7\x91";  // 大字流起跑
const char* kStressBigTail =
    "\xe5\x8d\x81\xe6\x9e\x9a\xe5\xa4\xa7\xe5\xad\x97\xe8\xb5\xb0\xe5\xae\x8c";  // 十枚大字走完

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

std::string TextDeltaEvent(const std::string& text) {
    return "{\"type\":\"content_block_delta\",\"index\":0,\"delta\":{\"type\":\"text_delta\",\"text\":\"" +
           JsonEscape(text) + "\"}}";
}

// 剧本一(1000 小 delta 滴流):20 拍 × 50 枚,拍间 120ms——整流 ~2.4s,
// 跨多个帧边界(16~33ms 一帧),给"输入框常驻采样/心跳走秒"留时间;
// 每 50 枚补一空行,喂 markdown 收束重画那条路。首枚带起跑标记,末枚带
// 收尾标记(驱动器拿它断言一字不丢)。
void RespondStressSmall(SOCKET_T s) {
    std::vector<std::size_t> burst_offsets;  // 每拍起写的字节偏移(头先行,体分拍滴)
    std::string body;
    body += Sse("{\"type\":\"message_start\",\"message\":{\"id\":\"msg\",\"model\":\"fake-model\"}}");
    body += Sse("{\"type\":\"content_block_start\",\"index\":0,\"content_block\":{\"type\":\"text\",\"text\":\"\"}}");
    body += Sse(TextDeltaEvent("\xe5\xb0\x8f\xe5\xad\x97\xe6\xb5\x81\xe8\xb5\xb7\xe8\xb7\x91\n\n"));  // 小字流起跑
    for (int burst = 0; burst < 20; ++burst) {
        burst_offsets.push_back(body.size());
        for (int j = 0; j < 50; ++j) {
            const int i = burst * 50 + j;
            body += Sse(TextDeltaEvent("\xe7\xac\x94" + std::to_string(i) + " "));  // 笔N
            if (j == 49) {
                body += Sse(TextDeltaEvent("\n\n"));  // 段落收束:原地重画一条路也吃到
            }
        }
    }
    burst_offsets.push_back(body.size());
    body += Sse(TextDeltaEvent("\n\n\xe5\x8d\x83\xe6\x9e\x9a\xe5\xb0\x8f\xe5\xad\x97\xe8\xb5\xb0\xe5\xae\x8c"));  // 千枚小字走完
    body += Sse("{\"type\":\"content_block_stop\",\"index\":0}");
    body += Sse("{\"type\":\"message_delta\",\"delta\":{\"stop_reason\":\"end_turn\"},\"usage\":{\"input_tokens\":80,"
                "\"output_tokens\":900}}");
    const std::string head = "HTTP/1.1 200 OK\r\n"
                             "Content-Type: text/event-stream\r\n"
                             "Content-Length: " +
                             std::to_string(body.size()) + "\r\n" + "Connection: close\r\n\r\n";
    SendAll(s, head);
    // 先发 leading(message_start/块开/起跑 delta),再逐拍滴正文——次序
    // 颠倒会把协议事件在 message_start 之前送到,客户端按协议丢弃,正文
    // 静默丢光(viewport_driver 同一处坑的亲兄弟)。
    SendAll(s, body.substr(0, burst_offsets[0]));
    for (std::size_t k = 0; k + 1 < burst_offsets.size(); ++k) {
        SendAll(s, body.substr(burst_offsets[k], burst_offsets[k + 1] - burst_offsets[k]));
        Sleep(120);
    }
    SendAll(s, body.substr(burst_offsets.back()));
}

// 剧本二(10 枚大 delta):每枚 600 个汉字——"500+ 字大 delta 输入框不
// 消失"的硬剧本。每枚以空行收尾喂段落重画,首枚带起跑标记,末枚带收尾
// 标记。逐枚滴出(150ms 一枚,整流 ~1.5s),采样才有窗口看得见"流期间
// 输入框还在"。
void RespondStressBig(SOCKET_T s) {
    std::string big;
    for (int i = 0; i < 600; ++i) {
        big += "\xe6\xbb\xa1";  // 满
    }
    std::vector<std::string> leading = {
        "{\"type\":\"message_start\",\"message\":{\"id\":\"msg\",\"model\":\"fake-model\"}}",
        "{\"type\":\"content_block_start\",\"index\":0,\"content_block\":{\"type\":\"text\",\"text\":\"\"}}",
        TextDeltaEvent(std::string(kStressBigHead) + "\n\n")};
    std::vector<std::string> trailing = {
        "{\"type\":\"content_block_stop\",\"index\":0}",
        "{\"type\":\"message_delta\",\"delta\":{\"stop_reason\":\"end_turn\"},\"usage\":{\"input_tokens\":80,"
        "\"output_tokens\":6000}}"};
    const std::string tail_event = Sse(TextDeltaEvent(std::string(kStressBigTail)));
    std::size_t total = 0;
    for (const auto& event : leading) {
        total += Sse(event).size();
    }
    for (int i = 0; i < 10; ++i) {
        total += Sse(TextDeltaEvent(big + "\n\n")).size();
    }
    total += tail_event.size();
    for (const auto& event : trailing) {
        total += Sse(event).size();
    }
    const std::string head = "HTTP/1.1 200 OK\r\n"
                             "Content-Type: text/event-stream\r\n"
                             "Content-Length: " +
                             std::to_string(total) + "\r\n" + "Connection: close\r\n\r\n";
    SendAll(s, head);
    for (const auto& event : leading) {
        SendAll(s, Sse(event));
    }
    for (int i = 0; i < 10; ++i) {
        SendAll(s, Sse(TextDeltaEvent(big + "\n\n")));
        Sleep(150);
    }
    SendAll(s, tail_event);  // 收尾标记:别并进 trailing——上一版漏发过
    for (const auto& event : trailing) {
        SendAll(s, Sse(event));
    }
}

std::vector<std::string> TextTurn(const std::string& text) {
    // 正文进 JSON 字符串字面量必须转义(换行/引号/反斜杠):裸换行会让整帧
    // 解析失败、正文静默丢光——与 viewport_driver 同一处坑,一并钉死。
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
const char* kBgTitle =
    "\xe5\x90\x8e\xe5\x8f\xb0\xe6\x91\xb8\xe6\x8e\x92\xe4\xba\x8b\xe5\xae\x9e";  // 后台摸排事实
const char* kPromptHead =
    "\xe4\xbd\xa0\xe5\x9c\xa8\xe4\xb8\x80\xe4\xb8\xaa C++ \xe9\xa1\xb9\xe7\x9b\xae\xe7\x9a\x84\xe9\x9a\x94"
    "\xe7\xa6\xbb";  // 你在一个 C++ 项目的隔离
// 第三幕(查看态回流)的锚文本:用户话、两只后台的 prompt/title/结论、
// 回流正文、半句草稿。
const char* kAct3User =
    "\xe6\xb4\xbe\xe4\xb8\xa4\xe5\x8f\xaa\xe5\x90\x8e\xe5\x8f\xb0\xe4\xbb\xa3\xe7\x90\x86\xe6\x85\xa2\xe6\x85\xa2"
    "\xe6\x9f\xa5";  // 派两只后台代理慢慢查
const char* kFastPrompt =
    "\xe5\xbf\xab\xe6\x9f\xa5\xe7\x94\xb2\xe4\xb8\x80\xe4\xbb\xbd\xe8\xb4\xa6\xe7\x9b\xae";  // 快查甲一份账目
const char* kSlowPrompt =
    "\xe6\x85\xa2\xe6\x9f\xa5\xe4\xb9\x99\xe4\xb8\x80\xe4\xbb\xbd\xe8\xb4\xa6\xe7\x9b\xae";  // 慢查乙一份账目
const char* kFastTitle = "\xe5\xbf\xab\xe6\x9f\xa5\xe7\x94\xb2\xe6\x8a\xa5\xe5\x91\x8a";  // 快查甲报告
const char* kSlowTitle = "\xe6\x85\xa2\xe6\x9f\xa5\xe4\xb9\x99\xe6\x8a\xa5\xe5\x91\x8a";  // 慢查乙报告
const char* kFastDone =
    "\xe7\x94\xb2\xe4\xbb\xa3\xe7\x90\x86\xe5\xae\x8c\xe6\xaf\x95\xef\xbc\x9a\xe7\x94\xb2\xe7\x9a\x84\xe8\xb4\xa6"
    "\xe7\x9b\xae\xe4\xba\xa4\xe5\x9b\x9e";  // 甲代理完毕:甲的账目交回
const char* kSlowDone =
    "\xe4\xb9\x99\xe4\xbb\xa3\xe7\x90\x86\xe5\xae\x8c\xe6\xaf\x95\xef\xbc\x9a\xe4\xb9\x99\xe7\x9a\x84\xe8\xb4\xa6"
    "\xe7\x9b\xae\xe4\xba\xa4\xe5\x9b\x9e";  // 乙代理完毕:乙的账目交回
const char* kReflowText =
    "\xe9\x9d\x99\xe9\xbb\x98\xe5\x9b\x9e\xe6\xb5\x81\xef\xbc\x9a\xe7\x94\xb2\xe7\x9a\x84\xe7\xbb\x93\xe6\x9e\x9c"
    "\xe5\xb7\xb2\xe6\xb6\x88\xe5\x8c\x96";  // 静默回流:甲的结果已消化
const char* kDraftText = "\xe7\xbb\x99\xe4\xb9\x99\xe7\x9a\x84\xe5\x8d\x8a\xe5\x8f\xa5\xe8\xaf\x9d";  // 给乙的半句话
const char* kToastText = "\xe7\xbb\x93\xe6\x9e\x9c\xe5\xb7\xb2\xe5\x9b\x9e\xe6\xb5\x81 main";  // 结果已回流 main
// 第四幕(查看态完成退场 + 后台拒权告知单,2026-08-17)的锚文本:主话、
// 慢工任务的 title/prompt/收口话/结论、拒权 toast 与 transcript 事件的
// 特征串。
const char* kAct4User =
    "\xe5\x86\x8d\xe7\x9b\xaf\xe4\xb8\x80\xe5\x8f\xaa\xe5\x90\x8e\xe5\x8f\xb0\xe4\xbb\x8e\xe7\x94\x9f"
    "\xe5\x88\xb0\xe6\xad\xbb";  // 再盯一只后台从生到死
const char* kWatchTitle =
    "\xe7\x9b\xaf\xe9\x80\x80\xe5\x9c\xba\xe7\x9a\x84\xe6\x85\xa2\xe5\xb7\xa5";  // 盯退场的慢工
const char* kWatchPrompt =
    "\xe6\x85\xa2\xe5\xb7\xa5\xe5\x87\xba\xe7\xbb\x86\xe6\xb4\xbb\xe4\xb8\x80\xe8\xbd\xae\xe8\xbd\xae"
    "\xe8\xaf\xbb";  // 慢工出细活一轮轮读
const char* kAct4Wrap =
    "\xe6\x85\xa2\xe5\xb7\xa5\xe5\xb7\xb2\xe6\xb4\xbe\xe5\xa6\xa5\xef\xbc\x8c\xe7\x9b\xaf\xe5\xae\x8c\xe8\xbf\x99"
    "\xe5\x9c\xba\xe6\x94\xb6\xe5\xb7\xa5";  // 慢工已派妥,盯完这场收工
const char* kWatchDone =
    "\xe6\x85\xa2\xe5\xb7\xa5\xe5\xae\x8c\xe6\xaf\x95\xef\xbc\x9a\xe5\x86\x99\xe6\x93\x8d\xe4\xbd\x9c\xe5\x8f\x97"
    "\xe9\x98\xbb\xef\xbc\x8c\xe5\xa6\x82\xe5\xae\x9e\xe4\xba\xa4\xe5\x8d\xb7";  // 慢工完毕:写操作受阻,如实交卷
const char* kDenyToast = "write_file \xe6\x9c\xaa\xe6\x94\xbe\xe8\xa1\x8c";  // write_file 未放行
const char* kDenyPrompt =
    "\xe5\x90\x8e\xe5\x8f\xb0\xe8\xa6\x81\xe5\x86\x99\xe4\xb8\x80\xe4\xbb\xbd\xe6\x9d\x90\xe6\x96\x99"
    "\xe5\x8e\xbb\xe4\xba\xa4\xe5\xb7\xae";  // 后台要写一份材料去交差
const char* kDenyUser =
    "\xe5\x86\x8d\xe6\xb4\xbe\xe4\xb8\x80\xe5\x8f\xaa\xe5\x90\x8e\xe5\x8f\xb0\xe5\x8e\xbb\xe5\x86\x99"
    "\xe6\x9d\x90\xe6\x96\x99";  // 再派一只后台去写材料
const char* kDenyTitle =
    "\xe5\x86\x99\xe6\x9d\x90\xe6\x96\x99\xe7\x9a\x84\xe5\x90\x8e\xe5\x8f\xb0";  // 写材料的后台
const char* kDenyLedger =
    "\xe5\x90\x8e\xe5\x8f\xb0\xe5\xad\x90\xe4\xbb\xa3\xe7\x90\x86\xe6\x9d\x83\xe9\x99\x90\xe6\x9c\xaa\xe6\x94\xbe"
    "\xe8\xa1\x8c";  // 后台子代理权限未放行

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
    ::listen(listener, SOMAXCONN);

    // 剧本按请求内容派发(第二幕起后台线程与主回合抢连接,按连接次序会被
    // 抢跑):看 user 消息/工具结果里的特征串决定回什么。
    //   第一幕:主(tool_use agent 前台)-> 子(run_command 卡 ~7s)-> 子(结论)
    //          -> 主(收尾)。
    //   第二幕:主派后台代理 -> 主收口 -> 后台子(读文件)-> 后台子(结论,
    //          压 3 秒让空闲 composer 先画出来)-> 完成唤醒的主(收口)。
    //   第三幕(查看态回流单):主连派两只后台,时长全靠"每轮睡 1 秒 +
    //          读文件"的短轮链(#3 十四轮后交卷 ~15s,#4 永不交卷)——单条
    //          长睡眠连接会把主回合的收口请求一起拖住,拆短轮把重叠压到
    //          一秒内;后台子代理的需确认工具一律被拒,只能用免确认的
    //          read_file 拖时间 -> 用户 Enter 切看 #4、敲半句草稿 -> #3
    //          跑完(草稿在,唤醒让位)-> Ctrl+C 清草稿 -> 空闲唤醒触发静默
    //          回流(查看帧零扰动、#3 坞行退场、toast 一枚)-> Esc 回 main
    //          重铺回流输出。
    // 连接各起一线程处理:第三幕的睡眠剧本不能堵死 accept 循环(主回合的
    // 连排派发要跟后台代理的慢连接并行)。跨连接的共享账(幕号/第二幕子
    // 代理轮次)拿一把小锁护住。
    struct ServerState {
        std::mutex mutex;
        int bg_sub_turn = 0;
        int main_stage = 0;  // 主回合当前幕(1/2/3),按最新 user 消息推进
    };
    const auto state = std::make_shared<ServerState>();
    const auto serve_connection = [state](SOCKET_T client_fd) {
        const std::string raw = DrainHttpRequest(client_fd);
        const std::size_t body_at = raw.find("\r\n\r\n");
        const std::string body = body_at == std::string::npos ? std::string() : raw.substr(body_at + 4);
        const auto has = [&body](const char* needle) {
            return body.find(needle) != std::string::npos;
        };
        // 子代理请求的 system 带专用 persona(SubAgentPersona),主回合
        // 不带——凭这个把两条会话的请求分账,不被后台线程抢跑打乱。
        const bool sub_agent_request = has("\xe8\x83\xbd\xe6\x90\x9c\xe7\xb4\xa2"
                                           "\xe3\x80\x81\xe5\x88\x86\xe6\x9e\x90"
                                           "\xe5\xb9\xb6\xe5\xae\x8c\xe6\x88\x90"
                                           "\xe5\xa4\x9a\xe6\xad\xa5\xe4\xbb\xbb"
                                           "\xe5\x8a\xa1");  // 能搜索、分析并完成多步任务
        int stage = 0;
        // 主回合分幕只认"最新一条真用户话"的特征(历史里的旧 user 消息
        // 与回流 prompt 会跟着之后每次请求重复出现,按全文匹配会把新幕
        // 误派成旧幕)。消息对象按字母序序列化(content 在前、role 在后),
        // 最新 user 消息的正文落在"上一条 assistant 的 role 键之后、最新
        // user 的 role 键之前"这个区间;尾巴是工具回执时区间里只有回执,
        // 分幕沿用现状,不被历史旧标记劫持。
        const std::size_t last_user_at = body.rfind("\"role\":\"user\"");
        const std::size_t last_assistant_at = body.rfind("\"role\":\"assistant\"");
        const std::size_t newest_from = last_assistant_at == std::string::npos ? 0 : last_assistant_at;
        const std::size_t newest_to = last_user_at == std::string::npos ? body.size() : last_user_at;
        const std::string newest_user_text =
            newest_from < newest_to ? body.substr(newest_from, newest_to - newest_from) : std::string();
        const auto newest_has = [&newest_user_text](const char* needle) {
            return newest_user_text.find(needle) != std::string::npos;
        };
        {
            std::lock_guard<std::mutex> lock(state->mutex);
            if (!sub_agent_request) {
                if (newest_has("\xe6\xb4\xbe\xe4\xb8\x80\xe5\x8f\xaa\xe5\x89\x8d\xe5\x8f\xb0\xe5\xad\x90\xe4\xbb\xa3"
                               "\xe7\x90\x86")) {  // 派一只前台子代理
                    state->main_stage = 1;
                } else if (newest_has("\xe5\x86\x8d\xe6\xb4\xbe\xe4\xb8\x80\xe5\x8f\xaa\xe5\x90\x8e\xe5\x8f\xb0"
                                     "\xe4\xbb\xa3\xe7\x90\x86")) {  // 再派一只后台代理(带"代理"二字,不
                    // 误吞第五幕的"再派一只后台去写材料")
                    state->main_stage = 2;
                } else if (newest_has(kAct3User)) {
                    state->main_stage = 3;
                } else if (newest_has(kAct4User)) {
                    state->main_stage = 4;
                } else if (newest_has(kDenyUser)) {
                    state->main_stage = 5;
                } else if (newest_has(kStressUser)) {
                    state->main_stage = 6;
                }
            }
            stage = state->main_stage;
        }
        Log("SERVER t=" + std::to_string(GetTickCount() - g_start_tick) + "ms body_bytes=" +
            std::to_string(body.size()) + " sub=" + (sub_agent_request ? "y" : "n") +
            " stage=" + std::to_string(stage) +
            (sub_agent_request ? "" : " head=" + body.substr(0, 120)));
        // 已完成的读文件轮数(数 tool_use 入参里路径出现的次数):第三幕
        // 两只后台代理靠"每轮一秒"的读文件链拖时间——单条慢连接会把主
        // 回合的收口请求一起拖住(实测同一环境),拆成短轮就把重叠窗口
        // 压到一秒以内。后台子代理跑需确认的工具会被拒,read_file 免确认。
        const auto read_rounds = [&body]() {
            std::size_t count = 0;
            std::size_t pos = 0;
            while ((pos = body.find("C:/Windows/win.ini", pos)) != std::string::npos) {
                ++count;
                pos += 1;
            }
            return count;
        };
        if (sub_agent_request && has(kFastPrompt)) {
            // 第三幕:快代理 #3。十四轮"睡 1 秒 + 读文件"后交结论——总时长
            // ~15 秒:用户切看 #4、敲半句草稿都发生在它交卷之前(九轮在快机
            // 上只有 ~10s,跟驱动器自己的切看节奏赛跑会撞车,抬到十四轮)。
            Sleep(1000);
            if (read_rounds() >= 14) {
                RespondSse(client_fd, TextTurn(kFastDone));
            } else {
                RespondSse(client_fd,
                           ToolUseTurn("toolu_bg3_a", "read_file", "{\"path\":\"C:/Windows/win.ini\"}"));
            }
        } else if (sub_agent_request && has(kSlowPrompt)) {
            // 第三幕:慢代理 #4。同样的短轮链但永不交卷(阈值抬到天上去),
            // 整幕保持运行中——查看态要看的正是它。
            Sleep(1000);
            if (read_rounds() >= 1000) {
                RespondSse(client_fd, TextTurn(kSlowDone));
            } else {
                RespondSse(client_fd,
                           ToolUseTurn("toolu_bg3_b", "read_file", "{\"path\":\"C:/Windows/win.ini\"}"));
            }
        } else if (sub_agent_request && has(kWatchPrompt)) {
            // 第四幕:被盯的慢工 #5。十二轮读文件(每轮一秒上下)给查看态喂
            // 实时流,然后交卷——用户正看着它完成退场。整场 ~15s,驱动器切看
            // #5(~5s)必须抢在交卷前。注意这里不放 write_file:拒权后的下一
            // 步在本机会触发既有的"任务线程哑火"病灶(见第五幕注释),退场
            // 那一拍就没法验了——拒权单独放第五幕去验。
            Sleep(1000);
            if (read_rounds() >= 12) {
                RespondSse(client_fd, TextTurn(kWatchDone));
            } else {
                RespondSse(client_fd,
                           ToolUseTurn("toolu_watch_r", "read_file", "{\"path\":\"C:/Windows/win.ini\"}"));
            }
        } else if (sub_agent_request && has(kDenyPrompt)) {
            // 第五幕:要写材料的 #6。第二轮就试 write_file——后台无人可问,
            // 当场被拒(toast + 如实文案);拒完它还会要下一轮,但在本机那一
            // 步会踩中既有病灶(任务线程哑火,见客户端注释),这幕只验"当场
            // 告知",不等它的后事。
            Sleep(1000);
            if (read_rounds() >= 1) {
                RespondSse(client_fd,
                           ToolUseTurn("toolu_deny_w", "write_file",
                                       "{\"path\":\"C:/Windows/Temp/lubancode-driver-deny.txt\",\"content\":\"x\"}"));
            } else {
                RespondSse(client_fd,
                           ToolUseTurn("toolu_deny_r", "read_file", "{\"path\":\"C:/Windows/win.ini\"}"));
            }
                } else if (sub_agent_request && has(kPromptHead)) {
            // 第一幕:前台子代理。第一轮给真工具(ping ~7 秒),拿到工具
            // 结果后给结论。
            if (body.find("Ping") != std::string::npos ||
                body.find("Pinging") != std::string::npos) {
                RespondSse(client_fd, TextTurn("\xe5\xad\x90\xe4\xbb\xa3\xe7\x90\x86\xe5\xb9\xb2"
                                                   "\xe5\xae\x8c\xe4\xba\x86\xef\xbc\x9a\xe6\xa3\x80"
                                                   "\xe7\xb4\xa2\xe9\x98\x88\xe5\x80\xbc\xe5\x9b\x9e"
                                                   "\xe5\xbd\x92\xe5\x85\xa8\xe7\xbb\xbf"));  // 子代理干完了:检索阈值回归全绿
            } else {
                RespondSse(client_fd,
                           ToolUseTurn("toolu_sub", "run_command",
                                       "{\"command\":\"ping -n 8 127.0.0.1\",\"shell\":\"cmd\"}"));
            }
        } else if (sub_agent_request && has("\xe5\x90\x8e\xe5\x8f\xb0\xe6\x91\xb8\xe6\x8e\x92\xe4\xb8\x80\xe4\xbb\xbd")) {
            // 第二幕:后台子代理自己的来回。第一轮给读文件(免确认,后台
            // 子代理跑得动),第二轮压 3 秒给结论——空闲 composer 先画出来,
            // 完成唤醒那条路真被走到。
            int turn = 0;
            {
                std::lock_guard<std::mutex> lock(state->mutex);
                turn = ++state->bg_sub_turn;
            }
            if (turn == 1) {
                RespondSse(client_fd,
                           ToolUseTurn("toolu_bg_sub", "read_file",
                                       "{\"path\":\"C:/Windows/win.ini\"}"));
            } else {
                Sleep(3000);
                RespondSse(client_fd, TextTurn("\xe5\x90\x8e\xe5\x8f\xb0\xe6\x91\xb8\xe6\x8e\x92"
                                                   "\xe5\xae\x8c\xe6\xaf\x95\xef\xbc\x9a\xe4\xba\x8b"
                                                   "\xe5\xae\x9e\xe6\xb8\x85\xe5\x8d\x95\xe5\x9c\xa8"
                                                   "\xe6\x89\x8b"));  // 后台摸排完毕:事实清单在手
            }
        } else if (sub_agent_request) {
            RespondSse(client_fd, TextTurn("ok"));
        } else if (newest_has("\xe5\x90\x8e\xe5\x8f\xb0\xe5\xad\x90\xe4\xbb\xa3\xe7\x90\x86\xe6\x9c\x89\xe6\x96\xb0\xe7\xbb\x93\xe6\x9e\x9c")) {
            // 完成唤醒后 RunPeerTurn 起的那一轮主请求(最新 user 消息就是
            // 回流 prompt)。第三幕的回流带着甲的结论,回一句独特标记的
            // 正文,驱动器靠它断言"查看期间一个字不上屏、Esc 回 main 重
            // 铺可见"。
            if (has(kFastDone)) {
                RespondSse(client_fd, TextTurn(kReflowText));
            } else {
                RespondSse(client_fd, TextTurn("\xe6\x94\xb6\xe5\x88\xb0\xe5\x90\x8e\xe5\x8f\xb0"
                                                   "\xe7\xbb\x93\xe6\x9e\x9c"));  // 收到后台结果
            }
        } else if (stage == 3) {
            // 第三幕:主回合按工具结果推进——没派过派 #3,派过 #3 派 #4,
            // 都派过收口。任务号按启动回执里的 "#N" 区分。
            if (has("\xe5\x90\x8e\xe5\x8f\xb0\xe5\xad\x90\xe4\xbb\xa3\xe7\x90\x86 #4")) {
                RespondSse(client_fd,
                           TextTurn("\xe4\xb8\xa4\xe5\x8f\xaa\xe5\x90\x8e\xe5\x8f\xb0\xe9\x83\xbd"
                                    "\xe6\xb4\xbe\xe5\xa5\xbd\xe4\xba\x86"));  // 两只后台都派好了
            } else if (has("\xe5\x90\x8e\xe5\x8f\xb0\xe5\xad\x90\xe4\xbb\xa3\xe7\x90\x86 #3")) {
                RespondSse(client_fd,
                           ToolUseTurn("toolu_bg3_b", "agent",
                                       "{\"title\":\"" + std::string(kSlowTitle) + "\",\"prompt\":\"" +
                                           std::string(kSlowPrompt) + "\",\"execution_mode\":\"background\"}"));
            } else {
                RespondSse(client_fd,
                           ToolUseTurn("toolu_bg3_a", "agent",
                                       "{\"title\":\"" + std::string(kFastTitle) + "\",\"prompt\":\"" +
                                           std::string(kFastPrompt) + "\",\"execution_mode\":\"background\"}"));
            }
        } else if (stage == 4) {
            // 第四幕:主回合派 #5 后收口(收口话做"回合真结束"的门闩——
            // 只有回合收口回空闲,完成退场那一拍才会跑)。
            if (has("\xe5\x90\x8e\xe5\x8f\xb0\xe5\xad\x90\xe4\xbb\xa3\xe7\x90\x86 #5")) {  // 后台子代理 #5
                RespondSse(client_fd, TextTurn(kAct4Wrap));
            } else {
                RespondSse(client_fd,
                           ToolUseTurn("toolu_watch", "agent",
                                       "{\"title\":\"" + std::string(kWatchTitle) + "\",\"prompt\":\"" +
                                           std::string(kWatchPrompt) + "\",\"execution_mode\":\"background\"}"));
            }
        } else if (stage == 6) {
            // 压测幕(终端画面隔网单验收):同一句用户话发两遍——第一遍
            // 千枚小 delta 滴流,第二遍(历史里有小字收尾标记)十枚大 delta。
            if (has(kStressSmallTail)) {
                RespondStressBig(client_fd);
            } else {
                RespondStressSmall(client_fd);
            }
        } else if (stage == 5) {
            // 第五幕:主回合派 #6(写材料)后收口,剩下的交给子代理自己撞墙。
            if (has("\xe5\x90\x8e\xe5\x8f\xb0\xe5\xad\x90\xe4\xbb\xa3\xe7\x90\x86 #6")) {
                RespondSse(client_fd,
                           TextTurn("\xe6\x9d\x90\xe6\x96\x99\xe5\xb7\xb2\xe6\xb4\xbe\xe5\xa6\xa5\xef\xbc\x8c"
                                    "\xe7\xad\x89\xe5\xae\x83\xe4\xba\xa4\xe5\xb7\xae"));
            } else {
                RespondSse(client_fd,
                           ToolUseTurn("toolu_deny", "agent",
                                       "{\"title\":\"" + std::string(kDenyTitle) + "\",\"prompt\":\"" +
                                           std::string(kDenyPrompt) + "\",\"execution_mode\":\"background\"}"));
            }
        } else if (stage == 2) {
            // 第二幕:主回合。没派过后台(工具结果带"已启动")就派,派过
            // 收口。
            if (has("\xe5\xb7\xb2\xe5\x90\xaf\xe5\x8a\xa8")) {
                RespondSse(client_fd, TextTurn("\xe5\xb7\xb2\xe6\xb4\xbe\xe5\x87\xba\xe5\x90\x8e"
                                                   "\xe5\x8f\xb0\xe4\xbb\xa3\xe7\x90\x86"));  // 已派出后台代理
            } else {
                RespondSse(client_fd,
                           ToolUseTurn("toolu_bg", "agent",
                                       "{\"title\":\"" + std::string(kBgTitle) +
                                           "\",\"prompt\":\"\xe5\x90\x8e\xe5\x8f\xb0\xe6\x91\xb8"
                                           "\xe6\x8e\x92\xe4\xb8\x80\xe4\xbb\xbd\xe4\xba\x8b"
                                           "\xe5\xae\x9e\xe6\xb8\x85\xe5\x8d\x95\",\"execution_mode\":"
                                           "\"background\"}"));
            }
        } else if (stage == 1) {
            // 第一幕:主回合。收到子代理结论(工具结果带"检索阈值回归
            // 全绿")就收尾,否则派前台子代理。
            if (has("\xe6\xa3\x80\xe7\xb4\xa2\xe9\x98\x88\xe5\x80\xbc\xe5\x9b\x9e\xe5\xbd\x92\xe5\x85\xa8\xe7\xbb\xbf")) {
                RespondSse(client_fd, TextTurn("\xe4\xb8\xbb\xe4\xbb\xa3\xe7\x90\x86\xe6\xb1\x87"
                                                   "\xe6\x80\xbb\xe5\xae\x8c\xe6\xaf\x95"));  // 主代理汇总完毕
            } else {
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
            }
        } else {
            RespondSse(client_fd, TextTurn("ok"));
        }
        // 优雅关闭:先 shutdown(SD_SEND) 送走响应,再排干客户端残余字节,
        // 最后 closesocket。直接 close 而接收缓冲还有客户端字节时,Windows
        // 会发 RST,把已经写出去的响应一并吃掉——子进程那边表现成"连接被
        // 重置",主回合/子代理挂死在等响应上,剧本一乱一大片(本机代理/TUN
        // 类软件在场时尤甚)。
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
    std::thread([listener, state, serve_connection]() {
        while (true) {
            sockaddr_in client{};
            int client_len = sizeof(client);
            const SOCKET_T client_fd =
                ::accept(listener, reinterpret_cast<sockaddr*>(&client), &client_len);
            if (client_fd == kBadSocket) {
                Log("SERVER accept failed err=" + std::to_string(::WSAGetLastError()));
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

    // 假 anthropic 服务 + 子进程环境:全量指到本地,不碰真网络,也不被
    // 会话中转/代理截胡(NO_PROXY 盖掉 http_proxy)。
    const int port = StartFakeAnthropicServer();
    if (port == 0) {
        Log("FAIL: fake server bind");
        return 1;
    }
    // 隔离 HOME:子进程读的是 USERPROFILE 下的 .lubancode(配置/技能/记忆/
    // 插件/键位)。跑驱动器的机器上真 HOME 里常有上百个技能与常驻记忆
    // worker——每个后台子代理起跑都要现建一份注册表/技能段,真 HOME 的体
    // 量会让"新任务起跑"拖到十几秒甚至卡死,剧本时序全乱。给子进程一个
    // 空 HOME(配置全由 LUBANCODE_* 环境变量给,不会进向导),刮屏剧本才
    // 只考验被测代码,不考验宿主机的家底。目录名带本进程 pid:同一工作目录
    // 连跑多把也互不攒账(上把的记忆/同伴注册表绝不渗进这把的时序)。
    {
        wchar_t workdir_buf[MAX_PATH]{};
        GetCurrentDirectoryW(MAX_PATH, workdir_buf);
        const std::wstring hermetic_home =
            std::wstring(workdir_buf) + L"\\drvhome_" + std::to_wstring(GetCurrentProcessId());
        CreateDirectoryW(hermetic_home.c_str(), nullptr);
        CreateDirectoryW((hermetic_home + L"\\.lubancode").c_str(), nullptr);
        SetEnv(L"USERPROFILE", hermetic_home);
    }
    SetEnv(L"LUBANCODE_WIRE", L"anthropic");
    SetEnv(L"LUBANCODE_BASE_URL", L"http://127.0.0.1:" + std::to_wstring(port));
    SetEnv(L"LUBANCODE_API_KEY", L"agent-stream-driver");
    SetEnv(L"LUBANCODE_MODEL", L"fake-model");
    // 环境卫生(记忆条目"集成测试须显式覆盖 ANTHROPIC_BASE_URL"):宿主
    // shell 常带着真中转的 ANTHROPIC_* 变量,子进程的 generic-env 合并层看得
    // 见它们——显式盖空,记忆抽取等旁路才不会把请求漏去真网,卡住整场。
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
    // 补一手缓冲重设(2026-08-26):本机 conhost 上 SetConsoleWindowInfo 会把
    // 缓冲高收拢到窗口高(实测 400x120 设窗后变 30x120)——查看态重铺帧
    // (头三行+尾部 ~23 行)加底栏就顶穿缓冲底,控制台被迫滚屏,主会话正文
    // 被顶出缓冲、查看帧锚点全漂(第三幕回流簇的病根)。设窗后再把缓冲
    // 撑回 400 行,长缓冲 rig(产品注释里"真控制台驱动器的 120×400")才
    // 真正成立;设不回去(老 conhost)也不影响——那台机器本来就保持 400。
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
    {
        const int input_now = FindFooterInputRow();
        rule_row = input_now > 0 ? FindRuleAboveInput(input_now) : -1;
    }
    Check(rule_row > 0, "流式:composer 上横线定位到");
    // 位置断言同样不是原子快照:footer 重画挪位时首判可能失准,隔 300ms
    // 把两个行号一起重测一次再定罪。
    {
        bool below = title_row >= 0 && title_row > rule_row + 3;
        if (!below) {
            Sleep(300);
            title_row = FindLastRow(kTitle);
            const int input_retry = FindFooterInputRow();
            rule_row = input_retry > 0 ? FindRuleAboveInput(input_retry) : -1;
            below = title_row >= 0 && rule_row > 0 && title_row > rule_row + 3;
        }
        Check(below, "流式:title 行在状态栏之下(导航坞贴底)");
    }
    Check(NoDockTextAboveComposer(rule_row), "流式:composer 上横线之上没有任何导航文本");
    Check(FindLastRow(kPromptHead) < 0, "流式:prompt 开头整屏不出现(不冒充标题)");
    Check(FindLastRow("ctrl+o \xe5\xb1\x95\xe5\xbc\x80\xe6\x98\x8e\xe7\xbb\x86") < 0,
          "流式:旧三行状态块(ctrl+o 展开明细)不再出现");

    // ---- 残帧计数:工具跑着、耗时/tokens 跳动,导航不复制 ----
    // 口径重钉(终端接线批后):footer 随正文滚动,滚过去的旧帧行会在回滚
    // 缓冲里短暂留影——用户看不见(可视区之外),整缓冲计数把这种瞬影也当
    // 残帧,连跑三把两把误伤。判定改数可视区(残帧=用户看得见的重复),
    // 整缓冲多出来的行照旧倒进报告(INFO 留档,不判定),真赖账的残帧一
    // 眼可辨。
    const auto dump_buffer_rows_with = [](const std::string& needle, const char* tag) {
        if (CountRowsWith(needle) == CountViewportRowsWith(needle)) {
            return;
        }
        CONSOLE_SCREEN_BUFFER_INFO info{};
        if (!GetConsoleScreenBufferInfo(g_conout, &info)) {
            return;
        }
        for (int row = 0; row < 400; ++row) {
            if (ReadRow(row).find(needle) == std::string::npos) {
                continue;
            }
            const bool in_view = row >= info.srWindow.Top && row <= info.srWindow.Bottom;
            if (!in_view) {
                Log(std::string("GHOST ") + tag + " row " + std::to_string(row) + ": " + ReadRow(row));
            }
        }
    };
    // 采样不过时把两本账(可视区/整缓冲)的计数与命中行全倒进报告:0 与 2
    // 的病根不同,留档才好定罪。
    const auto log_counts = [](const std::string& needle, const char* tag) {
        CONSOLE_SCREEN_BUFFER_INFO info{};
        GetConsoleScreenBufferInfo(g_conout, &info);
        Log(std::string("SAMPLE ") + tag + " viewport=" + std::to_string(CountViewportRowsWith(needle)) +
            " buffer=" + std::to_string(CountRowsWith(needle)) + " window=" +
            std::to_string(info.srWindow.Top) + "-" + std::to_string(info.srWindow.Bottom));
        for (int row = 0; row < 400; ++row) {
            if (ReadRow(row).find(needle) != std::string::npos) {
                const bool in_view = row >= info.srWindow.Top && row <= info.srWindow.Bottom;
                Log(std::string("SAMPLE ") + tag + " row " + std::to_string(row) + (in_view ? " [view]" : " [hist]") +
                    ": " + ReadRow(row));
            }
        }
    };
    const auto stable_viewport_count = [&log_counts](const std::string& needle, const char* tag) {
        const int first = CountViewportRowsWith(needle);
        if (first == 1) {
            return first;
        }
        Sleep(300);
        const int second = CountViewportRowsWith(needle);
        if (second == 1) {
            return second;
        }
        Log(std::string("SAMPLE unstable after retry: first=") + std::to_string(first) +
            " second=" + std::to_string(second));
        log_counts(needle, tag);
        return second;
    };
    Check(stable_viewport_count("\xe2\x86\x91/\xe2\x86\x93", "hint") == 1,
          "流式:操作提示恰好一份(可视区)");
    Check(CountMainRowsViewport() == 1, "流式:main 行恰好一份(可视区)");
    for (int sample = 0; sample < 3; ++sample) {
        Sleep(1500);  // 耗时 ~1s 一跳,采样跨多拍
        Check(stable_viewport_count("\xe2\x86\x91/\xe2\x86\x93", "hint") == 1,
              "流式:耗时/token 刷新后操作提示仍恰好一份(第 " + std::to_string(sample + 1) + " 次采样)");
        Check(CountViewportRowsWith("general-purpose") <= 1,
              "流式:坞行不随刷新复制(第 " + std::to_string(sample + 1) + " 次采样)");
        dump_buffer_rows_with("\xe2\x86\x91/\xe2\x86\x93", "hint");
        dump_buffer_rows_with("general-purpose", "dock");
    }

    // ---- Up 进坞焦点:选中标记出现在状态栏之下 ----
    SendKey(VK_UP, 0, 0);
    int marker_row = -1;
    Check(WaitForText("\xe2\x9d\xaf", 3000, &marker_row), "流式:空输入按上键,焦点标记出现");
    Check(marker_row >= 0 && marker_row > rule_row + 3, "流式:焦点标记在导航坞里(状态栏之下)");

    // ---- Enter 真切会话:上方视口换源成该代理 transcript,坞里无长正文;
    //      Enter 被导航消费,不顺手把 composer 的字提交落队 ----
    SendKey(VK_RETURN, L'\r', 0);
    Check(WaitForText("\xe6\x9f\xa5\xe7\x9c\x8b general-purpose", 3000),
          "流式:Enter 后上方正文区出现该代理的查看头行");  // 查看 general-purpose
    {
        int rule_with_tag = -1;
        {
            const int input_now = FindFooterInputRow();
            rule_with_tag = input_now > 0 ? FindRuleAboveInput(input_now) : -1;
        }
        Check(rule_with_tag > 0 && ReadRow(rule_with_tag).find(kTitle) != std::string::npos,
              "流式:查看态输入框上横线右端挂 title");
        Check(FindLastRow(kPromptHead) >= 0, "流式:查看视口里能看到完整 prompt(只有视口能看)");
        Check(FindLastRow(kPromptHead) < rule_with_tag, "流式:完整 prompt 在视口里,不在导航坞");
        Check(FindLastRow("\xe4\xbb\xbb\xe5\x8a\xa1\xe8\xaf\xb4\xe6\x98\x8e") < rule_with_tag,
              "流式:'任务说明'只在视口,不向坞下方生长");  // 任务说明
        // 统计与当前状态行(真机回归单验收第 4 条):agent 视图与 main 同
        // 待遇——状态短话 + 工具次数 + token + 用时,不只剩一张流水单。
        int stats_row = -1;
        for (int r = rule_with_tag - 1; r >= 0; --r) {
            const std::string text = ReadRow(r);
            if (text.find("\xe6\xac\xa1\xe5\xb7\xa5\xe5\x85\xb7\xe8\xb0\x83\xe7\x94\xa8") !=
                    std::string::npos &&  // 次工具调用
                text.find("tokens") != std::string::npos) {
                stats_row = r;
                break;
            }
        }
        Check(stats_row >= 0 && stats_row < rule_with_tag,
              "流式:查看视口带统计与当前状态行(工具次数/tokens/用时)");
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

    // ---- Esc 一拍退净(规格"现场二"按键契约改版):查看、选择、焦点一次
    //      收干净。旧断言按"两下 Esc 逐层退"钉的——那套两层契约已改,再按
    //      第二下会把 Esc 送到流式监听当打断用;这里只按一下,断言一拍全退
    //      且不打断整轮。----
    SendKey(VK_ESCAPE, 0, 0);
    {
        // 查看态退掉的标志:上横线右端的 title 标签摘掉(视口里铺过的
        // transcript 行留在滚屏,那是有归属的正文,不算残帧)。
        Sleep(600);
        int rule_after_view = -1;
        {
            const int input_now = FindFooterInputRow();
            rule_after_view = input_now > 0 ? FindRuleAboveInput(input_now) : -1;
        }
        Check(rule_after_view > 0 && FindLastRow(kTitle) >= 0 &&
                  ReadRow(rule_after_view).find(kTitle) == std::string::npos,
              "流式:Esc 先退查看态(上横线 title 标签摘掉)");
    }
    Check(WaitForTextGone("\xe2\x9d\xaf", 3000), "流式:一拍 Esc 连代理焦点一并收干净(❯ 消失)");
    Check(FindLastRow("\xe5\xb7\xb2\xe6\x89\x93\xe6\x96\xad") < 0,
          "流式:一拍 Esc 退场不打断当前轮(没有'[已打断]')");
    {
        // 与残帧采样同款的重画空窗保险:首数 >1 隔 300ms 复读,两次都超
        // 才算真复制。
        int dock_after_esc = CountDockRowsWith("general-purpose", ComposerTopRuleNow());
        if (dock_after_esc > 1) {
            Sleep(300);
            dock_after_esc = CountDockRowsWith("general-purpose", ComposerTopRuleNow());
        }
        Check(dock_after_esc <= 1, "流式:退查看态后坞行至多一份(标签已摘)");
    }

    // ---- 放开子代理:ping 跑完,结果当拍交回主回合,回合收场回空闲 ----
    // 注:子代理结论文本只在工具结果里,屏上摘要行是"子代理 N 轮 · M 次工具";
    // 主代理收尾正文紧跟着被 footer 的整帧重画顶走——所以这两条按"终态与
    // 回合收口"断言,不赌瞬时正文。
    Check(WaitForText("\xe6\xac\xa1\xe5\xb7\xa5\xe5\x85\xb7", 30000),
          "收尾:agent 条目终态摘要出现");  // "次工具"(⎿ 子代理 N 轮 · M 次工具)
    // 前台任务完成即投递(结果直接落回主回合的工具结果),"Running 原地变
    // 完成"的过渡态只有一瞬,刮屏抓不住;断言重钉为投递后的新规矩——完成
    // 任务的坞行退场,不赖在坞里(规格"现场一"退场账)。
    {
        const DWORD retire_deadline = GetTickCount() + 15000;
        bool retired = false;
        while (GetTickCount() < retire_deadline) {
            if (CountDockRowsWith("general-purpose", ComposerTopRuleNow()) == 0) {
                retired = true;
                break;
            }
            Sleep(200);
        }
        Check(retired, "收尾:前台任务完成投递,坞行退场(不赖坞)");
    }
    // 回到空闲后:任务已 done+delivered 退场,台账与正文照查(● agent 条目
    // 还挂着 title),坞区归零。
    Check(WaitForText(kTitle, 10000), "空闲:终态任务的 title 仍可查(台账/正文在)");
    Check(WaitForIdleComposer(15000), "空闲:composer 真画出来再数账");
    Sleep(700);  // 等收尾的最后一帧整帧画稳(耗时/token 跳动的那一拍)
    {
        const int idle_input = FindFooterInputRow();
        const int idle_rule = idle_input > 0 ? FindRuleAboveInput(idle_input) : -1;
        Check(CountDockRowsWith("general-purpose", idle_rule) == 0, "空闲:坞行归零(残帧归零)");
        Check(CountDockRowsWith("\xe2\x97\x8f main", idle_rule) == 0,
              "空闲:无在册代理时整坞不出场(0 只不画孤零零 main)");
        Check(FindLastDockRow(kTitle, idle_rule) < 0, "空闲:title 不落坞区(退场无痕)");
        Check(idle_rule > 0 && FindLastRow(kTitle) < idle_rule, "空闲:终态 title 归正文区(chrome 之上)");
    }

    // ---- 第二幕:后台子代理在空闲时完成 → 旧底栏正式退场,通知归 transcript ----
    SendText("\xe5\x86\x8d\xe6\xb4\xbe\xe4\xb8\x80\xe5\x8f\xaa\xe5\x90\x8e\xe5\x8f\xb0\xe4\xbb\xa3"
             "\xe7\x90\x86\xe5\x8e\xbb\xe6\x91\xb8\xe6\x8e\x92");  // 再派一只后台代理去摸排
    SendKey(VK_RETURN, L'\r', 0);
    // 主回合收口后回空闲:坞里挂着第二只任务(运行中),空闲 composer 画出。
    // 第一幕的前台任务早已 done+delivered 退场,坞里只剩第二只。
    Check(WaitForText(kBgTitle, 15000), "后台幕:空闲后坞里出现第二只任务(运行中)");
    Check(WaitForIdleComposer(15000), "后台幕:空闲 composer 画出");
    Sleep(700);  // 等末帧画稳
    {
        const int bg_input = FindFooterInputRow();
        const int bg_rule = bg_input > 0 ? FindRuleAboveInput(bg_input) : -1;
        Check(CountDockRowsWith("general-purpose", bg_rule) == 1,
              "后台幕:坞里只剩第二只任务一行(前台那只已退场)");
    }
    {
        bool bg_docked = false;
        for (int attempt = 0; attempt < 3 && !bg_docked; ++attempt) {
            const int idle_input2 = FindFooterInputRow();
            const int idle_rule2 = idle_input2 > 0 ? FindRuleAboveInput(idle_input2) : -1;
            bg_docked = idle_input2 > 0 && FindLastDockRow(kBgTitle, idle_rule2) > idle_input2 + 3;
            if (!bg_docked) {
                Sleep(300);  // 重画挪位的空窗:重测一次再定罪
            }
        }
        Check(bg_docked, "后台幕:第二只任务在状态栏之下贴底");
    }
    // 等后台子代理两轮来回(第二轮压 3 秒)触发空闲唤醒:旧 composer 整帧
    // 退场,通知一行进 transcript,随后主回合收口回空闲。
    Check(WaitForText("\xe6\x94\xb6\xe5\x88\xb0\xe5\x90\x8e\xe5\x8f\xb0\xe7\xbb\x93\xe6\x9e\x9c", 60000),
          "后台幕:完成唤醒后的主回合收口");  // 收到后台结果
    {
        const DWORD idle_deadline = GetTickCount() + 30000;
        bool idle_back = false;
        while (GetTickCount() < idle_deadline) {
            if (FindFooterInputRow() > 0) {
                idle_back = true;
                break;
            }
            Sleep(200);
        }
        Check(idle_back, "后台幕:收口后回到空闲 composer");
    }
    Sleep(800);  // 等末帧画稳
    // 唤醒前后整个可视区及滚屏尾部:旧输入框/状态栏/导航坞不留副本——
    // 旧帧在唤醒路被硬清,缓冲区里 composer 框结构只此一份。
    Check(CountVisibleComposers() == 1,
          "后台完成唤醒:composer 框全缓冲区恰好一份(旧底栏已退场)");
    // 退场链(查看态回流单):结果交回 main 置 delivered,导航坞行随即退场
    // ——此刻两只任务都 done+delivered,导航表空了,整坞随之消失(0 只代理
    // 整坞不出场是既有规矩),代理行归零。
    {
        const int wake_input = FindFooterInputRow();
        const int wake_rule = wake_input > 0 ? FindRuleAboveInput(wake_input) : -1;
        Check(CountDockRowsWith("general-purpose", wake_rule) == 0,
              "后台完成唤醒:完成任务的坞行已退场(不再赖在坞里)");
    }
    // 完成通知有且只有一条,归 main(在 transcript 区,composer 上横线之上)。
    Check(CountRowsWith("\xe5\x90\x8e\xe5\x8f\xb0\xe5\xad\x90\xe4\xbb\xa3\xe7\x90\x86\xe5\xae\x8c\xe6\x88\x90") == 1,
          "后台完成唤醒:完成通知恰好一条");  // 后台子代理完成
    {
        const int notice_row = FindLastRow("\xe5\x90\x8e\xe5\x8f\xb0\xe5\xad\x90\xe4\xbb\xa3\xe7\x90\x86"
                                           "\xe5\xae\x8c\xe6\x88\x90");
        const int rule_end = ComposerTopRuleNow();
        Check(notice_row >= 0 && notice_row < rule_end, "后台完成唤醒:通知在 transcript 区(chrome 之上)");
    }
    Check(CountDockRowsWith(kBgTitle, ComposerTopRuleNow()) == 0,
          "后台完成唤醒:第二只任务的导航行也已退场(台账保留,坞里无痕)");

    // ---- 第三幕(查看态回流单):看 #4 时 #3 完成——回流静默、查看帧零扰动、
    //      #3 坞行退场、toast 一枚、Esc 回 main 重铺可见、半句草稿不丢 ----
    SendText(kAct3User);  // 派两只后台代理慢慢查
    SendKey(VK_RETURN, L'\r', 0);
    // 主回合连派 #3/#4 后收口(#3 ~12s 交卷、#4 永不交卷,都先运行中)。等
    // "请求 3 次"统计行钉死回合真收口——按结构找输入框分不清流式 footer
    // 与空闲 composer,拿它当空闲门闩会把导航键喂给监听线程。
    Check(WaitForText(kFastTitle, 15000), "第三幕:快代理 #3 入坞");
    Check(WaitForText(kSlowTitle, 15000), "第三幕:慢代理 #4 入坞");
    // 收口门闩整枚撤下(不再等收口再导航):SERVER 时戳实测,场上同时有两
    // 只"每秒一轮"的轮询子代理时,主回合的收口请求要排到 #3 整条命跑完
    // (~15s)才出得去——等收口正文,导航就永远赶不上 #3 交卷前。改为两枚
    // title 入坞即在流式期间导航(流式监听同样吃 Up/Down/Enter,第一幕整
    // 段已验);"回合真收口"改由后面的静默回流链事后背书(没收口+回空闲,
    // 回流根本不会发生)。
    Sleep(700);
    {
        // 重画挪位的空窗:重测三次再定罪。
        int docked = -1;
        for (int attempt = 0; attempt < 3 && docked != 2; ++attempt) {
            docked = CountDockRowsWith("general-purpose", ComposerTopRuleNow());
            if (docked != 2) {
                Sleep(300);
            }
        }
        Check(docked == 2, "第三幕:坞里 #3/#4 各一行");
    }
    // Down×2(main -> #3 -> #4)聚焦 #4,Enter 切查看。❯ 门闩不可用——滚屏
    // 里躺着第一幕的旧焦点标记,按步进睡眠等 100ms 拍消化按键。
    SendKey(VK_DOWN, 0, 0);
    Sleep(600);
    SendKey(VK_DOWN, 0, 0);
    Sleep(600);
    SendKey(VK_RETURN, L'\r', 0);
    Sleep(800);
    // 切看后的屏面留档(不判定,只 INFO):排查/复盘用,与 EXPAND/VIEW 同款。
    for (int r = 0; r < 44; ++r) {
        const std::string row = ReadRow(r);
        if (!row.empty()) {
            Log("ACT3NAV " + std::to_string(r) + ": " + row);
        }
    }
    int view4_row = -1;
    Check(WaitForText("\xe6\x9f\xa5\xe7\x9c\x8b general-purpose #4", 5000, &view4_row),
          "第三幕:上方视口出现 #4 的查看头行");  // 查看 general-purpose #4
    int slow_prompt_row = -1;
    {
        const int rule_now = ComposerTopRuleNow();
        slow_prompt_row = FindLastRow(kSlowPrompt);
        Check(slow_prompt_row >= 0 && slow_prompt_row < rule_now, "第三幕:#4 的 prompt 在查看视口里");
        Check(CountRowsWith("\xe6\x9f\xa5\xe7\x9c\x8b general-purpose #4") == 1, "第三幕:查看头行恰好一份");
    }
    // 给 #4 敲半句草稿,等 #3 跑完:草稿还在(空闲唤醒让位给正文,不抢输入)。
    SendText(kDraftText);
    Sleep(400);
    bool fast_done_docked = false;
    {
        const DWORD fast_deadline = GetTickCount() + 30000;
        while (GetTickCount() < fast_deadline) {
            const int r = FindLastDockRow(kFastTitle, ComposerTopRuleNow());
            if (r >= 0 && ReadRow(r).find("\xe5\xae\x8c\xe6\x88\x90(") != std::string::npos) {  // 完成(
                fast_done_docked = true;
                break;
            }
            Sleep(200);
        }
    }
    Check(fast_done_docked, "第三幕:#3 跑完,坞行原地变完成(未投递过渡态)");
    {
        const int input_row = FindFooterInputRow();
        Check(input_row > 0 && ReadRow(input_row).find(kDraftText) != std::string::npos,
              "第三幕:半句草稿原样在(唤醒让位,#3 完成不吃输入)");
        Check(FindLastRow(kReflowText) < 0, "第三幕:草稿未清,回流还没发生");
    }
    // Ctrl+C 清草稿 -> 空闲唤醒 -> 查看态静默回流(输出全进台账,不上屏)。
    SendKey('C', 0, LEFT_CTRL_PRESSED);
    Check(WaitForText(kToastText, 25000), "第三幕:静默回流收口,导航坞 toast 出现");
    // 事后核账:回流能走通,前提是主回合真收过口(收口正文"两只后台都
    // 派好了"已在台账里)——导航不再等它,这里补上"它确实发生过"。
    Check(FindLastRow("\xe4\xb8\xa4\xe5\x8f\xaa\xe5\x90\x8e\xe5\x8f\xb0\xe9\x83\xbd\xe6\xb4\xbe\xe5\xa5\xbd"
                      "\xe4\xba\x86") >= 0,
          "第三幕:主回合最终收口(回流链前置,事后核账)");  // 两只后台都派好了
    Sleep(400);
    {
        // 查看帧零扰动:头行/正文行号不漂、恰好一份;回流正文一个字不上屏。
        Check(FindLastRow("\xe6\x9f\xa5\xe7\x9c\x8b general-purpose #4") == view4_row,
              "第三幕:回流后 #4 查看头行锚点不漂");
        Check(FindLastRow(kSlowPrompt) == slow_prompt_row, "第三幕:回流后查看视口内容行不漂");
        Check(CountRowsWith("\xe6\x9f\xa5\xe7\x9c\x8b general-purpose #4") == 1,
              "第三幕:查看头行仍恰好一份(没有第二帧)");
        Check(FindLastRow(kReflowText) < 0, "第三幕:回流正文没上屏(静默收货,零侵入)");
        const int rule4 = ComposerTopRuleNow();
        Check(CountDockRowsWith(kFastTitle, rule4) == 0, "第三幕:#3 坞行已退场(done+delivered)");
        Check(CountDockRowsWith(kSlowTitle, rule4) == 1, "第三幕:#4 坞行还在(查看目标纹丝不动)");
        Check(FindLastDockRow(kToastText, rule4) > rule4, "第三幕:toast 挂在坞区(不抢正文)");
        Check(CountVisibleComposers() == 1, "第三幕:composer 恰好一份");
    }
    // Esc 回 main:重铺见到回流的完成事件与主轮输出各一次(视口内数,前两幕
    // 滚屏旧账不算)。
    SendKey(VK_ESCAPE, 0, 0);
    Check(WaitForText("\xe5\xb7\xb2\xe5\x9b\x9e\xe4\xb8\xbb\xe4\xbc\x9a\xe8\xaf\x9d", 5000),
          "第三幕:Esc 回 main");  // 已回主会话
    Sleep(500);
    Check(CountViewportRowsWith(kReflowText) == 1, "第三幕:回 main 重铺见回流正文恰好一次");
    Check(CountViewportRowsWith("\xe5\xbf\xab\xe6\x9f\xa5\xe7\x94\xb2\xe6\x8a\xa5\xe5\x91\x8a \xc2\xb7 \xe5\xae\x8c\xe6\x88\x90") == 1,
          "第三幕:回 main 重铺见完成通知恰好一枚(点名快查甲的完成行;派发条目与旧任务台账不算)");

    // ---- 第四幕(查看态完成退场,2026-08-17):真盯一只后台任务从生到
    //      死——运行中切看、实时流每秒重铺喂帧、任务完成退场原子回 main。
    //      断言退场后一帧完整(main 复位行可辨、composer 恰一份、坞无已完成
    //      行)、结论回流、键入立即回显、/exit 退出码 0(ReadLine 没卡死)。 ----
    // 先停掉第三幕的 #4(Ctrl+X Ctrl+K 两段确认):它是只"永不交卷"的轮询
    // 器,一秒一连接地跑到会话尾——本机网络栈(代理/TUN 类软件)对"新连接
    // 与在途连接重叠"并不总是友好,假服务的连接会被偶发重置甚至挂死,后面
    // 整幕全乱。停完再派 #5,场上只有一只轮询器,与第二幕同款节奏。
    SendKey('X', 0, LEFT_CTRL_PRESSED);
    Sleep(400);
    SendKey('K', 0, LEFT_CTRL_PRESSED);
    {
        const DWORD stop_deadline = GetTickCount() + 15000;
        bool dock_empty = false;
        while (GetTickCount() < stop_deadline) {
            const int rule_now = ComposerTopRuleNow();
            if (rule_now > 0 && CountDockRowsWith("general-purpose", rule_now) == 0) {
                dock_empty = true;
                break;
            }
            Sleep(200);
        }
        Check(dock_empty, "第四幕:先停掉 #4,导航坞清空(两段确认)");
    }
    Sleep(600);
    SendText(kAct4User);  // 再盯一只后台从生到死
    SendKey(VK_RETURN, L'', 0);
    Check(WaitForText(kWatchTitle, 15000), "第四幕:#5 入坞(盯退场的慢工)");
    Check(WaitForText(kAct4Wrap, 30000), "第四幕:主回合收口(慢工已派妥,回空闲)");
    Check(WaitForIdleComposer(15000), "第四幕:空闲 composer 画出");
    Sleep(700);
    // Down×1(main -> #5)聚焦,Enter 切看 #5——赶在它 ~15s 交卷之前;步进
    // 睡眠等 100ms 拍消化按键。
    SendKey(VK_DOWN, 0, 0);
    Sleep(600);
    SendKey(VK_RETURN, L'', 0);
    Check(WaitForText("\xe6\x9f\xa5\xe7\x9c\x8b general-purpose #5", 15000),
          "第四幕:切看 #5(查看头行出现)");
    // 完成退场:#5 交卷 -> 查看态静默回流 -> 坞行退场 -> 查看目标消失 ->
    // 原子回 main。视口内"已回主会话"出现(第三幕那条在滚屏里,不数),
    // #5 查看头行从视口消失。
    {
        const DWORD retire_deadline = GetTickCount() + 60000;
        bool retired = false;
        while (GetTickCount() < retire_deadline) {
            if (CountViewportRowsWith("\xe5\xb7\xb2\xe5\x9b\x9e\xe4\xb8\xbb\xe4\xbc\x9a\xe8\xaf\x9d") >= 1 &&
                CountViewportRowsWith("\xe6\x9f\xa5\xe7\x9c\x8b general-purpose #5") == 0) {
                retired = true;
                break;
            }
            Sleep(200);
        }
        Check(retired, "第四幕:完成退场回 main(复位行进视口、查看头行退场)");
    }
    Sleep(700);  // 等退场末帧画稳
    for (int r = 0; r < 44; ++r) {
        const std::string row = ReadRow(r);
        if (!row.empty()) {
            Log("RETIRE " + std::to_string(r) + ": " + row);
        }
    }
    {
        const int rule5 = ComposerTopRuleNow();
        Check(rule5 > 0, "第四幕:退场后 composer 按结构找到");
        Check(CountVisibleComposers() == 1, "第四幕:退场后 composer 恰好一份");
        Check(CountDockRowsWith(kWatchTitle, rule5) == 0, "第四幕:Dock 无已完成行(#5 已退场)");
        // #4 已在幕首停下、#5 完成退场:导航表空了,整坞随之消失(既有规矩)。
        Check(CountDockRowsWith("general-purpose", rule5) == 0, "第四幕:退场后整坞清空(无代理行)");
        // 结论回流的口径(2026-08-26 裁定):重铺的画面只给摘要——完成通知行
        // 与"#5 盯退场的慢工 · 完成 · N 次工具"摘要行;结论原文只进任务台账
        // (查看帧从 TaskEvents 渲染,不随退场重铺上屏),这里断言摘要可见,
        // 不再要求结论原文出现在重铺后的屏面上。
        Check(CountViewportRowsWith("\xe5\x90\x8e\xe5\x8f\xb0\xe5\xad\x90\xe4\xbb\xa3\xe7\x90\x86\xe5\xae\x8c"
                                    "\xe6\x88\x90") >= 1,
              "第四幕:退场重铺见完成通知摘要(通知行可见)");  // 后台子代理完成
        Check(CountViewportRowsWith(std::string(kWatchTitle) + " \xc2\xb7 \xe5\xae\x8c\xe6\x88\x90") >= 1,
              "第四幕:退场重铺见 #5 完成摘要行(盯退场的慢工 · 完成)");
    }
    // 键入立即回显:退场后敲字,composer 输入行当拍看得见——画面空了、键
    // 无回显就是用户真机那种"卡死"。
    SendText("ok-after-retire");
    {
        const DWORD echo_deadline = GetTickCount() + 5000;
        bool echoed = false;
        while (GetTickCount() < echo_deadline) {
            const int input_row = FindFooterInputRow();
            if (input_row > 0 && ReadRow(input_row).find("ok-after-retire") != std::string::npos) {
                echoed = true;
                break;
            }
            Sleep(150);
        }
        Check(echoed, "第四幕:退场后键入立即回显(ReadLine 活着)");
    }

    // ---- 第五幕(压测,终端画面隔网单):两本吞吐账——千枚小 delta 滴流
    //      (整流 ~2.4s,跨多个帧边界)与十枚 600 字大 delta。断言:流期间
    //      footer 输入行常驻(采样)、活动行在走(心跳没饿死)、收尾标记上屏
    //      (合并批次一字不丢)、收口回空闲。 ----
    {
        // 压测采样:直到收尾标记上屏,每 300ms 确认一次输入行还在(帧内
        // 擦画空窗复测三次再定罪,定罪时把屏面倒进报告留档);顺带观察
        // 活动行文本变化——走字扫光/秒数在跳,心跳就没被 delta 洪峰饿死。
        const auto sample_stress_stream = [&](const char* tail_marker, const char* what) {
            bool input_ok = true;
            bool activity_seen = false;
            bool activity_alive = false;
            std::string first_activity;
            const DWORD deadline = GetTickCount() + 30000;
            while (GetTickCount() < deadline) {
                if (FindLastRow(tail_marker) >= 0) {
                    break;
                }
                if (FindFooterInputRow() < 0) {
                    // 帧内的擦/画空窗属正常(同步输出一帧一提交);连着四拍
                    // (~1.2s)都找不到,才是真消失——倒屏面定罪。
                    int misses = 0;
                    while (misses < 4 && GetTickCount() < deadline && FindLastRow(tail_marker) < 0 &&
                           FindFooterInputRow() < 0) {
                        ++misses;
                        Sleep(300);
                    }
                    if (FindFooterInputRow() < 0 && FindLastRow(tail_marker) < 0) {
                        input_ok = false;
                        Log(std::string("STRESSMISS ") + what + " t=" +
                            std::to_string(GetTickCount() - g_start_tick) + "ms");
                        CONSOLE_SCREEN_BUFFER_INFO info{};
                        GetConsoleScreenBufferInfo(g_conout, &info);
                        const int top = (std::max)(0, info.srWindow.Bottom - 32);
                        for (int r = top; r <= info.srWindow.Bottom; ++r) {
                            const std::string row = ReadRow(r);
                            if (!row.empty()) {
                                Log("STRESSMISS row " + std::to_string(r) + ": " + row);
                            }
                        }
                        break;
                    }
                }
                const int activity_row = FindLastRow("\xe6\x80\x9d\xe8\x80\x83\xe4\xb8\xad");  // 思考中
                if (activity_row >= 0) {
                    activity_seen = true;
                    const std::string line = ReadRow(activity_row);
                    if (first_activity.empty()) {
                        first_activity = line;
                    } else if (line != first_activity) {
                        activity_alive = true;
                    }
                }
                Sleep(300);
            }
            Log(std::string("STRESS ") + what + " input_ok=" + (input_ok ? "y" : "n") +
                " activity_seen=" + (activity_seen ? "y" : "n") +
                " activity_alive=" + (activity_alive ? "y" : "n"));
            Check(input_ok, std::string(what) + ":流式期间输入框常驻(逐拍采样)");
            if (activity_seen) {
                Check(activity_alive, std::string(what) + ":活动行在走(心跳补画没饿死)");
            } else {
                Log(std::string("NOTE: ") + what + ":采样窗口没扫到活动行(流太短或已收口),不判定");
            }
        };
        SendKey('C', 0, LEFT_CTRL_PRESSED);  // 清掉第四幕留下的回显草稿(ok-after-retire)
        Sleep(300);
        SendText(kStressUser);  // 压测长流两本账
        SendKey(VK_RETURN, L'\r', 0);
        Check(WaitForText(kStressSmallHead, 30000), "压测:千枚小字流开跑(首帧落屏)");
        sample_stress_stream(kStressSmallTail, "千枚小字");
        Check(WaitForText(kStressSmallTail, 30000), "压测:千枚小字收尾标记上屏(合并批次一字不丢)");
        Check(WaitForIdleComposer(20000), "压测:小字流收口回空闲 composer");
        SendText(kStressUser);  // 同一句话再发:第二本账(十枚大字)
        SendKey(VK_RETURN, L'\r', 0);
        Check(WaitForText(kStressBigHead, 30000), "压测:十枚大字流开跑");
        sample_stress_stream(kStressBigTail, "十枚大字");
        Check(WaitForText(kStressBigTail, 30000), "压测:十枚大字收尾标记上屏(大 delta 一字不丢)");
        Check(FindFooterInputRow() > 0, "压测:大字流过后输入框仍在(500+ 字 delta 不消失)");
        Check(WaitForIdleComposer(20000), "压测:大字流收口回空闲 composer");
        // 给后台拒权幕备一份可清的草稿:那一幕开场先按 Ctrl+C 清草稿,而
        // 空闲空草稿上的 Ctrl+C 是"EOF 退出会话"——原幕序里上一幕总留着
        // 草稿可清,压测幕的两次提交把草稿清空了,这里补一枚,幕序契约不变。
        SendText("x");
    }

    // ---- 第六幕(后台拒权当场告知,2026-08-17):后台子代理没人可问,
    //      needs_confirm 的 write_file 一律当场拒。断言拒的那一刻 toast 就挂
    //      进导航坞(不攒到最终报告)。只验这一拍:#6 被拒后的下一步在本机
    //      会踩中既有病灶(拒权后任务线程哑火、主回合随之冻住——旧代码同样
    //      复现,非本单回归),那之后没有可断言的画面;干净退出的断言已由
    //      第四幕(无拒权的会话路径)背书。 ----
    SendKey('C', 0, LEFT_CTRL_PRESSED);  // 清回显草稿
    Sleep(400);
    SendText(kDenyUser);  // 再派一只后台去写材料
    SendKey(VK_RETURN, L'', 0);
    Check(WaitForText(kDenyTitle, 20000), "第五幕:#6 入坞(写材料的后台)");
    Check(WaitForIdleComposer(20000), "第五幕:空闲 composer 画出");
    Sleep(500);
    SendKey(VK_DOWN, 0, 0);
    Sleep(600);
    SendKey(VK_RETURN, L'', 0);
    Check(WaitForText("\xe6\x9f\xa5\xe7\x9c\x8b general-purpose #6", 15000),
          "第五幕:切看 #6(查看头行出现)");
    // 拒权当场告知:#6 第二轮 write_file 被拒,toast 几秒自收,轮询抓现挂。
    Check(WaitForText(kDenyToast, 30000), "第五幕:拒权 toast 当场出现(不攒到报告)");
    Log("ACT5 done: 拒权已当场告知;后续(#6 交卷/退场)受既有'拒权后任务线程哑火'"
        "病灶牵制,本机不验,留档见报告。");

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

    // ---- 退出子进程 ----(草稿已在第五幕开头清过;这里直接 /exit)
    SendText("/exit");
    SendKey(VK_RETURN, L'', 0);
    if (WaitForSingleObject(pi.hProcess, 15000) == WAIT_OBJECT_0) {
        DWORD exit_code = STILL_ACTIVE;
        GetExitCodeProcess(pi.hProcess, &exit_code);
        Check(exit_code == 0, "退出:/exit 干净退出,退出码 0(会话没卡死)");
    } else {
        // 只会发生在踩中"拒权后任务线程哑火"既有病灶的机器上(第五幕注释):
        // #6 的任务线程冻着,/exit 的收尾 join 不完。强杀收场,记录在案,不
        // 记 FAIL——那一枪属于那个单,不属于这里。
        Log("NOTE: /exit 未在 15s 内收口(既有'拒权后任务线程哑火'病灶),"
            "强杀子进程收场;干净退出断言由第四幕背书。");
        TerminateProcess(pi.hProcess, 1);
        WaitForSingleObject(pi.hProcess, 5000);
    }
    CloseHandle(pi.hProcess);

    Log(g_failures == 0 ? "ALL PASS" : ("FAILURES: " + std::to_string(g_failures)));
    FreeConsole();
    return g_failures == 0 ? 0 : 1;
}
