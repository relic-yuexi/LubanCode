#include "gateway/status.hpp"

#include <fstream>

#include "platform/paths.hpp"
#include "trajectory/session_lock.hpp"

namespace lubancode::gateway {

namespace {

std::optional<GatewayLockRecord> ReadLockRecord(const std::filesystem::path& lock_file,
                                                std::string* error) {
    std::error_code ec;
    if (!std::filesystem::exists(lock_file, ec) || ec) {
        return std::nullopt;
    }
    std::ifstream stream(lock_file, std::ios::binary);
    if (!stream) {
        if (error != nullptr) *error = "锁文件在,但打不开";
        return std::nullopt;
    }
    std::string text((std::istreambuf_iterator<char>(stream)), std::istreambuf_iterator<char>());
    if (text.empty()) {
        if (error != nullptr) *error = "锁文件是空的";
        return std::nullopt;
    }
    try {
        const nlohmann::json parsed = nlohmann::json::parse(text);
        std::string parse_error;
        auto record = GatewayLockRecord::FromJsonStrict(parsed, &parse_error);
        if (!record.has_value() && error != nullptr) *error = "锁账读不懂: " + parse_error;
        return record;
    } catch (const nlohmann::json::exception&) {
        if (error != nullptr) *error = "锁文件不是合法 JSON(可能写了一半)";
        return std::nullopt;
    }
}

}  // namespace

GatewayProbe ProbeGateway(const GatewayProfilePaths& paths) {
    GatewayProbe probe;
    if (paths.root.empty()) {
        probe.detail = "profile 名不合法";
        return probe;
    }

    // control 快照:文件在但读不懂 = 坏 control endpoint,降级诊断不崩。
    std::string control_error;
    probe.control = ReadControlSnapshot(paths.control_file, &control_error);
    probe.control_error = control_error;
    probe.control_unreadable = !control_error.empty();

    const GatewayBootHistory history(paths.boot_history);
    probe.unclean_streak = CountUncleanBootStreak(history.ReadAll());

    std::string lock_error;
    const auto holder = ReadLockRecord(paths.lock_file, &lock_error);
    if (!holder.has_value() && !lock_error.empty()) {
        probe.state = GatewayProbe::State::BrokenLock;
        probe.detail = "gateway.lock_stale: " + lock_error + "(锁文件: " +
                       platform::PathToUtf8(paths.lock_file) + ")";
        return probe;
    }
    if (!holder.has_value()) {
        if (probe.control.has_value() && probe.control->state != "stopped") {
            probe.state = GatewayProbe::State::StaleRemnant;
            probe.detail = "未运行:无锁,但控制快照残留 state=" + probe.control->state +
                           "(上次未及收口或硬杀)";
            return probe;
        }
        probe.state = GatewayProbe::State::NotRunning;
        probe.detail = "gateway.not_running: 没有运行中的 Gateway";
        return probe;
    }

    probe.holder = *holder;
    const trajectory::SessionLockOwner owner{holder->pid, holder->start_token, 0};
    if (trajectory::ProbeLockHolder(owner) == trajectory::LockHolderState::Alive) {
        probe.state = GatewayProbe::State::Running;
        probe.detail = "运行中(boot " + holder->boot_id + ",pid " + std::to_string(holder->pid) +
                       ")";
        if (probe.control_unreadable) {
            probe.detail += ";控制快照读不懂: " + probe.control_error +
                            "(gateway.control_unreachable)";
        } else if (!probe.control.has_value()) {
            probe.detail += ";控制快照缺失";
        } else if (probe.control->safe_mode) {
            probe.detail += ";SafeMode(业务面暂停)";
        }
        return probe;
    }
    probe.state = GatewayProbe::State::StaleLock;
    probe.detail = "未运行:锁是陈旧的(持有进程已死或 PID 复用),下次启动自动清";
    return probe;
}

nlohmann::json ProbeToJson(const GatewayProbe& probe) {
    nlohmann::json json = nlohmann::json::object();
    const char* state_name = "not_running";
    switch (probe.state) {
        case GatewayProbe::State::Running:
            state_name = "running";
            break;
        case GatewayProbe::State::StaleLock:
            state_name = "stale_lock";
            break;
        case GatewayProbe::State::StaleRemnant:
            state_name = "stale_remnant";
            break;
        case GatewayProbe::State::BrokenLock:
            state_name = "broken_lock";
            break;
        case GatewayProbe::State::NotRunning:
            break;
    }
    json["state"] = state_name;
    json["detail"] = probe.detail;
    json["unclean_boot_streak"] = probe.unclean_streak;
    if (probe.holder.pid != 0) {
        nlohmann::json holder = nlohmann::json::object();
        holder["pid"] = probe.holder.pid;
        holder["start_token"] = probe.holder.start_token;
        holder["boot_id"] = probe.holder.boot_id;
        holder["acquired_at_ms"] = probe.holder.acquired_at_ms;
        json["holder"] = holder;
    }
    if (probe.control.has_value()) {
        json["control"] = probe.control->ToJson();
    } else {
        json["control"] = nullptr;
        if (probe.control_unreadable) {
            json["control_error"] = probe.control_error;
            json["error_code"] = "gateway.control_unreachable";
        }
    }
    if (probe.state == GatewayProbe::State::Running && probe.control.has_value()) {
        json["health"] = probe.control->health;
        json["safe_mode"] = probe.control->safe_mode;
    }
    return json;
}

std::vector<std::string> FormatProbeLines(const GatewayProbe& probe) {
    std::vector<std::string> lines;
    lines.push_back(probe.detail);
    if (probe.state == GatewayProbe::State::Running) {
        if (probe.control.has_value()) {
            lines.push_back("  状态: " + probe.control->state + " / health: " +
                            probe.control->health);
            lines.push_back("  启动于: " + std::to_string(probe.control->started_at_ms) +
                            "ms(boot " + probe.control->boot_id + ")");
            if (probe.control->safe_mode) {
                lines.push_back("  SafeMode: 连续非干净关机达阈值,业务面暂停;"
                                "干净关机一次即退出");
            }
        }
    }
    if (probe.unclean_streak > 0) {
        lines.push_back("  连续非干净关机: " + std::to_string(probe.unclean_streak) + " 次");
    }
    return lines;
}

}  // namespace lubancode::gateway
