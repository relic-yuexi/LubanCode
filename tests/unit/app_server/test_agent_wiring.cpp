// Agent/Skill 装配解析与提示部件组合的单测(应用Worker接入单 P2,
// GAP-01/02/03)。
//
// 钉的规矩:
//   - agentRef 必须解析到可用档案:不存在/解析坏(不可用条目)都明拒,
//     不回落编码默认提示词(§5.1);
//   - agentRef 缺席是合法档:计划照出,core 落内置默认人格;
//   - 提示部件组合走 prompt_assembler 既有管线:Profile 业务正文进 core,
//     运行环境/平台段恒在,能力段按实际工具面开合,宿主段盖不掉(§5.2);
//   - skills.preload 名字必须落在扫描清单里,缺名明拒(§5.1 缺依赖);
//   - 预装正文(eager)与清单段同场注入(§六)。
#include <doctest/doctest.h>

#include <atomic>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>

#include "app_server/agent_wiring.hpp"
#include "app_server/harness_profile.hpp"
#include "tools/skill_loader.hpp"

namespace fs = std::filesystem;
using namespace lubancode;
using namespace lubancode::app_server;

namespace {

// 临时材料根:要什么种什么。目录名带进程内单调号,册内各用例互不踩。
struct MaterialRoot {
    fs::path root;
    std::optional<fs::path> agents_dir;
    std::optional<fs::path> skills_dir;
    std::string prompts_dir_utf8;

    MaterialRoot() {
        static std::atomic<int> next_id{0};
        root = fs::temp_directory_path() /
               ("lubancode_agent_wiring_" + std::to_string(next_id.fetch_add(1)));
        std::error_code ec;
        fs::remove_all(root, ec);
        fs::create_directories(root, ec);
        agents_dir = root / "agents";
        skills_dir = root / "skills";
        prompts_dir_utf8 = (root / "prompts").generic_string();
    }
    ~MaterialRoot() {
        std::error_code ec;
        fs::remove_all(root, ec);
    }

    void WriteFile(const fs::path& relative, const std::string& content) {
        const fs::path path = root / relative;
        std::error_code ec;
        fs::create_directories(path.parent_path(), ec);
        std::ofstream out(path, std::ios::binary);
        out << content;
    }
};

HarnessAgentSources SourcesOf(const MaterialRoot& materials) {
    HarnessAgentSources sources;
    sources.agents_dir = materials.agents_dir;
    sources.skills_dir = materials.skills_dir;
    sources.prompts_dir_utf8 = materials.prompts_dir_utf8;
    return sources;
}

HarnessProfile ProfileWithRef(const std::string& agent_ref) {
    HarnessProfile profile;
    profile.name = "p2";
    profile.agent_ref = agent_ref;
    return profile;
}

}  // namespace

TEST_CASE("agentRef 解析:命中真档案,定义原样进计划") {
    MaterialRoot materials;
    materials.WriteFile("agents/research.yaml",
                        "schema: 1\n"
                        "name: research\n"
                        "description: 研究助理。\n"
                        "prompt:\n"
                        "  profile: research\n");
    const auto result = ResolveHarnessAgentPlan(ProfileWithRef("research"), SourcesOf(materials),
                                                /*wire=*/"chat", /*cwd=*/"/tmp/w");
    REQUIRE(result.plan.has_value());
    CHECK(result.error.empty());
    REQUIRE(result.plan->agent.has_value());
    CHECK(result.plan->agent->name == "research");
    CHECK(result.plan->agent->prompt.profile == "research");
    CHECK(result.plan->wire == "chat");
    CHECK(result.plan->cwd == "/tmp/w");
}

TEST_CASE("agentRef 缺件:不存在/解析坏都明拒,不回落默认") {
    MaterialRoot materials;
    materials.WriteFile("agents/minimal.yaml",
                        "schema: 1\n"
                        "name: minimal\n"
                        "description: 最小档案。\n");
    // 坏档案:未知字段让解析报 error,Catalog 登成 unavailable。
    materials.WriteFile("agents/broken.yaml",
                        "schema: 1\n"
                        "name: broken\n"
                        "description: 坏档案。\n"
                        "no-such-field: 1\n");

    SUBCASE("指名的档案不存在") {
        const auto result = ResolveHarnessAgentPlan(ProfileWithRef("no-such-agent"),
                                                    SourcesOf(materials), "chat", "/tmp/w");
        CHECK_FALSE(result.plan.has_value());
        REQUIRE_FALSE(result.error.empty());
        CHECK(result.error.find("no-such-agent") != std::string::npos);
    }
    SUBCASE("指名的档案解析坏(不可用条目)") {
        const auto result =
            ResolveHarnessAgentPlan(ProfileWithRef("broken"), SourcesOf(materials), "chat", "/tmp/w");
        CHECK_FALSE(result.plan.has_value());
        REQUIRE_FALSE(result.error.empty());
        CHECK(result.error.find("broken") != std::string::npos);
    }
    SUBCASE("码内内置档案可解析(发行层)") {
        const auto result = ResolveHarnessAgentPlan(ProfileWithRef("general-purpose"),
                                                    SourcesOf(materials), "chat", "/tmp/w");
        REQUIRE(result.plan.has_value());
        REQUIRE(result.plan->agent.has_value());
        CHECK(result.plan->agent->name == "general-purpose");
    }
}

TEST_CASE("agentRef 缺席是合法档:计划照出,core 落内置默认人格") {
    MaterialRoot materials;
    HarnessProfile profile = ProfileWithRef("");
    const auto result = ResolveHarnessAgentPlan(profile, SourcesOf(materials), "chat", "/tmp/w");
    REQUIRE(result.plan.has_value());
    CHECK_FALSE(result.plan->agent.has_value());

    // 组合:无档案 → 默认 core 人格在场(发行内置材料),档案 persona 不在。
    HarnessAgentPlan plan = *result.plan;
    HarnessPromptInput input;
    input.plan = &plan;
    std::vector<tools::SkillMeta> no_skills;
    input.skills = &no_skills;
    input.face_names = {};
    const auto composed = ComposeHarnessSystemPrompt(input);
    CHECK(composed.error.empty());
    CHECK(composed.text.find("命令行 AI 编程助手") != std::string::npos);  // 内置默认身份
}

TEST_CASE("提示部件组合:Profile 业务正文进 core,宿主段恒在,默认人格不混入") {
    MaterialRoot materials;
    materials.WriteFile("agents/research.yaml",
                        "schema: 1\n"
                        "name: research\n"
                        "description: 研究助理。\n"
                        "prompt:\n"
                        "  profile: research\n");
    materials.WriteFile("prompts/profiles/research/core/10-identity.md",
                        "# 身份\n\n你是 RESEARCH-PROFILE-IDENTITY。\n");
    const auto planned = ResolveHarnessAgentPlan(ProfileWithRef("research"), SourcesOf(materials),
                                                "chat", "/tmp/w");
    REQUIRE(planned.plan.has_value());
    HarnessAgentPlan plan = *planned.plan;

    std::vector<tools::SkillMeta> no_skills;
    HarnessPromptInput input;
    input.plan = &plan;
    input.skills = &no_skills;
    input.face_names = {"mcp__tools-approved__echo"};

    const auto composed = ComposeHarnessSystemPrompt(input);
    REQUIRE(composed.error.empty());
    // 业务正文:Profile 的 core 身份段。
    CHECK(composed.text.find("RESEARCH-PROFILE-IDENTITY") != std::string::npos);
    // 覆盖合同:编码助手默认身份不混入(replace 只替业务正文,不叠加)。
    CHECK(composed.text.find("命令行 AI 编程助手") == std::string::npos);
    // 宿主段盖不掉:运行环境段(cwd)与平台段(wire)恒在。
    CHECK(composed.text.find("/tmp/w") != std::string::npos);
    // 能力段按实际工具面开合:面里有 MCP 工具 → mcp 能力段注入。
    CHECK(composed.text.find("外接工具(MCP)") != std::string::npos);

    // 同一计划、空工具面:mcp 能力段一个字不占(能力说明随工具面走)。
    HarnessPromptInput bare_input = input;
    bare_input.face_names = {};
    const auto bare = ComposeHarnessSystemPrompt(bare_input);
    REQUIRE(bare.error.empty());
    CHECK(bare.text.find("外接工具(MCP)") == std::string::npos);
    CHECK(bare.text.find("RESEARCH-PROFILE-IDENTITY") != std::string::npos);  // 业务正文仍在
}

TEST_CASE("提示部件组合:档案无 Profile 时用档案 persona,内置身份让位") {
    MaterialRoot materials;
    materials.WriteFile("agents/minimal.yaml",
                        "schema: 1\n"
                        "name: minimal\n"
                        "description: 最小档案。\n");
    const auto planned =
        ResolveHarnessAgentPlan(ProfileWithRef("minimal"), SourcesOf(materials), "chat", "/tmp/w");
    REQUIRE(planned.plan.has_value());
    HarnessAgentPlan plan = *planned.plan;

    std::vector<tools::SkillMeta> no_skills;
    HarnessPromptInput input;
    input.plan = &plan;
    input.skills = &no_skills;
    input.face_names = {};
    const auto composed = ComposeHarnessSystemPrompt(input);
    REQUIRE(composed.error.empty());
    CHECK(composed.text.find("你是 minimal") != std::string::npos);
    CHECK(composed.text.find("命令行 AI 编程助手") == std::string::npos);
}

TEST_CASE("技能清单段与预装正文:清单进提示,preload 命中注入、缺名明拒") {
    MaterialRoot materials;
    materials.WriteFile("agents/research.yaml",
                        "schema: 1\n"
                        "name: research\n"
                        "description: 研究助理。\n"
                        "skills:\n"
                        "  preload:\n"
                        "    - greet\n");
    materials.WriteFile("skills/greet/SKILL.md",
                        "---\nname: greet\ndescription: 问候技能。\n---\nGREET-SKILL-BODY 正文。\n");
    const auto planned = ResolveHarnessAgentPlan(ProfileWithRef("research"), SourcesOf(materials),
                                                "chat", "/tmp/w");
    REQUIRE(planned.plan.has_value());
    HarnessAgentPlan plan = *planned.plan;

    // 扫描件(装配路的单根显式扫描同款入口)。
    std::vector<tools::SkillMeta> skills = tools::ScanSkillsDir(*materials.skills_dir, "材料根级");
    REQUIRE(skills.size() == 1);
    REQUIRE(skills[0].name == "greet");

    HarnessPromptInput input;
    input.plan = &plan;
    input.skills = &skills;
    input.face_names = {"skill"};
    const auto composed = ComposeHarnessSystemPrompt(input);
    REQUIRE(composed.error.empty());
    // 清单段:获准清单进提示材料,来源注记如实(headless 只认材料根一处)。
    CHECK(composed.text.find("greet") != std::string::npos);
    CHECK(composed.text.find("部署材料根的 skills/ 目录") != std::string::npos);
    CHECK(composed.text.find("~/.agents/skills") == std::string::npos);  // 终端五层约定不进 headless
    // 预装正文(eager)随场注入。
    CHECK(composed.text.find("GREET-SKILL-BODY") != std::string::npos);

    // preload 缺名:清单里没有 → 整场明拒(§5.1 缺依赖,不悄悄丢)。
    std::vector<tools::SkillMeta> wrong_skills = skills;
    wrong_skills[0].name = "other";
    HarnessPromptInput missing_input;
    missing_input.plan = &plan;
    missing_input.skills = &wrong_skills;
    missing_input.face_names = {"skill"};
    const auto missing = ComposeHarnessSystemPrompt(missing_input);
    CHECK_FALSE(missing.error.empty());
    CHECK(missing.error.find("greet") != std::string::npos);
}
