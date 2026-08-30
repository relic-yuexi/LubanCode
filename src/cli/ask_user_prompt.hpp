// ask_user 工具的终端问询(骨架拆解反弹·问题 1):PromptAskUser 整段自
// app/turn_runner.cpp 搬进 cli——它是独立的终端菜单/读行逻辑,不该住在
// "驱动一回合"的文件里。调用方(session_stack 的 ask_user_handler、
// workflow_commands 的 AskUser/AskText 节点)只调这个接口。
//
// 搬家规矩:行为一字不变——菜单项、序号解析、Esc 语义、答完的回显行,
// 逐字节照旧。
#pragma once

#include <expected>
#include <string>

#include "cli/theme.hpp"
#include "tools/ask_user.hpp"

namespace lubancode::cli {

// ask_user 的问话:真控制台起方向键菜单(多选/自填/讨论),管道/非交互
// 退成编号读行。返回 Answered(答案列表)/Declined(用户明确拒答)/
// unexpected(取消,工具层按 is_error 收口)。
std::expected<lubancode::tools::AskUserResponse, std::string> PromptAskUser(
    const lubancode::tools::AskUserQuestion& question, const Theme& theme);

}  // namespace lubancode::cli
