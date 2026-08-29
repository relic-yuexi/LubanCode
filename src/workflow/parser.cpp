// Workflow YAML 解析实现(自然语言编排单第 1 批)。
//
// yaml-cpp 只做"文本 -> 节点树";这层把节点树逐字段搬进强类型 AST,
// 搬不动(类型不对、枚举认不得、路径越界)就记 ParseIssue。绝不猜意思:
// 认不得的 schema_version 直接拒,单子"解析器不认新 schema 就拒跑"。

#include "workflow/parser.hpp"

#include <fstream>
#include <sstream>
#include <utility>

#include <yaml-cpp/yaml.h>

#include "platform/paths.hpp"

namespace lubancode::workflow {

namespace {

// YAML 节点 -> nlohmann::json(schema/inputs/outputs/input/result 这类
// 逐字段透传的形状)。yaml-cpp 的 Map/Sequence/Scalar 直译;tag 不认。
nlohmann::json YamlToJson(const YAML::Node& node) {
    switch (node.Type()) {
        case YAML::NodeType::Null:
            return nlohmann::json();
        case YAML::NodeType::Scalar: {
            const std::string& raw = node.Scalar();
            // yaml-cpp 在不同版本下会给普通标量 "?" 而非完整 core tag。
            // "!" 是显式字符串(含带引号标量),必须原样留下;其余先看
            // 完整 tag,再按完整词法还原 bool/int/float/null。
            if (node.Tag() == "tag:yaml.org,2002:int") {
                try { return nlohmann::json(std::stoll(raw)); } catch (...) { return raw; }
            }
            if (node.Tag() == "tag:yaml.org,2002:float") {
                try { return nlohmann::json(std::stod(raw)); } catch (...) { return raw; }
            }
            if (node.Tag() == "tag:yaml.org,2002:bool") {
                return nlohmann::json(raw == "true" || raw == "True" || raw == "yes" || raw == "on");
            }
            if (node.Tag() == "tag:yaml.org,2002:null") {
                return nlohmann::json();
            }
            if (node.Tag() != "!") {
                if (raw == "true" || raw == "True" || raw == "TRUE" || raw == "yes" || raw == "Yes" ||
                    raw == "YES" || raw == "on" || raw == "On" || raw == "ON") {
                    return true;
                }
                if (raw == "false" || raw == "False" || raw == "FALSE" || raw == "no" || raw == "No" ||
                    raw == "NO" || raw == "off" || raw == "Off" || raw == "OFF") {
                    return false;
                }
                if (raw == "null" || raw == "Null" || raw == "NULL" || raw == "~") return nlohmann::json();
                try {
                    std::size_t used = 0;
                    const auto integer = std::stoll(raw, &used);
                    if (used == raw.size()) return nlohmann::json(integer);
                } catch (...) {
                }
                try {
                    std::size_t used = 0;
                    const auto number = std::stod(raw, &used);
                    if (used == raw.size()) return nlohmann::json(number);
                } catch (...) {
                }
            }
            return raw;
        }
        case YAML::NodeType::Sequence: {
            nlohmann::json out = nlohmann::json::array();
            for (const auto& item : node) out.push_back(YamlToJson(item));
            return out;
        }
        case YAML::NodeType::Map: {
            nlohmann::json out = nlohmann::json::object();
            for (const auto& kv : node) out[kv.first.Scalar()] = YamlToJson(kv.second);
            return out;
        }
        default:
            return nlohmann::json();
    }
}

nlohmann::json LoopBoundFromYaml(const YAML::Node& node) {
    if (!node || !node.IsScalar()) return YamlToJson(node);
    const std::string raw = node.Scalar();
    try {
        std::size_t used = 0;
        const int value = std::stoi(raw, &used);
        if (used == raw.size()) return value;
    } catch (...) {
    }
    return raw;
}

std::string Where(const YAML::Node& node, const std::string& field) {
    const YAML::Mark mark = node.Mark();
    if (mark.line < 0) return field;
    return "workflow.yaml:" + std::to_string(mark.line + 1) + " " + field;
}

struct IssueSink {
    std::vector<ParseIssue> issues;

    void Add(const std::string& location, const std::string& message) {
        issues.push_back(ParseIssue{location, message});
    }
};

}  // namespace

bool IsSafePackageRelative(const std::string& ref) {
    if (ref.empty() || ref.size() > 512) return false;
    if (ref.front() == '/' || ref.front() == '\\') return false;
    if (ref.find('\\') != std::string::npos) return false;
    if (ref.find(":") != std::string::npos) return false;  // 盘符/URL 都不认
    std::size_t start = 0;
    while (start <= ref.size()) {
        const std::size_t slash = ref.find('/', start);
        const std::string seg = ref.substr(start, slash == std::string::npos ? std::string::npos : slash - start);
        if (seg.empty() || seg == "." || seg == "..") return false;
        for (const char c : seg) {
            const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                            c == '-' || c == '_' || c == '.';
            if (!ok) return false;
        }
        if (seg.find("..") != std::string::npos) return false;  // "a.." 这类也拦
        if (slash == std::string::npos) break;
        start = slash + 1;
    }
    return true;
}

std::optional<long long> ParseDurationSecs(const std::string& text) {
    if (text.empty()) return std::nullopt;
    std::size_t digits = 0;
    while (digits < text.size() && text[digits] >= '0' && text[digits] <= '9') ++digits;
    if (digits == 0) return std::nullopt;
    long long value = 0;
    try {
        value = std::stoll(text.substr(0, digits));
    } catch (...) {
        return std::nullopt;
    }
    const std::string unit = text.substr(digits);
    if (unit.empty() || unit == "s") return value;
    if (unit == "m") return value * 60;
    if (unit == "h") return value * 3600;
    return std::nullopt;
}

std::expected<WorkflowDefinition, std::vector<ParseIssue>> ParseWorkflowYaml(const std::string& yaml_text) {
    IssueSink sink;
    WorkflowDefinition def;

    YAML::Node root;
    try {
        root = YAML::Load(yaml_text);
    } catch (const YAML::Exception& e) {
        sink.Add("workflow.yaml", std::string("YAML 解析失败: ") + e.what());
        return std::unexpected(std::move(sink.issues));
    }
    if (!root.IsMap()) {
        sink.Add("workflow.yaml", "顶层必须是映射(mapping)");
        return std::unexpected(std::move(sink.issues));
    }

    // ---- 顶部标量字段 ----
    const auto scalar = [&](const char* key, std::string& into) {
        if (const auto& n = root[key]; n && n.IsScalar()) into = n.Scalar();
    };
    if (const auto& n = root["schema_version"]; n && n.IsScalar()) {
        try {
            def.schema_version = std::stoi(n.Scalar());
        } catch (...) {
            sink.Add(Where(n, "schema_version"), "schema_version 必须是整数");
        }
    } else {
        sink.Add("workflow.yaml", "缺 schema_version");
    }
    if (def.schema_version != kCurrentSchemaVersion) {
        sink.Add("workflow.yaml", "schema_version " + std::to_string(def.schema_version) +
                                      " 本版不认(支持: " + std::to_string(kCurrentSchemaVersion) +
                                      "),拒绝解析,不猜意思");
        return std::unexpected(std::move(sink.issues));
    }
    scalar("id", def.id);
    scalar("version", def.version);
    scalar("name", def.name);
    scalar("description", def.description);
    scalar("alias", def.alias);
    if (def.id.empty()) sink.Add("workflow.yaml", "缺 id");
    if (def.version.empty()) sink.Add("workflow.yaml", "缺 version(业务版本,如 1.0.0)");
    if (const auto& n = root["enabled"]; n && n.IsScalar()) {
        def.enabled = n.Scalar() == "true" || n.Scalar() == "yes" || n.Scalar() == "on";
    }

    if (const auto& n = root["inputs"]) def.inputs = YamlToJson(n);
    if (const auto& n = root["outputs"]) def.outputs = YamlToJson(n);
    if (const auto& n = root["entry"]; n && n.IsScalar()) def.entry = n.Scalar();

    // ---- limits ----
    if (const auto& lim = root["limits"]; lim && lim.IsMap()) {
        const auto int_field = [&](const char* key, int& into) {
            if (const auto& n = lim[key]; n && n.IsScalar()) {
                try {
                    into = std::stoi(n.Scalar());
                } catch (...) {
                    sink.Add(Where(n, std::string("limits.") + key), "必须是整数");
                }
            }
        };
        int_field("max_concurrency", def.limits.max_concurrency);
        int_field("max_nodes", def.limits.max_nodes);
        int_field("max_steps", def.limits.max_steps);
        int_field("tool_calls", def.limits.tool_calls);
        if (const auto& n = lim["timeout"]; n && n.IsScalar()) {
            if (const auto secs = ParseDurationSecs(n.Scalar())) {
                def.limits.timeout_secs = *secs;
            } else {
                sink.Add(Where(n, "limits.timeout"), "认不得的时长写法(支持 90s/10m/1h)");
            }
        }
        if (const auto& n = lim["tokens"]; n && n.IsScalar()) {
            try {
                def.limits.tokens = std::stoll(n.Scalar());
            } catch (...) {
                sink.Add(Where(n, "limits.tokens"), "必须是整数");
            }
        }
    }

    // ---- nodes ----
    const auto nodes = root["nodes"];
    if (!nodes || !nodes.IsMap()) {
        sink.Add("workflow.yaml", "缺 nodes(映射:节点id -> 节点定义)");
    } else {
        for (const auto& kv : nodes) {
            const std::string id = kv.first.Scalar();
            const YAML::Node& raw = kv.second;
            if (!raw.IsMap()) {
                sink.Add(Where(raw, "nodes." + id), "节点定义必须是映射");
                continue;
            }
            WorkflowNode node;
            node.id = id;
            if (const auto& n = raw["label"]; n && n.IsScalar()) node.label = n.Scalar();
            std::string kind = "tool";
            if (const auto& n = raw["type"]; n && n.IsScalar()) kind = n.Scalar();
            if (!ParseNodeKind(kind, node.kind)) {
                sink.Add(Where(raw["type"], "nodes." + id + ".type"), "认不得的节点类型: " + kind);
                continue;
            }
            const auto node_str = [&](const char* key, std::string& into) {
                if (const auto& n = raw[key]; n && n.IsScalar()) into = n.Scalar();
            };
            node_str("tool", node.tool);
            node_str("role", node.role);
            node_str("agent", node.agent);
            node_str("task", node.task);
            if (const auto& n = raw["allowed_tools"]; n && n.IsSequence()) {
                for (const auto& t : n) {
                    if (t.IsScalar()) node.allowed_tools.push_back(t.Scalar());
                }
            }
            if (const auto& n = raw["step_limit"]; n && n.IsScalar()) {
                try { node.step_limit = std::stoi(n.Scalar()); } catch (...) {}
            }
            node_str("model_role", node.model_role);
            node_str("prompt", node.prompt);
            if (const auto& n = raw["output_schema"]) node.output_schema = YamlToJson(n);
            node_str("skill", node.skill);
            node_str("operation", node.operation);
            node_str("template", node.template_path);
            node_str("subflow", node.subflow_id);
            node_str("subflow_version", node.subflow_version);
            if (const auto& n = raw["branches"]; n && n.IsSequence()) {
                for (const auto& t : n) {
                    if (t.IsScalar()) node.branches.push_back(t.Scalar());
                }
            }
            if (const auto& n = raw["join"]; n && n.IsScalar()) {
                if (!ParseJoinPolicy(n.Scalar(), node.join)) {
                    sink.Add(Where(n, "nodes." + id + ".join"),
                             "认不得的汇合策略: " + n.Scalar() +
                                 "(支持 all/all_settled/any/quorum/race)");
                }
            }
            if (const auto& n = raw["quorum"]; n && n.IsScalar()) {
                try { node.join_quorum = std::stoi(n.Scalar()); } catch (...) {}
            }
            if (const auto& n = raw["max_concurrency"]; n && n.IsScalar()) {
                try { node.max_concurrency = std::stoi(n.Scalar()); } catch (...) {}
            }
            node_str("items", node.items_ref);
            // loop 的 body 是顺次节点表;async/map/foreach 的 body 是一只节点。
            if (const auto& n = raw["body"]; n) {
                if (node.kind == NodeKind::Loop && n.IsSequence()) {
                    for (const auto& item : n) {
                        if (item.IsScalar()) node.loop_body.push_back(item.Scalar());
                    }
                } else if (node.kind == NodeKind::Async && n.IsScalar()) {
                    node.async_body = n.Scalar();
                } else if (n.IsScalar()) {
                    node.map_body = n.Scalar();
                    node.reduce_body = n.Scalar();
                }
            }
            node_str("reduce_body", node.reduce_body);
            node_str("initial", node.initial_ref);
            if (const auto& cs = raw["conditions"]; cs && cs.IsSequence()) {
                for (const auto& c : cs) {
                    if (!c.IsMap()) continue;
                    WorkflowNode::SwitchCase sc;
                    std::string op = "exists";
                    if (const auto& n = c["op"]; n && n.IsScalar()) op = n.Scalar();
                    if (!ParseConditionOp(op, sc.op)) {
                        sink.Add(Where(c, "nodes." + id + ".conditions"), "认不得的条件算子: " + op);
                        continue;
                    }
                    if (const auto& n = c["path"]; n && n.IsScalar()) sc.path = n.Scalar();
                    if (const auto& n = c["value"]) sc.literal = YamlToJson(n);
                    if (const auto& n = c["to"]; n && n.IsScalar()) sc.to = n.Scalar();
                    node.conditions.push_back(std::move(sc));
                }
            }
            node_str("default_to", node.default_to);
            if (node.kind == NodeKind::Loop) {
                if (const auto& until = raw["until"]; until && until.IsMap()) {
                    WorkflowNode::SwitchCase condition;
                    std::string op = "exists";
                    if (const auto& n = until["op"]; n && n.IsScalar()) op = n.Scalar();
                    if (!ParseConditionOp(op, condition.op)) {
                        sink.Add(Where(until, "nodes." + id + ".until"),
                                 "认不得的条件操作: " + op);
                    }
                    if (const auto& n = until["path"]; n && n.IsScalar()) condition.path = n.Scalar();
                    if (const auto& n = until["value"]) condition.literal = YamlToJson(n);
                    node.loop_until = std::move(condition);
                }
                if (const auto& n = raw["min_iterations"]) node.loop_min_iterations = LoopBoundFromYaml(n);
                if (const auto& n = raw["max_iterations"]) node.loop_max_iterations = LoopBoundFromYaml(n);
                if (const auto& n = raw["hard_limit"]; n && n.IsScalar()) {
                    try { node.loop_hard_limit = std::stoi(n.Scalar()); } catch (...) {}
                }
            }
            if (const auto& n = raw["input"]) node.input = YamlToJson(n);
            if (const auto& r = raw["retry"]; r && r.IsMap()) {
                RetryPolicy policy;
                if (const auto& n = r["attempts"]; n && n.IsScalar()) {
                    try { policy.attempts = std::max(1, std::stoi(n.Scalar())); } catch (...) {}
                }
                if (const auto& n = r["backoff"]; n && n.IsScalar()) {
                    policy.backoff = n.Scalar() == "fixed" ? BackoffKind::Fixed : BackoffKind::Exponential;
                }
                if (const auto& n = r["initial"]; n && n.IsScalar()) {
                    if (const auto ms = ParseDurationSecs(n.Scalar())) policy.initial_ms = *ms * 1000;
                }
                if (const auto& n = r["max"]; n && n.IsScalar()) {
                    if (const auto ms = ParseDurationSecs(n.Scalar())) policy.max_ms = *ms * 1000;
                }
                if (const auto& n = r["jitter"]; n && n.IsScalar()) {
                    policy.jitter = !(n.Scalar() == "false" || n.Scalar() == "no");
                }
                if (const auto& n = r["when"]; n && n.IsSequence()) {
                    for (const auto& w : n) {
                        if (w.IsScalar()) policy.when.push_back(w.Scalar());
                    }
                }
                node.retry = policy;
            }
            if (const auto& n = raw["on_unavailable"]; n && n.IsScalar()) {
                const std::string& v = n.Scalar();
                if (v == "fail") node.on_unavailable = OnUnavailable::Fail;
                else if (v == "skip") node.on_unavailable = OnUnavailable::Skip;
                else if (v == "fallback") node.on_unavailable = OnUnavailable::Fallback;
                else if (v == "ask") node.on_unavailable = OnUnavailable::Ask;
                else sink.Add(Where(n, "nodes." + id + ".on_unavailable"), "认不得的处置: " + v);
            }
            node_str("fallback_to", node.fallback_to);
            if (const auto& n = raw["checkpoint"]; n && n.IsScalar()) {
                node.checkpoint = !(n.Scalar() == "false" || n.Scalar() == "no");
            }
            if (const auto& n = raw["side_effects"]; n && n.IsScalar()) {
                node.has_side_effects = n.Scalar() == "true" || n.Scalar() == "yes";
            }
            node_str("idempotency_key", node.idempotency_key);

            // 包内引用守门:越界在这里就报,不留给 validator。
            const auto guard_ref = [&](const char* field, const std::string& value) {
                if (!value.empty() && !IsSafePackageRelative(value)) {
                    sink.Add("nodes." + id + "." + field,
                             "包内引用必须是目录内相对路径,拒绝越界: " + value);
                }
            };
            guard_ref("prompt", node.prompt);
            guard_ref("task", node.task);
            guard_ref("template", node.template_path);

            def.nodes.push_back(std::move(node));
        }
    }
    for (const WorkflowNode& node : def.nodes) {
        def.node_map.emplace(node.id, node);
    }

    // ---- edges ----
    if (const auto& es = root["edges"]; es && es.IsSequence()) {
        for (const auto& raw : es) {
            if (!raw.IsMap()) continue;
            WorkflowEdge edge;
            if (const auto& n = raw["from"]; n && n.IsScalar()) edge.from = n.Scalar();
            if (const auto& n = raw["on"]; n && n.IsScalar()) edge.outcome = n.Scalar();
            if (edge.outcome.empty()) edge.outcome = "success";
            if (const auto& n = raw["to"]; n && n.IsScalar()) edge.to = n.Scalar();
            def.edges.push_back(std::move(edge));
        }
    }

    if (const auto& n = root["result"]) def.result = YamlToJson(n);

    if (!sink.issues.empty()) {
        return std::unexpected(std::move(sink.issues));
    }
    def.normalized = BuildNormalizedJson(def);
    return def;
}

std::expected<WorkflowDefinition, std::vector<ParseIssue>> LoadWorkflowDefinition(
    const std::filesystem::path& workflow_yaml) {
    std::error_code ec;
    if (!std::filesystem::exists(workflow_yaml, ec)) {
        return std::unexpected(std::vector<ParseIssue>{
            ParseIssue{"workflow.yaml", "文件不存在: " + lubancode::platform::PathToUtf8(workflow_yaml)}});
    }
    std::ifstream file(workflow_yaml, std::ios::binary);
    if (!file) {
        return std::unexpected(std::vector<ParseIssue>{
            ParseIssue{"workflow.yaml", "打不开: " + lubancode::platform::PathToUtf8(workflow_yaml)}});
    }
    std::ostringstream buffer;
    buffer << file.rdbuf();
    return ParseWorkflowYaml(buffer.str());
}

// ---------------------------------------------------------------------------
// Emit(创建向导落盘)。按定义顺序逐字段写;YAML 锚点、流式集合一概不
// 用——写出来的文本喂回 ParseWorkflowYaml 必须解析出同一份 AST(单测钉)。
// ---------------------------------------------------------------------------

namespace {

// nlohmann 没有统一 is_scalar;这里的"标量"= 不是 object 也不是 array。
bool IsYamlScalar(const nlohmann::json& v) { return !v.is_object() && !v.is_array() && !v.is_null(); }

void EmitJsonAsYaml(std::ostream& out, const nlohmann::json& j, int indent);

void EmitScalar(std::ostream& out, const nlohmann::json& v) {
    if (v.is_string()) {
        // yaml-cpp 写字符串的引号规矩自己一套;这里统一双引号 + 转义,
        // 简单且 round-trip 稳。
        std::string escaped;
        for (const char c : v.get<std::string>()) {
            switch (c) {
                case '"': escaped += "\\\""; break;
                case '\\': escaped += "\\\\"; break;
                case '\n': escaped += "\\n"; break;
                case '\t': escaped += "\\t"; break;
                default: escaped += c; break;
            }
        }
        out << '"' << escaped << '"';
    } else if (v.is_boolean()) {
        out << (v.get<bool>() ? "true" : "false");
    } else {
        out << v.dump();
    }
}

void EmitJsonAsYaml(std::ostream& out, const nlohmann::json& j, int indent) {
    const std::string pad(static_cast<std::size_t>(indent), ' ');
    if (j.is_object()) {
        if (j.empty()) { out << "{}"; return; }
        out << "\n";
        for (auto it = j.begin(); it != j.end(); ++it) {
            out << pad << it.key() << ":";
            if (it.value().is_object() && !it.value().empty()) {
                EmitJsonAsYaml(out, it.value(), indent + 2);
            } else if (it.value().is_array() && !it.value().empty()) {
                out << "\n";
                const std::string item_pad(static_cast<std::size_t>(indent) + 2, ' ');
                for (const auto& item : it.value()) {
                    out << item_pad << "- ";
                    if (IsYamlScalar(item)) {
                        EmitScalar(out, item);
                        out << "\n";
                    } else {
                        EmitJsonAsYaml(out, item, indent + 4);
                    }
                }
            } else if (IsYamlScalar(it.value())) {
                out << " ";
                EmitScalar(out, it.value());
                out << "\n";
            } else {
                out << " " << (it.value().is_object() ? "{}" : "[]") << "\n";
            }
        }
    } else if (j.is_array()) {
        if (j.empty()) { out << "[]"; return; }
        out << "\n";
        const std::string item_pad(static_cast<std::size_t>(indent), ' ');
        for (const auto& item : j) {
            out << item_pad << "- ";
            if (IsYamlScalar(item)) {
                EmitScalar(out, item);
                out << "\n";
            } else {
                EmitJsonAsYaml(out, item, indent + 2);
            }
        }
    } else {
        // 裸标量(顶层整体一个 "${...}" 引用):值是 EmitScalar 的输出,
        // 前面由调用处的 ":" 之后接——这里补一个空格再落值。
        out << " ";
        EmitScalar(out, j);
        out << "\n";
    }
}

}  // namespace

// 顶层标量字段:统一走 EmitScalar(带引号/转义)——description 里一个
// 半角冒号就能让裸标量行变成嵌套 map,yaml-cpp 报 illegal map value。
std::string EmitWorkflowYaml(const WorkflowDefinition& def) {
    std::ostringstream out;
    out << "schema_version: " << def.schema_version << "\n";
    out << "id: " << def.id << "\n";
    out << "version: " << def.version << "\n";
    out << "name: " << def.name << "\n";
    out << "description: ";
    EmitScalar(out, def.description);
    out << "\n";
    if (!def.alias.empty()) {
        out << "alias: ";
        EmitScalar(out, def.alias);
        out << "\n";
    }
    if (!def.enabled) out << "enabled: false\n";
    if (!def.inputs.empty()) {
        out << "inputs:";
        EmitJsonAsYaml(out, def.inputs, 2);
    }
    if (!def.outputs.empty()) {
        out << "outputs:";
        EmitJsonAsYaml(out, def.outputs, 2);
    }
    out << "entry: " << def.entry << "\n";
    out << "limits:\n";
    out << "  max_concurrency: " << def.limits.max_concurrency << "\n";
    out << "  max_nodes: " << def.limits.max_nodes << "\n";
    out << "  max_steps: " << def.limits.max_steps << "\n";
    out << "  timeout: " << def.limits.timeout_secs << "s\n";
    out << "  tool_calls: " << def.limits.tool_calls << "\n";
    out << "  tokens: " << def.limits.tokens << "\n";

    out << "nodes:\n";
    for (const WorkflowNode& node : def.nodes) {
        out << "  " << node.id << ":\n";
        out << "    type: " << ToString(node.kind) << "\n";
        if (!node.label.empty()) out << "    label: " << node.label << "\n";
        if (!node.tool.empty()) {
            out << "    tool: ";
            EmitScalar(out, node.tool);
            out << "\n";
        }
        if (!node.role.empty()) out << "    role: " << node.role << "\n";
        if (!node.agent.empty()) out << "    agent: " << node.agent << "\n";
        if (!node.task.empty()) out << "    task: " << node.task << "\n";
        if (!node.allowed_tools.empty()) {
            out << "    allowed_tools:\n";
            for (const auto& t : node.allowed_tools) out << "      - " << t << "\n";
        }
        if (node.step_limit > 0) out << "    step_limit: " << node.step_limit << "\n";
        if (!node.model_role.empty()) out << "    model_role: " << node.model_role << "\n";
        if (!node.prompt.empty()) out << "    prompt: " << node.prompt << "\n";
        if (!node.output_schema.empty()) {
            out << "    output_schema:";
            EmitJsonAsYaml(out, node.output_schema, 6);
        }
        if (!node.skill.empty()) out << "    skill: " << node.skill << "\n";
        if (!node.operation.empty()) out << "    operation: " << node.operation << "\n";
        if (!node.template_path.empty()) out << "    template: " << node.template_path << "\n";
        if (!node.subflow_id.empty()) {
            out << "    subflow: " << node.subflow_id << "\n";
            if (!node.subflow_version.empty()) out << "    subflow_version: " << node.subflow_version << "\n";
        }
        if (!node.branches.empty()) {
            out << "    branches:\n";
            for (const auto& b : node.branches) out << "      - " << b << "\n";
        }
        if (node.kind == NodeKind::Parallel || node.kind == NodeKind::Join) {
            out << "    join: " << ToString(node.join) << "\n";
            if (node.join == JoinPolicy::Quorum) out << "    quorum: " << node.join_quorum << "\n";
        }
        if (node.max_concurrency > 0) out << "    max_concurrency: " << node.max_concurrency << "\n";
        if (!node.items_ref.empty()) out << "    items: " << node.items_ref << "\n";
        if (node.kind == NodeKind::Async && !node.async_body.empty()) {
            out << "    body: " << node.async_body << "\n";
        }
        if (!node.map_body.empty() && (node.kind == NodeKind::Map || node.kind == NodeKind::Foreach)) {
            out << "    body: " << node.map_body << "\n";
        }
        if (!node.reduce_body.empty()) out << "    reduce_body: " << node.reduce_body << "\n";
        if (!node.initial_ref.empty()) out << "    initial: " << node.initial_ref << "\n";
        if (!node.conditions.empty()) {
            out << "    conditions:\n";
            for (const auto& c : node.conditions) {
                out << "      - op: " << ToString(c.op) << "\n";
                out << "        path: " << c.path << "\n";
                if (!c.literal.is_null() && !c.literal.is_object()) {
                    out << "        value: ";
                    EmitScalar(out, c.literal);
                    out << "\n";
                }
                out << "        to: " << c.to << "\n";
            }
        }
        if (!node.default_to.empty()) out << "    default_to: " << node.default_to << "\n";
        if (node.kind == NodeKind::Loop) {
            out << "    body:\n";
            for (const auto& body_id : node.loop_body) out << "      - " << body_id << "\n";
            if (node.loop_until.has_value()) {
                out << "    until:\n";
                out << "      op: " << ToString(node.loop_until->op) << "\n";
                out << "      path: " << node.loop_until->path << "\n";
                if (!node.loop_until->literal.is_null() && !node.loop_until->literal.is_object()) {
                    out << "      value: ";
                    EmitScalar(out, node.loop_until->literal);
                    out << "\n";
                }
            }
            out << "    min_iterations: ";
            EmitScalar(out, node.loop_min_iterations);
            out << "\n    max_iterations: ";
            EmitScalar(out, node.loop_max_iterations);
            out << "\n    hard_limit: " << node.loop_hard_limit << "\n";
        }
        if (!node.input.empty()) {
            out << "    input:";
            EmitJsonAsYaml(out, node.input, 6);
        }
        if (node.retry.has_value()) {
            out << "    retry:\n";
            out << "      attempts: " << node.retry->attempts << "\n";
            out << "      backoff: " << (node.retry->backoff == BackoffKind::Fixed ? "fixed" : "exponential") << "\n";
            // 时长写法与 parser 对齐:ParseDurationSecs 认 "90s/10m",ms 级
            // 精度经毫秒数字写回会按秒误读(round-trip 钉死)。
            out << "      initial: " << node.retry->initial_ms / 1000 << "s\n";
            out << "      max: " << node.retry->max_ms / 1000 << "s\n";
            out << "      jitter: " << (node.retry->jitter ? "true" : "false") << "\n";
        }
        if (node.on_unavailable != OnUnavailable::Fail) {
            out << "    on_unavailable: " << ToString(node.on_unavailable) << "\n";
        }
        if (!node.fallback_to.empty()) out << "    fallback_to: " << node.fallback_to << "\n";
        if (!node.checkpoint) out << "    checkpoint: false\n";
        if (node.has_side_effects) {
            out << "    side_effects: true\n";
            if (!node.idempotency_key.empty()) out << "    idempotency_key: " << node.idempotency_key << "\n";
        }
    }

    if (!def.edges.empty()) {
        out << "edges:\n";
        for (const WorkflowEdge& e : def.edges) {
            out << "  - from: " << e.from << "\n";
            out << "    on: " << e.outcome << "\n";
            out << "    to: " << e.to << "\n";
        }
    }
    if (!def.result.empty()) {
        out << "result:";
        EmitJsonAsYaml(out, def.result, 2);
    }
    return out.str();
}

}  // namespace lubancode::workflow
