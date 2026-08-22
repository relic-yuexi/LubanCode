// 命令行参数的纯解析:只扫 argv、只攒结构,不读配置、不探终端、不打印、
// 不碰环境变量。早退动作(--version/--help/--check-update/
// --reset-system-prompt、--system-prompt 缺值)以枚举交回,由 RunCli 决定
// 怎么兑现;参数冲突与缺值的直接测试就钉在这层。
#pragma once

#include <string>
#include <vector>

namespace lubancode::app {

// 正常启动路径需要的参数(早退动作之外的全部)。
struct CliOptions {
    std::string positional;  // 位置参数按出现次序空格拼起来的单发问题
    bool auto_confirm = false;      // --yes
    bool print_config = false;      // --config
    bool continue_last = false;     // --continue
    bool app_server = false;        // app-server 子命令:无界面后台协议(stdio)
    std::string system_prompt_file_arg;  // --system-prompt <文件>(空 = 没给)
};

// 解析结果:action 不是 Proceed 时,RunCli 兑现完动作就退,不进会话。
enum class CliAction {
    Proceed,                  // 正常路径:按 options 继续启动
    RunAppServer,             // app-server 子命令:stdio 后台协议主循环
    PrintVersion,             // --version
    PrintHelp,                // --help
    CheckUpdate,              // --check-update
    ResetSystemPrompt,        // --reset-system-prompt
    MissingSystemPromptValue, // --system-prompt 没带值:报错退 1
};

struct ParsedCliArgs {
    CliAction action = CliAction::Proceed;
    CliOptions options;
};

// args[0] 是程序名,实参从 args[1] 起。多个早退参数同时出现时,按扫描
// 次序头一个生效(与旧的就地 return 语义一致)。
ParsedCliArgs ParseCliArgs(const std::vector<std::string>& args);

}  // namespace lubancode::app
