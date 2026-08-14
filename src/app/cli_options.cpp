// cli_options.hpp 的实现:纯扫描,行为与旧 RunCli 内联循环逐字对齐
// (只是把"就地打印退出"换成"交回动作枚举")。

#include "app/cli_options.hpp"

namespace lubancode::app {

ParsedCliArgs ParseCliArgs(const std::vector<std::string>& args) {
    ParsedCliArgs parsed;
    CliOptions& options = parsed.options;
    for (std::size_t i = 1; i < args.size(); ++i) {
        const std::string& arg = args[i];
        if (arg == "--continue") {
            options.continue_last = true;
            continue;
        }
        if (arg == "--version") {
            parsed.action = CliAction::PrintVersion;
            return parsed;
        }
        if (arg == "--check-update") {
            parsed.action = CliAction::CheckUpdate;
            return parsed;
        }
        if (arg == "--help") {
            parsed.action = CliAction::PrintHelp;
            return parsed;
        }
        if (arg == "--yes") {
            options.auto_confirm = true;
            continue;
        }
        if (arg == "--config") {
            options.print_config = true;
            continue;
        }
        if (arg == "--system-prompt") {
            if (i + 1 >= args.size()) {
                parsed.action = CliAction::MissingSystemPromptValue;
                return parsed;
            }
            options.system_prompt_file_arg = args[++i];
            continue;
        }
        if (arg == "--reset-system-prompt") {
            // 跟 /prompt reset 同效,只是不进交互、不二次确认(命令行参数
            // 本身就是明确意图),RunCli 打完结果就退。
            parsed.action = CliAction::ResetSystemPrompt;
            return parsed;
        }
        if (!options.positional.empty()) {
            options.positional += " ";
        }
        options.positional += arg;
    }
    return parsed;
}

}  // namespace lubancode::app
