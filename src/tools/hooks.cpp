#include "tools/hooks.hpp"

#include <iostream>

#include "tools/process_exec.hpp"

namespace lubancode::tools {

namespace {

bool MatcherHits(const std::string& matcher, const std::string& tool_name) {
    return matcher == "*" || matcher == tool_name;
}

// 取一段文本的前几行,拦截说明/警告里带一点上下文又不至于刷屏。
std::string FirstLines(const std::string& text, int max_lines) {
    std::string out;
    int lines = 0;
    std::size_t pos = 0;
    while (pos <= text.size() && lines < max_lines) {
        const std::size_t nl = text.find('\n', pos);
        const std::string line = (nl == std::string::npos) ? text.substr(pos) : text.substr(pos, nl - pos);
        if (!out.empty()) {
            out += "\n";
        }
        out += line;
        ++lines;
        if (nl == std::string::npos) {
            break;
        }
        pos = nl + 1;
    }
    return out;
}

#ifdef _WIN32
ProcessResult ExecuteHookCommand(const std::string& command_utf8,
                                  const std::vector<std::pair<std::string, std::string>>& env, int timeout_ms) {
    const std::wstring cmdline = BuildCmdCommandLine(command_utf8);
    ProcessResult result = RunProcess(cmdline, timeout_ms, env);
    // 钩子直接扔给 cmd.exe(没有走 PowerShell 的 UTF-8 输出编码那条路),
    // 输出跟 run_command 工具的 cmd 分支一样,要按系统 ANSI 代码页转一道。
    result.output = AcpBytesToUtf8(result.output);
    return result;
}
#else
ProcessResult ExecuteHookCommand(const std::string&, const std::vector<std::pair<std::string, std::string>>&, int) {
    ProcessResult result;
    result.spawn_failed = true;
    result.spawn_error = "hooks 眼下只实现了 Windows";
    return result;
}
#endif

void RunSessionHooks(const std::vector<config::HookEntry>& entries, const char* label) {
    for (const auto& entry : entries) {
        const ProcessResult exec = ExecuteHookCommand(entry.command, {}, kDefaultHookTimeoutMs);
        if (exec.spawn_failed) {
            std::cerr << "[hook] " << label << " 钩子起不来(" << entry.command << "): " << exec.spawn_error << "\n";
        } else if (exec.timed_out) {
            std::cerr << "[hook] " << label << " 钩子超时(" << entry.command << ")\n";
        } else if (exec.exit_code != 0) {
            std::cerr << "[hook] " << label << " 钩子退出码非 0(" << exec.exit_code << "): " << entry.command << "\n";
        }
    }
}

}  // namespace

PreToolHookOutcome RunPreToolHooks(const config::HooksConfig& hooks, const std::string& tool_name,
                                    const nlohmann::json& tool_input) {
    PreToolHookOutcome outcome;
    for (const auto& entry : hooks.pre_tool) {
        if (!MatcherHits(entry.matcher, tool_name)) {
            continue;
        }
        const std::vector<std::pair<std::string, std::string>> env = {
            {"LUBAN_TOOL_NAME", tool_name},
            {"LUBAN_TOOL_INPUT", tool_input.dump()},
        };
        const ProcessResult exec = ExecuteHookCommand(entry.command, env, kDefaultHookTimeoutMs);
        if (exec.spawn_failed) {
            std::cerr << "[hook] pre_tool 钩子起不来(" << entry.command << "): " << exec.spawn_error << "\n";
            continue;
        }
        if (exec.timed_out) {
            std::cerr << "[hook] pre_tool 钩子超时(" << entry.command << "),按放行处理\n";
            continue;
        }
        if (exec.exit_code != 0) {
            outcome.intercepted = true;
            outcome.block_message =
                "被 pre_tool 钩子拦截(退出码 " + std::to_string(exec.exit_code) + "): " + FirstLines(exec.output, 5);
            return outcome;
        }
    }
    return outcome;
}

void RunPostToolHooks(const config::HooksConfig& hooks, const std::string& tool_name,
                       const nlohmann::json& tool_input, const Tool::Result& result) {
    for (const auto& entry : hooks.post_tool) {
        if (!MatcherHits(entry.matcher, tool_name)) {
            continue;
        }
        std::string result_snippet = result.content;
        if (result_snippet.size() > 8192) {
            result_snippet.resize(8192);
        }
        const std::vector<std::pair<std::string, std::string>> env = {
            {"LUBAN_TOOL_NAME", tool_name},
            {"LUBAN_TOOL_INPUT", tool_input.dump()},
            {"LUBAN_TOOL_RESULT", result_snippet},
            {"LUBAN_TOOL_IS_ERROR", result.is_error ? "true" : "false"},
        };
        const ProcessResult exec = ExecuteHookCommand(entry.command, env, kDefaultHookTimeoutMs);
        if (exec.spawn_failed) {
            std::cerr << "[hook] post_tool 钩子起不来(" << entry.command << "): " << exec.spawn_error << "\n";
        } else if (exec.timed_out) {
            std::cerr << "[hook] post_tool 钩子超时(" << entry.command << ")\n";
        } else if (exec.exit_code != 0) {
            std::cerr << "[hook] post_tool 钩子退出码非 0(" << exec.exit_code << "): " << entry.command << "\n";
        }
    }
}

void RunSessionStartHooks(const config::HooksConfig& hooks) { RunSessionHooks(hooks.session_start, "session_start"); }
void RunSessionEndHooks(const config::HooksConfig& hooks) { RunSessionHooks(hooks.session_end, "session_end"); }

}  // namespace lubancode::tools
