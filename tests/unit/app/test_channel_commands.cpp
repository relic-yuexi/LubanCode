// 多渠道消息接入单阶段 2:/channels、/channel 命令面册(纯函数部分)。
// 二级解析、总览/详情/体检的行生成,都在这里钉;handler 只管打印与接线。
#include <doctest/doctest.h>

#include "app/commands/channel_commands.hpp"

using namespace lubancode::app;
using namespace lubancode::channel;

namespace {

ChannelManager::AccountSnapshot MakeSnapshot() {
    ChannelManager::AccountSnapshot snapshot;
    snapshot.channel_id = "qqbot";
    snapshot.account_id = "main";
    snapshot.state = ChannelAccountState::Running;
    snapshot.generation = 2;
    snapshot.inbox_pending = 3;
    snapshot.dead_letter_count = 1;
    snapshot.pairing_pending = 2;
    snapshot.pairing_approved = 1;
    snapshot.lock_held = true;
    snapshot.dm_policy = DmPolicy::Pairing;
    snapshot.group_policy = GroupPolicy::Allowlist;
    snapshot.credential = CredentialSource::FromEnv;
    snapshot.ingress_state_counts = {{"durable", 1}, {"queued", 2}};
    AccountStatusTransition transition;
    transition.from = ChannelAccountState::Connecting;
    transition.to = ChannelAccountState::Running;
    transition.reason = "";
    snapshot.recent_transitions.push_back(transition);
    return snapshot;
}

}  // namespace

TEST_CASE("二级解析:子命令与目标") {
    {
        const auto parsed = ParseChannelCommand("");
        CHECK(parsed.action == ChannelCommandAction::Overview);
    }
    {
        const auto parsed = ParseChannelCommand("show qqbot");
        CHECK(parsed.action == ChannelCommandAction::Show);
        CHECK(parsed.channel_id == "qqbot");
        CHECK(parsed.account_id.empty());
    }
    {
        const auto parsed = ParseChannelCommand("doctor qqbot main");
        CHECK(parsed.action == ChannelCommandAction::Doctor);
        CHECK(parsed.channel_id == "qqbot");
        CHECK(parsed.account_id == "main");
    }
    {
        const auto parsed = ParseChannelCommand("  restart   qqbot   main  ");
        CHECK(parsed.action == ChannelCommandAction::Restart);
        CHECK(parsed.channel_id == "qqbot");
        CHECK(parsed.account_id == "main");
    }
    {
        const auto parsed = ParseChannelCommand("frobnicate qqbot");
        CHECK(parsed.action == ChannelCommandAction::Invalid);
        CHECK(parsed.bad_word == "frobnicate");
    }
}

TEST_CASE("总览:没配渠道、配了没挂 manager、配了且在跑") {
    // 没配。
    {
        const auto lines = FormatChannelsOverview(nullptr, nullptr);
        REQUIRE_FALSE(lines.empty());
        CHECK(lines[0].find("没配渠道") != std::string::npos);
    }
    // 配了,普通交互进程(没挂 ChannelManager):显示 gateway not running。
    {
        std::map<std::string, ChannelUserConfig> channels;
        ChannelUserConfig channel;
        channel.enabled = true;
        channel.default_account = "main";
        ChannelAccountUserConfig account;
        account.enabled = true;
        account.secret_env = "QQBOT_SECRET";
        channel.accounts.emplace("main", account);
        channels.emplace("qqbot", channel);

        const auto lines = FormatChannelsOverview(&channels, nullptr);
        bool saw_gateway_hint = false;
        bool saw_account_row = false;
        for (const auto& line : lines) {
            if (line.find("gateway not running") != std::string::npos) saw_gateway_hint = true;
            if (line.find("main") != std::string::npos && line.find("env") != std::string::npos) {
                saw_account_row = true;
            }
        }
        CHECK(saw_account_row);
        CHECK(saw_gateway_hint);
    }
    // 配了且在跑:状态行带 running/pending/dead_letter。
    {
        const auto snapshot = MakeSnapshot();
        const std::vector<ChannelManager::AccountSnapshot> snapshots = {snapshot};
        std::map<std::string, ChannelUserConfig> channels;
        const auto lines = FormatChannelsOverview(&channels, &snapshots);
        bool saw_running = false;
        for (const auto& line : lines) {
            if (line.find("running") != std::string::npos && line.find("qqbot/main") != std::string::npos) {
                saw_running = true;
            }
        }
        CHECK(saw_running);
    }
}

TEST_CASE("show:详情行含状态/策略/密钥来源/水位/迁移账") {
    const auto snapshot = MakeSnapshot();
    const auto lines = FormatChannelShow(&snapshot);
    std::string joined;
    for (const auto& line : lines) joined += line + "\n";
    CHECK(joined.find("running") != std::string::npos);
    CHECK(joined.find("pairing") != std::string::npos);  // dm 策略
    CHECK(joined.find("env") != std::string::npos);      // 密钥来源(只报名)
    CHECK(joined.find("inbox=3") != std::string::npos);
    CHECK(joined.find("dead_letter=1") != std::string::npos);
    CHECK(joined.find("connecting -> running") != std::string::npos);

    // 空指针:一行不在册。
    const auto none = FormatChannelShow(nullptr);
    REQUIRE_FALSE(none.empty());
    CHECK(none[0].find("不在册") != std::string::npos);
}

TEST_CASE("doctor:明文密钥给 warning,缺失给 CredentialsMissing,不打印值") {
    // 明文。
    {
        auto snapshot = MakeSnapshot();
        snapshot.credential = CredentialSource::InlinePlaintext;
        const auto lines = FormatChannelDoctor(&snapshot);
        std::string joined;
        for (const auto& line : lines) joined += line + "\n";
        CHECK(joined.find("WARNING") != std::string::npos);
    }
    // 缺失。
    {
        auto snapshot = MakeSnapshot();
        snapshot.credential = CredentialSource::Missing;
        const auto lines = FormatChannelDoctor(&snapshot);
        std::string joined;
        for (const auto& line : lines) joined += line + "\n";
        CHECK(joined.find("CredentialsMissing") != std::string::npos);
    }
    // env 正常 + 体检不发外部请求的说明。
    {
        const auto snapshot = MakeSnapshot();
        const auto lines = FormatChannelDoctor(&snapshot);
        std::string joined;
        for (const auto& line : lines) joined += line + "\n";
        CHECK(joined.find("体检不发平台请求") != std::string::npos);
        CHECK(joined.find("pairing") != std::string::npos);  // 待审数
    }
    // Backoff 状态:报 retry_at 与次数。
    {
        auto snapshot = MakeSnapshot();
        snapshot.state = ChannelAccountState::Backoff;
        snapshot.retry_at_ms = 1724700001000;
        snapshot.backoff_attempt = 3;
        const auto lines = FormatChannelDoctor(&snapshot);
        std::string joined;
        for (const auto& line : lines) joined += line + "\n";
        CHECK(joined.find("backoff") != std::string::npos);
        CHECK(joined.find("1724700001000") != std::string::npos);
    }
}
