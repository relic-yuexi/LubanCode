// 命令安全分类器(0.20.x,auto 档"自动分析"用):纯函数,给一条 shell
// 命令判个"安全/要确认"。照 Claude Code 的路数——只读、探查类命令直接
// 放行,写盘、删改、联网、起进程这些一律要人点头。
//
// 总纲是保守:不认识的首词 = NeedsConfirm。宁可多问一句,不能放走一条
// 危险命令。规则细目(拆链、引号状态机、重定向、黑白名单、git 子命令、
// 探版放行、$env: 前缀)见 .cpp 顶注释和实现处逐条注释。
//
// 放在 tools/ 但不碰任何 IO、不认识 Tool 接口——main.cpp 的 on_tool_confirm
// 在 auto 档下拿 run_command 的 command/shell 两个字段来问一嘴,Safe 就
// 不弹确认。单测直接钉这函数。

#pragma once

#include <string>

namespace lubancode::tools {

enum class CommandSafety { Safe, NeedsConfirm };

// command:要执行的命令原文;shell:"powershell" 或 "cmd"(跟 run_command
// 工具的 shell 参数同一套语义)。别的 shell 值一律 NeedsConfirm。
CommandSafety ClassifyCommand(const std::string& command, const std::string& shell);

}  // namespace lubancode::tools
