#include "mcp/mcp_tool.hpp"

namespace lubancode::mcp {

McpTool::McpTool(Client& client, std::string server_name, ToolInfo info)
    : client_(client),
      server_name_(std::move(server_name)),
      info_(std::move(info)),
      full_name_("mcp__" + server_name_ + "__" + info_.name) {}

std::string McpTool::name() const {
    return full_name_;
}

std::string McpTool::description() const {
    return "[MCP:" + server_name_ + "] " + info_.description;
}

nlohmann::json McpTool::input_schema() const {
    return info_.input_schema;
}

bool McpTool::needs_confirm() const {
    // 外部代码,默认要确认——哪怕在 Auto 模式下也不该被当成文件类工具自动
    // 放行(main.cpp 的 on_tool_confirm 只对 write_file/edit_file 在 Auto
    // 模式下免确认,MCP 工具落在"命令类"这一档,天然满足这条要求)。
    return true;
}

tools::Tool::Result McpTool::execute(const nlohmann::json& input) {
    return execute(input, tools::ToolExecutionContext{});
}

tools::Tool::Result McpTool::execute(const nlohmann::json& input, const tools::ToolExecutionContext& context) {
    // 逐枚追踪单:内层 JSON-RPC id 随结果带出(details.jsonrpc_request_id),
    // 外层 execution 关联账从这拿(单子"MCP 外层 execution 要挂内层")。
    // MCP 富结果单 P0.5:本轮会话的 artifact 目录随 context 递进——二进制
    // 块先落 <会话>/mcp-artifacts 再入史;没有落盘地由解析层按稳定错收口。
    CallOptions options;
    options.artifact_dir = context.artifact_dir;
    if (info_.output_schema.has_value()) {
        options.output_schema = &*info_.output_schema;
    }
    std::int64_t jsonrpc_request_id = -1;
    tools::Tool::Result result = client_.CallTool(info_.name, input, &jsonrpc_request_id, options);
    if (jsonrpc_request_id >= 0) {
        result.details["jsonrpc_request_id"] = jsonrpc_request_id;
        result.details["mcp_server"] = server_name_;
        result.details["mcp_tool"] = info_.name;
        result.details["transport_generation"] = client_.transport_generation();
    }
    result.effect_summary = "mcp " + server_name_ + "/" + info_.name;
    return result;
}

}  // namespace lubancode::mcp
