// 系统提示的运行时拼装器(0.19.x 提示词模块化内置):src/prompts/ 下的
// .md 模块在构建期被 cmake/embed_prompts.cmake 嵌进 embedded_prompts.hpp,
// 这里按会话实际启用的能力条件拼装:
//   core 模块(合起来 = 内置默认人格/法的还原源)恒在,法(system_prompt.md
//   或 --system-prompt)非空时整段让位;
//   运行环境段(工作目录、当天日期、操作系统 + 工具调用硬规矩)恒在,现填;
//   features:files/shell/delegation/todo 恒在;skills 有技能才注(后面紧跟
//   技能清单);web/mcp/lsp 配了对应能力才注;
//   platform 按 wire 注一个(anthropic / responses)。
// 模型专属指令、魂、延迟工具索引仍走 prompts.hpp 的 With* 包装层往后叠,
// 不归这里管。prompts.hpp 的 DefaultPersona()/BuildSystemPrompt() 是这里的
// 薄壳,旧签名照用。

#pragma once

#include <string>

namespace lubancode::agent {

struct PromptOptions {
    std::string cwd;             // 运行环境段现填
    std::string persona;         // 非空 = 法/CLI 人格,整段替换 core 模块
    std::string skills_segment;  // 技能清单段;非空才注 skills 模块 + 清单本身
    bool mcp = false;            // 配置了 mcpServers
    bool web = false;            // 配置了 search 段(web_search 注册了)
    bool lsp = false;            // 配置了 lsp 段
    std::string wire;            // "anthropic" / "responses" / 空(不注平台段)
    std::string current_date;    // 空 = 拼装时取本机日期(YYYY-MM-DD);测试注入用
};

// 按上述规则拼一份完整系统提示(不含模型指令/魂/延迟索引那几层)。
std::string AssembleSystemPrompt(const PromptOptions& options);

// 嵌入的 core 模块按文件名序拼出的内置默认人格——prompts.hpp 的
// DefaultPersona()、法文件脚手架与 /prompt reset 的还原源,全打这儿来,
// 只此一处维护。
std::string AssembledDefaultPersona();

// 运行环境段:工作目录 + 当天日期 + 操作系统 + "优先调用工具"这条硬规矩。
// current_date 空串 = 现取本机日期。
std::string BuildEnvironmentSegment(const std::string& cwd, const std::string& current_date = std::string());

}  // namespace lubancode::agent
