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

// `lubancode plugin init <模板> [名字]` 子命令(plugins 单第 3 步:Python
// scaffold)。模板 v1 只认 "python";名字缺省取模板名。落盘动作交 RunCli
// 里的 HandlePluginInitCommand(纯解析不碰文件系统)。
struct PluginInitArgs {
    std::string template_name;  // "python"
    std::string plugin_name;    // 缺省 = 模板名;须过 IsValidPluginIdentifier
};

// 会话管理子命令(archive/unarchive/delete):session 引用是 id(完整或
// 唯一前缀)或标题;delete 的 --force 只给脚本显式使用(帮助里写明
// 不可恢复),交互终端缺它必走确认。
struct SessionManagementCommand {
    enum class Kind { Archive, Unarchive, Delete };
    Kind kind = Kind::Archive;
    std::string session_ref;  // 会话引用;空 = 缺参(报用法)
    bool force = false;       // delete --force:跳过确认(脚本用)
};

// 解析结果:action 不是 Proceed 时,RunCli 兑现完动作就退,不进会话。
enum class CliAction {
    Proceed,                  // 正常路径:按 options 继续启动
    RunAppServer,             // app-server 子命令:stdio 后台协议主循环
    RunPluginInit,            // plugin init 子命令:生成插件脚手架后退出
    PrintVersion,             // --version
    PrintHelp,                // --help
    CheckUpdate,              // --check-update
    ResetSystemPrompt,        // --reset-system-prompt
    MissingSystemPromptValue, // --system-prompt 没带值:报错退 1
    BadPluginInit,            // plugin init 的参数不对:人话已塞进 error_text
    ManageSession,            // archive/unarchive/delete 子命令
};

struct ParsedCliArgs {
    CliAction action = CliAction::Proceed;
    CliOptions options;
    PluginInitArgs plugin_init;  // action==RunPluginInit 时有效
    std::string error_text;      // action==BadPluginInit 时的人话
    SessionManagementCommand session_command;  // action == ManageSession 时有效
};

// args[0] 是程序名,实参从 args[1] 起。多个早退参数同时出现时,按扫描
// 次序头一个生效(与旧的就地 return 语义一致)。
ParsedCliArgs ParseCliArgs(const std::vector<std::string>& args);

}  // namespace lubancode::app
