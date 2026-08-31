// 多渠道消息接入单阶段 2:账号状态机、退避、迁移账的纯逻辑册。
// 唯一真源 docs/architecture/channels/configuration.md §9-10。
#include <doctest/doctest.h>

#include "channel/account_state.hpp"

using namespace lubancode::channel;

TEST_CASE("账号状态名与枚举往返") {
    for (const ChannelAccountState state :
         {ChannelAccountState::Disabled, ChannelAccountState::Validating,
          ChannelAccountState::Starting, ChannelAccountState::Authenticating,
          ChannelAccountState::Connecting, ChannelAccountState::Running,
          ChannelAccountState::Degraded, ChannelAccountState::Backoff,
          ChannelAccountState::Stopping, ChannelAccountState::Stopped,
          ChannelAccountState::Misconfigured, ChannelAccountState::TrustRequired,
          ChannelAccountState::NeedsLogin, ChannelAccountState::Fatal}) {
        const char* name = ChannelAccountStateName(state);
        CHECK(ChannelAccountStateFromName(name).has_value());
        CHECK(*ChannelAccountStateFromName(name) == state);
    }
    CHECK_FALSE(ChannelAccountStateFromName("running2").has_value());
    CHECK_FALSE(ChannelAccountStateFromName("").has_value());
}

TEST_CASE("不可恢复终态只有四枚,且无出边") {
    CHECK(IsUnrecoverableAccountState(ChannelAccountState::Misconfigured));
    CHECK(IsUnrecoverableAccountState(ChannelAccountState::TrustRequired));
    CHECK(IsUnrecoverableAccountState(ChannelAccountState::NeedsLogin));
    CHECK(IsUnrecoverableAccountState(ChannelAccountState::Fatal));
    CHECK_FALSE(IsUnrecoverableAccountState(ChannelAccountState::Stopped));
    CHECK_FALSE(IsUnrecoverableAccountState(ChannelAccountState::Backoff));
    for (const ChannelAccountState terminal :
         {ChannelAccountState::Misconfigured, ChannelAccountState::TrustRequired,
          ChannelAccountState::NeedsLogin, ChannelAccountState::Fatal}) {
        for (const ChannelAccountState to :
             {ChannelAccountState::Validating, ChannelAccountState::Starting,
              ChannelAccountState::Running, ChannelAccountState::Stopping}) {
            CHECK_FALSE(CanTransition(terminal, to));
        }
    }
}

TEST_CASE("起账号的主线路径合法") {
    CHECK(CanTransition(ChannelAccountState::Disabled, ChannelAccountState::Validating));
    CHECK(CanTransition(ChannelAccountState::Validating, ChannelAccountState::Starting));
    CHECK(CanTransition(ChannelAccountState::Starting, ChannelAccountState::Authenticating));
    CHECK(CanTransition(ChannelAccountState::Authenticating, ChannelAccountState::Connecting));
    CHECK(CanTransition(ChannelAccountState::Connecting, ChannelAccountState::Running));
    // 重复迁移与跳步非法。
    CHECK_FALSE(CanTransition(ChannelAccountState::Running, ChannelAccountState::Running));
    CHECK_FALSE(CanTransition(ChannelAccountState::Disabled, ChannelAccountState::Running));
    CHECK_FALSE(CanTransition(ChannelAccountState::Stopped, ChannelAccountState::Running));
    // 重开的复位边:Stopped -> Validating。
    CHECK(CanTransition(ChannelAccountState::Stopped, ChannelAccountState::Validating));
}

TEST_CASE("停机方向任何活跃态可入,Stopping -> Stopped") {
    for (const ChannelAccountState from :
         {ChannelAccountState::Validating, ChannelAccountState::Starting,
          ChannelAccountState::Authenticating, ChannelAccountState::Connecting,
          ChannelAccountState::Running, ChannelAccountState::Degraded,
          ChannelAccountState::Backoff}) {
        CHECK(CanTransition(from, ChannelAccountState::Stopping));
    }
    CHECK(CanTransition(ChannelAccountState::Stopping, ChannelAccountState::Stopped));
    CHECK(CanTransition(ChannelAccountState::Backoff, ChannelAccountState::Connecting));
    CHECK(CanTransition(ChannelAccountState::Running, ChannelAccountState::Degraded));
    CHECK(CanTransition(ChannelAccountState::Degraded, ChannelAccountState::Backoff));
    CHECK(CanTransition(ChannelAccountState::Degraded, ChannelAccountState::Running));
}

TEST_CASE("不自动重试清单(configuration.md §10)") {
    CHECK_FALSE(ShouldAutoRetry("misconfigured"));
    CHECK_FALSE(ShouldAutoRetry("trust_required"));
    CHECK_FALSE(ShouldAutoRetry("login_required"));
    CHECK_FALSE(ShouldAutoRetry("account_revoked"));
    CHECK_FALSE(ShouldAutoRetry("protocol_incompatible"));
    CHECK_FALSE(ShouldAutoRetry("account_in_use"));
    CHECK_FALSE(ShouldAutoRetry("transition_failed"));
    // 可重试的故障。
    CHECK(ShouldAutoRetry("spawn_failed"));
    CHECK(ShouldAutoRetry("process_crashed"));
    CHECK(ShouldAutoRetry("transport_failed"));
    CHECK(ShouldAutoRetry("shutdown_timeout"));
    CHECK(ShouldAutoRetry("rate_limited"));
    // 空与主动停不算故障。
    CHECK_FALSE(ShouldAutoRetry(""));
    CHECK_FALSE(ShouldAutoRetry("stopped"));
}

TEST_CASE("退避序列 1s..60s,越界钳末档,jitter 封顶 10%") {
    CHECK(BackoffDelayMs(0, 0.0) == 1000);
    CHECK(BackoffDelayMs(1, 0.0) == 2000);
    CHECK(BackoffDelayMs(2, 0.0) == 4000);
    CHECK(BackoffDelayMs(3, 0.0) == 8000);
    CHECK(BackoffDelayMs(4, 0.0) == 16000);
    CHECK(BackoffDelayMs(5, 0.0) == 30000);
    CHECK(BackoffDelayMs(6, 0.0) == 60000);
    CHECK(BackoffDelayMs(7, 0.0) == 60000);
    CHECK(BackoffDelayMs(100, 0.0) == 60000);
    // jitter 合法域 [0, 0.10]。
    CHECK(BackoffDelayMs(0, 0.10) == 1100);
    CHECK(BackoffDelayMs(0, 0.05) == 1050);
    // 越界钳回。
    CHECK(BackoffDelayMs(0, 0.5) == 1100);
    CHECK(BackoffDelayMs(0, -1.0) == 1000);
}

TEST_CASE("迁移账的 JSON 往返严格") {
    AccountStatusTransition transition;
    transition.channel_id = "qqbot";
    transition.account_id = "main";
    transition.timestamp_ms = 1724700000000;
    transition.from = ChannelAccountState::Running;
    transition.to = ChannelAccountState::Backoff;
    transition.reason = "transport_failed";
    transition.detail = "websocket 断线";
    transition.retry_at_ms = 1724700001000;
    transition.generation = 3;

    const nlohmann::json json = transition.ToJson();
    std::string error;
    const auto back = AccountStatusTransition::FromJsonStrict(json, &error);
    REQUIRE(back.has_value());
    CHECK(back->channel_id == transition.channel_id);
    CHECK(back->to == ChannelAccountState::Backoff);
    CHECK(back->generation == 3);
    CHECK(back->retry_at_ms == 1724700001000);

    // 未知字段拒绝。
    nlohmann::json bad = json;
    bad["extra"] = 1;
    CHECK_FALSE(AccountStatusTransition::FromJsonStrict(bad, &error).has_value());
    // 缺字段拒绝。
    nlohmann::json missing = json;
    missing.erase("reason");
    CHECK_FALSE(AccountStatusTransition::FromJsonStrict(missing, &error).has_value());
    // 坏枚举拒绝。
    nlohmann::json bad_state = json;
    bad_state["to"] = "sprinting";
    CHECK_FALSE(AccountStatusTransition::FromJsonStrict(bad_state, &error).has_value());
}
