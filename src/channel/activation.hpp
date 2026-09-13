// 渠道真实激活决策(QQ 机器人接入单 Q0)。
//
// 唯一真源 docs/architecture/channels/configuration.md §3(激活决策、
// 决策码冻结表、默认行为表)。一只权威函数,不在 CLI、ChannelManager、
// Package mounting 三处各猜一遍:
//   Gateway 模式 + 包已安装且已信任 + 渠道/账号双层 enabled + 凭据可读
//   + 账号锁齐,五闸全过才 Ready;只有 Ready 可 spawn sidecar(真 spawn
//   归 Q1,本批落决策合同)。普通 CLI/one-shot/App Server 一律
//   NotGatewayMode——零渠道进程、零监听。
//
// 纯函数件:不读配置文件、不碰锁、不起进程;调用方把状态快照递进来。
// 渠道库不反向依赖 package(trust 以 ChannelTrustState 快照传入)。
#pragma once

#include <optional>
#include <string>

#include "channel/channel_config.hpp"

namespace lubancode::channel {

// 宿主进程形态。决定"这屋里根本不许有渠道进程"这道最先的闸。
enum class ChannelProcessMode { InteractiveCli, OneShot, AppServer, Gateway };

// Package 信任快照(mounting 侧从 PackageTrustSnapshot 折进来)。
struct ChannelTrustState {
    bool installed = false;  // 包已安装且 channel.yaml 解析通过
    bool trusted = false;    // 当前内容哈希已过信任门
};

// 凭据状态快照(credentials resolver 的结论折进来;reason 是稳定码,
// 不带凭据值)。
struct ChannelCredentialState {
    bool ready = false;
    std::string reason;  // 稳定码(credentials_missing/secret_file_insecure/...);可空
};

// 账号锁快照(AccountLock 侧折进来)。
struct ChannelLockState {
    bool acquired = false;        // 本实例已持有
    bool held_elsewhere = false;  // 别的活实例持有(活进程,不是假锁)
};

struct ChannelActivationDecision {
    // 决策码冻结(configuration.md §3):顺序即判定次序,先到先得。
    enum class Code {
        DisabledByDefault,  // 没有 channels 配置
        NotGatewayMode,     // 进程不是 Gateway 形态
        PackageUntrusted,   // Package 未安装或未信任
        ChannelDisabled,    // channels.<id>.enabled != true
        AccountDisabled,    // accounts.<id>.enabled != true(或账号不在册)
        CredentialsMissing, // 凭据缺失或解析失败
        AccountInUse,       // 账号锁被另一活实例持有
        Ready,              // 唯一可 spawn sidecar 的状态
    };
    Code code = Code::DisabledByDefault;
    std::string detail;  // 脱敏诊断:不带凭据值、不带整份平台事件

    bool ready() const { return code == Code::Ready; }
};

const char* ChannelActivationCodeName(ChannelActivationDecision::Code code);

// 跑一遍五闸。channel 为 nullopt = channels 段里没有这只渠道
//(DisabledByDefault);账号在 channel->accounts 里找不到按 AccountDisabled。
// channel_id 只进诊断文案,不参与判定。
ChannelActivationDecision ResolveChannelActivation(
    ChannelProcessMode process_mode, const ChannelTrustState& trust,
    const std::optional<ChannelUserConfig>& channel, const std::string& channel_id,
    const std::string& account_id, const ChannelCredentialState& credentials,
    const ChannelLockState& lock);

}  // namespace lubancode::channel
