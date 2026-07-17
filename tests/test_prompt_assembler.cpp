// prompt_assembler(0.19.x 提示词模块化内置):src/prompts/ 的 .md 模块
// 构建期嵌进 embedded_prompts.hpp,运行时按能力条件拼装。这里测全:
// 嵌入常量非空且含关键短语、core 拼默认人格(跟 DefaultPersona 同源)、
// 恒在段、features 开关矩阵(skills/web/mcp/lsp)、法替换 core、
// 上下文行现填、platform 段按 wire。

#include <doctest/doctest.h>

#include <cstring>
#include <string>

#include "agent/prompt_assembler.hpp"
#include "agent/prompts.hpp"
#include "embedded_prompts.hpp"

using namespace lubancode::agent;

namespace {

// 固定 cwd/日期,断言可复现;开关全默认关。
PromptOptions BaseOptions() {
    PromptOptions options;
    options.cwd = "D:/work";
    options.current_date = "2026-07-18";
    return options;
}

bool Contains(const std::string& haystack, const char* needle) {
    return haystack.find(needle) != std::string::npos;
}

}  // namespace

TEST_CASE("嵌入生成:模块常量非空,各含自己的关键短语") {
    // core:至少一个模块,每个都非空,身份模块提到 lubancode。
    REQUIRE(std::size(embedded::kCoreModules) >= 1);
    for (const char* module : embedded::kCoreModules) {
        CHECK(std::strlen(module) > 0);
    }
    CHECK(Contains(embedded::kCoreModules[0], "lubancode"));

    // features:方针段各自点到自己的工具名。
    CHECK(Contains(embedded::kFeature_files, "read_file"));
    CHECK(Contains(embedded::kFeature_files, "edit_file"));
    CHECK(Contains(embedded::kFeature_shell, "run_command"));
    CHECK(Contains(embedded::kFeature_delegation, "agent"));
    CHECK(Contains(embedded::kFeature_todo, "todo_write"));
    CHECK(Contains(embedded::kFeature_skills, "skill"));
    CHECK(Contains(embedded::kFeature_web, "web_search"));
    CHECK(Contains(embedded::kFeature_web, "web_fetch"));
    CHECK(Contains(embedded::kFeature_mcp, "mcp__"));
    CHECK(Contains(embedded::kFeature_lsp, "lsp"));

    // platforms:两个协议段各认各的名号。
    CHECK(Contains(embedded::kPlatform_anthropic, "Anthropic"));
    CHECK(Contains(embedded::kPlatform_responses, "Responses"));
}

TEST_CASE("AssembledDefaultPersona: core 模块按文件名序拼出,DefaultPersona 同源") {
    const std::string persona = AssembledDefaultPersona();
    CHECK(persona.find(embedded::kCoreModules[0]) == 0);  // 第一个模块打头
    for (const char* module : embedded::kCoreModules) {
        CHECK(Contains(persona, module));
    }
    // prompts.hpp 的薄壳跟这里同一个源,法脚手架/reset 还原不再两处维护。
    CHECK(persona == DefaultPersona());
}

TEST_CASE("恒在段:core + 环境 + files/shell/delegation/todo,开关全关也在") {
    const std::string prompt = AssembleSystemPrompt(BaseOptions());
    CHECK(prompt.find(AssembledDefaultPersona()) == 0);
    CHECK(Contains(prompt, embedded::kFeature_files));
    CHECK(Contains(prompt, embedded::kFeature_shell));
    CHECK(Contains(prompt, embedded::kFeature_delegation));
    CHECK(Contains(prompt, embedded::kFeature_todo));
    // 没启用的能力一个字不占。
    CHECK_FALSE(Contains(prompt, embedded::kFeature_skills));
    CHECK_FALSE(Contains(prompt, embedded::kFeature_web));
    CHECK_FALSE(Contains(prompt, embedded::kFeature_mcp));
    CHECK_FALSE(Contains(prompt, embedded::kFeature_lsp));
    CHECK_FALSE(Contains(prompt, embedded::kPlatform_anthropic));
    CHECK_FALSE(Contains(prompt, embedded::kPlatform_responses));
}

TEST_CASE("上下文行:cwd、注入的日期、操作系统都现填在运行环境段里") {
    const std::string prompt = AssembleSystemPrompt(BaseOptions());
    CHECK(Contains(prompt, "# 运行环境"));
    CHECK(Contains(prompt, "- 工作目录: D:/work"));
    CHECK(Contains(prompt, "- 今天日期: 2026-07-18"));
    CHECK(Contains(prompt, "- 操作系统: "));
    // 环境段的硬规矩(该用工具就用)也在。
    CHECK(Contains(prompt, "优先调用工具"));
}

TEST_CASE("上下文行:不注入日期时现取本机日期,YYYY-MM-DD 格式") {
    const std::string seg = BuildEnvironmentSegment("/x");
    const std::size_t pos = seg.find("- 今天日期: ");
    REQUIRE(pos != std::string::npos);
    const std::string date = seg.substr(pos + std::string("- 今天日期: ").size(), 10);
    REQUIRE(date.size() == 10);
    CHECK(date[4] == '-');
    CHECK(date[7] == '-');
    for (const std::size_t i : {0u, 1u, 2u, 3u, 5u, 6u, 8u, 9u}) {
        CHECK(date[i] >= '0');
        CHECK(date[i] <= '9');
    }
}

TEST_CASE("features 开关矩阵:skills/web/mcp/lsp 各自独立开合") {
    SUBCASE("skills:段非空才注模块,清单紧随模块之后") {
        PromptOptions options = BaseOptions();
        options.skills_segment = "可用技能(用 skill 工具按名加载):\n- demo: 演示技能";
        const std::string prompt = AssembleSystemPrompt(options);
        const std::size_t module_pos = prompt.find(embedded::kFeature_skills);
        const std::size_t list_pos = prompt.find("- demo: 演示技能");
        REQUIRE(module_pos != std::string::npos);
        REQUIRE(list_pos != std::string::npos);
        CHECK(list_pos > module_pos);
    }
    SUBCASE("web 开") {
        PromptOptions options = BaseOptions();
        options.web = true;
        const std::string prompt = AssembleSystemPrompt(options);
        CHECK(Contains(prompt, embedded::kFeature_web));
        CHECK_FALSE(Contains(prompt, embedded::kFeature_mcp));
        CHECK_FALSE(Contains(prompt, embedded::kFeature_lsp));
    }
    SUBCASE("mcp 开") {
        PromptOptions options = BaseOptions();
        options.mcp = true;
        const std::string prompt = AssembleSystemPrompt(options);
        CHECK(Contains(prompt, embedded::kFeature_mcp));
        CHECK_FALSE(Contains(prompt, embedded::kFeature_web));
        CHECK_FALSE(Contains(prompt, embedded::kFeature_lsp));
    }
    SUBCASE("lsp 开") {
        PromptOptions options = BaseOptions();
        options.lsp = true;
        const std::string prompt = AssembleSystemPrompt(options);
        CHECK(Contains(prompt, embedded::kFeature_lsp));
        CHECK_FALSE(Contains(prompt, embedded::kFeature_web));
        CHECK_FALSE(Contains(prompt, embedded::kFeature_mcp));
    }
    SUBCASE("全开:四段都在,次序 skills < web < mcp < lsp") {
        PromptOptions options = BaseOptions();
        options.skills_segment = "可用技能(用 skill 工具按名加载):\n- demo: 演示技能";
        options.web = true;
        options.mcp = true;
        options.lsp = true;
        const std::string prompt = AssembleSystemPrompt(options);
        const std::size_t skills_pos = prompt.find(embedded::kFeature_skills);
        const std::size_t web_pos = prompt.find(embedded::kFeature_web);
        const std::size_t mcp_pos = prompt.find(embedded::kFeature_mcp);
        const std::size_t lsp_pos = prompt.find(embedded::kFeature_lsp);
        REQUIRE(skills_pos != std::string::npos);
        REQUIRE(web_pos != std::string::npos);
        REQUIRE(mcp_pos != std::string::npos);
        REQUIRE(lsp_pos != std::string::npos);
        CHECK(skills_pos < web_pos);
        CHECK(web_pos < mcp_pos);
        CHECK(mcp_pos < lsp_pos);
    }
}

TEST_CASE("法替换 core:人格非空时 core 让位,环境/features 段照拼") {
    PromptOptions options = BaseOptions();
    options.persona = "你只用文言文答话。";
    options.web = true;
    const std::string prompt = AssembleSystemPrompt(options);
    CHECK(prompt.find("你只用文言文答话。") == 0);  // 法打头
    CHECK_FALSE(Contains(prompt, AssembledDefaultPersona().c_str()));
    // 环境段、恒在 features、条件 features 一样不少。
    CHECK(Contains(prompt, "- 工作目录: D:/work"));
    CHECK(Contains(prompt, embedded::kFeature_files));
    CHECK(Contains(prompt, embedded::kFeature_web));
}

TEST_CASE("platform 段按 wire:anthropic/responses 各注各的,认不出不注") {
    PromptOptions options = BaseOptions();

    options.wire = "anthropic";
    std::string prompt = AssembleSystemPrompt(options);
    CHECK(Contains(prompt, embedded::kPlatform_anthropic));
    CHECK_FALSE(Contains(prompt, embedded::kPlatform_responses));

    options.wire = "responses";
    prompt = AssembleSystemPrompt(options);
    CHECK(Contains(prompt, embedded::kPlatform_responses));
    CHECK_FALSE(Contains(prompt, embedded::kPlatform_anthropic));

    options.wire = "别的什么";
    prompt = AssembleSystemPrompt(options);
    CHECK_FALSE(Contains(prompt, embedded::kPlatform_anthropic));
    CHECK_FALSE(Contains(prompt, embedded::kPlatform_responses));
}
