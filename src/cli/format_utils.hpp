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
};

// 状态行数据的局部更新:只改 context/tokens 两段的数字与旧值标记,其余
// 字段(model/cwd/git_branch/provider/effort/rec)原样保住——回合内主请求
// usage 到达时发布新快照用,不另造一份残缺 StatusPanelData。纯函数,单测
// 钉"其他段保住"这一条。measured=false 把数字标成旧值(见
// StatusPanelData::context_stale)。
StatusPanelData WithContextUpdate(StatusPanelData data, int context_percent, std::int64_t used_tokens,
                                  std::int64_t window_tokens, bool measured);

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

// 0.21.x 流式脚注文本(纯函数,i18n 驱动,不夹 ANSI/不认 IO)。
// StreamHintText:输入行空闲(没在键入)时的淡色占位提示——只提"键入并
// 回车排队",不再捎带打断说明(0.25.x 起"Esc 打断"挪进状态行)。
// StreamFooterInterruptText:状态行末尾追加的打断提示。plain 为真(plain
// 主题/不支持 ANSI)时两者都去掉 ⎋ 符号、退回纯 "ESC" 文字。
std::string StreamHintText(bool plain);
std::string StreamFooterInterruptText(bool plain);

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
// cache_read_tokens > 0 时对话历史行尾括注缓存命中量。window_tokens 为 0
// 不除零,百分比一律 0;占比超 100% 截断(条形打满、百分比钉在 100)。数字
// 全走 FormatTokenCount。返回逐行文本(行内不带换行符),打印由调用方管。
std::vector<std::string> FormatContextBreakdown(std::size_t sys_tokens, std::size_t tools_tokens,
                                                 std::size_t history_tokens_est, std::int64_t cache_read_tokens,
                                                 std::size_t window_tokens, std::size_t measured_used_tokens,
                                                 const Theme& theme, int bar_width = 16);

}  // namespace lubancode::cli
