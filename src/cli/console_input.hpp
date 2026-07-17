// 全程序唯一的 stdin 读入口。存在的理由是绕开一个 Windows 老毛病:
// main.cpp 里 SetConsoleCP(CP_UTF8) 之后,窄字符 std::getline(std::cin, ...)
// 读中文,在真控制台(conhost)下会跟 ReadFile 的 CP_UTF8 支持撞车——
// typed 的多字节字符偶发读空或读乱,尤其是几次 ReadFile 交替调用之后
// (交互模式里"主提示符读一行"跟"工具确认读一行"正好就是这种交替)。
// 见 console_input.cpp 开头注释,写了实测结论。

#pragma once

#include <optional>
#include <string>

namespace lubancode::cli {

// 打印 prompt(不含换行,可传空串跳过打印),读一行输入。
// Windows 下 stdin 是真控制台(GetFileType == FILE_TYPE_CHAR)时,走
// ReadConsoleW 读宽字符再转 UTF-8,彻底绕开窄字符 CP_UTF8 读取的坑;
// stdin 是管道/重定向文件时,回退到 std::getline(保住
// `echo "x" | lubancode.exe` 这种自动化用法和集成测试)。
// 统一剥掉行尾的 \r\n。EOF(Ctrl+Z 或管道读尽)返回 std::nullopt。
std::optional<std::string> ReadLine(const std::string& prompt);

}  // namespace lubancode::cli
