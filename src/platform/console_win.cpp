// Windows 实现:控制台原语。全部搬自现有代码,逻辑一字未改——
//   StdinIsInteractive        <- cli/console_input.cpp 的 StdinIsRealConsole
//   ProbeStdoutConsole        <- cli/theme.cpp 的 DetectConsoleCapability(探测段)
//   ConsoleWidth              <- cli/console_input.cpp 的 DetectConsoleWidth
//   GetScreenInfo/SetCursorPos/ClearRowFrom
//                             <- console_input.cpp / main.cpp 里反复出现的
//                                GetConsoleScreenBufferInfo /
//                                SetConsoleCursorPosition /
//                                FillConsoleOutputCharacterW 三件套
//   RawInputScope             <- ReadLineKeyByKey 开头的 GetConsoleMode/
//                                SetConsoleMode + ModeGuard
//   KeyReader::ReadOne        <- ReadLineKeyByKey / TurnInputListener 里的
//                                ReadConsoleInputW + KEY_EVENT 翻译(含 UTF-16
//                                代理对配对;两处的映射合成这一份,超集)
//   ReadLineCooked            <- ReadLineFromConsole
//   SetupConsoleUtf8          <- wmain 开头的 SetConsoleOutputCP/SetConsoleCP
#include "platform/console.hpp"

#include "platform/paste_burst.hpp"  // ClassifyTextBurst:同批字符折一次粘贴事务的纯判别
#include "platform/paths.hpp"        // Utf8ToWide/WideToUtf8:输入侧宽窄转换共用 platform 那份(不许抛)

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <iostream>
#include <iterator>
#include <deque>
#include <string_view>
#include <vector>

namespace lubancode::platform {

// NativeRowCell 的属性位与 Win32 控制台常量必须逐位同值(单 2 二轮·8.2:
// WriteNativeRow 直接把它们搬进 CHAR_INFO.Attributes,不同值就是画错色)。
static_assert(kNativeFgBlue == FOREGROUND_BLUE, "fg blue bit mismatch");
static_assert(kNativeFgGreen == FOREGROUND_GREEN, "fg green bit mismatch");
static_assert(kNativeFgRed == FOREGROUND_RED, "fg red bit mismatch");
static_assert(kNativeFgIntensity == FOREGROUND_INTENSITY, "fg intensity bit mismatch");
static_assert(kNativeBgBlue == BACKGROUND_BLUE, "bg blue bit mismatch");
static_assert(kNativeBgGreen == BACKGROUND_GREEN, "bg green bit mismatch");
static_assert(kNativeBgRed == BACKGROUND_RED, "bg red bit mismatch");
static_assert(kNativeBgIntensity == BACKGROUND_INTENSITY, "bg intensity bit mismatch");
static_assert(kNativeCellLeading == COMMON_LVB_LEADING_BYTE, "wide-char leading flag mismatch");
static_assert(kNativeCellTrailing == COMMON_LVB_TRAILING_BYTE, "wide-char trailing flag mismatch");

// 输入侧(剪贴板/按键)的宽窄转换直接用 paths_win 那两份(Utf8ToWide/
// WideToUtf8,经上面的 paths.hpp 引进来):同一份"坏字符替换 U+FFFD、
// 绝不抛"的合同,不在这只文件再养一份私有实现。

namespace {

std::deque<INPUT_RECORD>& PendingInputRecords() {
    static std::deque<INPUT_RECORD> records;
    return records;
}

bool ReadInputRecord(INPUT_RECORD& record, DWORD timeout_ms = INFINITE) {
    auto& pending = PendingInputRecords();
    if (!pending.empty()) {
        record = pending.front();
        pending.pop_front();
        return true;
    }
    const HANDLE input = GetStdHandle(STD_INPUT_HANDLE);
    if (timeout_ms != INFINITE && WaitForSingleObject(input, timeout_ms) != WAIT_OBJECT_0) {
        return false;
    }
    DWORD read = 0;
    return ReadConsoleInputW(input, &record, 1, &read) != 0 && read == 1;
}

void RestoreInputRecords(const std::vector<INPUT_RECORD>& records) {
    auto& pending = PendingInputRecords();
    for (auto it = records.rbegin(); it != records.rend(); ++it) {
        pending.push_front(*it);
    }
}

std::optional<wchar_t> ReadKeyChar(DWORD timeout_ms, std::vector<INPUT_RECORD>& consumed) {
    while (true) {
        INPUT_RECORD record{};
        if (!ReadInputRecord(record, timeout_ms)) {
            return std::nullopt;
        }
        consumed.push_back(record);
        if (record.EventType == KEY_EVENT && record.Event.KeyEvent.bKeyDown != FALSE &&
            record.Event.KeyEvent.uChar.UnicodeChar != 0) {
            return record.Event.KeyEvent.uChar.UnicodeChar;
        }
    }
}

std::wstring NormalizeNewlines(std::wstring_view text) {
    std::wstring normalized;
    normalized.reserve(text.size());
    for (std::size_t i = 0; i < text.size(); ++i) {
        if (text[i] == L'\r') {
            normalized.push_back(L'\n');
            if (i + 1 < text.size() && text[i + 1] == L'\n') {
                ++i;
            }
        } else {
            normalized.push_back(text[i]);
        }
    }
    return normalized;
}

std::optional<std::wstring> ReadClipboardText() {
    // VS Code 刚把正文写进剪贴板时，别的进程偶尔还攥着 clipboard 锁。
    // 略试几次便走；拿不到就退回原先的事件批探测，不耽误普通输入。
    bool opened = false;
    for (int attempt = 0; attempt < 5; ++attempt) {
        if (OpenClipboard(nullptr) != FALSE) {
            opened = true;
            break;
        }
        Sleep(5);
    }
    if (!opened) {
        return std::nullopt;
    }

    const HANDLE data = GetClipboardData(CF_UNICODETEXT);
    if (data == nullptr) {
        CloseClipboard();
        return std::nullopt;
    }
    const auto* chars = static_cast<const wchar_t*>(GlobalLock(data));
    if (chars == nullptr) {
        CloseClipboard();
        return std::nullopt;
    }
    const std::size_t capacity = GlobalSize(data) / sizeof(wchar_t);
    std::size_t length = 0;
    while (length < capacity && chars[length] != L'\0') {
        ++length;
    }
    std::wstring text(chars, length);
    GlobalUnlock(data);
    CloseClipboard();
    return text;
}

std::optional<std::wstring> MatchingMultilineClipboard(const std::wstring& prefix) {
    const std::optional<std::wstring> raw = ReadClipboardText();
    if (!raw.has_value()) {
        return std::nullopt;
    }
    std::wstring clipboard = NormalizeNewlines(*raw);
    if (clipboard.find(L'\n') == std::wstring::npos || prefix.size() > clipboard.size() ||
        clipboard.compare(0, prefix.size(), prefix) != 0) {
        return std::nullopt;
    }
    return clipboard;
}

std::optional<KeyInput> TryReadBracketedPaste() {
    constexpr std::wstring_view kStart = L"[200~";
    std::vector<INPUT_RECORD> probe;
    for (const wchar_t expected : kStart) {
        const std::optional<wchar_t> actual = ReadKeyChar(30, probe);
        if (!actual.has_value() || *actual != expected) {
            RestoreInputRecords(probe);
            return std::nullopt;
        }
    }

    constexpr std::wstring_view kEnd = L"\x1b[201~";
    std::wstring pasted;
    while (true) {
        INPUT_RECORD record{};
        if (!ReadInputRecord(record)) {
            break;
        }
        if (record.EventType != KEY_EVENT || record.Event.KeyEvent.bKeyDown == FALSE ||
            record.Event.KeyEvent.uChar.UnicodeChar == 0) {
            continue;
        }
        const KEY_EVENT_RECORD& key = record.Event.KeyEvent;
        const unsigned repeat = (std::max)(1U, static_cast<unsigned>(key.wRepeatCount));
        for (unsigned i = 0; i < repeat; ++i) {
            pasted.push_back(key.uChar.UnicodeChar);
        }
        if (pasted.size() >= kEnd.size() &&
            pasted.compare(pasted.size() - kEnd.size(), kEnd.size(), kEnd) == 0) {
            pasted.resize(pasted.size() - kEnd.size());
            break;
        }
    }

    KeyInput out;
    out.kind = KeyInput::Kind::Paste;
    out.text = WideToUtf8(NormalizeNewlines(pasted));
    return out;
}

bool AppendNativePasteKey(const INPUT_RECORD& record, std::wstring& text, bool& saw_newline,
                          bool& saw_text_after_newline) {
    if (record.EventType != KEY_EVENT) {
        return false;
    }
    const KEY_EVENT_RECORD& key = record.Event.KeyEvent;
    if (key.bKeyDown == FALSE) {
        return true;  // key-up 是同一批输入的一半，不进正文
    }
    const bool modifier_only = key.uChar.UnicodeChar == 0 &&
                               (key.wVirtualKeyCode == VK_SHIFT || key.wVirtualKeyCode == VK_LSHIFT ||
                                key.wVirtualKeyCode == VK_RSHIFT || key.wVirtualKeyCode == VK_CONTROL ||
                                key.wVirtualKeyCode == VK_LCONTROL || key.wVirtualKeyCode == VK_RCONTROL ||
                                key.wVirtualKeyCode == VK_MENU || key.wVirtualKeyCode == VK_LMENU ||
                                key.wVirtualKeyCode == VK_RMENU);
    if (modifier_only) {
        // Windows Terminal 把 paste 还原成一串逼真的 KEY_EVENT：括号、下划线
        // 前后会夹 Shift down/up，快捷键本身也会漏进 Ctrl/Shift down。它们
        // 没有正文字符，只是这趟 paste 的骨架，跳过即可。
        return true;
    }
    const bool ctrl = (key.dwControlKeyState & (LEFT_CTRL_PRESSED | RIGHT_CTRL_PRESSED)) != 0;
    const bool alt = (key.dwControlKeyState & (LEFT_ALT_PRESSED | RIGHT_ALT_PRESSED)) != 0;
    if (ctrl || alt) {
        return false;
    }

    wchar_t ch = key.uChar.UnicodeChar;
    if (key.wVirtualKeyCode == VK_RETURN || ch == L'\r' || ch == L'\n') {
        if (saw_newline) {
            saw_text_after_newline = true;  // 第二枚换行本身也说明这是多行内容
        }
        ch = L'\n';
        saw_newline = true;
    } else if (ch != L'\t' && ch < L' ') {
        return false;
    } else if (saw_newline) {
        saw_text_after_newline = true;
    }

    const unsigned repeat = (std::max)(1U, static_cast<unsigned>(key.wRepeatCount));
    text.append(repeat, ch);
    return true;
}

// VS Code/ConPTY 并不保证 bracketed paste 会以一条完整 VT 序列出现在
// ReadConsoleInputW 里；有些组合只把整段文字压成一批 KEY_EVENT。若照逐键
// 翻译，其中第一枚 VK_RETURN 会直接提交，余下各行便散成多条消息。
//
// 这里只认“一批连续文本里，换行后还有正文”的形状。ConPTY 可能把一次
// paste 拆成几批，第一批还偏生停在换行上；一见换行便留 60ms 的空闲窗口
// 续收，直到整批安静下来。普通打字只在按 Enter 时多等这一小拍，换行后
// 没正文仍按提交处理；方向键、Ctrl/Alt 等编辑键一混进来也整批原样放回。
std::optional<KeyInput> TryReadNativePasteBurst(const INPUT_RECORD& first) {
    std::vector<INPUT_RECORD> tail;
    INPUT_RECORD record{};
    while (ReadInputRecord(record, 0)) {
        tail.push_back(record);
    }

    std::wstring text;
    bool saw_newline = false;
    bool saw_text_after_newline = false;
    bool valid = AppendNativePasteKey(first, text, saw_newline, saw_text_after_newline);
    for (const INPUT_RECORD& item : tail) {
        if (!AppendNativePasteKey(item, text, saw_newline, saw_text_after_newline)) {
            valid = false;
            break;
        }
    }

    // ConPTY 有时按“每行一批”投递。一批之间能隔上百毫秒，靠空闲窗口
    // 猜边界终究会漏。首批若已含换行，就同 Windows 剪贴板逐字核对；
    // 对上后按剪贴板的确切长度收齐，既不误吞手敲 Enter，也不怕批间停顿。
    if (valid && saw_newline) {
        if (const std::optional<std::wstring> clipboard = MatchingMultilineClipboard(text);
            clipboard.has_value()) {
            constexpr ULONGLONG kClipboardCompletionMs = 2000;
            const ULONGLONG deadline = GetTickCount64() + kClipboardCompletionMs;
            while (valid && text.size() < clipboard->size()) {
                const ULONGLONG now = GetTickCount64();
                if (now >= deadline || !ReadInputRecord(record, static_cast<DWORD>(deadline - now))) {
                    break;
                }
                tail.push_back(record);
                if (record.EventType != KEY_EVENT || record.Event.KeyEvent.bKeyDown == FALSE) {
                    continue;
                }
                if (!AppendNativePasteKey(record, text, saw_newline, saw_text_after_newline) ||
                    text.size() > clipboard->size() || clipboard->compare(0, text.size(), text) != 0) {
                    valid = false;
                }
            }
            if (valid && text == *clipboard) {
                KeyInput out;
                out.kind = KeyInput::Kind::Paste;
                out.text = WideToUtf8(text);
                return out;
            }
        }
    }

    if (valid && saw_newline) {
        constexpr DWORD kContinuationIdleMs = 60;
        constexpr ULONGLONG kMaxContinuationMs = 1000;
        const ULONGLONG deadline = GetTickCount64() + kMaxContinuationMs;
        while (GetTickCount64() < deadline) {
            if (!ReadInputRecord(record, kContinuationIdleMs)) {
                break;  // 连续 60ms 没新字，这趟 paste 收口
            }
            tail.push_back(record);
            if (!AppendNativePasteKey(record, text, saw_newline, saw_text_after_newline)) {
                valid = false;
                break;
            }
            // 一枚到手后，把同一刻已经排队的也全吸进来；下一圈再等后续批。
            while (ReadInputRecord(record, 0)) {
                tail.push_back(record);
                if (!AppendNativePasteKey(record, text, saw_newline, saw_text_after_newline)) {
                    valid = false;
                    break;
                }
            }
            if (!valid) {
                break;
            }
        }
    }
    if (!valid || !saw_newline || !saw_text_after_newline) {
        RestoreInputRecords(tail);
        return std::nullopt;
    }

    KeyInput out;
    out.kind = KeyInput::Kind::Paste;
    out.text = WideToUtf8(text);
    return out;
}

// P2-4 输入重绘风暴的另一半:单行大粘贴(既没有换行、也没有 bracketed
// 标记,TryReadNativePasteBurst 的形状认不出)会逐字当打字交付,每个字
// 一轮终端帧。这里在逐键交付前先看一眼:同一时刻排队的一批纯文本攒过
// 阈值(paste_burst.hpp),整批折成一枚 Paste——一次编辑事务、一轮帧。
// 前几枚已经当打字交付出去的字符(跨批的粘贴尾巴)由 prior_text 带上,
// replace_before 交代编辑器把它们原位撤下再并进附件,与 Enter 路的
// TryFinishClipboardPaste 同一份账。
//
// 不越线(正常打字/输入法整词提交)就把吸进来的记录原样还回队列,
// 只当这一枚没来过,交付路径一字不改。
std::optional<KeyInput> TryReadCoalescedTextBurst(const INPUT_RECORD& first,
                                                  const std::wstring& prior_text,
                                                  std::size_t prior_chars) {
    std::vector<INPUT_RECORD> tail;
    INPUT_RECORD record{};
    while (ReadInputRecord(record, 0)) {
        tail.push_back(record);
    }

    std::wstring text;
    bool saw_newline = false;
    bool saw_text_after_newline = false;
    AppendNativePasteKey(first, text, saw_newline, saw_text_after_newline);
    std::size_t consumed = 0;
    for (std::size_t i = 0; i < tail.size(); ++i) {
        if (!AppendNativePasteKey(tail[i], text, saw_newline, saw_text_after_newline)) {
            break;  // 编辑键来了:只并它前面的文本,其余原样还回
        }
        ++consumed;
    }
    const TextBurstDecision decision = ClassifyTextBurst(std::move(text));
    if (!decision.is_paste) {
        RestoreInputRecords(tail);
        return std::nullopt;
    }
    if (consumed < tail.size()) {
        RestoreInputRecords(
            std::vector<INPUT_RECORD>(tail.begin() + static_cast<std::ptrdiff_t>(consumed), tail.end()));
    }

    KeyInput out;
    out.kind = KeyInput::Kind::Paste;
    out.text = WideToUtf8(NormalizeNewlines(prior_text + decision.text));
    out.replace_before = prior_chars;
    return out;
}

std::optional<KeyInput> TryFinishClipboardPaste(std::wstring received, std::size_t replace_before) {
    const std::optional<std::wstring> clipboard = MatchingMultilineClipboard(received);
    if (!clipboard.has_value()) {
        return std::nullopt;
    }

    std::vector<INPUT_RECORD> consumed;
    bool saw_newline = received.find(L'\n') != std::wstring::npos;
    bool saw_text_after_newline = false;
    constexpr ULONGLONG kCompletionMs = 2000;
    const ULONGLONG deadline = GetTickCount64() + kCompletionMs;
    while (received.size() < clipboard->size()) {
        const ULONGLONG now = GetTickCount64();
        INPUT_RECORD record{};
        if (now >= deadline || !ReadInputRecord(record, static_cast<DWORD>(deadline - now))) {
            RestoreInputRecords(consumed);
            return std::nullopt;
        }
        consumed.push_back(record);
        if (record.EventType != KEY_EVENT || record.Event.KeyEvent.bKeyDown == FALSE) {
            continue;
        }
        if (!AppendNativePasteKey(record, received, saw_newline, saw_text_after_newline) ||
            received.size() > clipboard->size() || clipboard->compare(0, received.size(), received) != 0) {
            RestoreInputRecords(consumed);
            return std::nullopt;
        }
    }

    KeyInput out;
    out.kind = KeyInput::Kind::Paste;
    out.text = WideToUtf8(received);
    out.replace_before = replace_before;
    return out;
}

}  // namespace

bool StdinIsInteractive() {
    const HANDLE h = GetStdHandle(STD_INPUT_HANDLE);
    if (h == nullptr || h == INVALID_HANDLE_VALUE) {
        return false;
    }
    if (GetFileType(h) != FILE_TYPE_CHAR) {
        return false;
    }
    DWORD mode = 0;
    return GetConsoleMode(h, &mode) != 0;
}

StdoutConsoleProbe ProbeStdoutConsole() {
    StdoutConsoleProbe probe;
    const HANDLE h = GetStdHandle(STD_OUTPUT_HANDLE);
    if (h != nullptr && h != INVALID_HANDLE_VALUE && GetFileType(h) == FILE_TYPE_CHAR) {
        DWORD mode = 0;
        if (GetConsoleMode(h, &mode) != 0) {
            probe.is_console = true;
            if ((mode & ENABLE_VIRTUAL_TERMINAL_PROCESSING) != 0) {
                probe.vt_enabled = true;
            } else if (SetConsoleMode(h, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING) != 0) {
                probe.vt_enabled = true;
            }
        }
    }
    return probe;
}

// DECRQM 应答的逐事件状态机(ProbeSyncOutputSupport 用):期望收到
// ESC [ ? 2026 ; <digit> $ y,任何一位走样就把"这一串"整批当用户输入
// 还回、从下一个 ESC 重头等。前缀配对中的记录单独攒着——问成了它们是
// 终端应答(吃掉),问不成它们是用户按键(还回),两头都不丢账。
namespace {

constexpr std::string_view kSyncReplyPrefix = "\x1b[?2026;";

struct SyncReplyReader {
    std::vector<INPUT_RECORD> prefix_records;  // 配对中的应答字节
    std::vector<INPUT_RECORD> stray_records;   // 夹带的用户输入/无关事件
    std::size_t matched = 0;                   // 前缀已对上的长度
    bool got_param = false;                    // ';' 后那一位数字
    int mode_value = -1;
    bool answered = false;

    void GiveUpPrefix() {
        stray_records.insert(stray_records.end(), prefix_records.begin(), prefix_records.end());
        prefix_records.clear();
        matched = 0;
        got_param = false;
        mode_value = -1;
    }

    // 喂一个字符事件。返回 false = 这个事件不是应答的一部分。
    bool Feed(char ch) {
        if (matched < kSyncReplyPrefix.size()) {
            if (ch != kSyncReplyPrefix[matched]) {
                return false;
            }
            ++matched;
            return true;
        }
        if (!got_param) {
            if (ch < '0' || ch > '9') {
                return false;
            }
            mode_value = ch - '0';
            got_param = true;
            return true;
        }
        if (!got_dollar) {
            if (ch != '$') {
                return false;
            }
            got_dollar = true;
            return true;
        }
        if (ch != 'y') {
            return false;
        }
        answered = true;
        return true;
    }

    bool got_dollar = false;
};

}  // namespace

bool ProbeSyncOutputSupport() {
    // 进程级缓存:0=没问过,1=确认支持,2=确认不支持。只缓存确定性结论
    // (终端答过话);输入锁没抢到这种"没问成"不缓存,下轮再问。
    static std::atomic<int> cached{0};
    const int state = cached.load(std::memory_order_acquire);
    if (state != 0) {
        return state == 1;
    }
    // 前置:真控制台且 VT 已开(顺带再试一次打开)。VT 不开的宿主,查询串
    // 会被当正文印到屏上,一发都不能发;那也就无所谓 2026,直接定案。
    if (!ProbeStdoutConsole().vt_enabled) {
        cached.store(2, std::memory_order_release);
        return false;
    }
    // 应答走控制台输入,与用户按键混流。限时拿输入锁:拿不到说明监听
    // 线程/前台编辑器正读键,这一轮不问(不缓存,下轮再试),绝不反向
    // 等成死锁(stdout 锁的持有方恰是本函数的调用方,见 footer 一路)。
    std::unique_lock<std::recursive_timed_mutex> input_lock(ConsoleInputMutex(), std::defer_lock);
    if (!input_lock.try_lock_for(std::chrono::milliseconds(120))) {
        return false;
    }
    std::cout << "\x1b[?2026$p" << std::flush;

    SyncReplyReader reader;
    const ULONGLONG deadline = GetTickCount64() + 200;
    while (!reader.answered) {
        const ULONGLONG now = GetTickCount64();
        if (now >= deadline) {
            break;
        }
        INPUT_RECORD record{};
        if (!ReadInputRecord(record, static_cast<DWORD>(deadline - now))) {
            break;
        }
        const bool text_key = record.EventType == KEY_EVENT && record.Event.KeyEvent.bKeyDown != FALSE &&
                              record.Event.KeyEvent.uChar.UnicodeChar != 0;
        if (!text_key) {
            reader.stray_records.push_back(record);  // 修饰键/鼠标/窗口事件:不是应答
            continue;
        }
        const char ch = static_cast<char>(record.Event.KeyEvent.uChar.UnicodeChar);
        if (!reader.Feed(ch)) {
            reader.GiveUpPrefix();
            reader.stray_records.push_back(record);  // 走样:多半是用户先敲的键
            continue;
        }
        reader.prefix_records.push_back(record);
    }
    // 结账:夹带的用户输入一律还回(问成也还);配对中的前缀只在问成时
    // 才算终端应答被吃掉,没问成它们同样是用户按键,一并还回。
    if (!reader.answered) {
        reader.GiveUpPrefix();
    }
    RestoreInputRecords(reader.stray_records);

    // 答了话(哪怕答 0=不认得)是确定性结论;预算耗尽没答也定案不支持
    // ——DECRPM 应答是即时的事,200ms 不答就是不会答,别叫每轮再等一遍。
    // 误伤方向是安全的:支持的终端被误判,走的只是降级路,不再漏中间态。
    const bool supported = reader.answered && reader.mode_value >= 1 && reader.mode_value <= 4;
    cached.store(supported ? 1 : 2, std::memory_order_release);
    return supported;
}

std::optional<int> ConsoleWidth() {
    const HANDLE h_out = GetStdHandle(STD_OUTPUT_HANDLE);
    if (h_out == nullptr || h_out == INVALID_HANDLE_VALUE) {
        return std::nullopt;
    }
    CONSOLE_SCREEN_BUFFER_INFO info{};
    if (!GetConsoleScreenBufferInfo(h_out, &info)) {
        return std::nullopt;
    }
    return static_cast<int>(info.dwSize.X);
}

bool SupportsScreenRepaint() {
    // 真探测,跟 TranscriptPainter 实际用来定锚点的 API 同一把尺——之前
    // 写死 true,mintty/ConPTY 之类 GetConsoleScreenBufferInfo 靠不住的
    // 环境里 TranscriptPainter 会以为自己能原地改写,实际锚点全程拿不到
    // 准确坐标,子代理连打工具调用时会画出"一黄一绿"的重复行(旧状态没
    // 擦掉、新状态又在别处新起一行)。别再无条件 true,也别另立一套
    // is_console 标准。
    return GetScreenInfo().has_value();
}

std::optional<ScreenInfo> GetScreenInfo() {
    const HANDLE h_out = GetStdHandle(STD_OUTPUT_HANDLE);
    CONSOLE_SCREEN_BUFFER_INFO info{};
    if (!GetConsoleScreenBufferInfo(h_out, &info)) {
        return std::nullopt;
    }
    ScreenInfo out;
    out.width = static_cast<int>(info.dwSize.X);
    out.height = static_cast<int>(info.dwSize.Y);
    out.cursor_x = static_cast<int>(info.dwCursorPosition.X);
    out.cursor_y = static_cast<int>(info.dwCursorPosition.Y);
    out.viewport_x = static_cast<int>(info.srWindow.Left);
    out.viewport_y = static_cast<int>(info.srWindow.Top);
    out.viewport_height = static_cast<int>(info.srWindow.Bottom) - static_cast<int>(info.srWindow.Top) + 1;
    return out;
}

void SetCursorPos(int x, int y) {
    const HANDLE h_out = GetStdHandle(STD_OUTPUT_HANDLE);
    SetConsoleCursorPosition(h_out, COORD{static_cast<SHORT>(x), static_cast<SHORT>(y)});
}

void ClearScreen() {
    // 光标归位 + 清可见区 + 清回滚缓冲。VT 序列走 std::cout(main.cpp 那份
    // 调用点已经用 is_console 判断过要不要调这个函数),不碰 Win32
    // FillConsoleOutputCharacterW 那套——ENABLE_VIRTUAL_TERMINAL_PROCESSING
    // 在真控制台/现代终端下开着,ProbeStdoutConsole 已经保证过。
    std::cout << "\x1b[H\x1b[2J\x1b[3J" << std::flush;
}

void ClearRowFrom(int x, int y, int count) {
    if (count <= 0) {
        return;
    }
    const HANDLE h_out = GetStdHandle(STD_OUTPUT_HANDLE);
    DWORD written = 0;
    FillConsoleOutputCharacterW(h_out, L' ', static_cast<DWORD>(count),
                                 COORD{static_cast<SHORT>(x), static_cast<SHORT>(y)}, &written);
}

void ClearRowHardFrom(int x, int y, int count) {
    if (count <= 0) {
        return;
    }
    const HANDLE h_out = GetStdHandle(STD_OUTPUT_HANDLE);
    CONSOLE_SCREEN_BUFFER_INFO info{};
    DWORD written = 0;
    const COORD pos{static_cast<SHORT>(x), static_cast<SHORT>(y)};
    FillConsoleOutputCharacterW(h_out, L' ', static_cast<DWORD>(count), pos, &written);
    if (GetConsoleScreenBufferInfo(h_out, &info)) {
        FillConsoleOutputAttribute(h_out, info.wAttributes, static_cast<DWORD>(count), pos, &written);
    }
}

bool WriteNativeRow(int x, int y, const NativeRowCell* cells, int cell_count) {
    if (cells == nullptr || cell_count <= 0 || x < 0 || y < 0) {
        return false;
    }
    const HANDLE h_out = GetStdHandle(STD_OUTPUT_HANDLE);
    if (h_out == nullptr || h_out == INVALID_HANDLE_VALUE) {
        return false;
    }
    CONSOLE_SCREEN_BUFFER_INFO info{};
    if (!GetConsoleScreenBufferInfo(h_out, &info)) {
        return false;  // 非真控制台(管道/重定向):没有缓冲区可直写
    }
    // 与缓冲区求交集,越界部分裁掉;交集为空 = 这行无笔可落,不算失败
    //(调用方的帧账按可视窗口预算,正常到不了这里,防御而已)。
    const int width = static_cast<int>(info.dwSize.X);
    const int height = static_cast<int>(info.dwSize.Y);
    int write_x = x;
    int begin = 0;
    int count = cell_count;
    if (write_x + count > width) {
        count = width - write_x;
    }
    if (y >= height || count <= 0) {
        return true;
    }

    // CHAR_INFO 一次成块:字符、属性(含宽字半格旗标)同步落,随后一发
    // WriteConsoleOutputW——不 SetCursorPos、不写 stdout,buffer 光标从头
    // 到尾没被碰过(单 2 二轮的核心合同,G0 高频轨迹判据就是验它)。
    std::vector<CHAR_INFO> buf(static_cast<std::size_t>(count));
    const WORD default_attr = info.wAttributes;
    for (int i = 0; i < count; ++i) {
        const NativeRowCell& cell = cells[begin + i];
        WORD attr = static_cast<WORD>(cell.attr & 0xFFu);
        if (attr == 0) {
            attr = default_attr;  // "默认属性"记号(构建器不动色的格子)
        }
        if ((cell.attr & kNativeCellLeading) != 0) {
            attr |= COMMON_LVB_LEADING_BYTE;
        }
        if ((cell.attr & kNativeCellTrailing) != 0) {
            attr |= COMMON_LVB_TRAILING_BYTE;
        }
        buf[static_cast<std::size_t>(i)].Attributes = attr;
        // 星面码点(>BMP)按半格旗标拆代理对:前半格高代理、后半格低代理
        //(footer 行全是 BMP 字符,这条只是别在罕见输入上画替换符的保险)。
        WCHAR wc;
        if (cell.ch >= 0x10000) {
            const std::uint32_t v = static_cast<std::uint32_t>(cell.ch) - 0x10000;
            wc = static_cast<WCHAR>((cell.attr & kNativeCellTrailing) != 0 ? (0xDC00 + (v & 0x3FF))
                                                                            : (0xD800 + (v >> 10)));
        } else {
            wc = static_cast<WCHAR>(cell.ch);
        }
        buf[static_cast<std::size_t>(i)].Char.UnicodeChar = wc;
    }
    SMALL_RECT region{static_cast<SHORT>(write_x), static_cast<SHORT>(y),
                      static_cast<SHORT>(write_x + count - 1), static_cast<SHORT>(y)};
    return WriteConsoleOutputW(h_out, buf.data(), COORD{static_cast<SHORT>(count), 1}, COORD{0, 0},
                               &region) != 0;
}

int PanViewportDown(int rows) {
    if (rows <= 0) {
        return 0;
    }
    const HANDLE h_out = GetStdHandle(STD_OUTPUT_HANDLE);
    CONSOLE_SCREEN_BUFFER_INFOEX info{};
    info.cbSize = sizeof(info);
    if (!GetConsoleScreenBufferInfoEx(h_out, &info)) {
        return 0;
    }
    const COORD size_before = info.dwSize;  // 平移前后缓冲高的对账底(见下)
    const int buffer_bottom = static_cast<int>(info.dwSize.Y) - 1;
    const int viewport_bottom = static_cast<int>(info.srWindow.Bottom);
    const int room = buffer_bottom - viewport_bottom;
    if (room <= 0) {
        return 0;  // 窗口已贴缓冲区底:没有可平移的余地,调用方退回滚内容
    }
    const int pan = (std::min)(rows, room);
    // 平移路(2026-08-26 改道,Windows 11 新 conhost):老路 SetConsoleScreen
    // BufferInfoEx 直接重锚窗口,新 conhost(实测 26300)那一拍会把窗口钳矮
    // 一行、缓冲高收拢到窗口高(400 -> 29 -> 28 -> 27 一路螺旋)——窗口上方
    // 的滚屏正文随收拢整段被丢,绝对锚点全漂(查看态回流簇的病根)。改走
    // "光标带窗口":把光标拨到目标窗口底行,conhost 自会把可视窗滑下来
    // 揭示它(经典 cursor-follow,不动缓冲尺寸);窗口真滑下来且缓冲没被动
    // 就认账,光标拨回原位(原位已在新窗口之外就不拨——那一拨会把窗口又
    // 滑回去,平移白做;调用方平移后本来就按绝对坐标重画,光标停在底行
    // 无害)。没走成才退回老路,并加"收拢护栏":发现缓冲被收拢就按原高
    // 撑回去,长缓冲不至于从此塌成窗口高。
    const COORD cursor_before = info.dwCursorPosition;
    const SHORT follow_row = static_cast<SHORT>(viewport_bottom + pan);
    SetConsoleCursorPosition(h_out, COORD{0, follow_row});
    {
        CONSOLE_SCREEN_BUFFER_INFO after_follow{};
        if (GetConsoleScreenBufferInfo(h_out, &after_follow) &&
            after_follow.srWindow.Bottom >= follow_row && after_follow.dwSize.Y >= size_before.Y) {
            // 光标拨回原位——但只在原位落在新窗口之内:落在窗外的话,这一拨
            // 会触发反向滚动把窗口又滑回去,平移白做。留在底行无害:调用方
            // 平移后本来就按绝对坐标重画(SetCursorPos 各自再拨)。
            if (cursor_before.Y >= after_follow.srWindow.Top &&
                cursor_before.Y <= after_follow.srWindow.Bottom) {
                SetConsoleCursorPosition(h_out, cursor_before);
            }
            return pan;
        }
    }
    // cursor-follow 没走成(窗口没滑到/缓冲被动了):拨回光标,走老路。
    SetConsoleCursorPosition(h_out, cursor_before);
    info.srWindow.Top += static_cast<SHORT>(pan);
    info.srWindow.Bottom += static_cast<SHORT>(pan);
    // dwCursorPosition 原样带回(缓冲没滚,绝对坐标仍有效),光标一个不挪
    // ——挪了反而可能触发控制台"把光标带回视野"的反向滚动,平移白做。
    if (!SetConsoleScreenBufferInfoEx(h_out, &info)) {
        return 0;
    }
    // 缓冲收拢护栏:发现 conhost 把缓冲高收拢了(实测 26300 会),按原高撑
    // 回去——窗口此刻高度不超过原缓冲,必然撑得回;丢掉的行救不回来,但
    // 长缓冲 rig 不至于从此塌成窗口高、后续每一帧都顶穿缓冲底。
    {
        CONSOLE_SCREEN_BUFFER_INFO after{};
        if (GetConsoleScreenBufferInfo(h_out, &after) && after.dwSize.Y < size_before.Y) {
            SetConsoleScreenBufferSize(h_out, COORD{size_before.X, size_before.Y});
        }
    }
    return pan;
}

std::optional<std::string> ReadRowText(int row) {
    const HANDLE h_out = GetStdHandle(STD_OUTPUT_HANDLE);
    if (h_out == nullptr || h_out == INVALID_HANDLE_VALUE) {
        return std::nullopt;
    }
    CONSOLE_SCREEN_BUFFER_INFO info{};
    if (!GetConsoleScreenBufferInfo(h_out, &info)) {
        return std::nullopt;
    }
    const int width = static_cast<int>(info.dwSize.X);
    const int height = static_cast<int>(info.dwSize.Y);
    if (width <= 0 || row < 0 || row >= height) {
        return std::nullopt;
    }
    std::vector<CHAR_INFO> cells(static_cast<std::size_t>(width));
    SMALL_RECT region{0, static_cast<SHORT>(row), static_cast<SHORT>(width - 1), static_cast<SHORT>(row)};
    if (!ReadConsoleOutputW(h_out, cells.data(), COORD{static_cast<SHORT>(width), 1}, COORD{0, 0}, &region)) {
        return std::nullopt;
    }
    std::wstring text;
    text.reserve(static_cast<std::size_t>(width));
    for (const CHAR_INFO& cell : cells) {
        if (cell.Attributes & COMMON_LVB_TRAILING_BYTE) {
            continue;  // 宽字符的后半格:值并入前半格,不重复计
        }
        text.push_back(cell.Char.UnicodeChar);
    }
    while (!text.empty() && (text.back() == L' ' || text.back() == L'\0')) {
        text.pop_back();
    }
    return WideToUtf8(text);
}

RawInputScope::RawInputScope() {
    const HANDLE h_in = GetStdHandle(STD_INPUT_HANDLE);
    DWORD original_mode = 0;
    if (!GetConsoleMode(h_in, &original_mode)) {
        return;
    }
    // 关掉行编辑、回显、Ctrl+C 自动处理(这三样都要自己接管):
    //   ENABLE_LINE_INPUT      —— 关掉之后 ReadConsoleInputW 才能逐键拿到,不用等回车
    //   ENABLE_ECHO_INPUT      —— 关掉控制台自带回显,回显交给我们自己按 RenderState 重画
    //   ENABLE_PROCESSED_INPUT —— 关掉之后 Ctrl+C 才会以 KEY_EVENT 形式读到,不会
    //                             被控制台直接当 SIGINT 处理掉、绕过我们的按键循环
    const DWORD new_mode = (original_mode & ~static_cast<DWORD>(ENABLE_LINE_INPUT) &
                             ~static_cast<DWORD>(ENABLE_ECHO_INPUT) & ~static_cast<DWORD>(ENABLE_PROCESSED_INPUT));
    if (!SetConsoleMode(h_in, new_mode)) {
        return;
    }
    original_mode_ = original_mode;
    ok_ = true;
}

RawInputScope::~RawInputScope() {
    if (ok_) {
        SetConsoleMode(GetStdHandle(STD_INPUT_HANDLE), static_cast<DWORD>(original_mode_));
    }
}

std::optional<KeyInput> KeyReader::ReadOne() {
    const auto reset_text_run = [this]() {
        rapid_text_run_.clear();
        rapid_char_count_ = 0;
        last_text_tick_ = 0;
    };
    INPUT_RECORD record{};
    if (!ReadInputRecord(record)) {
        return std::nullopt;
    }
    if (record.EventType != KEY_EVENT || record.Event.KeyEvent.bKeyDown == FALSE) {
        return KeyInput{};  // None
    }
    if (auto paste = TryReadNativePasteBurst(record); paste.has_value()) {
        if (!rapid_text_run_.empty()) {
            std::wstring combined = rapid_text_run_ + Utf8ToWide(paste->text);
            if (combined.find(L'\n') != std::wstring::npos) {
                if (auto completed = TryFinishClipboardPaste(std::move(combined), rapid_char_count_);
                    completed.has_value()) {
                    reset_text_run();
                    return completed;
                }
            }
        }
        reset_text_run();
        return paste;
    }
    const KEY_EVENT_RECORD& ke = record.Event.KeyEvent;
    const bool ctrl = (ke.dwControlKeyState & (LEFT_CTRL_PRESSED | RIGHT_CTRL_PRESSED)) != 0;
    const bool shift = (ke.dwControlKeyState & SHIFT_PRESSED) != 0;
    const bool alt = (ke.dwControlKeyState & (LEFT_ALT_PRESSED | RIGHT_ALT_PRESSED)) != 0;

    KeyInput out;
    if (ctrl && ke.wVirtualKeyCode == 'C') {
        reset_text_run();
        out.kind = KeyInput::Kind::CtrlC;
    } else if (ctrl && ke.wVirtualKeyCode == 'D') {
        reset_text_run();
        out.kind = KeyInput::Kind::CtrlD;
    } else if (ctrl && ke.wVirtualKeyCode == 'O') {
        reset_text_run();
        out.kind = KeyInput::Kind::CtrlO;  // UI-D:紧凑/详细切换
    } else if (ctrl && ke.wVirtualKeyCode == 'E') {
        reset_text_run();
        out.kind = KeyInput::Kind::CtrlE;  // UI-D:聚焦查看
    } else if (ctrl && ke.wVirtualKeyCode == 'T') {
        reset_text_run();
        out.kind = KeyInput::Kind::CtrlT;  // 会话选择器:转录浮层
    } else if (ctrl && ke.wVirtualKeyCode == 'X') {
        reset_text_run();
        out.kind = KeyInput::Kind::CtrlX;  // 子代理面板:停止全部(两段确认第一段)
    } else if (ctrl && ke.wVirtualKeyCode == 'K') {
        reset_text_run();
        out.kind = KeyInput::Kind::CtrlK;  // 子代理面板:停止全部(两段确认第二段)
    } else if (ctrl && ke.wVirtualKeyCode == 'L') {
        reset_text_run();
        out.kind = KeyInput::Kind::CtrlL;  // 底栏自救:整屏重画
    } else if (ctrl && ke.wVirtualKeyCode == 'P') {
        reset_text_run();
        out.kind = KeyInput::Kind::CtrlP;  // 历史:上一条(明确别名,不受多行位置影响)
    } else if (ctrl && ke.wVirtualKeyCode == 'N') {
        reset_text_run();
        out.kind = KeyInput::Kind::CtrlN;  // 历史:下一条
    } else if (ke.wVirtualKeyCode == VK_BACK) {
        reset_text_run();
        out.kind = KeyInput::Kind::Backspace;
    } else if (ke.wVirtualKeyCode == VK_LEFT) {
        // Shift+Left / Ctrl+Left 单独翻出来(排队消息取回键与可配置备用键)。
        // 纯 Shift/Ctrl 不叠别的修饰才认;Alt+Left 这类"历史后退"键照旧当 Left。
        reset_text_run();
        if (shift && !ctrl && !alt) {
            out.kind = KeyInput::Kind::ShiftLeft;
        } else if (ctrl && !shift && !alt) {
            out.kind = KeyInput::Kind::CtrlLeft;
        } else {
            out.kind = KeyInput::Kind::Left;
        }
    } else if (ke.wVirtualKeyCode == VK_RIGHT) {
        reset_text_run();
        out.kind = KeyInput::Kind::Right;
    } else if (ke.wVirtualKeyCode == VK_HOME) {
        reset_text_run();
        out.kind = KeyInput::Kind::Home;
    } else if (ke.wVirtualKeyCode == VK_END) {
        reset_text_run();
        out.kind = KeyInput::Kind::End;
    } else if (ke.wVirtualKeyCode == VK_UP) {
        reset_text_run();
        out.kind = KeyInput::Kind::Up;
    } else if (ke.wVirtualKeyCode == VK_DOWN) {
        reset_text_run();
        out.kind = KeyInput::Kind::Down;
    } else if (ke.wVirtualKeyCode == VK_PRIOR) {
        reset_text_run();
        out.kind = KeyInput::Kind::PageUp;
    } else if (ke.wVirtualKeyCode == VK_NEXT) {
        reset_text_run();
        out.kind = KeyInput::Kind::PageDown;
    } else if (ke.wVirtualKeyCode == VK_DELETE) {
        reset_text_run();
        out.kind = KeyInput::Kind::Delete;
    } else if (ke.wVirtualKeyCode == VK_TAB) {
        reset_text_run();
        out.kind = shift ? KeyInput::Kind::ShiftTab : KeyInput::Kind::Tab;
    } else if (ke.wVirtualKeyCode == VK_RETURN) {
        // UI-A:Alt+Enter / Shift+Enter 都翻成 NewLine(插换行)。实测这台
        // 机器:Windows Terminal 把 Alt+Enter 绑成了全屏切换,keydown 根本
        // 进不了输入缓冲(只漏一个 keyup,bKeyDown==FALSE 早被上面滤掉),
        // 等于天然收不到;conhost 下两个组合都完好。所以两个都认,文档里
        // 推荐 Shift+Enter。非 composer 读取里 NewLine 会被核心层当 Enter,
        // 确认提示那些单行场景语义不变。
        if (!shift && !alt && !rapid_text_run_.empty()) {
            std::wstring received = rapid_text_run_;
            received.push_back(L'\n');  // 当前 VK_RETURN 已从输入队列取走
            if (auto paste = TryFinishClipboardPaste(std::move(received), rapid_char_count_);
                paste.has_value()) {
                reset_text_run();
                return paste;
            }
        }
        reset_text_run();
        out.kind = (shift || alt) ? KeyInput::Kind::NewLine : KeyInput::Kind::Enter;
    } else if (ke.wVirtualKeyCode == VK_ESCAPE) {
        if (auto paste = TryReadBracketedPaste(); paste.has_value()) {
            reset_text_run();
            return paste;
        }
        reset_text_run();
        out.kind = KeyInput::Kind::Esc;
    } else if (ke.uChar.UnicodeChar != 0 && ctrl && !alt &&
               ke.wVirtualKeyCode >= 'A' && ke.wVirtualKeyCode <= 'Z') {
        // Ctrl+字母(未列专枚举的那批):按 Char 送出并置 ctrl,交给
        // cli/keymap 和弦层分派(编辑器核心对带修饰的 Char 不当正文插)。
        // 输入法/死键不会走到这——它们不带 VK_ 字母码。
        reset_text_run();
        out.kind = KeyInput::Kind::Char;
        out.ch = static_cast<char32_t>(ke.wVirtualKeyCode - 'A' + 'a');
        out.ctrl = true;
    } else if (ke.uChar.UnicodeChar != 0 && !ctrl && alt &&
               ((ke.wVirtualKeyCode >= 'A' && ke.wVirtualKeyCode <= 'Z') ||
                (ke.wVirtualKeyCode >= '0' && ke.wVirtualKeyCode <= '9'))) {
        // Alt+字母/数字:同上,作和弦送出(Alt+V 贴图这类)。原先是当
        // 普通字符插进编辑器——按住 Alt 打字不是正常输入姿势,改判和弦。
        reset_text_run();
        out.kind = KeyInput::Kind::Char;
        out.ch = static_cast<char32_t>(ke.wVirtualKeyCode >= 'A' && ke.wVirtualKeyCode <= 'Z'
                                           ? ke.wVirtualKeyCode - 'A' + 'a'
                                           : ke.wVirtualKeyCode);
        out.alt = true;
    } else if (ke.uChar.UnicodeChar != 0 && !ctrl) {
        const wchar_t wc = ke.uChar.UnicodeChar;
        if (wc == 0x1b) {
            // 裸 0x1b 字符事件(ConPTY 某些形状不给 VK_ESCAPE):先按
            // bracketed paste 的开头探一遍,标记字符紧跟着就到;探不到,
            // 它就是一枚普通的 Esc 键。旧路把 0x1b 静默丢弃,后面的
            // "[200~"便逐字漏进编辑器——正是实测里"标记也被逐字渲染"
            // 的病根。
            if (auto paste = TryReadBracketedPaste(); paste.has_value()) {
                reset_text_run();
                pending_high_surrogate_.reset();
                return paste;
            }
            reset_text_run();
            pending_high_surrogate_.reset();
            out.kind = KeyInput::Kind::Esc;
            return out;
        }
        constexpr ULONGLONG kRapidTextGapMs = 50;
        const ULONGLONG now = GetTickCount64();
        if (!rapid_text_run_.empty() && now - last_text_tick_ > kRapidTextGapMs) {
            reset_text_run();
        }
        if (auto burst = TryReadCoalescedTextBurst(record, rapid_text_run_, rapid_char_count_);
            burst.has_value()) {
            reset_text_run();
            pending_high_surrogate_.reset();
            return burst;
        }
        rapid_text_run_.push_back(wc);
        last_text_tick_ = now;
        if (wc >= 0xD800 && wc <= 0xDBFF) {
            pending_high_surrogate_ = static_cast<char32_t>(wc);
            return KeyInput{};  // 高代理项,等低代理项凑成一个完整码点再交
        }
        char32_t cp = static_cast<char32_t>(wc);
        if (wc >= 0xDC00 && wc <= 0xDFFF && pending_high_surrogate_.has_value()) {
            const char32_t high = *pending_high_surrogate_;
            pending_high_surrogate_.reset();
            cp = 0x10000 + ((high - 0xD800) << 10) + (static_cast<char32_t>(wc) - 0xDC00);
        }
        if (cp >= 0x20) {  // 过滤掉控制字符(Esc、独立的 Tab 已经在上面单独处理)
            ++rapid_char_count_;
            out.kind = KeyInput::Kind::Char;
            out.ch = cp;
        }
    }
    return out;
}

bool WaitForKeyEvent(int timeout_ms) {
    // 粘贴探测把事件吸干又还进了内部缓冲，这些事件不再让控制台句柄有信号。
    // 缓冲里还有货就算有键，别去干等句柄（输入法整词提交/快打连击会中招）。
    // 监听线程会先等事件、后抢读权；这里限时拿共用锁护住内部队列。
    // 前台编辑器已持锁时，同线程递归可入；别的线程至多候本次等待时限，
    // 免得菜单持锁时监听线程空转烧 CPU。
    {
        std::unique_lock<std::recursive_timed_mutex> input_lock(ConsoleInputMutex(), std::defer_lock);
        if (!input_lock.try_lock_for(std::chrono::milliseconds(timeout_ms > 0 ? timeout_ms : 0))) {
            return false;
        }
        if (!PendingInputRecords().empty()) {
            return true;
        }
    }
    const HANDLE h_in = GetStdHandle(STD_INPUT_HANDLE);
    return WaitForSingleObject(h_in, timeout_ms > 0 ? static_cast<DWORD>(timeout_ms) : 0) == WAIT_OBJECT_0;
}

KeyListenScope::KeyListenScope() {
    const HANDLE h_in = GetStdHandle(STD_INPUT_HANDLE);
    DWORD original_mode = 0;
    if (!GetConsoleMode(h_in, &original_mode)) {
        return;
    }
    // ReadConsoleInputW 也受控制台输入模式约束。若留着 LINE/ECHO，流式
    // footer 期间敲下的字会躺到回车才交付，并由 conhost/Terminal 自行
    // 回显到当前物理光标处，看起来像 PowerShell 提示符闯回了 TUI。
    // PROCESSED 一并关掉，让 Ctrl+C 仍以 KEY_EVENT 交给监听线程按
    // “单击打断、双击退出”处理，不越过应用直接发控制信号。
    const DWORD listen_mode =
        original_mode & ~static_cast<DWORD>(ENABLE_LINE_INPUT) & ~static_cast<DWORD>(ENABLE_ECHO_INPUT) &
        ~static_cast<DWORD>(ENABLE_PROCESSED_INPUT);
    if (!SetConsoleMode(h_in, listen_mode)) {
        return;
    }
    original_mode_ = original_mode;
    active_ = true;
}

KeyListenScope::~KeyListenScope() {
    if (active_) {
        SetConsoleMode(GetStdHandle(STD_INPUT_HANDLE), static_cast<DWORD>(original_mode_));
    }
}

std::optional<std::string> ReadLineCooked() {
    const HANDLE h = GetStdHandle(STD_INPUT_HANDLE);
    std::wstring wide;
    wchar_t buf[1024];
    while (true) {
        DWORD read = 0;
        const BOOL ok = ReadConsoleW(h, buf, static_cast<DWORD>(std::size(buf)), &read, nullptr);
        if (!ok || read == 0) {
            return std::nullopt;
        }
        wide.append(buf, read);
        if (!wide.empty() && wide.back() == L'\n') {
            break;
        }
    }
    while (!wide.empty() && (wide.back() == L'\n' || wide.back() == L'\r')) {
        wide.pop_back();
    }
    if (wide.size() == 1 && wide[0] == 0x1A) {
        return std::nullopt;  // 单独一个 Ctrl+Z:EOF
    }
    const int utf8_len =
        WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), nullptr, 0, nullptr, nullptr);
    std::string utf8;
    if (utf8_len > 0) {
        utf8.resize(static_cast<std::size_t>(utf8_len));
        WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), utf8.data(), utf8_len, nullptr,
                             nullptr);
    }
    return utf8;
}

void SetupConsoleUtf8() {
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);
}

}  // namespace lubancode::platform
