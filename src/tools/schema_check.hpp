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
#pragma once

#include <string>

#include <nlohmann/json.hpp>

namespace lubancode::tools {

// 返回 std::nullopt = 通过;有值 = 人话错误(第一个撞上的问题)。
std::optional<std::string> ValidateInputAgainstSchema(const nlohmann::json& input, const nlohmann::json& schema);

}  // namespace lubancode::tools
