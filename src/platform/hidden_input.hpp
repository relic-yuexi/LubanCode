// 终端隐藏输入(AppSecret 一类敏感值的键入口,QQBot Windows 修复单 §5.1)。
//
// 合同:
//   - 只在真终端上工作:stdin 不是控制台/TTY 时明报稳定码,不开向导,
//     不降级成明文读——自动化走配置里的 secret_file/secret_env。
//   - 回显关闭走"行编辑器仍在"的模式:Windows 只摘 ENABLE_ECHO_INPUT
//     (退格/粘贴仍由 conhost 行编辑器处理),POSIX 只摘 ECHO。用户看得
//     见提示、看不见所敲的字符。
//   - 退出与异常都恢复回显(RAII):析构复原;Windows 下 Ctrl+C 走临时
//     控制台处理器,先把模式复原再交默认处理,不留一个"哑终端"。
//   - 读到的值只回给调用方,本件不打日志、不进 trace。C++ 普通字符串
//     不承诺绝对擦除,由调用方尽速消费。
#pragma once

#include <expected>
#include <string>

#ifndef _WIN32
#include <termios.h>
#endif

namespace lubancode::platform {

// 稳定码:
//   hidden_input_no_tty        stdin 不是交互终端(管道/重定向/服务)
//   hidden_input_read_failed   读行失败(IO 错误)
//   hidden_input_cancelled     EOF/空行取消(用户直接回车取消空输入视作取消,
//                              交调用方决定语义;这里只报 EOF)
struct HiddenInputError {
    std::string reason;
    std::string detail;
};

// 回显关闭的 RAII。进不去(非终端/拿不到模式)ok() 为假——调用方须明报,
// 不得降级成明文输入。
class EchoOffScope {
public:
    EchoOffScope();
    ~EchoOffScope();

    EchoOffScope(const EchoOffScope&) = delete;
    EchoOffScope& operator=(const EchoOffScope&) = delete;

    bool ok() const { return ok_; }

private:
    bool ok_ = false;
    bool restore_ = false;
#ifdef _WIN32
    unsigned long original_mode_ = 0;
#else
    struct termios original_termios_{};
#endif
};

// 读一行隐藏输入(先 EchoOffScope 再走整行读入,退格/粘贴由平台行编辑
// 器处理)。行尾换行已剥。非终端返回 hidden_input_no_tty。
std::expected<std::string, HiddenInputError> ReadHiddenLine();

// stdin 是否真终端(console.hpp 的 StdinIsInteractive 的镜像口,单独列
// 一份避免向导层直依赖 console 细节;语义一致)。
bool StdinIsTerminal();

}  // namespace lubancode::platform
