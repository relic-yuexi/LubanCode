// POSIX 实现:终端原语。语义对齐 console_win.cpp——
//   探测:isatty;宽度/高度 ioctl TIOCGWINSZ;光标位 DSR("\x1b[6n" 问、
//   "\x1b[<row>;<col>R" 答);光标定位 CUP;清行 ECH(擦 n 格);逐键读
//   termios 原始模式 + ANSI 键序列解析(方向键 CSI A-D、Home/End、
//   Shift+Tab=CSI Z、Alt+Enter=ESC+CR、退格 0x7f/0x08、Ctrl 键低位码)。
//
// 转义序列/DSR 应答都走 stdin,跟用户按键混在同一条流里:所有从 stdin 读
// 字节的路径共用一个进程级的回压队列(PendingBytes)——DSR 等应答时读到
// 的用户按键先存进队列,KeyReader 下次优先从队列取,不丢键。队列只在
// "持有 cli::ConsoleReadMutex 的读取路径"上被碰(编辑器循环、监听线程
// 抢锁后的单次读),天然串行,不再加锁。
//
// SupportsScreenRepaint() 恒假:流式期间的原地重画要随时查光标位,DSR
// 应答会跟监听线程抢 stdin,这个赛点没堵之前诚实关掉(输出退化成纯流式,
// 跟管道模式一致);逐键行编辑不受限,编辑期间监听线程被互斥锁挡在门外。
//
// 验证状态:WSL Ubuntu 26.04(g++ 15.2)真机编译、单测通过;交互路径经
// 两道 pty 冒烟——script(1)(终端不答 DSR,验证 ws_col=0/无应答时降级到
// ReadLineCooked、exit 干净退出)和自制 DSR 应答探针(build/itest/
// pty_probe.py:答 \x1b[6n、敲 CJK/退格/ESC/exit,逐键重画与体面退出全
// 过)。真实终端模拟器上的长期交互体验、macOS 未经真机验证,待 CI 亮灯。
#include "platform/console.hpp"

#include <cstdio>
#include <deque>
#include <iostream>
#include <string_view>
#include <utility>

#include <poll.h>
#include <sys/ioctl.h>
#include <unistd.h>

namespace lubancode::platform {

namespace {

// DSR 等应答时"顺手"读到的用户按键,先存这儿,KeyReader 优先取。见文件头。
std::deque<unsigned char>& PendingBytes() {
    static std::deque<unsigned char> q;
    return q;
}

// 从 stdin 读一个字节:先掏回压队列;队列空就真读。返回 -1 = EOF/出错。
int ReadByteBlocking() {
    auto& q = PendingBytes();
    if (!q.empty()) {
        const int b = q.front();
        q.pop_front();
        return b;
    }
    unsigned char b = 0;
    ssize_t n = 0;
    do {
        n = read(STDIN_FILENO, &b, 1);
    } while (n < 0 && errno == EINTR);
    if (n <= 0) {
        return -1;
    }
    return b;
}

// 限时读一个字节(转义序列的后续字节等不等得到,用它分辨"裸 ESC"和
// "序列开头")。返回 -1 = 超时/EOF。
int ReadByteTimeout(int timeout_ms) {
    auto& q = PendingBytes();
    if (!q.empty()) {
        const int b = q.front();
        q.pop_front();
        return b;
    }
    struct pollfd pfd{STDIN_FILENO, POLLIN, 0};
    const int pr = poll(&pfd, 1, timeout_ms);
    if (pr <= 0) {
        return -1;
    }
    unsigned char b = 0;
    ssize_t n = 0;
    do {
        n = read(STDIN_FILENO, &b, 1);
    } while (n < 0 && errno == EINTR);
    if (n <= 0) {
        return -1;
    }
    return b;
}

// 把 termios 调成逐键原始输入:关行缓冲、回显、信号键(Ctrl+C 变可读
// 按键,对齐 Windows 关 ENABLE_PROCESSED_INPUT),关 ICRNL(回车原样给
// \r);输出侧(OPOST)不动,std::cout 的 "\n" 照常工作。
bool EnterRawTermios(struct termios* saved) {
    if (isatty(STDIN_FILENO) == 0) {
        return false;
    }
    if (tcgetattr(STDIN_FILENO, saved) != 0) {
        return false;
    }
    struct termios raw = *saved;
    raw.c_lflag &= ~static_cast<tcflag_t>(ICANON | ECHO | ISIG | IEXTEN);
    raw.c_iflag &= ~static_cast<tcflag_t>(IXON | ICRNL | INLCR);
    raw.c_cc[VMIN] = 1;
    raw.c_cc[VTIME] = 0;
    return tcsetattr(STDIN_FILENO, TCSANOW, &raw) == 0;
}

void RestoreTermios(const struct termios* saved) {
    tcsetattr(STDIN_FILENO, TCSANOW, saved);
}

// CSI 序列解析:ESC [ 已读掉,收集参数字节直到终止字节(0x40~0x7e),翻成
// 语义按键。认不出的序列整个吃掉、返回 None,不让参数字节漏成正文字符。
KeyInput ReadBracketedPaste() {
    constexpr std::string_view kEnd = "\x1b[201~";
    std::string text;
    while (true) {
        const int byte = ReadByteBlocking();
        if (byte < 0) {
            break;
        }
        text.push_back(static_cast<char>(byte));
        if (text.size() >= kEnd.size() &&
            text.compare(text.size() - kEnd.size(), kEnd.size(), kEnd) == 0) {
            text.resize(text.size() - kEnd.size());
            break;
        }
    }
    KeyInput out;
    out.kind = KeyInput::Kind::Paste;
    out.text = std::move(text);
    return out;
}

KeyInput ParseCsi() {
    std::string params;
    while (true) {
        const int b = ReadByteTimeout(50);
        if (b < 0) {
            return KeyInput{};  // 序列断在半路,丢弃
        }
        if (b >= 0x40 && b <= 0x7e) {
            const char final_byte = static_cast<char>(b);
            KeyInput out;
            switch (final_byte) {
                case 'A':
                    out.kind = KeyInput::Kind::Up;
                    return out;
                case 'B':
                    out.kind = KeyInput::Kind::Down;
                    return out;
                case 'C':
                    out.kind = KeyInput::Kind::Right;
                    return out;
                case 'D':
                    out.kind = KeyInput::Kind::Left;
                    return out;
                case 'H':
                    out.kind = KeyInput::Kind::Home;
                    return out;
                case 'F':
                    out.kind = KeyInput::Kind::End;
                    return out;
                case 'Z':
                    out.kind = KeyInput::Kind::ShiftTab;
                    return out;
                case '~':
                    // VT 风格:1~/7~ = Home,4~/8~ = End,其余(3~ Delete、
                    // 5~/6~ 翻页……)暂不映射。
                    if (params == "200") {
                        return ReadBracketedPaste();
                    }
                    if (params == "1" || params == "7") {
                        out.kind = KeyInput::Kind::Home;
                    } else if (params == "4" || params == "8") {
                        out.kind = KeyInput::Kind::End;
                    }
                    return out;
                default:
                    return KeyInput{};  // 认不出,整个吃掉
            }
        }
        params.push_back(static_cast<char>(b));
        if (params.size() > 16) {
            return KeyInput{};  // 序列长得不像话,放弃
        }
    }
}

// SS3 序列(ESC O A-D/H/F):有些终端方向键/Home/End 走这个前缀。
KeyInput ParseSs3() {
    const int b = ReadByteTimeout(50);
    KeyInput out;
    switch (b) {
        case 'A':
            out.kind = KeyInput::Kind::Up;
            break;
        case 'B':
            out.kind = KeyInput::Kind::Down;
            break;
        case 'C':
            out.kind = KeyInput::Kind::Right;
            break;
        case 'D':
            out.kind = KeyInput::Kind::Left;
            break;
        case 'H':
            out.kind = KeyInput::Kind::Home;
            break;
        case 'F':
            out.kind = KeyInput::Kind::End;
            break;
        default:
            break;  // 认不出/超时,None
    }
    return out;
}

}  // namespace

bool StdinIsInteractive() {
    return isatty(STDIN_FILENO) != 0;
}

StdoutConsoleProbe ProbeStdoutConsole() {
    StdoutConsoleProbe probe;
    probe.is_console = isatty(STDOUT_FILENO) != 0;
    // POSIX 真终端天然支持 ANSI,没有 Windows 那层 VT 开关。
    probe.vt_enabled = probe.is_console;
    return probe;
}

std::optional<int> ConsoleWidth() {
    struct winsize ws{};
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) != 0 || ws.ws_col == 0) {
        return std::nullopt;
    }
    return static_cast<int>(ws.ws_col);
}

bool SupportsScreenRepaint() {
    return false;  // 原因见文件头:DSR 应答与监听线程抢 stdin 的赛点没堵
}

std::optional<ScreenInfo> GetScreenInfo() {
    if (isatty(STDOUT_FILENO) == 0 || isatty(STDIN_FILENO) == 0) {
        return std::nullopt;
    }
    struct winsize ws{};
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) != 0 || ws.ws_col == 0 || ws.ws_row == 0) {
        return std::nullopt;
    }

    // 光标位:DSR 6n 问,应答 ESC [ <row> ; <col> R(1 基)。应答走 stdin,
    // 必须在原始模式下读——调用方(逐键编辑器)通常已在原始模式里,不在
    // 的话临时进出一趟。
    struct termios saved{};
    const bool need_raw = tcgetattr(STDIN_FILENO, &saved) == 0 && (saved.c_lflag & ICANON) != 0;
    struct termios saved_for_query{};
    if (need_raw && !EnterRawTermios(&saved_for_query)) {
        return std::nullopt;
    }

    std::cout << "\x1b[6n" << std::flush;

    std::optional<ScreenInfo> result;
    // 应答之前可能夹着用户刚敲的按键字节:原样搬进回压队列,一个不丢。
    std::deque<unsigned char> stray;
    const int kTotalBudgetMs = 250;
    int spent = 0;
    while (spent < kTotalBudgetMs) {
        const int b = ReadByteTimeout(50);
        if (b < 0) {
            spent += 50;
            continue;
        }
        if (b != 0x1b) {
            stray.push_back(static_cast<unsigned char>(b));
            continue;
        }
        // 读到 ESC:看是不是 [ <digits> ; <digits> R 的应答,不是就整段进
        // 回压队列。
        std::deque<unsigned char> seq{0x1b};
        const int b2 = ReadByteTimeout(50);
        if (b2 != '[') {
            if (b2 >= 0) {
                seq.push_back(static_cast<unsigned char>(b2));
            }
            for (const unsigned char c : seq) {
                stray.push_back(c);
            }
            continue;
        }
        seq.push_back('[');
        int row = 0;
        int col = 0;
        int* cur = &row;
        bool ok = true;
        bool done = false;
        while (!done) {
            const int c = ReadByteTimeout(50);
            if (c < 0) {
                ok = false;
                break;
            }
            seq.push_back(static_cast<unsigned char>(c));
            if (c >= '0' && c <= '9') {
                *cur = *cur * 10 + (c - '0');
            } else if (c == ';') {
                cur = &col;
            } else if (c == 'R') {
                done = true;
            } else {
                ok = false;
                break;
            }
        }
        if (ok && done && row > 0 && col > 0) {
            ScreenInfo info;
            info.width = static_cast<int>(ws.ws_col);
            info.height = static_cast<int>(ws.ws_row);
            info.cursor_x = col - 1;
            info.cursor_y = row - 1;
            result = info;
            break;
        }
        for (const unsigned char c : seq) {
            stray.push_back(c);
        }
    }

    // 夹带的用户按键回灌队列(保持原始顺序,插在队头之前——它们比队列里
    // 已有的字节先到)。
    auto& q = PendingBytes();
    for (auto it = stray.rbegin(); it != stray.rend(); ++it) {
        q.push_front(*it);
    }

    if (need_raw) {
        RestoreTermios(&saved_for_query);
    }
    return result;
}

void SetCursorPos(int x, int y) {
    std::cout << "\x1b[" << (y + 1) << ";" << (x + 1) << "H" << std::flush;
}

void ClearScreen() {
    // 光标归位 + 清可见区 + 清回滚缓冲——POSIX 终端天然认这三段 VT 序列,
    // 不用另外探测。main.cpp 调用点已经拿 is_console 判断过要不要调。
    std::cout << "\x1b[H\x1b[2J\x1b[3J" << std::flush;
}

void ClearRowFrom(int x, int y, int count) {
    if (count <= 0) {
        return;
    }
    // CUP 定位 + ECH 擦 n 格(不动别的格、不卷屏)。
    std::cout << "\x1b[" << (y + 1) << ";" << (x + 1) << "H\x1b[" << count << "X" << std::flush;
}

void ClearRowHardFrom(int x, int y, int count) {
    if (count <= 0) {
        return;
    }
    // 先 SGR 归零再擦,擦出来的格子带默认属性,主题色残底不留。
    std::cout << "\x1b[0m\x1b[" << (y + 1) << ";" << (x + 1) << "H\x1b[" << count << "X" << std::flush;
}

RawInputScope::RawInputScope() {
    ok_ = EnterRawTermios(&original_termios_);
}

RawInputScope::~RawInputScope() {
    if (ok_) {
        RestoreTermios(&original_termios_);
    }
}

std::optional<KeyInput> KeyReader::ReadOne() {
    (void)pending_high_surrogate_;  // Windows 专用状态,这里闲置
    const int b0 = ReadByteBlocking();
    if (b0 < 0) {
        return std::nullopt;
    }

    KeyInput out;
    if (b0 == 0x1b) {
        const int b1 = ReadByteTimeout(30);
        if (b1 < 0) {
            out.kind = KeyInput::Kind::Esc;  // 裸 ESC
            return out;
        }
        if (b1 == '[') {
            return ParseCsi();
        }
        if (b1 == 'O') {
            return ParseSs3();
        }
        if (b1 == '\r' || b1 == '\n') {
            out.kind = KeyInput::Kind::NewLine;  // Alt+Enter:ESC + CR
            return out;
        }
        // 其余 Alt+<key> 组合暂不映射,整个吃掉。
        return KeyInput{};
    }
    if (b0 == '\r' || b0 == '\n') {
        out.kind = KeyInput::Kind::Enter;
        return out;
    }
    if (b0 == 0x7f || b0 == 0x08) {
        out.kind = KeyInput::Kind::Backspace;
        return out;
    }
    if (b0 == 0x09) {
        out.kind = KeyInput::Kind::Tab;
        return out;
    }
    if (b0 == 0x03) {
        out.kind = KeyInput::Kind::CtrlC;
        return out;
    }
    if (b0 == 0x04) {
        out.kind = KeyInput::Kind::CtrlD;
        return out;
    }
    if (b0 == 0x0f) {
        out.kind = KeyInput::Kind::CtrlO;
        return out;
    }
    if (b0 == 0x05) {
        out.kind = KeyInput::Kind::CtrlE;
        return out;
    }
    if (b0 < 0x20) {
        return KeyInput{};  // 其余控制字符不映射
    }

    // UTF-8 解码:终端天然给 UTF-8 字节,按首字节定长收继续字节。坏序列
    // 按 None 丢弃,不往编辑器里塞半个字符。
    char32_t cp = 0;
    int extra = 0;
    if (b0 < 0x80) {
        cp = static_cast<char32_t>(b0);
    } else if ((b0 & 0xe0) == 0xc0) {
        cp = static_cast<char32_t>(b0 & 0x1f);
        extra = 1;
    } else if ((b0 & 0xf0) == 0xe0) {
        cp = static_cast<char32_t>(b0 & 0x0f);
        extra = 2;
    } else if ((b0 & 0xf8) == 0xf0) {
        cp = static_cast<char32_t>(b0 & 0x07);
        extra = 3;
    } else {
        return KeyInput{};  // 孤立的继续字节/非法首字节
    }
    for (int i = 0; i < extra; ++i) {
        const int bn = ReadByteTimeout(50);
        if (bn < 0 || (bn & 0xc0) != 0x80) {
            return KeyInput{};
        }
        cp = (cp << 6) | static_cast<char32_t>(bn & 0x3f);
    }
    out.kind = KeyInput::Kind::Char;
    out.ch = cp;
    return out;
}

bool WaitForKeyEvent(int timeout_ms) {
    if (!PendingBytes().empty()) {
        return true;
    }
    struct pollfd pfd{STDIN_FILENO, POLLIN, 0};
    return poll(&pfd, 1, timeout_ms) > 0;
}

KeyListenScope::KeyListenScope() {
    active_ = EnterRawTermios(&original_termios_);
}

KeyListenScope::~KeyListenScope() {
    if (active_) {
        RestoreTermios(&original_termios_);
    }
}

std::optional<std::string> ReadLineCooked() {
    // 不走 std::getline:这个兜底可能在两种模式下被调——终端还在原始模式
    // (逐键路径进去了一半、拿不到屏幕信息退回来),回车给的是裸 \r,
    // getline 等 \n 会吊死;也可能在规矩模式(行规程已把行编辑做完,读到
    // 的字节以 \n 收尾)。逐字节读、\r/\n 都算行尾,两种模式通吃;顺手
    // 处理退格(原始模式下没有行规程帮忙)。
    std::string line;
    bool got_any = false;
    while (true) {
        const int b = ReadByteBlocking();
        if (b < 0) {
            if (!got_any) {
                return std::nullopt;  // 干净的 EOF
            }
            break;  // 有内容的 EOF:先交出这一行
        }
        got_any = true;
        if (b == '\n' || b == '\r') {
            break;
        }
        if (b == 0x7f || b == 0x08) {
            if (!line.empty()) {
                line.pop_back();
            }
            continue;
        }
        if (b == 0x04 && line.empty()) {
            return std::nullopt;  // 行首 Ctrl+D:EOF 语义
        }
        line.push_back(static_cast<char>(b));
    }
    return line;
}

void SetupConsoleUtf8() {
    // POSIX 终端天然 UTF-8,不需要设代码页。
}

}  // namespace lubancode::platform
