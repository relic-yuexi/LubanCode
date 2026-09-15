#include "cli/gateway_command.hpp"

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "app/version.hpp"
#include "gateway/automation_store.hpp"
#include "gateway/control_server.hpp"
#include "gateway/doctor.hpp"
#include "gateway/process.hpp"
#include "gateway/service.hpp"
#include "gateway/status.hpp"
#include "gateway/work_pump.hpp"
#include "platform/paths.hpp"
#include "platform/wall_clock.hpp"

namespace lubancode::cli {

namespace {

// gateway 状态根走唯一口 gateway::DefaultGatewayRoot()(状态根/gateway,
// 应用根语义=数据根;个人布局=~/.lubancode/gateway 原样)——run 装配段
// 与本命令族解同一棵 profile 树,不再各拼各的。
std::filesystem::path DefaultGatewayRoot() {
    return gateway::DefaultGatewayRoot();
}

int RunGatewayProcess(const gateway::GatewayProfilePaths& paths, gateway::GatewayWorkPump* pump) {
    // 配置先装载:坏配置在取锁前退稳定码 3(单子 §13.2"坏配置用稳定退出
    // 码,避免无限拉起"),并记一笔 boot(reason=config_invalid)给 doctor。
    const gateway::GatewayConfigLoad config_load = gateway::LoadGatewayConfig(paths.config_file);
    if (config_load.status == gateway::GatewayConfigLoad::Status::Invalid) {
        const std::string record_error =
            gateway::GatewayProcess::RecordConfigInvalidBoot(paths, config_load.error);
        if (!record_error.empty()) {
            std::fprintf(stderr, "[gateway] 记坏配置 boot 失败: %s\n", record_error.c_str());
        }
        std::fprintf(stderr, "[gateway] %s\n", config_load.error.c_str());
        return 3;
    }

    gateway::GatewayProcess::Options options;
    options.paths = paths;
    options.config = config_load.config;
    options.version = std::string(app::kVersion);
    options.pump = pump;  // V1 有界主泵(装配层递进;空 = 无业务面)
    gateway::GatewayProcess process(std::move(options));

    const auto start = process.Start();
    if (start.status == gateway::GatewayProcess::StartResult::Status::AlreadyRunning) {
        std::fprintf(stderr, "[gateway] %s\n", start.detail.c_str());
        return 2;
    }
    if (start.status != gateway::GatewayProcess::StartResult::Status::Started) {
        std::fprintf(stderr, "[gateway] 启动失败: %s\n", start.detail.c_str());
        return 1;
    }
    return process.Run();
}

int PrintGatewayStatus(const gateway::GatewayProfilePaths& paths, bool json) {
    // V1 分栏:process(活探针)与 work/execution/delivery(领域账只读
    // 投影)分开采、分开报——进程 running 不冒充任务成功(单子 V1 第
    // 五件事)。零写盘零建目录不变。
    const gateway::GatewayProbe probe = gateway::ProbeGateway(paths);
    const gateway::GatewayStatusSections sections = gateway::ProbeStatusSections(paths);
    if (json) {
        nlohmann::json json_out = gateway::ProbeToJson(probe);
        const nlohmann::json sections_json = gateway::SectionsToJson(sections);
        json_out["work"] = sections_json["work"];
        json_out["execution"] = sections_json["execution"];
        json_out["delivery"] = sections_json["delivery"];
        std::printf("%s\n", json_out.dump().c_str());
        return probe.state == gateway::GatewayProbe::State::Running ? 0 : 1;
    }
    for (const std::string& line : gateway::FormatProbeLines(probe)) {
        std::printf("%s\n", line.c_str());
    }
    for (const std::string& line : gateway::FormatSectionLines(sections)) {
        std::printf("%s\n", line.c_str());
    }
    return probe.state == gateway::GatewayProbe::State::Running ? 0 : 1;
}

// job 族(V1 add/run-now/list + V2 update/pause/resume/cancel/read/
// import-loop):写操作落控制命令文件(持久任务创建/变更走本地命令,
// 不直接改文件),活着的 Gateway 消费进账;list/read 只读 automation 账。
// 命令落了但 Gateway 没跑:如实说(下次 run 起来消费),不冒充"任务已建"。
namespace {

// V2 计划参数(--every/--cron/--tz/--misfire/--deadline/--heartbeat/--at)
// 折进命令载荷;出现即设。
gateway::GatewayJobSchedulePatch JobSchedulePatchFromArgs(const GatewayCommandArgs& args) {
    gateway::GatewayJobSchedulePatch patch;
    if (args.due_at_ms != 0) {
        patch.set_due_at = true;
        patch.due_at_ms = args.due_at_ms;
    }
    if (args.interval_seconds > 0) {
        patch.set_interval = true;
        patch.interval_seconds = args.interval_seconds;
    }
    if (!args.cron_expr.empty()) {
        patch.set_cron = true;
        patch.cron_expr = args.cron_expr;
    }
    if (!args.timezone.empty()) {
        patch.set_timezone = true;
        patch.timezone = args.timezone;
    }
    if (!args.misfire.empty()) {
        patch.set_misfire = true;
        patch.misfire = args.misfire;
    }
    if (args.deadline_ms > 0) {
        patch.set_deadline = true;
        patch.deadline_ms = args.deadline_ms;
    }
    if (args.heartbeat) {
        patch.set_notify_on_change = true;
        patch.notify_on_change = true;
    }
    return patch;
}

std::string ScheduleSummary(const gateway::AutomationJob& job) {
    switch (job.schedule_kind) {
        case gateway::ScheduleKind::Once:
            return "once due " + std::to_string(job.due_at_ms) + "ms";
        case gateway::ScheduleKind::Interval:
            return "every " + std::to_string(job.interval_seconds) + "s";
        case gateway::ScheduleKind::Cron:
            return "cron \"" + job.cron_expr + "\" @" + job.timezone;
    }
    return "once";
}

nlohmann::json JobToJson(const gateway::AutomationJob& job) {
    nlohmann::json item = nlohmann::json::object();
    item["job_id"] = job.job_id;
    item["state"] = gateway::ToString(job.state);
    item["schedule_kind"] = gateway::ToString(job.schedule_kind);
    item["prompt"] = job.prompt;
    item["revision"] = job.revision;
    item["due_at_ms"] = job.due_at_ms;
    item["interval_seconds"] = job.interval_seconds;
    item["cron_expr"] = job.cron_expr.empty() ? nlohmann::json(nullptr)
                                              : nlohmann::json(job.cron_expr);
    item["timezone"] = job.timezone;
    item["misfire"] = gateway::ToString(job.misfire);
    item["deadline_ms"] = job.deadline_ms;
    item["notify_on_change"] = job.notify_on_change;
    item["imported_from"] = job.imported_from.empty()
                                ? nlohmann::json(nullptr)
                                : nlohmann::json(job.imported_from);
    return item;
}

nlohmann::json OccurrenceToJson(const gateway::AutomationOccurrence& occurrence) {
    nlohmann::json item = nlohmann::json::object();
    item["occurrence_id"] = occurrence.occurrence_id;
    item["job_id"] = occurrence.job_id;
    item["slot_ms"] = occurrence.slot_ms;
    item["reason"] = occurrence.reason;
    item["missed_count"] = occurrence.missed_count;
    item["attempt"] = occurrence.attempt;
    const char* state = occurrence.state == gateway::AutomationOccurrence::State::Scheduled
                            ? "scheduled"
                            : (occurrence.state == gateway::AutomationOccurrence::State::Claimed
                                   ? "claimed"
                                   : "settled");
    item["state"] = state;
    item["outcome"] =
        occurrence.outcome.empty() ? nlohmann::json(nullptr) : nlohmann::json(occurrence.outcome);
    item["detail"] =
        occurrence.detail.empty() ? nlohmann::json(nullptr) : nlohmann::json(occurrence.detail);
    item["session_id"] = occurrence.session_id.empty() ? nlohmann::json(nullptr)
                                                       : nlohmann::json(occurrence.session_id);
    item["turn_id"] =
        occurrence.turn_id.empty() ? nlohmann::json(nullptr) : nlohmann::json(occurrence.turn_id);
    return item;
}

}  // namespace

int RunGatewayJobCommand(const gateway::GatewayProfilePaths& paths, const GatewayCommandArgs& args) {
    const std::int64_t now_ms = platform::WallClockNowMs();
    if (args.job_verb == "add") {
        gateway::GatewayJobAddCommand command;
        command.prompt = args.prompt;
        command.idempotency_key = args.idempotency_key;
        command.job_id = args.job_id;
        command.due_at_ms = args.due_at_ms;
        command.requested_at_ms = now_ms;
        command.schedule = JobSchedulePatchFromArgs(args);
        const std::string error = gateway::WriteJobAddCommand(paths.control_dir, command);
        if (!error.empty()) {
            std::fprintf(stderr, "gateway job add 失败: %s\n", error.c_str());
            return 1;
        }
        std::printf("任务命令已落(等待运行中的 Gateway 消费;没在跑则下次启动时受理)。\n");
        return 0;
    }
    if (args.job_verb == "run-now") {
        gateway::GatewayJobRunNowCommand command;
        command.job_id = args.job_id;
        command.idempotency_key = args.idempotency_key;
        command.requested_at_ms = now_ms;
        const std::string error = gateway::WriteJobRunNowCommand(paths.control_dir, command);
        if (!error.empty()) {
            std::fprintf(stderr, "gateway job run-now 失败: %s\n", error.c_str());
            return 1;
        }
        std::printf("触发命令已落(等待运行中的 Gateway 消费)。\n");
        return 0;
    }
    if (args.job_verb == "update") {
        gateway::GatewayJobUpdateCommand command;
        command.job_id = args.job_id;
        command.expected_revision = static_cast<std::uint64_t>(args.expected_revision);
        command.idempotency_key = args.idempotency_key;
        command.prompt = args.prompt;  // --prompt 出现才有值(空 = 不改)
        command.schedule = JobSchedulePatchFromArgs(args);
        command.requested_at_ms = now_ms;
        const std::string error = gateway::WriteJobUpdateCommand(paths.control_dir, command);
        if (!error.empty()) {
            std::fprintf(stderr, "gateway job update 失败: %s\n", error.c_str());
            return 1;
        }
        std::printf("更新命令已落(等待运行中的 Gateway 消费)。\n");
        return 0;
    }
    if (args.job_verb == "pause" || args.job_verb == "resume" || args.job_verb == "cancel") {
        gateway::GatewayJobStateCommand command;
        command.verb = args.job_verb;
        command.job_id = args.job_id;
        command.expected_revision = static_cast<std::uint64_t>(args.expected_revision);
        command.idempotency_key = args.idempotency_key;
        command.requested_at_ms = now_ms;
        const std::string error = gateway::WriteJobStateCommand(paths.control_dir, command);
        if (!error.empty()) {
            std::fprintf(stderr, "gateway job %s 失败: %s\n", args.job_verb.c_str(),
                         error.c_str());
            return 1;
        }
        std::printf("%s 命令已落(等待运行中的 Gateway 消费)。\n", args.job_verb.c_str());
        return 0;
    }
    if (args.job_verb == "import-loop") {
        gateway::GatewayJobImportLoopCommand command;
        command.source_session_id = args.source_session_id;
        command.source_task_id = args.source_task_id;
        command.prompt = args.prompt;
        command.interval_seconds = args.interval_seconds;
        command.idempotency_key = args.idempotency_key;
        command.requested_at_ms = now_ms;
        const std::string error = gateway::WriteJobImportLoopCommand(paths.control_dir, command);
        if (!error.empty()) {
            std::fprintf(stderr, "gateway job import-loop 失败: %s\n", error.c_str());
            return 1;
        }
        std::printf(
            "导入命令已落(等待运行中的 Gateway 消费;导入即产 receipt,原 /loop 状态只读"
            "留档,不会被暗搬)。\n");
        return 0;
    }
    if (args.job_verb == "read") {
        // read:只读账(零写盘零建目录)。
        const gateway::AutomationProjection projection = gateway::ReadAutomationProjection(
            paths.automation_log);
        std::vector<gateway::AutomationOccurrence> occurrences;
        for (const auto& [id, occurrence] : projection.occurrences) {
            if (occurrence.job_id == args.job_id) {
                occurrences.push_back(occurrence);
            }
        }
        const auto job = projection.jobs.find(args.job_id);
        if (job == projection.jobs.end()) {
            std::printf("任务不存在: %s\n", args.job_id.c_str());
            return 1;
        }
        if (args.json) {
            nlohmann::json json = JobToJson(job->second);
            nlohmann::json occ_json = nlohmann::json::array();
            std::sort(occurrences.begin(), occurrences.end(),
                      [](const gateway::AutomationOccurrence& a,
                         const gateway::AutomationOccurrence& b) {
                          return a.slot_ms < b.slot_ms;
                      });
            for (const auto& occurrence : occurrences) {
                occ_json.push_back(OccurrenceToJson(occurrence));
            }
            json["occurrences"] = std::move(occ_json);
            std::printf("%s\n", json.dump().c_str());
            return 0;
        }
        std::printf("%s\t%s\t%s\trev %llu\n", job->second.job_id.c_str(),
                    gateway::ToString(job->second.state).c_str(),
                    ScheduleSummary(job->second).c_str(),
                    static_cast<unsigned long long>(job->second.revision));
        std::printf("  正文: %s\n", job->second.prompt.c_str());
        if (occurrences.empty()) {
            std::printf("  (尚无 occurrence)\n");
            return 0;
        }
        std::sort(occurrences.begin(), occurrences.end(),
                  [](const gateway::AutomationOccurrence& a,
                     const gateway::AutomationOccurrence& b) { return a.slot_ms < b.slot_ms; });
        for (const auto& occurrence : occurrences) {
            const char* state =
                occurrence.state == gateway::AutomationOccurrence::State::Scheduled
                    ? "scheduled"
                    : (occurrence.state == gateway::AutomationOccurrence::State::Claimed
                           ? "claimed"
                           : "settled");
            std::printf("  %s\tslot %lldms\t%s\tattempt %llu\tmissed %u\t%s\n",
                        occurrence.occurrence_id.c_str(),
                        static_cast<long long>(occurrence.slot_ms), state,
                        static_cast<unsigned long long>(occurrence.attempt),
                        occurrence.missed_count,
                        occurrence.outcome.empty() ? "-" : occurrence.outcome.c_str());
        }
        return 0;
    }
    // list:只读账。
    const gateway::AutomationProjection projection = gateway::ReadAutomationProjection(
        paths.automation_log);
    if (args.json) {
        nlohmann::json json = nlohmann::json::array();
        for (const auto& [id, job] : projection.jobs) {
            json.push_back(JobToJson(job));
        }
        std::printf("%s\n", json.dump().c_str());
        return 0;
    }
    if (projection.jobs.empty()) {
        std::printf("没有持久任务(账为空或尚无账)。\n");
        return 0;
    }
    for (const auto& [id, job] : projection.jobs) {
        std::printf("%s\t%s\t%s\trev %llu\t%s\n", job.job_id.c_str(),
                    gateway::ToString(job.state).c_str(), ScheduleSummary(job).c_str(),
                    static_cast<unsigned long long>(job.revision), job.prompt.c_str());
    }
    return 0;
}

int StopGatewayProcess(const gateway::GatewayProfilePaths& paths,
                       const gateway::GatewayProfileConfig& config) {
    // CLI 等待上限 = 关机宽限 + 5s 观察余量;超时如实报,不代杀(单子
    // §13.2:超时才由 supervisor 收进程组)。
    const int wait_ms = config.shutdown_grace_secs * 1000 + 5000;
    const gateway::GatewayStopOutcome outcome = gateway::StopGateway(paths, wait_ms);
    std::printf("%s\n", outcome.detail.c_str());
    switch (outcome.status) {
        case gateway::GatewayStopOutcome::Status::Stopped:
        case gateway::GatewayStopOutcome::Status::NotRunning:
            return 0;
        case gateway::GatewayStopOutcome::Status::StoppedUnclean:
            return 0;  // 进程已退,非干净关机已如实入账
        case gateway::GatewayStopOutcome::Status::Timeout:
            return 4;
        case gateway::GatewayStopOutcome::Status::Refused:
        case gateway::GatewayStopOutcome::Status::WriteFailed:
            return 1;
    }
    return 1;
}

// ---------------------------------------------------------------------------
// V4 运维族:服务安装与常驻运维(单子 §十 V4)。合同见 gateway_command.hpp
// 头注释与 contracts.md §14;停止语义统一走上面的文件控制面。
// ---------------------------------------------------------------------------

namespace {

// 装配服务定义:exe 用当前进程的可执行文件(服务单元钉绝对路径),状态
// 根钉进 --gateway-root 参数,日志落 profile 树 logs/。拿不到 exe 明错
// ——钉不了路径就不装。
bool BuildGatewayServiceSpec(const gateway::GatewayProfilePaths& paths,
                             const gateway::GatewayProfileConfig& config,
                             gateway::GatewayServiceSpec* spec, std::string* error) {
    const auto exe = platform::ExecutablePath();
    if (!exe.has_value() || exe->empty()) {
        if (error != nullptr) {
            *error = "定位不到当前可执行文件,服务单元钉不了 exe 路径(不装)";
        }
        return false;
    }
    spec->profile = paths.name.empty() ? std::string(gateway::kDefaultGatewayProfile)
                                       : paths.name;
    spec->exe_path = *exe;
    spec->gateway_root = paths.root;
    spec->working_dir = paths.root;
    spec->service_log = paths.logs_dir / "service.log";
    spec->service_err = paths.logs_dir / "service.err.log";
    spec->lubancode_version = std::string(app::kVersion);
    spec->shutdown_grace_secs = config.shutdown_grace_secs;
#ifdef _WIN32
    // LogonTrigger 钉当前用户(DOMAIN\name);拿不全就留空(= 任意用户
    // 登录触发,runbook 写明差异)。
    const auto domain = platform::GetEnvVar("USERDOMAIN");
    const auto user = platform::GetEnvVar("USERNAME");
    if (user.has_value()) {
        spec->user_name = domain.has_value() ? (*domain + "\\" + *user) : *user;
    }
#endif
    return true;
}

int RunGatewayInstall(const gateway::GatewayProfilePaths& paths) {
    // install 前校验配置可装载(可启动 dry 的第一道闸):坏配置拒装,退
    // 稳定码 3(与 run 同码;supervisor 语境下这也是防重启风暴的一半)。
    const gateway::GatewayConfigLoad config_load = gateway::LoadGatewayConfig(paths.config_file);
    if (config_load.status == gateway::GatewayConfigLoad::Status::Invalid) {
        std::fprintf(stderr, "[gateway] 配置坏,拒绝安装(先修 %s):\n%s\n",
                     platform::PathToUtf8(paths.config_file).c_str(),
                     config_load.error.c_str());
        return 3;
    }
    gateway::GatewayServiceSpec spec;
    std::string spec_error;
    if (!BuildGatewayServiceSpec(paths, config_load.config, &spec, &spec_error)) {
        std::fprintf(stderr, "[gateway] %s\n", spec_error.c_str());
        return 1;
    }
    const gateway::ServiceRunner runner = gateway::MakeDefaultServiceRunner();
    const gateway::ServiceInstallOutcome outcome = gateway::InstallGatewayService(
        spec, gateway::CurrentServicePlatform(), paths.profile_dir, runner);
    if (!outcome.op.ok) {
        std::fprintf(stderr, "[gateway] install 失败(%s): %s\n",
                     outcome.op.error_code.c_str(), outcome.op.detail.c_str());
        return 1;
    }
    std::printf("服务已注册(平台 %s;单元 %s;记录 %s)。\n",
                gateway::ServicePlatformName(gateway::CurrentServicePlatform()),
                platform::PathToUtf8(outcome.unit_file).c_str(),
                platform::PathToUtf8(outcome.record_file).c_str());
    std::printf("钉死:exe=%s 参数=gateway run --profile %s --gateway-root %s\n",
                platform::PathToUtf8(spec.exe_path).c_str(), spec.profile.c_str(),
                platform::PathToUtf8(spec.gateway_root).c_str());
    std::printf("下一步:gateway start 拉起;gateway doctor --wait-ready 30 验证。\n");
    // 凭据失效明列(不自动修,向导归 channel setup):跑 doctor 的配置/
    // 凭据面,只打 Warn 及以上,给装机的人当场看见。
    gateway::DoctorOptions doctor_options;
    doctor_options.lubancode_version = std::string(app::kVersion);
    const gateway::DoctorReport report = gateway::RunGatewayDoctor(paths, doctor_options);
    for (const auto& check : report.checks) {
        if (check.severity == gateway::DoctorSeverity::Warn ||
            check.severity == gateway::DoctorSeverity::Fail) {
            std::printf("[装机体检] %s: %s\n", check.code.c_str(), check.detail.c_str());
        }
    }
    return 0;
}

int RunGatewayUninstall(const gateway::GatewayProfilePaths& paths) {
    const gateway::GatewayConfigLoad config_load = gateway::LoadGatewayConfig(paths.config_file);
    gateway::GatewayServiceSpec spec;
    std::string spec_error;
    if (!BuildGatewayServiceSpec(paths, config_load.config, &spec, &spec_error)) {
        std::fprintf(stderr, "[gateway] %s\n", spec_error.c_str());
        return 1;
    }
    // 先文件面 stop(drain 语义与 gateway stop 统一):没停干净不摘,如实
    // 报给人工处置——摘了注册而进程还活着,下次 start 撞锁。
    const int stop_code = StopGatewayProcess(paths, config_load.config);
    if (stop_code != 0) {
        std::fprintf(stderr, "[gateway] 先停干净再 uninstall(上面那条没停净)。\n");
        return stop_code;
    }
    const gateway::ServiceRunner runner = gateway::MakeDefaultServiceRunner();
    const gateway::ServiceOpOutcome outcome = gateway::UninstallGatewayService(
        spec, gateway::CurrentServicePlatform(), paths.profile_dir, runner);
    if (!outcome.ok) {
        std::fprintf(stderr, "[gateway] uninstall 失败(%s): %s\n",
                     outcome.error_code.c_str(), outcome.detail.c_str());
        return 1;
    }
    std::printf("服务已摘除;任务账与 boot history 保留在 %s(不删数据)。\n",
                platform::PathToUtf8(paths.profile_dir).c_str());
    return 0;
}

// start/restart 共用:未安装明错(不裸 spawn——CLI 不另养暗 daemon)。
int RequireServiceInstalled(const gateway::GatewayProfilePaths& paths,
                            const gateway::GatewayProfileConfig& config,
                            gateway::GatewayServiceSpec* spec) {
    std::string spec_error;
    if (!BuildGatewayServiceSpec(paths, config, spec, &spec_error)) {
        std::fprintf(stderr, "[gateway] %s\n", spec_error.c_str());
        return 1;
    }
    const gateway::ServiceRunner runner = gateway::MakeDefaultServiceRunner();
    const gateway::ServiceOpOutcome query =
        gateway::QueryGatewayService(*spec, gateway::CurrentServicePlatform(), runner);
    if (!query.ok) {
        std::fprintf(stderr, "[gateway] service.not_installed: 服务未注册(%s);先 gateway "
                             "install。手动前台跑用 gateway run。\n",
                     query.detail.c_str());
        return 1;
    }
    return 0;
}

int RunGatewayStart(const gateway::GatewayProfilePaths& paths,
                    const gateway::GatewayProfileConfig& config) {
    gateway::GatewayServiceSpec spec;
    const int gate = RequireServiceInstalled(paths, config, &spec);
    if (gate != 0) return gate;
    const gateway::ServiceRunner runner = gateway::MakeDefaultServiceRunner();
    const gateway::ServiceOpOutcome outcome =
        gateway::StartGatewayService(spec, gateway::CurrentServicePlatform(), runner);
    if (!outcome.ok) {
        std::fprintf(stderr, "[gateway] start 失败(%s): %s\n", outcome.error_code.c_str(),
                     outcome.detail.c_str());
        return 1;
    }
    std::printf("已通过服务管理器拉起(平台 %s);gateway doctor --wait-ready 30 可验证。\n",
                gateway::ServicePlatformName(gateway::CurrentServicePlatform()));
    return 0;
}

int RunGatewayRestart(const gateway::GatewayProfilePaths& paths,
                      const gateway::GatewayProfileConfig& config) {
    gateway::GatewayServiceSpec spec;
    const int gate = RequireServiceInstalled(paths, config, &spec);
    if (gate != 0) return gate;
    // 停止语义统一:文件控制面 drain(与 gateway stop 同一条路),收干净
    // 再经服务管理器拉起。重启后先 reconcile 再接新活由泵保证
    //(TickOnce 恢复扫描先于新派发)。
    const int stop_code = StopGatewayProcess(paths, config);
    if (stop_code != 0) {
        std::fprintf(stderr, "[gateway] 没停干净,不再拉起(上面那条如实)。\n");
        return stop_code;
    }
    const gateway::ServiceRunner runner = gateway::MakeDefaultServiceRunner();
    const gateway::ServiceOpOutcome outcome =
        gateway::StartGatewayService(spec, gateway::CurrentServicePlatform(), runner);
    if (!outcome.ok) {
        std::fprintf(stderr, "[gateway] restart 拉起失败(%s): %s\n", outcome.error_code.c_str(),
                     outcome.detail.c_str());
        return 1;
    }
    std::printf("已停净并经服务管理器重新拉起。\n");
    return 0;
}

int RunGatewayDoctor(const gateway::GatewayProfilePaths& paths, const GatewayCommandArgs& args) {
    // SafeMode 显式 ack 先做(contracts §10.3 留给 V4 的口)。
    if (args.ack_safe_mode) {
        const std::string error = gateway::AckSafeMode(paths);
        if (!error.empty()) {
            std::fprintf(stderr, "[gateway] ack 落账失败: %s\n", error.c_str());
            return 1;
        }
        std::printf("已记 ack_safe_mode:SafeMode 连击清零(账上保留人工确认事实)。\n");
    }
    // 健康探针先等(install 后验证/外部监控用);超时如实退 1,不假 ready。
    if (args.wait_ready_secs > 0) {
        const gateway::WaitReadyOutcome wait =
            gateway::WaitForGatewayReady(paths, args.wait_ready_secs, {}, {});
        std::printf("%s\n", wait.detail.c_str());
        if (!wait.ready) {
            return 1;
        }
    }
    gateway::DoctorOptions options;
    options.service_runner = gateway::MakeDefaultServiceRunner();
    options.lubancode_version = std::string(app::kVersion);
    const gateway::DoctorReport report = gateway::RunGatewayDoctor(paths, options);
    if (args.json) {
        std::printf("%s\n", report.ToJson().dump().c_str());
    } else {
        for (const std::string& line : report.FormatLines()) {
            std::printf("%s\n", line.c_str());
        }
    }
    return report.ExitCode();
}

// 读文本文件的尾 N 行(文件不在给空)。
std::vector<std::string> ReadTailLines(const std::filesystem::path& file, int tail) {
    std::vector<std::string> lines;
    std::error_code ec;
    if (!std::filesystem::exists(file, ec) || ec) return lines;
    std::ifstream stream(file, std::ios::binary);
    if (!stream) return lines;
    std::string text((std::istreambuf_iterator<char>(stream)), std::istreambuf_iterator<char>());
    std::size_t start = 0;
    while (start <= text.size()) {
        const std::size_t end = text.find('\n', start);
        std::string line = end == std::string::npos ? text.substr(start)
                                                    : text.substr(start, end - start);
        if (!line.empty()) lines.push_back(std::move(line));
        if (end == std::string::npos) break;
        start = end + 1;
    }
    if (lines.size() > static_cast<std::size_t>(tail)) {
        lines.erase(lines.begin(), lines.end() - static_cast<std::size_t>(tail));
    }
    return lines;
}

int RunGatewayLogs(const gateway::GatewayProfilePaths& paths, const GatewayCommandArgs& args) {
    const int tail = args.tail_lines > 0 ? args.tail_lines : 20;
    std::printf("== boot history 尾 %d 行(%s) ==\n", tail,
                platform::PathToUtf8(paths.boot_history).c_str());
    {
        const auto lines = ReadTailLines(paths.boot_history, tail);
        if (lines.empty()) {
            std::printf("(还没有 boot history)\n");
        } else {
            for (const std::string& line : lines) {
                std::printf("%s\n", line.c_str());
            }
        }
    }
    std::printf("== gateway.log 尾 %d 行(%s) ==\n", tail,
                platform::PathToUtf8(paths.log_file).c_str());
    {
        const auto lines = ReadTailLines(paths.log_file, tail);
        if (lines.empty()) {
            std::printf("(还没有 gateway.log)\n");
        } else {
            for (const std::string& line : lines) {
                std::printf("%s\n", line.c_str());
            }
        }
    }
    // 服务 stdout/stderr 落位指引(不做聚合,只指路)。
    std::printf("== 服务输出落位 ==\n");
    if (gateway::CurrentServicePlatform() == gateway::ServicePlatform::Linux) {
        std::printf("systemd user 单元走 journal:journalctl --user -u %s\n",
                    gateway::SystemdUnitName(paths.name.empty()
                                                 ? std::string(gateway::kDefaultGatewayProfile)
                                                 : paths.name)
                        .c_str());
    } else {
        std::printf("计划任务/LaunchAgent 的 stdout+stderr 追加在:\n  %s\n",
                    platform::PathToUtf8(paths.logs_dir / "service.log").c_str());
        if (gateway::CurrentServicePlatform() == gateway::ServicePlatform::MacOS) {
            std::printf("launchd stderr 另落在:\n  %s\n",
                        platform::PathToUtf8(paths.logs_dir / "service.err.log").c_str());
        }
    }
    return 0;
}

}  // namespace

int RunGatewayCommand(const GatewayCommandArgs& args) {
    const std::string profile_name =
        args.profile.empty() ? std::string(gateway::kDefaultGatewayProfile) : args.profile;
    if (!gateway::IsValidGatewayProfileName(profile_name)) {
        std::fprintf(stderr, "gateway: profile 名须是单段名(不带路径): %s\n",
                     profile_name.c_str());
        return 1;
    }
    std::filesystem::path root = args.gateway_root;
    if (root.empty()) {
        root = DefaultGatewayRoot();
    }
    if (root.empty()) {
        std::fprintf(stderr,
                     "gateway: 状态根不可用(应用根变量坏或找不到主目录),无法定位 gateway 状态树\n");
        return 1;
    }
    const gateway::GatewayProfilePaths paths =
        gateway::ResolveGatewayProfilePaths(root, profile_name);

    if (args.verb == "run") {
        return RunGatewayProcess(paths, args.pump);
    }
    if (args.verb == "status") {
        return PrintGatewayStatus(paths, args.json);
    }
    if (args.verb == "job") {
        return RunGatewayJobCommand(paths, args);
    }
    if (args.verb == "stop") {
        // stop 只在需要时读配置拿宽限;配置坏也不拦停(停一只坏配置的
        // Gateway 恰恰是正事),按默认宽限等。
        const gateway::GatewayConfigLoad config_load = gateway::LoadGatewayConfig(paths.config_file);
        return StopGatewayProcess(paths, config_load.config);
    }
    // ---- V4 运维族(单子 §十"服务安装与常驻运维") ---------------------
    if (args.verb == "install") {
        return RunGatewayInstall(paths);
    }
    if (args.verb == "uninstall") {
        return RunGatewayUninstall(paths);
    }
    if (args.verb == "start") {
        const gateway::GatewayConfigLoad config_load =
            gateway::LoadGatewayConfig(paths.config_file);
        return RunGatewayStart(paths, config_load.config);
    }
    if (args.verb == "restart") {
        const gateway::GatewayConfigLoad config_load =
            gateway::LoadGatewayConfig(paths.config_file);
        return RunGatewayRestart(paths, config_load.config);
    }
    if (args.verb == "doctor") {
        return RunGatewayDoctor(paths, args);
    }
    if (args.verb == "logs") {
        return RunGatewayLogs(paths, args);
    }
    std::fprintf(stderr, "gateway: 认不得动词 \"%s\"\n", args.verb.c_str());
    return 1;
}

}  // namespace lubancode::cli
