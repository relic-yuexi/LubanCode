// 全程序唯一的 stdin 读入口。存在的理由是绕开一个 Windows 老毛病:
// main.cpp 里 SetConsoleCP(CP_UTF8) 之后,窄字符 std::getline(std::cin, ...)
// 读中文,在真控制台(conhost)下会跟 ReadFile 的 CP_UTF8 支持撞车——
// typed 的多字节字符偶发读空或读乱,尤其是几次 ReadFile 交替调用之后
// (交互模式里"主提示符读一行"跟"工具确认读一行"正好就是这种交替)。
// 见 console_input.cpp 开头注释,写了实测结论。
//
// M6.5 把真控制台这条路从"整行读入(ReadConsoleW)"升级成"逐键输入编辑器"
// (核心逻辑在 cli/line_editor.hpp 的 LineEditorCore,不认 Win32,可单测;
// 这里只是拿真实按键喂它、按它吐出来的 RenderState 重画屏幕),换来方向键
// 移光标、上下键翻历史、Tab 补全 slash 命令、Shift+Tab 循环切确认模式这些
// 花活。管道/重定向场景完全不受影响,还是走最下面的 std::getline 老路,
// 一个字节都没改。

#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "cli/agent_panel.hpp"  // AgentPanelEntry/Actions(面板纯逻辑层)
#include "cli/choice_menu.hpp"
#include "cli/format_utils.hpp"  // StatusPanelData(状态行数据源)
#include "cli/history_search.hpp"  // PromptHistoryDataset(Ctrl+R 数据)
#include "cli/line_editor.hpp"
#include "cli/mention_menu.hpp"  // FileMentionEntry(@ 提及数据)
#include "cli/queue_model.hpp"
#include "cli/theme.hpp"

namespace lubancode::cli {

// 打印 prompt(不含换行,可传空串跳过打印),读一行输入。
// Windows 下 stdin 是真控制台(GetFileType == FILE_TYPE_CHAR)时,走逐键
// 输入编辑器(方向键/历史/Tab 补全/Shift+Tab 切模式都在这条路上);
// stdin 是管道/重定向文件时,回退到 std::getline(保住
// `echo "x" | lubancode.exe` 这种自动化用法和集成测试,行为跟升级前完全
// 一致)。统一剥掉行尾的 \r\n。EOF(Ctrl+Z/Ctrl+D 或管道读尽)返回
// std::nullopt。
//
// theme 只用来给 slash 补全提示行、Shift+Tab 模式切换通知上色;不传就是
// 默认构造的空 Theme(没有颜色转义,不影响功能,只是没有颜色)。
//
// esc_rejects:M10 新增,给确认提示和可取消的编号选择用。true 时,按下 Esc
// 不再走"清空当前行、留在同一次读取里继续等"那条空闲编辑态的路,而是立刻
// 返回 nullopt。确认回调把它当拒绝；/model 把它当取消，故空行仍能选默认
// 第一项，Esc 却不会误选。默认 false，不影响主提示符与普通向导。
//
// composer:UI-A(0.11.0)多行 composer 开关,只有 main.cpp 的 `> ` 主提示符
// 传 true。真控制台下开了它:Alt+Enter / Shift+Enter 在光标处插换行(实测
// 这台机器 Windows Terminal 会吞 Alt+Enter——默认绑成全屏切换,keydown 根本
// 到不了输入缓冲;conhost 两个都收得到。所以 /help 里推荐 Shift+Enter),
// Enter 把全部行拼 '\n' 一次发出,全空白 composer 按 Enter 原地不动;编辑区
// 占 N 行,提示符只在第一行,后续行两空格续行缩进。默认 false 的调用点
// (确认提示、/model 编号选择、向导)单行语义跟升级前一字不差。管道/重定向
// 模式没有 composer 概念,这个参数完全没作用,照旧逐行 getline。
// 0.30.x 向导重排单:一次读取"为什么返回了"。Submitted = 正常提交(有返回
// 值);Esc = 按了 Esc(向导拿来当"返回上一步");Cancel = Ctrl+C / Ctrl+D /
// 管道读尽这类"整条流程别继续了"。只区分到这一层——向导的取消路对
// Ctrl+C 与 EOF 同等待遇,不必再细分。现有调用方不传指针,行为一字不变。
enum class ReadExitReason { Submitted, Esc, Cancel };

std::optional<std::string> ReadLine(const std::string& prompt, const Theme& theme = Theme{},
                                     bool esc_rejects = false, bool composer = false,
                                     ReadExitReason* exit_reason = nullptr);

// ask_user 的逐键选择菜单。单选用上下键移动、Enter 确认；多选用空格
// 勾选、Enter 提交。核心状态与终端绘制分开，方向键/多选规则可直接单测。
struct ChoiceMenuItem {
    std::string label;
    std::string description;
};

struct ChoiceMenuOptions {
    bool multi_select = false;
    std::optional<std::size_t> editable_index;
    std::optional<std::size_t> initial_cursor;  // 初始高亮项(0-based);不设则从首项起
    std::string hint;
    std::string invalid_hint;
    std::string editable_hint;
    std::string editable_placeholder = "____________";
};

struct ChoiceMenuResult {
    std::vector<std::size_t> selected_indices;
    std::optional<std::string> custom_text;
};

// 真终端绘制原地选择菜单。editable_index 指向的末项可直接键入文字，
// 返回普通选中项与行内文本；Esc/Ctrl+C/EOF 返回 nullopt。exit_reason
// (向导重排单)可选:Esc 填 Esc(向导当"返回上一步"),Ctrl+C/Ctrl+D/EOF
// 填 Cancel;选中则填 Submitted。不传行为一字不变。
std::optional<ChoiceMenuResult> ReadChoiceMenu(const std::vector<ChoiceMenuItem>& items,
                                                const ChoiceMenuOptions& options, const Theme& theme,
                                                ReadExitReason* exit_reason = nullptr);

// 会话级确认模式的查询/设置。真控制台下 Shift+Tab 会改这个状态(存在
// ReadLine() 内部维护的、贯穿整条交互会话的 LineEditorCore 实例里,见
// console_input.cpp 的 SharedEditor());main.cpp 的工具确认回调、主提示符
// 前缀都读这个。--yes 等价于启动时调一次
// SetConfirmMode(ConfirmMode::Yolo)。管道/重定向模式下这两个函数依然可用
// (状态本身不依赖真实控制台),只是永远不会被 Shift+Tab 改变——管道场景
// 根本读不到"按键",只有整行文本。
ConfirmMode CurrentConfirmMode();
void SetConfirmMode(ConfirmMode mode);

// AllSlashCommands() -> LineEditorCore 补全候选的唯一转换口:空闲 composer
// 的 SharedEditor() 与流式监听线程(TurnInputListener)的本地编辑器都从这
// 里拿候选。命令清单仍只有 slash_commands 那一份——命令增删、i18n 说明变
// 化后两只 composer 同一拍生效,不靠维护者记得改第二处。每次现转、不留静
// 态副本(语言切换后下一只编辑器自然拿到新说明)。
std::vector<CompletionCandidate> BuildSlashCompletionCandidates();

// UI-D(0.16.0):等输入期间(composer 主提示符)按下 Ctrl+O / Ctrl+E /
// 空 composer Tab / Shift+Tab / ESC(聚焦查看态返回用)时,终端层把语义
// 转发给应用层的动作。终端层自己零 transcript 知识:回调打印什么它不管,
// 回调返回 true(真干了事)就当"编辑区下面铺了一段新内容"处理——重打
// 提示符、重测锚点、按当前编辑内容原样重画;返回 false 当这个键没被消费,
// 按原语义继续(比如 Escape 不在聚焦查看态时还是"清空输入")。
enum class UiKeyAction {
    ToggleExpand,  // Ctrl+O:紧凑/详细全局切换
    FocusOlder,    // 焦点往旧走(0.17.0:空 composer Tab 进焦点态选最近一条,焦点态内 Tab)
    FocusNewer,    // 焦点往新走(0.17.0:焦点态内 Shift+Tab;态外 Shift+Tab 恒切确认档,不发这个)
    FocusView,     // Ctrl+E:聚焦查看当前焦点条目(已在聚焦态则返回)
    Escape,        // ESC:回调只在聚焦查看态消费它(返回会话画面),否则还给编辑器
    // Ctrl+L 整屏重画:终端层已作废帧锚点、清了可视区,应用层把 transcript
    // 快照重铺一遍(横幅+最近条目)。回调返回 true 表示真铺了正文。
    RepaintScreen,
    // 0.30.x 第四批转录导航(空 composer 的 { } [ v,键位走 keymap):
    PrevUserTurn,  // {:跳上一条用户提问,状态行写"第 N/M 轮"
    NextUserTurn,  // }:跳下一条
    ToScrollback,  // [:完整转录写进终端 scrollback(用终端自带搜索)
    ViewInEditor,  // v:转录写临时 Markdown 交 $VISUAL/$EDITOR 只读查看
};
using TranscriptUiHandler = std::function<bool(UiKeyAction)>;

// 注册/清除会话级的 UI 按键回调(InteractiveLoop 进循环前注册,退出前必须
// 清掉——回调抓着循环里的局部引用)。传空 handler 即清除。管道/重定向模式
// 走不到逐键路径,注册了也永远不会被调,天然无感。
void SetTranscriptUiHandler(TranscriptUiHandler handler);

// 会话内后台子代理导航坞(0.29.x 起画在 composer 下横线与状态行之后贴底)。
// 数据由应用层给,终端层只管选择与绘制:空 composer 按 ↑/↓ 进入代理焦点,
// Enter 设置 viewed_task_id(上方会话视口整块换源、composer 收件目标切到
// 这只子代理),Esc 逐层退出,x 停止/清除当前条目,Ctrl+X Ctrl+K 两段确认
// 停止全部。主会话固定算第 0 项,provider 只返回后台子代理项。条目结构/
// 动作/按键状态机在 cli/agent_panel.hpp。导航坞只放导航——查看态的长正文
// (完整 prompt、工具调用流水、结论)走下面那只视图切换钩子,整块换进
// 上方会话视口,不向坞下方生长。
using AgentPanelProvider = std::function<std::vector<AgentPanelEntry>()>;
void SetAgentPanelProvider(AgentPanelProvider provider);

// 视图切换钩子:viewed_task_id 变了(Enter 切进某只子代理 / Esc 回 main)
// 终端层调这个,应用层把"此刻该看的会话正文"铺出来——0 = 重铺 main 的
// 最近条目,task_id = 铺那只子代理的完整 transcript(prompt/工具调用/结果/
// 错误)。tail_rows = 0 整份铺(真切会话);>0 是实时流的重铺拍(追加需求
// "查看态实时思考流"):应用层只保头几行+最近 tail_rows 行,长会话不往滚屏
// 一秒刷一遍。钩子只打印、不擦旧帧(查看态完成退场花屏单,2026-08-17):
// 旧查看帧的擦账只有终端层 view_body_top 那一本,两条路(空闲 ReadLine/
// 流式监听)调钩子前都已按账擦净旧帧、把光标摆到帧顶;流式路钩子自己
// (在 StdoutWriteMutex 内)先擦 footer 再铺、铺完画回。传空钩子即清除。
void SetAgentViewSwitchHook(std::function<void(int viewed_task_id, int tail_rows)> hook);

// 面板动作(x 停止/清除、两段确认停全部)的接线口。终端层不直接碰
// AgentTool;停止必须走正式取消接口,等任务线程报终态再改灯。
void SetAgentPanelActions(AgentPanelActions actions);

// 会话收场(/clear、退出、切 worktree)用:把面板状态机(焦点/查看态/
// composer 收件目标)整份收干净——查看态那只任务已经没了,目标不能悬着。
// 管道/重定向模式下面板本来就不存在,调了也是空操作。
void ResetAgentPanelSession();

// 这一次 composer 读取的"收件目标":进入某只后台子代理查看态后,面板
// 控制器记着它的任务号;ReadLine 返回后应用层取这个,把提交的消息定向送
// 进那只子代理的 inbox(不经 main history)。nullopt = 归 main。每次
// ReadLine(composer) 开头清零;Esc 退查看态/切回 main/条目被清理时也随之
// 清零。管道/重定向模式走不到逐键路径,恒 nullopt。
std::optional<int> CurrentComposerAgentTarget();

// 查看态现值:会话层面板控制器的 viewed_task_id(0 = main)。与上面那只
// "收件目标"同源,但生命周期不跟 ReadLine(读取返回后仍准确)——主循环
// 在后台回流路上问它:正看着某只子代理时回流须静默,这个数是唯一真状态。
int CurrentAgentViewedTaskId();

// 查看态回流的短提示(toast):后台收货完,把"谁完成了"挂进导航坞的
// 提示行位置,几秒后随下一帧自动收——不抢屏、不进对话流、不碰上方查看帧。
// 与空闲路本地的 panel_notice 同一挂点;写点在会话主循环、读点在空闲
// composer 的 100ms 帧,都归主线程,不用加锁。
void ShowPanelToast(const std::string& text);

// 空闲唤醒钩子:composer 主提示符在逐键等待期间,每 100ms 的面板刷新一拍
// 顺带问一次;返回 true 表示应用层有系统侧事件要在会话空闲时处理(比如
// 后台子代理跑完、结果等着交回主代理),ReadLine 以空串返回让调用方的循环
// 顶去办自己的事。只在 composer 为空时问——用户敲了一半的正文不抢,等
// 提交后再说。传空钩子即清除;管道/重定向走不到逐键路径,设了也永不触发。
void SetIdleWakeHook(std::function<bool()> hook);

// 后台通知钩子(后台代理权限拒绝无告知单,2026-08-17):空闲 composer 每
// 100ms 的拍里叫一声,应用层把攒着的"当场要让人知道"的系统侧通知取走、
// 自己落账(导航坞 toast + transcript 事件)——比如后台子代理的 needs_confirm
// 工具被拒,用户当拍就能看见,不用等最终报告。与 IdleWakeHook 不同,这个
// 不让位、不起轮,纯通知。传空钩子即清除;管道/重定向走不到逐键路径。
void SetBackgroundNoticeHook(std::function<void()> hook);

// Ctrl+R 提问历史反向搜索的数据源(0.30.x 第二批):应用层从 session 事件
// 账只读现抽一份 PromptHistoryDataset(打开搜索框时取一次,范围轮换在
// 终端层本地过滤,不反复读盘)。传空清除;管道/重定向走不到逐键路径,
// 设了也永不触发。终端层不校验数据来源——只认喂进来的条目。
using PromptHistoryProvider = std::function<PromptHistoryDataset()>;
void SetPromptHistoryProvider(PromptHistoryProvider provider);

// ---------------------------------------------------------------------------
// 0.30.x 第三批:草稿 stash、@ 文件提及菜单、外部编辑器
// ---------------------------------------------------------------------------

// 草稿 stash(一格):composer.stash 动作(keymap 可绑键)一键收起当前
// 草稿,再按取回。分账:存下时的收件目标(查看态那只子代理,0 = main)
// 与 cwd;取回时目标或 cwd 对不上就拒(给甲写的话不送给乙)。只存内存,
// 不落盘——未发送内容不该进任何存档,进程退出即弃(隐私上明确的取舍)。
struct ComposerStashSnapshot {
    bool has = false;
    std::string text;
    int target_task_id = 0;
    std::string cwd;
};
bool ComposerStashHasContent();
ComposerStashSnapshot ComposerStashPeek();
void ComposerStashDiscard();

// @ 文件提及菜单的数据源:应用层扫 cwd/Git 根(排除 .git/构建产物)给
// 相对路径清单。缓存归应用层(索引可能上千条,不许每键一扫);终端层每
// 键拿清单做模糊匹配(纯函数 FuzzyMatchMentions,便宜)。传空清除。
using FileMentionProvider = std::function<std::vector<FileMentionEntry>()>;
void SetFileMentionProvider(FileMentionProvider provider);

// 外部编辑器($VISUAL/$EDITOR)读回的草稿转换:CRLF 归一成 '\n'、剥编辑器
// 补的一个行尾换行。纯函数,测试钉着。
std::string NormalizeEditorDraft(std::string bytes);

// 0.17.0:常驻状态行(composer 输入框下横线之下那一行)要展示的会话数据。
// main.cpp 在每轮给主提示符之前更新一次(/model 切换、context 百分比刷新
// 全走这一条路,反正每轮循环都会路过);终端层每帧重画状态行时读它,配上
// 此刻的确认档(SharedEditor 里那份)拼整行文本(拼装规则是
// cli/format_utils 的纯函数,单测钉在那边)。管道/重定向模式画不出状态行,
// 设不设都无感。
void SetStatusLineData(const StatusPanelData& values, const std::vector<std::string>& items,
                       const std::string& separator);

// 状态行的局部更新:主请求 usage 一到(BuildCallbacks::on_usage 更新
// ContextTracker 之后)把 context 百分比/已用/窗口发布进来——只改
// context/tokens 两段的数字与旧值标记,model、cwd、git_branch、provider、
// effort、REC 等其余字段原样保住,不在回调里另造一份残缺 StatusPanelData
// (拷贝语义是 cli::WithContextMeasured 纯函数,单测钉在那边)。
// 发布与重画分开:这里只拿 StdoutWriteMutex 线程安全地改数据,一个字节
// 都不往终端写;流式 footer 的现有重画事务(正文 OnDelta、子代理状态条
// 的 400ms ticker、挂起恢复的第一帧)在安全时机取新值——footer 挂起在
// ask_user/确认菜单里时不抢屏,菜单退出后的下一帧自然带出新数。
// measured=false 把数字标成旧值(ContextTracker::usage_stale,渲染带 ~)。
// cache_note(缓存诊断单):tokens 段尾部的缓存注记("缓存命中 X(Y%)" /
// "缓存未报告"),空串 = 抹掉旧注记。管道/重定向模式没有状态行,设了也无感。
void UpdateStatusLineContext(int context_percent, std::int64_t used_tokens, std::int64_t window_tokens,
                             bool measured, const std::string& cache_note = std::string());

// 状态行数据源此刻的快照(拿 StdoutWriteMutex 拷贝):测试/诊断用,常规
// 渲染路径不走这个(每帧重画在 BuildStatusLine 里现读)。
StatusPanelData SnapshotStatusLineValues();

// ---------------------------------------------------------------------------
// 0.30.x 第四批:终端标题与"轮到你了"
// ---------------------------------------------------------------------------

// 终端标题(OSC 0):真控制台才写,管道/重定向不添转义。/title 管的是会话
// 存档名,这里管的是终端 tab 上的那一枚(项目 · 分支 · 状态),两本账分开。
void SetTerminalTitle(const std::string& text);

// "轮到你了":BEL 一声。拿不到终端焦点状态(那要窗口管理器配合,诚实
// 承认不知),所以不做"未聚焦才响"的判断——由调用方按"长任务跑完/等待
// 确认"节流,只在状态翻转时叫一声,不刷风暴。桌面通知(OSC 9;9/777)
// 支持面参差,能力探测层报 Unknown,这里不假装会发。
void NotifyUserAttention();

// M11(0.10.0):真控制台此刻的显示宽度(列数),给分界线(cli::BuildDividerLine)
// 探测用。查的是 stdout 那个句柄(跟 DetectConsoleCapability 一致)——分界线
// 关心的是"要打印到哪儿",不是 stdin。探测不到(非真控制台、GetConsoleScreenBufferInfo
// 失败……)返回 std::nullopt,调用方按 80 列兜底。非 Windows 平台恒返回
// std::nullopt。
std::optional<int> DetectConsoleWidth();

// M10:main.cpp 的流式回调(打字机输出 on_text_delta、on_tool_start……)在
// 主线程上打印;TurnInputListener 的打断提示与排队脚注在监听线程上打印——
// 两边都写 std::cout,不加锁会在真终端上偶发交错、把画面
// 弄花。main.cpp 里凡是"流式期间"(Run() 还没返回)可能触发的 std::cout
// 写,都拿这把锁包一下,跟监听线程互斥。管道模式下监听线程压根不会起,
// 锁永远拿得到,不影响非交互路径的性能/行为。
std::mutex& StdoutWriteMutex();

// markdown 收束重画 × M10 监听线程的对账口子:TurnInputListener 在流式
// 当口改动打断提示或排队脚注后(此刻它正持着 StdoutWriteMutex)调这个钩子。
// 正文重画的行数账按"块首行号 + 光标位移"记；监听线程动过屏面却不通气，
// 收束重画便可能错行。main.cpp 的 RunTurn 把
// 钩子接到 StreamBodyTracker 上:作废当前块锚点,这一块保持原样不重画
// (宁可漏渲染,不擦用户的回显)。
// 约定:钩子在持有 StdoutWriteMutex 的前提下被调,钩子实现不得再去锁它;
// 设置/清除(传空)也在锁内做,跟监听线程的调用天然错开。RunTurn 在
// listener.Stop()(线程已 join,不会再有人调)之后立刻清除,钩子绝不
// 活过它抓引用的对象。
void SetStreamScreenPrintHook(std::function<void()> hook);

// footer / 状态块贴到缓冲区底时会主动滚屏。滚了几行，须让正文块与工具
// 条目的绝对锚点一同上移。钩子在持有 StdoutWriteMutex 时调用，不得重锁。
void SetStreamScreenScrollHook(std::function<void(int)> hook);

// -----------------------------------------------------------------------
// "ask_user 被子代理状态遮挡"一单的 repaint 协调层:阻塞式交互菜单
// (StreamFooterSuspendScope 存活期)取得整块屏面所有权,期间——脚注框
// (含框里的代理面板)不重画、心跳线程零输出、控制台输入不归监听线程。
// 规约全部钉在这几个入口上,单测见 tests/test_repaint_coord.cpp:
// -----------------------------------------------------------------------

// 全局 repaint 挂起计数(footer 的 suspend_depth / paint_depth 合起来看)。
// 挂起 = 交互菜单占屏(suspend_depth>0)或屏幕事务进行中(paint_depth>0)。
// 调用方必须已持有 StdoutWriteMutex(两枚深度的写点全在这把锁内);
// turn_runner 的 footer 心跳线程拿到锁后第一件事就是查它。
bool RepaintSuspendedLocked();

// 挂起计数>0(即阻塞式交互菜单开着)。自带 StdoutWriteMutex,给
// TurnInputListener 的监听线程当"让出读权"的判断用——菜单等输入期间
// 键盘全归菜单一处。
bool RepaintSuspendActive();

// 测试/诊断用:suspend 计数现状(嵌套/重复恢复/异常早退析构对账的观测口)。
int StreamFooterSuspendDepthForTest();

// M10:"谁在真的逐键读键盘,谁先拿这把锁"。ReadLineKeyByKey()/ReadChoiceMenu
// 整个调用期间攥着它,TurnInputListener 的监听线程只在 try_lock 抢到的
// 间隙才读一次——阻塞式菜单持锁等输入时,监听线程绝不消费方向键/Enter/
// Esc/普通字符,这条规约靠它保证(单测钉死:tests/test_repaint_coord.cpp)。
std::mutex& ConsoleReadMutex();

// -----------------------------------------------------------------------
// 0.21.x 流式脚注(footer),0.22.x 升级成跟 composer 视觉一致的完整框:
// 流式期间正文下方常驻上横线 + `> ` 输入行 + 下横线 + 状态行,一共 4 行,
// 让用户看见"能按 ESC 打断、能键入并回车排队下一条"——回归前用户实测
// "流式期间屏上啥提示都没有,以为程序坏了"。空闲时输入行显示淡色占位提示
// (取代老版本单独一行 hint);用户真键入排队消息时,输入行实时回显已键入
// 内容;Enter 落队后输入行复位,上方常驻最近几条待发送消息。物理光标留
// 在输入行,正文续写坐标另存,下一笔正文来时先回正文、写完再回来。
// 上下横线/状态行分别复用 composer 输入框那节的 BoxRuleLine/PrintStatusLine,
// 不重写一份画法(见 console_input.cpp)。
//
// 归属与协调:footer 的状态(显示/回显文本)由 RunTurn / 监听线程设置,但
// 真正落笔由 StreamBodyTracker::OnDelta 每笔正文前后带一手——正文落笔前
// EraseStreamFooterLocked() 把整个框擦掉(免得跟正文抢行),正文落笔后
// RedrawStreamFooterLocked() 把框重画在正文光标的下一行起、光标再拨回
// 正文末尾。这样框永远紧贴"正文当前底部"的下一行,正文往下长它就跟着挪,
// 绝不跟正文重叠。贴到 ConPTY 缓冲区底时会主动滚屏，并经 scroll hook 把
// 正文块与工具条目锚点一同上移；输入框不再因“底下没四行空位”而消失。
// 全程在 StdoutWriteMutex 之内,每次重画都拿 synchronized output
// (DEC 2026,`\x1b[?2026h`/`l`)包一层,避免终端半途刷出半帧。
//
// enabled:只在 is_console && platform::SupportsScreenRepaint()(即 Windows
// 真控制台)下为真——footer 要随时查光标位定位,POSIX 走 DSR 6n 会跟监听
// 线程抢 stdin,跟 StreamBodyTracker 的重画一样诚实关掉(退回纯流式)。
void BeginStreamFooter(const Theme& theme, bool enabled);
void EndStreamFooter();
// 下面两个要求调用方已持有 StdoutWriteMutex(OnDelta/OnBlockBreak 正持着)。
void EraseStreamFooterLocked();
void RedrawStreamFooterLocked();
// 从当前光标起为 rows_needed 行腾出位置；必要时滚屏并通知上面的钩子。
// 调用方须已持有 StdoutWriteMutex。末尾带换行的 N 行文字应传 N + 1。
bool EnsureStreamScreenRowsLocked(int rows_needed);

// ---------------------------------------------------------------------------
// 帧账的"保锚可见"原语(多智能体真机回归单):全程序独此一处管
// "要画的行必须落在可视区里"。从 top_row 起 rows_needed 行若伸出可视窗
// 口底,先平移视口(经典 conhost 长缓冲:窗口之下还有缓冲行,内容与绝对
// 锚点一个不动),平移到头(视口贴缓冲区底——Windows Terminal/ConPTY 的
// 常态)再退回"缓冲区末行写换行滚内容"的老法。返回内容实际滚动的行数
// (平移视口时为 0):调用方拿它把绝对锚点上移对账。调用方须已持有
// StdoutWriteMutex。
// 病根:绝对定位画帧(SetCursorPos/VT CUP)画出窗口底,控制台不会像顺
// 序打印那样把窗口带下去——帧画在缓冲区里、用户看不见,回合收口后
// composer 与代理坞"画在下方却不在 viewport 内"说的就是它,非得按字号
// 借终端 reflow 才救得回来。
// ---------------------------------------------------------------------------
int EnsureViewportRowsLocked(int top_row, int rows_needed);

// 上者的正文/工具账封装:锚点(anchor_row)护栏——贴底滚内容的行数比锚点
// 上方还多时返回 -1(账对不上,调用方按老规矩弃画/记 unsafe),平移视口
// 与正常滚内容返回实际滚动行数(平移为 0)。调用方须已持有
// StdoutWriteMutex。
int EnsureViewportRowsForAnchorLocked(int anchor_row, int top_row, int rows_needed);

// 流式回合里的 Working 不另占一套 stdout 绘制权。Spinner 只把当前帧
// 塞进 footer 状态，由 RedrawStreamFooterLocked 把 Working、队列、输入框
// 一气画完。Start 返回 false 表示当前没有 footer（如 /compact），调用方
// 可退回原来的独立单行 spinner。
bool StartStreamFooterWorking(const std::string& label);
void UpdateStreamFooterWorking(const std::string& label, std::size_t highlighted_glyph,
                               long long elapsed_seconds);
void StopStreamFooterWorking();

// ---- turn 级 Working 活动条(终端回合视觉收束单) ------------------------
// 现有 Start/Update/Stop 那组归 SpinnerBackend 管:每次模型请求新起、首个
// stream event 一到便停——一轮里"模型 -> 工具 -> 模型"计时会消失重来,
// 报的不是整轮用时。这组认整个 turn:
//   BeginTurnActivity:用户 prompt 过了本地校验、turn.started 落账那一刻亮。
//     started_at 是 turn 起点(epoch 毫秒);此后正文 delta、工具批次、
//     下一次模型请求、重试都不熄、秒数不归零。
//   UpdateTurnActivityElapsed:计时一秒一跳(或动画线程每拍报同一秒),
//     highlighted_glyph 走字扫光,字符数与显示宽恒不变。
//   SetTurnActivityInterruptRequested:ESC 置了 cancel,文案换 "Stopping..."
//     (终态落账后由 EndTurnActivity 退场,不瞬间消失)。
//   EndTurnActivity:turn.completed 一到就熄;返回最终显示的整秒数,交
//     Worked footer 对账(两边同一只钟,不得差一截)。
// 没起过的 EndTurn 返回 -1(不误伤 /compact 那类单次 spinner)。
void BeginTurnActivity(const std::string& label, std::int64_t started_at_ms);
void UpdateTurnActivityElapsed(std::size_t highlighted_glyph, std::int64_t elapsed_seconds);
void SetTurnActivityInterruptRequested();
long long EndTurnActivity();
// 当前 turn 活动条是否亮着(装配层判断要不要抢 Spinner 的绘制权)。
bool TurnActivityActive();

// 0.22.5:工具确认交互(main.cpp 的 PrintConfirmDetails/ShowDiffPreview 到
// ReadLine([y/a/N] 提示)那一整段)期间,流式脚注框必须让路——真机实测
// 报过两条病:1) 确认详情文字直接盖写在框的横线上,不清行尾,横线残留;
// 2) [y/a/N] 提示整个看不见。根子不是"没打印",是打印完之后被后续某次
// RedrawStreamFooterLocked() 覆盖掉了——最典型的是 turn_runner 的 footer
// 400ms 一次 ticker(main.cpp),不管是不是在等确认,只要 enabled 就无条件
// 重画一次框;PrintConfirmDetails/ReadLine 落笔时又都不清场,两边一撞,
// 框把确认文字整个盖掉,或者反过来在确认文字尾巴上留下没清干净的横线。
//
// 光擦一次(EraseStreamFooterLocked)堵不住——擦完 ticker 下一拍照样画
// 回来。得再加一层"挂起":构造时(拿锁)先把框彻底擦干净、标记挂起,
// 挂起期间 RedrawStreamFooterLocked() 直接空操作,不管是谁调的(ticker、
// StreamBodyTracker::OnDelta、监听线程的键入回显……全部一并压住,不用
// 逐个调用点接管)。析构时(拿锁)摘掉一层挂起；最外层退场便立即补画，
// 不再假定确认答完之后必定还有正文或 ticker 来救场。
//
// 用 RAII 是为了把"挂起期覆盖两条确认路径(run_command 等走
// PrintConfirmDetails,edit_file/write_file 走 ShowDiffPreview)+ 确认提示
// 打印前的空窗期(详情打印完、ReadLine 还没抢到 ConsoleReadMutex 那一段,
// 监听线程理论上能插进来读一次键)"这几处一次性堵严实,不用在每个可能
// 触发重画的调用点分别加判断——作用域对象存活多久,挂起就持续多久,
// main.cpp 只需要在"确定要真问一句"的那一刻建一个局部变量。
// 构造/析构各自只在临界区里逗留一瞬(拿锁——干活——放锁),不会跨越整个
// 确认交互一直攥着 StdoutWriteMutex,PrintConfirmDetails/ShowDiffPreview/
// ReadLine 自己该拿锁还是拿得到,不会自锁。
//
// "ask_user 被子代理状态遮挡"一单把这块挂起从"只管脚注框"升格成全局
// repaint 挂起计数:作用域存活期间(嵌套时计数没退干净前),不止
// RedrawStreamFooterLocked() 空操作——
//   1. 构造最外层时把整个框(含框里的代理面板)彻底擦净,菜单的标题/
//      问题/选项从正文末尾一次铺到底,中间没有浮动块插队;
//   2. turn_runner 的 footer 心跳线程每拍先查 RepaintSuspendedLocked(),挂起
//      期间零输出——单次擦除堵不住的那个"下一拍又画回来"从根上没了;
//   3. TurnInputListener 查 RepaintSuspendActive(),挂起期间不碰控制台输入,
//      连"问题打印完、菜单还没抢到 ConsoleReadMutex"的空窗也不留——键盘
//      全归菜单一处。
// 析构最外层退一层挂起并补画脚注框(恢复点在菜单结果之后);状态块由
// ticker 下一拍自己找回来,落笔位置自然在菜单结果之下,盖不住任何东西。
class StreamFooterSuspendScope {
public:
    StreamFooterSuspendScope();
    ~StreamFooterSuspendScope();

    StreamFooterSuspendScope(const StreamFooterSuspendScope&) = delete;
    StreamFooterSuspendScope& operator=(const StreamFooterSuspendScope&) = delete;
};

// 工具条目、diff、状态块要改写正文末尾时用的屏幕事务。构造时先擦 footer，
// 事务内所有 Redraw 请求只记状态、不落笔；最外层析构时再把 footer 一次
// 画回。与 SuspendScope 不同，它不等用户输入，只围住一小段同步重画。
class StreamFooterPaintScope {
public:
    explicit StreamFooterPaintScope(bool enabled = true);
    ~StreamFooterPaintScope();

    StreamFooterPaintScope(const StreamFooterPaintScope&) = delete;
    StreamFooterPaintScope& operator=(const StreamFooterPaintScope&) = delete;

private:
    bool active_ = false;
};

// M10:ESC 打断当前轮 + 消息排队用的监听器。main.cpp 在"发出请求到本轮
// Run() 结束"这段窗口期起一个实例:ESC 键按下就把 cancel_flag 置位、打一行
// 淡色 "[已打断]";其余可打印字符进内部排队缓冲(Backspace 能退格),遇
// Enter 就把整行（非空才算）落进队列，并刷新 footer 上方的常驻队列区。
// Shift+Tab 循环切确认档——跟空闲路(ReadLineKeyByKey)共用同一枚
// SharedEditor 档位与同一个 NextConfirmMode 轮转,切完 footer 状态行按新档
// 重画;Tab 不引入补全交互,维持不理会。
//
// 0.25.x 排队输入自然化升级,0.28.x 移库:队列本身是会话层的
// SteeringQueue(见 queue_model.hpp),监听线程只提交编辑动作(Enter 落队
// 带目标、Shift+←/上键 取回、Enter 原位替换、Esc 还原、Del 两段删除、
// 上下键在条目间走),不拥有最终数据——投递由会话泵在安全点(工具边界/
// 打断收场)执行,Enter 是排队而不是另开一轮。取回的正文装进一只真正的
// LineEditorCore(composer 模式,光标/退格/粘贴/多行软换行与空闲 composer
// 同一套),不是只会尾删的临时 buffer。两层 Esc:编辑态第一下只取消编辑、
// 还原原文;退出编辑态后再按 Esc 才打断当前轮。
//
// Ctrl+C(补于排查"ESC/Ctrl+C 都停不掉子代理"那次)语义对齐 bash/Python/
// Node REPL、Claude Code 官方文档确认过的通用约定:单击效果等同
// ESC(打断当前轮,cancel_flag 置位,不退出程序);1.2 秒内连按两次才是
// "强制退出整个进程"(std::exit,不走 cancel_flag 那套可能被挂起工具/子
// 代理拖住的收场流程)。双击计时只在这一个实例的生命周期内有效,不跨轮。
//
// 跟 SharedEditor() 那条"真正在读一行"的路径靠一把互斥锁
// (ConsoleReadMutex,console_input.cpp 持有、头文件有公开声明,两边共用
// 同一份)自动错开——监听线程只在抢到锁的间隙才调 ReadConsoleInputW,
// ReadLineKeyByKey()/ReadChoiceMenu() 整个调用期间一直攥着锁,监听线程
// 那段时间只能干等,绝不会跟"编辑器/菜单正在读"的窗口期抢同一份控制台
// 输入(工具确认提示 [y/a/N] 与 ask_user 的选择菜单走的也是这条路,
// 天然享受同样的互斥,不用另外接管)。在这之上,监听线程抢到锁后还会查
// 一眼 RepaintSuspendActive():阻塞式菜单开屏(挂起计数>0)期间干脆连
// WaitForKeyEvent 都不碰——"问题打印完、菜单还没抢到读权"的空窗期也
// 不消费按键,Esc 只取消当前菜单,不会落进流式待发队列或误打断整轮。
//
// stdin 不是真控制台(管道/重定向)时,构造函数直接不起线程,Stop()
// 是安全的空操作——管道场景本来就读不到"按键",这整个类形同虚设,
// 是刻意的、跟 ReadLine() 的管道回退逻辑对齐的设计。
class TurnInputListener {
public:
    // transcript_expanded:UI-D(0.16.0)紧凑/详细全局开关的地址,跟
    // ToolDisplay::expanded_ 共享同一份存储(main.cpp InteractiveLoop 那个
    // 局部变量)——回合执行期间按 Ctrl+O 也能翻这个开关,让"紧凑/详细"
    // 不再局限于两轮之间的 composer 主循环才能切。留空(nullptr,AskOnce
    // 单发模式/管道场景)时监听线程读到 CtrlO 直接忽略,行为跟没这个参数
    // 一样。atomic<bool>:这里写(监听线程)、ToolDisplay/TranscriptPainter
    // 那边读(Run() 所在的主线程)分处两个线程——真机驱动器实测踩到过用
    // 普通 bool 时的可见性问题(翻转了,但另一线程这一拍还没读到新值,
    // 一次仅有的子工具调用刚好卡在这个窗口),换成 atomic<bool> 用
    // load/store 的 acquire/release 语义堵上,不是"反正是显示态、错一帧
    // 也无所谓"能糊弄过去的。
    // expand_renderer 在 Ctrl+O 翻档后调用。调用时 stdout 锁已经拿住、
    // 流式 footer 已擦掉；回调不得再拿 stdout 锁。它可趁此收掉别的浮动
    // 状态、作废旧屏幕锚点，并返回要紧跟模式提示打印的转录文本。
    using ExpandRenderer = std::function<std::string(bool expanded)>;
    TurnInputListener(std::atomic<bool>& cancel_flag, const Theme& theme,
                       std::atomic<bool>* transcript_expanded = nullptr,
                       ExpandRenderer expand_renderer = {});
    ~TurnInputListener();

    TurnInputListener(const TurnInputListener&) = delete;
    TurnInputListener& operator=(const TurnInputListener&) = delete;

    // 停止监听、join 线程。main.cpp 在本轮 Run() 返回之后立刻调一次,保证
    // 下一次 ReadLine()(下一轮主提示符)开始之前,监听线程已经彻底退出
    // ——不依赖两边抢互斥锁的运气,干净收尾。若用户此刻还停在队列编辑态
    // 半途,未提交的修改按 Esc 同款处置:原文还原、解冻,不把冻结条目
    // 留给投递泵。幂等,重复调用/析构时再调都安全。
    void Stop();

private:
    void ThreadMain();

    std::atomic<bool>& cancel_flag_;
    const Theme& theme_;
    std::atomic<bool>* transcript_expanded_ = nullptr;
    ExpandRenderer expand_renderer_;
    std::thread thread_;
    std::atomic<bool> stop_requested_{false};
    bool enabled_ = false;
    // 监听线程在 Stop()(join 完)之后由主线程读:还没了结的编辑事务在
    // 这里收尾(CancelEdit 还原原文,解冻)。
    std::optional<SteeringQueue::EditHandle> open_edit_;
};

}  // namespace lubancode::cli
