// 待办清单渲染:纯函数,把 tools::TodoItem 列表画成人看的文本。工具层
// (tools/todo_tool.hpp)只管状态存取,不知道 theme 这回事;渲染这一段
// 单独拎出来放 cli 层,main.cpp 的 on_tool_done 回调、/todos 命令共用
// 这一份,不重复拼字符串逻辑。

#pragma once

#include <string>
#include <vector>

#include "cli/theme.hpp"
#include "tools/todo_tool.hpp"

namespace lubancode::cli {

// items 为空时返回"没有待办。\n"这一句;非空时每项一行、缩进两格:
//   completed   -> "☑ " + 内容,套 theme.stats(淡色)
//   in_progress -> "▸ " + 内容,套 theme.prompt(高亮)
//   pending     -> "☐ " + 内容,不着色
// plain 主题(theme.reset 为空,视作"不着色")下,三个符号退化成
// "[x]"/"[>]"/"[ ]"。结尾统一带换行,调用方直接 std::cout 打出来就行。
std::string FormatTodoList(const std::vector<tools::TodoItem>& items, const Theme& theme);

}  // namespace lubancode::cli
