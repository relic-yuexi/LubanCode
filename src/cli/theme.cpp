#include "cli/theme.hpp"

#include <optional>

#include "platform/console.hpp"
#include "platform/paths.hpp"

namespace lubancode::cli {

namespace {

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
    t.diff_add_bg = "\x1b[48;5;22m";   // 深绿底(默认前景在深色终端上看得清)
    t.diff_del_bg = "\x1b[48;5;52m";   // 深红底
    t.diff_line_no = "\x1b[2;37m";     // 行号栏淡灰
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
    t.diff_add_bg = "\x1b[48;5;194m";  // 浅绿底(浅色终端下深色前景照样清楚)
    t.diff_del_bg = "\x1b[48;5;224m";  // 浅红底
    t.diff_line_no = "\x1b[2;30m";     // 行号栏浅灰
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

ConsoleCapability DetectConsoleCapability() {
    // 探测 + VT 开启统一在 platform::ProbeStdoutConsole(Windows 下顺手把
    // ENABLE_VIRTUAL_TERMINAL_PROCESSING 打开;POSIX 真终端天然支持 ANSI,
    // vt_enabled 恒真),这里只剩强制着色开关的合成。
    const platform::StdoutConsoleProbe probe = platform::ProbeStdoutConsole();

    const auto force = platform::GetEnvVar("LUBANCODE_FORCE_COLOR");
    const bool force_color = force.has_value() && *force == "1";

    ConsoleCapability cap;
    cap.is_console = probe.is_console;
    cap.colors_enabled = ComputeColorsEnabled(probe.is_console, probe.vt_enabled, force_color);
    return cap;
}

}  // namespace lubancode::cli
