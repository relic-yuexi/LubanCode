// 平台抽象层:控制台/终端(v0.20.x 跨平台单)。
//
// 接口从现有 Windows 代码提炼——cli/console_input.cpp 的
// GetConsoleScreenBufferInfo/SetConsoleCursorPosition/FillConsoleOutputCharacterW/
// ReadConsoleInputW 那套、cli/theme.cpp 的 VT 探测、main.cpp 的
// TranscriptPainter/StreamBodyTracker 锚点记账,全部收拢到这几个原语上;
// 业务层(console_input/main/theme)只认这里,不再直接碰 windows.h。
//
// 坐标约定:统一 0 基,(x, y) = (列, 行)。Windows 下 height 是控制台缓冲区
// 总高(dwSize.Y,含回滚),y 是缓冲区绝对行号;POSIX 下 height 是可视
// 窗口行数(TIOCGWINSZ),y 是窗口内行号(DSR 6n 应答,转成 0 基)。两边
// 的"锚点 + 探底滚屏"算法在各自坐标系里同样成立:写到最后一行会整体
// 上滚,调用方(EnsureRoomForRows 那套账)自己滚够、自己修正锚点。
#pragma once

#include <optional>
#include <string>

#ifndef _WIN32
#include <termios.h>
#endif

namespace lubancode::platform {

// ---------------------------------------------------------------------------
// 探测
// ---------------------------------------------------------------------------

// stdin 是不是挂着一个真控制台/终端(不是管道、不是重定向的磁盘文件)。
bool StdinIsInteractive();

// stdout 探测 + VT 开启:Windows 下顺手把 ENABLE_VIRTUAL_TERMINAL_PROCESSING
// 打开(搬自 cli/theme.cpp 的 DetectConsoleCapability);POSIX 真终端天然
// 支持 ANSI,vt_enabled 恒真。
struct StdoutConsoleProbe {
    bool is_console = false;
    bool vt_enabled = false;
};
StdoutConsoleProbe ProbeStdoutConsole();

// 真控制台此刻的显示宽度(列数),查 stdout。探测不到返回 std::nullopt,
// 调用方按 80 列兜底。
std::optional<int> ConsoleWidth();

// 流式期间的原地重画(TranscriptPainter 工具条目改写、StreamBodyTracker
// markdown 收束重画)在这个平台上开不开。Windows 真控制台:开。POSIX:
// 暂不开——重画要随时查光标位(DSR 6n 应答走 stdin),会跟流式期间的
// 后台按键监听线程抢同一条输入流,赛点没堵之前诚实关掉,输出退化成纯
// 流式(跟管道模式一致,信息不丢)。逐键行编辑不受此限(编辑期间监听
// 线程被互斥锁挡在门外,DSR 应答安全)。
bool SupportsScreenRepaint();

// ---------------------------------------------------------------------------
// 屏幕(光标/清行)
// ---------------------------------------------------------------------------

struct ScreenInfo {
    int width = 0;     // 缓冲区宽(列)
    int height = 0;    // Windows: 缓冲区总高;POSIX: 窗口行数(见文件头坐标约定)
    int cursor_x = 0;  // 光标列,0 基
    int cursor_y = 0;  // 光标行,0 基
};

// 拿不到(非真控制台、查询失败)返回 std::nullopt,调用方这一帧放弃定位。
std::optional<ScreenInfo> GetScreenInfo();

void SetCursorPos(int x, int y);

// 从 (x, y) 起把 count 个单元格清成空格(不动光标语义:调用方随后总会
// SetCursorPos,不依赖清行后的光标位置)。
void ClearRowFrom(int x, int y, int count);

// 同上,但连字符属性(背景色等)一起还原成当前默认——CollapseBoxOnSubmit
// 擦框用,免得主题色的残底留在屏上。
void ClearRowHardFrom(int x, int y, int count);

// ---------------------------------------------------------------------------
// 逐键输入
// ---------------------------------------------------------------------------

// 语义化按键,跟 cli::KeyKind 一一平行(platform 不依赖 cli,镜像一份)。
// None = 消费了一个底层事件但没有可用按键(修饰键、鼠标/窗口事件、半个
// 代理对/转义序列……),调用方 continue 即可。
struct KeyInput {
    enum class Kind {
        None,
        Char,  // ch 有效(Unicode 码点)
        Backspace,
        Left,
        Right,
        Home,
        End,
        Up,
        Down,
        Tab,
        ShiftTab,
        Enter,
        NewLine,  // Alt+Enter / Shift+Enter(POSIX: ESC+CR;裸终端分不出 Shift+Enter,认 Alt+Enter)
        CtrlC,
        CtrlD,
        CtrlO,
        CtrlE,
        Esc,
    };
    Kind kind = Kind::None;
    char32_t ch = 0;
};

// 原始逐键模式的进入/退出(RAII)。Windows: SetConsoleMode 关掉
// ENABLE_LINE_INPUT/ECHO_INPUT/PROCESSED_INPUT;POSIX: termios 关掉
// ICANON/ECHO/ISIG(Ctrl+C 变成可读按键,对齐 Windows 关 PROCESSED_INPUT
// 的语义)。ok() 为假表示进不去(非标准终端),调用方退回整行读入。
class RawInputScope {
public:
    RawInputScope();
    ~RawInputScope();

    RawInputScope(const RawInputScope&) = delete;
    RawInputScope& operator=(const RawInputScope&) = delete;

    bool ok() const { return ok_; }

private:
    bool ok_ = false;
#ifdef _WIN32
    unsigned long original_mode_ = 0;
#else
    struct termios original_termios_ {};
#endif
};

// 读一个底层输入事件,翻成语义按键。阻塞;返回 std::nullopt = EOF/读失败。
// Kind::None = 这个事件没产出按键(调用方 continue)。一次调用最多消费
// "一个"底层事件(Windows: 一条 INPUT_RECORD;POSIX: 一个按键的字节序列)
// ——后台监听线程靠这一点做到"读一下就放锁",绝不长期攥着输入流。
//
// 有跨事件状态(Windows 的 UTF-16 代理对配对),所以是个类;每段读取
// 逻辑(一次 ReadLine 调用、一条监听线程)各建各的实例。
class KeyReader {
public:
    std::optional<KeyInput> ReadOne();

private:
    std::optional<char32_t> pending_high_surrogate_;  // 仅 Windows 用;POSIX 下闲置无害
};

// 监听线程探测:timeout_ms 内 stdin 有没有输入事件可读(只问不消费)。
bool WaitForKeyEvent(int timeout_ms);

// M10 监听线程的会话档位:Windows 下 ReadConsoleInputW 不需要改控制台模式
// 就能读到按键事件,这里是空操作;POSIX 下必须进 termios 原始模式才能逐键
// 拿到(否则字符躺在行规程里等回车、还会回显),RAII 进出。
class KeyListenScope {
public:
    KeyListenScope();
    ~KeyListenScope();

    KeyListenScope(const KeyListenScope&) = delete;
    KeyListenScope& operator=(const KeyListenScope&) = delete;

private:
#ifndef _WIN32
    bool active_ = false;
    struct termios original_termios_ {};
#endif
};

// 整行读入的兜底(逐键模式进不去时用):Windows 用 ReadConsoleW 读宽字符
// 一整行再转 UTF-8(conhost 的行编辑器处理退格/输入法);POSIX 直接
// std::getline。EOF 返回 std::nullopt;行尾 \r\n 已剥掉。
std::optional<std::string> ReadLineCooked();

// 进程启动时的控制台编码初始化:Windows 把输入/输出代码页都设成 UTF-8
// (搬自 main.cpp 的 wmain);POSIX 天然 UTF-8,空操作。
void SetupConsoleUtf8();

}  // namespace lubancode::platform
