// 实测结论(诊断交互模式"答完一轮就自动退出"的 bug 时留下的记录):
//
// 现象:交互模式里过了两次工具确认([y]/[a]/[N] 各读一行),模型答完回到
// `> ` 主提示符,用户接着输中文,程序却直接退出了——跟"空行退出"的规则对上了,
// 说明那次 std::getline 读到的是空串,不是用户真敲的内容。
//
// 复现条件:这台机器上的 shell 工具(git-bash 起子进程)拿不到真控制台句柄
// (GetFileType(stdin) 恒为 FILE_TYPE_PIPE),没法用自动化脚本直接敲键盘去
// 触发 conhost 的 bug,已用 GetFileType/GetConsoleMode 探测程序实测确认了
// 这一点。管道场景下 CP_UTF8 这条坑本来就不会踩上(SetConsoleCP 只影响
// "真控制台"的 ReadFile 语义,对管道没有任何编码转换),所以自动化管道测试
// 天然复现不出这个 bug——这跟用户反馈"部分场景正常、部分场景炸"的说法也对得上。
//
// 病根按代码走查 + 已知问题排查确认为疑点 1:
//   Windows 的 conhost 在输入代码页设为 CP_UTF8(65001)时,narrow 版
//   ReadFile/ReadConsoleA 读多字节字符有年头的已知 bug——多次 ReadFile 交替
//   调用之后,内部对"上一次没读完的多字节序列"的状态会跟丢,后续一次读到
//   空串或半个字符。main.cpp 里主提示符的 getline 和确认提示的 getline
//   正好是两次独立的窄字符 ReadFile 调用,过了两次确认提示、读了两次之后,
//   再读主提示符这次撞上算是刚好齐活。
//   （疑点 2 排除:std::getline 读到 "y"/"a" 都是成功返回,不会置 failbit/eofbit,
//   std::cin 的流状态本身没脏,所以不是"确认提示弄脏了流状态"这条。
//   疑点 3 单独看不足以解释"整行变空串"，最多影响到行尾多一个字符。）
//
// 修法:交互模式全程只留这一个 stdin 入口——真控制台就用 ReadConsoleW 读
// 宽字符再转 UTF-8,彻底不走窄字符 CP_UTF8 那条路;stdin 是管道/重定向时
// 走原来的 std::getline,不影响 `echo "x" | lubancode.exe` 这种自动化用法。
//
// ReadConsoleW 这条路没法在当前 headless 环境里自动化敲键盘验证(见上面
// "复现条件"),已经过编译告警检查(/W4 /permissive- 无告警)和逐行代码走查。

#include "cli/console_input.hpp"

#include <iostream>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <cstddef>
#endif

namespace lubancode::cli {

namespace {

void StripTrailingCrLf(std::string& s) {
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) {
        s.pop_back();
    }
}

#ifdef _WIN32

// stdin 是不是挂着一个真控制台(不是管道、不是重定向的磁盘文件)。
bool StdinIsRealConsole() {
    const HANDLE h = GetStdHandle(STD_INPUT_HANDLE);
    if (h == nullptr || h == INVALID_HANDLE_VALUE) {
        return false;
    }
    if (GetFileType(h) != FILE_TYPE_CHAR) {
        return false;
    }
    DWORD mode = 0;
    return GetConsoleMode(h, &mode) != 0;
}

// 用 ReadConsoleW 读一整行宽字符(conhost 的行编辑器——退格、方向键、
// 输入法——都在这一步之前处理好了,交出来的是敲完回车的完整一行),
// 再转 UTF-8。宽字符层面天然没有"多字节序列被截断"的问题,绕开了
// CP_UTF8 窄读的坑。
std::optional<std::string> ReadLineFromConsole() {
    const HANDLE h = GetStdHandle(STD_INPUT_HANDLE);
    std::wstring wide;
    wchar_t buf[1024];
    while (true) {
        DWORD read = 0;
        const BOOL ok = ReadConsoleW(h, buf, static_cast<DWORD>(std::size(buf)), &read, nullptr);
        if (!ok || read == 0) {
            // 真出错或者读到 0 字节(极少见),当 EOF 处理,别在这儿死循环。
            return std::nullopt;
        }
        wide.append(buf, read);
        if (!wide.empty() && wide.back() == L'\n') {
            break;  // 一行敲完回车,conhost 会在这次 ReadConsoleW 里交出结尾的 \n
        }
    }
    while (!wide.empty() && (wide.back() == L'\n' || wide.back() == L'\r')) {
        wide.pop_back();
    }
    // Ctrl+Z 独占一行时,conhost 转成 0x1A(SUB)字符交给我们,不是流层面的
    // 真 EOF 信号,这里按老习惯当 EOF 处理,免得死循环问下去。
    if (wide.size() == 1 && wide[0] == 0x1A) {
        return std::nullopt;
    }
    const int utf8_len =
        WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), nullptr, 0, nullptr, nullptr);
    std::string utf8;
    if (utf8_len > 0) {
        utf8.resize(static_cast<std::size_t>(utf8_len));
        WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), utf8.data(), utf8_len, nullptr,
                             nullptr);
    }
    return utf8;
}

#endif  // _WIN32

}  // namespace

std::optional<std::string> ReadLine(const std::string& prompt) {
    if (!prompt.empty()) {
        std::cout << prompt;
        std::cout.flush();
    }

#ifdef _WIN32
    if (StdinIsRealConsole()) {
        return ReadLineFromConsole();
    }
#endif

    std::string line;
    if (!std::getline(std::cin, line)) {
        return std::nullopt;
    }
    StripTrailingCrLf(line);
    return line;
}

}  // namespace lubancode::cli
