// 会话标题精修"交互时序四幕"专用刮屏驱动器(通知时序缺陷单·交互时序
// 回归册):与 history_search_driver 同一套手法——进程内假 anthropic SSE
// 服务,真 lubancode.exe 连上去跑真回合、真落盘;标题精修请求(max_tokens
// =24)单独识别,可"扣住不放"再择机放行,构成可控假标题后端;键直接写
// 进子进程控制台输入队列(可控 ReadLine 驱动器),刮屏认提交回显与提示符。
// 不进 ctest,要真控制台,集成验证手动跑:
//   title_wake_driver <lubancode.exe 路径> <子进程工作目录> <报告文件路径> [竞态轮数 1-20]
//
// 四幕(单子§五"交互时序测试",按当前 main 现状解读——自动标题采纳全程
// 静默,正文区不该再出现任何标题行,"标题已设为"只属人工 /title):
//   一、起飞边界:主 turn 滴流期间假服务收不到标题请求;主 turn 收口后
//      才到;账上 title_refine 旁路 turn 与主 turn 不重叠(无
//      state.turn_overlap),正文区无"标题已设为"。
//   二、空闲唤醒:标题请求扣住不放,期间一个键不喂;放行后主循环须自醒
//      收货(control.title.changed 落盘),量放行→落账时延,提示符仍在屏,
//      正文区无标题行。
//   三、草稿保护:扣住期间打入半句草稿,放行后草稿原文/光标不丢;继续
//      打字、退格、补字、提交,提交回显行逐字完整。
//   四、提交竞态:N 轮重复(缺省 6,可传参至 20)——第二问草稿在握时放行
//      标题并立刻 Enter,提交回显行下绝不出现标题行,账照落。
// 每幕之间 /clear 开新场(ResetForNewSession 复位自动起名,下一问重新
// 走本地起名+精修)。报告逐条 PASS/FAIL + 时延原始数,退出码按失败数。
// env 全量指到假服务,不碰真网络。

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#pragma comment(lib, "ws2_32.lib")

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
    if (row < 0 || row > 799) {
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

bool ScreenContains(const std::string& needle) {
    for (int row = 0; row < 400; ++row) {
        if (ReadRow(row).find(needle) != std::string::npos) {
            return true;
        }
    }
    return false;
}

int FindLastRowWith(const std::string& needle) {
    for (int row = 399; row >= 0; --row) {
        if (ReadRow(row).find(needle) != std::string::npos) {
            return row;
        }
    }
    return -1;
}

bool WaitForText(const std::string& needle, int timeout_ms) {
    const DWORD deadline = GetTickCount() + static_cast<DWORD>(timeout_ms);
    while (GetTickCount() < deadline) {
        if (ScreenContains(needle)) {
            return true;
        }
        Sleep(50);
    }
    return ScreenContains(needle);
}

void SendKey(WORD vk, wchar_t ch, DWORD mods) {
    INPUT_RECORD rec[2]{};
    rec[0].EventType = KEY_EVENT;
    rec[0].Event.KeyEvent.bKeyDown = TRUE;
    rec[0].Event.KeyEvent.wRepeatCount = 1;
    rec[0].Event.KeyEvent.wVirtualKeyCode = vk;
    rec[0].Event.KeyEvent.wVirtualScanCode = static_cast<WORD>(MapVirtualKeyW(vk, MAPVK_VK_TO_VSC));
    rec[0].Event.KeyEvent.uChar.UnicodeChar = ch;
    rec[0].Event.KeyEvent.dwControlKeyState = mods;
    rec[1] = rec[0];
    rec[1].Event.KeyEvent.bKeyDown = FALSE;
    DWORD written = 0;
    WriteConsoleInputW(g_conin, rec, 2, &written);
}

void SendText(const std::string& utf8) {
    for (const wchar_t ch : Utf8ToWide(utf8)) {
        SendKey(0, ch, 0);
    }
}

void SendEnter() {
    SendKey(VK_RETURN, L'\r', 0);
}

void SendBackspace(int count) {
    for (int i = 0; i < count; ++i) {
        SendKey(VK_BACK, L'\b', 0);
    }
}

// ---- 假 anthropic SSE 服务(可控假标题后端) ----

using SOCKET_T = SOCKET;

std::atomic<int> g_title_requests{0};      // 标题请求到达数
std::atomic<DWORD> g_title_first_seen{0};  // 首枚标题请求到达 tick
std::atomic<DWORD> g_main_done_tick{0};    // 最近一次主 turn 响应发完 tick
std::atomic<bool> g_title_hold{false};     // 扣住标题请求不放(择机放行)
std::atomic<DWORD> g_title_release_tick{0};
std::atomic<int> g_turn_seq{0};            // 主回合序号:尾锚唯一化,防旧锚残留假等

const char* kMainHead = "\xe4\xb8\xbb\xe7\xad\x94\xe8\xb5\xb7\xe8\xb7\x91";  // 主答起跑
const char* kMainTail = "\xe4\xb8\xbb\xe7\xad\x94\xe6\x94\xb6\xe5\xae\x8c";  // 主答收完
const char* kRefinedTitle = "\xe6\xa0\x87\xe9\xa2\x98\xe7\xb2\xbe\xe4\xbf\xae\xe8\x90\xbd\xe5\x9c\xb0";  // 标题精修落地
const char* kLocalTitleRow = "\xe4\xbc\x9a\xe8\xaf\x9d\xe6\xa0\x87\xe9\xa2\x98\xef\xbc\x88\xe5\x8f\x96\xe8\x87\xaa\xe9\xa6\x96\xe9\x97\xae\xef\xbc\x89";  // 会话标题(取自首问)
const char* kTitleSetRow = "\xe6\xa0\x87\xe9\xa2\x98\xe5\xb7\xb2\xe8\xae\xbe\xe4\xb8\xba";               // 标题已设为

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

std::string Sse(const std::string& json) {
    return "data: " + json + "\n\n";
}

std::string TextDeltaEvent(const std::string& text) {
    return "{\"type\":\"content_block_delta\",\"index\":0,\"delta\":{\"type\":\"text_delta\",\"text\":\"" +
           JsonEscape(text) + "\"}}";
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
    char buf[8192];
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

// 标题精修请求的判别:max_tokens 键的数值恰为 24(kTitleRefineMaxTokens,
// 单子预算钉)。主回合请求的 max_tokens 大得多,不会撞。
bool IsTitleRequest(const std::string& body) {
    const std::size_t pos = body.find("\"max_tokens\"");
    if (pos == std::string::npos) {
        return false;
    }
    std::size_t i = body.find(':', pos + 12);
    if (i == std::string::npos) {
        return false;
    }
    ++i;
    while (i < body.size() && (body[i] == ' ' || body[i] == '\t')) {
        ++i;
    }
    if (body.compare(i, 2, "24") != 0) {
        return false;
    }
    const char after = i + 2 < body.size() ? body[i + 2] : '\0';
    return after == ',' || after == '}' || after == '\0';
}

void RespondSseHead(SOCKET_T s, std::size_t body_size) {
    const std::string head = "HTTP/1.1 200 OK\r\n"
                             "Content-Type: text/event-stream\r\n"
                             "Content-Length: " +
                             std::to_string(body_size) + "\r\n" + "Connection: close\r\n\r\n";
    SendAll(s, head);
}

// 主回合剧本:正文锚句起跑 → 滴流四拍(~600ms,给"主 turn 尚开"窗) →
// 收完锚句(带回合序号——旧轮的尾锚会滚留屏上,不带序号的锚会让
// WaitForText 秒回假等,时序全乱,首跑实测的坑) + end_turn。发完记
// g_main_done_tick。
void RespondMainTurn(SOCKET_T s) {
    std::vector<std::string> leading = {
        "{\"type\":\"message_start\",\"message\":{\"id\":\"msg\",\"model\":\"fake-model\"}}",
        "{\"type\":\"content_block_start\",\"index\":0,\"content_block\":{\"type\":\"text\",\"text\":\"\"}}",
        TextDeltaEvent(std::string(kMainHead) + "\n\n")};
    std::vector<std::string> trailing = {
        TextDeltaEvent(std::string(kMainTail) + std::to_string(g_turn_seq.load())),
        "{\"type\":\"content_block_stop\",\"index\":0}",
        "{\"type\":\"message_delta\",\"delta\":{\"stop_reason\":\"end_turn\"},\"usage\":{\"input_tokens\":80,"
        "\"output_tokens\":40}}"};
    std::string body;
    for (const auto& event : leading) {
        body += Sse(event);
    }
    for (int i = 0; i < 4; ++i) {
        body += Sse(TextDeltaEvent(std::string("\xe6\xbb\xb4") + std::to_string(i) + "\n\n"));  // 滴N
    }
    for (const auto& event : trailing) {
        body += Sse(event);
    }
    RespondSseHead(s, body.size());
    SendAll(s, body.substr(0, Sse(leading[0]).size() + Sse(leading[1]).size() + Sse(leading[2]).size()));
    for (int i = 0; i < 4; ++i) {
        SendAll(s, Sse(TextDeltaEvent(std::string("\xe6\xbb\xb4") + std::to_string(i) + "\n\n")));
        Sleep(150);
    }
    for (const auto& event : trailing) {
        SendAll(s, Sse(event));
    }
    g_main_done_tick.store(GetTickCount());
}

// 标题剧本:可扣住。扣住时等 g_title_hold 翻假才答(放行时刻记档);答案
// 是一枚短标题 + end_turn。标题请求只认次序不认内容差异——一场一问。
void RespondTitleRefine(SOCKET_T s) {
    const int seq = g_title_requests.fetch_add(1) + 1;
    if (seq == 1) {
        g_title_first_seen.store(GetTickCount());
    }
    Log("SERVER title request #" + std::to_string(seq) + " at " + std::to_string(GetTickCount()));
    while (g_title_hold.load()) {
        Sleep(10);
    }
    g_title_release_tick.store(GetTickCount());
    std::string body;
    body += Sse("{\"type\":\"message_start\",\"message\":{\"id\":\"msg\",\"model\":\"fake-model\"}}");
    body += Sse("{\"type\":\"content_block_start\",\"index\":0,\"content_block\":{\"type\":\"text\",\"text\":\"\"}}");
    body += Sse(TextDeltaEvent(kRefinedTitle));
    body += Sse("{\"type\":\"content_block_stop\",\"index\":0}");
    body += Sse("{\"type\":\"message_delta\",\"delta\":{\"stop_reason\":\"end_turn\"},\"usage\":{\"input_tokens\":60,"
                "\"output_tokens\":8}}");
    RespondSseHead(s, body.size());
    SendAll(s, body);
    Log("SERVER title answered #" + std::to_string(seq) + " at " + std::to_string(GetTickCount()));
}

u_short StartFakeServer() {
    WSADATA wsa{};
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        Log("FAIL: WSAStartup");
        return 0;
    }
    const SOCKET_T listener = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listener == INVALID_SOCKET) {
        Log("FAIL: socket");
        return 0;
    }
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;
    if (::bind(listener, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0 ||
        ::listen(listener, 8) != 0) {
        Log("FAIL: fake server bind/listen");
        return 0;
    }
    int len = sizeof(addr);
    ::getsockname(listener, reinterpret_cast<sockaddr*>(&addr), &len);
    const u_short port = ntohs(addr.sin_port);
    std::thread([listener]() {
        for (;;) {
            sockaddr_in client{};
            int client_len = sizeof(client);
            const SOCKET_T fd =
                ::accept(listener, reinterpret_cast<sockaddr*>(&client), &client_len);
            if (fd == INVALID_SOCKET) {
                return;
            }
            std::thread([fd]() {
                for (;;) {  // 一连接可能串多请求(keep-alive);读完一答一
                    const std::string raw = DrainHttpRequest(fd);
                    if (raw.empty()) {
                        break;
                    }
                    if (IsTitleRequest(raw)) {
                        RespondTitleRefine(fd);
                    } else {
                        RespondMainTurn(fd);
                    }
                }
                ::closesocket(fd);
            }).detach();
        }
    }).detach();
    return port;
}

// ---- 账面数数:隔离 home 下所有 main.jsonl 汇总 ----

struct LedgerStats {
    int title_changed = 0;    // control.title.changed
    int refine_prepared = 0;  // purpose=title_refine 的 model.request.prepared
    int turn_overlap = 0;     // state.turn_overlap(不许出现)
    bool found = false;
};

void CollectJsonlFiles(const std::wstring& dir, std::vector<std::wstring>* out) {
    WIN32_FIND_DATAW fd{};
    const HANDLE find = FindFirstFileW((dir + L"\\*").c_str(), &fd);
    if (find == INVALID_HANDLE_VALUE) {
        return;
    }
    do {
        const std::wstring name = fd.cFileName;
        if (name == L"." || name == L"..") {
            continue;
        }
        const std::wstring full = dir + L"\\" + name;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            CollectJsonlFiles(full, out);
        } else if (name.size() > 6 && name.substr(name.size() - 6) == L".jsonl") {
            out->push_back(full);
        }
    } while (FindNextFileW(find, &fd));
    FindClose(find);
}

int CountInFile(const std::wstring& path, const std::string& needle) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        return 0;
    }
    std::string line;
    int count = 0;
    while (std::getline(file, line)) {
        if (line.find(needle) != std::string::npos) {
            ++count;
        }
    }
    return count;
}

// 全 home 汇总:每场一枚 main.jsonl(含 title.changed 的事件行)。
LedgerStats CountLedger(const std::wstring& home) {
    LedgerStats stats;
    std::vector<std::wstring> files;
    CollectJsonlFiles(home + L"\\.lubancode\\workspaces", &files);
    std::vector<std::wstring> main_files;
    for (const auto& file : files) {
        const std::wstring name = file.substr(file.find_last_of(L"\\") + 1);
        if (name == L"main.jsonl") {
            main_files.push_back(file);
        }
    }
    stats.found = !main_files.empty();
    for (const auto& file : main_files) {
        stats.title_changed += CountInFile(file, "\"kind\":\"control.title.changed\"");
        stats.turn_overlap += CountInFile(file, "\"kind\":\"state.turn_overlap\"");
        std::ifstream in(file, std::ios::binary);
        std::string line;
        while (std::getline(in, line)) {
            if (line.find("\"kind\":\"model.request.prepared\"") != std::string::npos &&
                line.find("\"purpose\":\"title_refine\"") != std::string::npos) {
                ++stats.refine_prepared;
            }
        }
    }
    return stats;
}

// 有界等账面条件(control.title.changed 达到 expected 或以上)。
LedgerStats WaitForTitleChanged(const std::wstring& home, int expected, int timeout_ms) {
    const DWORD deadline = GetTickCount() + static_cast<DWORD>(timeout_ms);
    LedgerStats stats = CountLedger(home);
    while (GetTickCount() < deadline && stats.title_changed < expected) {
        Sleep(25);
        stats = CountLedger(home);
    }
    return stats;
}

// 数场次目录(/clear 开新场的硬证据)。
int CountSessions(const std::wstring& home) {
    std::vector<std::wstring> files;
    CollectJsonlFiles(home + L"\\.lubancode\\workspaces", &files);
    int sessions = 0;
    for (const auto& file : files) {
        if (file.find(L"\\sessions\\") != std::wstring::npos &&
            file.substr(file.find_last_of(L"\\") + 1) == L"main.jsonl") {
            ++sessions;
        }
    }
    return sessions;
}

// /clear 开新场:发令后**硬同步**——轮询场次目录数 +1 才算换场成功。
// 只靠定时睡不保险(首跑实测:/clear 间歇不生效,后续首问全进老场,一场
// 一次精修,标题请求永远不来);等不到就明败,锅归 /clear 不归时序。
bool ClearSessionAndWaitForNew(const std::wstring& home, const std::string& stage) {
    const int before = CountSessions(home);
    SendText("/clear");
    SendEnter();
    const DWORD deadline = GetTickCount() + 10000;
    while (GetTickCount() < deadline && CountSessions(home) <= before) {
        Sleep(50);
    }
    const bool ok = CountSessions(home) > before;
    Check(ok, stage + ":/clear 换场(场次目录 +1)");
    if (ok) {
        Sleep(400);  // 换账收尾(UI 重建)与首问之间留一口气
    }
    return ok;
}

void SetEnv(const std::wstring& name, const std::wstring& value) {
    SetEnvironmentVariableW(name.c_str(), value.c_str());
}

// 发一条话并等主答收完(回合收口锚带序号,防旧锚残留假等)。
void AskAndAwaitReply(const std::string& text) {
    const int seq = g_turn_seq.fetch_add(1) + 1;
    SendText(text);
    SendEnter();
    WaitForText(std::string(kMainTail) + std::to_string(seq), 30000);
    Sleep(700);  // 收口 + 空闲边界(标题起飞点) + 提示符重画
}

// /clear 开新场:见 ClearSessionAndWaitForNew(硬同步在那一侧)。

}  // namespace

// 路径转绝对:USERPROFILE 若带相对成分,子进程会把它相对自己的 cwd 再拼
// 一层(workspaces 落点嵌套漂移,首跑实测的坑)。
std::wstring ToAbsolute(const wchar_t* raw) {
    wchar_t buf[MAX_PATH]{};
    _wfullpath(buf, raw, MAX_PATH);
    return std::wstring(buf);
}

int wmain(int argc, wchar_t** argv) {
    if (argc < 4) {
        std::fprintf(stderr, "usage: title_wake_driver <lubancode.exe> <workdir> <report> [rounds 1-20]\n");
        return 2;
    }
    const std::wstring exe_path = ToAbsolute(argv[1]);
    const std::wstring workdir = ToAbsolute(argv[2]);
    const std::wstring report_path = ToAbsolute(argv[3]);
    int rounds = 6;
    if (argc >= 5) {
        rounds = _wtoi(argv[4]);
        if (rounds < 1) rounds = 1;
        if (rounds > 20) rounds = 20;
    }
    g_report.open(WideToUtf8(report_path), std::ios::binary | std::ios::trunc);
    if (!g_report) {
        return 2;
    }
    CreateDirectoryW(workdir.c_str(), nullptr);

    const u_short port = StartFakeServer();
    if (port == 0) {
        return 2;
    }
    Log("fake anthropic server on 127.0.0.1:" + std::to_string(port));

    // 隔离家底:USERPROFILE 指到本进程私目录,workspaces 落这底下,验收后
    // 数账。
    std::wstring hermetic_home;
    {
        hermetic_home = workdir + L"\\twake_home_" + std::to_wstring(GetCurrentProcessId());
        CreateDirectoryW(hermetic_home.c_str(), nullptr);
        SetEnv(L"USERPROFILE", hermetic_home);
    }
    SetEnv(L"LUBANCODE_WIRE", L"anthropic");
    SetEnv(L"LUBANCODE_BASE_URL", L"http://127.0.0.1:" + std::to_wstring(port));
    SetEnv(L"LUBANCODE_API_KEY", L"title-wake-driver");
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
    g_conin = CreateFileW(L"CONIN$", GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                          &inheritable, OPEN_EXISTING, 0, nullptr);
    g_conout = CreateFileW(L"CONOUT$", GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                           &inheritable, OPEN_EXISTING, 0, nullptr);
    if (g_conin == INVALID_HANDLE_VALUE || g_conout == INVALID_HANDLE_VALUE) {
        Log("FAIL: open CONIN$/CONOUT$");
        return 1;
    }
    SMALL_RECT small{0, 0, 1, 1};
    SetConsoleWindowInfo(g_conout, TRUE, &small);
    SetConsoleScreenBufferSize(g_conout, COORD{120, 800});
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
    if (!CreateProcessW(exe_path.c_str(), cmdline.data(), nullptr, nullptr, TRUE, 0, nullptr, workdir.c_str(),
                        &si, &pi)) {
        Log("FAIL: CreateProcess " + std::to_string(GetLastError()));
        return 1;
    }
    CloseHandle(pi.hThread);

    Check(WaitForText("fake-model", 30000), "开场:状态栏出现(程序活着)");
    Sleep(500);

    // ---- 幕一:起飞边界 ----
    {
        Log("---- 幕一:主 turn 尚开时精修不起飞,收口后才起飞 ----");
        const int titles_before = g_title_requests.load();
        const int turn1 = g_turn_seq.fetch_add(1) + 1;
        SendText("一号首问问点事情");
        SendEnter();
        // 主 turn 滴流中(~600ms 窗):标题请求一枚都不许到。
        Check(WaitForText(kMainHead, 20000), "幕一:主答起跑上屏");
        Sleep(500);
        Check(g_title_requests.load() == titles_before,
              "幕一:主 turn 尚开期间标题请求一枚未发(got " +
                  std::to_string(g_title_requests.load() - titles_before) + ")");
        Check(WaitForText(std::string(kMainTail) + std::to_string(turn1), 20000), "幕一:主答收完上屏");
        Sleep(900);  // 收口 + 空闲边界起飞
        const DWORD deadline = GetTickCount() + 8000;
        while (g_title_requests.load() == titles_before && GetTickCount() < deadline) {
            Sleep(25);
        }
        Check(g_title_requests.load() > titles_before, "幕一:主 turn 收口后标题请求到达");
        const DWORD title_seen = g_title_first_seen.load();
        const DWORD main_done = g_main_done_tick.load();
        Check(title_seen >= main_done,
              "幕一:标题请求到达不早于主答发完(标题 " + std::to_string(title_seen) + " vs 主答 " +
                  std::to_string(main_done) + ")");
        // 账面:本地+精修两条 title.changed,一枚 title_refine prepared,无重叠。
        const LedgerStats stats = WaitForTitleChanged(hermetic_home, 2, 8000);
        Check(stats.found, "幕一:main.jsonl 落盘");
        Check(stats.title_changed == 2, "幕一:本地+精修恰两条 title.changed(got " +
                                            std::to_string(stats.title_changed) + ")");
        Check(stats.refine_prepared == 1, "幕一:恰一枚 title_refine prepared(got " +
                                              std::to_string(stats.refine_prepared) + ")");
        Check(stats.turn_overlap == 0, "幕一:无 state.turn_overlap");
        Check(!ScreenContains(kTitleSetRow), "幕一:正文区无标题已设为(采纳静默)");
    }

    // ---- 幕二:空闲唤醒 ----
    {
        Log("---- 幕二:不喂键,精修完成后主循环自醒收货 ----");
        if (!ClearSessionAndWaitForNew(hermetic_home, "幕二")) {
            Log("幕二:换场失败,跳过本幕");
        }
        const int titles_before = g_title_requests.load();
        g_title_hold.store(true);
        AskAndAwaitReply("二号首问问点别的");
        const DWORD deadline = GetTickCount() + 8000;
        while (g_title_requests.load() == titles_before && GetTickCount() < deadline) {
            Sleep(25);
        }
        Check(g_title_requests.load() > titles_before, "幕二:新场标题请求到达");
        // 新场重新本地起名的证据走账面:title.changed 从 2 涨到 4(本场本地
        // +精修);本地起名行被流式重画滚出可视区,屏面锚不作数。
        Sleep(800);  // 扣住期间:一个键不喂,留给"假唤醒也不会有"的静默窗
        // 放行,量"放行→control.title.changed 落盘"时延。
        const DWORD t0 = GetTickCount();
        g_title_hold.store(false);
        const int target = CountLedger(hermetic_home).title_changed + 1;
        LedgerStats stats;
        const DWORD poll_deadline = t0 + 5000;
        while (GetTickCount() < poll_deadline) {
            stats = CountLedger(hermetic_home);
            if (stats.title_changed >= target) {
                break;
            }
            Sleep(25);
        }
        const DWORD elapsed = GetTickCount() - t0;
        Check(stats.title_changed >= target,
              "幕二:不喂键标题也落账(title.changed " + std::to_string(stats.title_changed) + ")");
        Check(elapsed < 1000, "幕二:放行到落账 <1000ms(实测 " + std::to_string(elapsed) + "ms)");
        Log("幕二时延原始数: 放行→title.changed 落账 = " + std::to_string(elapsed) + "ms");
        Check(FindLastRowWith("> ") >= 0, "幕二:提示符仍在屏(空闲重画)");
        Check(!ScreenContains(kTitleSetRow), "幕二:正文区无标题行(采纳静默)");
        Check(stats.turn_overlap == 0, "幕二:无 state.turn_overlap");
    }

    // ---- 幕三:草稿保护 ----
    {
        Log("---- 幕三:半句草稿在握,精修完成不抢输入 ----");
        if (!ClearSessionAndWaitForNew(hermetic_home, "幕三")) {
            Log("幕三:换场失败,跳过本幕");
        }
        const int titles_before = g_title_requests.load();
        g_title_hold.store(true);
        AskAndAwaitReply("三号首问问点草稿");
        const DWORD deadline = GetTickCount() + 8000;
        while (g_title_requests.load() == titles_before && GetTickCount() < deadline) {
            Sleep(25);
        }
        Check(g_title_requests.load() > titles_before, "幕三:新场标题请求到达");
        // 半句草稿:打进去,不提交。
        const std::string draft_head = "半句草稿开头没提交";
        const std::string draft_more = "后半句补齐";  // 先打,再退格删掉末 2 字
        const std::string draft_tail = "结尾";
        SendText(draft_head);
        Sleep(400);
        Check(ScreenContains(draft_head), "幕三:半句草稿上屏");
        // 草稿在握时放行标题;唤醒拍看见 Ready,但草稿非空不让位。
        g_title_hold.store(false);
        Sleep(900);  // 足够一个 100ms 拍尝试让位
        Check(ScreenContains(draft_head), "幕三:放行后草稿原文还在(未被抢走)");
        // 继续打字、退格两下(删"补齐")、补字、提交:原文逐字不丢。
        SendText(draft_more);
        SendBackspace(2);
        SendText(draft_tail);
        const std::string expected_echo = draft_head + "后半句" + draft_tail;
        const int draft_turn = g_turn_seq.fetch_add(1) + 1;
        SendEnter();
        Sleep(600);
        const int echo_row = FindLastRowWith(expected_echo);
        Check(echo_row >= 0, "幕三:提交回显行逐字完整(" + expected_echo + ")");
        Check(WaitForText(std::string(kMainTail) + std::to_string(draft_turn), 20000),
              "幕三:第三场主答收完");
        // 账照落:幕一 2 + 幕二 2 + 本场本地+精修 2 = 6。
        const LedgerStats stats = WaitForTitleChanged(hermetic_home, 6, 8000);
        Check(stats.title_changed >= 6, "幕三:三场标题全落账(got " + std::to_string(stats.title_changed) + ")");
        Check(!ScreenContains(kTitleSetRow), "幕三:正文区无标题行");
    }

    // ---- 幕四:提交竞态(N 轮) ----
    {
        Log("---- 幕四:第二问提交与精修完成的竞态,重复 " + std::to_string(rounds) + " 轮 ----");
        for (int round = 1; round <= rounds; ++round) {
            if (!ClearSessionAndWaitForNew(hermetic_home, "幕四第" + std::to_string(round) + "轮")) {
                continue;  // 换场失败已记 FAIL,这轮没法做
            }
            const int titles_before = g_title_requests.load();
            g_title_hold.store(true);
            AskAndAwaitReply("四号首问第" + std::to_string(round) + "轮");
            DWORD deadline = GetTickCount() + 8000;
            while (g_title_requests.load() == titles_before && GetTickCount() < deadline) {
                Sleep(25);
            }
            if (g_title_requests.load() == titles_before) {
                Check(false, "幕四第" + std::to_string(round) + "轮:标题请求未到");
                // 收尾放行:迟到的请求别扣着污染下一轮的计数与门闩。
                g_title_hold.store(false);
                Sleep(1200);
                continue;
            }
            const std::string second = "第二问抢在标题完成临界点第" + std::to_string(round) + "轮";
            SendText(second);
            Sleep(400);  // 草稿上屏
            const int before_changed = CountLedger(hermetic_home).title_changed;
            g_title_hold.store(false);
            // 竞态窗两形态交替:偶数轮放行即提交(标题完成与 Enter 撞同一拍),
            // 奇数轮给 60ms(标题先 Ready,Enter 紧随)。
            if (round % 2 == 1) {
                Sleep(60);
            }
            const int second_turn = g_turn_seq.fetch_add(1) + 1;
            SendEnter();
            if (!WaitForText(std::string(kMainTail) + std::to_string(second_turn), 20000)) {
                Check(false, "幕四第" + std::to_string(round) + "轮:第二问主答未收口");
                continue;
            }
            Sleep(600);
            // 断言:正文区绝不出现标题行(现状:采纳静默,"第二问\n标题已设为"
            // 的排版从根上不可能);提交回显在屏;账面本轮精修已落。
            const bool bad_notice = ScreenContains(kTitleSetRow);
            const int second_row = FindLastRowWith(second);
            LedgerStats stats;
            deadline = GetTickCount() + 8000;
            const int target = before_changed + 1;  // 本场精修采纳那条(本地已在 before 里)
            do {
                stats = CountLedger(hermetic_home);
                if (stats.title_changed >= target) {
                    break;
                }
                Sleep(25);
            } while (GetTickCount() < deadline);
            Check(!bad_notice, "幕四第" + std::to_string(round) + "轮:正文区无标题行");
            Check(second_row >= 0, "幕四第" + std::to_string(round) + "轮:第二问提交回显在屏");
            Check(stats.title_changed >= target,
                  "幕四第" + std::to_string(round) + "轮:本轮本地+精修均落账(got " +
                      std::to_string(stats.title_changed) + ", want " + std::to_string(target) + ")");
            Check(stats.turn_overlap == 0, "幕四第" + std::to_string(round) + "轮:无 state.turn_overlap");
        }
    }

    // 收场:exit 退场,报告汇总。
    SendText("exit");
    SendEnter();
    if (WaitForSingleObject(pi.hProcess, 10000) != WAIT_OBJECT_0) {
        TerminateProcess(pi.hProcess, 0);
        Log("FAIL: 子进程未在 10s 内退场,已Terminate");
        ++g_failures;
    }
    CloseHandle(pi.hProcess);

    Log(g_failures == 0 ? "ALL PASS" : ("FAILURES: " + std::to_string(g_failures)));
    return g_failures == 0 ? 0 : 1;
}
