// AgentProfileResolver(自定义 Agent 与 Prompt Profile 单·阶段 3):父上下文
// + AgentDefinition -> Resolved AgentProfile 的唯一权威。散在 AgentTool 派发
// 链里的手工合并(预算、工具过滤、prompt 三笔开关、模型继承)收进这一处;
// Workflow 的 agent 绑定走同一只解析器(单子 §6.4:一条业务规矩只许有一处
// 权威)。契约 docs/reference/agents.md §8 的对照表是合并口径。
//
// 合并次序(定死,测试钉):
//   1. runtime 预算:base = 父皮 runtime(三级解析已在父处算完);
//      五字段各有两级——max_steps_per_turn 走 入参显式 > YAML > 配置默认,
//      其余四枚(max_output_tokens/max_context_chars/context_window_tokens/
//      length_continuations)走 YAML 显式 > 父值(单子定死:子代理只该收窄,
//      不自开预算口子——YAML 显式给了就按给的上,没给落父值);
//      max_output_tokens 视同 config 级显式声明(来源标 ConfigFile)。
//   2. 模型角色:role 空/inherit 照抄父;cheap/normal/lao 按回落链解析
//      (normal ?? 父模型;cheap ?? normal;lao ?? normal)。effort 同理,
//      给了且 provider 声明了思考档表而不含它,报 agent.effort_not_supported。
//   3. 工具过滤:allow 空 = 全继承;allow 逐名对父有效面查账,不在则报
//      agent.tool_not_granted;deny 压过 allow;有效名单保父面次序。
//      requires.tools 必须落在有效面里,缺了报 agent.missing_dependency。
//   4. MCP/Skill 引用:只许点名已挂载服务与已扫描技能,缺了报
//      agent.missing_dependency,不悄悄放宽。
//   5. permissions:inherit = 同父；显式五档按自动能力集合求交，may_prompt
//      取 AND，子代理不能扩大父能力。
//   6. prompt 三笔与缺省档:profile 名、project_instructions 继承开关、
//      soul 启停、execution_mode/isolation 缺省档,原样决议。
//
// 不碰的东西(契约 §6.2/§8,阶段 2 黄金测试钉着):system_prompt 拼装、
// model_instructions、Soul 正文、deferred tool index 都是请求期活口,由宿主
// 装配;Resolver 连搬运都不搬运(输出皮上这些字段一律清空)。
//
// 依赖方向:agent 层不 include config/runtime 一族(见 runtime_profile.hpp
// 的同款规矩),父会话的账(权限档、角色路由、思考档表、技能/MCP 名单)由
// 宿主折成纯数据递进来;同一份父上下文 + 同一份定义,解析结果完全确定。
#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <vector>

#include "agent/agent.hpp"             // AgentProfile:合并结果的皮
#include "agent/agent_definition.hpp"  // AgentDefinition/Issue:定义与诊断
#include "agent/permission_mode.hpp"   // AgentPermissionMode:权限三档(阶段 5 拆出的轻头)
#include "agent/turn_budget.hpp"       // AgentTurnBudgetProfile:任务 turn 预算档(turn 预算单 §10.2)

namespace lubancode::agent {


// 一档模型角色路由:model 空 = 该角色未配置(走回落链);provider 空 =
// 沿用父 provider(与 config 的 ModelRoleRouteConfig 同义,剥掉 effort/
// 窗口等与 Agent YAML 无关的格)。
struct AgentRoleRoute {
    std::string provider;
    std::string model;
};

// 派发时的入参显式覆盖(agent 工具入参 / Workflow 节点字段):给了就压过
// YAML runtime(契约 §4.8:调用方显式给值压过定义的缺省档)。
struct AgentDispatchOverrides {
    std::optional<int> max_steps_per_turn;  // agent 工具 max_steps_per_turn / 节点 step_limit
    // 任务总 turn 的宿主 typed override(turn 预算单 §4.2/§10.3):模型
    // schema 不暴露,只有宿主/Workflow 节点/测试夹具可给。只可收窄——
    // 把定义的正数改小可以,改大或把正数改成 0(不限)报
    // agent.turn_budget_widening;定义本就是 0(不限)时可给正数。
    std::optional<int> max_turns;
};

// 父会话的活材料账(AgentTool 自己没有、宿主递进来的那几笔)。工具名不
// 在这里——父有效工具面由调用方按自家注册表算(两张注册表各算各的)。
// 全空(宿主没接)时权限与依赖校验按"没账可查"跳过:不报错,也不放宽,
// 与 doctor 的"会话工具表不可用,跳过比对"同一骨气。
struct AgentProfileResolveEnvironment {
    AgentPermissionMode parent_permission = AgentPermissionMode::Default;
    std::vector<std::string> skill_names;        // 已扫描技能名(skills.preload 查账)
    std::vector<std::string> mcp_server_names;   // 已配置且挂载的 MCP 服务名
    AgentRoleRoute role_normal;                  // 三档角色路由;normal 未配置回落父模型
    AgentRoleRoute role_cheap;                   // 空 = 回落 normal
    AgentRoleRoute role_lao;                     // 空 = 回落 normal
    std::vector<std::string> supported_efforts;  // provider 声明的思考档;空 = 未声明,不查
};

// 一份解析请求:定义 + 父上下文 + 入参覆盖。
struct AgentProfileResolveRequest {
    AgentDefinition definition;
    AgentProfile parent_profile;  // 父 Agent 的有效皮(provider/request/runtime/四段开关)
    AgentPermissionMode parent_permission = AgentPermissionMode::Default;
    std::vector<std::string> parent_tool_names;  // 父会话有效工具面(完整名,allow 只许点名这里)
    // 环境账:给了(生产装配必给)就查权限收窄与技能/MCP/思考档;std::nullopt
    //= 宿主没接(旧调用方/单测),这几笔按"没账可查"跳过——不报错,也不
    // 放宽。requires.tools 断言不靠环境(工具面是注册表折的),恒查。
    std::optional<AgentProfileResolveEnvironment> environment;
    // max_steps_per_turn 三级里的最底层(子代理:subagent 段一脉的配置默认;
    // Workflow:父皮的步数值)。YAML 与入参都缺席时用它。
    int default_max_steps_per_turn = 0;
    // 任务总 turn 三级里的最底层(turn 预算单 §4.2:subagent.default_max_
    // turns,0/未设 = 不限)。定义与 override 都缺席时用它。
    int default_max_turns = 0;
    // 会话另行同步的上下文窗口(AgentTool::SetContextWindowTokens 那口;
    // 0 = 用父皮里的值)。父值与它之间它赢——这是"发轮前再同步"的活值。
    std::size_t context_window_tokens = 0;
    AgentDispatchOverrides overrides;
};

// 解析结果:皮上的静态决策一份。ok() 为假时 profile 仍是"尽量合并完"的
// 份(诊断与展示用),派发层必须拒发。
struct ResolvedAgentProfile {
    AgentProfile profile;  // provider/request/runtime/prompt_sections 已合并;
                           // tool_filter/denial 已按 allow/deny 装好(自定义路)
    std::vector<std::string> effective_tools;  // 过滤后的有效工具名(保父面次序)
    std::string prompt_profile;                // 空 = 未选 Profile(生成 persona 让位)
    bool project_instructions = true;          // 继承 AGENTS.md 段;false = omit
    bool soul = true;                          // 魂启停;false = off
    std::string execution_mode;                // 定义缺省档:auto/foreground/background
    std::string isolation;                     // 定义缺省档:none/worktree
    AgentPermissionMode permission = AgentPermissionMode::Default;  // 决议后的权限档
    // 任务总 turn 预算(turn 预算单 §10.2):数值 + 来源 + legacy per-input
    // step 的影子账,与皮上那枚含糊的 runtime.max_steps_per_turn 分家。宿主
    // override 只可收窄,放宽在解析口明拒(agent.turn_budget_widening)。
    AgentTurnBudgetProfile turn_budget;
    std::vector<AgentDefinitionIssue> issues;  // 结构化错误(code 见契约 §9.2/§9.3)

    bool ok() const {
        for (const AgentDefinitionIssue& issue : issues) {
            if (!issue.warning) {
                return false;
            }
        }
        return true;
    }
};

// 唯一权威:合并 + 校验,一步出。纯函数,不摸盘、不发请求、不读环境变量。
ResolvedAgentProfile ResolveAgentProfile(const AgentProfileResolveRequest& request);

// AgentTool 派发路的请求装配:把 AgentTool 自持的父材料(皮、配置默认步数、
// 同步窗口)折成一份 resolve request。agent_tool.cpp 的 execute() 从这一口
// 进,对账测试也从这一口进——这条路的规矩不许在调用点另写一遍。
AgentProfileResolveRequest BuildSubagentResolveRequest(const AgentDefinition& definition,
                                                        const AgentProfile& parent_profile,
                                                        std::vector<std::string> parent_tool_names,
                                                        int default_max_steps_per_turn,
                                                        int default_max_turns,
                                                        std::size_t context_window_tokens,
                                                        std::optional<AgentProfileResolveEnvironment> environment,
                                                        const AgentDispatchOverrides& overrides);

// Workflow agent 绑定路的请求装配:会话材料折的父皮(阶段 3 起 default
// binding 也走统一 resolver;`agent: <name>` 点名自定义 Agent 的 schema 接
// 线在阶段 5,届时同一口换定义即可)。没有"会话另行同步的窗口"这回事,
// context_window 落父皮值。
AgentProfileResolveRequest BuildWorkflowAgentResolveRequest(const AgentDefinition& definition,
                                                             const AgentProfile& parent_profile,
                                                             std::vector<std::string> parent_tool_names,
                                                             int default_max_steps_per_turn,
                                                             int default_max_turns,
                                                             std::optional<AgentProfileResolveEnvironment> environment,
                                                             const AgentDispatchOverrides& overrides);

// 结构化错误的一行情报(派发层明拒时用):"[code] field: message" 逐条,
// 不打密钥、不打环境变量值。
std::string FormatResolutionIssues(const std::vector<AgentDefinitionIssue>& issues);

}  // namespace lubancode::agent
