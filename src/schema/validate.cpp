// Schema 校验核心实现:原语与递归验证器全在此,四个领域入口(钩子改参
// 复验/插件合同/workflow 入参/workflow 产物)只留顶层政策与人话文案的
// 适配层。档位语义与差异矩阵见头注释与合同测试。
#include "schema/validate.hpp"

#include <cstddef>
#include <cstdint>
#include <string_view>
#include <utility>

namespace lubancode::schema {

namespace {

// 类型原语:原先 tools 与 plugin 各抄一份,收口后只此一家。认不得的
// 类型声明不拦——从严校验拦"值不对",不拦"声明怪";type:"null" 验
// 不验由档位定(workflow 旧扫描链没有 null 分支)。
bool JsonTypeMatches(const nlohmann::json& value, const std::string& expected, bool enforce_null_type) {
    if (expected == "string") {
        return value.is_string();
    }
    if (expected == "number") {
        return value.is_number();
    }
    if (expected == "integer") {
        return value.is_number_integer();
    }
    if (expected == "boolean") {
        return value.is_boolean();
    }
    if (expected == "array") {
        return value.is_array();
    }
    if (expected == "object") {
        return value.is_object();
    }
    if (expected == "null") {
        return !enforce_null_type || value.is_null();
    }
    return true;
}

// UTF-8 码点计数(长度界按码点算,与 JSON Schema 语义一致)。入参在
// 协议层已保证合法 UTF-8,这里只数不验。
std::size_t CodePointCount(std::string_view text) {
    std::size_t count = 0;
    for (const unsigned char c : text) {
        if ((c & 0xC0) != 0x80) {
            ++count;
        }
    }
    return count;
}

void Push(std::vector<Finding>& out, Finding::Code code, std::string path, std::string field = std::string(),
          std::string expected = std::string(), std::string actual = std::string()) {
    out.push_back(Finding{code, std::move(path), std::move(field), std::move(expected), std::move(actual)});
}

// 本层标量关键字(与插件旧验证器同序:type → const → enum → 数值界 →
// 字符串长度界 → 数组界)。schema 非对象不查——没给 schema 的位置无从
// 校验。收集不短路,产出序即旧码的首错序。
void CollectScalarFindings(const nlohmann::json& value, const nlohmann::json& schema, const std::string& path,
                           const Profile& profile, std::vector<Finding>& out) {
    if (!schema.is_object()) {
        return;
    }
    if (const auto type = schema.find("type"); type != schema.end() && type->is_string()) {
        const std::string& expected = type->get_ref<const std::string&>();
        if (!JsonTypeMatches(value, expected, profile.enforce_null_type)) {
            Push(out, Finding::Code::TypeMismatch, path, std::string(), expected, value.type_name());
        }
    }
    if (profile.check_const) {
        if (const auto fixed = schema.find("const"); fixed != schema.end() && value != *fixed) {
            Push(out, Finding::Code::ConstMismatch, path, std::string(), fixed->dump());
        }
    }
    if (profile.check_enum) {
        if (const auto enumeration = schema.find("enum");
            enumeration != schema.end() && enumeration->is_array()) {
            bool in_enum = false;
            for (const auto& allowed : *enumeration) {
                if (value == allowed) {
                    in_enum = true;
                    break;
                }
            }
            if (!in_enum) {
                Push(out, Finding::Code::EnumMismatch, path);
            }
        }
    }
    if (profile.check_numeric_bounds && value.is_number()) {
        const double number = value.get<double>();
        if (const auto minimum = schema.find("minimum"); minimum != schema.end() && minimum->is_number()) {
            if (number < minimum->get<double>()) {
                Push(out, Finding::Code::BelowMinimum, path, std::string(), minimum->dump());
            }
        }
        if (const auto maximum = schema.find("maximum"); maximum != schema.end() && maximum->is_number()) {
            if (number > maximum->get<double>()) {
                Push(out, Finding::Code::AboveMaximum, path, std::string(), maximum->dump());
            }
        }
    }
    if (profile.check_string_length && value.is_string()) {
        const std::size_t length = CodePointCount(value.get_ref<const std::string&>());
        if (const auto min_length = schema.find("minLength");
            min_length != schema.end() && min_length->is_number_integer()) {
            if (length < static_cast<std::size_t>(min_length->get<std::int64_t>())) {
                Push(out, Finding::Code::BelowMinLength, path, std::string(), min_length->dump());
            }
        }
        if (const auto max_length = schema.find("maxLength");
            max_length != schema.end() && max_length->is_number_integer()) {
            if (length > static_cast<std::size_t>(max_length->get<std::int64_t>())) {
                Push(out, Finding::Code::AboveMaxLength, path, std::string(), max_length->dump());
            }
        }
    }
    if (profile.check_array_bounds && value.is_array()) {
        const std::size_t size = value.size();
        if (const auto min_items = schema.find("minItems");
            min_items != schema.end() && min_items->is_number_integer()) {
            if (size < static_cast<std::size_t>(min_items->get<std::int64_t>())) {
                Push(out, Finding::Code::BelowMinItems, path, std::string(), min_items->dump());
            }
        }
        if (const auto max_items = schema.find("maxItems");
            max_items != schema.end() && max_items->is_number_integer()) {
            if (size > static_cast<std::size_t>(max_items->get<std::int64_t>())) {
                Push(out, Finding::Code::AboveMaxItems, path, std::string(), max_items->dump());
            }
        }
    }
}

void CollectValue(const nlohmann::json& value, const nlohmann::json& schema, const std::string& path, int depth,
                  const Profile& profile, std::vector<Finding>& out);

// 对象合同分支:required → 属性逐键(入参键序,与插件旧码同向)→
// additionalProperties。递归档往子 schema 走(深度+1);浅档就地查标量,
// 不下去——与 tools/workflow 旧码同口径。
void CollectObjectBranch(const nlohmann::json& value, const nlohmann::json& schema, const std::string& path,
                         int depth, const Profile& profile, std::vector<Finding>& out) {
    if (!schema.is_object()) {
        return;
    }
    if (const auto required = schema.find("required"); required != schema.end() && required->is_array()) {
        for (const auto& key : *required) {
            if (!key.is_string()) {
                continue;
            }
            const auto found = value.find(key.get<std::string>());
            if (found == value.end() || (profile.null_as_missing && found->is_null())) {
                Push(out, Finding::Code::MissingRequired, path, key.get<std::string>());
            }
        }
    }
    if (!value.is_object()) {
        return;  // 属性遍历按键走;非对象没有键可对(旧码同口径)
    }
    const auto properties = schema.find("properties");
    const bool has_properties = properties != schema.end() && properties->is_object();
    const auto additional = schema.find("additionalProperties");
    const bool ap_false =
        profile.check_additional_properties && additional != schema.end() && additional->is_boolean() &&
        !additional->get<bool>();
    const bool ap_schema =
        profile.check_additional_properties && additional != schema.end() && additional->is_object();
    for (auto it = value.begin(); it != value.end(); ++it) {
        const std::string child_path = path.empty() ? it.key() : path + "." + it.key();
        if (has_properties) {
            const auto prop = properties->find(it.key());
            if (prop != properties->end()) {
                if (profile.null_as_missing && it->is_null()) {
                    continue;  // workflow 档:null 当缺失,属性检查跳过
                }
                if (profile.recursive) {
                    CollectValue(it.value(), prop.value(), child_path, depth + 1, profile, out);
                } else {
                    CollectScalarFindings(it.value(), prop.value(), child_path, profile, out);
                }
                continue;
            }
        }
        if (!profile.check_additional_properties) {
            continue;  // 浅档默认放行:properties 之外的键不管
        }
        if (ap_false) {
            Push(out, Finding::Code::AdditionalProperty, child_path, it.key());
            continue;
        }
        if (ap_schema) {
            if (profile.recursive) {
                CollectValue(it.value(), *additional, child_path, depth + 1, profile, out);
            } else {
                CollectScalarFindings(it.value(), *additional, child_path, profile, out);
            }
        }
    }
}

// 深度帽最先看(与插件旧码同位),再看本层标量,最后按档位下数组/对象。
void CollectValue(const nlohmann::json& value, const nlohmann::json& schema, const std::string& path, int depth,
                  const Profile& profile, std::vector<Finding>& out) {
    if (depth > profile.max_depth) {
        Push(out, Finding::Code::DepthExceeded, path, std::string(), std::to_string(profile.max_depth));
        return;
    }
    CollectScalarFindings(value, schema, path, profile, out);
    if (profile.recursive && value.is_array()) {
        if (const auto items = schema.find("items"); items != schema.end() && items->is_object()) {
            for (std::size_t i = 0; i < value.size(); ++i) {
                CollectValue(value[i], *items, path + "[" + std::to_string(i) + "]", depth + 1, profile, out);
            }
        }
    }
    if (value.is_object()) {
        CollectObjectBranch(value, schema, path, depth, profile, out);
    }
}

}  // namespace

Profile HookRewriteProfile() {
    // 钩子改参复验:浅扫一层;required 只看键在不在;属性查 type+enum;
    // 嵌套/const/界/additionalProperties 不查。
    Profile profile;
    profile.check_enum = true;
    return profile;
}

Profile PluginContractProfile() {
    // 插件合同:manifest 是合同,子集全落、递归、深度 32,声明怪也拦
    // (插件作者是外人,合同写岔了要在调用期看得见)。
    Profile profile;
    profile.recursive = true;
    profile.max_depth = 32;
    profile.check_const = true;
    profile.check_enum = true;
    profile.check_numeric_bounds = true;
    profile.check_string_length = true;
    profile.check_array_bounds = true;
    profile.check_additional_properties = true;
    return profile;
}

Profile WorkflowScanProfile() {
    // workflow 两档(入参/产物)同一扫法:null 当缺失;只查属性 type;
    // 旧扫描链没有 null 分支,声明 type:"null" 也全过;enum/const/界/
    // additionalProperties 不查,不递归。
    Profile profile;
    profile.null_as_missing = true;
    profile.enforce_null_type = false;
    return profile;
}

std::vector<Finding> CollectValueFindings(const nlohmann::json& value, const nlohmann::json& schema,
                                          const Profile& profile) {
    std::vector<Finding> findings;
    CollectValue(value, schema, std::string(), 0, profile, findings);
    return findings;
}

std::vector<Finding> CollectObjectFindings(const nlohmann::json& value, const nlohmann::json& schema,
                                           const Profile& profile) {
    std::vector<Finding> findings;
    CollectObjectBranch(value, schema, std::string(), 0, profile, findings);
    return findings;
}

}  // namespace lubancode::schema
