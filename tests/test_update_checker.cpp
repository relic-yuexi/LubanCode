#include <doctest/doctest.h>

#include "config/update_checker.hpp"

namespace config = lubancode::config;

TEST_CASE("CompareSemanticVersions: 认 v、数字大小与构建元数据") {
    CHECK(*config::CompareSemanticVersions("v0.25.0", "0.24.9") == 1);
    CHECK(*config::CompareSemanticVersions("0.24.0", "v0.24.0") == 0);
    CHECK(*config::CompareSemanticVersions("0.24.0+local", "0.24.0+ci") == 0);
    CHECK(*config::CompareSemanticVersions("1.0.0", "2.0.0") == -1);
}

TEST_CASE("CompareSemanticVersions: 正式版高于预发布版") {
    CHECK(*config::CompareSemanticVersions("1.0.0", "1.0.0-rc.1") == 1);
    CHECK(*config::CompareSemanticVersions("1.0.0-rc.2", "1.0.0-rc.1") == 1);
    CHECK(*config::CompareSemanticVersions("1.0.0-beta.2", "1.0.0-beta.11") == -1);
    CHECK(*config::CompareSemanticVersions("1.0.0-beta.184467440737095516160",
                                           "1.0.0-beta.99999999999999999999") == 1);
}

TEST_CASE("CompareSemanticVersions: 坏版本报错") {
    CHECK_FALSE(config::CompareSemanticVersions("0.24", "0.24.0").has_value());
    CHECK_FALSE(config::CompareSemanticVersions("0.024.0", "0.24.0").has_value());
    CHECK_FALSE(config::CompareSemanticVersions("version-next", "0.24.0").has_value());
}

TEST_CASE("ParseLatestReleaseJson: 看出新版并规范化 v 前缀") {
    const auto parsed = config::ParseLatestReleaseJson(
        R"({"tag_name":"v0.25.0","html_url":"https://github.com/relic-yuexi/LubanCode/releases/tag/v0.25.0"})",
        "0.24.0");
    REQUIRE(parsed.has_value());
    CHECK(parsed->current_version == "0.24.0");
    CHECK(parsed->latest_version == "0.25.0");
    CHECK(parsed->update_available);
}

TEST_CASE("ParseLatestReleaseJson: 同版与旧版不报更新") {
    const auto same = config::ParseLatestReleaseJson(
        R"({"tag_name":"v0.24.0","html_url":"https://example.test/same"})", "v0.24.0");
    REQUIRE(same.has_value());
    CHECK_FALSE(same->update_available);

    const auto older = config::ParseLatestReleaseJson(
        R"({"tag_name":"v0.23.0","html_url":"https://example.test/older"})", "0.24.0");
    REQUIRE(older.has_value());
    CHECK_FALSE(older->update_available);
}

TEST_CASE("ParseLatestReleaseJson: 坏响应说清字段") {
    CHECK_FALSE(config::ParseLatestReleaseJson("not json", "0.24.0").has_value());
    CHECK_FALSE(config::ParseLatestReleaseJson(R"({"html_url":"https://example.test"})", "0.24.0")
                    .has_value());
    CHECK_FALSE(config::ParseLatestReleaseJson(R"({"tag_name":"v0.25.0"})", "0.24.0")
                    .has_value());
}
