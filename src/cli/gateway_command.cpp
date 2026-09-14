#include "cli/gateway_command.hpp"

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

#include "app/version.hpp"
#include "gateway/automation_store.hpp"
#include "gateway/control_server.hpp"
#include "gateway/process.hpp"
#include "gateway/status.hpp"
#include "gateway/work_pump.hpp"
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
    std::fprintf(stderr, "gateway: 认不得动词 \"%s\"\n", args.verb.c_str());
    return 1;
}

}  // namespace lubancode::cli
