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
    // WS 承载(多前端外壳单阶段 A):--app-server-ws <port | host:port>,
    // 只在 app-server 子命令下有效(RunCli 守)。空 = stdio 承载。裸端口
    // 绑回环;host:port 显式给非回环地址须配 token(装配层拒)。
    std::string app_server_ws_bind;
    // --app-server-ws-token <token>:显式 token,启用首帧门;不 给 则 看
    // LUBANCODE_APPSERVER_TOKEN(装配层)。token 不进任何日志。
    std::string app_server_ws_token;
    std::string system_prompt_file_arg;  // --system-prompt <文件>(空 = 没给)
    // Plan 模式单:--mode plan(只认 "plan";"default" 等价没给)。非法值
    // 在解析层就退 BadMode——认不得的值报错,不静默落回 Default(单子:
    // "不能安静落回 Default,让用户误以为只读保护已经开了")。
    std::string mode;               // "--mode <plan|default>";空 = 没给
    bool mode_given = false;
    // 统一 Package 封装单:--package-dir <path>(可重复)——开发调试层,
    // 目录下每个直接子目录是一只 Package,优先级最高(dev > project >
    // user > official)。只喂给 /package 的只读面,不挂任何组件。
    std::vector<std::string> package_dirs;
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

// 自进化闭环阶段 3 的 CI 子命令:`luban evolve test <candidate-dir>
// [--baseline <package-dir>] [--json]`。非交互:stdout 吐 JSON(--json 时)
// 或人话,退出码按结果(全过 0 / 有 fail 1 / 夹具缺失 2)。评测引擎与
// /evolve test 同一枚 EvolutionCoordinator。
struct EvolveTestArgs {
    std::string candidate_dir;  // 候选目录(<root>/<package-id>/<candidate-id>)
    std::string baseline_dir;   // --baseline <package-dir>;空 = 按计划的 baseline 节走
    bool json = false;          // --json:stdout 吐 JSON(结果逐项+汇总+unverified)
};

// P0-3 轨迹子命令:`lubancode trajectory <verify|replay|harness-replay>
// <session-id>`。只读诊断,不进会话;退出码 0/1/2(过/用法/验账未过)。
struct TrajectoryCliArgs {
    std::string verb;        // verify | replay | harness-replay | usage | gc | doctor |
                             // export | export-workspace
    std::string session_id;  // trajectory session id(usage/gc/doctor/export-workspace 档当
                             // workspace-key)
    bool gc_derived_only = false;  // gc --derived-only:真删可重建/派生物(默认 dry-run)
    std::string format;             // export/export-workspace 的 --format;唯一认 training-v1
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
    BadMode,                  // --mode 认不得:人话已塞进 error_text(Plan 单)
    BadPackageDir,            // --package-dir 缺值:人话已塞进 error_text(Package 单)
    BadAppServerWs,           // --app-server-ws[-token] 参数不对:人话在 error_text(WS 承载单)
    RunEvolveTest,            // evolve test 子命令:跑候选评测后退(自进化阶段 3)
    BadEvolveTest,            // evolve test 参数不对:人话已塞进 error_text
    RunTrajectory,            // trajectory 子命令:verify/replay/harness-replay 后退(P0-3)
    BadTrajectory,            // trajectory 参数不对:人话已塞进 error_text
};

struct ParsedCliArgs {
    CliAction action = CliAction::Proceed;
    CliOptions options;
    PluginInitArgs plugin_init;  // action==RunPluginInit 时有效
    std::string error_text;      // action==BadPluginInit/BadEvolveTest/BadTrajectory 时的人话
    SessionManagementCommand session_command;  // action == ManageSession 时有效
    EvolveTestArgs evolve_test;  // action == RunEvolveTest 时有效
    TrajectoryCliArgs trajectory;  // action == RunTrajectory 时有效
};

// args[0] 是程序名,实参从 args[1] 起。多个早退参数同时出现时,按扫描
// 次序头一个生效(与旧的就地 return 语义一致)。
ParsedCliArgs ParseCliArgs(const std::vector<std::string>& args);

}  // namespace lubancode::app
