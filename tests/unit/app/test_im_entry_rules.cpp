// im 目标裁决规则册(§6.1)+ 启动过滤器(§6.0"选择只成启动过滤器")。
// 纯函数断言:显式 > 最近 > default_account > 唯一;停用账号不暗改;
// 未知平台报错不写空壳;过滤器不改全局配置语义。
#include <doctest/doctest.h>

#include <map>
#include <optional>
#include <string>

#include "app/gateway_launch.hpp"
#include "app/im_entry.hpp"
#include "config/im_preferences.hpp"

using namespace lubancode::app;
using namespace lubancode::channel;
using namespace lubancode::config;

namespace {

ChannelAccountUserConfig MakeAccount(bool enabled, const std::string& app_id = "app") {
    ChannelAccountUserConfig account;
    account.enabled = enabled;
    account.app_id = app_id;
    return account;
}

ChannelUserConfig MakeChannel(bool enabled,
                              std::map<std::string, ChannelAccountUserConfig> accounts,
                              const std::string& default_account = std::string()) {
    ChannelUserConfig channel;
    channel.enabled = enabled;
    channel.accounts = std::move(accounts);
    channel.default_account = default_account;
    return channel;
}

ImPreferences Prefs(const std::string& channel, const std::string& account) {
    ImPreferences preferences;
    preferences.last = ImRecentSelection{channel, account};
    preferences.recent_account[channel] = account;
    return preferences;
}

}  // namespace

TEST_CASE("显式平台+账号为准:可启动直达,停用走询问,没配过进向导") {
    std::map<std::string, ChannelUserConfig> channels;
    channels["qqbot"] = MakeChannel(
        true, {{"main", MakeAccount(true)}, {"second", MakeAccount(false)}}, "main");
    ImPreferences no_prefs;

    SUBCASE("显式可启动账号") {
        ImTargetQuery query;
        query.channels = &channels;
        query.preferences = &no_prefs;
        query.explicit_channel = "qqbot";
        query.explicit_account = "main";
        const auto resolution = ResolveImTarget(query);
        REQUIRE(resolution.status == ImTargetResolution::Status::ReadyUnique);
        REQUIRE(resolution.target.has_value());
        CHECK(resolution.target->channel_id == "qqbot");
        CHECK(resolution.target->account_id == "main");
    }
    SUBCASE("显式停用账号:不暗改,标 DisabledAccount") {
        ImTargetQuery query;
        query.channels = &channels;
        query.preferences = &no_prefs;
        query.explicit_channel = "qqbot";
        query.explicit_account = "second";
        const auto resolution = ResolveImTarget(query);
        REQUIRE(resolution.status == ImTargetResolution::Status::DisabledAccount);
        CHECK(resolution.target->account_id == "second");
    }
    SUBCASE("显式没配过的账号:MissingAccount,不悄悄连别的") {
        ImTargetQuery query;
        query.channels = &channels;
        ImPreferences stale_prefs = Prefs("qqbot", "main");  // 最近也不能顶替显式目标
        query.preferences = &stale_prefs;
        query.explicit_channel = "qqbot";
        query.explicit_account = "ghost";
        const auto resolution = ResolveImTarget(query);
        REQUIRE(resolution.status == ImTargetResolution::Status::MissingAccount);
        CHECK_FALSE(resolution.target.has_value());
    }
    SUBCASE("未知平台:报错不写空壳") {
        ImTargetQuery query;
        query.channels = &channels;
        query.explicit_channel = "telegram";
        const auto resolution = ResolveImTarget(query);
        REQUIRE(resolution.status == ImTargetResolution::Status::UnknownPlatform);
    }
    SUBCASE("未实现平台:feishu 尚未支持") {
        ImTargetQuery query;
        query.channels = &channels;
        query.explicit_channel = "feishu";
        const auto resolution = ResolveImTarget(query);
        REQUIRE(resolution.status == ImTargetResolution::Status::PlatformNotImplemented);
    }
}

TEST_CASE("只指定平台:平台最近账号 > default_account > 唯一;多个弹选择") {
    std::map<std::string, ChannelUserConfig> channels;
    ImPreferences prefs;

    SUBCASE("平台最近账号优先于 default_account(全局最近不覆盖显式平台)") {
        channels["qqbot"] = MakeChannel(
            true, {{"main", MakeAccount(true)}, {"work", MakeAccount(true)}}, "main");
        prefs = Prefs("qqbot", "work");  // 全局最近=qqbot/work
        ImTargetQuery query;
        query.channels = &channels;
        query.preferences = &prefs;
        query.explicit_channel = "qqbot";
        const auto resolution = ResolveImTarget(query);
        REQUIRE(resolution.status == ImTargetResolution::Status::ReadyUnique);
        CHECK(resolution.target->account_id == "work");
    }
    SUBCASE("平台最近悬空时落 default_account") {
        channels["qqbot"] = MakeChannel(
            true, {{"main", MakeAccount(true)}, {"work", MakeAccount(true)}}, "main");
        prefs = Prefs("qqbot", "deleted-account");
        ImTargetQuery query;
        query.channels = &channels;
        query.preferences = &prefs;
        query.explicit_channel = "qqbot";
        const auto resolution = ResolveImTarget(query);
        REQUIRE(resolution.status == ImTargetResolution::Status::ReadyUnique);
        CHECK(resolution.target->account_id == "main");
    }
    SUBCASE("无最近无默认,唯一账号自动选") {
        channels["qqbot"] =
            MakeChannel(true, {{"only", MakeAccount(true)}});
        ImTargetQuery query;
        query.channels = &channels;
        query.explicit_channel = "qqbot";
        const auto resolution = ResolveImTarget(query);
        REQUIRE(resolution.status == ImTargetResolution::Status::ReadyUnique);
        CHECK(resolution.target->account_id == "only");
    }
    SUBCASE("多账号无最近无默认:NeedSelect 带全量候选") {
        channels["qqbot"] =
            MakeChannel(true, {{"a", MakeAccount(true)}, {"b", MakeAccount(false)}});
        ImTargetQuery query;
        query.channels = &channels;
        query.explicit_channel = "qqbot";
        const auto resolution = ResolveImTarget(query);
        REQUIRE(resolution.status == ImTargetResolution::Status::NeedSelect);
        REQUIRE(resolution.candidates.size() == 2);
        CHECK(resolution.candidates[0].ref.account_id == "a");
        CHECK(resolution.candidates[0].enabled);
        CHECK_FALSE(resolution.candidates[1].enabled);  // 停用照列,标已停用
    }
}

TEST_CASE("未指定平台:最近选择 > 唯一可用 > 列表/向导") {
    std::map<std::string, ChannelUserConfig> channels;

    SUBCASE("有效最近选择直接用") {
        channels["qqbot"] = MakeChannel(true, {{"main", MakeAccount(true)}});
        ImPreferences prefs = Prefs("qqbot", "main");
        ImTargetQuery query;
        query.channels = &channels;
        query.preferences = &prefs;
        const auto resolution = ResolveImTarget(query);
        REQUIRE(resolution.status == ImTargetResolution::Status::ReadyUnique);
    }
    SUBCASE("最近悬空(账号被删):回唯一可用,不复活已删账号") {
        channels["qqbot"] = MakeChannel(true, {{"main", MakeAccount(true)}});
        ImPreferences prefs = Prefs("qqbot", "deleted");
        ImTargetQuery query;
        query.channels = &channels;
        query.preferences = &prefs;
        const auto resolution = ResolveImTarget(query);
        REQUIRE(resolution.status == ImTargetResolution::Status::ReadyUnique);
        CHECK(resolution.target->account_id == "main");
    }
    SUBCASE("多个可用:NeedSelect 全量候选") {
        channels["qqbot"] =
            MakeChannel(true, {{"a", MakeAccount(true)}, {"b", MakeAccount(true)}});
        ImTargetQuery query;
        query.channels = &channels;
        const auto resolution = ResolveImTarget(query);
        REQUIRE(resolution.status == ImTargetResolution::Status::NeedSelect);
        CHECK(resolution.candidates.size() == 2);
    }
    SUBCASE("唯一账号但停用:DisabledAccount(问启用,不暗改)") {
        channels["qqbot"] = MakeChannel(true, {{"main", MakeAccount(false)}});
        ImTargetQuery query;
        query.channels = &channels;
        const auto resolution = ResolveImTarget(query);
        REQUIRE(resolution.status == ImTargetResolution::Status::DisabledAccount);
    }
    SUBCASE("渠道层 enabled=false 也算停用") {
        channels["qqbot"] = MakeChannel(false, {{"main", MakeAccount(true)}});
        ImTargetQuery query;
        query.channels = &channels;
        const auto resolution = ResolveImTarget(query);
        REQUIRE(resolution.status == ImTargetResolution::Status::DisabledAccount);
    }
    SUBCASE("什么都没配:NeedSetup 进向导") {
        ImTargetQuery query;
        query.channels = &channels;
        const auto resolution = ResolveImTarget(query);
        REQUIRE(resolution.status == ImTargetResolution::Status::NeedSetup);
    }
    SUBCASE("--select 恒开列表(最近有效也不直达)") {
        channels["qqbot"] = MakeChannel(true, {{"main", MakeAccount(true)}});
        ImPreferences prefs = Prefs("qqbot", "main");
        ImTargetQuery query;
        query.channels = &channels;
        query.preferences = &prefs;
        query.force_select = true;
        const auto resolution = ResolveImTarget(query);
        REQUIRE(resolution.status == ImTargetResolution::Status::NeedSelect);
        CHECK(resolution.candidates.size() == 1);
    }
}

TEST_CASE("ImTargetConfigured:在册判定(悬空/在册分得清)") {
    std::map<std::string, ChannelUserConfig> channels;
    channels["qqbot"] = MakeChannel(true, {{"main", MakeAccount(false)}});
    CHECK(ImTargetConfigured(channels, ChannelAccountRef{"qqbot", "main"}));
    CHECK_FALSE(ImTargetConfigured(channels, ChannelAccountRef{"qqbot", "ghost"}));
    CHECK_FALSE(ImTargetConfigured(channels, ChannelAccountRef{"feishu", "main"}));
}

TEST_CASE("FilterConfigForChannelAccount:选择只成启动过滤器,不改全局语义") {
    lubancode::config::Config config;
    {
        ChannelUserConfig qq = MakeChannel(
            true, {{"main", MakeAccount(true, "app-main")},
                   {"second", MakeAccount(true, "app-second")}},
            "main");
        qq.bindings.push_back(ChannelBindingConfig{});  // 渠道层账:保留
        ChannelUserConfig other = MakeChannel(true, {{"x", MakeAccount(true)}});
        config.channels = {{"qqbot", std::move(qq)}, {"other", std::move(other)}};
    }

    // 无过滤器:原样副本。
    const auto unfiltered = FilterConfigForChannelAccount(config, std::nullopt);
    CHECK(unfiltered.channels.size() == 2);

    // 单账号过滤器:只剩所选渠道+账号;渠道层 bindings/default 保留。
    const auto filtered =
        FilterConfigForChannelAccount(config, ChannelAccountRef{"qqbot", "second"});
    REQUIRE(filtered.channels.size() == 1);
    REQUIRE(filtered.channels.count("qqbot") == 1);
    const ChannelUserConfig& channel = filtered.channels.at("qqbot");
    REQUIRE(channel.accounts.size() == 1);
    REQUIRE(channel.accounts.count("second") == 1);
    CHECK(channel.accounts.at("second").app_id == "app-second");
    CHECK(channel.default_account == "main");  // 渠道层账不动
    CHECK_FALSE(channel.bindings.empty());

    // 过滤是副本:全局配置原样(其他账号没被改 enabled,更没被删)。
    REQUIRE(config.channels.at("qqbot").accounts.count("main") == 1);
    REQUIRE(config.channels.count("other") == 1);
}
