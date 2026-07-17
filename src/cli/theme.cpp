#include "cli/theme.hpp"

#include <cstdlib>
#include <optional>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace lubancode::cli {

namespace {

std::optional<std::string> GetEnvVar(const char* name) {
#ifdef _WIN32
    char* buffer = nullptr;
    std::size_t size = 0;
    const errno_t err = _dupenv_s(&buffer, &size, name);
    if (err != 0 || buffer == nullptr) {
        return std::nullopt;
    }
    std::string value(buffer);
    std::free(buffer);
    if (value.empty()) {
        return std::nullopt;
    }
    return value;
#else
    const char* value = std::getenv(name);
    if (value == nullptr || value[0] == '\0') {
        return std::nullopt;
    }
    return std::string(value);
#endif
}

Theme DarkTheme() {
    Theme t;
    t.banner = "\x1b[1;36m";     // 亮青,加粗
    t.prompt = "\x1b[1;32m";     // 亮绿,加粗
    t.tool_line = "\x1b[33m";    // 黄
    t.confirm = "\x1b[35m";      // 品红
    t.error = "\x1b[1;31m";      // 亮红,加粗
    t.stats = "\x1b[2;37m";      // 暗淡的白(灰)
    t.spinner = "\x1b[36m";      // 青
    t.reset = "\x1b[0m";
    return t;
}

Theme LightTheme() {
    Theme t;
    t.banner = "\x1b[1;34m";  // 深蓝,加粗
    t.prompt = "\x1b[0;32m";  // 深绿
    t.tool_line = "\x1b[0;33m";  // 深黄(棕)
    t.confirm = "\x1b[0;35m";    // 深品红
    t.error = "\x1b[1;31m";      // 红,加粗(哪个背景下都得显眼)
    t.stats = "\x1b[2;30m";      // 暗淡的黑(浅灰)
    t.spinner = "\x1b[0;36m";    // 深青
    t.reset = "\x1b[0m";
    return t;
}

Theme PlainTheme() {
    return Theme{};  // 全部字段默认就是空串
}

}  // namespace

Theme BuiltinTheme(const std::string& name) {
    if (name == "light") {
        return LightTheme();
    }
    if (name == "plain") {
        return PlainTheme();
    }
    return DarkTheme();  // 默认、以及任何不认得的名字
}

Theme ResolveTheme(const std::string& name, bool enable_colors) {
    if (!enable_colors) {
        return PlainTheme();
    }
    return BuiltinTheme(name);
}

bool ComputeColorsEnabled(bool stdout_is_console, bool vt_processing_ok, bool force_color) {
    if (force_color) {
        return true;
    }
    return stdout_is_console && vt_processing_ok;
}

#ifdef _WIN32

ConsoleCapability DetectConsoleCapability() {
    ConsoleCapability cap;

    const HANDLE h = GetStdHandle(STD_OUTPUT_HANDLE);
    bool is_console = false;
    bool vt_ok = false;
    if (h != nullptr && h != INVALID_HANDLE_VALUE && GetFileType(h) == FILE_TYPE_CHAR) {
        DWORD mode = 0;
        if (GetConsoleMode(h, &mode) != 0) {
            is_console = true;
            if ((mode & ENABLE_VIRTUAL_TERMINAL_PROCESSING) != 0) {
                vt_ok = true;
            } else if (SetConsoleMode(h, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING) != 0) {
                vt_ok = true;
            }
        }
    }

    const auto force = GetEnvVar("LUBANCODE_FORCE_COLOR");
    const bool force_color = force.has_value() && *force == "1";

    cap.is_console = is_console;
    cap.colors_enabled = ComputeColorsEnabled(is_console, vt_ok, force_color);
    return cap;
}

#else

ConsoleCapability DetectConsoleCapability() {
    ConsoleCapability cap;
    cap.is_console = isatty(fileno(stdout)) != 0;
    const auto force = GetEnvVar("LUBANCODE_FORCE_COLOR");
    const bool force_color = force.has_value() && *force == "1";
    // 非 Windows 平台没有 VT 处理这层开关,真终端天然支持 ANSI。
    cap.colors_enabled = ComputeColorsEnabled(cap.is_console, /*vt_processing_ok=*/true, force_color);
    return cap;
}

#endif

}  // namespace lubancode::cli
