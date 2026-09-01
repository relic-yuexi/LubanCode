// WorkflowDefinition 的序列化与 hash(自然语言编排单第 1 批)。
//
// 归一化 JSON 用 ordered_json 钉死字段顺序:同一份定义在任何平台序列化
// 出同一串字节,内容 hash 才有意义。FromJson 只认归一化形状(宽容未知
// 字段——协议演进不崩老档),但认不得的枚举字符串抛错,与 runtime 合同
// 同一规矩:不静默映射默认值。

#include "workflow/definition.hpp"

#include <sstream>
#include <stdexcept>
#include <utility>

#include "hooks/hash.hpp"

namespace lubancode::workflow {

namespace {

using ordered = nlohmann::ordered_json;

// 归一化时丢掉 cached 的 normalized 自身,防自嵌套。
ordered Normalize(const WorkflowDefinition& def) {
    ordered j = ordered::object();
    j["schema_version"] = def.schema_version;
    j["id"] = def.id;
    j["version"] = def.version;
    j["name"] = def.name;
    j["description"] = def.description;
    j["alias"] = def.alias;
    j["enabled"] = def.enabled;
    j["inputs"] = def.inputs;
    j["outputs"] = def.outputs;
    j["entry"] = def.entry;

    ordered limits = ordered::object();
    limits["max_concurrency"] = def.limits.max_concurrency;
    limits["max_nodes"] = def.limits.max_nodes;
    limits["max_steps"] = def.limits.max_steps;
    limits["timeout_secs"] = def.limits.timeout_secs;
    limits["tool_calls"] = def.limits.tool_calls;
    limits["tokens"] = def.limits.tokens;
    j["limits"] = limits;

    ordered nodes = ordered::array();
    for (const WorkflowNode& node : def.nodes) {
        ordered n = ordered::object();
        n["id"] = node.id;
        if (!node.label.empty()) {
            n["label"] = node.label;
        }
        n["kind"] = ToString(node.kind);
        if (!node.tool.empty()) n["tool"] = node.tool;
        if (!node.role.empty()) n["role"] = node.role;
        if (!node.agent.empty()) n["agent"] = node.agent;  // 阶段 5:JSON roundtrip 补账(0.26.92 收字段时的欠账)
        if (!node.task.empty()) n["task"] = node.task;
        if (!node.allowed_tools.empty()) n["allowed_tools"] = node.allowed_tools;
        if (node.step_limit > 0) n["step_limit"] = node.step_limit;
        if (node.turn_limit > 0) n["turn_limit"] = node.turn_limit;
        if (!node.model_role.empty()) n["model_role"] = node.model_role;
        if (!node.prompt.empty()) n["prompt"] = node.prompt;
        if (!node.output_schema.empty()) n["output_schema"] = node.output_schema;
        if (!node.skill.empty()) n["skill"] = node.skill;
        if (!node.operation.empty()) n["operation"] = node.operation;
        if (!node.template_path.empty()) n["template"] = node.template_path;
        if (!node.subflow_id.empty()) {
            n["subflow"] = node.subflow_id;
            if (!node.subflow_version.empty()) n["subflow_version"] = node.subflow_version;
        }
        if (node.kind == NodeKind::Async && !node.async_body.empty()) n["body"] = node.async_body;
        if (!node.branches.empty()) n["branches"] = node.branches;
        if (node.kind == NodeKind::Parallel || node.kind == NodeKind::Join) {
            n["join"] = ToString(node.join);
            if (node.join == JoinPolicy::Quorum) n["quorum"] = node.join_quorum;
        }
        if (node.max_concurrency > 0) n["max_concurrency"] = node.max_concurrency;
        if (!node.items_ref.empty()) n["items"] = node.items_ref;
        if (!node.map_body.empty() && (node.kind == NodeKind::Map || node.kind == NodeKind::Foreach)) {
            n["body"] = node.map_body;
        }
        if (!node.reduce_body.empty()) n["reduce_body"] = node.reduce_body;
        if (!node.initial_ref.empty()) n["initial"] = node.initial_ref;
        if (!node.conditions.empty()) {
            ordered cases = ordered::array();
            for (const WorkflowNode::SwitchCase& c : node.conditions) {
                ordered cc = ordered::object();
                cc["op"] = ToString(c.op);
                cc["path"] = c.path;
                if (!c.literal.is_null() && !c.literal.is_object()) cc["literal"] = c.literal;
                cc["to"] = c.to;
                cases.push_back(std::move(cc));
            }
            n["conditions"] = std::move(cases);
            if (!node.default_to.empty()) n["default_to"] = node.default_to;
        }
        if (node.kind == NodeKind::Loop) {
            n["body"] = node.loop_body;
            if (node.loop_until.has_value()) {
                ordered until = ordered::object();
                until["op"] = ToString(node.loop_until->op);
                until["path"] = node.loop_until->path;
                if (!node.loop_until->literal.is_null() && !node.loop_until->literal.is_object()) {
                    until["literal"] = node.loop_until->literal;
                }
                n["until"] = std::move(until);
            }
            n["min_iterations"] = node.loop_min_iterations;
            n["max_iterations"] = node.loop_max_iterations;
            n["hard_limit"] = node.loop_hard_limit;
        }
        n["input"] = node.input;
        if (node.retry.has_value()) {
            ordered r = ordered::object();
            r["attempts"] = node.retry->attempts;
            r["backoff"] = node.retry->backoff == BackoffKind::Exponential ? "exponential" : "fixed";
            r["initial_ms"] = node.retry->initial_ms;
            r["max_ms"] = node.retry->max_ms;
            r["jitter"] = node.retry->jitter;
            if (!node.retry->when.empty()) r["when"] = node.retry->when;
            n["retry"] = std::move(r);
        }
        if (node.on_unavailable != OnUnavailable::Fail || !node.fallback_to.empty()) {
            n["on_unavailable"] = ToString(node.on_unavailable);
            if (!node.fallback_to.empty()) n["fallback_to"] = node.fallback_to;
        }
        if (!node.checkpoint) n["checkpoint"] = false;
        if (node.has_side_effects) {
            n["side_effects"] = true;
            if (!node.idempotency_key.empty()) n["idempotency_key"] = node.idempotency_key;
        }
        nodes.push_back(std::move(n));
    }
    j["nodes"] = std::move(nodes);

    ordered edges = ordered::array();
    for (const WorkflowEdge& e : def.edges) {
        ordered ee = ordered::object();
        ee["from"] = e.from;
        ee["outcome"] = e.outcome;
        ee["to"] = e.to;
        edges.push_back(std::move(ee));
    }
    j["edges"] = std::move(edges);
    j["result"] = def.result;
    return j;
}

template <typename Json>
std::string GetStr(const Json& j, const char* key) {
    const auto it = j.find(key);
    if (it == j.end() || it->is_string()) {
        return it == j.end() ? std::string() : it->template get<std::string>();
    }
    return std::string();
}

template <typename Json>
int GetInt(const Json& j, const char* key, int fallback) {
    const auto it = j.find(key);
    if (it == j.end() || !it->is_number_integer()) return fallback;
    return it->template get<int>();
}

template <typename Json>
std::int64_t GetI64(const Json& j, const char* key, std::int64_t fallback) {
    const auto it = j.find(key);
    if (it == j.end() || !it->is_number()) return fallback;
    return it->template get<std::int64_t>();
}

template <typename Json>
Json GetObj(const Json& j, const char* key) {
    const auto it = j.find(key);
    if (it == j.end() || !it->is_object()) return Json::object();
    return *it;
}

template <typename Json>
RetryPolicy ParseRetry(const Json& j) {
    RetryPolicy r;
    r.attempts = GetInt(j, "attempts", 1);
    const std::string backoff = GetStr(j, "backoff");
    r.backoff = backoff == "fixed" ? BackoffKind::Fixed : BackoffKind::Exponential;
    r.initial_ms = GetI64(j, "initial_ms", 1000);
    r.max_ms = GetI64(j, "max_ms", 30000);
    if (const auto it = j.find("jitter"); it != j.end() && it->is_boolean()) {
        r.jitter = it->template get<bool>();
    }
    if (const auto it = j.find("when"); it != j.end() && it->is_array()) {
        for (const auto& w : *it) {
            if (w.is_string()) r.when.push_back(w.template get<std::string>());
        }
    }
    return r;
}

}  // namespace

nlohmann::json BuildNormalizedJson(const WorkflowDefinition& def) {
    return Normalize(def);
}

std::string ContentHash(const WorkflowDefinition& def) {
    const nlohmann::json normalized = Normalize(def);
    const std::string dumped = normalized.dump();
    return lubancode::hooks::Sha256Hex(dumped);
}

nlohmann::json WorkflowDefinition::ToJson() const {
    return Normalize(*this);
}

WorkflowDefinition WorkflowDefinition::FromJson(const nlohmann::json& in) {
    if (!in.is_object()) {
        throw std::runtime_error("workflow definition json is not an object");
    }
    WorkflowDefinition def;
    def.schema_version = GetInt(in, "schema_version", 0);
    def.id = GetStr(in, "id");
    def.version = GetStr(in, "version");
    def.name = GetStr(in, "name");
    def.description = GetStr(in, "description");
    def.alias = GetStr(in, "alias");
    if (const auto it = in.find("enabled"); it != in.end() && it->is_boolean()) {
        def.enabled = it->get<bool>();
    }
    def.inputs = GetObj(in, "inputs");
    def.outputs = GetObj(in, "outputs");
    def.entry = GetStr(in, "entry");

    if (const auto it = in.find("limits"); it != in.end() && it->is_object()) {
        def.limits.max_concurrency = GetInt(*it, "max_concurrency", 4);
        def.limits.max_nodes = GetInt(*it, "max_nodes", 64);
        def.limits.max_steps = GetInt(*it, "max_steps", 128);
        def.limits.timeout_secs = GetI64(*it, "timeout_secs", 600);
        def.limits.tool_calls = GetInt(*it, "tool_calls", 100);
        def.limits.tokens = GetI64(*it, "tokens", 120000);
    }

    if (const auto it = in.find("nodes"); it != in.end() && it->is_array()) {
        for (const auto& raw : *it) {
            if (!raw.is_object()) continue;
            WorkflowNode node;
            node.id = GetStr(raw, "id");
            node.label = GetStr(raw, "label");
            const std::string kind = GetStr(raw, "kind");
            if (!kind.empty() && !ParseNodeKind(kind, node.kind)) {
                throw std::runtime_error("unknown node kind: " + kind);
            }
            node.tool = GetStr(raw, "tool");
            node.role = GetStr(raw, "role");
            node.agent = GetStr(raw, "agent");  // 阶段 5:与 ToJson 同批补齐
            node.task = GetStr(raw, "task");
            if (const auto a = raw.find("allowed_tools"); a != raw.end() && a->is_array()) {
                for (const auto& t : *a) {
                    if (t.is_string()) node.allowed_tools.push_back(t.get<std::string>());
                }
            }
            node.step_limit = GetInt(raw, "step_limit", 0);
            node.turn_limit = GetInt(raw, "turn_limit", 0);
            // 新旧限制同现明拒(turn 预算单 §4.3):两者作用域不同(任务总
            // turn vs 单轮 step),静默择一会猜错——要作者删掉一枚。
            if (node.step_limit > 0 && node.turn_limit > 0) {
                throw std::runtime_error("node " + node.id +
                                         ": step_limit 与 turn_limit 不能同现;"
                                         "前者是待移除的单轮旧限制,后者是任务总 turn 帽,请删掉一枚");
            }
            node.model_role = GetStr(raw, "model_role");
            node.prompt = GetStr(raw, "prompt");
            node.output_schema = GetObj(raw, "output_schema");
            node.skill = GetStr(raw, "skill");
            node.operation = GetStr(raw, "operation");
            node.template_path = GetStr(raw, "template");
            node.subflow_id = GetStr(raw, "subflow");
            node.subflow_version = GetStr(raw, "subflow_version");
            if (const auto b = raw.find("branches"); b != raw.end() && b->is_array()) {
                for (const auto& t : *b) {
                    if (t.is_string()) node.branches.push_back(t.get<std::string>());
                }
            }
            if (const auto jp = raw.find("join"); jp != raw.end() && jp->is_string()) {
                if (!ParseJoinPolicy(jp->get<std::string>(), node.join)) {
                    throw std::runtime_error("unknown join policy: " + jp->get<std::string>());
                }
            }
            node.join_quorum = GetInt(raw, "quorum", 0);
            node.max_concurrency = GetInt(raw, "max_concurrency", 0);
            node.items_ref = GetStr(raw, "items");
            if (node.kind == NodeKind::Loop) {
                if (const auto body = raw.find("body"); body != raw.end() && body->is_array()) {
                    for (const auto& id : *body) {
                        if (id.is_string()) node.loop_body.push_back(id.get<std::string>());
                    }
                }
                if (const auto until = raw.find("until"); until != raw.end() && until->is_object()) {
                    WorkflowNode::SwitchCase condition;
                    const std::string op = GetStr(*until, "op");
                    if (!op.empty() && !ParseConditionOp(op, condition.op)) {
                        throw std::runtime_error("unknown loop condition op: " + op);
                    }
                    condition.path = GetStr(*until, "path");
                    if (const auto lit = until->find("literal"); lit != until->end() && !lit->is_object()) {
                        condition.literal = *lit;
                    }
                    node.loop_until = std::move(condition);
                }
                if (const auto min = raw.find("min_iterations"); min != raw.end()) {
                    node.loop_min_iterations = *min;
                }
                if (const auto max = raw.find("max_iterations"); max != raw.end()) {
                    node.loop_max_iterations = *max;
                }
                node.loop_hard_limit = GetInt(raw, "hard_limit", 32);
            } else if (node.kind == NodeKind::Async) {
                node.async_body = GetStr(raw, "body");
            } else {
                node.map_body = GetStr(raw, "body");
            }
            node.reduce_body = GetStr(raw, "reduce_body");
            node.initial_ref = GetStr(raw, "initial");
            if (const auto cs = raw.find("conditions"); cs != raw.end() && cs->is_array()) {
                for (const auto& c : *cs) {
                    if (!c.is_object()) continue;
                    WorkflowNode::SwitchCase sc;
                    const std::string op = GetStr(c, "op");
                    if (!op.empty() && !ParseConditionOp(op, sc.op)) {
                        throw std::runtime_error("unknown condition op: " + op);
                    }
                    sc.path = GetStr(c, "path");
                    if (const auto lit = c.find("literal"); lit != c.end() && !lit->is_object()) {
                        if (!lit->is_null()) sc.literal = *lit;
                    }
                    sc.to = GetStr(c, "to");
                    node.conditions.push_back(std::move(sc));
                }
            }
            node.default_to = GetStr(raw, "default_to");
            node.input = raw.contains("input") && raw["input"].is_object()
                             ? raw["input"]
                             : (raw.contains("input") && raw["input"].is_string() ? raw["input"]
                                                                                  : nlohmann::json::object());
            if (const auto r = raw.find("retry"); r != raw.end() && r->is_object()) {
                node.retry = ParseRetry(*r);
            }
            const std::string on_unavailable = GetStr(raw, "on_unavailable");
            if (!on_unavailable.empty()) {
                if (on_unavailable == "fail") node.on_unavailable = OnUnavailable::Fail;
                else if (on_unavailable == "skip") node.on_unavailable = OnUnavailable::Skip;
                else if (on_unavailable == "fallback") node.on_unavailable = OnUnavailable::Fallback;
                else if (on_unavailable == "ask") node.on_unavailable = OnUnavailable::Ask;
                else throw std::runtime_error("unknown on_unavailable: " + on_unavailable);
            }
            node.fallback_to = GetStr(raw, "fallback_to");
            if (const auto cp = raw.find("checkpoint"); cp != raw.end() && cp->is_boolean()) {
                node.checkpoint = cp->get<bool>();
            }
            if (const auto se = raw.find("side_effects"); se != raw.end() && se->is_boolean()) {
                node.has_side_effects = se->get<bool>();
            }
            node.idempotency_key = GetStr(raw, "idempotency_key");
            def.nodes.push_back(std::move(node));
        }
    }
    for (const WorkflowNode& node : def.nodes) {
        def.node_map.emplace(node.id, node);
    }

    if (const auto it = in.find("edges"); it != in.end() && it->is_array()) {
        for (const auto& raw : *it) {
            if (!raw.is_object()) continue;
            WorkflowEdge edge;
            edge.from = GetStr(raw, "from");
            edge.outcome = GetStr(raw, "outcome");
            if (edge.outcome.empty()) edge.outcome = "success";
            edge.to = GetStr(raw, "to");
            def.edges.push_back(std::move(edge));
        }
    }

    def.result = GetObj(in, "result");
    def.normalized = Normalize(def);
    return def;
}

// ---- 枚举 <-> 稳定字符串 ---------------------------------------------------

std::string ToString(NodeKind kind) {
    switch (kind) {
        case NodeKind::Tool: return "tool";
        case NodeKind::Agent: return "agent";
        case NodeKind::Llm: return "llm";
        case NodeKind::Skill: return "skill";
        case NodeKind::Template: return "template";
        case NodeKind::Transform: return "transform";
        case NodeKind::Approval: return "approval";
        case NodeKind::AskUser: return "ask_user";
        case NodeKind::Subflow: return "subflow";
        case NodeKind::Async: return "async";
        case NodeKind::Parallel: return "parallel";
        case NodeKind::Join: return "join";
        case NodeKind::Map: return "map";
        case NodeKind::Reduce: return "reduce";
        case NodeKind::Switch: return "switch";
        case NodeKind::Foreach: return "foreach";
        case NodeKind::Loop: return "loop";
        case NodeKind::Checkpoint: return "checkpoint";
        case NodeKind::End: return "end";
    }
    return "tool";
}

bool ParseNodeKind(const std::string& s, NodeKind& out) {
    if (s == "tool") { out = NodeKind::Tool; return true; }
    if (s == "agent") { out = NodeKind::Agent; return true; }
    if (s == "llm") { out = NodeKind::Llm; return true; }
    if (s == "skill") { out = NodeKind::Skill; return true; }
    if (s == "template") { out = NodeKind::Template; return true; }
    if (s == "transform") { out = NodeKind::Transform; return true; }
    if (s == "approval") { out = NodeKind::Approval; return true; }
    if (s == "ask_user") { out = NodeKind::AskUser; return true; }
    if (s == "subflow") { out = NodeKind::Subflow; return true; }
    if (s == "async") { out = NodeKind::Async; return true; }
    if (s == "parallel") { out = NodeKind::Parallel; return true; }
    if (s == "join") { out = NodeKind::Join; return true; }
    if (s == "map") { out = NodeKind::Map; return true; }
    if (s == "reduce") { out = NodeKind::Reduce; return true; }
    if (s == "switch") { out = NodeKind::Switch; return true; }
    if (s == "foreach") { out = NodeKind::Foreach; return true; }
    if (s == "loop") { out = NodeKind::Loop; return true; }
    if (s == "checkpoint") { out = NodeKind::Checkpoint; return true; }
    if (s == "end") { out = NodeKind::End; return true; }
    return false;
}

std::string ToString(JoinPolicy policy) {
    switch (policy) {
        case JoinPolicy::All: return "all";
        case JoinPolicy::AllSettled: return "all_settled";
        case JoinPolicy::Any: return "any";
        case JoinPolicy::Quorum: return "quorum";
        case JoinPolicy::Race: return "race";
    }
    return "all_settled";
}

bool ParseJoinPolicy(const std::string& s, JoinPolicy& out) {
    if (s == "all") { out = JoinPolicy::All; return true; }
    if (s == "all_settled") { out = JoinPolicy::AllSettled; return true; }
    if (s == "any") { out = JoinPolicy::Any; return true; }
    if (s == "quorum") { out = JoinPolicy::Quorum; return true; }
    if (s == "race") { out = JoinPolicy::Race; return true; }
    return false;
}

std::string ToString(ConditionOp op) {
    switch (op) {
        case ConditionOp::Exists: return "exists";
        case ConditionOp::NotExists: return "not_exists";
        case ConditionOp::Equals: return "equals";
        case ConditionOp::NotEquals: return "not_equals";
        case ConditionOp::GreaterThan: return "gt";
        case ConditionOp::LessThan: return "lt";
        case ConditionOp::Contains: return "contains";
        case ConditionOp::StartsWith: return "starts_with";
        case ConditionOp::NonEmpty: return "non_empty";
    }
    return "exists";
}

bool ParseConditionOp(const std::string& s, ConditionOp& out) {
    if (s == "exists") { out = ConditionOp::Exists; return true; }
    if (s == "not_exists") { out = ConditionOp::NotExists; return true; }
    if (s == "equals") { out = ConditionOp::Equals; return true; }
    if (s == "not_equals") { out = ConditionOp::NotEquals; return true; }
    if (s == "gt") { out = ConditionOp::GreaterThan; return true; }
    if (s == "lt") { out = ConditionOp::LessThan; return true; }
    if (s == "contains") { out = ConditionOp::Contains; return true; }
    if (s == "starts_with") { out = ConditionOp::StartsWith; return true; }
    if (s == "non_empty") { out = ConditionOp::NonEmpty; return true; }
    return false;
}

std::string ToString(OnUnavailable value) {
    switch (value) {
        case OnUnavailable::Fail: return "fail";
        case OnUnavailable::Skip: return "skip";
        case OnUnavailable::Fallback: return "fallback";
        case OnUnavailable::Ask: return "ask";
    }
    return "fail";
}

}  // namespace lubancode::workflow
