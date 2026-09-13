// agent_wiring.hpp 的实现。Catalog/Profile/Skill 全是既有件:AgentCatalog
// 管扫描与分层,prompt_assembler 管五层回路拼装,SkillLoader 管 SKILL.md
// 解析与正文读取——这里只做 headless 的折中接线(来源显式、缺件明拒)。
#include "app_server/agent_wiring.hpp"

#include <algorithm>
#include <utility>

#include "agent/agent_catalog.hpp"
#include "tools/agent_tool.hpp"  // AppendPreloadedSkills:预装技能段的同源件

namespace lubancode::app_server {

namespace {

// 档案没点名 Prompt Profile 时的业务正文。与 agent_tool 的
// CustomAgentPersona 同骨(名+描述+姿态),语气按 app-server 主代理改写——
// 那边是"子代理",这边是本场主事人。这只是 persona 一段的内容;拼装次序
// 与其余各段仍归 AssembleSystemPrompt,不另起一套。
std::string HarnessAgentPersona(const agent::AgentDefinition& definition) {
    std::string persona = "你是 " + definition.name + "。";
    if (!definition.description.empty()) {
        persona += definition.description + " ";
    }
    persona += "专注部署档交代的任务,完成后直接给出结论。";
    return persona;
}

// headless 的技能清单来源注记:终端清单段讲五层目录约定,这里只认部署
// 材料根一处,如实说,不让模型去 ~/.agents 里找不存在的技能。
constexpr const char* kSkillSourceNote =
    "本场技能只来自部署材料根的 skills/ 目录(仅此一处,别处不扫)。";

}  // namespace

HarnessAgentPlanResult ResolveHarnessAgentPlan(const HarnessProfile& harness, HarnessAgentSources sources,
                                               std::string wire, std::string cwd) {
    HarnessAgentPlanResult result;
    HarnessAgentPlan plan;
    plan.sources = std::move(sources);
    plan.wire = std::move(wire);
    plan.cwd = std::move(cwd);
    if (!harness.agent_ref.empty()) {
        // headless 的 Catalog 解析面:码内内置 + 参数根 agents/ 一层。终端
        // 的发行 yaml 内置层与 cwd 项目层不进 headless(§13.2 来源裁剪)。
        agent::AgentCatalogScanRoots roots;
        roots.user_dir = plan.sources.agents_dir;
        const agent::AgentCatalog catalog = agent::LoadAgentCatalog(roots);
        const agent::AgentCatalogEntry* entry = catalog.Find(harness.agent_ref);
        if (entry == nullptr) {
            result.error = "agentRef 指名的档案不存在: " + harness.agent_ref +
                           "(解析面:码内内置 + 材料根 agents/;缺件明拒,不回落默认提示词)";
            return result;
        }
        if (!entry->available || !entry->definition.has_value()) {
            const std::string reason = entry->FirstError();
            result.error = "agentRef 指名的档案不可用: " + harness.agent_ref + "(" +
                           (reason.empty() ? std::string("解析失败") : reason) + ")";
            return result;
        }
        plan.agent = *entry->definition;
    }
    result.plan = std::move(plan);
    return result;
}

HarnessPromptResult ComposeHarnessSystemPrompt(const HarnessPromptInput& input) {
    HarnessPromptResult result;
    const HarnessAgentPlan& plan = *input.plan;

    agent::PromptOptions options;
    options.cwd = plan.cwd;
    options.wire = plan.wire;
    options.prompts_dir = plan.sources.prompts_dir_utf8;
    // project_instructions 恒空(见文件头覆盖合同):headless 不读
    // cwd/AGENTS.md,PromptOptions 缺省即空串,这里不注一笔。

    // 预装技能段(skills.preload,§六"固定业务流程允许显式 eager 注入"):
    // 名字必须落在已扫描清单里——缺名即缺依赖,整场明拒(§5.1)。
    // skills 缺席按空清单算:preload 非空时第一笔查账就报缺名。
    const std::vector<tools::SkillMeta> no_skills;
    const std::vector<tools::SkillMeta>* skills =
        input.skills != nullptr ? input.skills : &no_skills;
    std::string preload_appendix;
    if (plan.agent.has_value() && !plan.agent->skills_preload.empty()) {
        const std::vector<std::string>& names = plan.agent->skills_preload;
        std::vector<std::string> bodies;
        bodies.reserve(names.size());
        for (const std::string& name : names) {
            const auto meta = std::find_if(
                skills->begin(), skills->end(),
                [&](const tools::SkillMeta& candidate) { return candidate.name == name; });
            if (meta == skills->end()) {
                result.error = "Agent 预装技能不在本场获准清单里: " + name +
                               "(Skill 来源:材料根 skills/;缺件明拒,不回落)";
                return result;
            }
            const auto body = tools::ReadSkillBody(*meta);
            bodies.push_back(body.value_or(std::string()));
        }
        preload_appendix = tools::AppendPreloadedSkills(names, bodies);
    }

    if (plan.agent.has_value()) {
        const agent::AgentDefinition& definition = *plan.agent;
        if (definition.prompt.profile.has_value()) {
            // 业务正文:Profile 五层回路(嵌入 Profile 层/参数根用户层)。
            options.profile = *definition.prompt.profile;
        } else {
            options.persona = HarnessAgentPersona(definition);
        }
    }
    // plan.agent 缺席(档没点名 agentRef):persona 空、profile 空——core 落
    // 内置默认人格(发行内置材料),宿主段照拼。这是"没点名"的显式结果,
    // 不是缺件回落。

    // 能力段按真实工具面开合(§5.2):capabilities 给了值,features 四件套
    // 与 web/mcp/lsp 段就只认这张表,不看配置开关。
    options.capabilities = agent::DerivePromptCapabilities(input.face_names);

    if (!skills->empty()) {
        options.skills_segment = tools::BuildSkillsPromptSegment(*skills, kSkillSourceNote);
        if (!preload_appendix.empty()) {
            options.skills_segment += "\n\n";
            options.skills_segment += preload_appendix;
        }
    }

    result.text = agent::AssembleSystemPrompt(options, &result.ledger);
    return result;
}

}  // namespace lubancode::app_server
