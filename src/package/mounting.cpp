// 会话钉快照与内容组件挂载的实现(统一 Package 封装单阶段 3;阶段 6 增
// 启停门、显式 PackageSnapshot 与原子 reload)。
#include "package/mounting.hpp"

#include <algorithm>
#include <chrono>
#include <exception>
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
    // 清单坏(缺 package.yaml/解析失败)的包也进"一件不挂"账:按目录名
    // 报账,被胜者遮住的低层坏副本不算(rejected 只记"这只包当前起不来")。
    for (const auto& candidate : candidates) {
        if (candidate.manifest.has_value()) continue;
        if (winners.count(candidate.dir_name) > 0) continue;  // 同 id 有好副本在,坏副本是遮蔽账
        if (std::find(mount.rejected_ids.begin(), mount.rejected_ids.end(), candidate.dir_name) ==
            mount.rejected_ids.end()) {
            mount.rejected_ids.push_back(candidate.dir_name);
        }
    }

    // 跨包全名引用的对账索引:全部候选都算"已存在包"(与 doctor 同口径),
    // 挂载只挑胜者,引用解析的视野不因遮蔽而缺页。
    const PackageRefIndex ref_index = BuildPackageRefIndex(candidates);

    for (const auto& [package_id, candidate] : winners) {
        (void)package_id;  // 键与 record.inventory.package_id 一致,值里取
        // 启停门(阶段 6):停用的包挂载一律跳过——连内容组件一件不挂。
        // 扫描发现照旧(list/doctor 可见,list 标 disabled),这里只管不挂。
        if (!input.state.IsEnabled(package_id)) {
            mount.disabled_skipped_ids.push_back(package_id);
            continue;
        }
        PackageRecord record =
            AnalyzePackage(*candidate, input.scan, ref_index, input.external, &input.trust);
        if (!record.valid || !record.mount_plan.has_value()) {
            mount.rejected_ids.push_back(record.inventory.package_id);
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
            // 清账只收真挂了的内容组件;plugin/mcp/channel 走 code_trust 那
            // 一笔(PendingTrust 一件不挂;Trusted 也只记账,channel 的挂载
            // 事务在阶段 2 起的 ChannelManager,这里不提前算作"已挂")。
            if (plan_entry.kind == ComponentKind::Plugin || plan_entry.kind == ComponentKind::McpServer ||
                plan_entry.kind == ComponentKind::Channel) {
                continue;
            }
            entry.mounted_canonical_ids.push_back(plan_entry.canonical_id);
        }
        mount.entries.push_back(std::move(entry));
        mount.records.push_back(std::move(record));
    }

    // Channel 对外渠道 id(channel.yaml:id,不是 local id/canonical id)全局
    // 唯一(channel-manifest.md §3.1/§5:"同一时刻只许一份已挂载实现占用
    // 同一渠道 id;冲突按 Package 原子挂载失败,不临时挑一份")。这条对账
    // 只能在全部包都分析完、跨包视野齐了才做——单包分析(AnalyzePackage)
    // 看不见别的包。"不临时挑一份"读作:谁也不许悄悄当赢家——冲突涉及
    // 的全部包一并退回 rejected_ids(哪怕是同一个包里两份 channel.yaml
    // 撞了同一个对外 id,那个包自己也算冲突方),不是"先到者留、后到者
    // 让"的仲裁。诊断落在各自 rejected_ids 账上,详情走 /package doctor。
    {
        std::map<std::string, std::set<std::string>> channel_id_owners;  // channel.yaml:id -> 涉及的 package_id 集
        for (const auto& record : mount.records) {
            for (const auto& component : record.components) {
                if (component.kind != ComponentKind::Channel || !component.channel.has_value()) continue;
                const std::string& external_id = component.channel->id;
                if (external_id.empty()) continue;  // 静态解析已在 AnalyzePackage 报过,这里不重复
                channel_id_owners[external_id].insert(record.inventory.package_id);
            }
        }
        std::set<std::string> conflicted;
        for (const auto& [external_id, owners] : channel_id_owners) {
            (void)external_id;
            if (owners.size() > 1) conflicted.insert(owners.begin(), owners.end());
        }
        // 同一个包里两份 channel.yaml 撞了同一个对外 id(owners 集合只会有
        // 一个 package_id,插入两次也只留一份)也要挡——单独再扫一遍逐
        // component 计数,owners.size()==1 但同 id 出现 >1 次的场景。
        {
            std::map<std::string, std::map<std::string, int>> per_package_id_counts;  // package_id -> external_id -> 次数
            for (const auto& record : mount.records) {
                for (const auto& component : record.components) {
                    if (component.kind != ComponentKind::Channel || !component.channel.has_value()) continue;
                    if (component.channel->id.empty()) continue;
                    ++per_package_id_counts[record.inventory.package_id][component.channel->id];
                }
            }
            for (const auto& [package_id, counts] : per_package_id_counts) {
                for (const auto& [external_id, count] : counts) {
                    (void)external_id;
                    if (count > 1) conflicted.insert(package_id);
                }
            }
        }
        if (!conflicted.empty()) {
            mount.entries.erase(
                std::remove_if(mount.entries.begin(), mount.entries.end(),
                               [&](const PackageMountEntry& entry) {
                                   return conflicted.count(entry.package_id) > 0;
                               }),
                mount.entries.end());
            mount.records.erase(
                std::remove_if(mount.records.begin(), mount.records.end(),
                               [&](const PackageRecord& record) {
                                   return conflicted.count(record.inventory.package_id) > 0;
                               }),
                mount.records.end());
            for (const std::string& package_id : conflicted) {
                mount.rejected_ids.push_back(package_id);
            }
        }
    }

    // 一件不挂的账按 id 字节序排稳(清单坏的在前面按扫描序攒的,这里统一)。
    std::sort(mount.rejected_ids.begin(), mount.rejected_ids.end());
    return mount;
}

// ---------------------------------------------------------------------------
// PackageSnapshot 与原子 reload(阶段 6)
// ---------------------------------------------------------------------------

std::optional<std::string> PackageSnapshot::SkillBody(const std::string& canonical_id) const {
    const auto it = skill_bodies.find(canonical_id);
    if (it == skill_bodies.end()) {
        return std::nullopt;
    }
    return it->second;
}

std::vector<std::string> PackageSnapshot::SkillCanonicalIds() const {
    std::vector<std::string> ids;
    ids.reserve(skill_bodies.size());
    for (const auto& [id, body] : skill_bodies) {
        (void)body;
        ids.push_back(id);
    }
    return ids;  // map 按字节序,稳
}

std::shared_ptr<const PackageSnapshot> BuildPackageSnapshot(const PackageMountInput& input, int generation) {
    auto snapshot = std::make_shared<PackageSnapshot>();
    snapshot->mount_ = BuildPackageMount(input);
    snapshot->pinned_trust = input.trust;
    snapshot->state = input.state;
    snapshot->generation = generation;
    snapshot->built_at_unix = std::to_string(
        std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch())
            .count());
    // 包内技能正文摊平成查表:records 里组件 parser 已把 SKILL.md 正文读
    // 进内存,在跑引用经快照取,不回盘(盘中删改不影响钉住这份的会话)。
    for (const auto& record : snapshot->mount_.records) {
        for (const auto& component : record.components) {
            if (component.kind == ComponentKind::Skill && component.skill.has_value()) {
                snapshot->skill_bodies.emplace(component.canonical_id, component.skill->body);
            }
        }
    }
    return snapshot;
}

// 与旧快照对账的一行账:新增 / 移除 / 内容已变(哈希变,新装配用新内容)。
namespace {
std::vector<std::string> DiffSnapshots(const PackageSnapshot& old_snapshot,
                                       const PackageSnapshot& new_snapshot) {
    std::vector<std::string> lines;
    std::map<std::string, const PackageMountEntry*> before, after;
    for (const auto& entry : old_snapshot.mount().entries) before.emplace(entry.package_id, &entry);
    for (const auto& entry : new_snapshot.mount().entries) after.emplace(entry.package_id, &entry);
    std::vector<std::string> added, removed, changed;
    for (const auto& [id, entry] : after) {
        const auto it = before.find(id);
        if (it == before.end()) {
            added.push_back(id);
        } else if (it->second->content_hash != entry->content_hash) {
            changed.push_back(id);  // 同 id 换了内容(改文件/换版本/被高层遮)
        }
    }
    for (const auto& [id, entry] : before) {
        (void)entry;
        if (after.count(id) == 0) removed.push_back(id);
    }
    auto join = [](const std::vector<std::string>& ids) {
        std::string out;
        for (const auto& id : ids) {
            if (!out.empty()) out += ", ";
            out += id;
        }
        return out;
    };
    if (!added.empty()) lines.push_back("新增 " + std::to_string(added.size()) + " 包: " + join(added));
    if (!removed.empty()) {
        lines.push_back("移除 " + std::to_string(removed.size()) + " 包: " + join(removed) +
                        "(目录没了或已停用)");
    }
    if (!changed.empty()) {
        lines.push_back("内容已变 " + std::to_string(changed.size()) + " 包: " + join(changed) +
                        "(新装配用新内容)");
    }
    if (added.empty() && removed.empty() && changed.empty()) {
        lines.push_back("与上一折逐包一致(内容哈希均未变)。");
    }
    return lines;
}
}  // namespace

PackageReloadReport ReloadPackageSnapshot(const std::shared_ptr<const PackageSnapshot>& current,
                                          const PackageMountInput& fresh_input) {
    PackageReloadReport report;
    const int generation = (current != nullptr ? current->generation : 0) + 1;
    // 信任账钉 current 那份(会话启动定终身的门禁);没有 current(异常
    // 场景)才用输入自带的那份。
    PackageMountInput input = fresh_input;
    if (current != nullptr) {
        input.trust = current->pinned_trust;
    }
    // 折好才换:全部折算落在崭新对象上;中途任何错兜住,旧快照一分不动。
    std::shared_ptr<const PackageSnapshot> folded;
    try {
        folded = BuildPackageSnapshot(input, generation);
    } catch (const std::exception& e) {
        report.error = std::string("重折失败,旧快照未动: ") + e.what();
        return report;
    } catch (...) {
        report.error = "重折失败(未知异常),旧快照未动。";
        return report;
    }
    report.ok = true;
    report.snapshot = std::move(folded);

    std::size_t content_count = 0;
    for (const auto& entry : report.snapshot->mount().entries) {
        content_count += entry.mounted_canonical_ids.size();
    }
    report.lines.push_back("已重折 Package 快照(第 " + std::to_string(generation) +
                           " 折):挂载 " + std::to_string(report.snapshot->mount().entries.size()) + " 包," +
                           std::to_string(content_count) + " 件内容组件。");
    if (current != nullptr) {
        for (const std::string& line : DiffSnapshots(*current, *report.snapshot)) {
            report.lines.push_back(line);
        }
    }
    if (!report.snapshot->mount().rejected_ids.empty()) {
        std::string joined;
        for (const auto& id : report.snapshot->mount().rejected_ids) {
            if (!joined.empty()) joined += ", ";
            joined += id;
        }
        report.lines.push_back("invalid 包 " + std::to_string(report.snapshot->mount().rejected_ids.size()) +
                               " 件,一件未挂: " + joined + "(诊断: /package doctor <id>)");
    }
    if (!report.snapshot->mount().disabled_skipped_ids.empty()) {
        std::string joined;
        for (const auto& id : report.snapshot->mount().disabled_skipped_ids) {
            if (!joined.empty()) joined += ", ";
            joined += id;
        }
        report.lines.push_back("停用跳过 " +
                               std::to_string(report.snapshot->mount().disabled_skipped_ids.size()) +
                               " 包: " + joined);
    }
    report.lines.push_back("content 组件(agent/skill/workflow/prompt)对下一次装配生效;code 组件"
                           "(Plugin/MCP)只在会话启动挂载——须新会话,本会话不热插、不卸载。");
    return report;
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
