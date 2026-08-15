// 系统提示的运行时拼装器(0.19.x 提示词模块化内置;0.21.x 运行时化):
// src/prompts/ 下的 .md 模块在构建期被 cmake/embed_prompts.cmake 嵌进
// embedded_prompts.hpp,这里按会话实际启用的能力条件拼装。
// 运行时化:PromptOptions.prompts_dir 非空(通常是 ~/.lubancode/prompts)时,
// 逐模块先看用户目录同名文件——存在且剥空白后非空就用文件,否则回退嵌入
// 常量。拼装发生在启动构造 AgentLoop 和每次 /clear 重建时,用户改模块文件,
// 开新会话即生效,不用重编不用重启;嵌入版降级为默认值、播种源、回退源。
//   core 模块(合起来 = 内置默认人格/法的还原源)恒在,法(system_prompt.md
//   或 --system-prompt)非空时整段让位;
//   运行环境段(工作目录、当天日期、操作系统 + 工具调用硬规矩)恒在,现填;
//   features:files/shell/delegation/todo 恒在;skills 有技能才注(后面紧跟
//   技能清单);web/mcp/lsp 配了对应能力才注;
//   platform 按 wire 注一个(anthropic / responses / chat_completions)。
// 模型专属指令、魂、延迟工具索引仍走 prompts.hpp 的 With* 包装层往后叠,
// 不归这里管。prompts.hpp 的 DefaultPersona()/BuildSystemPrompt() 是这里的
// 薄壳,旧签名照用。

#pragma once

#include <string>
#include <utility>
#include <vector>

namespace lubancode::agent {

struct PromptOptions {
    std::string cwd;             // 运行环境段现填
    std::string persona;         // 非空 = 法/CLI 人格,整段替换 core 模块
    std::string skills_segment;  // 技能清单段;非空才注 skills 模块 + 清单本身
    std::string project_instructions;  // AGENTS.md 分层内容;非空才注入
    bool mcp = false;            // 配置了 mcpServers
    bool web = false;            // 配置了 search 段(web_search 注册了)
    bool lsp = false;            // 配置了 lsp 段
    std::string wire;            // 三种正式 wire 名 / 空(不注平台段)
    std::string current_date;    // 空 = 拼装时取本机日期(YYYY-MM-DD);测试注入用
    std::string prompts_dir;     // 用户模块目录(~/.lubancode/prompts);空 = 只用嵌入版
};

// 按上述规则拼一份完整系统提示(不含模型指令/魂/延迟索引那几层)。
// prompts_dir 非空时逐模块读盘:用户文件存在且非空用文件,否则回退嵌入版。
std::string AssembleSystemPrompt(const PromptOptions& options);

// 嵌入的 core 模块按文件名序拼出的内置默认人格——prompts.hpp 的
// DefaultPersona()、法文件脚手架与 /prompt reset 的还原源,全打这儿来,
// 只此一处维护。恒用嵌入版,不看用户目录(它就是"默认值"本身)。
std::string AssembledDefaultPersona();

// core 模块按运行时规则(用户文件优先、嵌入回退)拼出的人格段。
// prompts_dir 空 = AssembledDefaultPersona()。/prompt 报字数、拼装内部共用。
std::string AssembledCorePersona(const std::string& prompts_dir);

// 模块播种清单:{相对路径(如 core/10-identity.md), 嵌入正文}。
// EnsurePromptScaffold 播种 ~/.lubancode/prompts/ 用——config 层不依赖
// agent 层,清单由调用方(main.cpp)从这儿取了递过去。
std::vector<std::pair<std::string, std::string>> PromptModuleSeeds();

// 各模块此刻的来源:prompts_dir 下同名文件存在且剥空白后非空 = 用户文件,
// 否则内置。全量播种后人人都是"用户文件",单看这一位分不出改没改——
// differs_from_embedded 再补一位:用户文件内容(归一后)是否已偏离嵌入版。
// /prompt 裸敲的来源统计用。
struct PromptModuleSource {
    std::string rel_path;
    bool from_user_file = false;
    bool differs_from_embedded = false;  // 仅 from_user_file 时有意义
};
std::vector<PromptModuleSource> PromptModuleSources(const std::string& prompts_dir);

// 运行环境段:工作目录 + 当天日期 + 操作系统 + "优先调用工具"这条硬规矩。
// current_date 空串 = 现取本机日期。
std::string BuildEnvironmentSegment(const std::string& cwd, const std::string& current_date = std::string());

// 按相对路径取一个模块正文:用户文件优先,嵌入版回退;找不到模块返回
// 空串。给系统提示之外的提示词消费方用(如 memory 回合总结的分型提示词
// features/memory-summary-*.md),同一套"用户可改、内置兜底"规矩。
std::string ModuleTextByPath(const std::string& prompts_dir, const std::string& rel_path);

}  // namespace lubancode::agent
