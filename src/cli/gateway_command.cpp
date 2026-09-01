#include "cli/gateway_command.hpp"

#include <cstdio>
#include <filesystem>
#include <string>

#include "app/version.hpp"
#include "config/config.hpp"
#include "gateway/control_server.hpp"
#include "gateway/process.hpp"
#include "gateway/status.hpp"
#include "platform/wall_clock.hpp"
#include "tools/path_utils.hpp"

namespace lubancode::cli {

namespace {

std::filesystem::path DefaultGatewayRoot() {
    const auto home = config::HomeLubancodeDir();
    if (!home.has_value()) {
        return std::filesystem::path();
    }
    return tools::Utf8ToPath(*home) / "gateway";
}

int RunGatewayProcess(const gateway::GatewayProfilePaths& paths) {
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
    const gateway::GatewayProbe probe = gateway::ProbeGateway(paths);
    if (json) {
        std::printf("%s\n", gateway::ProbeToJson(probe).dump().c_str());
        return probe.state == gateway::GatewayProbe::State::Running ? 0 : 1;
    }
    for (const std::string& line : gateway::FormatProbeLines(probe)) {
        std::printf("%s\n", line.c_str());
    }
    return probe.state == gateway::GatewayProbe::State::Running ? 0 : 1;
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
        std::fprintf(stderr, "gateway: 找不到用户主目录,无法定位 ~/.lubancode/gateway\n");
        return 1;
    }
    const gateway::GatewayProfilePaths paths =
        gateway::ResolveGatewayProfilePaths(root, profile_name);

    if (args.verb == "run") {
        return RunGatewayProcess(paths);
    }
    if (args.verb == "status") {
        return PrintGatewayStatus(paths, args.json);
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
