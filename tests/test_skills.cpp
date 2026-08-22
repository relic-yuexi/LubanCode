// M9:技能系统测试。三块:
//   1) ParseSkillMarkdown —— 手写的 frontmatter 解析器,纯函数。
//   2) LoadSkills/ScanSkillsDir —— 真在临时目录里造两级技能目录,验证扫描
//      结果和"项目级覆盖主目录级"。
//   3) SkillTool —— 命中/未命中两种情况下 execute() 的返回内容。

#include <doctest/doctest.h>

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "tools/skill_loader.hpp"
#include "tools/skill_tool.hpp"

using namespace lubancode;

// ---------------------------------------------------------------------------
// 1) ParseSkillMarkdown
// ---------------------------------------------------------------------------

TEST_CASE("ParseSkillMarkdown: 正常 frontmatter,name/description 都有") {
    const std::string content =
        "---\n"
        "name: poem-style\n"
        "description: 五言绝句写作规范\n"
        "---\n"
        "正文第一行\n"
        "正文第二行\n";
    const auto parsed = tools::ParseSkillMarkdown(content);
    REQUIRE(parsed.has_value());
    REQUIRE(parsed->name.has_value());
    CHECK(*parsed->name == "poem-style");
    REQUIRE(parsed->description.has_value());
    CHECK(*parsed->description == "五言绝句写作规范");
    CHECK(parsed->body == "正文第一行\n正文第二行\n");
}

TEST_CASE("ParseSkillMarkdown: 缺 name 字段,调用方该用目录名兜底(这里只管 nullopt)") {
    const std::string content =
        "---\n"
        "description: 只有说明没有名字\n"
        "---\n"
        "正文\n";
    const auto parsed = tools::ParseSkillMarkdown(content);
    REQUIRE(parsed.has_value());
    CHECK_FALSE(parsed->name.has_value());
    REQUIRE(parsed->description.has_value());
    CHECK(*parsed->description == "只有说明没有名字");
}

TEST_CASE("ParseSkillMarkdown: 缺 description 字段") {
    const std::string content =
        "---\n"
        "name: no-desc\n"
        "---\n"
        "正文\n";
    const auto parsed = tools::ParseSkillMarkdown(content);
    REQUIRE(parsed.has_value());
    REQUIRE(parsed->name.has_value());
    CHECK(*parsed->name == "no-desc");
    CHECK_FALSE(parsed->description.has_value());
}

TEST_CASE("ParseSkillMarkdown: 没有 frontmatter,body 就是整篇原文,不算错") {
    const std::string content = "这篇技能没有 frontmatter,直接是正文。\n";
    const auto parsed = tools::ParseSkillMarkdown(content);
    REQUIRE(parsed.has_value());
    CHECK_FALSE(parsed->name.has_value());
    CHECK_FALSE(parsed->description.has_value());
    CHECK(parsed->body == content);
}

TEST_CASE("ParseSkillMarkdown: frontmatter 起了头但没有闭合的 ---,视为损坏,返回 nullopt") {
    const std::string content =
        "---\n"
        "name: broken\n"
        "正文,没有第二个 ---\n";
    const auto parsed = tools::ParseSkillMarkdown(content);
    CHECK_FALSE(parsed.has_value());
}

TEST_CASE("ParseSkillMarkdown: 引号包裹的值会被去掉引号") {
    const std::string content =
        "---\n"
        "name: \"quoted-name\"\n"
        "description: 'single quoted'\n"
        "---\n"
        "正文\n";
    const auto parsed = tools::ParseSkillMarkdown(content);
    REQUIRE(parsed.has_value());
    REQUIRE(parsed->name.has_value());
    CHECK(*parsed->name == "quoted-name");
    REQUIRE(parsed->description.has_value());
    CHECK(*parsed->description == "single quoted");
}

// ---------------------------------------------------------------------------
// 2) ScanSkillsDir / LoadSkills:真在临时目录造文件。
// ---------------------------------------------------------------------------

namespace {

class TempSkillsRoot {
public:
    TempSkillsRoot() {
        dir_ = std::filesystem::temp_directory_path() /
               ("lubancode_skills_test_" + std::to_string(reinterpret_cast<std::uintptr_t>(this)));
        std::error_code ec;
        std::filesystem::remove_all(dir_, ec);
        std::filesystem::create_directories(dir_, ec);
    }
    ~TempSkillsRoot() {
        std::error_code ec;
        std::filesystem::remove_all(dir_, ec);
    }

    std::string Path() const { return dir_.string(); }

    // 在 <root>/<home_or_project>/.lubancode/skills/<skill_name>/SKILL.md 里写一份技能。
    void WriteSkill(const std::string& base_subdir, const std::string& skill_name, const std::string& content) const {
        const std::filesystem::path skill_dir = dir_ / base_subdir / ".lubancode" / "skills" / skill_name;
        std::error_code ec;
        std::filesystem::create_directories(skill_dir, ec);
        std::ofstream file(skill_dir / "SKILL.md", std::ios::binary);
        file << content;
    }

    void WriteOfficialSkill(const std::string& skill_name, const std::string& content) const {
        const std::filesystem::path skill_dir = dir_ / "official" / skill_name;
        std::error_code ec;
        std::filesystem::create_directories(skill_dir, ec);
        std::ofstream file(skill_dir / "SKILL.md", std::ios::binary);
        file << content;
    }

    std::string BaseDir(const std::string& base_subdir) const { return (dir_ / base_subdir).string(); }

private:
    std::filesystem::path dir_;
};

std::string SkillContent(const std::string& name, const std::string& description, const std::string& body) {
    return "---\nname: " + name + "\ndescription: " + description + "\n---\n" + body;
}

}  // namespace

TEST_CASE("ScanSkillsDir: 目录不存在,返回空 vector,不报错") {
    TempSkillsRoot root;
    const auto metas = tools::ScanSkillsDir(std::filesystem::path(root.Path()) / "not-exists", "项目级");
    CHECK(metas.empty());
}

TEST_CASE("ScanSkillsDir: 扫到一个正常技能") {
    TempSkillsRoot root;
    root.WriteSkill("proj", "poem-style", SkillContent("poem-style", "五言绝句写作规范", "写诗要押韵。\n"));

    const auto metas =
        tools::ScanSkillsDir(std::filesystem::path(root.Path()) / "proj" / ".lubancode" / "skills", "项目级");
    REQUIRE(metas.size() == 1);
    CHECK(metas[0].name == "poem-style");
    CHECK(metas[0].description == "五言绝句写作规范");
    CHECK(metas[0].source_level == "项目级");
}

TEST_CASE("LoadSkills: 同名技能,项目级覆盖主目录级") {
    TempSkillsRoot root;
    root.WriteSkill("home", "shared-skill", SkillContent("shared-skill", "主目录级的说明", "主目录级正文\n"));
    root.WriteSkill("proj", "shared-skill", SkillContent("shared-skill", "项目级的说明", "项目级正文\n"));
    root.WriteSkill("home", "home-only", SkillContent("home-only", "只有主目录有", "只在主目录\n"));

    const auto skills = tools::LoadSkills(root.BaseDir("proj"), root.BaseDir("home"));

    REQUIRE(skills.size() == 2);
    const auto shared_it = std::find_if(skills.begin(), skills.end(),
                                         [](const tools::SkillMeta& m) { return m.name == "shared-skill"; });
    REQUIRE(shared_it != skills.end());
    CHECK(shared_it->description == "项目级的说明");  // 项目级赢
    CHECK(shared_it->source_level == "项目级");

    const auto home_only_it = std::find_if(skills.begin(), skills.end(),
                                            [](const tools::SkillMeta& m) { return m.name == "home-only"; });
    REQUIRE(home_only_it != skills.end());
    CHECK(home_only_it->source_level == "主目录级");
}

TEST_CASE("LoadSkills: 官方、主目录、项目三级按顺序覆盖") {
    TempSkillsRoot root;
    root.WriteOfficialSkill("shared-skill", SkillContent("shared-skill", "官方说明", "官方正文\n"));
    root.WriteOfficialSkill("official-only", SkillContent("official-only", "官方独有", "官方正文\n"));
    root.WriteSkill("home", "shared-skill", SkillContent("shared-skill", "主目录说明", "主目录正文\n"));
    root.WriteSkill("proj", "shared-skill", SkillContent("shared-skill", "项目说明", "项目正文\n"));

    const auto skills =
        tools::LoadSkills(root.BaseDir("proj"), root.BaseDir("home"), root.BaseDir("official"));
    REQUIRE(skills.size() == 2);
    const auto shared = std::find_if(skills.begin(), skills.end(),
                                     [](const tools::SkillMeta& meta) { return meta.name == "shared-skill"; });
    REQUIRE(shared != skills.end());
    CHECK(shared->description == "项目说明");
    CHECK(shared->source_level == "项目级");
    const auto official = std::find_if(skills.begin(), skills.end(),
                                       [](const tools::SkillMeta& meta) { return meta.name == "official-only"; });
    REQUIRE(official != skills.end());
    CHECK(official->source_level == "官方");
}

TEST_CASE("LoadSkills: 旧版播种的官方维护副本让位给发行包新版") {
    TempSkillsRoot root;
    root.WriteOfficialSkill("lubancode-config",
                            SkillContent("lubancode-config", "发行包新版", "官方正文\n"));
    root.WriteSkill("home", "lubancode-config",
                    SkillContent("lubancode-config", "主目录旧版",
                                 "<!-- lubancode 系统维护,随版本自动更新;自定义请另建技能 -->\n旧正文\n"));

    const auto skills =
        tools::LoadSkills(root.BaseDir("proj"), root.BaseDir("home"), root.BaseDir("official"));
    REQUIRE(skills.size() == 1);
    CHECK(skills[0].description == "发行包新版");
    CHECK(skills[0].source_level == "官方");
}

TEST_CASE("官方 lubancode-config SKILL.md 可解析且路由词齐全") {
    const std::filesystem::path root = std::filesystem::path(LUBANCODE_TEST_OFFICIAL_SKILLS_DIR);
    const auto skills = tools::ScanSkillsDir(root, "官方");
    const auto config = std::find_if(skills.begin(), skills.end(),
                                     [](const tools::SkillMeta& meta) { return meta.name == "lubancode-config"; });
    REQUIRE(config != skills.end());
    CHECK(config->description.find("MCP") != std::string::npos);
    CHECK(config->description.find("技能") != std::string::npos);
    CHECK(config->source_level == "官方");

    tools::SkillTool tool({*config});
    const auto loaded = tool.execute(nlohmann::json{{"name", "lubancode-config"}});
    REQUIRE_FALSE(loaded.is_error);
    CHECK(loaded.content.size() < 3000);
    CHECK(loaded.content.find("references/document-map.md") != std::string::npos);
    CHECK(loaded.content.find("../../docs") != std::string::npos);
    CHECK(loaded.content.find("`mcpServers` 的键是服务器名") == std::string::npos);

    const std::filesystem::path skill_dir = root / "lubancode-config";
    CHECK(std::filesystem::is_regular_file(skill_dir / "references" / "document-map.md"));
    CHECK(std::filesystem::weakly_canonical(skill_dir / ".." / ".." / "docs") ==
          std::filesystem::weakly_canonical(root.parent_path() / "docs"));
    CHECK(std::filesystem::is_regular_file(root.parent_path() / "docs" / "reference" / "configuration.md"));
}

TEST_CASE("LoadSkills: 一个技能都没有,返回空 vector") {
    TempSkillsRoot root;
    const auto skills = tools::LoadSkills(root.BaseDir("proj"), root.BaseDir("home"));
    CHECK(skills.empty());
}

TEST_CASE("BuildSkillsPromptSegment: 没有技能时返回空串,一个字都不注入") {
    const std::vector<tools::SkillMeta> empty_skills;
    CHECK(tools::BuildSkillsPromptSegment(empty_skills).empty());
}

TEST_CASE("BuildSkillsPromptSegment: 有技能时按格式列出") {
    std::vector<tools::SkillMeta> skills;
    skills.push_back(tools::SkillMeta{"poem-style", "五言绝句写作规范", "/some/dir", "项目级"});
    const std::string segment = tools::BuildSkillsPromptSegment(skills);
    CHECK(segment.find("可用技能") != std::string::npos);
    CHECK(segment.find("skill 工具") != std::string::npos);
    CHECK(segment.find("- poem-style: 五言绝句写作规范") != std::string::npos);
    CHECK(segment.find("~/.lubancode/skills") != std::string::npos);
    CHECK(segment.find(".agents/skills") != std::string::npos);
}

// ---------------------------------------------------------------------------
// 3) SkillTool
// ---------------------------------------------------------------------------

TEST_CASE("SkillTool: 命中,返回目录行 + body 正文") {
    TempSkillsRoot root;
    root.WriteSkill("proj", "poem-style", SkillContent("poem-style", "五言绝句", "写诗必须五言绝句。\n"));
    const auto skills = tools::ScanSkillsDir(
        std::filesystem::path(root.Path()) / "proj" / ".lubancode" / "skills", "项目级");
    REQUIRE(skills.size() == 1);

    tools::SkillTool tool(skills);
    nlohmann::json input;
    input["name"] = "poem-style";
    const auto result = tool.execute(input);

    CHECK_FALSE(result.is_error);
    CHECK(result.content.find("技能目录: ") != std::string::npos);
    CHECK(result.content.find(skills[0].dir_path) != std::string::npos);
    CHECK(result.content.find("写诗必须五言绝句") != std::string::npos);
}

TEST_CASE("SkillTool: 未命中,is_error 并列出可用名字") {
    tools::SkillTool tool({tools::SkillMeta{"a", "desc-a", "/dir/a", "项目级"},
                            tools::SkillMeta{"b", "desc-b", "/dir/b", "主目录级"}});
    nlohmann::json input;
    input["name"] = "不存在的技能";
    const auto result = tool.execute(input);

    CHECK(result.is_error);
    CHECK(result.content.find("a") != std::string::npos);
    CHECK(result.content.find("b") != std::string::npos);
}

TEST_CASE("SkillTool: 没有任何技能时,未命中提示信息不同") {
    tools::SkillTool tool({});
    nlohmann::json input;
    input["name"] = "随便什么";
    const auto result = tool.execute(input);

    CHECK(result.is_error);
    CHECK(result.content.find("没有扫描到任何技能") != std::string::npos);
}

TEST_CASE("SkillTool: 缺 name 参数报错") {
    tools::SkillTool tool({});
    const auto result = tool.execute(nlohmann::json::object());
    CHECK(result.is_error);
}
