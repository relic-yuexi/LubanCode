// 渠道激活决策实现(QQ 机器人接入单 Q0)。合同见 activation.hpp;
// 判定次序照 configuration.md §3 决策码冻结表,一闸不过即返,不往下猜。
#include "channel/activation.hpp"

namespace lubancode::channel {

const char* ChannelActivationCodeName(ChannelActivationDecision::Code code) {
    switch (code) {
        case ChannelActivationDecision::Code::DisabledByDefault: return "disabled_by_default";
        case ChannelActivationDecision::Code::NotGatewayMode: return "not_gateway_mode";
        case ChannelActivationDecision::Code::PackageUntrusted: return "package_untrusted";
        case ChannelActivationDecision::Code::ChannelDisabled: return "channel_disabled";
        case ChannelActivationDecision::Code::AccountDisabled: return "account_disabled";
        case ChannelActivationDecision::Code::CredentialsMissing: return "credentials_missing";
        case ChannelActivationDecision::Code::AccountInUse: return "account_in_use";
        case ChannelActivationDecision::Code::Ready: return "ready";
    }
    return "unknown";
}

ChannelActivationDecision ResolveChannelActivation(
    ChannelProcessMode process_mode, const ChannelTrustState& trust,
    const std::optional<ChannelUserConfig>& channel, const std::string& channel_id,
    const std::string& account_id, const ChannelCredentialState& credentials,
    const ChannelLockState& lock) {
    ChannelActivationDecision decision;

    if (!channel.has_value()) {
        decision.code = ChannelActivationDecision::Code::DisabledByDefault;
        decision.detail = "channels 段没有渠道 " + channel_id + " 的配置";
        return decision;
    }
    if (process_mode != ChannelProcessMode::Gateway) {
        // 普通 CLI/one-shot/App Server:零渠道进程、零监听,不是"少一步",
        // 是这道门根本不开(configuration.md §3 默认行为表)。
        decision.code = ChannelActivationDecision::Code::NotGatewayMode;
        decision.detail = "只有 Gateway 形态可起渠道账号";
        return decision;
    }
    if (!trust.installed || !trust.trusted) {
        decision.code = ChannelActivationDecision::Code::PackageUntrusted;
        decision.detail = trust.installed ? "Package 未过信任门(/package trust)"
                                          : "Package 未安装或 channel.yaml 不可用";
        return decision;
    }
    if (!channel->enabled) {
        decision.code = ChannelActivationDecision::Code::ChannelDisabled;
        decision.detail = "channels." + channel_id + ".enabled != true";
        return decision;
    }
    const auto account = channel->accounts.find(account_id);
    if (account == channel->accounts.end()) {
        decision.code = ChannelActivationDecision::Code::AccountDisabled;
        decision.detail = "账号不在册: " + account_id;
        return decision;
    }
    if (!account->second.enabled) {
        decision.code = ChannelActivationDecision::Code::AccountDisabled;
        decision.detail = "accounts." + account_id + ".enabled != true";
        return decision;
    }
    if (!credentials.ready) {
        decision.code = ChannelActivationDecision::Code::CredentialsMissing;
        // reason 是稳定码,不带值;空时给兜底名。
        decision.detail = credentials.reason.empty() ? "凭据缺失或解析失败"
                                                     : credentials.reason;
        return decision;
    }
    if (lock.held_elsewhere) {
        decision.code = ChannelActivationDecision::Code::AccountInUse;
        decision.detail = "账号锁被另一活实例持有";
        return decision;
    }
    decision.code = ChannelActivationDecision::Code::Ready;
    decision.detail = "五闸全过,可启动渠道执行载体";
    return decision;
}

}  // namespace lubancode::channel
