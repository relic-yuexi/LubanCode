// 受约束动态图实现(自然语言编排单第 6 批)。

#include "workflow/planner.hpp"

#include <algorithm>
#include <set>
#include <sstream>
#include <utility>

#include "workflow/parser.hpp"

namespace lubancode::workflow {

namespace {

const char* kTemplatePrefix = "template:";

}  // namespace

PlannerAllowance ExtractAllowance(const WorkflowDefinition& def) {
    PlannerAllowance allowance;
    for (const auto& node : def.nodes) {
        if (node.label.rfind(kTemplatePrefix, 0) != 0) continue;
        WorkflowNode templ = node;
        templ.label = node.label.substr(std::char_traits<char>::length(kTemplatePrefix));
        if (templ.kind == NodeKind::Tool && !templ.tool.empty()) {
            allowance.tool_allowlist.push_back(templ.tool);
        }
        allowance.node_templates.push_back(std::move(templ));
    }
    return allowance;
}

std::string SerializeGraphPatch(const GraphPatch& patch) {
    nlohmann::json j = nlohmann::json::object();
    j["attach_after"] = patch.attach_after;
    nlohmann::json nodes = nlohmann::json::array();
    for (const auto& node : patch.append_nodes) {
        nlohmann::json n = nlohmann::json{{"id", node.id}, {"kind", ToString(node.kind)}};
        if (!node.tool.empty()) n["tool"] = node.tool;
        if (!node.operation.empty()) n["operation"] = node.operation;
        if (!node.input.empty()) n["input"] = node.input;
        nodes.push_back(std::move(n));
    }
    j["append_nodes"] = std::move(nodes);
    nlohmann::json edges = nlohmann::json::array();
    for (const auto& edge : patch.append_edges) {
        edges.push_back(nlohmann::json{{"from", edge.from}, {"outcome", edge.outcome}, {"to", edge.to}});
    }
    j["append_edges"] = std::move(edges);
    return j.dump();
}

std::optional<GraphPatch> ParseGraphPatch(const std::string& text) {
    nlohmann::json j;
    try {
        j = nlohmann::json::parse(text);
    } catch (...) {
        return std::nullopt;
    }
    if (!j.is_object()) return std::nullopt;
    GraphPatch patch;
    if (const auto it = j.find("attach_after"); it != j.end() && it->is_string()) {
        patch.attach_after = it->get<std::string>();
    }
    if (const auto it = j.find("append_nodes"); it != j.end() && it->is_array()) {
        for (const auto& raw : *it) {
            if (!raw.is_object()) continue;
            WorkflowNode node;
            if (const auto id = raw.find("id"); id != raw.end() && id->is_string()) {
                node.id = id->get<std::string>();
            } else {
                return std::nullopt;
            }
            std::string kind = "tool";
            if (const auto k = raw.find("kind"); k != raw.end() && k->is_string()) kind = k->get<std::string>();
            if (!ParseNodeKind(kind, node.kind)) return std::nullopt;
            if (const auto t = raw.find("tool"); t != raw.end() && t->is_string()) node.tool = t->get<std::string>();
            if (const auto o = raw.find("operation"); o != raw.end() && o->is_string()) {
                node.operation = o->get<std::string>();
            }
            if (const auto i = raw.find("input"); i != raw.end()) node.input = *i;
            patch.append_nodes.push_back(std::move(node));
        }
    }
    if (const auto it = j.find("append_edges"); it != j.end() && it->is_array()) {
        for (const auto& raw : *it) {
            if (!raw.is_object()) continue;
            WorkflowEdge edge;
            if (const auto f = raw.find("from"); f != raw.end() && f->is_string()) edge.from = f->get<std::string>();
            if (const auto t = raw.find("to"); t != raw.end() && t->is_string()) edge.to = t->get<std::string>();
            if (const auto o = raw.find("outcome"); o != raw.end() && o->is_string()) {
                edge.outcome = o->get<std::string>();
            } else {
                edge.outcome = "success";
            }
            if (edge.from.empty() || edge.to.empty()) return std::nullopt;
            patch.append_edges.push_back(std::move(edge));
        }
    }
    return patch;
}

std::expected<WorkflowDefinition, PatchRejection> ApplyGraphPatch(
    const WorkflowDefinition& def, const GraphPatch& patch, const PlannerAllowance& allowance,
    const std::optional<CapabilityTable>& capabilities) {
    PatchRejection reject;

    // attach_after 必须是既有节点(planner 不得修改已成功节点,只能在其
    // 后添后继)。
    if (patch.attach_after.empty() || def.node_map.count(patch.attach_after) == 0) {
        reject.code = "patch_unknown_attach";
        reject.message = "attach_after 必须指向既有节点: " + patch.attach_after;
        return std::unexpected(reject);
    }
    if (static_cast<int>(patch.append_nodes.size()) > std::max(1, allowance.max_patch_nodes)) {
        reject.code = "patch_too_large";
        reject.message = "一份 patch 至多添 " + std::to_string(allowance.max_patch_nodes) + " 只节点";
        return std::unexpected(reject);
    }
    if (allowance.node_templates.empty()) {
        reject.code = "patch_no_allowance";
        reject.message = "定义未授权任何节点模板(label 前缀 template:),planner 关死";
        return std::unexpected(reject);
    }

    // 节点 id 不冲突;每只节点都要在模板集里(工具再过 allowlist)。
    std::set<std::string> existing_ids;
    for (const auto& node : def.nodes) existing_ids.insert(node.id);
    for (const auto& node : patch.append_nodes) {
        if (node.id.empty() || existing_ids.count(node.id) > 0) {
            reject.code = "patch_id_conflict";
            reject.message = "patch 节点 id 冲突或为空: " + node.id;
            return std::unexpected(reject);
        }
        bool matched = false;
        for (const auto& templ : allowance.node_templates) {
            if (templ.kind != node.kind) continue;
            if (node.kind == NodeKind::Tool) {
                if (templ.tool != node.tool) continue;
                if (std::find(allowance.tool_allowlist.begin(), allowance.tool_allowlist.end(), node.tool) ==
                    allowance.tool_allowlist.end()) {
                    continue;
                }
            }
            if (node.kind == NodeKind::Transform && templ.operation != node.operation) continue;
            matched = true;
            break;
        }
        if (!matched) {
            reject.code = "patch_forbidden_node";
            reject.message = "节点不在授权模板集里: " + node.id + " (" + ToString(node.kind) +
                             (node.tool.empty() ? "" : " " + node.tool) + ")";
            return std::unexpected(reject);
        }
    }

    // 边的两端必须认识(既有或新添)。
    std::set<std::string> known = existing_ids;
    for (const auto& node : patch.append_nodes) known.insert(node.id);
    for (const auto& edge : patch.append_edges) {
        if (!known.count(edge.from) || !known.count(edge.to)) {
            reject.code = "patch_dangling_edge";
            reject.message = "patch 边指向未知节点: " + edge.from + " -> " + edge.to;
            return std::unexpected(reject);
        }
    }

    // 应用(append-only 节点;attach 节点的旧出边按需收窄)。
    WorkflowDefinition patched = def;
    for (const auto& node : patch.append_nodes) {
        patched.nodes.push_back(node);
        patched.node_map.emplace(node.id, node);
    }
    for (const auto& edge : patch.append_edges) {
        patched.edges.push_back(edge);
    }
    // 收窄(单子:planner 只可添后继、收窄未跑分支):patch 边从
    // attach_after 出发且带 outcome 时,attach 节点同 outcome 的旧边移除,
    // 旧终点改由 patch 链末梢接续(与 patch 自己写的末梢边重复则不重复
    // 接)。不这么收,同 outcome 两条边就是歧义边。
    if (!patch.append_nodes.empty()) {
        const std::string last_patch_node = patch.append_nodes.back().id;
        std::set<std::string> rerouted;
        for (const auto& edge : patch.append_edges) {
            if (edge.from == patch.attach_after) rerouted.insert(edge.outcome);
        }
        std::set<std::string> patch_edge_keys;
        for (const auto& edge : patch.append_edges) {
            patch_edge_keys.insert(edge.from + "|" + edge.outcome + "|" + edge.to);
        }
        std::vector<WorkflowEdge> kept_edges;
        for (const auto& edge : patched.edges) {
            const bool is_patch_edge =
                patch_edge_keys.count(edge.from + "|" + edge.outcome + "|" + edge.to) > 0;
            if (!is_patch_edge && edge.from == patch.attach_after && rerouted.count(edge.outcome) > 0) {
                const WorkflowEdge reroute{last_patch_node, edge.outcome, edge.to};
                if (patch_edge_keys.count(reroute.from + "|" + reroute.outcome + "|" + reroute.to) > 0) {
                    continue;  // patch 已写了这条,不重复
                }
                kept_edges.push_back(reroute);  // 旧终点改从 patch 链末梢接(保可达)
                continue;
            }
            kept_edges.push_back(edge);
        }
        patched.edges = std::move(kept_edges);
    }
    patched.normalized = BuildNormalizedJson(patched);

    // 全套校验(含能力对账)。
    const ValidationResult result = ValidateDefinition(patched, capabilities);
    if (!result.ok()) {
        reject.code = "patch_validation_failed";
        std::ostringstream msg;
        msg << "补丁应用后校验不过: ";
        for (std::size_t i = 0; i < result.issues.size() && i < 3; ++i) {
            msg << result.issues[i].path << " " << result.issues[i].message << (i + 1 < result.issues.size() ? "; " : "");
        }
        reject.message = msg.str();
        return std::unexpected(reject);
    }
    return patched;
}

ValidationResult ValidatePatchedDefinition(const WorkflowDefinition& patched, const WorkflowDefinition& original) {
    ValidationResult result;
    const auto add = [&](const char* code, const std::string& path, const std::string& message) {
        result.issues.push_back(ValidationIssue{code, path, message});
    };
    // 节点数帽(单子:查 max_nodes/max_depth/max_iterations)。
    if (static_cast<int>(patched.nodes.size()) > patched.limits.max_nodes) {
        add("max_nodes", "limits.max_nodes",
            "补丁后节点数 " + std::to_string(patched.nodes.size()) + " 越过 max_nodes " +
                std::to_string(patched.limits.max_nodes));
    }
    // 既有节点不许被改(逐字段比对 id 相同的节点)。
    for (const auto& [id, old_node] : original.node_map) {
        const auto it = patched.node_map.find(id);
        if (it == patched.node_map.end()) {
            add("patch_removed_node", "nodes." + id, "补丁删了既有节点(只许 append)");
            continue;
        }
        if (!(it->second == old_node)) {
            add("patch_modified_node", "nodes." + id, "补丁改了既有节点(只许 append)");
        }
    }
    return result;
}

PatchRisk AssessPatchRisk(const WorkflowDefinition& patched, const WorkflowDefinition& original) {
    PatchRisk risk;
    std::set<NodeKind> original_kinds;
    for (const auto& node : original.nodes) original_kinds.insert(node.kind);
    for (const auto& [id, node] : patched.node_map) {
        if (original.node_map.count(id) > 0) continue;  // 新添的才看
        if (node.has_side_effects) {
            risk.adds_side_effects = true;
            risk.reasons.push_back("添了副作用节点: " + id);
        }
        if (!node.prompt.empty() && !IsSafePackageRelative(node.prompt)) {
            risk.expands_paths = true;
            risk.reasons.push_back("prompt 引用越出包目录: " + id);
        }
        if (original_kinds.count(node.kind) == 0) {
            risk.changes_scope = true;
            risk.reasons.push_back("节点种类超出原图: " + ToString(node.kind));
        }
        if (node.kind == NodeKind::Approval || node.kind == NodeKind::AskUser) {
            risk.changes_scope = true;
            risk.reasons.push_back("添了交互节点: " + id);
        }
    }
    return risk;
}

}  // namespace lubancode::workflow
