// AR-09 合同测试:Schema 校验四个入口的档位差异矩阵,收敛前先钉死。
// 四个入口:
//   1. 钩子改参复验 tools::ValidateInputAgainstSchema——浅扫一层,required
//      只看键在不在,属性查 type+enum,认不得的声明不拦,不递归;
//   2. 插件合同 runtime::ValidateArgumentsAgainstSchema——子集全落、递归、
//      32 层深度帽、additionalProperties 也管(插件作者是外人,manifest 是
//      合同,合同写岔了要看得见);
//   3. workflow 入参校验——经 WorkflowRuntime::Run 的 invalid_inputs 文案
//      观察:null 当缺失,只查属性 type,enum 不查;
//   4. workflow 产物合同——经编排账拒收件的 validation.checks 观察:候选
//      必须是 object,checks 全收不只报首个。
// 每格断言的都是当前现行为。迁移只许换实现,不许换合同——同名校验
// 不等于相同合同,差异矩阵:
//   | 格子 | tools | 插件 | wf入参 | wf产物 |
//   |---|---|---|---|---|
//   | required 缺键 | 拒 | 拒 | 拒 | 拒 |
//   | required 显式 null(属性无 type) | 过 | 过 | 拒 | 拒 |
//   | required 显式 null(属性声明 string) | 拒在 type | 拒在 type | 拒在 required | 拒在 required |
//   | 属性 type 不合 | 拒 | 拒 | 拒 | 拒 |
//   | 声明 type:"null" 给非 null 值 | 拒 | 拒 | 过(旧扫描链没有 null 分支) | 过 |
//   | enum | 查 | 查 | 不查 | 不查 |
//   | const/items/界/additionalProperties | 不查 | 查 | 不查 | 不查 |
//   | 嵌套递归与深度帽 | 不进 | 进,32 层帽 | 不进 | 不进 |
//   | 顶层 object | 声明 type:object 才强制 | 恒强制 | 不查(null/缺键照报) | 候选必须 object |

#include <doctest/doctest.h>

#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <string>

#include <nlohmann/json.hpp>

#include "runtime/plugin_contract.hpp"
#include "tools/schema_check.hpp"
#include "workflow/account.hpp"
#include "workflow/parser.hpp"
#include "workflow/runtime.hpp"

namespace {

namespace fs = std::filesystem;
using namespace lubancode;

// ---------------------------------------------------------------------------
// workflow 侧夹具:入参档走 headless Run(校验在起跑前,失败即返,零落
// 盘);产物档走编排账路(拒收件里带 validation.checks)。
// ---------------------------------------------------------------------------

class EchoExecutor : public workflow::NodeExecutor {
public:
    workflow::NodeExecResult Execute(const workflow::NodeExecRequest&) override {
        workflow::NodeExecResult result;
        result.ok = true;
        result.output = nlohmann::json{{"echo", true}};
        return result;
    }
};

class FixedOutputExecutor : public workflow::NodeExecutor {
public:
    nlohmann::json output = nlohmann::json::object();

    workflow::NodeExecResult Execute(const workflow::NodeExecRequest&) override {
        workflow::NodeExecResult result;
        result.ok = true;
        result.output = output;
        return result;
    }
};

// 入参档:invalid_inputs 的人话;过验则一路跑成功,返回 nullopt。
std::optional<std::string> InputsProblem(const workflow::WorkflowDefinition& definition, nlohmann::json values) {
    workflow::RuntimeOptions options;
    options.executors[workflow::NodeKind::Transform] = std::make_shared<EchoExecutor>();
    workflow::WorkflowRuntime runtime(options);
    const auto summary = runtime.Run(definition, workflow::RunInputs{std::move(values)});
    if (summary.error_code == "invalid_inputs") {
        return summary.error_message;
    }
    CHECK(summary.state == workflow::RunState::Succeeded);  // 过验就别在别处炸
    return std::nullopt;
}

fs::path TempRoot(const char* tag) {
    static int counter = 0;
    ++counter;
    const fs::path dir =
        fs::temp_directory_path() / ("lubancode_ar09_" + std::string(tag) + "_" + std::to_string(counter));
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir, ec);
    return dir;
}

// 产物档:跑一帧带 output_schema 的节点,取 outputs/out-000001.json 里的
// validation JSON(passed + checks)。合格产物与拒收候选都落这件;任一步
// 出岔返回 nullopt,调用方断言。
std::optional<nlohmann::json> OutputValidationJson(const char* schema_yaml, nlohmann::json candidate) {
    const std::string yaml = std::string(R"YAML(schema_version: 1
id: ar09-out
version: 1.0.0
entry: gen
nodes:
  gen:
    type: transform
    operation: echo
    output_schema:
)YAML") + schema_yaml + R"YAML(  fin: { type: end }
edges:
  - { from: gen, on: success, to: fin }
)YAML";
    auto parsed = workflow::ParseWorkflowYaml(yaml);
    if (!parsed.has_value()) return std::nullopt;
    auto executor = std::make_shared<FixedOutputExecutor>();
    executor->output = std::move(candidate);
    workflow::RuntimeOptions options;
    options.executors[workflow::NodeKind::Transform] = executor;
    options.account_root = TempRoot("out");
    options.run_id_generator = [] { return "run-ar09"; };
    workflow::WorkflowRuntime runtime(options);
    runtime.Run(*parsed, workflow::RunInputs{});
    std::ifstream file(options.account_root / "run-ar09" / "outputs" / "out-000001.json", std::ios::binary);
    if (!file.is_open()) return std::nullopt;
    const auto saved = workflow::OutputCommitRecord::FromJson(nlohmann::json::parse(file));
    if (!saved.has_value()) return std::nullopt;
    return saved->validation;
}

// 深嵌套夹具(深度帽用):N 层 {"a": ...} 包一个布尔叶。
nlohmann::json DeepValue(int depth) {
    nlohmann::json value = true;
    for (int i = 0; i < depth; ++i) {
        value = nlohmann::json{{"a", std::move(value)}};
    }
    return value;
}

nlohmann::json DeepSchema(int depth) {
    nlohmann::json schema = nlohmann::json{{"type", "boolean"}};
    for (int i = 0; i < depth; ++i) {
        nlohmann::json wrapper = nlohmann::json::object();
        wrapper["type"] = "object";
        wrapper["properties"] = nlohmann::json{{"a", std::move(schema)}};
        schema = std::move(wrapper);
    }
    return schema;
}

}  // namespace

// ---------------------------------------------------------------------------
// 矩阵逐格
// ---------------------------------------------------------------------------

TEST_CASE("矩阵·required 缺键:四档全拒,文案各守各的") {
    const nlohmann::json schema = nlohmann::json::parse(R"json({
        "type": "object", "required": ["mode"], "properties": {"mode": {"type": "string"}}
    })json");

    const auto tools_problem = tools::ValidateInputAgainstSchema(nlohmann::json{{"other", 1}}, schema);
    REQUIRE(tools_problem.has_value());
    CHECK(*tools_problem == "缺少必填字段: mode");

    const auto plugin_problem = runtime::ValidateArgumentsAgainstSchema(nlohmann::json{{"other", 1}}, schema);
    REQUIRE(plugin_problem.has_value());
    // 顶层 path 是空串,旧码 path + " 缺少必填字段: " 拼出前导空格——照钉。
    CHECK(*plugin_problem == " 缺少必填字段: mode");
}

TEST_CASE("矩阵·required 显式 null:workflow 两档当缺失拒,tools/插件轮到 type 说话") {
    const nlohmann::json with_type = nlohmann::json::parse(R"json({
        "type": "object", "required": ["mode"], "properties": {"mode": {"type": "string"}}
    })json");
    const nlohmann::json no_type = nlohmann::json{{"type", "object"}, {"required", nlohmann::json::array({"mode"})}};

    // 属性声明 string 而 null:tools/插件 required 过、type 拦。
    const auto tools_typed = tools::ValidateInputAgainstSchema(nlohmann::json{{"mode", nullptr}}, with_type);
    REQUIRE(tools_typed.has_value());
    CHECK(*tools_typed == "字段 mode 的类型应是 string");
    const auto plugin_typed = runtime::ValidateArgumentsAgainstSchema(nlohmann::json{{"mode", nullptr}}, with_type);
    REQUIRE(plugin_typed.has_value());
    CHECK(*plugin_typed == "mode 的类型应是 string");

    // 属性没声明 type 而 null:tools/插件全过(只有属性 type 不认 null 才拒)。
    CHECK_FALSE(tools::ValidateInputAgainstSchema(nlohmann::json{{"mode", nullptr}}, no_type).has_value());
    CHECK_FALSE(runtime::ValidateArgumentsAgainstSchema(nlohmann::json{{"mode", nullptr}}, no_type).has_value());
}

TEST_CASE("矩阵·workflow 入参:null 当缺失,required 与 type 的文案照旧") {
    auto parsed = workflow::ParseWorkflowYaml(R"YAML(
schema_version: 1
id: ar09-in
version: 1.0.0
entry: a
inputs:
  type: object
  required: [mode]
  properties:
    mode: { type: string }
    lvl: { type: integer }
nodes:
  a: { type: transform, operation: echo }
  fin: { type: end }
edges:
  - { from: a, on: success, to: fin }
)YAML");
    REQUIRE(parsed.has_value());

    CHECK(InputsProblem(*parsed, nlohmann::json{{"other", 1}}) ==
          std::optional<std::string>("inputs.mode 缺必填字段"));
    // null 当缺失:required 拒,不轮到 type。
    CHECK(InputsProblem(*parsed, nlohmann::json{{"mode", nullptr}}) ==
          std::optional<std::string>("inputs.mode 缺必填字段"));
    // null 值跳过属性检查:lvl 给 null 不报 type。
    CHECK(InputsProblem(*parsed, nlohmann::json{{"mode", "fast"}, {"lvl", nullptr}}) == std::nullopt);
    // type 不合:文案带实给类型名(nlohmann 口径 integer 是 number)。
    CHECK(InputsProblem(*parsed, nlohmann::json{{"mode", "fast"}, {"lvl", "4"}}) ==
          std::optional<std::string>("inputs.lvl 期望 integer,给的是 string"));
    CHECK(InputsProblem(*parsed, nlohmann::json{{"mode", 5}}) ==
          std::optional<std::string>("inputs.mode 期望 string,给的是 number"));
    // 顶层不查 object:入参非对象被 defaults 归成空 object,照报缺键。
    CHECK(InputsProblem(*parsed, nlohmann::json::array({1, 2})) ==
          std::optional<std::string>("inputs.mode 缺必填字段"));
    CHECK(InputsProblem(*parsed, nlohmann::json{{"mode", "fast"}}) == std::nullopt);
}

TEST_CASE("矩阵·声明 type:null:tools/插件验 is_null,workflow 旧链没这分支全过") {
    const nlohmann::json schema = nlohmann::json::parse(R"json({
        "type": "object", "properties": {"x": {"type": "null"}}
    })json");

    const auto tools_problem = tools::ValidateInputAgainstSchema(nlohmann::json{{"x", 5}}, schema);
    REQUIRE(tools_problem.has_value());
    CHECK(*tools_problem == "字段 x 的类型应是 null");
    const auto plugin_problem = runtime::ValidateArgumentsAgainstSchema(nlohmann::json{{"x", 5}}, schema);
    REQUIRE(plugin_problem.has_value());
    CHECK(*plugin_problem == "x 的类型应是 null");
    CHECK_FALSE(runtime::ValidateArgumentsAgainstSchema(nlohmann::json{{"x", nullptr}}, schema).has_value());

    auto parsed = workflow::ParseWorkflowYaml(R"YAML(
schema_version: 1
id: ar09-null
version: 1.0.0
entry: a
inputs:
  properties:
    x: { type: "null" }
nodes:
  a: { type: transform, operation: echo }
  fin: { type: end }
edges:
  - { from: a, on: success, to: fin }
)YAML");
    REQUIRE(parsed.has_value());
    CHECK(InputsProblem(*parsed, nlohmann::json{{"x", 5}}) == std::nullopt);
}

TEST_CASE("矩阵·enum:tools/插件查且文案不同,workflow 不查") {
    const nlohmann::json schema = nlohmann::json::parse(R"json({
        "type": "object", "properties": {"mode": {"type": "string", "enum": ["fast", "slow"]}}
    })json");

    const auto tools_problem = tools::ValidateInputAgainstSchema(nlohmann::json{{"mode", "medium"}}, schema);
    REQUIRE(tools_problem.has_value());
    CHECK(*tools_problem == "字段 mode 的取值不在枚举表里");
    const auto plugin_problem = runtime::ValidateArgumentsAgainstSchema(nlohmann::json{{"mode", "medium"}}, schema);
    REQUIRE(plugin_problem.has_value());
    CHECK(*plugin_problem == "mode 的取值不在枚举表里");
    CHECK_FALSE(tools::ValidateInputAgainstSchema(nlohmann::json{{"mode", "fast"}}, schema).has_value());

    auto parsed = workflow::ParseWorkflowYaml(R"YAML(
schema_version: 1
id: ar09-enum
version: 1.0.0
entry: a
inputs:
  properties:
    mode: { type: string, enum: [fast, slow] }
nodes:
  a: { type: transform, operation: echo }
  fin: { type: end }
edges:
  - { from: a, on: success, to: fin }
)YAML");
    REQUIRE(parsed.has_value());
    CHECK(InputsProblem(*parsed, nlohmann::json{{"mode", "medium"}}) == std::nullopt);
}

TEST_CASE("矩阵·const/items/additionalProperties/嵌套:插件档独查,文案照旧") {
    const nlohmann::json const_schema = nlohmann::json::parse(R"json({
        "type": "object", "properties": {"level": {"const": 3}}
    })json");
    // tools 不查 const。
    CHECK_FALSE(tools::ValidateInputAgainstSchema(nlohmann::json{{"level", 4}}, const_schema).has_value());
    const auto const_problem = runtime::ValidateArgumentsAgainstSchema(nlohmann::json{{"level", 4}}, const_schema);
    REQUIRE(const_problem.has_value());
    CHECK(*const_problem == "level 必须恒等于 3");

    const nlohmann::json nested_schema = nlohmann::json::parse(R"json({
        "type": "object",
        "properties": {"box": {"type": "object", "properties": {"n": {"type": "integer"}}}}
    })json");
    // 浅档不进嵌套:box 里 n 给字符串,tools 照过。
    CHECK_FALSE(tools::ValidateInputAgainstSchema(nlohmann::json{{"box", nlohmann::json{{"n", "字符串"}}}},
                                                  nested_schema)
                    .has_value());
    const auto nested_problem = runtime::ValidateArgumentsAgainstSchema(
        nlohmann::json{{"box", nlohmann::json{{"n", "字符串"}}}}, nested_schema);
    REQUIRE(nested_problem.has_value());
    CHECK(*nested_problem == "box.n 的类型应是 integer");

    const nlohmann::json ap_schema = nlohmann::json::parse(R"json({
        "type": "object", "properties": {"mode": {"type": "string"}}, "additionalProperties": false
    })json");
    // 浅档不认 additionalProperties:多给键照过。
    CHECK_FALSE(
        tools::ValidateInputAgainstSchema(nlohmann::json{{"mode", "fast"}, {"extra", 1}}, ap_schema).has_value());
    const auto ap_problem =
        runtime::ValidateArgumentsAgainstSchema(nlohmann::json{{"mode", "fast"}, {"extra", 1}}, ap_schema);
    REQUIRE(ap_problem.has_value());
    CHECK(*ap_problem == "extra 不在声明的字段里(additionalProperties=false)");

    const nlohmann::json items_schema = nlohmann::json::parse(R"json({
        "type": "object", "properties": {"list": {"type": "array", "items": {"type": "integer"}}}
    })json");
    CHECK_FALSE(tools::ValidateInputAgainstSchema(nlohmann::json{{"list", nlohmann::json{1, "x"}}}, items_schema)
                    .has_value());
    const auto items_problem =
        runtime::ValidateArgumentsAgainstSchema(nlohmann::json{{"list", nlohmann::json{1, "x"}}}, items_schema);
    REQUIRE(items_problem.has_value());
    CHECK(*items_problem == "list[1] 的类型应是 integer");

    // workflow 入参:嵌套里 n 给字符串,type 扫描只看顶层 box 是 object,过。
    auto parsed = workflow::ParseWorkflowYaml(R"YAML(
schema_version: 1
id: ar09-deep
version: 1.0.0
entry: a
inputs:
  properties:
    box:
      type: object
      properties:
        n: { type: integer }
nodes:
  a: { type: transform, operation: echo }
  fin: { type: end }
edges:
  - { from: a, on: success, to: fin }
)YAML");
    REQUIRE(parsed.has_value());
    CHECK(InputsProblem(*parsed, nlohmann::json{{"box", nlohmann::json{{"n", "字符串"}}}}) == std::nullopt);
}

TEST_CASE("矩阵·插件深度帽:32 层照旧,浅档不递归恒过") {
    CHECK_FALSE(runtime::ValidateArgumentsAgainstSchema(DeepValue(30), DeepSchema(30)).has_value());
    const auto deep_problem = runtime::ValidateArgumentsAgainstSchema(DeepValue(40), DeepSchema(40));
    REQUIRE(deep_problem.has_value());
    CHECK(deep_problem->find("嵌套超过 32 层") != std::string::npos);
    // 浅档不进嵌套:40 层深照样过。
    CHECK_FALSE(tools::ValidateInputAgainstSchema(DeepValue(40), DeepSchema(40)).has_value());
}

TEST_CASE("矩阵·顶层 object:三档三种政策") {
    // tools:声明 type:object 才强制;没声明就放行无从校验。
    const auto tools_declared =
        tools::ValidateInputAgainstSchema(nlohmann::json::array({1, 2}), nlohmann::json{{"type", "object"}});
    REQUIRE(tools_declared.has_value());
    CHECK(*tools_declared == "入参必须是 object");
    CHECK_FALSE(tools::ValidateInputAgainstSchema(nlohmann::json::array({1, 2}), nlohmann::json::object())
                    .has_value());

    // 插件:恒强制;顶层声明非 object 类型也真拦(顶层 path 空,文案带前导空格)。
    const auto plugin_object =
        runtime::ValidateArgumentsAgainstSchema(nlohmann::json::array({1, 2}), nlohmann::json::object());
    REQUIRE(plugin_object.has_value());
    CHECK(*plugin_object == "入参必须是 object");
    const auto plugin_type =
        runtime::ValidateArgumentsAgainstSchema(nlohmann::json::object(), nlohmann::json{{"type", "array"}});
    REQUIRE(plugin_type.has_value());
    CHECK(*plugin_type == " 的类型应是 array");
}

TEST_CASE("矩阵·认不得的声明与未知键:四档全放行") {
    const nlohmann::json weird_type = nlohmann::json::parse(R"json({
        "type": "object", "properties": {"x": {"type": "weird"}}
    })json");
    const nlohmann::json input{{"x", nlohmann::json::array({1})}};
    CHECK_FALSE(tools::ValidateInputAgainstSchema(input, weird_type).has_value());
    CHECK_FALSE(runtime::ValidateArgumentsAgainstSchema(input, weird_type).has_value());

    // properties 之外多给的键,浅档默认放行。
    const nlohmann::json schema = nlohmann::json::parse(R"json({
        "type": "object", "properties": {"mode": {"type": "string"}}
    })json");
    CHECK_FALSE(
        tools::ValidateInputAgainstSchema(nlohmann::json{{"mode", "fast"}, {"extra", 1}}, schema).has_value());
}

TEST_CASE("矩阵·产物合同:checks 全收,required 先于 type,null 当缺失") {
    const char* schema_yaml = R"YAML(      type: object
      required: [summary, extra]
      properties:
        summary: { type: string }
        count: { type: integer }
)YAML";

    // 候选非 object:单条 output_not_object,不带别的。
    const auto not_object = OutputValidationJson(schema_yaml, nlohmann::json::array({1, 2}));
    REQUIRE(not_object.has_value());
    CHECK(*not_object == nlohmann::json::parse(R"json({
        "passed": false,
        "checks": [{"code": "output_not_object", "field": "", "expected": "object", "actual": "array"}]
    })json"));

    // null 当缺失:summary 给 null 算缺。
    const auto null_missing = OutputValidationJson(schema_yaml, nlohmann::json{{"summary", nullptr}, {"extra", 1}});
    REQUIRE(null_missing.has_value());
    CHECK(*null_missing == nlohmann::json::parse(R"json({
        "passed": false,
        "checks": [{"code": "missing_required_field", "field": "summary", "expected": "必填", "actual": "缺字段"}]
    })json"));

    // 全收 + 顺序:required 挨个报完才轮到 type(数组序即产出序)。
    const auto all_bad = OutputValidationJson(schema_yaml, nlohmann::json{{"count", "x"}});
    REQUIRE(all_bad.has_value());
    CHECK(*all_bad == nlohmann::json::parse(R"json({
        "passed": false,
        "checks": [
            {"code": "missing_required_field", "field": "summary", "expected": "必填", "actual": "缺字段"},
            {"code": "missing_required_field", "field": "extra", "expected": "必填", "actual": "缺字段"},
            {"code": "type_mismatch", "field": "count", "expected": "integer", "actual": "string"}
        ]
    })json"));

    // 合格产物:passed=true 零 checks,run 成功。
    const auto good = OutputValidationJson(schema_yaml,
                                           nlohmann::json{{"summary", "s"}, {"extra", 1}, {"count", 2}});
    REQUIRE(good.has_value());
    CHECK(*good == nlohmann::json::parse(R"json({"passed": true, "checks": []})json"));
}
