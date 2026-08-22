// M8:把一个 MCP 服务器暴露的单个工具,包装成 tools::Tool,好塞进
// ToolRegistry,跟 read_file/run_command 这些内置工具一视同仁——agent loop
// 那边完全不知道它背后是个外部子进程。
//
// 持有 mcp::Client& 而不是 shared_ptr,跟 AgentTool 持有 api::Backend&/
// ToolRegistry& 是同一个路数:Client 的生命周期由 main.cpp 里更外层的
// McpServerRuntime 管着,声明顺序保证 Client 活得比持有它引用的 McpTool 久
// (main.cpp 里 mcp_servers 声明在 registry 之前,先声明的后析构)。
#pragma once

#include <string>

#include "mcp/client.hpp"
#include "tools/tool.hpp"

namespace lubancode::mcp {

class McpTool : public lubancode::tools::Tool {
public:
    McpTool(Client& client, std::string server_name, ToolInfo info);

    std::string name() const override;
    std::string description() const override;
    nlohmann::json input_schema() const override;
    bool needs_confirm() const override;
    // 逐枚追踪单:MCP 远端工具按远端未知副作用声明。协议没声明幂等/
    // 补偿,一律按远端不可逆档处理——server metadata 将来若明示
    // idempotent/compensatable,须在装配层显式改,不默认放宽。
    tools::EffectClass effect_class() const override { return tools::EffectClass::RemoteIrreversible; }
    tools::Tool::Result execute(const nlohmann::json& input) override;

private:
    Client& client_;
    std::string server_name_;
    ToolInfo info_;
    std::string full_name_;
};

}  // namespace lubancode::mcp
