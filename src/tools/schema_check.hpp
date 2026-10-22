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
// AR-09:扫描原语收在 schema 核心(档位 HookRewriteProfile),本文件只剩
// 顶层政策与人话文案的适配层;与插件档/workflow 两档的差异矩阵见
// tests/unit/schema/test_schema_contract.cpp。
#pragma once

#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace lubancode::tools {

// 返回 std::nullopt = 通过;有值 = 人话错误(第一个撞上的问题)。
std::optional<std::string> ValidateInputAgainstSchema(const nlohmann::json& input, const nlohmann::json& schema);

// 模型入参的执行前浅检(开源模型坏 JSON 单):只查 required 与顶层
// properties 的基本 type(string/integer/number/boolean/object/array),
// enum 不查(工具自己的 execute 给得出更贴切的人话),不递归。宽容一味:
// integer/number 字段收到数字字符串("120")就地转成数,记进 coerced。
// error 有值 = 不过(缺哪个字段/期望什么类型的人话,首错);coerced 是
// 转换说明,调用方附注给模型,不静默。
struct ModelInputPrecheck {
    std::optional<std::string> error;
    std::vector<std::string> coerced;
};
ModelInputPrecheck PrecheckModelToolInput(nlohmann::json& input, const nlohmann::json& schema);

}  // namespace lubancode::tools
