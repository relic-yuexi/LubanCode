// 前台行编辑器主循环(composer 机器)——骨架拆解反弹·问题 5 第一步自
// console_input.cpp 拆出:ReadLineKeyByKey 一整只逐键读取循环(约 1660 行,
// 全仓最大函数)及其直接辅助(空闲底栏重画/提交收框/帧账审计/编辑器与
// 面板的会话级槽)。机器的键位语义、帧账与查看帧规矩全在这;跨机器的
// 共享底层(锁、状态行组行、键位翻译、面板会话)在 console_input.cpp,
// 经 console_input_internal.hpp 对齐签名。管道/重定向不走这条路(ReadLine
// 壳在 console_input.cpp 分流)。
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
// composer 机器的独占槽(骨架拆解反弹·问题 5 第二步):以下 10 枚会话级
// 槽只有这台机器读写(装卸口是 console_input.hpp 的 SetXxx 导出函数),
// 从前平铺在大文件作用域,现在随机器住进本文件。跨机器的槽(SharedEditor/
// PanelSession/ComposerTarget/ViewFrameLedger/AgentViewSwitchHook)留在
// console_input.cpp 的共享上下文,本文件经 console_input_internal.hpp 取。
// ---------------------------------------------------------------------------
// UI-D(0.16.0):会话级 UI 按键回调(Ctrl+O/Ctrl+E/焦点导航)。存这儿、
// 由 SetTranscriptUiHandler 装卸;ReadLineKeyByKey 只在 composer 读取里查它。
TranscriptUiHandler& UiHandlerSlot() {
    static TranscriptUiHandler handler;
    return handler;
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

// 轮次打断/用户排队的广播(监督器单 P1-0):ESC 或 Ctrl+C 打断当前轮、
// 以及用户往待发队列里排队消息的那一刻叫一声——应用层拿它去唤醒正睡在
// agent_watch 等待里的 watcher(台账 NotifyExternalWake),单子 §9.2 的
// "用户输入、取消提前唤醒"。在监听线程上被调,钩子须自备线程安全、只做
// 唤醒不做重活。
std::function<void()>& TurnInterruptBroadcastSlot() {
    static std::function<void()> hook;
    return hook;
}

// 状态行"后台任务"段的现折数据源(background 管理面单):BuildStatusLine
// 每次组行前叫一声,拿最新段文本(应用层从 BackgroundTaskRegistry 折,
// 空串 = 没任务,段收起)。空闲路在主线程、footer 路在 StdoutWriteMutex
// 内被调——与 StatusDataSlot 的既有写纪律同款(圈边界主线程写,回合内
// 锁内写),不加新锁。装 nullptr 回到"只用 StatusDataSlot 里存的那份"。
std::function<std::string()>& BackgroundStatusProviderSlot() {
    static std::function<std::string()> provider;
    return provider;
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
// 空闲路的同款账:ReadLineKeyByKey 一整次读取里真画了几帧、写了多少字节。
struct IdleFrameAudit {
    std::uint64_t frames = 0;
    std::uint64_t bytes = 0;
};
IdleFrameAudit& IdleFrameAuditSlot() {
    static IdleFrameAudit audit;
    return audit;
}
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
// 空闲路的底栏重画(Composer 合流 P1 起收成薄 adapter):这里只管锚点、
// 滚屏腾位与落笔,输入行拼装、软换行、padding、最小正文高度、光标全部
// 交给唯一的 BuildBottomChromeLayout——忙时那条路(RedrawStreamFooter
// Locked)组的是同一只 BottomChromeModel,两条路不再各拼一套行序。
//
// 0.29.x"导航贴底"一单定下的行次序(队列 → 上横线 → composer → 下横线 →
// 状态行 → 代理坞 → slash 提示)如今写在 bottom_chrome.cpp 的布局函数里。
// 锚点 start_row 仍是 composer 首行(提示符行),帧顶 = start_row - 上方
// 行数 - 横线/留白 由这里推;上一帧的绝对帧顶记在 prev_frame_origin,面板
// 增减/终端缩放/滚屏挪了位就对不上,整帧重画。
//
// chrome_out 回传行数账(含 composer 摘要指纹):100ms 拍的指纹比较与
// 真画那一帧必须出自同一颗布局函数,不然"tick 拿 slash 提示算指纹、屏上
// 画的是搜索行"这类账目错位又会回来。
void RedrawEditArea(int& start_row, const BottomChromeModel& model, const Theme& theme,
                    int& prev_body_row_count, std::optional<InlineFrame>& previous_frame,
                    int& prev_frame_origin, bool vt_enabled, BottomChromeFrame* chrome_out = nullptr) {
    const std::optional<platform::ScreenInfo> info = platform::GetScreenInfo();
    if (!info.has_value()) {
        return;  // 拿不到屏幕信息就没法定位,这一帧放弃重画,下一帧再试
    }
    const int buffer_width = info->width;
    const int buffer_height = info->height;

    // 贴缓冲顶时舍弃最老的队列行(锚点 start_row 上方摆不下),与合流前
    // 同一条护栏;裁完再进布局函数。
    BottomChromeModel effective = model;
    const int fixed_rows_above_input =
            (model.framed ? 1 : 0) + (model.framed ? model.composer.top_padding_rows : 0);
    int above_count = static_cast<int>(effective.queue_rows.size());
    if (above_count + fixed_rows_above_input > start_row) {
        above_count = std::max(0, start_row - fixed_rows_above_input);
        effective.queue_rows.erase(effective.queue_rows.begin(),
                                   effective.queue_rows.end() - above_count);
    }
    // 帮助层垫在队列之上,贴缓冲顶装不下时保头舍尾——表头写着怎么收,
    // 丢了头用户就找不到门(与队列"保尾舍头"方向相反,各按各的语义裁)。
    int help_above = static_cast<int>(effective.help_rows.size());
    const int help_room = start_row - fixed_rows_above_input - above_count;
    if (help_above > help_room) {
        help_above = std::max(0, help_room);
        effective.help_rows.resize(static_cast<std::size_t>(help_above));
    }
    int frame_top = start_row - above_count - help_above - fixed_rows_above_input;
    if (frame_top < 0) {
        frame_top = 0;  // 理论到不了(box 模式提示符行上方至少有一行),防越界
    }

    // 高度预算(终端画面隔网单·战术二)传可视窗口高:空闲帧(多行输入 +
    // 队列 + 坞)超过窗口时在布局里钳制,输入行必画得下。
    const int viewport_rows =
            info->viewport_height > 0 ? info->viewport_height : buffer_height;
    const BottomChromeLayout layout = BuildBottomChromeLayout(effective, theme, buffer_width, viewport_rows);
    const InlineFrame& next = layout.frame;

    // 锚点(提示符行)之下、帧顶之上还有队列行与横线留白;body_rows 记
    // "帧顶之下共几行"(整帧账,retire/collapse 按它擦旧帧)。
    const int body_rows = std::max(0, static_cast<int>(next.rows.size()) - 1);
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
    // 帧顶挪了还有一笔账要清:diff 只认原点没挪的帧,旧帧范围里落在
    // 新帧之外的行(帮助层收起/队列抽条那一侧)没人认领会留残影——
    // 先把旧帧范围整块擦净,再落新帧(帮助层开合单;对生长方向是白擦
    // 一层马上盖回,对收小方向正是去鬼影的那一笔)。
    const InlineFrame* previous =
        previous_frame.has_value() && prev_frame_origin == frame_top ? &*previous_frame : nullptr;
    const bool stale_extent = previous == nullptr && prev_frame_origin >= 0;
    if (stale_extent) {
        const int old_bottom = (std::min)(prev_frame_origin + prev_body_row_count, buffer_height - 1);
        for (int y = (std::max)(0, prev_frame_origin); y <= old_bottom; ++y) {
            platform::ClearRowHardFrom(0, y, buffer_width);
        }
    }
    // VT 终端把擦行、落字、归光标攒进一段字节,一次 write + flush;老终端
    // 仍走 Console API 兼容路。
    if (vt_enabled) {
        platform::TerminalBatch batch(viewport_x, viewport_y, platform::ProbeSyncOutputSupport());
        QueueInlineFrameDiff(batch, previous, next, frame_top);
        if (batch.has_commands()) {
            const std::size_t batch_bytes = batch.Finish().size();
            batch.Flush();
            if (FrameAuditEnabled() && batch_bytes > 0) {
                IdleFrameAudit& audit = IdleFrameAuditSlot();
                ++audit.frames;
                audit.bytes += batch_bytes;
            }
        } else {
            batch.Flush();
        }
    } else {
        PaintInlineFrameLegacy(previous, next, frame_top);
    }
    previous_frame = std::move(next);
    prev_frame_origin = frame_top;
    prev_body_row_count = body_rows;
    if (chrome_out != nullptr) {
        *chrome_out = layout.chrome;
    }
}

// 0.17.0 输入框化的提交收尾:横线擦掉、提交行保留。取舍:两个方案里选了
// "只留 `> 内容`"这条——框连横线留在滚动历史的话,每一问上下各一根 100 列
// 横线,再加 RunTurn 紧接着打的输入/输出分界线,三根线叠一块,滚动历史
// 全是线;擦掉横线后历史里就是干干净净的 `> 问题` + 分界线 + 回答,跟
// 0.16.0 的历史观感一致,框只属于"正在输入"这一刻。
//
// 做法:把整个框(0.28.x 起含上横线之上的代理面板行,帧顶即 frame_top =
// prev_frame_origin)统统清掉,从帧顶起重打提交内容,光标停在末行末尾,
// 调用方接着换行。面板行随提交收走,滚动历史里只留干净的问题,代理的最终
// 结论照旧走 transcript,不在面板里留残骸。
//
// 用户输入背景块单(0.31.x):重打的那份不再印裸 `> 内容`,改由
// cli::FormatUserPromptBlock 铺成整行淡底色块——与 resume/Ctrl+L 重放同一
// 颗 formatter,同一笔 transaction 里退 editing chrome、落 user surface
// (规格单"与 Enter 修复的交接":不先收成裸文本再回头涂背景,那会闪、
// 会在 scrollback 里重打一份)。折行宽度与 composer 同一套(LayoutComposer
// Rows 与 LayoutUserPromptBlock 都按显示宽断,CJK/emoji 不切半个宽字),
// 提交前后不忽然换行。plain 主题(Theme 全空 token)自动退化成老样子。
void CollapseBoxOnSubmit(int frame_top, int prompt_width, int prev_body_row_count,
                         const RenderState& state, const std::string& prompt, bool vt_enabled,
                         const Theme& theme) {
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
    // 用户块:同一颗 formatter 铺整行底色(plain 主题退成裸 "> 内容")。
    // 折行宽度与 composer 一致(首行容 "> ",续行缩两格,容量同为
    // buffer_width - 1 - padding*2 - 2),提交前后不忽然换行。
    const std::string block_text = FormatUserPromptBlock(Utf32ToUtf8(state.line), theme, buffer_width);
    const int block_rows = CountLines(block_text);
    const auto row_text = [&](std::size_t i) {
        return i == 0 ? prompt + Utf32ToUtf8(layout.rows[i].text)
                      : std::string(kContinuationIndent, ' ') + Utf32ToUtf8(layout.rows[i].text);
    };
    const std::size_t last = layout.rows.empty() ? 0 : layout.rows.size() - 1;
    const int final_x = (last == 0 ? prompt_width : kContinuationIndent) +
                        (layout.rows.empty() ? 0 : layout.rows[last].display_width);
    const int final_y = top + static_cast<int>(last);

    if (vt_enabled) {
        platform::TerminalBatch batch(info->viewport_x, info->viewport_y, platform::ProbeSyncOutputSupport());
        for (int r = top; r <= bottom; ++r) {
            batch.ClearRowHardFrom(0, r, buffer_width);
        }
        if (block_text.empty()) {
            // 空白提交退老路:裸 "> 内容" 一行(空块连色面都不给)。
            for (std::size_t i = 0; i < layout.rows.size(); ++i) {
                batch.MoveTo(0, top + static_cast<int>(i));
                batch.Write(row_text(i));
            }
        } else {
            batch.MoveTo(0, top);
            batch.Write(block_text);
        }
        batch.MoveTo(final_x, final_y);
        batch.Flush();
        return;
    }

    for (int r = top; r <= bottom; ++r) {
        platform::ClearRowHardFrom(0, r, buffer_width);
    }
    if (block_text.empty()) {
        for (std::size_t i = 0; i < layout.rows.size(); ++i) {
            platform::SetCursorPos(0, top + static_cast<int>(i));
            TermOut() << row_text(i);
            TermOut().flush();
        }
    } else {
        platform::SetCursorPos(0, top);
        TermOut() << block_text;
        TermOut().flush();
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
            TermOut() << "\x1b[?2004h" << std::flush;
        }
    }
    ~BracketedPasteScope() {
        if (enabled_) {
            TermOut() << "\x1b[?2004l" << std::flush;
        }
    }

private:
    bool enabled_;
};
}  // namespace

std::optional<std::string> ReadLineKeyByKey(const std::string& prompt, const Theme& theme, bool esc_rejects,
                                             bool composer, ReadExitReason* exit_reason) {
    // 整个函数体都攥着这把锁:M10 的 TurnInputListener 监听线程只在抢到锁
    // 的间隙才读控制台输入,这一行锁一上,就等于宣布"编辑器正在读",监听
    // 线程会自动让出、不跟这里抢同一份键盘输入。
    std::lock_guard<std::recursive_timed_mutex> console_read_lock(ConsoleReadMutex());

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
    editor.SetSlashCandidates(BuildSlashCompletionCandidates());
    editor.BeginLine(composer);

    // 0.17.0:composer 读取开输入框(上横线 + 按内容长高的正文区 + 下横线 +
    // 状态行)。0.29.x 起状态行之下还有代理导航坞贴底(整帧记账,见
    // RedrawEditArea);导航在下方长,EnsureRoomForRows 探底滚屏自会腾位,
    // 不再需要"锚点上方预留面板行数"那一步。
    const bool box = composer;
    BoxChrome chrome{box, &theme, editor.confirm_mode()};
    if (box) {
        const std::optional<platform::ScreenInfo> pre_info = platform::GetScreenInfo();
        const int console_width = pre_info.has_value() ? pre_info->width : 80;
        TermOut() << BoxRuleLine(theme, console_width) << "\n";
        for (int i = 0; i < kComposerTopPaddingRows; ++i) {
            TermOut() << "\n";
        }
    }

    // 0.21.x:提示符统一回归 `> `,不再冠 [auto]/[yolo] 档位前缀——档位改
    // 由常驻状态行(颜色 + 文字)承载,提示符不再重复一遍。
    TermOut() << prompt;
    TermOut().flush();

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
    // 钉在 tests/unit/agent/test_agent_panel.cpp)。空闲与流式监听共用同一份,选择按稳定
    // task id 记。
    AgentPanelSession& panel_session = PanelSessionSlot();
    std::string panel_fingerprint;  // 上一帧面板指纹(条目+状态机+成行),变了才重画
    std::string panel_notice;       // 停全部的回执,挂在提示行下面两秒就收
    std::chrono::steady_clock::time_point panel_notice_until{};
    int prev_frame_origin = -1;  // 上一帧的绝对帧顶;面板增减/滚屏后对不上就整帧重画

    // 场景按键帮助层(? 键位帮助只能展开不能收起单):帮助不是打进滚屏的
    // 一段输出,而是底栏帧最顶的一块 retained 层(help_rows 进 BottomChrome
    // Model/Frame)——开合、让位全走 HelpOverlayNext 状态机,展开时帧向上
    // 长高直接覆写可视对话(不写滚屏),收起时整屏重建把可视对话请回来,
    // 滚屏里从头到尾没有表。
    bool help_visible = false;

    if (composer) {
        SetComposerTarget(std::nullopt);  // 每次读取开始先归 main;首帧 build_panel 会按查看态再挂回去
    }

    auto panel_entries = [&]() -> std::vector<AgentPanelEntry> {
        if (!composer || !SessionAgentPanelHost().provider()) {
            return {};
        }
        return SessionAgentPanelHost().provider()();
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
    // dock_tints(监督器单 P1-1):与返回行按位对齐的监督色;toast/提示行
    // 插入时同步补 Normal,build_model 原样带走。
    std::vector<AgentHealthTint> dock_tints;
    const auto build_dock = [&](const std::vector<AgentPanelEntry>& entries,
                                std::string& tag_out) -> std::vector<std::string> {
        tag_out.clear();
        dock_tints.clear();
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
        dock_tints = DockRowTints(layout);
        // 小提示挂点:插在首行(操作提示)之下,只认非空坞。全部条目退场
        // (done+delivered/cancelled)或矮屏摆不下时 LayoutAgentDock 整坞
        // 不出场,lines 是空的——begin()+1 已越过 end(),insert 属未定义
        // 行为:MSVC 在新配的 1 格缓冲之外落笔,写出堆外。查看态完成退场
        // 一拍(结果交回 main、toast 刚挂上、场上再无活动代理)正踩中这条。
        // 空坞连提示一起不挂:没有可挂的帧,按过期自收,绝不越界落笔。
        const auto hang_dock_notice_row = [&lines, &tints = dock_tints](std::string row) {
            if (!lines.empty()) {
                lines.insert(lines.begin() + 1, std::move(row));
                tints.insert(tints.begin() + 1, AgentHealthTint::Normal);  // 行与色按位对齐
            }
        };
        if (!panel_notice.empty()) {
            if (std::chrono::steady_clock::now() < panel_notice_until) {
                // move 版 insert:提示行打完就不再用 panel_notice(走 else 分支
                // 才 clear),顺带绕开 GCC 13 对 const& 插入路径的
                // -Warray-bounds 误报(把内联后的栈上 string 认成地址零)。
                hang_dock_notice_row(std::move(panel_notice));
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
                hang_dock_notice_row(toast.text);
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
        if (!composer) {
            return {};  // 向导/确认提示是单行输入，不得把主 composer 的待发区带进面板
        }
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
    // slash 提示垫最底。Composer 合流 P1 起空闲路组的是 BottomChromeModel,
    // 行数账与指纹都出自唯一的 BuildBottomChromeLayout——与流式 footer 同
    // 一颗布局函数,两条路不许再各拼一套行序,也不许一边按逻辑行数报高、
    // 一边写死一行。
    const auto build_model = [&](const RenderState& state, const std::vector<std::string>& queue_rows,
                                 const std::vector<std::string>& dock, const std::string& tag,
                                 int selected_task_id) {
        BottomChromeModel model;
        model.framed = box;
        if (help_visible && state.line.empty()) {
            // 帮助层垫帧最顶:行内容出自 keymap(表头带实际和弦),摆位、
            // 淡色、按屏宽截断、高度预算钳制全归布局函数,与队列/坞同一本账。
            // 只属空 composer——草稿一有正文就不进帧(粘贴/取回这类不走逐键
            // 路径的入口,当拍也不闪帮助),状态机随后一拍正式收掉。
            model.help_rows = keymap::BuildSceneHelpLines(keymap::ActiveKeymap());
        }
        model.queue_rows = queue_rows;
        model.agent_dock_rows = dock;
        model.agent_dock_tints = dock_tints;  // build_dock 刚写的那份,按位对齐
        model.transient_rows = state.hint_lines;
        model.rule_tag = tag;
        model.selected_task_id = selected_task_id;
        model.composer.editor = state;
        model.composer.prompt = prompt;
        model.composer.mode = ComposerMode::Idle;
        model.composer.confirm_mode = editor.confirm_mode();
        if (box) {
            const std::optional<platform::ScreenInfo> info_now = platform::GetScreenInfo();
            const int width = info_now.has_value() ? info_now->width : 80;
            model.status_rows = {BuildComposerModeLine(
                chrome, static_cast<int>(SessionSkillCount()), (std::max)(0, width - 1))};
        }
        return model;
    };
    // 只算行数账不落笔(100ms 拍的指纹比较用);与真画那一帧出自同一颗
    // 布局函数,指纹才与画面一致。状态行文本不进指纹,宽一点窄一点无妨。
    // 高度预算与真画那一帧同一本账(战术二)。
    const auto build_frame = [&](const BottomChromeModel& model) {
        const std::optional<platform::ScreenInfo> info_now = platform::GetScreenInfo();
        const int width = info_now.has_value() ? info_now->width : 80;
        const int budget_rows =
                info_now.has_value()
                    ? (info_now->viewport_height > 0 ? info_now->viewport_height : info_now->height)
                    : 0;
        return BuildBottomChromeLayout(model, theme, width, budget_rows).chrome;
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
        if (!composer) {
            // `/provider add` 等向导也复用 ReadLine。主 composer 的键位速览、
            // slash 候选与临时菜单若漏进来，会占掉 WizardPanel 的输入余量，
            // 下一帧便可能按错锚点。单行读取只画 prompt 与正文。
            state.hint_lines.clear();
            state.selected_index = -1;
            return;
        }
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
        chrome.mode = editor.confirm_mode();
        const BottomChromeModel model =
            build_model(state, queue_rows, dock, tag, snapshot.selected_task_id);
        BottomChromeFrame frame;
        RedrawEditArea(start_row, model, theme, prev_body_row_count, previous_frame,
                       prev_frame_origin, vt_enabled, &frame);
        panel_fingerprint = fingerprint_of(entries, frame, snapshot);
    };

    // 内容铺完后的重锚(UI 按键回调路 / 视图切换路共用):重打上横线与提示
    // 符、重测锚点、作废旧帧、整帧重画。铺出的正文把旧 chrome 自然顶进滚屏。
    const auto reanchor_prompt_and_redraw = [&]() {
        // 重锚 = 上方刚铺了新正文,场景换了:帮助层跟着旧锚一起退场,
        // 不然重画会把帮助表盖在新铺的正文上(焦点导航/查看切换这类路)。
        help_visible = HelpOverlayNext(help_visible, HelpOverlayEvent::SceneChanged);
        if (box) {
            const std::optional<platform::ScreenInfo> rule_info = platform::GetScreenInfo();
            const int console_width = rule_info.has_value() ? rule_info->width : 80;
            TermOut() << BoxRuleLine(theme, console_width) << "\n";
            for (int i = 0; i < kComposerTopPaddingRows; ++i) {
                TermOut() << "\n";
            }
        }
        TermOut() << prompt;
        TermOut().flush();
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

    const auto retire_idle_chrome = [&]() {
        if (!box) {
            return;
        }
        const std::optional<platform::ScreenInfo> info = platform::GetScreenInfo();
        if (!info.has_value()) {
            return;  // 拿不到屏幕信息就不硬擦,退回旧行为(换行让位)
        }
        int top = prev_frame_origin;
        if (top < 0) {
            top = start_row > kComposerTopPaddingRows
                      ? start_row - kComposerTopPaddingRows - 1
                      : 0;
        }
        int bottom = top + prev_body_row_count;
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
        // 帮助层长在底栏帧最顶:底栏整帧退场,帮助跟着退,不单独留一块
        // 没人认领的表在屏上。收起路(再按/Esc)不走这里,那条要整屏重建
        // 恢复可视对话;这里只是让位(外部编辑器/转录导航/查看态切换)。
        help_visible = HelpOverlayNext(help_visible, HelpOverlayEvent::SceneChanged);
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
            TermOut() << theme.error << tr("editor.no_temp") << theme.reset << "\n";
            return;
        }
        {
            std::ofstream out(file, std::ios::binary | std::ios::trunc);
            if (!out) {
                TermOut() << theme.error << tr("editor.write_failed") << theme.reset << "\n";
                return;
            }
            out << draft;
            out.flush();
            if (!out) {
                std::error_code rm;
                fs::remove(file, rm);
                TermOut() << theme.error << tr("editor.write_failed") << theme.reset << "\n";
                return;
            }
        }
        retire_idle_chrome();
        TermOut() << "\n" << std::flush;
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
            TermOut() << theme.error << trf("editor.nonzero", exit_code) << theme.reset << "\n";
            reanchor_prompt_and_redraw();
            return;
        }
        if (!read_ok) {
            TermOut() << theme.error << tr("editor.file_gone") << theme.reset << "\n";
            reanchor_prompt_and_redraw();
            return;
        }
        if (!platform::IsValidUtf8(read_back)) {
            TermOut() << theme.error << tr("editor.bad_utf8") << theme.reset << "\n";
            reanchor_prompt_and_redraw();
            return;
        }
        const std::string normalized = NormalizeEditorDraft(read_back);
        editor.LoadTextWithCursor(Utf8ToUtf32(normalized), normalized.size());
        TermOut() << theme.stats << trf("editor.done", editor_cmd) << theme.reset << "\n";
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
        std::optional<platform::ScreenInfo> before;
        if (tail_rows == 0) {
            // 真切页(main <-> agent):上半屏是一块独立 Panel，整块换源。
            // 旧页哪怕滚过屏、锚点漂过，也不能在新页留半截正文。
            std::lock_guard<std::mutex> stdout_lock(StdoutWriteMutex());
            EraseStreamFooterLocked();
            before = ClearVisibleAgentPanelLocked();
            view_body_top.reset();
        } else {
            // 同一 agent 的实时尾帧刷新仍走原位擦铺，免得每秒整屏闪一下。
            erase_previous_view_body();
            before = platform::GetScreenInfo();
        }
        const auto& view_hook = AgentViewSwitchHookSlot();
        if (view_hook) {
            view_hook(viewed_after, tail_rows);
        }
        if (before.has_value()) {
            view_body_top = before->cursor_y;
        }
        // 跨读取账同步(见 ViewFrameLedgerSlot 注释):查看帧记"顶行+缓冲宽",
        // main 帧(退场/回 main)作废——重进 composer 时凭它判断上一帧还在
        // 不在原处。
        ViewFrameLedger& view_ledger = ViewFrameLedgerSlot();
        if (viewed_after != 0 && before.has_value()) {
            view_ledger.body_top = before->cursor_y;
            view_ledger.width = before->width;
        } else {
            view_ledger.body_top = -1;
        }
    };

    if (box) {
        // 进门先记查看态:第一帧 build_dock 会 OnEntriesChanged,查看的任务
        // 若在两段读取之间退场(后台回流轮置 delivered、x 清条目后重进),
        // viewed 在这一帧里翻 0——翻转发生在帧内,下面 100ms 拍的
        // viewed_before_tick 已经读不到旧值,原子回 main 得在这里补上。
        // 窗顶/缓冲宽也在首帧重画之前先量一拍:首帧自己就会平移视口(比如
        // 回流 toast 多出一行坞区)、把窗推向缓冲底,拿画完的屏面去对"跨
        // 读取账"会把好好在原处的帧误判成漂移——账要对着进门那一刻的屏面
        // 比(两段读取之间有没有人动过,与咱自己进门第一笔无关)。
        const int viewed_before_entry = panel_session.SnapshotFor(nav_ids_for(panel_entries())).viewed_task_id;
        const std::optional<platform::ScreenInfo> entry_info = platform::GetScreenInfo();
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
            // 消息后重进 composer……):本段的 view_body_top 还是空的——不设
            // 账的话,下一拍实时流重铺会在旧帧下方再铺一份,查看帧成双。
            // 先查跨读取账(ViewFrameLedgerSlot),三条判据全过就"原处认账"
            // (回流单规格第一节"查看帧零扰动"):窗浮在长缓冲里(内容行不可
            // 能被滚,平移只挪窗)、缓冲宽没变、期间没有非静默轮写屏(静默
            // 回流轮一个字不上屏,正是这形态)——上一帧稳稳在原处,锚点直
            // 接采纳,画面一个像素不动,上方主会话正文也不被抹。任一不满足
            // 则绝对行号已失准,走原来的"不认跨拍锚点"规矩:可视区整块清
            // 掉、该代理的查看帧整份重铺,帧账从这一帧重新起。
            const ViewFrameLedger& view_ledger = ViewFrameLedgerSlot();
            const std::optional<platform::ScreenInfo> clear_info = platform::GetScreenInfo();
            const bool window_floats =
                entry_info.has_value() &&
                entry_info->viewport_y + entry_info->viewport_height < entry_info->height;
            if (view_ledger.body_top >= 0 && window_floats && entry_info.has_value() &&
                view_ledger.width == entry_info->width &&
                view_ledger.body_top >= entry_info->viewport_y) {
                view_body_top = view_ledger.body_top;  // 上一帧原处认账,零扰动
            } else if (clear_info.has_value()) {
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

    // 帮助层展开前的沉底(短会话形态):提示符若停在窗腰(下方还有整片
    // 空行),帮助表上方装不下。终端里内容只能上移不能下移,但底栏是自家
    // 帧——擦掉旧帧、把锚点挪到窗底重画,上方的净空就出来了。不滚屏、
    // 不写滚屏历史,收起时 rebuild_screen 自会按 transcript 重锚回原位。
    // 返回真 = 真沉了,调用方需要再画一帧。
    const auto dock_chrome_to_bottom_for_help = [&](int help_rows) -> bool {
        if (prev_frame_origin < 0 || help_rows <= 0) {
            return false;  // 没有上一帧的账可对,不硬挪
        }
        const std::optional<platform::ScreenInfo> info = platform::GetScreenInfo();
        if (!info.has_value()) {
            return false;
        }
        const int vh = info->viewport_height > 0 ? info->viewport_height : info->height;
        const int rows_below_anchor =
                (std::max)(0, prev_frame_origin + prev_body_row_count - start_row);
        const int max_start =
                (std::min)(info->viewport_y + vh - 1, info->height - 1) - rows_below_anchor;
        // 帧顶到视口顶的余地(帮助层从新帧顶向上铺,盖的是可视对话,只受
        // "不画出视口"这一条约束)。
        const int rows_above_now = prev_frame_origin - info->viewport_y;
        if (help_rows <= rows_above_now || start_row >= max_start) {
            return false;  // 上方本就装得下,或已贴底无处可沉
        }
        const bool keep_help = help_visible;
        retire_idle_chrome();   // 擦旧帧、帧账归零(顺带把 help_visible 归 false)
        help_visible = keep_help;  // 沉底不是退场:帮助层跟着新锚一起重画
        start_row = max_start;
        return true;
    };

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
            TermOut() << BoxRuleLine(theme, console_width) << "\n";
            for (int i = 0; i < kComposerTopPaddingRows; ++i) {
                TermOut() << "\n";
            }
        }
        TermOut() << prompt;
        TermOut().flush();
        if (const std::optional<platform::ScreenInfo> after_info = platform::GetScreenInfo();
            after_info.has_value()) {
            start_row = after_info->cursor_y;
            prompt_end_col = after_info->cursor_x;
        }
        prev_body_row_count = 0;
        previous_frame.reset();
        prev_frame_origin = -1;
        redraw_with_panel(editor.CurrentRenderState(), panel_entries());
        // 帮助层还开着的话补一笔沉底:重建把提示符重锚到 transcript 尾——
        // 短会话里那就是窗顶,上方没了余地,帮助会被锚点护栏钳到零行
        // (resize 改宽后"表格重排"正是这条路)。照开合那套再沉一次,表
        // 跟着新锚回来。
        if (help_visible && editor.CurrentRenderState().line.empty() && composer) {
            if (dock_chrome_to_bottom_for_help(
                    static_cast<int>(keymap::BuildSceneHelpLines(keymap::ActiveKeymap()).size()))) {
                redraw_with_panel(editor.CurrentRenderState(), panel_entries());
            }
        }
    };

    // 收帮助层(再按和弦/Esc/场景换):整屏重建一条路——可视区清干净、
    // 正文从 transcript 快照重铺、提示符重锚、底栏整帧重画。帮助展开时
    // 是直接覆写在对话上的,光擦帮助行只会留一片空白,重建才对得起
    // "收起后恢复原 composer、底栏与可视对话"。滚屏从头到尾没有表,
    // 重建也一张表都不添。
    const auto close_scene_help = [&]() {
        if (!help_visible) {
            return;
        }
        help_visible = HelpOverlayNext(help_visible, HelpOverlayEvent::SceneChanged);
        rebuild_screen();
    };

    // resize 探测:宽变了走整屏重建(下一拍内完成),不能等指纹——
    // 尺寸不在指纹里,而 conhost 重排过的旧行靠增量 diff 擦不净。
    // 高变了只重画底栏,不清历史——拉高窗口时保留滚动缓冲区的对话记录。
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
                if (last_screen_width != -1) {
                    const bool width_changed = size_info->width != last_screen_width;
                    const bool height_changed = size_info->height != last_screen_height;
                    if (width_changed) {
                        // 宽度变了:conhost 会重排旧行,必须整屏重建
                        rebuild_screen();
                    } else if (height_changed) {
                        // 只是高度变了:重画当前输入框与底栏,不清历史滚动记录
                        previous_frame.reset();
                        prev_frame_origin = -1;
                        redraw_with_panel(editor.CurrentRenderState(), panel_entries());
                    } else if (prev_frame_origin >= 0) {
                        // 帧整体漂出可视区(改宽连动滚屏/窗口被外部强拉,宽高
                        // 都没再变的那拍):绝对锚点画帧全都落在窗外,屏上空白、
                        // 键入无回显——像死了,其实活着。锚点在窗外就整屏重建,
                        // 把帧拉回可视区(改宽瞬间流式疑停摆单:改宽锤击下屏面
                        // 全空、composer 不见,进程与输入全健在)。
                        const int frame_bottom = prev_frame_origin + prev_body_row_count + 1;
                        const int vp_height =
                            size_info->viewport_height > 0 ? size_info->viewport_height : size_info->height;
                        if (frame_bottom < size_info->viewport_y ||
                            prev_frame_origin > size_info->viewport_y + vp_height - 1) {
                            rebuild_screen();
                        }
                    }
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
            const BottomChromeFrame frame =
                build_frame(build_model(tick_state, queue_rows, dock, tag, snapshot.selected_task_id));
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
                if (help_visible && !tick_state.line.empty()) {
                    // 草稿非空了(粘贴/取回一类不走逐键路径的入口):场景换
                    // 了,帮助层整屏重建收掉,可视对话回来。
                    help_visible = HelpOverlayNext(help_visible, HelpOverlayEvent::DraftFilled);
                    rebuild_screen();
                } else {
                    redraw_with_panel(editor.CurrentRenderState(), entries);
                }
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
                    TermOut() << "\n";
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
            // Alt+V / Ctrl+V 共用的"PNG 在手,落盘插路径"段:PNG 落受限临时
            // 文件,光标处插 @<路径>(提交时走既有视觉附件路,尺寸/超限校验
            // 在那头还有一道),打一行回执再重画。落盘失败只报错,原草稿
            // 不动。
            const auto paste_clipboard_png = [&](const std::vector<unsigned char>& png) {
                namespace fs = std::filesystem;
                static unsigned paste_seq = 0;
                fs::path file;
                try {
                    file = fs::temp_directory_path() /
                           ("lubancode-paste-" + std::to_string(platform::CurrentProcessId()) +
                            "-" + std::to_string(++paste_seq) + ".png");
                } catch (const std::exception&) {
                    TermOut() << theme.error << tr("editor.no_temp") << theme.reset << "\n";
                    redraw_with_panel(editor.CurrentRenderState(), entries_before_key);
                    return;
                }
                {
                    std::ofstream out(file, std::ios::binary | std::ios::trunc);
                    if (!out) {
                        TermOut() << theme.error << tr("editor.write_failed") << theme.reset << "\n";
                        redraw_with_panel(editor.CurrentRenderState(), entries_before_key);
                        return;
                    }
                    out.write(reinterpret_cast<const char*>(png.data()),
                              static_cast<std::streamsize>(png.size()));
                }
                // 光标处插 @<路径>(临时路径常带空格,一律角括号形)。
                const RenderState before = editor.CurrentRenderState();
                const std::string token = "@<" + lubancode::tools::PathToUtf8(file) + "> ";
                std::string joined = Utf32ToUtf8(before.line);
                const std::size_t at = (std::min)(before.cursor, joined.size());
                joined.insert(at, token);
                editor.LoadTextWithCursor(Utf8ToUtf32(joined), at + token.size());
                TermOut() << theme.stats
                          << trf("image.pasted", png.size() / 1024,
                                 lubancode::tools::PathToUtf8(file))
                          << theme.reset << "\n";
                redraw_with_panel(editor.CurrentRenderState(), entries_before_key);
            };
            const auto search_chord = keymap::ChordFromKeyInput(*raw_key);
            if (!history_search.active() && !queue_edit.has_value() && search_chord.has_value()) {
                // 帮助层开着时 Esc 优先收帮助(改绑 help.show 后的确定出口),
                // 不落给编辑器(老语义清草稿/停 loop)也不落给面板(逐层退)。
                if (help_visible && search_chord->key == keymap::KeyChord::Key::Esc &&
                    !search_chord->ctrl && !search_chord->alt) {
                    help_visible = HelpOverlayNext(help_visible, HelpOverlayEvent::EscapePressed);
                    rebuild_screen();
                    continue;
                }
                using keymap::ActionId;
                switch (keymap::ActiveKeymap().Lookup(keymap::KeyScope::Composer, *search_chord)) {
                    case ActionId::ChatSearchHistory: {
                        // Ctrl+R 反向搜索:原草稿整份存起(取消时一字不少装
                        // 回),编辑器清成空查询。没有数据源(单发/管道/未注册)
                        // 时这个键不消费,落回编辑器原语义。场景换了,帮助层
                        // 先收(它列的是空闲场景的键,搜索开着便词不达意)。
                        if (!PromptHistoryProviderSlot()) {
                            break;
                        }
                        close_scene_help();
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
                                // 取回的正文一进草稿,帮助层就让位(场景换了)。
                                help_visible =
                                        HelpOverlayNext(help_visible, HelpOverlayEvent::DraftFilled);
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
                        // '?' 场景帮助(可开可合的临时层):只列当前场景有效键,
                        // 键位从 keymap 反查,用户改键后表头/表尾跟着改。空
                        // composer 才当帮助,有正文时 '?' 是普通字符。展开=
                        // 帮助行进底栏帧最顶(覆写可视对话,不写滚屏);收起=
                        // 整屏重建恢复可视对话。同一枚键,同一个动作。
                        if (!editor.CurrentRenderState().line.empty()) {
                            break;
                        }
                        const bool next_visible =
                                HelpOverlayNext(help_visible, HelpOverlayEvent::TogglePressed);
                        if (next_visible) {
                            // 展开:短会话里提示符若停在窗腰,先把底栏帧沉到
                            // 窗底,给整张表腾出净空(装不下的尾巴由高度预算
                            // 钳制,保头舍尾)。行内容在 build_model 里按
                            // help_visible 现拼,重画自会带上。
                            dock_chrome_to_bottom_for_help(
                                    static_cast<int>(keymap::BuildSceneHelpLines(
                                                             keymap::ActiveKeymap())
                                                             .size()));
                            help_visible = true;
                            redraw_with_panel(editor.CurrentRenderState(), entries_before_key);
                        } else {
                            help_visible = next_visible;  // 状态机翻面(false)
                            rebuild_screen();
                        }
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
                        TermOut() << "\n";
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
                            TermOut() << theme.error
                                      << trf("image.paste_failed", paste_error)
                                      << theme.reset << "\n";
                            redraw_with_panel(editor.CurrentRenderState(), entries_before_key);
                            continue;
                        }
                        paste_clipboard_png(*png);
                        continue;
                    }
                    case ActionId::ClipboardSmartPaste: {
                        // Ctrl+V 智能粘贴:先探剪贴板位图——有图走 Alt+V 同
                        // 一条直贴路;没图读剪贴板文本,交编辑器的 Paste 事件
                        //(与终端 bracketed paste 同一条正文插入路,多行/大段
                        // 折附件的手感分毫不差)。两头都读不了才如实报一行,
                        // 不装作贴过。终端把 Ctrl+V 拦去自己粘贴(Windows
                        // Terminal 等)的场合根本收不到这枚和弦,日常文本粘贴
                        // 手感不变;Alt+V 保留不撤。
                        if (platform::ClipboardHasImage()) {
                            std::string paste_error;
                            const auto png =
                                platform::ReadClipboardImagePng(kMaxImageBytes, paste_error);
                            if (!png.has_value()) {
                                TermOut() << theme.error
                                          << trf("image.paste_failed", paste_error)
                                          << theme.reset << "\n";
                                redraw_with_panel(editor.CurrentRenderState(), entries_before_key);
                                continue;
                            }
                            paste_clipboard_png(*png);
                            continue;
                        }
                        std::string text_error;
                        const auto text = platform::ReadClipboardTextUtf8(text_error);
                        if (!text.has_value()) {
                            TermOut() << theme.error
                                      << trf("clipboard.paste_text_failed", text_error)
                                      << theme.reset << "\n";
                            redraw_with_panel(editor.CurrentRenderState(), entries_before_key);
                            continue;
                        }
                        (void)editor.HandleKey(KeyEvent::Paste(*text, 0));
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
                                            state, prompt, vt_enabled, theme);
                    }
                    TermOut() << "\n";
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
                // 取回的条目进草稿,帮助层让位(正文非空,帮助帧当拍就退,
                // 状态一拍内正式收平)。
                help_visible = HelpOverlayNext(help_visible, HelpOverlayEvent::DraftFilled);
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
                    const AgentPanelActions& actions = SessionAgentPanelHost().actions();
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
                    const AgentPanelActions& actions = SessionAgentPanelHost().actions();
                    if (selected_entry != nullptr) {
                        if (selected_entry->running && actions.cancel_task) {
                            // 停止回执(子代理 x 停止失效单):按 CancelTask 的
                            // 返话出 toast——已受理(行随后显"停止中")或已不在
                            // 运行,不再静默吞掉,面板 x 不当死键。
                            const bool accepted = actions.cancel_task(selected_entry->task_id);
                            panel_notice = accepted ? trf("agent_panel.stop_notice", selected_entry->task_id)
                                                    : trf("agent_panel.stop_not_running", selected_entry->task_id);
                            panel_notice_until = std::chrono::steady_clock::now() + std::chrono::seconds(2);
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
                    const int frame_top = prev_frame_origin >= 0 ? prev_frame_origin : start_row;
                    int last_row = frame_top + prev_body_row_count;
                    if (last_row >= before_info->height) {
                        last_row = before_info->height - 1;
                    }
                    platform::SetCursorPos(0, last_row);
                }
                const bool handled = UiHandlerSlot()(*action);
                if (handled) {
                    if (relay_frame_top.has_value()) {
                        view_body_top = relay_frame_top;  // 重铺帧的顶,下一次切换照账擦
                        // 跨读取账同步:Ctrl+O 重铺挪了查看帧的顶,下一段
                        // 读取的"原处认账"要认新顶(缓冲宽取当前值)。
                        ViewFrameLedger& view_ledger = ViewFrameLedgerSlot();
                        view_ledger.body_top = *relay_frame_top;
                        if (const std::optional<platform::ScreenInfo> relay_ledger_info =
                                platform::GetScreenInfo();
                            relay_ledger_info.has_value()) {
                            view_ledger.width = relay_ledger_info->width;
                        }
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
            // 0.17.0 输入框化的提交收尾:横线/状态行/面板行擦掉,提交内容
            // 铺成用户输入背景块(与 resume/Ctrl+L 同源,见 CollapseBoxOnSubmit
            // 注释)。帧顶用上一帧记的绝对帧顶(含面板行与上横线)。
            CollapseBoxOnSubmit(prev_frame_origin, prompt_end_col, prev_body_row_count, state, prompt,
                                vt_enabled, theme);
            TermOut() << "\n";
            if (exit_reason != nullptr) {
                *exit_reason = ReadExitReason::Submitted;
            }
            return Utf32ToUtf8(state.line);
        }

        if (help_visible && !state.line.empty()) {
            // 帮助开着,草稿却有了正文(打的字当拍就到这):场景换了,整屏
            // 重建收帮助——对话回来,光标留在新正文上,一字不动。
            help_visible = HelpOverlayNext(help_visible, HelpOverlayEvent::DraftFilled);
            rebuild_screen();
            state = editor.CurrentRenderState();  // 重建已按最新状态重画,别再叠一帧
        } else {
            redraw_with_panel(state, entries_before_key);
        }

        if (state.esc_pressed && esc_rejects) {
            // 确认与可取消选择场景:Esc 不留在循环里继续等，直接交回
            // nullopt。不能拿空串代替——/model 明明把空串当默认第一项。
            TermOut() << "\n";
            if (exit_reason != nullptr) {
                *exit_reason = ReadExitReason::Esc;
            }
            return std::nullopt;
        }
        if (state.eof_requested) {
            // Ctrl+D/Ctrl+Z 可能按在多行 composer 中间某一行,框下面还垫着
            // 横线/状态行:先把光标挪到整帧最下面一行再换行,免得接下来的
            // 输出打在残留画面身上。帧顶不能拿 start_row 代替——队列或
            // 自定义 padding 仍可能压在输入行上头。
            if (queue_edit.has_value()) {
                // 整次读取要退场了:未提交的编辑按 Esc 同款还原,不留冻结条目。
                steering.CancelEdit(*queue_edit);
                queue_edit.reset();
            }
            if (prev_body_row_count > 0) {
                const int frame_top = prev_frame_origin >= 0 ? prev_frame_origin : start_row;
                platform::SetCursorPos(0, frame_top + prev_body_row_count);
            }
            TermOut() << "\n";
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
            TermOut() << "\n";
            if (exit_reason != nullptr) {
                *exit_reason = ReadExitReason::Submitted;
            }
            return Utf32ToUtf8(state.line);
        }
    }
}

// ReadLine 壳的帧账审计收尾(骨架拆解反弹·问题 5:随 IdleFrameAudit 从
// 原文件搬来,行为一字未改;LUBANCODE_FRAME_AUDIT 置位才落笔)。
void ReportAndResetIdleFrameAudit() {
    if (!FrameAuditEnabled()) {
        return;
    }
    // 这次读取(一次输入事务,打字/粘贴/编辑都在内)真画了几帧、写了
    // 多少字节、光标落在哪——P2-4 验收的账从这收。
    const IdleFrameAudit& audit = IdleFrameAuditSlot();
    std::string cursor_note;
    if (const std::optional<platform::ScreenInfo> info = platform::GetScreenInfo(); info.has_value()) {
        cursor_note = " cursor=(" + std::to_string(info->cursor_x) + "," + std::to_string(info->cursor_y) + ")";
    }
    TermErr() << "[frame-audit] idle_composer frames=" << audit.frames << " bytes=" << audit.bytes
              << cursor_note << "\n";
    TermErr().flush();
    IdleFrameAuditSlot() = IdleFrameAudit{};
}

void SetTranscriptUiHandler(TranscriptUiHandler handler) { UiHandlerSlot() = std::move(handler); }

void ShowPanelToast(const std::string& text) {
    PanelToastSlot().text = text;
    PanelToastSlot().until = std::chrono::steady_clock::now() + std::chrono::seconds(4);
}

void SetIdleWakeHook(std::function<bool()> hook) { IdleWakeHookSlot() = std::move(hook); }

void SetBackgroundNoticeHook(std::function<void()> hook) { BackgroundNoticeHookSlot() = std::move(hook); }

void SetTurnInterruptBroadcast(std::function<void()> hook) { TurnInterruptBroadcastSlot() = std::move(hook); }

void BroadcastTurnInterrupted() {
    if (const auto& broadcast = TurnInterruptBroadcastSlot()) {
        broadcast();
    }
}

void SetPromptHistoryProvider(PromptHistoryProvider provider) {
    PromptHistoryProviderSlot() = std::move(provider);
}

bool ComposerStashHasContent() { return ComposerStashSlot().has; }

ComposerStashSnapshot ComposerStashPeek() { return ComposerStashSlot(); }

void ComposerStashDiscard() { ComposerStashSlot() = ComposerStashSnapshot{}; }

void SetFileMentionProvider(FileMentionProvider provider) {
    FileMentionProviderSlot() = std::move(provider);
}

}  // namespace lubancode::cli
