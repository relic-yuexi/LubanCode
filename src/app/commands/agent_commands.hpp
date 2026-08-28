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

// ---------------- 纯函数(单测钉住) ----------------

// /agents 的全部输出行:每名一志(名称/层/可用性/描述/模型/Profile/工具/
// Skill 数),不可用带第一条错,跨层覆盖带"盖住"账,末尾摆加载警告。
// 不摸 IO,行怎么打由调用方定。
std::vector<std::string> FormatAgentCatalogListing(const lubancode::agent::AgentCatalog& catalog);

// /agent doctor <name> 的静态预检报告(单子八"agent doctor"清单里阶段 1
// 能查的:定义解析、覆盖链、Skill/MCP/工具引用、模型角色写法、runtime 与
// permissions 登账;Profile 存在性属阶段 2,权限越界比对属阶段 3,都如实
// 写明,不装查过了)。查无此名给一行"没这个 Agent,先 /agents"。
std::vector<std::string> FormatAgentDoctorReport(const lubancode::agent::AgentCatalog& catalog,
                                                 const std::string& name, const AgentDoctorMaterials& materials);

// ---------------- 执行(IO) ----------------

// 会话里现算三层扫描根:builtin = <exe 目录>/agents(通常没有)、user =
// ~/.lubancode/agents、project = 项目根(FindProjectRoot,复用 instruction
// 发现规则)/.lubancode/agents。目录不存在照旧进 roots——Catalog 对缺席
// 层静默跳过。
lubancode::agent::AgentCatalogScanRoots ComputeAgentScanRoots();

// 命令分派注册制:/agents 与 /agent 的分派位。
CommandFlow HandleSlashAgents(SlashDispatchContext& ctx, const lubancode::cli::ParsedSlashCommand& parsed);
CommandFlow HandleSlashAgent(SlashDispatchContext& ctx, const lubancode::cli::ParsedSlashCommand& parsed);

}  // namespace lubancode::app
