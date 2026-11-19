#include "runtime/assembly/mcp.hpp"

namespace lubancode::runtime::assembly {
namespace {

std::expected<std::unique_ptr<mcp::Client>, std::string> LaunchStdio(const McpLaunchRequest& request) {
    auto client = std::make_unique<mcp::Client>(request.name);
    const auto started = client->StartProcess(request.command, request.args, request.env, request.env_mode,
                                             request.cwd_utf8);
    if (!started.success) return std::unexpected(started.error);
    return client;
}

bool Cancelled(const McpStartupOptions& options) {
    return options.cancel != nullptr && options.cancel->load();
}

McpStartupFailure CancelledFailure(const McpLaunchRequest& request, McpStartupStage stage) {
    return {stage, "MCP 服务器 " + request.name + " 启动已取消"};
}

}  // namespace

std::expected<McpServerRuntime, McpStartupFailure> StartMcpServer(
    const McpLaunchRequest& request, const McpStartupOptions& options, const McpClientLauncher& launcher) {
    if (Cancelled(options)) return std::unexpected(CancelledFailure(request, McpStartupStage::Start));
    auto launched = launcher ? launcher(request) : LaunchStdio(request);
    if (!launched) return std::unexpected(McpStartupFailure{McpStartupStage::Start, launched.error()});
    auto client = std::move(*launched);
    if (!client) {
        return std::unexpected(McpStartupFailure{McpStartupStage::Start, "MCP 启动器交回空 Client"});
    }
    client->SetTimeouts(options.request_timeout_ms, options.tool_call_timeout_ms);
    if (Cancelled(options)) return std::unexpected(CancelledFailure(request, McpStartupStage::Initialize));
    const auto initialized = client->Initialize(options.cancel);
    if (!initialized) {
        return std::unexpected(McpStartupFailure{McpStartupStage::Initialize, initialized.error()});
    }
    if (Cancelled(options)) return std::unexpected(CancelledFailure(request, McpStartupStage::Discover));
    auto discovered = client->ListTools(options.cancel);
    if (!discovered) {
        return std::unexpected(McpStartupFailure{McpStartupStage::Discover, discovered.error()});
    }
    if (Cancelled(options)) return std::unexpected(CancelledFailure(request, McpStartupStage::Discover));
    McpServerRuntime runtime;
    runtime.name = request.name;
    runtime.client = std::move(client);
    runtime.tools = std::move(*discovered);
    return runtime;
}

}  // namespace lubancode::runtime::assembly
