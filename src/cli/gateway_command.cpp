#include "cli/gateway_command.hpp"

#include <cstdio>
#include <filesystem>
#include <string>

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

// V1 job 族:add/run-now 落控制命令文件(持久任务创建走本地命令,不直接
// 改文件);list 只读 automation 账。命令落了但 Gateway 没跑:如实说
// (下次 run 起来消费),不冒充"任务已建"。
int RunGatewayJobCommand(const gateway::GatewayProfilePaths& paths, const GatewayCommandArgs& args) {
    const std::int64_t now_ms = platform::WallClockNowMs();
    if (args.job_verb == "add") {
        gateway::GatewayJobAddCommand command;
        command.prompt = args.prompt;
        command.idempotency_key = args.idempotency_key;
        command.job_id = args.job_id;
        command.due_at_ms = args.due_at_ms;
        command.requested_at_ms = now_ms;
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
    // list:只读账。
    const gateway::AutomationProjection projection = gateway::ReadAutomationProjection(
        paths.automation_log);
    if (args.json) {
        nlohmann::json json = nlohmann::json::array();
        for (const auto& [id, job] : projection.jobs) {
            nlohmann::json item = nlohmann::json::object();
            item["job_id"] = job.job_id;
            item["prompt"] = job.prompt;
            item["due_at_ms"] = job.due_at_ms;
            json.push_back(std::move(item));
        }
        std::printf("%s\n", json.dump().c_str());
        return 0;
    }
    if (projection.jobs.empty()) {
        std::printf("没有持久任务(账为空或尚无账)。\n");
        return 0;
    }
    for (const auto& [id, job] : projection.jobs) {
        std::printf("%s\tdue %lldms\t%s\n", job.job_id.c_str(),
                    static_cast<long long>(job.due_at_ms), job.prompt.c_str());
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
