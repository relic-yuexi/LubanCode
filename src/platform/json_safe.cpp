#include "platform/json_safe.hpp"

#include <utility>

#include "platform/text_encoding.hpp"  // IsValidUtf8 / SanitizeExternalText

namespace lubancode::platform {

namespace {

// 深度优先找坏串,找到就停(诊断只要第一处)。path 是从树根起算的字段
// 路径,object 用 ".key",array 用 "[i]"。
bool FindInvalidUtf8FieldImpl(const nlohmann::json& value, const std::string& path,
                              std::optional<std::string>& found) {
    if (value.is_string()) {
        const std::string& text = value.get_ref<const std::string&>();
        if (!IsValidUtf8(text)) {
            found = path.empty() ? std::optional<std::string>("(root)") : std::optional<std::string>(path);
            return true;
        }
        return false;
    }
    if (value.is_array()) {
        for (std::size_t i = 0; i < value.size(); ++i) {
            if (FindInvalidUtf8FieldImpl(value[i], path + "[" + std::to_string(i) + "]", found)) {
                return true;
            }
        }
        return false;
    }
    if (value.is_object()) {
        for (auto it = value.begin(); it != value.end(); ++it) {
            const std::string child = path.empty() ? it.key() : path + "." + it.key();
            if (FindInvalidUtf8FieldImpl(it.value(), child, found)) {
                return true;
            }
        }
    }
    return false;
}

// 递归把树里所有非法字符串过一遍 SanitizeExternalText(合法的不动)。
void SanitizeJsonStrings(nlohmann::json& value) {
    if (value.is_string()) {
        std::string text = value.get<std::string>();
        if (!IsValidUtf8(text)) {
            value = SanitizeExternalText(text);
        }
        return;
    }
    if (value.is_array()) {
        for (auto& item : value) {
            SanitizeJsonStrings(item);
        }
        return;
    }
    if (value.is_object()) {
        for (auto& item : value) {
            SanitizeJsonStrings(item);
        }
    }
}

}  // namespace

std::optional<std::string> FindInvalidUtf8Field(const nlohmann::json& value) {
    std::optional<std::string> found;
    FindInvalidUtf8FieldImpl(value, std::string(), found);
    return found;
}

std::string DumpJsonSanitized(const nlohmann::json& value, bool* changed_out) {
    if (changed_out != nullptr) {
        *changed_out = false;
    }
    try {
        return value.dump();
    } catch (const nlohmann::json::exception&) {
        // 树里有坏串(dump 对非法 UTF-8 抛 type_error.316,这里放宽到整个
        // json::exception 家族,反正重试一遍就知道是不是编码的事)。
    }
    nlohmann::json sanitized = value;
    SanitizeJsonStrings(sanitized);
    if (changed_out != nullptr) {
        *changed_out = true;
    }
    try {
        return sanitized.dump();
    } catch (const nlohmann::json::exception&) {
        // 全树字符串都清洗过了还 dump 不动,理论上到不了这里。保底给一行
        // 可解析的 JSON,调用方(会话/录制落盘)拿到它至少不落坏行。
        return "{\"error\":\"dump failed after utf-8 sanitization\"}";
    }
}

std::string DescribeDumpFailure(const nlohmann::json& body, const nlohmann::json::exception& error) {
    std::string message = "请求体序列化失败(非法 UTF-8): ";
    message += error.what();
    const auto field = FindInvalidUtf8Field(body);
    if (field.has_value()) {
        message += "; 字段: " + *field;
    }
    return message;
}

}  // namespace lubancode::platform
