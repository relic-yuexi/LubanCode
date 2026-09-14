// Agent/Skill 装配的解析与提示部件组合(应用Worker接入补齐单 P2,§五/§六;
// GAP-01/02/03 的接线件)。
//
// 职责切分:
//   ResolveHarnessAgentPlan —— 进程启动时(RunAppServerMode)解析部署档的
//       Agent 面:agentRef 必须落到可用档案(AgentCatalog 既有目录),找不到/
//       不可用即明拒,不回落编码助手默认提示词(单子 §5.1;P0 铁律"缺件即
//       拒")。解析结果是冻结件:开场后文件改动不热换,新场照旧(§五)。
//   ComposeHarnessSystemPrompt —— 会话装配期(session_assembly)把冻结计划
//       折成系统提示。拼装走 prompt_assembler 既有管线(AssembleSystemPrompt
//       五层回路),这里只折 PromptOptions,不另写一套拼装。
//
// 覆盖合同(§5.2)按字段种类落:
//   - 业务正文:档案点名的 Prompt Profile(嵌入选中层/参数根用户层的五层
//     回路)或档案 persona(名+描述);档案缺 Profile 时 core 落内置默认
//     人格(发行内置材料,§13.2 允许——这是档案声明的继承,不是缺件回落)。
//   - 宿主段(运行环境/能力说明/平台/模式)恒在,由实际工具面与 wire 现拼,
//     正文/Skill 都盖不掉("宿主安全规则不可覆盖")。
//   - project_instructions 恒空:headless 不读 cwd/AGENTS.md(§13.2 来源
//     裁剪;部署档点名来源的口子未开,不注就不注)。
//
// 来源纪律:目录由调用方按 RuntimePaths 折好递进来(参数根下 agents/
// skills/prompts 三处;个人模式即 ~/.lubancode 同名目录)。这一层不摸环境
// 变量、不做隐式多层扫描——终端 LoadSkills 的五层合并与 cwd 项目层不进
// headless(§13.2;GAP-03:不为 headless 复制隐式扫描)。
#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include "agent/agent_definition.hpp"
#include "agent/prompt_assembler.hpp"
#include "app_server/harness_profile.hpp"
#include "tools/skill_loader.hpp"

namespace lubancode::app_server {

// Agent/Skill 装配的显式来源根。
struct HarnessAgentSources {
    // Agent 档案根(<材料根>/agents)。空 = 无磁盘档案层——Catalog 只剩码内
    // 内置两枚(general-purpose/Explore);发行 yaml 内置层与 cwd 项目层
    // 不进 headless 视野(§13.2),不在这里补。
    std::optional<std::filesystem::path> agents_dir;
    // Skill 材料根(<材料根>/skills)。空 = 无技能来源(不扫描)。
    std::optional<std::filesystem::path> skills_dir;
    // 提示模块/Profile 用户层(<材料根>/prompts),UTF-8。空 = 只用嵌入版。
    std::string prompts_dir_utf8;
};

// 启动时冻结的 Agent 装配计划(单子 §5.1 SessionSpec 的 P2 最小形状)。
struct HarnessAgentPlan {
    // agentRef 的解析结果。档点名了 agentRef 就必非空(解析失败在
    // ResolveHarnessAgentPlan 明拒,到不了这里);档没点名 = nullopt。
    std::optional<lubancode::agent::AgentDefinition> agent;
    HarnessAgentSources sources;
    std::string wire;  // PromptOptions.wire(平台段;app-server 会话 wire 名)
    std::string cwd;   // PromptOptions.cwd(运行环境段)
};

struct HarnessAgentPlanResult {
    std::optional<HarnessAgentPlan> plan;  // 空 = 拒启(error 有人话)
    std::string error;
};

// 解析部署档的 Agent 面。agentRef 必须解析到可用档案:不存在、解析坏了
// (Catalog 登成 unavailable)都明拒。纯读盘,零进程副作用。
HarnessAgentPlanResult ResolveHarnessAgentPlan(const HarnessProfile& harness,
                                               HarnessAgentSources sources, std::string wire,
                                               std::string cwd);

// 提示部件组合的入参:冻结计划 + 本场实际材料。
struct HarnessPromptInput {
    const HarnessAgentPlan* plan = nullptr;
    // 已扫描冻结的技能清单(session_assembly 按显式单根扫出的那份)。
    const std::vector<lubancode::tools::SkillMeta>* skills = nullptr;
    // 本场注册表实际工具名(wire 名,如 mcp__server__tool、skill)——能力
    // 段按真实工具面开合(§5.2"宿主规则与能力说明由实际权限/工具快照生成")。
    std::vector<std::string> face_names;
};

struct HarnessPromptResult {
    std::string text;   // 拼装好的系统提示(error 空时有效)
    std::string error;  // 非空 = 明拒(如 skills.preload 缺名)
    // 来源账:拼装现场逐段记账(§五"组合顺序、各段来源"的账)。各段带
    // 渲染正文的 content_hash 与拼装次序(prompt_assembler A1 就地记),
    // server 在 v3 场把整本账折成 prompt.composition.applied 事实行落 V3
    // 轨迹(§五 134)。
    lubancode::agent::PromptSourceLedger ledger;
    // 最终拼装正文的 SHA-256(小写 hex64;"最终快照 ID")。error 非空时
    // 为空串。
    std::string snapshot_id;
};

// 把冻结计划折成系统提示(prompt_assembler 既有管线)。skills.preload 的
// 名字必须落在已扫描清单里,缺名明拒(§5.1 缺依赖拒启,不悄悄丢)。
HarnessPromptResult ComposeHarnessSystemPrompt(const HarnessPromptInput& input);

}  // namespace lubancode::app_server
