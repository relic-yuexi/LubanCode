// terminal_caps.hpp 的实现:环境快照的被动探测。每个分支的取舍都在
// 注释里写明"凭什么"——探测层最忌想当然。

#include "cli/terminal_caps.hpp"

#include <algorithm>
#include <initializer_list>
#include <string_view>

#include "platform/console.hpp"
#include "platform/paths.hpp"

namespace lubancode::cli {

const char* CapStateWord(CapState state) {
    switch (state) {
        case CapState::Yes: return "支持";
        case CapState::No: return "不支持";
        case CapState::Unknown: return "不知";
    }
    return "";
}

namespace {

bool HasValue(const std::string& s) { return !s.empty(); }

// $TERM 是否命中一组前缀/全名(xterm*、tmux*、screen* 这类命名习惯)。
bool TermMatches(const std::string& term, std::initializer_list<std::string_view> names) {
    for (const std::string_view pattern : names) {
        if (term == pattern) {
            return true;
        }
        // 前缀匹配只对带 * 语义的词条(tmux-256color 命中 tmux*)
        if (!pattern.empty() && pattern.back() == '*' &&
            std::string_view(term).substr(0, pattern.size() - 1) == pattern.substr(0, pattern.size() - 1)) {
            return true;
        }
    }
    return false;
}

}  // namespace

CapabilityEnv CollectCapabilityEnv() {
    CapabilityEnv env;
    env.term = platform::GetEnvVar("TERM").value_or("");
    env.term_program = platform::GetEnvVar("TERM_PROGRAM").value_or("");
    env.wt_session = platform::GetEnvVar("WT_SESSION").value_or("");
    env.tmux = platform::GetEnvVar("TMUX").value_or("");
    env.screen = platform::GetEnvVar("STY").value_or("");
    env.ssh_tty = platform::GetEnvVar("SSH_TTY").value_or("");
    env.ssh_connection = platform::GetEnvVar("SSH_CONNECTION").value_or("");
    env.conemu = platform::GetEnvVar("ConEmuANSI").value_or("");
    env.vscode = env.term_program == "vscode" ? "1" : "";
#ifdef _WIN32
    env.ostype = "windows";
#else
    env.ostype = "posix";
#endif
    env.stdin_interactive = platform::StdinIsInteractive();
    const platform::StdoutConsoleProbe probe = platform::ProbeStdoutConsole();
    env.stdout_console = probe.is_console;
    env.vt_enabled = probe.vt_enabled;
    return env;
}

TerminalCapabilities ProbeTerminalCapabilities(const CapabilityEnv& env) {
    TerminalCapabilities caps;

    // ---- VT/ANSI:唯一有平台 API 应答的一项。POSIX 真终端天然恒真
    // (ProbeStdoutConsole 对真终端恒报 vt_enabled=true),Windows 真控制台
    // 报 ENABLE_VIRTUAL_TERMINAL_PROCESSING 的实况。 ----
    caps.vt.key = "caps.vt";
    if (env.stdin_interactive || env.stdout_console) {
        caps.vt.state = env.vt_enabled ? CapState::Yes : CapState::No;
        caps.vt.note = "ProbeStdoutConsole 应答";
    } else {
        // 管道/重定向:谈不上支持不支持,输出侧根本不该写转义。
        caps.vt.state = CapState::No;
        caps.vt.note_key = "caps.note.no_console";
    }

    // ---- bracketed paste:程序自己开了 ?2004h;终端是否遵守,多数终端
    // 不应答、也查不到。Windows Terminal/VS Code/WezTerm/ kitty/ iTerm2/
    // mintty 有据可查;tmux 2.6+ 转发;conhost 不认(静默忽略)——但静默
    // 忽略的终端与支持的表现区分不开,归 Unknown 而不是 No,诚实。
    caps.bracketed_paste.key = "caps.bracketed_paste";
    caps.bracketed_paste.note_key = "caps.note.enabled_by_app";
    if (HasValue(env.wt_session) || env.vscode == "1" || env.term_program == "WezTerm" ||
        env.term_program == "iTerm.app" || env.term_program == "mintty" ||
        TermMatches(env.term, {"kitty"})) {
        caps.bracketed_paste.state = CapState::Yes;
        caps.bracketed_paste.note = env.term_program.empty() ? "TERM" : env.term_program;
    } else if (HasValue(env.tmux) || HasValue(env.screen)) {
        caps.bracketed_paste.state = CapState::Unknown;  // 复用器转发,与本机配置有关
        caps.bracketed_paste.note_key = "caps.note.multiplexer";
    } else {
        caps.bracketed_paste.state = CapState::Unknown;
    }

    // ---- synchronized output(DEC 2026):程序在用,但它是私有模式,终端
    // 按约定"不认得就吞掉"。不认得的终端与支持的终端都不报错,无从分辨
    // ——一律 Unknown(已知兼容名录除外)。
    caps.sync_output.key = "caps.sync_output";
    caps.sync_output.state = CapState::Unknown;
    caps.sync_output.note_key = "caps.note.private_mode";
    if (HasValue(env.wt_session)) {
        // Windows Terminal 1.24 起原生实现(conhost 同引擎);更老版本静默
        // 吞掉。没有版本号可查,仍归 Unknown,注里点名这条。
        caps.sync_output.note = "Windows Terminal(1.24+ 实现,旧版静默忽略)";
    }

    // ---- OSC 52(剪贴板):xterm 需要资源开关、tmux 需 passthrough 配置、
    // SSH 下取决于"本地"终端——都问不出,Unknown;mintty/kitty/WezTerm/
    // iTerm2 默认开。
    caps.osc52.key = "caps.osc52";
    if (env.term_program == "WezTerm" || env.term_program == "iTerm.app" ||
        TermMatches(env.term, {"mintty", "kitty"})) {
        caps.osc52.state = CapState::Yes;
        caps.osc52.note = env.term_program.empty() ? "TERM" : env.term_program;
    } else if (HasValue(env.tmux) || HasValue(env.screen)) {
        caps.osc52.state = CapState::Unknown;
        caps.osc52.note_key = "caps.note.tmux_passthrough";
    } else if (HasValue(env.ssh_connection)) {
        caps.osc52.state = CapState::Unknown;
        caps.osc52.note_key = "caps.note.depends_local_terminal";
    } else {
        caps.osc52.state = CapState::Unknown;
    }
    // Windows 本机:剪贴板走 Win32 API,不依赖 OSC 52——/copy 有确定路。
    // 只接管"本来就不明"的结论;已判支持的名录终端(kitty 跑在 Windows 上
    // 这种)不动。
    if (env.ostype == "windows" && caps.osc52.state == CapState::Unknown && !HasValue(env.tmux) &&
        !HasValue(env.screen) && !HasValue(env.ssh_connection)) {
        caps.osc52.state = CapState::No;
        caps.osc52.note = "Win32 剪贴板直写,不走 OSC 52";
    }

    // ---- 桌面通知:OSC 9/777 支持面参差,Windows 上另有系统通知。程序
    // 的策略是"拿不到就退 bell/标题",能力本身诚实报 Unknown,除 Windows
    // Terminal(OSC 9;9 toast 有官方文档)。
    caps.desktop_notify.key = "caps.desktop_notify";
    caps.desktop_notify.state = CapState::Unknown;
    caps.desktop_notify.note_key = "caps.note.notify_fallback";
    if (HasValue(env.wt_session)) {
        caps.desktop_notify.state = CapState::Yes;
        caps.desktop_notify.note = "Windows Terminal OSC 9;9";
    } else if (env.term_program == "iTerm.app") {
        caps.desktop_notify.state = CapState::Yes;
        caps.desktop_notify.note = "iTerm2 OSC 9";
    }

    // ---- modifyOtherKeys / kitty keyboard:程序不启用它们(键位够用),
    // 也就不声明能力。两项都 Unknown,注写明"未启用"。
    caps.modify_other_keys.key = "caps.modify_other_keys";
    caps.modify_other_keys.state = CapState::Unknown;
    caps.modify_other_keys.note_key = "caps.note.not_enabled";
    caps.kitty_keyboard.key = "caps.kitty_keyboard";
    caps.kitty_keyboard.state = CapState::Unknown;
    caps.kitty_keyboard.note_key = "caps.note.not_enabled";

    // ---- 尺寸 ----
    if (env.stdin_interactive || env.stdout_console) {
        if (const auto info = platform::GetScreenInfo()) {
            caps.width = info->width;
            caps.height = info->height;
        }
    }

    // ---- 处境行(不是能力,是"你在哪") ----
    if (HasValue(env.tmux)) {
        caps.environment.push_back({"caps.env.tmux", CapState::Yes, "TMUX", ""});
    }
    if (HasValue(env.screen)) {
        caps.environment.push_back({"caps.env.screen", CapState::Yes, "STY", ""});
    }
    if (HasValue(env.ssh_connection) || HasValue(env.ssh_tty)) {
        caps.environment.push_back({"caps.env.ssh", CapState::Yes, "SSH_CONNECTION/SSH_TTY", ""});
    }
    if (env.vscode == "1") {
        caps.environment.push_back({"caps.env.vscode", CapState::Yes, "TERM_PROGRAM=vscode", ""});
    }
    if (!caps.width) {
        caps.environment.push_back({"caps.env.no_tty", CapState::Yes, "", ""});
    }

    return caps;
}

std::vector<CapabilityRow> TerminalCapabilities::AllRows() const {
    std::vector<CapabilityRow> rows;
    rows.reserve(7 + environment.size());
    rows.push_back(vt);
    rows.push_back(bracketed_paste);
    rows.push_back(sync_output);
    rows.push_back(osc52);
    rows.push_back(desktop_notify);
    rows.push_back(modify_other_keys);
    rows.push_back(kitty_keyboard);
    for (const auto& row : environment) {
        rows.push_back(row);
    }
    return rows;
}

}  // namespace lubancode::cli
