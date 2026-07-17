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

#include <optional>
#include <string>

#include "cli/line_editor.hpp"
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
std::optional<std::string> ReadLine(const std::string& prompt, const Theme& theme = Theme{});

// 会话级确认模式的查询/设置。真控制台下 Shift+Tab 会改这个状态(存在
// ReadLine() 内部维护的、贯穿整条交互会话的 LineEditorCore 实例里,见
// console_input.cpp 的 SharedEditor());main.cpp 的工具确认回调、主提示符
// 前缀都读这个。--yes 等价于启动时调一次
// SetConfirmMode(ConfirmMode::Yolo)。管道/重定向模式下这两个函数依然可用
// (状态本身不依赖真实控制台),只是永远不会被 Shift+Tab 改变——管道场景
// 根本读不到"按键",只有整行文本。
ConfirmMode CurrentConfirmMode();
void SetConfirmMode(ConfirmMode mode);

}  // namespace lubancode::cli
