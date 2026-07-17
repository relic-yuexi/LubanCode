// lubancode - C++ AI 编程 CLI
// M1:打通 Anthropic Messages 格式的流式问答通路。
// 有位置参数就一次问答;没参数就进入简单的读一行、问一句的循环。

#include <iostream>
#include <string>
#include <string_view>
#include <type_traits>
#include <variant>
#include <vector>

#include "api/anthropic/client.hpp"
#include "api/types.hpp"
#include "config/config.hpp"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

namespace {

constexpr std::string_view kVersion = "0.1.0";

void PrintVersion() {
    std::cout << "lubancode " << kVersion << "\n";
}

void PrintHelp() {
    std::cout << "lubancode " << kVersion << " - C++ AI 编程 CLI\n\n"
              << "用法:\n"
              << "  lubancode [选项]\n"
              << "  lubancode \"问题\"          一次问答,流式打印回复\n"
              << "  lubancode                  不带参数则进入交互循环,空行或 exit 退出\n\n"
              << "选项:\n"
              << "  --version   打印版本号\n"
              << "  --help      打印本帮助\n\n"
              << "环境变量:\n"
              << "  ANTHROPIC_BASE_URL    API 地址,默认 https://api.minimaxi.com/anthropic\n"
              << "  ANTHROPIC_AUTH_TOKEN  认证令牌,必填\n"
              << "  ANTHROPIC_MODEL       模型名,默认 MiniMax-M3\n";
}

// 发一轮单次问答:拼一个只含这一句用户话的 Request,流式打印回复。
// TextDelta 一到就 flush 输出,不攒。返回 0 表示成功,非 0 表示出错。
int AskOnce(const lubancode::config::Config& config, const std::string& question) {
    lubancode::api::anthropic::AnthropicBackend backend(config.base_url, config.auth_token);

    lubancode::api::Request request;
    request.model = config.model;
    request.max_tokens = 4096;

    lubancode::api::Message message;
    message.role = lubancode::api::Role::User;
    message.content.push_back(lubancode::api::TextBlock{question});
    request.messages.push_back(std::move(message));

    bool had_stream_error = false;
    bool printed_anything = false;

    const auto result = backend.send_stream(request, [&](const lubancode::api::StreamEvent& event) {
        std::visit(
            [&](const auto& e) {
                using T = std::decay_t<decltype(e)>;
                if constexpr (std::is_same_v<T, lubancode::api::TextDelta>) {
                    std::cout << e.text;
                    std::cout.flush();
                    printed_anything = true;
                } else if constexpr (std::is_same_v<T, lubancode::api::StreamError>) {
                    std::cerr << "\n[错误] " << e.message << "\n";
                    had_stream_error = true;
                }
                // MessageStart / ToolUseStart / ToolUseInputDelta /
                // ContentBlockDone / MessageDone:M1 的单轮问答不需要
                // 额外处理,交给以后的 agent 循环用。
            },
            event);
    });

    if (!result.has_value()) {
        std::cerr << "[错误] " << result.error().message << "\n";
        return 1;
    }
    if (printed_anything) {
        std::cout << "\n";
    }
    return had_stream_error ? 1 : 0;
}

// 没带参数时的交互循环:读一行、问一句,空行或 exit 退出。
// M1 不攒对话历史,每次都是单发的一轮问答。
void InteractiveLoop(const lubancode::config::Config& config) {
    std::cout << "lubancode " << kVersion << " - 输入问题回车发送,空行或 exit 退出\n";
    std::string line;
    while (true) {
        std::cout << "> ";
        if (!std::getline(std::cin, line)) {
            break;
        }
        if (line.empty() || line == "exit") {
            break;
        }
        AskOnce(config, line);
    }
}

// 真正的入口逻辑,跟平台无关:args[0] 是程序名,args[1..] 是实参。
// Windows 下 argv 单独处理(见文件末尾的 wmain),是为了绕开 Windows
// 那套"窄字符 argv 按系统 ANSI 代码页解码"的老规矩——命令行里的中文字符
// 一旦经这条路转一圈,就会被拆成不合法的 UTF-8 字节,喂给 nlohmann::json
// 的 dump() 时直接抛 type_error(316: invalid UTF-8 byte)崩掉。
int RunCli(const std::vector<std::string>& args) {
    std::string positional;
    for (std::size_t i = 1; i < args.size(); ++i) {
        const std::string& arg = args[i];
        if (arg == "--version") {
            PrintVersion();
            return 0;
        }
        if (arg == "--help") {
            PrintHelp();
            return 0;
        }
        if (!positional.empty()) {
            positional += " ";
        }
        positional += arg;
    }

    const auto config = lubancode::config::LoadFromEnv();
    if (!config.has_value()) {
        std::cerr << config.error() << "\n";
        return 1;
    }

    // 兜底:JSON 编码、网络库内部等地方万一抛出没接住的异常,也不能让
    // 整个进程崩掉(崩掉的话用户只会看到一个莫名其妙的退出码)。
    try {
        if (!positional.empty()) {
            return AskOnce(*config, positional);
        }
        InteractiveLoop(*config);
    } catch (const std::exception& e) {
        std::cerr << "[错误] 未预料的异常: " << e.what() << "\n";
        return 1;
    }
    return 0;
}

}  // namespace

#ifdef _WIN32

// Windows 下用宽字符入口:窄字符 main(argc, char**) 的 argv 是 CRT 按
// "系统 ANSI 代码页"(不是 UTF-8)解码来的,中文命令行参数会被解码错。
// wmain 拿到的是原始的 UTF-16 参数,自己用 WideCharToMultiByte 转成
// UTF-8,才能跟程序内部统一按 UTF-8 处理的字符串对上。
int wmain(int argc, wchar_t** argv) {
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);

    std::vector<std::string> args;
    args.reserve(static_cast<std::size_t>(argc));
    for (int i = 0; i < argc; ++i) {
        const int utf8_len = WideCharToMultiByte(CP_UTF8, 0, argv[i], -1, nullptr, 0, nullptr, nullptr);
        std::string utf8;
        if (utf8_len > 0) {
            utf8.resize(static_cast<std::size_t>(utf8_len - 1));  // 去掉结尾的 \0
            if (utf8_len > 1) {
                WideCharToMultiByte(CP_UTF8, 0, argv[i], -1, utf8.data(), utf8_len, nullptr, nullptr);
            }
        }
        args.push_back(std::move(utf8));
    }
    return RunCli(args);
}

#else

int main(int argc, char** argv) {
    std::vector<std::string> args(argv, argv + argc);
    return RunCli(args);
}

#endif
