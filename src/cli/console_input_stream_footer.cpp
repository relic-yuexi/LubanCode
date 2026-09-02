// 流式 footer 渲染引擎(骨架拆解反弹·问题 5 第一步自 console_input.cpp
// 拆出):Begin/End/Redraw/Erase/Prepare 一族 + Working/turn 活动条 + 200ms
// 心跳线程 + repaint 挂起协调 + 帧账"保锚可见"原语。框怎么贴着正文长、
// 怎么 diff 跳帧、挂起期间谁闭嘴,全在这;与空闲 composer 共用的状态行
// 组行、面板会话、键位翻译在 console_input.cpp,经 console_input_internal.hpp
// 对齐签名。全部落笔都在 StdoutWriteMutex 之内(规约见 console_input.hpp)。
#include "cli/agent_panel_host.hpp"
#include "cli/console_input.hpp"
#include "cli/terminal_port.hpp"  // TermOut/TermErr:散打 std::cout 清零,统一走输出端口

#include "cli/bottom_chrome.hpp"

#include <algorithm>
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
#include "cli/console_input_internal.hpp"

namespace lubancode::cli {

namespace {

// ---------------------------------------------------------------------------
// footer 机器的独占槽(骨架拆解反弹·问题 5 第二步):FooterState 一枚 +
// 正文重画/滚屏两枚钩子槽,从前平铺在大文件作用域,现在随机器住进本
// 文件;监听线程要摸 footer 状态时走 console_input_internal.hpp 的窄口
// (StreamFooterEnabled/DisableStreamFooter/SetStreamFooterComposer/
// RunStreamScreenPrintHook),不直接进这只匿名段。
// ---------------------------------------------------------------------------
// 0.21.x 流式脚注状态(见 console_input.hpp 里 BeginStreamFooter 一带的注释)。
// 全部读写都在 StdoutWriteMutex 之内:RunTurn(Begin/End)、监听线程(键入
// 回显)、StreamBodyTracker::OnDelta(每笔正文前后 Erase/Redraw)三处都先
// 拿锁再碰它,故字段本身不用再套原子。
struct StreamFooterState {
    bool enabled = false;   // 只有能可靠重画的真终端才为真
    std::string hint;       // 空闲占位提示(BeginStreamFooter 按主题 plain 与否建好)
    // 迁移注记(Composer 合流 P1):echo/hints 是编辑器状态的旧镜像,布局
    // 已改读下面那份完整 RenderState(多行软换行/光标/提示全从它来),这两
    // 个字段不再有人写、也不再有人读。留在这儿是给 P5 的删除清单对账,
    // 在那之前不许往里添新能力,也不许让新代码再依赖它们。
    std::string echo;       // (废弃,见上;P5 删)
    std::vector<std::string> hints;  // (废弃,见上;P5 删)
    // 忙时 composer 的完整镜像:TurnInputListener::refresh_footer 每次编辑
    // 后整份写入,RedrawStreamFooterLocked 组 BottomChromeModel 时直接带上。
    // P1 之前 footer 只拿首行回显加"另有 N 行"顶数,多行光标全靠猜——
    // 这份镜像就是那条捷径的替代。
    RenderState composer;
    std::string color;      // 淡色前缀(theme.stats);plain 主题为空串
    std::string reset;      // theme.reset;plain 主题为空串
    Theme theme;             // 完整主题:画框(BoxRuleLine)、状态行(PrintStatusLine)
                             // 直接复用 composer 那两个画法,得传整个 Theme,不能只传片段。
    // 0.28.x:队列区不再在 footer 里存副本——每帧重画时现拉
    // SessionSteeringQueue() 的轻量快照(锁内拷贝、用完即放),工具边界
    // 送达/打断收场动了账,下一帧自然对上,不会挂着旧条目。
    bool working = false;             // true 时在输入框上方合成 Working 动画
    std::string working_label;
    long long working_seconds = 0;
    // turn 级活动条(终端回合视觉收束单):认整个 turn,不认单次模型请求。
    // Spinner 那组 Start/Update/Stop 只在 turn_working 为假时才有绘制权
    //(正常聊天 turn 不再由 SpinnerBackend 掌活动条;/compact 等单次后台
    // 活照旧走 Spinner)。
    bool turn_working = false;
    std::int64_t turn_started_at_ms = 0;   // turn 起点(epoch ms);秒数从这里算,不归零
    bool turn_interrupt_requested = false; // ESC 已置 cancel:文案换 Stopping...
    int row = -1;             // 整块 footer 顶行,-1 = 没画
    int rows = 0;             // 上次实际画了几行(队列区会让高度变化)
    int body_x = -1;          // footer 下方藏着的正文续写位置
    int body_y = -1;
    int last_width = -1;      // 上一帧的终端列宽
    int input_row = -1;       // 上一帧输入行的绝对坐标(改宽后拿它追 reflow)
    std::size_t input_row_index = 0;
    int input_cursor_column = 0;
    std::vector<int> painted_row_widths;  // 上一帧各逻辑行的纯文本显示宽
    // ---- 忙路对齐空闲路(终端画面隔网单·战术三):行级双缓冲 + 指纹跳帧 ----
    // last_frame 是上一帧真画出去的 InlineFrame;last_frame_origin 是它落笔
    // 的绝对行号。重画时原点没挪就做行级 diff(没变的行一字不写),挪了才
    // 整帧重画。last_fingerprint 配它做"内容没变就一个字节都不写"的跳帧
    // (心跳 200ms 一拍,闲拍零输出,POSIX 端连屏幕探测后的整帧往返都省了)。
    // needs_repaint 是战术一的"待补画":腾位失败/探测失败的帧置上,下一拍
    // (心跳或下一笔正文)先强制整帧,不叫"框已擦、没画回"过夜。
    std::optional<InlineFrame> last_frame;
    int last_frame_origin = -1;
    std::string last_fingerprint;
    bool needs_repaint = false;
    // 帧账审计(P2-4 验收):LUBANCODE_FRAME_AUDIT 置位才统计,EndStreamFooter
    // 落一行 stderr。驱动测试拿帧数/字节数对"有帽、只出脏行"的账。
    std::uint64_t audit_frames = 0;
    std::uint64_t audit_bytes = 0;
    std::uint64_t audit_dirty_rows = 0;
    // VT 批量序列(TerminalBatch)这一场能不能用:BeginStreamFooter 探一次
    // 存住(探测带 SetConsoleMode,不该每帧来一遍)。
    bool vt_batch = false;
    // DEC 2026 同步输出这一场能不能包(终端思考活动条单·P0 治根):与
    // vt_batch 各是各的档,没确认 2026 的宿主批里一枚标记都不发,单帧
    // 事务(kSyncOutputBegin/End 的散装用法)同样不发——静默假装原子提交
    // 比不包装更糟。
    bool sync_output = false;
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
void ForgetStreamFooterFrame(StreamFooterState& f) {
    f.row = -1;
    f.rows = 0;
    f.body_x = -1;
    f.body_y = -1;
    f.input_row = -1;
    f.input_row_index = 0;
    f.input_cursor_column = 0;
    f.painted_row_widths.clear();
    f.last_frame.reset();
    f.last_frame_origin = -1;
    f.last_fingerprint.clear();
}

// 0.22.x 流式脚注框化:跟 composer 视觉一致的完整框——上横线 + 输入行
// (`> ` + 已键入内容 / 空闲占位提示) + 下横线 + 状态行;有排队消息时再在
// 上方加常驻队列区。上下横线复用 BoxRuleLine、状态行复用 BuildStatusLine,
// 不重写一份画法。
// 迁移注记(Composer 合流 P1):kStreamFooterBoxRows(基础 4 行的固定高度
// 假设)已无引用——框高改由 BuildBottomChromeLayout 按真实物理行 + 留白
// 报账。常量本体 P5 随收尾删除清单一并清走,在那之前不许再拿"框永远四行"
// 当前提写新代码。
constexpr int kStreamFooterBoxRows = 4;
// synchronized output(DEC 私有模式 2026):写之前 h、写完 l,把一次重画钉成
// 一帧提交,避免终端半途刷出"擦了一半/画了一半"的画面。已用 web 检索核实过
// 假设:ECMA-48/xterm 的通用约定是私有模式号不认得就直接吞掉、不报错也不
// 触发别的动作(iTerm2 Feature Reporting Spec、xterm ctlseqs 文档都明确要求
// "未知但格式合法的 CSI 私有模式必须被正确解析后安全忽略");Windows
// Terminal 从 1.24 Preview 起已经原生实现 DECSET 2026(conhost 共用同一套
// VT 引擎),老版本 Windows Terminal/conhost 不认这个模式号,按上面的约定
// 静默吞掉,不会有副作用——不是"想当然",是查过 xterm 规范原文 + Windows
// Terminal 官方 PR 说明后的结论。(常量本体 kSyncOutputBegin/End 自
// console_input.cpp 拆出后住在 console_input_internal.hpp,菜单帧与本文件
// 共用同一对。)
// 把 top_row 起 rows 行清空(连字符属性一起还原,不留主题色
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

// 散装单帧事务的开关(P0 治根):只在确认支持 DEC 2026 的档位包这对标记。
// 没确认的宿主发了也只是被吞掉,平添"这一段已原子提交"的错觉——两枚
// 都不发,漏不漏中间态交给选路(vt_batch=false 的宿主压根不走 CUP 路)。
void SyncFrameBegin(const StreamFooterState& f) {
    if (f.sync_output) {
        TermOut() << kSyncOutputBegin;
    }
}

void SyncFrameEnd(const StreamFooterState& f) {
    if (f.sync_output) {
        TermOut() << kSyncOutputEnd;
    }
}

// Composer 合流 P1:StripAnsiForDisplayWidth/FooterRowDisplayWidth(逐行量
// 显示宽)随行拼装一并挪去 cli/bottom_chrome.cpp 的 PlainDisplayWidth——
// footer 不再自己拼行,自然不再自己量行。

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
    // Stopping 态标签换词(换词只发生在置 cancel 那一拍,不逐帧抖)。
    const std::string label =
        f.turn_working && f.turn_interrupt_requested ? tr("spinner.stopping") : f.working_label;
    const std::vector<std::string> glyphs = FooterUtf8Glyphs(label);
    const std::string timer = " (" + std::to_string(f.working_seconds) + "s)";
    const std::string cancel = tr("spinner.cancel_hint");
    const std::string shortcut = tr("input.shortcuts_hint");
    const std::string suffix = " " + cancel + timer;
    const std::string prefix = "• ";
    const int prefix_width = static_cast<int>(DisplayWidthUtf8(prefix));
    const int suffix_width = static_cast<int>(DisplayWidthUtf8(suffix));
    const int shortcut_width = static_cast<int>(DisplayWidthUtf8(shortcut));
    const int content_limit = (std::max)(0, width - 1);
    if (shortcut_width >= content_limit) {
        return f.theme.stats + TruncateUtf8ToDisplayWidth(shortcut, content_limit) + f.reset;
    }
    const int left_limit = (std::max)(0, content_limit - shortcut_width - 1);
    const int label_room = (std::max)(0, left_limit - prefix_width - suffix_width);

    // 圆点按 turn 级态上色:Stopping 用 error 色(用户看得见"真置了"),
    // 常态用 spinner 色;字符本体不变。
    const std::string& dot_color = (f.turn_working && f.turn_interrupt_requested) ? f.theme.error : f.theme.spinner;
    std::string line = dot_color + prefix + f.reset;
    int used = 0;
    for (std::size_t i = 0; i < glyphs.size(); ++i) {
        const int glyph_width = static_cast<int>(DisplayWidthUtf8(glyphs[i]));
        if (used + glyph_width > label_room) {
            break;
        }
        // 逐字扫光已撤(P0 止血,终端思考活动条单):亮字每 200ms 轮换一
        // 位,活动行指纹便每拍都变,拖着实体光标在活动行与输入框之间来回
        // 跳——这正是真机"约每秒闪五次"的那一拍。活动行动态只剩圆点颜
        // 色(阶段/中断态)与秒钟,重画判据收敛到 TurnActivityRowChanged。
        line += f.theme.stats + glyphs[i];
        used += glyph_width;
    }
    int left_used = prefix_width + used;
    if (left_used + suffix_width <= left_limit) {
        line += f.theme.stats + suffix;
        left_used += suffix_width;
    }
    const int shortcut_room = content_limit - left_used;
    if (shortcut_room > 0) {
        const std::string kept = TruncateUtf8ToDisplayWidth(shortcut, shortcut_room);
        const int kept_width = static_cast<int>(DisplayWidthUtf8(kept));
        line += std::string(static_cast<std::size_t>((std::max)(1, content_limit - left_used - kept_width)), ' ');
        line += f.theme.stats + kept;
    }
    line += f.reset;
    return line;
}
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
// 改宽残帧的收尾清扫(改宽瞬间流式疑停摆单)。conhost 改宽会把屏上旧行
// 整块重排,ComputeFooterResizeRecovery 靠"光标行反推"猜旧帧新落点——猜
// 准了万事大吉,猜偏了(光标被改宽联动滚屏挪走、窗口被外部强拉等)旧帧
// 就有一部分留在屏上没人认领,最典型是冻结的"• 思考中 (Ns)"活动行:回合
// 早就收口了,这行残影让驱动器/用户都以为"流还在读、回合拖了十倍"。
//
// 这里现读可视区各行,凡以当前活动行签名("• " + 活动标签)起头的,一律
// 清掉。活动行文案是我们自己拼的、签名唯一(转录的思考条目是"• 思考 Ns",
// 差一个字,不会误伤),现读现比对模型猜的落点可靠——猜偏到哪儿都逃不出
// 这枚签名。只在改宽那一拍调用(读屏是控制台往返,不进每帧热路);POSIX
// 读不到屏,读一行失败即收手,退化为纯模型路。
void SweepStaleWorkingRowsLocked(const StreamFooterState& f, const platform::ScreenInfo& info) {
    const std::string label =
        f.turn_working && f.turn_interrupt_requested ? tr("spinner.stopping") : f.working_label;
    if (label.empty()) {
        return;
    }
    const std::string prefix = "• " + label;
    const int viewport_height = info.viewport_height > 0 ? info.viewport_height : info.height;
    const int top = (std::max)(0, info.viewport_y);
    const int bottom = (std::min)(info.height, top + viewport_height);
    for (int y = top; y < bottom; ++y) {
        const std::optional<std::string> row = platform::ReadRowText(y);
        if (!row.has_value()) {
            break;  // 读不了屏:退化为模型路,别硬扫
        }
        if (row->rfind(prefix, 0) == 0) {
            platform::ClearRowHardFrom(0, y, info.width);
        }
    }
}
}  // namespace

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
void SetStreamScreenPrintHook(std::function<void()> hook) {
    std::lock_guard<std::mutex> lock(StdoutWriteMutex());
    StreamScreenPrintHookSlot() = std::move(hook);
}

void SetStreamScreenScrollHook(std::function<void(int)> hook) {
    std::lock_guard<std::mutex> lock(StdoutWriteMutex());
    StreamScreenScrollHookSlot() = std::move(hook);
}

void EchoDeliveredQueuedMessages(const std::vector<QueuedMessage>& messages, const Theme& theme) {
    if (messages.empty()) {
        return;
    }
    std::lock_guard<std::mutex> lock(StdoutWriteMutex());
    EraseStreamFooterLocked();
    for (const auto& message : messages) {
        TermOut() << theme.prompt << "> " << theme.reset << message.text << "\n";
    }
    TermOut().flush();
    if (const auto& hook = StreamScreenPrintHookSlot()) {
        hook();  // 持久插入不在流式正文行数账里，旧 Markdown 锚点不能再重画
    }
    RedrawStreamFooterLocked();
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
            TermOut() << "\n";
        }
        TermOut().flush();
        return shortfall + plan.scroll_rows;
    }
    // 视口已贴缓冲区底(WT/ConPTY 常态):老法,末行写换行滚内容。滚掉的
    // 内容行数就是返回值,调用方把锚点上移对齐。
    if (plan.scroll_rows > 0) {
        platform::SetCursorPos(0, info->height - 1);
        for (int i = 0; i < plan.scroll_rows; ++i) {
            TermOut() << "\n";
        }
        TermOut().flush();
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
        ShiftStreamFooterFrameOriginLocked(overflow);  // 框随内容上移,帧账跟上
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
        int bx = f.body_x >= 0 ? f.body_x : info->cursor_x;
        int by = f.body_y >= 0 ? f.body_y : info->cursor_y;
        SyncFrameBegin(f);
        if (f.last_width != -1 && f.last_width != info->width &&
            f.input_row >= 0 && !f.painted_row_widths.empty()) {
            // 正文 delta 可能抢在 200ms heartbeat 前到。Erase 自己也要追
            // resize 后的旧框，不能只指望下一次 Redraw 来收残局。
            const FooterResizeRecoveryPlan recovery = ComputeFooterResizeRecovery(
                f.row, f.input_row, info->cursor_y, f.painted_row_widths,
                f.input_row_index, f.input_cursor_column, info->width);
            ClearStreamFooterRowsAt(recovery.top_row, recovery.rows_to_clear,
                                    info->width, info->height);
            SweepStaleWorkingRowsLocked(f, *info);
            if (recovery.cursor_reflowed) {
                bx = 0;
                by = recovery.top_row;
            }
        } else {
            ClearStreamFooterRowsAt(f.row, f.rows, info->width, info->height);
        }
        SyncFrameEnd(f);
        TermOut().flush();
        platform::SetCursorPos(bx, by);
        f.last_width = info->width;
    }
    ForgetStreamFooterFrame(f);
}

// P2-4 流式重绘风暴的外科版"擦脚注":正文落笔前的准备。旧路每笔 delta
// 都整框擦掉再整框重画(EraseStreamFooterLocked 抹净全部脚注行,帧账也
// Forget),行级 diff 与指纹跳帧形同虚设——正文只在自家行上续写、脚注
// 原点没挪的那一拍,本该一个字节都不写。这里只办三件事:
//   1. 光标钉回正文续写点(旧路由 Erase 顺带办);
//   2. 改宽了照旧走整框 Erase 的 resize 追账(reflow 后绝对锚点不可信);
//   3. 正文真要写进脚注区(行往下长,顶穿框顶)时,只清被压住的那几行,
//      并把帧账里对应的行标脏——下一拍 diff 只重画这些行,其余行一字
//      不写。整框 Forget 不再发生,diff 的"无变化行不重刷"才真正生效。
// 调用方持 StdoutWriteMutex。delta_newlines/delta_width_cols 由正文侧
// 按这笔 delta 的文本量报上来(高估无害:多清一两行只是小账,低估才会
// 留脏行)。
void PrepareStreamBodyWriteLocked(int delta_newlines, int delta_width_cols) {
    StreamFooterState& f = FooterSlot();
    if (!f.enabled || f.row < 0) {
        return;  // 框没画:没有可护的行,光标也早就在正文手上
    }
    const std::optional<platform::ScreenInfo> info = platform::GetScreenInfo();
    if (!info.has_value()) {
        ForgetStreamFooterFrame(f);  // 探不到屏,旧账不可信,退回整框路
        return;
    }
    if (f.last_width != -1 && f.last_width != info->width) {
        EraseStreamFooterLocked();  // 改宽:reflow 后只能整框追账,旧路原样
        return;
    }
    const int bx = f.body_x >= 0 ? f.body_x : info->cursor_x;
    const int by = f.body_y >= 0 ? f.body_y : info->cursor_y;
    platform::SetCursorPos(bx, by);  // 正文续写点(上一拍重画后光标停在输入行)

    // 这笔正文要占的行数:从续写点 (bx,by) 起算——换行数 + (bx+新宽度)
    // 折过的整行数,再垫一行余量吸收延迟 EOL/宽字取整的误差。不能像
    // PrintPieceLocked 的滚屏预算那样垫两行:这里多算的每一行都会白清
    // 一行脚注、白付一次重画,行内增量(最常见的帧)就再也不是零输出了。
    const int width = (std::max)(1, info->width);
    const int rows_needed = delta_newlines + (bx + delta_width_cols) / width + 1;
    const int write_bottom = by + rows_needed;  // 正文可能写到的最深行(不含)
    if (write_bottom <= f.row || f.rows <= 0) {
        return;  // 够不着框:一行都不用清,diff 自会跳过整帧
    }
    const int clear_top = (std::max)(f.row, by);
    const int clear_bottom = (std::min)(write_bottom, f.row + f.rows);
    if (clear_bottom <= clear_top) {
        return;
    }
    SyncFrameBegin(f);
    ClearStreamFooterRowsAt(clear_top, clear_bottom - clear_top, info->width, info->height);
    SyncFrameEnd(f);
    TermOut().flush();
    platform::SetCursorPos(bx, by);
    // 帧账里被压住的行标脏(x 挪到取不到的负值,永远比不中),下一拍
    // diff 只重画它们;needs_repaint 同步置上,压住指纹跳帧的那一道闸。
    if (f.last_frame.has_value()) {
        for (int i = clear_top; i < clear_bottom; ++i) {
            const std::size_t index = static_cast<std::size_t>(i - f.row);
            if (index < f.last_frame->rows.size()) {
                f.last_frame->rows[index].x = -1000;
            }
        }
    }
    f.needs_repaint = true;
}

// 正文这笔写完,光标落在新续写点。旧路靠 Erase 的 Forget 让 Redraw 从
// 光标现值认领正文位;帧账保下来之后,Redraw 的 bx/by 会优先读旧账,
// 这里把账面拨到现值,框顶 = 新正文末尾 + 1 才算得对。调用方持锁。
void NoteStreamBodyCursorLocked() {
    StreamFooterState& f = FooterSlot();
    if (!f.enabled || f.row < 0) {
        return;
    }
    const std::optional<platform::ScreenInfo> info = platform::GetScreenInfo();
    if (!info.has_value()) {
        return;
    }
    f.body_x = info->cursor_x;
    f.body_y = info->cursor_y;
}

// 内容滚屏后脚注帧账的对账:框随内容一起上移 rows 行。旧路每笔 delta
// 都 Forget,错不错账都整框重画;帧账保下来之后,滚屏不记账会让
// "原点恰好又对上"的巧合骗过 diff,拿旧行当新行跳过——屏上留的是滚
// 过的旧内容。凡在锁内主动滚了屏的正文路(PrintPieceLocked/
// RepaintBlockLocked/EnsureStreamScreenRowsLocked)都要叫这一声。
void ShiftStreamFooterFrameOriginLocked(int rows) {
    StreamFooterState& f = FooterSlot();
    if (rows <= 0 || f.row < 0) {
        return;
    }
    f.row = (std::max)(-1, f.row - rows);
    f.last_frame_origin = (std::max)(-1, f.last_frame_origin - rows);
    f.input_row = f.input_row >= 0 ? (std::max)(0, f.input_row - rows) : -1;
}

// 正文侧要往 [top_row, top_row+rows) 写字、且自带清行时(收束重画的整块
// 重铺):物理清写由调用方办,这里只把压住的脚注行在帧账里标脏,下一拍
// diff 记得重画它们。调用方持 StdoutWriteMutex。
void MarkStreamFooterRowsDirtyLocked(int top_row, int rows) {
    StreamFooterState& f = FooterSlot();
    if (f.row < 0 || rows <= 0) {
        return;
    }
    const int top = (std::max)(f.row, top_row);
    const int bottom = (std::min)(f.row + f.rows, top_row + rows);
    if (bottom <= top) {
        return;
    }
    if (f.last_frame.has_value()) {
        for (int i = top; i < bottom; ++i) {
            const std::size_t index = static_cast<std::size_t>(i - f.row);
            if (index < f.last_frame->rows.size()) {
                f.last_frame->rows[index].x = -1000;
            }
        }
    }
    f.needs_repaint = true;
}

// 忙路重画(终端画面隔网单·战术三重写,对齐空闲路 RedrawEditArea 的帧账):
//   1. 指纹跳帧:内容没变、帧没挪,一个字节都不写(心跳闲拍零输出);
//   2. 行级 diff:原点没挪时走 QueueInlineFrameDiff/PaintInlineFrameLegacy,
//      没变的行一字不写,回显只画受影响行;
//   3. 屏幕探测收敛到每帧一次,只有真滚了屏才重探(POSIX 的 DSR 往返尤贵)。
// 战术一:腾位失败的帧不裸退——降级画最小框(横线+输入行+横线+状态行),
// 再失败置 needs_repaint 待补画,光标拨回正文位,旧框清账,不留"擦了没画回"。
void RedrawStreamFooterLocked() {
    StreamFooterState& f = FooterSlot();
    if (!f.enabled || f.suspend_depth > 0 || f.paint_depth > 0) {
        return;  // 挂起期间(工具确认交互中)一律不画,见 StreamFooterSuspendScope 注释
    }
    const std::optional<platform::ScreenInfo> info = platform::GetScreenInfo();
    if (!info.has_value()) {
        f.needs_repaint = true;  // 战术一:待补画,心跳/下一笔正文再来,不裸退
        return;
    }
    // footer 已在屏上时，物理光标位于输入行；正文落笔位要从 state 取。
    // 第一次画才从当前光标认领正文位置。
    int bx = f.row >= 0 && f.body_x >= 0 ? f.body_x : info->cursor_x;
    int by = f.row >= 0 && f.body_y >= 0 ? f.body_y : info->cursor_y;

    if (f.last_width != -1 && f.last_width != info->width && f.row >= 0 &&
        f.input_row >= 0 && !f.painted_row_widths.empty()) {
        // Windows Terminal 改宽会把屏上旧行重排。旧 top_row 失效，却能借
        // 仍停在输入框里的物理光标反推新 top_row。先擦 reflow 后的整块旧
        // 帧，再从那里续画；不再把旧帧丢在历史里。反推失准的兜底见
        // SweepStaleWorkingRowsLocked 头注释——改宽残影会让"已收口的
        // 回合"看起来还在跑。
        const FooterResizeRecoveryPlan recovery = ComputeFooterResizeRecovery(
            f.row, f.input_row, info->cursor_y, f.painted_row_widths,
            f.input_row_index, f.input_cursor_column, info->width);
        SyncFrameBegin(f);
        ClearStreamFooterRowsAt(recovery.top_row, recovery.rows_to_clear,
                                info->width, info->height);
        SweepStaleWorkingRowsLocked(f, *info);
        SyncFrameEnd(f);
        TermOut().flush();
        if (recovery.cursor_reflowed) {
            bx = 0;
            by = recovery.top_row;
        }
        ForgetStreamFooterFrame(f);
    }
    f.last_width = info->width;

    // 代理导航坞(0.29.x 层级反转):正文/Working > 待发队列 > 上横线(右端挂
    // 当前代理 title)> composer > 下横线 > 状态栏 > 导航坞 > slash 提示。
    // 数据与状态机跟空闲同源(AgentPanelProvider + 会话级 AgentPanelSession +
    // 同一个 LayoutAgentDock),不另开第二本账;流式期间的选择/详情在这里
    // 原地重画,转空闲自然保得住。plain 主题行内无 ANSI。坞贴底、预算封在
    // 半屏以内,输入框与状态栏始终留在视口里。
    std::vector<std::string> dock_rows_text;
    std::vector<AgentHealthTint> dock_rows_tints;  // 监督色(P1-1):与行按位对齐
    std::string footer_rule_tag;
    int dock_selected_task_id = 0;
    if (SessionAgentPanelHost().provider()) {
        const std::vector<AgentPanelEntry> panel_entries = SessionAgentPanelHost().provider()();
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
            dock_rows_tints = DockRowTints(dock_layout);
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

    // Composer 合流 P1:footer 与空闲 composer 组同一只 BottomChromeModel、
    // 调唯一的 BuildBottomChromeLayout。输入区不再单行会计——完整 RenderState
    // (全部逻辑行、软换行、真实光标)进布局,placeholder 沿用 f.hint;状态
    // 行尾部照旧多一段 Esc 打断提示,由这里拼好递给布局摆位。
    // 高度预算(战术二)传可视窗口高:"输入行必画得下"在布局里立成硬约束,
    // 坞/队列/提示按次序舍,整帧不再撑爆窗口。
    const int width = info->width;
    const int viewport_rows = info->viewport_height > 0 ? info->viewport_height : info->height;
    const BoxChrome chrome{true, &f.theme, SharedEditor().confirm_mode()};
    BottomChromeModel model;
    if (f.working) {
        model.activity_rows = {BuildFooterWorkingLine(f, width)};
    }
    model.queue_rows = queue_rows_text;
    model.agent_dock_rows = dock_rows_text;
    model.agent_dock_tints = dock_rows_tints;  // 监督色(P1-1):与行按位对齐
    if (const auto notice_mode = ModeNoticeSlot().VisibleMode(); notice_mode.has_value()) {
        model.mode_notice_rows = {PresentApprovalMode(*notice_mode).notice};
    }
    model.transient_rows = f.composer.hint_lines;
    model.rule_tag = footer_rule_tag;
    model.selected_task_id = dock_selected_task_id;
    model.composer.editor = f.composer;
    model.composer.prompt = "> ";
    model.composer.placeholder = f.hint;
    model.composer.mode = ComposerMode::BusyQueue;
    model.composer.confirm_mode = chrome.mode;
    model.status_rows = {BuildComposerModeLine(chrome, static_cast<int>(SessionSkillCount()),
                                               (std::max)(0, width - 1))};
    const BottomChromeLayout layout = BuildBottomChromeLayout(model, f.theme, width, viewport_rows);

    if (f.hint.empty() && f.composer.line.empty() && f.composer.hint_lines.empty()) {
        // 没启用 / 还没准备好文案:这一帧不画新框;旧框(若在)整块擦净
        // 再走,不留半帧残影。
        if (f.row >= 0) {
            SyncFrameBegin(f);
            ClearStreamFooterRowsAt(f.row, f.rows, info->width, info->height);
            SyncFrameEnd(f);
            TermOut().flush();
            platform::SetCursorPos(bx, by);
            ForgetStreamFooterFrame(f);
        }
        return;
    }

    // 指纹跳帧(战术三):内容指纹(含状态行文本、上横线标签、列宽/窗高)
    // 没变、帧还在原处、也不欠补画——这一拍零输出。心跳 200ms 的闲拍从
    // "整框擦画"变成一笔不写。光标的账:这一拍没写一个字节,没人挪过它;
    // 只有探测发现物理光标真离了输入位(被外部/异常挪走)才补钉一笔——
    // 不再每拍无条件打原生 API(VT 批路混用平台光标口的正是这笔,终端
    // 思考活动条单收拢所有权)。
    const auto fingerprint_of = [&](const BottomChromeLayout& frame_layout) {
        std::string value = BottomChromeFingerprint(frame_layout.chrome);
        for (const std::string& row : model.status_rows) {
            value += "s:" + row + "\n";
        }
        value += "r:" + model.rule_tag + "\n";
        value += "w" + std::to_string(width) + "h" + std::to_string(viewport_rows);
        return value;
    };
    if (!f.needs_repaint && f.row >= 0 && f.last_frame.has_value() &&
        f.last_frame_origin == f.row && fingerprint_of(layout) == f.last_fingerprint) {
        if (info->cursor_x != f.input_cursor_column || info->cursor_y != f.input_row) {
            platform::SetCursorPos(f.input_cursor_column, f.input_row);
        }
        return;
    }

    // 框落在正文光标的下一行:正文停在行中(bx>0)就 by+1,正文刚换行
    // 停在行首(bx==0)就落在 by 这空行上——两种都是"正文当前底部的下一行"。
    // ConPTY 常把缓冲区高度报成窗口高度，正文一长，框很快便贴底。旧版
    // 这时干脆不画，正是“Working 还在，输入框忽然没了”的根子。如今先
    // 主动滚够行数，再由 scroll hook 把正文/工具锚点一同上移。
    platform::SetCursorPos(bx, by);
    const int body_offset = bx > 0 ? 1 : 0;
    const BottomChromeLayout* paint = &layout;
    BottomChromeLayout degraded;  // 战术一的降级帧(横线+输入行+横线+状态行)
    if (!EnsureStreamScreenRowsLocked(body_offset + static_cast<int>(layout.frame.rows.size()))) {
        // 腾位失败不裸退:舍掉坞/队列/活动条/提示,只保输入框与状态行。
        // 高度预算已把整帧钳进窗口,走到这条的多半是锚点护栏的绝境
        // (要滚的比锚点上方还多);最小框还画不下才置待补画收场。
        BottomChromeModel shrunk = model;
        shrunk.agent_dock_rows.clear();
        shrunk.queue_rows.clear();
        shrunk.activity_rows.clear();
        shrunk.transient_rows.clear();
        degraded = BuildBottomChromeLayout(shrunk, f.theme, width, /*height_budget=*/0);
        if (EnsureStreamScreenRowsLocked(body_offset + static_cast<int>(degraded.frame.rows.size()))) {
            paint = &degraded;
        } else {
            f.needs_repaint = true;  // 待补画:下一拍强制整帧,不叫"擦了没画回"过夜
            platform::SetCursorPos(bx, by);
            ForgetStreamFooterFrame(f);
            return;
        }
    }
    // Ensure 之后一律重探一次:平移视口(pan,内容与锚点不动、overflow 报 0)
    // 也会挪 viewport 原点,VT 批的 CUP 坐标按 viewport 相对定位——拿旧原点
    // 画帧会整体错位(压测驱动器实锤:活动行重影、框画出窗口外)。滚屏
    // (scroll)另会挪光标,同一次重探一并收账。
    int viewport_x = info->viewport_x;
    int viewport_y = info->viewport_y;
    if (const std::optional<platform::ScreenInfo> after_scroll = platform::GetScreenInfo();
        after_scroll.has_value()) {
        bx = after_scroll->cursor_x;
        by = after_scroll->cursor_y;
        viewport_x = after_scroll->viewport_x;
        viewport_y = after_scroll->viewport_y;
    }
    const int target = by + (bx > 0 ? 1 : 0);

    // 行级双缓冲(战术三):原点没挪就 diff——没变的行一字不写,回显只画
    // 受影响行;挪了(或首帧)先清掉旧帧残行,再整帧落笔。两条路与空闲路
    // RedrawEditArea 的 QueueInlineFrameDiff/PaintInlineFrameLegacy 同一套账。
    const InlineFrame* previous = nullptr;
    if (f.last_frame.has_value() && f.last_frame_origin == target) {
        previous = &*f.last_frame;
    }
    if (previous == nullptr && f.row >= 0) {
        // 旧帧还挂在屏上但原点对不上:清残行再画,别让旧框垫在新帧底下。
        // 清扫只从 max(旧帧顶, 新帧顶) 起:正文往下长了把框顶下去时,
        // [旧帧顶, 新帧顶) 这几行现在是刚落笔的正文,擦它们等于吃字。
        const int sweep_top = (std::max)(f.row, target);
        const int sweep_rows = f.row + f.rows - sweep_top;
        if (sweep_rows > 0) {
            if (f.vt_batch) {
                platform::TerminalBatch sweep(viewport_x, viewport_y, f.sync_output);
                for (int i = 0; i < sweep_rows; ++i) {
                    sweep.ClearRowHardFrom(0, sweep_top + i, width);
                }
                sweep.Flush();
            } else {
                ClearStreamFooterRowsAt(sweep_top, sweep_rows, width, info->height);
            }
        }
    }
    // 三档选路 + 光标所有权(P0 治根,终端思考活动条单):vt_batch/sync_
    // output 两枚档位在 BeginStreamFooter 探好。VT 批有命令落笔时,批的
    // 末笔 MoveTo+ShowCursor 就是本帧唯一的光标恢复路;原生路 legacy 的
    // 末笔 SetCursorPos 同理。批里一个命令都没落(指纹跳帧漏网的等价帧)
    // 才由平台口补钉。旧路批落完再叠一笔原生 SetCursorPos——两条恢复路
    // 各说各话,ConPTY 异步渲染与直写缓冲两套时序打架,正是实体光标在
    // 活动行与输入框之间来回跳的另一半病根。
    bool cursor_pinned_by_paint = false;
    if (f.vt_batch) {
        platform::TerminalBatch batch(viewport_x, viewport_y, f.sync_output);
        const InlineFrameDiffStats diff_stats = QueueInlineFrameDiff(batch, previous, paint->frame, target);
        const std::size_t batch_bytes = batch.has_commands() ? batch.Finish().size() : 0;
        batch.Flush();
        cursor_pinned_by_paint = batch_bytes > 0;
        if (FrameAuditEnabled() && batch_bytes > 0) {
            ++f.audit_frames;
            f.audit_bytes += batch_bytes;
            f.audit_dirty_rows += diff_stats.changed_rows;
        }
    } else {
        PaintInlineFrameLegacy(previous, paint->frame, target);
        cursor_pinned_by_paint = true;
    }
    // 光标落在输入行(布局算出的真实软换行位置;resize 追踪跟着光标走,
    // ComputeFooterResizeRecovery 的"input row"语义即"光标所在行")。
    const int cursor_row = paint->cursor_row;
    const int cursor_column = paint->cursor_x;
    if (!cursor_pinned_by_paint) {
        platform::SetCursorPos(cursor_column, target + cursor_row);
    }
    f.row = target;
    f.rows = static_cast<int>(paint->frame.rows.size());
    f.body_x = bx;
    f.body_y = by;
    f.input_row = target + cursor_row;
    f.input_row_index = static_cast<std::size_t>(cursor_row);
    f.input_cursor_column = cursor_column;
    f.painted_row_widths = paint->painted_row_widths;
    f.last_frame = paint->frame;
    f.last_frame_origin = target;
    // 指纹记"真画出去的那帧"——降级帧也记它自己的指纹,下一拍全量帧
    // 对不上便老实重画,不会拿全量指纹误判"已在屏上"。
    f.last_fingerprint = fingerprint_of(*paint);
    f.needs_repaint = false;
}

// 监听线程的窄口(骨架拆解反弹·问题 5:原先同住一个编译单元直摸
// FooterSlot(),拆开后经这三只口过,语义与直改逐字相同)。
bool StreamFooterEnabled() {
    return FooterSlot().enabled;
}

void DisableStreamFooter() {
    FooterSlot().enabled = false;
}

void SetStreamFooterComposer(const RenderState& composer) {
    FooterSlot().composer = composer;
}

void RunStreamScreenPrintHook() {
    // 没挂钩子就是空操作(与原先"if (hook) hook()"同语义)。
    if (const auto& hook = StreamScreenPrintHookSlot()) {
        hook();
    }
}

void BeginStreamFooter(const Theme& theme, bool enabled) {
    std::lock_guard<std::mutex> lock(StdoutWriteMutex());
    StreamFooterState& f = FooterSlot();
    f.enabled = enabled;
    ForgetStreamFooterFrame(f);
    f.last_width = -1;
    f.needs_repaint = false;
    f.audit_frames = 0;
    f.audit_bytes = 0;
    f.audit_dirty_rows = 0;
    // 三档能力分开问(P0 治根):vt/sync 各探各的,选路规则集中在
    // PlanInlineRepaint。被动探针只答 is_console/vt,同步输出是主动一问
    // (DECRQM,进程级缓存,交互会话开场已预热),这里拿现成结论,不再付
    // 探测窗。
    platform::StdoutConsoleProbe probe = platform::ProbeStdoutConsole();
    probe.sync_output = platform::ProbeSyncOutputSupport();
    const platform::InlineRepaintPlan plan = platform::PlanInlineRepaint(probe);
    f.vt_batch = plan.vt_batch;
    f.sync_output = plan.sync_output;
    f.composer = RenderState{};  // 忙时草稿从空开始(取回编辑除外,那由取回方写)
    f.echo.clear();   // 废弃镜像(见 StreamFooterState 迁移注记),归零防旧值漏读
    f.hints.clear();
    f.working = false;
    f.working_label.clear();
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
    if (FrameAuditEnabled()) {
        // 一轮流式的脚注帧账:驱动测试对"帧数有帽、字节只花在脏行"用的
        // 就是这一行(stderr,不搅正文)。paint 报这一场走的档位(vt-sync/
        // vt/native),降级不静默——真机录证拿它对账。
        const char* paint_tier = f.vt_batch ? (f.sync_output ? "vt-sync" : "vt") : "native";
        TermErr() << "[frame-audit] busy_footer paint=" << paint_tier
                  << " frames=" << f.audit_frames
                  << " bytes=" << f.audit_bytes << " dirty_rows=" << f.audit_dirty_rows << "\n";
        TermErr().flush();
    }
    f.enabled = false;
    f.composer = RenderState{};
    f.echo.clear();
    f.hints.clear();
    f.hint.clear();
    f.working = false;
    f.working_label.clear();
    // turn 活动条随 footer 一并收(这条路只在轮收口后走;正常路径由
    // EndTurnActivity 先熄,这里兜异常退场的底,不留幽灵 Working)。
    f.turn_working = false;
    f.turn_started_at_ms = 0;
    f.turn_interrupt_requested = false;
    f.suspend_depth = 0;
    f.paint_depth = 0;
}

bool StartStreamFooterWorking(const std::string& label) {
    std::lock_guard<std::mutex> lock(StdoutWriteMutex());
    StreamFooterState& f = FooterSlot();
    if (!f.enabled) {
        return false;
    }
    // turn 级活动条亮着时,SpinnerBackend(每次模型请求新起一只)不许抢
    // 绘制权——它的 Stop 会把整轮计时熄掉,报的就不是整轮用时(单子第
    // 六节的病根)。Start 照样返回 true(那只 Spinner 以为自己占了 footer,
    // 便不再走独立单行路,不花屏),但它的一切实时帧在 Update 里被挡掉。
    if (f.turn_working) {
        return true;
    }
    f.working = true;
    f.working_label = label;
    f.working_seconds = 0;
    RedrawStreamFooterLocked();
    return true;
}

void UpdateStreamFooterWorking(const std::string& label, long long elapsed_seconds) {
    std::lock_guard<std::mutex> lock(StdoutWriteMutex());
    StreamFooterState& f = FooterSlot();
    if (!f.enabled || !f.working) {
        return;
    }
    if (f.turn_working) {
        return;  // turn 活动条当家:Spinner 的帧不进(含它的"从 0 重数")
    }
    // P0 止血:活动行只在秒数/标签/中断态变化时才值得重画(TurnActivityRow
    // Changed 钉的合同)。同一秒的闲拍在这里就收手,不进布局与屏幕探测,
    // 帧审计零新增落笔;指纹跳帧那道闸继续兜底。
    if (!TurnActivityRowChanged(f.working_label, f.working_seconds, f.turn_interrupt_requested,
                                label, elapsed_seconds, f.turn_interrupt_requested)) {
        return;
    }
    f.working_label = label;
    f.working_seconds = elapsed_seconds;
    RedrawStreamFooterLocked();
}

void StopStreamFooterWorking() {
    std::lock_guard<std::mutex> lock(StdoutWriteMutex());
    StreamFooterState& f = FooterSlot();
    if (!f.working) {
        return;
    }
    if (f.turn_working) {
        return;  // turn 活动条当家:Spinner 的"首个流事件一到便停"不许熄它
    }
    f.working = false;
    f.working_label.clear();
    f.working_seconds = 0;
    RedrawStreamFooterLocked();
}

// ---- turn 级 Working 活动条(终端回合视觉收束单) --------------------------
// 单子第六节:活动条属于 BottomChrome,钉在 composer 上方;生命周期认整个
// turn,不认单次 HTTP/model request。这里是"账"的那半——绘制复用 footer
// 的 working 行(同一行、同一布局),但秒数从 turn_started_at_ms 现算,
// Spinner 的 StopStreamFooterWorking 不碰它(Start 那边见下面的让位逻辑)。

void BeginTurnActivity(const std::string& label, std::int64_t started_at_ms) {
    std::lock_guard<std::mutex> lock(StdoutWriteMutex());
    StreamFooterState& f = FooterSlot();
    if (!f.enabled) {
        return;
    }
    f.turn_working = true;
    f.turn_started_at_ms = started_at_ms;
    f.turn_interrupt_requested = false;
    f.working = true;
    f.working_label = label;
    f.working_seconds = 0;
    RedrawStreamFooterLocked();
}

void UpdateTurnActivityElapsed(std::int64_t elapsed_seconds) {
    std::lock_guard<std::mutex> lock(StdoutWriteMutex());
    StreamFooterState& f = FooterSlot();
    if (!f.enabled || !f.turn_working) {
        return;
    }
    // P0 止血:活动行只在秒数/标签/中断态变化时才值得重画(TurnActivityRow
    // Changed 钉的合同)。同一秒的闲拍(200ms 心跳每拍都来)在这里就收手,
    // 不进布局与屏幕探测,帧审计零新增落笔;指纹跳帧那道闸继续兜底。
    if (!TurnActivityRowChanged(f.working_label, f.working_seconds, f.turn_interrupt_requested,
                                f.working_label, static_cast<long long>(elapsed_seconds),
                                f.turn_interrupt_requested)) {
        return;
    }
    f.working = true;
    f.working_seconds = static_cast<long long>(elapsed_seconds);
    RedrawStreamFooterLocked();
}

void SetTurnActivityInterruptRequested() {
    std::lock_guard<std::mutex> lock(StdoutWriteMutex());
    StreamFooterState& f = FooterSlot();
    if (!f.enabled || !f.turn_working) {
        return;
    }
    f.turn_interrupt_requested = true;
    RedrawStreamFooterLocked();
}

long long EndTurnActivity() {
    std::lock_guard<std::mutex> lock(StdoutWriteMutex());
    StreamFooterState& f = FooterSlot();
    if (!f.turn_working) {
        return -1;
    }
    const long long final_seconds = f.working_seconds;
    f.turn_working = false;
    f.turn_started_at_ms = 0;
    f.turn_interrupt_requested = false;
    f.working = false;
    f.working_label.clear();
    f.working_seconds = 0;
    RedrawStreamFooterLocked();
    return final_seconds;
}

bool TurnActivityActive() {
    std::lock_guard<std::mutex> lock(StdoutWriteMutex());
    return FooterSlot().turn_working;
}

StreamFooterHeartbeat::StreamFooterHeartbeat(bool enabled,
                                             std::chrono::steady_clock::time_point started_at,
                                             const std::atomic<bool>* cancel)
    : cancel_(cancel) {
    ResetElapsed(started_at);
    if (enabled) {
        thread_ = std::thread([this] { ThreadMain(); });
    }
}

StreamFooterHeartbeat::~StreamFooterHeartbeat() { Stop(); }

void StreamFooterHeartbeat::ResetElapsed(std::chrono::steady_clock::time_point started_at) {
    started_at_ms_.store(
        std::chrono::duration_cast<std::chrono::milliseconds>(started_at.time_since_epoch()).count(),
        std::memory_order_release);
}

void StreamFooterHeartbeat::Stop() {
    stop_.store(true, std::memory_order_release);
    if (thread_.joinable()) {
        thread_.join();
    }
}

void StreamFooterHeartbeat::ThreadMain() {
    try {
        bool stopping_reported = false;
        bool mode_notice_was_visible = ModeNoticeSlot().VisibleMode().has_value();
        while (!stop_.load(std::memory_order_acquire)) {
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
            if (stop_.load(std::memory_order_acquire)) return;

            const bool mode_notice_visible = ModeNoticeSlot().VisibleMode().has_value();
            const bool mode_notice_changed = mode_notice_visible != mode_notice_was_visible;
            mode_notice_was_visible = mode_notice_visible;
            // 活动态的公开口各自拿 stdout 锁；非活动态才在这里拿锁，调用
            // “Locked” 重画口。两者倒过来套会在 MSVC 下撞递归上锁异常。
            if (TurnActivityActive()) {
                const auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                        std::chrono::steady_clock::now().time_since_epoch())
                                        .count();
                const auto elapsed = (std::max<std::int64_t>)(
                    0, now_ms - started_at_ms_.load(std::memory_order_acquire)) / 1000;
                if (cancel_ != nullptr && cancel_->load(std::memory_order_acquire) &&
                    !stopping_reported) {
                    SetTurnActivityInterruptRequested();
                    stopping_reported = true;
                }
                // 走字扫光已撤(P0 止血):心跳只报秒数,同一秒的拍在
                // UpdateTurnActivityElapsed 里就收手,帧审计零新增落笔。
                UpdateTurnActivityElapsed(elapsed);
                if (mode_notice_changed) {
                    std::lock_guard<std::mutex> lock(StdoutWriteMutex());
                    if (!RepaintSuspendedLocked()) RedrawStreamFooterLocked();
                }
            } else {
                std::lock_guard<std::mutex> lock(StdoutWriteMutex());
                if (RepaintSuspendedLocked()) continue;
                RedrawStreamFooterLocked();
            }
        }
    } catch (const std::exception& e) {
        TermErr() << "\n[footer-heartbeat] " << e.what() << "\n";
        TermErr().flush();
    } catch (...) {
        TermErr() << "\n[footer-heartbeat] unknown exception\n";
        TermErr().flush();
    }
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

}  // namespace lubancode::cli
