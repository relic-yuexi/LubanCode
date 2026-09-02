// 多渠道消息接入单阶段 2:channels 配置段解析与合并层级册。
// 唯一真源 docs/architecture/channels/configuration.md §1/§2/§7。
// 规矩:严格解析(未知字段/坏枚举/坏类型报错);项目级 channels 一律明拒;
// 老配置没 channels 段零行为变化。
#include <doctest/doctest.h>

#include <nlohmann/json.hpp>

#include "channel/channel_config.hpp"
#include "config/config.hpp"

using namespace lubancode::channel;
using lubancode::config::LubancodeEnvValues;

namespace {

std::optional<std::map<std::string, ChannelUserConfig>> Parse(const std::string& json_text,
                                                              std::string* error = nullptr) {
    const nlohmann::json json = nlohmann::json::parse(json_text);
    return ParseChannelsUserConfig(json, "config.json", error);
}

}  // namespace

TEST_CASE("完整样例(configuration.md §1)解析:字段与默认值") {
    const auto parsed = Parse(R"({
      "qqbot": {
        "enabled": true,
        "default_account": "main",
        "accounts": {
          "main": {
            "enabled": true,
            "transport": "websocket",
            "app_id": "123456",
            "secret_env": "QQBOT_CLIENT_SECRET",
            "dm_policy": "allowlist",
            "allow_from": ["owner_openid"],
            "group_policy": "allowlist",
            "group_allow_from": ["group_openid"],
            "require_mention": true,
            "agent": "general-purpose",
            "reply": {"mode": "block", "tool_progress": false}
          }
        }
      }
    })");
    REQUIRE(parsed.has_value());
    REQUIRE(parsed->count("qqbot") == 1);
    const ChannelUserConfig& channel = parsed->at("qqbot");
    CHECK(channel.enabled);
    CHECK(channel.default_account == "main");
    REQUIRE(channel.accounts.count("main") == 1);
    const ChannelAccountUserConfig& account = channel.accounts.at("main");
    CHECK(account.enabled);
    CHECK(account.transport == "websocket");
    CHECK(account.secret_env == std::optional<std::string>("QQBOT_CLIENT_SECRET"));
    CHECK(account.dm_policy == DmPolicy::Allowlist);
    REQUIRE(account.allow_from.size() == 1);
    CHECK(account.allow_from[0] == "owner_openid");
    CHECK(account.group_policy == GroupPolicy::Allowlist);
    CHECK(account.require_mention);
    CHECK(account.agent == "general-purpose");
    CHECK(account.reply.mode == ReplyMode::Block);
    CHECK_FALSE(account.reply.tool_progress);
    CHECK(DescribeCredentialSource(account) == CredentialSource::FromEnv);
}

TEST_CASE("默认值:dm=pairing、group=allowlist、mention=true、bots=false") {
    const auto parsed = Parse(R"({"qqbot": {"accounts": {"main": {}}}})");
    REQUIRE(parsed.has_value());
    const auto& account = parsed->at("qqbot").accounts.at("main");
    CHECK_FALSE(account.enabled);
    CHECK(account.dm_policy == DmPolicy::Pairing);
    CHECK(account.group_policy == GroupPolicy::Allowlist);
    CHECK(account.require_mention);
    CHECK_FALSE(account.allow_bots);
    CHECK(account.reply.mode == ReplyMode::Block);
    CHECK(DescribeCredentialSource(account) == CredentialSource::Missing);
}

TEST_CASE("严格性:未知字段、坏枚举、坏类型逐项报错") {
    std::string error;
    CHECK_FALSE(Parse(R"({"qqbot": {"nope": 1}})", &error).has_value());
    CHECK(error.find("channels.qqbot.nope") != std::string::npos);

    CHECK_FALSE(Parse(R"({"qqbot": {"accounts": {"main": {"nope": 1}}}})", &error).has_value());
    CHECK(error.find("accounts.main.nope") != std::string::npos);

    CHECK_FALSE(Parse(R"({"qqbot": {"accounts": {"main": {"dm_policy": "yolo"}}}})", &error)
                    .has_value());
    CHECK(error.find("dm_policy") != std::string::npos);

    CHECK_FALSE(Parse(R"({"qqbot": {"accounts": {"main": {"enabled": "yes"}}}})", &error)
                    .has_value());
    CHECK(error.find("enabled") != std::string::npos);

    CHECK_FALSE(Parse(R"({"qqbot": {"accounts": {"main": {"reply": {"mode": "instant"}}}}})",
                      &error)
                    .has_value());
    CHECK(error.find("reply.mode") != std::string::npos);

    CHECK_FALSE(Parse(R"({"qqbot": {"accounts": {"main": {"allow_from": "owner"}}}})", &error)
                    .has_value());
    CHECK(error.find("allow_from") != std::string::npos);
}

TEST_CASE("密钥来源三种 + 明文兼容") {
    const auto parsed = Parse(R"({
      "a": {"accounts": {"m": {"secret_env": "ENV_NAME"}}},
      "b": {"accounts": {"m": {"secret_file": "/secure/qq.key"}}},
      "c": {"accounts": {"m": {"secret": "plaintext-here"}}}
    })");
    REQUIRE(parsed.has_value());
    CHECK(DescribeCredentialSource(parsed->at("a").accounts.at("m")) == CredentialSource::FromEnv);
    CHECK(DescribeCredentialSource(parsed->at("b").accounts.at("m")) == CredentialSource::FromFile);
    CHECK(DescribeCredentialSource(parsed->at("c").accounts.at("m")) ==
          CredentialSource::InlinePlaintext);
}

TEST_CASE("config 合并:全局 channels 进 Config,项目级 channels 明拒,没段零变化") {
    const LubancodeEnvValues env{};

    // 没配:channels 空 map,不报错。
    {
        const auto merged = lubancode::config::MergeConfig(env, std::nullopt, std::nullopt);
        REQUIRE(merged.has_value());
        CHECK(merged->config.channels.empty());
    }
    // 全局配:进 Config。
    {
        const auto global = lubancode::config::ParseFileConfigJson(
            R"({"channels": {"qqbot": {"accounts": {"main": {"enabled": true}}}}})",
            "global.json");
        REQUIRE(global.has_value());
        const auto merged = lubancode::config::MergeConfig(env, std::nullopt, *global);
        REQUIRE(merged.has_value());
        REQUIRE(merged->config.channels.count("qqbot") == 1);
        CHECK(merged->config.channels.at("qqbot").accounts.at("main").enabled);
    }
    // 项目级出现 channels:整份加载明拒(configuration.md §2 层级规矩)。
    {
        const auto project = lubancode::config::ParseFileConfigJson(
            R"({"channels": {"qqbot": {"enabled": true}}})", "project.json");
        REQUIRE(project.has_value());
        const auto merged = lubancode::config::MergeConfig(env, *project, std::nullopt);
        REQUIRE_FALSE(merged.has_value());
        CHECK(merged.error().find("channels") != std::string::npos);
        CHECK(merged.error().find("项目") != std::string::npos);
    }
    // 坏 channels 段(类型错):ParseFileConfigJson 就报,不进合并。
    {
        const auto bad = lubancode::config::ParseFileConfigJson(R"({"channels": []})", "global.json");
        CHECK_FALSE(bad.has_value());
    }
}
