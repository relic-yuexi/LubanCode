// 会话钉快照与内容组件挂载的实现(统一 Package 封装单阶段 3)。
#include "package/mounting.hpp"

#include <algorithm>
#include <map>
#include <set>
#include <utility>

#include "platform/paths.hpp"

namespace lubancode::package {

namespace {

using platform::PathToUtf8;
using platform::Utf8ToPath;

// /skills、/agents、workflow list 的来源标签:包层带上 scope,人看得出来
// 自哪一层装的("包(project)"、"包(dev)"...)。
std::string PackageSourceLevel(PackageScope scope) {
    return "包(" + ScopeToString(scope) + ")";
}

// 本包组件账(轻量:kind -> local id 集),折 canonical 用。
struct OwnComponents {
    std::set<std::string> agents, prompt_profiles, skills, workflows;

    bool Has(ComponentKind kind, const std::string& local_id) const {
        switch (kind) {
            case ComponentKind::Agent: return agents.count(local_id) > 0;
            case ComponentKind::PromptProfile: return prompt_profiles.count(local_id) > 0;
            case ComponentKind::Skill: return skills.count(local_id) > 0;
            case ComponentKind::Workflow: return workflows.count(local_id) > 0;
            default: return false;  // plugin/mcp 不折 canonical 短名(阶段 4/5 再挂)
        }
    }
};

OwnComponents CollectOwn(const PackageRecord& record) {
    OwnComponents own;
    for (const auto& component : record.components) {
        switch (component.kind) {
            case ComponentKind::Agent: own.agents.insert(component.local_id); break;
            case ComponentKind::PromptProfile: own.prompt_profiles.insert(component.local_id); break;
            case ComponentKind::Skill: own.skills.insert(component.local_id); break;
            case ComponentKind::Workflow: own.workflows.insert(component.local_id); break;
            default: break;
        }
    }
    return own;
}

// 包内短名折 canonical:本包有这件组件就折;没有(外部裸名、显式全名、
// "default"/空)原样保留。引用闭合已在 AnalyzePackage 判过,这里只折名,
// 不再报错。
std::string CanonicalizeRef(const OwnComponents& own, const std::string& package_id,
                            const std::string& raw, ComponentKind kind) {
    if (raw.empty() || raw == "default") return raw;
    if (raw.find(':') != std::string::npos) return raw;  // 显式全名,原样
    if (own.Has(kind, raw)) return package_id + ":" + raw;
    return raw;  // 外部裸名(standalone skill 等),原样
}

// 未信任 code 组件的连坐账(阶段 4):包未过信任门时,直接引用本包
// Plugin/MCP 的组件(Agent 的 mcp_servers、tools.allow/requires.tools 的
// wire 名;Workflow 的 tool 节点)不可用;引用不可用组件的(Workflow 的
// agent 节点)同样不可用——定点迭代到不再涨。返回 canonical id -> 缘由。
// 过了门或没有 code 组件的包,连坐账为空(内容件全数照挂)。
std::map<std::string, std::string> UnavailableForUntrustedCode(const PackageRecord& record) {
    std::map<std::string, std::string> out;
    if (record.code_trust != CodeTrustStatus::PendingTrust) {
        return out;
    }
    std::map<std::string, ComponentKind> kinds;
    for (const auto& component : record.components) {
        kinds.emplace(component.canonical_id, component.kind);
    }
    bool grew = true;
    while (grew) {
        grew = false;
        for (const auto& ref : record.references) {
            if (!ref.resolved || !ref.in_package || ref.is_file_ref) continue;
            if (out.count(ref.from) > 0) continue;
            // 工具型 target 是 <canonical>:<tool>(两枚冒号),剥尾段回到组件。
            std::string target = ref.target;
            if (std::count(target.begin(), target.end(), ':') == 2) {
                target = target.substr(0, target.rfind(':'));
            }
            const auto kind_it = kinds.find(target);
            if (kind_it == kinds.end()) continue;
            if (kind_it->second == ComponentKind::Plugin || kind_it->second == ComponentKind::McpServer) {
                out.emplace(ref.from,
                            "依赖的 " + std::string(ComponentKindName(kind_it->second)) + " " + target +
                                " 未过信任门(包未批准或文件动过哈希已变),批准前不挂不执行;"
                                "批准: /package trust " + record.inventory.package_id);
                grew = true;
            } else if (out.count(target) > 0) {
                out.emplace(ref.from, "依赖的 " + target + " 因未信任的 code 组件不可用");
                grew = true;
            }
        }
    }
    return out;
}

}  // namespace

// ---------------------------------------------------------------------------
// BuildPackageMount
// ---------------------------------------------------------------------------

const PackageMountEntry* PackageMount::Find(const std::string& package_id) const {
    for (const auto& entry : entries) {
        if (entry.package_id == package_id) return &entry;
    }
    return nullptr;
}

PackageMount BuildPackageMount(const PackageMountInput& input) {
    PackageMount mount;

    // 四层扫描;同 id 定胜者:ScanPackages 已按优先级从高到低排,先见者胜
    //(与 BuildPackageRefIndex 同口径)。被遮的候选不挂载——/package 命令
    // 的现扫账里它们照旧可 list/show/doctor。store 的选中版本(阶段 4)按
    // 优先级插进 project 与 user 之间:dev > project > store > user > official。
    std::vector<PackageCandidate> candidates = ScanPackages(input.scan);
    if (!input.store_candidates.empty()) {
        const auto insert_at = std::find_if(
            candidates.begin(), candidates.end(), [](const PackageCandidate& scanned) {
                return ScopePrecedence(scanned.scope) < ScopePrecedence(PackageScope::Store);
            });
        candidates.insert(insert_at, input.store_candidates.begin(), input.store_candidates.end());
    }
    std::map<std::string, const PackageCandidate*> winners;
    for (const auto& candidate : candidates) {
        if (!candidate.manifest.has_value()) continue;  // 清单坏的包进不了挂载账
        const auto [it, inserted] = winners.emplace(candidate.manifest->id, &candidate);
        (void)it;
        (void)inserted;  // 先到者(优先级高)已占住,后来的低层副本不盖
    }

    // 跨包全名引用的对账索引:全部候选都算"已存在包"(与 doctor 同口径),
    // 挂载只挑胜者,引用解析的视野不因遮蔽而缺页。
    const PackageRefIndex ref_index = BuildPackageRefIndex(candidates);

    for (const auto& [package_id, candidate] : winners) {
        (void)package_id;  // 键与 record.inventory.package_id 一致,值里取
        PackageRecord record =
            AnalyzePackage(*candidate, input.scan, ref_index, input.external, &input.trust);
        if (!record.valid || !record.mount_plan.has_value()) {
            continue;  // 整包成整包败:一件也不挂,诊断走 /package doctor
        }
        PackageMountEntry entry;
        entry.package_id = record.inventory.package_id;
        entry.version_text = record.inventory.version_text;
        entry.scope = record.inventory.scope;
        entry.package_root = record.inventory.package_root;
        entry.content_hash = record.inventory.content_hash;
        entry.code_trust = record.code_trust;
        for (const auto& plan_entry : record.mount_plan->entries) {
            // 清账只收真挂了的内容组件;plugin/mcp 走 code_trust 那一笔
            //(PendingTrust 一件不挂;Trusted 也只记账,挂载事务在阶段 5)。
            if (plan_entry.kind == ComponentKind::Plugin || plan_entry.kind == ComponentKind::McpServer) {
                continue;
            }
            entry.mounted_canonical_ids.push_back(plan_entry.canonical_id);
        }
        mount.entries.push_back(std::move(entry));
        mount.records.push_back(std::move(record));
    }
    return mount;
}

// ---------------------------------------------------------------------------
// 四张表的挂载材料
// ---------------------------------------------------------------------------

std::vector<tools::PackagedSkillRoot> MountSkillRoots(const PackageMount& mount) {
    std::vector<tools::PackagedSkillRoot> roots;
    for (const auto& record : mount.records) {
        tools::PackagedSkillRoot root;
        root.skills_dir = record.inventory.package_root / "skills";
        root.package_id = record.inventory.package_id;
        root.source_level = PackageSourceLevel(record.inventory.scope);
        roots.push_back(std::move(root));
    }
    return roots;  // 包里没有 skills/ 目录时 ScanSkillsDir 自然空手,不另判
}

std::vector<agent::PackagedAgentEntry> MountAgentEntries(const PackageMount& mount) {
    std::vector<agent::PackagedAgentEntry> entries;
    for (const auto& record : mount.records) {
        const OwnComponents own = CollectOwn(record);
        const std::map<std::string, std::string> unavailable = UnavailableForUntrustedCode(record);
        const std::string& package_id = record.inventory.package_id;
        for (const auto& component : record.components) {
            if (component.kind != ComponentKind::Agent || !component.agent.has_value()) continue;
            agent::PackagedAgentEntry out;
            out.canonical_name = component.canonical_id;
            out.package_id = package_id;
            out.definition = *component.agent;  // name 保持 local 人话
            if (out.definition.prompt.profile.has_value()) {
                out.definition.prompt.profile = CanonicalizeRef(
                    own, package_id, *out.definition.prompt.profile, ComponentKind::PromptProfile);
            }
            for (std::string& skill : out.definition.skills_preload) {
                skill = CanonicalizeRef(own, package_id, skill, ComponentKind::Skill);
            }
            // mcp_servers 不折:包内 MCP 待信任门,派发期的依赖校验照实报
            // 缺——登记不是放行。包未过门时,引它的 Agent 整件 unavailable
            //(缘由注明),Catalog 里看得见、用不了(阶段 4 连坐)。
            const auto blocked = unavailable.find(component.canonical_id);
            if (blocked != unavailable.end()) {
                out.available = false;
                out.unavailable_reason = blocked->second;
            }
            out.file_utf8 = PathToUtf8(record.inventory.package_root / Utf8ToPath(component.rel_path));
            entries.push_back(std::move(out));
        }
    }
    return entries;
}

std::vector<workflow::PackagedWorkflowSource> MountWorkflowSources(const PackageMount& mount) {
    std::vector<workflow::PackagedWorkflowSource> sources;
    for (const auto& record : mount.records) {
        const OwnComponents own = CollectOwn(record);
        const std::map<std::string, std::string> unavailable = UnavailableForUntrustedCode(record);
        const std::string& package_id = record.inventory.package_id;
        for (const auto& component : record.components) {
            if (component.kind != ComponentKind::Workflow || !component.workflow.has_value()) continue;
            workflow::PackagedWorkflowSource out;
            out.dir = record.inventory.package_root / Utf8ToPath(component.rel_path);
            out.canonical_id = component.canonical_id;
            out.package_id = package_id;
            out.definition = *component.workflow;
            out.definition.id = component.canonical_id;  // 挂载名 = canonical
            for (auto& node : out.definition.nodes) {
                if (!node.agent.empty()) {
                    node.agent = CanonicalizeRef(own, package_id, node.agent, ComponentKind::Agent);
                }
                if (!node.skill.empty()) {
                    node.skill = CanonicalizeRef(own, package_id, node.skill, ComponentKind::Skill);
                }
                if (!node.subflow_id.empty()) {
                    node.subflow_id =
                        CanonicalizeRef(own, package_id, node.subflow_id, ComponentKind::Workflow);
                }
                // task/prompt/template 是相对 workflow 目录的文件引用,原样:
                // CatalogEntry.dir 指包内目录,执行器照旧相对解析。
            }
            out.content_hash = workflow::ContentHash(*component.workflow);
            const auto blocked = unavailable.find(component.canonical_id);
            if (blocked != unavailable.end()) {
                out.available = false;
                out.unavailable_reason = blocked->second;
            }
            sources.push_back(std::move(out));
        }
    }
    return sources;
}

std::vector<agent::PackageProfileRoot> MountProfileRoots(const PackageMount& mount) {
    std::vector<agent::PackageProfileRoot> roots;
    for (const auto& record : mount.records) {
        agent::PackageProfileRoot root;
        root.package_id = record.inventory.package_id;
        root.profiles_dir_utf8 =
            PathToUtf8(record.inventory.package_root / "prompts" / "profiles");
        roots.push_back(std::move(root));
    }
    return roots;
}

}  // namespace lubancode::package
