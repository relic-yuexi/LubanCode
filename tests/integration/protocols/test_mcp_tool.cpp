// M8:McpTool——把 MCP 服务器的一个工具包成 tools::Tool。用 Client + 注入的
// FakeTransport 验证:name() 的拼接规则(mcp__服务器名__工具名,防止跟别的
// 服务器/内置工具撞名)、description() 前缀、input_schema() 原样透传、
// needs_confirm() 恒真、execute() 真的转发到 Client::CallTool。

#include <doctest/doctest.h>

#include "mcp/mcp_tool.hpp"

using namespace lubancode;

namespace {

class FakeTransport : public mcp::Transport {
public:
    std::function<void(const std::string&)> on_write;
    bool WriteLine(const std::string& line) override {
        if (on_write) {
            on_write(line);
        }
        return true;
    }
    void Shutdown(int) override {}
    bool IsAlive() const override { return true; }
    std::string StderrTail() const override { return std::string(); }
};

}  // namespace

TEST_CASE("McpTool: name() 拼成 mcp__服务器名__工具名") {
    mcp::Client client("myserver");
    mcp::ToolInfo info;
    info.name = "echo";
    info.description = "回显文本";
    info.input_schema = nlohmann::json::object();

    mcp::McpTool tool(client, "myserver", info);
    CHECK(tool.name() == "mcp__myserver__echo");
}

TEST_CASE("McpTool: description() 前面挂 [MCP:服务器名] 前缀") {
    mcp::Client client("srv");
    mcp::ToolInfo info;
    info.name = "add";
    info.description = "两数相加";
    mcp::McpTool tool(client, "srv", info);
    CHECK(tool.description() == "[MCP:srv] 两数相加");
}

TEST_CASE("McpTool: input_schema() 原样透传服务器给的 JSON Schema") {
    mcp::Client client("srv");
    mcp::ToolInfo info;
    info.name = "add";
    info.input_schema = nlohmann::json::parse(R"({
        "type": "object",
        "properties": {"a": {"type": "number"}, "b": {"type": "number"}},
        "required": ["a", "b"]
    })");
    mcp::McpTool tool(client, "srv", info);
    CHECK(tool.input_schema() == info.input_schema);
}

TEST_CASE("McpTool: needs_confirm() 恒为 true——外部代码,哪怕 Auto 模式也不该被当文件类工具自动放行") {
    mcp::Client client("srv");
    mcp::ToolInfo info;
    info.name = "echo";
    mcp::McpTool tool(client, "srv", info);
    CHECK(tool.needs_confirm());
}

TEST_CASE("McpTool: execute() 真的转发到 Client::CallTool,参数和结果原样传递") {
    FakeTransport transport;
    mcp::Client client("srv");
    client.AttachTransportForTest(&transport);

    nlohmann::json captured_params;
    transport.on_write = [&](const std::string& line) {
        const auto request = nlohmann::json::parse(line);
        captured_params = request.at("params");
        const nlohmann::json response = {
            {"jsonrpc", "2.0"},
            {"id", request.at("id")},
            {"result", {{"content", nlohmann::json::array({{{"type", "text"}, {"text", "结果来了"}}})}}}};
        client.OnLine(response.dump());
    };

    mcp::ToolInfo info;
    info.name = "echo";
    mcp::McpTool tool(client, "srv", info);

    const nlohmann::json input = {{"text", "你好"}};
    const auto result = tool.execute(input);

    CHECK_FALSE(result.is_error);
    CHECK(result.content == "结果来了");
    CHECK(captured_params.at("name") == "echo");        // 转发的是 MCP 原始工具名,不是拼接后的全名
    CHECK(captured_params.at("arguments") == input);
}
