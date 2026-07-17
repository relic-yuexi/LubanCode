// lubancode - C++ AI 编程 CLI
// M2:agent 循环接上两个工具(read_file、run_command),从"会说话"变成"会干活"。
// 有位置参数就一次问答;没参数就进入交互循环,历史跨轮保留。

#include <filesystem>
#include <iostream>
#include <memory>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

#include "agent/loop.hpp"
#include "agent/prompts.hpp"
#include "api/anthropic/client.hpp"
#include "api/backend.hpp"
#include "api/responses/client.hpp"
#include "cli/console_input.hpp"
#include "config/config.hpp"
#include "tools/edit_file.hpp"
#include "tools/read_file.hpp"
#include "tools/registry.hpp"
#include "tools/run_command.hpp"
#include "tools/search.hpp"
#include "tools/write_file.hpp"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

namespace {

constexpr std::string_view kVersion = "0.2.0";

void PrintVersion() {
    std::cout << "lubancode " << kVersion << "\n";
}

void PrintHelp() {
    std::cout << "lubancode " << kVersion << " - C++ AI 编程 CLI\n\n"
              << "用法:\n"
              << "  lubancode [选项]\n"
              << "  lubancode \"问题\"          一次问答,能用工具就用工具\n"
              << "  lubancode                  不带参数则进入交互循环,exit/quit 或 EOF(Ctrl+Z / 管道读尽)退出;空行只是重新给提示符,不退出\n\n"
              << "选项:\n"
              << "  --version   打印版本号\n"
              << "  --help      打印本帮助\n"
              << "  --yes       自动确认所有需要确认的工具调用(比如 run_command),不再逐条询问\n\n"
              << "环境变量:\n"
              << "  LUBANCODE_WIRE        协议选择,anthropic(默认)或 responses\n"
              << "  ANTHROPIC_BASE_URL    wire=anthropic 时的 API 地址,默认 https://api.minimaxi.com/anthropic\n"
              << "  ANTHROPIC_AUTH_TOKEN  wire=anthropic 时的认证令牌,必填\n"
              << "  ANTHROPIC_MODEL       wire=anthropic 时的模型名,默认 MiniMax-M3\n"
              << "  OPENAI_BASE_URL       wire=responses 时的 API 地址,默认 https://api.minimaxi.com/v1\n"
              << "  OPENAI_API_KEY        wire=responses 时的认证令牌,必填\n"
              << "  OPENAI_MODEL          wire=responses 时的模型名,默认 MiniMax-M3\n";
}

// 按 wire 造对应的后端实现。agent 层只认 Backend 这个抽象接口,不关心
// 背后具体是哪个协议在干活。
std::unique_ptr<lubancode::api::Backend> BuildBackend(const lubancode::config::Config& config) {
    if (config.wire == lubancode::config::Wire::Responses) {
        return std::make_unique<lubancode::api::responses::ResponsesBackend>(config.base_url, config.auth_token);
    }
    return std::make_unique<lubancode::api::anthropic::AnthropicBackend>(config.base_url, config.auth_token);
}

// 当前工作目录,转成 UTF-8 字符串(拼进系统提示词里给模型看)。
std::string CurrentDirUtf8() {
    const std::filesystem::path cwd = std::filesystem::current_path();
    const std::u8string u8 = cwd.u8string();
    return std::string(reinterpret_cast<const char*>(u8.data()), u8.size());
}

lubancode::tools::ToolRegistry BuildToolRegistry() {
    lubancode::tools::ToolRegistry registry;
    registry.Register(std::make_unique<lubancode::tools::ReadFileTool>());
    registry.Register(std::make_unique<lubancode::tools::RunCommandTool>());
    registry.Register(std::make_unique<lubancode::tools::WriteFileTool>());
    registry.Register(std::make_unique<lubancode::tools::EditFileTool>());
    registry.Register(std::make_unique<lubancode::tools::SearchTool>());
    return registry;
}

// 打印一段文本的前几行,超过就注明省略了多少行。给确认前的改动摘要用。
void PrintFirstLines(const std::string& text, int max_lines) {
    std::vector<std::string> lines;
    std::istringstream iss(text);
    std::string line;
    while (std::getline(iss, line)) {
        lines.push_back(line);
    }
    if (lines.empty() && !text.empty()) {
        lines.push_back(text);  // 没有换行符的单行内容
    }
    const int total = static_cast<int>(lines.size());
    for (int i = 0; i < total && i < max_lines; ++i) {
        std::cout << "      " << lines[static_cast<std::size_t>(i)] << "\n";
    }
    if (total > max_lines) {
        std::cout << "      ...(共 " << total << " 行,已省略其余)\n";
    }
}

// 确认前把工具的入参打印清楚,好让人一眼看明白将要发生什么:
// write_file/edit_file 显示路径和内容/改动的前几行摘要,run_command 显示
// 完整命令,别的按通用 JSON 打印兜底。
void PrintConfirmDetails(const std::string& name, const nlohmann::json& input) {
    if (name == "write_file") {
        const std::string path = input.value("path", std::string());
        const std::string content = input.value("content", std::string());
        std::cout << "    路径: " << path << "\n";
        std::cout << "    内容(" << content.size() << " 字节),前几行:\n";
        PrintFirstLines(content, 5);
    } else if (name == "edit_file") {
        const std::string path = input.value("path", std::string());
        const std::string old_s = input.value("old_string", std::string());
        const std::string new_s = input.value("new_string", std::string());
        const bool replace_all = input.value("replace_all", false);
        std::cout << "    路径: " << path << (replace_all ? "  (replace_all=true,全部替换)" : "") << "\n";
        std::cout << "    - 旧文本:\n";
        PrintFirstLines(old_s, 3);
        std::cout << "    + 新文本:\n";
        PrintFirstLines(new_s, 3);
    } else if (name == "run_command") {
        const std::string command = input.value("command", std::string());
        std::cout << "    命令: " << command << "\n";
    } else {
        std::cout << "    参数: " << input.dump() << "\n";
    }
    std::cout.flush();
}

// 交互循环、单发模式共用的回调:文本打字机打印,工具调用打一行提示,
// needs_confirm 的工具按 auto_confirm 决定是自动放行还是问用户一句
// (三选:y 本次允许 / a 本会话总是允许该工具 / N 拒绝)。always_allowed_tools
// 由调用方持有,跨多轮 Run() 保留,选过 a 的工具本会话内不会再问。
lubancode::agent::Callbacks BuildCallbacks(bool auto_confirm, std::set<std::string>& always_allowed_tools) {
    lubancode::agent::Callbacks callbacks;

    callbacks.on_text_delta = [](const std::string& text) {
        std::cout << text;
        std::cout.flush();
    };

    callbacks.on_tool_start = [](const std::string& name, const nlohmann::json& input) {
        std::cout << "\n[工具] " << name << " " << input.dump() << "\n";
        std::cout.flush();
    };

    callbacks.on_tool_confirm = [auto_confirm, &always_allowed_tools](const std::string& name,
                                                                       const nlohmann::json& input) -> bool {
        if (auto_confirm) {
            return true;
        }
        if (always_allowed_tools.count(name) != 0) {
            return true;
        }
        PrintConfirmDetails(name, input);
        const std::optional<std::string> answer =
            lubancode::cli::ReadLine("[y] 本次允许  [a] 本会话总是允许(该工具)  [N] 拒绝: ");
        if (!answer.has_value()) {
            return false;  // 读到 EOF,按拒绝处理,不要在这儿卡住
        }
        if (*answer == "a" || *answer == "A") {
            always_allowed_tools.insert(name);
            return true;
        }
        return *answer == "y" || *answer == "Y";
    };

    callbacks.on_tool_done = [](const std::string& name, const lubancode::tools::Tool::Result& result) {
        if (result.is_error) {
            std::cout << "[工具出错] " << name << ": " << result.content << "\n";
            std::cout.flush();
        }
    };

    return callbacks;
}

// 发一轮用户输入,走 agent loop(可能会有若干次工具调用来回),流式打字机
// 打印回复。返回 0 表示成功,非 0 表示出错。always_allowed_tools 由调用方
// 持有,记录本会话内选过"总是允许"的工具。
int RunTurn(lubancode::agent::AgentLoop& loop, const std::string& user_input, bool auto_confirm,
            std::set<std::string>& always_allowed_tools) {
    const lubancode::agent::Callbacks callbacks = BuildCallbacks(auto_confirm, always_allowed_tools);

    const auto result = loop.Run(user_input, callbacks);
    std::cout << "\n";

    if (!result.has_value()) {
        std::cerr << "[错误] " << result.error() << "\n";
        return 1;
    }
    return 0;
}

// 没带参数时的交互循环:读一行、问一句,exit/quit 或 EOF 退出。
// 空行不退出——只是重新给一次提示符,继续等下一行(老规则"空行退出"跟
// Windows 控制台偶发读空串的老毛病撞在一块,会把读空串误当成用户要退出,
// 改成只认 exit/quit/EOF 才靠谱)。
// AgentLoop 在循环外面建一次,历史跨轮保留;always_allowed_tools 同样在
// 循环外面建一次,"本会话总是允许"才能真的跨多轮用户输入生效。
void InteractiveLoop(const lubancode::config::Config& config, bool auto_confirm) {
    std::cout << "lubancode " << kVersion << " - 输入问题回车发送,exit 退出\n";

    std::unique_ptr<lubancode::api::Backend> backend = BuildBackend(config);
    lubancode::tools::ToolRegistry registry = BuildToolRegistry();
    lubancode::agent::AgentLoop loop(*backend, registry, config.model, lubancode::agent::BuildSystemPrompt(CurrentDirUtf8()));
    std::set<std::string> always_allowed_tools;

    while (true) {
        const std::optional<std::string> line = lubancode::cli::ReadLine("> ");
        if (!line.has_value()) {
            break;  // EOF:Ctrl+Z 或管道读尽
        }
        if (line->empty()) {
            continue;  // 空行不退出,重新给提示符
        }
        if (*line == "exit" || *line == "quit") {
            break;
        }
        RunTurn(loop, *line, auto_confirm, always_allowed_tools);
    }
}

// 单发模式(位置参数):也走 agent loop,同样支持工具,只是只问这一句。
int AskOnce(const lubancode::config::Config& config, const std::string& question, bool auto_confirm) {
    std::unique_ptr<lubancode::api::Backend> backend = BuildBackend(config);
    lubancode::tools::ToolRegistry registry = BuildToolRegistry();
    lubancode::agent::AgentLoop loop(*backend, registry, config.model, lubancode::agent::BuildSystemPrompt(CurrentDirUtf8()));
    std::set<std::string> always_allowed_tools;

    return RunTurn(loop, question, auto_confirm, always_allowed_tools);
}

// 真正的入口逻辑,跟平台无关:args[0] 是程序名,args[1..] 是实参。
// Windows 下 argv 单独处理(见文件末尾的 wmain),是为了绕开 Windows
// 那套"窄字符 argv 按系统 ANSI 代码页解码"的老规矩——命令行里的中文字符
// 一旦经这条路转一圈,就会被拆成不合法的 UTF-8 字节,喂给 nlohmann::json
// 的 dump() 时直接抛 type_error(316: invalid UTF-8 byte)崩掉。
int RunCli(const std::vector<std::string>& args) {
    std::string positional;
    bool auto_confirm = false;
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
        if (arg == "--yes") {
            auto_confirm = true;
            continue;
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
            return AskOnce(*config, positional, auto_confirm);
        }
        InteractiveLoop(*config, auto_confirm);
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
