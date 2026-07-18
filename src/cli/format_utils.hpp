// 0.17.0:数字/状态行文本的纯格式化函数。不碰任何 IO、不认 Win32,单测
// 直接钉断言。终端层(console_input.cpp 的状态行)、main.cpp(统计行、
// /context)共用,不各写一份。

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
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

// 0.21.x 流式脚注文本(纯函数,i18n 驱动,不夹 ANSI/不认 IO)。流式期间在
// 正文下方常驻一行,让用户看见"能按 ESC 打断、能键入并回车排队下一条"。
// StreamHintText:空闲(没在键入)时的可发现性提示。
// StreamQueueEchoText:用户正键入排队消息时的实时回显,尾巴接上已键入内容。
// plain 为真(plain 主题/不支持 ANSI)时去掉 ⎋ 符号、退回纯 "ESC" 文字。
std::string StreamHintText(bool plain);
std::string StreamQueueEchoText(const std::string& typed, bool plain);

// /context 裸敲的分类占用分析:系统提示/工具定义/对话历史三类,配一条按
// "占窗口比例"取整的条形图(默认 16 格,█ 实 ░ 空;plain 主题回退 # 和 -,
// 拿 theme.reset 是不是空串当探针——plain 全字段空串,真主题 reset 恒非空),
// 之后是已用、自动压缩线(kAutoCompactThresholdPercent)、剩余,末尾一行口径
// 说明。
// measured_used_tokens 是 tracker 实测(最近一次请求的 usage 精确 token),
// 是唯一该信的数,分两支:
//   实测 > 0(至少发过一轮请求)——已用/剩余/压缩百分比一律用实测,不带 ~;
//     系统提示、工具仍按字符数/3 估(带 ~,可单独算的确定部分),历史 =
//     max(0, 实测总量 - 系统 - 工具)反推(不带 ~,行尾注"=实测总量−系统−
//     工具"),三分项之和恒等于实测总量;已用行尾标"(实测)"。
//   实测 == 0(如刚启动)——三项全用字符估(字符数/3),整体带 ~,末行注明
//     "尚无实测,启动估算"。
// cache_read_tokens > 0 时对话历史行尾括注缓存命中量。window_tokens 为 0
// 不除零,百分比一律 0;占比超 100% 截断(条形打满、百分比钉在 100)。数字
// 全走 FormatTokenCount。返回逐行文本(行内不带换行符),打印由调用方管。
std::vector<std::string> FormatContextBreakdown(std::size_t sys_chars, std::size_t tools_chars,
                                                 std::size_t history_chars, std::int64_t cache_read_tokens,
                                                 std::size_t window_tokens, std::size_t measured_used_tokens,
                                                 const Theme& theme, int bar_width = 16);

}  // namespace lubancode::cli
