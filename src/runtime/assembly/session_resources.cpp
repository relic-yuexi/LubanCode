#include "runtime/assembly/session_resources.hpp"

#include <set>
#include <utility>

#include "platform/text_encoding.hpp"
#include "tools/path_utils.hpp"

namespace lubancode::runtime::assembly {

SessionResourcesResult BuildSessionResources(SessionResourcesRequest request) {
    if (!request.backend_factory) {
        return std::unexpected(SessionResourceFailure{SessionResourceStage::Plan,
            "assembly.backend_factory_missing", "backend factory is required", {}, {}});
    }
    if (!request.registry_factory) {
        return std::unexpected(SessionResourceFailure{SessionResourceStage::Plan,
            "assembly.registry_factory_missing", "registry factory is required", {}, {}});
    }
    std::set<std::string> names;
    std::vector<std::optional<SessionResourceFailure>> launch_errors;
    launch_errors.reserve(request.mcp_servers.size());
    for (const auto& spec : request.mcp_servers) {
        if (spec.launch.name.empty() || !names.insert(spec.launch.name).second ||
            (spec.required && spec.launch.command.empty()) || !platform::IsValidUtf8(spec.launch.cwd_utf8) ||
            !tools::Utf8ToPath(spec.launch.cwd_utf8).is_absolute()) {
            return std::unexpected(SessionResourceFailure{SessionResourceStage::Plan,
                "assembly.mcp.invalid_spec", "MCP requires a unique name, command and absolute cwd",
                spec.launch.name, {}});
        }
        // A malformed execution context is ambiguous even for an optional
        // component. An unavailable optional command retains the host's ordinary
        // startup-degradation policy, without attempting to spawn an empty name.
        if (spec.launch.command.empty()) {
            launch_errors.emplace_back(SessionResourceFailure{SessionResourceStage::Mcp,
                "assembly.mcp.start_failed", "MCP command is empty", spec.launch.name, McpStartupStage::Start});
        } else {
            launch_errors.emplace_back(std::nullopt);
        }
    }

    auto candidate = std::unique_ptr<SessionResources>(new SessionResources());
    candidate->backend_ = request.backend_factory();
    if (!candidate->backend_) {
        return std::unexpected(SessionResourceFailure{SessionResourceStage::Backend,
            "assembly.backend_unavailable", "backend factory returned null", {}, {}});
    }
    for (std::size_t i = 0; i < request.mcp_servers.size(); ++i) {
        const auto& spec = request.mcp_servers[i];
        if (launch_errors[i]) {
            candidate->degraded_.push_back(std::move(*launch_errors[i]));
            continue;
        }
        auto started = StartMcpServer(spec.launch, spec.startup, request.mcp_launcher);
        if (!started) {
            SessionResourceFailure failure{SessionResourceStage::Mcp, "assembly.mcp.start_failed",
                started.error().error, spec.launch.name, started.error().stage};
            if (spec.required) return std::unexpected(std::move(failure));
            candidate->degraded_.push_back(std::move(failure));
            continue;
        }
        candidate->mcp_servers_.push_back(std::move(*started));
    }
    auto registry = request.registry_factory(candidate->mcp_servers_);
    if (!registry) {
        auto failure = std::move(registry.error());
        failure.stage = SessionResourceStage::Registry;
        return std::unexpected(std::move(failure));
    }
    if (!*registry) {
        return std::unexpected(SessionResourceFailure{SessionResourceStage::Registry,
            "assembly.registry_unavailable", "registry factory returned null", {}, {}});
    }
    candidate->registry_ = std::move(*registry);
    return candidate;
}

}  // namespace lubancode::runtime::assembly
