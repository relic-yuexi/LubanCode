// 系统提示的运行时拼装器(0.19.x 提示词模块化内置;0.21.x 运行时化;
// 自定义 Agent 与 Prompt Profile 单·阶段 2 加 Profile overlay):
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
//
// Prompt Profile(阶段 2,契约 docs/reference/agents.md §6):Profile 是一组
// 稀疏覆盖,沿用现有模块树,只改自己点名的文件。同一模块按五层次序找,
// 越往后权越大:
//   内置 default 模块 -> 用户全局 default 覆盖 -> 内置选中 Profile 覆盖
//   -> 用户选中 Profile 覆盖 -> 项目选中 Profile 覆盖
// profile 空或 "default" = 不选 Profile,行为与 0.21.x 完全一致(黄金基线
// 见 tests/unit/config/test_prompt_profile.cpp)。每段进了提示词,来源都记进
// PromptSourceLedger(哪层哪文件,/agent inspect 与测试用)。自定义 Agent
// 另可带 PromptCapabilities:从过滤后的有效工具表推导,有效工具没有的能力,
// feature 文案一个字不装(单子 §5.4"只给有效能力配说明")。

#pragma once

#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace lubancode::agent {

// 自定义 Agent 的有效能力(单子 §5.4):从过滤后的工具名表推导,推导规矩
// 见 DerivePromptCapabilities。主 Agent 不用它(旧逻辑照旧),这条首版只
// 用于自定义 Agent。
struct PromptCapabilities {
    bool files = false;       // 任一文件读写工具(read_file/write_file/edit_file/undo_file_edit)
    bool shell = false;       // run_command
    bool delegation = false;  // agent(委派子代理)
    bool todo = false;        // todo_write
    bool web = false;         // web_search / web_fetch
    bool mcp = false;         // 任一 mcp__<server>__<tool>
    bool lsp = false;         // lsp
};

// 从有效工具名表推导 PromptCapabilities。工具名须是过滤后的最终名单
//(Agent 的 tools.allow/deny 过滤后剩什么,这里就只认什么)。
PromptCapabilities DerivePromptCapabilities(const std::vector<std::string>& tool_names);

// Profile 名是否生效:空 = 没选;显式 "default" = 强制用内置默认(契约
// §4.2)。两者都不走 Profile 覆盖层。拼装与命令面(doctor/inspect)共用
// 这一口径,不许各写一遍。
bool IsPromptProfileActive(const std::string& profile);

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
    // Plan 模式(只读研究硬闸单):拼装末尾注入的模式段。空串 = Default
    // 模式,同样注 Default 模板(明令结束旧 Plan 指令,防切档残留)。段恒用
    // 嵌入版——用户/项目目录里的同名文件不覆盖(单子:mode instructions
    // 宿主内置,不给项目覆盖)。
    bool plan_mode = false;
    // ---- Prompt Profile(阶段 2) ----
    std::string profile;  // Profile 名;空或 "default" = 不选,走隐式 default
    // 项目模块目录(<项目根>/.lubancode/prompts)。只有"项目选中 Profile
    // 覆盖"这一层用它——default 上下文没有项目层,主 Agent 的拼装不受
    // 项目目录影响(契约 §6.2 的五层次序如此,黄金基线也钉住这一点)。
    std::string project_prompts_dir;
    // 自定义 Agent 的能力推导:有值时 feature 段(含恒在四件套)按能力
    // 开合,web/mcp/lsp 也以能力为准(不看上面的配置开关——开关是父会话
    // 的账,自定义 Agent 只认自己的有效工具表)。空 = 旧行为。
    std::optional<PromptCapabilities> capabilities;
};

// 一段提示词的来源层。前五层是契约 §6.2 的模块五层;后三段是拼装里的
// 宿主段(现填/继承/内置),也一并记账。
enum class PromptModuleOrigin {
    EmbeddedDefault,      // 内置 default 模块(嵌入常量)
    UserDefault,          // 用户全局 default 覆盖(~/.lubancode/prompts/<相对路径>)
    EmbeddedProfile,      // 内置选中 Profile 覆盖(src/prompts/profiles/<名>/<相对路径>)
    UserProfile,          // 用户选中 Profile 覆盖(~/.lubancode/prompts/profiles/<名>/<相对路径>)
    ProjectProfile,       // 项目选中 Profile 覆盖(<项目根>/.lubancode/prompts/profiles/<名>/<相对路径>)
    Persona,              // 法/CLI 人格/persona 整段替换 core(拼装外的活字)
    RuntimeEnvironment,   // 运行环境段(工作目录/日期/系统,现填)
    ProjectInstructions,  // AGENTS.md 分层内容(继承时注入)
    EmbeddedHostPolicy,   // modes/ 模式段(宿主内置,不可覆盖)
};
std::string ToString(PromptModuleOrigin origin);

// 来源账本的一条:哪个模块(或哪段)从哪层哪文件来。file 只在磁盘层填
//(UTF-8 全路径);嵌入/现填段为空。FormatLine 是契约 §6.5 样张口径。
struct PromptSourceLedgerEntry {
    std::string rel_path;  // 模块相对路径(如 core/10-identity.md)或段名
    PromptModuleOrigin origin = PromptModuleOrigin::EmbeddedDefault;
    std::string profile;   // 选中 Profile 时记名;default 上下文为空
    std::string file;      // 磁盘来源的 UTF-8 全路径;嵌入/现填段为空

    // 契约 §6.5 口径的一行:"<相对路径> <- <来源标签>"。
    std::string FormatLine() const;
};

// 来源账本(单子 §5.5):解析结果不只是最终字符串,每段都带来源。出了
// Prompt 覆盖问题,一眼看见是谁压了谁。
struct PromptSourceLedger {
    std::vector<PromptSourceLedgerEntry> entries;

    const PromptSourceLedgerEntry* Find(const std::string& rel_path) const;
};

// 按上述规则拼一份完整系统提示(不含模型指令/魂/延迟索引那几层)。
// prompts_dir 非空时逐模块读盘:用户文件存在且非空用文件,否则回退嵌入版。
// ledger 非空时逐段记账(拼进去的每段:模块来自哪层哪文件,宿主段记段名)。
std::string AssembleSystemPrompt(const PromptOptions& options, PromptSourceLedger* ledger = nullptr);

// 整表记账:default 树里每个可覆盖模块(core/features/platforms)在给定
// Profile 上下文下会解析到哪层,末尾补 modes/ 的宿主内置行。不论拼装时
// feature 开关实际注没注——开关是能力的事,账本管来源。/agent inspect 用。
PromptSourceLedger BuildPromptProfileLedger(const std::string& profile, const std::string& prompts_dir,
                                            const std::string& project_prompts_dir);

// 嵌入的 core 模块按文件名序拼出的内置默认人格——prompts.hpp 的
// DefaultPersona()、法文件脚手架与 /prompt reset 的还原源,全打这儿来,
// 只此一处维护。恒用嵌入版,不看用户目录(它就是"默认值"本身)。
std::string AssembledDefaultPersona();

// core 模块按运行时规则(用户文件优先、嵌入回退)拼出的人格段。
// prompts_dir 空 = AssembledDefaultPersona()。/prompt 报字数、拼装内部共用。
// (Prompt Profile 的 core 解析走 AssembleSystemPrompt 的五层回路;这条
// 老路只管 default 上下文,/prompt 与法脚手架用。)
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

// 模式段(Plan 模式单):modes/default.md 或 modes/plan.md 的**嵌入版**
// 正文。恒不读用户目录——mode instructions 是宿主内置合同,项目提示不许
// 覆盖(单子:用户覆盖 prompts 不能删 Plan 硬指令)。plan_mode=false 给
// Default 模板(内含"旧 Plan 指令已结束"的明令,防切档残留)。
std::string ModeInstructionSegment(bool plan_mode);

}  // namespace lubancode::agent
