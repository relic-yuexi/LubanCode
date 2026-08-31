// 渠道账号状态机、generation、退避(多渠道消息接入单阶段 2)。
//
// 唯一真源 docs/architecture/channels/configuration.md §9-10(状态枚举、
// 迁移携带的账、退避序列与不自动重试清单)。本文件是纯逻辑,零 IO:
//   - 状态枚举与名称往返(线上/日志用稳定名);
//   - 迁移合法性表(哪些边存在;不可恢复终态无出边);
//   - AccountStatusTransition——一次迁移的完整账(channel/account/
//     timestamp/reason/detail/retry_at/generation),JSON 往返严格;
//   - BackoffSchedule——1s..60s 序列 + 10% jitter + 稳定归零;
//   - ShouldAutoRetry(reason)——不自动重试清单。
//
// 依赖铁律沿 channel 库:只用标准库 + nlohmann::json。
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include <nlohmann/json.hpp>

namespace lubancode::channel {

// ---------------------------------------------------------------------------
// 账号状态(configuration.md §9)
// ---------------------------------------------------------------------------

enum class ChannelAccountState {
    Disabled,
    Validating,
    Starting,
    Authenticating,
    Connecting,
    Running,
    Degraded,
    Backoff,
    Stopping,
    Stopped,
    // 不可恢复终态(无出边;重开须显式回到 Disabled 再来)。
    Misconfigured,
    TrustRequired,
    NeedsLogin,
    Fatal,
};

const char* ChannelAccountStateName(ChannelAccountState state);
std::optional<ChannelAccountState> ChannelAccountStateFromName(std::string_view name);

// 不可恢复终态判定(Misconfigured/TrustRequired/NeedsLogin/Fatal)。
bool IsUnrecoverableAccountState(ChannelAccountState state);

// 迁移合法性(configuration.md §9 的线性序展开成语义边;不可恢复终态无
// 出边。停机方向任意活跃态可入 Stopping——Ctrl+C/析构走同一条幂等路径,
// 见 bridge-protocol.md §7)。
bool CanTransition(ChannelAccountState from, ChannelAccountState to);

// ---------------------------------------------------------------------------
// 状态机专属 reason 稳定名
// ---------------------------------------------------------------------------

// 除 bridge-protocol.md §6 的 domain 稳定名(protocol_incompatible/
// spawn_failed/process_crashed/login_required/account_revoked 等,由
// DomainErrorStableName 供)外,状态机另有三枚(configuration.md §9):
inline constexpr std::string_view kReasonMisconfigured = "misconfigured";
inline constexpr std::string_view kReasonTrustRequired = "trust_required";
inline constexpr std::string_view kReasonAccountInUse = "account_in_use";
// 状态迁移自身失败(锁拿不到以外,如 replay 账损坏)。
inline constexpr std::string_view kReasonTransitionFailed = "transition_failed";
// 用户/宿主主动停(不算故障)。
inline constexpr std::string_view kReasonStopped = "stopped";
inline constexpr std::string_view kReasonTransportFailed = "transport_failed";
// 停机宽限用尽直接收口(杀树归阶段 5 真进程 transport)。
inline constexpr std::string_view kReasonShutdownTimeout = "shutdown_timeout";

// 不自动重试清单(configuration.md §10:manifest/schema 错、trust 未批、
// 凭据缺失、账号吊销、协议不兼容、状态迁移失败;账号锁被占同属不可自愈)。
// reason 用 bridge 协议的 domain 稳定名与上面的状态机名。
bool ShouldAutoRetry(std::string_view reason);

// ---------------------------------------------------------------------------
// AccountStatusTransition:一次迁移的完整账
// ---------------------------------------------------------------------------

struct AccountStatusTransition {
    std::string channel_id;
    std::string account_id;
    std::int64_t timestamp_ms = 0;
    ChannelAccountState from = ChannelAccountState::Disabled;
    ChannelAccountState to = ChannelAccountState::Disabled;
    std::string reason;       // 稳定名(空 = 正常推进)
    std::string detail;       // 脱敏人话(不得带 token/secret)
    std::int64_t retry_at_ms = 0;  // reason 可重试时给下一次尝试时刻
    int generation = 1;

    nlohmann::json ToJson() const;
    // 严格解析:未知键、缺必填、坏枚举一律拒绝。
    static std::optional<AccountStatusTransition> FromJsonStrict(const nlohmann::json& json,
                                                                 std::string* error);
};

// ---------------------------------------------------------------------------
// 退避(configuration.md §10)
// ---------------------------------------------------------------------------

// 序列 1s, 2s, 4s, 8s, 16s, 30s, 60s;attempt 超界钳在末档 60s。
inline constexpr std::int64_t kBackoffScheduleMs[] = {1000, 2000, 4000, 8000, 16000, 30000,
                                                      60000};

// 第 attempt 次(从 0 数)失败的退避毫秒。jitter_fraction 由调用方给,
// 合法域 [0, 0.10](文档"加 10% jitter");越界钳回域内,不拒——时钟与
// 随机源都归调用方,这里只保证不超过 +10%。
std::int64_t BackoffDelayMs(int attempt, double jitter_fraction);

// 稳定运行多久后退避计数归零(configuration.md §10"成功稳定运行一段后
// 归零";一段 = 5 分钟,首版钉死,不进配置)。
inline constexpr std::int64_t kBackoffResetAfterStableMs = 5 * 60 * 1000;

}  // namespace lubancode::channel
