// M9:技能系统测试。四块:
//   1) ParseSkillMarkdown —— 手写的 frontmatter 解析器,纯函数。
//   2) LoadSkills/ScanSkillsDir —— 真在临时目录里造两级技能目录,验证扫描
//      结果和"项目级覆盖主目录级"。
//   3) SkillTool —— 命中/未命中两种情况下 execute() 的返回内容。
//   4) EnumerateSkillLayers —— /skill list 的四层全量账:五处根摊开、四层
//      标签、遮蔽标注,与 LoadSkills 的胜者口径对齐。

#include <doctest/doctest.h>

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <set>
#include <string>
#include <system_error>
#include <vector>

#include "tools/skill_loader.hpp"
#include "tools/skill_tool.hpp"
#include "tools/path_utils.hpp"
#include "platform/sha256.hpp"

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

TEST_CASE("ParseSkillMarkdown: 真 YAML 多行 description 可解析") {
    const std::string content =
        "---\n"
        "name: multiline-description\n"
        "description: >\n"
        "  处理表格与图表。\n"
        "  用户提到数据分析时使用。\n"
        "metadata:\n"
        "  author: example\n"
        "---\n"
        "正文\n";
    const auto parsed = tools::ParseSkillMarkdown(content);
    REQUIRE(parsed.has_value());
    REQUIRE(parsed->description.has_value());
    CHECK(*parsed->description == "处理表格与图表。 用户提到数据分析时使用。\n");
}

TEST_CASE("ParseSkillMarkdown: 兼容 description 裸值里的冒号") {
    const std::string content =
        "---\n"
        "name: legacy-colon\n"
        "description: Use when: the user asks about PDFs\n"
        "---\n"
        "正文\n";
    const auto parsed = tools::ParseSkillMarkdown(content);
    REQUIRE(parsed.has_value());
    REQUIRE(parsed->description.has_value());
    CHECK(*parsed->description == "Use when: the user asks about PDFs");
}

TEST_CASE("Agent Skills name 校验遵守标准字汇与边界") {
    CHECK(tools::IsValidAgentSkillName("pdf-processing"));
    CHECK(tools::IsValidAgentSkillName("skill2"));
    CHECK_FALSE(tools::IsValidAgentSkillName("PDF-processing"));
    CHECK_FALSE(tools::IsValidAgentSkillName("pdf_processing"));
    CHECK_FALSE(tools::IsValidAgentSkillName("-pdf"));
    CHECK_FALSE(tools::IsValidAgentSkillName("pdf--processing"));
    CHECK_FALSE(tools::IsValidAgentSkillName(std::string(65, 'a')));
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

    void WriteAgentSkill(const std::string& base_subdir, const std::string& skill_name,
                         const std::string& content) const {
        const std::filesystem::path skill_dir = dir_ / base_subdir / ".agents" / "skills" / skill_name;
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

TEST_CASE("ScanSkillsDir: 缺 Agent Skills 必填元数据就跳过") {
    TempSkillsRoot root;
    root.WriteSkill("proj", "missing-name", "---\ndescription: 有说明\n---\n正文\n");
    root.WriteSkill("proj", "missing-description", "---\nname: missing-description\n---\n正文\n");
    root.WriteSkill("proj", "plain-markdown", "只有正文\n");

    const auto metas =
        tools::ScanSkillsDir(std::filesystem::path(root.Path()) / "proj" / ".lubancode" / "skills", "项目级");
    CHECK(metas.empty());
}

TEST_CASE("ScanSkillsDir: 名字不合规范或不匹配目录时警告但兼容加载") {
    TempSkillsRoot root;
    root.WriteSkill("proj", "folder-name", SkillContent("Legacy_Name", "旧客户端技能", "正文\n"));

    const auto metas =
        tools::ScanSkillsDir(std::filesystem::path(root.Path()) / "proj" / ".lubancode" / "skills", "项目级");
    REQUIRE(metas.size() == 1);
    CHECK(metas[0].name == "Legacy_Name");
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

TEST_CASE("LoadSkills: .agents 与原生目录五处按固定次序覆盖") {
    TempSkillsRoot root;
    root.WriteOfficialSkill("shared-skill", SkillContent("shared-skill", "官方", "官方正文\n"));
    root.WriteAgentSkill("home", "shared-skill", SkillContent("shared-skill", "用户共享", "正文\n"));
    root.WriteSkill("home", "shared-skill", SkillContent("shared-skill", "用户原生", "正文\n"));
    root.WriteAgentSkill("proj", "shared-skill", SkillContent("shared-skill", "项目共享", "正文\n"));
    root.WriteSkill("proj", "shared-skill", SkillContent("shared-skill", "项目原生", "正文\n"));
    root.WriteAgentSkill("home", "home-agent-only",
                         SkillContent("home-agent-only", "用户 .agents", "正文\n"));
    root.WriteAgentSkill("proj", "project-agent-only",
                         SkillContent("project-agent-only", "项目 .agents", "正文\n"));

    const auto skills =
        tools::LoadSkills(root.BaseDir("proj"), root.BaseDir("home"), root.BaseDir("official"));
    REQUIRE(skills.size() == 3);
    const auto shared = std::find_if(skills.begin(), skills.end(),
                                     [](const tools::SkillMeta& meta) { return meta.name == "shared-skill"; });
    REQUIRE(shared != skills.end());
    CHECK(shared->description == "项目原生");
    CHECK(shared->dir_path.find(".lubancode") != std::string::npos);

    const auto home_agent = std::find_if(
        skills.begin(), skills.end(), [](const tools::SkillMeta& meta) { return meta.name == "home-agent-only"; });
    REQUIRE(home_agent != skills.end());
    CHECK(home_agent->source_level == "主目录级");
    CHECK(home_agent->dir_path.find(".agents") != std::string::npos);

    const auto project_agent = std::find_if(
        skills.begin(), skills.end(), [](const tools::SkillMeta& meta) { return meta.name == "project-agent-only"; });
    REQUIRE(project_agent != skills.end());
    CHECK(project_agent->source_level == "项目级");
    CHECK(project_agent->dir_path.find(".agents") != std::string::npos);
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
    const auto schema = tool.input_schema();
    REQUIRE(schema["properties"]["name"]["enum"].is_array());
    CHECK(schema["properties"]["name"]["enum"] == nlohmann::json::array({"poem-style"}));
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

// ---------------------------------------------------------------------------
// 3.5) 应用Worker接入单 §六:漂移校验、受控资源读取、依赖声明消费
// ---------------------------------------------------------------------------

namespace {

// 一份带依赖声明的技能正文。
std::string SkillContentWithRequires(const std::string& name, const std::string& requires_yaml,
                                     const std::string& body) {
    std::string front = "---\nname: " + name + "\ndescription: " + name + " 的说明。\n";
    if (!requires_yaml.empty()) {
        front += requires_yaml;
    }
    return front + "---\n" + body;
}

}  // namespace

TEST_CASE("ParseSkillMarkdown: requires-tools 声明——真 YAML 路收清单,坏 YAML 回退路不收") {
    SUBCASE("字符串清单照收") {
        const auto parsed = tools::ParseSkillMarkdown(
            "---\nname: s\ndescription: d\nrequires-tools:\n  - run_command\n  - mcp__srv__echo\n---\nbody\n");
        REQUIRE(parsed.has_value());
        REQUIRE(parsed->requires_tools.has_value());
        REQUIRE(parsed->requires_tools->size() == 2);
        CHECK((*parsed->requires_tools)[0] == "run_command");
        CHECK((*parsed->requires_tools)[1] == "mcp__srv__echo");
    }
    SUBCASE("未声明 = nullopt(不冒充空清单)") {
        const auto parsed = tools::ParseSkillMarkdown("---\nname: s\ndescription: d\n---\nbody\n");
        REQUIRE(parsed.has_value());
        CHECK_FALSE(parsed->requires_tools.has_value());
    }
    SUBCASE("类型不对(标量)= 按未声明处理,不弃整份技能") {
        const auto parsed =
            tools::ParseSkillMarkdown("---\nname: s\ndescription: d\nrequires-tools: run_command\n---\nbody\n");
        REQUIRE(parsed.has_value());
        CHECK_FALSE(parsed->requires_tools.has_value());
    }
    SUBCASE("元素混入非标量(嵌套序列)= 整份声明不收") {
        const auto parsed = tools::ParseSkillMarkdown(
            "---\nname: s\ndescription: d\nrequires-tools:\n  - [run_command]\n  - ok-tool\n---\nbody\n");
        REQUIRE(parsed.has_value());
        CHECK_FALSE(parsed->requires_tools.has_value());
    }
}

TEST_CASE("ScanSkillsDir: 扫描件带 SKILL.md 冻结指纹与依赖声明") {
    TempSkillsRoot root;
    const std::filesystem::path skills_dir =
        std::filesystem::path(root.Path()) / "proj" / ".lubancode" / "skills";
    root.WriteSkill("proj", "helper", SkillContentWithRequires(
                                        "helper", "requires-tools:\n  - run_command\n",
                                        "依赖声明进清单。\n"));
    const auto skills = tools::ScanSkillsDir(skills_dir, "项目级");
    REQUIRE(skills.size() == 1);
    // 指纹 = SKILL.md 全文 SHA-256(64 位十六进制)。
    CHECK(skills[0].content_hash.size() == 64);
    bool hex = true;
    for (const char ch : skills[0].content_hash) {
        const bool ok = (ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f');
        hex = hex && ok;
    }
    CHECK(hex);
    REQUIRE(skills[0].requires_tools.size() == 1);
    CHECK(skills[0].requires_tools[0] == "run_command");
}

TEST_CASE("ScanSkillsDirReported: 坏技能的跳过人话进警告账(供 required 拒启诊断)") {
    TempSkillsRoot root;
    const std::filesystem::path skills_dir =
        std::filesystem::path(root.Path()) / "proj" / ".lubancode" / "skills";
    root.WriteSkill("proj", "good", SkillContent("good", "好技能", "正文。\n"));
    root.WriteSkill("proj", "broken",
                    "---\nname: broken\ndescription: 坏 frontmatter,没有闭合\n正文\n");
    std::vector<std::string> warnings;
    const auto skills = tools::ScanSkillsDirReported(skills_dir, "项目级", &warnings);
    REQUIRE(skills.size() == 1);
    CHECK(skills[0].name == "good");
    REQUIRE_FALSE(warnings.empty());
    CHECK(warnings[0].find("broken") != std::string::npos);
}

TEST_CASE("SkillTool 漂移校验:同场改 SKILL.md,再读即拒(§六 143)") {
    TempSkillsRoot root;
    const std::filesystem::path skills_dir =
        std::filesystem::path(root.Path()) / "proj" / ".lubancode" / "skills";
    root.WriteSkill("proj", "drifty", SkillContent("drifty", "会被改的技能", "原正文。\n"));
    auto skills = tools::ScanSkillsDir(skills_dir, "项目级");
    REQUIRE(skills.size() == 1);
    tools::SkillTool tool(skills);

    nlohmann::json input;
    input["name"] = "drifty";
    const auto before = tool.execute(input);
    CHECK_FALSE(before.is_error);
    CHECK(before.content.find("原正文") != std::string::npos);

    // 同场中途改文件:指纹对不上,拒读——不悄悄给修改版。
    root.WriteSkill("proj", "drifty", SkillContent("drifty", "会被改的技能", "偷改的正文。\n"));
    const auto after = tool.execute(input);
    CHECK(after.is_error);
    CHECK(after.error_code == "skill.drifted");
    CHECK(after.content.find("漂移") != std::string::npos);
    CHECK(after.content.find("偷改的正文") == std::string::npos);  // 修改版正文一个字不给
}

TEST_CASE("SkillTool 受控资源读取:技能内相对材料可读,越根/外链明拒(§六 144)") {
    TempSkillsRoot root;
    const std::filesystem::path skills_dir =
        std::filesystem::path(root.Path()) / "proj" / ".lubancode" / "skills";
    root.WriteSkill("proj", "with-refs", SkillContent("with-refs", "带引用材料", "正文见引用。\n"));
    const std::filesystem::path skill_dir = skills_dir / "with-refs";
    {
        std::error_code ec;
        std::filesystem::create_directories(skill_dir / "references", ec);
        std::ofstream out(skill_dir / "references" / "style.md", std::ios::binary);
        out << "引用材料正文。\n";
        std::ofstream outer(skills_dir / "outside.txt", std::ios::binary);
        outer << "根内但技能目录外的文件。\n";
    }
    const auto skills = tools::ScanSkillsDir(skills_dir, "项目级");
    REQUIRE(skills.size() == 1);
    tools::SkillTool tool(skills);

    const auto run = [&tool](const char* path) {
        nlohmann::json input;
        input["name"] = "with-refs";
        input["path"] = path;
        return tool.execute(input);
    };

    SUBCASE("技能内相对材料:读到") {
        const auto ok = run("references/style.md");
        CHECK_FALSE(ok.is_error);
        CHECK(ok.content.find("引用材料正文") != std::string::npos);
    }
    SUBCASE("子目录形式分隔符也认") {
        const auto ok = run("references\\style.md");
        CHECK_FALSE(ok.is_error);
    }
    SUBCASE(".. 越根:拒") {
        const auto escape = run("../outside.txt");
        CHECK(escape.is_error);
        CHECK(escape.content.find("越根") != std::string::npos);
    }
    SUBCASE("绝对路径:拒") {
        CHECK(run("/etc/passwd").is_error);
    }
    SUBCASE("盘符路径:拒") {
        CHECK(run("C:/windows/system32/config").is_error);
    }
    SUBCASE("段里带冒号(盘符段会顶掉前缀路径):拒") {
        CHECK(run("C:whatever").is_error);
        CHECK(run("references/C:evil").is_error);
    }
    SUBCASE("UNC:拒") {
        CHECK(run("\\\\server\\share\\file").is_error);
    }
    SUBCASE("外链 http:不自动 fetch,明示走数据源流程") {
        const auto link = run("https://example.com/data.json");
        CHECK(link.is_error);
        CHECK(link.content.find("外链") != std::string::npos);
        CHECK(link.content.find("数据源") != std::string::npos);
    }
    SUBCASE("file:// 也是外链:拒") {
        CHECK(run("file:///etc/passwd").is_error);
    }
    SUBCASE("空段(双斜杠):拒") {
        CHECK(run("references//style.md").is_error);
    }
    SUBCASE("不存在的材料:明说") {
        const auto missing = run("references/nope.md");
        CHECK(missing.is_error);
        CHECK(missing.content.find("不存在") != std::string::npos);
    }
    SUBCASE("指向技能目录本身的 path:拒") {
        CHECK(run(".").is_error);
    }
}

TEST_CASE("SkillTool 受控资源读取:目录内符号链接指向外头,解析后拒(§六 144)") {
    TempSkillsRoot root;
    const std::filesystem::path skills_dir =
        std::filesystem::path(root.Path()) / "proj" / ".lubancode" / "skills";
    root.WriteSkill("proj", "linked", SkillContent("linked", "带链接的技能", "正文。\n"));
    const std::filesystem::path skill_dir = skills_dir / "linked";
    const std::filesystem::path outside = std::filesystem::path(root.Path()) / "proj" / "secret.txt";
    {
        std::ofstream out(outside, std::ios::binary);
        out << "技能目录外的机密。\n";
    }
    std::error_code link_ec;
    std::filesystem::create_symlink(outside, skill_dir / "leak.md", link_ec);
    if (link_ec) {
        // 无符号链接权限的环境(部分 Windows 配置):如实缺证据,不冒充。
        MESSAGE("create_symlink 不可用(", link_ec.message(), "),跳过链接绕过用例");
        return;
    }
    const auto skills = tools::ScanSkillsDir(skills_dir, "项目级");
    REQUIRE(skills.size() == 1);
    tools::SkillTool tool(skills);
    nlohmann::json input;
    input["name"] = "linked";
    input["path"] = "leak.md";
    const auto result = tool.execute(input);
    CHECK(result.is_error);
    CHECK(result.content.find("越出技能目录") != std::string::npos);
    CHECK(result.content.find("机密") == std::string::npos);
}

TEST_CASE("SkillTool 依赖声明消费:缺获准执行工具回 capability_unavailable(§六 145)") {
    TempSkillsRoot root;
    const std::filesystem::path skills_dir =
        std::filesystem::path(root.Path()) / "proj" / ".lubancode" / "skills";
    root.WriteSkill("proj", "needs-shell",
                    SkillContentWithRequires("needs-shell", "requires-tools:\n  - run_command\n", "要 shell。\n"));
    root.WriteSkill("proj", "self-contained", SkillContent("self-contained", "无依赖", "自己就够。\n"));
    const auto skills = tools::ScanSkillsDir(skills_dir, "项目级");
    REQUIRE(skills.size() == 2);

    SUBCASE("递了冻结工具面:声明的工具不在面上,加载明拒 capability_unavailable") {
        tools::SkillTool tool(skills, std::set<std::string>{"skill"});
        nlohmann::json input;
        input["name"] = "needs-shell";
        const auto result = tool.execute(input);
        CHECK(result.is_error);
        CHECK(result.error_code == "capability_unavailable");
        CHECK(result.content.find("capability_unavailable") != std::string::npos);
        CHECK(result.content.find("run_command") != std::string::npos);
        CHECK(result.content.find("要 shell") == std::string::npos);  // 正文不给——依赖没过
    }
    SUBCASE("面上有声明的工具:照常加载") {
        tools::SkillTool tool(skills, std::set<std::string>{"skill", "run_command"});
        nlohmann::json input;
        input["name"] = "needs-shell";
        const auto result = tool.execute(input);
        CHECK_FALSE(result.is_error);
        CHECK(result.content.find("要 shell") != std::string::npos);
    }
    SUBCASE("没递工具面(终端缺省路):不执法,声明只留在清单") {
        tools::SkillTool tool(skills);
        nlohmann::json input;
        input["name"] = "needs-shell";
        const auto result = tool.execute(input);
        CHECK_FALSE(result.is_error);
    }
    SUBCASE("无声明技能不受影响") {
        tools::SkillTool tool(skills, std::set<std::string>{"skill"});
        nlohmann::json input;
        input["name"] = "self-contained";
        const auto result = tool.execute(input);
        CHECK_FALSE(result.is_error);
    }
}

// ---------------------------------------------------------------------------
// 4) EnumerateSkillLayers:/skill list 的四层全量账。
// ---------------------------------------------------------------------------

namespace {

// 按名字 + 路径片段找枚举条目(.agents 两处与 .lubancode 两处可能同名,
// 靠路径片段区分层)。分隔符跟现场走(Windows 反斜杠、POSIX 正斜杠),
// 免得硬编码斜杠跨平台翻车。
const tools::SkillLayerEntry& FindEntry(const std::vector<tools::SkillLayerEntry>& entries,
                                        const std::string& name, const std::string& base_subdir,
                                        const std::string& root_kind) {
    std::string sep = "/";
    if (!entries.empty() && entries.front().meta.dir_path.find('\\') != std::string::npos) {
        sep = "\\";
    }
    const std::string needle =
        root_kind.empty() ? (sep + base_subdir + sep) : (sep + base_subdir + sep + root_kind + sep);
    const auto it = std::find_if(entries.begin(), entries.end(), [&](const tools::SkillLayerEntry& e) {
        return e.meta.name == name && e.meta.dir_path.find(needle) != std::string::npos;
    });
    REQUIRE(it != entries.end());
    return *it;
}

}  // namespace

TEST_CASE("EnumerateSkillLayers: 五处根全数摊开,四层标签对号,无遮蔽全生效") {
    TempSkillsRoot root;
    root.WriteOfficialSkill("official-only", SkillContent("official-only", "官方技能", "正文\n"));
    root.WriteAgentSkill("home", "home-agent", SkillContent("home-agent", "用户共享", "正文\n"));
    root.WriteSkill("home", "home-native", SkillContent("home-native", "用户原生", "正文\n"));
    root.WriteAgentSkill("proj", "proj-agent", SkillContent("proj-agent", "项目共享", "正文\n"));
    root.WriteSkill("proj", "proj-native", SkillContent("proj-native", "项目原生", "正文\n"));

    const auto entries = tools::EnumerateSkillLayers(root.BaseDir("proj"), root.BaseDir("home"),
                                                     root.BaseDir("official"));
    REQUIRE(entries.size() == 5);
    for (const auto& entry : entries) {
        CHECK(entry.active);
        CHECK(entry.shadowed_by.empty());
    }
    CHECK(FindEntry(entries, "official-only", "official", "").meta.source_level == "官方");
    CHECK(FindEntry(entries, "home-agent", "home", ".agents").meta.source_level == "agents 共享");
    CHECK(FindEntry(entries, "home-native", "home", ".lubancode").meta.source_level == "主目录级");
    CHECK(FindEntry(entries, "proj-agent", "proj", ".agents").meta.source_level == "agents 共享");
    CHECK(FindEntry(entries, "proj-native", "proj", ".lubancode").meta.source_level == "项目级");
}

TEST_CASE("EnumerateSkillLayers: 五处同名,遮蔽链逐级标注,胜者与 LoadSkills 对齐") {
    TempSkillsRoot root;
    root.WriteOfficialSkill("shared-skill", SkillContent("shared-skill", "官方", "官方正文\n"));
    root.WriteAgentSkill("home", "shared-skill", SkillContent("shared-skill", "用户共享", "正文\n"));
    root.WriteSkill("home", "shared-skill", SkillContent("shared-skill", "用户原生", "正文\n"));
    root.WriteAgentSkill("proj", "shared-skill", SkillContent("shared-skill", "项目共享", "正文\n"));
    root.WriteSkill("proj", "shared-skill", SkillContent("shared-skill", "项目原生", "正文\n"));

    const auto entries = tools::EnumerateSkillLayers(root.BaseDir("proj"), root.BaseDir("home"),
                                                     root.BaseDir("official"));
    REQUIRE(entries.size() == 5);

    // 胜者:项目原生(.lubancode),其余四份都被后到的高优先级层顶掉。
    const auto winner = FindEntry(entries, "shared-skill", "proj", ".lubancode");
    CHECK(winner.active);
    CHECK(winner.shadowed_by.empty());
    CHECK(winner.meta.description == "项目原生");

    const auto official = FindEntry(entries, "shared-skill", "official", "");
    CHECK_FALSE(official.active);
    CHECK(official.shadowed_by == "agents 共享");

    const auto home_agent = FindEntry(entries, "shared-skill", "home", ".agents");
    CHECK_FALSE(home_agent.active);
    CHECK(home_agent.shadowed_by == "主目录级");

    const auto home_native = FindEntry(entries, "shared-skill", "home", ".lubancode");
    CHECK_FALSE(home_native.active);
    CHECK(home_native.shadowed_by == "agents 共享");

    const auto proj_agent = FindEntry(entries, "shared-skill", "proj", ".agents");
    CHECK_FALSE(proj_agent.active);
    CHECK(proj_agent.shadowed_by == "项目级");

    // 与 LoadSkills 同一套合并法则:唯一生效条目就是 LoadSkills 的那条。
    const auto loaded = tools::LoadSkills(root.BaseDir("proj"), root.BaseDir("home"), root.BaseDir("official"));
    REQUIRE(loaded.size() == 1);
    CHECK(loaded[0].description == "项目原生");
}

TEST_CASE("EnumerateSkillLayers: 旧版播种的官方副本让位并标注;无新版时照常生效") {
    TempSkillsRoot root;
    root.WriteOfficialSkill("lubancode-config", SkillContent("lubancode-config", "发行包新版", "官方正文\n"));
    root.WriteSkill("home", "lubancode-config",
                    SkillContent("lubancode-config", "主目录旧版",
                                 "<!-- lubancode 系统维护,随版本自动更新;自定义请另建技能 -->\n旧正文\n"));

    const auto entries =
        tools::EnumerateSkillLayers(root.BaseDir("proj"), root.BaseDir("home"), root.BaseDir("official"));
    REQUIRE(entries.size() == 2);
    const auto official = FindEntry(entries, "lubancode-config", "official", "");
    CHECK(official.active);
    CHECK(official.meta.description == "发行包新版");
    const auto legacy = FindEntry(entries, "lubancode-config", "home", ".lubancode");
    CHECK_FALSE(legacy.active);
    CHECK(legacy.shadowed_by == "官方");

    // 官方层没有同名新版时,副本不跟谁让位,照常生效。
    TempSkillsRoot solo;
    solo.WriteSkill("home", "lubancode-config",
                    SkillContent("lubancode-config", "主目录旧版",
                                 "<!-- lubancode 系统维护,随版本自动更新;自定义请另建技能 -->\n旧正文\n"));
    const auto solo_entries = tools::EnumerateSkillLayers(solo.BaseDir("proj"), solo.BaseDir("home"));
    REQUIRE(solo_entries.size() == 1);
    CHECK(solo_entries[0].active);
    CHECK(solo_entries[0].meta.description == "主目录旧版");
}

TEST_CASE("EnumerateSkillLayers: 主目录与官方缺位,只剩项目两处也照数") {
    TempSkillsRoot root;
    root.WriteSkill("proj", "proj-native", SkillContent("proj-native", "项目原生", "正文\n"));
    root.WriteAgentSkill("proj", "proj-agent", SkillContent("proj-agent", "项目共享", "正文\n"));

    const auto entries = tools::EnumerateSkillLayers(root.BaseDir("proj"), std::nullopt, std::nullopt);
    REQUIRE(entries.size() == 2);
    CHECK(FindEntry(entries, "proj-native", "proj", ".lubancode").meta.source_level == "项目级");
    CHECK(FindEntry(entries, "proj-agent", "proj", ".agents").meta.source_level == "agents 共享");
}

namespace {
constexpr const char* kStrictSkillSource = "技能仅来自宿主显式目录；未列名不加载。";

void WriteStrictSkillFile(const std::filesystem::path& path, const std::string& bytes) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream file(path, std::ios::binary);
    REQUIRE(file.is_open());
    file.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    file.close();
    REQUIRE(file.good());
}

struct StrictSkillsFixture {
    TempSkillsRoot temporary;
    std::filesystem::path root = std::filesystem::path(temporary.Path()) / "explicit";
    StrictSkillsFixture() { std::filesystem::create_directories(root); }
    void Write(const std::string& name, const std::string& content) const {
        WriteStrictSkillFile(root / name / "SKILL.md", content);
    }
    auto Scan(const std::vector<std::string>& names) const {
        return tools::ScanSkillsDirStrict(root, names, kStrictSkillSource);
    }
};

void RequireSkillSymlink(const std::filesystem::path& target, const std::filesystem::path& link,
                         bool directory = false) {
    std::error_code ec;
    if (directory) std::filesystem::create_directory_symlink(target, link, ec);
    else std::filesystem::create_symlink(target, link, ec);
    REQUIRE_MESSAGE(!ec, "Strict Skills link coverage unavailable: ", ec.message());
}
}

TEST_CASE("StrictSkills: pure parsing preserves exact body and string types") {
    const auto parsed = tools::ParseSkillMarkdownStrict(
        "---\r\nname: alpha\r\ndescription: '处理当前材料'\r\nrequires-tools: [run_command, mcp__srv__echo]\r\n---\r\nBODY\r\n");
    REQUIRE(parsed.has_value());
    CHECK(parsed->name == "alpha");
    CHECK(parsed->description == "处理当前材料");
    CHECK(parsed->body == "BODY\r\n");
    REQUIRE(parsed->requires_tools.has_value());
    const std::vector<std::string> expected_tools{"run_command", "mcp__srv__echo"};
    CHECK(*parsed->requires_tools == expected_tools);
    const auto quoted = tools::ParseSkillMarkdownStrict("---\nname: '123'\ndescription: 'true'\n---\nBODY");
    REQUIRE(quoted.has_value());
    CHECK(quoted->name == "123");
    CHECK(quoted->description == "true");
}

TEST_CASE("StrictSkills: invalid YAML dependencies and duplicate keys never take legacy fallback") {
    const std::vector<std::string> fronts = {
        "name: alpha\ndescription: Use when: old format\n",
        "name: alpha\ndescription: valid\nrequires-tools: run_command\n",
        "name: alpha\ndescription: valid\nrequires-tools: [[run_command]]\n",
        "name: alpha\ndescription: valid\nrequires-tools: [true]\n",
        "name: alpha\ndescription: valid\nrequires-tools: [17]\n",
        "name: alpha\ndescription: valid\nrequires-tools: [null]\n",
        "name: alpha\ndescription: valid\nrequires-tools: ['']\n",
        "name: alpha\ndescription: valid\nrequires-tools: [run_command, run_command]\n",
        "name: alpha\nname: beta\ndescription: valid\n",
        "name: alpha\ndescription: first\ndescription: second\n",
        "name: alpha\ndescription: valid\nrequires-tools: broken\nrequires-tools: []\n",
        "name: alpha\ndescription: false\n",
    };
    for (const auto& front : fronts) {
        INFO(front);
        CHECK_FALSE(tools::ParseSkillMarkdownStrict("---\n" + front + "---\nBODY").has_value());
    }
    CHECK(tools::ParseSkillMarkdown("---\nname: alpha\ndescription: Use when: old format\n---\nBODY").has_value());
    CHECK(tools::ParseSkillMarkdown("---\nname: alpha\ndescription: valid\nrequires-tools: broken\n---\nBODY").has_value());
}

TEST_CASE("StrictSkills: parser validates Unicode NUL description and frontmatter budgets") {
    std::string description;
    for (int i = 0; i < 1024; ++i) description += "字";
    REQUIRE(tools::ParseSkillMarkdownStrict(SkillContent("alpha", description, "BODY")).has_value());
    CHECK_FALSE(tools::ParseSkillMarkdownStrict(SkillContent("alpha", description + "字", "BODY")).has_value());
    CHECK_FALSE(tools::ParseSkillMarkdownStrict(SkillContent("alpha", "valid", std::string("A\0B", 3))).has_value());
    CHECK_FALSE(tools::ParseSkillMarkdownStrict(SkillContent("alpha", "valid", std::string(1, '\xff'))).has_value());
    CHECK_FALSE(tools::ParseSkillMarkdownStrict("---\nname: alpha\ndescription: \"A\\0B\"\n---\nBODY").has_value());
    const auto front_limit = tools::ParseSkillMarkdownStrict(
        "---\nname: alpha\ndescription: valid\n#" + std::string(tools::StrictSkillLimits::frontmatter_bytes, 'F') + "\n---\nBODY");
    REQUIRE_FALSE(front_limit.has_value());
    CHECK(front_limit.error().code == "skill.frontmatter.limit");
    const auto file_limit = tools::ParseSkillMarkdownStrict(std::string(tools::StrictSkillLimits::file_bytes + 1, 'B'));
    REQUIRE_FALSE(file_limit.has_value());
    CHECK(file_limit.error().code == "skill.file.limit");
    auto dependencies = std::string("---\nname: alpha\ndescription: valid\nrequires-tools:\n");
    for (int i = 0; i < 65; ++i) dependencies += "  - tool" + std::to_string(i) + "\n";
    CHECK_FALSE(tools::ParseSkillMarkdownStrict(dependencies + "---\nBODY").has_value());
    CHECK_FALSE(tools::ParseSkillMarkdownStrict(
        "---\nname: alpha\ndescription: valid\nrequires-tools: ['" + std::string(513, 'T') + "']\n---\nBODY").has_value());
}

TEST_CASE("StrictSkills: exact explicit scan owns same-byte hashes sorted selection and source prompt") {
    StrictSkillsFixture fixture;
    fixture.root /= tools::Utf8ToPath("技能源-🚀");
    std::filesystem::create_directories(fixture.root);
    const auto alpha = SkillContent("alpha", "ALPHA", "A-BODY\n");
    fixture.Write("alpha", alpha);
    fixture.Write("beta", SkillContent("beta", "BETA", "B-BODY\n"));
    fixture.Write("decoy", SkillContent("decoy", "DECOY", "D-BODY\n"));
    WriteStrictSkillFile(fixture.root.parent_path() / ".agents" / "skills" / "ambient" / "SKILL.md",
                         SkillContent("ambient", "AMBIENT", "WRONG\n"));
    const auto scan = fixture.Scan({"beta", "alpha"});
    REQUIRE(scan.has_value());
    REQUIRE(scan->skills.size() == 2);
    CHECK(scan->skills[0].name == "alpha");
    CHECK(scan->skills[1].name == "beta");
    CHECK(scan->skills[0].content_hash == platform::Sha256Hex(alpha));
    CHECK(scan->skills[0].dir_path == tools::PathToUtf8(std::filesystem::canonical(fixture.root / "alpha")));
    CHECK(scan->skills[0].skill_path == tools::PathToUtf8(std::filesystem::canonical(fixture.root / "alpha" / "SKILL.md")));
    CHECK(scan->skills[0].source_dir_path == tools::PathToUtf8(fixture.root / "alpha"));
    CHECK(scan->read_policy.canonical_root_path == std::filesystem::canonical(fixture.root));
    CHECK(scan->prompt_segment == std::string(kStrictSkillSource) + "\n可用技能(用 skill 工具按名加载):\n- alpha: ALPHA\n- beta: BETA");
    CHECK(scan->diagnostics.empty());
    CHECK(scan->prompt_segment.find("DECOY") == std::string::npos);
    CHECK(scan->prompt_segment.find("AMBIENT") == std::string::npos);
}

TEST_CASE("StrictSkills: missing roots empty duplicate selections and implicit sources fail closed") {
    StrictSkillsFixture fixture;
    fixture.Write("alpha", SkillContent("alpha", "valid", "BODY"));
    CHECK_FALSE(fixture.Scan({}).has_value());
    CHECK_FALSE(fixture.Scan({"alpha", "alpha"}).has_value());
    CHECK_FALSE(fixture.Scan({"Alpha"}).has_value());
    CHECK_FALSE(fixture.Scan({"missing"}).has_value());
    CHECK_FALSE(tools::ScanSkillsDirStrict("relative", {"alpha"}, kStrictSkillSource).has_value());
    CHECK_FALSE(tools::ScanSkillsDirStrict(fixture.root / "missing", {"alpha"}, kStrictSkillSource).has_value());
    CHECK_FALSE(tools::ScanSkillsDirStrict(fixture.root / "alpha" / "SKILL.md", {"alpha"}, kStrictSkillSource).has_value());
    CHECK_FALSE(tools::ScanSkillsDirStrict(fixture.root, {"alpha"}, "").has_value());
    std::vector<std::string> names(129, "alpha");
    CHECK_FALSE(fixture.Scan(names).has_value());
}

TEST_CASE("StrictSkills: selected invalid metadata refuses while unselected bad metadata is diagnosed") {
    StrictSkillsFixture fixture;
    fixture.Write("alpha", SkillContent("alpha", "valid", "BODY"));
    fixture.Write("broken", "---\nname: broken\ndescription: Use when: legacy\n---\nBODY");
    const auto allowed = fixture.Scan({"alpha"});
    REQUIRE(allowed.has_value());
    REQUIRE(allowed->skills.size() == 1);
    CHECK_FALSE(allowed->diagnostics.empty());
    CHECK_FALSE(fixture.Scan({"broken"}).has_value());
    fixture.Write("alpha", SkillContent("another-name", "valid", "BODY"));
    const auto mismatch = fixture.Scan({"alpha"});
    REQUIRE_FALSE(mismatch.has_value());
    CHECK(mismatch.error().code == "skill.metadata.invalid");
}

TEST_CASE("StrictSkills: actual reads stop at cap plus one and charge invalid text") {
    StrictSkillsFixture fixture;
    const auto path = fixture.root / "ordinary.txt";
    WriteStrictSkillFile(path, std::string(1024, 'A'));
    std::size_t read = 0;
    const auto limited = tools::ReadSkillFileBounded(path, 32, &read);
    REQUIRE_FALSE(limited.has_value());
    CHECK(limited.error().code == "skill.file.limit");
    CHECK(read == 33);
    const auto complete = tools::ReadSkillFileBounded(path, 1024, &read);
    REQUIRE(complete.has_value());
    CHECK(complete->size() == 1024);
    CHECK(read == 1024);
    WriteStrictSkillFile(path, std::string("A\0B", 3));
    const auto nul = tools::ReadSkillFileBounded(path, 32, &read);
    REQUIRE_FALSE(nul.has_value());
    CHECK(nul.error().code == "skill.text.invalid");
    CHECK(read == 3);
    CHECK_FALSE(tools::ReadSkillFileBounded(fixture.root).has_value());
}

TEST_CASE("StrictSkills: scan total charges unselected invalid UTF8 and candidate limits include unselected") {
    SUBCASE("invalid text consumes the total scan budget") {
        StrictSkillsFixture fixture;
        std::string invalid(tools::StrictSkillLimits::file_bytes, 'B');
        invalid[0] = '\xff';
        for (int i = 0; i < 17; ++i) fixture.Write("bad" + std::to_string(i), invalid);
        fixture.Write("valid", SkillContent("valid", "valid", "BODY"));
        const auto scan = fixture.Scan({"valid"});
        REQUIRE_FALSE(scan.has_value());
        CHECK(scan.error().code == "skill.scan.limit");
    }
    SUBCASE("unselected candidates consume the candidate budget") {
        StrictSkillsFixture fixture;
        fixture.Write("alpha", SkillContent("alpha", "valid", "BODY"));
        for (int i = 0; i < 127; ++i) fixture.Write("unused" + std::to_string(i), "bad metadata");
        REQUIRE(fixture.Scan({"alpha"}).has_value());
        fixture.Write("extra", "bad metadata");
        const auto scan = fixture.Scan({"alpha"});
        REQUIRE_FALSE(scan.has_value());
        CHECK(scan.error().code == "skill.scan.limit");
    }
}

TEST_CASE("StrictSkills: every root entry counts including noncandidate files") {
    StrictSkillsFixture fixture;
    fixture.Write("alpha", SkillContent("alpha", "valid", "BODY"));
    for (int i = 0; i < 4095; ++i) WriteStrictSkillFile(fixture.root / ("empty" + std::to_string(i)), "");
    REQUIRE(fixture.Scan({"alpha"}).has_value());
    WriteStrictSkillFile(fixture.root / "one-too-many", "");
    const auto scan = fixture.Scan({"alpha"});
    REQUIRE_FALSE(scan.has_value());
    CHECK(scan.error().code == "skill.scan.limit");
}

TEST_CASE("StrictSkills: whole-file boundary and final serialized prompt limits are independent") {
    SUBCASE("whole file includes frontmatter") {
        StrictSkillsFixture fixture;
        auto bytes = SkillContent("alpha", "valid", "BODY");
        bytes.append(tools::StrictSkillLimits::file_bytes - bytes.size(), 'P');
        fixture.Write("alpha", bytes);
        REQUIRE(fixture.Scan({"alpha"}).has_value());
        fixture.Write("alpha", bytes + "P");
        const auto over = fixture.Scan({"alpha"});
        REQUIRE_FALSE(over.has_value());
        CHECK(over.error().code == "skill.scan.limit");
    }
    SUBCASE("valid individual metadata can exceed aggregate prompt cap") {
        StrictSkillsFixture fixture;
        std::vector<std::string> names;
        for (int i = 0; i < 65; ++i) {
            const auto name = "skill" + std::to_string(i);
            names.push_back(name);
            fixture.Write(name, SkillContent(name, std::string(1024, 'D'), "BODY"));
        }
        const auto scan = fixture.Scan(names);
        REQUIRE_FALSE(scan.has_value());
        CHECK(scan.error().code == "skill.prompt.limit");
    }
}

TEST_CASE("StrictSkills: body drift fails while ordinary attachment updates remain live") {
    StrictSkillsFixture fixture;
    fixture.Write("alpha", SkillContent("alpha", "valid", "FROZEN-BODY"));
    WriteStrictSkillFile(fixture.root / "alpha" / "references" / "note.md", "NOTE-ONE");
    const auto scan = fixture.Scan({"alpha"});
    REQUIRE(scan.has_value());
    tools::SkillTool tool(scan->skills, std::set<std::string>{"skill"}, scan->read_policy);
    CHECK(tool.execute({{"name", "alpha"}}).content.find("FROZEN-BODY") != std::string::npos);
    const auto note_one = tool.execute({{"name", "alpha"}, {"path", "references/note.md"}});
    CHECK(note_one.content == "技能材料 alpha/references/note.md:\nNOTE-ONE");
    WriteStrictSkillFile(fixture.root / "alpha" / "references" / "note.md", "NOTE-TWO");
    fixture.Write("alpha", SkillContent("alpha", "valid", "NEW-BODY"));
    const auto changed = tool.execute({{"name", "alpha"}});
    CHECK(changed.is_error);
    CHECK(changed.error_code == "skill.drifted");
    CHECK(changed.content.find("NEW-BODY") == std::string::npos);
    const auto note_two = tool.execute({{"name", "alpha"}, {"path", "references/note.md"}});
    CHECK_FALSE(note_two.is_error);
    CHECK(note_two.content == "技能材料 alpha/references/note.md:\nNOTE-TWO");
    CHECK_THROWS(tool.SetSkills({}));
}

TEST_CASE("StrictSkills: explicit path rejects null empty tail segments invalid text and oversized material") {
    StrictSkillsFixture fixture;
    fixture.Write("alpha", SkillContent("alpha", "valid", "BODY"));
    WriteStrictSkillFile(fixture.root / "alpha" / "note.md", "NOTE");
    const auto scan = fixture.Scan({"alpha"});
    REQUIRE(scan.has_value());
    tools::SkillTool tool(scan->skills, std::set<std::string>{"skill"}, scan->read_policy);
    const std::vector<nlohmann::json> paths = {nullptr, 17, "", "note.md/", "note.md\\", "note.md//",
        ".", "../note.md", "/note.md", "C:note.md", "\\\\server\\share", "http://example.com",
        std::string("a\0b", 3), std::string(1, '\xff')};
    for (const auto& path : paths) {
        const auto reply = tool.execute({{"name", "alpha"}, {"path", path}});
        CHECK(reply.is_error);
        CHECK(reply.error_code == "skill.path.invalid");
    }
    WriteStrictSkillFile(fixture.root / "alpha" / "note.md", std::string("A\0B", 3));
    CHECK(tool.execute({{"name", "alpha"}, {"path", "note.md"}}).error_code == "skill.text.invalid");
    WriteStrictSkillFile(fixture.root / "alpha" / "note.md", std::string(tools::StrictSkillLimits::file_bytes + 1, 'N'));
    CHECK(tool.execute({{"name", "alpha"}, {"path", "note.md"}}).error_code == "skill.file.limit");
    std::filesystem::create_directory(fixture.root / "alpha" / "folder");
    CHECK(tool.execute({{"name", "alpha"}, {"path", "folder"}}).is_error);
}

TEST_CASE("StrictSkills: ordinary body and symlink hardlink aliases cannot bypass fingerprint via path") {
    StrictSkillsFixture fixture;
    fixture.Write("alpha", SkillContent("alpha", "valid", "BODY"));
    const auto body = fixture.root / "alpha" / "SKILL.md";
    RequireSkillSymlink(body, fixture.root / "alpha" / "body-link.md");
    std::error_code ec;
    std::filesystem::create_hard_link(body, fixture.root / "alpha" / "body-hard.md", ec);
    REQUIRE_MESSAGE(!ec, "Strict Skills hard-link coverage unavailable: ", ec.message());
    const auto scan = fixture.Scan({"alpha"});
    REQUIRE(scan.has_value());
    tools::SkillTool tool(scan->skills, std::set<std::string>{"skill"}, scan->read_policy);
    fixture.Write("alpha", SkillContent("alpha", "valid", "NEW-BODY"));
    for (const auto* path : {"SKILL.md", "body-link.md", "body-hard.md"}) {
        const auto reply = tool.execute({{"name", "alpha"}, {"path", path}});
        CHECK(reply.is_error);
        CHECK(reply.error_code == "skill.path.body_alias");
        CHECK(reply.content.find("NEW-BODY") == std::string::npos);
    }
    const auto outside = fixture.root / "outside.md";
    WriteStrictSkillFile(outside, "OUTSIDE");
    RequireSkillSymlink(outside, fixture.root / "alpha" / "escape.md");
    CHECK(tool.execute({{"name", "alpha"}, {"path", "escape.md"}}).error_code == "skill.path.outside");

    SUBCASE("an alpha attachment cannot hard-link the selected beta body") {
        fixture.Write("beta", SkillContent("beta", "valid", "BETA-BODY"));
        std::filesystem::create_directories(fixture.root / "alpha" / "references");
        std::filesystem::create_hard_link(fixture.root / "beta" / "SKILL.md",
            fixture.root / "alpha" / "references" / "beta.txt", ec);
        REQUIRE_MESSAGE(!ec, "Cross-skill hard-link coverage unavailable: ", ec.message());
        const auto both = fixture.Scan({"alpha", "beta"});
        REQUIRE(both.has_value());
        tools::SkillTool selected(both->skills, std::set<std::string>{"skill"}, both->read_policy);
        fixture.Write("beta", SkillContent("beta", "valid", "CHANGED-BETA-BODY"));
        CHECK(selected.execute({{"name", "beta"}}).error_code == "skill.drifted");
        const auto reply = selected.execute({{"name", "alpha"}, {"path", "references/beta.txt"}});
        CHECK(reply.is_error);
        CHECK(reply.error_code == "skill.path.body_alias");
        CHECK(reply.content.find("CHANGED-BETA-BODY") == std::string::npos);
    }
    SUBCASE("nested SKILL.md is reserved even without another selected body identity") {
        fixture.Write("beta", SkillContent("beta", "valid", "BETA-BODY"));
        WriteStrictSkillFile(fixture.root / "alpha" / "references" / "SKILL.md", "NESTED-BODY");
        for (bool select_beta : {false, true}) {
            const auto selected_names = select_beta ? std::vector<std::string>{"alpha", "beta"} :
                std::vector<std::string>{"alpha"};
            const auto names = fixture.Scan(selected_names);
            REQUIRE(names.has_value());
            tools::SkillTool selected(names->skills, std::set<std::string>{"skill"}, names->read_policy);
            const auto nested = selected.execute({{"name", "alpha"}, {"path", "references/SKILL.md"}});
            CHECK(nested.error_code == "skill.path.body_alias");
            CHECK(nested.content.find("NESTED-BODY") == std::string::npos);
#ifdef _WIN32
            CHECK(selected.execute({{"name", "alpha"}, {"path", "references/sKiLl.Md"}}).error_code == "skill.path.body_alias");
#else
            WriteStrictSkillFile(fixture.root / "alpha" / "references" / "skill.md", "LIVE-LOWERCASE");
            const auto lowercase = selected.execute({{"name", "alpha"}, {"path", "references/skill.md"}});
            CHECK_FALSE(lowercase.is_error);
            CHECK(lowercase.content == "技能材料 alpha/references/skill.md:\nLIVE-LOWERCASE");
#endif
        }
    }
    SUBCASE("missing selected body makes attachment identity verification fail closed") {
        fixture.Write("beta", SkillContent("beta", "valid", "BETA-BODY"));
        WriteStrictSkillFile(fixture.root / "alpha" / "ordinary.txt", "LIVE-MATERIAL");
        const auto both = fixture.Scan({"alpha", "beta"});
        REQUIRE(both.has_value());
        tools::SkillTool selected(both->skills, std::set<std::string>{"skill"}, both->read_policy);
        REQUIRE_FALSE(selected.execute({{"name", "alpha"}, {"path", "ordinary.txt"}}).is_error);
        REQUIRE(std::filesystem::remove(fixture.root / "beta" / "SKILL.md"));
        const auto missing = selected.execute({{"name", "alpha"}, {"path", "ordinary.txt"}});
        CHECK(missing.is_error);
        CHECK(missing.error_code == "skill.source.changed");
        CHECK(missing.content.find("LIVE-MATERIAL") == std::string::npos);
    }
}

TEST_CASE("StrictSkills: directory aliases refuse ambiguous scans and frozen retargeting") {
    SUBCASE("two candidates cannot map to one real directory") {
        StrictSkillsFixture fixture;
        fixture.Write("alpha", SkillContent("alpha", "valid", "BODY"));
        RequireSkillSymlink(fixture.root / "alpha", fixture.root / "alias", true);
        const auto scan = fixture.Scan({"alpha"});
        REQUIRE_FALSE(scan.has_value());
        CHECK(scan.error().code == "skill.source.duplicate");
    }
    SUBCASE("same bytes in a new target do not revive the frozen directory mapping") {
        StrictSkillsFixture fixture;
        const auto first = fixture.root / "storage" / "first";
        const auto second = fixture.root / "storage" / "second";
        WriteStrictSkillFile(first / "SKILL.md", SkillContent("alpha", "valid", "BODY"));
        WriteStrictSkillFile(second / "SKILL.md", SkillContent("alpha", "valid", "BODY"));
        WriteStrictSkillFile(first / "note.md", "FIRST");
        WriteStrictSkillFile(second / "note.md", "SECOND");
        RequireSkillSymlink(first, fixture.root / "alpha", true);
        const auto scan = fixture.Scan({"alpha"});
        REQUIRE(scan.has_value());
        tools::SkillTool tool(scan->skills, std::set<std::string>{"skill"}, scan->read_policy);
        REQUIRE_FALSE(tool.execute({{"name", "alpha"}}).is_error);
        std::filesystem::remove(fixture.root / "alpha");
        RequireSkillSymlink(second, fixture.root / "alpha", true);
        CHECK(tool.execute({{"name", "alpha"}}).error_code == "skill.source.changed");
        CHECK(tool.execute({{"name", "alpha"}, {"path", "note.md"}}).error_code == "skill.source.changed");
    }
    SUBCASE("declared root mapping cannot be retargeted") {
        StrictSkillsFixture first, second;
        first.Write("alpha", SkillContent("alpha", "valid", "SAME-BODY"));
        second.Write("alpha", SkillContent("alpha", "valid", "SAME-BODY"));
        const auto declared = first.root.parent_path() / "declared";
        RequireSkillSymlink(first.root, declared, true);
        const auto scan = tools::ScanSkillsDirStrict(declared, {"alpha"}, kStrictSkillSource);
        REQUIRE(scan.has_value());
        tools::SkillTool tool(scan->skills, std::set<std::string>{"skill"}, scan->read_policy);
        REQUIRE_FALSE(tool.execute({{"name", "alpha"}}).is_error);
        std::filesystem::remove(declared);
        RequireSkillSymlink(second.root, declared, true);
        CHECK(tool.execute({{"name", "alpha"}}).error_code == "skill.source.changed");
    }
    SUBCASE("canonical body mapping stays frozen even for equal bytes inside the root") {
        StrictSkillsFixture fixture;
        std::filesystem::create_directory(fixture.root / "alpha");
        WriteStrictSkillFile(fixture.root / "first.md", SkillContent("alpha", "valid", "SAME-BODY"));
        WriteStrictSkillFile(fixture.root / "second.md", SkillContent("alpha", "valid", "SAME-BODY"));
        const auto declared = fixture.root / "alpha" / "SKILL.md";
        RequireSkillSymlink(fixture.root / "first.md", declared);
        const auto scan = fixture.Scan({"alpha"});
        REQUIRE(scan.has_value());
        tools::SkillTool tool(scan->skills, std::set<std::string>{"skill"}, scan->read_policy);
        REQUIRE_FALSE(tool.execute({{"name", "alpha"}}).is_error);
        std::filesystem::remove(declared);
        RequireSkillSymlink(fixture.root / "second.md", declared);
        CHECK(tool.execute({{"name", "alpha"}}).error_code == "skill.source.changed");
    }
    SUBCASE("selected directory or body links cannot escape the explicit root") {
        StrictSkillsFixture first, second;
        second.Write("alpha", SkillContent("alpha", "valid", "OUTSIDE-BODY"));
        RequireSkillSymlink(second.root / "alpha", first.root / "alpha", true);
        CHECK_FALSE(first.Scan({"alpha"}).has_value());
        std::filesystem::remove(first.root / "alpha");
        std::filesystem::create_directory(first.root / "alpha");
        RequireSkillSymlink(second.root / "alpha" / "SKILL.md", first.root / "alpha" / "SKILL.md");
        CHECK_FALSE(first.Scan({"alpha"}).has_value());
    }
}

TEST_CASE("StrictSkills: dependencies and value owners remain per object while CLI path keeps compatibility") {
    StrictSkillsFixture first, second;
    first.Write("alpha", SkillContentWithRequires("alpha", "requires-tools: [run_command]\n", "FIRST"));
    second.Write("alpha", SkillContent("alpha", "valid", "SECOND"));
    const auto a = first.Scan({"alpha"}), b = second.Scan({"alpha"});
    REQUIRE(a.has_value());
    REQUIRE(b.has_value());
    tools::SkillTool denied(a->skills, std::set<std::string>{"skill"}, a->read_policy);
    tools::SkillTool allowed(a->skills, std::set<std::string>{"skill", "run_command"}, a->read_policy);
    tools::SkillTool other(b->skills, std::set<std::string>{"skill"}, b->read_policy);
    CHECK(denied.execute({{"name", "alpha"}}).error_code == "capability_unavailable");
    CHECK(allowed.execute({{"name", "alpha"}}).content.find("FIRST") != std::string::npos);
    CHECK(other.execute({{"name", "alpha"}}).content.find("SECOND") != std::string::npos);
    CHECK(other.execute({{"name", "alpha"}}).content.find("FIRST") == std::string::npos);
    tools::SkillTool mismatched(a->skills, std::set<std::string>{"skill", "run_command"}, b->read_policy);
    CHECK(mismatched.execute({{"name", "alpha"}}).error_code == "skill.source.invalid");
    first.Write("alpha", SkillContent("alpha", "valid", "CLI-CURRENT"));
    tools::SkillTool legacy(tools::ScanSkillsDir(first.root, "legacy"));
    CHECK_FALSE(legacy.execute({{"name", "alpha"}, {"path", nullptr}}).is_error);
    first.Write("alpha", SkillContent("alpha", "valid", "CLI-UPDATED"));
    const auto legacy_path = legacy.execute({{"name", "alpha"}, {"path", "SKILL.md"}});
    CHECK_FALSE(legacy_path.is_error);
    CHECK(legacy_path.content.find("CLI-UPDATED") != std::string::npos);
    CHECK(legacy.execute({{"name", "alpha"}}).error_code == "skill.drifted");
}
