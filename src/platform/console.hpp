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

#include <cstdint>
#include <mutex>
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

// stdin 字节流的进程级所有权。POSIX 的按键与 DSR 光标应答走同一条
// 输入流，逐键编辑器、后台监听和 GetScreenInfo 必须共用这把锁；Windows
// 也走同一接口，免得 cli 层另养一把平台外的锁。
//
// 用 recursive_timed_mutex 有两层缘故：逐键编辑器会持锁调用
// GetScreenInfo，同线程须能重入；流式画屏彼时已持 stdout 锁，若前台菜单
// 正攥着输入锁，DSR 查询只候一小会儿便退，不能反向等成死锁。
std::recursive_timed_mutex& ConsoleInputMutex();

// 流式期间的原地重画(TranscriptPainter 工具条目改写、StreamBodyTracker
// markdown 收束重画)在这个平台上开不开。Windows 用控制台 API 真探；
// POSIX 真终端发一次 DSR 实探，答得上才开。DSR 与按键的输入争用由
// ConsoleInputMutex 串开，探不到便退回纯流式，信息不丢。
bool SupportsScreenRepaint();

// ---------------------------------------------------------------------------
// 屏幕(光标/清行)
// ---------------------------------------------------------------------------

struct ScreenInfo {
    int width = 0;     // 缓冲区宽(列)
    int height = 0;    // Windows: 缓冲区总高;POSIX: 窗口行数(见文件头坐标约定)
    int cursor_x = 0;  // 光标列,0 基
    int cursor_y = 0;  // 光标行,0 基
    int viewport_x = 0;  // Windows 可视窗口左上角在缓冲区里的坐标;POSIX 恒 0
    int viewport_y = 0;
    int viewport_height = 0;  // 可视窗口行数;0 = 未知(调用方兜底用 height)
};

// 拿不到(非真控制台、查询失败)返回 std::nullopt,调用方这一帧放弃定位。
std::optional<ScreenInfo> GetScreenInfo();

void SetCursorPos(int x, int y);

// 清屏 + 清回滚缓冲(VT `\x1b[H\x1b[2J\x1b[3J`):光标归位、可见区清空、
// 历史回滚一并抹掉——对齐 Claude Code /clear 的"真清屏"体验。不探测
// is_console/VT 支持,调用方(main.cpp)自己判断要不要在非真终端/管道场景
// 跳过(ANSI 转义混进管道输出会污染脚本消费者)。
void ClearScreen();

// 从 (x, y) 起把 count 个单元格清成空格(不动光标语义:调用方随后总会
// SetCursorPos,不依赖清行后的光标位置)。
void ClearRowFrom(int x, int y, int count);

// 同上,但连字符属性(背景色等)一起还原成当前默认——CollapseBoxOnSubmit
// 擦框用,免得主题色的残底留在屏上。
void ClearRowHardFrom(int x, int y, int count);

// 把可视窗口往下平移 rows 行(内容一个字节不动,缓冲区绝对行号全部保真)。
// 经典 conhost 长缓冲里"绝对定位画帧画到窗口底下"全靠它救——写换行只会
// 在缓冲区末尾滚内容,救不了窗口中间的视野。返回实际平移的行数(贴到
// 缓冲区底就停);POSIX 无此原语(窗口即缓冲),恒 0。
int PanViewportDown(int rows);

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
        Paste,  // text 是 bracketed paste 捕获的完整 UTF-8 内容
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
        // 0.28.x 排队消息"取回编辑":Shift+Left(VK_LEFT+SHIFT_PRESSED / CSI 1;2D)
        // 与备用的 Ctrl+Left(CSI 1;5D)。编辑器本身没有"按词选择"语义,别处
        // 收到这两个键一律当普通 Left 处理(console 层 MapKey 的缺省映射),
        // 只有"composer 空、队列非空、非编辑态"那一处才当取回键(纯函数
        // ShouldRecallQueuedMessage 钉规矩,见 cli/queue_model.hpp)。
        ShiftLeft,
        CtrlLeft,
        CtrlC,
        CtrlD,
        CtrlO,
        CtrlE,
        // 会话选择器 Ctrl+T:看所选会话的转录浮层(开/收都在 picker 这一处
        // 消费,别处按死键处理,跟 Ctrl+O/Ctrl+E 的规矩一样)。
        CtrlT,
        // 0.28.x 子代理面板:Ctrl+X -> Ctrl+K 两段确认"停止全部代理"。
        // 只在面板这一处消费,别处按 Ctrl+X/Ctrl+K 仍是死键,跟升级前一样。
        CtrlX,
        CtrlK,
        // 0.30.x 多行历史边缘:Ctrl+P/Ctrl+N 是"上一条/下一条历史"的明确
        // 别名,不受多行光标位置影响(Up/Down 在多行里先走行间移动)。
        CtrlP,
        CtrlN,
        // 0.29.x 底栏自救:Ctrl+L 整屏重画(作废锚点、清可视区、从状态重建)。
        CtrlL,
        Esc,
        Delete,  // Del 键(排队待发消息浏览里"删当前项"用;两平台键序都认)
        // PageUp/PageDown(VK_PRIOR/VK_NEXT、CSI 5~/6~):会话选择器翻页用。
        // 只在 SessionPicker 一处消费,别处按死键处理,跟升级前一样。
        PageUp,
        PageDown,
    };
    Kind kind = Kind::None;
    char32_t ch = 0;
    std::string text;
    std::size_t replace_before = 0;  // Paste:先撤掉光标前多少个码点，再放附件
    // 修饰键标志(交互抛光总账:keymap 和弦层)。只对 Kind::Char 有意义:
    // 平台层把 Ctrl+字母/Alt+字母 这类"没有专枚举"的组合按 Char 送出并
    // 置位修饰键;编辑器核心对带修饰的 Char 一律不当正文插入,键位分发层
    // (cli/keymap)拿它匹配和弦。专枚举键(CtrlC/CtrlO……)不置位——它们
    // 自带语义,别处不该再当和弦二次匹配。
    bool ctrl = false;
    bool alt = false;
    bool shift = false;
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
    std::wstring rapid_text_run_;                     // Windows:高速到达、尚未遇到编辑键的正文
#ifdef _WIN32
    // 仅 console_win.cpp 用;POSIX 下编译掉——平凡类型闲置会报
    // -Wunused-private-field(rapid_text_run_ 非平凡,构造/析构即"使用",不报)。
    std::size_t rapid_char_count_ = 0;
    std::uint64_t last_text_tick_ = 0;
#endif
};

// 监听线程探测:timeout_ms 内 stdin 有没有输入事件可读(只问不消费)。
bool WaitForKeyEvent(int timeout_ms);

// M10 监听线程的会话档位:两边都须进逐键、无回显模式。Windows 虽用
// ReadConsoleInputW,但 ENABLE_LINE_INPUT/ECHO_INPUT 留着时字符仍会等回车
// 才放行,还会被控制台自行画到 footer 光标处;POSIX 同理受 termios 行规程
// 管。RAII 进出,监听收场后原样恢复。
class KeyListenScope {
public:
    KeyListenScope();
    ~KeyListenScope();

    KeyListenScope(const KeyListenScope&) = delete;
    KeyListenScope& operator=(const KeyListenScope&) = delete;

private:
    bool active_ = false;
#ifdef _WIN32
    unsigned long original_mode_ = 0;
#else
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
