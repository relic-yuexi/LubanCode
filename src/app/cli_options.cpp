// cli_options.hpp 的实现:纯扫描,行为与旧 RunCli 内联循环逐字对齐
// (只是把"就地打印退出"换成"交回动作枚举")。

#include "app/cli_options.hpp"

namespace lubancode::app {

ParsedCliArgs ParseCliArgs(const std::vector<std::string>& args) {
    ParsedCliArgs parsed;
    CliOptions& options = parsed.options;
    for (std::size_t i = 1; i < args.size(); ++i) {
        const std::string& arg = args[i];
        // app-server 子命令:只认第一个位置参数是裸 "app-server" 的情形
        // (子命令长这样,单子定调)。认到即设旗标;后续参数照旧并进
        // positional(骨架期子命令不带参数,多给的当普通位置参数走
        // 旧路,不拦)。--version 这些早退参数出现在它前面时,扫描次序
        // 头一个生效的旧规矩不变——早退在先就早退,子命令在先就子命令。
        if (arg == "app-server" && options.positional.empty()) {
            options.app_server = true;
            continue;
        }
        // plugin 子命令:`lubancode plugin init <模板> [名字]`。只认第一个
        // 位置参数是裸 "plugin" 的情形;后随参数全部收进 plugin_init,
        // 形状不对当场退(不静默当普通位置参数走单发问句)。
        if (arg == "plugin" && options.positional.empty()) {
            const std::size_t rest = args.size() - i - 1;
            if (rest == 0 || args[i + 1] != "init") {
                parsed.action = CliAction::BadPluginInit;
                parsed.error_text = "用法: lubancode plugin init <python|lua> [插件名]";
                return parsed;
            }
            if (rest < 2) {
                parsed.action = CliAction::BadPluginInit;
                parsed.error_text = "plugin init 缺模板名,用法: lubancode plugin init <python|lua> [插件名]";
                return parsed;
            }
            if (rest > 3) {
                parsed.action = CliAction::BadPluginInit;
                parsed.error_text = "plugin init 参数太多(最多 模板名 + 插件名)";
                return parsed;
            }
            parsed.plugin_init.template_name = args[i + 2];
            parsed.plugin_init.plugin_name = rest == 3 ? args[i + 3] : args[i + 2];
            parsed.action = CliAction::RunPluginInit;
            return parsed;
        }
        // 会话管理子命令(会话管理器单第四、五步):archive/unarchive/delete。
        // 只认裸词打头、且此前没有位置参数(与 app-server 同规矩)。格式:
        //   lubancode archive <SESSION> [--force 只 delete 认]
        // 其余参数照旧并进 positional(不该有的参数不吞,老路兜底)。
        if (options.positional.empty() &&
            (arg == "archive" || arg == "unarchive" || arg == "delete")) {
            SessionManagementCommand cmd;
            cmd.kind = arg == "archive"       ? SessionManagementCommand::Kind::Archive
                       : arg == "unarchive"   ? SessionManagementCommand::Kind::Unarchive
                                              : SessionManagementCommand::Kind::Delete;
            // 后续:引用 + 可选 --force(只在 delete 认;别处给了报用法,
            // 不静默忽略)。
            for (std::size_t j = i + 1; j < args.size(); ++j) {
                if (args[j] == "--force") {
                    cmd.force = true;
                    continue;
                }
                if (!cmd.session_ref.empty()) {
                    cmd.session_ref += " ";
                }
                cmd.session_ref += args[j];
            }
            if (cmd.force && cmd.kind != SessionManagementCommand::Kind::Delete) {
                cmd.session_ref.clear();  // 别的子命令带 --force:按缺参报用法
            }
            parsed.action = CliAction::ManageSession;
            parsed.session_command = cmd;
            return parsed;
        }
        // 自进化闭环阶段 3 的 CI 子命令:luban evolve test <candidate-dir>
        // [--baseline <package-dir>] [--json]。只认第一个位置参数是裸
        // "evolve" 且第二个是 "test" 的情形;参数形状不对当场退用法,
        // 不静默当普通位置参数走单发问句。
        if (arg == "evolve" && options.positional.empty()) {
            const std::size_t rest = args.size() - i - 1;
            if (rest == 0 || args[i + 1] != "test") {
                parsed.action = CliAction::BadEvolveTest;
                parsed.error_text =
                    "用法: lubancode evolve test <候选目录> [--baseline <父包目录>] [--json]";
                return parsed;
            }
            if (rest < 2) {
                parsed.action = CliAction::BadEvolveTest;
                parsed.error_text = "evolve test 缺候选目录路径";
                return parsed;
            }
            EvolveTestArgs evolve;
            evolve.candidate_dir = args[i + 2];
            if (evolve.candidate_dir.rfind("--", 0) == 0) {
                parsed.action = CliAction::BadEvolveTest;
                parsed.error_text = "evolve test 第一个参数须是候选目录,不是旗标: " +
                                    evolve.candidate_dir;
                return parsed;
            }
            for (std::size_t j = i + 3; j < args.size(); ++j) {
                if (args[j] == "--json") {
                    evolve.json = true;
                    continue;
                }
                if (args[j] == "--baseline") {
                    if (j + 1 >= args.size()) {
                        parsed.action = CliAction::BadEvolveTest;
                        parsed.error_text = "--baseline 需要一个父包目录路径";
                        return parsed;
                    }
                    evolve.baseline_dir = args[++j];
                    continue;
                }
                parsed.action = CliAction::BadEvolveTest;
                parsed.error_text = "evolve test 认不得参数 \"" + args[j] +
                                    "\":只认 <候选目录> --baseline <父包目录> --json";
                return parsed;
            }
            parsed.action = CliAction::RunEvolveTest;
            parsed.evolve_test = evolve;
            return parsed;
        }
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
        if (arg == "--mode") {
            // Plan 模式单:--mode <plan|default>。缺值/认不得当场退——
            // 认不得的值报错,不静默落回 Default(单子:不能让用户误以为
            // 只读保护已经开了)。
            if (i + 1 >= args.size()) {
                parsed.action = CliAction::BadMode;
                parsed.error_text = "--mode 需要一个值:--mode plan 或 --mode default";
                return parsed;
            }
            const std::string& value = args[++i];
            if (value != "plan" && value != "default") {
                parsed.action = CliAction::BadMode;
                parsed.error_text = "--mode 认不得 \"" + value + "\":只认 plan 或 default";
                return parsed;
            }
            options.mode = value;
            options.mode_given = true;
            continue;
        }
        if (arg == "--package-dir") {
            // 统一 Package 封装单:开发调试层,可重复。缺值当场退——
            // 静默吞掉一个空目录会把"想挂的没挂上"藏到 /package list 里。
            if (i + 1 >= args.size()) {
                parsed.action = CliAction::BadPackageDir;
                parsed.error_text = "--package-dir 需要一个目录路径(可重复)";
                return parsed;
            }
            options.package_dirs.push_back(args[++i]);
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
