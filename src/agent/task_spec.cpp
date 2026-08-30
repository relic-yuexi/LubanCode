// task_spec.hpp 的实现:parse/validate/canonicalize/render 四件,纯函数。
#include "agent/task_spec.hpp"

#include <algorithm>
#include <sstream>
#include <utility>

#include "hooks/hash.hpp"

namespace lubancode::agent {

namespace {

// ---- 字符串小件 ----

bool HasNul(const std::string& text) {
    return text.find('\0') != std::string::npos;
}

std::string Trimmed(const std::string& text) {
    const std::size_t first = text.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) {
        return std::string();
    }
    const std::size_t last = text.find_last_not_of(" \t\r\n");
    return text.substr(first, last - first + 1);
}

// 字符串数组组的解析与校验:错类型/空串/NUL/超条数/超单条,都按
// "<path>[i] <原因>" 报。返回假时 error_out 已写好。
bool ParseStringGroup(const nlohmann::json& parent, const char* key, const char* path_prefix,
                      std::vector<std::string>& out, std::string& error_out) {
    if (!parent.contains(key)) {
        return true;  // 可选段
    }
    const nlohmann::json& value = parent.at(key);
    if (value.is_null()) {
        return true;  // null 视为未填
    }
    if (!value.is_array()) {
        error_out = std::string(path_prefix) + "." + key + " 必须是字符串数组";
        return false;
    }
    if (value.size() > kMaxTaskSpecItems) {
        error_out = std::string(path_prefix) + "." + key + " 条数超限(至多 " +
                    std::to_string(kMaxTaskSpecItems) + " 条)";
        return false;
    }
    out.reserve(value.size());
    for (std::size_t i = 0; i < value.size(); ++i) {
        const nlohmann::json& item = value[i];
        if (!item.is_string()) {
            error_out = std::string(path_prefix) + "." + key + "[" + std::to_string(i) +
                        "] 必须是非空字符串";
            return false;
        }
        std::string text = item.get<std::string>();
        if (Trimmed(text).empty()) {
            error_out = std::string(path_prefix) + "." + key + "[" + std::to_string(i) +
                        "] 必须是非空字符串";
            return false;
        }
        if (HasNul(text)) {
            error_out = std::string(path_prefix) + "." + key + "[" + std::to_string(i) +
                        "] 不允许包含 NUL 字符";
            return false;
        }
        if (text.size() > kMaxTaskSpecItemBytes) {
            error_out = std::string(path_prefix) + "." + key + "[" + std::to_string(i) +
                        "] 超长(单条上限 " + std::to_string(kMaxTaskSpecItemBytes) + " 字节)";
            return false;
        }
        out.push_back(std::move(text));
    }
    return true;
}

// 必填非空字符串段:缺/类型错/空白/NUL/超长按 "<path> <原因>" 报。
bool ParseRequiredText(const nlohmann::json& parent, const char* key, const char* path_prefix,
                       std::string& out, std::string& error_out) {
    if (!parent.contains(key) || parent.at(key).is_null()) {
        error_out = std::string(path_prefix) + "." + key + " 必填(非空字符串)";
        return false;
    }
    if (!parent.at(key).is_string()) {
        error_out = std::string(path_prefix) + "." + key + " 必须是非空字符串";
        return false;
    }
    std::string text = parent.at(key).get<std::string>();
    if (Trimmed(text).empty()) {
        error_out = std::string(path_prefix) + "." + key + " 不能是空白";
        return false;
    }
    if (HasNul(text)) {
        error_out = std::string(path_prefix) + "." + key + " 不允许包含 NUL 字符";
        return false;
    }
    if (text.size() > kMaxTaskSpecItemBytes) {
        error_out = std::string(path_prefix) + "." + key + " 超长(上限 " +
                    std::to_string(kMaxTaskSpecItemBytes) + " 字节)";
        return false;
    }
    out = std::move(text);
    return true;
}

// 多行文本压成逐行 "- " 列表;单行原样。字段内换行统一逐行加 "- ",不让
// 内容伪造新栏(单子 §5.4 渲染规则)。
void AppendBulletSection(std::string& out, const char* heading, const std::vector<std::string>& items) {
    if (items.empty()) {
        return;
    }
    out += "\n\n";
    out += heading;
    for (const std::string& item : items) {
        out += "\n- ";
        out += item;
    }
}

void AppendTextSection(std::string& out, const char* heading, const std::string& text) {
    if (text.empty()) {
        return;
    }
    out += "\n\n";
    out += heading;
    out += "\n";
    out += text;
}

}  // namespace

AgentTaskSpecParseResult ParseAgentTaskSpec(const nlohmann::json& task, const std::string& title) {
    AgentTaskSpecParseResult result;
    AgentTaskSpec spec;
    spec.title = title;
    if (task.is_null()) {
        result.error = "task 对象缺失";
        return result;
    }
    if (!task.is_object()) {
        result.error = "task 必须是对象";
        return result;
    }
    if (task.contains("schema_version") && !task.at("schema_version").is_null()) {
        if (!task.at("schema_version").is_number_integer()) {
            result.error = "task.schema_version 必须是整数(当前支持 1)";
            return result;
        }
        const int version = task.at("schema_version").get<int>();
        if (version != 1) {
            result.error = "task.schema_version 不支持(" + std::to_string(version) +
                           ",当前支持 1)";
            return result;
        }
        spec.schema_version = version;
    }
    if (!ParseRequiredText(task, "goal", "task", spec.goal, result.error)) {
        return result;
    }
    if (!ParseRequiredText(task, "deliverable", "task", spec.deliverable, result.error)) {
        return result;
    }
    if (task.contains("source_request") && !task.at("source_request").is_null()) {
        if (!task.at("source_request").is_string()) {
            result.error = "task.source_request 必须是字符串(只放用户原话摘录)";
            return result;
        }
        std::string text = task.at("source_request").get<std::string>();
        if (Trimmed(text).empty()) {
            result.error = "task.source_request 不能是空白(没有就整段省略)";
            return result;
        }
        if (HasNul(text) || text.size() > kMaxTaskSpecItemBytes) {
            result.error = "task.source_request 不允许 NUL,长度上限 " +
                           std::to_string(kMaxTaskSpecItemBytes) + " 字节";
            return result;
        }
        spec.source_request = std::move(text);
    }
    if (!ParseStringGroup(task, "context", "task", spec.context, result.error)) {
        return result;
    }
    if (!ParseStringGroup(task, "constraints", "task", spec.constraints, result.error)) {
        return result;
    }
    if (!ParseStringGroup(task, "acceptance", "task", spec.acceptance, result.error)) {
        return result;
    }
    if (task.contains("scope") && !task.at("scope").is_null()) {
        const nlohmann::json& scope = task.at("scope");
        if (!scope.is_object()) {
            result.error = "task.scope 必须是对象";
            return result;
        }
        if (!ParseStringGroup(scope, "include_paths", "task.scope", spec.scope.include_paths,
                              result.error)) {
            return result;
        }
        if (!ParseStringGroup(scope, "exclude_paths", "task.scope", spec.scope.exclude_paths,
                              result.error)) {
            return result;
        }
    }
    const std::string validate_error = ValidateAgentTaskSpec(spec);
    if (!validate_error.empty()) {
        result.error = validate_error;
        return result;
    }
    result.spec = std::move(spec);
    return result;
}

AgentTaskSpec CanonicalizeLegacyPrompt(const std::string& title, const std::string& prompt) {
    AgentTaskSpec spec;
    spec.title = title;
    spec.goal = prompt;
    spec.deliverable = "按任务说明交付结果";
    spec.legacy_prompt = true;
    return spec;
}

std::string ValidateAgentTaskSpec(const AgentTaskSpec& spec) {
    if (spec.title.empty()) {
        return "title 必填";
    }
    if (Trimmed(spec.goal).empty()) {
        return "task.goal 必填(非空字符串)";
    }
    if (HasNul(spec.goal)) {
        return "task.goal 不允许包含 NUL 字符";
    }
    if (Trimmed(spec.deliverable).empty()) {
        return "task.deliverable 必填(非空字符串)";
    }
    if (HasNul(spec.deliverable)) {
        return "task.deliverable 不允许包含 NUL 字符";
    }
    const auto check_group = [](const std::vector<std::string>& items,
                                const char* path) -> std::string {
        if (items.size() > kMaxTaskSpecItems) {
            return std::string(path) + " 条数超限(至多 " + std::to_string(kMaxTaskSpecItems) + " 条)";
        }
        for (std::size_t i = 0; i < items.size(); ++i) {
            if (Trimmed(items[i]).empty()) {
                return std::string(path) + "[" + std::to_string(i) + "] 必须是非空字符串";
            }
            if (HasNul(items[i])) {
                return std::string(path) + "[" + std::to_string(i) + "] 不允许包含 NUL 字符";
            }
            if (items[i].size() > kMaxTaskSpecItemBytes) {
                return std::string(path) + "[" + std::to_string(i) + "] 超长(单条上限 " +
                       std::to_string(kMaxTaskSpecItemBytes) + " 字节)";
            }
        }
        return std::string();
    };
    if (const std::string error = check_group(spec.context, "task.context"); !error.empty()) {
        return error;
    }
    if (const std::string error = check_group(spec.constraints, "task.constraints"); !error.empty()) {
        return error;
    }
    if (const std::string error = check_group(spec.acceptance, "task.acceptance"); !error.empty()) {
        return error;
    }
    if (const std::string error = check_group(spec.scope.include_paths, "task.scope.include_paths");
        !error.empty()) {
        return error;
    }
    if (const std::string error = check_group(spec.scope.exclude_paths, "task.scope.exclude_paths");
        !error.empty()) {
        return error;
    }
    if (CanonicalSpecJson(spec).dump().size() > kMaxTaskSpecBytes) {
        return "task 整份超长(上限 " + std::to_string(kMaxTaskSpecBytes) + " 字节)";
    }
    return std::string();
}

nlohmann::json CanonicalSpecJson(const AgentTaskSpec& spec) {
    nlohmann::json out = nlohmann::json::object();
    out["schema_version"] = spec.schema_version;
    out["title"] = spec.title;
    out["goal"] = spec.goal;
    if (spec.source_request.has_value()) {
        out["source_request"] = *spec.source_request;
    }
    if (!spec.context.empty()) {
        out["context"] = spec.context;
    }
    if (!spec.scope.empty()) {
        nlohmann::json scope = nlohmann::json::object();
        if (!spec.scope.include_paths.empty()) {
            scope["include_paths"] = spec.scope.include_paths;
        }
        if (!spec.scope.exclude_paths.empty()) {
            scope["exclude_paths"] = spec.scope.exclude_paths;
        }
        out["scope"] = std::move(scope);
    }
    if (!spec.constraints.empty()) {
        out["constraints"] = spec.constraints;
    }
    if (!spec.acceptance.empty()) {
        out["acceptance"] = spec.acceptance;
    }
    out["deliverable"] = spec.deliverable;
    // legacy_prompt 是过渡账,不进 canonical 合同本体(渲染与 hash 都不含它);
    // 需要时从 AgentTool 的派工统计口读。
    return out;
}

std::string RenderDelegatedTask(const AgentTaskSpec& spec) {
    std::string out = "[委派任务 v" + std::to_string(spec.schema_version) + "]\n标题:" + spec.title;
    AppendTextSection(out, "目标", spec.goal);
    if (spec.source_request.has_value()) {
        AppendTextSection(out, "用户原话摘录", *spec.source_request);
    }
    AppendBulletSection(out, "已知上下文", spec.context);
    if (!spec.scope.empty()) {
        AppendBulletSection(out, "范围(包含)", spec.scope.include_paths);
        AppendBulletSection(out, "范围(排除)", spec.scope.exclude_paths);
    }
    AppendBulletSection(out, "约束", spec.constraints);
    AppendBulletSection(out, "验收", spec.acceptance);
    AppendTextSection(out, "交付", spec.deliverable);
    if (spec.legacy_prompt) {
        // legacy 路不添栏——渲染与结构化路同一形状,模型侧不需要知道这是旧账。
        (void)0;
    }
    return out;
}

std::string TaskSpecHash(const AgentTaskSpec& spec) {
    const std::string full = lubancode::hooks::Sha256Hex(CanonicalSpecJson(spec).dump());
    return full.size() > 16 ? full.substr(0, 16) : full;
}

}  // namespace lubancode::agent
