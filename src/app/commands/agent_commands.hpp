// /agents 与 /agent doctor(自定义 Agent 与 Prompt Profile 单·阶段 1):只读
// 骨架——只列只诊,不改不装。Catalog 在命令触发时现扫现建(首版不做热
// 重载,单子"加载范围与信任");阶段 1 没有任何执行路径吃这份 Catalog,
// general-purpose/Explore 行为一字不动。
//
// 输出走字面量中文、不走 i18n(同 /background 的先例):这是给开发者与
// 高级用户的诊断视图,阶段 1 先不铺多语言;/help 里的两行命令说明仍走
// i18n(slash.desc.agents / slash.desc.agent),补全与帮助不缺位。
#pragma once

#include <string>
#include <vector>

#include "agent/agent_catalog.hpp"
#include "app/commands/command_flow.hpp"  // CommandFlow(分派注册制)
#include "cli/slash_commands.hpp"          // ParsedSlashCommand(分派注册制)
#include "tools/registry.hpp"
#include "tools/skill_loader.hpp"  // SkillMeta

namespace lubancode::app {

struct SlashDispatchContext;

// /agent doctor 的静态预检材料(全部可空:没有就跳过那一节,不猜)。
// 三层来源:
//   - skills:会话扫到的技能清单(SkillMeta::name 比对 skills.preload)
//   - registry:主工具表(tools.allow/deny/requires.tools 的名字比对;
//     MCP 工具挂进表里的完整名是 mcp__<服务>__<工具>)
//   - mcp_server_names:已挂载的 MCP 服务名(mcp_servers 比对)
struct AgentDoctorMaterials {
    const std::vector<lubancode::tools::SkillMeta>* skills = nullptr;
    const lubancode::tools::ToolRegistry* registry = nullptr;
    const std::vector<std::string>* mcp_server_names = nullptr;
};

// Prompt Profile 的解析上下文(阶段 2):用户层与项目层的 prompts 根。
// 纯函数不摸环境——handler 现算好递进来(ComputeProjectPromptsRoot /
// HomeLubancodeDir),单测想造什么层就造什么层。都空 = 只有内置层。
struct AgentPromptContext {
    std::string user_prompts_dir;
    std::string project_prompts_dir;
};

// ---------------- 纯函数(单测钉住) ----------------

// /agents 的全部输出行:每名一志(名称/层/可用性/描述/模型/Profile/工具/
// Skill 数),不可用带第一条错,跨层覆盖带"盖住"账,末尾摆加载警告。
// 不摸 IO,行怎么打由调用方定。
std::vector<std::string> FormatAgentCatalogListing(const lubancode::agent::AgentCatalog& catalog);

// /agent doctor <name> 的静态预检报告(单子八"agent doctor"清单里阶段 1
// 能查的:定义解析、覆盖链、Skill/MCP/工具引用、模型角色写法、runtime 与
// permissions 登账;Profile 覆盖存在性阶段 2 已查,权限越界比对属阶段 3,
// 都如实写明,不装查过了)。查无此名给一行"没这个 Agent,先 /agents"。
std::vector<std::string> FormatAgentDoctorReport(const lubancode::agent::AgentCatalog& catalog,
                                                 const std::string& name, const AgentDoctorMaterials& materials,
                                                 const AgentPromptContext& prompts = {});

// /agent inspect <name> 的报告(阶段 2):定义来源与覆盖链、prompt 段三笔
// 开关、PromptSourceLedger 逐模块来源账(单子 §5.5)。依赖预检归 doctor,
// 这里不重复。
std::vector<std::string> FormatAgentInspectReport(const lubancode::agent::AgentCatalog& catalog,
                                                  const std::string& name, const AgentPromptContext& prompts);

// ---------------- 执行(IO) ----------------

// 会话里现算三层扫描根:builtin = <exe 目录>/agents(通常没有)、user =
// ~/.lubancode/agents、project = 项目根(FindProjectRoot,复用 instruction
// 发现规则)/.lubancode/agents。目录不存在照旧进 roots——Catalog 对缺席
// 层静默跳过。
lubancode::agent::AgentCatalogScanRoots ComputeAgentScanRoots();

// Prompt Profile 的项目层根(阶段 2):<项目根>/.lubancode/prompts,UTF-8
// 串。会话装配(给 AgentTool 塞项目层)与 /agent inspect 共用这一个口,
// 不各自猜 cwd。
std::string ComputeProjectPromptsRoot();

// 命令分派注册制:/agents 与 /agent 的分派位。
CommandFlow HandleSlashAgents(SlashDispatchContext& ctx, const lubancode::cli::ParsedSlashCommand& parsed);
CommandFlow HandleSlashAgent(SlashDispatchContext& ctx, const lubancode::cli::ParsedSlashCommand& parsed);

}  // namespace lubancode::app
