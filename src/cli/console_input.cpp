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
// 真控制台从"ReadConsoleW 整行读入"改走"逐键输入编辑器"
// (逐个键盘事件读,翻成 cli::KeyEvent 喂 LineEditorCore,按吐出来的
// RenderState 重画)。这一步没法在当前 headless 环境里自动化敲键盘验证
// (见上面"复现条件"),已经过编译告警检查(/W4 /permissive- 无告警)、
// 逐行代码走查、以及 LineEditorCore 纯逻辑部分的完整单测(见
// tests/unit/cli/test_line_editor.cpp)。原始逐键模式进不去(极少见,比如某些非标准
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
#include "cli/agent_panel_host.hpp"
#include "cli/console_input.hpp"
#include "cli/terminal_port.hpp"  // TermOut/TermErr:散打 std::cout 清零,统一走输出端口

#include "cli/bottom_chrome.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdio>
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
#include "cli/transcript.hpp"  // FormatUserPromptBlock/CountLines(提交收框铺用户块)
#include "platform/clipboard.hpp"
#include "platform/console.hpp"
#include "platform/paths.hpp"
#include "platform/process.hpp"
#include "platform/terminal_batch.hpp"
#include "platform/text_encoding.hpp"
#include "tools/path_utils.hpp"
#include "cli/console_input_internal.hpp"  // 家族内部共享口(仅本家族四文件用)

namespace lubancode::cli {

namespace {

std::mutex& AdditionalSlashCandidatesMutex() {
    static std::mutex mutex;
    return mutex;
}

std::vector<CompletionCandidate>& AdditionalSlashCandidatesSlot() {
    static std::vector<CompletionCandidate> candidates;
    return candidates;
}

}  // namespace

void SetAdditionalSlashCompletionCandidates(std::vector<CompletionCandidate> candidates) {
    std::lock_guard<std::mutex> lock(AdditionalSlashCandidatesMutex());
    AdditionalSlashCandidatesSlot() = std::move(candidates);
}

std::vector<CompletionCandidate> BuildSlashCompletionCandidates() {
    // 见 console_input.hpp 的声明注释:空闲 SharedEditor() 与流式监听线程
    // 的本地编辑器共用这唯一一只转换口,组装循环不许再抄第二遍。
    std::vector<CompletionCandidate> candidates;
    std::lock_guard<std::mutex> lock(AdditionalSlashCandidatesMutex());
    candidates.reserve(AllSlashCommands().size() + AdditionalSlashCandidatesSlot().size());
    for (const auto& cmd : AllSlashCommands()) {
        candidates.push_back(CompletionCandidate{cmd.name, cmd.description});
    }
    for (const auto& candidate : AdditionalSlashCandidatesSlot()) {
        const bool duplicate = std::any_of(candidates.begin(), candidates.end(), [&](const auto& existing) {
            return existing.name == candidate.name;
        });
        if (!duplicate) candidates.push_back(candidate);
    }
    return candidates;
}

namespace {

// Composer 合流 P1:输入区的上下留白/最小正文高度常量挪去 bottom_chrome.hpp
// (kComposerTopPaddingRows/kComposerMinBodyRows),Idle 与 Busy 同源取值。
// 本地那份 0/1 是 b4834b6 顺手退掉的旧值,与 ReadLineKeyByKey 开框时打的
// "\n\n"(上横线+一行留白)对不上账,合流时一并归位成 1/3。

void StripTrailingCrLf(std::string& s) {
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) {
        s.pop_back();
    }
}

// (导航坞的布局/折叠/窗口/状态机全在 cli/agent_panel.cpp 的纯逻辑层,
// 行序与帧账见 cli/bottom_chrome.hpp;这里只管接线与落笔。)

}  // namespace

// ---------------------------------------------------------------------------
// 跨机器的显式共享上下文(骨架拆解反弹·问题 5 第二步)。
// 从前 17 枚模块级 XxxSlot() 单例平铺在文件作用域,谁都能摸、分不清归
// 谁管;拆分后各自归位:
//   - 只被一台机器用的,搬进该机器自己的文件(composer 的 10 枚在
//     console_input_composer.cpp,footer 的 3 枚在 console_input_stream_
//     footer.cpp,监听线程没有独占槽);
//   - 真跨机器的收在这——本文件就是那只"共享上下文":下面每枚都注明
//     哪几台机器在用,签名在 console_input_internal.hpp(家族四文件之外
//     不许 include)。往这加新槽前先问一句:能不能只给一台机器。
// ---------------------------------------------------------------------------

// 贯穿整条交互会话存活的编辑器实例:main.cpp 里 `> ` 主循环、工具确认
// 提示、/model 选择、初次配置向导,全部经这一个 ReadLine() 入口,底下共用
// 这一份 LineEditorCore——历史列表、确认模式才有地方跨多轮读取存住。
// 补全候选从 BuildSlashCompletionCandidates() 转来,不重复写一份命令清单。
// [共享] composer(主循环)/footer(状态行读确认档)/监听线程(Shift+Tab 切档)/
// 导出口 CurrentConfirmMode、SetConfirmMode。
LineEditorCore& SharedEditor() {
    static LineEditorCore editor = [] { return LineEditorCore(BuildSlashCompletionCandidates()); }();
    return editor;
}
// 视图切换钩子的槽(viewed_task_id 变了才被调;tail_rows>0 = 实时流重铺拍,
// 见 console_input.hpp)。
// [共享] composer(空闲切看铺帧)/监听线程(流式切看铺帧)/导出口
// SetAgentViewSwitchHook。
std::function<void(int, int)>& AgentViewSwitchHookSlot() {
    static std::function<void(int, int)> hook;
    return hook;
}
// 查看帧的跨读取账(回流单规格第一节"查看帧零扰动"):ReadLine 的帧锚
// (view_body_top)是函数局部量,两段读取之间就丢了——重进 composer 时只好
// 整块清屏重铺,把还稳稳在原处的查看帧连着上方主会话正文一起抹掉再挪到
// 可视区顶(锚点漂移、收口正文从可视区消失)。这里补一本会话级账:铺查看
// 帧那一拍记下"正文顶行 + 当时缓冲宽"。重进时凭三条判据原处认账、画面
// 一个像素不动:窗浮在长缓冲里(窗底之下还有缓冲行——内容行不可能被滚,
// 视口平移只挪窗不挪内容)、缓冲宽没变(宽变了 conhost 整屏重排,绝对行号
// 全废)、期间没有非静默轮跑过(非静默轮的正文会在帧区落笔,RunTurn 起
// 跑/收口时作废这本账——静默回流轮一个字不上屏,正是要保的形态)。三条
// 任一不满足,照旧走整块清屏重铺的老规矩。主线程独占(铺帧与重进都在
// ReadLine/监听线程的按键路径上,不并发)。
// [共享] composer(重进读取认账)/监听线程(流式切看记账)/导出口
// InvalidateViewFrameLedger。
ViewFrameLedger& ViewFrameLedgerSlot() {
    static ViewFrameLedger ledger;
    return ledger;
}

// Agent 会话切页认整块可视区，不认上一页留下的绝对行号。main/subagent
// 都是同级 Panel；切到谁，上半屏就只铺谁。调用方须持 StdoutWriteMutex，
// 免得流式正文或 footer 心跳在清、铺之间插进来。
// [共享] composer(空闲真切页)/监听线程(流式真切页)。
std::optional<platform::ScreenInfo> ClearVisibleAgentPanelLocked() {
    const std::optional<platform::ScreenInfo> info = platform::GetScreenInfo();
    if (!info.has_value()) {
        return std::nullopt;
    }
    const int viewport_height = info->viewport_height > 0 ? info->viewport_height : info->height;
    const int top = info->viewport_y;
    for (int y = top; y < top + viewport_height && y < info->height; ++y) {
        platform::ClearRowHardFrom(0, y, info->width);
    }
    platform::SetCursorPos(0, top);
    platform::ScreenInfo cleared = *info;
    cleared.cursor_x = 0;
    cleared.cursor_y = top;
    return cleared;
}

// [共享] footer(重画时拉导航表)/监听线程(面板键位分派)。
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
// [共享] composer/footer/监听线程三台共用(空闲与流式的面板状态必须是
// 同一份,转场才不跳);导出口 ResetAgentPanelSession、
// CurrentAgentViewedTaskId 也走它。
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
// [共享] composer(读取开头/首帧)/footer(Redraw 重画时)/监听线程
// (落队带目标)/导出口 CurrentComposerAgentTarget。
void SetComposerTarget(std::optional<int> target) {
    std::lock_guard<std::mutex> lock(ComposerTargetMutex());
    ComposerTargetSlot() = std::move(target);
}
std::optional<int> GetComposerTarget() {
    std::lock_guard<std::mutex> lock(ComposerTargetMutex());
    return ComposerTargetSlot();
}
namespace {

// 状态行"后台任务"段的现折数据源(background 管理面单):BuildStatusLine
// 每次组行前叫一声,拿最新段文本(应用层从 BackgroundTaskRegistry 折,
// 空串 = 没任务,段收起)。空闲路在主线程、footer 路在 StdoutWriteMutex
// 内被调——与 StatusDataSlot 的既有写纪律同款(圈边界主线程写,回合内
// 锁内写),不加新锁。装 nullptr 回到"只用 StatusDataSlot 里存的那份"。
std::function<std::string()>& BackgroundStatusProviderSlot() {
    static std::function<std::string()> provider;
    return provider;
}
std::atomic<std::size_t>& SessionSkillCountSlot() {
    static std::atomic<std::size_t> count{0};
    return count;
}
}  // namespace

// 键位缝:platform 语义按键 -> 面板动作 id(PanelKey)。0.30.x 交互抛光
// 总账起,这层从手写小表换成 keymap 查表(Panel 作用域):用户 /keymap
// 改绑 agent.* 动作,面板跟脚换键,这函数一个字不用改。状态机与布局仍在
// cli/agent_panel(纯逻辑,单测钉)。
// [共享] composer(面板键位缝)/监听线程(面板其余键位)。
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
namespace {

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
}  // namespace

// 帧账审计开关(P2-4 验收):LUBANCODE_FRAME_AUDIT 置位即开。
// [共享] composer(空闲帧账)/footer(忙帧账)。忙路脚注、
// 空闲 composer 各落一行 stderr,统计真写出去的帧数与字节——跳过的帧
// 一个字节都不写,自然也不计数。驱动测试(刮屏/管道)拿 stderr 收账。
bool FrameAuditEnabled() {
    static const bool enabled = [] {
        const char* raw = std::getenv("LUBANCODE_FRAME_AUDIT");
        return raw != nullptr && *raw != '\0' && std::string_view(raw) != "0";
    }();
    return enabled;
}
// M10:谁在真的逐键读键盘,谁就得先拿到这把锁——ReadLineKeyByKey()/
// ReadChoiceMenu() 整个调用期间(从进函数到返回)一直攥着它,
// TurnInputListener 的监听线程只在抢到锁的间隙才读一次。这样"监听只活在
// 编辑器/菜单不在读的窗口期"这条要求不用靠回调层层传参去手动维护,两边
// 天然靠锁互斥错开——工具确认提示 [y/a/N] 与 ask_user 选择菜单走的也是
// 这条路,天然一并受益,不用另外接管。定义在下面公共区(声明在
// console_input.hpp),规约由 tests/unit/cli/test_repaint_coord.cpp 钉死。

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
        case PK::CtrlT:
            // 会话选择器的转录键:编辑器核心没有这个语义,照实搬运(死键),
            // picker 面板自己认。
            return KeyEvent::Simple(KeyKind::CtrlT);
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
// BoxRuleLine 挪去 cli/bottom_chrome.cpp(布局函数画横线要用,不能留在
// 终端层);这里经 bottom_chrome.hpp 直接用。

std::string BuildComposerModeLine(const BoxChrome& chrome, int skill_count, int max_width) {
    if (max_width <= 0) {
        return {};
    }
    const Theme& theme = *chrome.theme;
    const ModePresentation presentation = PresentApprovalMode(chrome.mode);
    const std::string left_plain =
        chrome.mode == ConfirmMode::Confirm
            ? trf("status.mode_switch_hint", presentation.next_label)
            : presentation.current_label + "   " + trf("status.mode_switch_hint", presentation.next_label);
    const std::string right = trf("status.skills_count", skill_count);
    const int right_width = static_cast<int>(DisplayWidthUtf8(right));
    const int gap = 2;
    const int left_room = (std::max)(0, max_width - right_width - gap);
    const std::string left = TruncateUtf8ToDisplayWidth(left_plain, left_room);
    const int left_width = static_cast<int>(DisplayWidthUtf8(left));
    const int spaces = (std::max)(1, max_width - left_width - right_width);

    std::string colored_left = left;
    if (chrome.mode != ConfirmMode::Confirm) {
        const std::string visible_label = TruncateUtf8ToDisplayWidth(presentation.current_label, left_room);
        if (!visible_label.empty() && left.rfind(visible_label, 0) == 0) {
            std::string color;
            switch (presentation.color_role) {
                case ModeColorRole::AcceptEdits: color = theme.mode_accept_edits; break;
                case ModeColorRole::Yolo: color = theme.mode_yolo; break;
                case ModeColorRole::Auto: color = theme.mode_auto; break;
                case ModeColorRole::DontAsk: color = theme.mode_dont_ask; break;
                case ModeColorRole::Default: break;
            }
            if (!color.empty()) {
                colored_left = color + visible_label + theme.reset + left.substr(visible_label.size());
            }
        }
    }
    if (right_width >= max_width) {
        return theme.stats + TruncateUtf8ToDisplayWidth(right, max_width) + theme.reset;
    }
    return colored_left + std::string(static_cast<std::size_t>(spaces), ' ') + theme.stats + right + theme.reset;
}

// 状态行:模式段按档配色(确认=默认色、auto=stats、yolo=error),信息段
// 恒 stats 淡色。0.21.x 起状态行是档位的唯一去处(提示符不再带前缀)。文本拼装是
// cli/format_utils 的纯函数,这里只管配色和按控制台宽度分段截断(截断得
// 按段做——夹着 ANSI 的整行没法安全截)。
std::string BuildStatusLine(const BoxChrome& chrome, int max_width) {
    const Theme& theme = *chrome.theme;
    const StatusLineData& data = StatusDataSlot();
    // 后台任务段现折(background 管理面单):空闲 100ms 拍与 footer 每帧
    // 都从这里取最新数,后台起/收那一刻底栏跟着变,不等主循环边界。局部
    // 拷贝刷新,不回写共享槽——线程纪律照旧。
    StatusPanelData values = data.values;
    if (const auto& provider = BackgroundStatusProviderSlot()) {
        values.background = provider();
    }
    auto segments = BuildStatusPanelSegments(data.items, chrome.mode, values);

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
            switch (PresentApprovalMode(chrome.mode).color_role) {
                case ModeColorRole::Default: color.clear(); break;
                case ModeColorRole::AcceptEdits: color = theme.mode_accept_edits; break;
                case ModeColorRole::Yolo: color = theme.mode_yolo; break;
                case ModeColorRole::Auto: color = theme.mode_auto; break;
                case ModeColorRole::DontAsk: color = theme.mode_dont_ask; break;
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
            TermOut() << new_row->text;
            TermOut().flush();
        }
    }
    platform::SetCursorPos(next.cursor_x, origin_y + next.cursor_row);
}

std::optional<std::string> ReadLine(const std::string& prompt, const Theme& theme, bool esc_rejects, bool composer,
                                    ReadExitReason* exit_reason) {
    if (platform::StdinIsInteractive()) {
        const std::optional<std::string> result =
            ReadLineKeyByKey(prompt, theme, esc_rejects, composer, exit_reason);
        ReportAndResetIdleFrameAudit();  // 帧账审计收尾(骨架拆解反弹·问题 5:本体随 composer 机器搬家)
        return result;
    }
    (void)composer;  // 管道/重定向:没有 composer 概念,照旧逐行 getline

    if (!prompt.empty()) {
        TermOut() << prompt;
        TermOut().flush();
    }
    std::string line;
    if (!std::getline(std::cin, line)) {
        if (exit_reason != nullptr) {
            *exit_reason = ReadExitReason::Cancel;
        }
        return std::nullopt;
    }
    StripTrailingCrLf(line);
    // 管道/重定向进来的字节可能不是 UTF-8(GBK/ANSI 原样透传),先洗成
    // 合法 UTF-8 再当用户输入用,免得带病进历史、wire 序列化 316。
    line = platform::SanitizeUtf8(line);
    if (exit_reason != nullptr) {
        *exit_reason = ReadExitReason::Submitted;
    }
    return line;
}
// -----------------------------------------------------------------------
// 长菜单的搜索+分页:ChoiceMenuSearchCore 的实现与搜索路径的绘制。
// provider 预设目录从 9 家涨到 75 家,方向键整单直列一屏列不下,照
// /resume 会话选择器(cli/session_picker_panel.cpp + SessionPickerCore)
// 的体验加搜索行与窗口分页。ReadChoiceMenu 按 search_threshold 分流,
// 阈值以下的老路径(ChoiceMenuCore 整单直列)一个字节不动。
// -----------------------------------------------------------------------

namespace {

// 与 cli/choice_menu.cpp、session_picker.cpp 文件内私货同款的几个小工具,
// 这里就地再备一份(那边都是各自的匿名 namespace,够不着)。
void AppendMenuUtf8(std::string& out, char32_t cp) {
    if (cp <= 0x7F) {
        out.push_back(static_cast<char>(cp));
    } else if (cp <= 0x7FF) {
        out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else if (cp <= 0xFFFF) {
        out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else if (cp <= 0x10FFFF) {
        out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    }
}

void EraseMenuLastUtf8(std::string& text) {
    if (text.empty()) {
        return;
    }
    std::size_t pos = text.size() - 1;
    while (pos > 0 && (static_cast<unsigned char>(text[pos]) & 0xC0) == 0x80) {
        --pos;
    }
    text.erase(pos);
}

bool MenuHasVisibleText(const std::string& text) {
    return std::any_of(text.begin(), text.end(), [](unsigned char ch) { return ch > 0x20 && ch != 0x7F; });
}

void AppendMenuSearchPasted(std::string& out, const std::string& pasted) {
    for (const char ch : pasted) {
        if (ch == '\r' || ch == '\n' || ch == '\t') {
            out.push_back(' ');
        } else if (ch != '\0') {
            out.push_back(ch);
        }
    }
}

std::string MenuToLowerAscii(std::string text) {
    for (char& c : text) {
        if (c >= 'A' && c <= 'Z') {
            c = static_cast<char>(c - 'A' + 'a');
        }
    }
    return text;
}

// 能翻页时的 hint。把匹配字段当面说清，免得用户只见列表变化，却猜不出
// 搜的是显示名、说明、id 还是模型名。
constexpr const char* kChoiceMenuPagingHint =
    "上/下移动 · PgUp/PgDn 翻页 · 输入搜索名称/说明 · Enter 确认";

}  // namespace

std::size_t ChoiceMenuSearchWindowRows(int viewport_rows, std::optional<std::size_t> max_visible_rows) {
    const std::size_t viewport_budget = static_cast<std::size_t>((std::max)(1, viewport_rows - 2));
    return max_visible_rows.has_value()
               ? (std::min)(viewport_budget, (std::max)(std::size_t{1}, *max_visible_rows))
               : viewport_budget;
}

ChoiceMenuQuestionLayout PlanChoiceMenuQuestionLayout(int viewport_rows, int item_count,
                                                       int described_item_count, bool has_header,
                                                       int desired_question_rows) {
    const int items = (std::max)(0, item_count);
    const int descriptions = (std::max)(0, described_item_count);
    const int viewport = (std::max)(1, viewport_rows);
    const int desired = (std::max)(1, desired_question_rows);
    // 4 = 顶横线、问话后留白、选项后留白、hint；另有动作/底横线 1 行。
    const int fixed_rows = 5 + items + (has_header ? 2 : 0);

    ChoiceMenuQuestionLayout out;
    out.question_rows = desired;
    out.stack_descriptions = fixed_rows + desired + descriptions <= viewport;
    if (!out.stack_descriptions && fixed_rows + out.question_rows > viewport) {
        out.question_rows = (std::max)(1, viewport - fixed_rows);
    }
    out.total_rows = fixed_rows + out.question_rows +
                     (out.stack_descriptions ? descriptions : 0);
    return out;
}

ChoiceMenuSearchCore::ChoiceMenuSearchCore(std::vector<ChoiceMenuItem> items, bool multi_select,
                                           std::optional<std::size_t> editable_index,
                                           std::size_t initial_cursor)
    : items_(std::move(items)), multi_select_(multi_select), editable_index_(editable_index) {
    state_.selected.assign(items_.size(), false);
    if (editable_index_.has_value() && *editable_index_ >= items_.size()) {
        editable_index_.reset();
    }
    window_rows_ = items_.empty() ? 1 : items_.size();
    // 初始光标按原索引定位;搜索词为空,视图即全量(视图位 == 原索引)。
    // 越界钳到首项——与 ChoiceMenuCore 构造同款。
    const std::size_t initial = !items_.empty() && initial_cursor < items_.size() ? initial_cursor : 0;
    Refilter(initial);
}

void ChoiceMenuSearchCore::SetWindowRows(std::size_t rows) {
    window_rows_ = rows == 0 ? 1 : rows;
    ClampScroll();
}

const ChoiceMenuState& ChoiceMenuSearchCore::HandleKey(const KeyEvent& event) {
    if (state_.submitted || state_.cancelled || state_.selected.empty()) {
        return state_;
    }
    state_.invalid = false;
    const std::size_t current = view_.empty() ? state_.cursor : view_[view_cursor_];
    const bool on_editable = editable_index_.has_value() && current == *editable_index_;
    switch (event.kind) {
        case KeyKind::Up:
        case KeyKind::ShiftTab:
            // 到头不绕圈(与 /resume 选择器同口径,老路径的循环移动只属于
            // 一屏列得下的短单)。
            if (view_cursor_ > 0) {
                --view_cursor_;
                ClampScroll();
            }
            break;
        case KeyKind::Down:
        case KeyKind::Tab:
            if (view_cursor_ + 1 < view_.size()) {
                ++view_cursor_;
                ClampScroll();
            }
            break;
        case KeyKind::Home:
            view_cursor_ = 0;
            scroll_ = 0;
            break;
        case KeyKind::End:
            if (!view_.empty()) {
                view_cursor_ = view_.size() - 1;
                ClampScroll();
            }
            break;
        case KeyKind::PageUp:
            view_cursor_ = view_cursor_ > window_rows_ ? view_cursor_ - window_rows_ : 0;
            ClampScroll();
            break;
        case KeyKind::PageDown:
            if (!view_.empty()) {
                view_cursor_ = (std::min)(view_cursor_ + window_rows_, view_.size() - 1);
                ClampScroll();
            }
            break;
        case KeyKind::Char:
            if (multi_select_ && event.ch == U' ' && !view_.empty() && !on_editable) {
                // 勾选作用于过滤视图当前项,账写回原索引。
                state_.selected[current] = !state_.selected[current];
            } else if (on_editable && event.ch >= 0x20 && event.ch != 0x7F) {
                // 光标在 editable 项上:照老路径的行内文本走。
                AppendMenuUtf8(state_.custom_text, event.ch);
            } else if (event.ch >= 0x20 && event.ch != 0x7F && !event.ctrl && !event.alt) {
                // 不在 editable 上:进搜索词,重筛后选中跳到过滤视图第一项。
                AppendMenuUtf8(search_, event.ch);
                Refilter(std::nullopt);
            }
            break;
        case KeyKind::Paste:
            if (on_editable) {
                AppendMenuSearchPasted(state_.custom_text, event.text);
            } else {
                AppendMenuSearchPasted(search_, event.text);
                Refilter(std::nullopt);
            }
            break;
        case KeyKind::Backspace:
            if (on_editable) {
                EraseMenuLastUtf8(state_.custom_text);
            } else if (!search_.empty()) {
                EraseMenuLastUtf8(search_);
                // 尽量守住当前项(新搜索词是旧词的前缀,命中只会变多,当前
                // 项几乎必在);真不在了由 Refilter 落到过滤视图第一项。
                Refilter(view_.empty() ? std::nullopt : std::optional<std::size_t>(view_[view_cursor_]));
            }
            break;
        case KeyKind::Enter:
        case KeyKind::NewLine:
            if (on_editable) {
                state_.custom_submitted = MenuHasVisibleText(state_.custom_text);
                state_.submitted = state_.custom_submitted;
                state_.invalid = !state_.submitted;
            } else if (view_.empty()) {
                state_.invalid = true;  // 搜空且无 editable:没东西可交,不当提交
            } else if (!multi_select_) {
                state_.selected[view_[view_cursor_]] = true;
                state_.submitted = true;
            } else {
                state_.submitted =
                    std::find(state_.selected.begin(), state_.selected.end(), true) != state_.selected.end();
                state_.invalid = !state_.submitted;
            }
            break;
        case KeyKind::Esc:
        case KeyKind::CtrlC:
        case KeyKind::CtrlD:
            state_.cancelled = true;
            break;
        default:
            break;
    }
    SyncCursor();
    return state_;
}

bool ChoiceMenuSearchCore::cursor_on_editable() const {
    return editable_index_.has_value() && !view_.empty() && view_[view_cursor_] == *editable_index_;
}

std::vector<std::size_t> ChoiceMenuSearchCore::SelectedIndices() const {
    std::vector<std::size_t> out;
    for (std::size_t i = 0; i < state_.selected.size(); ++i) {
        if (state_.selected[i]) {
            out.push_back(i);
        }
    }
    return out;
}

void ChoiceMenuSearchCore::Refilter(std::optional<std::size_t> keep_original) {
    view_.clear();
    const std::string needle = MenuToLowerAscii(search_);
    for (std::size_t i = 0; i < items_.size(); ++i) {
        const bool editable = editable_index_.has_value() && i == *editable_index_;
        // editable 项恒显示不过滤;其余按 label/description 大小写不敏感的
        // 子串匹配本地筛。搜索词空 = 全量。
        if (editable || needle.empty() ||
            MenuToLowerAscii(items_[i].label).find(needle) != std::string::npos ||
            MenuToLowerAscii(items_[i].description).find(needle) != std::string::npos) {
            view_.push_back(i);
        }
    }
    std::size_t want = 0;
    if (keep_original.has_value()) {
        for (std::size_t i = 0; i < view_.size(); ++i) {
            if (view_[i] == *keep_original) {
                want = i;
                break;
            }
        }
        // 原选中项不在新命中里:落到过滤视图第一项(want 保持 0)。
    }
    view_cursor_ = view_.empty() ? 0 : (std::min)(want, view_.size() - 1);
    ClampScroll();
    SyncCursor();
}

void ChoiceMenuSearchCore::ClampScroll() {
    if (view_.empty()) {
        view_cursor_ = 0;
        scroll_ = 0;
        return;
    }
    if (view_cursor_ >= view_.size()) {
        view_cursor_ = view_.size() - 1;
    }
    if (view_cursor_ < scroll_) {
        scroll_ = view_cursor_;
    } else if (view_cursor_ >= scroll_ + window_rows_) {
        scroll_ = view_cursor_ + 1 > window_rows_ ? view_cursor_ + 1 - window_rows_ : 0;
    }
    if (scroll_ + window_rows_ > view_.size()) {
        scroll_ = view_.size() > window_rows_ ? view_.size() - window_rows_ : 0;
    }
}

void ChoiceMenuSearchCore::SyncCursor() {
    if (!view_.empty()) {
        state_.cursor = view_[view_cursor_];
    }
}

namespace {

// 搜索+分页路径的绘制与帧循环,规矩照 ReadChoiceMenu 老路径:StdoutWriteMutex
// + synchronized output(kSyncOutputBegin/End,同一匿名 namespace)单帧事务、
// 藏光标、ClearRowHardFrom 清行、EnsureStreamScreenRowsLocked 腾位、
// ConsoleReadMutex 攥到底。帧的行账:搜索行(1) + 列表窗口(命中数与
// 窗口高的较小者;搜空画一行"无匹配项") + hint 行(1)。进门按最坏总
// 行数(全量时)一次腾够——此后过滤只会收窄,超不过它;每帧重画先清
// max(上一帧行数, 本帧行数) 行,窗口缩了旧尾巴也擦得净。
std::optional<ChoiceMenuResult> ReadChoiceMenuSearch(const std::vector<ChoiceMenuItem>& items,
                                                     const ChoiceMenuOptions& options, const Theme& theme,
                                                     ReadExitReason* exit_reason) {
    std::lock_guard<std::recursive_timed_mutex> console_read_lock(ConsoleReadMutex());
    platform::RawInputScope raw_scope;
    if (!raw_scope.ok()) {
        if (exit_reason != nullptr) {
            *exit_reason = ReadExitReason::Cancel;
        }
        return std::nullopt;
    }

    int start_row = 0;
    int frame_rows = 0;
    std::size_t window_budget = 1;
    {
        std::lock_guard<std::mutex> stdout_lock(StdoutWriteMutex());
        const std::optional<platform::ScreenInfo> info = platform::GetScreenInfo();
        if (!info.has_value()) {
            if (exit_reason != nullptr) {
                *exit_reason = ReadExitReason::Cancel;
            }
            return std::nullopt;
        }
        // 窗口高 = 可视行数减搜索行与 hint 行,不写死;查不到可视高退
        // 缓冲高(ScreenInfo 的约定),至少留一行。
        const int viewport_rows = info->viewport_height > 0 ? info->viewport_height : info->height;
        window_budget = ChoiceMenuSearchWindowRows(viewport_rows, options.max_visible_rows);
        const int worst_rows = 2 + static_cast<int>((std::min)(items.size(), window_budget));
        if (!EnsureStreamScreenRowsLocked(worst_rows)) {
            if (exit_reason != nullptr) {
                *exit_reason = ReadExitReason::Cancel;
            }
            return std::nullopt;
        }
        const std::optional<platform::ScreenInfo> after = platform::GetScreenInfo();
        if (!after.has_value()) {
            if (exit_reason != nullptr) {
                *exit_reason = ReadExitReason::Cancel;
            }
            return std::nullopt;
        }
        start_row = after->cursor_y;
    }

    ChoiceMenuSearchCore menu(items, options.multi_select, options.editable_index,
                              options.initial_cursor.value_or(0));
    menu.SetWindowRows(window_budget);

    auto draw = [&]() -> bool {
        std::lock_guard<std::mutex> stdout_lock(StdoutWriteMutex());
        const std::optional<platform::ScreenInfo> info = platform::GetScreenInfo();
        if (!info.has_value()) {
            return false;
        }
        const int width = info->width;
        const std::vector<std::size_t>& view = menu.view();
        const std::size_t visible =
            view.empty() ? 0 : (std::min)(view.size() - menu.scroll(), menu.window_rows());
        const int rows_now = 2 + (view.empty() ? 1 : static_cast<int>(visible));
        const int clear_rows = (std::max)(frame_rows, rows_now);
        TermOut() << kSyncOutputBegin << "\x1b[?25l";
        for (int r = 0; r < clear_rows; ++r) {
            platform::ClearRowHardFrom(0, start_row + r, width);
        }
        // 搜索行:词尾下划线当光标,命中数/总数跟上,淡色。
        const std::string search_line = "搜索名称/说明: " + menu.search() + "_ (" +
                                        std::to_string(view.size()) + "/" + std::to_string(items.size()) + ")";
        platform::SetCursorPos(0, start_row);
        TermOut() << theme.stats << TruncateUtf8ToDisplayWidth(search_line, width - 1) << theme.reset;
        // 窗口内条目:行样式与老路径一字同款(> 前缀、多选 [x]、选中高亮、
        // description 同行尾随、editable 行内文本)。
        for (std::size_t row = 0; row < visible; ++row) {
            const std::size_t index = menu.scroll() + row;  // 过滤视图偏移
            const std::size_t original = view[index];       // 原索引
            const bool active = index == menu.view_cursor();
            const bool editable = options.editable_index.has_value() && original == *options.editable_index;
            std::string prefix = active ? "> " : "  ";
            if (options.multi_select && !editable) {
                prefix += menu.state().selected[original] ? "[x] " : "[ ] ";
            } else if (options.multi_select) {
                prefix += "    ";
            }
            const int room = (std::max)(0, width - static_cast<int>(DisplayWidthUtf8(prefix)) - 1);
            std::string raw_label = items[original].label;
            if (editable) {
                raw_label += ": ";
                raw_label += menu.state().custom_text.empty()
                                 ? options.editable_placeholder
                                 : menu.state().custom_text + (active ? "_" : "");
            }
            const std::string label = TruncateUtf8ToDisplayWidth(raw_label, room);
            int description_room = room - static_cast<int>(DisplayWidthUtf8(label)) - 3;

            platform::SetCursorPos(0, start_row + 1 + static_cast<int>(row));
            if (active) {
                TermOut() << theme.confirm;
            }
            TermOut() << prefix << label << theme.reset;
            if (!items[original].description.empty() && description_room > 0) {
                TermOut() << theme.stats << " - "
                          << TruncateUtf8ToDisplayWidth(items[original].description, description_room)
                          << theme.reset;
            }
        }
        if (view.empty()) {
            platform::SetCursorPos(0, start_row + 1);
            TermOut() << theme.stats << TruncateUtf8ToDisplayWidth(std::string("无匹配项"), width - 1)
                      << theme.reset;
        }
        // hint 行:invalid 最优先,editable 行内编辑次之,能翻页给翻页提示,
        // 其余照老规则用调用方的 hint。
        platform::SetCursorPos(0, start_row + rows_now - 1);
        std::string hint;
        if (menu.state().invalid) {
            hint = options.invalid_hint;
        } else if (menu.cursor_on_editable()) {
            hint = options.editable_hint;
        } else if (menu.scrollable()) {
            hint = kChoiceMenuPagingHint;
        } else {
            hint = options.hint;
        }
        TermOut() << (menu.state().invalid ? theme.error : theme.stats)
                  << TruncateUtf8ToDisplayWidth(hint, width - 1) << theme.reset << kSyncOutputEnd;
        TermOut().flush();
        frame_rows = rows_now;
        return true;
    };

    auto clear = [&] {
        std::lock_guard<std::mutex> stdout_lock(StdoutWriteMutex());
        const std::optional<platform::ScreenInfo> info = platform::GetScreenInfo();
        if (info.has_value()) {
            TermOut() << kSyncOutputBegin;
            for (int r = 0; r < frame_rows; ++r) {
                platform::ClearRowHardFrom(0, start_row + r, info->width);
            }
            platform::SetCursorPos(0, start_row);
            TermOut() << "\x1b[?25h" << kSyncOutputEnd;
            TermOut().flush();
        } else {
            TermOut() << "\x1b[?25h" << std::flush;
        }
    };

    if (!draw()) {
        if (exit_reason != nullptr) {
            *exit_reason = ReadExitReason::Cancel;
        }
        return std::nullopt;
    }
    platform::KeyReader key_reader;
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

}  // namespace

std::optional<ChoiceMenuResult> ReadChoiceMenu(const std::vector<ChoiceMenuItem>& items,
                                                const ChoiceMenuOptions& options, const Theme& theme,
                                                ReadExitReason* exit_reason) {
    if (items.empty() || !platform::StdinIsInteractive()) {
        if (exit_reason != nullptr) {
            *exit_reason = ReadExitReason::Cancel;
        }
        return std::nullopt;
    }
    // 条目超过阈值(或显式 always_search):走搜索+分页路径;等于/低于阈值
    // 仍是下面的整单直列老路径,一个字节不动。
    if (!options.question_panel.has_value() &&
        (options.always_search || items.size() > options.search_threshold)) {
        return ReadChoiceMenuSearch(items, options, theme, exit_reason);
    }
    std::lock_guard<std::recursive_timed_mutex> console_read_lock(ConsoleReadMutex());
    platform::RawInputScope raw_scope;
    if (!raw_scope.ok()) {
        if (exit_reason != nullptr) {
            *exit_reason = ReadExitReason::Cancel;
        }
        return std::nullopt;
    }

    int start_row = 0;
    const bool question_panel = options.question_panel.has_value();
    const bool panel_has_separator =
        question_panel && options.separator_before_index.has_value() &&
        *options.separator_before_index > 0 && *options.separator_before_index < items.size();
    // 问话多行化:折行要屏宽,menu_rows 又得先于 EnsureStreamScreenRowsLocked
    // 算出来——先探一次屏面做预算(探不到按 80 列/24 行保守折),问话预折成
    // question_lines,行数自此定死。draw 每次重画只描这份预折结果,不现折;
    // 窗口中途改宽不重折(整单菜单本就不响应改宽),重画时按当前屏宽再截
    // 一刀,防预折行溢出把下方行账折乱。
    std::vector<std::string> question_lines;
    bool stack_descriptions = question_panel;
    int panel_height = 24;
    if (question_panel) {
        int panel_width = 80;
        const std::optional<platform::ScreenInfo> probe = platform::GetScreenInfo();
        if (probe.has_value()) {
            if (probe->width > 0) {
                panel_width = probe->width;
            }
            const int visible_rows =
                probe->viewport_height > 0 ? probe->viewport_height : probe->height;
            if (visible_rows > 0) {
                panel_height = visible_rows;
            }
        }
        // 问话行帽:可视屏高的一半,3 行起步、12 行封顶。问话再长也不能把
        // 选项区挤没了;超帽截断,末行带省略号标记,全文用户仍可从上方
        // 事件账/非交互路径看。
        const int desired_cap = (std::min)(12, (std::max)(3, panel_height / 2));
        int described_items = 0;
        for (const ChoiceMenuItem& item : items) {
            if (!item.description.empty()) ++described_items;
        }
        const ChoiceMenuQuestionLayout plan = PlanChoiceMenuQuestionLayout(
            panel_height, static_cast<int>(items.size()), described_items,
            !options.question_panel->header.empty(), desired_cap);
        stack_descriptions = plan.stack_descriptions;
        question_lines = WrapUtf8ToDisplayWidthCapped(options.question_panel->question,
                                                      (std::max)(1, panel_width - 1),
                                                      plan.question_rows);
    }
    int menu_rows = static_cast<int>(items.size()) + 1;
    if (question_panel) {
        // 顶横线、问话(折行后占 question_lines.size() 行)、两处留白、选项
        // 与 hint；有题头、有说明或动作分隔线时再各加一行。没有动作分隔线
        // 的普通问题面板补一根底横线。
        menu_rows = 4 + static_cast<int>(question_lines.size()) + static_cast<int>(items.size());
        if (!options.question_panel->header.empty()) {
            menu_rows += 2;  // 题头 + 题头后的留白
        }
        ++menu_rows;  // 动作分隔线，或普通面板的底横线
        for (const ChoiceMenuItem& item : items) {
            if (stack_descriptions && !item.description.empty()) {
                ++menu_rows;
            }
        }
    }
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
                        options.initial_cursor.value_or(0), options.immediate_submit_index);
    auto draw = [&] {
        std::lock_guard<std::mutex> stdout_lock(StdoutWriteMutex());
        const std::optional<platform::ScreenInfo> info = platform::GetScreenInfo();
        if (!info.has_value()) {
            return false;
        }
        const int width = info->width;
        TermOut() << kSyncOutputBegin << "\x1b[?25l";
        int row = 0;
        if (question_panel) {
            platform::ClearRowHardFrom(0, start_row + row, width);
            platform::SetCursorPos(0, start_row + row++);
            TermOut() << BoxRuleLine(theme, width);
            if (!options.question_panel->header.empty()) {
                platform::ClearRowHardFrom(0, start_row + row, width);
                platform::SetCursorPos(0, start_row + row++);
                TermOut() << theme.banner
                          << TruncateUtf8ToDisplayWidth("□ " + options.question_panel->header, width - 1)
                          << theme.reset;
                platform::ClearRowHardFrom(0, start_row + row, width);
                ++row;
            }
            // 问话逐行描;行是起手屏宽预折的,这里按当前屏宽兜底截一刀
            for (const std::string& question_line : question_lines) {
                platform::ClearRowHardFrom(0, start_row + row, width);
                platform::SetCursorPos(0, start_row + row++);
                TermOut() << TruncateUtf8ToDisplayWidth(question_line, width - 1);
            }
            platform::ClearRowHardFrom(0, start_row + row, width);
            ++row;
        }
        for (std::size_t i = 0; i < items.size(); ++i) {
            if (panel_has_separator && i == *options.separator_before_index) {
                platform::ClearRowHardFrom(0, start_row + row, width);
                platform::SetCursorPos(0, start_row + row++);
                TermOut() << BoxRuleLine(theme, width);
            }
            const bool active = i == menu.state().cursor;
            const bool editable = options.editable_index.has_value() && i == *options.editable_index;
            const bool immediate = options.immediate_submit_index.has_value() &&
                                   i == *options.immediate_submit_index;
            std::string prefix = active ? "> " : "  ";
            if (options.multi_select && !editable && !immediate) {
                prefix += menu.state().selected[i] ? "[x] " : "[ ] ";
            } else if (options.multi_select) {
                prefix += "    ";
            }
            if (question_panel) {
                prefix += std::to_string(i + 1) + ". ";
            }
            const int room = (std::max)(0, width - static_cast<int>(DisplayWidthUtf8(prefix)) - 1);
            std::string raw_label = items[i].label;
            if (editable) {
                if (question_panel) {
                    if (!menu.state().custom_text.empty()) {
                        raw_label += ": " + menu.state().custom_text + (active ? "_" : "");
                    } else if (active) {
                        raw_label += ": _";
                    }
                } else {
                    raw_label += ": ";
                    raw_label += menu.state().custom_text.empty()
                                     ? options.editable_placeholder
                                     : menu.state().custom_text + (active ? "_" : "");
                }
            }
            const std::string label = TruncateUtf8ToDisplayWidth(raw_label, room);

            platform::ClearRowHardFrom(0, start_row + row, width);
            platform::SetCursorPos(0, start_row + row++);
            if (active) {
                TermOut() << (question_panel ? theme.banner : theme.confirm);
            }
            TermOut() << prefix << label << theme.reset;
            if (!items[i].description.empty()) {
                if (question_panel && stack_descriptions) {
                    const std::string indent(static_cast<std::size_t>(options.multi_select ? 9 : 5), ' ');
                    platform::ClearRowHardFrom(0, start_row + row, width);
                    platform::SetCursorPos(0, start_row + row++);
                    TermOut() << theme.stats << indent
                              << TruncateUtf8ToDisplayWidth(
                                     items[i].description,
                                     (std::max)(0, width - static_cast<int>(indent.size()) - 1))
                              << theme.reset;
                } else {
                    const int description_room = room - static_cast<int>(DisplayWidthUtf8(label)) - 3;
                    if (description_room > 0) {
                        TermOut() << theme.stats << " - "
                                  << TruncateUtf8ToDisplayWidth(items[i].description, description_room)
                                  << theme.reset;
                    }
                }
            }
        }
        if (question_panel && !panel_has_separator) {
            platform::ClearRowHardFrom(0, start_row + row, width);
            platform::SetCursorPos(0, start_row + row++);
            TermOut() << BoxRuleLine(theme, width);
        }
        if (question_panel) {
            platform::ClearRowHardFrom(0, start_row + row, width);
            ++row;
        }
        platform::ClearRowHardFrom(0, start_row + row, width);
        platform::SetCursorPos(0, start_row + row);
        std::string hint;
        if (menu.state().invalid) {
            hint = options.invalid_hint;
        } else if (options.editable_index.has_value() && menu.state().cursor == *options.editable_index) {
            hint = options.editable_hint;
        } else {
            hint = options.hint;
        }
        TermOut() << (menu.state().invalid ? theme.error : theme.stats)
                  << TruncateUtf8ToDisplayWidth(hint, width - 1) << theme.reset << kSyncOutputEnd;
        TermOut().flush();
        return true;
    };

    auto clear = [&] {
        std::lock_guard<std::mutex> stdout_lock(StdoutWriteMutex());
        const std::optional<platform::ScreenInfo> info = platform::GetScreenInfo();
        if (info.has_value()) {
            TermOut() << kSyncOutputBegin;
            for (int r = 0; r < menu_rows; ++r) {
                platform::ClearRowHardFrom(0, start_row + r, info->width);
            }
            platform::SetCursorPos(0, start_row);
            TermOut() << "\x1b[?25h" << kSyncOutputEnd;
            TermOut().flush();
        } else {
            TermOut() << "\x1b[?25h" << std::flush;
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
// 非静默轮的"屏面可能被写"记账(见 ViewFrameLedgerSlot 注释):RunTurn 起
// 跑/收口时调用,作废跨读取账——流式正文在自己的续写行落笔、可能与查看
// 帧抢行,锚点从此不可信。静默轮不调(输出全进台账,屏面零扰动),账保住。
void InvalidateViewFrameLedger() {
    ViewFrameLedgerSlot().body_top = -1;
}

void SetAgentViewSwitchHook(std::function<void(int viewed_task_id, int tail_rows)> hook) {
    AgentViewSwitchHookSlot() = std::move(hook);
}

void ResetAgentPanelSession() { PanelSessionSlot().Reset(); }

std::optional<int> CurrentComposerAgentTarget() { return GetComposerTarget(); }

int CurrentAgentViewedTaskId() {
    // 会话层面板控制器的真状态,不跟某次 ReadLine 的生命周期:主循环在两次
    // 读取之间(回流轮前后)问它,答案必须仍然准确。
    return PanelSessionSlot().SnapshotFor({}).viewed_task_id;
}
void SetBackgroundStatusProvider(std::function<std::string()> provider) {
    BackgroundStatusProviderSlot() = std::move(provider);
}
void SetSessionSkillCount(std::size_t count) {
    SessionSkillCountSlot().store(count, std::memory_order_relaxed);
}
std::size_t SessionSkillCount() {
    return SessionSkillCountSlot().load(std::memory_order_relaxed);
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
    TermOut() << "\x1b]0;" << text << "\x07";
    TermOut().flush();
}

void NotifyUserAttention() {
    if (!platform::ProbeStdoutConsole().is_console) {
        return;
    }
    std::lock_guard<std::mutex> lock(StdoutWriteMutex());
    TermOut() << "\a";
    TermOut().flush();
}

std::optional<int> DetectConsoleWidth() {
    return platform::ConsoleWidth();
}

std::mutex& StdoutWriteMutex() {
    static std::mutex m;
    return m;
}

std::recursive_timed_mutex& ConsoleReadMutex() {
    return platform::ConsoleInputMutex();
}

}  // namespace lubancode::cli
