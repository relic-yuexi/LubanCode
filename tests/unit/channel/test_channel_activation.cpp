// 渠道激活决策册(QQ 机器人接入单 Q0)。
// 真源:docs/architecture/channels/configuration.md §3(五闸、决策码冻结表、
// 默认行为表)。纯函数件:全部输入就地造,不起 IO。
//
// 钉的合同:
//   - 普通 CLI/one-shot/App Server 一律 NotGatewayMode——零渠道进程、
//     零监听,不是"少一步",是这道门不开;
//   - 只有 Ready 可 spawn sidecar(真 spawn 归 Q1,本批钉决策);
//   - 判定次序照决策码表,一闸不过即返。
#include <doctest/doctest.h>

#include <optional>

#include "channel/activation.hpp"

using namespace lubancode::channel;

namespace {

ChannelUserConfig ReadyChannel() {
    ChannelUserConfig channel;
    channel.enabled = true;
    ChannelAccountUserConfig account;
    account.enabled = true;
    channel.accounts.emplace("main", std::move(account));
    return channel;
}

struct AllGatesReady {
    ChannelTrustState trust{true, true};
    std::optional<ChannelUserConfig> channel = ReadyChannel();
    ChannelCredentialState credentials{true, ""};
    ChannelLockState lock{true, false};
};

ChannelActivationDecision Decide(ChannelProcessMode mode, const AllGatesReady& ready,
                                 const std::string& account_id = "main") {
    return ResolveChannelActivation(mode, ready.trust, ready.channel, "qqbot", account_id,
                                    ready.credentials, ready.lock);
}

}  // namespace

TEST_CASE("五闸全过才 Ready") {
    AllGatesReady ready;
    const auto decision = Decide(ChannelProcessMode::Gateway, ready);
    CHECK(decision.code == ChannelActivationDecision::Code::Ready);
    CHECK(decision.ready());
    CHECK(std::string(ChannelActivationCodeName(decision.code)) == "ready");
}

TEST_CASE("普通 CLI/one-shot/App Server:NotGatewayMode,零渠道进程零监听") {
    AllGatesReady ready;
    // 配置 enabled、信任、凭据、锁全齐——只换进程形态,门照样不开。
    for (const ChannelProcessMode mode :
         {ChannelProcessMode::InteractiveCli, ChannelProcessMode::OneShot,
          ChannelProcessMode::AppServer}) {
        const auto decision = Decide(mode, ready);
        CHECK(decision.code == ChannelActivationDecision::Code::NotGatewayMode);
        CHECK_FALSE(decision.ready());
    }
    CHECK(std::string(ChannelActivationCodeName(
              ChannelActivationDecision::Code::NotGatewayMode)) == "not_gateway_mode");
}

TEST_CASE("没有 channels 配置:DisabledByDefault,先于其余闸") {
    // 即便不是 Gateway 形态,没配置也先报 DisabledByDefault(判定次序照
    // 决策码表)。
    AllGatesReady ready;
    ready.channel = std::nullopt;
    const auto decision = Decide(ChannelProcessMode::InteractiveCli, ready);
    CHECK(decision.code == ChannelActivationDecision::Code::DisabledByDefault);
    CHECK(decision.detail.find("qqbot") != std::string::npos);
}

TEST_CASE("包未安装/未信任:PackageUntrusted") {
    AllGatesReady ready;
    ready.trust.installed = false;
    ready.trust.trusted = false;
    CHECK(Decide(ChannelProcessMode::Gateway, ready).code ==
          ChannelActivationDecision::Code::PackageUntrusted);

    AllGatesReady installed_only;
    installed_only.trust.installed = true;
    installed_only.trust.trusted = false;
    const auto decision = Decide(ChannelProcessMode::Gateway, installed_only);
    CHECK(decision.code == ChannelActivationDecision::Code::PackageUntrusted);
    CHECK(decision.detail.find("信任") != std::string::npos);
}

TEST_CASE("双层 enabled:渠道关/账号关/账号不在册分别报") {
    AllGatesReady ready;
    ready.channel->enabled = false;
    CHECK(Decide(ChannelProcessMode::Gateway, ready).code ==
          ChannelActivationDecision::Code::ChannelDisabled);

    AllGatesReady account_off;
    account_off.channel->accounts.at("main").enabled = false;
    const auto off = Decide(ChannelProcessMode::Gateway, account_off);
    CHECK(off.code == ChannelActivationDecision::Code::AccountDisabled);
    CHECK(off.detail.find("main") != std::string::npos);

    AllGatesReady account_missing;
    CHECK(Decide(ChannelProcessMode::Gateway, account_missing, "ghost").code ==
          ChannelActivationDecision::Code::AccountDisabled);
}

TEST_CASE("凭据不可读:CredentialsMissing,reason 只报稳定码不报值") {
    AllGatesReady ready;
    ready.credentials.ready = false;
    ready.credentials.reason = "secret_file_insecure";
    const auto decision = Decide(ChannelProcessMode::Gateway, ready);
    CHECK(decision.code == ChannelActivationDecision::Code::CredentialsMissing);
    CHECK(decision.detail == "secret_file_insecure");
    // detail 里没有密钥值的容身之处:结构上只装稳定码/短语。
    ready.credentials.reason.clear();
    CHECK_FALSE(Decide(ChannelProcessMode::Gateway, ready).detail.empty());
}

TEST_CASE("账号锁被别人持有:AccountInUse") {
    AllGatesReady ready;
    ready.lock.acquired = false;
    ready.lock.held_elsewhere = true;
    CHECK(Decide(ChannelProcessMode::Gateway, ready).code ==
          ChannelActivationDecision::Code::AccountInUse);
}

TEST_CASE("判定次序:配置缺失先于非 Gateway;非 Gateway 先于信任") {
    {
        AllGatesReady ready;
        ready.channel = std::nullopt;
        ready.trust = ChannelTrustState{false, false};
        CHECK(Decide(ChannelProcessMode::OneShot, ready).code ==
              ChannelActivationDecision::Code::DisabledByDefault);
    }
    {
        AllGatesReady ready;
        ready.trust = ChannelTrustState{false, false};
        CHECK(Decide(ChannelProcessMode::OneShot, ready).code ==
              ChannelActivationDecision::Code::NotGatewayMode);
    }
}
