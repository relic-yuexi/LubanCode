#include "tools/schema_check.hpp"

namespace lubancode::tools {

namespace {

bool JsonTypeMatches(const nlohmann::json& value, const std::string& expected) {
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
        return value.is_null();
    }
    return true;  // 认不得的类型声明不拦(从严校验拦"值不对",不拦"声明怪")
}

}  // namespace

std::optional<std::string> ValidateInputAgainstSchema(const nlohmann::json& input, const nlohmann::json& schema) {
    if (!schema.is_object()) {
        return std::nullopt;  // 工具没给有效 schema,无从校验
    }
    if (schema.contains("type")) {
        if (schema["type"].is_string() && schema["type"].get<std::string>() == "object" && !input.is_object()) {
            return "入参必须是 object";
        }
    }
    if (!input.is_object()) {
        return std::nullopt;  // 顶层类型没声明 object 时,其余键校验无从谈起
    }
    if (schema.contains("required") && schema["required"].is_array()) {
        for (const auto& key : schema["required"]) {
            if (!key.is_string()) {
                continue;
            }
            if (!input.contains(key.get<std::string>())) {
                return "缺少必填字段: " + key.get<std::string>();
            }
        }
    }
    if (schema.contains("properties") && schema["properties"].is_object()) {
        for (auto it = input.begin(); it != input.end(); ++it) {
            const auto props = schema["properties"].find(it.key());
            if (props == schema["properties"].end()) {
                continue;  // additionalProperties 默认放行(多数工具留了活口)
            }
            if (props.value().contains("type") && props.value()["type"].is_string()) {
                const std::string expected = props.value()["type"].get<std::string>();
                if (!JsonTypeMatches(it.value(), expected)) {
                    return "字段 " + it.key() + " 的类型应是 " + expected;
                }
            }
            if (props.value().contains("enum") && props.value()["enum"].is_array()) {
                bool in_enum = false;
                for (const auto& allowed : props.value()["enum"]) {
                    if (it.value() == allowed) {
                        in_enum = true;
                        break;
                    }
                }
                if (!in_enum) {
                    return "字段 " + it.key() + " 的取值不在枚举表里";
                }
            }
        }
    }
    return std::nullopt;
}

}  // namespace lubancode::tools
