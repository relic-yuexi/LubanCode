// tools 层的工具基类。每个工具只管把一件事做好(读文件、跑命令……),
// 不关心是谁在调它——agent 层拿着 ToolRegistry 按名字找、按 schema 拼进请求。

#pragma once

#include <string>

#include <nlohmann/json.hpp>

namespace lubancode::tools {

class Tool {
public:
    virtual ~Tool() = default;

    // 工具名,模型靠这个发起调用(tool_use 里的 name)。
    virtual std::string name() const = 0;

    // 给模型看的一段话,说明这工具是干什么的、什么时候该用。
    virtual std::string description() const = 0;

    // 入参的 JSON Schema,手写、拼进请求里给模型看。
    virtual nlohmann::json input_schema() const = 0;

    // 执行前要不要先问用户一句。默认不用(比如 read_file 这种只读的)。
    virtual bool needs_confirm() const { return false; }

    // tool_search:这个工具是不是"延迟挂载"的——注册表总工具数超过阈值时,
    // 延迟工具不直接进请求的 tools 数组,只在系统提示的索引段里露个名字,
    // 模型用 tool_search 检索命中后才真正挂载。默认 false(内置九件套和
    // web_fetch/web_search 都是核心,恒在);McpTool(经 DeferredTool 包装)、
    // PluginTool、LuaTool 这些外挂工具才是 true——它们是工具表膨胀的主力。
    virtual bool deferred() const { return false; }

    struct Result {
        std::string content;    // 回传给模型的文本(成功的结果,或者人能看懂的错误说明)
        bool is_error = false;  // 执行失败/被拒绝时置位
    };

    // 真正执行。input 是模型给的入参(已经是解析好的 JSON 对象)。
    // 失败(参数错、文件不存在、命令跑挂了……)不抛异常,用 Result::is_error 传回去。
    virtual Result execute(const nlohmann::json& input) = 0;
};

}  // namespace lubancode::tools
