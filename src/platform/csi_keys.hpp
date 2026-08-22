// CSI 转义序列 -> 语义按键的纯映射(0.28.x 排队消息取回键一单)。
//
// 为什么单独抽出来:POSIX 的序列解析住在 console_posix.cpp(只有 POSIX 才
// 编),而"CSI 1;2D = Shift+Left、CSI 1;5D = Ctrl+Left"这套参数表是纯逻辑,
// Windows 上也该能单测钉死(规格"测试"节点名要验 POSIX CSI)。于是把
// "参数串 + 终止字节 -> KeyInput"提成头文件内联的纯函数,console_posix.cpp
// 负责收字节(带超时/回压那套 IO),这里只管翻;两边都不重复写参数表。
//
// 认不出的序列整个翻成 Kind::None,参数字节绝不漏成正文字符(与
// console_posix.cpp 原 ParseCsi 的取舍一致)。bracketed paste(CSI 200~)要
// 继续读后续字节,是 IO 层的事,这里按"认不出"返回 None,由调用方先拦。
#pragma once

#include <string>

#include "platform/console.hpp"

namespace lubancode::platform {

// params:CSI 序列里终止字节之前的参数字节串(比如 "1;2"、"200");可为空
// (裸 CSI D)。final_byte:终止字节(0x40~0x7e)。
inline KeyInput MapCsiToKey(const std::string& params, char final_byte) {
    KeyInput out;
    switch (final_byte) {
        case 'A':
            out.kind = KeyInput::Kind::Up;
            break;
        case 'B':
            out.kind = KeyInput::Kind::Down;
            break;
        case 'C':
            out.kind = KeyInput::Kind::Right;
            break;
        case 'D':
            // xterm 修饰键约定:首个参数 1 = 方向键,第二个参数是修饰位
            // (2=Shift、5=Ctrl、6=Shift+Ctrl)。这里只拆左右两档:Shift+Left
            // 是排队消息取回主键,Ctrl+Left 是终端不报 Shift 时的备用键;
            // 其余修饰组合(含 Shift+Ctrl)一律退回普通 Left,不当取回键,
            // 免得"想按词跳"的终端习惯被误伤。
            if (params == "1;2") {
                out.kind = KeyInput::Kind::ShiftLeft;
            } else if (params == "1;5") {
                out.kind = KeyInput::Kind::CtrlLeft;
            } else {
                out.kind = KeyInput::Kind::Left;
            }
            break;
        case 'H':
            out.kind = KeyInput::Kind::Home;
            break;
        case 'F':
            out.kind = KeyInput::Kind::End;
            break;
        case 'Z':
            out.kind = KeyInput::Kind::ShiftTab;
            break;
        case '~':
            // VT 风格:1~/7~ = Home,4~/8~ = End,3~ = Delete,5~/6~ =
            // PageUp/PageDown;200~ 是 bracketed paste 的开头,由 IO 层
            // 先拦,这里不认。
            if (params == "1" || params == "7") {
                out.kind = KeyInput::Kind::Home;
            } else if (params == "4" || params == "8") {
                out.kind = KeyInput::Kind::End;
            } else if (params == "3") {
                out.kind = KeyInput::Kind::Delete;
            } else if (params == "5") {
                out.kind = KeyInput::Kind::PageUp;
            } else if (params == "6") {
                out.kind = KeyInput::Kind::PageDown;
            }
            break;
        default:
            break;  // 认不出,None
    }
    return out;
}

}  // namespace lubancode::platform
