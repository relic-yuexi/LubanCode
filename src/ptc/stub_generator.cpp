// stub_generator.hpp 的实现。纯函数;同一份定义永远生成同一份文本。

#include "ptc/stub_generator.hpp"

#include <algorithm>
#include <array>
#include <map>
#include <set>

namespace lubancode::ptc {

namespace {

// Python 关键字(3.x 全量,含软关键字 match/case——避开它们不改语义,
// 只是省得读脚本的人误会)。
bool IsPythonKeyword(std::string_view name) {
    static const std::set<std::string_view> kKeywords = {
        "False", "None",     "True",   "and",    "as",      "async",  "await",   "break",
        "class", "continue", "def",    "del",    "elif",    "else",   "except",  "finally",
        "for",   "from",     "global", "if",     "import",  "in",     "is",      "lambda",
        "nonlocal", "not",   "or",     "pass",   "raise",   "return", "try",     "while",
        "with",  "yield",    "match",  "case",   "type",    "_",
    };
    return kKeywords.count(name) != 0;
}

// 把 description 压成一行(去换行、连续空白合一),docstring 首行用。
std::string OneLine(std::string_view text) {
    std::string out;
    out.reserve(text.size());
    bool pending_space = false;
    for (const char ch : text) {
        if (ch == '\n' || ch == '\r' || ch == '\t' || ch == ' ') {
            if (!out.empty()) {
                pending_space = true;
            }
            continue;
        }
        if (pending_space) {
            out.push_back(' ');
            pending_space = false;
        }
        out.push_back(ch);
    }
    return out;
}

// schema 节点里的类型字串(没有 type 字段返回空)。
std::string SchemaTypeOf(const nlohmann::json& schema) {
    if (!schema.is_object() || !schema.contains("type") || !schema["type"].is_string()) {
        return {};
    }
    return schema["type"].get<std::string>();
}

// enum 数组 -> 逗号拼接的值列表(约束摘要用)。
std::string EnumValuesText(const nlohmann::json& enum_array) {
    std::string out;
    for (const auto& value : enum_array) {
        if (!out.empty()) {
            out += ", ";
        }
        if (value.is_string()) {
            out += value.get<std::string>();
        } else {
            out += value.dump();
        }
    }
    return out;
}

// 数字节点的展示(number/int 混着 JSON 解析,统一转 double 再判断整数)。
std::optional<double> AsNumber(const nlohmann::json& value) {
    if (value.is_number_integer()) {
        return static_cast<double>(value.get<std::int64_t>());
    }
    if (value.is_number_unsigned()) {
        return static_cast<double>(value.get<std::uint64_t>());
    }
    if (value.is_number_float()) {
        return value.get<double>();
    }
    return std::nullopt;
}

// 约束摘要里的数字文本:整数不带小数点,小数原样 dump。
std::string TrimNumber(double value) {
    if (value == static_cast<double>(static_cast<std::int64_t>(value))) {
        return std::to_string(static_cast<std::int64_t>(value));
    }
    return std::to_string(value);
}

}  // namespace

std::string PythonIdentifier(std::string_view name) {
    std::string out(name);
    if (out.empty()) {
        return "_";
    }
    // 数字开头的标识符不合法,前面垫下划线(注册表工具名不会走到这条,
    // 参数名也极少;兜底而非常态)。
    if (out[0] >= '0' && out[0] <= '9') {
        out.insert(out.begin(), '_');
    }
    if (IsPythonKeyword(out)) {
        out += "_";
    }
    return out;
}

std::string SchemaToPythonType(const nlohmann::json& schema) {
    if (!schema.is_object()) {
        return "Any";
    }
    // enum 优先:Literal 比裸 str 精确。
    if (schema.contains("enum") && schema["enum"].is_array() && !schema["enum"].empty()) {
        std::string out = "Literal[";
        bool first = true;
        for (const auto& value : schema["enum"]) {
            if (!first) {
                out += ", ";
            }
            first = false;
            if (value.is_string()) {
                out += "'" + PythonEscape(value.get<std::string>()) + "'";
            } else {
                out += value.dump();
            }
        }
        out += "]";
        return out;
    }
    // anyOf / oneOf:Union 展开;空数组兜底 Any。
    for (const char* key : {"anyOf", "oneOf"}) {
        if (schema.contains(key) && schema[key].is_array() && !schema[key].empty()) {
            std::string out;
            bool saw_null = false;
            for (const auto& sub : schema[key]) {
                const std::string sub_type = SchemaToPythonType(sub);
                if (SchemaTypeOf(sub) == "null") {
                    saw_null = true;
                    continue;
                }
                if (!out.empty()) {
                    out += ", ";
                }
                out += sub_type;
            }
            if (out.empty()) {
                return "Any";
            }
            if (saw_null) {
                return "Optional[" + out + "]";
            }
            if (out.find(',') == std::string::npos) {
                return out;  // 单成员 Union 不裹
            }
            return "Union[" + out + "]";
        }
    }
    const std::string type = SchemaTypeOf(schema);
    if (type == "string") {
        return "str";
    }
    if (type == "integer") {
        return "int";
    }
    if (type == "number") {
        return "float";
    }
    if (type == "boolean") {
        return "bool";
    }
    if (type == "array") {
        // items 存在且是单值类型时给出 list[T],否则放宽 list。
        if (schema.contains("items")) {
            const std::string item_type = SchemaToPythonType(schema["items"]);
            if (item_type != "Any") {
                return "list[" + item_type + "]";
            }
        }
        return "list";
    }
    if (type == "object") {
        return "dict";
    }
    if (type == "null") {
        return "None";
    }
    return "Any";
}

std::string SchemaConstraintSummary(const nlohmann::json& schema) {
    if (!schema.is_object()) {
        return {};
    }
    std::vector<std::string> parts;
    if (schema.contains("enum") && schema["enum"].is_array() && !schema["enum"].empty()) {
        parts.push_back("取值: " + EnumValuesText(schema["enum"]));
    }
    struct Bound {
        const char* key;
        const char* label;
    };
    for (const auto& bound : std::array<Bound, 6>{{{"minimum", "最小"}, {"maximum", "最大"},
                                                   {"minLength", "最短"}, {"maxLength", "最长"},
                                                   {"minItems", "至少"}, {"maxItems", "至多"}}}) {
        if (schema.contains(bound.key)) {
            const auto number = AsNumber(schema[bound.key]);
            if (number.has_value()) {
                parts.push_back(std::string(bound.label) + " " + TrimNumber(*number));
            }
        }
    }
    if (schema.contains("pattern") && schema["pattern"].is_string()) {
        parts.push_back("模式: " + schema["pattern"].get<std::string>());
    }
    if (schema.contains("default")) {
        parts.push_back("默认 " + schema["default"].dump());
    }
    std::string out;
    for (std::size_t i = 0; i < parts.size(); ++i) {
        if (i > 0) {
            out += "; ";
        }
        out += parts[i];
    }
    return out;
}

std::string PythonEscape(std::string_view text) {
    std::string out;
    out.reserve(text.size());
    for (const char ch : text) {
        switch (ch) {
            case '\\': out += "\\\\"; break;
            case '\'': out += "\\'"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default: out += ch; break;
        }
    }
    return out;
}

namespace {

// 一个参数的签名片段与 docstring 片段。
struct ParamStub {
    std::string signature;  // "path: str" 或 "limit: Optional[int] = None"
    std::string doc_line;   // "path: (约束)"
    bool required = true;
};

// 默认值文本:schema default 有值用它;否则可选参数一律 = None。
std::string DefaultText(const nlohmann::json& schema) {
    if (schema.is_object() && schema.contains("default")) {
        const auto& value = schema["default"];
        if (value.is_string()) {
            return "'" + PythonEscape(value.get<std::string>()) + "'";
        }
        if (value.is_null()) {
            return "None";
        }
        if (value.is_number() || value.is_boolean()) {
            return value.dump();
        }
    }
    return "None";
}

}  // namespace

StubModule GenerateStubModule(const std::vector<StubToolInfo>& tools, StubMode mode) {
    StubModule out;
    out.tool_count = tools.size();

    // 模块头:docstring + 公共 import + 从运行时库进来的底座。
    std::string header;
    header += "\"\"\"LubanCode typed tool stubs (PTC).\n\n";
    header += "用法:\n";
    header += "  from luban_tools import read_file, search\n";
    header += "  hits = search(mode=\"grep\", path=\"src\", pattern=\"HookDispatcher\")\n";
    header += "  files = [read_file(path=h[\"path\"]) for h in hits[:8]]\n";
    header += "  emit({\"hits\": hits, \"files\": files})\n";
    header += "\n";
    header += "约定:\n";
    header += "  - 每次调用都经过 LubanCode 的 schema 校验/权限/hooks/审计,与 JSON 工具调用同一条链。\n";
    header += "  - 成功结果是一个 dict: {\"content\": str, ...工具自有字段}; 工具层失败(文件不存在等)\n";
    header += "    与被拒绝(权限/hooks/限额)都会抛 ToolCallError(tool, message),用 try/except 收口。\n";
    header += "  - 结尾必须 emit(摘要), 摘要会送回模型; 不要 print 长文本。\n";
    header += "  - 可用标准库仅限纯计算(json/math/re/itertools/...); 禁网络/文件系统/子进程/环境变量。\n";
    header += "  - 结果对象也支持 await(asyncio.gather 可直接收拢;调用本身在发出时已完成)。\n";
    header += "  - 要并发发请求用 parallel([...]) 线程并发上线;定义 `async def main():` 会被自动调用,\n";
    header += "    自己不要再 asyncio.run。\n";
    header += "\n";
    header += "已挂载的工具:\n";
    for (const auto& tool : tools) {
        header += "  - " + PythonIdentifier(tool.definition.name) + ": " + OneLine(tool.definition.description) +
                  "\n";
    }
    header += "\n";
    header += "并发: parallel([...]) 可把一批调用同时发上线(asyncio.gather 兼容但串行完成)。\n";
    if (mode == StubMode::IndexOnly) {
        header += "\n(索引模式:先 tool_search 选中一组工具,再生成这些工具的完整 stub。)\n";
    }
    header += "\"\"\"\n\n";
    header += "from typing import Any, Literal, Optional, Union\n\n";
    header += "from ptc_runtime import ToolCallError, ToolResult, call_tool\n\n";

    out.python_source = header;

    // 每工具一个 stub。
    for (const auto& tool : tools) {
        const std::string func_name = PythonIdentifier(tool.definition.name);
        const nlohmann::json& schema = tool.definition.input_schema;
        // properties/required 从 schema 里取;形状不对(没 properties)时当
        // 无参工具处理,如实留一行说明。
        std::map<std::string, nlohmann::json> properties;
        if (schema.is_object() && schema.contains("properties") && schema["properties"].is_object()) {
            for (auto it = schema["properties"].begin(); it != schema["properties"].end(); ++it) {
                properties[it.key()] = it.value();
            }
        }
        std::set<std::string> required;
        if (schema.is_object() && schema.contains("required") && schema["required"].is_array()) {
            for (const auto& key : schema["required"]) {
                if (key.is_string()) {
                    required.insert(key.get<std::string>());
                }
            }
        }

        // docstring 首行:描述压一行;权限/并发注记随后;Args 段列参数约束。
        std::string doc;
        doc += "    \"\"\"" + OneLine(tool.definition.description) + "\n\n";
        if (tool.needs_confirm) {
            doc += "    权限: 调用前会请求用户确认。\n";
        } else {
            doc += "    权限: 只读,免确认。\n";
        }
        if (tool.parallel_safe) {
            doc += "    并发: parallel_safe(可与其他只读调用并发)。\n";
        }
        if (!properties.empty()) {
            doc += "\n    Args:\n";
        }
        std::vector<ParamStub> params;
        // required 在前(位置参数),可选在后(带默认值)——稳定次序:
        // 按 properties 的原始次序,再按 required/可选分两拨。
        for (const auto& [key, prop_schema] : properties) {
            if (required.count(key) == 0) {
                continue;
            }
            ParamStub param;
            const std::string py_name = PythonIdentifier(key);
            param.signature = py_name + ": " + SchemaToPythonType(prop_schema);
            param.doc_line = py_name;
            const std::string constraint = SchemaConstraintSummary(prop_schema);
            if (!constraint.empty()) {
                param.doc_line += ": " + constraint;
            }
            param.required = true;
            params.push_back(std::move(param));
        }
        for (const auto& [key, prop_schema] : properties) {
            if (required.count(key) != 0) {
                continue;
            }
            ParamStub param;
            const std::string py_name = PythonIdentifier(key);
            // 可选参数:默认值是 None 的裹 Optional[T];schema 给了真默认值
            // (如 limit=500)就不裹——类型如实;类型本身容纳任意值(Any)
            // 或已是 Optional 的照旧。
            std::string type = SchemaToPythonType(prop_schema);
            const std::string default_text = DefaultText(prop_schema);
            if (default_text == "None" && type != "Any" && type.rfind("Optional", 0) != 0) {
                type = "Optional[" + type + "]";
            }
            param.signature = py_name + ": " + type + " = " + default_text;
            param.doc_line = py_name + "(可选)";
            const std::string constraint = SchemaConstraintSummary(prop_schema);
            if (!constraint.empty()) {
                param.doc_line += ": " + constraint;
            }
            param.required = false;
            params.push_back(std::move(param));
        }
        for (const auto& param : params) {
            doc += "        " + param.doc_line + "\n";
        }
        doc += "\n    Returns:\n";
        doc += "        ToolResult(dict): {\"content\": str} —— 工具的文本结果;失败抛 ToolCallError。\n";
        doc += "    \"\"\"\n";

        // 签名行。
        std::string signature = "def " + func_name + "(";
        for (std::size_t i = 0; i < params.size(); ++i) {
            if (i > 0) {
                signature += ", ";
            }
            signature += params[i].signature;
        }
        signature += ") -> ToolResult:";

        out.python_source += signature + "\n" + doc;
        if (mode == StubMode::Full) {
            // 函数体:收进 **kwargs 之前,把入参逐个放进字典(保持名字映射
            // 稳定——就算参数被改名,发给宿主的键仍是注册表里的原名)。
            std::string body = "    payload = {}\n";
            for (const auto& [key, prop_schema] : properties) {
                const std::string py_name = PythonIdentifier(key);
                body += "    if " + py_name + " is not None:\n";
                body += "        payload['" + PythonEscape(key) + "'] = " + py_name + "\n";
            }
            body += "    return call_tool('" + PythonEscape(tool.definition.name) + "', payload)\n\n";
            out.python_source += body;
        } else {
            out.python_source += "    raise NotImplementedError('尚未挂载: 先 tool_search 选中后再生成完整 "
                                 "stubs')\n\n";
        }

        // 人读签名索引(给模型提示词)。
        out.signatures += signature + "\n";
    }

    // emit/parallel 的重导出:脚本 `from luban_tools import emit` 也行。
    out.python_source +=
        "from ptc_runtime import emit, parallel  # noqa: E402  (重导出,脚本两种 import 都认)\n";
    return out;
}

}  // namespace lubancode::ptc
