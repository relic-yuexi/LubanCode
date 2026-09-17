// cli_options.hpp 的实现:纯扫描,行为与旧 RunCli 内联循环逐字对齐
// (只是把"就地打印退出"换成"交回动作枚举")。

#include "app/cli_options.hpp"

#include <cstdlib>
#include <set>
#include <string>

#include "gateway/profile.hpp"

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
        // P0-3 轨迹子命令:lubancode trajectory <verify|replay|harness-replay>
        // <session-id>;P0-4 增 usage|gc|doctor <workspace-key> 与 gc 的
        // --dry-run/--derived-only;P0-5 增 export/export-workspace 与
        // --format training-v1;Harbor Harness 派生 JSONL 单增 export 的
        // --format harness-v1 与 --output <path>(补导路)。只认裸词打头且
        // 此前没有位置参数;形状不对当场退用法。
        if (arg == "trajectory" && options.positional.empty()) {
            static const std::set<std::string> kVerbs = {
                "verify", "replay", "harness-replay", "usage",
                "gc",     "doctor", "export",         "export-workspace"};
            const std::size_t rest = args.size() - i - 1;
            if (rest == 0 || kVerbs.count(args[i + 1]) == 0) {
                parsed.action = CliAction::BadTrajectory;
                parsed.error_text =
                    "用法: lubancode trajectory "
                    "<verify|replay|harness-replay|usage|gc|doctor|export|export-workspace> "
                    "<session-id|workspace-key>";
                return parsed;
            }
            if (rest < 2) {
                parsed.action = CliAction::BadTrajectory;
                parsed.error_text = "trajectory " + args[i + 1] + " 缺 id";
                return parsed;
            }
            TrajectoryCliArgs trajectory;
            trajectory.verb = args[i + 1];
            trajectory.session_id = args[i + 2];
            trajectory.format = "training-v1";  // 缺省格式(§十四)
            // 修饰词只能跟在 id 之后:gc 的 --dry-run/--derived-only 与
            // export 的 --format <名> / --output <path>(harness-v1 专用)。
            for (std::size_t extra = i + 3; extra < args.size(); ++extra) {
                if (args[extra] == "--dry-run") {
                    continue;  // 默认档,明写也认
                }
                if (args[extra] == "--derived-only") {
                    trajectory.gc_derived_only = true;
                    continue;
                }
                if (args[extra] == "--format") {
                    if (extra + 1 >= args.size()) {
                        parsed.action = CliAction::BadTrajectory;
                        parsed.error_text = "trajectory " + trajectory.verb +
                                            " 的 --format 需要一个值(training-v1 | harness-v1)";
                        return parsed;
                    }
                    trajectory.format = args[++extra];
                    continue;
                }
                if (args[extra] == "--output") {
                    if (extra + 1 >= args.size() || args[extra + 1].empty()) {
                        parsed.action = CliAction::BadTrajectory;
                        parsed.error_text =
                            "trajectory export --format harness-v1 的 --output 需要一个文件路径";
                        return parsed;
                    }
                    trajectory.output_path = args[++extra];
                    continue;
                }
                parsed.action = CliAction::BadTrajectory;
                parsed.error_text = "trajectory " + trajectory.verb + " 认不得参数 \"" + args[extra] +
                                    "\":只认 --dry-run / --derived-only / --format <名> / --output <路径>";
                return parsed;
            }
            if (trajectory.verb == "export") {
                if (trajectory.format != "training-v1" && trajectory.format != "harness-v1") {
                    parsed.action = CliAction::BadTrajectory;
                    parsed.error_text = "trajectory export 只认 --format training-v1 或 "
                                        "harness-v1,不认 \"" +
                                        trajectory.format + "\"";
                    return parsed;
                }
                if (!trajectory.output_path.empty() && trajectory.format != "harness-v1") {
                    parsed.action = CliAction::BadTrajectory;
                    parsed.error_text = "--output 只在 --format harness-v1 下有效";
                    return parsed;
                }
            } else {
                if (trajectory.format != "training-v1") {
                    parsed.action = CliAction::BadTrajectory;
                    parsed.error_text =
                        "trajectory " + trajectory.verb + " 只认 --format training-v1,不认 \"" +
                        trajectory.format + "\"";
                    return parsed;
                }
                if (!trajectory.output_path.empty()) {
                    parsed.action = CliAction::BadTrajectory;
                    parsed.error_text = "--output 只在 trajectory export --format harness-v1 下有效";
                    return parsed;
                }
            }
            if (trajectory.session_id.rfind("-", 0) != 0 &&
                trajectory.session_id.find("/") == std::string::npos &&
                trajectory.session_id.find("..") == std::string::npos &&
                trajectory.session_id.find("\\") == std::string::npos) {
                parsed.action = CliAction::RunTrajectory;
                parsed.trajectory = trajectory;
                return parsed;
            }
            parsed.action = CliAction::BadTrajectory;
            parsed.error_text = "trajectory 的 id 须是单段名(不带路径): " +
                                trajectory.session_id;
            return parsed;
        }
        // Gateway 子命令(总装单 V0 起,V1 加 job 族,V4 加运维族):lubancode
        // gateway <run|status|stop|job|install|uninstall|start|restart|doctor|
        // logs ...> [--profile <名>]。只认裸词打头且此前没有位置参数;形状
        // 不对当场退用法,不静默当普通位置参数走单发问句。
        if (arg == "gateway" && options.positional.empty()) {
            static const std::set<std::string> kVerbs = {
                "run", "status", "stop", "job", "install", "uninstall",
                "start", "restart", "doctor", "logs"};
            const std::size_t rest = args.size() - i - 1;
            if (rest == 0 || kVerbs.count(args[i + 1]) == 0) {
                parsed.action = CliAction::BadGateway;
                parsed.error_text =
                    "用法: lubancode gateway <run|status|stop|job ...|install|uninstall|"
                    "start|restart|doctor|logs> [--profile <名>]";
                return parsed;
            }
            GatewayCliArgs gateway;
            gateway.verb = args[i + 1];
            std::size_t extra = i + 2;
            if (gateway.verb == "job") {
                // gateway job add "<prompt>" [--at ms|--every 秒|--cron 表达式]
                //     [--tz 名] [--misfire coalesce|skip] [--deadline ms]
                //     [--heartbeat] [--id 名] [--idem 键]
                // gateway job run-now <jobId> [--idem 键]
                // gateway job list / read <jobId>
                // gateway job update <jobId> --rev N [--prompt "正文"]
                //     [--every 秒|--cron 表达式|--at ms] [--tz|--misfire|--deadline]
                // gateway job pause|resume|cancel <jobId> --rev N
                // gateway job import-loop <sessionId> "<prompt>" --task loop-N
                //     --every 秒 [--idem 键]
                if (extra >= args.size()) {
                    parsed.action = CliAction::BadGateway;
                    parsed.error_text =
                        "用法: lubancode gateway job <add \"正文\"|run-now <jobId>|list|"
                        "read <jobId>|update <jobId> --rev N|pause <jobId> --rev N|"
                        "resume <jobId> --rev N|cancel <jobId> --rev N|"
                        "import-loop <会话id> \"正文\" --task loop-N --every 秒>";
                    return parsed;
                }
                gateway.job_verb = args[extra++];
                if (gateway.job_verb == "add") {
                    if (extra >= args.size()) {
                        parsed.action = CliAction::BadGateway;
                        parsed.error_text = "gateway job add 需要任务正文(引号包住)";
                        return parsed;
                    }
                    gateway.prompt = args[extra++];
                } else if (gateway.job_verb == "run-now" || gateway.job_verb == "read" ||
                           gateway.job_verb == "update" || gateway.job_verb == "pause" ||
                           gateway.job_verb == "resume" || gateway.job_verb == "cancel") {
                    if (extra >= args.size()) {
                        parsed.action = CliAction::BadGateway;
                        parsed.error_text =
                            "gateway job " + gateway.job_verb + " 需要任务 id";
                        return parsed;
                    }
                    gateway.job_id = args[extra++];
                } else if (gateway.job_verb == "import-loop") {
                    if (extra + 1 >= args.size()) {
                        parsed.action = CliAction::BadGateway;
                        parsed.error_text =
                            "gateway job import-loop 需要 <来源会话id> 与 \"任务正文\" 两个参数";
                        return parsed;
                    }
                    gateway.source_session_id = args[extra++];
                    gateway.prompt = args[extra++];
                } else if (gateway.job_verb != "list") {
                    parsed.action = CliAction::BadGateway;
                    parsed.error_text =
                        "gateway job 认不得子命令 \"" + gateway.job_verb +
                        "\":只认 add|run-now|list|read|update|pause|resume|cancel|import-loop";
                    return parsed;
                }
            }
            for (; extra < args.size(); ++extra) {
                if (args[extra] == "--json") {
                    if (gateway.verb != "status" && gateway.verb != "doctor" &&
                        !(gateway.verb == "job" &&
                          (gateway.job_verb == "list" || gateway.job_verb == "read"))) {
                        parsed.action = CliAction::BadGateway;
                        parsed.error_text =
                            "--json 只在 gateway status / doctor / job list / job read 下有效";
                        return parsed;
                    }
                    gateway.json = true;
                    continue;
                }
                if (args[extra] == "--profile") {
                    if (extra + 1 >= args.size()) {
                        parsed.action = CliAction::BadGateway;
                        parsed.error_text = "--profile 需要一个名字(单段名,如 default)";
                        return parsed;
                    }
                    gateway.profile = args[++extra];
                    continue;
                }
                if (args[extra] == "--gateway-root") {
                    if (extra + 1 >= args.size() || args[extra + 1].empty()) {
                        parsed.action = CliAction::BadGateway;
                        parsed.error_text =
                            "--gateway-root 需要一个绝对路径(install 会把它钉进服务单元)";
                        return parsed;
                    }
                    // 绝对路径轻校验(POSIX 首字符 /;Windows 盘符 X:)——
                    // 相对路径钉进服务单元后,服务换工作目录就找不到状态根。
                    const std::string& value = args[extra + 1];
                    const bool absolute =
                        (!value.empty() && (value[0] == '/' || value[0] == '\\')) ||
                        (value.size() >= 2 && value[1] == ':');
                    if (!absolute) {
                        parsed.action = CliAction::BadGateway;
                        parsed.error_text =
                            "--gateway-root 须是绝对路径(服务单元钉死用,相对路径换目录即失效): " +
                            value;
                        return parsed;
                    }
                    gateway.gateway_root_arg = value;
                    ++extra;  // 消费值参(同 --profile 惯例),别让下轮再扫它
                    continue;
                }
                if (gateway.verb == "doctor" && args[extra] == "--wait-ready") {
                    if (extra + 1 >= args.size()) {
                        parsed.action = CliAction::BadGateway;
                        parsed.error_text = "--wait-ready 需要秒数(如 30)";
                        return parsed;
                    }
                    gateway.wait_ready_secs = std::atoi(args[++extra].c_str());
                    gateway.wait_ready_given = true;
                    if (gateway.wait_ready_secs < 0) {
                        parsed.action = CliAction::BadGateway;
                        parsed.error_text = "--wait-ready 秒数不能为负";
                        return parsed;
                    }
                    continue;
                }
                if (gateway.verb == "doctor" && args[extra] == "--ack-safe-mode") {
                    gateway.ack_safe_mode = true;
                    continue;
                }
                if (gateway.verb == "logs" && args[extra] == "--tail") {
                    if (extra + 1 >= args.size()) {
                        parsed.action = CliAction::BadGateway;
                        parsed.error_text = "--tail 需要行数(如 50)";
                        return parsed;
                    }
                    gateway.tail_lines = std::atoi(args[++extra].c_str());
                    if (gateway.tail_lines <= 0) {
                        parsed.action = CliAction::BadGateway;
                        parsed.error_text = "--tail 行数须为正";
                        return parsed;
                    }
                    continue;
                }
                if (gateway.verb == "job" && args[extra] == "--at") {
                    if (extra + 1 >= args.size()) {
                        parsed.action = CliAction::BadGateway;
                        parsed.error_text = "--at 需要毫秒时间戳";
                        return parsed;
                    }
                    gateway.due_at_ms = std::atoll(args[++extra].c_str());
                    continue;
                }
                if (gateway.verb == "job" && args[extra] == "--every") {
                    if (extra + 1 >= args.size()) {
                        parsed.action = CliAction::BadGateway;
                        parsed.error_text = "--every 需要周期秒数(>= 1)";
                        return parsed;
                    }
                    gateway.interval_seconds = std::atoll(args[++extra].c_str());
                    continue;
                }
                if (gateway.verb == "job" && args[extra] == "--cron") {
                    if (extra + 1 >= args.size()) {
                        parsed.action = CliAction::BadGateway;
                        parsed.error_text =
                            "--cron 需要五字段表达式(引号包住,如 \"0 9 * * 1-5\")";
                        return parsed;
                    }
                    gateway.cron_expr = args[++extra];
                    continue;
                }
                if (gateway.verb == "job" && args[extra] == "--tz") {
                    if (extra + 1 >= args.size()) {
                        parsed.action = CliAction::BadGateway;
                        parsed.error_text = "--tz 需要时区名(如 UTC / Asia/Shanghai)";
                        return parsed;
                    }
                    gateway.timezone = args[++extra];
                    continue;
                }
                if (gateway.verb == "job" && args[extra] == "--misfire") {
                    if (extra + 1 >= args.size() ||
                        (args[extra + 1] != "coalesce" && args[extra + 1] != "skip")) {
                        parsed.action = CliAction::BadGateway;
                        parsed.error_text = "--misfire 只认 coalesce|skip";
                        return parsed;
                    }
                    gateway.misfire = args[++extra];
                    continue;
                }
                if (gateway.verb == "job" && args[extra] == "--deadline") {
                    if (extra + 1 >= args.size()) {
                        parsed.action = CliAction::BadGateway;
                        parsed.error_text = "--deadline 需要毫秒时间戳";
                        return parsed;
                    }
                    gateway.deadline_ms = std::atoll(args[++extra].c_str());
                    continue;
                }
                if (gateway.verb == "job" && args[extra] == "--heartbeat") {
                    gateway.heartbeat = true;
                    continue;
                }
                if (gateway.verb == "job" && args[extra] == "--rev") {
                    if (extra + 1 >= args.size()) {
                        parsed.action = CliAction::BadGateway;
                        parsed.error_text = "--rev 需要 expectedRevision(领域操作的 CAS)";
                        return parsed;
                    }
                    gateway.expected_revision = std::atoll(args[++extra].c_str());
                    continue;
                }
                if (gateway.verb == "job" && args[extra] == "--task") {
                    if (extra + 1 >= args.size()) {
                        parsed.action = CliAction::BadGateway;
                        parsed.error_text = "--task 需要原 /loop 任务名(如 loop-2)";
                        return parsed;
                    }
                    gateway.source_task_id = args[++extra];
                    continue;
                }
                if (gateway.verb == "job" && args[extra] == "--prompt") {
                    if (extra + 1 >= args.size()) {
                        parsed.action = CliAction::BadGateway;
                        parsed.error_text = "--prompt 需要任务正文(引号包住)";
                        return parsed;
                    }
                    gateway.prompt = args[++extra];
                    continue;
                }
                if (gateway.verb == "job" && args[extra] == "--id") {
                    if (extra + 1 >= args.size()) {
                        parsed.action = CliAction::BadGateway;
                        parsed.error_text = "--id 需要任务名";
                        return parsed;
                    }
                    gateway.job_id = args[++extra];
                    continue;
                }
                if (gateway.verb == "job" && args[extra] == "--idem") {
                    if (extra + 1 >= args.size()) {
                        parsed.action = CliAction::BadGateway;
                        parsed.error_text = "--idem 需要幂等键";
                        return parsed;
                    }
                    gateway.idempotency_key = args[++extra];
                    continue;
                }
                parsed.action = CliAction::BadGateway;
                parsed.error_text =
                    "gateway " + gateway.verb + " 认不得参数 \"" + args[extra] +
                    "\":只认 --profile <名> --json --gateway-root <路径>(job 族另认 "
                    "--at/--every/--cron/--tz/--misfire/--deadline/--heartbeat/--rev/--task/"
                    "--prompt/--id/--idem;doctor 另认 --wait-ready/--ack-safe-mode;logs 另认 "
                    "--tail)";
                return parsed;
            }
            // 写操作族的前置形状检查(细校验归活 Gateway 的账面;这里只挡
            // 明显缺参:CAS 必须显式、import-loop 必须带来源与周期)。
            if (gateway.verb == "job" &&
                (gateway.job_verb == "update" || gateway.job_verb == "pause" ||
                 gateway.job_verb == "resume" || gateway.job_verb == "cancel") &&
                gateway.expected_revision <= 0) {
                parsed.action = CliAction::BadGateway;
                parsed.error_text = "gateway job " + gateway.job_verb +
                                    " 需要 --rev <expectedRevision>(领域操作必须显式 CAS)";
                return parsed;
            }
            if (gateway.verb == "job" && gateway.job_verb == "import-loop" &&
                (gateway.source_task_id.empty() || gateway.interval_seconds <= 0)) {
                parsed.action = CliAction::BadGateway;
                parsed.error_text =
                    "gateway job import-loop 需要 --task <原任务名> 与 --every <周期秒>";
                return parsed;
            }
            if (!gateway.profile.empty() && !lubancode::gateway::IsValidGatewayProfileName(gateway.profile)) {
                parsed.action = CliAction::BadGateway;
                parsed.error_text = "profile 名须是单段名(不带路径): " + gateway.profile;
                return parsed;
            }
            parsed.action = CliAction::RunGateway;
            parsed.gateway = gateway;
            return parsed;
        }
        // channel 子命令:status(连接状态单 §三 P0-A,跨进程只读连接快照)
        // 与 setup(QQBot Windows 修复单 §5.1,交互式配置向导)。只认裸词
        // 打头且此前没有位置参数;形状不对当场退用法。setup 没有任何
        // --secret/--app-id 旗标——凭据只从向导的隐藏输入或既有
        // secret_file/secret_env 来,不走 argv。
        if (arg == "channel" && options.positional.empty()) {
            const std::size_t rest = args.size() - i - 1;
            if (rest == 0) {
                parsed.action = CliAction::BadChannelSetup;
                parsed.error_text =
                    "用法: lubancode channel <status|setup|pairing> ...("
                    "status <渠道> <账号> [--json] 连接快照;"
                    " setup <平台> [--account <账号>] 配置向导;"
                    " pairing approve|reject <渠道> <账号> <配对码或身份> 配对批准)";
                return parsed;
            }
            if (args[i + 1] == "status") {
                const std::size_t status_rest = args.size() - i - 2;
                if (status_rest < 2) {
                    parsed.action = CliAction::BadChannelStatus;
                    parsed.error_text = "channel status 需要 <渠道> <账号>(如 qqbot main)";
                    return parsed;
                }
                ChannelStatusCliArgs channel_status;
                channel_status.channel_id = args[i + 2];
                channel_status.account_id = args[i + 3];
                for (std::size_t extra = i + 4; extra < args.size(); ++extra) {
                    if (args[extra] == "--json") {
                        channel_status.json = true;
                        continue;
                    }
                    parsed.action = CliAction::BadChannelStatus;
                    parsed.error_text = "channel status 认不得参数 \"" + args[extra] +
                                        "\":只认 --json";
                    return parsed;
                }
                parsed.action = CliAction::RunChannelStatus;
                parsed.channel_status = channel_status;
                return parsed;
            }
            if (args[i + 1] == "setup") {
                if (rest < 2 || args[i + 2].rfind("--", 0) == 0 || args[i + 2].empty()) {
                    parsed.action = CliAction::BadChannelSetup;
                    parsed.error_text = "channel setup 需要一个平台名(如 qqbot): "
                                        "lubancode channel setup qqbot --account main";
                    return parsed;
                }
                ChannelCliArgs channel_args;
                channel_args.verb = "setup";
                channel_args.platform = args[i + 2];
                for (std::size_t extra = i + 3; extra < args.size(); ++extra) {
                    if (args[extra] == "--permissions") {
                        channel_args.permissions_only = true;
                        continue;
                    }
                    if (args[extra] == "--account") {
                        if (extra + 1 >= args.size() || args[extra + 1].empty() ||
                            args[extra + 1].rfind("--", 0) == 0) {
                            parsed.action = CliAction::BadChannelSetup;
                            parsed.error_text = "--account 需要一个账号名(单段名,如 main)";
                            return parsed;
                        }
                        channel_args.account = args[++extra];
                        continue;
                    }
                    parsed.action = CliAction::BadChannelSetup;
                    parsed.error_text = "channel setup 认不得参数 \"" + args[extra] +
                                        "\":只认 --account <账号>、--permissions";
                    return parsed;
                }
                parsed.action = CliAction::RunChannelSetup;
                parsed.channel = channel_args;
                return parsed;
            }
            if (args[i + 1] == "pairing") {
                // Q1b 本地批准控制入口:`lubancode channel pairing
                // approve|reject <渠道> <账号> <配对码或身份>`。token 没有
                // 任何敏感豁免——它本身就是配对码(一次性)或平台身份串。
                if (rest < 5 || args[i + 2] != "approve" && args[i + 2] != "reject") {
                    parsed.action = CliAction::BadChannelPairing;
                    parsed.error_text =
                        "channel pairing 需要 approve|reject <渠道> <账号> <配对码或身份>"
                        "(如 lubancode channel pairing approve qqbot main ABCD2345)";
                    return parsed;
                }
                ChannelPairingCliArgs pairing_args;
                pairing_args.action = args[i + 2];
                pairing_args.channel_id = args[i + 3];
                pairing_args.account_id = args[i + 4];
                pairing_args.token = args[i + 5];
                for (std::size_t extra = i + 6; extra < args.size(); ++extra) {
                    if (args[extra] == "--profile") {
                        if (extra + 1 >= args.size() || args[extra + 1].empty() ||
                            args[extra + 1].rfind("--", 0) == 0) {
                            parsed.action = CliAction::BadChannelPairing;
                            parsed.error_text = "--profile 需要一个 profile 名(单段名)";
                            return parsed;
                        }
                        pairing_args.profile = args[++extra];
                        continue;
                    }
                    if (args[extra] == "--timeout") {
                        if (extra + 1 >= args.size()) {
                            parsed.action = CliAction::BadChannelPairing;
                            parsed.error_text = "--timeout 需要一个秒数(如 --timeout 30)";
                            return parsed;
                        }
                        char* end = nullptr;
                        const long secs = std::strtol(args[extra + 1].c_str(), &end, 10);
                        if (end == nullptr || *end != '\0' || secs <= 0 || secs > 600) {
                            parsed.action = CliAction::BadChannelPairing;
                            parsed.error_text = "--timeout 认不得 \"" + args[extra + 1] +
                                                "\":要 1..600 的秒数";
                            return parsed;
                        }
                        pairing_args.timeout_ms = static_cast<int>(secs) * 1000;
                        ++extra;
                        continue;
                    }
                    parsed.action = CliAction::BadChannelPairing;
                    parsed.error_text = "channel pairing 认不得参数 \"" + args[extra] +
                                        "\":只认 --profile <名> 与 --timeout <秒>";
                    return parsed;
                }
                if (!lubancode::gateway::IsValidGatewayProfileName(pairing_args.profile.empty()
                                                                        ? "default"
                                                                        : pairing_args.profile)) {
                    parsed.action = CliAction::BadChannelPairing;
                    parsed.error_text = "profile 名须是单段名(不带路径): " + pairing_args.profile;
                    return parsed;
                }
                parsed.action = CliAction::RunChannelPairing;
                parsed.channel_pairing = pairing_args;
                return parsed;
            }
            parsed.action = CliAction::BadChannelSetup;
            parsed.error_text = "channel 认不得子命令 \"" + args[i + 1] +
                                "\":只认 status(连接快照)、setup(配置向导)与"
                                " pairing(配对批准)";
            return parsed;
        }
        // im 子命令(§六 6.1):lubancode im [--select] [平台]
        // [--account <账号>] [--profile <名>];lubancode im setup [...] 只进
        // 配置管理。只认裸词打头且此前没有位置参数。
        if (arg == "im" && options.positional.empty()) {
            ImCliArgs im_args;
            std::size_t extra = i + 1;
            if (extra < args.size() && args[extra] == "setup") {
                im_args.setup = true;
                ++extra;
            }
            if (extra < args.size() && args[extra].rfind("--", 0) != 0 &&
                !args[extra].empty()) {
                im_args.platform = args[extra++];  // 位置参数 = 平台名
            }
            for (; extra < args.size(); ++extra) {
                if (args[extra] == "--select") {
                    im_args.select = true;
                    continue;
                }
                if (args[extra] == "--account") {
                    if (extra + 1 >= args.size() || args[extra + 1].empty() ||
                        args[extra + 1].rfind("--", 0) == 0) {
                        parsed.action = CliAction::BadIm;
                        parsed.error_text = "--account 需要一个账号名(单段名,如 main)";
                        return parsed;
                    }
                    im_args.account = args[++extra];
                    continue;
                }
                if (args[extra] == "--profile") {
                    if (extra + 1 >= args.size() || args[extra + 1].empty()) {
                        parsed.action = CliAction::BadIm;
                        parsed.error_text = "--profile 需要一个名字(单段名,如 default)";
                        return parsed;
                    }
                    im_args.profile = args[++extra];
                    continue;
                }
                parsed.action = CliAction::BadIm;
                parsed.error_text =
                    "im 认不得参数 \"" + args[extra] +
                    "\":只认 [setup] [平台] --select --account <账号> --profile <名>";
                return parsed;
            }
            if (im_args.platform.empty() && !im_args.account.empty()) {
                parsed.action = CliAction::BadIm;
                parsed.error_text = "--account 需要搭配平台: lubancode im <平台> --account <账号>";
                return parsed;
            }
            if (!im_args.profile.empty() && !lubancode::gateway::IsValidGatewayProfileName(im_args.profile)) {
                parsed.action = CliAction::BadIm;
                parsed.error_text = "profile 名须是单段名(不带路径): " + im_args.profile;
                return parsed;
            }
            if (im_args.setup && im_args.select) {
                parsed.action = CliAction::BadIm;
                parsed.error_text = "im setup 不认 --select(setup 本来就直接进配置管理)";
                return parsed;
            }
            parsed.action = CliAction::RunIm;
            parsed.im = im_args;
            return parsed;
        }
        // assistant 子命令(常驻助理 Web 主界面单 W1):lubancode assistant
        // [--no-open] [--port N] [--profile <名>]。只认裸词打头且此前没有
        // 位置参数;形状不对当场退用法,不静默当普通位置参数走单发问句。
        if (arg == "assistant" && options.positional.empty()) {
            AssistantCliArgs assistant;
            for (std::size_t extra = i + 1; extra < args.size(); ++extra) {
                if (args[extra] == "--no-open") {
                    assistant.no_open = true;
                    continue;
                }
                if (args[extra] == "--port") {
                    if (extra + 1 >= args.size()) {
                        parsed.action = CliAction::BadAssistant;
                        parsed.error_text = "--port 需要一个端口号(1-65535)";
                        return parsed;
                    }
                    const std::string& port_text = args[++extra];
                    bool port_ok = !port_text.empty() && port_text.size() <= 5;
                    int port = 0;
                    for (const char digit : port_text) {
                        if (digit < '0' || digit > '9') {
                            port_ok = false;
                            break;
                        }
                        port = port * 10 + (digit - '0');
                    }
                    if (port_ok && (port < 1 || port > 65535)) {
                        port_ok = false;
                    }
                    if (!port_ok) {
                        parsed.action = CliAction::BadAssistant;
                        parsed.error_text = "--port 认不得 \"" + port_text +
                                            "\":要 1-65535 的数字端口号";
                        return parsed;
                    }
                    assistant.port = port;
                    assistant.port_given = true;
                    continue;
                }
                if (args[extra] == "--profile") {
                    if (extra + 1 >= args.size() || args[extra + 1].empty()) {
                        parsed.action = CliAction::BadAssistant;
                        parsed.error_text = "--profile 需要一个名字(单段名,如 default)";
                        return parsed;
                    }
                    assistant.profile = args[++extra];
                    continue;
                }
                parsed.action = CliAction::BadAssistant;
                parsed.error_text = "assistant 认不得参数 \"" + args[extra] +
                                    "\":只认 --no-open / --port <端口号> / --profile <名>";
                return parsed;
            }
            parsed.action = CliAction::RunAssistant;
            parsed.assistant = assistant;
            return parsed;
        }
        if (arg == "--continue") {
            options.continue_last = true;
            continue;
        }
        // WS 承载(app-server 子命令的修饰):值是 <port> 或 <host>:<port>。
        // 这里只查形状(端口 1..65535 的数字;host 留给装配层的 bind 去验),
        // "没配 app-server 子命令"的跨参数规矩归 RunCli。
        if (arg == "--app-server-ws") {
            if (i + 1 >= args.size()) {
                parsed.action = CliAction::BadAppServerWs;
                parsed.error_text = "--app-server-ws 需要一个值:<端口> 或 <主机>:<端口>";
                return parsed;
            }
            const std::string& value = args[++i];
            const std::size_t colon = value.rfind(':');
            const std::string port_text =
                colon == std::string::npos ? value : value.substr(colon + 1);
            bool port_ok = !port_text.empty() && port_text.size() <= 5;
            int port = 0;
            for (const char digit : port_text) {
                if (digit < '0' || digit > '9') {
                    port_ok = false;
                    break;
                }
                port = port * 10 + (digit - '0');
            }
            if (port_ok && (port < 1 || port > 65535)) {
                port_ok = false;
            }
            if (colon != std::string::npos && (colon == 0 || colon == value.size() - 1)) {
                port_ok = false; // ":9001" / "host:" 这类半截
            }
            if (!port_ok) {
                parsed.action = CliAction::BadAppServerWs;
                parsed.error_text =
                    "--app-server-ws 认不得 \"" + value + "\":要 <端口> 或 <主机>:<端口>(1-65535)";
                return parsed;
            }
            options.app_server_ws_bind = value;
            continue;
        }
        if (arg == "--app-server-ws-token") {
            if (i + 1 >= args.size()) {
                parsed.action = CliAction::BadAppServerWs;
                parsed.error_text = "--app-server-ws-token 需要一个 token 值";
                return parsed;
            }
            options.app_server_ws_token = args[++i];
            continue;
        }
        // 部署档路径(P1):这里只查"带了值且非空";文件读不读得动、档合
        // 不合法,归 RunAppServerMode 的装配前奏(启动即拒,人话给全)。
        if (arg == "--app-server-profile") {
            if (i + 1 >= args.size() || args[i + 1].empty()) {
                parsed.action = CliAction::BadAppServerProfile;
                parsed.error_text = "--app-server-profile 需要一个部署档 JSON 路径";
                return parsed;
            }
            options.app_server_profile_path = args[++i];
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
        if (arg == "--output") {
            // Harbor Harness 派生 JSONL 单:one-shot 收口后的轨迹导出落点。
            // 缺值/空值当场退;值照单全收(与 --system-prompt 同规矩,下一
            // 参数就是值,不猜它是不是旗标)。模式错配(交互/app-server/
            // 子命令)归 RunCli 的门卫,解析层只管形状。
            if (i + 1 >= args.size() || args[i + 1].empty()) {
                parsed.action = CliAction::BadOutput;
                parsed.error_text =
                    "--output 需要一个文件路径(单发模式专用:lubancode --yes --output "
                    "<文件路径> \"<任务正文>\")";
                return parsed;
            }
            options.output_path = args[++i];
            options.output_given = true;
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
