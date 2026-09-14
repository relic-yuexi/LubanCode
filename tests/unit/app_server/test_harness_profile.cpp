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

// ---------------------------------------------------------------------------
// P2(应用Worker接入单):内置 skill 工具名与插件点名通道
// ---------------------------------------------------------------------------

TEST_CASE("P2 解析:allow 点名 skill——放行 skills 才收,deny 胜出") {
    const auto deployment = ReadJsonFile(kFixturesRoot / "profile.minimal-tools.json");
    REQUIRE(deployment.has_value());

    SUBCASE("features 放行 skills:skill 进 allow") {
        json doc = *deployment;
        doc["harnessProfiles"]["minimal-tools"]["features"]["enabled"] = json::array({"mcp", "skills"});
        doc["harnessProfiles"]["minimal-tools"]["tools"]["allow"] =
            json::array({"mcp:tools-approved:echo", "skill"});
        const auto parsed = app_server::ParseHarnessDeployment(doc, "minimal-tools");
        REQUIRE(parsed.profile.has_value());
        REQUIRE(parsed.profile->tools.allow.size() == 2);
        CHECK(parsed.profile->tools.allow[1] == "skill");
    }
    SUBCASE("features 未放行 skills:点名 skill 是配置矛盾,明拒") {
        json doc = *deployment;
        doc["harnessProfiles"]["minimal-tools"]["tools"]["allow"] =
            json::array({"mcp:tools-approved:echo", "skill"});
        const auto parsed = app_server::ParseHarnessDeployment(doc, "minimal-tools");
        CHECK_FALSE(parsed.profile.has_value());
        REQUIRE_FALSE(parsed.error.empty());
        CHECK(parsed.error.find("skills") != std::string::npos);
    }
    SUBCASE("其余裸名仍明拒(解释不出的名字不进 allow)") {
        json doc = *deployment;
        doc["harnessProfiles"]["minimal-tools"]["tools"]["allow"] =
            json::array({"mcp:tools-approved:echo", "run_command"});
        const auto parsed = app_server::ParseHarnessDeployment(doc, "minimal-tools");
        CHECK_FALSE(parsed.profile.has_value());
        CHECK_FALSE(parsed.error.empty());
    }
    SUBCASE("deny 裁掉 allow 里的 skill(deny 胜出)") {
        json doc = *deployment;
        doc["harnessProfiles"]["minimal-tools"]["features"]["enabled"] = json::array({"mcp", "skills"});
        doc["harnessProfiles"]["minimal-tools"]["tools"]["allow"] =
            json::array({"mcp:tools-approved:echo", "skill"});
        doc["harnessProfiles"]["minimal-tools"]["tools"]["deny"] = json::array({"skill"});
        const auto parsed = app_server::ParseHarnessDeployment(doc, "minimal-tools");
        REQUIRE(parsed.profile.has_value());
        REQUIRE(parsed.profile->tools.allow.size() == 1);
        CHECK(parsed.profile->tools.allow[0] == "mcp:tools-approved:echo");
    }
}

TEST_CASE("P2 解析:components.plugins 点名通道——收名单,未放行 plugins 是矛盾") {
    const auto deployment = ReadJsonFile(kFixturesRoot / "profile.zero-tools.json");
    REQUIRE(deployment.has_value());

    SUBCASE("放行 plugins:名单原样收") {
        json doc = *deployment;
        doc["harnessProfiles"]["zero-tools"]["features"]["enabled"] = json::array({"plugins"});
        doc["harnessProfiles"]["zero-tools"]["components"] = json{{"plugins", json::array({"demo.lua-tool"})}};
        const auto parsed = app_server::ParseHarnessDeployment(doc, "zero-tools");
        REQUIRE(parsed.profile.has_value());
        REQUIRE(parsed.profile->plugins.size() == 1);
        CHECK(parsed.profile->plugins[0] == "demo.lua-tool");
    }
    SUBCASE("未放行 plugins:点名是配置矛盾,明拒") {
        json doc = *deployment;
        doc["harnessProfiles"]["zero-tools"]["components"] = json{{"plugins", json::array({"demo.lua-tool"})}};
        const auto parsed = app_server::ParseHarnessDeployment(doc, "zero-tools");
        CHECK_FALSE(parsed.profile.has_value());
        REQUIRE_FALSE(parsed.error.empty());
        CHECK(parsed.error.find("plugins") != std::string::npos);
    }
    SUBCASE("components 未知键照旧拒绝") {
        json doc = *deployment;
        doc["harnessProfiles"]["zero-tools"]["components"] = json{{"widgets", json::array({"x"})}};
        const auto parsed = app_server::ParseHarnessDeployment(doc, "zero-tools");
        CHECK_FALSE(parsed.profile.has_value());
        CHECK_FALSE(parsed.error.empty());
    }
}

TEST_CASE("P5 解析:plugin__<id>__<tool> 进 allow——依赖解释与 deny 裁") {
    const auto deployment = ReadJsonFile(kFixturesRoot / "profile.zero-tools.json");
    REQUIRE(deployment.has_value());

    // 点名+放行+mode=only 的底版(名字段全改写:golden 是零工具档)。
    const auto MakePluginDoc = [&deployment]() {
        json doc = *deployment;
        doc["harnessProfiles"]["zero-tools"]["features"]["enabled"] = json::array({"plugins"});
        doc["harnessProfiles"]["zero-tools"]["components"] = json{{"plugins", json::array({"demo-lua"})}};
        doc["harnessProfiles"]["zero-tools"]["tools"] = json{
            {"mode", "only"}, {"allow", json::array({"plugin__demo-lua__search"})}, {"deny", json::array()}};
        return doc;
    };

    SUBCASE("点名+放行:allow 收 plugin__ 名") {
        const auto parsed = app_server::ParseHarnessDeployment(MakePluginDoc(), "zero-tools");
        REQUIRE(parsed.profile.has_value());
        REQUIRE(parsed.profile->tools.allow.size() == 1);
        CHECK(parsed.profile->tools.allow[0] == "plugin__demo-lua__search");
        REQUIRE(parsed.profile->plugins.size() == 1);
        CHECK(parsed.profile->plugins[0] == "demo-lua");
    }
    SUBCASE("id 段不在 components.plugins:配置矛盾明拒") {
        json doc = MakePluginDoc();
        doc["harnessProfiles"]["zero-tools"]["tools"]["allow"] =
            json::array({"plugin__other-lua__search"});
        const auto parsed = app_server::ParseHarnessDeployment(doc, "zero-tools");
        CHECK_FALSE(parsed.profile.has_value());
        REQUIRE_FALSE(parsed.error.empty());
        CHECK(parsed.error.find("plugin__other-lua__search") != std::string::npos);
        CHECK(parsed.error.find("components.plugins") != std::string::npos);
    }
    SUBCASE("features 未放行 plugins:allow 点名 plugin 工具明拒") {
        json doc = MakePluginDoc();
        doc["harnessProfiles"]["zero-tools"]["features"]["enabled"] = json::array();
        const auto parsed = app_server::ParseHarnessDeployment(doc, "zero-tools");
        CHECK_FALSE(parsed.profile.has_value());
        REQUIRE_FALSE(parsed.error.empty());
        CHECK(parsed.error.find("plugin__demo-lua__search") != std::string::npos);
        CHECK(parsed.error.find("plugins") != std::string::npos);
    }
    SUBCASE("缺工具段的病态名:明拒不猜") {
        json doc = MakePluginDoc();
        doc["harnessProfiles"]["zero-tools"]["tools"]["allow"] = json::array({"plugin__demo-lua"});
        const auto parsed = app_server::ParseHarnessDeployment(doc, "zero-tools");
        CHECK_FALSE(parsed.profile.has_value());
        REQUIRE_FALSE(parsed.error.empty());
        CHECK(parsed.error.find("plugin__demo-lua") != std::string::npos);
    }
    SUBCASE("deny 裁掉 allow 里的 plugin__ 工具(deny 胜出,与 mcp/skill 同款)") {
        json doc = MakePluginDoc();
        doc["harnessProfiles"]["zero-tools"]["tools"]["deny"] = json::array({"plugin__demo-lua__search"});
        const auto parsed = app_server::ParseHarnessDeployment(doc, "zero-tools");
        REQUIRE(parsed.profile.has_value());
        CHECK(parsed.profile->tools.allow.empty());
    }
}

// ---------------------------------------------------------------------------
// 应用Worker接入单 §六:components.skills 来源声明式 schema(additive)
// ---------------------------------------------------------------------------

namespace {

// 声明了 skills 的档底版:放行 skills + tools 点名 skill,required/optional
// 由各用例改写。
json SkillsDeclarationDoc(std::vector<std::string> required, std::vector<std::string> optional) {
    json doc;
    doc["schemaVersion"] = 1;
    doc["service"] = {{"mode", "managed"},
                      {"listeners", {{"stdio", {{"enabled", true}}}}},
                      {"defaultProfile", "p"}};
    json skills = json::object();
    if (!required.empty()) {
        skills["required"] = required;
    }
    if (!optional.empty()) {
        skills["optional"] = optional;
    }
    doc["harnessProfiles"] = {{"p",
                               {{"agentRef", "general-purpose"},
                                {"features",
                                 {{"default", "disabled"}, {"enabled", json::array({"skills"})}}},
                                {"components", {{"skills", std::move(skills)}}},
                                {"tools",
                                 {{"mode", "only"}, {"allow", json::array({"skill"})}, {"deny", json::array()}}},
                                {"exposure", {{"default", "direct"}}}}}};
    return doc;
}

}  // namespace

TEST_CASE("skills 声明解析:required/optional 收账,sourceDir 形状校验") {
    SUBCASE("required+optional 原样收") {
        const auto parsed =
            app_server::ParseHarnessDeploymentDefault(SkillsDeclarationDoc({"a"}, {"b", "c"}));
        REQUIRE(parsed.profile.has_value());
        REQUIRE(parsed.profile->skills_required.size() == 1);
        CHECK(parsed.profile->skills_required[0] == "a");
        REQUIRE(parsed.profile->skills_optional.size() == 2);
        CHECK(parsed.profile->skills_optional[0] == "b");
        CHECK(parsed.profile->skills_optional[1] == "c");
        CHECK(parsed.profile->skills_source_dir.empty());
        CHECK(parsed.profile->DeclaresSkills());
    }
    SUBCASE("sourceDir 相对子目录收") {
        json doc = SkillsDeclarationDoc({"a"}, {});
        doc["harnessProfiles"]["p"]["components"]["skills"]["sourceDir"] = "research/team";
        const auto parsed = app_server::ParseHarnessDeploymentDefault(doc);
        REQUIRE(parsed.profile.has_value());
        CHECK(parsed.profile->skills_source_dir == "research/team");
    }
    SUBCASE("sourceDir 绝对路径:拒") {
        json doc = SkillsDeclarationDoc({"a"}, {});
        doc["harnessProfiles"]["p"]["components"]["skills"]["sourceDir"] = "/abs/skills";
        const auto parsed = app_server::ParseHarnessDeploymentDefault(doc);
        CHECK_FALSE(parsed.profile.has_value());
        CHECK_FALSE(parsed.error.empty());
    }
    SUBCASE("sourceDir 带 ..:拒") {
        json doc = SkillsDeclarationDoc({"a"}, {});
        doc["harnessProfiles"]["p"]["components"]["skills"]["sourceDir"] = "a/../b";
        const auto parsed = app_server::ParseHarnessDeploymentDefault(doc);
        CHECK_FALSE(parsed.profile.has_value());
    }
    SUBCASE("sourceDir 反斜杠:拒") {
        json doc = SkillsDeclarationDoc({"a"}, {});
        doc["harnessProfiles"]["p"]["components"]["skills"]["sourceDir"] = "a\\b";
        const auto parsed = app_server::ParseHarnessDeploymentDefault(doc);
        CHECK_FALSE(parsed.profile.has_value());
    }
    SUBCASE("sourceDir 段里带冒号(盘符段会顶掉前缀路径):拒") {
        json doc = SkillsDeclarationDoc({"a"}, {});
        doc["harnessProfiles"]["p"]["components"]["skills"]["sourceDir"] = "C:/skills";
        const auto parsed = app_server::ParseHarnessDeploymentDefault(doc);
        CHECK_FALSE(parsed.profile.has_value());
    }
    SUBCASE("空 sourceDir:拒(缺省不给键,不是给空串)") {
        json doc = SkillsDeclarationDoc({"a"}, {});
        doc["harnessProfiles"]["p"]["components"]["skills"]["sourceDir"] = "";
        const auto parsed = app_server::ParseHarnessDeploymentDefault(doc);
        CHECK_FALSE(parsed.profile.has_value());
    }
    SUBCASE("skills 未知键:拒") {
        json doc = SkillsDeclarationDoc({"a"}, {});
        doc["harnessProfiles"]["p"]["components"]["skills"]["loadPolicy"] = "eager";
        const auto parsed = app_server::ParseHarnessDeploymentDefault(doc);
        CHECK_FALSE(parsed.profile.has_value());
        CHECK(parsed.error.find("components.skills 未知键") != std::string::npos);
    }
    SUBCASE("空声明对象:合法(等于没声明,P2 全量约定照旧)") {
        json doc = SkillsDeclarationDoc({}, {});
        doc["harnessProfiles"]["p"]["components"] = json{{"skills", json::object()}};
        const auto parsed = app_server::ParseHarnessDeploymentDefault(doc);
        REQUIRE(parsed.profile.has_value());
        CHECK_FALSE(parsed.profile->DeclaresSkills());
    }
}

TEST_CASE("skills 声明解析:同名冲突与重复点名明示,不猜先后") {
    SUBCASE("同一枚名 required 与 optional 两现:拒") {
        const auto parsed = app_server::ParseHarnessDeploymentDefault(SkillsDeclarationDoc({"a"}, {"a"}));
        CHECK_FALSE(parsed.profile.has_value());
        REQUIRE_FALSE(parsed.error.empty());
        CHECK(parsed.error.find("required/optional 两现") != std::string::npos);
        CHECK(parsed.error.find("a") != std::string::npos);
    }
    SUBCASE("required 名单内重复:拒") {
        const auto parsed = app_server::ParseHarnessDeploymentDefault(SkillsDeclarationDoc({"a", "a"}, {}));
        CHECK_FALSE(parsed.profile.has_value());
        CHECK(parsed.error.find("重复点名") != std::string::npos);
    }
    SUBCASE("optional 名单内重复:拒") {
        const auto parsed = app_server::ParseHarnessDeploymentDefault(SkillsDeclarationDoc({}, {"b", "b"}));
        CHECK_FALSE(parsed.profile.has_value());
        CHECK(parsed.error.find("两现") != std::string::npos);
    }
}

TEST_CASE("skills 声明解析:依赖解释——须放行 skills 且 tools 点名 skill") {
    SUBCASE("features 未放行 skills:配置矛盾,明拒") {
        json doc = SkillsDeclarationDoc({"a"}, {});
        doc["harnessProfiles"]["p"]["features"]["enabled"] = json::array();
        const auto parsed = app_server::ParseHarnessDeploymentDefault(doc);
        CHECK_FALSE(parsed.profile.has_value());
        REQUIRE_FALSE(parsed.error.empty());
        CHECK(parsed.error.find("skills") != std::string::npos);
    }
    SUBCASE("tools 未点名 skill:声明装不进本场,明拒") {
        json doc = SkillsDeclarationDoc({"a"}, {});
        doc["harnessProfiles"]["p"]["tools"] = json{{"mode", "only"}, {"allow", json::array()}};
        const auto parsed = app_server::ParseHarnessDeploymentDefault(doc);
        CHECK_FALSE(parsed.profile.has_value());
        REQUIRE_FALSE(parsed.error.empty());
        CHECK(parsed.error.find("未点名 skill") != std::string::npos);
    }
    SUBCASE("deny 裁掉 skill 后声明仍在:明拒(deny 折算之后对账)") {
        json doc = SkillsDeclarationDoc({"a"}, {});
        doc["harnessProfiles"]["p"]["tools"]["deny"] = json::array({"skill"});
        const auto parsed = app_server::ParseHarnessDeploymentDefault(doc);
        CHECK_FALSE(parsed.profile.has_value());
        CHECK(parsed.error.find("未点名 skill") != std::string::npos);
    }
    SUBCASE("未声明 skills 的档:tools 不点名 skill 照旧合法(P2 约定)") {
        const auto deployment = ReadJsonFile(kFixturesRoot / "profile.zero-tools.json");
        REQUIRE(deployment.has_value());
        const auto parsed = app_server::ParseHarnessDeploymentDefault(*deployment);
        REQUIRE(parsed.profile.has_value());
    }
}
