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
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "cli/choice_menu.hpp"
#include "cli/format_utils.hpp"  // StatusPanelData(状态行数据源)
#include "cli/line_editor.hpp"
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
std::optional<std::string> ReadLine(const std::string& prompt, const Theme& theme = Theme{},
                                     bool esc_rejects = false, bool composer = false);

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
// 返回普通选中项与行内文本；Esc/Ctrl+C/EOF 返回 nullopt。
std::optional<ChoiceMenuResult> ReadChoiceMenu(const std::vector<ChoiceMenuItem>& items,
                                                const ChoiceMenuOptions& options, const Theme& theme);

// 会话级确认模式的查询/设置。真控制台下 Shift+Tab 会改这个状态(存在
// ReadLine() 内部维护的、贯穿整条交互会话的 LineEditorCore 实例里,见
// console_input.cpp 的 SharedEditor());main.cpp 的工具确认回调、主提示符
// 前缀都读这个。--yes 等价于启动时调一次
// SetConfirmMode(ConfirmMode::Yolo)。管道/重定向模式下这两个函数依然可用
// (状态本身不依赖真实控制台),只是永远不会被 Shift+Tab 改变——管道场景
// 根本读不到"按键",只有整行文本。
ConfirmMode CurrentConfirmMode();
void SetConfirmMode(ConfirmMode mode);

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
};
using TranscriptUiHandler = std::function<bool(UiKeyAction)>;

// 注册/清除会话级的 UI 按键回调(InteractiveLoop 进循环前注册,退出前必须
// 清掉——回调抓着循环里的局部引用)。传空 handler 即清除。管道/重定向模式
// 走不到逐键路径,注册了也永远不会被调,天然无感。
void SetTranscriptUiHandler(TranscriptUiHandler handler);

// 会话内后台子代理面板。数据由应用层给，终端层只管选择与绘制：空
// composer 按 ↑/↓ 进入代理焦点，Enter 展开详情，Esc 收起/退出。主会话
// 固定算第 0 项，provider 只返回后台子代理项。
struct AgentPanelEntry {
    std::string name;
    std::string description;
    std::string state;
    std::vector<std::string> detail_lines;
    bool running = false;
    bool failed = false;
};
using AgentPanelProvider = std::function<std::vector<AgentPanelEntry>()>;
void SetAgentPanelProvider(AgentPanelProvider provider);

// 空闲唤醒钩子:composer 主提示符在逐键等待期间,每 100ms 的面板刷新一拍
// 顺带问一次;返回 true 表示应用层有系统侧事件要在会话空闲时处理(比如
// 后台子代理跑完、结果等着交回主代理),ReadLine 以空串返回让调用方的循环
// 顶去办自己的事。只在 composer 为空时问——用户敲了一半的正文不抢,等
// 提交后再说。传空钩子即清除;管道/重定向走不到逐键路径,设了也永不触发。
void SetIdleWakeHook(std::function<bool()> hook);

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
// 管道/重定向模式没有状态行,设了也无感。
void UpdateStatusLineContext(int context_percent, std::int64_t used_tokens, std::int64_t window_tokens,
                             bool measured);

// 状态行数据源此刻的快照(拿 StdoutWriteMutex 拷贝):测试/诊断用,常规
// 渲染路径不走这个(每帧重画在 BuildStatusLine 里现读)。
StatusPanelData SnapshotStatusLineValues();

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
// (StreamFooterSuspendScope 存活期)取得整块屏面所有权,期间——脚注框不
// 重画、子代理浮动状态块零输出、控制台输入不归监听线程。规约全部钉在
// 这几个入口上,单测见 tests/test_repaint_coord.cpp:
// -----------------------------------------------------------------------

// 全局 repaint 挂起计数(footer 的 suspend_depth / paint_depth 合起来看)。
// 挂起 = 交互菜单占屏(suspend_depth>0)或屏幕事务进行中(paint_depth>0)。
// 调用方必须已持有 StdoutWriteMutex(两枚深度的写点全在这把锁内);
// AgentStatusPainter::Tick 拿到锁后第一件事就是查它。
bool RepaintSuspendedLocked();

// 挂起计数>0(即阻塞式交互菜单开着)。自带 StdoutWriteMutex,给
// TurnInputListener 的监听线程当"让出读权"的判断用——菜单等输入期间
// 键盘全归菜单一处。
bool RepaintSuspendActive();

// 测试/诊断用:suspend 计数现状(嵌套/重复恢复/异常早退析构对账的观测口)。
int StreamFooterSuspendDepthForTest();

// 交互菜单开屏(最外层 StreamFooterSuspendScope 进入)时收走浮动状态块的
// 钩子。AgentStatusPainter 构造时自登记、Stop 时撤销;调用时已持有
// StdoutWriteMutex,实现不得再拿这把锁,也不得读控制台输入。传空即撤销。
void SetRepaintSuspendHideHook(std::function<void()> hook);

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

// 流式回合里的 Working 不另占一套 stdout 绘制权。Spinner 只把当前帧
// 塞进 footer 状态，由 RedrawStreamFooterLocked 把 Working、队列、输入框
// 一气画完。Start 返回 false 表示当前没有 footer（如 /compact），调用方
// 可退回原来的独立单行 spinner。
bool StartStreamFooterWorking(const std::string& label);
void UpdateStreamFooterWorking(const std::string& label, std::size_t highlighted_glyph,
                               long long elapsed_seconds);
void StopStreamFooterWorking();

// 0.22.5:工具确认交互(main.cpp 的 PrintConfirmDetails/ShowDiffPreview 到
// ReadLine([y/a/N] 提示)那一整段)期间,流式脚注框必须让路——真机实测
// 报过两条病:1) 确认详情文字直接盖写在框的横线上,不清行尾,横线残留;
// 2) [y/a/N] 提示整个看不见。根子不是"没打印",是打印完之后被后续某次
// RedrawStreamFooterLocked() 覆盖掉了——最典型的是 AgentStatusPainter 的
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
//   1. 构造最外层时经 SetRepaintSuspendHideHook 登记的钩子(AgentStatusPainter
//      构造时自登记)把子代理状态块整块收走,菜单的标题/问题/选项从正文
//      末尾一次铺到底,中间没有浮动块插队;
//   2. AgentStatusPainter 的 ticker 每拍先查 RepaintSuspendedLocked(),挂起
//      期间零输出——单次 Hide() 堵不住的那个"下一拍又画回来"从根上没了;
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
// 0.25.x 排队输入自然化:队列本身是 PendingQueueCore(见 queue_model.hpp),
// 输入行只画 `> ` 和正在键入的字;Enter 落队后正文挪进上方待发区(逐条摆,
// 超上限只添一行"另有 N 条");空输入按上键取回最后一条待发消息改写,
// 上下键在待发消息间走,Delete 删当前项,Esc 放回队列——编辑态的 Esc 不
// 再打断当前轮。
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
// stdin 不是真控制台(管道/重定向)时,构造函数直接不起线程,Stop()/
// TakeQueuedLines() 都是安全的空操作——管道场景本来就读不到"按键",这整个
// 类形同虚设,是刻意的、跟 ReadLine() 的管道回退逻辑对齐的设计。
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
    // 下一次 ReadLine()(排队消息回显那个 "> " 提示,或者下一轮主提示符)
    // 开始之前,监听线程已经彻底退出——不依赖两边抢互斥锁的运气,干净收尾。
    // 幂等,重复调用/析构时再调都安全。
    void Stop();

    // 取走这次监听期间排队攒下的整行输入,按落队的原始顺序。Stop() 之后
    // 调,取完队列内部清空,不会重复吐给下一轮。
    std::vector<std::string> TakeQueuedLines();

private:
    void ThreadMain();

    std::atomic<bool>& cancel_flag_;
    const Theme& theme_;
    std::atomic<bool>* transcript_expanded_ = nullptr;
    ExpandRenderer expand_renderer_;
    std::thread thread_;
    std::atomic<bool> stop_requested_{false};
    bool enabled_ = false;
    std::mutex queue_mutex_;
    PendingQueueCore queue_;  // 待发消息队列(键入/落队/取回/编辑/删除),见 queue_model.hpp
};

}  // namespace lubancode::cli
