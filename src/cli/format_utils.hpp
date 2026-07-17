// 0.17.0:数字/状态行文本的纯格式化函数。不碰任何 IO、不认 Win32,单测
// 直接钉断言。终端层(console_input.cpp 的状态行)、main.cpp(统计行、
// /context)共用,不各写一份。

#pragma once

#include <cstdint>
#include <string>

#include "cli/line_editor.hpp"  // ConfirmMode

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

}  // namespace lubancode::cli
