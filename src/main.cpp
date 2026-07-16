// lubancode - C++ AI 编程 CLI
// M0 骨架:只管解析 --version / --help,顺带证明 cpr、nlohmann-json 两个依赖能链接、能用。

#include <cpr/cpr.h>
#include <nlohmann/json.hpp>

#include <iostream>
#include <string>
#include <string_view>

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
              << "  lubancode [选项]\n\n"
              << "选项:\n"
              << "  --version   打印版本号\n"
              << "  --help      打印本帮助\n";
}

// 拼一个 nlohmann::json 对象,证明 nlohmann-json 能用。
void DemoJson() {
    nlohmann::json info = {
        {"name", "lubancode"},
        {"version", kVersion},
        {"deps", {"cpr", "nlohmann-json"}},
    };
    std::cout << "[demo] nlohmann::json -> " << info.dump() << "\n";
}

// 只打印 cpr 的版本宏,不发任何网络请求,证明 cpr 链接成功。
void DemoCpr() {
    std::cout << "[demo] cpr 链接成功, CPR_VERSION = " << CPR_VERSION << "\n";
}

}  // namespace

int main(int argc, char** argv) {
#ifdef _WIN32
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);
#endif

    for (int i = 1; i < argc; ++i) {
        const std::string_view arg = argv[i];
        if (arg == "--version") {
            PrintVersion();
            return 0;
        }
        if (arg == "--help") {
            PrintHelp();
            return 0;
        }
    }

    PrintVersion();
    DemoJson();
    DemoCpr();
    return 0;
}
