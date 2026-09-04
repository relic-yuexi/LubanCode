// 组件解析的实现(统一 Package 封装单阶段 2)。文件头规矩见 component.hpp:
// 调原生 parser,不复制 schema;包层只补 local id 规矩、名字一致性与
// mcp.yaml(包自己的格式)。整件只诊不执行——Plugin/MCP 的 command 一个
// 不起,Skill 正文不装上下文,Workflow 不排程。
#include "package/component.hpp"

#include <algorithm>
#include <cstring>
#include <fstream>
#include <iterator>
#include <set>
#include <sstream>
#include <system_error>

#include <yaml-cpp/yaml.h>

#include "platform/paths.hpp"
#include "workflow/parser.hpp"

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

std::string RelUtf8(const std::filesystem::path& root, const std::filesystem::path& file) {
    const std::u8string u8 = file.lexically_relative(root).generic_u8string();
    return std::string(reinterpret_cast<const char*>(u8.data()), u8.size());
}

// local id 的包内规矩(packages.md §2):小写 kebab-case,1-64,横线不顶头
// 不收尾不连写。与 Skill/Agent 的原生词法同款,这里给 Profile/Plugin/MCP
// 目录名与 Agent 文件名一道统一口径。
bool IsKebabCaseLocalId(std::string_view id) {
    if (id.empty() || id.size() > 64) return false;
    if (id.front() == '-' || id.back() == '-') return false;
    if (id.find("--") != std::string_view::npos) return false;
    for (const unsigned char ch : id) {
        if ((ch < 'a' || ch > 'z') && (ch < '0' || ch > '9') && ch != '-') return false;
    }
    return true;
}

std::string Trimmed(std::string s) {
    std::size_t begin = 0;
    while (begin < s.size() && std::isspace(static_cast<unsigned char>(s[begin])) != 0) ++begin;
    std::size_t end = s.size();
    while (end > begin && std::isspace(static_cast<unsigned char>(s[end - 1])) != 0) --end;
    return s.substr(begin, end - begin);
}

}  // namespace

std::string_view ComponentKindName(ComponentKind kind) {
    switch (kind) {
        case ComponentKind::Agent: return "agent";
        case ComponentKind::PromptProfile: return "prompt_profile";
        case ComponentKind::Skill: return "skill";
        case ComponentKind::Workflow: return "workflow";
        case ComponentKind::Plugin: return "plugin";
        case ComponentKind::McpServer: return "mcp_server";
        case ComponentKind::Channel: return "channel";
    }
    return "?";
}

std::string_view ComponentKindDir(ComponentKind kind) {
    switch (kind) {
        case ComponentKind::Agent: return "agents";
        case ComponentKind::PromptProfile: return "prompts";
        case ComponentKind::Skill: return "skills";
        case ComponentKind::Workflow: return "workflows";
        case ComponentKind::Plugin: return "plugins";
        case ComponentKind::McpServer: return "mcp";
        case ComponentKind::Channel: return "channels";
    }
    return "?";
}

std::string ComponentIssue::Format() const {
    std::string out = path;
    if (line > 0) {
        out += ":" + std::to_string(line);
        if (column > 0) out += ":" + std::to_string(column);
    }
    if (!out.empty()) out += " ";
    out += error ? "[error] " : "[warn]  ";
    if (!field.empty()) out += "`" + field + "`: ";
    out += message;
    return out;
}

bool ParsedComponent::HasError() const {
    if (!ok) return true;
    for (const auto& issue : issues) {
        if (issue.error) return true;
    }
    return false;
}

// ---------------------------------------------------------------------------
// mcp.yaml(契约 packages.md §5)
// ---------------------------------------------------------------------------

namespace {

// ${env:NAME} 占位:整值必须是这一个形状(契约:只许 ${env:NAME},读宿主
// 已有变量;值不落清单不进日志——诊断只报变量名)。
bool IsEnvPlaceholder(std::string_view value) {
    if (!value.starts_with("${env:") || !value.ends_with("}")) return false;
    const std::string_view name = value.substr(6, value.size() - 7);
    if (name.empty()) return false;
    const auto name_char_ok = [](char ch) {
        return (ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') ||
               (ch >= '0' && ch <= '9') || ch == '_';
    };
    const char first = name.front();
    if (!((first >= 'A' && first <= 'Z') || (first >= 'a' && first <= 'z') || first == '_')) {
        return false;  // 变量名首字符:字母或下划线
    }
    for (const char ch : name) {
        if (!name_char_ok(ch)) return false;
    }
    return true;
}

// 占位符起的尾部展开后仍在包根内?按词法规范化比(不碰盘,symlink 已由
// 盘点层另行记账)。tail 以包根开头(占位符视作路径头),规范化后相对
// 包根若走到 ".." 外面,就是越界。
bool EscapesPackageRoot(const std::string& tail, const std::filesystem::path& package_root) {
    const std::filesystem::path root = package_root.lexically_normal();
    const std::filesystem::path normalized = Utf8ToPath(tail).lexically_normal();
    const std::string rel = PathToUtf8(normalized.lexically_relative(root));
    return rel == ".." || rel.rfind("../", 0) == 0 || rel.rfind("..\\", 0) == 0;
}

// args/env 值里认得的占位符:${package_dir}、${package_data}、${env:NAME}。
// ${project_dir} 首版不认(契约 §5),别的 ${...} 一概报错——不猜。
bool CheckPlaceholders(const std::string& value, const std::filesystem::path& package_root,
                       bool env_value, const std::string& field, int line,
                       std::vector<McpComponentError>& errors) {
    std::size_t pos = 0;
    while (pos < value.size()) {
        const std::size_t hit = value.find("${", pos);
        if (hit == std::string::npos) break;
        const std::size_t close = value.find('}', hit);
        if (close == std::string::npos) {
            errors.push_back({field, line, "占位符没有闭合的 }: " + value.substr(hit)});
            return false;
        }
        const std::string token = value.substr(hit, close - hit + 1);
        if (token == "${package_dir}") {
            if (env_value) {
                errors.push_back({field, line, "env 值只许 ${env:NAME},不许 ${package_dir}"});
            } else {
                const std::string tail =
                    PathToUtf8(package_root.lexically_normal()) + value.substr(close + 1);
                if (EscapesPackageRoot(tail, package_root)) {
                    errors.push_back({field, line, "path_escape: " + token + " 展开后逃出包根"});
                }
            }
        } else if (token == "${package_data}") {
            if (env_value) {
                errors.push_back({field, line, "env 值只许 ${env:NAME},不许 ${package_data}"});
            }
        } else if (token.starts_with("${env:")) {
            if (!env_value) {
                errors.push_back({field, line, "args 里不认 " + token + "(env 占位只进 env 值)"});
            } else if (!IsEnvPlaceholder(token)) {
                errors.push_back({field, line, "坏的环境变量占位: " + token});
            }
        } else if (token == "${project_dir}") {
            errors.push_back({field, line, "${project_dir} 首版不认(见契约占位符规矩)"});
        } else {
            errors.push_back({field, line, "认不得的占位符: " + token});
        }
        pos = close + 1;
    }
    return true;
}

int LineOf(const YAML::Node& node) {
    const YAML::Mark mark = node.Mark();
    return mark.line >= 0 ? mark.line + 1 : 0;
}

}  // namespace

std::expected<McpComponentDefinition, std::vector<McpComponentError>> ParseMcpComponentYaml(
    std::string_view yaml_text, const std::filesystem::path& package_root) {
    std::vector<McpComponentError> errors;
    YAML::Node root;
    try {
        root = YAML::Load(std::string(yaml_text));
    } catch (const YAML::Exception& ex) {
        return std::unexpected(std::vector<McpComponentError>{
            {"(yaml)", ex.mark.line >= 0 ? ex.mark.line + 1 : 0, std::string("YAML 语法坏: ") + ex.what()}});
    }
    if (!root.IsMap()) {
        return std::unexpected(
            std::vector<McpComponentError>{{"(yaml)", 1, "根必须是映射"}});
    }

    static const std::set<std::string> kKnownTop = {"schema", "id", "description", "transport",
                                                    "runtime", "permissions"};
    for (const auto& kv : root) {
        const std::string key = kv.first.as<std::string>();
        if (kKnownTop.count(key) == 0) {
            errors.push_back({"(yaml)", LineOf(kv.first), "未知字段: " + key});
        }
    }

    McpComponentDefinition def;
    if (const auto& n = root["schema"]; n && n.IsScalar()) {
        try {
            def.schema = n.as<int>();
        } catch (const YAML::Exception&) {
            errors.push_back({"schema", LineOf(n), "必须是整数"});
        }
    } else {
        errors.push_back({"schema", LineOf(n), "缺 schema(必填)"});
    }
    if (def.schema != 1 && errors.empty()) {
        errors.push_back({"schema", LineOf(root["schema"]),
                          "只认 1,给了 " + std::to_string(def.schema) + "(不静默猜结构)"});
    }

    const auto read_string = [&](const char* key, std::string& into, bool required) {
        const auto& n = root[key];
        if (!n || !n.IsScalar()) {
            if (required) errors.push_back({key, LineOf(n), "缺必填字符串"});
            return;
        }
        into = n.Scalar();
        if (required && Trimmed(into).empty()) {
            errors.push_back({key, LineOf(n), "不许为空"});
        }
    };
    read_string("id", def.id, true);
    read_string("description", def.description, true);
    read_string("transport", def.transport, true);
    if (!def.transport.empty() && def.transport != "stdio") {
        errors.push_back({"transport", LineOf(root["transport"]),
                          "首版只收 stdio,给了 " + def.transport});
    }
    if (!def.id.empty() && !IsKebabCaseLocalId(def.id)) {
        errors.push_back({"id", LineOf(root["id"]),
                          "id_invalid: 须小写 kebab-case(字母数字单横线),给了 " + def.id});
    }

    const auto& runtime = root["runtime"];
    if (!runtime || !runtime.IsMap()) {
        errors.push_back({"runtime", LineOf(runtime), "缺 runtime 映射(command 必填)"});
    } else {
        for (const auto& kv : runtime) {
            const std::string key = kv.first.as<std::string>();
            if (key != "command" && key != "args" && key != "env" && key != "timeout_ms") {
                errors.push_back({"runtime." + key, LineOf(kv.first), "未知字段"});
            }
        }
        if (const auto& n = runtime["command"]; n && n.IsScalar()) {
            def.command = n.Scalar();
            if (Trimmed(def.command).empty()) {
                errors.push_back({"runtime.command", LineOf(n), "不许为空"});
            }
        } else {
            errors.push_back({"runtime.command", LineOf(runtime["command"]), "缺必填(可执行文件,exec form)"});
        }
        if (const auto& args = runtime["args"]; args && args.IsSequence()) {
            for (const auto& item : args) {
                if (!item.IsScalar()) {
                    errors.push_back({"runtime.args", LineOf(item), "元素必须是字符串"});
                    continue;
                }
                const std::string value = item.Scalar();
                CheckPlaceholders(value, package_root, false, "runtime.args", LineOf(item), errors);
                def.args.push_back(value);
            }
        } else if (args && !args.IsSequence()) {
            errors.push_back({"runtime.args", LineOf(args), "必须是字符串数组"});
        }
        if (const auto& env = runtime["env"]; env && env.IsMap()) {
            for (const auto& kv : env) {
                const std::string name = kv.first.as<std::string>();
                if (!kv.second.IsScalar()) {
                    errors.push_back({"runtime.env." + name, LineOf(kv.second), "值必须是字符串"});
                    continue;
                }
                const std::string value = kv.second.Scalar();
                if (value.find("${") != std::string::npos) {
                    CheckPlaceholders(value, package_root, true, "runtime.env." + name,
                                      LineOf(kv.second), errors);
                    // 契约:env 值整个就是一枚 ${env:NAME} 占位,不夹明文。
                    if (!IsEnvPlaceholder(value)) {
                        errors.push_back({"runtime.env." + name, LineOf(kv.second),
                                          "env 值只许整个写成 ${env:NAME},不夹明文"});
                    }
                } else {
                    // 契约:env 只许 ${env:NAME} 占位。明文字样写进来,按
                    // 结构错报(清单不得写真实密钥)。
                    errors.push_back({"runtime.env." + name, LineOf(kv.second),
                                      "env 值只许 ${env:NAME} 占位,不收明文"});
                }
                def.env.emplace_back(name, value);
            }
        } else if (env && !env.IsMap()) {
            errors.push_back({"runtime.env", LineOf(env), "必须是映射"});
        }
        if (const auto& n = runtime["timeout_ms"]; n && n.IsScalar()) {
            try {
                def.timeout_ms = n.as<int>();
                if (def.timeout_ms < 0) {
                    errors.push_back({"runtime.timeout_ms", LineOf(n), "不许为负"});
                }
            } catch (const YAML::Exception&) {
                errors.push_back({"runtime.timeout_ms", LineOf(n), "必须是整数"});
            }
        }
    }

    if (const auto& perms = root["permissions"]; perms && perms.IsMap()) {
        for (const auto& kv : perms) {
            const std::string key = kv.first.as<std::string>();
            if (key != "network") {
                errors.push_back({"permissions." + key, LineOf(kv.first), "未知字段"});
            }
        }
        if (const auto& n = perms["network"]; n && n.IsScalar()) {
            try {
                def.network_allowed = n.as<bool>();
            } catch (const YAML::Exception&) {
                errors.push_back({"permissions.network", LineOf(n), "必须是布尔"});
            }
        }
    }

    if (!errors.empty()) return std::unexpected(std::move(errors));
    return def;
}

// ---------------------------------------------------------------------------
// 六类 loader
// ---------------------------------------------------------------------------

namespace {

void AddNativeIssues(ParsedComponent& out, const std::vector<agent::AgentDefinitionIssue>& issues) {
    for (const auto& issue : issues) {
        out.issues.push_back(ComponentIssue{!issue.warning, out.rel_path, issue.line, issue.column,
                                            issue.field, issue.message});
    }
}

ParsedComponent ParseAgentComponent(const ComponentSourceRoot& source) {
    ParsedComponent out;
    out.kind = ComponentKind::Agent;
    out.local_id = source.local_id;
    out.canonical_id = source.package_id + ":" + source.local_id;
    out.rel_path = source.rel_path;

    if (!IsKebabCaseLocalId(source.local_id)) {
        out.issues.push_back(ComponentIssue{true, source.rel_path, -1, -1, "(local id)",
                                            "id_invalid: 文件名须小写 kebab-case,给了 " + source.local_id});
    }

    const auto text = ReadFileBytes(source.component_path);
    if (!text.has_value()) {
        out.issues.push_back(ComponentIssue{true, source.rel_path, -1, -1, "(file)", "读不动(权限或占用)"});
        return out;
    }
    const agent::AgentDefinitionParseResult parsed =
        agent::ParseAgentDefinitionYaml(*text, source.rel_path);
    AddNativeIssues(out, parsed.issues);
    if (parsed.definition.has_value()) {
        out.agent = std::move(parsed.definition);
        out.ok = true;
        // 名字一致性:文件名与 name 不一致,warning(契约 agents.md §2,
        // 以 name 为准;local id 记账仍用文件名)。
        if (out.local_id != out.agent->name) {
            out.issues.push_back(ComponentIssue{false, source.rel_path, -1, -1, "name",
                                                "name_mismatch: 文件名 " + out.local_id + " 与 name " +
                                                    out.agent->name + " 不一致(以 name 为准)"});
        }
    }
    return out;
}

ParsedComponent ParseSkillComponent(const ComponentSourceRoot& source) {
    ParsedComponent out;
    out.kind = ComponentKind::Skill;
    out.local_id = source.local_id;
    out.canonical_id = source.package_id + ":" + source.local_id;
    out.rel_path = source.rel_path;

    const std::filesystem::path skill_md = source.component_path / "SKILL.md";
    const std::string rel_md = out.rel_path + "/SKILL.md";
    const auto text = ReadFileBytes(skill_md);
    if (!text.has_value()) {
        out.issues.push_back(ComponentIssue{true, rel_md, -1, -1, "(file)", "读不动(权限或占用)"});
        return out;
    }
    auto parsed = tools::ParseSkillMarkdown(*text);
    if (!parsed.has_value()) {
        out.issues.push_back(
            ComponentIssue{true, rel_md, -1, -1, "(frontmatter)", "frontmatter 损坏(没有闭合的 ---)"});
        return out;
    }
    if (!parsed->name.has_value() || parsed->name->empty()) {
        out.issues.push_back(ComponentIssue{true, rel_md, 2, -1, "name", "缺必填 name"});
        return out;
    }
    if (!parsed->description.has_value() || Trimmed(*parsed->description).empty()) {
        out.issues.push_back(ComponentIssue{true, rel_md, -1, -1, "description", "缺必填 description"});
        return out;
    }
    if (!tools::IsValidAgentSkillName(*parsed->name)) {
        out.issues.push_back(ComponentIssue{
            true, rel_md, 2, -1, "name",
            "id_invalid: name 须小写 kebab-case(1-64,横线不顶头不收尾不连写),给了 " + *parsed->name});
    }
    if (*parsed->name != source.local_id) {
        out.issues.push_back(ComponentIssue{true, rel_md, 2, -1, "name",
                                            "name_mismatch: frontmatter name " + *parsed->name +
                                                " 与目录名 " + source.local_id + " 不一致(canonical id 用目录名)"});
    }
    out.skill = std::move(parsed);
    out.ok = true;
    return out;
}

// workflow.yaml 的 ParseIssue.location 是 "workflow.yaml:42 字段" 或纯字段
// 路径两种;这里换算成包内相对路径前缀,行号抠出来。
ComponentIssue WorkflowIssueToComponent(const ComponentSourceRoot& source,
                                        const workflow::ParseIssue& issue) {
    ComponentIssue out;
    out.error = true;
    std::string location = issue.location;
    int line = -1;
    if (location.rfind("workflow.yaml:", 0) == 0) {
        const std::size_t line_end = location.find(' ', strlen("workflow.yaml:"));
        const std::string line_text = location.substr(strlen("workflow.yaml:"),
            line_end == std::string::npos ? std::string::npos : line_end - strlen("workflow.yaml:"));
        try {
            line = std::stoi(line_text);
        } catch (...) {
            line = -1;
        }
        if (line_end != std::string::npos) {
            location = Trimmed(location.substr(line_end + 1));
        } else {
            location.clear();
        }
    }
    out.path = source.rel_path + "/workflow.yaml";
    out.line = line;
    out.field = location;
    out.message = issue.message;
    return out;
}

ParsedComponent ParseWorkflowComponent(const ComponentSourceRoot& source) {
    ParsedComponent out;
    out.kind = ComponentKind::Workflow;
    out.local_id = source.local_id;
    out.canonical_id = source.package_id + ":" + source.local_id;
    out.rel_path = source.rel_path;

    const std::filesystem::path workflow_yaml = source.component_path / "workflow.yaml";
    auto parsed = workflow::LoadWorkflowDefinition(workflow_yaml);
    if (!parsed.has_value()) {
        for (const auto& issue : parsed.error()) {
            out.issues.push_back(WorkflowIssueToComponent(source, issue));
        }
        return out;
    }
    if (parsed->id.empty()) {
        out.issues.push_back(ComponentIssue{true, source.rel_path + "/workflow.yaml", -1, -1, "id",
                                            "缺 id"});
    } else if (parsed->id != source.local_id) {
        out.issues.push_back(
            ComponentIssue{true, source.rel_path + "/workflow.yaml", -1, -1, "id",
                           "name_mismatch: workflow id " + parsed->id + " 与目录名 " + source.local_id +
                               " 不一致(canonical id 用目录名)"});
    }
    out.workflow = std::move(*parsed);
    out.ok = true;
    return out;
}

ParsedComponent ParsePluginComponent(const ComponentSourceRoot& source) {
    ParsedComponent out;
    out.kind = ComponentKind::Plugin;
    out.local_id = source.local_id;
    out.canonical_id = source.package_id + ":" + source.local_id;
    out.rel_path = source.rel_path;

    if (!IsKebabCaseLocalId(source.local_id)) {
        out.issues.push_back(ComponentIssue{true, source.rel_path, -1, -1, "(local id)",
                                            "id_invalid: 目录名须小写 kebab-case(点号是命名空间分隔符),给了 " +
                                                source.local_id});
    }

    const std::filesystem::path manifest_path = source.component_path / "plugin.json";
    const auto text = ReadFileBytes(manifest_path);
    if (!text.has_value()) {
        out.issues.push_back(ComponentIssue{true, source.rel_path + "/plugin.json", -1, -1, "(file)",
                                            "读不动(权限或占用)"});
        return out;
    }
    auto parsed = runtime::ParsePluginManifest(*text, source.component_path);
    if (!parsed.has_value()) {
        out.issues.push_back(
            ComponentIssue{true, source.rel_path + "/plugin.json", -1, -1, "(manifest)", parsed.error()});
        return out;
    }
    if (parsed->id != source.local_id) {
        out.issues.push_back(
            ComponentIssue{true, source.rel_path + "/plugin.json", -1, -1, "id",
                           "name_mismatch: plugin id " + parsed->id + " 与目录名 " + source.local_id +
                               " 不一致(契约:须与目录名一致)"});
    }
    out.plugin = std::move(*parsed);
    out.ok = true;
    return out;
}

ParsedComponent ParseMcpComponent(const ComponentSourceRoot& source) {
    ParsedComponent out;
    out.kind = ComponentKind::McpServer;
    out.local_id = source.local_id;
    out.canonical_id = source.package_id + ":" + source.local_id;
    out.rel_path = source.rel_path;

    const std::filesystem::path mcp_yaml = source.component_path / "mcp.yaml";
    const auto text = ReadFileBytes(mcp_yaml);
    if (!text.has_value()) {
        out.issues.push_back(ComponentIssue{true, source.rel_path + "/mcp.yaml", -1, -1, "(file)",
                                            "读不动(权限或占用)"});
        return out;
    }
    auto parsed = ParseMcpComponentYaml(*text, source.package_root);
    if (!parsed.has_value()) {
        for (const auto& error : parsed.error()) {
            out.issues.push_back(
                ComponentIssue{true, source.rel_path + "/mcp.yaml", error.line, -1, error.field,
                               error.detail});
        }
        return out;
    }
    if (!parsed->id.empty() && parsed->id != source.local_id) {
        out.issues.push_back(ComponentIssue{true, source.rel_path + "/mcp.yaml", -1, -1, "id",
                                            "name_mismatch: mcp id " + parsed->id + " 与目录名 " +
                                                source.local_id + " 不一致(契约:须与目录名一致)"});
    }
    out.mcp = std::move(*parsed);
    out.ok = true;
    return out;
}

// channel.yaml(多渠道消息接入单阶段 1;契约 docs/architecture/channels/
// channel-manifest.md)。与 ParseMcpComponent 同一件事的另一份:local id
// 规矩 + 名字一致性(channel.yaml:id 与目录名一致——冻结文档没有明文要求
// 这条,但同一渠道 id 全局唯一、canonical id 由目录名给出,两边不一致会
// 让"对外渠道 id"与"包内引用名"永久错位,照 Plugin/MCP 先例一并拦。
// runtime.command 是否可执行、账号凭据、Package 信任门——这些不是静态
// 解析管的账,阶段 1 只诊不执行(channel-manifest.md §5 十关,这里只占
// "static parse"一关)。
ParsedComponent ParseChannelComponent(const ComponentSourceRoot& source) {
    ParsedComponent out;
    out.kind = ComponentKind::Channel;
    out.local_id = source.local_id;
    out.canonical_id = source.package_id + ":" + source.local_id;
    out.rel_path = source.rel_path;

    if (!IsKebabCaseLocalId(source.local_id)) {
        out.issues.push_back(ComponentIssue{true, source.rel_path, -1, -1, "(local id)",
                                            "id_invalid: 目录名须小写 kebab-case,给了 " + source.local_id});
    }

    const std::filesystem::path manifest_path = source.component_path / "channel.yaml";
    const auto text = ReadFileBytes(manifest_path);
    if (!text.has_value()) {
        out.issues.push_back(ComponentIssue{true, source.rel_path + "/channel.yaml", -1, -1, "(file)",
                                            "读不动(权限或占用)"});
        return out;
    }
    auto parsed =
        channel::ParseChannelManifestYaml(*text, source.package_root, source.component_path);
    if (!parsed.has_value()) {
        for (const auto& error : parsed.error()) {
            out.issues.push_back(ComponentIssue{true, source.rel_path + "/channel.yaml", error.line, -1,
                                                error.field, error.detail});
        }
        return out;
    }
    if (!parsed->id.empty() && parsed->id != source.local_id) {
        out.issues.push_back(ComponentIssue{true, source.rel_path + "/channel.yaml", -1, -1, "id",
                                            "name_mismatch: channel.yaml id " + parsed->id + " 与目录名 " +
                                                source.local_id + " 不一致(契约:须与目录名一致)"});
    }
    out.channel = std::move(*parsed);
    out.ok = true;
    return out;
}

// Prompt Profile:目录整体即组件,无单一入口。原生规矩(agents.md §6.3)
// 是"core/features/platforms 可换,modes 不可"。逐文件记账;local id 规矩
// 同其余五类。
ParsedComponent ParsePromptProfileComponent(const ComponentSourceRoot& source) {
    ParsedComponent out;
    out.kind = ComponentKind::PromptProfile;
    out.local_id = source.local_id;
    out.canonical_id = source.package_id + ":" + source.local_id;
    out.rel_path = source.rel_path;

    if (!IsKebabCaseLocalId(source.local_id)) {
        out.issues.push_back(ComponentIssue{true, source.rel_path, -1, -1, "(local id)",
                                            "id_invalid: profile 目录名须小写 kebab-case,给了 " +
                                                source.local_id});
    }

    std::vector<std::filesystem::path> files;
    std::error_code ec;
    for (auto it = std::filesystem::recursive_directory_iterator(source.component_path,
                                                                 std::filesystem::directory_options::none, ec);
         it != std::filesystem::recursive_directory_iterator(); it.increment(ec)) {
        if (ec) break;
        if (it->is_regular_file()) files.push_back(it->path());
    }
    std::sort(files.begin(), files.end(), [](const auto& a, const auto& b) {
        return PathToUtf8(a) < PathToUtf8(b);
    });

    if (files.empty()) {
        out.issues.push_back(ComponentIssue{false, source.rel_path, -1, -1, "(dir)",
                                            "profile 目录没有任何文件,覆盖为空"});
    }
    for (const auto& file : files) {
        const std::string rel = RelUtf8(source.package_root, file);
        if (!rel.ends_with(".md")) {
            out.issues.push_back(ComponentIssue{false, rel, -1, -1, "(file)",
                                                "不是 .md,不进覆盖(保留在包里)"});
            continue;
        }
        // rel 形如 prompts/profiles/<profile>/<模块树>/...。跳过前缀与
        // profile 名,取模块树根(modes 在这里现形)。
        const std::string after_prefix = rel.substr(strlen("prompts/profiles/"));
        const std::size_t name_slash = after_prefix.find('/');
        const std::string after_name =
            name_slash == std::string::npos ? std::string() : after_prefix.substr(name_slash + 1);
        const std::size_t module_slash = after_name.find('/');
        const std::string module_root = module_slash == std::string::npos
                                            ? after_name
                                            : after_name.substr(0, module_slash);
        if (module_root == "modes") {
            out.issues.push_back(ComponentIssue{true, rel, -1, -1, "(module)",
                                                "modes/ 是宿主策略,Profile 换不得(agents.md §6.3)"});
            continue;
        }
        if (module_root != "core" && module_root != "features" && module_root != "platforms") {
            out.issues.push_back(ComponentIssue{false, rel, -1, -1, "(module)",
                                                "未知模块树 " + module_root + "/,覆盖只认 core/features/platforms"});
        }
        out.profile_files.push_back(rel);
    }
    out.ok = true;
    return out;
}

}  // namespace

ParsedComponent ParsePackageComponent(const ComponentSourceRoot& source) {
    ParsedComponent out = [&]() {
        switch (source.kind) {
            case ComponentKind::Agent: return ParseAgentComponent(source);
            case ComponentKind::PromptProfile: return ParsePromptProfileComponent(source);
            case ComponentKind::Skill: return ParseSkillComponent(source);
            case ComponentKind::Workflow: return ParseWorkflowComponent(source);
            case ComponentKind::Plugin: return ParsePluginComponent(source);
            case ComponentKind::McpServer: return ParseMcpComponent(source);
            case ComponentKind::Channel: return ParseChannelComponent(source);
        }
        return ParsedComponent{};
    }();
    // ok 的口径:原生 parser 解析过,且包层没有补出 error(warning 不挡)。
    // HasError() 同义,两处一起守,别漂。
    if (out.ok) {
        for (const auto& issue : out.issues) {
            if (issue.error) {
                out.ok = false;
                break;
            }
        }
    }
    return out;
}

}  // namespace lubancode::package
