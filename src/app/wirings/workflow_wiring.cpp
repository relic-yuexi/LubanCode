// workflow 执行器装配根的实现(骨架拆解反弹·问题 3):BuildWorkflowExecutors
// 整段自 commands/workflow_commands.cpp 搬来,行为一字未改——注释一并随行。
// 这是"把零件接起来"的装配活(第 4 批宿主执行器表:/workflow run 与
// alias 直呼原先各拼一份一模一样,收进这一只函数两路共用),归 wirings/;
// 命令层只管调用。渲染不在这层——本文件没有直接终端 IO。
#include "app/wirings/workflow_wiring.hpp"

#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "agent/agent.hpp"        // AgentProfile(批四自立门户)
#include "agent/agent_catalog.hpp"  // BuiltinGeneralPurposeDefinition:workflow default binding 的定义
#include "agent/agent_profile_resolver.hpp"  // ResolveAgentProfile:阶段 3 统一解析(workflow 绑定同源)
#include "app/tool_runtime.hpp"   // ResolveCustomAgentMaterial(阶段 6:agent 节点解析钉快照)
#include "tools/path_utils.hpp"   // Utf8ToPath
#include "tools/skill_loader.hpp"  // ParseSkillMarkdown(skill 节点正文)

namespace lubancode::app {

// ---- 执行器装配(终端接线收尾单自大类两段重复装配收口;原文随行) -------

std::map<lubancode::workflow::NodeKind, std::shared_ptr<lubancode::workflow::NodeExecutor>>
BuildWorkflowExecutors(const WorkflowCommandContext& wf_ctx, const WorkflowExecutorContext& exec_ctx,
                       const std::string& workflow_id) {
    std::map<lubancode::workflow::NodeKind, std::shared_ptr<lubancode::workflow::NodeExecutor>> executors;
    auto transform = std::make_shared<lubancode::workflow::TransformExecutor>();
    transform->Register("json_merge", [](const nlohmann::json& in) { return in; });
    executors[lubancode::workflow::NodeKind::Transform] = transform;
    executors[lubancode::workflow::NodeKind::Template] =
        std::make_shared<lubancode::workflow::TemplateExecutor>();
    {
        lubancode::workflow::ToolExecutor::Options tool_options = exec_ctx.build_tool_options();
        // tool 节点同病同治(治 BuildWorkflowToolOptions 那句"确认门暂不
        // 接"):宿主确认口缺位时借 agent 节点那份 TurnWiring 补上。trace/
        // 钩子/Plan 闸是 build_tool_options 自己装的,原样保留,只补这一枚口。
        if (exec_ctx.build_agent_callbacks && !tool_options.callbacks.on_tool_confirm &&
            !tool_options.callbacks.on_tool_confirm_async) {
            tool_options.callbacks.on_tool_confirm = exec_ctx.build_agent_callbacks().on_tool_confirm;
        }
        executors[lubancode::workflow::NodeKind::Tool] =
            std::make_shared<lubancode::workflow::ToolExecutor>(std::move(tool_options));
    }
    std::shared_ptr<lubancode::workflow::LlmExecutor> llm_executor;
    {
        // prompt 从 workflow 目录读(包内相对路径;越界已被 validator
        // 拦,这里只管读)。包层成品件一并喂:packaged workflow 的 prompt
        // 相对包内 workflows/<名>/ 目录解析(dir 指到那里)。
        const lubancode::workflow::Catalog wf_catalog = lubancode::workflow::LoadCatalog(
            wf_ctx.project_root, wf_ctx.user_root, wf_ctx.packaged_workflows);
        const lubancode::workflow::CatalogEntry* wf_entry = wf_catalog.Find(workflow_id);
        const std::filesystem::path prompt_dir =
            wf_entry != nullptr ? wf_entry->dir : std::filesystem::path();
        const lubancode::workflow::PromptLoader workflow_prompt_loader = [prompt_dir](const std::string& relative) {
            if (prompt_dir.empty()) return std::string();
            std::error_code ec;
            const std::filesystem::path file = prompt_dir / relative;
            if (!std::filesystem::exists(file, ec)) return std::string();
            std::ifstream in(file, std::ios::binary);
            std::ostringstream buffer;
            buffer << in.rdbuf();
            return buffer.str();
        };
        lubancode::workflow::AgentExecutor::Options agent_options;
        agent_options.default_binding.backend = exec_ctx.backend;
        // 阶段 3:default binding 走统一 AgentProfileResolver(单子 §6.4——
        // Workflow 复用同一解析器,合并不许在两处各写一遍)。父上下文 = 会话
        // 材料(provider/model/effort/reasoning + main 运行档案),定义 = 内置
        // general-purpose(全继承,合并结果与直填父值一字不差);节点级
        // allowed_tools/step_limit 仍由 AgentExecutor 按派发参数叠加(契约
        // §4.8:调用方显式压过定义缺省)。阶段 5 接 `agent: <name>` 时同一
        // 口换定义即可。
        lubancode::agent::AgentProfile workflow_parent;
        workflow_parent.provider = exec_ctx.provider;
        workflow_parent.request.model = exec_ctx.model;
        workflow_parent.request.reasoning_effort = exec_ctx.effort;
        if (exec_ctx.model_catalog != nullptr) {
            if (const auto* entry = exec_ctx.model_catalog->FindByProviderAndSlug(exec_ctx.provider, exec_ctx.model);
                entry != nullptr) {
                workflow_parent.request.reasoning = entry->reasoning;
            }
        }
        workflow_parent.runtime = exec_ctx.agent_profile;
        std::vector<std::string> workflow_parent_tools;
        if (exec_ctx.registry != nullptr) {
            for (const auto& tool : exec_ctx.registry->All()) {
                workflow_parent_tools.push_back(tool->name());
            }
        }
        // 父上下文与父工具面留下原份:default binding 与自定义 Agent 的
        // 节点级解析共用同一份(阶段 5——两路喂同一只 Resolver 的前提)。
        const lubancode::agent::AgentProfile custom_parent = workflow_parent;
        const std::vector<std::string> custom_parent_tools = workflow_parent_tools;
        agent_options.default_binding.profile = lubancode::agent::ResolveAgentProfile(
            lubancode::agent::BuildWorkflowAgentResolveRequest(
                lubancode::agent::BuiltinGeneralPurposeDefinition(), workflow_parent,
                std::move(workflow_parent_tools), exec_ctx.agent_profile.max_steps_per_turn,
                /*default_max_turns=*/0,
                lubancode::agent::AgentProfileResolveEnvironment{}, lubancode::agent::AgentDispatchOverrides{}))
            .profile;
        // 阶段 5:`agent: <name>` 节点的解析口。查名与预装技能正文复用
        // 会话 agent 工具的同一只 resolver(Catalog 现扫现查),环境账
        //(权限档/技能/MCP/角色路由/思考档)复用它的同一只供应商——
        // 同一份定义从 agent 工具与 Workflow 节点两路解析,喂的是同一套
        // 口子,结果逐字段一致(对账册钉死的验收线)。节点 step_limit 走
        // overrides(入参显式 > YAML > 父步数)。
        // 阶段 6:解析钉 exec_ctx.package_snapshot 这份快照——跑一趟钉
        // 一份,半场 /package reload 不换这趟的 Skill/Agent 账(验收线:
        // reload 不会让半场 Workflow 换 Skill);没接快照退回会话级
        // resolver(现行快照,行为与从前一致)。
        if (exec_ctx.agent_tool != nullptr) {
            lubancode::tools::AgentTool* agent_tool = exec_ctx.agent_tool;
            const std::shared_ptr<const lubancode::package::PackageSnapshot> pinned = exec_ctx.package_snapshot;
            const std::vector<lubancode::tools::SkillMeta>* pinned_skills = exec_ctx.skills;
            agent_options.custom_agent_resolver =
                [agent_tool, pinned, pinned_skills, custom_parent, custom_parent_tools,
                 default_steps = exec_ctx.agent_profile.max_steps_per_turn](
                    const lubancode::workflow::WorkflowNode& node,
                    std::string& error) -> std::optional<lubancode::workflow::CustomAgentNodeResolution> {
                std::optional<lubancode::tools::CustomAgentMaterial> material =
                    (pinned != nullptr && pinned_skills != nullptr)
                        ? lubancode::app::ResolveCustomAgentMaterial(*pinned_skills, pinned.get(), node.agent)
                        : agent_tool->custom_agent_resolver()(node.agent);
                if (!material.has_value()) {
                    error = "没有名叫 \"" + node.agent + "\" 的 Agent(可用清单看 /agents)";
                    return std::nullopt;
                }
                std::optional<lubancode::agent::AgentProfileResolveEnvironment> environment;
                if (agent_tool->resolve_environment_provider()) {
                    environment = agent_tool->resolve_environment_provider()();
                }
                lubancode::agent::AgentDispatchOverrides overrides;
                if (node.step_limit > 0) {
                    overrides.max_steps_per_turn = node.step_limit;  // 入参显式压过 YAML
                }
                // 任务总 turn(turn 预算单 §4.3):节点的 turn_limit 走同一只
                // Resolver 的 overrides——与 agent 工具路同一笔账,只可收窄。
                // 配置默认(subagent.default_max_turns)也从 agent 工具那份
                // 只读口取,两条解析链逐级一致,不各养一本。
                if (node.turn_limit > 0) {
                    overrides.max_turns = node.turn_limit;
                }
                lubancode::workflow::CustomAgentNodeResolution out;
                out.resolved = lubancode::agent::ResolveAgentProfile(
                    lubancode::agent::BuildWorkflowAgentResolveRequest(
                        material->definition, custom_parent, custom_parent_tools, default_steps,
                        agent_tool->default_max_turns(), environment, overrides));
                out.material = std::move(*material);
                out.resolved_name = node.agent;
                if (environment.has_value() &&
                    lubancode::agent::AgentPermissionModeRank(out.resolved.permission) <
                        lubancode::agent::AgentPermissionModeRank(environment->parent_permission)) {
                    out.permission_floor = out.resolved.permission;
                }
                return out;
            };
        }
        agent_options.subagent_prompt_material = exec_ctx.subagent_prompt_material;
        // 作用域单 P0:Resolver 取会话级 agent 工具那只(与主代理同一份,
        // 三路不各养一只);没接 agent_tool(headless/旧装配)= 节点不过闸。
        if (exec_ctx.agent_tool != nullptr) {
            agent_options.instruction_resolver = exec_ctx.agent_tool->instruction_resolver();
        }
        agent_options.registry = exec_ctx.registry;
        agent_options.task_loader = workflow_prompt_loader;
        // 批二:agent 节点上事件流(会话 sink,seq 与主回合同源)。
        agent_options.event_sink = exec_ctx.event_sink;
        agent_options.thread_id = exec_ctx.thread_id;
        agent_options.ids = exec_ctx.id_authority;
        agent_options.steering = exec_ctx.steering;
        // 审批口:宿主给了确认回调就接上——needs_confirm 工具不再落
        // AgentExecutor 兜底的"未接审批宿主"明拒。
        if (exec_ctx.build_agent_callbacks) {
            agent_options.callbacks = exec_ctx.build_agent_callbacks();
        }
        executors[lubancode::workflow::NodeKind::Agent] =
            std::make_shared<lubancode::workflow::AgentExecutor>(std::move(agent_options));
        lubancode::workflow::LlmExecutor::Options llm_options;
        llm_options.backend = exec_ctx.backend;
        llm_options.model = exec_ctx.model;
        llm_options.reasoning_effort = exec_ctx.effort;
        llm_options.resolve_binding = exec_ctx.resolve_llm_binding;
        llm_options.prompt_loader = workflow_prompt_loader;
        llm_options.event_sink = exec_ctx.event_sink;
        llm_options.thread_id = exec_ctx.thread_id;
        llm_options.ids = exec_ctx.id_authority;
        llm_options.steering = exec_ctx.steering;
        llm_executor = std::make_shared<lubancode::workflow::LlmExecutor>(llm_options);
        executors[lubancode::workflow::NodeKind::Llm] = llm_executor;
    }

    executors[lubancode::workflow::NodeKind::Approval] =
        std::make_shared<lubancode::workflow::ApprovalExecutor>(exec_ctx.interaction_broker.get());
    executors[lubancode::workflow::NodeKind::AskUser] =
        std::make_shared<lubancode::workflow::AskUserExecutor>(exec_ctx.interaction_broker.get());

    std::map<std::string, std::string> skill_bodies;
    if (exec_ctx.skills != nullptr) {
        for (const auto& skill : *exec_ctx.skills) {
            const std::filesystem::path file = lubancode::tools::Utf8ToPath(skill.dir_path) / "SKILL.md";
            std::ifstream in(file, std::ios::binary);
            if (!in) continue;
            std::ostringstream content;
            content << in.rdbuf();
            const auto parsed = lubancode::tools::ParseSkillMarkdown(content.str());
            if (parsed.has_value()) skill_bodies.emplace(skill.name, parsed->body);
        }
    }
    executors[lubancode::workflow::NodeKind::Skill] =
        std::make_shared<lubancode::workflow::SkillExecutor>(llm_executor, std::move(skill_bodies));

    // nesting 首版只开一层。子流程另起自己的 Store 与预算账，只拿父节点
    // 明写的 input；事件、发号局与交互门仍沿用本场会话。
    if (exec_ctx.subflow_depth < 1) {
        const auto catalog = std::make_shared<lubancode::workflow::Catalog>(lubancode::workflow::LoadCatalog(
            wf_ctx.project_root, wf_ctx.user_root, wf_ctx.packaged_workflows));
        lubancode::workflow::SubflowExecutor::DefinitionResolver resolver =
            [catalog](const std::string& id) -> std::optional<lubancode::workflow::WorkflowDefinition> {
                const auto* entry = catalog->Find(id);
                if (entry == nullptr || entry->broken) return std::nullopt;
                return entry->definition;
            };
        lubancode::workflow::SubflowExecutor::RuntimeRunner runner =
            [wf_ctx, exec_ctx](const lubancode::workflow::WorkflowDefinition& definition,
                               const nlohmann::json& inputs) {
                WorkflowExecutorContext child_ctx = exec_ctx;
                ++child_ctx.subflow_depth;
                lubancode::workflow::RuntimeOptions options;
                options.executors = BuildWorkflowExecutors(wf_ctx, child_ctx, definition.id);
                options.event_sink = exec_ctx.event_sink;
                options.broker = exec_ctx.interaction_broker.get();
                options.thread_id = exec_ctx.thread_id;
                options.id_authority = exec_ctx.id_authority;
                lubancode::workflow::WorkflowRuntime runtime(std::move(options));
                return runtime.Run(definition, lubancode::workflow::RunInputs(inputs));
            };
        executors[lubancode::workflow::NodeKind::Subflow] =
            std::make_shared<lubancode::workflow::SubflowExecutor>(std::move(resolver), std::move(runner));
    }
    return executors;
}

}  // namespace lubancode::app
