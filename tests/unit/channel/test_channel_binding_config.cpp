// 渠道配置 bindings/group_scope 段解析单测(多渠道消息接入单阶段 3)。
// 真源:docs/architecture/channels/configuration.md §8(binding 冻结形状)。

#include <doctest/doctest.h>

#include <nlohmann/json.hpp>

#include "channel/channel_config.hpp"

using namespace lubancode::channel;

namespace {

std::optional<std::map<std::string, ChannelUserConfig>> Parse(const std::string& json,
                                                              std::string* error) {
    const auto parsed = nlohmann::json::parse(json, nullptr, /*allow_exceptions=*/false);
    if (parsed.is_discarded()) {
        *error = "bad json";
        return std::nullopt;
    }
    return ParseChannelsUserConfig(parsed, "config.json", error);
}

}  // namespace

TEST_CASE("bindings 解析:冻结形状全字段") {
    const auto config = Parse(R"({
      "qqbot": {
        "enabled": true,
        "accounts": {"main": {"enabled": true}},
        "bindings": [
          {
            "agent": "ops-agent",
            "match": {
              "account": "main",
              "conversation": {"kind": "group", "id": "group_openid"},
              "thread": "t-1"
            },
            "policy": {
              "tools": {"allow": ["read_file", "search"], "deny": ["shell"]},
              "memory": {"user": true, "project": false}
            }
          }
        ]
      }
    })",
                              nullptr);
    REQUIRE(config.has_value());
    REQUIRE(config->at("qqbot").bindings.size() == 1);
    const auto& binding = config->at("qqbot").bindings[0];
    CHECK(binding.agent == "ops-agent");
    CHECK(binding.match.channel.empty());  // 渠道层可省 = 本渠道
    CHECK(binding.match.account == "main");
    REQUIRE(binding.match.conversation.has_value());
    CHECK(binding.match.conversation->kind == "group");
    CHECK(binding.match.conversation->id == "group_openid");
    CHECK(binding.match.thread_id == "t-1");
    REQUIRE(binding.policy.tools.allow.size() == 2);
    CHECK(binding.policy.tools.allow[0] == "read_file");
    REQUIRE(binding.policy.tools.deny.size() == 1);
    CHECK(binding.policy.tools.deny[0] == "shell");
    REQUIRE(binding.policy.memory.has_value());
    CHECK(binding.policy.memory->user.value_or(false));
    CHECK_FALSE(binding.policy.memory->project.value_or(true));
}

TEST_CASE("bindings 解析:match.channel 指别家渠道明拒") {
    std::string error;
    const auto config = Parse(R"({
      "qqbot": {
        "accounts": {"main": {}},
        "bindings": [{"agent": "a", "match": {"channel": "weixin"}}]
      }
    })",
                              &error);
    CHECK_FALSE(config.has_value());
    CHECK(error.find("不一致") != std::string::npos);
}

TEST_CASE("bindings 解析:缺 match、未知字段、坏类型明拒") {
    SUBCASE("缺 match") {
        std::string error;
        CHECK_FALSE(Parse(R"({"qqbot": {"bindings": [{"agent": "a"}]}})", &error).has_value());
        CHECK(error.find("缺 match") != std::string::npos);
    }
    SUBCASE("binding 里塞未知字段") {
        std::string error;
        CHECK_FALSE(Parse(R"({"qqbot": {"bindings": [{"match": {}, "who": 1}]}})", &error)
                        .has_value());
        CHECK(error.find("认不得的字段") != std::string::npos);
    }
    SUBCASE("policy.tools.allow 不是数组") {
        std::string error;
        CHECK_FALSE(Parse(
                        R"({"qqbot": {"bindings": [{"match": {}, "policy": {"tools": {"allow": "read"}}}]}})",
                        &error)
                        .has_value());
        CHECK(error.find("必须是字符串数组") != std::string::npos);
    }
    SUBCASE("conversation.kind 认不得") {
        std::string error;
        CHECK_FALSE(Parse(R"({"qqbot": {"bindings": [{"match": {"conversation": {"kind": "dm"}}}]}})",
                          &error)
                        .has_value());
        CHECK(error.find("只认 direct/group/guild/channel/thread") != std::string::npos);
    }
}

TEST_CASE("group_scope 解析:四档合法,别的明拒") {
    for (const std::string scope :
         {"group", "group_sender", "group_thread", "group_thread_sender"}) {
        std::string error;
        nlohmann::json channels = nlohmann::json::object();
        nlohmann::json account = nlohmann::json::object();
        account["group_scope"] = scope;
        nlohmann::json channel = nlohmann::json::object();
        channel["accounts"] = nlohmann::json{{"main", account}};
        channels["qqbot"] = channel;
        const auto config = ParseChannelsUserConfig(channels, "config.json", &error);
        REQUIRE(config.has_value());
        CHECK(config->at("qqbot").accounts.at("main").group_scope == scope);
    }
    std::string error;
    CHECK_FALSE(Parse(R"({"qqbot": {"accounts": {"main": {"group_scope": "solo"}}}})", &error)
                    .has_value());
    CHECK(error.find("group_scope 只认") != std::string::npos);
}
