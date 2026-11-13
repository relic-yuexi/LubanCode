// terminal 工具的按键记号解析(纯函数,两平台都编都测)。
//
// 模型送来的是一段文本,里头可夹尖括号记号表示特殊键,解析成写进 PTY
// 主端的原始字节。记号不分大小写:
//   <Enter> <CR> <Return>      回车(\r;tty 的 ICRNL 会转成换行)
//   <Tab> <S-Tab>              制表 / 反向制表(ESC [ Z)
//   <Esc>                      ESC
//   <BS> <Backspace>           退格(DEL 0x7f,xterm 惯例)
//   <Space>                    空格
//   <Up> <Down> <Left> <Right> 方向键;app_cursor_keys 为真(DECCKM,vim/less
//                              常开)时发 ESC O x,否则 ESC [ x
//   <Home> <End> <PageUp> <PageDown> <Insert> <Delete>/<Del>
//   <F1>..<F12>
//   <C-a>..<C-z>               Ctrl 组合(<C-c> 中断、<C-d> EOF、<C-z> 挂起)
//   <C-@> <C-[> <C-\> <C-]> <C-^> <C-_>   其余 C0 控制字
//   <M-x> <A-x>                Alt/Meta 组合:ESC 前缀 + 单字符
//   <lt>                       字面 "<"
//
// 取舍:形如 <字母数字-> 且闭合的记号若不认识,直接报错,不当字面发——
// 拼错一个 <Entr> 就不回车,模型会干等到超时,还不如当场说清。孤立的
// "<"(后面不是记号形状,如 "a < b"、"<<EOF")原样当字面。要整段原样发
// (比如往编辑器里粘一段 HTML)可走 literal 模式,不解析任何记号。
#pragma once

#include <expected>
#include <string>
#include <string_view>

namespace lubancode::tools {

// 解析按键文本为原始字节。失败返回人话错误(含出错的记号原文)。
std::expected<std::string, std::string> ParseTerminalKeys(std::string_view text, bool app_cursor_keys = false);

}  // namespace lubancode::tools
