// UI-B(0.12.0):工具调用条目化渲染的数据模型 + 纯渲染函数。
//
// 一次工具调用在屏幕上是一个"条目":
//   ● run_command(git log --oneline -3)
//     ⎿ Done · 退出码 0 · 1.2s
// 首行是状态灯 + 工具名(关键参数摘要),次行起缩进两空格、⎿ 开头是结果
// 摘要,续行再缩两空格。工具启动先画"执行中"态,结束后由 cli 层原地改写成
// 终态(成功/失败/拒绝/打断)——改写本身是 Win32 控制台的活(main.cpp 的
// TranscriptPainter),这个文件只管两件事:
//   1. TranscriptItem:一个条目的全部数据(UI-C/D 的 Ctrl+E 全文查看、
//      会话回放都靠它,full_output 现在就存好,渲染只用摘要);
//   2. FormatTranscriptItem 及一串摘要生成小函数:纯函数,不碰 IO,单测
//      主战场。
//
// plain 主题(theme.reset 为空)下状态灯换成文字 [RUNNING]/[OK]/[ERROR]/
// [CANCELLED]/[INTERRUPTED](待确认态是 [CONFIRM]),不靠颜色也能辨。
// 彩色主题只染状态灯,正文参数不染色。

#pragma once

#include <chrono>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "api/types.hpp"
#include "cli/theme.hpp"

namespace lubancode::cli {

// 条目状态。Pending 是"needs_confirm 的工具画出来了、还在等用户点头"那个
// 档;Running 起是正经五态(执行中/成功/失败/拒绝/ESC 打断)。
enum class TranscriptStatus { Pending, Running, Ok, Error, Cancelled, Interrupted, Blocked };

// 主循环的工具是 Tool,子代理内层的工具是 SubTool(渲染时整体再缩四空格)。
// Thinking 是模型思考过程折叠块("思考 Xs",Ctrl+O 展开看正文)。
enum class TranscriptKind { Tool, SubTool, Thinking };

// full_output 的截断上限:64KB。UI-C/D 的 Ctrl+E 全文查看用,别让一次
// 超大输出把会话内存吃穿。
inline constexpr std::size_t kFullOutputCapBytes = 64 * 1024;

// ---- 思考流中预览(逐帧露尾与完毕自折叠单):视图状态机 + 数据状态 ----
//
// 每条 thinking item 的"画法"由这只状态机独占(数据状态另记,见下面
// ProviderContentKind 与 TranscriptItem 的字节账):默认路自动露尾预览,
// 完毕自折叠成一行;用户 Ctrl+O 伸手展开过的,尊重选择、不自动收折。
//
//   Hidden --首枚 delta--> AutoPreviewRunning --done--> CollapsedDone
//                                |--Ctrl+O--> ExplicitExpandedRunning --done-->
//                                ExplicitExpandedDone
//   ExplicitExpandedRunning --Ctrl+O--> CollapsedRunning --Ctrl+O--> 回展开
//   CollapsedDone --Ctrl+O--> ExplicitExpandedDone(整组重打路,全局开关管)
enum class ThinkingPhase {
    Hidden,                  // 一枚 delta 都没来过(item 还没建)
    AutoPreviewRunning,      // 默认:标题带秒表,正文露尾三条视觉行
    CollapsedRunning,        // 用户展开后又收起:只留标题,delta 照收存
    ExplicitExpandedRunning, // 用户展开:正文全文随流续画,不再收折
    CollapsedDone,           // 完毕自折叠:一行"思考 Xs(Ctrl+O 展开)"
    ExplicitExpandedDone,    // 完毕且用户展开过:保持展开,不擅自关门
};

// 状态机吃的事件。Toggle* 只由用户按键产生;FirstDelta/Done 由数据流产生。
enum class ThinkingSignal { FirstDelta, Done, ToggleExpand, ToggleCollapse };

// 纯转移函数(单测主战场):非法转移原样返回(状态机不许有第二条隐路)。
ThinkingPhase NextThinkingPhase(ThinkingPhase phase, ThinkingSignal signal);

// provider 真交回来的思考是哪一路(数据状态,不与画法混)。redacted/
// unavailable 没正文可露:报时长与"未提供摘要",不造内容。
enum class ProviderContentKind { Thinking, ReasoningSummary, Redacted, Unavailable };

// 自动预览露尾的最大视觉行数(单上"最多露三条视觉行";宽窄自适应在
// ThinkingPreviewRows 里折行)。
inline constexpr int kThinkingPreviewMaxRows = 3;

struct TranscriptItem {
    int id = 0;
    TranscriptKind kind = TranscriptKind::Tool;
    std::string tool_name;                    // 原始工具名(run_command / mcp__x__y ……)
    std::string title;                        // 首行正文:工具名(关键参数摘要),不含状态灯
    std::string input_json;                   // 完整入参紧凑 JSON(UI-D 展开版/聚焦查看用;空 = 没有入参)
    std::vector<std::string> summary_lines;   // ⎿ 之后那几行
    std::string full_output;                  // 完整结果,截 kFullOutputCapBytes
    TranscriptStatus status = TranscriptStatus::Running;
    std::chrono::steady_clock::time_point start_time{};
    std::chrono::steady_clock::time_point end_time{};
    // ---- 思考条目专用(其余 kind 不碰这些字段) ----
    // 画法状态(单上五态机;Hidden 是"还没建条目"的兜底,渲染视同折叠)。
    ThinkingPhase thinking_phase = ThinkingPhase::Hidden;
    // 数据状态:provider 交的是哪一路正文(只记事实,不掺画法)。
    ProviderContentKind provider_content_kind = ProviderContentKind::Thinking;
    // 收到的正文/签名字节数(signature 只记协议账,不进预览、不计可见字数;
    // 四家 wire 里只有 Anthropic 有,引擎尚未透传,先立字段记 0)。
    int thinking_text_bytes = 0;
    int thinking_signature_bytes = 0;
};

// 把一个条目渲染成完整的多行文本(每行以 \n 收尾)。width 是终端宽度,
// 首行超宽按显示宽度截断加 "..."(绝不物理折行——折行会毁掉原地改写的
// 行数记账);width <= 0 表示不截断。摘要行不截(可能夹着 todo 清单自带
// 的颜色序列,按显示宽截断会把 ANSI 剪碎)。
//
// UI-D(0.16.0)两个新开关:
//   expanded —— 详细版:摘要之后追加完整入参 JSON 一行("参数: {...}"),
//     再接 full_output 全文(含 UI-C 并进去的完整 diff),一行分隔标题
//     "── 完整输出(N 行)──"、正文每行缩进;full_output 为空补一行
//     "(无完整输出)"。width > 0 时这些行同样按显示宽截断(Ctrl+O 的整块
//     重打走 TranscriptPainter 之外的裸打印也不许物理折行,免得铺屏乱套);
//     width <= 0 不截(Ctrl+E 聚焦查看给 0,全文如实铺,终端自然折行/滚动)。
//     夹 ANSI 的行照旧不截。
//     思考条目(Thinking)的档位由 thinking_phase 与 expanded 合成:
//     展开(expanded 或 ExplicitExpandedRunning)→ 标题追加 "· N 字",
//     正文全文随流续画(用户既伸手打开,不再设行帽,完毕也不自动收折);
//     AutoPreviewRunning → 标题下露尾最多 kThinkingPreviewMaxRows 条视觉行
//     (弱色、不跑 Markdown);其余(Hidden/Collapsed*)→ 只留标题一行。
//     正文一个字没到时不铺占位行,不露空框。
//   focused —— 焦点条目:首行行首加 "► " 醒目标记(占两列,宽度记账让位)。
std::string FormatTranscriptItem(const TranscriptItem& item, const Theme& theme, int width,
                                  bool expanded = false, bool focused = false);

// Ctrl+O 整组重打用。详细档逐条铺出(含子代理内层工具)，紧凑档只留
// 顶层工具；focus_index 是原 items 下标，-1 表示不标焦点；expanded_index
// 是单独展开的那一条下标(-1 = 无),该条 expanded=true、其余照 expanded——
// Ctrl+O 用它只展开最近一条,不再全局全展开。
// 条目之间的空白由间距表唯一决定(主/Subagent 面板同构渲染单 P0):两枚
// 连续顶层 Tool 之间恰留一口气(GapBetween=1),父 Tool 与其 SubTool、同父
// SubTool 批次紧排(0),SubTool 批结束后下一枚顶层 Tool 重新留间隔。首枚
// 打印条目之前不垫(块外 gap 归调用方的块间距账,如 RenderSessionBlocks)。
std::string FormatTranscriptItems(const std::vector<TranscriptItem>& items, const Theme& theme,
                                  int width, bool expanded, int focus_index = -1,
                                  int expanded_index = -1);

// 思考自动预览的露尾排版(纯函数):把思考正文按 \n 拆逻辑行、每行按
// 显示宽折行,取末尾 max_rows 条视觉行。返回的行已剥 ANSI/控制字符
// (provider 传来的转义不能改色挪光标)、已按宽度截好,渲染层只管加缩进
// 与弱色。width <= 0 按 80 兜底;超长无空格串照折,不许撑破终端。尾部
// 逐行从后往前取,单条巨长逻辑行只折够用的尾段,O(宽×行) 不 O(全文)。
std::vector<std::string> ThinkingPreviewRows(const std::string& text, int width, int max_rows);

// 思考正文里有没有可见内容(剥掉空白后还有东西吗)。空 thinking、纯
// 空白、全控制字符的正文都算"没交内容"——不露空框、不算可见字数。
bool ThinkingHasVisibleText(const std::string& text);

// plain 主题下的状态文字([RUNNING]/[OK]/…),渲染和单测共用一份映射。
std::string TranscriptStatusWord(TranscriptStatus status);

// ---- 用户输入背景块(终端用户输入背景块单) --------------------------------
//
// 已提交的用户输入是一块有身份的 surface:整行铺淡底色(不只染字),多行
// 逐行补齐,左右各留一格 padding,块后留一空行。live 提交(CollapseBoxOnSubmit
// 之后)、resume 重放(FormatRestoredHistory)、Ctrl+L 重画(RenderTurnView)
// 三路共用这一个纯函数——一处定样子,三处一个脸。

// 一块用户输入的物理行:每行自带背景开/关(背景 ANSI 每行开、每行关,不靠
// 软换行把颜色带去下一行)。text 已含 padding 与提示符,拼进屏幕时逐行落。
struct UserPromptRow {
    std::string text;  // 整行内容(bg + padding + marker + 正文 + padding + reset)
    int display_width = 0;  // 这一行占的显示列(含 padding,不含 ANSI)
};

struct UserPromptLayout {
    std::vector<UserPromptRow> rows;
    int block_width = 0;   // 色面铺到的安全宽度(列;含右 padding)
    int content_width = 0; // 正文可用宽度(安全宽减左右 padding 与提示符)
};

// 折行宽度:首行容下提示符("> ")后的窄区,续行统一缩进 kUserPromptIndent。
// 与 composer(LayoutComposerRows)同一套 grapheme/cell 宽度算法
// (CharDisplayWidth/DisplayWidth),提交前后不忽然换行。
inline constexpr int kUserPromptMarkerWidth = 2;  // "> "
inline constexpr int kUserPromptIndent = 2;       // 续行缩进(与 composer 续行同款)
inline constexpr int kUserPromptPadding = 1;      // 左右各留一格;窄屏(<20 列)可降为 0

// 把已提交的用户文本排成背景块。text 先按 \n 拆逻辑行,再按显示宽折行;
// width <= 0 按 80 兜底。空白文本返回空 rows(空 prompt 不生成空色块)。
// 用户文本里的 ANSI/ESC 按字节原样进正文——调用方(会话主路)拿到的都是
// 本程序自己拼的 UTF-8,不带控制序列;外部粘贴进来的内容由 composer 的
// 编辑路径先行过滤,这里不再做第二遍转义。
UserPromptLayout LayoutUserPromptBlock(const std::string& text, const Theme& theme, int width);

// 一块的整段渲染(每行以 \n 收尾,块后不再多垫空行——gap 归调用方的
// 间距表管)。live/resume/重画共用。
std::string FormatUserPromptBlock(const std::string& text, const Theme& theme, int width);

// ---- 间距表(单子"间距不是换行,是布局数据") ------------------------------
//
// 空白物理行数由前后块的相邻关系定,首版硬编码在这张表里,最终收口成
// GapBetween 纯函数。block 角色只取间距表用得着的几枚(完整 RenderBlock
// 模型留给 TurnView 合流的后续单,这里先给"谁挨着谁"的账)。

enum class BlockRole { UserPrompt, Thinking, Tool, SubTool, AssistantText, Warning, Error, TurnFooter, SystemNotice };

// 前块 -> 后块的默认间距(空白物理行数)。表外的组合一律 1(异常不黏正文)。
// SubTool 贴父项/同父批次紧排是 0,其余块与块之间留一口气。
int GapBetween(BlockRole before, BlockRole after);

// ---- 会话块(主/Subagent 面板同构渲染单) ----------------------------------
//
// 查看态会话正文的一块:工具/思考卡组(整组交 FormatTranscriptItems,吃
// 同一份紧凑/详细开关与 SubTool 折叠规矩)、markdown 文本(正文/用户消息/
// 介入)、通知行(压缩检查点/终局/失败/legacy 诊断)。块与块之间的空白由
// GapBetween 唯一决定——presenter 不再散落空行特判,Main 与 Subagent 的
// 查看页共用这一个"会话块列表 -> 行组"入口(面板只换数据源,不换 renderer)。

struct SessionBlock {
    enum class Kind { Items, Markdown, Notice };
    Kind kind = Kind::Notice;
    std::vector<TranscriptItem> items;  // Kind::Items:整组渲染(顺序即事件顺序)
    std::string header;                  // Kind::Markdown:头行原文(调用方拼好 ANSI)
    std::string body;                    // Kind::Markdown:markdown 正文
    std::string line;                    // Kind::Notice:一行通知(调用方拼好 ANSI)
    BlockRole role = BlockRole::SystemNotice;
};

// 块列表 -> 行组(每行原样,不含行尾换行)。expanded 只作用于 Items 块
//(FormatTranscriptItems 的紧凑/详细档,与 Main 面板 Ctrl+O 同一颗开关);
// 空块跳过,不产空行。
std::vector<std::string> RenderSessionBlocks(const std::vector<SessionBlock>& blocks, const Theme& theme,
                                             int width, bool expanded);

// ---- 首行参数摘要 ------------------------------------------------------

// 按工具名挑关键参数拼首行:run_command 显示命令;read/write/edit 显示
// 路径;agent 显示任务前 40 个码点;web_search 显示查询词;todo_write
// 显示几项;MCP(mcp__ 前缀)和其余工具显示入参紧凑 JSON。只拼 "name(摘要)",宽度截断交给
// FormatTranscriptItem。
std::string BuildToolTitle(const std::string& name, const nlohmann::json& input);

// /resume 的历史重放。用户/助手正文按当前主题渲染，tool_use 与随后
// tool_result 配成终态工具条目；只含工具结果的 user 消息不另画一轮用户。
std::string FormatRestoredHistory(const std::vector<api::Message>& messages, const Theme& theme,
                                  int width, const std::vector<std::size_t>& compact_positions = {});

// ---- 结果摘要小函数(每个都可单测) ------------------------------------

// 数一段文本有几行:空串 0 行;末尾没有 \n 的最后一截也算一行。
int CountLines(const std::string& text);

// run_command 的结果开头是 "[退出码 N]\n",解析出 N;对不上格式给 nullopt。
std::optional<int> ParseRunCommandExitCode(const std::string& content);

// "Done · 退出码 0 · 1.2s"。耗时由 cli 层在 on_tool_start/on_tool_done
// 之间掐出来传进(秒),退出码从结果文本解析,解析不出就省掉那一节。
std::string RunCommandDoneSummary(const std::string& content, double seconds);

// "3.2s"。思考折叠块标题用,跟工具摘要的耗时格式一致。
std::string FormatSeconds(double seconds);

// "读取 N 行"。
std::string ReadFileDoneSummary(const std::string& content);

// "新增 N 行,删除 M 行"。removed_lines 为空(写的是新文件)时只有
// "新增 N 行"。
std::string WriteDiffSummary(int added_lines, std::optional<int> removed_lines);

// "命中 N 处"。"没搜到匹配的内容"/"没找到匹配的文件" 算 0;截断提示行
// (……开头)不计入。
std::string SearchDoneSummary(const std::string& content);

// "子代理 N 轮 · M 次工具"。
std::string AgentDoneSummary(int step_count, int tool_call_count);

// 失败态摘要:首行固定 "Error: <首行>",接错误输出的后续行,总共最多取
// 前 5 行;更长的补一行截断标注 "(共 N 行,Ctrl+E 查看完整)"。
// run_command 的失败结果([退出码 N] 开头)首行改写成 "Error: 退出码 N"。
std::vector<std::string> ErrorSummaryLines(const std::string& tool_name, const std::string& content);

// UTF-8 安全截断到 max_bytes 字节以内(不劈开多字节字符),full_output
// 入库前过一遍。
std::string TruncateUtf8Bytes(const std::string& text, std::size_t max_bytes);

// UTF-8 按码点截断,超长加 "..."(agent 任务摘要"前 40 字"用)。
std::string TruncateUtf8Codepoints(const std::string& text, std::size_t max_codepoints);

// 数一段 UTF-8 文本有几个码点(思考条目展开档标题的「· N 字」用)。
int CountUtf8Codepoints(const std::string& text);

// ---- 条目工厂(终端接线收尾单:手工拼 TranscriptItem 的口子全仓清零) ----
//
// 病灶四(用户查账原文):公共投影(TurnItemView→TranscriptItem 的
// ProjectTurnItem)之外,各事件源(通知/静默归档/查看态事件账)仍在
// 手工拼条目、各写一遍状态映射——改状态映射时多处跟改。工厂收口:
// 状态与字段怎么落,这里一处定;调用方只给事实数据。id 由调用方的
// transcript 账本现发(工厂不碰账本)。

// 通知类条目(后台子代理完成/权限拒绝一类):标题 + 状态 + 摘要行。
TranscriptItem MakeNoticeItem(int id, const std::string& title, TranscriptStatus status,
                              std::vector<std::string> summary_lines);

// 后台通知的双标题归类(后台代理管控三连 bug 单,Bug A):权限拒绝与监督
// 提醒各有各的标题,不许张冠李戴——监督提醒(疑似断流/空转/恢复/工具
// 静默)若顶着"权限未放行"的标题,用户会把工具全放行的健康代理读成
// 全线被拒(真机实录:五条监督 toast 全被读成权限拒绝连刷)。
std::string BackgroundNoticeTitle(bool permission_denial);

// 静默档正文归档条目(查看态回流轮):正文全文入库,头两行各 120 码点
// 折成紧凑档摘要(渲染层还会按终端宽再截)。
TranscriptItem MakeAssistantArchiveItem(int id, std::string body, TranscriptStatus status);

// 查看态工具卡(子代理事件账的 start/done 配对折一条条目):done=false 是
// 还在跑的 Running 卡;input_json 解析不出对象时标题退 "名字(...)"的老
// 兜底(BuildToolTitle 自带)。kind 由调用方按"当前查看根"定(同构渲染单
// P0):查看根自己调用的工具是 Tool,它派出的下一层代理的内层工具才是
// SubTool——工厂不再无条件造 SubTool。摘要行(Main 重放同款):结果首行
// 当 ⎿ 摘要,多行补 "+N 行";Running 卡没有摘要。
TranscriptItem MakeAgentTaskToolItem(int id, const std::string& tool_name, const std::string& input_json,
                                     bool done, bool is_error, const std::string& result,
                                     TranscriptKind kind);

// 查看态思考卡:streaming=true 折 Running 卡(标题「思考中 · N 字」随
// 重铺拍跳),false 折收定 Ok 卡。
TranscriptItem MakeAgentTaskThinkingItem(int id, const std::string& text, bool streaming);

}  // namespace lubancode::cli
