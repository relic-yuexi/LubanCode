// Gateway doctor(总装单 V4)实现。码表与退出码冻结于 contracts.md §14。
#include "gateway/doctor.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <thread>

#include "channel/channel_config.hpp"
#include "channel/credentials.hpp"
#include "config/config.hpp"
#include "gateway/automation_store.hpp"
#include "gateway/process.hpp"
#include "platform/paths.hpp"
#include "platform/process.hpp"
#include "platform/wall_clock.hpp"
#include "trajectory/session_lock.hpp"

namespace lubancode::gateway {

namespace {

std::int64_t CallNowMs(const std::function<std::int64_t()>& now_ms) {
    return now_ms ? now_ms() : platform::WallClockNowMs();
}

// channels 段的生产读取:全局 config.json 全文(doctor 只切 channels 段,
// 其余不碰)。
std::optional<std::string> ReadGlobalConfigText() {
    const auto path = config::GlobalConfigFilePath();
    if (!path.has_value()) return std::nullopt;
    std::error_code ec;
    if (!std::filesystem::exists(platform::Utf8ToPath(*path), ec) || ec) {
        return std::string("{}");  // 没有全局配置 = 没有 channels(合法态)
    }
    std::ifstream stream(platform::Utf8ToPath(*path), std::ios::binary);
    if (!stream) return std::nullopt;
    return std::string((std::istreambuf_iterator<char>(stream)), std::istreambuf_iterator<char>());
}

bool DefaultDiskWriteProbe(const std::filesystem::path& dir) {
    // 即写即删的探针文件;目录不在/不可写如实失败。零建目录。
    std::error_code ec;
    if (!std::filesystem::exists(dir, ec) || ec) return false;
    const std::filesystem::path probe =
        dir / ("doctor-probe-" + std::to_string(platform::CurrentProcessId()) + ".tmp");
    {
        std::ofstream stream(probe, std::ios::binary | std::ios::trunc);
        if (!stream) return false;
        stream << "probe";
        stream.flush();
        if (!stream) return false;
    }
    std::filesystem::remove(probe, ec);
    return true;
}

}  // namespace

const char* DoctorSeverityName(DoctorSeverity severity) {
    switch (severity) {
        case DoctorSeverity::Ok: return "ok";
        case DoctorSeverity::Info: return "info";
        case DoctorSeverity::Warn: return "warn";
        case DoctorSeverity::Fail: return "fail";
    }
    return "unknown";
}

nlohmann::json DoctorCheck::ToJson() const {
    nlohmann::json json = nlohmann::json::object();
    json["code"] = code;
    json["severity"] = DoctorSeverityName(severity);
    json["detail"] = detail;
    return json;
}

bool DoctorReport::HasFail() const {
    for (const DoctorCheck& check : checks) {
        if (check.severity == DoctorSeverity::Fail) return true;
    }
    return false;
}

bool DoctorReport::HasWarn() const {
    for (const DoctorCheck& check : checks) {
        if (check.severity == DoctorSeverity::Warn) return true;
    }
    return false;
}

int DoctorReport::ExitCode() const {
    if (HasFail()) return 2;
    if (HasWarn()) return 1;
    return 0;
}

nlohmann::json DoctorReport::ToJson() const {
    nlohmann::json json = nlohmann::json::object();
    nlohmann::json items = nlohmann::json::array();
    for (const DoctorCheck& check : checks) {
        items.push_back(check.ToJson());
    }
    json["checks"] = std::move(items);
    json["exit_code"] = ExitCode();
    json["unclean_streak"] = unclean_streak;
    return json;
}

std::vector<std::string> DoctorReport::FormatLines() const {
    std::vector<std::string> lines;
    for (const DoctorCheck& check : checks) {
        std::string tag;
        switch (check.severity) {
            case DoctorSeverity::Ok: tag = "[ok]  "; break;
            case DoctorSeverity::Info: tag = "[info]"; break;
            case DoctorSeverity::Warn: tag = "[warn]"; break;
            case DoctorSeverity::Fail: tag = "[FAIL]"; break;
        }
        std::string line = std::string(tag) + " " + check.code;
        if (!check.detail.empty()) line += " —— " + check.detail;
        lines.push_back(std::move(line));
    }
    lines.emplace_back("退出码 " + std::to_string(ExitCode()) + "(0=全绿 1=有警 2=有病)");
    return lines;
}

// ---------------------------------------------------------------------------
// RunGatewayDoctor
// ---------------------------------------------------------------------------

DoctorReport RunGatewayDoctor(const GatewayProfilePaths& paths, const DoctorOptions& options) {
    DoctorReport report;

    // ---- 1. 进程/锁活态(复用只读探针) ----
    const GatewayProbe probe = ProbeGateway(paths);
    switch (probe.state) {
        case GatewayProbe::State::Running:
            report.checks.push_back({"process.running", DoctorSeverity::Ok,
                                     "pid " + std::to_string(probe.holder.pid) + " 持锁"});
            break;
        case GatewayProbe::State::NotRunning:
            report.checks.push_back(
                {"process.not_running", DoctorSeverity::Info, "没有运行中的 Gateway"});
            break;
        case GatewayProbe::State::StaleLock:
            report.checks.push_back({"process.stale_lock", DoctorSeverity::Warn,
                                     "锁是陈旧的(持有者死透/PID 复用),下次启动自动清"});
            break;
        case GatewayProbe::State::StaleRemnant:
            report.checks.push_back({"process.stale_remnant", DoctorSeverity::Warn,
                                     "锁没了但 control 还说 running(崩在关机半路)"});
            break;
        case GatewayProbe::State::BrokenLock:
            report.checks.push_back(
                {"process.broken_lock", DoctorSeverity::Fail,
                 probe.control_error.empty() ? "锁文件读不懂" : probe.control_error});
            break;
    }

    // ---- 2. 配置(可启动 dry:装不上/起不来的第一道闸) ----
    const auto config_load = LoadGatewayConfig(paths.config_file);
    if (config_load.status == GatewayConfigLoad::Status::Ok) {
        report.checks.push_back({"config.ok", DoctorSeverity::Ok,
                                 "shutdown_grace=" + std::to_string(config_load.config.shutdown_grace_secs) + "s"});
    } else if (config_load.status == GatewayConfigLoad::Status::Missing) {
        report.checks.push_back(
            {"config.missing", DoctorSeverity::Info, "没有 gateway.json,按默认值跑"});
    } else {
        report.checks.push_back(
            {"config.invalid", DoctorSeverity::Fail, config_load.error});
    }

    // ---- 3. 服务注册状态(supervisor 面;缺席是环境事实不是病) ----
    if (!options.service_runner) {
        report.checks.push_back(
            {"service.skipped", DoctorSeverity::Info, "未装配服务管理器探测"});
    } else {
        GatewayServiceSpec query_spec;
        query_spec.profile = paths.name.empty() ? std::string(kDefaultGatewayProfile) : paths.name;
        const ServiceOpOutcome query =
            QueryGatewayService(query_spec, options.platform, options.service_runner);
        if (!query.ok && query.error_code == "service.manager_unavailable") {
            report.checks.push_back({"service.manager_unavailable", DoctorSeverity::Warn,
                                     "服务管理器起不来(" + query.detail + ")"});
        } else if (query.ok) {
            report.checks.push_back({"service.registered", DoctorSeverity::Ok,
                                     std::string("平台 ") + ServicePlatformName(options.platform)});
        } else {
            // 管理器在、报"没注册":装没装服务是选择,不是病。
            report.checks.push_back(
                {"service.not_registered", DoctorSeverity::Info,
                 "服务未注册(lubancode gateway install 可装;本机照常手动 run)"});
        }
    }

    // ---- 4. 安装记录对账(exe 在不在/版本对不对) ----
    {
        std::string load_error;
        const auto record = ServiceInstallRecord::Load(paths.profile_dir / "service" / "install.json",
                                                       &load_error);
        if (!record.has_value() && !load_error.empty()) {
            report.checks.push_back({"install.record_unreadable", DoctorSeverity::Warn, load_error});
        } else if (!record.has_value()) {
            report.checks.push_back({"install.not_recorded", DoctorSeverity::Info,
                                     "没有安装记录(未走 gateway install)"});
        } else {
            std::error_code ec;
            const std::filesystem::path exe = platform::Utf8ToPath(record->exe_path);
            if (!std::filesystem::exists(exe, ec) || ec) {
                report.checks.push_back({"install.exe_missing", DoctorSeverity::Fail,
                                         "安装记录钉的 exe 不在了: " + record->exe_path +
                                             "(重装或 gateway install 覆盖)"});
            } else if (!options.lubancode_version.empty() &&
                       options.lubancode_version != record->lubancode_version) {
                report.checks.push_back(
                    {"install.version_mismatch", DoctorSeverity::Warn,
                     "服务单元钉的是 " + record->lubancode_version + ",当前 exe 是 " +
                         options.lubancode_version +
                         "(升级后重跑 gateway install 刷新记录;回滚步骤见 runbook)"});
            } else {
                report.checks.push_back({"install.ok", DoctorSeverity::Ok,
                                         "exe " + record->exe_path + " 版本 " +
                                             record->lubancode_version});
            }
        }
    }

    // ---- 5. 凭据面(channels 配置;不显值;V3 渠道线未总装,激活闸只跑
    //         配置/凭据两闸,不下"渠道能起"的结论) ----
    std::function<std::optional<std::string>()> read_channels = options.read_channels_config;
    if (!read_channels) read_channels = ReadGlobalConfigText;
    const auto config_text = read_channels();
    if (!config_text.has_value()) {
        report.checks.push_back({"credentials.skipped", DoctorSeverity::Info,
                                 "读不到全局 config(channels 面跳过)"});
    } else {
        nlohmann::json parsed;
        bool parse_ok = true;
        try {
            parsed = nlohmann::json::parse(*config_text);
        } catch (const nlohmann::json::exception&) {
            parse_ok = false;
        }
        if (!parse_ok || !parsed.is_object()) {
            report.checks.push_back(
                {"channels.config_invalid", DoctorSeverity::Warn, "全局 config 不是合法 JSON object"});
        } else if (!parsed.contains("channels")) {
            report.checks.push_back({"channels.none_configured", DoctorSeverity::Info,
                                     "没有 channels 配置(首版无渠道是合法态)"});
        } else if (!parsed["channels"].is_object()) {
            report.checks.push_back({"channels.config_invalid", DoctorSeverity::Warn,
                                     "channels 字段必须是 JSON object"});
        } else {
            std::string channels_error;
            const auto channels = channel::ParseChannelsUserConfig(
                parsed["channels"], "<config.json>", &channels_error);
            if (!channels.has_value()) {
                report.checks.push_back(
                    {"channels.config_invalid", DoctorSeverity::Warn, channels_error});
            } else {
                std::size_t account_count = 0;
                for (const auto& [channel_id, channel_config] : *channels) {
                    for (const auto& [account_id, account] : channel_config.accounts) {
                        account_count += 1;
                        const std::string prefix =
                            "credentials." + channel_id + "." + account_id;
                        if (!account.enabled || !channel_config.enabled) {
                            report.checks.push_back(
                                {prefix + ".disabled", DoctorSeverity::Info, "账号/渠道未启用"});
                            continue;
                        }
                        // 凭据可读性:resolver 全跑一遍,值不出这个函数。
                        const auto resolved = channel::ResolveChannelCredential(account);
                        if (resolved.has_value()) {
                            const auto source = channel::DescribeCredentialSource(account);
                            if (source == channel::CredentialSource::InlinePlaintext) {
                                report.checks.push_back(
                                    {prefix + ".insecure_source", DoctorSeverity::Warn,
                                     "凭据走配置明文(建议改 secret_file/secret_env)"});
                            } else {
                                report.checks.push_back(
                                    {prefix + ".ok", DoctorSeverity::Ok,
                                     std::string("凭据可读(来源 ") +
                                         channel::CredentialSourceName(source) + ")"});
                            }
                        } else {
                            report.checks.push_back(
                                {prefix + ".resolve_failed", DoctorSeverity::Warn,
                                 resolved.error().reason + ": " + resolved.error().detail});
                        }
                    }
                }
                if (account_count == 0) {
                    report.checks.push_back({"credentials.none_configured", DoctorSeverity::Info,
                                             "channels 配了但没有账号"});
                } else {
                    // V3 渠道线未总装:五闸的 trust/lock/渠道载体不在本面,
                    // 如实说明,不冒充"激活闸全过"。
                    report.checks.push_back(
                        {"activation.deferred_to_v3", DoctorSeverity::Info,
                         "渠道执行载体归 V3 总装;本面只验配置/凭据两闸"});
                }
            }
        }
    }

    // ---- 6. 磁盘可写(profile 目录内即写即删;不建目录) ----
    {
        std::function<bool(const std::filesystem::path&)> probe_write = options.write_probe;
        if (!probe_write) probe_write = DefaultDiskWriteProbe;
        std::error_code ec;
        if (!std::filesystem::exists(paths.profile_dir, ec) || ec) {
            report.checks.push_back({"disk.no_profile_dir", DoctorSeverity::Info,
                                     "profile 目录还不存在(没跑过 gateway run/install)"});
        } else if (probe_write(paths.profile_dir)) {
            report.checks.push_back({"disk.writable", DoctorSeverity::Ok,
                                     platform::PathToUtf8(paths.profile_dir)});
        } else {
            report.checks.push_back({"disk.read_only", DoctorSeverity::Fail,
                                     "profile 目录写不进: " + platform::PathToUtf8(paths.profile_dir)});
        }
    }

    // ---- 7. 坏账/在飞/死信(领域账只读投影) ----
    {
        const GatewayStatusSections sections = ProbeStatusSections(paths);
        if (sections.occurrences_needs_review > 0) {
            report.checks.push_back(
                {"ledger.needs_review", DoctorSeverity::Warn,
                 std::to_string(sections.occurrences_needs_review) +
                     " 枚 occurrence 停审(gateway job read <jobId> 逐枚看;处置走恢复裁决)"});
        } else if (sections.work_ledger_present) {
            report.checks.push_back({"ledger.needs_review", DoctorSeverity::Ok, "无停审 occurrence"});
        } else {
            report.checks.push_back(
                {"ledger.absent", DoctorSeverity::Info, "没有 automation 账(没跑过)"});
        }
        if (sections.occurrences_in_flight > 0) {
            const bool running = probe.state == GatewayProbe::State::Running;
            report.checks.push_back(
                {"ledger.in_flight", running ? DoctorSeverity::Info : DoctorSeverity::Warn,
                 std::to_string(sections.occurrences_in_flight) +
                     (running ? " 枚在飞(运行中,正常)" : " 枚在飞但进程没在跑——重启后 reconcile 先裁决再接新活")});
        }
        if (sections.delivery_flagged > 0) {
            report.checks.push_back({"dead_letter.outbox_flagged", DoctorSeverity::Warn,
                                     std::to_string(sections.delivery_flagged) +
                                         " 枚本地投递 flagged(dead letter)"});
        }
        std::size_t channel_dead = 0;
        for (const auto& account : sections.channels) {
            channel_dead += account.dead_letter;
        }
        if (channel_dead > 0) {
            report.checks.push_back({"dead_letter.channel", DoctorSeverity::Warn,
                                     std::to_string(channel_dead) + " 枚渠道死信"});
        }
        if (sections.delivery_ledger_present && sections.delivery_flagged == 0) {
            report.checks.push_back({"dead_letter.outbox_flagged", DoctorSeverity::Ok, "无 flagged"});
        }
    }

    // ---- 8. SafeMode 与最近一次关机(boot history) ----
    {
        const GatewayBootHistory history(paths.boot_history);
        const std::vector<GatewayBootLine> lines = history.ReadAll();
        report.unclean_streak = CountUncleanBootStreak(lines);
        const int threshold =
            config_load.status == GatewayConfigLoad::Status::Ok
                ? config_load.config.safe_mode_threshold
                : GatewayProfileConfig{}.safe_mode_threshold;
        if (report.unclean_streak >= threshold) {
            report.checks.push_back({"safe_mode.on", DoctorSeverity::Warn,
                                     "连续非干净关机 " + std::to_string(report.unclean_streak) +
                                         " 次达阈值 " + std::to_string(threshold) +
                                         "(业务面暂停;排查后 gateway doctor --ack-safe-mode 显式确认)"});
        } else {
            report.checks.push_back({"safe_mode.off", DoctorSeverity::Ok,
                                     "非干净关机连击 " + std::to_string(report.unclean_streak) +
                                         "/" + std::to_string(threshold)});
        }
        // 最近一次 shutdown 事实(尾部倒找)。
        const auto last_shutdown = std::find_if(
            lines.rbegin(), lines.rend(), [](const GatewayBootLine& line) {
                return line.kind == GatewayBootLine::Kind::Shutdown;
            });
        if (last_shutdown == lines.rend()) {
            report.checks.push_back(
                {"boot.no_history", DoctorSeverity::Info, "还没有关机记录"});
        } else if (last_shutdown->clean) {
            report.checks.push_back({"boot.last_shutdown_clean", DoctorSeverity::Ok,
                                     "reason=" + last_shutdown->reason});
        } else {
            std::string detail = "非干净关机(reason=" + last_shutdown->reason + ")";
            if (!last_shutdown->uncollected_work.empty()) {
                detail += ";" + std::to_string(last_shutdown->uncollected_work.size()) +
                          " 枚 work 未收净已入账(occurrence: " +
                          last_shutdown->uncollected_work.front() + " 等)";
            }
            report.checks.push_back({"boot.last_shutdown_unclean", DoctorSeverity::Warn, detail});
        }
    }
    return report;
}

// ---------------------------------------------------------------------------
// 健康探针
// ---------------------------------------------------------------------------

bool IsGatewayReady(const GatewayProbe& probe) {
    if (probe.state != GatewayProbe::State::Running) return false;
    if (!probe.control.has_value()) return false;
    if (probe.control->state != "running") return false;
    if (probe.control->health != "ok") return false;
    if (probe.control->safe_mode) return false;
    return true;
}

WaitReadyOutcome WaitForGatewayReady(const GatewayProfilePaths& paths, int timeout_secs,
                                     const std::function<std::int64_t()>& now_ms,
                                     const std::function<void(int)>& sleep_ms) {
    WaitReadyOutcome outcome;
    const std::int64_t started = CallNowMs(now_ms);
    const std::int64_t budget_ms =
        static_cast<std::int64_t>(timeout_secs > 0 ? timeout_secs : 0) * 1000;
    while (true) {
        const GatewayProbe probe = ProbeGateway(paths);
        if (IsGatewayReady(probe)) {
            outcome.ready = true;
            outcome.detail = "ready(boot " + probe.control->boot_id + ",pid " +
                             std::to_string(probe.holder.pid) + ")";
            outcome.waited_ms = CallNowMs(now_ms) - started;
            return outcome;
        }
        outcome.waited_ms = CallNowMs(now_ms) - started;
        if (outcome.waited_ms >= budget_ms) {
            outcome.detail = "gateway.not_ready: 等了 " + std::to_string(outcome.waited_ms) +
                             "ms 未 ready(" + probe.detail + ")";
            return outcome;
        }
        if (sleep_ms) {
            sleep_ms(250);
        } else {
            std::this_thread::sleep_for(std::chrono::milliseconds(250));
        }
    }
}

// ---------------------------------------------------------------------------
// SafeMode ack
// ---------------------------------------------------------------------------

std::string AckSafeMode(const GatewayProfilePaths& paths) {
    GatewayBootLine ack;
    ack.kind = GatewayBootLine::Kind::Ack;
    ack.boot_id = std::string();  // ack 不是一次 boot,无 boot_id
    ack.pid = platform::CurrentProcessId();
    ack.start_token = trajectory::CurrentProcessStartToken();
    ack.reason = "ack_safe_mode";
    return GatewayBootHistory(paths.boot_history).Append(ack);
}

}  // namespace lubancode::gateway
