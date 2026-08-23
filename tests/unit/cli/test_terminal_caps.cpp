// Capability 层(交互抛光总账第一步"三层地基"之三)的纯逻辑测试:
// 喂什么环境快照出什么结论。重点钉"诚实"——认不出写不知,不瞎报支持。

#include <doctest/doctest.h>

#include "cli/terminal_caps.hpp"

using lubancode::cli::CapState;
using lubancode::cli::CapabilityEnv;
using lubancode::cli::ProbeTerminalCapabilities;

TEST_CASE("裸 POSIX 终端:VT 有据,OSC52/通知不知") {
    CapabilityEnv env;
    env.term = "xterm-256color";
    env.stdin_interactive = true;
    env.stdout_console = false;  // POSIX 探针语义:真终端但非 Win32 控制台
    env.vt_enabled = true;
    env.ostype = "posix";

    const auto caps = ProbeTerminalCapabilities(env);
    CHECK(caps.vt.state == CapState::Yes);
    // xterm 家族的 OSC 52 要资源开关,问不出——不知,不是"支持"。
    CHECK(caps.osc52.state == CapState::Unknown);
    CHECK(caps.desktop_notify.state == CapState::Unknown);
    // 不启用的协议不声明能力。
    CHECK(caps.modify_other_keys.state == CapState::Unknown);
    CHECK(caps.kitty_keyboard.state == CapState::Unknown);
}

TEST_CASE("tmux/SSH:OSC52 归不知并给处境行") {
    CapabilityEnv env;
    env.term = "screen-256color";
    env.tmux = "/tmp/tmux-1000/default,123,0";
    env.stdin_interactive = true;
    env.vt_enabled = true;
    env.ostype = "posix";

    auto caps = ProbeTerminalCapabilities(env);
    CHECK(caps.osc52.state == CapState::Unknown);
    CHECK(caps.bracketed_paste.state == CapState::Unknown);
    bool saw_tmux = false;
    for (const auto& row : caps.environment) {
        if (row.key == "caps.env.tmux") {
            saw_tmux = true;
        }
    }
    CHECK(saw_tmux);

    env.tmux.clear();
    env.ssh_connection = "10.0.0.2 5555 10.0.0.3 22";
    caps = ProbeTerminalCapabilities(env);
    bool saw_ssh = false;
    for (const auto& row : caps.environment) {
        if (row.key == "caps.env.ssh") {
            saw_ssh = true;
        }
    }
    CHECK(saw_ssh);
    CHECK(caps.osc52.state == CapState::Unknown);  // 取决于本地终端,不知
}

TEST_CASE("Windows Terminal:粘贴/通知有据,OSC52 走 Win32 直写") {
    CapabilityEnv env;
    env.wt_session = "some-guid";
    env.stdin_interactive = true;
    env.stdout_console = true;
    env.vt_enabled = true;
    env.ostype = "windows";

    const auto caps = ProbeTerminalCapabilities(env);
    CHECK(caps.vt.state == CapState::Yes);
    CHECK(caps.bracketed_paste.state == CapState::Yes);
    CHECK(caps.desktop_notify.state == CapState::Yes);
    CHECK(caps.osc52.state == CapState::No);  // Win32 剪贴板直写,不走 OSC 52
}

TEST_CASE("知名终端名录:mintty/kitty/WezTerm 的 OSC52 判支持") {
    CapabilityEnv env;
    env.stdin_interactive = true;
    env.vt_enabled = true;
    env.ostype = "posix";

    env.term_program = "WezTerm";
    CHECK(ProbeTerminalCapabilities(env).osc52.state == CapState::Yes);

    env.term_program.clear();
    env.term = "kitty";
    CHECK(ProbeTerminalCapabilities(env).osc52.state == CapState::Yes);
    CHECK(ProbeTerminalCapabilities(env).bracketed_paste.state == CapState::Yes);
}

TEST_CASE("管道/重定向:VT 判不支持,处境行点名") {
    CapabilityEnv env;
    env.stdin_interactive = false;
    env.stdout_console = false;
    env.ostype = "posix";

    const auto caps = ProbeTerminalCapabilities(env);
    CHECK(caps.vt.state == CapState::No);
    bool saw_no_tty = false;
    for (const auto& row : caps.environment) {
        if (row.key == "caps.env.no_tty") {
            saw_no_tty = true;
        }
    }
    CHECK(saw_no_tty);
}

TEST_CASE("AllRows:七项能力在前,处境行殿后") {
    CapabilityEnv env;
    env.stdin_interactive = true;
    env.vt_enabled = true;
    env.ostype = "posix";
    auto caps = ProbeTerminalCapabilities(env);
    const auto rows = caps.AllRows();
    REQUIRE(rows.size() >= 7);
    CHECK(rows[0].key == std::string("caps.vt"));
    CHECK(rows[6].key == std::string("caps.kitty_keyboard"));
}
