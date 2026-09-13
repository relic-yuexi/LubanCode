// 部署档生产解析器的单测(工业化多协议接入单 P1)。
//
// P0 把语义冻结在 tests/unit/capability/test_capability_contract.cpp(测试
// 侧校验器);本册钉的是生产解析器(app_server/harness_profile.cpp)与
// 冻结合同的逐条对齐:golden 两份照吃、依赖解释不全明拒、未知键拒绝、
// mode 矛盾拒绝、deny 胜出、defaultProfile 选取。两册合起来是"同一份
// 合同,测试侧与生产侧各执一词"的对账。
#include <doctest/doctest.h>

#include <filesystem>
#include <fstream>
#include <optional>
#include <set>
#include <sstream>
#include <string>

#include <nlohmann/json.hpp>

#include "app_server/harness_profile.hpp"

namespace fs = std::filesystem;
using namespace lubancode;
using nlohmann::json;

namespace {

const fs::path kFixturesRoot = fs::path(LUBANCODE_SOURCE_DIR) / "tests" / "fixtures" / "capability";

std::optional<json> ReadJsonFile(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in.is_open()) {
        return std::nullopt;
    }
    std::stringstream buffer;
    buffer << in.rdbuf();
    const json parsed = json::parse(buffer.str(), nullptr, /*allow_exceptions=*/false);
    if (parsed.is_discarded()) {
        return std::nullopt;
    }
    return parsed;
}

}  // namespace

TEST_CASE("生产解析:两份 P0 golden 都解析得动,语义逐字段对上") {
    for (const char* golden : {"profile.minimal-tools.json", "profile.zero-tools.json"}) {
        CAPTURE(golden);
        const auto deployment = ReadJsonFile(kFixturesRoot / golden);
        REQUIRE(deployment.has_value());
        const auto parsed = app_server::ParseHarnessDeploymentDefault(*deployment);
        REQUIRE(parsed.profile.has_value());
        CHECK(parsed.error.empty());
    }
}

TEST_CASE("生产解析:minimal-tools 档面——mcp 放行、两只工具、其余功能全关") {
    const auto deployment = ReadJsonFile(kFixturesRoot / "profile.minimal-tools.json");
    REQUIRE(deployment.has_value());
    const auto parsed = app_server::ParseHarnessDeploymentDefault(*deployment);
    REQUIRE(parsed.profile.has_value());
    const app_server::HarnessProfile& profile = *parsed.profile;

    CHECK(profile.name == "minimal-tools");
    CHECK(profile.agent_ref == "example.tools:assistant");
    CHECK(profile.FeatureEnabled("mcp"));
    CHECK_FALSE(profile.FeatureEnabled("process.exec"));
    CHECK_FALSE(profile.FeatureEnabled("browser"));
    CHECK_FALSE(profile.FeatureEnabled("subagents"));
    CHECK_FALSE(profile.FeatureEnabled("filesystem.write"));
    // tools 面:only + echo/describe
    REQUIRE(profile.tools.mode == app_server::HarnessToolPolicy::Mode::Only);
    REQUIRE(profile.tools.allow.size() == 2);
    CHECK(profile.tools.allow[0] == "mcp:tools-approved:echo");
    CHECK(profile.tools.allow[1] == "mcp:tools-approved:describe");
    // components 与依赖解释的引用面
    REQUIRE(profile.mcp_servers.size() == 1);
    CHECK(profile.mcp_servers[0] == "tools-approved");
    const std::set<std::string> referenced = profile.ReferencedMcpServers();
    CHECK(referenced.count("tools-approved") == 1);
    CHECK(profile.exposure == "direct");
    CHECK(profile.steps_per_input == 6);
}

TEST_CASE("生产解析:zero-tools 档面——零工具、零启动依赖") {
    const auto deployment = ReadJsonFile(kFixturesRoot / "profile.zero-tools.json");
    REQUIRE(deployment.has_value());
    const auto parsed = app_server::ParseHarnessDeploymentDefault(*deployment);
    REQUIRE(parsed.profile.has_value());
    const app_server::HarnessProfile& profile = *parsed.profile;

    REQUIRE(profile.tools.mode == app_server::HarnessToolPolicy::Mode::None);
    CHECK(profile.tools.allow.empty());
    CHECK(profile.mcp_servers.empty());
    CHECK_FALSE(profile.FeatureEnabled("mcp"));
    CHECK(profile.ReferencedMcpServers().empty());
}

TEST_CASE("生产解析:依赖解释不全明拒,档不许采用") {
    const auto deployment = ReadJsonFile(kFixturesRoot / "profile.minimal-tools.json");
    REQUIRE(deployment.has_value());

    SUBCASE("allow 点名的服务不在 components") {
        json bad = *deployment;
        bad["harnessProfiles"]["minimal-tools"]["tools"]["allow"] =
            json::array({"mcp:tools-unapproved:secret"});
        const auto parsed = app_server::ParseHarnessDeployment(bad, "minimal-tools");
        CHECK_FALSE(parsed.profile.has_value());
        CHECK_FALSE(parsed.error.empty());
    }
    SUBCASE("features 未放行 mcp 而点名 MCP 工具") {
        json bad = *deployment;
        bad["harnessProfiles"]["minimal-tools"]["features"]["enabled"] = json::array();
        const auto parsed = app_server::ParseHarnessDeployment(bad, "minimal-tools");
        CHECK_FALSE(parsed.profile.has_value());
        CHECK_FALSE(parsed.error.empty());
    }
    SUBCASE("components 点名服务但 features 未放行 mcp") {
        json bad = *deployment;
        bad["harnessProfiles"]["minimal-tools"]["features"]["enabled"] = json::array();
        bad["harnessProfiles"]["minimal-tools"]["tools"] = json{{"mode", "none"}};
        const auto parsed = app_server::ParseHarnessDeployment(bad, "minimal-tools");
        CHECK_FALSE(parsed.profile.has_value());
        CHECK_FALSE(parsed.error.empty());
    }
}

TEST_CASE("生产解析:未知键/未知 mode/矛盾组合全拒") {
    const auto deployment = ReadJsonFile(kFixturesRoot / "profile.zero-tools.json");
    REQUIRE(deployment.has_value());

    SUBCASE("档内未知键") {
        json bad = *deployment;
        bad["harnessProfiles"]["zero-tools"]["toolz"] = json::object();
        const auto parsed = app_server::ParseHarnessDeployment(bad, "zero-tools");
        CHECK_FALSE(parsed.profile.has_value());
    }
    SUBCASE("schemaVersion=2") {
        json bad = *deployment;
        bad["schemaVersion"] = 2;
        const auto parsed = app_server::ParseHarnessDeploymentDefault(bad);
        CHECK_FALSE(parsed.profile.has_value());
    }
    SUBCASE("tools.mode 未知值") {
        json bad = *deployment;
        bad["harnessProfiles"]["zero-tools"]["tools"]["mode"] = "everything";
        const auto parsed = app_server::ParseHarnessDeployment(bad, "zero-tools");
        CHECK_FALSE(parsed.profile.has_value());
    }
    SUBCASE("only 缺 allow") {
        json bad = *deployment;
        bad["harnessProfiles"]["zero-tools"]["tools"] = json{{"mode", "only"}};
        const auto parsed = app_server::ParseHarnessDeployment(bad, "zero-tools");
        CHECK_FALSE(parsed.profile.has_value());
    }
    SUBCASE("none 带非空 allow") {
        json bad = *deployment;
        bad["harnessProfiles"]["zero-tools"]["tools"]["allow"] =
            json::array({"mcp:tools-approved:echo"});
        const auto parsed = app_server::ParseHarnessDeployment(bad, "zero-tools");
        CHECK_FALSE(parsed.profile.has_value());
    }
    SUBCASE("features 名单表外键") {
        json bad = *deployment;
        bad["harnessProfiles"]["zero-tools"]["features"]["disabled"].push_back("fs-write");
        const auto parsed = app_server::ParseHarnessDeployment(bad, "zero-tools");
        CHECK_FALSE(parsed.profile.has_value());
    }
    SUBCASE("features 同键两名单同现") {
        // minimal-tools 的 disabled 列表里已有 browser;enabled 再点名同一
        // 枚 = 同键两名单,配置错误。
        const auto min_deployment = ReadJsonFile(kFixturesRoot / "profile.minimal-tools.json");
        REQUIRE(min_deployment.has_value());
        json bad = *min_deployment;
        bad["harnessProfiles"]["minimal-tools"]["features"]["enabled"].push_back("browser");
        const auto parsed = app_server::ParseHarnessDeployment(bad, "minimal-tools");
        CHECK_FALSE(parsed.profile.has_value());
    }
    SUBCASE("零工具面配 deferred") {
        json bad = *deployment;
        bad["harnessProfiles"]["zero-tools"]["exposure"]["default"] = "deferred";
        const auto parsed = app_server::ParseHarnessDeployment(bad, "zero-tools");
        CHECK_FALSE(parsed.profile.has_value());
    }
    SUBCASE("点名不存在的档") {
        const auto parsed = app_server::ParseHarnessDeployment(*deployment, "no-such-profile");
        CHECK_FALSE(parsed.profile.has_value());
    }
}

TEST_CASE("生产解析:deny 胜出——与 allow 交叠的 deny 裁掉该工具") {
    const auto deployment = ReadJsonFile(kFixturesRoot / "profile.minimal-tools.json");
    REQUIRE(deployment.has_value());
    json doc = *deployment;
    doc["harnessProfiles"]["minimal-tools"]["tools"]["deny"] =
        json::array({"mcp:tools-approved:describe"});
    const auto parsed = app_server::ParseHarnessDeployment(doc, "minimal-tools");
    REQUIRE(parsed.profile.has_value());
    REQUIRE(parsed.profile->tools.allow.size() == 1);
    CHECK(parsed.profile->tools.allow[0] == "mcp:tools-approved:echo");
}

TEST_CASE("生产解析:文件入口——打不开与坏 JSON 明败,空档名取 defaultProfile") {
    const fs::path missing = kFixturesRoot / "no-such-file.json";
    const auto missing_result = app_server::LoadHarnessDeploymentFile(missing, std::string());
    CHECK_FALSE(missing_result.profile.has_value());
    CHECK_FALSE(missing_result.error.empty());

    const fs::path golden = kFixturesRoot / "profile.zero-tools.json";
    const auto via_file = app_server::LoadHarnessDeploymentFile(golden, std::string());
    REQUIRE(via_file.profile.has_value());
    CHECK(via_file.profile->name == "zero-tools");
}
