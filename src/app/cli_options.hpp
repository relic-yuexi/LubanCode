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
    // --app-server-profile <部署档.json>(工业化多协议接入单 P1):生产
    // headless 的部署档(deployment schema 1,冻结合同 capability-
    // contract.md §3)。空 = 显式零工具默认档(不照搬终端全部工具,也
    // 不拿空工厂充当已接好)。档解析失败(依赖解释不全/未知键)在
    // RunAppServerMode 启动即拒,不静默落回默认。文件内未点名档名则取
    // service.defaultProfile。
    std::string app_server_profile_path;
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
    // Harbor Harness 派生 JSONL 单:--output <path>——one-shot 收口后把
    // 轨迹导成便携 JSONL 落到用户点名的路径。只在带任务正文的单发模式
    // 生效;其余模式(交互/app-server/子命令)带了它由 RunCli 明报
    // cli.output_mode_mismatch。缺值/空值在解析层退 BadOutput。
    std::string output_path;     // 空 = 没给
    bool output_given = false;
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
    std::string format;             // export/export-workspace 的 --format:training-v1 |
                                    // harness-v1(后者只 export 认,Harbor 派生 JSONL)
    std::string output_path;        // export --format harness-v1 的 --output <path>:导出
                                    // 落点(空 = <session>/exports/harness-v1/trajectory
                                    // .jsonl)。补导路(one-shot 导出失败后按 session id
                                    // 重导)与 Harbor adapter 收尾都用它。
};

// Gateway 子命令(总装单 G1 + V1/V2 job 族 + V4 运维族):`lubancode gateway
// run|status|stop [--profile <名>] [--json 只 status 认]`;`gateway job
// add|run-now|list|read|update|pause|resume|cancel|import-loop` 是持久任务
// 入口(写操作落控制命令文件,活着的 Gateway 消费;list/read 只读账);
// V4 服务安装与常驻运维:`gateway install|uninstall|start|restart|doctor|
// logs`(install 生成平台服务单元并注册,不 start;start/restart 走服务
// 管理器不裸 spawn;doctor 体检 + --wait-ready 健康探针)。
// run 是前台真进程;status/stop/job list/job read/doctor(无 --ack)绝不
// 暗起 Gateway(零副作用合同)。
struct GatewayCliArgs {
    std::string verb;    // run | status | stop | job | install | uninstall |
                         // start | restart | doctor | logs
    std::string profile; // --profile <名>;空 = default
    bool json = false;   // status --json:机器可读快照(job list/read、doctor 也认)
    // job 子族(verb == "job"):写操作落命令文件等消费;list/read 只读
    // automation 账。
    std::string job_verb;             // add | run-now | list | read | update |
                                      // pause | resume | cancel | import-loop
    std::string prompt;               // add/update/import-loop 的任务正文
    std::string job_id;               // add --id / run-now / read / update /
                                      // pause / resume / cancel 位置参数
    std::string idempotency_key;      // --idem(重发同键回原回执)
    long long due_at_ms = 0;          // add --at(0 = 立即)
    // V2 计划与领域操作参数(--every/--cron/--tz/--misfire/--deadline/
    // --heartbeat/--rev/--task)。
    long long interval_seconds = 0;   // --every(add/update/import-loop;秒)
    std::string cron_expr;            // --cron(五字段受限子集)
    std::string timezone;             // --tz(缺省 UTC;显式存储)
    std::string misfire;              // --misfire(coalesce|skip;缺省 coalesce)
    long long deadline_ms = 0;        // --deadline(0 = 不设)
    bool heartbeat = false;           // --heartbeat(notify_on_change)
    long long expected_revision = 0;  // --rev(update/pause/resume/cancel 的 CAS)
    std::string source_session_id;    // import-loop 位置参数(原 /loop 会话)
    std::string source_task_id;       // import-loop --task(原 loop-N)
    // V4 运维族参数。
    int wait_ready_secs = 0;          // doctor --wait-ready <秒>(0 = 不等)
    bool wait_ready_given = false;
    bool ack_safe_mode = false;       // doctor --ack-safe-mode
    int tail_lines = 20;              // logs --tail <行>(缺省 20)
    std::string gateway_root_arg;     // --gateway-root <路径>(install 钉进服务单元;
                                      // 其余 verb 显式指状态根,空 = 默认)
};

// channel status 子命令(连接状态单 §三 P0-A):`lubancode channel status
// <渠道> <账号> [--json]`。跨进程只读——读 Gateway 发布的脱敏连接快照
// (connection-status.json),校验进程存活与快照新鲜度;不把旧快照当在线,
// 不凭 PID 宣告成功。在线(connected=true)退 0,其余非零。
struct ChannelStatusCliArgs {
    std::string channel_id;  // 目标渠道(如 qqbot)
    std::string account_id;  // 目标账号(如 main)
    bool json = false;       // --json:stdout 吐快照 + verdict
};

// `lubancode channel setup <平台> [--account <账号>]`(QQBot Windows 修复单
// §5.1):交互式渠道配置向导。AppSecret 走隐藏输入,绝不收命令行明文
// secret——这里没有任何 --secret 旗标。
struct ChannelCliArgs {
    std::string verb;     // 只认 setup
    std::string platform; // qqbot
    std::string account;  // --account <名>;空 = main
    bool permissions_only = false;
};

// `lubancode channel pairing approve|reject <渠道> <账号> <配对码或身份>
// [--profile <名>] [--timeout <秒>]`(QQ 接入单 Q1b):本地批准控制入口。
// 命令经 Gateway 文件控制面送达持锁实例——普通交互进程不拿空 manager
// 冒充批准成功(无持锁实例时明确报错指引)。
struct ChannelPairingCliArgs {
    std::string action;      // approve | reject
    std::string channel_id;  // qqbot
    std::string account_id;  // main
    std::string token;       // 配对码或 sender 身份
    std::string profile;     // --profile;空 = default
    int timeout_ms = 10'000; // --timeout <秒>
};

// `lubancode im [--select] [平台] [--account <账号>] [--profile <名>]` 与
// `lubancode im setup [平台] [--account <账号>]`(§六 6.1)。im 是日常 IM
// 入口;im setup 只进配置管理,不启动。
struct ImCliArgs {
    bool setup = false;      // im setup
    bool select = false;     // --select:强制开选择列表
    std::string platform;    // 位置参数;空 = 不指定
    std::string account;     // --account;空 = 不指定
    std::string profile;     // --profile;空 = default
};

// assistant 子命令(常驻助理 Web 主界面单 W1):`lubancode assistant
// [--no-open] [--port N] [--profile <名>]`。前台进程起本地 Web 服务(只绑
// 127.0.0.1;端口缺省系统分配,指定端口被占明报不偷换),监听就绪才开
// 浏览器。--no-open 只打印可复制的 URL。重复启动同 profile:验旧实例身份
// 只开其页面,不杀原进程。
struct AssistantCliArgs {
    bool no_open = false;    // --no-open:不起浏览器,只打印 URL
    int port = 0;            // --port N:0 = 系统分配
    bool port_given = false;
    std::string profile;     // --profile <名>;空 = default

};

// LuaHook 单 P1-D 的 hook 子命令:`lubancode hook validate <包目录> [--json]`
// 与 `lubancode hook test <包目录> [--json]`(§8.2 校验与试跑;validate 只跑
// 静态档,test 静态 + fixtures fake 档)。另有 `lubancode hook init <名字>
// [--dir <父目录>]`:落官方 scaffold(带 fixtures 的可跑样例)。退出码
// 0 全过 / 1 有 fail / 2 包读不到或用法不对。报告四档分账(静态过/fake
// 过/真实集成过/未验),不为校验默认访问外部服务或写业务文件。
struct HookCliArgs {
    std::string verb;          // validate | test | init
    std::string package_dir;   // validate/test 的包目录
    std::string name;          // init 的 hook 名(validate/test 不用)
    std::string parent_dir;    // init --dir <父目录>;空 = 当前目录
    bool json = false;         // --json:stdout 吐 JSON 报告
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
    BadAppServerProfile,      // --app-server-profile 缺值/空值:人话在 error_text(P1 部署档)
    RunEvolveTest,            // evolve test 子命令:跑候选评测后退(自进化阶段 3)
    BadEvolveTest,            // evolve test 参数不对:人话已塞进 error_text
    RunTrajectory,            // trajectory 子命令:verify/replay/harness-replay 后退(P0-3)
    BadTrajectory,            // trajectory 参数不对:人话已塞进 error_text
    BadOutput,                // --output 缺值/空值:人话已塞进 error_text(Harbor JSONL 单)
    RunGateway,               // gateway 子命令:run/status/stop(总装单 G1)
    BadGateway,               // gateway 参数不对:人话已塞进 error_text
    RunChannelStatus,         // channel status 子命令:跨进程只读连接快照(§三)
    BadChannelStatus,         // channel 子命令参数不对:人话已塞进 error_text
    RunChannelSetup,          // channel setup 子命令:渠道配置向导(§5.1)
    BadChannelSetup,          // channel 参数不对:人话已塞进 error_text
    RunChannelPairing,        // channel pairing approve/reject 子命令(Q1b)
    BadChannelPairing,        // channel pairing 参数不对:人话已塞进 error_text
    RunIm,                    // im 子命令:统一 IM 选择与启动入口(§六)
    BadIm,                    // im 参数不对:人话已塞进 error_text
    RunAssistant,             // assistant 子命令:常驻助理 Web 主界面(W1)
    BadAssistant,             // assistant 子命令参数不对:人话已塞进 error_text
    RunHookValidate,          // hook validate/test 子命令:LuaHook P1-D 校验与试跑
    RunHookInit,              // hook init 子命令:落官方 scaffold
    BadHook,                  // hook 子命令参数不对:人话已塞进 error_text
};

struct ParsedCliArgs {
    CliAction action = CliAction::Proceed;
    CliOptions options;
    PluginInitArgs plugin_init;  // action==RunPluginInit 时有效
    std::string error_text;      // action==BadPluginInit/BadEvolveTest/BadTrajectory 时的人话
    SessionManagementCommand session_command;  // action == ManageSession 时有效
    EvolveTestArgs evolve_test;  // action == RunEvolveTest 时有效
    TrajectoryCliArgs trajectory;  // action == RunTrajectory 时有效
    GatewayCliArgs gateway;  // action == RunGateway 时有效
    ChannelStatusCliArgs channel_status;  // action == RunChannelStatus 时有效
    ChannelCliArgs channel;  // action == RunChannelSetup 时有效
    ChannelPairingCliArgs channel_pairing;  // action == RunChannelPairing 时有效
    ImCliArgs im;            // action == RunIm 时有效
    AssistantCliArgs assistant;  // action == RunAssistant 时有效(W1)
    HookCliArgs hook;        // action == RunHookValidate/RunHookInit 时有效(P1-D)
};

// args[0] 是程序名,实参从 args[1] 起。多个早退参数同时出现时,按扫描
// 次序头一个生效(与旧的就地 return 语义一致)。
ParsedCliArgs ParseCliArgs(const std::vector<std::string>& args);

}  // namespace lubancode::app
