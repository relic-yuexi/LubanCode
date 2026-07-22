// 实测结论(诊断交互模式"答完一轮就自动退出"的 bug 时留下的记录):
//
// 现象:交互模式里过了两次工具确认([y]/[a]/[N] 各读一行),模型答完回到
// `> ` 主提示符,用户接着输中文,程序却直接退出了——跟"空行退出"的规则对上了,
// 说明那次 std::getline 读到的是空串,不是用户真敲的内容。
//
// 复现条件:这台机器上的 shell 工具(git-bash 起子进程)拿不到真控制台句柄
// (GetFileType(stdin) 恒为 FILE_TYPE_PIPE),没法用自动化脚本直接敲键盘去
// 触发 conhost 的 bug,已用 GetFileType/GetConsoleMode 探测程序实测确认了
// 这一点。管道场景下 CP_UTF8 这条坑本来就不会踩上(SetConsoleCP 只影响
// "真控制台"的 ReadFile 语义,对管道没有任何编码转换),所以自动化管道测试
// 天然复现不出这个 bug——这跟用户反馈"部分场景正常、部分场景炸"的说法也对得上。
//
// 病根按代码走查 + 已知问题排查确认为疑点 1:
//   Windows 的 conhost 在输入代码页设为 CP_UTF8(65001)时,narrow 版
//   ReadFile/ReadConsoleA 读多字节字符有年头的已知 bug——多次 ReadFile 交替
//   调用之后,内部对"上一次没读完的多字节序列"的状态会跟丢,后续一次读到
//   空串或半个字符。
//
// 修法:交互模式全程只留这一个 stdin 入口——真控制台就用宽字符 API 读,
// 彻底不走窄字符 CP_UTF8 那条路;stdin 是管道/重定向时走原来的
// std::getline,不影响 `echo "x" | lubancode.exe` 这种自动化用法。
//
// M6.5 补充:真控制台这条路从"ReadConsoleW 整行读入"升级成"逐键输入编辑器"
// (逐个键盘事件读,翻成 cli::KeyEvent 喂 LineEditorCore,按吐出来的
// RenderState 重画)。这一步没法在当前 headless 环境里自动化敲键盘验证
// (见上面"复现条件"),已经过编译告警检查(/W4 /permissive- 无告警)、
// 逐行代码走查、以及 LineEditorCore 纯逻辑部分的完整单测(见
// tests/test_line_editor.cpp)。原始逐键模式进不去(极少见,比如某些非标准
// 终端模拟器)时,退回到整行读入(没有补全/历史/模式切换,但至少能用)。
//
// 补丁记录:用户实测报过 slash 补全提示"越敲越堆、清不干净"——病根有二:
//   1. 老版本提示是单行大拼串,超过控制台宽度被自动折行,重画逻辑记的
//      起始行/光标位这两个锚点没算上折出去的那些物理行,清不到。
//   2. 上一帧画了几行提示没记账,新帧不知道该擦几行。
// 修法:核心层 RenderState 把提示从单行字符串(hint_line)改成
// std::vector<std::string> hint_lines(一元素一逻辑行,核心层保证不折行由
// 终端层截断兜底);终端层每行落笔前按屏宽截断、记住上一帧画了几行提示、
// 多退少补地清。顺带把"离缓冲区底部太近导致自动滚屏、锚点跟着失效"这条
// 以前记在案但没堵上的已知限制也一并处理了(见 RedrawEditArea/
// EnsureRoomForRows)。
//
// 跨平台单(v0.20.x):Win32 控制台 API(GetConsoleScreenBufferInfo/
// SetConsoleCursorPosition/FillConsoleOutputCharacterW/ReadConsoleInputW)
// 原样搬进 platform/console_win.cpp,这里的重画/锚点算法一字未改,只是改
// 调 platform:: 的原语——同一套算法在 POSIX(ANSI 光标定位 + termios 逐
// 键)上也能跑,windows.h 不再进这个文件。

#include "cli/console_input.hpp"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdlib>
#include <iostream>
#include <mutex>

#include "cli/divider.hpp"
#include "cli/format_utils.hpp"
#include "cli/i18n.hpp"
#include "cli/slash_commands.hpp"
#include "platform/console.hpp"

namespace lubancode::cli {

namespace {

void StripTrailingCrLf(std::string& s) {
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) {
        s.pop_back();
    }
}

// 贯穿整条交互会话存活的编辑器实例:main.cpp 里 `> ` 主循环、工具确认
// 提示、/model 选择、初次配置向导,全部经这一个 ReadLine() 入口,底下共用
// 这一份 LineEditorCore——历史列表、确认模式才有地方跨多轮读取存住。
// 补全候选从 cli::slash_commands 现有定义转过来,不重复写一份命令清单。
LineEditorCore& SharedEditor() {
    static LineEditorCore editor = [] {
        std::vector<CompletionCandidate> candidates;
        for (const auto& cmd : AllSlashCommands()) {
            candidates.push_back(CompletionCandidate{cmd.name, cmd.description});
        }
        return LineEditorCore(std::move(candidates));
    }();
    return editor;
}

// UI-D(0.16.0):会话级 UI 按键回调(Ctrl+O/Ctrl+E/焦点导航)。存这儿、
// 由 SetTranscriptUiHandler 装卸;ReadLineKeyByKey 只在 composer 读取里查它。
TranscriptUiHandler& UiHandlerSlot() {
    static TranscriptUiHandler handler;
    return handler;
}

// 0.17.0:常驻状态行数据(模型名/context 百分比/token 数)。main.cpp 每轮
// 更新,ReadLineKeyByKey 每帧重画状态行时读——都在主线程上,不用加锁
// (监听线程从不碰状态行)。
struct StatusLineData {
    std::string model;
    int percent = 0;
    long long used_tokens = 0;
    long long window_tokens = 0;
};
StatusLineData& StatusDataSlot() {
    static StatusLineData data;
    return data;
}

// 0.21.x 流式脚注状态(见 console_input.hpp 里 BeginStreamFooter 一带的注释)。
// 全部读写都在 StdoutWriteMutex 之内:RunTurn(Begin/End)、监听线程(键入
// 回显)、StreamBodyTracker::OnDelta(每笔正文前后 Erase/Redraw)三处都先
// 拿锁再碰它,故字段本身不用再套原子。
struct StreamFooterState {
    bool enabled = false;   // 只有 Windows 真控制台 + 开了重画才为真
    std::string hint;       // 空闲占位提示(BeginStreamFooter 按主题 plain 与否建好)
    std::string echo;       // 排队实时回显文本;空串 = 输入行显示占位提示
    std::string color;      // 淡色前缀(theme.stats);plain 主题为空串
    std::string reset;      // theme.reset;plain 主题为空串
    Theme theme;             // 完整主题:画框(BoxRuleLine)、状态行(PrintStatusLine)
                             // 直接复用 composer 那两个画法,得传整个 Theme,不能只传片段。
    std::vector<std::string> queued;  // 已落队、等本轮结束后发送的消息
    int row = -1;             // 整块 footer 顶行,-1 = 没画
    int rows = 0;             // 上次实际画了几行(队列区会让高度变化)
    int body_x = -1;          // footer 下方藏着的正文续写位置
    int body_y = -1;
    // 0.22.5:工具确认交互期间为真——见 console_input.hpp
    // StreamFooterSuspendScope 注释。挂起期 RedrawStreamFooterLocked() 直接
    // 空操作,不管调用方是谁(ticker/OnDelta/监听线程键入回显),不用逐个
    // 调用点接管。
    bool suspended = false;
};
StreamFooterState& FooterSlot() {
    static StreamFooterState f;
    return f;
}

// 0.22.x 流式脚注框化:跟 composer 视觉一致的完整框——上横线 + 输入行
// (`> ` + 已键入内容 / 空闲占位提示) + 下横线 + 状态行,基础 4 行；有
// 排队消息时再在上方加常驻队列区。上下横线复用 BoxRuleLine、状态行复用
// PrintStatusLine(定义见下面 composer 输入框那节),不重写一份画法。
constexpr int kStreamFooterBoxRows = 4;
constexpr std::size_t kMaxVisibleQueuedLines = 3;
// synchronized output(DEC 私有模式 2026):写之前 h、写完 l,把一次重画钉成
// 一帧提交,避免终端半途刷出"擦了一半/画了一半"的画面。已用 web 检索核实过
// 假设:ECMA-48/xterm 的通用约定是私有模式号不认得就直接吞掉、不报错也不
// 触发别的动作(iTerm2 Feature Reporting Spec、xterm ctlseqs 文档都明确要求
// "未知但格式合法的 CSI 私有模式必须被正确解析后安全忽略");Windows
// Terminal 从 1.24 Preview 起已经原生实现 DECSET 2026(conhost 共用同一套
// VT 引擎),老版本 Windows Terminal/conhost 不认这个模式号,按上面的约定
// 静默吞掉,不会有副作用——不是"想当然",是查过 xterm 规范原文 + Windows
// Terminal 官方 PR 说明后的结论。
constexpr const char* kSyncOutputBegin = "\x1b[?2026h";
constexpr const char* kSyncOutputEnd = "\x1b[?2026l";

// 把 top_row 起 kStreamFooterBoxRows 行清空(连字符属性一起还原,不留主题色
// 残底,同 CollapseBoxOnSubmit 的取舍)——越界的行(贴着缓冲区顶/底)直接
// 跳过,不是错误。
void ClearStreamFooterRowsAt(int top_row, int rows, int width, int height) {
    for (int i = 0; i < rows; ++i) {
        const int y = top_row + i;
        if (y < 0 || y >= height) {
            continue;
        }
        platform::ClearRowHardFrom(0, y, width);
    }
}

// M10:谁在真的逐键读键盘,谁就得先拿到这把锁——ReadLineKeyByKey() 整个
// 调用期间(从进函数到返回)一直攥着它,TurnInputListener 的监听线程只在
// 抢到锁的间隙才读一次。这样"监听只活在编辑器不在读的窗口期"这条要求
// 不用靠回调层层传参去手动维护,两边天然靠锁互斥错开——工具确认提示
// [y/a/N] 走的也是 ReadLineKeyByKey(),天然一并受益,不用另外接管。
std::mutex& ConsoleReadMutex() {
    static std::mutex m;
    return m;
}

// platform 层的语义按键 -> 核心层 KeyEvent。两个枚举一一平行(platform 不
// 依赖 cli,镜像了一份),这里只是搬运;None 翻成 nullopt,调用方 continue。
std::optional<KeyEvent> MapKey(const platform::KeyInput& key) {
    using PK = platform::KeyInput::Kind;
    switch (key.kind) {
        case PK::None:
            return std::nullopt;
        case PK::Char:
            return KeyEvent::Char(key.ch);
        case PK::Backspace:
            return KeyEvent::Simple(KeyKind::Backspace);
        case PK::Left:
            return KeyEvent::Simple(KeyKind::Left);
        case PK::Right:
            return KeyEvent::Simple(KeyKind::Right);
        case PK::Home:
            return KeyEvent::Simple(KeyKind::Home);
        case PK::End:
            return KeyEvent::Simple(KeyKind::End);
        case PK::Up:
            return KeyEvent::Simple(KeyKind::Up);
        case PK::Down:
            return KeyEvent::Simple(KeyKind::Down);
        case PK::Tab:
            return KeyEvent::Simple(KeyKind::Tab);
        case PK::ShiftTab:
            return KeyEvent::Simple(KeyKind::ShiftTab);
        case PK::Enter:
            return KeyEvent::Simple(KeyKind::Enter);
        case PK::NewLine:
            return KeyEvent::Simple(KeyKind::NewLine);
        case PK::CtrlC:
            return KeyEvent::Simple(KeyKind::CtrlC);
        case PK::CtrlD:
            return KeyEvent::Simple(KeyKind::CtrlD);
        case PK::CtrlO:
            return KeyEvent::Simple(KeyKind::CtrlO);
        case PK::CtrlE:
            return KeyEvent::Simple(KeyKind::CtrlE);
        case PK::Esc:
            return KeyEvent::Simple(KeyKind::Esc);
    }
    return std::nullopt;
}

// 探一下从 start_row 起要画 total_rows 行(编辑行 + 提示行)会不会撞到
// 屏幕缓冲区最后一行(GetScreenInfo 的 height)——真撞上了,后面往下写
// 东西会让缓冲区内容整体往上滚,start_row 记的那个绝对行号就跟着报废。
// 这里主动先滚够所需的行数(挪到缓冲区最后一行、写换行逼滚屏)、同步把
// start_row 往上修正相同的行数,账目对平之后再画,不留"锚点失效"的口子
// ——这是修复"提示行堆残骸"时确认到的另一个锚点失效来源,原地一并堵上。
void EnsureRoomForRows(int& start_row, int buffer_height, int total_rows) {
    const int needed_bottom_row = start_row + total_rows - 1;
    if (needed_bottom_row < buffer_height) {
        return;  // 缓冲区里本来就放得下,不用滚
    }
    const int overflow = needed_bottom_row - buffer_height + 1;
    platform::SetCursorPos(0, buffer_height - 1);
    for (int i = 0; i < overflow; ++i) {
        std::cout << "\n";
    }
    std::cout.flush();
    start_row -= overflow;
    if (start_row < 0) {
        start_row = 0;
    }
}

// -----------------------------------------------------------------------
// 0.17.0 输入框化(Claude Code 版式):composer 主提示符的编辑区装进两根
// 长横线之间——上横线、`> ` 输入行(多行 composer 随内容长高)、下横线,
// 下横线之下再常驻一行状态行(确认档/模型名/context 占比),slash 候选
// 提示行挪到状态行之下。只有 composer 读取开框;确认提示、/model 编号
// 选择、向导这些单行读取跟从前一个样。管道/重定向走不到逐键路径,天然
// 无框无状态行。
// -----------------------------------------------------------------------

struct BoxChrome {
    bool enabled = false;
    const Theme* theme = nullptr;
    ConfirmMode mode = ConfirmMode::Confirm;
};

// 一根框线(带主题淡色;plain 主题 theme.stats/reset 都是空串,自动退化成
// 无色 '-' 线,不用另判断)。0.21.x:去掉旧的 100 列上限,框线满终端宽
// (console_width - 1)随终端跑——max_width 传 console_width 自身,
// min(console_width - 1, console_width) 恒等于 console_width - 1。
std::string BoxRuleLine(const Theme& theme, int console_width) {
    const bool plain = theme.reset.empty();
    return theme.stats + BuildDividerLine(console_width, plain, console_width) + theme.reset;
}

// 状态行:模式段按档配色(确认=默认色、auto=stats、yolo=error),信息段
// 恒 stats 淡色。0.21.x 起状态行是档位的唯一去处(提示符不再带前缀)。文本拼装是
// cli/format_utils 的纯函数,这里只管配色和按控制台宽度分段截断(截断得
// 按段做——夹着 ANSI 的整行没法安全截)。
void PrintStatusLine(const BoxChrome& chrome, int max_width) {
    const Theme& theme = *chrome.theme;
    const StatusLineData& data = StatusDataSlot();
    std::string seg_mode = StatusLineModeSegment(chrome.mode);
    std::string seg_info =
        StatusLineInfoSegment(data.model, data.percent, data.used_tokens, data.window_tokens);
    seg_mode = TruncateUtf8ToDisplayWidth(seg_mode, max_width);
    const int rest = max_width - static_cast<int>(DisplayWidthUtf8(seg_mode));
    seg_info = rest > 0 ? TruncateUtf8ToDisplayWidth(seg_info, rest) : std::string();

    std::string mode_color;
    switch (chrome.mode) {
        case ConfirmMode::Auto:
            mode_color = theme.stats;
            break;
        case ConfirmMode::Yolo:
            mode_color = theme.error;
            break;
        case ConfirmMode::Confirm:
            break;  // 默认色
    }
    std::cout << mode_color << seg_mode << (mode_color.empty() ? std::string() : theme.reset)
              << theme.stats << seg_info << theme.reset;
}

// 按 RenderState 重画"编辑区域"。UI-A(0.11.0)起编辑区不止一行:第一行是
// 提示符前缀 + prompt + composer 第一行,composer 后续行各占一个物理行、
// 行首两空格续行缩进(提示符只在第一行),再往下紧跟 0~N 行 hint_lines
// (每个元素一个逻辑行,列出匹配的 slash 命令——多行 composer 下核心层
// 保证 hint_lines 恒空)。start_row/prompt_end_col 是这次 ReadLine() 调用
// 一开始(或者 Shift+Tab 切模式重打提示符之后)测出来的,之后的每次重画
// 都锚定在这个位置上。
//
// 记账规则沿用修"提示堆残骸"那次立下的两条,只是从"提示行数"扩成"第一行
// 之外的全部行数"(composer 续行 + 提示行统一算):
//   1. 每行落笔之前按控制台此刻的实际宽度截断,物理上绝不折行——折行会让
//      "起始行/光标位"这两个锚点失效,是清不干净的根子。每一行 composer
//      内容都各自按可视窗口截断(ComputeEditLineWindow 按行应用,光标所在
//      行给真光标位、其余行从行首截),绝不折行。
//   2. 上一帧第一行之外到底画了几行得记账(prev_body_row_count):这一帧
//      行数比上一帧少(删行、清空、提示消失),多出来的旧行要显式清空,
//      不能只清"新内容覆盖到的那部分"。
// 顺带用 EnsureRoomForRows 按总行数探底滚屏,把"离缓冲区底部太近导致自动
// 滚屏、坐标跟着失效"这条已知限制堵住(增删行时锚点账目同步修正)。
void RedrawEditArea(int& start_row, int& prompt_end_col, const RenderState& state,
                     int& prev_body_row_count, const BoxChrome& chrome = BoxChrome{}) {
    const std::optional<platform::ScreenInfo> info = platform::GetScreenInfo();
    if (!info.has_value()) {
        return;  // 拿不到屏幕信息就没法定位,这一帧放弃重画,下一帧再试
    }
    const int buffer_width = info->width;
    const int buffer_height = info->height;

    constexpr int kContinuationIndent = 2;  // composer 续行的两空格缩进

    // 核心层保证 state.lines 至少一个元素,这里再兜一手,免得空 vector 把
    // 下标访问带崩。
    const std::vector<std::u32string> fallback_lines{std::u32string()};
    const std::vector<std::u32string>& edit_lines = state.lines.empty() ? fallback_lines : state.lines;
    const int edit_row_count = static_cast<int>(edit_lines.size());
    const int hint_count = static_cast<int>(state.hint_lines.size());
    // 0.17.0 输入框化:开框时最后一个编辑行下面再垫两行——下横线 + 状态行,
    // 提示行(hint_lines)排在状态行之下。锚点记账原样扩展:这两行也算进
    // body_rows,多退少补、EnsureRoomForRows 探底,全套现成机制照走。
    const int box_rows = chrome.enabled ? 2 : 0;
    // "第一行之外"这一帧要占的行数(composer 续行 + 框线/状态行 + 提示行),
    // 跟上一帧取较大值,保证旧帧多出来的行(删行、清 composer、提示消失)
    // 不漏清。
    const int body_rows = (edit_row_count - 1) + box_rows + hint_count;
    const int rows_to_touch = std::max(body_rows, prev_body_row_count);

    EnsureRoomForRows(start_row, buffer_height, 1 + rows_to_touch);

    // 光标最终该落的位置,画完统一挪过去(先给个兜底值:第一行提示符后)。
    int final_cursor_x = prompt_end_col;
    int final_cursor_y = start_row;

    // 第一行:只清"提示符之后"这一段(从 prompt_end_col 到行尾),提示符
    // 本身那几列一个字符都不碰——清整行会把 "> "/"[auto] > " 盖没,是修过
    // 的老 bug,别再犯。
    {
        const int content_width = buffer_width - prompt_end_col - 1;
        const std::size_t cursor_in_row = state.cursor_row == 0 ? state.cursor_col : 0;
        const EditLineWindow window = ComputeEditLineWindow(edit_lines[0], cursor_in_row, content_width);
        const int clear_width = buffer_width - prompt_end_col;
        if (clear_width > 0) {
            platform::ClearRowFrom(prompt_end_col, start_row, clear_width);
        }
        platform::SetCursorPos(prompt_end_col, start_row);
        std::cout << Utf32ToUtf8(window.text);
        std::cout.flush();
        if (state.cursor_row == 0) {
            final_cursor_x = prompt_end_col + static_cast<int>(window.cursor_display_col);
            final_cursor_y = start_row;
        }
    }

    // 第一行之外:逐行先整行清、再按行的身份画——composer 续行(两空格
    // 缩进 + 窗口截断)、下横线/状态行(开框时)、提示行(截宽),或者
    // 旧帧残行(只清不画)。
    for (int i = 1; i <= rows_to_touch; ++i) {
        const int row_y = start_row + i;
        platform::ClearRowFrom(0, row_y, buffer_width);
        if (i < edit_row_count) {
            const int content_width = buffer_width - kContinuationIndent - 1;
            const std::size_t row_index = static_cast<std::size_t>(i);
            const std::size_t cursor_in_row = state.cursor_row == row_index ? state.cursor_col : 0;
            const EditLineWindow window = ComputeEditLineWindow(edit_lines[row_index], cursor_in_row, content_width);
            platform::SetCursorPos(kContinuationIndent, row_y);
            std::cout << Utf32ToUtf8(window.text);
            std::cout.flush();
            if (state.cursor_row == row_index) {
                final_cursor_x = kContinuationIndent + static_cast<int>(window.cursor_display_col);
                final_cursor_y = row_y;
            }
        } else if (chrome.enabled && i == edit_row_count) {
            // 下横线,紧贴最后一个编辑行。
            platform::SetCursorPos(0, row_y);
            std::cout << BoxRuleLine(*chrome.theme, buffer_width);
            std::cout.flush();
        } else if (chrome.enabled && i == edit_row_count + 1) {
            // 常驻状态行。
            platform::SetCursorPos(0, row_y);
            PrintStatusLine(chrome, buffer_width - 1);
            std::cout.flush();
        } else if (i - edit_row_count - box_rows >= 0 && i - edit_row_count - box_rows < hint_count) {
            const int hint_width = buffer_width - 1;
            const std::string truncated = TruncateUtf8ToDisplayWidth(
                state.hint_lines[static_cast<std::size_t>(i - edit_row_count - box_rows)], hint_width);
            platform::SetCursorPos(0, row_y);
            std::cout << truncated;
            std::cout.flush();
        }
    }
    prev_body_row_count = body_rows;

    // 画完把光标挪回它该在的那一行那一列——不管画没画续行/提示行,这一步
    // 都得做,不然光标就停在最后一行上了。
    platform::SetCursorPos(final_cursor_x, final_cursor_y);
}

// 0.17.0 输入框化的提交收尾:横线擦掉、提交行保留。取舍:两个方案里选了
// "只留 `> 内容`"这条——框连横线留在滚动历史的话,每一问上下各一根 100 列
// 横线,再加 RunTurn 紧接着打的输入/输出分界线,三根线叠一块,滚动历史
// 全是线;擦掉横线后历史里就是干干净净的 `> 问题` + 分界线 + 回答,跟
// 0.16.0 的历史观感一致,框只属于"正在输入"这一刻。
//
// 做法:把整个框(上横线在 start_row - 1,下横线/状态行/提示行在编辑行
// 之下)统统清掉,从上横线那一行起重打提交内容(`> ` 第一行、两空格续行,
// 每行按宽截断防折行——完整内容反正在历史/存档里),光标停在末行末尾,
// 调用方接着换行。内容整体上移一行,不留空行。
void CollapseBoxOnSubmit(int start_row, int prev_body_row_count, const RenderState& state,
                          const std::string& prompt) {
    const std::optional<platform::ScreenInfo> info = platform::GetScreenInfo();
    if (!info.has_value()) {
        return;  // 拿不到屏幕信息就不收尾了,提交帧画面已经在屏上,不算坏
    }
    const int buffer_width = info->width;
    const int top = start_row > 0 ? start_row - 1 : start_row;
    int bottom = start_row + prev_body_row_count;
    if (bottom >= info->height) {
        bottom = info->height - 1;
    }
    for (int r = top; r <= bottom; ++r) {
        platform::ClearRowHardFrom(0, r, buffer_width);
    }
    platform::SetCursorPos(0, top);
    std::cout << prompt;
    std::cout.flush();
    int first_width = buffer_width - 3;
    if (const std::optional<platform::ScreenInfo> after = platform::GetScreenInfo(); after.has_value()) {
        first_width = buffer_width - after->cursor_x - 1;
    }
    if (!state.lines.empty()) {
        std::cout << Utf32ToUtf8(TruncateToDisplayWidth(state.lines[0], first_width));
        for (std::size_t i = 1; i < state.lines.size(); ++i) {
            std::cout << "\n  "
                      << Utf32ToUtf8(TruncateToDisplayWidth(state.lines[i], buffer_width - 3));
        }
    }
    std::cout.flush();
}

// 逐键读入这一次输入(UI-A 起,composer 模式下可能是多行),真控制台专用。
// platform::KeyReader 逐个键盘事件读、翻成语义按键,再映射成 cli::KeyEvent
// 喂 SharedEditor(),按吐出来的 RenderState 重画。
//
// esc_rejects/composer:见 console_input.hpp 里 ReadLine() 的同名参数注释。
std::optional<std::string> ReadLineKeyByKey(const std::string& prompt, const Theme& theme, bool esc_rejects,
                                             bool composer) {
    // 整个函数体都攥着这把锁:M10 的 TurnInputListener 监听线程只在抢到锁
    // 的间隙才读控制台输入,这一行锁一上,就等于宣布"编辑器正在读",监听
    // 线程会自动让出、不跟这里抢同一份键盘输入。
    std::lock_guard<std::mutex> console_read_lock(ConsoleReadMutex());

    // 原始逐键模式进不去(极少见,非标准终端)就退回整行读入。
    platform::RawInputScope raw_scope;
    if (!raw_scope.ok()) {
        return platform::ReadLineCooked();
    }

    LineEditorCore& editor = SharedEditor();
    editor.BeginLine(composer);

    // 0.17.0:composer 读取开输入框(上横线 + `> ` 输入行 + 下横线 + 状态
    // 行)。上横线打在提示符上一行,不进重画账(锚点 start_row 是提示符
    // 行,横线在 start_row - 1,重画从不碰它;EnsureRoomForRows 逼滚屏时
    // 整个缓冲区内容连横线一起上移,start_row 同步修正,账目还是平的)。
    const bool box = composer;
    BoxChrome chrome{box, &theme, editor.confirm_mode()};
    if (box) {
        const std::optional<platform::ScreenInfo> pre_info = platform::GetScreenInfo();
        const int console_width = pre_info.has_value() ? pre_info->width : 80;
        std::cout << BoxRuleLine(theme, console_width) << "\n";
    }

    // 0.21.x:提示符统一回归 `> `,不再冠 [auto]/[yolo] 档位前缀——档位改
    // 由常驻状态行(颜色 + 文字)承载,提示符不再重复一遍。
    std::cout << prompt;
    std::cout.flush();

    const std::optional<platform::ScreenInfo> info = platform::GetScreenInfo();
    if (!info.has_value()) {
        return platform::ReadLineCooked();  // 拿不到屏幕信息就没法定位光标,退回整行读入
    }
    int start_row = info->cursor_y;
    int prompt_end_col = info->cursor_x;
    int prev_body_row_count = 0;  // 上一帧第一行之外画了几行(composer 续行 + 框线/状态行 + 提示行)

    if (box) {
        // 开场帧:下横线 + 状态行立刻就位,不等第一个按键。
        RedrawEditArea(start_row, prompt_end_col, editor.CurrentRenderState(), prev_body_row_count, chrome);
    }

    platform::KeyReader key_reader;

    while (true) {
        const std::optional<platform::KeyInput> raw_key = key_reader.ReadOne();
        if (!raw_key.has_value()) {
            return std::nullopt;  // EOF/读失败
        }
        const std::optional<KeyEvent> mapped = MapKey(*raw_key);
        if (!mapped.has_value()) {
            continue;  // 修饰键、没映射到的键、半个组合序列,跳过
        }

        const RenderState state = editor.HandleKey(*mapped);

        // UI-D(0.16.0):composer 读取里,把核心层翻好的 UI 按键语义转发给
        // 应用层回调。流程:光标先挪到编辑区最后一行、换行(回调的输出从
        // 编辑区下面开始铺,旧编辑区画面留在上面,滚动历史自然带走);回调
        // 返回 true 表示真铺了内容——重打提示符、重测锚点、按当前编辑内容
        // 原样重画,接着等键;返回 false 表示这个键没被消费(比如焦点导航
        // 时 transcript 是空的、ESC 不在聚焦查看态),落回下面的原有处理
        // (RedrawEditArea 会把光标摆回去,挪动无痕)。
        if (composer && UiHandlerSlot()) {
            std::optional<UiKeyAction> action;
            if (state.toggle_expand_requested) {
                action = UiKeyAction::ToggleExpand;
            } else if (state.focus_view_requested) {
                action = UiKeyAction::FocusView;
            } else if (state.focus_move > 0) {
                action = UiKeyAction::FocusOlder;
            } else if (state.focus_move < 0) {
                action = UiKeyAction::FocusNewer;
            } else if (state.esc_pressed) {
                action = UiKeyAction::Escape;
            }
            if (action.has_value()) {
                if (const std::optional<platform::ScreenInfo> before_info = platform::GetScreenInfo();
                    before_info.has_value()) {
                    int last_row = start_row + prev_body_row_count;
                    if (last_row >= before_info->height) {
                        last_row = before_info->height - 1;
                    }
                    platform::SetCursorPos(0, last_row);
                }
                const bool handled = UiHandlerSlot()(*action);
                if (handled) {
                    // 回调铺完内容,整个框(上横线起)重打一遍、重测锚点。
                    if (box) {
                        const std::optional<platform::ScreenInfo> rule_info = platform::GetScreenInfo();
                        const int console_width = rule_info.has_value() ? rule_info->width : 80;
                        std::cout << BoxRuleLine(theme, console_width) << "\n";
                    }
                    std::cout << prompt;
                    std::cout.flush();
                    if (const std::optional<platform::ScreenInfo> after_info = platform::GetScreenInfo();
                        after_info.has_value()) {
                        start_row = after_info->cursor_y;
                        prompt_end_col = after_info->cursor_x;
                    }
                    prev_body_row_count = 0;
                    chrome.mode = editor.confirm_mode();
                    RedrawEditArea(start_row, prompt_end_col, state, prev_body_row_count, chrome);
                    continue;
                }
                // 0.17.0:焦点导航请求没被消费(比如 transcript 还是空的)——
                // 把核心层的焦点态退掉,不然屏上什么都没发生,下一下
                // Shift+Tab 却被当成"焦点往新走"吞掉,切不了档。
                if (*action == UiKeyAction::FocusOlder || *action == UiKeyAction::FocusNewer) {
                    editor.ExitFocusMode();
                }
            }
        }

        // M11(0.10.0):切档原地更新,不打印任何新行——用户反馈过"连按几轮
        // Shift+Tab,'已切换到 X 模式' 通知行滚出一屏残骸"。0.21.x 起提示符
        // 不再带档位前缀,切档不动提示符那一行;档位只体现在常驻状态行上
        // (下面 chrome.mode 刚同步过,紧接着的 RedrawEditArea 每帧重画状态行,
        // 自然带上新档),屏上零新增行。

        if (box && state.submitted) {
            // 0.17.0 输入框化的提交收尾:横线/状态行擦掉,只留 `> 内容`,
            // 换行收尾——取舍见 CollapseBoxOnSubmit 注释。
            CollapseBoxOnSubmit(start_row, prev_body_row_count, state, prompt);
            std::cout << "\n";
            return Utf32ToUtf8(state.line);
        }

        chrome.mode = editor.confirm_mode();
        RedrawEditArea(start_row, prompt_end_col, state, prev_body_row_count, chrome);

        if (state.esc_pressed && esc_rejects) {
            // 确认提示 [y/a/N] 场景:Esc 不留在循环里继续等,直接当这次
            // 读取交了个空串——main.cpp 的确认回调本来就把"不是 y/Y/a/A"
            // 都当拒绝,空串天然就是拒绝。
            std::cout << "\n";
            return std::string();
        }
        if (state.eof_requested) {
            // Ctrl+D/Ctrl+Z 可能按在多行 composer 中间某一行,框下面还垫着
            // 横线/状态行:先把光标挪到编辑区(含框)最下面一行再换行,免得
            // 接下来的输出打在残留画面身上。prev_body_row_count 刚被
            // RedrawEditArea 更新过,就是"第一行之外"的总行数。
            if (prev_body_row_count > 0) {
                platform::SetCursorPos(0, start_row + prev_body_row_count);
            }
            std::cout << "\n";
            return std::nullopt;
        }
        if (state.cleared) {
            continue;  // composer 已经清空,继续在同一次调用里编辑
        }
        if (state.submitted) {
            // 非框读取的提交:RedrawEditArea 刚按提交帧把完整多行内容画在
            // 屏幕上、光标停在末行末尾,这里换行收尾。
            std::cout << "\n";
            return Utf32ToUtf8(state.line);
        }
    }
}

}  // namespace

std::optional<std::string> ReadLine(const std::string& prompt, const Theme& theme, bool esc_rejects, bool composer) {
    if (platform::StdinIsInteractive()) {
        return ReadLineKeyByKey(prompt, theme, esc_rejects, composer);
    }
    (void)composer;  // 管道/重定向:没有 composer 概念,照旧逐行 getline

    if (!prompt.empty()) {
        std::cout << prompt;
        std::cout.flush();
    }
    std::string line;
    if (!std::getline(std::cin, line)) {
        return std::nullopt;
    }
    StripTrailingCrLf(line);
    return line;
}

ConfirmMode CurrentConfirmMode() { return SharedEditor().confirm_mode(); }

void SetConfirmMode(ConfirmMode mode) { SharedEditor().set_confirm_mode(mode); }

void SetTranscriptUiHandler(TranscriptUiHandler handler) { UiHandlerSlot() = std::move(handler); }

void SetStatusLineData(const std::string& model, int context_percent, long long used_tokens,
                        long long window_tokens) {
    StatusLineData& data = StatusDataSlot();
    data.model = model;
    data.percent = context_percent;
    data.used_tokens = used_tokens;
    data.window_tokens = window_tokens;
}

std::optional<int> DetectConsoleWidth() {
    return platform::ConsoleWidth();
}

std::mutex& StdoutWriteMutex() {
    static std::mutex m;
    return m;
}

namespace {

// 见头文件 SetStreamScreenPrintHook 注释。读与写全在 StdoutWriteMutex
// 之内(设置方锁内赋值,监听线程锁内取用),不必再套一层原子/锁。
std::function<void()>& StreamScreenPrintHookSlot() {
    static std::function<void()> hook;
    return hook;
}

}  // namespace

void SetStreamScreenPrintHook(std::function<void()> hook) {
    std::lock_guard<std::mutex> lock(StdoutWriteMutex());
    StreamScreenPrintHookSlot() = std::move(hook);
}

void EraseStreamFooterLocked() {
    StreamFooterState& f = FooterSlot();
    if (f.row < 0) {
        return;  // 没画,不用擦
    }
    if (const std::optional<platform::ScreenInfo> info = platform::GetScreenInfo(); info.has_value()) {
        // 屏上光标停在输入行；正文续写点另存在 state 里。擦完须拨回
        // 正文，下一笔流式文字才能接对地方。
        const int bx = f.body_x >= 0 ? f.body_x : info->cursor_x;
        const int by = f.body_y >= 0 ? f.body_y : info->cursor_y;
        std::cout << kSyncOutputBegin;
        ClearStreamFooterRowsAt(f.row, f.rows, info->width, info->height);
        std::cout << kSyncOutputEnd;
        std::cout.flush();
        platform::SetCursorPos(bx, by);
    }
    f.row = -1;
    f.rows = 0;
    f.body_x = -1;
    f.body_y = -1;
}

void RedrawStreamFooterLocked() {
    StreamFooterState& f = FooterSlot();
    if (!f.enabled || f.suspended) {
        return;  // 挂起期间(工具确认交互中)一律不画,见 StreamFooterSuspendScope 注释
    }
    const std::optional<platform::ScreenInfo> info = platform::GetScreenInfo();
    if (!info.has_value()) {
        return;  // 拿不到屏幕信息这一笔就不画,下一笔再来
    }
    // footer 已在屏上时，物理光标位于输入行；正文落笔位要从 state 取。
    // 第一次画才从当前光标认领正文位置。
    const int bx = f.row >= 0 && f.body_x >= 0 ? f.body_x : info->cursor_x;
    const int by = f.row >= 0 && f.body_y >= 0 ? f.body_y : info->cursor_y;

    std::cout << kSyncOutputBegin;  // 擦旧框 + 画新框整个当一帧提交,别让半路的画面露出来

    // 先擦掉旧框(正文可能已把它顶走 / 输入行内容变了)。
    if (f.row >= 0) {
        ClearStreamFooterRowsAt(f.row, f.rows, info->width, info->height);
        f.row = -1;
        f.rows = 0;
    }

    if (f.hint.empty() && f.echo.empty()) {
        // 没启用 / 还没准备好文案:跟老逻辑一样,这一笔不画。
        std::cout << kSyncOutputEnd;
        std::cout.flush();
        platform::SetCursorPos(bx, by);
        return;
    }

    const std::size_t visible_queued = (std::min)(f.queued.size(), kMaxVisibleQueuedLines);
    const int queue_rows = f.queued.empty() ? 0 : 1 + static_cast<int>(visible_queued);
    const int total_rows = queue_rows + kStreamFooterBoxRows;

    // 框落在正文光标的下一行:正文停在行中(bx>0)就 by+1,正文刚换行
    // 停在行首(bx==0)就落在 by 这空行上——两种都是"正文当前底部的下一行"。
    const int target = by + (bx > 0 ? 1 : 0);
    // 框比老光杆一行高(上横线 + 输入行 + 下横线 + 状态行,共
    // kStreamFooterBoxRows 行),但绝不自己触发滚屏——理由跟老版一致:滚屏
    // 会打乱正文块(main.cpp StreamBodyTracker)的锚点账,这轮明确不碰它,
    // 也没有复用 RedrawEditArea/EnsureRoomForRows(那套是给 LineEditorCore
    // 的多行编辑区记账的,这里没有那个状态,硬凑反而画蛇添足)。没地方
    // 整框落地就这一笔不画,下一笔正文把光标推着往下挪、腾出空间时再画;
    // 而 GetScreenInfo 的 height 是缓冲区总高(含回滚,常年是几千行),这个
    // "没地方"分支实际只在贴着缓冲区真正末尾时才会触发,极少见。
    if (target < 0 || target + total_rows - 1 >= info->height) {
        std::cout << kSyncOutputEnd;
        std::cout.flush();
        platform::SetCursorPos(bx, by);
        return;
    }

    const int width = info->width;
    const BoxChrome chrome{true, &f.theme, SharedEditor().confirm_mode()};
    int box_top = target;

    if (!f.queued.empty()) {
        platform::SetCursorPos(0, target);
        std::cout << f.color << trf("input.queue_header", f.queued.size()) << f.reset;
        const std::size_t first = f.queued.size() - visible_queued;
        for (std::size_t i = 0; i < visible_queued; ++i) {
            platform::SetCursorPos(0, target + 1 + static_cast<int>(i));
            const std::string prefix = "  ↳ ";
            const int room = (std::max)(0, width - static_cast<int>(DisplayWidthUtf8(prefix)) - 1);
            std::cout << f.color << prefix
                      << TruncateUtf8ToDisplayWidth(f.queued[first + i], room) << f.reset;
        }
        box_top += queue_rows;
    }

    platform::SetCursorPos(0, box_top);
    std::cout << BoxRuleLine(f.theme, width);

    // 输入行:"> " + 已键入内容(纯文本,照真实输入的样子);空闲时 "> " +
    // 淡色占位提示,充当"这里能打字"的可发现性提示(取代老版本单独一行
    // hint)。截断跟 PrintStatusLine 一个道理——夹着 ANSI 的整行不能整个截,
    // 先截纯文本内容,颜色包在截完的文本外头。
    platform::SetCursorPos(0, box_top + 1);
    const std::string prefix = "> ";
    const int content_width = width - 1 - static_cast<int>(DisplayWidthUtf8(prefix));
    std::cout << prefix;
    if (content_width > 0) {
        if (!f.echo.empty()) {
            std::cout << TruncateUtf8ToDisplayWidth(f.echo, content_width);
        } else {
            std::cout << f.color << TruncateUtf8ToDisplayWidth(f.hint, content_width) << f.reset;
        }
    }

    platform::SetCursorPos(0, box_top + 2);
    std::cout << BoxRuleLine(f.theme, width);

    platform::SetCursorPos(0, box_top + 3);
    PrintStatusLine(chrome, width - 1);

    std::cout << kSyncOutputEnd;
    std::cout.flush();
    // 正文位置藏起来，肉眼所见的光标留在输入框。下一笔正文来时
    // EraseStreamFooterLocked 会先拨回 bx/by。
    const int typed_width = f.echo.empty() ? 0 : static_cast<int>(DisplayWidthUtf8(f.echo));
    platform::SetCursorPos((std::min)(width - 1, 2 + typed_width), box_top + 1);
    f.row = target;
    f.rows = total_rows;
    f.body_x = bx;
    f.body_y = by;
}

void BeginStreamFooter(const Theme& theme, bool enabled) {
    std::lock_guard<std::mutex> lock(StdoutWriteMutex());
    StreamFooterState& f = FooterSlot();
    f.enabled = enabled;
    f.row = -1;
    f.rows = 0;
    f.body_x = -1;
    f.body_y = -1;
    f.echo.clear();
    f.queued.clear();
    f.theme = theme;
    f.color = theme.stats;
    f.reset = theme.reset;
    f.hint = enabled ? StreamHintText(theme.reset.empty()) : std::string();
    // 开场不主动画:此刻正文还没吐,"思考中"转轮正占着这一行(跟框同一行会
    // 打架)。等第一笔正文经 OnDelta 落地,RedrawStreamFooterLocked 自然把
    // 框摆到正文下方;之后每笔正文都续着重画,一直常驻到收束。
}

void EndStreamFooter() {
    std::lock_guard<std::mutex> lock(StdoutWriteMutex());
    StreamFooterState& f = FooterSlot();
    EraseStreamFooterLocked();  // 擦掉常驻那行,免得残留在 markdown 收束重画区之外
    f.enabled = false;
    f.echo.clear();
    f.queued.clear();
    f.hint.clear();
}

// 见 console_input.hpp StreamFooterSuspendScope 的注释。构造/析构各自只在
// 临界区里拿一下 StdoutWriteMutex,不会跨整个确认交互一直攥着锁。
StreamFooterSuspendScope::StreamFooterSuspendScope() {
    std::lock_guard<std::mutex> lock(StdoutWriteMutex());
    EraseStreamFooterLocked();       // 落笔前先把框彻底擦干净(整行清,不留旧字符残留)
    FooterSlot().suspended = true;   // 挂起后续所有 RedrawStreamFooterLocked() 调用
}

StreamFooterSuspendScope::~StreamFooterSuspendScope() {
    std::lock_guard<std::mutex> lock(StdoutWriteMutex());
    FooterSlot().suspended = false;  // 摘挂起标记,不主动补画——下一笔正文/ticker 自然画回来
}

TurnInputListener::TurnInputListener(std::atomic<bool>& cancel_flag, const Theme& theme,
                                      std::atomic<bool>* transcript_expanded)
    : cancel_flag_(cancel_flag), theme_(theme), transcript_expanded_(transcript_expanded) {
    if (platform::StdinIsInteractive()) {
        enabled_ = true;
        thread_ = std::thread([this] { ThreadMain(); });
    }
}

TurnInputListener::~TurnInputListener() { Stop(); }

void TurnInputListener::Stop() {
    if (!enabled_) {
        return;
    }
    stop_requested_.store(true);
    if (thread_.joinable()) {
        thread_.join();
    }
    enabled_ = false;  // 幂等:重复调用/析构再调都不会再 join 一次已经空了的 thread_
}

std::vector<std::string> TurnInputListener::TakeQueuedLines() {
    std::lock_guard<std::mutex> lock(queue_mutex_);
    std::vector<std::string> out = std::move(queued_lines_);
    queued_lines_.clear();
    return out;
}

void TurnInputListener::ThreadMain() {
    // POSIX 下监听期间要进 termios 原始模式才能逐键拿到(Windows 的
    // ReadConsoleInputW 不用改模式,这个 scope 在那边是空操作)。
    platform::KeyListenScope listen_scope;
    // 排队缓冲按码点存(不是 UTF-8 字节),Backspace 才不会把一个多字节字符
    // 切成半个、写出乱码——跟 line_editor 那套一个思路,只是这里不需要
    // 完整编辑器的光标移动/历史/补全,故意从简。
    std::u32string buffer;
    platform::KeyReader key_reader;  // 跨事件状态(代理对配对)整条线程存活
    const bool plain = theme_.reset.empty();

    // 排队实时回显:把此刻已键入的内容塞进流式脚注的 echo 段(空就复位回
    // 提示),立刻重画一帧。footer 落在正文下方、不挪正文,故不作废块锚。
    // Windows 真控制台之外(footer.enabled 为假)这两句是空操作,退回老的
    // "不回显、只 Enter 时整条显示"。
    auto refresh_echo = [&] {
        std::lock_guard<std::mutex> stdout_lock(StdoutWriteMutex());
        FooterSlot().echo = buffer.empty() ? std::string() : StreamQueueEchoText(Utf32ToUtf8(buffer), plain);
        RedrawStreamFooterLocked();
    };

    // Esc 和单击 Ctrl+C 打断当前轮的收场动作完全一致,抽成一个共用 lambda——
    // 置 cancel_flag、擦脚注、打一行 "[已打断]"、通知正文块作废锚点、关脚注。
    auto interrupt_turn = [&] {
        cancel_flag_.store(true);
        std::lock_guard<std::mutex> stdout_lock(StdoutWriteMutex());
        EraseStreamFooterLocked();  // 先把脚注那行擦掉,[已打断] 才打得干净
        std::cout << "\n" << theme_.stats << tr("input.interrupted") << theme_.reset << "\n";
        std::cout.flush();
        if (const auto& hook = StreamScreenPrintHookSlot()) {
            hook();  // 插打了整行,正文块的行数账作废(锁还攥着,见头文件约定)
        }
        // 打断后本轮就要收场:关掉脚注,别让残余正文再把提示行重画回来。
        FooterSlot().enabled = false;
    };

    // Ctrl+C 双击退出的计时基准——只在这一次 ThreadMain() 存活期(即这一轮
    // Run() 的窗口)内有效,跨轮不留痕:TurnInputListener 是 RunTurn() 每轮
    // 现建的新实例,下一轮按键节奏重新计时,不会因为"上一轮末尾按过一次"
    // 而在下一轮开头误判成双击。
    bool has_last_ctrlc = false;
    std::chrono::steady_clock::time_point last_ctrlc_time{};
    // 双击窗口:参照 bash/Python/Node REPL 以及 Claude Code 官方文档"Ctrl+C
    // 打断、双击退出"的通用约定(经 web 检索核实,见交活报告),1200ms 内
    // 认作"这是刚才那下的续拍"。
    constexpr auto kCtrlCDoubleTapWindow = std::chrono::milliseconds(1200);

    while (!stop_requested_.load()) {
        // try_lock:抢不到就说明编辑器(ReadLineKeyByKey,含工具确认提示)
        // 正在读,乖乖让出、睡一下再抢,绝不跟前台读键盘的那次调用抢同一份
        // 控制台输入。
        std::unique_lock<std::mutex> read_lock(ConsoleReadMutex(), std::try_to_lock);
        if (!read_lock.owns_lock()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
            continue;
        }
        // WaitForKeyEvent 只是问"有没有事件可读",不消费——真正消费在
        // 下面的 ReadOne。超时就松开锁、回到循环顶部重新抢,好让出机会给
        // 这段时间可能开始的前台读取。
        if (!platform::WaitForKeyEvent(50)) {
            continue;
        }
        if (stop_requested_.load()) {
            break;
        }
        const std::optional<platform::KeyInput> key = key_reader.ReadOne();
        read_lock.unlock();  // 这一个事件读完了,处理逻辑不用再攥着锁

        if (!key.has_value()) {
            continue;  // 读失败/EOF:跟老逻辑一样跳过,循环靠 stop_requested_ 退出
        }
        using PK = platform::KeyInput::Kind;
        if (key->kind == PK::Esc) {
            interrupt_turn();
            continue;
        }
        if (key->kind == PK::CtrlC) {
            // 根因二:这里以前完全没有 PK::CtrlC 分支,按了什么反应都没有,
            // 直接被上面 "其余按键不理会" 那句注释描述的空路径吞掉——不管
            // 外层 cancel_flag 有没有别的办法置位,Ctrl+C 本身在流式期间是
            // 死键。语义对齐 bash/Python/Node REPL 和 Claude Code 官方文档
            // 确认过的通用约定:单击等同 Esc(打断当前轮,不退出程序),
            // 短时间内(kCtrlCDoubleTapWindow)连按两次才是"我要强制退出
            // 整个程序"——双击不再走 cancel_flag 那套"打断收场"流程(它可能
            // 被挂起的工具调用/子代理拖住迟迟不收场),直接 std::exit,保证
            // "用户想跑路"这条路径永远畅通。
            const auto now = std::chrono::steady_clock::now();
            const bool double_tap = has_last_ctrlc && (now - last_ctrlc_time) < kCtrlCDoubleTapWindow;
            has_last_ctrlc = true;
            last_ctrlc_time = now;
            if (double_tap) {
                {
                    std::lock_guard<std::mutex> stdout_lock(StdoutWriteMutex());
                    EraseStreamFooterLocked();
                    std::cout << "\n" << theme_.stats << tr("input.ctrlc_exit") << theme_.reset << "\n";
                    std::cout.flush();
                }  // 退出前先放锁——std::exit 会跑静态对象析构,别让它们卡死在这把锁上。
                std::exit(130);  // 130 = 128+SIGINT,"被 Ctrl+C 中断"的约定退出码
            }
            interrupt_turn();
            continue;
        }
        if (key->kind == PK::CtrlO) {
            // 根因三:回合执行期间(ThreadMain 这条监听线程活着的这段窗口)
            // 以前完全没有 CtrlO 分支,按了直接被吞——两轮之间的 composer
            // 主循环(SetTranscriptUiHandler 那条链路)才处理得了,回合跑着
            // 的时候按 Ctrl+O 没有任何反应。这里跟那边共用同一份 i18n 文案
            // (ui.expanded/ui.compact),让用户至少能看见"按了确实有反应"。
            // 取舍:这一刻已经收尾、被紧凑折叠收走的历史子工具条目不补画
            // (风险/代价配不上收益,详情见交付说明)——只影响切换那一刻
            // 之后新发生的子工具调用是否按展开画,不回溯改写屏幕上已经
            // 定格的旧条目。
            if (transcript_expanded_ != nullptr) {
                *transcript_expanded_ = !*transcript_expanded_;
                std::lock_guard<std::mutex> stdout_lock(StdoutWriteMutex());
                EraseStreamFooterLocked();
                std::cout << "\n" << theme_.stats
                          << (*transcript_expanded_ ? tr("ui.expanded") : tr("ui.compact")) << theme_.reset
                          << "\n";
                std::cout.flush();
                if (const auto& hook = StreamScreenPrintHookSlot()) {
                    hook();  // 同 Esc/Enter 分支:插了整行,正文块的行数账作废
                }
                RedrawStreamFooterLocked();
            }
            continue;
        }
        if (key->kind == PK::Enter || key->kind == PK::NewLine) {
            if (!buffer.empty()) {
                const std::string line = Utf32ToUtf8(buffer);
                {
                    std::lock_guard<std::mutex> lock(queue_mutex_);
                    queued_lines_.push_back(line);
                }
                buffer.clear();
                {
                    std::lock_guard<std::mutex> stdout_lock(StdoutWriteMutex());
                    FooterSlot().queued.push_back(line);
                    FooterSlot().echo.clear();
                    RedrawStreamFooterLocked();
                }
            }
            continue;
        }
        if (key->kind == PK::Backspace) {
            if (!buffer.empty()) {
                buffer.pop_back();
                refresh_echo();  // 退格后实时更新回显
            }
            continue;
        }
        if (key->kind == PK::Char) {
            buffer.push_back(key->ch);
            refresh_echo();  // 每敲一个字符实时回显到提示行
            continue;
        }
        // 其余按键(Tab/方向键/Ctrl 组合……)监听期间不理会,跟老逻辑一致。
    }
}

}  // namespace lubancode::cli
