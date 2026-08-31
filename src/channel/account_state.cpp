#include "channel/account_state.hpp"

#include <algorithm>
#include <array>

namespace lubancode::channel {

const char* ChannelAccountStateName(ChannelAccountState state) {
    switch (state) {
        case ChannelAccountState::Disabled: return "disabled";
        case ChannelAccountState::Validating: return "validating";
        case ChannelAccountState::Starting: return "starting";
        case ChannelAccountState::Authenticating: return "authenticating";
        case ChannelAccountState::Connecting: return "connecting";
        case ChannelAccountState::Running: return "running";
        case ChannelAccountState::Degraded: return "degraded";
        case ChannelAccountState::Backoff: return "backoff";
        case ChannelAccountState::Stopping: return "stopping";
        case ChannelAccountState::Stopped: return "stopped";
        case ChannelAccountState::Misconfigured: return "misconfigured";
        case ChannelAccountState::TrustRequired: return "trust_required";
        case ChannelAccountState::NeedsLogin: return "needs_login";
        case ChannelAccountState::Fatal: return "fatal";
    }
    return "unknown";
}

std::optional<ChannelAccountState> ChannelAccountStateFromName(std::string_view name) {
    static const std::array<std::pair<std::string_view, ChannelAccountState>, 14> kNames = {{
        {"disabled", ChannelAccountState::Disabled},
        {"validating", ChannelAccountState::Validating},
        {"starting", ChannelAccountState::Starting},
        {"authenticating", ChannelAccountState::Authenticating},
        {"connecting", ChannelAccountState::Connecting},
        {"running", ChannelAccountState::Running},
        {"degraded", ChannelAccountState::Degraded},
        {"backoff", ChannelAccountState::Backoff},
        {"stopping", ChannelAccountState::Stopping},
        {"stopped", ChannelAccountState::Stopped},
        {"misconfigured", ChannelAccountState::Misconfigured},
        {"trust_required", ChannelAccountState::TrustRequired},
        {"needs_login", ChannelAccountState::NeedsLogin},
        {"fatal", ChannelAccountState::Fatal},
    }};
    for (const auto& [key, value] : kNames) {
        if (key == name) return value;
    }
    return std::nullopt;
}

bool IsUnrecoverableAccountState(ChannelAccountState state) {
    switch (state) {
        case ChannelAccountState::Misconfigured:
        case ChannelAccountState::TrustRequired:
        case ChannelAccountState::NeedsLogin:
        case ChannelAccountState::Fatal:
            return true;
        default:
            return false;
    }
}

bool CanTransition(ChannelAccountState from, ChannelAccountState to) {
    if (from == to) return false;
    // 不可恢复终态无出边(重开须显式回到 Disabled:调用方先复位再走起)。
    if (IsUnrecoverableAccountState(from)) return false;
    // 停机方向:任何非终态都可入 Stopping(Ctrl+C/析构/reload 共用)。
    if (to == ChannelAccountState::Stopping) return true;
    // 复位边:Stopped/Disabled 手动重启回 Validating。
    if (to == ChannelAccountState::Validating) {
        return from == ChannelAccountState::Stopped || from == ChannelAccountState::Disabled;
    }

    switch (from) {
        case ChannelAccountState::Disabled:
            return to == ChannelAccountState::Stopped;  // 停一台本来就没起的
        case ChannelAccountState::Validating:
            // 静态检查没过:配置错/信任未批。
            return to == ChannelAccountState::Misconfigured ||
                   to == ChannelAccountState::TrustRequired ||
                   to == ChannelAccountState::Starting;
        case ChannelAccountState::Starting:
            // 起进程失败 → Fatal(spawn_failed);协议不兼容在握手里报。
            return to == ChannelAccountState::Authenticating ||
                   to == ChannelAccountState::Fatal ||
                   to == ChannelAccountState::Backoff;
        case ChannelAccountState::Authenticating:
            // 凭据缺失/失效 → NeedsLogin,停自动重试风暴。
            return to == ChannelAccountState::Connecting ||
                   to == ChannelAccountState::NeedsLogin ||
                   to == ChannelAccountState::Backoff ||
                   to == ChannelAccountState::Fatal;
        case ChannelAccountState::Connecting:
            return to == ChannelAccountState::Running ||
                   to == ChannelAccountState::Degraded ||
                   to == ChannelAccountState::Backoff ||
                   to == ChannelAccountState::Fatal ||
                   to == ChannelAccountState::Misconfigured;  // 握手版本不合
        case ChannelAccountState::Running:
            return to == ChannelAccountState::Degraded ||
                   to == ChannelAccountState::Backoff ||
                   to == ChannelAccountState::Stopped;  // sidecar 自报 stopped
        case ChannelAccountState::Degraded:
            return to == ChannelAccountState::Running ||
                   to == ChannelAccountState::Backoff ||
                   to == ChannelAccountState::Stopped ||
                   to == ChannelAccountState::Fatal;
        case ChannelAccountState::Backoff:
            return to == ChannelAccountState::Connecting ||
                   to == ChannelAccountState::Stopped ||
                   to == ChannelAccountState::Fatal ||
                   to == ChannelAccountState::NeedsLogin ||
                   to == ChannelAccountState::Misconfigured;
        case ChannelAccountState::Stopping:
            return to == ChannelAccountState::Stopped;
        case ChannelAccountState::Stopped:
            return false;  // 只剩回 Validating 的复位边(上面已收)
        default:
            return false;
    }
}

bool ShouldAutoRetry(std::string_view reason) {
    // 不自动重试清单(configuration.md §10)。reason 空算正常推进。
    if (reason.empty() || reason == kReasonStopped) return false;
    if (reason == kReasonMisconfigured || reason == kReasonTrustRequired ||
        reason == kReasonAccountInUse || reason == kReasonTransitionFailed) {
        return false;
    }
    // Bridge 协议的 domain 稳定名里这几枚不可自愈。
    if (reason == "protocol_incompatible" || reason == "login_required" ||
        reason == "account_revoked") {
        return false;
    }
    // 其余(spawn_failed/process_crashed/transport_failed/shutdown_timeout/
    // rate_limited/spool_write_failed...)都允许按退避重试。
    return true;
}

nlohmann::json AccountStatusTransition::ToJson() const {
    nlohmann::json json = nlohmann::json::object();
    json["channel_id"] = channel_id;
    json["account_id"] = account_id;
    json["timestamp_ms"] = timestamp_ms;
    json["from"] = ChannelAccountStateName(from);
    json["to"] = ChannelAccountStateName(to);
    json["reason"] = reason;
    json["detail"] = detail;
    json["retry_at_ms"] = retry_at_ms;
    json["generation"] = generation;
    return json;
}

std::optional<AccountStatusTransition> AccountStatusTransition::FromJsonStrict(
    const nlohmann::json& json, std::string* error) {
    const auto fail = [error](const std::string& message) {
        if (error != nullptr) *error = message;
        return std::nullopt;
    };
    if (!json.is_object()) return fail("账号状态迁移行必须是 JSON object");
    const std::array<const char*, 9> kRequired = {
        "channel_id", "account_id", "timestamp_ms", "from", "to",
        "reason",     "detail",     "retry_at_ms",  "generation"};
    for (const char* key : kRequired) {
        if (!json.contains(key)) return fail(std::string("缺必填字段 ") + key);
    }
    for (auto it = json.begin(); it != json.end(); ++it) {
        bool known = false;
        for (const char* key : kRequired) {
            if (it.key() == key) {
                known = true;
                break;
            }
        }
        if (!known) return fail("未知字段 " + it.key());
    }
    if (!json["channel_id"].is_string() || !json["account_id"].is_string() ||
        !json["reason"].is_string() || !json["detail"].is_string()) {
        return fail("channel_id/account_id/reason/detail 都必须是字符串");
    }
    if (!json["timestamp_ms"].is_number_integer() || !json["retry_at_ms"].is_number_integer() ||
        !json["generation"].is_number_integer()) {
        return fail("timestamp_ms/retry_at_ms/generation 都必须是整数");
    }
    if (!json["from"].is_string() || !json["to"].is_string()) {
        return fail("from/to 都必须是字符串");
    }
    const auto from = ChannelAccountStateFromName(json["from"].get<std::string>());
    if (!from.has_value()) return fail("from 不是认得的账号状态名");
    const auto to = ChannelAccountStateFromName(json["to"].get<std::string>());
    if (!to.has_value()) return fail("to 不是认得的账号状态名");

    AccountStatusTransition transition;
    transition.channel_id = json["channel_id"].get<std::string>();
    transition.account_id = json["account_id"].get<std::string>();
    transition.timestamp_ms = json["timestamp_ms"].get<std::int64_t>();
    transition.from = *from;
    transition.to = *to;
    transition.reason = json["reason"].get<std::string>();
    transition.detail = json["detail"].get<std::string>();
    transition.retry_at_ms = json["retry_at_ms"].get<std::int64_t>();
    transition.generation = json["generation"].get<int>();
    return transition;
}

std::int64_t BackoffDelayMs(int attempt, double jitter_fraction) {
    constexpr int kScheduleLength =
        static_cast<int>(sizeof(kBackoffScheduleMs) / sizeof(kBackoffScheduleMs[0]));
    const int clamped = std::clamp(attempt, 0, kScheduleLength - 1);
    const double jitter = std::clamp(jitter_fraction, 0.0, 0.10);
    const double base = static_cast<double>(kBackoffScheduleMs[clamped]);
    return static_cast<std::int64_t>(base * (1.0 + jitter));
}

}  // namespace lubancode::channel
