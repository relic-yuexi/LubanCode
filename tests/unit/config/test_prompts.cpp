// agent::BuildSystemPrompt:M6 把系统提示词拆成人格段(可被 --system-prompt
// 替换)+ 环境段(工作目录、工具调用指令,运行必需,不受人格段影响)。
// 这里验证:默认没传 custom_persona 时用内置人格;传了就整段替换掉人格,
// 环境段(cwd、"优先调用工具"这句)始终原样追加。

#include <doctest/doctest.h>

#include "agent/prompts.hpp"

using namespace lubancode::agent;

TEST_CASE("BuildSystemPrompt: 不传自定义人格,用内置默认人格 + 环境段") {
    const std::string prompt = BuildSystemPrompt("/some/dir");
    CHECK(prompt.find(DefaultPersona()) != std::string::npos);
    CHECK(prompt.find("/some/dir") != std::string::npos);
    CHECK(prompt.find("工具") != std::string::npos);
}

TEST_CASE("BuildSystemPrompt: 传自定义人格,整段替换掉默认人格,环境段照样追加") {
    const std::string custom = "你只能用文言文回答问题。";
    const std::string prompt = BuildSystemPrompt("/work/dir", custom);

    CHECK(prompt.find(custom) != std::string::npos);
    // 默认人格的原句不该再出现了(整段替换,不是追加)。
    CHECK(prompt.find(DefaultPersona()) == std::string::npos);
    // 环境段(工作目录 + 工具调用指令)不受人格替换影响,始终都在。
    CHECK(prompt.find("/work/dir") != std::string::npos);
    CHECK(prompt.find("工具") != std::string::npos);
}

TEST_CASE("BuildSystemPrompt: 空字符串人格视同没传,退回默认人格") {
    const std::string prompt = BuildSystemPrompt("/dir", "");
    CHECK(prompt.find(DefaultPersona()) != std::string::npos);
}

TEST_CASE("EnvironmentSegment: 包含传入的工作目录") {
    const std::string seg = EnvironmentSegment("/foo/bar");
    CHECK(seg.find("/foo/bar") != std::string::npos);
}

// ---------------------------------------------------------------------------
// 魂法分家(0.16.x):StripPromptComments / ResolvePersona / WithSoul。
// 注入顺序、回退逻辑全是纯函数,这里测全:法缺失回退内置、空白回退、
// CLI 压过文件、魂在最后、注释剥离。
// ---------------------------------------------------------------------------

TEST_CASE("StripPromptComments: 剥掉 HTML 注释,留下正文") {
    const std::string text = "<!-- 顶部说明 -->\n只用文言文答话。\n";
    CHECK(StripPromptComments(text) == "只用文言文答话。");
}

TEST_CASE("StripPromptComments: 跨行注释、多段注释都剥") {
    const std::string text = "<!-- 第一行\n第二行 -->正文A<!-- 又一段 -->正文B";
    CHECK(StripPromptComments(text) == "正文A正文B");
}

TEST_CASE("StripPromptComments: 没闭合的注释,后面整段丢掉") {
    CHECK(StripPromptComments("正文<!-- 没闭合的注释") == "正文");
}

TEST_CASE("StripPromptComments: 只有注释和空白,剥完是空串") {
    CHECK(StripPromptComments("<!-- 只有注释 -->\n  \n").empty());
    CHECK(StripPromptComments("").empty());
    CHECK(StripPromptComments("   \r\n").empty());
}

TEST_CASE("ResolvePersona: CLI 人格压过法文件内容") {
    CHECK(ResolvePersona("CLI 给的人格", "法文件内容") == "CLI 给的人格");
}

TEST_CASE("ResolvePersona: 没有 CLI 人格时用法文件内容(注释剥掉)") {
    CHECK(ResolvePersona("", "<!-- 注释 -->\n法文件正文") == "法文件正文");
}

TEST_CASE("ResolvePersona: 法文件缺失/全空白,返回空串,BuildSystemPrompt 回退内置默认") {
    const std::string persona = ResolvePersona("", "<!-- 只有注释 -->\n");
    CHECK(persona.empty());
    const std::string prompt = BuildSystemPrompt("/dir", persona);
    CHECK(prompt.find(DefaultPersona()) != std::string::npos);
}

TEST_CASE("WithSoul: 魂全空白(缺失/只有注释),系统提示原样返回,一个字符都不多") {
    const std::string base = BuildSystemPrompt("/dir");
    CHECK(WithSoul(base, "") == base);
    CHECK(WithSoul(base, "<!-- 留空 = 无效果 -->\n") == base);
    CHECK(WithSoul(base, "  \n  ") == base);
}

TEST_CASE("WithSoul: 魂叠加在最后,注释剥掉不注入") {
    const std::string base = BuildSystemPrompt("/dir");
    const std::string out = WithSoul(base, "<!-- 文言示例 -->\n只用文言文答话。");
    CHECK(out.find(base) == 0);  // 原提示原样打头
    CHECK(out.find("只用文言文答话。") != std::string::npos);
    CHECK(out.find("<!--") == std::string::npos);  // 注释不注入
}

TEST_CASE("WithSoul: 排在模型专属指令(base_instructions)之后,永远压轴") {
    const std::string base = BuildSystemPrompt("/dir");
    const std::string with_instr = WithModelInstructions(base, "模型专属指令内容");
    const std::string out = WithSoul(with_instr, "魂的风格指令");
    const std::size_t instr_pos = out.find("模型专属指令内容");
    const std::size_t soul_pos = out.find("魂的风格指令");
    REQUIRE(instr_pos != std::string::npos);
    REQUIRE(soul_pos != std::string::npos);
    CHECK(soul_pos > instr_pos);
    // 魂之后没有别的段落了(它是最后一段)。
    CHECK(out.rfind("魂的风格指令") + std::string("魂的风格指令").size() == out.size());
}
