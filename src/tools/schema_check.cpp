// 轻量 JSON Schema 校验:PreToolUse 钩子的 updatedInput 改写工具入参后,
// 必须重过一遍工具自己的 input_schema,不许钩子借改参绕过 schema(规格
// "决策归并"与"不做"清单)。子集够用、语义从严:
//   - 顶层 type(认 object);
//   - required 键齐不齐;
//   - properties 里声明的类型(string/number/integer/boolean/array/object)
//     对不对;integer 额外要求是整数值;
//   - enum 枚举值在不在表里。
// 嵌套 schema(nested properties/items)不递归——工具入参基本都是一层
// 平对象,深层的复杂校验交给工具自己的 execute 兜底;这里只拦"钩子把
// 入参改成了明显不是这工具要的形状"。校验失败 = 改写打回,当次工具调用
// 按拦截处理(钩子明确想改参,悄悄按原参数跑出去才是危险的那条路)。
//
// AR-09:扫描原语与档位收在 schema 核心(HookRewriteProfile:浅扫一层,
// enum 也查);这里只剩顶层政策(只在声明 type:object 时强制入参是
// object)与首错文案。档位差异矩阵见 tests/unit/schema/test_schema_contract.cpp。
#include "tools/schema_check.hpp"

#include <cerrno>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <cstdlib>

#include "schema/validate.hpp"

namespace lubancode::tools {

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
    const auto findings = schema::CollectObjectFindings(input, schema, schema::HookRewriteProfile());
    if (findings.empty()) {
        return std::nullopt;
    }
    const schema::Finding& first = findings.front();
    switch (first.code) {
        case schema::Finding::Code::MissingRequired:
            return "缺少必填字段: " + first.field;
        case schema::Finding::Code::TypeMismatch:
            // 浅档顶层 path 即键名。
            return "字段 " + first.path + " 的类型应是 " + first.expected;
        case schema::Finding::Code::EnumMismatch:
            return "字段 " + first.path + " 的取值不在枚举表里";
        default:
            return std::nullopt;  // 本档只开 required/type/enum,不会有别的码
    }
}

namespace {

// 声明的类型名(只认字符串形 type;联合类型/缺省给空)。
std::string DeclaredType(const nlohmann::json& schema, const std::string& field) {
    if (!schema.contains("properties") || !schema["properties"].is_object()) {
        return {};
    }
    const nlohmann::json& properties = schema["properties"];
    if (!properties.contains(field) || !properties[field].is_object()) {
        return {};
    }
    const nlohmann::json& property = properties[field];
    if (!property.contains("type") || !property["type"].is_string()) {
        return {};
    }
    return property["type"].get<std::string>();
}

// 数字字符串转数:整串(去首尾空白)必须是一枚数。integer 只收整数写法;
// number 先试整数,再试浮点(strtod,from_chars 浮点档老 libc++ 不全)。
std::optional<nlohmann::json> CoerceNumericString(const std::string& text, bool want_integer) {
    std::size_t begin = 0;
    std::size_t end = text.size();
    while (begin < end && (text[begin] == ' ' || text[begin] == '\t')) {
        ++begin;
    }
    while (end > begin && (text[end - 1] == ' ' || text[end - 1] == '\t')) {
        --end;
    }
    if (begin == end) {
        return std::nullopt;
    }
    const char* first = text.data() + begin;
    const char* last = text.data() + end;
    std::int64_t integer = 0;
    const auto [ptr, ec] = std::from_chars(first, last, integer);
    if (ec == std::errc() && ptr == last) {
        return nlohmann::json(integer);
    }
    if (want_integer) {
        return std::nullopt;
    }
    const std::string slice(first, last);
    char* parsed_end = nullptr;
    errno = 0;
    const double number = std::strtod(slice.c_str(), &parsed_end);
    if (errno != 0 || parsed_end != slice.c_str() + slice.size() || !std::isfinite(number)) {
        return std::nullopt;
    }
    return nlohmann::json(number);
}

}  // namespace

ModelInputPrecheck PrecheckModelToolInput(nlohmann::json& input, const nlohmann::json& schema) {
    ModelInputPrecheck result;
    if (!schema.is_object()) {
        return result;  // 工具没给有效 schema,无从校验
    }
    if (schema.contains("type") && schema["type"].is_string() &&
        schema["type"].get<std::string>() == "object" && !input.is_object()) {
        result.error = "入参必须是一个 JSON 对象,收到的是 " + std::string(input.type_name());
        return result;
    }
    if (!input.is_object()) {
        return result;
    }
    // 默认档 = 最浅档:required + 顶层属性 type,null 不算缺失,不查 enum。
    const schema::Profile profile;
    std::vector<schema::Finding> findings = schema::CollectObjectFindings(input, schema, profile);
    bool coerced_any = false;
    for (const schema::Finding& finding : findings) {
        if (finding.code != schema::Finding::Code::TypeMismatch ||
            (finding.expected != "integer" && finding.expected != "number") || !input.contains(finding.path)) {
            continue;
        }
        nlohmann::json& slot = input[finding.path];
        if (slot.is_number_float() && finding.expected == "integer") {
            // 120.0 这种整值浮点:等值转整数,记一笔。
            const double value = slot.get<double>();
            if (std::isfinite(value) && std::trunc(value) == value && std::fabs(value) < 9.0e15) {
                slot = static_cast<std::int64_t>(value);
                result.coerced.push_back("字段 " + finding.path + " 收到整值浮点,已按 integer " + slot.dump() + " 处理");
                coerced_any = true;
            }
            continue;
        }
        if (!slot.is_string()) {
            continue;
        }
        const std::string original = slot.get<std::string>();
        auto number = CoerceNumericString(original, finding.expected == "integer");
        if (!number.has_value()) {
            continue;
        }
        result.coerced.push_back("字段 " + finding.path + " 收到数字字符串 \"" + original + "\",已按 " +
                                 finding.expected + " " + number->dump() + " 处理");
        slot = std::move(*number);
        coerced_any = true;
    }
    if (coerced_any) {
        findings = schema::CollectObjectFindings(input, schema, profile);
    }
    // 可选字段给 null(OpenAI strict 口径常见):不拦,交工具自己按缺省处理。
    const schema::Finding* first_hit = nullptr;
    for (const schema::Finding& finding : findings) {
        if (finding.code == schema::Finding::Code::TypeMismatch && input.contains(finding.path) &&
            input[finding.path].is_null()) {
            continue;
        }
        first_hit = &finding;
        break;
    }
    if (first_hit == nullptr) {
        return result;
    }
    const schema::Finding& first = *first_hit;
    switch (first.code) {
        case schema::Finding::Code::MissingRequired: {
            const std::string type = DeclaredType(schema, first.field);
            result.error = "缺少必填字段 " + first.field + (type.empty() ? std::string() : "(期望类型 " + type + ")");
            break;
        }
        case schema::Finding::Code::TypeMismatch:
            result.error = "字段 " + first.path + " 的类型应是 " + first.expected + ",收到的是 " + first.actual;
            break;
        default:
            break;  // 浅档只出 required/type,别的码不拦
    }
    return result;
}

}  // namespace lubancode::tools
