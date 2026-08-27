// 图渲染实现(自然语言编排单第 1 批)。纯函数,不碰盘。

#include "workflow/graph_view.hpp"

#include <functional>
#include <map>
#include <ostream>
#include <set>
#include <sstream>
#include <string>
#include <vector>

namespace lubancode::workflow {

namespace {

struct GraphShape {
    std::map<std::string, std::map<std::string, std::vector<std::string>>> by_outcome;
};

GraphShape BuildShape(const WorkflowDefinition& def) {
    GraphShape shape;
    for (const auto& node : def.nodes) {
        shape.by_outcome.emplace(node.id, std::map<std::string, std::vector<std::string>>{});
    }
    for (const auto& edge : def.edges) {
        if (!shape.by_outcome.count(edge.from)) continue;
        shape.by_outcome[edge.from][edge.outcome].push_back(edge.to);
    }
    // 隐含控制流:parallel/join 的 branches、map/foreach 的 body、reduce 的
    // reduce_body、switch 的条件分支与 default、fallback——普通边之外,
    // 图的"形状"还包括这些;渲染要看得见(可达性检查在 validator 另算)。
    const auto implicit = [&](const std::string& from, const std::string& to, const char* outcome) {
        if (to.empty() || !shape.by_outcome.count(from) || !shape.by_outcome.count(to)) return;
        shape.by_outcome[from][outcome].push_back(to);
    };
    for (const auto& node : def.nodes) {
        for (const auto& b : node.branches) implicit(node.id, b, "branch");
        if (!node.async_body.empty()) implicit(node.id, node.async_body, "await");
        if (!node.map_body.empty()) implicit(node.id, node.map_body, "each");
        if (!node.reduce_body.empty()) implicit(node.id, node.reduce_body, "reduce");
        if (!node.loop_body.empty()) {
            implicit(node.id, node.loop_body.front(), "iterate");
            for (std::size_t i = 1; i < node.loop_body.size(); ++i) {
                implicit(node.loop_body[i - 1], node.loop_body[i], "then");
            }
        }
        for (const auto& c : node.conditions) implicit(node.id, c.to, "case");
        if (!node.default_to.empty()) implicit(node.id, node.default_to, "default");
        if (!node.fallback_to.empty()) implicit(node.id, node.fallback_to, "fallback");
    }
    return shape;
}

}  // namespace

std::string NodeSummaryLine(const WorkflowNode& node) {
    std::ostringstream out;
    const std::string label = node.label.empty() ? node.id : node.label;
    out << label << " [" << ToString(node.kind) << "]";
    switch (node.kind) {
        case NodeKind::Tool:
            out << " " << node.tool;
            break;
        case NodeKind::Agent:
            out << " role=" << node.role << " task=" << node.task;
            break;
        case NodeKind::Llm:
            out << " " << node.prompt;
            break;
        case NodeKind::Skill:
            out << " " << node.skill;
            break;
        case NodeKind::Transform:
            out << " " << node.operation;
            break;
        case NodeKind::Template:
            out << " " << node.template_path;
            break;
        case NodeKind::Subflow:
            out << " " << node.subflow_id << (node.subflow_version.empty() ? "" : "@" + node.subflow_version);
            break;
        case NodeKind::Async:
            out << " body=" << node.async_body;
            break;
        case NodeKind::Parallel:
        case NodeKind::Join:
            out << " " << ToString(node.join) << " 分支 " << node.branches.size() << " 路";
            break;
        case NodeKind::Map:
        case NodeKind::Foreach:
            out << " items=" << node.items_ref << " body=" << node.map_body;
            break;
        case NodeKind::Reduce:
            out << " body=" << node.reduce_body;
            break;
        case NodeKind::Switch:
            out << " " << node.conditions.size() << " 条分支";
            break;
        case NodeKind::Loop:
            out << " body=" << node.loop_body.size() << " hard_limit=" << node.loop_hard_limit;
            break;
        default:
            break;
    }
    if (node.on_unavailable == OnUnavailable::Skip) out << " · 缺则跳过";
    if (node.retry.has_value() && node.retry->attempts > 1) out << " · 重试 " << node.retry->attempts;
    return out.str();
}

std::string RenderAsciiGraph(const WorkflowDefinition& def, std::size_t max_width) {
    (void)max_width;  // 首版不硬包宽:树形缩进本身有限,超宽交给终端折行
    const GraphShape shape = BuildShape(def);
    std::ostringstream out;
    std::set<std::string> drawn;

    // 深度优先画树;同一节点第二次出现画 "-> id(见上)" 不重展开。
    const std::function<void(const std::string&, const std::string&, bool)> draw =
        [&](const std::string& id, const std::string& prefix, bool last) {
            const auto node_it = def.node_map.find(id);
            if (node_it == def.node_map.end()) return;
            const WorkflowNode& node = node_it->second;
            out << prefix << (last ? "`- " : "|- ") << NodeSummaryLine(node) << "\n";
            if (!drawn.insert(id).second) {
                return;  // 已展开过,不递归
            }
            const std::string child_prefix = prefix + (last ? "   " : "|  ");
            const auto branch_it = shape.by_outcome.find(id);
            if (branch_it == shape.by_outcome.end()) return;
            for (const auto& [outcome, tos] : branch_it->second) {
                for (std::size_t i = 0; i < tos.size(); ++i) {
                    if (tos.size() > 1 || outcome != "success") {
                        out << child_prefix << "   (" << outcome << ")\n";
                    }
                    draw(tos[i], child_prefix + "   ", i + 1 == tos.size());
                }
            }
        };

    if (def.node_map.count(def.entry)) {
        const auto node_it = def.node_map.find(def.entry);
        out << NodeSummaryLine(node_it->second) << "\n";
        drawn.insert(def.entry);
        const auto branch_it = shape.by_outcome.find(def.entry);
        if (branch_it != shape.by_outcome.end()) {
            for (const auto& [outcome, tos] : branch_it->second) {
                for (std::size_t i = 0; i < tos.size(); ++i) {
                    if (tos.size() > 1 || outcome != "success") {
                        out << "  (" << outcome << ")\n";
                    }
                    draw(tos[i], "  ", i + 1 == tos.size());
                }
            }
        }
    } else {
        out << "(entry 缺失)\n";
    }
    return out.str();
}

std::string RenderMermaidGraph(const WorkflowDefinition& def) {
    std::ostringstream out;
    out << "flowchart TD\n";
    for (const auto& node : def.nodes) {
        const std::string label = node.label.empty() ? node.id : node.label;
        out << "  " << node.id << "[\"" << label << "<br/>" << ToString(node.kind) << "\"]\n";
    }
    const GraphShape shape = BuildShape(def);
    for (const auto& [from, outs] : shape.by_outcome) {
        for (const auto& [outcome, tos] : outs) {
            const std::string suffix = outcome == "success" ? "" : "|\"" + outcome + "\"|";
            for (const auto& to : tos) {
                out << "  " << from << " -->" << suffix << " " << to << "\n";
            }
        }
    }
    return out.str();
}

}  // namespace lubancode::workflow
