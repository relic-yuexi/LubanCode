// features.telemetry 激活合同的实现。合同见 activation.hpp 文件头。
//
// 环境变量读法照 runtime::ResolveTrajectoryEnabled 的同款(Windows 走
// _dupenv_s 免 getenv 的 TLS 坑;写坏的值不当意见,配置照旧——救命阀
// 字段的待遇)。
#include "telemetry/activation.hpp"

#include <cstdlib>
#include <utility>

namespace lubancode::telemetry {
namespace {

std::optional<std::string> ReadEnv(const char* name) {
#ifdef _WIN32
    char* buffer = nullptr;
    std::size_t size = 0;
    const errno_t err = _dupenv_s(&buffer, &size, name);
    if (err != 0 || buffer == nullptr) {
        return std::nullopt;
    }
    std::string value = buffer;
    std::free(buffer);
    return value;
#else
    const char* raw = std::getenv(name);
    if (raw == nullptr) {
        return std::nullopt;
    }
    return std::string(raw);
#endif
}

// LUBANCODE_TELEMETRY 的显式意见(§8.3)。空串/auto/坏值 = 没意见。
std::optional<bool> TelemetryEnvOpinion() {
    const std::optional<std::string> raw = ReadEnv("LUBANCODE_TELEMETRY");
    if (!raw.has_value() || raw->empty() || *raw == "auto") {
        return std::nullopt;
    }
    if (*raw == "1" || *raw == "true" || *raw == "on" || *raw == "yes") {
        return true;
    }
    if (*raw == "0" || *raw == "false" || *raw == "off" || *raw == "no") {
        return false;
    }
    return std::nullopt;
}

// 紧急总闸(§8.3):=1 拉下。其它值(含 true 之外的怪值)不拉——总闸
// 语义是"紧急关",认 1 与 true,不拿坏值误伤。
bool KillSwitchPulled() {
    const std::optional<std::string> raw = ReadEnv("LUBANCODE_DISABLE_TELEMETRY");
    return raw.has_value() && (*raw == "1" || *raw == "true");
}

}  // namespace

TelemetryActivation ResolveTelemetryActivation(bool config_telemetry,
                                               bool config_trajectory) {
    TelemetryActivation activation;
    activation.config_telemetry = config_telemetry;
    activation.config_trajectory = config_trajectory;
    activation.env_opinion = TelemetryEnvOpinion();
    activation.kill_switch = KillSwitchPulled();

    if (activation.kill_switch) {
        activation.status = TelemetryActivationStatus::DisabledByKillSwitch;
        activation.reason_code = "telemetry.kill_switch";
        return activation;
    }
    const bool effective =
        activation.env_opinion.has_value() ? *activation.env_opinion : config_telemetry;
    if (!effective) {
        activation.status = TelemetryActivationStatus::Disabled;
        activation.reason_code =
            activation.env_opinion.has_value() && !*activation.env_opinion
                ? "telemetry.env_off"
                : "telemetry.off";
        return activation;
    }
    if (!config_trajectory) {
        // §8.2/不变量 5:不暗开 trajectory,明报缺前置。
        activation.status = TelemetryActivationStatus::RequiresTrajectory;
        activation.reason_code = "telemetry.requires_trajectory";
        return activation;
    }
    activation.status = TelemetryActivationStatus::Active;
    activation.reason_code = "telemetry.active";
    return activation;
}

}  // namespace lubancode::telemetry
