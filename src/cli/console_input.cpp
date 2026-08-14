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
#include <utility>

#include "cli/divider.hpp"
#include "cli/format_utils.hpp"
#include "cli/i18n.hpp"
#include "cli/queue_model.hpp"
#include "cli/slash_commands.hpp"
#include "cli/terminal_frame.hpp"
#include "platform/console.hpp"
#include "platform/terminal_batch.hpp"

namespace lubancode::cli {

namespace {

void StripTrailingCrLf(std::string& s) {
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) {
        s.pop_back();
    }
}

// (FormatAgentPanel 已搬去 cli/agent_panel.cpp 的 LayoutAgentPanel:面板从
// hint_lines 挪到输入框上方后,布局/窗口/状态机全在那边的纯逻辑层。)

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

// 流式输入行 slash 提示的候选表:跟 SharedEditor 同一份来源(AllSlashCommands
// 转候选),但单独建一份静态表——监听线程只拿它喂纯函数
// StreamSlashHintLines 现算提示行,不碰 SharedEditor 的编辑状态/历史,两边的
// 键盘路径互不知晓。命令清单仍只有 slash_commands 那一份,不抄第二遍。
const std::vector<CompletionCandidate>& StreamHintCandidates() {
    static const std::vector<CompletionCandidate> candidates = [] {
        std::vector<CompletionCandidate> out;
        for (const auto& cmd : AllSlashCommands()) {
            out.push_back(CompletionCandidate{cmd.name, cmd.description});
        }
        return out;
    }();
    return candidates;
}

// UI-D(0.16.0):会话级 UI 按键回调(Ctrl+O/Ctrl+E/焦点导航)。存这儿、
// 由 SetTranscriptUiHandler 装卸;ReadLineKeyByKey 只在 composer 读取里查它。
TranscriptUiHandler& UiHandlerSlot() {
    static TranscriptUiHandler handler;
    return handler;
}

AgentPanelProvider& AgentPanelProviderSlot() {
    static AgentPanelProvider provider;
    return provider;
}

// 详情按需取的槽(查看态打开的那只才被调,见 agent_panel.hpp)。
std::function<std::vector<std::string>(int)>& AgentPanelDetailSlot() {
    static std::function<std::vector<std::string>(int)> provider;
    return provider;
}

AgentPanelActions& AgentPanelActionsSlot() {
    static AgentPanelActions actions;
    return actions;
}

// composer 这一次读取的收件目标(查看态那只子代理的任务号)。0.28.x 起
// 流式监听线程落队时也要读它(排队的消息要带目标),读写都过一把小锁
// ——两个线程碰同一份 optional,不能光靠"通常错不开"的运气。
std::optional<int>& ComposerTargetSlot() {
    static std::optional<int> target;
    return target;
}
std::mutex& ComposerTargetMutex() {
    static std::mutex m;
    return m;
}
void SetComposerTarget(std::optional<int> target) {
    std::lock_guard<std::mutex> lock(ComposerTargetMutex());
    ComposerTargetSlot() = std::move(target);
}
std::optional<int> GetComposerTarget() {
    std::lock_guard<std::mutex> lock(ComposerTargetMutex());
    return ComposerTargetSlot();
}

// 空闲唤醒钩子的存取点(跟 AgentPanelProvider 同一套会话级静态槽)。只在
// ReadLineKeyByKey 的 100ms 面板刷新一拍里被读,主线程独占,不用加锁。
std::function<bool()>& IdleWakeHookSlot() {
    static std::function<bool()> hook;
    return hook;
}

// 键位缝:platform 语义按键 -> 面板动作 id(PanelKey)。面板键位要换/要接
// keymap,只动这一张小表;状态机与布局都在 cli/agent_panel(纯逻辑,单测钉)。
std::optional<PanelKey> MapToPanelKey(const platform::KeyInput& key) {
    using PK = platform::KeyInput::Kind;
    switch (key.kind) {
        case PK::Up:
            return PanelKey::Up;
        case PK::Down:
            return PanelKey::Down;
        case PK::Enter:
            return PanelKey::EnterView;  // NewLine(Shift/Alt+Enter)不进面板,照旧插换行
        case PK::Esc:
            return PanelKey::Esc;
        case PK::CtrlX:
            return PanelKey::StopAllArm;
        case PK::CtrlK:
            return PanelKey::StopAllConfirm;
        case PK::Char:
            if (key.ch == U'x' || key.ch == U'X') {
                return PanelKey::StopEntry;
            }
            return std::nullopt;  // 其余字母只进 composer
        default:
            return std::nullopt;
    }
}

// 0.17.0:常驻状态行数据。InteractiveLoop 每圈整份重建(SetStatusLineData,
// 主线程,圈边界没有别的线程碰它,不用加锁);回合内 on_usage 局部更新
// (UpdateStatusLineContext,自己拿 StdoutWriteMutex——流式期间 footer 由
// ticker/监听线程在同一把锁内重画读这份表)。BuildStatusLine 的读取:空闲
// composer 路径只在主线程(此时 worker 线程都已收掉);footer 路径在
// StdoutWriteMutex 内,与局部更新天然互斥。
struct StatusLineData {
    StatusPanelData values;
    std::vector<std::string> items{
        "permission_mode", "model", "cwd", "git_branch", "context", "tokens"};
    std::string separator = " · ";
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
    // 0.28.x:队列区不再在 footer 里存副本——每帧重画时现拉
    // SessionSteeringQueue() 的轻量快照(锁内拷贝、用完即放),工具边界
    // 送达/打断收场动了账,下一帧自然对上,不会挂着旧条目。
    std::vector<std::string> hints;   // 流式输入行的 slash 命令提示行(画在状态行
                                      // 之下,跟空闲 composer 同一层级),空 = 没有提示
    bool working = false;             // true 时在输入框上方合成 Working 动画
    std::string working_label;
    std::size_t working_highlight = 0;
    long long working_seconds = 0;
    int row = -1;             // 整块 footer 顶行,-1 = 没画
    int rows = 0;             // 上次实际画了几行(队列区会让高度变化)
    int body_x = -1;          // footer 下方藏着的正文续写位置
    int body_y = -1;
    // 两枚深度都只在 StdoutWriteMutex 内改。确认/ask_user 可嵌套挂起；
    // 工具条目重画也可套着状态块重画。任何一层尚未退完，Redraw 都只记
    // 状态不落笔，最外层析构负责补回完整帧。
    int suspend_depth = 0;
    int paint_depth = 0;
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

std::vector<std::string> FooterUtf8Glyphs(const std::string& text) {
    std::vector<std::string> out;
    for (std::size_t i = 0; i < text.size();) {
        const unsigned char lead = static_cast<unsigned char>(text[i]);
        std::size_t bytes = 1;
        if ((lead & 0xE0U) == 0xC0U) {
            bytes = 2;
        } else if ((lead & 0xF0U) == 0xE0U) {
            bytes = 3;
        } else if ((lead & 0xF8U) == 0xF0U) {
            bytes = 4;
        }
        bytes = (std::min)(bytes, text.size() - i);
        out.push_back(text.substr(i, bytes));
        i += bytes;
    }
    return out;
}

std::string BuildFooterWorkingLine(const StreamFooterState& f, int width) {
    const std::vector<std::string> glyphs = FooterUtf8Glyphs(f.working_label);
    const std::string prefix = "• ";
    const std::string suffix = " (" + std::to_string(f.working_seconds) + "s · " +
                               tr("spinner.interrupt_hint") + ")";
    const int prefix_width = static_cast<int>(DisplayWidthUtf8(prefix));
    const int suffix_width = static_cast<int>(DisplayWidthUtf8(suffix));
    const int label_room = (std::max)(0, width - 1 - prefix_width - suffix_width);

    std::string line = f.theme.spinner + prefix + f.reset;
    int used = 0;
    for (std::size_t i = 0; i < glyphs.size(); ++i) {
        const int glyph_width = static_cast<int>(DisplayWidthUtf8(glyphs[i]));
        if (used + glyph_width > label_room) {
            break;
        }
        const bool highlighted = !f.reset.empty() && i == f.working_highlight % glyphs.size();
        line += highlighted ? f.theme.spinner : f.theme.stats;
        line += glyphs[i];
        used += glyph_width;
    }
    if (prefix_width + used + suffix_width < width) {
        line += f.theme.stats + suffix;
    }
    line += f.reset;
    return line;
}

void PrintFooterWorkingLine(const StreamFooterState& f, int width) {
    std::cout << BuildFooterWorkingLine(f, width);
}

// M10:谁在真的逐键读键盘,谁就得先拿到这把锁——ReadLineKeyByKey()/
// ReadChoiceMenu() 整个调用期间(从进函数到返回)一直攥着它,
// TurnInputListener 的监听线程只在抢到锁的间隙才读一次。这样"监听只活在
// 编辑器/菜单不在读的窗口期"这条要求不用靠回调层层传参去手动维护,两边
// 天然靠锁互斥错开——工具确认提示 [y/a/N] 与 ask_user 选择菜单走的也是
// 这条路,天然一并受益,不用另外接管。定义在下面公共区(声明在
// console_input.hpp),规约由 tests/test_repaint_coord.cpp 钉死。

// platform 层的语义按键 -> 核心层 KeyEvent。两个枚举一一平行(platform 不
// 依赖 cli,镜像了一份),这里只是搬运;None 翻成 nullopt,调用方 continue。
std::optional<KeyEvent> MapKey(const platform::KeyInput& key) {
    using PK = platform::KeyInput::Kind;
    switch (key.kind) {
        case PK::None:
            return std::nullopt;
        case PK::Char:
            return KeyEvent::Char(key.ch);
        case PK::Paste:
            return KeyEvent::Paste(key.text, key.replace_before);
        case PK::Backspace:
            return KeyEvent::Simple(KeyKind::Backspace);
        case PK::Left:
            return KeyEvent::Simple(KeyKind::Left);
        case PK::ShiftLeft:
        case PK::CtrlLeft:
            // 取回键的缺省语义就是普通光标移动(编辑器没有按词选择)。真正的
            // "取回排队消息"由 ReadLineKeyByKey/TurnInputListener 在喂编辑器
            // 之前拦(ShouldRecallQueuedMessage 那一套),拦不到的场合——正文
            // 非空、队列为空、确认提示这类单行读取——行为与升级前的 Left 分毫不差。
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
        case PK::CtrlX:
        case PK::CtrlK:
            // 面板两段确认键:只在 ReadLineKeyByKey 的面板缝里消费,不该进
            // 编辑器(核心层也没有这两个语义)。
            return std::nullopt;
        case PK::Esc:
            return KeyEvent::Simple(KeyKind::Esc);
        case PK::Delete:
            return KeyEvent::Simple(KeyKind::Delete);
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
std::string BuildStatusLine(const BoxChrome& chrome, int max_width) {
    const Theme& theme = *chrome.theme;
    const StatusLineData& data = StatusDataSlot();
    auto segments = BuildStatusPanelSegments(data.items, chrome.mode, data.values);

    // 宽度不够时先从左边收工作目录，保住项目末级目录与它后面的分支；
    // 还不够才按用户给的字段顺序从行尾截。
    int total_width = 0;
    for (std::size_t i = 0; i < segments.size(); ++i) {
        if (i > 0) {
            total_width += static_cast<int>(DisplayWidthUtf8(data.separator));
        }
        total_width += static_cast<int>(DisplayWidthUtf8(segments[i].text));
    }
    if (total_width > max_width) {
        for (auto& segment : segments) {
            if (segment.key != "cwd") {
                continue;
            }
            const int old_width = static_cast<int>(DisplayWidthUtf8(segment.text));
            const int room = (std::max)(6, old_width - (total_width - max_width));
            segment.text = CompactStatusPath(segment.text, room);
            break;
        }
    }

    int remaining = max_width;
    bool emitted = false;
    std::string line;
    for (const auto& segment : segments) {
        if (remaining <= 0) {
            break;
        }
        if (emitted) {
            const std::string separator = TruncateUtf8ToDisplayWidth(data.separator, remaining);
            line += theme.stats + separator + theme.reset;
            remaining -= static_cast<int>(DisplayWidthUtf8(separator));
            if (remaining <= 0) {
                break;
            }
        }
        const std::string text = TruncateUtf8ToDisplayWidth(segment.text, remaining);
        std::string color = theme.stats;
        if (segment.key == "permission_mode") {
            if (chrome.mode == ConfirmMode::Confirm) {
                color.clear();
            } else if (chrome.mode == ConfirmMode::Yolo) {
                color = theme.error;
            }
        } else if (segment.key == "model") {
            color = theme.tool_line;
        } else if (segment.key == "cwd") {
            color = theme.prompt;
        } else if (segment.key == "git_branch") {
            color = theme.banner;
        }
        line += color + text + (color.empty() ? std::string() : theme.reset);
        remaining -= static_cast<int>(DisplayWidthUtf8(text));
        emitted = true;
    }
    return line;
}

void PrintStatusLine(const BoxChrome& chrome, int max_width) {
    std::cout << BuildStatusLine(chrome, max_width);
}

void PaintInlineFrameLegacy(const InlineFrame* previous, const InlineFrame& next,
                            int origin_y) {
    const std::size_t old_size = previous == nullptr ? 0 : previous->rows.size();
    const std::size_t row_count = (std::max)(old_size, next.rows.size());
    for (std::size_t i = 0; i < row_count; ++i) {
        const InlineFrameRow* old_row = i < old_size ? &previous->rows[i] : nullptr;
        const InlineFrameRow* new_row = i < next.rows.size() ? &next.rows[i] : nullptr;
        if (old_row != nullptr && new_row != nullptr && *old_row == *new_row) {
            continue;
        }
        int x = new_row != nullptr ? new_row->x : old_row->x;
        int end = x + (new_row != nullptr ? new_row->clear_width : old_row->clear_width);
        if (old_row != nullptr) {
            x = (std::min)(x, old_row->x);
            end = (std::max)(end, old_row->x + old_row->clear_width);
        }
        const bool hard = (new_row != nullptr && new_row->hard_clear) ||
                          (old_row != nullptr && old_row->hard_clear);
        if (hard) {
            platform::ClearRowHardFrom(x, origin_y + static_cast<int>(i), end - x);
        } else {
            platform::ClearRowFrom(x, origin_y + static_cast<int>(i), end - x);
        }
        if (new_row != nullptr && !new_row->text.empty()) {
            platform::SetCursorPos(new_row->x, origin_y + static_cast<int>(i));
            std::cout << new_row->text;
            std::cout.flush();
        }
    }
    platform::SetCursorPos(next.cursor_x, origin_y + next.cursor_row);
}

// composer 先排成物理行，再拿新旧两帧逐行比较。提示符后的第一行窄些，
// 续行留两格缩进；正文软换行，不再水平滚。VT 终端把擦行、落字、归光标
// 攒进一段字节，一次 write + flush。老终端仍走 Console API 兼容路。
//
// 0.28.x:编辑帧从"提示符行 + 下面的框"扩成整帧记账——代理面板
// (rows_above)在上横线之上,上横线(带查看态的右端短标签 rule_tag)进帧,
// 下横线/状态行/普通 slash 提示在下。锚点 start_row 仍是 composer 首行
// (提示符行),帧顶 = start_row - rows_above - 1 由这里推;上一帧的绝对
// 帧顶记在 prev_frame_origin,面板增减/终端缩放/滚屏挪了位就对不上,整帧
// 重画。提示符并进首行文本(x=0),不再靠进函数前那一次 std::cout 存活。
void RedrawEditArea(int& start_row, int& prompt_end_col, const std::string& prompt,
                    const RenderState& state, const std::vector<std::string>& rows_above,
                    const std::string& rule_tag, int& prev_body_row_count,
                    std::optional<InlineFrame>& previous_frame, int& prev_frame_origin,
                    bool vt_enabled, const BoxChrome& chrome = BoxChrome{}) {
    const std::optional<platform::ScreenInfo> info = platform::GetScreenInfo();
    if (!info.has_value()) {
        return;  // 拿不到屏幕信息就没法定位,这一帧放弃重画,下一帧再试
    }
    const int buffer_width = info->width;
    const int buffer_height = info->height;

    constexpr int kContinuationIndent = 2;
    const std::vector<std::u32string> fallback_lines{std::u32string()};
    const std::vector<std::u32string>& edit_lines = state.lines.empty() ? fallback_lines : state.lines;
    const WrappedComposerLayout layout = LayoutComposerRows(
        edit_lines, state.cursor_row, state.cursor_col,
        buffer_width - prompt_end_col - 1, buffer_width - kContinuationIndent - 1);

    // 上方行(代理面板)从头上收:锚点以上放不下(光标贴着缓冲区顶)就少
    // 摆几行,保住横线与 composer。极端情形在真机上几乎碰不着,兜底而已。
    const bool with_rule = chrome.enabled;
    int above_count = static_cast<int>(rows_above.size());
    if (above_count + (with_rule ? 1 : 0) > start_row) {
        above_count = std::max(0, start_row - (with_rule ? 1 : 0));
    }
    const int first_above = static_cast<int>(rows_above.size()) - above_count;
    int frame_top = start_row - above_count - (with_rule ? 1 : 0);
    if (frame_top < 0) {
        frame_top = 0;  // 理论到不了(box 模式提示符行上方至少有一行),防越界
    }

    InlineFrame next;
    next.rows.reserve(above_count + layout.rows.size() + (chrome.enabled ? 3U : 0U) +
                      state.hint_lines.size());
    for (int i = first_above; i < static_cast<int>(rows_above.size()); ++i) {
        next.rows.push_back(InlineFrameRow{
            0, buffer_width, false, TruncateUtf8ToDisplayWidth(rows_above[i], buffer_width - 1)});
    }
    if (with_rule) {
        const std::string rule =
            rule_tag.empty()
                ? BoxRuleLine(*chrome.theme, buffer_width)
                : BuildRuleWithTag(chrome.theme->stats, chrome.theme->reset, rule_tag, buffer_width);
        next.rows.push_back(InlineFrameRow{0, buffer_width, true, rule});
    }
    for (std::size_t i = 0; i < layout.rows.size(); ++i) {
        const bool first = i == 0;
        InlineFrameRow row;
        row.x = first ? 0 : kContinuationIndent;
        row.clear_width = buffer_width - row.x;
        row.text = first ? prompt + Utf32ToUtf8(layout.rows[i].text)
                         : std::string(kContinuationIndent, ' ') + Utf32ToUtf8(layout.rows[i].text);
        next.rows.push_back(std::move(row));
    }

    if (chrome.enabled) {
        next.rows.push_back(InlineFrameRow{0, buffer_width, true,
                                           BoxRuleLine(*chrome.theme, buffer_width)});
        next.rows.push_back(InlineFrameRow{0, buffer_width, true,
                                           BuildStatusLine(chrome, buffer_width - 1)});
    }
    for (const auto& hint : state.hint_lines) {
        next.rows.push_back(InlineFrameRow{
            0, buffer_width, false, TruncateUtf8ToDisplayWidth(hint, buffer_width - 1)});
    }

    const int above_total = above_count + (with_rule ? 1 : 0);
    next.cursor_x = layout.cursor_row == 0
                        ? prompt_end_col + layout.cursor_col
                        : kContinuationIndent + layout.cursor_col;
    next.cursor_row = above_total + static_cast<int>(layout.cursor_row);

    const int hint_count = static_cast<int>(state.hint_lines.size());
    const int box_rows = chrome.enabled ? 2 : 0;
    const int body_rows = static_cast<int>(layout.rows.size()) - 1 + box_rows + hint_count + above_total;
    const int rows_to_touch = std::max(body_rows, prev_body_row_count);
    const int total_rows = std::max(1, static_cast<int>(next.rows.size()));
    int frame_origin = frame_top;
    EnsureRoomForRows(frame_origin, buffer_height, std::max(total_rows, 1 + rows_to_touch));
    if (frame_origin != frame_top) {
        // 探底滚屏:帧顶与锚点(提示符行)一起上移,账目对平。
        const int delta = frame_origin - frame_top;
        start_row += delta;
        frame_top = frame_origin;
    }
    int viewport_x = info->viewport_x;
    int viewport_y = info->viewport_y;
    if (const auto after_scroll = platform::GetScreenInfo(); after_scroll.has_value()) {
        viewport_x = after_scroll->viewport_x;
        viewport_y = after_scroll->viewport_y;
    }

    // 上一帧的绝对帧顶对不上(面板增减/滚屏/换锚)就不比了,整帧重画。
    const InlineFrame* previous =
        previous_frame.has_value() && prev_frame_origin == frame_top ? &*previous_frame : nullptr;
    if (vt_enabled) {
        platform::TerminalBatch batch(viewport_x, viewport_y);
        QueueInlineFrameDiff(batch, previous, next, frame_top);
        batch.Flush();
    } else {
        PaintInlineFrameLegacy(previous, next, frame_top);
    }
    previous_frame = std::move(next);
    prev_frame_origin = frame_top;
    prev_body_row_count = body_rows;
}

// 0.17.0 输入框化的提交收尾:横线擦掉、提交行保留。取舍:两个方案里选了
// "只留 `> 内容`"这条——框连横线留在滚动历史的话,每一问上下各一根 100 列
// 横线,再加 RunTurn 紧接着打的输入/输出分界线,三根线叠一块,滚动历史
// 全是线;擦掉横线后历史里就是干干净净的 `> 问题` + 分界线 + 回答,跟
// 0.16.0 的历史观感一致,框只属于"正在输入"这一刻。
//
// 做法:把整个框(0.28.x 起含上横线之上的代理面板行,帧顶即 frame_top =
// prev_frame_origin)统统清掉,从帧顶起重打提交内容(`> ` 第一行、两空格
// 续行,每个逻辑行照 composer 的宽度铺成物理行),光标停在末行末尾,调用
// 方接着换行。面板行随提交收走,滚动历史里只留干净的问题,代理的最终
// 结论照旧走 transcript,不在面板里留残骸。
void CollapseBoxOnSubmit(int frame_top, int prompt_width, int prev_body_row_count,
                         const RenderState& state, const std::string& prompt, bool vt_enabled) {
    const std::optional<platform::ScreenInfo> info = platform::GetScreenInfo();
    if (!info.has_value()) {
        return;  // 拿不到屏幕信息就不收尾了,提交帧画面已经在屏上,不算坏
    }
    const int buffer_width = info->width;
    const int top = frame_top > 0 ? frame_top : 0;
    int bottom = frame_top + prev_body_row_count;
    if (bottom >= info->height) {
        bottom = info->height - 1;
    }
    constexpr int kContinuationIndent = 2;
    const WrappedComposerLayout layout = LayoutComposerRows(
        state.lines, state.cursor_row, state.cursor_col, buffer_width - prompt_width - 1,
        buffer_width - kContinuationIndent - 1);
    const auto row_text = [&](std::size_t i) {
        return i == 0 ? prompt + Utf32ToUtf8(layout.rows[i].text)
                      : std::string(kContinuationIndent, ' ') + Utf32ToUtf8(layout.rows[i].text);
    };
    const std::size_t last = layout.rows.empty() ? 0 : layout.rows.size() - 1;
    const int final_x = (last == 0 ? prompt_width : kContinuationIndent) +
                        (layout.rows.empty() ? 0 : layout.rows[last].display_width);
    const int final_y = top + static_cast<int>(last);

    if (vt_enabled) {
        platform::TerminalBatch batch(info->viewport_x, info->viewport_y);
        for (int r = top; r <= bottom; ++r) {
            batch.ClearRowHardFrom(0, r, buffer_width);
        }
        for (std::size_t i = 0; i < layout.rows.size(); ++i) {
            batch.MoveTo(0, top + static_cast<int>(i));
            batch.Write(row_text(i));
        }
        batch.MoveTo(final_x, final_y);
        batch.Flush();
        return;
    }

    for (int r = top; r <= bottom; ++r) {
        platform::ClearRowHardFrom(0, r, buffer_width);
    }
    for (std::size_t i = 0; i < layout.rows.size(); ++i) {
        platform::SetCursorPos(0, top + static_cast<int>(i));
        std::cout << row_text(i);
        std::cout.flush();
    }
    platform::SetCursorPos(final_x, final_y);
}

// 逐键读入这一次输入(UI-A 起,composer 模式下可能是多行),真控制台专用。
// platform::KeyReader 逐个键盘事件读、翻成语义按键,再映射成 cli::KeyEvent
// 喂 SharedEditor(),按吐出来的 RenderState 重画。
//
// esc_rejects/composer:见 console_input.hpp 里 ReadLine() 的同名参数注释。
class BracketedPasteScope {
public:
    explicit BracketedPasteScope(bool enabled) : enabled_(enabled) {
        if (enabled_) {
            std::cout << "\x1b[?2004h" << std::flush;
        }
    }
    ~BracketedPasteScope() {
        if (enabled_) {
            std::cout << "\x1b[?2004l" << std::flush;
        }
    }

private:
    bool enabled_;
};

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
    BracketedPasteScope paste_scope(composer && !theme.reset.empty());

    LineEditorCore& editor = SharedEditor();
    editor.BeginLine(composer);

    // 0.17.0:composer 读取开输入框(上横线 + `> ` 输入行 + 下横线 + 状态
    // 行)。0.28.x 起上横线之上还有代理面板(整帧记账,见 RedrawEditArea)。
    const bool box = composer;
    BoxChrome chrome{box, &theme, editor.confirm_mode()};
    // 面板预留:开框前锚点上方至少留出"面板预算 + 框线"的行数,不够就先
    // 换行把提示符压下去(缓冲区没到底时只是挪光标,不滚内容;到底了自然
    // 滚走旧 transcript,输入框仍钉在视口下部)。新会话开场光标贴着缓冲区
    // 顶,没有这一步,面板只能开个两三条的小窗。
    const auto reserve_panel_room = [&]() {
        if (!box) {
            return;
        }
        const std::optional<platform::ScreenInfo> now = platform::GetScreenInfo();
        if (!now.has_value()) {
            return;
        }
        const int entry_cap = std::max(3, std::min(now->height / 2 - 1, 16));
        const int wanted = entry_cap + 5;  // 提示/计数/上下横线/详情几行
        if (now->cursor_y < wanted && wanted < now->height - 1) {
            for (int i = 0; i < wanted - now->cursor_y; ++i) {
                std::cout << "\n";
            }
            std::cout.flush();
        }
    };
    reserve_panel_room();
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
    std::optional<InlineFrame> previous_frame;
    const bool vt_enabled = platform::ProbeStdoutConsole().vt_enabled;

    platform::KeyReader key_reader;
    // 面板状态机(纯逻辑,键位缝在 MapToPanelKey:选择/查看/x 停止清除/
    // Ctrl+X Ctrl+K 两段确认全在里面,单测钉在 tests/test_agent_panel.cpp)。
    AgentPanelController panel_controller;
    std::string panel_fingerprint;  // 上一帧面板指纹(条目+状态机+成行),变了才重画
    std::string panel_notice;       // 停全部的回执,挂在提示行下面两秒就收
    std::chrono::steady_clock::time_point panel_notice_until{};
    int prev_frame_origin = -1;  // 上一帧的绝对帧顶;面板增减/滚屏后对不上就整帧重画

    if (composer) {
        SetComposerTarget(std::nullopt);  // 每次读取开始,收件目标先归 main
    }

    auto panel_entries = [&]() -> std::vector<AgentPanelEntry> {
        if (!composer || !AgentPanelProviderSlot()) {
            return {};
        }
        return AgentPanelProviderSlot()();
    };

    // 面板帧:窗口化布局(纯函数)+ 查看态详情(按需取,只有打开的那只才
    // 拉)+ 输入框上横线右端短标签 + composer 收件目标。面板画在输入框
    // 上方(rows_above),不再借 hint_lines 落脚。
    const auto build_panel = [&](const std::vector<AgentPanelEntry>& entries,
                                 std::string& tag_out) -> std::vector<std::string> {
        const int total = static_cast<int>(entries.size()) + 1;
        panel_controller.OnEntriesChanged(total);
        if (entries.empty()) {
            SetComposerTarget(std::nullopt);
            tag_out.clear();
            return {};
        }
        // 查看态目标条目被清掉(x 清除/任务全没了):收起查看态,标签摘掉。
        if (const auto target = panel_controller.target_index();
            target.has_value() && *target >= total) {
            panel_controller.CloseView();
        }
        std::vector<std::string> detail_lines;
        if (panel_controller.detail_open() && panel_controller.selected() > 0 && AgentPanelDetailSlot()) {
            detail_lines = AgentPanelDetailSlot()(entries[static_cast<std::size_t>(
                                                      panel_controller.selected()) - 1]
                                                      .task_id);
        }
        // 窗口预算有两道:条目数至多半屏上下(下限 3 条,上限 16 条);整块
        // 面板(提示/计数/条目/详情)不得越过锚点上方的空间——空间不够就在
        // 布局函数里开窗/截详情,首行操作提示永不丢。宽度用进函数时的屏幕
        // 信息,RedrawEditArea 还会按当前屏宽再截一道,缩放不会裂边。
        int max_entries = 0;
        if (info.has_value()) {
            max_entries = std::max(3, std::min(info->height / 2 - 1, 16));
        }
        const int rows_above_budget = std::max(0, start_row - 1);
        auto layout = LayoutAgentPanel(entries, panel_controller.selected(), panel_controller.focused(),
                                       panel_controller.detail_open(), detail_lines, max_entries,
                                       rows_above_budget, info.has_value() ? info->width : 80,
                                       panel_controller.stop_all_armed());
        tag_out.clear();
        if (const auto target = panel_controller.target_index(); target.has_value()) {
            const AgentPanelEntry& entry = entries[static_cast<std::size_t>(*target) - 1];
            SetComposerTarget(entry.task_id);  // composer 此刻以它为收件人
            tag_out = entry.description;           // 短标题取任务短述,不取代理类型
        } else {
            SetComposerTarget(std::nullopt);
        }
        if (!panel_notice.empty()) {
            if (std::chrono::steady_clock::now() < panel_notice_until) {
                layout.lines.insert(layout.lines.begin() + 1, panel_notice);
            } else {
                panel_notice.clear();
            }
        }
        return layout.lines;
    };
    const auto fingerprint_of = [](const std::vector<AgentPanelEntry>& entries,
                                   const std::vector<std::string>& above,
                                   const AgentPanelController& controller) {
        std::string value;
        for (const auto& entry : entries) {
            value += std::to_string(entry.task_id) + "\x1f" + entry.name + "\x1f" + entry.description +
                     "\x1f" + entry.state + "\x1f" + (entry.running ? "R" : entry.failed ? "F" : "D") +
                     "\n";
        }
        value += "#" + std::to_string(controller.selected()) + (controller.focused() ? "f" : "-") +
                 (controller.detail_open() ? "d" : "-") + (controller.stop_all_armed() ? "a" : "-") + "\n";
        for (const auto& line : above) {
            value += line + "\n";
        }
        return value;
    };
    // 0.28.x 排队消息区:画在代理面板之下、composer 上横线之上(规格"显示"
    // 节的次序)。空队列连标题都不画;标题随状态变(编辑中/本轮收尾后送出)。
    // 空闲时没有工具边界可等,标题写"本轮收尾后送出"。
    SteeringQueue& steering = SessionSteeringQueue();
    std::optional<SteeringQueue::EditHandle> queue_edit;  // 正在取回编辑的凭据
    bool queue_delete_armed = false;
    std::chrono::steady_clock::time_point queue_delete_armed_until{};
    const auto queue_rows_now = [&]() -> std::vector<std::string> {
        const auto snapshot = steering.Snapshot();
        if (snapshot.empty()) {
            return {};
        }
        QueueViewOptions view;
        view.visible_cap = kMaxVisibleQueuedLines;
        view.title_mode = steering.immediate_delivery_requested() ? QueueTitleMode::Immediate
                              : queue_edit.has_value() ? QueueTitleMode::Editing
                                                       : QueueTitleMode::EndOfTurn;
        return BuildSteeringQueueRows(snapshot, view);
    };

    auto redraw_with_panel = [&](const RenderState& state, const std::vector<AgentPanelEntry>& entries) {
        std::string tag;
        std::vector<std::string> above = build_panel(entries, tag);
        const std::vector<std::string> queue_rows = queue_rows_now();
        above.insert(above.end(), queue_rows.begin(), queue_rows.end());
        chrome.mode = editor.confirm_mode();
        RedrawEditArea(start_row, prompt_end_col, prompt, state, above, tag, prev_body_row_count,
                       previous_frame, prev_frame_origin, vt_enabled, chrome);
        panel_fingerprint = fingerprint_of(entries, above, panel_controller);
    };

    if (box) {
        redraw_with_panel(editor.CurrentRenderState(), panel_entries());
    }

    while (true) {
        // ReadOne 原本会一直堵到下一枚按键，后台代理即便完成，面板也只会
        // 在用户敲键后才变。100ms 探一次队列；没键就只重画这一帧。
        if (!platform::WaitForKeyEvent(100)) {
            const auto entries = panel_entries();
            const bool armed_expired = panel_controller.ExpireArmed(std::chrono::steady_clock::now());
            std::string tag;
            const std::vector<std::string> above = build_panel(entries, tag);
            if (armed_expired || fingerprint_of(entries, above, panel_controller) != panel_fingerprint) {
                redraw_with_panel(editor.CurrentRenderState(), entries);
            }
            // 空闲唤醒:系统侧有事件(后台子代理跑完等)要在会话空闲时处理。
            // composer 空着才让位——用户敲了一半的正文不抢;空串返回,调用方
            // 循环顶自会去办,办完回来重新给提示符。
            if (composer && editor.CurrentRenderState().line.empty()) {
                const auto& wake = IdleWakeHookSlot();
                if (wake && wake()) {
                    if (queue_edit.has_value()) {
                        steering.CancelEdit(*queue_edit);
                        queue_edit.reset();
                    }
                    if (prev_body_row_count > 0) {
                        platform::SetCursorPos(0, start_row + prev_body_row_count);
                    }
                    std::cout << "\n";
                    return std::string();
                }
            }
            continue;
        }
        const std::optional<platform::KeyInput> raw_key = key_reader.ReadOne();
        if (!raw_key.has_value()) {
            if (queue_edit.has_value()) {
                steering.CancelEdit(*queue_edit);
                queue_edit.reset();
            }
            return std::nullopt;  // EOF/读失败
        }
        const auto entries_before_key = panel_entries();

        // ---- 0.28.x 排队消息的取回/编辑(只作用于 composer 读取) ----
        // 取回键:正文空、非编辑态、队列有可取条目,三者齐备才取最新一条
        // (ShouldRecallQueuedMessage 钉规矩);正文非空时 Shift+Left 仍是
        // composer 的光标键,不抢。
        if (composer && IsQueueRecallKey(raw_key->kind) &&
            ShouldRecallQueuedMessage(editor.CurrentRenderState().line.empty(), queue_edit.has_value(),
                                      steering.editable_size())) {
            if (auto handle = steering.BeginEditLatest(); handle.has_value()) {
                queue_edit = std::move(*handle);
                editor.LoadText(Utf8ToUtf32(queue_edit->text));
                queue_delete_armed = false;
                redraw_with_panel(editor.CurrentRenderState(), entries_before_key);
            }
            continue;
        }
        if (composer && queue_edit.has_value()) {
            using PK = platform::KeyInput::Kind;
            const auto finish_edit_clear = [&]() {
                queue_edit.reset();
                queue_delete_armed = false;
                editor.BeginLine(/*composer=*/true);
            };
            const auto redraw_queue_frame = [&]() {
                redraw_with_panel(editor.CurrentRenderState(), panel_entries());
            };
            if (raw_key->kind == PK::Enter) {
                // 原位替换:保 id、目标、排队次序;版本冲突 = 那条已变动,
                // 提示并保留正文(下一次 Enter 就是普通提交,走会话主路)。
                const auto status = steering.CommitEdit(*queue_edit, Utf32ToUtf8(editor.CurrentRenderState().line));
                if (status == SteeringQueue::CommitStatus::Ok) {
                    finish_edit_clear();
                } else {
                    queue_edit.reset();
                    queue_delete_armed = false;
                    panel_notice = tr("queue.commit_conflict");
                    panel_notice_until = std::chrono::steady_clock::now() + std::chrono::seconds(3);
                }
                redraw_queue_frame();
                continue;
            }
            if (raw_key->kind == PK::NewLine) {
                editor.HandleKey(KeyEvent::Simple(KeyKind::NewLine));
                redraw_queue_frame();
                continue;
            }
            if (raw_key->kind == PK::Esc || raw_key->kind == PK::CtrlC) {
                // 第一层 Esc:只取消编辑、还原原文,不打断任何东西。
                steering.CancelEdit(*queue_edit);
                finish_edit_clear();
                redraw_queue_frame();
                continue;
            }
            if (raw_key->kind == PK::Delete) {
                // 两段删除:第一下亮提示(编辑态标题自带"Del 再按一次删除"),
                // 第二下 2 秒窗口内真删。
                const auto now = std::chrono::steady_clock::now();
                if (queue_delete_armed && now < queue_delete_armed_until) {
                    steering.DeleteMessage(*queue_edit);
                    finish_edit_clear();
                } else {
                    queue_delete_armed = true;
                    queue_delete_armed_until = now + std::chrono::seconds(2);
                }
                redraw_queue_frame();
                continue;
            }
            if (raw_key->kind == PK::Up || raw_key->kind == PK::Down) {
                // 在队列条目间走:先把改到一半的正文写回当前位,再取相邻。
                const bool up = raw_key->kind == PK::Up;
                const auto snapshot = steering.Snapshot();
                std::size_t index = 0;
                for (std::size_t i = 0; i < snapshot.size(); ++i) {
                    if (snapshot[i].id == queue_edit->id) {
                        index = i;
                    }
                }
                const auto status =
                    steering.CommitEdit(*queue_edit, Utf32ToUtf8(editor.CurrentRenderState().line));
                if (status == SteeringQueue::CommitStatus::Ok) {
                    finish_edit_clear();
                } else {
                    queue_edit.reset();
                    queue_delete_armed = false;
                }
                const bool go_prev = up && index > 0;
                const bool go_next = !up && index + 1 < snapshot.size();
                if (go_prev || go_next) {
                    const std::size_t next = go_prev ? index - 1 : index + 1;
                    if (auto handle = steering.BeginEdit(snapshot[next].id); handle.has_value()) {
                        queue_edit = std::move(*handle);
                        editor.LoadText(Utf8ToUtf32(queue_edit->text));
                        queue_delete_armed = false;
                    }
                }
                redraw_queue_frame();
                continue;
            }
            // 编辑态拦下面板键(切 main/subagent、进查看前先了结编辑,不串
            // 目标);其余键照常进编辑器。
            if (MapToPanelKey(*raw_key).has_value()) {
                panel_notice = tr("queue.edit_blocks_panel");
                panel_notice_until = std::chrono::steady_clock::now() + std::chrono::seconds(2);
                redraw_queue_frame();
                continue;
            }
        }
        // 面板按键(键位缝 MapToPanelKey):上下选择、Enter 查看、Esc 逐层退、
        // x 停止/清除、Ctrl+X Ctrl+K 两段确认停全部。正文非空时状态机自己
        // 放行——上下归 composer 历史,普通字母 x 只进 composer,Enter 照旧
        // 提交给当前收件目标。停止走应用层接好的正式取消接口;面板只等任务
        // 线程报出终态的那一拍改灯,不先抹行。
        if (composer) {
            if (const std::optional<PanelKey> panel_key = MapToPanelKey(*raw_key);
                panel_key.has_value()) {
                const bool empty_now = editor.CurrentRenderState().line.empty();
                const auto outcome = panel_controller.HandleKey(
                    *panel_key, static_cast<int>(entries_before_key.size()) + 1, empty_now,
                    std::chrono::steady_clock::now());
                if (outcome.stop_all) {
                    const AgentPanelActions& actions = AgentPanelActionsSlot();
                    if (actions.cancel_all) {
                        const int stopped = actions.cancel_all();
                        panel_notice = trf("agent_panel.stop_all_notice", stopped);
                        panel_notice_until =
                            std::chrono::steady_clock::now() + std::chrono::seconds(2);
                    }
                }
                if (outcome.stop_current && !entries_before_key.empty() &&
                    panel_controller.selected() > 0) {
                    const AgentPanelEntry& entry =
                        entries_before_key[static_cast<std::size_t>(panel_controller.selected()) - 1];
                    const AgentPanelActions& actions = AgentPanelActionsSlot();
                    if (entry.running && actions.cancel_task) {
                        actions.cancel_task(entry.task_id);
                    } else if (!entry.running && actions.clear_task) {
                        actions.clear_task(entry.task_id);
                    }
                }
                if (outcome.consumed || outcome.redraw) {
                    redraw_with_panel(editor.CurrentRenderState(), panel_entries());
                    if (outcome.consumed) {
                        continue;
                    }
                }
            }
        }

        const std::optional<KeyEvent> mapped = MapKey(*raw_key);
        if (!mapped.has_value()) {
            continue;  // 修饰键、没映射到的键、半个组合序列,跳过
        }

        RenderState state = editor.HandleKey(*mapped);
        if (!state.line.empty() && !panel_controller.detail_open()) {
            // 敲了正文即离开面板焦点(上下键归历史);查看态(收件目标)例外
            // ——那只标签还挂着,话要送去那只子代理。
            panel_controller.Reset();
        }

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
                    // 回调铺完内容,整个框(上横线起,含面板行)重打一遍、重测锚点。
                    reserve_panel_room();
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
                    previous_frame.reset();
                    prev_frame_origin = -1;
                    redraw_with_panel(state, panel_entries());
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
            // 0.17.0 输入框化的提交收尾:横线/状态行/面板行擦掉,只留
            // `> 内容`,换行收尾——取舍见 CollapseBoxOnSubmit 注释。帧顶用
            // 上一帧记的绝对帧顶(含面板行与上横线)。
            CollapseBoxOnSubmit(prev_frame_origin, prompt_end_col, prev_body_row_count, state, prompt,
                                vt_enabled);
            std::cout << "\n";
            return Utf32ToUtf8(state.line);
        }

        redraw_with_panel(state, entries_before_key);

        if (state.esc_pressed && esc_rejects) {
            // 确认与可取消选择场景:Esc 不留在循环里继续等，直接交回
            // nullopt。不能拿空串代替——/model 明明把空串当默认第一项。
            std::cout << "\n";
            return std::nullopt;
        }
        if (state.eof_requested) {
            // Ctrl+D/Ctrl+Z 可能按在多行 composer 中间某一行,框下面还垫着
            // 横线/状态行:先把光标挪到编辑区(含框)最下面一行再换行,免得
            // 接下来的输出打在残留画面身上。prev_body_row_count 刚被
            // RedrawEditArea 更新过,就是"第一行之外"的总行数。
            if (queue_edit.has_value()) {
                // 整次读取要退场了:未提交的编辑按 Esc 同款还原,不留冻结条目。
                steering.CancelEdit(*queue_edit);
                queue_edit.reset();
            }
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

std::optional<ChoiceMenuResult> ReadChoiceMenu(const std::vector<ChoiceMenuItem>& items,
                                                const ChoiceMenuOptions& options, const Theme& theme) {
    if (items.empty() || !platform::StdinIsInteractive()) {
        return std::nullopt;
    }
    std::lock_guard<std::mutex> console_read_lock(ConsoleReadMutex());
    platform::RawInputScope raw_scope;
    if (!raw_scope.ok()) {
        return std::nullopt;
    }

    int start_row = 0;
    const int menu_rows = static_cast<int>(items.size()) + 1;
    {
        std::lock_guard<std::mutex> stdout_lock(StdoutWriteMutex());
        if (!EnsureStreamScreenRowsLocked(menu_rows)) {
            return std::nullopt;
        }
        const std::optional<platform::ScreenInfo> info = platform::GetScreenInfo();
        if (!info.has_value()) {
            return std::nullopt;
        }
        start_row = info->cursor_y;
    }

    ChoiceMenuCore menu(items.size(), options.multi_select, options.editable_index,
                        options.initial_cursor.value_or(0));
    auto draw = [&] {
        std::lock_guard<std::mutex> stdout_lock(StdoutWriteMutex());
        const std::optional<platform::ScreenInfo> info = platform::GetScreenInfo();
        if (!info.has_value()) {
            return false;
        }
        const int width = info->width;
        std::cout << kSyncOutputBegin << "\x1b[?25l";
        for (std::size_t i = 0; i < items.size(); ++i) {
            const bool active = i == menu.state().cursor;
            const bool editable = options.editable_index.has_value() && i == *options.editable_index;
            std::string prefix = active ? "> " : "  ";
            if (options.multi_select && !editable) {
                prefix += menu.state().selected[i] ? "[x] " : "[ ] ";
            } else if (options.multi_select) {
                prefix += "    ";
            }
            const int room = (std::max)(0, width - static_cast<int>(DisplayWidthUtf8(prefix)) - 1);
            std::string raw_label = items[i].label;
            if (editable) {
                raw_label += ": ";
                raw_label += menu.state().custom_text.empty() ? options.editable_placeholder
                                                               : menu.state().custom_text + (active ? "_" : "");
            }
            const std::string label = TruncateUtf8ToDisplayWidth(raw_label, room);
            int description_room = room - static_cast<int>(DisplayWidthUtf8(label)) - 3;

            platform::ClearRowHardFrom(0, start_row + static_cast<int>(i), width);
            platform::SetCursorPos(0, start_row + static_cast<int>(i));
            if (active) {
                std::cout << theme.confirm;
            }
            std::cout << prefix << label << theme.reset;
            if (!items[i].description.empty() && description_room > 0) {
                std::cout << theme.stats << " - "
                          << TruncateUtf8ToDisplayWidth(items[i].description, description_room)
                          << theme.reset;
            }
        }
        platform::ClearRowHardFrom(0, start_row + static_cast<int>(items.size()), width);
        platform::SetCursorPos(0, start_row + static_cast<int>(items.size()));
        std::string hint;
        if (menu.state().invalid) {
            hint = options.invalid_hint;
        } else if (options.editable_index.has_value() && menu.state().cursor == *options.editable_index) {
            hint = options.editable_hint;
        } else {
            hint = options.hint;
        }
        std::cout << (menu.state().invalid ? theme.error : theme.stats)
                  << TruncateUtf8ToDisplayWidth(hint, width - 1) << theme.reset << kSyncOutputEnd;
        std::cout.flush();
        return true;
    };

    auto clear = [&] {
        std::lock_guard<std::mutex> stdout_lock(StdoutWriteMutex());
        const std::optional<platform::ScreenInfo> info = platform::GetScreenInfo();
        if (info.has_value()) {
            std::cout << kSyncOutputBegin;
            for (int r = 0; r < menu_rows; ++r) {
                platform::ClearRowHardFrom(0, start_row + r, info->width);
            }
            platform::SetCursorPos(0, start_row);
            std::cout << "\x1b[?25h" << kSyncOutputEnd;
            std::cout.flush();
        } else {
            std::cout << "\x1b[?25h" << std::flush;
        }
    };

    if (!draw()) {
        return std::nullopt;
    }
    platform::KeyReader key_reader;
    while (!menu.state().submitted && !menu.state().cancelled) {
        const std::optional<platform::KeyInput> raw_key = key_reader.ReadOne();
        if (!raw_key.has_value()) {
            clear();
            return std::nullopt;
        }
        const std::optional<KeyEvent> mapped = MapKey(*raw_key);
        if (!mapped.has_value()) {
            continue;
        }
        menu.HandleKey(*mapped);
        if (!menu.state().submitted && !menu.state().cancelled) {
            if (!draw()) {
                clear();
                return std::nullopt;
            }
        }
    }
    const bool cancelled = menu.state().cancelled;
    ChoiceMenuResult result;
    result.selected_indices = menu.SelectedIndices();
    if (menu.state().custom_submitted) {
        result.custom_text = menu.state().custom_text;
    }
    clear();
    return cancelled ? std::nullopt : std::optional<ChoiceMenuResult>(std::move(result));
}

ConfirmMode CurrentConfirmMode() { return SharedEditor().confirm_mode(); }

void SetConfirmMode(ConfirmMode mode) { SharedEditor().set_confirm_mode(mode); }

void SetTranscriptUiHandler(TranscriptUiHandler handler) { UiHandlerSlot() = std::move(handler); }

void SetAgentPanelProvider(AgentPanelProvider provider) { AgentPanelProviderSlot() = std::move(provider); }

void SetAgentPanelDetailProvider(std::function<std::vector<std::string>(int task_id)> provider) {
    AgentPanelDetailSlot() = std::move(provider);
}

void SetAgentPanelActions(AgentPanelActions actions) { AgentPanelActionsSlot() = std::move(actions); }

std::optional<int> CurrentComposerAgentTarget() { return GetComposerTarget(); }

void SetIdleWakeHook(std::function<bool()> hook) { IdleWakeHookSlot() = std::move(hook); }

void SetStatusLineData(const StatusPanelData& values, const std::vector<std::string>& items,
                       const std::string& separator) {
    StatusLineData& data = StatusDataSlot();
    data.values = values;
    data.items = items;
    data.separator = separator;
}

void UpdateStatusLineContext(int context_percent, std::int64_t used_tokens, std::int64_t window_tokens,
                             bool measured) {
    // 见 console_input.hpp 的注释:只改数据、不落笔,footer 的重画事务在
    // 安全时机(下一笔正文/ticker 一拍/挂起恢复)取新值。锁跟 footer 重画
    // 读的是同一把,发布与重画互不越界。
    std::lock_guard<std::mutex> lock(StdoutWriteMutex());
    StatusLineData& data = StatusDataSlot();
    data.values = WithContextUpdate(data.values, context_percent, used_tokens, window_tokens, measured);
}

StatusPanelData SnapshotStatusLineValues() {
    std::lock_guard<std::mutex> lock(StdoutWriteMutex());
    return StatusDataSlot().values;
}

std::optional<int> DetectConsoleWidth() {
    return platform::ConsoleWidth();
}

std::mutex& StdoutWriteMutex() {
    static std::mutex m;
    return m;
}

std::mutex& ConsoleReadMutex() {
    static std::mutex m;
    return m;
}

// -----------------------------------------------------------------------
// "ask_user 被子代理状态遮挡"的 repaint 协调层,见 console_input.hpp 同名
// 一节的注释。suspend/paint 两枚深度都在 StreamFooterState 里(写点全在
// StdoutWriteMutex 之内),这里只做读口与登记槽。
// -----------------------------------------------------------------------

namespace {

// 交互菜单开屏时"收走浮动状态块"的钩子(AgentStatusPainter 自登记)。
// 登记槽的读写全在 StdoutWriteMutex 之内,不再套锁。
std::function<void()>& RepaintSuspendHideHookSlot() {
    static std::function<void()> hook;
    return hook;
}

}  // namespace

void SetRepaintSuspendHideHook(std::function<void()> hook) {
    std::lock_guard<std::mutex> lock(StdoutWriteMutex());
    RepaintSuspendHideHookSlot() = std::move(hook);
}

bool RepaintSuspendedLocked() {
    const StreamFooterState& f = FooterSlot();
    return f.suspend_depth > 0 || f.paint_depth > 0;
}

bool RepaintSuspendActive() {
    std::lock_guard<std::mutex> lock(StdoutWriteMutex());
    return FooterSlot().suspend_depth > 0;
}

int StreamFooterSuspendDepthForTest() {
    std::lock_guard<std::mutex> lock(StdoutWriteMutex());
    return FooterSlot().suspend_depth;
}

namespace {

// 见头文件 SetStreamScreenPrintHook 注释。读与写全在 StdoutWriteMutex
// 之内(设置方锁内赋值,监听线程锁内取用),不必再套一层原子/锁。
std::function<void()>& StreamScreenPrintHookSlot() {
    static std::function<void()> hook;
    return hook;
}

std::function<void(int)>& StreamScreenScrollHookSlot() {
    static std::function<void(int)> hook;
    return hook;
}

}  // namespace

void SetStreamScreenPrintHook(std::function<void()> hook) {
    std::lock_guard<std::mutex> lock(StdoutWriteMutex());
    StreamScreenPrintHookSlot() = std::move(hook);
}

void SetStreamScreenScrollHook(std::function<void(int)> hook) {
    std::lock_guard<std::mutex> lock(StdoutWriteMutex());
    StreamScreenScrollHookSlot() = std::move(hook);
}

bool EnsureStreamScreenRowsLocked(int rows_needed) {
    if (rows_needed <= 0) {
        return true;
    }
    const std::optional<platform::ScreenInfo> info = platform::GetScreenInfo();
    if (!info.has_value() || info->height <= 0) {
        return false;
    }
    const int needed_bottom = info->cursor_y + rows_needed - 1;
    if (needed_bottom < info->height) {
        return true;
    }
    const int overflow = needed_bottom - info->height + 1;
    if (overflow > info->cursor_y) {
        return false;
    }
    platform::SetCursorPos(0, info->height - 1);
    for (int i = 0; i < overflow; ++i) {
        std::cout << "\n";
    }
    std::cout.flush();
    if (const auto& hook = StreamScreenScrollHookSlot()) {
        hook(overflow);
    }
    platform::SetCursorPos(info->cursor_x, info->cursor_y - overflow);
    return true;
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
    if (!f.enabled || f.suspend_depth > 0 || f.paint_depth > 0) {
        return;  // 挂起期间(工具确认交互中)一律不画,见 StreamFooterSuspendScope 注释
    }
    const std::optional<platform::ScreenInfo> info = platform::GetScreenInfo();
    if (!info.has_value()) {
        return;  // 拿不到屏幕信息这一笔就不画,下一笔再来
    }
    // footer 已在屏上时，物理光标位于输入行；正文落笔位要从 state 取。
    // 第一次画才从当前光标认领正文位置。
    int bx = f.row >= 0 && f.body_x >= 0 ? f.body_x : info->cursor_x;
    int by = f.row >= 0 && f.body_y >= 0 ? f.body_y : info->cursor_y;

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

    const int working_rows = f.working ? 1 : 0;
    // 待发区行:现拉会话层队列的轻量快照(标题模式随状态变:Esc 立即送/
    // 编辑中/等下一个工具边界),行怎么摆是 BuildSteeringQueueRows 的纯逻辑,
    // 单测钉在那边。空队列连标题都不画(规格)。
    SteeringQueue& steering = SessionSteeringQueue();
    const std::vector<QueuedMessage> steering_snapshot = steering.Snapshot();
    QueueViewOptions queue_view;
    queue_view.visible_cap = kMaxVisibleQueuedLines;
    queue_view.title_mode = steering.immediate_delivery_requested() ? QueueTitleMode::Immediate
                                : std::any_of(steering_snapshot.begin(), steering_snapshot.end(),
                                              [](const QueuedMessage& item) { return item.edit_open; })
                                      ? QueueTitleMode::Editing
                                      : QueueTitleMode::Boundary;
    const std::vector<std::string> queue_rows_text =
        steering_snapshot.empty()
            ? std::vector<std::string>{}
            : BuildSteeringQueueRows(steering_snapshot, queue_view);
    const int queue_rows = static_cast<int>(queue_rows_text.size());
    // slash 提示行画在状态行之下(跟空闲 composer 的视觉层级一致),高度一并
    // 记进 total_rows——探底滚屏、旧框擦除(都按 f.rows 报账)随之生效。
    const int hint_rows = static_cast<int>(f.hints.size());
    const int total_rows = working_rows + queue_rows + kStreamFooterBoxRows + hint_rows;

    // 框落在正文光标的下一行:正文停在行中(bx>0)就 by+1,正文刚换行
    // 停在行首(bx==0)就落在 by 这空行上——两种都是"正文当前底部的下一行"。
    // ConPTY 常把缓冲区高度报成窗口高度，正文一长，框很快便贴底。旧版
    // 这时干脆不画，正是“Working 还在，输入框忽然没了”的根子。如今先
    // 主动滚够行数，再由 scroll hook 把正文/工具锚点一同上移。
    platform::SetCursorPos(bx, by);
    const int body_offset = bx > 0 ? 1 : 0;
    if (!EnsureStreamScreenRowsLocked(body_offset + total_rows)) {
        std::cout << kSyncOutputEnd;
        std::cout.flush();
        platform::SetCursorPos(bx, by);
        return;
    }
    if (const std::optional<platform::ScreenInfo> after_scroll = platform::GetScreenInfo(); after_scroll.has_value()) {
        bx = after_scroll->cursor_x;
        by = after_scroll->cursor_y;
    }
    const int target = by + (bx > 0 ? 1 : 0);

    const int width = info->width;
    const BoxChrome chrome{true, &f.theme, SharedEditor().confirm_mode()};
    int box_top = target;

    if (f.working) {
        platform::SetCursorPos(0, box_top);
        PrintFooterWorkingLine(f, width);
        box_top += working_rows;
    }

    for (std::size_t i = 0; i < queue_rows_text.size(); ++i) {
        platform::SetCursorPos(0, box_top + static_cast<int>(i));
        const int room = (std::max)(0, width - 1);
        std::cout << f.color << TruncateUtf8ToDisplayWidth(queue_rows_text[i], room) << f.reset;
    }
    if (!queue_rows_text.empty()) {
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
    // 状态行 = 常规状态段 + "Esc 打断"提示(规格:打断提示进状态行,不许
    // 挤进输入行)。先按留出的余量截常规段,再以纯文本段追加——两段各自
    // 包色,不做整行截断,跟 PrintStatusLine 的分段截断一个路数。plain 主题
    // f.color/f.reset 均为空串,自然无 ANSI。
    {
        const std::string interrupt = StreamFooterInterruptText(f.reset.empty());
        const int interrupt_cols = 3 + static_cast<int>(DisplayWidthUtf8(interrupt));  // " · " + 提示
        PrintStatusLine(chrome, (std::max)(20, width - 1 - interrupt_cols));
        std::cout << f.color << " · " << interrupt << f.reset;
    }

    // slash 提示行:状态行之下逐行摆(空闲 composer 同一层级),纯文本、按屏
    // 宽截断(跟 ReadLineKeyByKey 画 hint_lines 一个路数);plain 主题不夹
    // ANSI,天然无色。待发队列区在上横线之上,与提示行同屏共存、互不挤占。
    for (std::size_t i = 0; i < f.hints.size(); ++i) {
        platform::SetCursorPos(0, box_top + 4 + static_cast<int>(i));
        std::cout << TruncateUtf8ToDisplayWidth(f.hints[i], (std::max)(0, width - 1));
    }

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
    f.hints.clear();
    f.working = false;
    f.working_label.clear();
    f.working_highlight = 0;
    f.working_seconds = 0;
    f.suspend_depth = 0;
    f.paint_depth = 0;
    f.theme = theme;
    f.color = theme.stats;
    f.reset = theme.reset;
    f.hint = enabled ? StreamHintText(theme.reset.empty()) : std::string();
    // 开场不在这里画。紧随其后的 Spinner 会调用 StartStreamFooterWorking，
    // 由那一笔把 Working 与输入框合成首帧；首个流事件之后则由 OnDelta 接棒。
}

void EndStreamFooter() {
    std::lock_guard<std::mutex> lock(StdoutWriteMutex());
    StreamFooterState& f = FooterSlot();
    EraseStreamFooterLocked();  // 擦掉常驻那行,免得残留在 markdown 收束重画区之外
    f.enabled = false;
    f.echo.clear();
    f.hints.clear();
    f.hint.clear();
    f.working = false;
    f.working_label.clear();
    f.suspend_depth = 0;
    f.paint_depth = 0;
}

bool StartStreamFooterWorking(const std::string& label) {
    std::lock_guard<std::mutex> lock(StdoutWriteMutex());
    StreamFooterState& f = FooterSlot();
    if (!f.enabled) {
        return false;
    }
    f.working = true;
    f.working_label = label;
    f.working_highlight = 0;
    f.working_seconds = 0;
    RedrawStreamFooterLocked();
    return true;
}

void UpdateStreamFooterWorking(const std::string& label, std::size_t highlighted_glyph,
                               long long elapsed_seconds) {
    std::lock_guard<std::mutex> lock(StdoutWriteMutex());
    StreamFooterState& f = FooterSlot();
    if (!f.enabled || !f.working) {
        return;
    }
    f.working_label = label;
    f.working_highlight = highlighted_glyph;
    f.working_seconds = elapsed_seconds;
    RedrawStreamFooterLocked();
}

void StopStreamFooterWorking() {
    std::lock_guard<std::mutex> lock(StdoutWriteMutex());
    StreamFooterState& f = FooterSlot();
    if (!f.working) {
        return;
    }
    f.working = false;
    f.working_label.clear();
    f.working_highlight = 0;
    f.working_seconds = 0;
    RedrawStreamFooterLocked();
}

// 见 console_input.hpp StreamFooterSuspendScope 的注释。构造/析构各自只在
// 临界区里拿一下 StdoutWriteMutex,不会跨整个确认交互一直攥着锁。
StreamFooterSuspendScope::StreamFooterSuspendScope() {
    std::lock_guard<std::mutex> lock(StdoutWriteMutex());
    StreamFooterState& f = FooterSlot();
    if (f.suspend_depth == 0) {
        EraseStreamFooterLocked();  // 最外层落笔前把框彻底擦净
        // 再把子代理浮动状态块整块收走:菜单要从正文末尾一次铺到底,状态
        // 块留着就会插进标题/问题/选项中间。钩子由 AgentStatusPainter 构造
        // 时自登记,此刻锁在我们手里,实现不得重锁(见头文件约定)。之后
        // ticker 每拍都先查 RepaintSuspendedLocked(),挂起期间零输出——比
        // "手调一次 Hide"可靠,不存在下一拍又画回来的口子。
        if (const auto& hook = RepaintSuspendHideHookSlot()) {
            hook();
        }
    }
    ++f.suspend_depth;
}

StreamFooterSuspendScope::~StreamFooterSuspendScope() {
    std::lock_guard<std::mutex> lock(StdoutWriteMutex());
    StreamFooterState& f = FooterSlot();
    if (f.suspend_depth > 0) {
        --f.suspend_depth;
    }
    if (f.suspend_depth == 0) {
        RedrawStreamFooterLocked();  // 不再赌“后面总还有一笔事件”
    }
}

StreamFooterPaintScope::StreamFooterPaintScope(bool enabled) : active_(enabled) {
    if (!active_) {
        return;
    }
    std::lock_guard<std::mutex> lock(StdoutWriteMutex());
    StreamFooterState& f = FooterSlot();
    if (f.paint_depth == 0) {
        EraseStreamFooterLocked();
    }
    ++f.paint_depth;
}

StreamFooterPaintScope::~StreamFooterPaintScope() {
    if (!active_) {
        return;
    }
    std::lock_guard<std::mutex> lock(StdoutWriteMutex());
    StreamFooterState& f = FooterSlot();
    if (f.paint_depth > 0) {
        --f.paint_depth;
    }
    if (f.paint_depth == 0) {
        RedrawStreamFooterLocked();
    }
}

TurnInputListener::TurnInputListener(std::atomic<bool>& cancel_flag, const Theme& theme,
                                      std::atomic<bool>* transcript_expanded,
                                      ExpandRenderer expand_renderer)
    : cancel_flag_(cancel_flag),
      theme_(theme),
      transcript_expanded_(transcript_expanded),
      expand_renderer_(std::move(expand_renderer)) {
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
    // 线程已 join,没人再动账:还挂在半途的编辑事务按 Esc 同款收尾——
    // 原文还原、解冻。不收的话那条会一直冻着,投递泵跳过它,editable_size
    // 也数不到它,像"丢了一条"。
    if (open_edit_.has_value()) {
        SessionSteeringQueue().CancelEdit(*open_edit_);
        open_edit_.reset();
    }
}

void TurnInputListener::ThreadMain() {
    // POSIX 下监听期间要进 termios 原始模式才能逐键拿到(Windows 的
    // ReadConsoleInputW 不用改模式,这个 scope 在那边是空操作)。
    platform::KeyListenScope listen_scope;
    platform::KeyReader key_reader;  // 跨事件状态(代理对配对)整条线程存活

    // 会话层队列:监听线程只提交编辑动作,不拥有最终数据(见 queue_model.hpp)。
    SteeringQueue& steering = SessionSteeringQueue();

    // 排队输入/取回编辑共用一只真正的 LineEditorCore(composer 模式)。不用
    // SharedEditor():那份是前台"真正在读一行"的编辑器,历史/档位是会话级
    // 状态,监听线程不能去碰;这里只要光标/退格/粘贴/多行软换行这些纯编辑
    // 能力,补全候选给空表(流式输入行的 slash 提示由 StreamSlashHintLines
    // 现算,不碰编辑器状态)。
    LineEditorCore editor;
    editor.BeginLine(/*composer=*/true);

    // 正在编辑的排队消息(编辑事务凭据)。空 = 在敲新消息。
    std::optional<SteeringQueue::EditHandle> edit;
    // Del 两段删除的计时(规格:经明确提示后删掉——第一下亮提示,第二下
    // 2 秒窗口内才真删;提示就写在队列区标题的编辑态文案里)。
    bool delete_armed = false;
    std::chrono::steady_clock::time_point delete_armed_until{};

    // 输入行回显:多行正文只摆首行,尾巴带行数提示(完整正文在取回编辑器
    // 里看/改;footer 的输入行是单行会计,不能塞换行)。
    auto echo_text = [&] {
        const RenderState state = editor.CurrentRenderState();
        const std::string first = Utf32ToUtf8(state.lines.empty() ? std::u32string() : state.lines[0]);
        if (state.lines.size() <= 1) {
            return first;
        }
        return first + trf("queue.echo_more_lines", state.lines.size() - 1);
    };

    // footer 快照:输入行回显 + slash 提示(队列区在 RedrawStreamFooterLocked
    // 里现拉 SteeringQueue 快照,这里不用搬)。Windows 真控制台之外
    // (footer.enabled 为假)这些都是空操作,退回老的"不回显、只 Enter 时
    // 整条落队"。
    auto refresh_footer = [&] {
        std::lock_guard<std::mutex> stdout_lock(StdoutWriteMutex());
        FooterSlot().echo = echo_text();
        // slash 提示:编辑态(取回改写)和敲字态同一条规则——buffer 以 '/'
        // 开头、还没敲空格就实时列出匹配命令,纯函数现算,不碰任何编辑器状态。
        FooterSlot().hints = StreamSlashHintLines(StreamHintCandidates(), FooterSlot().echo);
        RedrawStreamFooterLocked();
    };

    // 取回一条排队消息进编辑器(光标落末尾),Del 待确认解除。
    auto begin_edit = [&](SteeringQueue::EditHandle handle) {
        editor.LoadText(Utf8ToUtf32(handle.text));
        delete_armed = false;
        edit = std::move(handle);
        refresh_footer();
    };
    auto close_edit_clear = [&]() {
        edit.reset();
        delete_armed = false;
        editor.BeginLine(/*composer=*/true);
    };
    auto editor_text_utf8 = [&] { return Utf32ToUtf8(editor.CurrentRenderState().line); };

    // 编辑态里上下键在队列条目间走:先把改到一半的正文按 Esc 语义提交到
    // 当前位(原位替换;提交失败说明那条已被边界送走,正文留在编辑器里
    // 继续当草稿),再取相邻一条。过末条往下 = 退出编辑态回"敲新消息"。
    auto browse_edit = [&](bool up) {
        if (!edit.has_value()) {
            return;
        }
        std::size_t index = 0;
        const auto snapshot = steering.Snapshot();
        for (std::size_t i = 0; i < snapshot.size(); ++i) {
            if (snapshot[i].id == edit->id) {
                index = i;
            }
        }
        const auto commit_status = steering.CommitEdit(*edit, editor_text_utf8());
        if (commit_status == SteeringQueue::CommitStatus::Ok) {
            close_edit_clear();
        } else {
            // 版本冲突:那条已变动。编辑器正文保留,事务就此了结。
            edit.reset();
            delete_armed = false;
        }
        // 相邻一条(方向钳住;正常情况下只有刚才那条冻着,BeginEdit 认不出
        // 冻结条目自然返回空,防御到位)。
        const bool go_prev = up && index > 0;
        const bool go_next = !up && index + 1 < snapshot.size();
        if (go_prev || go_next) {
            const std::size_t next = go_prev ? index - 1 : index + 1;
            if (auto handle = steering.BeginEdit(snapshot[next].id); handle.has_value()) {
                begin_edit(std::move(*handle));
                return;
            }
        }
        refresh_footer();
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
        // 阻塞式交互菜单(ask_user 选择菜单、工具确认……)开屏期间,键盘
        // 全归菜单一处:挂起计数>0 就算这一拍抢到了读权也不碰输入——
        // WaitForKeyEvent/ReadOne 一律不做,连"问题刚打印完、菜单还没抢到
        // 读权"的空窗期也不留给监听线程消费一枚键。菜单退出(计数归零)
        // 后流式排队输入、ESC 打断恢复原语义。
        if (RepaintSuspendActive()) {
            read_lock.unlock();
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

        // 取回键(Shift+←,备用 Ctrl+←):正文空、非编辑态、队列里有可取的
        // 条目,三者齐备才取最新一条;其余场合 Shift+Left 落到 MapKey 的缺省
        // 映射,仍是普通光标移动,不抢输入。
        if (IsQueueRecallKey(key->kind) && ShouldRecallQueuedMessage(editor.CurrentRenderState().line.empty(),
                                                                     edit.has_value(), steering.editable_size())) {
            if (auto handle = steering.BeginEditLatest(); handle.has_value()) {
                begin_edit(std::move(*handle));
            }
            continue;
        }

        if (key->kind == PK::Esc) {
            // 两层 Esc(规格):编辑态第一下只取消编辑、还原原文(不打断当前
            // 轮);退出编辑态后的 Esc 才是打断。队列里还有可送的:打断之外
            // 还要"立即送"——收场泵会在最近安全点把消息送进原目标,送达前
            // 队列区标题显示"正在打断并送达"。队列为空:Esc 仍只打断。
            if (edit.has_value()) {
                steering.CancelEdit(*edit);
                close_edit_clear();
                refresh_footer();
                continue;
            }
            if (steering.HasAnyDeliverable()) {
                steering.RequestImmediateDelivery();
            }
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
            // 单击对齐 Esc 的两层语义:编辑态先取消编辑;队列非空时同样
            // "打断 + 立即送"。
            if (edit.has_value()) {
                steering.CancelEdit(*edit);
                close_edit_clear();
                refresh_footer();
                continue;
            }
            if (steering.HasAnyDeliverable()) {
                steering.RequestImmediateDelivery();
            }
            interrupt_turn();
            continue;
        }
        if (key->kind == PK::CtrlO) {
            if (transcript_expanded_ != nullptr) {
                const bool expanded = !transcript_expanded_->load(std::memory_order_acquire);
                transcript_expanded_->store(expanded, std::memory_order_release);
                std::lock_guard<std::mutex> stdout_lock(StdoutWriteMutex());
                EraseStreamFooterLocked();
                std::cout << "\n" << theme_.stats
                          << (expanded ? tr("ui.expanded") : tr("ui.compact")) << theme_.reset << "\n";
                if (expand_renderer_) {
                    std::cout << expand_renderer_(expanded);
                }
                std::cout.flush();
                if (const auto& hook = StreamScreenPrintHookSlot()) {
                    hook();  // 模式行和转录快照都不在正文行数账里,旧锚点作废
                }
                RedrawStreamFooterLocked();
            }
            continue;
        }
        if (key->kind == PK::ShiftTab) {
            // 流式期间 Shift+Tab 切确认档——跟空闲路(ReadLineKeyByKey ->
            // LineEditorCore::HandleKey)同一套语义:同一枚 SharedEditor 状态、
            // 同一个 NextConfirmMode 纯函数,不抄第二份档序。档位来源也只有
            // 这一处:RedrawStreamFooterLocked 画状态行时现查
            // SharedEditor().confirm_mode(),切完下一帧自然带出新档,不用另
            // 记账。改档拿 ConsoleReadMutex(try_lock):前台真在读一行
            // (工具确认/ask_user)时说明那条路自己会处理按键,这里让路、
            // 不抢。重画走 stdout 锁:footer 挂起期间(确认菜单里)自动只记
            // 不画,退场第一帧带出;连切只原地换状态行,不铺提示行残骸
            // (对齐空闲路 M11 的取舍)。Tab 不跟着做:流式输入行不引入补全
            // 交互,维持不理会。
            {
                std::unique_lock<std::mutex> mode_lock(ConsoleReadMutex(), std::try_to_lock);
                if (!mode_lock.owns_lock()) {
                    continue;
                }
                LineEditorCore& shared_editor = SharedEditor();
                shared_editor.set_confirm_mode(NextConfirmMode(shared_editor.confirm_mode()));
            }
            std::lock_guard<std::mutex> stdout_lock(StdoutWriteMutex());
            RedrawStreamFooterLocked();
            continue;
        }
        if (key->kind == PK::Up || key->kind == PK::Down) {
            if (edit.has_value()) {
                // 编辑态:在队列条目间走(先把改到一半的正文写回当前位)。
                browse_edit(key->kind == PK::Up);
                continue;
            }
            if (editor.CurrentRenderState().line.empty() && steering.editable_size() > 0) {
                // 旧"空输入按上键取回最后一条"入口,保留作别名;帮助文案与
                // 队列标题只写 Shift+← 这一套主键(规格第四步)。
                if (auto handle = steering.BeginEditLatest(); handle.has_value()) {
                    begin_edit(std::move(*handle));
                }
                continue;
            }
            // 队列空/正文非空:进编辑器(翻历史,composer 里上下键的老现职)。
            editor.HandleKey(KeyEvent::Simple(key->kind == PK::Up ? KeyKind::Up : KeyKind::Down));
            refresh_footer();
            continue;
        }

        if (key->kind == PK::Delete) {
            // 编辑态:两段删除——第一下亮提示(编辑态标题自带"Del 再按一次
            // 删除"),第二下 2 秒窗口内真删。
            if (edit.has_value()) {
                const auto now = std::chrono::steady_clock::now();
                if (delete_armed && now < delete_armed_until) {
                    steering.DeleteMessage(*edit);
                    close_edit_clear();
                } else {
                    delete_armed = true;
                    delete_armed_until = now + std::chrono::seconds(2);
                }
                refresh_footer();
            }
            continue;  // 非编辑态的 Del 维持不理会(老行为)
        }

        if (key->kind == PK::NewLine) {
            // Shift/Alt+Enter:光标处插换行(编辑排队消息也享受多行能力)。
            editor.HandleKey(KeyEvent::Simple(KeyKind::NewLine));
            refresh_footer();
            continue;
        }

        if (key->kind == PK::Enter) {
            if (edit.has_value()) {
                // 原位替换:保 id、目标、排队次序。版本对不上 = 那条已在工具
                // 边界送走/被别处改过——提交失败,打一行提示,编辑器正文保留
                // 当新草稿(再 Enter 即落新队),绝不"一边送旧文一边显示已保存"。
                const auto status = steering.CommitEdit(*edit, editor_text_utf8());
                if (status == SteeringQueue::CommitStatus::Ok) {
                    close_edit_clear();
                } else {
                    edit.reset();
                    delete_armed = false;
                    {
                        std::lock_guard<std::mutex> stdout_lock(StdoutWriteMutex());
                        EraseStreamFooterLocked();
                        std::cout << "\n" << theme_.stats << tr("queue.commit_conflict") << theme_.reset << "\n";
                        std::cout.flush();
                        if (const auto& hook = StreamScreenPrintHookSlot()) {
                            hook();  // 插打了整行,正文块的行数账作废(锁还攥着)
                        }
                    }
                }
                refresh_footer();
                continue;
            }
            // 落队:带目标的 QueuedMessage 进会话层队列,正文挪进上方队列区,
            // 输入行清空回占位提示。全空白不落。
            const std::string text = editor_text_utf8();
            if (!text.empty()) {
                const std::optional<int> agent_target = GetComposerTarget();
                steering.Enqueue(agent_target.has_value() ? MessageTarget::Agent(*agent_target)
                                                          : MessageTarget::Main(),
                                 text);
                editor.BeginLine(/*composer=*/true);
            }
            refresh_footer();
            continue;
        }

        // 其余按键统一喂编辑器(字符/粘贴/退格/左右/Home/End;Tab 等 MapKey
        // 不认的维持不理会,跟老逻辑一致)。任意编辑动作解除 Del 待确认。
        if (const std::optional<KeyEvent> mapped = MapKey(*key); mapped.has_value()) {
            delete_armed = false;
            editor.HandleKey(*mapped);
            refresh_footer();
        }
    }
    // 交出还没了结的编辑事务:Stop() 在 join 之后读它并按 Esc 同款收尾。
    open_edit_ = edit;
}

}  // namespace lubancode::cli
