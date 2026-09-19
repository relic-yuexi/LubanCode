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

// ---------------------------------------------------------------------------
// GitHubRelease自动更新单 P1:update 子命令的 Release 解析与资产挑选
// ---------------------------------------------------------------------------

TEST_CASE("ParseReleaseInfoJson: 记全 Release 与资产账") {
    const std::string text = R"({
      "tag_name": "v0.26.300",
      "html_url": "https://github.com/relic-yuexi/LubanCode/releases/tag/v0.26.300",
      "id": 123456789,
      "prerelease": false,
      "assets": [
        {"name": "lubancode-v0.26.300-windows-x64.zip", "id": 111, "size": 89128960,
         "digest": "sha256:aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
         "browser_download_url": "https://example.test/win.zip"},
        {"name": "lubancode-v0.26.300-linux-x64.tar.gz", "id": 222, "size": 7,
         "digest": "sha256:bbbb", "browser_download_url": "https://example.test/lin.tgz"},
        {"name": "notes.txt", "id": 333}
      ]
    })";
    const auto parsed = config::ParseReleaseInfoJson(text);
    REQUIRE(parsed.has_value());
    CHECK(parsed->tag_name == "v0.26.300");
    CHECK(parsed->version == "0.26.300");
    CHECK(parsed->id == 123456789);
    CHECK_FALSE(parsed->prerelease);
    REQUIRE(parsed->assets.size() == 3);
    CHECK(parsed->assets[0].digest ==
          "sha256:aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa");
    CHECK(parsed->assets[1].size == 7);  // 坏摘要不挡解析:如实交账,调用方拒
    CHECK(parsed->assets[2].digest.empty());
    CHECK(parsed->assets[2].download_url.empty());
}

TEST_CASE("ParseReleaseInfoJson: 坏 JSON/缺 tag/坏版本明错") {
    CHECK_FALSE(config::ParseReleaseInfoJson("not json").has_value());
    CHECK_FALSE(config::ParseReleaseInfoJson(R"({"id": 1})").has_value());
    CHECK_FALSE(config::ParseReleaseInfoJson(R"({"tag_name": "0.26"})").has_value());
}

TEST_CASE("PickAssetForPlatform: 按平台精确挑,挑不到明说") {
    config::ReleaseInfo release;
    release.tag_name = "v0.26.300";
    release.version = "0.26.300";
    config::ReleaseAssetInfo win;
    win.name = "lubancode-v0.26.300-windows-x64.zip";
    win.id = 111;
    config::ReleaseAssetInfo mac;
    mac.name = "lubancode-v0.26.300-macos-arm64.tar.gz";
    mac.id = 222;
    release.assets = {win, mac};

    const auto picked = config::PickAssetForPlatform(release, "windows-x64");
    REQUIRE(picked.has_value());
    CHECK(picked->id == 111);
    CHECK(picked->name == "lubancode-v0.26.300-windows-x64.zip");

    const auto missing = config::PickAssetForPlatform(release, "linux-x64");
    CHECK_FALSE(missing.has_value());
    CHECK(missing.error().find("linux-x64") != std::string::npos);
}

TEST_CASE("ExeVersionFromTag: 剥 v 与预发布尾巴(release.yml 口径)") {
    CHECK(config::ExeVersionFromTag("v0.26.300") == "0.26.300");
    CHECK(config::ExeVersionFromTag("0.26.300") == "0.26.300");
    CHECK(config::ExeVersionFromTag("v1.0.0-beta.1") == "1.0.0");
    CHECK(config::ExeVersionFromTag("v1.0.0-rc.1") == "1.0.0");
}
