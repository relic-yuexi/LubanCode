// AgentProfileResolver 的实现(自定义 Agent 与 Prompt Profile 单·阶段 3)。
// 合并次序与每级口径见头文件;这里的每一步都是纯函数,不摸盘不发请求。
#include "agent/agent_profile_resolver.hpp"

#include <algorithm>
#include <memory>
#include <set>
#include <utility>

namespace lubancode::agent {

namespace {

// 一条结构化错误的速记:合并期没有 YAML 行列(错在"父子合不上",不在
// 单份文件),line/column 留 -1,来源由调用方的诊断上下文补。
AgentDefinitionIssue MakeIssue(const std::string& code, const std::string& field, std::string message) {
    AgentDefinitionIssue issue;
    issue.field = field;
    issue.message = std::move(message);
    issue.code = code;
    return issue;
}

// 逐名对账:names 里哪些不在 ledger 里(保 names 原序)。
std::vector<std::string> MissingNames(const std::vector<std::string>& names,
                                      const std::vector<std::string>& ledger) {
    const std::set<std::string> known(ledger.begin(), ledger.end());
    std::vector<std::string> missing;
    for (const std::string& name : names) {
        if (known.count(name) == 0) {
            missing.push_back(name);
        }
    }
    return missing;
}

std::string JoinNames(const std::vector<std::string>& names) {
    std::string out;
    for (const std::string& name : names) {
        out += out.empty() ? name : ("、" + name);
    }
    return out;
}

// 一档角色路由的回落链一格:该档未配置(model 空)就落到上一档。
const AgentRoleRoute& OrFallback(const AgentRoleRoute& route, const AgentRoleRoute& fallback) {
    return route.model.empty() ? fallback : route;
}

}  // namespace

// AgentPermissionMode 与 Rank/ToString 已拆去 permission_mode.hpp(阶段 5:
// loop.hpp 也要用,不拆与 agent.hpp 循环)。

ResolvedAgentProfile ResolveAgentProfile(const AgentProfileResolveRequest& request) {
    ResolvedAgentProfile resolved;
    const AgentDefinition& definition = request.definition;
    const AgentProfile& parent = request.parent_profile;
    // 环境账缺席(旧调用方/单测):权限收窄与技能/MCP/思考档按"没账可查"
    // 跳过;三档角色路由落全空,回落链自然落到父模型。
    const AgentProfileResolveEnvironment empty_env;
    const AgentProfileResolveEnvironment& env =
        request.environment.has_value() ? *request.environment : empty_env;
    const bool has_env = request.environment.has_value();

    // ---- 1. runtime 预算:base = 父皮,五字段按各自的级差覆盖 ----------------
    // 次序(单子阶段 3 定死,对账测试钉):入参显式 > YAML runtime > 父值/
    // 配置默认。四枚预算字段的继承语义照 AgentRuntimeProfile 的注释——
    // 子代理只该收窄,不自开预算口子:YAML 显式给了就按给的上,没给落
    // 父值(父值在父处已走完三级解析,来源照抄,不重算)。
    AgentRuntimeProfile runtime = parent.runtime;
    runtime.max_output_tokens = definition.max_output_tokens.has_value()
                                    ? definition.max_output_tokens
                                    : parent.runtime.max_output_tokens;
    runtime.max_output_tokens_source =
        definition.max_output_tokens.has_value() ? OutputBudgetSource::ConfigFile  // 视同 config 级显式
                                                 : parent.runtime.max_output_tokens_source;
    runtime.max_context_chars =
        definition.max_context_chars.value_or(parent.runtime.max_context_chars);
    runtime.context_window_tokens = definition.context_window_tokens.value_or(
        request.context_window_tokens > 0 ? request.context_window_tokens
                                          : parent.runtime.context_window_tokens);
    runtime.length_continuations =
        definition.length_continuations.value_or(parent.runtime.length_continuations);
    runtime.max_steps_per_turn = request.overrides.max_steps_per_turn.value_or(
        definition.max_steps_per_turn.value_or(request.default_max_steps_per_turn));

    // ---- 1b. 任务总 turn 预算(turn 预算单 §4.2/§10.2/§10.3)-----------------
    // 解析链:宿主 typed override > Agent Definition runtime.max_turns >
    // subagent.default_max_turns > 0(不限)。与上面那枚 per-input step 分家:
    // 各自的数值、来源、文案都不混写。override 只可收窄——定义是正数时
    // override 不得更大、不得为 0(放宽报 agent.turn_budget_widening);
    // 定义是 0(不限)时宿主可给正数。
    resolved.turn_budget.legacy_max_steps_per_input = definition.max_steps_per_turn;
    if (request.overrides.max_turns.has_value()) {
        const int override_turns = *request.overrides.max_turns;
        const int definition_turns = definition.max_turns.value_or(0);
        const bool widening =
            definition_turns > 0 && (override_turns <= 0 || override_turns > definition_turns);
        if (widening) {
            // 诊断用的"尽量合并完"份仍带上定义值;ok() 已翻假,派发层拒发。
            resolved.turn_budget.max_turns = definition_turns;
            resolved.turn_budget.source = TurnBudgetSource::Definition;
            resolved.issues.push_back(MakeIssue(
                "agent.turn_budget_widening", "runtime.max_turns",
                "宿主 override 的任务 turn 预算(" + std::to_string(override_turns) +
                    ")比定义的正数上限(" + std::to_string(definition_turns) +
                    ")宽(override=0 也算放宽);预算只能收窄,想放宽去改 Agent 定义"));
        } else {
            resolved.turn_budget.max_turns = override_turns;
            resolved.turn_budget.source = TurnBudgetSource::HostOverride;
        }
    } else if (definition.max_turns.has_value()) {
        resolved.turn_budget.max_turns = *definition.max_turns;
        resolved.turn_budget.source = TurnBudgetSource::Definition;
    } else if (request.default_max_turns > 0) {
        resolved.turn_budget.max_turns = request.default_max_turns;
        resolved.turn_budget.source = TurnBudgetSource::Config;
    }

    resolved.profile = parent;
    resolved.profile.runtime = runtime;
    // 请求期活口不搬运(契约 §8 反向边界):system_prompt 是拼装结果、
    // model_instructions 来自 models.json、Soul 归 souls/、deferred index 是
    // 宿主注入的活口——Agent 文件写不得,Resolver 也不替宿主搬。
    resolved.profile.system_prompt.clear();
    resolved.profile.model_instructions.clear();
    resolved.profile.soul.clear();
    resolved.profile.deferred_index_provider = nullptr;
    resolved.profile.tool_filter = nullptr;
    resolved.profile.tool_filter_denial.clear();
    // 动态工具 P1:代理引用的活接线同属宿主注入,不随 Agent 文件解析搬运
    //——派发处(AgentTool::RunTask)按子侧自己的账另灌。
    resolved.profile.tool_ref_resolver = nullptr;
    resolved.profile.tool_execution_policy = nullptr;
    resolved.profile.tool_execution_denial.clear();

    // ---- 2. 模型角色与 effort -----------------------------------------------
    // 回落链照现有路由链(契约 §4.3):normal ?? 父模型;cheap ?? normal;
    // lao ?? normal(plan 是 lao 别名,解析层已归一)。provider 空 = 沿用父。
    const AgentRoleRoute normal{env.role_normal.model.empty() ? AgentRoleRoute{"", parent.request.model}
                                                               : env.role_normal};
    const std::string& role = definition.model.role;
    if (!role.empty() && role != "inherit") {
        const AgentRoleRoute* picked = &normal;
        if (role == "cheap") {
            picked = &OrFallback(env.role_cheap, normal);
        } else if (role == "lao" || role == "plan") {
            picked = &OrFallback(env.role_lao, normal);
        }
        // role == "normal" 落 normal 本档。解析层已把枚举钉死,到这里只可能
        // 是四值之一;认不得的当 inherit(防御,不另立错误口径)。
        if (!picked->model.empty()) {
            resolved.profile.request.model = picked->model;
            if (!picked->provider.empty()) {
                resolved.profile.provider = picked->provider;
            }
        }
    }
    const std::string& effort = definition.model.effort;
    if (!effort.empty() && effort != "inherit") {
        if (!env.supported_efforts.empty() &&
            std::find(env.supported_efforts.begin(), env.supported_efforts.end(), effort) ==
                env.supported_efforts.end()) {
            resolved.issues.push_back(MakeIssue(
                "agent.effort_not_supported", "model.effort",
                "effort \"" + effort + "\" 不在 provider 声明的思考档里(声明档:" +
                    JoinNames(env.supported_efforts) + ");越界报错,不悄悄降档"));
        } else {
            resolved.profile.request.reasoning_effort = effort;
        }
    }

    // ---- 3. 工具过滤:allow 只许点名父有效面,deny 压过 allow ----------------
    const std::set<std::string> allow(definition.tools.allow.begin(), definition.tools.allow.end());
    const std::set<std::string> deny(definition.tools.deny.begin(), definition.tools.deny.end());
    if (const std::vector<std::string> not_granted = MissingNames(definition.tools.allow,
                                                                  request.parent_tool_names);
        !not_granted.empty()) {
        resolved.issues.push_back(MakeIssue(
            "agent.tool_not_granted", "tools.allow",
            "allow 点到父 Agent 没有的工具: " + JoinNames(not_granted) +
                "(allow 不是授权,只许在父有效工具面里挑;父面以当前会话注册表为准)"));
    }
    for (const std::string& name : request.parent_tool_names) {
        if (deny.count(name) != 0) {
            continue;  // deny 压过 allow(单子测试账)
        }
        if (!allow.empty() && allow.count(name) == 0) {
            continue;  // 非空 allow = 白名单,名单外的全不可见
        }
        resolved.effective_tools.push_back(name);
    }
    // requires.tools 是启动前断言:必须落在有效面里,缺了报缺,不悄悄放宽
    //(契约 §4.7)。
    if (const std::vector<std::string> missing = MissingNames(definition.requires_tools,
                                                              resolved.effective_tools);
        !missing.empty()) {
        resolved.issues.push_back(MakeIssue(
            "agent.missing_dependency", "requires.tools",
            "requires.tools 断言不过: " + JoinNames(missing) +
                " 不在过滤后的有效工具面里(缺项报错,不退化成全工具 Agent)"));
    }
    // 非空 allow/deny 写进皮(与 AgentTool 派发链此前手工构的同一只谓词,
        // 搬家不改语义);allow 空 = 不过滤,沿用调用方装配。
    if (!definition.tools.allow.empty() || !definition.tools.deny.empty()) {
        auto allow_copy = std::make_shared<const std::set<std::string>>(allow);
        auto deny_copy = std::make_shared<const std::set<std::string>>(deny);
        resolved.profile.tool_filter = [allow_copy, deny_copy](const tools::Tool& tool) {
            if (deny_copy->count(tool.name()) != 0) {
                return false;  // deny 压过 allow(与 doctor 同一本账)
            }
            return allow_copy->empty() || allow_copy->count(tool.name()) != 0;
        };
        resolved.profile.tool_filter_denial =
            "此工具不在自定义 Agent " + definition.name + " 的工具边界内(角色限制):定义只开放 tools.allow 名单"
            "(deny 再压一层)。请改用名单内工具完成;确需边界外操作,把建议写进结论交回主代理执行。";
    }

    // ---- 4. MCP/Skill 引用:只许点名已有账,缺了报缺 -------------------------
    if (has_env) {
        if (const std::vector<std::string> missing = MissingNames(definition.mcp_servers,
                                                                  env.mcp_server_names);
            !missing.empty()) {
            resolved.issues.push_back(MakeIssue(
                "agent.missing_dependency", "mcp_servers",
                "引用的 MCP 服务未配置或未挂载: " + JoinNames(missing) +
                    "(只许引用已配置、已信任的服务名;缺项报错,不悄悄放宽)"));
        }
        if (const std::vector<std::string> missing = MissingNames(definition.skills_preload,
                                                                  env.skill_names);
            !missing.empty()) {
            resolved.issues.push_back(MakeIssue(
                "agent.missing_dependency", "skills.preload",
                "预装技能不在已扫描技能清单里: " + JoinNames(missing) +
                    "(缺项报错,不静默跳过;先看 /skills 清单或 /agent doctor)"));
        }
    }

    // ---- permissions:父子能力求交 -------------------------------------------
    // inherit/空 = 同父。显式子档不因“更宽”拒发，而是按自动能力集合求交；
    // may_prompt 单独取 AND。Yolo 的 may_prompt=true，含义是其自身 All 能力
    // 通常无需问，不会封死父 Yolo + 子 Default 沿 floored 链拉回主会话；
    // 只有 DontAsk 禁止后代询问。
    const AgentPermissionMode parent_permission =
        has_env ? env.parent_permission : request.parent_permission;
    resolved.permission = parent_permission;
    const std::string& declared = definition.permissions_mode;
    if (!declared.empty() && declared != "inherit") {
        AgentPermissionMode child = AgentPermissionMode::Default;
        if (declared == "accept_edits") {
            child = AgentPermissionMode::AcceptEdits;
        } else if (declared == "auto") {
            child = AgentPermissionMode::Auto;
        } else if (declared == "yolo") {
            child = AgentPermissionMode::Yolo;
        } else if (declared == "dont_ask") {
            child = AgentPermissionMode::DontAsk;
        }
        resolved.permission = IntersectPermissionModes(parent_permission, child);
    }

    // ---- 6. prompt 三笔与缺省档:原样决议 ------------------------------------
    resolved.prompt_profile = definition.prompt.profile.value_or(std::string());
    resolved.project_instructions =
        definition.prompt.project_instructions != AgentPromptSpec::ProjectInstructions::Omit;
    resolved.soul = definition.prompt.soul != AgentPromptSpec::Soul::Off;
    resolved.execution_mode = definition.execution_mode.empty() ? std::string("auto")
                                                                : definition.execution_mode;
    resolved.isolation = definition.isolation.empty() ? std::string("none") : definition.isolation;
    return resolved;
}

AgentProfileResolveRequest BuildSubagentResolveRequest(
    const AgentDefinition& definition, const AgentProfile& parent_profile,
    std::vector<std::string> parent_tool_names, int default_max_steps_per_turn, int default_max_turns,
    std::size_t context_window_tokens, std::optional<AgentProfileResolveEnvironment> environment,
    const AgentDispatchOverrides& overrides) {
    AgentProfileResolveRequest request;
    request.definition = definition;
    request.parent_profile = parent_profile;
    request.parent_tool_names = std::move(parent_tool_names);
    request.environment = std::move(environment);
    request.default_max_steps_per_turn = default_max_steps_per_turn;
    request.default_max_turns = default_max_turns;
    request.context_window_tokens = context_window_tokens;
    request.overrides = overrides;
    return request;
}

AgentProfileResolveRequest BuildWorkflowAgentResolveRequest(
    const AgentDefinition& definition, const AgentProfile& parent_profile,
    std::vector<std::string> parent_tool_names, int default_max_steps_per_turn, int default_max_turns,
    std::optional<AgentProfileResolveEnvironment> environment, const AgentDispatchOverrides& overrides) {
    AgentProfileResolveRequest request;
    request.definition = definition;
    request.parent_profile = parent_profile;
    request.parent_tool_names = std::move(parent_tool_names);
    request.environment = std::move(environment);
    request.default_max_steps_per_turn = default_max_steps_per_turn;
    request.default_max_turns = default_max_turns;
    request.context_window_tokens = 0;  // Workflow 没有会话另行同步的窗口,落父皮值
    request.overrides = overrides;
    return request;
}

std::string FormatResolutionIssues(const std::vector<AgentDefinitionIssue>& issues) {
    std::string out;
    for (const AgentDefinitionIssue& issue : issues) {
        if (issue.warning) {
            continue;
        }
        if (!out.empty()) {
            out += "\n";
        }
        out += "[" + issue.code + "] " + issue.field + ": " + issue.message;
    }
    return out;
}

}  // namespace lubancode::agent
