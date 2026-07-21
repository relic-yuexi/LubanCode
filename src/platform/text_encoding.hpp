// UTF-8 校验与清洗:不可信文本(工具输出、hook 输出、MCP 响应……)进对话
// 历史、进 nlohmann::json 序列化之前的最后一道关口。
//
// 背景(0.22.x 编码坑):PowerShell 5.1 遇到脚本解析错误(比如用户命令里
// 带 `&&`,PS 5.1 根本不认这个语法)时,错误信息在脚本真正跑到
// `[Console]::OutputEncoding=UTF8` 那一行之前就已经吐出来了,走的是系统
// ANSI 代码页(国内机器是 GBK)。这坨非法 UTF-8 字节一旦被当 UTF-8 塞进
// nlohmann::json,下次序列化就是 type_error.316,异常穿透到顶层,整个会话
// 崩掉。触发面不止 `&&`——任何 PowerShell 解析期错误、任何绕开控制台
// 编码设置直接写控制台的原生程序,都可能吐 GBK。
//
// IsValidUtf8 原是 config/prompt_files.cpp 里 SOUL.md 校验用的本地函数,
// 这里搬出来跟 SanitizeUtf8 共享同一套解码逻辑,别再造第二份。
#pragma once

#include <string>

namespace lubancode::platform {

// 严格校验一段字节是否是合法 UTF-8(拒绝过长编码、UTF-16 代理项、越界
// 码点、截断的多字节序列)。
bool IsValidUtf8(const std::string& text);

// 把任意字节串清洗成保证合法的 UTF-8:
//   - 已经合法 -> 原样返回。
//   - 非法时,Windows 上先整段按系统 ANSI 代码页(CP_ACP)重新解释一遍——
//     PowerShell/cmd 解析期错误文本、原生程序绕开控制台编码直接写出的
//     字节,十有八九是这个来路,转完往往就合法了。
//   - 转完仍不合法(非 Windows 平台,或者 ACP 转换后依然非法)——逐段
//     替换:每一处解不出来的非法字节,替换成一个 U+FFFD(替换字符),合法
//     片段原样保留,保证出口一定是合法 UTF-8。
std::string SanitizeUtf8(const std::string& text);

}  // namespace lubancode::platform
