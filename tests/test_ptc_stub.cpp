// PTC stub 生成器单测:类型映射、必选/可选、关键字改名、enum Literal、
// docstring 约束、索引模式、确定性(同输入同输出)。

#include <doctest/doctest.h>

#include "ptc/stub_generator.hpp"

using namespace lubancode::ptc;
using lubancode::api::ToolDefinition;
using nlohmann::json;

namespace {

// 一枚带完整 schema 的假工具定义,常见形状都盖上。
StubToolInfo MakeSampleTool() {
    ToolDefinition definition;
    definition.name = "read_file";
    definition.description = "读文件,按行号显示。";
    definition.input_schema = nlohmann::json::parse(R"({
        "type": "object",
        "properties": {
            "path": {"type": "string", "minLength": 1},
            "offset": {"type": "integer", "minimum": 1},
            "limit": {"type": "integer", "minimum": 1, "maximum": 2000, "default": 500}
        },
        "required": ["path"]
    })");
    return StubToolInfo{definition, false, true};
}

}  // namespace

TEST_CASE("PythonIdentifier: 关键字加后缀,数字开头垫下划线,其余原样") {
    CHECK(PythonIdentifier("read_file") == "read_file");
    CHECK(PythonIdentifier("import") == "import_");
    CHECK(PythonIdentifier("class") == "class_");
    CHECK(PythonIdentifier("match") == "match_");
    CHECK(PythonIdentifier("123abc") == "_123abc");
    CHECK(PythonIdentifier("") == "_");
}

TEST_CASE("SchemaToPythonType: 常见 JSON Schema 类型映射") {
    CHECK(SchemaToPythonType(nlohmann::json{{"type", "string"}}) == "str");
    CHECK(SchemaToPythonType(nlohmann::json{{"type", "integer"}}) == "int");
    CHECK(SchemaToPythonType(nlohmann::json{{"type", "number"}}) == "float");
    CHECK(SchemaToPythonType(nlohmann::json{{"type", "boolean"}}) == "bool");
    CHECK(SchemaToPythonType(nlohmann::json{{"type", "object"}}) == "dict");
    CHECK(SchemaToPythonType(nlohmann::json{{"type", "null"}}) == "None");
    CHECK(SchemaToPythonType(nlohmann::json{{"type", "array"}}) == "list");
    CHECK(SchemaToPythonType(nlohmann::json{{"type", "array"}, {"items", {{"type", "integer"}}}}) == "list[int]");
    CHECK(SchemaToPythonType(nlohmann::json{{"enum", nlohmann::json::array({"grep", "glob"})}}) ==
          "Literal['grep', 'glob']");
    // anyOf 带 null -> Optional
    CHECK(SchemaToPythonType(nlohmann::json{{"anyOf", nlohmann::json::array({{{"type", "string"}}, {{"type", "null"}}})}}) ==
          "Optional[str]");
    // 没有类型信息 -> Any
    CHECK(SchemaToPythonType(nlohmann::json{{}}) == "Any");
    CHECK(SchemaToPythonType(nlohmann::json{}) == "Any");
}

TEST_CASE("SchemaConstraintSummary: 约束摘要把数值/枚举说清") {
    const std::string text = SchemaConstraintSummary(nlohmann::json{
        {"type", "integer"}, {"minimum", 1}, {"maximum", 2000}, {"default", 500}});
    CHECK(text.find("最小 1") != std::string::npos);
    CHECK(text.find("最大 2000") != std::string::npos);
    CHECK(text.find("默认 500") != std::string::npos);
    const std::string enums =
        SchemaConstraintSummary(nlohmann::json{{"enum", nlohmann::json::array({"a", "b"})}});
    CHECK(enums.find("取值: a, b") != std::string::npos);
    CHECK(SchemaConstraintSummary(nlohmann::json{{"type", "string"}}).empty());
}

TEST_CASE("GenerateStubModule: 签名/docstring/函数体要点") {
    const auto module = GenerateStubModule({MakeSampleTool()}, StubMode::Full);
    CHECK(module.tool_count == 1);
    // 必选参数在前;可选参数按属性名字母序(稳定),带默认(schema default
    // 生效;None 兜底的裹 Optional)。
    CHECK(module.python_source.find("def read_file(path: str, limit: int = 500, "
                                    "offset: Optional[int] = None) -> ToolResult:") != std::string::npos);
    // docstring 有描述、权限、并发注记与约束。
    CHECK(module.python_source.find("读文件,按行号显示。") != std::string::npos);
    CHECK(module.python_source.find("权限: 只读,免确认。") != std::string::npos);
    CHECK(module.python_source.find("parallel_safe") != std::string::npos);
    CHECK(module.python_source.find("path: 最短 1") != std::string::npos);
    CHECK(module.python_source.find("limit(可选): 最小 1; 最大 2000; 默认 500") != std::string::npos);
    // 函数体把入参按注册表原名送宿主。
    CHECK(module.python_source.find("call_tool('read_file', payload)") != std::string::npos);
    CHECK(module.python_source.find("payload['path'] = path") != std::string::npos);
    CHECK(module.python_source.find("payload['offset'] = offset") != std::string::npos);
    // 人读签名索引同步生成。
    CHECK(module.signatures.find("def read_file(") != std::string::npos);
}

TEST_CASE("GenerateStubModule: 关键字参数名稳定改名,但发宿主用原名") {
    ToolDefinition definition;
    definition.name = "search";
    definition.description = "search";
    definition.input_schema = nlohmann::json::parse(R"({
        "type": "object",
        "properties": {"import": {"type": "string"}, "in": {"type": "string"}},
        "required": ["import"]
    })");
    const auto module = GenerateStubModule({StubToolInfo{definition, false, false}}, StubMode::Full);
    CHECK(module.python_source.find("def search(import_: str, in_: Optional[str] = None) -> ToolResult:") !=
          std::string::npos);
    CHECK(module.python_source.find("payload['import'] = import_") != std::string::npos);
    CHECK(module.python_source.find("payload['in'] = in_") != std::string::npos);
}

TEST_CASE("GenerateStubModule: 需确认工具在 docstring 标权限档") {
    auto info = MakeSampleTool();
    info.definition.name = "run_command";
    info.needs_confirm = true;
    info.parallel_safe = false;
    const auto module = GenerateStubModule({info}, StubMode::Full);
    CHECK(module.python_source.find("权限: 调用前会请求用户确认。") != std::string::npos);
    CHECK(module.python_source.find("并发: parallel_safe") == std::string::npos);
}

TEST_CASE("GenerateStubModule: IndexOnly 不生成调用体,留未挂载说明") {
    const auto module = GenerateStubModule({MakeSampleTool()}, StubMode::IndexOnly);
    CHECK(module.python_source.find("raise NotImplementedError") != std::string::npos);
    CHECK(module.python_source.find("call_tool('read_file', payload)") == std::string::npos);
    CHECK(module.python_source.find("索引模式") != std::string::npos);
}

TEST_CASE("GenerateStubModule: 空表也产出合法模块;输出逐字节确定") {
    const auto empty = GenerateStubModule({}, StubMode::Full);
    CHECK(empty.tool_count == 0);
    CHECK(empty.python_source.find("from ptc_runtime import") != std::string::npos);
    CHECK(empty.signatures.empty());

    const auto first = GenerateStubModule({MakeSampleTool()}, StubMode::Full);
    const auto second = GenerateStubModule({MakeSampleTool()}, StubMode::Full);
    CHECK(first.python_source == second.python_source);
    CHECK(first.signatures == second.signatures);
}

TEST_CASE("GenerateStubModule: 工具重名时后一个不覆盖前一个(两个函数都在)") {
    auto a = MakeSampleTool();
    auto b = MakeSampleTool();
    const auto module = GenerateStubModule({a, b}, StubMode::Full);
    CHECK(module.tool_count == 2);
    CHECK(module.python_source.find("def read_file(") != module.python_source.rfind("def read_file("));
}
