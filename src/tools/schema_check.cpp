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

}  // namespace lubancode::tools
