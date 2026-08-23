// shell 环境探针(进程生命线单 P2"shell 方言、版本、profile 与交互环境
// 须明牌"):run_command 实际唤起的 shell 是什么、版本多少、吃不吃 profile、
// stdin/stdout 是不是 TTY——这些是产品边界,不是 bug,但常被用户当 bug。
// /doctor shell 打印这份账,用户文档同源。
#pragma once

#include <string>
#include <vector>

namespace lubancode::tools {

// 一条 shell 的画像。
struct ShellReport {
    std::string id;            // powershell / cmd / sh
    std::string executable;    // 实际唤起的路径(powershell.exe / cmd.exe / /bin/sh)
    std::string version;       // 版本串(探不到留空)
    bool login_shell = false;  // 是否 login shell(sh -l 才算;我们不带 -l)
    bool profile_loaded = false;  // 是否加载用户 profile(NoProfile -> false)
    bool stdin_is_tty = false;    // 工具路径下 stdin 接 /dev/null 或 NUL,恒 false
    bool stdout_is_tty = false;   // 工具路径下 stdout 是管道,恒 false
    std::string notes;            // 人话备注(dash/bash 之辨、编码、PATH 来源)
};

// 探测本平台 run_command 会用到的 shell(实跑一次 --version 级别的探针,
// 失败如实写进 notes/version,不猜)。阻塞式,给 /doctor 用。
std::vector<ShellReport> ProbeShells();

}  // namespace lubancode::tools
