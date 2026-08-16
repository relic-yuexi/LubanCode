// ToolDefinition -> Python stub 模块生成器(规格"真正的 stub 模块"节)。
//
// 把注册表里的工具 schema 翻成只读 Python 模块文本 luban_tools.py:
//   - JSON Schema 的 object/string/integer/number/boolean/array/enum/anyOf
//     映射成 Python 类型注解(Literal/Union/嵌套放宽为 dict/list,如实注明);
//   - required 参数是位置参数,可选参数带默认值(schema default 或 None);
//   - docstring 留参数约束、权限等级(needs_confirm)、输出摘要,不把几页
//     说明全塞进去;
//   - 工具名/参数名冲突时用稳定规则(Python 关键字/内置名加后缀 "_"),
//     不临场改名;
//   - tool_search 延迟挂载仍生效:GenerateStubModule 吃的是"已挂载"的
//     ToolDefinition 列表,延迟未挂载的工具根本不在名单里;另有 IndexOnly
//     模式只生成索引(名字 + 一行说明),给首屏用,选中一组后再生成全量。
//
// 纯函数,不碰 IO:给定同一份定义,输出逐字节确定(可单测钉死)。

#pragma once

#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "api/types.hpp"

namespace lubancode::ptc {

// stub 生成器的输出。
struct StubModule {
    std::string python_source;  // 完整 luban_tools.py 文本
    std::string signatures;     // 人读的签名索引(每工具一行),喂给模型的提示词用
    std::size_t tool_count = 0;
};

// 一枚工具生成 stub 需要的元数据(比 api::ToolDefinition 多一档权限信息)。
struct StubToolInfo {
    api::ToolDefinition definition;
    bool needs_confirm = false;   // 权限等级:docstring 里写"调用前会请求用户确认"
    bool parallel_safe = false;   // 只读可并发标记(P2 并发窗用),docstring 注明
};

// 生成模式。
enum class StubMode {
    // 全量:每个工具一个完整 stub 函数(签名 + docstring + RPC 调用)。
    Full,
    // 索引:模块 docstring 里只列工具名 + 一行说明,函数体只有 raise
    // NotImplementedError("未挂载")——首屏省 token,选中后重新生成全量。
    IndexOnly,
};

// 生成 luban_tools.py。tools 为空时也产出合法模块(只有运行时说明)。
StubModule GenerateStubModule(const std::vector<StubToolInfo>& tools, StubMode mode);

// ---- 内部纯函数(导出供单测直接钉) ----

// 工具名/参数名 -> 合法 Python 标识符。关键字/内置名冲突加 "_" 后缀,
// 其余字符原样(注册表里的名字本就是 snake_case);空名兜底成 "_"。
std::string PythonIdentifier(std::string_view name);

// JSON Schema 节点 -> Python 类型注解文本。known_only=false(默认)时嵌套
// object/array 放宽成 dict[str, object] / list,并在 docstring 的参数约束里
// 留原始 shape(如实,不装精确)。
std::string SchemaToPythonType(const nlohmann::json& schema);

// 参数约束摘要(enum/minimum/maximum/minLength/pattern/description),
// docstring 的 Args: 段用。返回空串 = 没有可摘的约束。
std::string SchemaConstraintSummary(const nlohmann::json& schema);

// Python 字符串字面量转义(单引号版,docstring/描述文本用)。
std::string PythonEscape(std::string_view text);

}  // namespace lubancode::ptc
