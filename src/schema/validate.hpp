// JSON Schema 子集校验核心(AR-09:Schema 校验机制散落四个入口)。
// 原先四个领域入口各写一份 required+type 扫描,插件另藏一份递归
// validator——加一种 JSON 类型、修一处数值判断,要同步 tools、plugin
// 与 workflow 两段。这里收成一份核心:
//   - 类型/required/属性遍历原语(JsonTypeMatches 原先抄了两份);
//   - 递归验证器(插件那份下沉,含 const/enum/数值长度数组界/items/
//     additionalProperties 与 32 层深度帽)。
// 档位(Profile)把入口差异摆上台面,不许靠读调用方猜:
//   - null 算不算缺失(workflow 算,tools/插件不算);
//   - 声明 type:"null" 验不验(workflow 旧扫描链没有 null 分支,不验);
//   - 递归与否、深度帽(插件递归 32 层;其余三档只扫顶层);
//   - 认哪些关键字(enum/const/界/items/additionalProperties 插件档独有)。
// 顶层要不要 object、顶层类型/const/enum 查不查,各入口在适配层自己定
// (tools 只在声明 type:object 时强制;插件恒强制;workflow 入参不查、
// 产物要求候选是 object)。核心只出结构化 findings,人话文案各入口自
// 排——首阶段错误码/路径/文案一个字不动。不冒充完整 JSON Schema:只
// 收口当前已声明的子集,未知类型声明与未知关键字不拦。
//
// 差异矩阵的合同测试:tests/unit/schema/test_schema_contract.cpp。
#pragma once

#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace lubancode::schema {

// 一条不合。按遍历序产出(与旧码首错序一致);各入口拿去拼自己的人话。
struct Finding {
    enum class Code {
        TypeMismatch,        // path 的类型应是 expected(actual 是实给类型名)
        ConstMismatch,       // path 必须恒等于 expected
        EnumMismatch,        // path 的取值不在枚举表里
        BelowMinimum,        // path 小于最小值 expected
        AboveMaximum,        // path 大于最大值 expected
        BelowMinLength,      // path 短于 minLength expected
        AboveMaxLength,      // path 长于 maxLength expected
        BelowMinItems,       // path 的元素个数少于 minItems expected
        AboveMaxItems,       // path 的元素个数多于 maxItems expected
        MissingRequired,     // path(所在对象处)缺必填字段 field
        AdditionalProperty,  // path 不在声明的字段里(additionalProperties=false)
        DepthExceeded,       // 嵌套超过 expected 层
    };
    Code code = Code::TypeMismatch;
    std::string path;      // 插件口径:顶层空串,嵌套 a.b / list[1]
    std::string field;     // 涉事键名(MissingRequired/AdditionalProperty 用)
    std::string expected;  // 期望值人话:类型名/界值 dump/深度帽
    std::string actual;    // 实给类型名(TypeMismatch 用)
};

// 档位:字段默认值即最浅档(只查 required + 属性 type),插件档全开。
struct Profile {
    // null 算不算缺失:workflow 两档算(required 拒显式 null、属性检查跳
    // 过 null 值);tools/插件不算(required 只看键在不在,null 值照过
    // 类型检查——声明 string 而 null,轮到 type 拦)。
    bool null_as_missing = false;
    // 声明 type:"null" 要不要真验:tools/插件验 is_null;workflow 旧扫
    // 描链没有 null 分支,声明了也全过。
    bool enforce_null_type = true;
    // 要不要往嵌套里走(属性子 schema/数组 items/additionalProperties
    // 子 schema):插件走,其余三档只扫顶层。
    bool recursive = false;
    // 递归深度帽(插件 32;浅档不递归,恒不触发)。
    int max_depth = 32;
    // 关键字集合(插件档全开,其余档全关):
    bool check_const = false;                  // const
    bool check_enum = false;                   // enum
    bool check_numeric_bounds = false;         // minimum/maximum
    bool check_string_length = false;          // minLength/maxLength(按码点)
    bool check_array_bounds = false;           // minItems/maxItems(items 递归归 recursive)
    bool check_additional_properties = false;  // additionalProperties(false/schema)
};

// 领域档位(差异矩阵见合同测试):
// 钩子改参复验(tools::ValidateInputAgainstSchema):浅扫一层,enum 也查。
Profile HookRewriteProfile();
// 插件合同(runtime::ValidateArgumentsAgainstSchema):全关键字、递归、
// 深度 32,additionalProperties 也管。
Profile PluginContractProfile();
// workflow 两档(入参校验 ValidateInputsAgainstSchema 与产物合同
// ValidateNodeOutput)同一扫法:null 当缺失、只查属性 type、不递归。
Profile WorkflowScanProfile();

// 值对 schema 的递归校验(插件递归验证器下沉):本层标量关键字(type/
// const/enum/界)也查,再按档位往数组/对象里走。schema 非对象(没给
// schema)不产 finding。插件适配层从顶层进这口;浅档入口别直呼——顶层
// 类型/const/enum 的政策各入口自己拿。
std::vector<Finding> CollectValueFindings(const nlohmann::json& value, const nlohmann::json& schema,
                                          const Profile& profile);

// 对象合同分支(required + 属性逐键 + additionalProperties):浅档入口的
// 顶层就走到这层,不查本层类型/const/enum——与 tools/workflow 旧码同
// 口径。value 非对象时 required 照查(find 落空即缺),属性遍历跳过。
std::vector<Finding> CollectObjectFindings(const nlohmann::json& value, const nlohmann::json& schema,
                                           const Profile& profile);

}  // namespace lubancode::schema
