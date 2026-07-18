// 魂法分家(0.16.x)文件侧:EnsurePromptScaffold 首启生成四样、已存在不
// 覆盖;ResetSystemPromptFile 备份 .bak 与重写;ListSouls 扫描列表。全部
// 在临时目录里做,不碰真实用户主目录。

#include <doctest/doctest.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

#include "agent/prompts.hpp"
#include "config/prompt_files.hpp"
#include "tools/skill_loader.hpp"

using namespace lubancode::config;

namespace {

class TempDir {
public:
    TempDir() {
        path_ = std::filesystem::temp_directory_path() /
                ("lubancode_prompt_files_test_" +
                 std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        std::filesystem::create_directories(path_);
    }
    ~TempDir() {
        std::error_code ec;
        std::filesystem::remove_all(path_, ec);
    }
    std::string Str() const { return path_.string(); }
    std::filesystem::path Path() const { return path_; }

private:
    std::filesystem::path path_;
};

std::string ReadAll(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    std::ostringstream buffer;
    buffer << file.rdbuf();
    return buffer.str();
}

void WriteAll(const std::filesystem::path& path, const std::string& content) {
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    file << content;
}

// fs::path -> UTF-8 字符串。path::string() 走系统 ANSI 代码页,中文文件名
// 直接抛异常,得用 u8string 这条路(跟被测代码同一套约定)。
std::string Utf8(const std::filesystem::path& path) {
    const std::u8string u8 = path.u8string();
    return std::string(reinterpret_cast<const char*>(u8.data()), u8.size());
}

const std::string kPersona = "测试人格段:你是一只测试用的木鸢。";

}  // namespace

TEST_CASE("EnsurePromptScaffold: 首启四样落地——法、魂、文言魂、配置技能") {
    TempDir dir;
    const std::string base = (dir.Path() / ".lubancode").string();

    const auto created = EnsurePromptScaffold(base, kPersona);
    CHECK(created.size() == 4);

    // system_prompt.md:顶部注释 + 内置默认人格段原样副本。
    const std::string law = ReadAll(dir.Path() / ".lubancode" / "system_prompt.md");
    CHECK(law.find("<!--") == 0);
    CHECK(law.find("/prompt reset") != std::string::npos);
    CHECK(law.find(kPersona) != std::string::npos);

    // SOUL.md:只有注释说明,实际内容空白。
    const std::string soul = ReadAll(dir.Path() / ".lubancode" / "SOUL.md");
    CHECK(soul.find("<!--") == 0);
    CHECK(soul.find("/soul") != std::string::npos);
    // 注释之外没有别的内容(剥掉注释后应为空白——这里粗验:注释闭合后只剩换行)。
    const std::size_t close = soul.find("-->");
    REQUIRE(close != std::string::npos);
    for (std::size_t i = close + 3; i < soul.size(); ++i) {
        CHECK((soul[i] == '\n' || soul[i] == '\r' || soul[i] == ' '));
    }

    // souls/wenyan.md:文言文示例。
    const std::string wenyan = ReadAll(dir.Path() / ".lubancode" / "souls" / "wenyan.md");
    CHECK(wenyan.find("文言") != std::string::npos);

    // skills/lubancode-config/SKILL.md:首启生成,frontmatter 能被正式解析器认出。
    const std::string skill = ReadAll(dir.Path() / ".lubancode" / "skills" / "lubancode-config" / "SKILL.md");
    REQUIRE_FALSE(skill.empty());
    const auto parsed = lubancode::tools::ParseSkillMarkdown(skill);
    REQUIRE(parsed.has_value());
    REQUIRE(parsed->name.has_value());
    CHECK(*parsed->name == "lubancode-config");
    REQUIRE(parsed->description.has_value());
    // 0.21.x:description 扩了关键词(soul/主题/模型/MCP/快捷键……),旧断言
    // "lubancode 自身配置的完整说明"整句替换。
    CHECK(*parsed->description ==
          "lubancode 自身配置与功能说明(soul/魂/主题/模型/MCP/钩子/技能/语言/快捷键/会话),"
          "用户问及或要改 lubancode 自身时先加载本技能");
    // 系统维护标记在正文里(frontmatter 之后),启动刷新机制认它。
    CHECK(skill.find(kManagedSkillMarker) != std::string::npos);
    CHECK(skill == DefaultLubancodeConfigSkillContent());
}

TEST_CASE("EnsurePromptScaffold: 已存在不覆盖,缺哪样只补哪样") {
    TempDir dir;
    const std::string base = (dir.Path() / ".lubancode").string();
    REQUIRE(EnsurePromptScaffold(base, kPersona).size() == 4);

    // 用户改了法、魂和配置技能——再跑一遍,一个字都不能动。
    WriteAll(dir.Path() / ".lubancode" / "system_prompt.md", "用户自定义的法");
    WriteAll(dir.Path() / ".lubancode" / "SOUL.md", "用户自定义的魂");
    WriteAll(dir.Path() / ".lubancode" / "skills" / "lubancode-config" / "SKILL.md", "用户自定义的配置技能");
    CHECK(EnsurePromptScaffold(base, kPersona).empty());
    CHECK(ReadAll(dir.Path() / ".lubancode" / "system_prompt.md") == "用户自定义的法");
    CHECK(ReadAll(dir.Path() / ".lubancode" / "SOUL.md") == "用户自定义的魂");
    CHECK(ReadAll(dir.Path() / ".lubancode" / "skills" / "lubancode-config" / "SKILL.md") == "用户自定义的配置技能");

    // 删掉 wenyan.md——只补这一样。
    std::filesystem::remove(dir.Path() / ".lubancode" / "souls" / "wenyan.md");
    const auto created = EnsurePromptScaffold(base, kPersona);
    REQUIRE(created.size() == 1);
    CHECK(created[0].find("wenyan.md") != std::string::npos);
    CHECK(ReadAll(dir.Path() / ".lubancode" / "system_prompt.md") == "用户自定义的法");
    CHECK(ReadAll(dir.Path() / ".lubancode" / "skills" / "lubancode-config" / "SKILL.md") == "用户自定义的配置技能");
}

TEST_CASE("EnsurePromptScaffold: 排他创建——抢先落地的用户文件一字不动,也不计入 created") {
    // 修复钉子:老写法先 exists 探一眼、再 trunc 开写,两步之间有窗口,
    // 并发进程恰在此刻落地的用户文件会被覆盖。现在检查与创建并成一步
    // (Windows CREATE_NEW / POSIX fopen "wbx"),窗口本身已不存在。真正的
    // 并发撞车没法在单测里稳定摆出来,这里钉的是"存在即失败"这半边契约:
    // 哪怕是空文件,也算存在,绝不覆盖、绝不记账。
    TempDir dir;
    const std::string base = (dir.Path() / ".lubancode").string();
    std::filesystem::create_directories(dir.Path() / ".lubancode");
    WriteAll(dir.Path() / ".lubancode" / "system_prompt.md", "用户抢先写的法");
    WriteAll(dir.Path() / ".lubancode" / "SOUL.md", "");  // 空文件也算存在

    const auto created = EnsurePromptScaffold(base, kPersona);
    CHECK(created.size() == 2);  // 只补 wenyan.md 和 SKILL.md
    for (const auto& path : created) {
        CHECK(path.find("system_prompt.md") == std::string::npos);
        CHECK(path.find("SOUL.md") == std::string::npos);
    }
    CHECK(ReadAll(dir.Path() / ".lubancode" / "system_prompt.md") == "用户抢先写的法");
    CHECK(ReadAll(dir.Path() / ".lubancode" / "SOUL.md").empty());
}

TEST_CASE("ResetSystemPromptFile: 旧文件挪成 .bak,重写内置默认") {
    TempDir dir;
    const std::string base = (dir.Path() / ".lubancode").string();
    REQUIRE(EnsurePromptScaffold(base, kPersona).size() == 4);
    WriteAll(dir.Path() / ".lubancode" / "system_prompt.md", "每句话末尾加【鲁班】");

    const auto result = ResetSystemPromptFile(base, kPersona);
    REQUIRE(result.has_value());
    CHECK(result->find("system_prompt.md.bak") != std::string::npos);

    CHECK(ReadAll(dir.Path() / ".lubancode" / "system_prompt.md.bak") == "每句话末尾加【鲁班】");
    const std::string law = ReadAll(dir.Path() / ".lubancode" / "system_prompt.md");
    CHECK(law.find(kPersona) != std::string::npos);
    CHECK(law == DefaultSystemPromptFileContent(kPersona));
}

TEST_CASE("ResetSystemPromptFile: 已有 .bak 就覆盖") {
    TempDir dir;
    const std::string base = (dir.Path() / ".lubancode").string();
    REQUIRE(EnsurePromptScaffold(base, kPersona).size() == 4);
    WriteAll(dir.Path() / ".lubancode" / "system_prompt.md.bak", "旧备份");
    WriteAll(dir.Path() / ".lubancode" / "system_prompt.md", "新的怪规矩");

    const auto result = ResetSystemPromptFile(base, kPersona);
    REQUIRE(result.has_value());
    CHECK(ReadAll(dir.Path() / ".lubancode" / "system_prompt.md.bak") == "新的怪规矩");
}

TEST_CASE("ResetSystemPromptFile: 原文件不存在,直接写默认,没有 .bak") {
    TempDir dir;
    const std::string base = (dir.Path() / ".lubancode").string();

    const auto result = ResetSystemPromptFile(base, kPersona);
    REQUIRE(result.has_value());
    CHECK(result->empty());  // 没有旧文件可备份
    CHECK(ReadAll(dir.Path() / ".lubancode" / "system_prompt.md") == DefaultSystemPromptFileContent(kPersona));
    CHECK(!std::filesystem::exists(dir.Path() / ".lubancode" / "system_prompt.md.bak"));
}

TEST_CASE("ListSouls: 扫 souls/*.md,名字去扩展名,字典序;非 .md 不算") {
    TempDir dir;
    const std::string base = (dir.Path() / ".lubancode").string();
    REQUIRE(EnsurePromptScaffold(base, kPersona).size() == 4);
    WriteAll(dir.Path() / ".lubancode" / "souls" / "pirate.md", "海盗腔");
    WriteAll(dir.Path() / ".lubancode" / "souls" / "notes.txt", "不是魂");

    const auto souls = ListSouls(base);
    REQUIRE(souls.size() == 2);
    CHECK(souls[0] == "pirate");
    CHECK(souls[1] == "wenyan");
}

TEST_CASE("ListSouls: souls 目录不存在,空列表不算错") {
    TempDir dir;
    CHECK(ListSouls((dir.Path() / "没有这个目录").string()).empty());
}

TEST_CASE("ReadTextFileIfExists: 不存在返回 nullopt,存在原样读(中文文件名也认)") {
    TempDir dir;
    // 中文文件名要走 u8 字面量进 fs::path——窄字面量会被 MSVC 按系统 ANSI
    // 代码页解释,UTF-8 字节喂进去就乱了(被测代码自己的 UTF-8 约定不受
    // 影响,这是测试造路径这一步的事)。
    CHECK(!ReadTextFileIfExists(Utf8(dir.Path() / std::filesystem::path(u8"不存在.md"))).has_value());
    WriteAll(dir.Path() / std::filesystem::path(u8"有.md"), "内容\n第二行");
    const auto content = ReadTextFileIfExists(Utf8(dir.Path() / std::filesystem::path(u8"有.md")));
    REQUIRE(content.has_value());
    CHECK(*content == "内容\n第二行");
}

TEST_CASE("SOUL.md: 写入后能读回,clear 还原默认内容") {
    TempDir dir;
    const std::string base = Utf8(dir.Path() / std::filesystem::path(u8".lubancode"));
    const std::string soul = "答话短些。\n先动手,后回话。\n";

    REQUIRE(WriteSoulFile(base, soul).has_value());
    const auto loaded = ReadSoulFile(base);
    REQUIRE(loaded.has_value());
    CHECK(*loaded == soul);
    // WithSoul 注入前会过 StripPromptComments,两端空白(含这里文件天然带的
    // 收尾换行)按设计一并剥掉——跟"法"文件(ResolvePersona)同一套规矩,
    // 不是读回丢了内容。故这里拿掉两端空白后的版本去比对,不直接找带
    // 收尾换行的原串。
    CHECK(lubancode::agent::WithSoul("base prompt", *loaded)
              .find(lubancode::agent::StripPromptComments(soul)) != std::string::npos);

    REQUIRE(ClearSoulFile(base).has_value());
    const auto cleared = ReadSoulFile(base);
    REQUIRE(cleared.has_value());
    CHECK(*cleared == DefaultSoulFileContent());
}

TEST_CASE("SOUL.md: 损坏或不可读时按无魂回退,不抛错") {
    TempDir dir;
    const std::filesystem::path base = dir.Path() / ".lubancode";
    std::filesystem::create_directories(base);

    // 非法 UTF-8 不传进系统提示;ReadSoulFile 返回空让启动方照常继续。
    WriteAll(base / "SOUL.md", std::string("答", 3) + std::string("\xFF", 1));
    CHECK_FALSE(ReadSoulFile(Utf8(base)).has_value());

    std::filesystem::remove(base / "SOUL.md");
    std::filesystem::create_directories(base / "SOUL.md");
    CHECK_FALSE(ReadSoulFile(Utf8(base)).has_value());
}

TEST_CASE("SoulPathByName / 各路径拼接") {
    const std::string base = "C:/home/.lubancode";
    CHECK(SystemPromptFilePath(base).find("system_prompt.md") != std::string::npos);
    CHECK(SoulFilePath(base).find("SOUL.md") != std::string::npos);
    const std::string wenyan = SoulPathByName(base, "wenyan");
    CHECK(wenyan.find("souls") != std::string::npos);
    CHECK(wenyan.find("wenyan.md") != std::string::npos);
}

// ---------------------------------------------------------------------------
// 0.21.x 提示词运行时化:prompts/ 模块播种 + lubancode-config/SKILL.md
// 系统维护件刷新。
// ---------------------------------------------------------------------------

TEST_CASE("EnsurePromptScaffold: prompts/ 模块播种——缺哪补哪,已存在不覆盖") {
    TempDir dir;
    const std::string base = (dir.Path() / ".lubancode").string();
    const std::vector<std::pair<std::string, std::string>> modules = {
        {"core/10-identity.md", "身份模块正文"},
        {"features/files.md", "文件模块正文"},
        {"platforms/anthropic.md", "平台模块正文"},
    };

    // 首启:老四样 + 三个模块。
    const auto created = EnsurePromptScaffold(base, kPersona, modules);
    CHECK(created.size() == 7);
    CHECK(ReadAll(dir.Path() / ".lubancode" / "prompts" / "core" / "10-identity.md") == "身份模块正文\n");
    CHECK(ReadAll(dir.Path() / ".lubancode" / "prompts" / "features" / "files.md") == "文件模块正文\n");
    CHECK(ReadAll(dir.Path() / ".lubancode" / "prompts" / "platforms" / "anthropic.md") == "平台模块正文\n");

    // 用户改了一个模块、删了一个模块——再跑:改过的不动,删掉的补回。
    WriteAll(dir.Path() / ".lubancode" / "prompts" / "core" / "10-identity.md", "用户改过的身份");
    std::filesystem::remove(dir.Path() / ".lubancode" / "prompts" / "features" / "files.md");
    const auto again = EnsurePromptScaffold(base, kPersona, modules);
    REQUIRE(again.size() == 1);
    CHECK(again[0].find("files.md") != std::string::npos);
    CHECK(ReadAll(dir.Path() / ".lubancode" / "prompts" / "core" / "10-identity.md") == "用户改过的身份");
}

TEST_CASE("EnsurePromptScaffold: 带管理标记的 SKILL.md 内容过期 → 覆盖刷新(不算新建)") {
    TempDir dir;
    const std::string base = (dir.Path() / ".lubancode").string();
    REQUIRE(EnsurePromptScaffold(base, kPersona).size() == 4);

    // 模拟旧版本落下的维护件:带标记,但内容跟当前内置版不一样。
    const std::filesystem::path skill_path =
        dir.Path() / ".lubancode" / "skills" / "lubancode-config" / "SKILL.md";
    WriteAll(skill_path, std::string("---\nname: lubancode-config\ndescription: 旧版说明\n---\n") +
                              kManagedSkillMarker + "\n\n旧版正文\n");

    const auto created = EnsurePromptScaffold(base, kPersona);
    CHECK(created.empty());  // 刷新不算新建
    CHECK(ReadAll(skill_path) == DefaultLubancodeConfigSkillContent());
}

TEST_CASE("EnsurePromptScaffold: 无标记的 SKILL.md(用户自建/删了标记)一个字不动") {
    TempDir dir;
    const std::string base = (dir.Path() / ".lubancode").string();
    REQUIRE(EnsurePromptScaffold(base, kPersona).size() == 4);

    const std::filesystem::path skill_path =
        dir.Path() / ".lubancode" / "skills" / "lubancode-config" / "SKILL.md";
    WriteAll(skill_path, "---\nname: lubancode-config\ndescription: 用户自己的手册\n---\n用户正文\n");

    CHECK(EnsurePromptScaffold(base, kPersona).empty());
    CHECK(ReadAll(skill_path) == "---\nname: lubancode-config\ndescription: 用户自己的手册\n---\n用户正文\n");
}

TEST_CASE("EnsurePromptScaffold: 内容已是当前内置版的 SKILL.md 不重写(时间戳不折腾)") {
    TempDir dir;
    const std::string base = (dir.Path() / ".lubancode").string();
    REQUIRE(EnsurePromptScaffold(base, kPersona).size() == 4);
    // 再跑一遍:内容一致,不新建也不刷新。
    CHECK(EnsurePromptScaffold(base, kPersona).empty());
    const std::filesystem::path skill_path =
        dir.Path() / ".lubancode" / "skills" / "lubancode-config" / "SKILL.md";
    CHECK(ReadAll(skill_path) == DefaultLubancodeConfigSkillContent());
}
