// 0.17.0:数字/状态行文本的纯格式化函数。不碰任何 IO、不认 Win32,单测
// 直接钉断言。终端层(console_input.cpp 的状态行)、main.cpp(统计行、
// /context)共用,不各写一份。

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "cli/line_editor.hpp"  // ConfirmMode
#include "cli/context_tracker.hpp"  // CacheRequestRecord/CacheMissKind(问题 9 诊断账的显示层)
#include "cli/theme.hpp"

namespace lubancode::cli {

// token 数字 k 化:
//   n < 10000      —— 原样十进制("9999")
//   n >= 10000     —— 一位小数 k("12.3k"),尾随 .0 省略("12k")
//   n >= 1000000   —— 两位小数 M("1.05M"),尾随 0 逐位省略("1.5M"/"1M")
// 四舍五入;负数不该出现,出现了原样十进制打出来,不猜。
std::string FormatTokenCount(std::int64_t n);

// 常驻状态行的"确认档"段:"⏵⏵ 确认模式 (shift+tab 切换)" / "⏵⏵ auto ..." /
// "⏵⏵ yolo ..."。纯文本不夹 ANSI,配什么色是终端层的事(auto 淡色、yolo
// 红、confirm 默认色)。0.21.x 起这是档位的唯一去处(提示符不再带前缀)。
std::string StatusLineModeSegment(ConfirmMode mode);

// 常驻状态行的"信息"段:" · <模型名> · context <p>%",used_tokens > 0 时
// 再接 " (<用量>/<窗口>)",数字走 FormatTokenCount。模型名为空就跳过那一节
// (状态行别摆一个空档)。
std::string StatusLineInfoSegment(const std::string& model, int context_percent,
                                   std::int64_t used_tokens, std::int64_t window_tokens);

// 整行状态行文本 = 模式段 + 信息段。单测钉这一个就把拼装规则全钉住了。
std::string BuildStatusLineText(ConfirmMode mode, const std::string& model, int context_percent,
                                 std::int64_t used_tokens, std::int64_t window_tokens);

// 可定制 status panel 的动态值。items 决定哪些字段出现、按什么顺序出现；
// 空值字段自动跳过，不留下两个连着的分隔符。
struct StatusPanelData {
    std::string model;
    std::string cwd;
    std::string git_branch;
    std::string provider;
    std::string effort;
    // worktree 房名(0.27.x):非空时会话住在隔离 worktree 里,状态行恒亮
    // "WT <名字>" 一段(纯文本,不进 items 配置,跟 REC 同理——在不在房里
    // 是安全相关的状态,用户没配也得看得见)。空串 = 没住房,零影响。
    std::string worktree;
    // 录制标记(0.25.x"录一遍生成技能"):"REC · 名字"/"REC 已停",非空
    // 时恒显示为第一段——密钥这道门要求录制中始终看得见 REC,不受 items
    // 配置影响(用户没配 "rec" 也得挂出来)。空串 = 没在录,零影响。
    std::string rec;
    int context_percent = 0;
    std::int64_t used_tokens = 0;
    std::int64_t window_tokens = 0;
    // 最近一次"请求结束"没带回实测 usage(provider 没给):context/tokens
    // 两段显示的是上一次的实测值,渲染时前缀 ~ 标旧,别让人当成本次新数。
    // 数据来源是 ContextTracker::usage_stale(),空闲重建与回合内局部更新
    // 两条路都带同一份。
    bool context_stale = false;
    // 缓存注记(缓存诊断单):服务端回了 cached_tokens 就写"缓存命中 X(Y%)",
    // 没回写"缓存未报告";挂在 tokens 段尾部,不单开一个 items 字段。空串 =
    // 一次实测都还没有,不显示。数据来源 ContextTracker(本场累计口径)。
    std::string cache_note;
    // 工具调用后端档(PTC 单):非空时状态行恒亮 "tools <档>" 一段——跟
    // REC/WT 同待遇,不进 items 配置(后端选择是能力与安全状态,用户没配
    // 也该看得见;规格 UI 节)。取值 "ptc" / "auto→json" / "auto→ptc" /
    // "ptc→json(原因)"。tool_calling=json(默认)时留空,状态行零变化。
    std::string tools;
    // Plan 模式标记(只读研究硬闸单):非空时恒亮一段 "plan"(与
    // permission_mode 并列,如 "plan · ⏵⏵ 确认模式"),不进 items 配置——
    // 只读硬闸开没开是安全状态,用户没配也得看得见。空串 = Default,零影响。
    std::string plan_mode;
    // goal/loop 会话状态段(goal 单合流 + loop 单的终端面):非空时恒亮
    // 一段,形如 "goal r2·iter3 · loop×2 next 4m"。goal 部分给
    // "goal <state 短码>·iter<N>"(状态短码:run/eval/pause/blocked/done/
    // budget);loop 部分给 "loop×<活任务数> next <最近一拍还差>"(没有
    // next 的场合省略)。两样都空 = 整段不挂,零影响。不进 items 配置:
    // 有没有常驻自动工作在跑是"背景会自己动"的状态,用户没配也得看得见
    // (与 REC/WT/tools/plan 同待遇)。文字由应用层拼好递进来,这里只管
    // 摆——渲染层不做 goal/loop 的状态机翻译。
    std::string goal_loop;
    // 后台命令任务段(background 管理面单):非空时恒亮一段,形如
    // "后台 2 运行 / 1 完成"。数据出自 BackgroundTaskRegistry(应用层
    // provider 现折),一只任务都没有时为空串——整段收起,零影响。与
    // goal_loop 同待遇:不进 items 配置(后台有没有东西在跑,用户没配
    // 也得看得见),文字由应用层拼好递进来。
    std::string background;
};

// 状态行数据的局部更新:只改 context/tokens 两段的数字、旧值标记与缓存
// 注记,其余字段(model/cwd/git_branch/provider/effort/rec)原样保住——
// 回合内主请求 usage 到达时发布新快照用,不另造一份残缺 StatusPanelData。
// 纯函数,单测钉"其他段保住"这一条。measured=false 把数字标成旧值(见
// StatusPanelData::context_stale)。cache_note 空串照写(一次实测都还没
// 有时抹掉旧注记)。
StatusPanelData WithContextUpdate(StatusPanelData data, int context_percent, std::int64_t used_tokens,
                                  std::int64_t window_tokens, bool measured,
                                  const std::string& cache_note = std::string());

struct StatusPanelSegment {
    std::string key;
    std::string text;
};

std::vector<StatusPanelSegment> BuildStatusPanelSegments(
    const std::vector<std::string>& items, ConfirmMode mode, const StatusPanelData& data);
std::string BuildStatusPanelText(const std::vector<std::string>& items,
                                 std::string_view separator, ConfirmMode mode,
                                 const StatusPanelData& data);

// 状态栏挤不下完整路径时保住盘符/根标记与末级目录，例如
// D:\very\long\project -> D:\…\project。max_width 按终端显示列算。
std::string CompactStatusPath(std::string_view path, int max_width);

// 短状态行按终端显示宽折行(P3-3 括号断行单):宽度账把 ANSI 转义(零
// 宽)与中文宽字一起算;断点只认空格,右括号一类的收口符紧跟着前一个
// 词,不许折在它前头——"node(v24.0.0)"要么整段留在本行,要么整段挪到
// 下一行,再不会出现右括号孤零零掉行。单个词比整行还宽时不切(不切半
// 个宽字),原样占一行交给终端裁。width<=0 原样一行。
std::vector<std::string> WrapStatusRows(const std::string& utf8, int width);

// 0.21.x 流式脚注文本(纯函数,i18n 驱动,不夹 ANSI/不认 IO)。
// StreamHintText:输入行空闲(没在键入)时的淡色占位提示——只提"键入并
// 回车排队",不再捎带打断说明(0.25.x 起"Esc 打断"挪进状态行)。
std::string StreamHintText(bool plain);

// tokens 段尾部的缓存注记(缓存诊断单,2026-08):cached_tokens 有则摆
// "缓存命中 X(Y%)"(本场累计口径,见 ContextTracker);最近一次没回 usage
// 写"缓存未报告";服务端禁用结论在 tracker 里时写"服务端未启用缓存";
// 其余 0 命中如实写"缓存 0 命中"。一次实测都没有(累计输入 0)返回空串。
// last_usage_reported = 最近一次请求是否真回了 usage(调用方手头的
// report.reported();空闲重建路径传 !tracker.usage_stale())。
std::string BuildCacheNote(const class ContextTracker& tracker, bool last_usage_reported);

// /context 缓存卡片的"逐请求缓存命中"块(真实实测问题单问题 5 + 问题 9):
// 把 tracker 的请求级环形缓冲按外层用户轮次分组,一行组头(用户轮次 #N
// (turn-id):标签)+ 每次模型请求一行(请求 n 输入 X / 命中 Y(Z%))。
// 头两行把口径说死:仅主会话、会话内(跨用户轮次)最近 N 次模型请求;
// 达到环形上限时明写"仅保留最近 12 次,全会话共 M 次",不把 12 冒充
// 总数;缺测请求(unreported)写"未回报",不冒充 0%。
// 问题 9 起,带诊断的请求行再追加本地前缀视角段:epoch(断则点名断因)、
// 追加律(前缀追加/前缀断(因))、稳定前缀条数(稳定 k/总 n 条)、
// miss 分型(上游未命中/首请求)、诊断模式下的 wire 公共前缀字节;
// 没带诊断的行写"诊断未随行",不猜。窗口内另出一行分型小计。一次请求
// 都没有返回空表。行内不带换行符,缩进已含(组头两格、组内四格、请求行
// 六格),打印由调用方管。
std::vector<std::string> BuildCacheRequestHistoryLines(const ContextTracker& tracker);

// 一笔请求的 miss 分型人话(问题 9):Hit -> "命中";FirstRequest ->
// "首请求";EpochBreak -> "断 epoch";UpstreamMiss -> "上游未命中";
// Unreported -> "未回报";Unknown -> 空串(诊断未随行,不猜)。单测钉文案。
std::string CacheMissKindLabel(ContextTracker::CacheMissKind kind);

// 一笔请求的诊断段(问题 9):" · epoch N(断:reason) · 前缀追加|前缀断 ·
// 稳定前缀 k/n 条 · <分型> · wire 前缀 NB"。诊断没随行返回"诊断未随行"。
// 只含 hash 截断、长度与分类词,不含正文。单测钉拼装。
std::string BuildCacheDiagSegment(const ContextTracker::CacheRequestRecord& record);

// /context 精细分类明细(全量对齐截图式占用面板):把"系统提示"/"工具
// 定义"两个粗桶按来源再拆一层,外加自动压缩缓冲/空闲空间两条独立的条形
// 行。nullptr(默认)= 不展示明细,FormatContextBreakdown 行为与从前逐
// 字节一致(9 行旧断言不动);给了就在对应粗桶后追加缩进子行。
//
// 口径:*_tokens 字段只管"分",不管"核对"——细分之和理应等于对应粗桶
// (system_prompt_tokens+skills_tokens ≈ sys_tokens;三类工具 mounted 之和
// ≈ tools_tokens),调用方(RunContextCommand)负责让账对上,这里只管画,
// 不做减法兜底(不像 history 那样拿总量反推)。deferred 三项是 tool_search
// 索引段的分类估算,不进 tools_tokens 那份"总量恒等式"(索引段本就是
// mounted 之外的另一份开销,道理跟原有 tools_tokens 里"索引段直接加总"
// 一致)。
struct ContextBreakdownDetail {
    // 系统提示细分:正文与技能目录清单分开——技能一多,目录本身就是一笔
    // 不可忽视的开销,揉在"系统提示"一个数字里看不出来。
    std::size_t system_prompt_tokens = 0;
    std::size_t skills_tokens = 0;

    // 工具定义细分,按来源三分(内置/MCP/插件),mounted(进 tools 数组)
    // 与 deferred(只在索引段露名字)分开算——deferred 三项全零时那一组
    // 子行整体不画(一个都没有,不摆空行,同 BuildDeferredToolsIndexSegment
    // 的"一个都没有就不注段"待遇)。
    std::size_t system_tools_tokens = 0;
    std::size_t system_tools_deferred_tokens = 0;
    std::size_t mcp_tools_tokens = 0;
    std::size_t mcp_tools_deferred_tokens = 0;
    std::size_t plugin_tools_tokens = 0;
    std::size_t plugin_tools_deferred_tokens = 0;

    // MCP 工具按 server 分组(mounted 口径;deferred 的不进这份明细,数字
    // 已在 mcp_tools_deferred_tokens 里)。调用方按 tokens 降序排好,这里
    // 只管原样打——空表就不画这层子级。
    struct McpServerEntry {
        std::string server;
        std::size_t tool_count = 0;
        std::size_t tokens = 0;
    };
    std::vector<McpServerEntry> mcp_servers;
};

// /context 裸敲的分类占用分析:系统提示/工具定义/对话历史三类,配一条按
// "占窗口比例"取整的条形图(默认 16 格,█ 实 ░ 空;plain 主题回退 # 和 -,
// 拿 theme.reset 是不是空串当探针——plain 全字段空串,真主题 reset 恒非空),
// 之后是已用、自动压缩线(kAutoCompactThresholdPercent)、剩余,末尾一行口径
// 说明。
// measured_used_tokens 是 tracker 实测(最近一次请求的 usage 精确 token),
// 是唯一该信的数,分两支:
//   实测 > 0(至少发过一轮请求)——已用/剩余/压缩百分比一律用实测,不带 ~;
//     系统提示、工具仍按统一口径估(带 ~,可单独算的确定部分),历史 =
//     max(0, 实测总量 - 系统 - 工具)反推(不带 ~,行尾注"=实测总量−系统−
//     工具"),三分项之和恒等于实测总量;已用行尾标"(实测)"。
//   实测 == 0(如刚启动)——三项全用统一口径估,整体带 ~,末行注明
//     "尚无实测,启动估算"。
// 前三个参数是 token 估算值(统一口径,agent::EstimateUtf8Tokens),
// 不是字符数——/3 与 /2 两把旧尺已并轨。
// cache_read_tokens > 0 时对话历史行尾括注缓存命中量;cache_hit_percent
// >= 0(分母只取输入,不带 output)时括注里带命中率,-1 = 服务端没回报
// usage,只摆命中量、不伪造 0%。window_tokens 为 0
// 不除零,百分比一律 0;占比超 100% 截断(条形打满、百分比钉在 100)。数字
// 全走 FormatTokenCount。返回逐行文本(行内不带换行符),打印由调用方管。
// detail(默认 nullptr):给了就在系统提示/工具定义两个粗桶后追加缩进子行
// (标签见 ContextBreakdownDetail 注释),并在"剩余"之后追加两行独立条形
// ——"自动压缩缓冲"(窗口 − 自动压缩线,固定预留)与"空闲空间"(自动压缩
// 线 − 已用,下限钉 0)。旧调用点不传这个参数,输出与从前逐字节一致。
// current_estimated:传入总量来自换链后的完整请求估算，不能标作实测。
std::vector<std::string> FormatContextBreakdown(std::size_t sys_tokens, std::size_t tools_tokens,
                                                 std::size_t history_tokens_est, std::int64_t cache_read_tokens,
                                                 std::size_t window_tokens, std::size_t measured_used_tokens,
                                                 const Theme& theme, int bar_width = 16,
                                                 int cache_hit_percent = -1,
                                                 const ContextBreakdownDetail* detail = nullptr,
                                                 bool current_estimated = false);

// ---- 回合视觉收束(终端回合视觉收束单):耗时人话与 turn footer ----------

// 耗时人话(单子第五节):十秒内留一位小数;十至五十九秒取整;一分钟以上
// Xm Ys;一小时以上 Xh Ym。输入毫秒,输出不带 ANSI。Working 活动条与
// Worked footer 同用这一把尺——同一只计时器,两边不得差一截。
// 例:9400 -> "9.4s";42300 -> "42s";401000 -> "6m 41s";5400000 -> "1h 30m"。
std::string FormatTurnDuration(std::int64_t milliseconds);

// turn footer 的词干:按终态挑动词——正常完成 "Worked for X"、用户打断
// "Stopped after X"、失败/预算耗尽 "Failed after X"。秒表数字走
// FormatTurnDuration 同一把尺。动词不进 i18n 表(中英同形,Codex 风格的
// 画面签名);status 只认三档:cancelled/interrupted -> Stopped,failed ->
// Failed,其余 -> Worked。
enum class TurnFooterTone { Worked, Stopped, Failed };
std::string FormatTurnFooterText(std::int64_t milliseconds, TurnFooterTone tone);

// 详细态的审批等待附注:"waited 35s for approval";approval_wait_ms <= 0
// 给空串(缺省不写,单子:缺省只留前半句)。
std::string FormatApprovalWaitNote(std::int64_t approval_wait_ms);

// ---- 列对齐小工具(TUI 界面美化单批 0) ------------------------------------
// cli::frame::* 的表格/键值对列对齐共用这一把尺;渲染层不许再手写
// pad/truncate。宽度全按 UTF-8 显示列算(中文/emoji 占双列),截断走
// TruncateUtf8ToDisplayWidth 同一把尺——不劈半个宽字。

namespace format {

// 左对齐:短了右侧补空格到 width 显示列;超了先按显示宽截断(保字头,
// 与 TruncateUtf8ToDisplayWidth 同一把尺)再补。width <= 0 给空串。
std::string AlignLeft(std::string_view text, int width);

// 右对齐:短了左侧补空格;超了同一把尺截断(保字头)后不再补空格。
// width <= 0 给空串。
std::string AlignRight(std::string_view text, int width);

}  // namespace lubancode::cli::format

}  // namespace lubancode::cli
