// lubancode - C++ AI 编程 CLI 的平台入口。这里只做两件事:argv 转码
// (Windows 的 wmain 先转 UTF-8)和转交 lubancode::app::RunCli。其余
// 一切——参数解析、配置向导、装配、交互循环、单发——都在 src/app/ 各
// 件里(分层与搬家账目见 todos/重构maincpp.todo)。

#include <string>
#include <vector>

#include "app/cli_app.hpp"
#include "platform/console.hpp"
#include "platform/paths.hpp"

#ifdef _WIN32

// Windows 下用宽字符入口:窄字符 main(argc, char**) 的 argv 是 CRT 按
// "系统 ANSI 代码页"(不是 UTF-8)解码来的,中文命令行参数会被解码错。
// wmain 拿到的是原始的 UTF-16 参数,经 platform::WideToUtf8 转成 UTF-8,
// 才能跟程序内部统一按 UTF-8 处理的字符串对上。这是全程序最后一处
// #ifdef _WIN32(平台差异其余都收进 platform/ 了)。
int wmain(int argc, wchar_t** argv) {
    lubancode::platform::SetupConsoleUtf8();

    std::vector<std::string> args;
    args.reserve(static_cast<std::size_t>(argc));
    for (int i = 0; i < argc; ++i) {
        args.push_back(lubancode::platform::WideToUtf8(argv[i]));
    }
    return lubancode::app::RunCli(args);
}

#else

// POSIX 下 argv 天然就是字节串(约定 UTF-8),直通。
int main(int argc, char** argv) {
    std::vector<std::string> args(argv, argv + argc);
    return lubancode::app::RunCli(args);
}

#endif
