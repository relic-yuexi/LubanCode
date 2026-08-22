#include "tools/hooks.hpp"


#include "platform/process.hpp"
#include "platform/log_sink.hpp"

namespace lubancode::tools {

using platform::ProcessResult;

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

// 钩子命令走平台默认 shell:Windows 是 cmd.exe(RunShellCommand 内部已把
// 系统 ANSI 代码页的输出转成 UTF-8——钩子没有走 PowerShell 的 UTF-8 输出
// 编码那条路,跟 run_command 工具的 cmd 分支一个待遇);POSIX 是 /bin/sh,
// 输出天然 UTF-8。
ProcessResult ExecuteHookCommand(const std::string& command_utf8,
                                  const std::vector<std::pair<std::string, std::string>>& env, int timeout_ms) {
    return platform::RunShellCommand(command_utf8, timeout_ms, env);
}

void RunSessionHooks(const std::vector<config::HookEntry>& entries, const char* label) {
    for (const auto& entry : entries) {
        const ProcessResult exec = ExecuteHookCommand(entry.command, {}, kDefaultHookTimeoutMs);
        if (exec.spawn_failed) {
            platform::LogSink::Instance().Warn("hooks",
                std::string(label) + " 钩子起不来(" + entry.command + "): " + exec.spawn_error);
        } else if (exec.timed_out) {
            platform::LogSink::Instance().Warn("hooks", std::string(label) + " 钩子超时(" + entry.command + ")");
        } else if (exec.exit_code != 0) {
            platform::LogSink::Instance().Warn("hooks",
                std::string(label) + " 钩子退出码非 0(" + std::to_string(exec.exit_code) + "): " + entry.command);
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
            platform::LogSink::Instance().Warn("hooks",
                "pre_tool 钩子起不来(" + entry.command + "): " + exec.spawn_error);
            continue;
        }
        if (exec.timed_out) {
            platform::LogSink::Instance().Warn("hooks",
                "pre_tool 钩子超时(" + entry.command + "),按放行处理");
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
            platform::LogSink::Instance().Warn("hooks",
                "post_tool 钩子起不来(" + entry.command + "): " + exec.spawn_error);
        } else if (exec.timed_out) {
            platform::LogSink::Instance().Warn("hooks", "post_tool 钩子超时(" + entry.command + ")");
        } else if (exec.exit_code != 0) {
            platform::LogSink::Instance().Warn("hooks",
                "post_tool 钩子退出码非 0(" + std::to_string(exec.exit_code) + "): " + entry.command);
        }
    }
}

void RunSessionStartHooks(const config::HooksConfig& hooks) { RunSessionHooks(hooks.session_start, "session_start"); }
void RunSessionEndHooks(const config::HooksConfig& hooks) { RunSessionHooks(hooks.session_end, "session_end"); }

}  // namespace lubancode::tools
