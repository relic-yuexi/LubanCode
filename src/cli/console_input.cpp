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

#include "cli/bottom_chrome.hpp"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <mutex>
#include <sstream>
#include <utility>

#include "cli/divider.hpp"
#include "cli/format_utils.hpp"
#include "cli/i18n.hpp"
#include "cli/image_input.hpp"  // kMaxImageBytes(Alt+V 贴图的上限)
#include "cli/keymap.hpp"
#include "cli/mention_menu.hpp"
#include "cli/queue_model.hpp"
#include "cli/slash_commands.hpp"
#include "cli/terminal_frame.hpp"
#include "platform/clipboard.hpp"
#include "platform/console.hpp"
#include "platform/paths.hpp"
#include "platform/process.hpp"
#include "platform/terminal_batch.hpp"
#include "platform/text_encoding.hpp"
#include "tools/path_utils.hpp"

namespace lubancode::cli {

std::vector<CompletionCandidate> BuildSlashCompletionCandidates() {
    // 见 console_input.hpp 的声明注释:空闲 SharedEditor() 与流式监听线程
    // 的本地编辑器共用这唯一一只转换口,组装循环不许再抄第二遍。
    std::vector<CompletionCandidate> candidates;
    candidates.reserve(AllSlashCommands().size());
    for (const auto& cmd : AllSlashCommands()) {
        candidates.push_back(CompletionCandidate{cmd.name, cmd.description});
    }
    return candidates;
}

namespace {

void StripTrailingCrLf(std::string& s) {
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) {
        s.pop_back();
    }
}

// (导航坞的布局/折叠/窗口/状态机全在 cli/agent_panel.cpp 的纯逻辑层,
// 行序与帧账见 cli/bottom_chrome.hpp;这里只管接线与落笔。)

// 贯穿整条交互会话存活的编辑器实例:main.cpp 里 `> ` 主循环、工具确认
// 提示、/model 选择、初次配置向导,全部经这一个 ReadLine() 入口,底下共用
// 这一份 LineEditorCore——历史列表、确认模式才有地方跨多轮读取存住。
// 补全候选从 BuildSlashCompletionCandidates() 转来,不重复写一份命令清单。
LineEditorCore& SharedEditor() {
    static LineEditorCore editor = [] { return LineEditorCore(BuildSlashCompletionCandidates()); }();
    return editor;
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

// 视图切换钩子的槽(viewed_task_id 变了才被调;tail_rows>0 = 实时流重铺拍,
// 见 console_input.hpp)。
std::function<void(int, int)>& AgentViewSwitchHookSlot() {
    static std::function<void(int, int)> hook;
    return hook;
}

AgentPanelActions& AgentPanelActionsSlot() {
    static AgentPanelActions actions;
    return actions;
}

std::vector<int> PanelEntryIds(const std::vector<AgentPanelEntry>& entries) {
    std::vector<int> ids;
    ids.reserve(entries.size());
    for (const auto& entry : entries) {
        ids.push_back(entry.task_id);
    }
    return ids;
}

// 会话级面板状态机(规格三):空闲 composer 与流式监听线程共用同一份
// 选择/焦点/详情/两段确认——流式转空闲状态不跳,不靠两边各自记账。
AgentPanelSession& PanelSessionSlot() {
    static AgentPanelSession session;
    return session;
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

// 后台通知钩子的存取点(同一套会话级静态槽;主线程独占):空闲 composer
// 的 100ms 拍里叫一声,应用层把攒着的"当场要让人知道"的系统侧通知(比如
// 后台子代理的权限拒绝)取走自己落账(toast + transcript 事件)。
std::function<void()>& BackgroundNoticeHookSlot() {
    static std::function<void()> hook;
    return hook;
}

// Ctrl+R 提问历史搜索的数据源槽(同一套会话级静态槽;主线程读写)。
PromptHistoryProvider& PromptHistoryProviderSlot() {
    static PromptHistoryProvider provider;
    return provider;
}

// 草稿 stash(一格,主线程读写;取回在键路、询问在应用收场路,都是主线程)。
ComposerStashSnapshot& ComposerStashSlot() {
    static ComposerStashSnapshot stash;
    return stash;
}

// @ 文件提及菜单的数据源槽(同一套;主线程)。
FileMentionProvider& FileMentionProviderSlot() {
    static FileMentionProvider provider;
    return provider;
}

// 查看态回流的短提示(见 console_input.hpp ShowPanelToast 注释):挂进
// 导航坞提示行位置,到时随下一帧自动收。读写都在主线程(会话主循环写、
// 空闲 composer 帧读),不加锁。
struct PanelToastState {
    std::string text;
    std::chrono::steady_clock::time_point until{};
};
PanelToastState& PanelToastSlot() {
    static PanelToastState state;
    return state;
}

// 键位缝:platform 语义按键 -> 面板动作 id(PanelKey)。0.30.x 交互抛光
// 总账起,这层从手写小表换成 keymap 查表(Panel 作用域):用户 /keymap
// 改绑 agent.* 动作,面板跟脚换键,这函数一个字不用改。状态机与布局仍在
// cli/agent_panel(纯逻辑,单测钉)。
std::optional<PanelKey> MapToPanelKey(const platform::KeyInput& key) {
    // NewLine(Shift/Alt+Enter)不是和弦,不进面板,照旧插换行。
    if (key.kind == platform::KeyInput::Kind::NewLine) {
        return std::nullopt;
    }
    const auto chord = keymap::ChordFromKeyInput(key);
    if (!chord.has_value()) {
        return std::nullopt;
    }
    using keymap::ActionId;
    switch (keymap::ActiveKeymap().Lookup(keymap::KeyScope::Panel, *chord)) {
        case ActionId::AgentNavUp:
            return PanelKey::Up;
        case ActionId::AgentNavDown:
            return PanelKey::Down;
        case ActionId::AgentView:
            return PanelKey::EnterView;
        case ActionId::AgentBack:
            return PanelKey::Esc;
        case ActionId::AgentStop:
            return PanelKey::StopEntry;
        case ActionId::AgentStopAllArm:
            return PanelKey::StopAllArm;
        case ActionId::AgentStopAllConfirm:
            return PanelKey::StopAllConfirm;
        default:
            return std::nullopt;  // 没绑到面板动作的键,交回 composer
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
    int last_width = -1;      // 上一帧的终端列宽;变了说明 resize 过,旧锚点作废
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
// 导航坞常态最多单列几只代理(仿 Claude Code 的窗口密度);再多便开窗,
// 选中行始终留在窗口里。
constexpr int kDockMaxVisibleEntries = 5;
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
        case PK::Char: {
            KeyEvent event = KeyEvent::Char(key.ch);
            event.ctrl = key.ctrl;
            event.alt = key.alt;  // 和弦修饰随行,编辑器核心自己不当正文插
            return event;
        }
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
        case PK::CtrlL:
            // 整屏重画键:同上,只在 ReadLineKeyByKey 的自救缝里消费。
            return std::nullopt;
        case PK::CtrlP:
            return KeyEvent::Simple(KeyKind::CtrlP);
        case PK::CtrlN:
            return KeyEvent::Simple(KeyKind::CtrlN);
        case PK::Esc:
            return KeyEvent::Simple(KeyKind::Esc);
        case PK::Delete:
            return KeyEvent::Simple(KeyKind::Delete);
        case PK::PageUp:
            return KeyEvent::Simple(KeyKind::PageUp);
        case PK::PageDown:
            return KeyEvent::Simple(KeyKind::PageDown);
    }
    return std::nullopt;
}

// 帧账的"保锚可见"原语定义在文件后段(匿名 namespace 之外——头文件里
// 对外声明了,live_transcript.hpp 的正文账也要调),这里只留注释指路。

// 探一下从 start_row 起要画 total_rows 行(编辑行 + 提示行)会不会伸出
// 可视窗口底——真伸出了,先按帧账原语 EnsureViewportRowsLocked 把可视区
// 腾够(长缓冲平移视口,贴底滚内容),start_row 只随内容滚动平移,账目
// 对平之后再画。旧版按"缓冲区最后一行"探底,长缓冲(conhost 9001 行、
// 真控制台驱动器的 120×400)里帧会画到窗口底下用户看不见——正是"回合
// 收口后 composer 掉出可视区"那单的根子。
void EnsureRoomForRows(int& start_row, int buffer_height, int total_rows) {
    const int overflow = EnsureViewportRowsLocked(start_row, total_rows);
    if (overflow > 0) {
        start_row = (std::max)(0, start_row - overflow);
    }
    (void)buffer_height;
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
// 0.29.x"导航贴底"一单:整帧记账的次序定为 规格"固定布局"——
//   rows_above(待发队列) → 上横线(带查看态右端短标签 rule_tag)→ composer
//   → 下横线 → 状态行 → rows_below(代理导航坞) → slash 提示。
// 导航坞挪到 composer 与状态栏之后贴底,不再从输入框上方向上长;待发队列
// 仍属 composer 上方(它是即将发送的内容,不属代理导航)。锚点 start_row 仍
// 是 composer 首行(提示符行),帧顶 = start_row - rows_above - 1 由这里推;
// 上一帧的绝对帧顶记在 prev_frame_origin,面板增减/终端缩放/滚屏挪了位就
// 对不上,整帧重画。提示符并进首行文本(x=0),不再靠进函数前那一次
// std::cout 存活。
void RedrawEditArea(int& start_row, int& prompt_end_col, const std::string& prompt,
                    const RenderState& state, const std::vector<std::string>& rows_above,
                    const std::vector<std::string>& rows_below,
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
                      rows_below.size() + state.hint_lines.size());
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
    // 导航坞:状态行之下贴底(0.29.x 层级反转);slash 提示是短命 UI,垫最底。
    for (const auto& dock : rows_below) {
        next.rows.push_back(InlineFrameRow{
            0, buffer_width, false, TruncateUtf8ToDisplayWidth(dock, buffer_width - 1)});
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
    const int body_rows = static_cast<int>(layout.rows.size()) - 1 + box_rows + hint_count +
                          above_total + static_cast<int>(rows_below.size());
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
                                             bool composer, ReadExitReason* exit_reason) {
    // 整个函数体都攥着这把锁:M10 的 TurnInputListener 监听线程只在抢到锁
    // 的间隙才读控制台输入,这一行锁一上,就等于宣布"编辑器正在读",监听
    // 线程会自动让出、不跟这里抢同一份键盘输入。
    std::lock_guard<std::mutex> console_read_lock(ConsoleReadMutex());

    // 原始逐键模式进不去(极少见,非标准终端)就退回整行读入。
    platform::RawInputScope raw_scope;
    if (!raw_scope.ok()) {
        const std::optional<std::string> cooked = platform::ReadLineCooked();
        if (exit_reason != nullptr) {
            *exit_reason = cooked.has_value() ? ReadExitReason::Submitted : ReadExitReason::Cancel;
        }
        return cooked;
    }
    BracketedPasteScope paste_scope(composer && !theme.reset.empty());

    LineEditorCore& editor = SharedEditor();
    editor.BeginLine(composer);

    // 0.17.0:composer 读取开输入框(上横线 + `> ` 输入行 + 下横线 + 状态
    // 行)。0.29.x 起状态行之下还有代理导航坞贴底(整帧记账,见
    // RedrawEditArea);导航在下方长,EnsureRoomForRows 探底滚屏自会腾位,
    // 不再需要"锚点上方预留面板行数"那一步。
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
    std::optional<InlineFrame> previous_frame;
    const bool vt_enabled = platform::ProbeStdoutConsole().vt_enabled;

    platform::KeyReader key_reader;
    // 面板状态机:会话级 AgentPanelSession(纯逻辑在 cli/agent_panel,键位缝在
    // MapToPanelKey:选择/查看/x 停止清除/Ctrl+X Ctrl+K 两段确认全在里面,单测
    // 钉在 tests/test_agent_panel.cpp)。空闲与流式监听共用同一份,选择按稳定
    // task id 记。
    AgentPanelSession& panel_session = PanelSessionSlot();
    std::string panel_fingerprint;  // 上一帧面板指纹(条目+状态机+成行),变了才重画
    std::string panel_notice;       // 停全部的回执,挂在提示行下面两秒就收
    std::chrono::steady_clock::time_point panel_notice_until{};
    int prev_frame_origin = -1;  // 上一帧的绝对帧顶;面板增减/滚屏后对不上就整帧重画

    if (composer) {
        SetComposerTarget(std::nullopt);  // 每次读取开始先归 main;首帧 build_panel 会按查看态再挂回去
    }

    auto panel_entries = [&]() -> std::vector<AgentPanelEntry> {
        if (!composer || !AgentPanelProviderSlot()) {
            return {};
        }
        return AgentPanelProviderSlot()();
    };
    const auto panel_ids = [](const std::vector<AgentPanelEntry>& entries) {
        std::vector<int> ids;
        ids.reserve(entries.size());
        for (const auto& entry : entries) {
            ids.push_back(entry.task_id);
        }
        return ids;
    };
    // 导航表:条目经闲置折叠后的可导航 id 序列(main=0 在首位,折叠时在首个
    // 被折条目的位置插 kIdleSummaryTaskId 哨兵)。布局与按键状态机共用这
    // 一份,选择永远不会落进被折起来的区域。
    const auto nav_ids_for = [&](const std::vector<AgentPanelEntry>& entries) {
        const AgentPanelSession::Snapshot snap0 = panel_session.SnapshotFor(panel_ids(entries));
        return DockNavigationIds(entries, snap0.idle_expanded, snap0.target_task_id.value_or(0));
    };

    // 导航坞帧:折叠+窗口化布局(纯函数)+ 输入框上横线右端短标签 + composer
    // 收件目标。0.29.x 起坞画在状态行之下贴底(rows_below),空闲与流式共用
    // 同一套纯逻辑;导航坞只放导航,查看态长正文走视图切换钩子进上方视口。
    const auto build_dock = [&](const std::vector<AgentPanelEntry>& entries,
                                std::string& tag_out) -> std::vector<std::string> {
        tag_out.clear();
        if (!composer || entries.empty()) {
            if (composer && entries.empty()) {
                panel_session.OnEntriesChanged({});  // 子代理全没了:焦点/查看态收干净
            }
            SetComposerTarget(std::nullopt);
            return {};
        }
        const std::vector<int> nav_ids = nav_ids_for(entries);
        panel_session.OnEntriesChanged(nav_ids);
        const AgentPanelSession::Snapshot snapshot = panel_session.SnapshotFor(nav_ids);
        // 预算:常态最多单列 5 只代理(窗口围绕选中开);整坞封在半屏上下,
        // 输入框与状态栏始终留在视口里。宽度/高度每拍现查,resize 下一帧
        // 就对上。
        const std::optional<platform::ScreenInfo> now = platform::GetScreenInfo();
        const int width = now.has_value() ? now->width : (info.has_value() ? info->width : 80);
        const int dock_budget =
            now.has_value() ? std::max(2, std::min(now->height / 2, 24)) : 0;
        const AgentDockLayout layout =
            LayoutAgentDock(entries, snapshot.selected_index, snapshot.focused,
                            kDockMaxVisibleEntries, dock_budget, width,
                            snapshot.stop_all_armed, /*streaming=*/false, snapshot.idle_expanded,
                            snapshot.viewed_task_id);
        if (snapshot.target_task_id.has_value()) {
            // composer 此刻以查看态那只子代理为收件人;上横线右端挂它的 title。
            for (const auto& entry : entries) {
                if (entry.task_id == *snapshot.target_task_id) {
                    SetComposerTarget(entry.task_id);
                    tag_out = entry.title;
                    break;
                }
            }
        }
        if (!snapshot.target_task_id.has_value()) {
            SetComposerTarget(std::nullopt);
        }
        std::vector<std::string> lines = RenderAgentDockLines(layout, width);
        if (!panel_notice.empty()) {
            if (std::chrono::steady_clock::now() < panel_notice_until) {
                // move 版 insert:提示行打完就不再用 panel_notice(走 else 分支
                // 才 clear),顺带绕开 GCC 13 对 const& 插入路径的
                // -Warray-bounds 误报(把内联后的栈上 string 认成地址零)。
                lines.insert(lines.begin() + 1, std::move(panel_notice));
            } else {
                panel_notice.clear();
            }
        }
        // 查看态回流 toast(ShowPanelToast):与 panel_notice 同一挂点、同一
        // 过期规矩。到时 build_dock 少还这一行,帧指纹跟着变,下一拍整帧
        // 重画自然收走——不抢屏,不进对话流。
        PanelToastState& toast = PanelToastSlot();
        if (!toast.text.empty()) {
            if (std::chrono::steady_clock::now() < toast.until) {
                lines.insert(lines.begin() + 1, toast.text);
            } else {
                toast.text.clear();
            }
        }
        return lines;
    };
    const auto fingerprint_of = [](const std::vector<AgentPanelEntry>& entries,
                                   const BottomChromeFrame& frame,
                                   const AgentPanelSession::Snapshot& snapshot) {
        std::string value;
        for (const auto& entry : entries) {
            value += std::to_string(entry.task_id) + "\x1f" + entry.name + "\x1f" + entry.title +
                     "\x1f" + entry.state + "\x1f" +
                     (entry.running ? "R" : entry.failed ? "F" : entry.cancelled ? "C"
                                                        : entry.done_delivered ? "X" : "D") +
                     "\n";
        }
        value += "#" + std::to_string(snapshot.selected_task_id) + (snapshot.focused ? "f" : "-") +
                 "v" + std::to_string(snapshot.viewed_task_id) +
                 (snapshot.stop_all_armed ? "a" : "-") + (snapshot.idle_expanded ? "i" : "-") + "\n";
        value += BottomChromeFingerprint(frame);
        return value;
    };
    // 0.28.x 排队消息区:画在代理面板之下、composer 上横线之上(规格"显示"
    // 节的次序)。空队列连标题都不画;标题随状态变(编辑中/本轮收尾后送出)。
    // 空闲时没有工具边界可等,标题写"本轮收尾后送出"。
    SteeringQueue& steering = SessionSteeringQueue();
    std::optional<SteeringQueue::EditHandle> queue_edit;  // 正在取回编辑的凭据
    bool queue_delete_armed = false;
    std::chrono::steady_clock::time_point queue_delete_armed_until{};

    // Ctrl+R 提问历史搜索(0.30.x 第二批):查询文本就活在编辑器缓冲里,
    // 命中行挂在提示行位置(transient rows);打开时原草稿整份存起,取消
    // 时一字不少装回。范围轮换/选位都在纯逻辑层(HistorySearchSession)。
    HistorySearchSession history_search;
    std::string history_search_draft;
    std::string history_search_last_query;  // 查询未变就不重跑匹配

    // @ 文件提及菜单(0.30.x 第三批):索引每次读取取一次(应用层缓存),
    // 模糊匹配按词元签名缓存在这里;词元变了选位归零、Esc 收起自动重开。
    std::vector<FileMentionEntry> mention_entries;
    bool mention_index_loaded = false;
    std::string mention_cache_token;
    std::vector<std::size_t> mention_matches_cache;
    int mention_selected = 0;
    bool mention_dismissed = false;
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

    // 一本帧账:待发队列在 composer 上横线之上,导航坞在状态栏之下贴底,
    // slash 提示垫最底。空闲 composer 与流式 footer 认的是同一只
    // BottomChromeFrame(同一套 LayoutAgentDock + BuildSteeringQueueRows),
    // 两条路不许各拼一套行序。
    const auto build_frame = [&](const std::vector<std::string>& queue_rows,
                                 const std::vector<std::string>& dock, const RenderState& state,
                                 int selected_task_id) {
        BottomChromeFrame frame;
        frame.queue_rows = queue_rows;
        frame.agent_dock_rows = dock;
        frame.transient_rows = state.hint_lines;
        frame.composer_rows = std::max(1, static_cast<int>(state.lines.size()));
        frame.selected_task_id = selected_task_id;
        frame.revision = BottomChromeRevision(frame);
        return frame;
    };

    // 搜索开着时把 RenderState 的提示行换成命中清单;查询变化就地重跑
    // 匹配。redraw_with_panel 与 100ms tick 的指纹账共用这一份,不然
    // tick 拿 slash 提示算指纹,与真画出的搜索行对不上,帧帧空转重画。
    // 搜索之外,@ 提及菜单也走这里换装(第三批)。
    const auto mention_menu_for = [&](const RenderState& state) -> const std::vector<std::size_t>* {
        if (!composer || history_search.active() || queue_edit.has_value() || !FileMentionProviderSlot()) {
            mention_cache_token.clear();
            return nullptr;
        }
        if (state.cursor_row >= state.lines.size()) {
            mention_cache_token.clear();
            return nullptr;
        }
        const auto token = FindMentionToken(state.lines[state.cursor_row], state.cursor_col);
        if (!token.has_value()) {
            mention_cache_token.clear();  // 离开词元:缓存作废(含选位)
            return nullptr;
        }
        const std::string signature = std::to_string(token->start) + ":" + token->query;
        if (signature != mention_cache_token) {
            mention_cache_token = signature;
            mention_dismissed = false;
            mention_selected = 0;
            if (!mention_index_loaded) {
                mention_entries = FileMentionProviderSlot()();
                mention_index_loaded = true;
            }
            mention_matches_cache = FuzzyMatchMentions(mention_entries, token->query);
        }
        if (mention_dismissed || mention_matches_cache.empty()) {
            return nullptr;
        }
        return &mention_matches_cache;
    };

    const auto apply_search_hints = [&](RenderState& state) {
        if (history_search.active()) {
            const std::string query = Utf32ToUtf8(state.line);
            if (query != history_search_last_query) {
                history_search.Rerun(query);
                history_search_last_query = query;
            }
            const std::optional<platform::ScreenInfo> info_now = platform::GetScreenInfo();
            const int width = info_now.has_value() ? info_now->width : 80;
            state.hint_lines =
                BuildHistorySearchLines(history_search, query, width, theme.stats, theme.reset);
            state.selected_index = -1;  // slash 菜单的选中镜像不适用于搜索行
            return;
        }
        if (const auto* matches = mention_menu_for(state); matches != nullptr) {
            const std::optional<platform::ScreenInfo> info_now = platform::GetScreenInfo();
            const int width = info_now.has_value() ? info_now->width : 80;
            state.hint_lines =
                BuildMentionMenuLines(mention_entries, *matches, mention_selected, width);
            state.selected_index = -1;
            return;
        }
        // 空 composer 的常用键速览(规格:"footer 先摆三四枚常用键,? 展开
        // 全表"):键位从 keymap 反查,改键后提示跟着改。
        if (state.lines.size() == 1 && state.lines[0].empty()) {
            const keymap::Keymap& km = keymap::ActiveKeymap();
            std::string hint;
            const auto add = [&](keymap::ActionId action, const char* label_key) {
                const auto chord = km.ChordFor(action);
                if (!chord.has_value()) {
                    return;
                }
                if (!hint.empty()) {
                    hint += " · ";
                }
                hint += keymap::FormatKeyChord(*chord) + " " + tr(label_key);
            };
            add(keymap::ActionId::HelpShow, "hint.keys.help");
            add(keymap::ActionId::ChatSearchHistory, "hint.keys.search_history");
            add(keymap::ActionId::TranscriptToggleExpand, "hint.keys.expand");
            add(keymap::ActionId::ChatExternalEditor, "hint.keys.editor");
            if (!hint.empty()) {
                state.hint_lines = {hint};
            }
        }
    };

    auto redraw_with_panel = [&](const RenderState& raw_state, const std::vector<AgentPanelEntry>& entries) {
        // 搜索开着:提示行位置换装成命中清单(查询变化就地重跑匹配)。
        RenderState state = raw_state;
        apply_search_hints(state);
        std::string tag;
        const std::vector<std::string> dock = build_dock(entries, tag);
        const std::vector<std::string> queue_rows = queue_rows_now();
        const AgentPanelSession::Snapshot snapshot = panel_session.SnapshotFor(nav_ids_for(entries));
        const BottomChromeFrame frame =
            build_frame(queue_rows, dock, state, snapshot.selected_task_id);
        chrome.mode = editor.confirm_mode();
        RedrawEditArea(start_row, prompt_end_col, prompt, state, frame.queue_rows, frame.agent_dock_rows,
                       tag, prev_body_row_count, previous_frame, prev_frame_origin, vt_enabled, chrome);
        panel_fingerprint = fingerprint_of(entries, frame, snapshot);
    };

    // 内容铺完后的重锚(UI 按键回调路 / 视图切换路共用):重打上横线与提示
    // 符、重测锚点、作废旧帧、整帧重画。铺出的正文把旧 chrome 自然顶进滚屏。
    const auto reanchor_prompt_and_redraw = [&]() {
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
        redraw_with_panel(editor.CurrentRenderState(), panel_entries());
    };

    const auto retire_idle_chrome = [&]() {        if (!box) {
            return;
        }
        const std::optional<platform::ScreenInfo> info = platform::GetScreenInfo();
        if (!info.has_value()) {
            return;  // 拿不到屏幕信息就不硬擦,退回旧行为(换行让位)
        }
        int top = prev_frame_origin;
        if (top < 0) {
            top = start_row > 0 ? start_row - 1 : 0;
        }
        int bottom = start_row + prev_body_row_count;
        if (bottom >= info->height) {
            bottom = info->height - 1;
        }
        for (int r = top; r <= bottom; ++r) {
            if (r >= 0) {
                platform::ClearRowHardFrom(0, r, info->width);
            }
        }
        platform::SetCursorPos(0, top);
        previous_frame.reset();
        prev_frame_origin = -1;
        prev_body_row_count = 0;
    };

    // Ctrl+G 外部编辑器(0.30.x 第三批):收掉底栏帧把整屏让给编辑器,
    // $VISUAL/$EDITOR(都没有时 Windows notepad、POSIX vi)继承控制台跑;
    // 读回保多行,CRLF 归一、剥编辑器补的一个行尾换行。启动失败/非零退出/
    // 文件被删/非法 UTF-8,原草稿一字不动,短错一行。临时文件用完即删。
    const auto run_external_editor = [&]() -> void {
        const std::string draft = Utf32ToUtf8(editor.CurrentRenderState().line);
        std::string editor_cmd;
        if (const auto visual = platform::GetEnvVar("VISUAL"); visual.has_value() && !visual->empty()) {
            editor_cmd = *visual;
        } else if (const auto ed = platform::GetEnvVar("EDITOR"); ed.has_value() && !ed->empty()) {
            editor_cmd = *ed;
        } else {
#ifdef _WIN32
            editor_cmd = "notepad";
#else
            editor_cmd = "vi";
#endif
        }
        namespace fs = std::filesystem;
        fs::path file;
        try {
            file = fs::temp_directory_path() /
                   ("lubancode-draft-" + std::to_string(platform::CurrentProcessId()) + "-" +
                    std::to_string(
                        std::chrono::steady_clock::now().time_since_epoch().count()) +
                    ".md");
        } catch (const std::exception&) {
            std::cout << theme.error << tr("editor.no_temp") << theme.reset << "\n";
            return;
        }
        {
            std::ofstream out(file, std::ios::binary | std::ios::trunc);
            if (!out) {
                std::cout << theme.error << tr("editor.write_failed") << theme.reset << "\n";
                return;
            }
            out << draft;
            out.flush();
            if (!out) {
                std::error_code rm;
                fs::remove(file, rm);
                std::cout << theme.error << tr("editor.write_failed") << theme.reset << "\n";
                return;
            }
        }
        retire_idle_chrome();
        std::cout << "\n" << std::flush;
        const std::string command = editor_cmd + " \"" + lubancode::tools::PathToUtf8(file) + "\"";
        const int exit_code = platform::RunInteractiveCommand(command);
        std::string read_back;
        bool read_ok = false;
        {
            std::ifstream in(file, std::ios::binary);
            if (in) {
                std::ostringstream buffer;
                buffer << in.rdbuf();
                read_back = buffer.str();
                read_ok = true;
            }
        }
        std::error_code rm_ec;
        fs::remove(file, rm_ec);  // 用完即清;崩溃残件留给系统临时目录回收
        if (exit_code != 0) {
            std::cout << theme.error << trf("editor.nonzero", exit_code) << theme.reset << "\n";
            reanchor_prompt_and_redraw();
            return;
        }
        if (!read_ok) {
            std::cout << theme.error << tr("editor.file_gone") << theme.reset << "\n";
            reanchor_prompt_and_redraw();
            return;
        }
        if (!platform::IsValidUtf8(read_back)) {
            std::cout << theme.error << tr("editor.bad_utf8") << theme.reset << "\n";
            reanchor_prompt_and_redraw();
            return;
        }
        const std::string normalized = NormalizeEditorDraft(read_back);
        editor.LoadTextWithCursor(Utf8ToUtf32(normalized), normalized.size());
        std::cout << theme.stats << trf("editor.done", editor_cmd) << theme.reset << "\n";
        reanchor_prompt_and_redraw();
    };

    // RetireIdleChrome(规格"现场二"):空闲 composer 的底栏所有权交接。wake
    // 要把系统侧事件交回主循环时,先按上一帧的账把整帧(待发队列、上下横
    // 线、输入行、状态栏、导航坞、提示行)正式收束——硬清干净、帧账归零,
    // 绝不靠一个换行把旧帧推进滚屏留小尾巴。清完即 NoChrome,主循环的通知
    // 与流式 footer 才接手所有权;任何时刻只有一名 owner。流式那头的
    // RetireStreamingChrome 是 EndStreamFooter 里的 EraseStreamFooterLocked,
    // 同一本帧账的另一头。
    // -------------------------------------------------------------------
    // 会话视口的 retained view(规格"现场四"):查看态正文不靠"往 scrollback
    // 再打印一份"冒充切页。上一视图正文落帧时记下它的缓冲顶行
    // (view_body_top);真切会话时先把旧帧从可视区硬擦干净,新帧原位落
    // 下——Esc 回 main 后代理正文不再占着当前 viewport 冒充活状态。正文
    // 长过回滚缓冲顶时(顶部早已滚出 viewport),viewport 之内也保证擦净,
    // 滚出部分自然留在滚屏历史,不清回滚账。
    // 锚点只在本段读取内有效:两段读取之间可能跑过整轮(流式正文滚屏),
    // 绝对行号全部失效——跨调用的"查看任务退场"走下面进门路的清屏重铺,
    // 不认旧锚点。
    //
    // 擦账只此一本(查看态完成退场花屏单,2026-08-17):0.26.11 曾在 app 侧
    // PrintViewedTranscript 里另立一份帧账(绝对行顶/行数/视口顶),进门先按
    // 那份账擦一遍再画——两把擦子先后下手,console 侧刚把旧帧区清空、app 侧
    // 又按一份跨调用且会被实时流重铺/滚屏弄失准的绝对行账再擦一矩形,光标
    // 被带偏到别处,退场回 main 那一拍整个画面跟着报销。现在 app 侧钩子只打
    // 印不擦,擦旧帧全归这本账(每次铺帧前现记现擦,绝不跨调用攒)。
    // -------------------------------------------------------------------
    std::optional<int> view_body_top;  // 上一视图正文的缓冲顶行;nullopt = 无
    const auto erase_previous_view_body = [&]() {
        if (!view_body_top.has_value()) {
            return;
        }
        const std::optional<platform::ScreenInfo> info = platform::GetScreenInfo();
        if (!info.has_value()) {
            view_body_top.reset();
            return;  // 拿不到屏幕信息:退回旧行为,不硬擦
        }
        int top = *view_body_top;
        if (top < info->viewport_y) {
            top = info->viewport_y;  // 只管当前 viewport,滚屏历史不动
        }
        for (int y = top; y < info->height; ++y) {
            platform::ClearRowHardFrom(0, y, info->width);
        }
        platform::SetCursorPos(0, top);
        view_body_top.reset();
    };
    // 擦旧帧与铺新帧永远成对(先擦净再落帧,钩子只打印):调这一个就够,
    // 不给"只打印不擦"或"只擦不重记"留缝。view_hook 前取一次光标:正文顶行
    // 落在打印前的光标处,新帧从这里开始,也是下一次切换要擦的顶。
    // tail_rows>0 是实时流的重铺拍(只铺头几行+最近 N 行,见 console_input.hpp
    // 的钩子注释)。
    const auto print_view_frame = [&](int viewed_after, int tail_rows = 0) {
        erase_previous_view_body();
        const std::optional<platform::ScreenInfo> before = platform::GetScreenInfo();
        const auto& view_hook = AgentViewSwitchHookSlot();
        if (view_hook) {
            view_hook(viewed_after, tail_rows);
        }
        if (before.has_value()) {
            view_body_top = before->cursor_y;
        }
    };

    if (box) {
        // 进门先记查看态:第一帧 build_dock 会 OnEntriesChanged,查看的任务
        // 若在两段读取之间退场(后台回流轮置 delivered、x 清条目后重进),
        // viewed 在这一帧里翻 0——翻转发生在帧内,下面 100ms 拍的
        // viewed_before_tick 已经读不到旧值,原子回 main 得在这里补上。
        const int viewed_before_entry = panel_session.SnapshotFor(nav_ids_for(panel_entries())).viewed_task_id;
        redraw_with_panel(editor.CurrentRenderState(), panel_entries());
        const int viewed_after_entry = panel_session.SnapshotFor(nav_ids_for(panel_entries())).viewed_task_id;
        if (viewed_before_entry != 0 && viewed_after_entry == 0) {
            // 原子回 main(规格"现场一/四"),与 tick 路同义。但锚点跨调用
            // 不可信(期间可能整轮流式滚过屏),改走 Ctrl+L 的办法:清整个
            // 可视区、main 帧重铺、重锚整帧重画——不按绝对行号硬擦,滚屏
            // 历史照旧不清。回流轮的新输出已在 transcript 里,重铺时带出。
            const std::optional<platform::ScreenInfo> clear_info = platform::GetScreenInfo();
            if (clear_info.has_value()) {
                const int vh =
                    clear_info->viewport_height > 0 ? clear_info->viewport_height : clear_info->height;
                const int top = clear_info->viewport_y;
                for (int y = top; y < top + vh && y < clear_info->height; ++y) {
                    platform::ClearRowHardFrom(0, y, clear_info->width);
                }
                platform::SetCursorPos(0, top);
                view_body_top.reset();
                print_view_frame(0);
            }
            reanchor_prompt_and_redraw();
        } else if (viewed_before_entry != 0 && viewed_after_entry != 0) {
            // 跨读取段仍是查看态(流式监听里切看后回合收口、查看态提交介入
            // 消息后重进 composer……):上一段记的帧锚点在这段里不可信,而
            // 本段的 view_body_top 还是空的——不重铺的话,下一拍实时流重铺
            // 会在旧帧下方再铺一份,查看帧成双。进门就把可视区整块清掉、该
            // 代理的查看帧整份重铺,帧账从这一帧重新起(与上面退场回 main
            // 同一条"不认跨拍锚点"的规矩)。
            const std::optional<platform::ScreenInfo> clear_info = platform::GetScreenInfo();
            if (clear_info.has_value()) {
                const int vh =
                    clear_info->viewport_height > 0 ? clear_info->viewport_height : clear_info->height;
                const int top = clear_info->viewport_y;
                for (int y = top; y < top + vh && y < clear_info->height; ++y) {
                    platform::ClearRowHardFrom(0, y, clear_info->width);
                }
                platform::SetCursorPos(0, top);
                view_body_top.reset();
                print_view_frame(viewed_after_entry);
                reanchor_prompt_and_redraw();
            }
        }
    }

    // Ctrl+L 与 resize 共用的整屏重建(规格"整屏重画"节):作废帧锚点、清
    // 可视区(不清回滚历史)、请应用层重铺 transcript、重打提示符与底栏。
    // composer 草稿、面板选择、查看态与收件目标全在状态里,一个都不丢;
    // 重复行随整帧重画归零。resize 后 conhost 会把宽行重排成多行,旧锚点
    // 全部失准,增量 diff 修不回来——只有整屏重建一条路干净。
    const auto rebuild_screen = [&]() {
        const std::optional<platform::ScreenInfo> clear_info = platform::GetScreenInfo();
        if (!clear_info.has_value()) {
            return;
        }
        const int vh = clear_info->viewport_height > 0 ? clear_info->viewport_height : clear_info->height;
        const int top = clear_info->viewport_y;
        for (int y = top; y < top + vh && y < clear_info->height; ++y) {
            platform::ClearRowHardFrom(0, y, clear_info->width);
        }
        platform::SetCursorPos(0, top);
        // 正文重铺:查看态铺该代理的消息账(view_frame),main 态从 transcript
        // 快照重铺——resize/reflow/整屏重建后,正在看的会话不漂(规格销单线 8)。
        const int viewed_now = panel_session.SnapshotFor(nav_ids_for(panel_entries())).viewed_task_id;
        if (viewed_now != 0) {
            view_body_top.reset();  // 旧锚点随整屏清一起作废,view_frame 会重记
            print_view_frame(viewed_now);
        } else if (UiHandlerSlot()) {
            (void)UiHandlerSlot()(UiKeyAction::RepaintScreen);  // 正文从 transcript 快照重铺
        }
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
        redraw_with_panel(editor.CurrentRenderState(), panel_entries());
    };

    // resize 探测:宽高一变就走整屏重建(下一拍内完成),不能等指纹——
    // 尺寸不在指纹里,而 conhost 重排过的旧行靠增量 diff 擦不净。
    int last_screen_width = -1;
    int last_screen_height = -1;

    // 查看态实时流(追加需求"查看态实时思考流"):正看一只运行中子代理、
    // 它的内容修订号动了(思考/正文增量、事件追加、阶段翻页),按 1s 节流
    // 重铺查看帧——retained view 原地擦旧帧、铺"头三行+最近 N 行",滚屏
    // 历史不刷屏。切进查看态那一拍记下起始修订号,免得刚切完就白铺一遍。
    std::uint64_t live_view_revision = 0;
    std::chrono::steady_clock::time_point live_view_last_refresh{};
    const auto note_view_revision = [&](const std::vector<AgentPanelEntry>& entries, int viewed_task_id) {
        live_view_revision = 0;
        if (viewed_task_id == 0) {
            return;
        }
        for (const auto& entry : entries) {
            if (entry.task_id == viewed_task_id) {
                live_view_revision = entry.content_revision;
                break;
            }
        }
    };

    while (true) {
        // ReadOne 原本会一直堵到下一枚按键，后台代理即便完成，面板也只会
        // 在用户敲键后才变。100ms 探一次队列；没键就只重画这一帧。
        if (!platform::WaitForKeyEvent(100)) {
            if (const std::optional<platform::ScreenInfo> size_info = platform::GetScreenInfo();
                size_info.has_value()) {
                if (last_screen_width != -1 &&
                    (size_info->width != last_screen_width || size_info->height != last_screen_height)) {
                    rebuild_screen();
                }
                last_screen_width = size_info->width;
                last_screen_height = size_info->height;
            }
            // 后台代理权限拒绝等"当场要让人知道"的通知:应用层这一拍取走
            // 攒着的,自己落 toast 与 transcript 事件(终端层只叫一声,不管
            // 通知内容)。放在帧构建之前,toast 当拍就能进这一帧的坞区。
            if (const auto& notice_hook = BackgroundNoticeHookSlot()) {
                notice_hook();
            }
            const auto entries = panel_entries();
            const int viewed_before_tick = panel_session.SnapshotFor(nav_ids_for(entries)).viewed_task_id;
            const bool armed_expired = panel_session.ExpireArmed(std::chrono::steady_clock::now());
            std::string tag;
            const std::vector<std::string> dock = build_dock(entries, tag);
            const std::vector<std::string> queue_rows = queue_rows_now();
            const AgentPanelSession::Snapshot snapshot = panel_session.SnapshotFor(nav_ids_for(entries));
            RenderState tick_state = editor.CurrentRenderState();
            apply_search_hints(tick_state);  // 搜索行进指纹,与真画的那份同一账
            const BottomChromeFrame frame = build_frame(queue_rows, dock, tick_state,
                                                         snapshot.selected_task_id);
            if (viewed_before_tick != 0 && snapshot.viewed_task_id == 0) {
                // 正在查看的任务完成退场(结果交回 main)/被清理:原子回
                // main——上方视口换源、composer 目标回 main、选择落相邻运行
                // 项,全在这一拍办完;旧代理正文先擦净再落 main 帧,不得继续
                // 躺在视口里冒充当前会话(规格"现场一/四")。完成结果的短行
                // 由主循环投递路记一条有归属的 transcript 事件,这里不重复报。
                //
                // 退场清屏不认任何跨拍锚点(与进门路同款):查看期间实时流
                // 每 ≥1s 重铺一拍,重铺后的滚屏/平移会把上一拍记的绝对行号
                // 全弄失准,按账硬擦轻则漏擦留残影、重则擦到别处(查看态完
                // 成退场整屏空白卡死单,2026-08-17)。直接把当前可视区整块清
                // 干净、main 帧整帧重铺——滚屏历史照旧不清,结果一行不丢。
                retire_idle_chrome();
                const std::optional<platform::ScreenInfo> clear_info = platform::GetScreenInfo();
                if (clear_info.has_value()) {
                    const int vh =
                        clear_info->viewport_height > 0 ? clear_info->viewport_height : clear_info->height;
                    const int top = clear_info->viewport_y;
                    for (int y = top; y < top + vh && y < clear_info->height; ++y) {
                        platform::ClearRowHardFrom(0, y, clear_info->width);
                    }
                    platform::SetCursorPos(0, top);
                    view_body_top.reset();  // 旧帧随整块清屏作废,print_view_frame 会重记
                }
                print_view_frame(0);
                reanchor_prompt_and_redraw();
                continue;
            }
            // 实时流判定(追加需求):查看目标还在导航表里、修订号动了、距上
            // 次重铺 ≥1s——重铺"头三行+尾部"一帧(reanchor_prompt_and_redraw
            // 收尾自带整帧重画与指纹记账)。终态任务的收尾事件也会动修订号,
            // 自然重铺一次终局(此后不再动)。
            if (snapshot.viewed_task_id != 0) {
                std::uint64_t viewed_revision = 0;
                for (const auto& entry : entries) {
                    if (entry.task_id == snapshot.viewed_task_id) {
                        viewed_revision = entry.content_revision;
                        break;
                    }
                }
                const auto now_tick = std::chrono::steady_clock::now();
                if (live_view_last_refresh.time_since_epoch().count() == 0) {
                    live_view_last_refresh = now_tick;  // 进查看态后的首拍只记时不铺
                } else if (viewed_revision != 0 && viewed_revision != live_view_revision &&
                           now_tick - live_view_last_refresh >= std::chrono::seconds(1)) {
                    live_view_revision = viewed_revision;
                    live_view_last_refresh = now_tick;
                    const std::optional<platform::ScreenInfo> live_info = platform::GetScreenInfo();
                    const int viewport_rows = live_info.has_value()
                                                  ? (std::max)(6, live_info->viewport_height -
                                                                      live_info->viewport_height / 4)
                                                  : 20;
                    retire_idle_chrome();
                    print_view_frame(snapshot.viewed_task_id, viewport_rows);
                    reanchor_prompt_and_redraw();
                    continue;
                }
            }
            if (armed_expired || fingerprint_of(entries, frame, snapshot) != panel_fingerprint) {
                redraw_with_panel(editor.CurrentRenderState(), entries);
            }
            // 空闲唤醒:系统侧有事件(后台子代理跑完等)要在会话空闲时处理。
            // composer 空着才让位——用户敲了一半的正文不抢;让位前先正式
            // 收束旧底栏帧(RetireIdleChrome),空串返回,调用方循环顶自会去
            // 办,办完回来重新给提示符(IdleComposer 再起)。
            if (composer && editor.CurrentRenderState().line.empty()) {
                const auto& wake = IdleWakeHookSlot();
                if (wake && wake()) {
                    if (queue_edit.has_value()) {
                        steering.CancelEdit(*queue_edit);
                        queue_edit.reset();
                    }
                    retire_idle_chrome();
                    std::cout << "\n";
                    if (exit_reason != nullptr) {
                        *exit_reason = ReadExitReason::Submitted;
                    }
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
            if (exit_reason != nullptr) {
                *exit_reason = ReadExitReason::Cancel;
            }
            return std::nullopt;  // EOF/读失败
        }
        const auto entries_before_key = panel_entries();

        // ---- Composer 域动作(0.30.x 第二/三批;键位全走 keymap) ----
        // 搜索开着时整个键盘优先归它:命中的动作键(更早/换范围/接受/提交/
        // 取消)与 ↑/↓ 选位都在这里消费;其余键(打字、退格、左右移)落到
        // 编辑器当查询输入,命中清单随重画自然刷新。
        if (composer) {
            const auto search_chord = keymap::ChordFromKeyInput(*raw_key);
            if (!history_search.active() && !queue_edit.has_value() && search_chord.has_value()) {
                using keymap::ActionId;
                switch (keymap::ActiveKeymap().Lookup(keymap::KeyScope::Composer, *search_chord)) {
                    case ActionId::ChatSearchHistory: {
                        // Ctrl+R 反向搜索:原草稿整份存起(取消时一字不少装
                        // 回),编辑器清成空查询。没有数据源(单发/管道/未注册)
                        // 时这个键不消费,落回编辑器原语义。
                        if (!PromptHistoryProviderSlot()) {
                            break;
                        }
                        history_search_draft = Utf32ToUtf8(editor.CurrentRenderState().line);
                        history_search.Open(PromptHistoryProviderSlot()(), HistorySearchScope::Session);
                        history_search_last_query.clear();
                        editor.BeginLine(/*composer=*/true);
                        redraw_with_panel(editor.CurrentRenderState(), entries_before_key);
                        continue;
                    }
                    case ActionId::ChatExternalEditor: {
                        // Ctrl+G 外部编辑器:草稿写临时文件,$VISUAL/$EDITOR
                        // 继承控制台编辑;读回保多行,失败原草稿一字不动。
                        run_external_editor();
                        continue;
                    }
                    case ActionId::ComposerStash: {
                        // 草稿收起/取回(一格):存下收件目标与 cwd,取回对
                        // 不上就拒,给甲写的话不送给乙。
                        const RenderState now = editor.CurrentRenderState();
                        ComposerStashSnapshot& stash = ComposerStashSlot();
                        if (stash.has) {
                            const std::optional<int> target = GetComposerTarget();
                            const std::string cwd_now =
                                lubancode::tools::PathToUtf8(std::filesystem::current_path());
                            if (stash.target_task_id != target.value_or(0) || stash.cwd != cwd_now) {
                                panel_notice = tr("stash.restore_refused");
                            } else {
                                editor.LoadText(Utf8ToUtf32(stash.text));
                                ComposerStashSlot() = ComposerStashSnapshot{};
                                panel_notice = tr("stash.restored");
                            }
                        } else if (now.line.empty()) {
                            panel_notice = tr("stash.empty");
                        } else {
                            stash.has = true;
                            stash.text = Utf32ToUtf8(now.line);
                            stash.target_task_id = GetComposerTarget().value_or(0);
                            stash.cwd = lubancode::tools::PathToUtf8(std::filesystem::current_path());
                            editor.BeginLine(/*composer=*/true);
                            panel_notice = tr("stash.stashed");
                        }
                        panel_notice_until = std::chrono::steady_clock::now() + std::chrono::seconds(3);
                        redraw_with_panel(editor.CurrentRenderState(), entries_before_key);
                        continue;
                    }
                    case ActionId::HelpShow: {
                        // '?' 场景帮助:只列当前场景有效键,不倒整本 /help;
                        // 键位从 keymap 反查,用户改键后提示跟着改。空
                        // composer 才当帮助,有正文时 '?' 是普通字符。
                        if (!editor.CurrentRenderState().line.empty()) {
                            break;
                        }
                        retire_idle_chrome();
                        std::cout << "\n";
                        {
                            std::lock_guard<std::mutex> stdout_lock(StdoutWriteMutex());
                            std::cout << theme.tool_line << tr("help.scene_header") << theme.reset << "\n";
                            for (const auto& record : keymap::ActiveKeymap().AllBindings()) {
                                if (record.scope == keymap::KeyScope::Streaming) {
                                    continue;  // 流式脚注那批不属"当前场景"(空闲 composer)
                                }
                                const std::string chord_text =
                                    record.has_default ? keymap::FormatKeyChord(record.chord) : "-";
                                std::cout << theme.stats << "  " << chord_text;
                                for (int pad = static_cast<int>(chord_text.size()); pad < 12; ++pad) {
                                    std::cout << ' ';
                                }
                                std::cout << keymap::ActionName(record.action);
                                if (!record.bindable) {
                                    std::cout << tr("help.fixed_suffix");
                                } else if (!record.has_default) {
                                    std::cout << tr("help.unbound_suffix");
                                }
                                std::cout << theme.reset << "\n";
                            }
                            std::cout << theme.stats << tr("help.scene_footer") << theme.reset << "\n";
                            std::cout.flush();
                        }
                        reanchor_prompt_and_redraw();
                        continue;
                    }
                    case ActionId::TranscriptPrevUserTurn:
                    case ActionId::TranscriptNextUserTurn:
                    case ActionId::TranscriptToScrollback:
                    case ActionId::TranscriptViewInEditor: {
                        // 转录导航(空 composer 的 { } [ v):有正文时全是
                        // 普通字符。动作交应用层(HandleTranscriptUi),没
                        // 人认(空会话)就补帧了事。
                        if (!editor.CurrentRenderState().line.empty() || search_chord->ctrl ||
                            search_chord->alt) {
                            break;
                        }
                        UiKeyAction action = UiKeyAction::PrevUserTurn;
                        switch (keymap::ActiveKeymap().Lookup(keymap::KeyScope::Composer, *search_chord)) {
                            case ActionId::TranscriptPrevUserTurn: action = UiKeyAction::PrevUserTurn; break;
                            case ActionId::TranscriptNextUserTurn: action = UiKeyAction::NextUserTurn; break;
                            case ActionId::TranscriptToScrollback: action = UiKeyAction::ToScrollback; break;
                            case ActionId::TranscriptViewInEditor: action = UiKeyAction::ViewInEditor; break;
                            default: break;
                        }
                        retire_idle_chrome();
                        std::cout << "\n";
                        if (UiHandlerSlot()) {
                            (void)UiHandlerSlot()(action);
                        }
                        reanchor_prompt_and_redraw();
                        continue;
                    }
                    case ActionId::ImagePasteClipboard: {
                        // Alt+V 剪贴板位图直贴(0.30.x 第三批):取剪贴板图,
                        // PNG 落受限临时文件,光标处插 @路径(提交时走既有
                        // 视觉附件路,尺寸/超限校验在那头还有一道)。无图/
                        // 超限/平台不支持明报错,不暗降糊图,原草稿不动。
                        std::string paste_error;
                        const auto png =
                            platform::ReadClipboardImagePng(kMaxImageBytes, paste_error);
                        if (!png.has_value()) {
                            std::cout << theme.error
                                      << trf("image.paste_failed", paste_error)
                                      << theme.reset << "\n";
                            redraw_with_panel(editor.CurrentRenderState(), entries_before_key);
                            continue;
                        }
                        namespace fs = std::filesystem;
                        static unsigned paste_seq = 0;
                        fs::path file;
                        try {
                            file = fs::temp_directory_path() /
                                   ("lubancode-paste-" + std::to_string(platform::CurrentProcessId()) +
                                    "-" + std::to_string(++paste_seq) + ".png");
                        } catch (const std::exception&) {
                            std::cout << theme.error << tr("editor.no_temp") << theme.reset << "\n";
                            continue;
                        }
                        {
                            std::ofstream out(file, std::ios::binary | std::ios::trunc);
                            if (!out) {
                                std::cout << theme.error << tr("editor.write_failed") << theme.reset << "\n";
                                continue;
                            }
                            out.write(reinterpret_cast<const char*>(png->data()),
                                      static_cast<std::streamsize>(png->size()));
                        }
                        // 光标处插 @<路径>(临时路径常带空格,一律角括号形)。
                        const RenderState before = editor.CurrentRenderState();
                        const std::string token =
                            "@<" + lubancode::tools::PathToUtf8(file) + "> ";
                        std::string joined = Utf32ToUtf8(before.line);
                        const std::size_t at = (std::min)(before.cursor, joined.size());
                        joined.insert(at, token);
                        editor.LoadTextWithCursor(Utf8ToUtf32(joined), at + token.size());
                        std::cout << theme.stats
                                  << trf("image.pasted", png->size() / 1024,
                                         lubancode::tools::PathToUtf8(file))
                                  << theme.reset << "\n";
                        redraw_with_panel(editor.CurrentRenderState(), entries_before_key);
                        continue;
                    }
                    default:
                        break;
                }
            } else if (history_search.active() && search_chord.has_value()) {
                using keymap::ActionId;
                const auto search_action =
                    keymap::ActiveKeymap().Lookup(keymap::KeyScope::Search, *search_chord);
                const auto finish_search = [&](bool submit) -> std::optional<std::string> {
                    const PromptHistoryEntry* entry = history_search.SelectedEntry();
                    if (entry != nullptr) {
                        editor.LoadText(Utf8ToUtf32(entry->text));  // 取回即新草稿,不改任何原件
                    }
                    history_search.Close();
                    if (!submit) {
                        return std::nullopt;
                    }
                    // Enter 接受并提交:合成一次 Enter 走编辑器的正常提交路
                    // (历史账、提交帧全按老规矩),收尾与下面 box 提交同款。
                    const RenderState state = editor.HandleKey(KeyEvent::Simple(KeyKind::Enter));
                    if (box) {
                        CollapseBoxOnSubmit(prev_frame_origin, prompt_end_col, prev_body_row_count,
                                            state, prompt, vt_enabled);
                    }
                    std::cout << "\n";
                    if (exit_reason != nullptr) {
                        *exit_reason = ReadExitReason::Submitted;
                    }
                    return Utf32ToUtf8(state.line);
                };
                switch (search_action) {
                    case ActionId::SearchOlder:
                        history_search.MoveOlder();
                        redraw_with_panel(editor.CurrentRenderState(), entries_before_key);
                        continue;
                    case ActionId::SearchScopeCycle: {
                        history_search.CycleScope();
                        history_search.Rerun(Utf32ToUtf8(editor.CurrentRenderState().line));
                        history_search_last_query = Utf32ToUtf8(editor.CurrentRenderState().line);
                        redraw_with_panel(editor.CurrentRenderState(), entries_before_key);
                        continue;
                    }
                    case ActionId::SearchAccept:
                        // Tab/Esc:接受命中回 composer 继续改;没有命中就当
                        // 收起搜索还原草稿(与取消同效,不丢已敲的查询以外的东西)。
                        if (history_search.SelectedEntry() == nullptr) {
                            editor.LoadText(Utf8ToUtf32(history_search_draft));
                        } else {
                            (void)finish_search(/*submit=*/false);
                        }
                        history_search.Close();
                        redraw_with_panel(editor.CurrentRenderState(), entries_before_key);
                        continue;
                    case ActionId::SearchAcceptSubmit: {
                        if (const auto submitted = finish_search(/*submit=*/true);
                            submitted.has_value()) {
                            return *submitted;
                        }
                        redraw_with_panel(editor.CurrentRenderState(), entries_before_key);
                        continue;
                    }
                    case ActionId::SearchCancel:
                        editor.LoadText(Utf8ToUtf32(history_search_draft));  // 原草稿一字不少
                        history_search.Close();
                        redraw_with_panel(editor.CurrentRenderState(), entries_before_key);
                        continue;
                    default:
                        break;
                }
                // ↑/↓ 在命中条目间走(核心方向键,不是和弦)。
                if (raw_key->kind == platform::KeyInput::Kind::Up) {
                    history_search.MoveOlder();
                    redraw_with_panel(editor.CurrentRenderState(), entries_before_key);
                    continue;
                }
                if (raw_key->kind == platform::KeyInput::Kind::Down) {
                    history_search.MoveNewer();
                    redraw_with_panel(editor.CurrentRenderState(), entries_before_key);
                    continue;
                }
            }
        }


        // ---- @ 提及菜单(0.30.x 第三批):方向键走、Enter/Tab 选中插入、
        // Esc 只收菜单。菜单开着时 Enter 只插入不提交(防误发),Tab 也不
        // 进焦点态(composer 必然非空)。----
        if (composer && !history_search.active()) {
            const RenderState mention_preview = editor.CurrentRenderState();
            if (const auto* mention_matches = mention_menu_for(mention_preview);
                mention_matches != nullptr) {
                using PK = platform::KeyInput::Kind;
                if (raw_key->kind == PK::Up || raw_key->kind == PK::Down) {
                    if (raw_key->kind == PK::Up) {
                        if (mention_selected > 0) {
                            --mention_selected;
                        }
                    } else if (mention_selected + 1 < static_cast<int>(mention_matches->size())) {
                        ++mention_selected;
                    }
                    redraw_with_panel(mention_preview, entries_before_key);
                    continue;
                }
                if (raw_key->kind == PK::Enter || raw_key->kind == PK::Tab) {
                    const auto token =
                        FindMentionToken(mention_preview.lines[mention_preview.cursor_row],
                                         mention_preview.cursor_col);
                    if (token.has_value()) {
                        const FileMentionEntry& entry =
                            mention_entries[(*mention_matches)[mention_selected]];
                        const std::string insertion = MentionInsertionString(entry) + " ";
                        // 整份重装:替换光标行,光标落词元尾(替换串末尾)。
                        std::u32string joined;
                        std::size_t cursor_abs = 0;
                        for (std::size_t r = 0; r < mention_preview.lines.size(); ++r) {
                            if (r > 0) {
                                joined.push_back(U'\n');
                            }
                            if (r == mention_preview.cursor_row) {
                                joined += ReplaceMentionToken(
                                    mention_preview.lines[mention_preview.cursor_row], *token, insertion);
                                cursor_abs = joined.size();
                            } else {
                                joined += mention_preview.lines[r];
                            }
                        }
                        editor.LoadTextWithCursor(joined, cursor_abs);
                    }
                    mention_dismissed = true;  // 插完收菜单;再敲 @ 会重开
                    redraw_with_panel(editor.CurrentRenderState(), entries_before_key);
                    continue;
                }
                if (raw_key->kind == PK::Esc) {
                    mention_dismissed = true;  // 只收菜单,不清行、不打断
                    redraw_with_panel(mention_preview, entries_before_key);
                    continue;
                }
            }
        }

        // ---- Ctrl+L 整屏重画(规格"整屏重画"节):从状态重建,不吞输入 ----
        // 与 resize 共用 rebuild_screen。composer 草稿、面板选择、查看态与
        // 收件目标全在状态里,一个都不丢;重复行随整帧重画归零。管道/plain
        // 场景走不到逐键路径,天然无感。
        if (composer && raw_key->kind == platform::KeyInput::Kind::CtrlL) {
            rebuild_screen();
            continue;
        }

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
                const int viewed_before =
                    panel_session.SnapshotFor(nav_ids_for(entries_before_key)).viewed_task_id;
                const auto outcome = panel_session.HandleKey(*panel_key, nav_ids_for(entries_before_key),
                                                             empty_now, std::chrono::steady_clock::now());
                if (outcome.stop_all) {
                    const AgentPanelActions& actions = AgentPanelActionsSlot();
                    if (actions.cancel_all) {
                        const int stopped = actions.cancel_all();
                        panel_notice = trf("agent_panel.stop_all_notice", stopped);
                        panel_notice_until =
                            std::chrono::steady_clock::now() + std::chrono::seconds(2);
                    }
                }
                if (outcome.stop_current && outcome.stop_current_task_id > 0) {
                    // 动作目标按稳定 task id 分派(不按下标回查,列表重排也打不
                    // 到邻居):运行中停止,终态清除。
                    const AgentPanelEntry* selected_entry = nullptr;
                    for (const auto& entry : entries_before_key) {
                        if (entry.task_id == outcome.stop_current_task_id) {
                            selected_entry = &entry;
                            break;
                        }
                    }
                    const AgentPanelActions& actions = AgentPanelActionsSlot();
                    if (selected_entry != nullptr) {
                        if (selected_entry->running && actions.cancel_task) {
                            actions.cancel_task(selected_entry->task_id);
                        } else if (!selected_entry->running && actions.clear_task) {
                            actions.clear_task(selected_entry->task_id);
                        }
                    }
                }
                if (outcome.consumed || outcome.redraw) {
                    const int viewed_after =
                        panel_session.SnapshotFor(nav_ids_for(panel_entries())).viewed_task_id;
                    if (outcome.consumed && viewed_after != viewed_before) {
                        // 真切会话(规格"现场一/四"):Enter 设 viewed_task_id / Esc
                        // 清零,上方视口整块换源。换源前先正式收束旧底栏帧
                        // (RetireIdleChrome:整帧硬清、帧账归零),再把上一视图
                        // 正文从可视区擦净(retained view:旧帧先擦,新帧原位
                        // 落——Esc 回 main 后代理正文不再占着 viewport 冒充活
                        // 状态);随后请应用层铺出"此刻该看的 transcript",重打
                        // 提示符、重锚、整帧重画。Enter 只切视图,草稿不提交
                        // (能进到这的 Enter 必然发生在 composer 为空时)。
                        retire_idle_chrome();
                        print_view_frame(viewed_after);
                        note_view_revision(panel_entries(), viewed_after);
                        live_view_last_refresh = std::chrono::steady_clock::now();
                        reanchor_prompt_and_redraw();
                        continue;
                    }
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
        if (!state.line.empty() && panel_session.SnapshotFor(nav_ids_for(panel_entries())).viewed_task_id == 0) {
            // 敲了正文即离开面板焦点(上下键归历史);查看态例外——那只标签
            // 还挂着,话要送去那只子代理。
            panel_session.Reset();
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
                // 查看态里的 Ctrl+O 是"查看帧重铺拍"(HandleTranscriptUi 会整份
                // 重铺那只子代理的 transcript):与真切会话同款,先收底栏、按
                // view_body_top 这本账把旧查看帧擦净——app 侧只打印不擦,不在这
                // 再开第二本账(查看态完成退场花屏单)。
                const bool view_relay = *action == UiKeyAction::ToggleExpand && CurrentAgentViewedTaskId() != 0;
                std::optional<int> relay_frame_top;
                if (view_relay) {
                    retire_idle_chrome();
                    erase_previous_view_body();
                    if (const std::optional<platform::ScreenInfo> relay_info = platform::GetScreenInfo();
                        relay_info.has_value()) {
                        relay_frame_top = relay_info->cursor_y;
                    }
                } else if (const std::optional<platform::ScreenInfo> before_info = platform::GetScreenInfo();
                           before_info.has_value()) {
                    int last_row = start_row + prev_body_row_count;
                    if (last_row >= before_info->height) {
                        last_row = before_info->height - 1;
                    }
                    platform::SetCursorPos(0, last_row);
                }
                const bool handled = UiHandlerSlot()(*action);
                if (handled) {
                    if (relay_frame_top.has_value()) {
                        view_body_top = relay_frame_top;  // 重铺帧的顶,下一次切换照账擦
                    }
                    // 回调铺完内容,重打提示符、重测锚点,整帧(含导航坞)重画。
                    reanchor_prompt_and_redraw();
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
            if (exit_reason != nullptr) {
                *exit_reason = ReadExitReason::Submitted;
            }
            return Utf32ToUtf8(state.line);
        }

        redraw_with_panel(state, entries_before_key);

        if (state.esc_pressed && esc_rejects) {
            // 确认与可取消选择场景:Esc 不留在循环里继续等，直接交回
            // nullopt。不能拿空串代替——/model 明明把空串当默认第一项。
            std::cout << "\n";
            if (exit_reason != nullptr) {
                *exit_reason = ReadExitReason::Esc;
            }
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
            if (exit_reason != nullptr) {
                *exit_reason = ReadExitReason::Cancel;
            }
            return std::nullopt;
        }
        if (state.cleared) {
            continue;  // composer 已经清空,继续在同一次调用里编辑
        }
        if (state.submitted) {
            // 非框读取的提交:RedrawEditArea 刚按提交帧把完整多行内容画在
            // 屏幕上、光标停在末行末尾,这里换行收尾。
            std::cout << "\n";
            if (exit_reason != nullptr) {
                *exit_reason = ReadExitReason::Submitted;
            }
            return Utf32ToUtf8(state.line);
        }
    }
}

}  // namespace

std::optional<std::string> ReadLine(const std::string& prompt, const Theme& theme, bool esc_rejects, bool composer,
                                    ReadExitReason* exit_reason) {
    if (platform::StdinIsInteractive()) {
        return ReadLineKeyByKey(prompt, theme, esc_rejects, composer, exit_reason);
    }
    (void)composer;  // 管道/重定向:没有 composer 概念,照旧逐行 getline

    if (!prompt.empty()) {
        std::cout << prompt;
        std::cout.flush();
    }
    std::string line;
    if (!std::getline(std::cin, line)) {
        if (exit_reason != nullptr) {
            *exit_reason = ReadExitReason::Cancel;
        }
        return std::nullopt;
    }
    StripTrailingCrLf(line);
    if (exit_reason != nullptr) {
        *exit_reason = ReadExitReason::Submitted;
    }
    return line;
}

std::optional<ChoiceMenuResult> ReadChoiceMenu(const std::vector<ChoiceMenuItem>& items,
                                                const ChoiceMenuOptions& options, const Theme& theme,
                                                ReadExitReason* exit_reason) {
    if (items.empty() || !platform::StdinIsInteractive()) {
        if (exit_reason != nullptr) {
            *exit_reason = ReadExitReason::Cancel;
        }
        return std::nullopt;
    }
    std::lock_guard<std::mutex> console_read_lock(ConsoleReadMutex());
    platform::RawInputScope raw_scope;
    if (!raw_scope.ok()) {
        if (exit_reason != nullptr) {
            *exit_reason = ReadExitReason::Cancel;
        }
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
    // 取消是哪个键按的:Esc(向导当"返回上一步")还是 Ctrl+C/Ctrl+D(取消
    // 整条流程)。draw() 失败那类环境性早退按 Cancel 报,不区分。
    bool cancel_was_esc = false;
    while (!menu.state().submitted && !menu.state().cancelled) {
        const std::optional<platform::KeyInput> raw_key = key_reader.ReadOne();
        if (!raw_key.has_value()) {
            clear();
            if (exit_reason != nullptr) {
                *exit_reason = ReadExitReason::Cancel;
            }
            return std::nullopt;
        }
        const std::optional<KeyEvent> mapped = MapKey(*raw_key);
        if (!mapped.has_value()) {
            continue;
        }
        menu.HandleKey(*mapped);
        if (menu.state().cancelled && mapped->kind == KeyKind::Esc) {
            cancel_was_esc = true;
        }
        if (!menu.state().submitted && !menu.state().cancelled) {
            if (!draw()) {
                clear();
                if (exit_reason != nullptr) {
                    *exit_reason = ReadExitReason::Cancel;
                }
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
    if (exit_reason != nullptr) {
        *exit_reason = cancelled ? (cancel_was_esc ? ReadExitReason::Esc : ReadExitReason::Cancel)
                                 : ReadExitReason::Submitted;
    }
    return cancelled ? std::nullopt : std::optional<ChoiceMenuResult>(std::move(result));
}

ConfirmMode CurrentConfirmMode() { return SharedEditor().confirm_mode(); }

void SetConfirmMode(ConfirmMode mode) { SharedEditor().set_confirm_mode(mode); }

void SetTranscriptUiHandler(TranscriptUiHandler handler) { UiHandlerSlot() = std::move(handler); }

void SetAgentPanelProvider(AgentPanelProvider provider) { AgentPanelProviderSlot() = std::move(provider); }

void SetAgentViewSwitchHook(std::function<void(int viewed_task_id, int tail_rows)> hook) {
    AgentViewSwitchHookSlot() = std::move(hook);
}

void SetAgentPanelActions(AgentPanelActions actions) { AgentPanelActionsSlot() = std::move(actions); }

void ResetAgentPanelSession() { PanelSessionSlot().Reset(); }

std::optional<int> CurrentComposerAgentTarget() { return GetComposerTarget(); }

int CurrentAgentViewedTaskId() {
    // 会话层面板控制器的真状态,不跟某次 ReadLine 的生命周期:主循环在两次
    // 读取之间(回流轮前后)问它,答案必须仍然准确。
    return PanelSessionSlot().SnapshotFor({}).viewed_task_id;
}

void ShowPanelToast(const std::string& text) {
    PanelToastSlot().text = text;
    PanelToastSlot().until = std::chrono::steady_clock::now() + std::chrono::seconds(4);
}

void SetIdleWakeHook(std::function<bool()> hook) { IdleWakeHookSlot() = std::move(hook); }

void SetBackgroundNoticeHook(std::function<void()> hook) { BackgroundNoticeHookSlot() = std::move(hook); }

void SetPromptHistoryProvider(PromptHistoryProvider provider) {
    PromptHistoryProviderSlot() = std::move(provider);
}

bool ComposerStashHasContent() { return ComposerStashSlot().has; }

ComposerStashSnapshot ComposerStashPeek() { return ComposerStashSlot(); }

void ComposerStashDiscard() { ComposerStashSlot() = ComposerStashSnapshot{}; }

void SetFileMentionProvider(FileMentionProvider provider) {
    FileMentionProviderSlot() = std::move(provider);
}

std::string NormalizeEditorDraft(std::string bytes) {
    // CRLF / 裸 CR 归一成 '\n';编辑器普遍在文件尾补一个换行,读回时剥
    // 掉那一个(用户真想留空末行会留两个——剥一个不伤)。
    std::string out;
    out.reserve(bytes.size());
    for (std::size_t i = 0; i < bytes.size(); ++i) {
        if (bytes[i] == '\r' && i + 1 < bytes.size() && bytes[i + 1] == '\n') {
            continue;  // "\r\n" 里的 '\r' 丢弃,下一轮收 '\n'
        }
        out.push_back(bytes[i] == '\r' ? '\n' : bytes[i]);
    }
    if (!out.empty() && out.back() == '\n') {
        out.pop_back();
    }
    return out;
}

void SetStatusLineData(const StatusPanelData& values, const std::vector<std::string>& items,
                       const std::string& separator) {
    StatusLineData& data = StatusDataSlot();
    data.values = values;
    data.items = items;
    data.separator = separator;
}

void UpdateStatusLineContext(int context_percent, std::int64_t used_tokens, std::int64_t window_tokens,
                             bool measured, const std::string& cache_note) {
    // 见 console_input.hpp 的注释:只改数据、不落笔,footer 的重画事务在
    // 安全时机(下一笔正文/ticker 一拍/挂起恢复)取新值。锁跟 footer 重画
    // 读的是同一把,发布与重画互不越界。
    std::lock_guard<std::mutex> lock(StdoutWriteMutex());
    StatusLineData& data = StatusDataSlot();
    data.values = WithContextUpdate(data.values, context_percent, used_tokens, window_tokens, measured, cache_note);
}

StatusPanelData SnapshotStatusLineValues() {
    std::lock_guard<std::mutex> lock(StdoutWriteMutex());
    return StatusDataSlot().values;
}

void SetTerminalTitle(const std::string& text) {
    const platform::StdoutConsoleProbe probe = platform::ProbeStdoutConsole();
    if (!probe.is_console || !probe.vt_enabled) {
        return;  // 管道/重定向不添转义
    }
    std::lock_guard<std::mutex> lock(StdoutWriteMutex());
    std::cout << "\x1b]0;" << text << "\x07";
    std::cout.flush();
}

void NotifyUserAttention() {
    if (!platform::ProbeStdoutConsole().is_console) {
        return;
    }
    std::lock_guard<std::mutex> lock(StdoutWriteMutex());
    std::cout << "\a";
    std::cout.flush();
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
// StdoutWriteMutex 之内),这里只做读口。
// -----------------------------------------------------------------------

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

// 帧账的"保锚可见"原语(规格见 console_input.hpp):全程序独此一处管
// "要画的行必须落在可视区里"。从 top_row 起 rows_needed 行若伸出可视窗
// 口底,先平移视口(经典 conhost 长缓冲:窗口之下还有缓冲行,内容与绝对
// 锚点一个不动),平移到头(视口贴缓冲区底——Windows Terminal/ConPTY 的
// 常态)再退回"缓冲区末行写换行滚内容"的老法。返回内容实际滚动的行数
// (平移视口时为 0):调用方拿它把绝对锚点上移对账。
int EnsureViewportRowsLocked(int top_row, int rows_needed) {
    const std::optional<platform::ScreenInfo> info = platform::GetScreenInfo();
    if (!info.has_value() || info->height <= 0) {
        return 0;
    }
    const ViewportRevealPlan plan = ComputeViewportReveal(info->height, info->viewport_y, info->viewport_height,
                                                          top_row, rows_needed);
    // 长缓冲:窗口底下还有缓冲行,平移视口把帧带进可视区——内容一个字节
    // 不动,所有绝对锚点原样保真,不用任何对账(平移不足的部分落到滚内容)。
    if (plan.pan_rows > 0 && platform::PanViewportDown(plan.pan_rows) < plan.pan_rows) {
        // 平移没到位(异常/贴底):按老法滚内容兜底,总量按原计划。
        const int shortfall = plan.pan_rows;
        platform::SetCursorPos(0, info->height - 1);
        for (int i = 0; i < shortfall + plan.scroll_rows; ++i) {
            std::cout << "\n";
        }
        std::cout.flush();
        return shortfall + plan.scroll_rows;
    }
    // 视口已贴缓冲区底(WT/ConPTY 常态):老法,末行写换行滚内容。滚掉的
    // 内容行数就是返回值,调用方把锚点上移对齐。
    if (plan.scroll_rows > 0) {
        platform::SetCursorPos(0, info->height - 1);
        for (int i = 0; i < plan.scroll_rows; ++i) {
            std::cout << "\n";
        }
        std::cout.flush();
        return plan.scroll_rows;
    }
    return 0;
}

// 帧账原语的正文/工具账封装:多一枚"锚点护栏"。长缓冲平移视口不问锚点
// (内容不动,想滚都没滚);贴缓冲底要滚内容时,要滚的行数比 anchor_row
// (这一块的锚点/块首)还多,滚完锚点也就负了——账对不上,返回 -1 让调用
// 方按各自的老规矩收场(footer 弃画这一帧、正文块记 unsafe 放弃重画)。
int EnsureViewportRowsForAnchorLocked(int anchor_row, int top_row, int rows_needed) {
    const std::optional<platform::ScreenInfo> info = platform::GetScreenInfo();
    if (!info.has_value() || info->height <= 0) {
        return 0;
    }
    const ViewportRevealPlan plan = ComputeViewportReveal(info->height, info->viewport_y, info->viewport_height,
                                                          top_row, rows_needed);
    if (plan.scroll_rows > anchor_row) {
        return -1;
    }
    return EnsureViewportRowsLocked(top_row, rows_needed);
}

bool EnsureStreamScreenRowsLocked(int rows_needed) {
    if (rows_needed <= 0) {
        return true;
    }
    const std::optional<platform::ScreenInfo> info = platform::GetScreenInfo();
    if (!info.has_value() || info->height <= 0) {
        return false;
    }
    // 锚点护栏在封装里:要滚的行数比光标上方的内容还多,滚了锚点也救不回,
    // 这一帧放弃(-1)。
    const int overflow = EnsureViewportRowsForAnchorLocked(info->cursor_y, info->cursor_y, rows_needed);
    if (overflow < 0) {
        return false;
    }
    if (overflow > 0) {
        if (const auto& hook = StreamScreenScrollHookSlot()) {
            hook(overflow);
        }
        platform::SetCursorPos(info->cursor_x, info->cursor_y - overflow);
    }
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
    if (f.last_width != -1 && f.last_width != info->width) {
        // resize:conhost 把宽行重排成了多行,上一帧的绝对锚点(顶行/正文
        // 续写位)全部失准——按旧 f.row/f.rows 擦会擦错行。当作没画过,
        // 从当前光标重新认领正文位置,整帧重画。
        f.row = -1;
        f.rows = 0;
        f.body_x = -1;
        f.body_y = -1;
    }
    f.last_width = info->width;
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
    // 代理导航坞(0.29.x 层级反转):正文/Working > 待发队列 > 上横线(右端挂
    // 当前代理 title)> composer > 下横线 > 状态栏 > 导航坞 > slash 提示。
    // 数据与状态机跟空闲同源(AgentPanelProvider + 会话级 AgentPanelSession +
    // 同一个 LayoutAgentDock),不另开第二本账;流式期间的选择/详情在这里
    // 原地重画,转空闲自然保得住。plain 主题行内无 ANSI。坞贴底、预算封在
    // 半屏以内,输入框与状态栏始终留在视口里。
    std::vector<std::string> dock_rows_text;
    std::string footer_rule_tag;
    int dock_selected_task_id = 0;
    if (AgentPanelProviderSlot()) {
        const std::vector<AgentPanelEntry> panel_entries = AgentPanelProviderSlot()();
        const AgentPanelSession::Snapshot snap0 =
            PanelSessionSlot().SnapshotFor(PanelEntryIds(panel_entries));
        const std::vector<int> nav_ids =
            DockNavigationIds(panel_entries, snap0.idle_expanded, snap0.target_task_id.value_or(0));
        PanelSessionSlot().OnEntriesChanged(nav_ids);
        if (!panel_entries.empty()) {
            const AgentPanelSession::Snapshot panel_snapshot = PanelSessionSlot().SnapshotFor(nav_ids);
            const int panel_budget = (std::max)(2, (std::min)(info->height / 2, 24));
            const AgentDockLayout dock_layout =
                LayoutAgentDock(panel_entries, panel_snapshot.selected_index, panel_snapshot.focused,
                                kDockMaxVisibleEntries, panel_budget, info->width,
                                panel_snapshot.stop_all_armed,
                                /*streaming=*/true, panel_snapshot.idle_expanded,
                                panel_snapshot.viewed_task_id);
            dock_rows_text = RenderAgentDockLines(dock_layout, info->width);
            dock_selected_task_id = panel_snapshot.selected_task_id;
            if (panel_snapshot.target_task_id.has_value()) {
                for (const auto& entry : panel_entries) {
                    if (entry.task_id == *panel_snapshot.target_task_id) {
                        footer_rule_tag = entry.title;
                        SetComposerTarget(entry.task_id);  // 流式 composer 的收件人 = 查看态那只子代理
                        break;
                    }
                }
            }
        }
        if (footer_rule_tag.empty()) {
            SetComposerTarget(std::nullopt);
        }
    }
    const int panel_rows = static_cast<int>(dock_rows_text.size());
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
    // 一本帧账(与空闲 composer 同一只 BottomChromeFrame):队列在上横线之
    // 上、坞在状态栏之下、slash 提示垫最底。高度全部从 frame 报——探底滚
    // 屏、旧框擦除(都按 f.rows 报账)随之生效。
    BottomChromeFrame frame;
    frame.queue_rows = queue_rows_text;
    frame.agent_dock_rows = dock_rows_text;
    frame.transient_rows = f.hints;
    frame.composer_rows = 1;  // footer 输入行单行会计(多行尾巴只写行数提示)
    frame.selected_task_id = dock_selected_task_id;
    frame.revision = BottomChromeRevision(frame);
    const int total_rows = working_rows + frame.TotalRows();

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

    // 待发队列:上横线之上、Working 之下(它是即将发送的内容,不属代理导航)。
    for (std::size_t i = 0; i < frame.queue_rows.size(); ++i) {
        platform::SetCursorPos(0, box_top + static_cast<int>(i));
        const int room = (std::max)(0, width - 1);
        std::cout << f.color << TruncateUtf8ToDisplayWidth(frame.queue_rows[i], room) << f.reset;
    }
    if (!frame.queue_rows.empty()) {
        box_top += queue_rows;
    }

    platform::SetCursorPos(0, box_top);
    // 上横线:查看态挂着子代理时右端挂它的 title(规格六),横线保底宽度、
    // 塞不下退整线——BuildRuleWithTag 自己会算。
    std::cout << (footer_rule_tag.empty() ? BoxRuleLine(f.theme, width)
                                          : BuildRuleWithTag(f.theme.stats, f.theme.reset, footer_rule_tag,
                                                             width));

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

    // 导航坞:状态栏之下贴底(0.29.x 层级反转),逐行摆、按屏宽截断(渲染
    // 出的行不带 ANSI,plain 主题天然纯文本)。上横线起整块框从唯一锚点
    // 一次画完,擦除按上一帧 f.rows 报账——坞增高时旧帧整个先擦净,不留
    // 第二份提示或 main。
    int dock_top = box_top + 4;
    for (std::size_t i = 0; i < frame.agent_dock_rows.size(); ++i) {
        platform::SetCursorPos(0, dock_top + static_cast<int>(i));
        const int room = (std::max)(0, width - 1);
        std::cout << f.color << TruncateUtf8ToDisplayWidth(frame.agent_dock_rows[i], room) << f.reset;
    }

    // slash 提示行:导航坞之下垫最底(短命 UI),纯文本、按屏宽截断(跟
    // ReadLineKeyByKey 画 hint_lines 一个路数);plain 主题不夹 ANSI。
    for (std::size_t i = 0; i < frame.transient_rows.size(); ++i) {
        platform::SetCursorPos(0, dock_top + panel_rows + static_cast<int>(i));
        std::cout << TruncateUtf8ToDisplayWidth(frame.transient_rows[i], (std::max)(0, width - 1));
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
        // 最外层落笔前把框(含框里的代理面板)彻底擦净:菜单要从正文末尾
        // 一次铺到底,面板留着就会插进标题/问题/选项中间。挂起期间
        // RedrawStreamFooterLocked 一律空操作,turn_runner 的心跳线程每拍
        // 也先查 RepaintSuspendedLocked——菜单等输入再久,面板也插不进来。
        EraseStreamFooterLocked();
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
    // 能力。补全候选走与空闲路同一只 BuildSlashCompletionCandidates():slash
    // 提示、Tab 补全/公共前缀/轮转全在编辑器一处记账,footer 只读它的
    // RenderState(见下面 refresh_footer),不再养第二套提示状态机。方向键
    // 直选菜单关掉:流式期间 Up/Down 分给代理面板、队列条目编辑与历史浏览
    // (规格五),本单只补 Tab。
    LineEditorCore editor(BuildSlashCompletionCandidates());
    editor.set_menu_selection_enabled(false);
    editor.BeginLine(/*composer=*/true);

    // 代理面板(规格三/四):流式期间与空闲 composer 共用同一枚会话级
    // AgentPanelSession,选择按稳定 task id。面板交互只在 footer 真画得动的
    // 场合(Windows 真控制台)承诺;plain/管道 footer 不开,键位照旧归队列
    // 编辑与历史,不承诺交互切换。
    auto panel_ids_now = [&]() -> std::vector<int> {
        if (!FooterSlot().enabled) {
            return {};
        }
        const AgentPanelProvider& provider = AgentPanelProviderSlot();
        if (!provider) {
            return {};
        }
        // 导航表与空闲路同源:条目经闲置折叠后的可导航 id 序列(含汇总哨兵)。
        const std::vector<AgentPanelEntry> entries = provider();
        const AgentPanelSession::Snapshot snap0 = PanelSessionSlot().SnapshotFor(PanelEntryIds(entries));
        return DockNavigationIds(entries, snap0.idle_expanded, snap0.target_task_id.value_or(0));
    };
    // 面板动作分派(x 停止/清除、两段确认停全部):按稳定 task id 找条目,
    // 绝不按列表下标。停全部的回执插打一行,正文行数账由 print hook 作废。
    auto dispatch_panel_outcome = [&](const AgentPanelController::Outcome& outcome) {
        const AgentPanelActions& actions = AgentPanelActionsSlot();
        if (outcome.stop_all && actions.cancel_all) {
            const int stopped = actions.cancel_all();
            std::lock_guard<std::mutex> stdout_lock(StdoutWriteMutex());
            EraseStreamFooterLocked();
            std::cout << "\n" << theme_.stats << trf("agent_panel.stop_all_notice", stopped) << theme_.reset << "\n";
            std::cout.flush();
            if (const auto& hook = StreamScreenPrintHookSlot()) {
                hook();
            }
        }
        if (outcome.stop_current && outcome.stop_current_task_id > 0) {
            const AgentPanelProvider& provider = AgentPanelProviderSlot();
            const std::vector<AgentPanelEntry> entries = provider ? provider() : std::vector<AgentPanelEntry>{};
            for (const auto& entry : entries) {
                if (entry.task_id != outcome.stop_current_task_id) {
                    continue;
                }
                if (entry.running && actions.cancel_task) {
                    actions.cancel_task(entry.task_id);
                } else if (!entry.running && actions.clear_task) {
                    actions.clear_task(entry.task_id);
                }
                break;
            }
        }
    };

    // 正在编辑的排队消息(编辑事务凭据)。空 = 在敲新消息。
    std::optional<SteeringQueue::EditHandle> edit;
    // Del 两段删除的计时(规格:经明确提示后删掉——第一下亮提示,第二下
    // 2 秒窗口内才真删;提示就写在队列区标题的编辑态文案里)。
    bool delete_armed = false;
    std::chrono::steady_clock::time_point delete_armed_until{};

    // 流式路查看帧的擦账(与空闲路 ReadLineKeyByKey 里那本同款规矩,各自
    // 一份、只在本段读取内有效):真切会话前先按上一帧的缓冲顶行把旧查看
    // 帧从可视区擦净再铺新帧。app 侧视图切换钩子只打印不擦(查看态完成
    // 退场花屏单,2026-08-17)——擦账全程序只认"铺帧前现记的 console 侧
    // 这一本",绝不并立第二本。
    std::optional<int> view_body_top;
    const auto erase_previous_view_body = [&]() {
        if (!view_body_top.has_value()) {
            return;
        }
        const std::optional<platform::ScreenInfo> info = platform::GetScreenInfo();
        if (!info.has_value()) {
            view_body_top.reset();
            return;  // 拿不到屏幕信息:退回旧行为,不硬擦
        }
        int top = *view_body_top;
        if (top < info->viewport_y) {
            top = info->viewport_y;  // 只管当前 viewport,滚屏历史不动
        }
        for (int y = top; y < info->height; ++y) {
            platform::ClearRowHardFrom(0, y, info->width);
        }
        platform::SetCursorPos(0, top);
        view_body_top.reset();
    };
    const auto print_view_frame = [&](int viewed_after) {
        erase_previous_view_body();
        const std::optional<platform::ScreenInfo> before = platform::GetScreenInfo();
        const auto& view_hook = AgentViewSwitchHookSlot();
        if (view_hook) {
            view_hook(viewed_after, /*tail_rows=*/0);
        }
        if (before.has_value()) {
            view_body_top = before->cursor_y;
        }
    };

    // footer 快照:输入行回显 + slash 提示,全部取本地编辑器同一份
    // RenderState(队列区在 RedrawStreamFooterLocked 里现拉 SteeringQueue
    // 快照,这里不用搬)。回显多行正文只摆首行、尾巴带行数提示(完整正文在
    // 取回编辑器里看/改;footer 的输入行是单行会计,不能塞换行)。slash 提示
    // 直接用编辑器的 hint_lines:候选名单、Tab 轮转的 "> " 选中标记、收起门槛
    // (还在敲命令词才列,补成 "/effort " 这类带空格的完成态就收)全在编辑器
    // 一处记账,footer 不拿回显文本另算一份没有状态的菜单。Windows 真控制台
    // 之外(footer.enabled 为假)这些都是空操作,退回老的"不回显、只 Enter
    // 时整条落队"。
    auto refresh_footer = [&] {
        std::lock_guard<std::mutex> stdout_lock(StdoutWriteMutex());
        const RenderState state = editor.CurrentRenderState();
        const std::string first = Utf32ToUtf8(state.lines.empty() ? std::u32string() : state.lines[0]);
        FooterSlot().echo =
            state.lines.size() <= 1 ? first : first + trf("queue.echo_more_lines", state.lines.size() - 1);
        FooterSlot().hints = state.hint_lines;
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
            // Esc 逐层退(规格四):队列编辑态先取消编辑;面板层再依次退两段
            // 确认/详情/代理焦点;三者都没有,这一下才打断当前流式响应——
            // 不可第一下便掐掉整轮。
            if (edit.has_value()) {
                steering.CancelEdit(*edit);
                close_edit_clear();
                refresh_footer();
                continue;
            }
            if (FooterSlot().enabled) {
                const std::vector<int> ids = panel_ids_now();
                const AgentPanelSession::Snapshot snapshot = PanelSessionSlot().SnapshotFor(ids);
                if (!ids.empty() &&
                    (snapshot.stop_all_armed || snapshot.viewed_task_id != 0 || snapshot.focused)) {
                    (void)PanelSessionSlot().HandleKey(PanelKey::Esc, ids,
                                                       editor.CurrentRenderState().line.empty(),
                                                       std::chrono::steady_clock::now());
                    refresh_footer();
                    continue;
                }
            }
            if (steering.HasAnyDeliverable()) {
                steering.RequestImmediateDelivery();
            }
            interrupt_turn();
            continue;
        }
        if (key->kind == PK::CtrlC) {
            // 有字先清字(规格《Ctrl+C 优先清空非空输入框》流式 footer 路):
            // footer 草稿非空时,Ctrl+C 只清正在敲、尚未 Enter 的那一条——
            // cancel_flag 不动、模型继续输出,清字那一下也不进双击退出计时
            // (免得用户清完字再按一下便误退进程)。已落队消息不受影响;
            // 取回编辑排队消息时同样只清编辑框,编辑事务与原文还原(Esc)
            // 规矩不变。空了才走下面既有的打断/双击退出状态机。
            if (!editor.CurrentRenderState().line.empty()) {
                editor.BeginLine(/*composer=*/true);
                delete_armed = false;
                refresh_footer();
                continue;
            }
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
            // (对齐空闲路 M11 的取舍)。Tab 与它分工:Shift+Tab 只切档,非空
            // 的 Tab 落进本地编辑器走 slash 补全,空正文 Tab 在下面明拦 no-op。
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
                // 优先级一:正在编辑排队消息,上下键只在队列条目间走。
                browse_edit(key->kind == PK::Up);
                continue;
            }
            // 优先级二/三:面板已聚焦或详情开着,上下键只切 main/代理;composer
            // 为空、面板有代理、未处于队列编辑,上下键进面板焦点并切换。
            if (FooterSlot().enabled) {
                const std::vector<int> ids = panel_ids_now();
                if (!ids.empty()) {
                    const bool empty_now = editor.CurrentRenderState().line.empty();
                    const AgentPanelSession::Snapshot snapshot = PanelSessionSlot().SnapshotFor(ids);
                    if (snapshot.focused || snapshot.viewed_task_id != 0 || empty_now) {
                        const auto outcome = PanelSessionSlot().HandleKey(
                            key->kind == PK::Up ? PanelKey::Up : PanelKey::Down, ids, empty_now,
                            std::chrono::steady_clock::now());
                        if (outcome.consumed) {
                            refresh_footer();
                            continue;
                        }
                        // 未消费(正文非空):落回编辑器历史,不抢键。
                    }
                }
            }
            if (editor.CurrentRenderState().line.empty() && steering.editable_size() > 0) {
                // 旧"空输入按上键取回最后一条"入口,保留作别名;有代理面板时
                // 上面已经抢先把键给了面板,这里只在无面板场合生效(规格四:
                // 主入口仍是 Shift+←)。
                if (auto handle = steering.BeginEditLatest(); handle.has_value()) {
                    begin_edit(std::move(*handle));
                }
                continue;
            }
            // 优先级四:队列空/正文非空,进编辑器(翻历史,composer 上下键的
            // 老现职)。
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
            // 面板聚焦且 composer 空:Enter 设 viewed_task_id(与空闲同语义,
            // composer 收件目标切给该代理,上方视口换源);否则 Enter 才落队。
            if (FooterSlot().enabled && !edit.has_value()) {
                const std::vector<int> ids = panel_ids_now();
                const AgentPanelSession::Snapshot snapshot = PanelSessionSlot().SnapshotFor(ids);
                if (!ids.empty() && snapshot.focused && editor.CurrentRenderState().line.empty()) {
                    (void)PanelSessionSlot().HandleKey(PanelKey::EnterView, ids, /*composer_empty=*/true,
                                                       std::chrono::steady_clock::now());
                    const int viewed_after = PanelSessionSlot().SnapshotFor(ids).viewed_task_id;
                    if (viewed_after != snapshot.viewed_task_id) {
                        // 真切会话:先按上一帧的账擦净旧查看帧,钩子只打印
                        // (自管锁:先擦 footer 再铺、铺完重画 footer),这里
                        // 不持锁调用。
                        print_view_frame(viewed_after);
                    }
                    refresh_footer();
                    continue;
                }
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

        // 面板其余键位(x 停止/清除、Ctrl+X Ctrl+K 两段确认):composer 空、
        // 非队列编辑态时交给同一套状态机;打字中途状态机自己放行(字母 x 只
        // 进 composer)。
        if (FooterSlot().enabled && !edit.has_value() &&
            (key->kind == PK::CtrlX || key->kind == PK::CtrlK ||
             (key->kind == PK::Char && (key->ch == U'x' || key->ch == U'X')))) {
            const std::optional<PanelKey> panel_key = MapToPanelKey(*key);
            const std::vector<int> ids = panel_ids_now();
            if (panel_key.has_value() && !ids.empty()) {
                const bool empty_now = editor.CurrentRenderState().line.empty();
                const auto outcome =
                    PanelSessionSlot().HandleKey(*panel_key, ids, empty_now, std::chrono::steady_clock::now());
                dispatch_panel_outcome(outcome);
                if (outcome.consumed) {
                    refresh_footer();
                    continue;
                }
            }
        }

        // 忙碌且 composer 为空时的 Tab:监听线程明确 no-op。这一下若喂给
        // 编辑器,composer 模式会把它翻成"进焦点态 + focus_move",而监听
        // 线程既不消费那个返回值、也不真切换焦点——画面没动,编辑器内部却
        // 换了暗状态,后续 Tab 的补全语义跟着遭殃。流式期间焦点浏览本就不
        // 开(docs/features/terminal/README.md),这里当场拦下;拦过之后再键入 /eff,第
        // 一下 Tab 照常补全,不受任何残留状态影响。
        if (key->kind == PK::Tab && editor.CurrentRenderState().line.empty()) {
            continue;
        }

        // 其余按键统一喂编辑器(字符/粘贴/退格/左右/Home/End;非空的 Tab
        // 落进编辑器走 slash 补全/轮转)。任意编辑动作解除 Del 待确认。
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
