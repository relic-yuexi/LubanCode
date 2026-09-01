// AgentDefinition 的 yaml-cpp 严格解析实现。规矩(单子 4.2)逐条落码:
//   未知字段报错、类型不合报文件行列、枚举只认白名单、数组去重保序、
//   诊断不打印密钥与环境变量值。与 workflow/parser.cpp 那套"认不出就跳过"
//   的宽解析相反——Agent 定义是运行配置,拼错一个字段就该当场红,不能
//   带着半懂的意思上线。
#include "agent/agent_definition.hpp"

#include <algorithm>
#include <string>
#include <unordered_set>
#include <utility>

#include <yaml-cpp/yaml.h>

namespace lubancode::agent {

namespace {

// 诊断行:"file:3:5 `tools.allow`: ..."。行列缺席(整文件级问题)就短写,
// 不摆 ":−1:−1" 这种占位数字。
std::string FormatIssueLocation(const std::string& file_label, int line, int column) {
    std::string where = file_label;
    if (line > 0) {
        where += ":" + std::to_string(line);
        if (column > 0) {
            where += ":" + std::to_string(column);
        }
    }
    return where;
}

// 单层 map 的已知字段表:逐键核对,未知键报 error(指到键的行列)。
// 返回 true = 该层没出 error(出 warning 不算)。
bool CheckUnknownFields(const YAML::Node& map, const std::string& prefix,
                        const std::unordered_set<std::string>& known, std::vector<AgentDefinitionIssue>& issues) {
    bool ok = true;
    for (const auto& kv : map) {
        const std::string key = kv.first.Scalar();
        if (known.count(key) != 0) {
            continue;
        }
        const YAML::Mark mark = kv.first.Mark();
        issues.push_back(AgentDefinitionIssue{
            prefix + key,
            "未知字段 \"" + key + "\"(本版 schema " + std::to_string(kAgentDefinitionSchemaVersion) +
                " 不认;写字段前先对 docs 的 Agent schema)",
            mark.line + 1, mark.column + 1, /*warning=*/false});
        ok = false;
    }
    return ok;
}

// 取一层子映射:节点缺失/为空算"省了"(合法);给了但不是映射报 error。
const YAML::Node MapField(const YAML::Node& parent, const char* key, std::vector<AgentDefinitionIssue>& issues,
                          bool* has_error) {
    const YAML::Node node = parent[key];
    if (!node || node.IsNull()) {
        return YAML::Node();
    }
    if (!node.IsMap()) {
        const YAML::Mark mark = node.Mark();
        issues.push_back(AgentDefinitionIssue{key, "必须是映射(mapping),如 \"key:\" 换行缩进两格", mark.line + 1,
                                              mark.column + 1, false});
        *has_error = true;
        return YAML::Node();
    }
    return node;
}

// 取标量字符串字段:节点在但不是标量报 error;空串交给调用方判断必填。
bool ScalarField(const YAML::Node& map, const char* key, const std::string& prefix, std::string& into,
                 std::vector<AgentDefinitionIssue>& issues) {
    const YAML::Node node = map[key];
    if (!node || node.IsNull()) {
        return true;  // 省了,不算错
    }
    if (!node.IsScalar()) {
        const YAML::Mark mark = node.Mark();
        issues.push_back(AgentDefinitionIssue{
            prefix + key, "必须是字符串(标量)", mark.line + 1, mark.column + 1, false});
        return false;
    }
    into = node.Scalar();
    return true;
}

// 取受限枚举字段:值不在白名单报 error(白名单原样进报错文案,不让人猜)。
bool EnumField(const YAML::Node& map, const char* key, const std::string& prefix,
               const std::vector<std::string>& allowed, std::string& into,
               std::vector<AgentDefinitionIssue>& issues) {
    std::string value;
    if (!ScalarField(map, key, prefix, value, issues)) {
        return false;
    }
    if (value.empty()) {
        return true;  // 省了 = 默认值
    }
    if (std::find(allowed.begin(), allowed.end(), value) == allowed.end()) {
        const YAML::Node node = map[key];
        const YAML::Mark mark = node.Mark();
        std::string list;
        for (const std::string& item : allowed) {
            list += list.empty() ? item : (" / " + item);
        }
        issues.push_back(AgentDefinitionIssue{prefix + key, "认不得的值 \"" + value + "\"(只收 " + list + ")",
                                              mark.line + 1, mark.column + 1, false});
        return false;
    }
    into = value;
    return true;
}

// 取字符串序列字段:非序列、非标量项都报 error,项空串也报。去重保序在
// 收集完之后统一做(单子 4.2)。
bool StringListField(const YAML::Node& map, const char* key, const std::string& prefix,
                     std::vector<std::string>& into, std::vector<AgentDefinitionIssue>& issues) {
    const YAML::Node node = map[key];
    if (!node || node.IsNull()) {
        return true;
    }
    if (!node.IsSequence()) {
        const YAML::Mark mark = node.Mark();
        issues.push_back(AgentDefinitionIssue{prefix + key, "必须是字符串列表(每项一行,前加 \"- \")",
                                              mark.line + 1, mark.column + 1, false});
        return false;
    }
    bool ok = true;
    for (const auto& item : node) {
        if (!item.IsScalar()) {
            const YAML::Mark mark = item.Mark();
            issues.push_back(
                AgentDefinitionIssue{prefix + key, "列表项必须是字符串", mark.line + 1, mark.column + 1, false});
            ok = false;
            continue;
        }
        const std::string value = item.Scalar();
        if (value.empty()) {
            const YAML::Mark mark = item.Mark();
            issues.push_back(AgentDefinitionIssue{prefix + key, "列表项不许为空串", mark.line + 1,
                                                  mark.column + 1, false});
            ok = false;
            continue;
        }
        into.push_back(value);
    }
    return ok;
}

// 取整数字段(契约 4.8 的 runtime 预算键):十进制整数标量、落在
// [min_value, max_value] 才收。负号开头、非十进制、越界都报 error 指到
// 行列;int 字段递 int 上限、size_t 字段递 size_t 上限,绝不截断装小。
// 省了不算错(into 保持 nullopt = 继承)。
constexpr long long kIntFieldMax = 2147483647;                    // int 字段上限(runtime_profile 的 int 键)
constexpr long long kSizeFieldMax = 1099511627776LL;              // size_t 字段上限(1 TiB 级,写超这数必是笔误)
bool IntField(const YAML::Node& map, const char* key, const std::string& prefix, long long min_value,
              long long max_value, std::optional<unsigned long long>& into,
              std::vector<AgentDefinitionIssue>& issues) {
    into.reset();  // 省了/出错都先清干净:into 复用时绝不带上一键的旧值
    const YAML::Node node = map[key];
    if (!node || node.IsNull()) {
        return true;  // 省了,不算错
    }
    bool ok = false;
    unsigned long long value = 0;
    if (node.IsScalar()) {
        const std::string text = node.Scalar();
        const std::size_t first = text.find_first_not_of(" \t");
        if (!text.empty() && first != std::string::npos && text[first] != '-') {
            try {
                value = std::stoull(text.substr(first));
                ok = true;
            } catch (...) {
            }
        }
    }
    const YAML::Mark mark = node.Mark();
    if (!ok || static_cast<long long>(value) < min_value) {
        issues.push_back(AgentDefinitionIssue{
            prefix + key, "必须是整数(且不小于 " + std::to_string(min_value) + ")", mark.line + 1,
            mark.column + 1, false});
        return false;
    }
    if (static_cast<long long>(value) > max_value) {
        issues.push_back(AgentDefinitionIssue{
            prefix + key, "整数超上限 " + std::to_string(max_value), mark.line + 1, mark.column + 1, false});
        return false;
    }
    into = value;
    return true;
}

}  // namespace

std::string AgentDefinitionIssue::Format(const std::string& file_label) const {
    const std::string code_part = code.empty() ? std::string() : (" [" + code + "]");
    return FormatIssueLocation(file_label, line, column) + " `" + field + "`" + code_part + ": " + message;
}

bool IsValidAgentName(const std::string& name) {
    if (name.empty() || name.size() > 64) {
        return false;
    }
    if (name.front() == '-' || name.back() == '-') {
        return false;
    }
    char previous = '\0';
    for (const char c : name) {
        const bool ok = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-';
        if (!ok) {
            return false;
        }
        if (c == '-' && previous == '-') {
            return false;  // 横线不连写
        }
        previous = c;
    }
    return true;
}

std::vector<std::string> DedupePreserveOrder(const std::vector<std::string>& items) {
    std::vector<std::string> out;
    std::unordered_set<std::string> seen;
    out.reserve(items.size());
    for (const std::string& item : items) {
        if (seen.insert(item).second) {
            out.push_back(item);
        }
    }
    return out;
}

AgentDefinitionParseResult ParseAgentDefinitionYaml(const std::string& yaml_text,
                                                    const std::string& file_label) {
    AgentDefinitionParseResult result;
    std::vector<AgentDefinitionIssue>& issues = result.issues;
    bool has_error = false;

    YAML::Node root;
    try {
        root = YAML::Load(yaml_text);
    } catch (const YAML::Exception& e) {
        // YAML::Exception 带 mark(0 起);整篇语法坏,谈不上字段。
        const YAML::Mark mark = e.mark;
        issues.push_back(AgentDefinitionIssue{"(yaml)", std::string("YAML 语法解析失败: ") + e.msg, mark.line + 1,
                                              mark.column + 1, false});
        return result;
    }
    if (root.IsNull()) {
        issues.push_back(AgentDefinitionIssue{"(yaml)", "文件是空的:Agent 定义至少要有 schema/name/description",
                                              -1, -1, false});
        return result;
    }
    if (!root.IsMap()) {
        const YAML::Mark mark = root.Mark();
        issues.push_back(AgentDefinitionIssue{"(yaml)", "顶层必须是映射(mapping)", mark.line + 1, mark.column + 1,
                                              false});
        return result;
    }

    has_error = !CheckUnknownFields(root, "", {"schema", "name", "description", "prompt", "model", "skills",
                                               "tools", "mcp_servers", "requires", "runtime", "permissions"},
                                    issues) ||
                has_error;

    AgentDefinition def;

    // ---- schema:必填,只收 1;高版本拒绝并提示升级(单子"兼容与发布") ----
    const YAML::Node schema_node = root["schema"];
    if (!schema_node || schema_node.IsNull()) {
        issues.push_back(AgentDefinitionIssue{"schema", "缺必填字段 schema(本版只收 " +
                                                            std::to_string(kAgentDefinitionSchemaVersion) + ")",
                                              -1, -1, false});
        has_error = true;
    } else {
        int schema_value = 0;
        bool schema_ok = false;
        if (schema_node.IsScalar()) {
            try {
                schema_value = std::stoi(schema_node.Scalar());
                schema_ok = true;
            } catch (...) {
            }
        }
        if (!schema_ok) {
            const YAML::Mark mark = schema_node.Mark();
            issues.push_back(
                AgentDefinitionIssue{"schema", "必须是整数(本版只收 " + std::to_string(kAgentDefinitionSchemaVersion) + ")",
                                     mark.line + 1, mark.column + 1, false});
            has_error = true;
        } else if (schema_value != kAgentDefinitionSchemaVersion) {
            const YAML::Mark mark = schema_node.Mark();
            issues.push_back(AgentDefinitionIssue{
                "schema",
                "schema " + std::to_string(schema_value) + " 本版不认(只收 " +
                    std::to_string(kAgentDefinitionSchemaVersion) + ")" +
                    (schema_value > kAgentDefinitionSchemaVersion
                         ? ";这份定义来自更新的版本,请升级 LubanCode 再用"
                         : ""),
                mark.line + 1, mark.column + 1, false});
            has_error = true;
        }
    }

    // ---- name:必填,小写 kebab-case(词法与 Skill 名同一套) ----
    if (!ScalarField(root, "name", "", def.name, issues)) {
        has_error = true;
    } else if (def.name.empty()) {
        issues.push_back(AgentDefinitionIssue{"name", "缺必填字段 name(小写 kebab-case,如 browser-tester)", -1,
                                              -1, false});
        has_error = true;
    } else if (!IsValidAgentName(def.name)) {
        const YAML::Node node = root["name"];
        const YAML::Mark mark = node.Mark();
        issues.push_back(AgentDefinitionIssue{
            "name",
            "name 只收小写 kebab-case:1-64 个小写字母/数字/横线,横线不顶头、不收尾、不连写(收到 \"" +
                def.name + "\")",
            mark.line + 1, mark.column + 1, false});
        has_error = true;
    }

    // ---- description:必填,写何时派它出场 ----
    if (!ScalarField(root, "description", "", def.description, issues)) {
        has_error = true;
    } else if (def.description.empty()) {
        issues.push_back(AgentDefinitionIssue{"description", "缺必填字段 description(写何时派它出场,主 Agent 派活靠它)",
                                              -1, -1, false});
        has_error = true;
    }

    // ---- prompt ----
    if (const YAML::Node prompt = MapField(root, "prompt", issues, &has_error); prompt) {
        has_error = !CheckUnknownFields(prompt, "prompt.", {"profile", "project_instructions", "soul"}, issues) ||
                    has_error;
        std::string profile;
        if (!ScalarField(prompt, "profile", "prompt.", profile, issues)) {
            has_error = true;
        } else if (!profile.empty()) {
            def.prompt.profile = profile;
        }
        std::string project_instructions;
        if (!EnumField(prompt, "project_instructions", "prompt.", {"inherit", "omit"}, project_instructions,
                       issues)) {
            has_error = true;
        } else if (project_instructions == "omit") {
            def.prompt.project_instructions = AgentPromptSpec::ProjectInstructions::Omit;
        }
        std::string soul;
        if (!EnumField(prompt, "soul", "prompt.", {"inherit", "off"}, soul, issues)) {
            has_error = true;
        } else if (soul == "off") {
            def.prompt.soul = AgentPromptSpec::Soul::Off;
        }
    }

    // ---- model ----
    if (const YAML::Node model = MapField(root, "model", issues, &has_error); model) {
        has_error = !CheckUnknownFields(model, "model.", {"role", "effort"}, issues) || has_error;
        if (!EnumField(model, "role", "model.", {"inherit", "cheap", "normal", "lao"}, def.model.role, issues)) {
            has_error = true;
        }
        // effort 只收非空字符串:档位值(如 minimal/low/high/xhigh)在解析层
        // 不校验能力——是否越过 provider 能力是 doctor 与 resolver 的事。
        if (!ScalarField(model, "effort", "model.", def.model.effort, issues)) {
            has_error = true;
        }
    }

    // ---- skills ----
    if (const YAML::Node skills = MapField(root, "skills", issues, &has_error); skills) {
        has_error = !CheckUnknownFields(skills, "skills.", {"preload"}, issues) || has_error;
        has_error = !StringListField(skills, "preload", "skills.", def.skills_preload, issues) || has_error;
    }

    // ---- tools ----
    if (const YAML::Node tools = MapField(root, "tools", issues, &has_error); tools) {
        has_error = !CheckUnknownFields(tools, "tools.", {"allow", "deny"}, issues) || has_error;
        has_error = !StringListField(tools, "allow", "tools.", def.tools.allow, issues) || has_error;
        has_error = !StringListField(tools, "deny", "tools.", def.tools.deny, issues) || has_error;
    }

    // ---- mcp_servers:只许引用服务名,绝不收内联启动配置(单子安全规矩;
    //     任何非标量项在这里就红,command/args/env 根本进不来) ----
    has_error = !StringListField(root, "mcp_servers", "", def.mcp_servers, issues) || has_error;

    // ---- requires(requires 是 C++20 关键字,变量名叫 requires_node) ----
    if (const YAML::Node requires_node = MapField(root, "requires", issues, &has_error); requires_node) {
        has_error = !CheckUnknownFields(requires_node, "requires.", {"tools"}, issues) || has_error;
        has_error = !StringListField(requires_node, "tools", "requires.", def.requires_tools, issues) ||
                    has_error;
    }

    // ---- runtime(字段名与 AgentRuntimeProfile 一字不差,契约 4.8;见头文件
    //      对齐账)----
    if (const YAML::Node runtime = MapField(root, "runtime", issues, &has_error); runtime) {
        has_error = !CheckUnknownFields(runtime, "runtime.",
                                        {"max_output_tokens", "max_steps_per_turn", "max_turns",
                                         "max_context_chars", "context_window_tokens", "length_continuations",
                                         "execution_mode", "isolation"},
                                        issues) ||
                    has_error;
        // 六个预算键:类型/下界/上界各异,共用 IntField;省了 = 继承(nullopt)。
        std::optional<unsigned long long> value;
        if (!IntField(runtime, "max_output_tokens", "runtime.", 1, kIntFieldMax, value, issues)) {
            has_error = true;
        } else if (value.has_value()) {
            def.max_output_tokens = static_cast<int>(*value);
        }
        if (!IntField(runtime, "max_steps_per_turn", "runtime.", 0, kIntFieldMax, value, issues)) {
            has_error = true;
        } else if (value.has_value()) {
            def.max_steps_per_turn = static_cast<int>(*value);
        }
        // 任务总 turn(turn 预算单 §4.1):非负整数,沿用现有整数硬帽
        //(§3.5:首版不另拍新数)。
        if (!IntField(runtime, "max_turns", "runtime.", 0, kIntFieldMax, value, issues)) {
            has_error = true;
        } else if (value.has_value()) {
            def.max_turns = static_cast<int>(*value);
        }
        if (!IntField(runtime, "max_context_chars", "runtime.", 1, kSizeFieldMax, value, issues)) {
            has_error = true;
        } else if (value.has_value()) {
            def.max_context_chars = static_cast<std::size_t>(*value);
        }
        if (!IntField(runtime, "context_window_tokens", "runtime.", 0, kSizeFieldMax, value, issues)) {
            has_error = true;
        } else if (value.has_value()) {
            def.context_window_tokens = static_cast<std::size_t>(*value);
        }
        if (!IntField(runtime, "length_continuations", "runtime.", 0, kIntFieldMax, value, issues)) {
            has_error = true;
        } else if (value.has_value()) {
            def.length_continuations = static_cast<int>(*value);
        }
        if (!EnumField(runtime, "execution_mode", "runtime.", {"auto", "foreground", "background"},
                       def.execution_mode, issues)) {
            has_error = true;
        }
        if (!EnumField(runtime, "isolation", "runtime.", {"none", "worktree"}, def.isolation, issues)) {
            has_error = true;
        }
    }

    // ---- permissions(契约 4.9:mode 只认 inherit/confirm/auto/yolo) ----
    // read_only 不进首版——只读由 tools.allow 白名单表达(Explore 即此做法),
    // 单独点名指路,别让人猜;其余认不得的值走通用枚举错(agent.bad_enum)。
    if (const YAML::Node permissions = MapField(root, "permissions", issues, &has_error); permissions) {
        has_error = !CheckUnknownFields(permissions, "permissions.", {"mode"}, issues) || has_error;
        const YAML::Node mode = permissions["mode"];
        if (mode && mode.IsScalar() && mode.Scalar() == "read_only") {
            const YAML::Mark mark = mode.Mark();
            issues.push_back(AgentDefinitionIssue{
                "permissions.mode",
                "认不得的值 \"read_only\"(只收 inherit / confirm / auto / yolo);只读限制用 tools.allow 白名单表达",
                mark.line + 1, mark.column + 1, false});
            has_error = true;
        } else if (!EnumField(permissions, "mode", "permissions.", {"inherit", "confirm", "auto", "yolo"},
                              def.permissions_mode, issues)) {
            has_error = true;
        }
    }

    if (has_error) {
        return result;  // 不交半份定义出去
    }

    // 数组去重保序(单子 4.2):此处做,别处不重做。
    def.skills_preload = DedupePreserveOrder(def.skills_preload);
    def.tools.allow = DedupePreserveOrder(def.tools.allow);
    def.tools.deny = DedupePreserveOrder(def.tools.deny);
    def.mcp_servers = DedupePreserveOrder(def.mcp_servers);
    def.requires_tools = DedupePreserveOrder(def.requires_tools);

    result.definition = std::move(def);
    return result;
}

}  // namespace lubancode::agent
