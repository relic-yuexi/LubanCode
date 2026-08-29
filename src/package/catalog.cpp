// 分析入口的实现(统一 Package 封装单阶段 2)。次序照契约 §八 前六步:
// 盘点(1-2,阶段 1) -> 逐件原生解析(3) -> 引用解析(4) -> wire 名检查(5)
// -> MountPlan(6)。任何一步出 Error,整包 invalid,plan 不出;但组件逐件
// 诊断与引用账照全给——"不因第一个错停摆"。
#include "package/catalog.hpp"

#include <algorithm>
#include <fstream>
#include <iterator>
#include <system_error>

#include "package/trust.hpp"  // ScopeRequiresTrust/PackageTrustSnapshot(第 7 步信任门)
#include "platform/paths.hpp"
#include "workflow/parser.hpp"  // IsSafePackageRelative(workflow 文件引用的越界口径)

namespace lubancode::package {

namespace {

using platform::PathToUtf8;
using platform::Utf8ToPath;

std::optional<std::string> ReadFileBytes(const std::filesystem::path& path) {
    std::error_code ec;
    if (!std::filesystem::is_regular_file(path, ec) || ec) return std::nullopt;
    std::ifstream file(path, std::ios::binary);
    if (!file.is_open()) return std::nullopt;
    return std::string((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
}

std::vector<std::string> DedupePreserveOrder(const std::vector<std::string>& items) {
    std::vector<std::string> out;
    std::set<std::string> seen;
    for (const auto& item : items) {
        if (seen.insert(item).second) out.push_back(item);
    }
    return out;
}

}  // namespace

// ---------------------------------------------------------------------------
// PackageRefIndex
// ---------------------------------------------------------------------------

bool PackageComponentSet::Has(ComponentKind kind, const std::string& local_id) const {
    switch (kind) {
        case ComponentKind::Agent: return agents.count(local_id) > 0;
        case ComponentKind::PromptProfile: return prompt_profiles.count(local_id) > 0;
        case ComponentKind::Skill: return skills.count(local_id) > 0;
        case ComponentKind::Workflow: return workflows.count(local_id) > 0;
        case ComponentKind::Plugin: return plugins.count(local_id) > 0;
        case ComponentKind::McpServer: return mcp_servers.count(local_id) > 0;
    }
    return false;
}

const PackageComponentSet* PackageRefIndex::Find(const std::string& package_id) const {
    const auto it = packages.find(package_id);
    return it == packages.end() ? nullptr : &it->second;
}

PackageRefIndex BuildPackageRefIndex(const std::vector<PackageCandidate>& candidates) {
    PackageRefIndex index;
    for (const auto& candidate : candidates) {
        if (!candidate.manifest.has_value()) continue;  // 清单坏的包进不了引用账
        PackageComponentSet set;
        set.package_id = candidate.manifest->id;
        set.package_root = candidate.package_root;
        for (const auto& component :
             ListPackageComponents(candidate.package_root, candidate.manifest->id)) {
            const std::string prefix = component.rel_path.substr(0, component.rel_path.find('/'));
            if (prefix == "agents") {
                set.agents.insert(component.local_id);
            } else if (prefix == "prompts") {
                set.prompt_profiles.insert(component.local_id);
            } else if (prefix == "skills") {
                set.skills.insert(component.local_id);
            } else if (prefix == "workflows") {
                set.workflows.insert(component.local_id);
            } else if (prefix == "plugins") {
                set.plugins.insert(component.local_id);
            } else if (prefix == "mcp") {
                set.mcp_servers.insert(component.local_id);
            }
        }
        // 同 id 多候选:ScanPackages 已按优先级从高到低排,先到的胜;后来
        // 的低层副本不盖账(与 list 的遮蔽口径一致)。
        index.packages.emplace(set.package_id, std::move(set));
    }
    return index;
}

// ---------------------------------------------------------------------------
// 引用解析
// ---------------------------------------------------------------------------

std::string ComponentRef::Format() const {
    std::string out = from + " `" + field + "`: " + raw;
    if (resolved) {
        out += " -> " + target;
        if (is_file_ref) return out + " (文件)";
        if (!in_package && !is_canonical) return out + " (包外既有名)";
        if (!in_package) return out + " (跨包)";
        return out;
    }
    out += " [悬空] " + message;
    return out;
}

namespace {

// 本包组件账(解析阶段攒的;坏的也在——引用指得到,账上就有名)。
struct OwnComponents {
    std::set<std::string> agents, prompt_profiles, skills, workflows, plugins, mcp_servers;

    bool Has(ComponentKind kind, const std::string& local_id) const {
        switch (kind) {
            case ComponentKind::Agent: return agents.count(local_id) > 0;
            case ComponentKind::PromptProfile: return prompt_profiles.count(local_id) > 0;
            case ComponentKind::Skill: return skills.count(local_id) > 0;
            case ComponentKind::Workflow: return workflows.count(local_id) > 0;
            case ComponentKind::Plugin: return plugins.count(local_id) > 0;
            case ComponentKind::McpServer: return mcp_servers.count(local_id) > 0;
        }
        return false;
    }

    void Add(ComponentKind kind, const std::string& local_id) {
        switch (kind) {
            case ComponentKind::Agent: agents.insert(local_id); break;
            case ComponentKind::PromptProfile: prompt_profiles.insert(local_id); break;
            case ComponentKind::Skill: skills.insert(local_id); break;
            case ComponentKind::Workflow: workflows.insert(local_id); break;
            case ComponentKind::Plugin: plugins.insert(local_id); break;
            case ComponentKind::McpServer: mcp_servers.insert(local_id); break;
        }
    }
};

// 解析时共用的一束材料。
struct ResolveCtx {
    std::string package_id;
    std::filesystem::path package_root;
    OwnComponents own;
    const PackageRefIndex* index = nullptr;
    const ExternalNamespaces* external = nullptr;
    std::vector<ComponentRef>* refs = nullptr;
    // 跨包 plugin 工具闭合要现读对方 plugin.json;读过的记一笔,别重复读。
    std::map<std::string, std::optional<runtime::PluginManifest>> cross_plugin_cache;

    const std::set<std::string>* ExternalSet(ComponentKind kind) const {
        switch (kind) {
            case ComponentKind::Skill: return &external->skills;
            case ComponentKind::Agent: return &external->agents;
            case ComponentKind::Workflow: return &external->workflows;
            case ComponentKind::McpServer: return &external->mcp_servers;
            default: return nullptr;  // Profile/Plugin 没有包外裸名账
        }
    }
};

const char* KindLabel(ComponentKind kind) {
    switch (kind) {
        case ComponentKind::Agent: return "agent";
        case ComponentKind::PromptProfile: return "prompt profile";
        case ComponentKind::Skill: return "skill";
        case ComponentKind::Workflow: return "workflow";
        case ComponentKind::Plugin: return "plugin";
        case ComponentKind::McpServer: return "mcp server";
    }
    return "?";
}

// 短名/全名组件引用的统一解析(单子 §七)。
ComponentRef ResolveName(ResolveCtx& ctx, const ParsedComponent& owner, const std::string& field,
                         const std::string& raw, ComponentKind kind) {
    ComponentRef ref;
    ref.from = owner.canonical_id;
    ref.from_kind = owner.kind;
    ref.from_local_id = owner.local_id;
    ref.field = field;
    ref.raw = raw;

    if (const std::size_t colon = raw.find(':'); colon != std::string::npos) {
        // canonical 全名:<包id>:<local名>。短名里不许冒号,见着就按全名走。
        ref.is_canonical = true;
        const std::string pkg = raw.substr(0, colon);
        const std::string local = raw.substr(colon + 1);
        if (!IsValidPackageId(pkg)) {
            ref.message = "全名的包 id 段不合规矩: " + pkg;
            return ref;
        }
        if (pkg == ctx.package_id) {
            if (ctx.own.Has(kind, local)) {
                ref.resolved = true;
                ref.in_package = true;
                ref.target = pkg + ":" + local;
            } else {
                ref.message = "全名指回本包,但 " + std::string(KindLabel(kind)) + " " + local + " 不在";
            }
            return ref;
        }
        const PackageComponentSet* target = ctx.index != nullptr ? ctx.index->Find(pkg) : nullptr;
        if (target == nullptr) {
            ref.message = "全名跨包引用须指向已存在包: " + pkg + " 不在扫描账里";
            return ref;
        }
        if (!target->Has(kind, local)) {
            ref.message = pkg + " 里没有 " + std::string(KindLabel(kind)) + " " + local;
            return ref;
        }
        ref.resolved = true;
        ref.in_package = false;
        ref.target = pkg + ":" + local;
        return ref;
    }

    // 短名:先本包,再包外既有名,都不中就悬空(不猜)。
    if (ctx.own.Has(kind, raw)) {
        ref.resolved = true;
        ref.in_package = true;
        ref.target = ctx.package_id + ":" + raw;
        return ref;
    }
    if (const std::set<std::string>* external = ctx.ExternalSet(kind);
        external != nullptr && external->count(raw) > 0) {
        ref.resolved = true;
        ref.in_package = false;
        ref.target = raw;
        return ref;
    }
    ref.message = "短名 " + raw + " 在本包" + KindLabel(kind) + "里没有;包外引用须写全名 <包id>:" +
                  raw;
    return ref;
}

// workflow 的包内文件引用(task/prompt/template):相对 workflow 目录解析,
// 只查存在(越界 parser 已拒)。
ComponentRef ResolveWorkflowFile(ResolveCtx& ctx, const ParsedComponent& owner,
                                 const std::string& field, const std::string& raw,
                                 const std::filesystem::path& workflow_dir) {
    ComponentRef ref;
    ref.from = owner.canonical_id;
    ref.from_kind = owner.kind;
    ref.from_local_id = owner.local_id;
    ref.field = field;
    ref.raw = raw;
    ref.is_file_ref = true;
    if (!workflow::IsSafePackageRelative(raw)) {
        ref.message = "越界或绝对的包内引用: " + raw;
        return ref;
    }
    std::error_code ec;
    const std::filesystem::path resolved = (workflow_dir / Utf8ToPath(raw)).lexically_normal();
    if (std::filesystem::is_regular_file(resolved, ec) && !ec) {
        ref.resolved = true;
        ref.in_package = true;
        const std::u8string rel_u8 = resolved.lexically_relative(ctx.package_root).generic_u8string();
        ref.target = std::string(reinterpret_cast<const char*>(rel_u8.data()), rel_u8.size());
    } else {
        ref.message = "文件不在: " + PathToUtf8(workflow_dir.lexically_relative(ctx.package_root)) +
                      "/" + raw;
    }
    return ref;
}

// 工具引用:plugin__<编码>__<tool> / mcp__<编码>__<tool>。编码段解出带点
// 说明是 packaged 组件(本包或跨包),无点是旧 standalone 名字空间。
ComponentRef ResolveToolName(ResolveCtx& ctx, const ParsedComponent& owner,
                             const std::string& field, const std::string& raw,
                             const std::vector<ParsedComponent>& components) {
    ComponentRef ref;
    ref.from = owner.canonical_id;
    ref.from_kind = owner.kind;
    ref.from_local_id = owner.local_id;
    ref.field = field;
    ref.raw = raw;

    const auto as_plain_name = [&]() -> ComponentRef {
        // 不是 packaged 前缀形状:按既有工具名兜底。注册表全表核对留到
        // 挂载期(阶段 3/5),这里不背内置工具清单。
        ref.resolved = true;
        ref.target = raw;
        ref.message = "按既有工具名记账(挂载期再对注册表)";
        return ref;
    };

    const std::size_t first_sep = raw.find("__");
    if (first_sep == std::string::npos) return as_plain_name();
    const std::string prefix = raw.substr(0, first_sep);
    if (prefix != "plugin" && prefix != "mcp") return as_plain_name();

    const std::string rest = raw.substr(first_sep + 2);
    const std::size_t sep = rest.find("__");
    if (sep == std::string::npos) return as_plain_name();  // 少一段,不是本格式
    const std::string idseg = rest.substr(0, sep);
    const std::string tool = rest.substr(sep + 2);
    const auto decoded = runtime::DecodeToolWireId(idseg);
    if (!decoded.has_value()) {
        ref.message = "wire 名的组件段不是合法 %HH 编码: " + idseg;
        return ref;
    }
    if (decoded->find('.') == std::string::npos) {
        ref.resolved = true;
        ref.target = raw;
        ref.message = "standalone 名字空间,不归本包检查";
        return ref;
    }

    // 带点:packaged 组件。先试本包(整段 = <包id>.<local>),再试跨包。
    const std::string own_segment = ctx.package_id + ".";
    if (decoded->rfind(own_segment, 0) == 0) {
        const std::string local = decoded->substr(own_segment.size());
        const ComponentKind kind = prefix == "plugin" ? ComponentKind::Plugin : ComponentKind::McpServer;
        if (!ctx.own.Has(kind, local)) {
            ref.message = "wire 名指回本包,但 " + std::string(KindLabel(kind)) + " " + local + " 不在";
            return ref;
        }
        if (kind == ComponentKind::McpServer) {
            ref.resolved = true;
            ref.in_package = true;
            ref.target = ctx.package_id + ":" + local;
            ref.message = "MCP 工具名要握手才知道,这里只查服务在不在";
            return ref;
        }
        // plugin:工具名对 manifest 静态闭合。
        for (const auto& component : components) {
            if (component.kind == ComponentKind::Plugin && component.local_id == local) {
                if (component.plugin.has_value()) {
                    for (const auto& def : component.plugin->tools) {
                        if (def.name == tool) {
                            ref.resolved = true;
                            ref.in_package = true;
                            ref.target = component.canonical_id + ":" + tool;
                            return ref;
                        }
                    }
                    ref.message = "本包 plugin " + local + " 没有工具 " + tool;
                    return ref;
                }
                ref.resolved = true;  // 组件本身坏了另有一条 error,这里不算悬空
                ref.in_package = true;
                ref.target = component.canonical_id + ":" + tool;
                ref.message = "plugin manifest 解析失败,工具闭合查不了(另有 error)";
                return ref;
            }
        }
        ref.message = "内部账对不上: " + raw;
        return ref;
    }

    // 跨包:按包 id 前缀拆(包 id 自带点,先长后短)。
    std::vector<std::string> package_ids;
    if (ctx.index != nullptr) {
        for (const auto& [id, set] : ctx.index->packages) {
            if (id != ctx.package_id) package_ids.push_back(id);
        }
    }
    std::sort(package_ids.begin(), package_ids.end(),
              [](const std::string& a, const std::string& b) { return a.size() > b.size(); });
    for (const std::string& pkg : package_ids) {
        const std::string seg = pkg + ".";
        if (decoded->rfind(seg, 0) != 0) continue;
        const std::string local = decoded->substr(seg.size());
        const PackageComponentSet* target = ctx.index->Find(pkg);
        const ComponentKind kind =
            prefix == "plugin" ? ComponentKind::Plugin : ComponentKind::McpServer;
        if (target == nullptr || !target->Has(kind, local)) {
            ref.message = pkg + " 里没有 " + std::string(KindLabel(kind)) + " " + local;
            return ref;
        }
        if (kind == ComponentKind::McpServer) {
            ref.resolved = true;
            ref.target = pkg + ":" + local;
            ref.message = "MCP 工具名要握手才知道,这里只查服务在不在";
            return ref;
        }
        // 跨包 plugin 工具:现读对方的 plugin.json 对账。
        const std::string key = pkg + "/" + local;
        if (!ctx.cross_plugin_cache.count(key)) {
            const std::filesystem::path plugin_dir =
                target->package_root / "plugins" / Utf8ToPath(local);
            const auto text = ReadFileBytes(plugin_dir / "plugin.json");
            std::optional<runtime::PluginManifest> manifest;
            if (text.has_value()) {
                auto parsed = runtime::ParsePluginManifest(*text, plugin_dir);
                if (parsed.has_value()) manifest = std::move(*parsed);
            }
            ctx.cross_plugin_cache[key] = std::move(manifest);
        }
        const auto& cached = ctx.cross_plugin_cache[key];
        if (!cached.has_value()) {
            ref.message = pkg + " 的 plugin " + local + " 的 plugin.json 读不动/解析不过";
            return ref;
        }
        for (const auto& def : cached->tools) {
            if (def.name == tool) {
                ref.resolved = true;
                ref.target = pkg + ":" + local + ":" + tool;
                return ref;
            }
        }
        ref.message = pkg + " 的 plugin " + local + " 没有工具 " + tool;
        return ref;
    }
    ref.message = "wire 名的组件段对不上任何已知包: " + *decoded;
    return ref;
}

// 一件组件的全部引用逐条解,账进 refs。
void ResolveComponentRefs(ResolveCtx& ctx, ParsedComponent& component,
                          const std::vector<ParsedComponent>& components) {
    const auto record = [&](ComponentRef ref) {
        ctx.refs->push_back(std::move(ref));
    };

    if (component.kind == ComponentKind::Agent && component.agent.has_value()) {
        const auto& agent = *component.agent;
        if (agent.prompt.profile.has_value() && !agent.prompt.profile->empty() &&
            *agent.prompt.profile != "default") {
            record(ResolveName(ctx, component, "prompt.profile", *agent.prompt.profile,
                               ComponentKind::PromptProfile));
        }
        for (std::size_t i = 0; i < agent.skills_preload.size(); ++i) {
            record(ResolveName(ctx, component, "skills.preload[" + std::to_string(i) + "]",
                               agent.skills_preload[i], ComponentKind::Skill));
        }
        for (std::size_t i = 0; i < agent.mcp_servers.size(); ++i) {
            record(ResolveName(ctx, component, "mcp_servers[" + std::to_string(i) + "]",
                               agent.mcp_servers[i], ComponentKind::McpServer));
        }
        const auto tool_list = [&](const char* field, const std::vector<std::string>& names) {
            for (std::size_t i = 0; i < names.size(); ++i) {
                record(ResolveToolName(ctx, component,
                                       std::string(field) + "[" + std::to_string(i) + "]",
                                       names[i], components));
            }
        };
        tool_list("tools.allow", agent.tools.allow);
        tool_list("tools.deny", agent.tools.deny);
        tool_list("requires.tools", agent.requires_tools);
        return;
    }

    if (component.kind == ComponentKind::Workflow && component.workflow.has_value()) {
        const auto& def = *component.workflow;
        const std::filesystem::path workflow_dir = ctx.package_root / Utf8ToPath(component.rel_path);
        for (const auto& node : def.nodes) {
            if (!node.agent.empty()) {
                record(ResolveName(ctx, component, "nodes." + node.id + ".agent", node.agent,
                                   ComponentKind::Agent));
            }
            if (!node.skill.empty()) {
                record(ResolveName(ctx, component, "nodes." + node.id + ".skill", node.skill,
                                   ComponentKind::Skill));
            }
            if (!node.subflow_id.empty()) {
                record(ResolveName(ctx, component, "nodes." + node.id + ".subflow", node.subflow_id,
                                   ComponentKind::Workflow));
            }
            if (!node.tool.empty()) {
                record(ResolveToolName(ctx, component, "nodes." + node.id + ".tool", node.tool,
                                       components));
            }
            for (std::size_t i = 0; i < node.allowed_tools.size(); ++i) {
                record(ResolveToolName(ctx, component,
                                       "nodes." + node.id + ".allowed_tools[" + std::to_string(i) + "]",
                                       node.allowed_tools[i], components));
            }
            if (!node.task.empty()) {
                record(ResolveWorkflowFile(ctx, component, "nodes." + node.id + ".task", node.task,
                                           workflow_dir));
            }
            if (!node.prompt.empty()) {
                record(ResolveWorkflowFile(ctx, component, "nodes." + node.id + ".prompt", node.prompt,
                                           workflow_dir));
            }
            if (!node.template_path.empty()) {
                record(ResolveWorkflowFile(ctx, component, "nodes." + node.id + ".template",
                                           node.template_path, workflow_dir));
            }
        }
        return;
    }
}

// wire 名长度帽(契约 §6.1:编码后的完整工具名不超 64,超了 doctor 报错)。
// MCP 工具名要握手才知道,按"最短工具名 1 字符"预算。
void CheckWireNameBudget(ParsedComponent& component) {
    if (component.kind == ComponentKind::Plugin && component.plugin.has_value()) {
        for (const auto& tool : component.plugin->tools) {
            const std::string wire = runtime::BuildPackagedToolWireName(
                "plugin", component.canonical_id.substr(0, component.canonical_id.find(':')),
                component.local_id, tool.name);
            if (wire.size() > runtime::kToolWireNameMaxLength) {
                component.issues.push_back(
                    ComponentIssue{true, component.rel_path + "/plugin.json", -1, -1,
                                   std::string("tools[") + tool.name + "]",
                                   "wire 名 " + wire + " 长 " + std::to_string(wire.size()) +
                                       ",超 64 字符帽(package id 太长,%2E 一枚吃三格)"});
            }
        }
    } else if (component.kind == ComponentKind::McpServer && component.mcp.has_value()) {
        const std::string pkg = component.canonical_id.substr(0, component.canonical_id.find(':'));
        const std::string prefix = runtime::BuildPackagedToolWireName("mcp", pkg, component.local_id, "x");
        if (prefix.size() + 1 > runtime::kToolWireNameMaxLength) {
            component.issues.push_back(ComponentIssue{
                true, component.rel_path + "/mcp.yaml", -1, -1, "(wire budget)",
                "MCP 工具名前缀 " + prefix + " 已长 " + std::to_string(prefix.size()) +
                    ",加工具名必超 64 字符帽"});
        }
    }
}

}  // namespace

// ---------------------------------------------------------------------------
// MountPlan
// ---------------------------------------------------------------------------

std::string MountTargetTable(ComponentKind kind) {
    switch (kind) {
        case ComponentKind::Agent: return "AgentCatalog source roots";
        case ComponentKind::PromptProfile: return "PromptProfileResolver roots";
        case ComponentKind::Skill: return "Skill loader source roots";
        case ComponentKind::Workflow: return "WorkflowCatalog source roots";
        case ComponentKind::Plugin: return "Plugin runtime(manifest 入口)";
        case ComponentKind::McpServer: return "MCP config sources(mcp.yaml)";
    }
    return "?";
}

std::size_t PackageMountPlan::CountKind(ComponentKind kind) const {
    std::size_t count = 0;
    for (const auto& entry : entries) {
        if (entry.kind == kind) ++count;
    }
    return count;
}

bool PackageMountPlan::HasCodeBearing() const {
    for (const auto& entry : entries) {
        if (entry.code_bearing) return true;
    }
    return false;
}

std::string_view CodeTrustStatusText(CodeTrustStatus status) {
    switch (status) {
        case CodeTrustStatus::NoCode: return "无 code 组件";
        case CodeTrustStatus::Trusted: return "已过信任门";
        case CodeTrustStatus::PendingTrust: return "待信任门";
    }
    return "?";
}

// ---------------------------------------------------------------------------
// AnalyzePackage
// ---------------------------------------------------------------------------

const ParsedComponent* PackageRecord::FindComponent(ComponentKind kind,
                                                    const std::string& local_id) const {
    for (const auto& component : components) {
        if (component.kind == kind && component.local_id == local_id) return &component;
    }
    return nullptr;
}

PackageRecord AnalyzePackage(const PackageCandidate& candidate, const ScanOptions& options,
                             const PackageRefIndex& ref_index, const ExternalNamespaces& external,
                             const PackageTrustSnapshot* trust) {
    PackageRecord record;
    record.inventory = BuildPackageInventory(candidate, options);

    // ---- 第 3 步:逐件调原生 parser(全件都跑,不因第一个错停) ----
    const struct {
        const std::vector<PackageComponent>* list;
        ComponentKind kind;
    } groups[] = {
        {&record.inventory.agents, ComponentKind::Agent},
        {&record.inventory.prompt_profiles, ComponentKind::PromptProfile},
        {&record.inventory.skills, ComponentKind::Skill},
        {&record.inventory.workflows, ComponentKind::Workflow},
        {&record.inventory.plugins, ComponentKind::Plugin},
        {&record.inventory.mcp_servers, ComponentKind::McpServer},
    };
    ResolveCtx ctx;
    ctx.package_id = record.inventory.package_id;
    ctx.package_root = record.inventory.package_root;
    ctx.index = &ref_index;
    ctx.external = &external;
    ctx.refs = &record.references;

    // 信任门(阶段 4)在解析前就算好:组件 parser 吃的 ComponentSourceRoot
    // 带 trusted_for_code,阶段 5 的挂载事务据此放行。口径与 plan 里的
    // code_trust 同一式:免审层放置即信任;外来层只认账上那枚哈希。
    const CodeTrustStatus code_trust =
        [&] {
            if (record.inventory.plugins.empty() && record.inventory.mcp_servers.empty()) {
                return CodeTrustStatus::NoCode;
            }
            if (!ScopeRequiresTrust(record.inventory.scope)) {
                return CodeTrustStatus::Trusted;
            }
            if (trust != nullptr &&
                trust->IsTrusted(record.inventory.package_id, record.inventory.content_hash)) {
                return CodeTrustStatus::Trusted;
            }
            return CodeTrustStatus::PendingTrust;
        }();

    for (const auto& group : groups) {
        for (const auto& item : *group.list) {
            ComponentSourceRoot source;
            source.package_root = record.inventory.package_root;
            source.component_path = record.inventory.package_root / platform::Utf8ToPath(item.rel_path);
            source.rel_path = item.rel_path;
            source.local_id = item.local_id;
            source.kind = group.kind;
            source.scope = record.inventory.scope;
            source.package_id = record.inventory.package_id;
            source.package_version = record.inventory.version_text;
            source.content_hash = record.inventory.content_hash;
            source.trusted_for_code = code_trust == CodeTrustStatus::Trusted;
            ctx.own.Add(group.kind, item.local_id);
            record.components.push_back(ParsePackageComponent(source));
        }
    }

    // ---- 第 5 步(先于引用):wire 名长度帽,超帽落成组件 error ----
    for (auto& component : record.components) {
        CheckWireNameBudget(component);
    }

    // ---- 第 4 步:引用解析(本包账已齐,逐件过) ----
    for (auto& component : record.components) {
        ResolveComponentRefs(ctx, component, record.components);
    }
    for (const auto& ref : record.references) {
        if (!ref.resolved) {
            for (auto& component : record.components) {
                if (component.kind == ref.from_kind && component.local_id == ref.from_local_id) {
                    component.issues.push_back(
                        ComponentIssue{true, component.rel_path, -1, -1, ref.field,
                                       "引用悬空: " + ref.raw + "(" + ref.message + ")"});
                    break;
                }
            }
        }
    }

    // ---- 整包成败 ----
    record.valid = record.inventory.valid;
    for (const auto& component : record.components) {
        if (component.HasError()) record.valid = false;
    }
    for (const auto& ref : record.references) {
        if (!ref.resolved) record.valid = false;
    }

    // ---- 第 6 步:MountPlan(valid 才产) ----
    if (record.valid) {
        PackageMountPlan plan;
        plan.package_id = record.inventory.package_id;
        plan.package_version = record.inventory.version_text;
        plan.content_hash = record.inventory.content_hash;
        for (const auto& component : record.components) {
            MountPlanEntry entry;
            entry.kind = component.kind;
            entry.local_id = component.local_id;
            entry.canonical_id = component.canonical_id;
            entry.rel_path = component.rel_path;
            entry.target_table = MountTargetTable(component.kind);
            entry.source_root = component.rel_path;
            entry.code_bearing =
                component.kind == ComponentKind::Plugin || component.kind == ComponentKind::McpServer;
            if (component.kind == ComponentKind::Plugin || component.kind == ComponentKind::McpServer) {
                entry.wire_component_id = runtime::EncodeToolWireId(
                    record.inventory.package_id + "." + component.local_id);
            }
            if (component.kind == ComponentKind::Plugin && component.plugin.has_value()) {
                for (const auto& tool : component.plugin->tools) {
                    MountPlanTool out;
                    out.short_name = tool.name;
                    out.wire_name = runtime::BuildPackagedToolWireName(
                        "plugin", record.inventory.package_id, component.local_id, tool.name);
                    out.display_name = runtime::BuildPackagedToolDisplayName(
                        "plugin", record.inventory.package_id, component.local_id, tool.name);
                    entry.tools.push_back(std::move(out));
                }
            }
            for (const auto& ref : record.references) {
                if (ref.from_kind != component.kind || ref.from_local_id != component.local_id ||
                    !ref.resolved || !ref.in_package || ref.is_file_ref) {
                    continue;
                }
                // depends_on 收组件 canonical id(<包id>:<local>)。工具型 target
                // 是 <canonical>:<tool>(两枚冒号),剥掉尾段落回组件。
                std::string dep = ref.target;
                if (std::count(dep.begin(), dep.end(), ':') == 2) {
                    dep = dep.substr(0, dep.rfind(':'));
                }
                entry.depends_on.push_back(std::move(dep));
            }
            entry.depends_on = DedupePreserveOrder(entry.depends_on);
            plan.entries.push_back(std::move(entry));
        }
        // ---- 第 7 步(阶段 4):信任门 ----
        // 与解析前算的同一式(免审层放置即信任;外来层只认账上那枚哈希
        // ——文件动一个字节,IsTrusted 即翻脸,code 件全数退回待信任)。
        plan.code_trust = code_trust;
        for (auto& entry : plan.entries) {
            if (entry.code_bearing) {
                entry.trusted = plan.code_trust == CodeTrustStatus::Trusted;
            }
        }
        record.code_trust = plan.code_trust;
        record.mount_plan = std::move(plan);
    }
    return record;
}

}  // namespace lubancode::package
