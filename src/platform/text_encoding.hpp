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

// 首个非法字节的偏移;整段合法返回 std::string::npos。诊断信息(坏字节在
// 哪儿)用它,别在外面再写一遍解码器。
std::size_t FirstInvalidUtf8Offset(const std::string& text);

// 把任意字节串清洗成保证合法的 UTF-8:
//   - 已经合法 -> 原样返回。
//   - 非法时,Windows 上先整段按系统 ANSI 代码页(CP_ACP)重新解释一遍——
//     PowerShell/cmd 解析期错误文本、原生程序绕开控制台编码直接写出的
//     字节,十有八九是这个来路,转完往往就合法了。
//   - 转完仍不合法(非 Windows 平台,或者 ACP 转换后依然非法)——逐段
//     替换:每一处解不出来的非法字节,替换成一个 U+FFFD(替换字符),合法
//     片段原样保留,保证出口一定是合法 UTF-8。
std::string SanitizeUtf8(const std::string& text);

// 工具信任边界专用的规范化,比 SanitizeUtf8 更保守:
//   - 已经合法 -> 原样返回。
//   - 非法时先看成分:坏字节比合法多字节序列还多(或整段没有多字节序
//     列),多半是整段系统 ANSI 代码页字节(PowerShell 报错那一类),
//     Windows 上按 CP_ACP 试转一把,转完合法就用它。
//   - 坏字节零星混在合法多字节序列之间("UTF-8 文件里夹了几个 GBK 字节"
//     那类混合内容)——只把解不出来的字节逐个换成 U+FFFD,合法片段原样
//     保留。整段按 ACP 强转反而会把本来正常的中文一起转成乱码。
//   - 出口保证一定是合法 UTF-8。
// SanitizeUtf8(ACP 优先)留给 run_command 那类"输出整段都是本机编码"的
// 老场景;外来文本的公共边界一律用这个。
std::string SanitizeExternalText(const std::string& text);

// 一行诊断:字段名、字节长度、首个坏字节偏移、坏字节前后各约 6 字节的
// 十六进制窗口。编码出事时日志只记这些——不倒源码、不倒密钥、不倒正文。
std::string DescribeUtf8Issue(const std::string& field, const std::string& text);

}  // namespace lubancode::platform
