#pragma once

#include <expected>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "api/backend.hpp"
#include "runtime/assembly/mcp.hpp"
#include "tools/registry.hpp"

namespace lubancode::runtime::assembly {

enum class SessionResourceStage { Plan, Backend, Mcp, Registry };
struct SessionResourceFailure {
    SessionResourceStage stage;
    std::string code;
    std::string message;
    std::string component;
    std::optional<McpStartupStage> mcp_stage;
};

struct SessionMcpSpec {
    McpLaunchRequest launch;
    McpStartupOptions startup;
    bool required = true;
};

using SessionRegistryResult =
    std::expected<std::unique_ptr<tools::ToolRegistry>, SessionResourceFailure>;

struct SessionResourcesRequest {
    // Fully interpreted host input. No configuration/home/environment lookup is
    // performed here. Every MCP launch must carry an absolute session cwd.
    std::vector<SessionMcpSpec> mcp_servers;
    McpClientLauncher mcp_launcher;
    std::function<std::unique_ptr<api::Backend>()> backend_factory;
    // Called synchronously, after discovery; never retained in the owner. The
    // host chooses tools and records their origin/permission metadata. Returned
    // tools may borrow these Clients; all other dependencies must outlive owner.
    std::function<SessionRegistryResult(std::span<const McpServerRuntime>)> registry_factory;
};

class SessionResources;
using SessionResourcesResult =
    std::expected<std::unique_ptr<SessionResources>, SessionResourceFailure>;

// One session's execution dependencies. Publish/move only the unique_ptr: an
// Agent borrows backend/registry, and McpTool borrows a Client. Stop and destroy
// every such borrower before releasing this owner. No executor lives here.
class SessionResources final {
public:
    ~SessionResources() = default;
    SessionResources(const SessionResources&) = delete;
    SessionResources& operator=(const SessionResources&) = delete;
    SessionResources(SessionResources&&) = delete;
    SessionResources& operator=(SessionResources&&) = delete;

    api::Backend& backend() const { return *backend_; }
    tools::ToolRegistry& registry() const { return *registry_; }
    std::span<const McpServerRuntime> mcp_servers() const { return mcp_servers_; }
    const std::vector<SessionResourceFailure>& degraded() const { return degraded_; }

private:
    SessionResources() = default;
    friend SessionResourcesResult BuildSessionResources(SessionResourcesRequest);

    // Destruction order is registry -> Clients -> backend. Neither a failed
    // registry callback nor a failed later MCP launch publishes a partial owner.
    std::unique_ptr<api::Backend> backend_;
    std::vector<McpServerRuntime> mcp_servers_;
    std::unique_ptr<tools::ToolRegistry> registry_;
    std::vector<SessionResourceFailure> degraded_;
};

// Validate the complete launch plan before calling a factory or starting a
// process. Required failure rolls back all candidates; optional MCP failure is
// returned as data. Callback exceptions also unwind candidates, then propagate
// to the host's existing error boundary; no logging or process-state mutation.
SessionResourcesResult BuildSessionResources(SessionResourcesRequest request);

}  // namespace lubancode::runtime::assembly
