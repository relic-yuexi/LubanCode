// MCP 单件装配事务。宿主先定名单、环境和失败策略，再把获准的一件交进来。
// 这里只管启动、握手、发现和拥有关系；不授权工具、不渲染诊断。
#pragma once

#include <atomic>
#include <expected>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "mcp/client.hpp"
#include "tools/registry.hpp"

namespace lubancode::runtime::assembly {

// Client 地址不随 vector 扩容移动。持 Client& 的工具表须晚于 owner
// 声明，先销毁工具表，再关闭连接。package_origin 只记来源，不授予权限。
struct McpServerRuntime {
    std::string name;
    std::unique_ptr<mcp::Client> client;
    std::vector<mcp::ToolInfo> tools;
    std::optional<lubancode::tools::ToolOrigin> package_origin;
};

struct McpLaunchRequest {
    std::string name;
    std::string command;
    std::vector<std::string> args;
    std::vector<std::pair<std::string, std::string>> env;
    platform::EnvMode env_mode = platform::EnvMode::Inherit;
};

struct McpStartupOptions {
    // 非正值沿用 Client 默认；正值须在握手前落入 Client。
    int request_timeout_ms = 0;
    int tool_call_timeout_ms = 0;
    const std::atomic<bool>* cancel = nullptr;
};

enum class McpStartupStage { Start, Initialize, Discover };

struct McpStartupFailure {
    McpStartupStage stage;
    std::string error;
};

// 注入缝只替换进程启动，不绕过真实 Client 的握手/协议校验/发现。
// 成功必须交回独占 Client；自定义传输须活得比 Client 久。
using McpClientLauncher = std::function<std::expected<std::unique_ptr<mcp::Client>, std::string>(
    const McpLaunchRequest&)>;

// 失败不交出半成品，已启动的 Client 随栈关闭；成功仅交发现清单，
// 注册哪些工具、必需失败是否整场拒绝仍由宿主决定。空 launcher 用 stdio。
std::expected<McpServerRuntime, McpStartupFailure> StartMcpServer(
    const McpLaunchRequest& request, const McpStartupOptions& options = {},
    const McpClientLauncher& launcher = {});

}  // namespace lubancode::runtime::assembly
