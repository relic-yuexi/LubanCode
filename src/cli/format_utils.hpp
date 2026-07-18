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
// "⏵⏵ yolo ..."。纯文本不夹 ANSI,配什么色是终端层的事(跟
// ConfirmModePromptPrefix 一个规矩)。
std::string StatusLineModeSegment(ConfirmMode mode);

// 常驻状态行的"信息"段:" · <模型名> · context <p>%",used_tokens > 0 时
// 再接 " (<用量>/<窗口>)",数字走 FormatTokenCount。模型名为空就跳过那一节
// (状态行别摆一个空档)。
std::string StatusLineInfoSegment(const std::string& model, int context_percent,
                                   std::int64_t used_tokens, std::int64_t window_tokens);

// 整行状态行文本 = 模式段 + 信息段。单测钉这一个就把拼装规则全钉住了。
std::string BuildStatusLineText(ConfirmMode mode, const std::string& model, int context_percent,
                                 std::int64_t used_tokens, std::int64_t window_tokens);

// /context 裸敲的分类占用分析:系统提示/工具定义/对话历史三类,各拿字符数
// 估 token(字符数/3,数字前带 ~),配一条按"占窗口比例"取整的条形图
// (默认 16 格,█ 实 ░ 空;plain 主题回退 # 和 -,拿 theme.reset 是不是
// 空串当探针——plain 全字段空串,真主题 reset 恒非空),之后是合计、自动
// 压缩线(kAutoCompactThresholdPercent)、剩余,末尾一行估算口径说明。
// 已用取"三类估算之和"与 measured_used_tokens(tracker 实测,通常更准)
// 中较大者,用实测时行尾标"(实测)"、数字不带 ~。cache_read_tokens > 0
// 时对话历史行尾括注缓存命中量。window_tokens 为 0 不除零,百分比一律 0;
// 占比超 100% 截断(条形打满、百分比钉在 100)。数字全走 FormatTokenCount。
// 返回逐行文本(行内不带换行符),打印由调用方管。
std::vector<std::string> FormatContextBreakdown(std::size_t sys_chars, std::size_t tools_chars,
                                                 std::size_t history_chars, std::int64_t cache_read_tokens,
                                                 std::size_t window_tokens, std::size_t measured_used_tokens,
                                                 const Theme& theme, int bar_width = 16);

}  // namespace lubancode::cli
