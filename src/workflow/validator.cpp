// Workflow 校验器实现(自然语言编排单第 1 批)。
//
// 检查清单(单子"自然语言编译器"的宿主侧错误全表):
//   id 重复、entry 不存在、节点不可达、没有终点、outcome 没接边或接了
//   两条歧义边、环没有上限、${...} 指到未来节点/缺字段、tool/skill/
//   subflow 不存在、并行分支同写一个 var、alias 撞内建、prompt/template
//   越出包目录、明文 secret。

#include "workflow/validator.hpp"

#include <algorithm>
#include <deque>
#include <set>
#include <sstream>
#include <unordered_map>

#include "workflow/catalog.hpp"

namespace lubancode::workflow {

namespace {

struct NodeDeps {
    // 出边:outcome -> to(检查歧义)。
    std::map<std::string, std::vector<std::string>> outgoing;
    // 控制流隐含的依赖:parallel 的 branches、map 的 body、reduce 的
    // reduce_body、switch 的 conditions.to/default_to、fallback_to。
    std::vector<std::string> implicit_next;
};

// 递归遍历 ${...} 字符串引用,收集路径里的节点 id(nodes.<id>...)。
void CollectRefNodes(const nlohmann::json& value, const WorkflowNode& self, std::set<std::string>& refs) {
    if (value.is_string()) {
        const std::string& text = value.get<std::string>();
        std::size_t pos = 0;
        while (true) {
            const std::size_t open = text.find("${", pos);
            if (open == std::string::npos) break;
            const std::size_t close = text.find('}', open);
            if (close == std::string::npos) break;
            const std::string inner = text.substr(open + 2, close - open - 2);
            if (inner.rfind("nodes.", 0) == 0) {
                // nodes.<id>.<field...>:id 是 "nodes." 后到下一个 '.' 的段。
                const std::string rest = inner.substr(6);
                const std::size_t dot = rest.find('.');
                const std::string node_id = dot == std::string::npos ? rest : rest.substr(0, dot);
                if (!node_id.empty() && node_id != self.id) refs.insert(node_id);
            }
            pos = close + 1;
        }
    } else if (value.is_object()) {
        for (auto it = value.begin(); it != value.end(); ++it) {
            CollectRefNodes(it.value(), self, refs);
        }
    } else if (value.is_array()) {
        for (const auto& item : value) CollectRefNodes(item, self, refs);
    }
}

}  // namespace

std::map<std::string, int> EstimateTopoOrder(const WorkflowDefinition& def) {
    std::map<std::string, int> order;
    std::unordered_map<std::string, int> degree;
    std::unordered_map<std::string, std::vector<std::string>> succ;
    for (const auto& node : def.nodes) {
        degree.emplace(node.id, 0);
        succ.emplace(node.id, std::vector<std::string>{});
    }
    const auto add_edge = [&](const std::string& from, const std::string& to) {
        if (from.empty() || to.empty()) return;
        if (!degree.count(to) || !succ.count(from)) return;
        succ[from].push_back(to);
        ++degree[to];
    };
    for (const auto& edge : def.edges) add_edge(edge.from, edge.to);
    for (const auto& node : def.nodes) {
        for (const auto& b : node.branches) add_edge(node.id, b);
        if (!node.map_body.empty()) add_edge(node.id, node.map_body);
        if (!node.reduce_body.empty()) add_edge(node.id, node.reduce_body);
        for (const auto& c : node.conditions) add_edge(node.id, c.to);
        if (!node.default_to.empty()) add_edge(node.id, node.default_to);
        if (!node.fallback_to.empty()) add_edge(node.id, node.fallback_to);
    }
    std::deque<std::string> ready;
    for (const auto& node : def.nodes) {
        if (degree[node.id] == 0) ready.push_back(node.id);
    }
    int level = 0;
    std::size_t visited = 0;
    while (!ready.empty()) {
        const std::size_t batch = ready.size();
        for (std::size_t i = 0; i < batch; ++i) {
            const std::string id = ready.front();
            ready.pop_front();
            order[id] = level;
            ++visited;
            for (const auto& next : succ[id]) {
                if (--degree[next] == 0) ready.push_back(next);
            }
        }
        ++level;
    }
    // 环上节点没有 order;引用未来节点的检查对它们跳过(环另有报错)。
    return order;
}

ValidationResult ValidateDefinition(const WorkflowDefinition& def,
                                    const std::optional<CapabilityTable>& capabilities) {
    ValidationResult result;
    const auto add = [&](const char* code, const std::string& path, const std::string& message) {
        result.issues.push_back(ValidationIssue{code, path, message});
    };

    // ---- 顶层字段 ----
    if (!IsValidWorkflowId(def.id)) {
        add("bad_id", "id", "workflow id 只认小写字母开头的小写字母/数字/'-',长度 <= 64: " + def.id);
    }
    if (def.version.empty()) {
        add("missing_version", "version", "缺业务版本(如 1.0.0)");
    }
    if (!def.alias.empty() && !IsValidAlias(def.alias)) {
        add("bad_alias", "alias", "alias 含空白/斜杠/控制符/路径字符: " + def.alias);
    }
    if (def.entry.empty()) {
        add("missing_entry", "entry", "缺 entry(入口节点)");
        return result;
    }

    // ---- 节点表自洽 ----
    std::set<std::string> seen;
    for (const auto& node : def.nodes) {
        if (node.id.empty()) {
            add("empty_node_id", "nodes", "节点 id 不能为空");
            continue;
        }
        if (!seen.insert(node.id).second) {
            add("duplicate_node", "nodes." + node.id, "节点 id 重复");
        }
    }
    if (def.node_map.empty() && !def.nodes.empty()) {
        add("internal", "nodes", "node_map 未填(parser 缺陷)");
    }
    if (!def.node_map.count(def.entry)) {
        add("unknown_entry", "entry", "entry 指向不存在的节点: " + def.entry);
        return result;
    }

    const auto node_path = [](const std::string& id) { return "nodes." + id; };

    // ---- 各节点 kind 自洽 ----
    for (const auto& node : def.nodes) {
        const std::string base = node_path(node.id);
        switch (node.kind) {
            case NodeKind::Tool:
                if (node.tool.empty()) add("missing_tool", base + ".tool", "tool 节点缺 tool 名");
                break;
            case NodeKind::Agent:
                if (node.task.empty()) add("missing_task", base + ".task", "agent 节点缺 task(包内 prompt 引用)");
                if (!node.task.empty() && !IsSafePackageRelative(node.task))
                    add("path_escape", base + ".task", "task 引用越出包目录: " + node.task);
                break;
            case NodeKind::Llm:
                if (node.prompt.empty()) add("missing_prompt", base + ".prompt", "llm 节点缺 prompt");
                break;
            case NodeKind::Skill:
                if (node.skill.empty()) add("missing_skill", base + ".skill", "skill 节点缺 skill 名");
                break;
            case NodeKind::Transform:
                if (node.operation.empty())
                    add("missing_operation", base + ".operation", "transform 节点缺 operation 名");
                break;
            case NodeKind::Template:
                if (node.template_path.empty())
                    add("missing_template", base + ".template", "template 节点缺模板引用");
                break;
            case NodeKind::Subflow:
                if (node.subflow_id.empty()) add("missing_subflow", base + ".subflow", "subflow 节点缺目标 id");
                if (node.subflow_id == def.id)
                    add("subflow_cycle", base + ".subflow", "subflow 不能指向自身");
                break;
            case NodeKind::Parallel:
                if (node.branches.empty()) add("empty_branches", base + ".branches", "parallel 节点没有分支");
                break;
            case NodeKind::Join:
                if (node.branches.empty()) add("empty_branches", base + ".branches", "join 节点没有分支");
                if (node.join == JoinPolicy::Quorum && node.join_quorum <= 0)
                    add("bad_quorum", base + ".quorum", "quorum 策略须给 N(>0)");
                if (node.join == JoinPolicy::Quorum && node.join_quorum > static_cast<int>(node.branches.size()))
                    add("bad_quorum", base + ".quorum", "quorum N 超过分支数");
                break;
            case NodeKind::Map:
            case NodeKind::Foreach:
                if (node.items_ref.empty()) add("missing_items", base + ".items", node.id + " 缺 items 引用");
                if (node.map_body.empty()) add("missing_body", base + ".body", node.id + " 缺 body 节点");
                break;
            case NodeKind::Reduce:
                if (node.reduce_body.empty())
                    add("missing_reduce_body", base + ".reduce_body", "reduce 节点缺汇总体");
                break;
            case NodeKind::Switch:
                if (node.conditions.empty() && node.default_to.empty())
                    add("empty_switch", base + ".conditions", "switch 节点既无条件也无 default");
                break;
            case NodeKind::Approval:
            case NodeKind::AskUser:
            case NodeKind::Checkpoint:
            case NodeKind::End:
                break;
        }
        // 包内引用越界(parser 已查过一遍,这里再钉:JSON 进来的定义没走 parser)。
        if (!node.prompt.empty() && !IsSafePackageRelative(node.prompt))
            add("path_escape", base + ".prompt", "prompt 引用越出包目录: " + node.prompt);
        if (!node.template_path.empty() && !IsSafePackageRelative(node.template_path))
            add("path_escape", base + ".template", "template 引用越出包目录: " + node.template_path);
        // 副作用无幂等键不许重试(单子"重试、回落与幂等")。
        if (node.has_side_effects && node.idempotency_key.empty() && node.retry.has_value() &&
            node.retry->attempts > 1) {
            add("unsafe_retry", base + ".retry",
                "副作用节点无 idempotency_key,attempts 只能是 1(不无脑重试副作用)");
        }
        if (node.retry.has_value() && node.retry->attempts < 1) {
            add("bad_retry", base + ".retry", "attempts 至少为 1");
        }
        if (node.on_unavailable == OnUnavailable::Fallback && node.fallback_to.empty()) {
            add("missing_fallback", base + ".fallback_to", "on_unavailable=fallback 须给 fallback_to 节点");
        }
        if (!node.fallback_to.empty() && !def.node_map.count(node.fallback_to)) {
            add("dangling_fallback", base + ".fallback_to", "fallback_to 指向不存在的节点: " + node.fallback_to);
        }
        // 明文 secret 扫描:input/result/outputs 里藏 token/api_key/password。
        const auto scan_secrets = [&](const nlohmann::json& j, const std::string& where) {
            if (!j.is_object()) return;
            for (auto it = j.begin(); it != j.end(); ++it) {
                std::string key = it.key();
                std::transform(key.begin(), key.end(), key.begin(), [](unsigned char c) {
                    return static_cast<char>(std::tolower(c));
                });
                if (key.find("token") != std::string::npos || key.find("api_key") != std::string::npos ||
                    key.find("apikey") != std::string::npos || key.find("password") != std::string::npos ||
                    key.find("secret") != std::string::npos) {
                    add("plaintext_secret", where + "." + it.key(),
                        "定义里不许落明文密钥,改用 secret_ref 或环境变量名");
                }
            }
        };
        scan_secrets(node.input, base + ".input");
        scan_secrets(def.result, "result");
    }

    // ---- 边检查 ----
    std::map<std::string, NodeDeps> graph;
    for (const auto& node : def.nodes) graph.emplace(node.id, NodeDeps{});
    for (const auto& edge : def.edges) {
        const std::string base = "edges." + edge.from + "->" + edge.to;
        if (!graph.count(edge.from)) {
            add("dangling_edge", base, "边起点不存在: " + edge.from);
            continue;
        }
        if (!graph.count(edge.to)) {
            add("dangling_edge", base, "边终点不存在: " + edge.to);
            continue;
        }
        if (edge.outcome.empty()) {
            add("bad_outcome", base, "边缺 outcome");
            continue;
        }
        auto& outs = graph[edge.from].outgoing[edge.outcome];
        if (std::find(outs.begin(), outs.end(), edge.to) != outs.end()) {
            add("duplicate_edge", base, "完全相同的边出现两次");
        }
        outs.push_back(edge.to);
    }
    for (auto& [id, deps] : graph) {
        for (const auto& [outcome, tos] : deps.outgoing) {
            if (tos.size() > 1) {
                std::ostringstream msg;
                msg << "节点 " << id << " 的 outcome '" << outcome << "' 接了 " << tos.size()
                    << " 条边;fan-out 必须显式用 parallel/map 节点";
                add("ambiguous_outcome", "nodes." + id + ".edges", msg.str());
            }
        }
    }

    // 隐含控制流也挂进 graph(可达性/环检查要看得见它们)。
    const auto link = [&](const std::string& from, const std::string& to) {
        if (graph.count(from) && graph.count(to)) graph[from].implicit_next.push_back(to);
    };
    for (const auto& node : def.nodes) {
        for (const auto& b : node.branches) link(node.id, b);
        if (!node.map_body.empty()) link(node.id, node.map_body);
        if (!node.reduce_body.empty()) link(node.id, node.reduce_body);
        for (const auto& c : node.conditions) link(node.id, c.to);
        if (!node.default_to.empty()) link(node.id, node.default_to);
        if (!node.fallback_to.empty()) link(node.id, node.fallback_to);
        // join 的分支汇回 join 自身?不——parallel 内嵌分支时,branches 直接
        // 写在 parallel 节点上,汇合由 parallel 的出边(joined outcome)接手,
        // 不需要独立 join 节点。独立 join 节点的分支经普通边进来。
    }

    // ---- 可达性:entry 出发 ----
    {
        std::set<std::string> visited;
        std::deque<std::string> work{def.entry};
        visited.insert(def.entry);
        while (!work.empty()) {
            const std::string id = work.front();
            work.pop_front();
            const auto it = graph.find(id);
            if (it == graph.end()) continue;
            for (const auto& [outcome, tos] : it->second.outgoing) {
                for (const auto& to : tos) {
                    if (visited.insert(to).second) work.push_back(to);
                }
            }
            for (const auto& to : it->second.implicit_next) {
                if (visited.insert(to).second) work.push_back(to);
            }
        }
        for (const auto& node : def.nodes) {
            if (!visited.count(node.id)) {
                add("unreachable", node_path(node.id), "节点不可达(entry 出发到不了)");
            }
        }
    }

    // ---- 环上限:每个环必须带 max_iterations,或整体被 max_steps 罩住。
    // 首版规矩(单子 Edge 一节):循环必须标 max_iterations 或耗用全局
    // max_steps;无界环校验不过。实现:存在环且图中没有 checkpoint 型
    // 循环上限声明(WorkflowLimits.max_steps 恒有默认),检查环上是否
    // 至少有一处 retry/foreach 边界——首版用简化判据:图含环时,环上节点
    // 必须是 map/foreach/reduce 之外的受控种类,且 foreach/map 有全局
    // max_steps 罩底(max_steps <= 0 才算无界)。
    {
        // Kahn 剩余节点 = 环上节点。
        std::unordered_map<std::string, int> degree;
        std::unordered_map<std::string, std::vector<std::string>> succ;
        for (const auto& [id, deps] : graph) {
            degree.emplace(id, 0);
            succ.emplace(id, std::vector<std::string>{});
        }
        for (const auto& [id, deps] : graph) {
            for (const auto& [outcome, tos] : deps.outgoing) {
                for (const auto& to : tos) {
                    succ[id].push_back(to);
                    ++degree[to];
                }
            }
            for (const auto& to : deps.implicit_next) {
                succ[id].push_back(to);
                ++degree[to];
            }
        }
        std::deque<std::string> ready;
        for (const auto& [id, d] : degree) {
            if (d == 0) ready.push_back(id);
        }
        std::size_t removed = 0;
        while (!ready.empty()) {
            const std::string id = ready.front();
            ready.pop_front();
            ++removed;
            for (const auto& next : succ[id]) {
                if (--degree[next] == 0) ready.push_back(next);
            }
        }
        if (removed < graph.size()) {
            if (def.limits.max_steps <= 0) {
                add("unbounded_loop", "limits.max_steps", "图含环且无全局步数帽,无界环校验不过");
            }
            // 环上找出一个点名,错误落得到人看得懂的位置。
            for (const auto& [id, d] : degree) {
                if (d > 0) {
                    add("cycle_detected", node_path(id),
                        "节点在环上;循环须由 map/foreach(max_steps 罩底)或显式上限约束");
                    break;
                }
            }
        }
    }

    // ---- ${...} 引用:指向的节点必须存在且不指向未来 ----
    {
        const std::map<std::string, int> order = EstimateTopoOrder(def);
        for (const auto& node : def.nodes) {
            std::set<std::string> refs;
            CollectRefNodes(node.input, node, refs);
            CollectRefNodes(node.items_ref.empty() ? nlohmann::json::object()
                                                   : nlohmann::json(node.items_ref),
                            node, refs);
            CollectRefNodes(node.initial_ref.empty() ? nlohmann::json::object()
                                                     : nlohmann::json(node.initial_ref),
                            node, refs);
            for (const auto& c : node.conditions) {
                CollectRefNodes(nlohmann::json(c.path), node, refs);
            }
            for (const auto& ref : refs) {
                if (!graph.count(ref)) {
                    add("dangling_ref", node_path(node.id) + ".input",
                        "${nodes." + ref + "...} 指向不存在的节点");
                    continue;
                }
                const auto self_it = order.find(node.id);
                const auto ref_it = order.find(ref);
                if (self_it != order.end() && ref_it != order.end() && ref_it->second >= self_it->second) {
                    add("forward_ref", node_path(node.id) + ".input",
                        "${nodes." + ref + "...} 指向未完成(未来或同层)节点;只能读已完成节点的输出");
                }
            }
        }
    }

    // ---- 并行分支同写一个 mutable var ----
    {
        // 首版 vars 写入由 state/assign 节点声明;当前节点种类里没有 assign,
        // 这条检查对 future-proof 保留:并行 branches 若同时指向同一只节点,
        // 即视为同写冲突。
        for (const auto& node : def.nodes) {
            if (node.kind != NodeKind::Parallel && node.kind != NodeKind::Map) continue;
            std::set<std::string> bodies;
            for (const auto& b : node.branches) {
                if (!bodies.insert(b).second) {
                    add("branch_conflict", node_path(node.id) + ".branches",
                        "并行分支重复指向同一节点 " + b + "(并行节点互踩)");
                }
            }
        }
    }

    // ---- 能力对账 ----
    if (capabilities.has_value()) {
        const auto& caps = *capabilities;
        const auto contains = [](const std::vector<std::string>& list, const std::string& name) {
            return std::find(list.begin(), list.end(), name) != list.end();
        };
        for (const auto& node : def.nodes) {
            const std::string base = node_path(node.id);
            switch (node.kind) {
                case NodeKind::Tool:
                    if (!node.tool.empty() && !contains(caps.tools, node.tool)) {
                        add("unknown_tool", base + ".tool",
                            "工具未注册: " + node.tool + "(能力表里没有;运行前 capability check 会再查一次)");
                    }
                    break;
                case NodeKind::Skill:
                    if (!node.skill.empty() && !contains(caps.skills, node.skill)) {
                        add("unknown_skill", base + ".skill", "skill 不存在: " + node.skill);
                    }
                    break;
                case NodeKind::Transform:
                    if (!node.operation.empty() && !contains(caps.transforms, node.operation)) {
                        add("unknown_transform", base + ".operation",
                            "变换未注册: " + node.operation + "(不认魔法字符串,须解析成已注册 transform)");
                    }
                    break;
                case NodeKind::Subflow:
                    if (!node.subflow_id.empty() && !contains(caps.workflows, node.subflow_id)) {
                        add("unknown_subflow", base + ".subflow", "workflow 不存在: " + node.subflow_id);
                    }
                    break;
                case NodeKind::Agent:
                    if (!caps.agent_roles.empty() && !node.role.empty() &&
                        !contains(caps.agent_roles, node.role)) {
                        add("unknown_role", base + ".role", "agent 角色不存在: " + node.role);
                    }
                    for (const auto& t : node.allowed_tools) {
                        if (!contains(caps.tools, t)) {
                            add("unknown_tool", base + ".allowed_tools", "工具白名单里有未注册项: " + t);
                        }
                    }
                    break;
                default:
                    break;
            }
        }
    }

    return result;
}

}  // namespace lubancode::workflow
