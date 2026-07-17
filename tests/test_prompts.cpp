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
