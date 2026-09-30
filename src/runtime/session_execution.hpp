#pragma once

#include <memory>
#include <optional>
#include <vector>

#include "agent/agent.hpp"
#include "runtime/assembly/session_resources.hpp"

namespace lubancode::runtime {

// Clear only the profile fields that can borrow execution resources. This is
// an internal consumption boundary, not a change to AgentProfile's value type.
void ClearExecutionProfileBorrowers(agent::AgentProfile& profile) noexcept;

// A session's model execution and everything it borrows. Hosts interpret their
// profiles and resource plans; construction, history restoration and destruction
// use this owner. It does not dispatch inputs or install per-turn callbacks.
class SessionExecution final {
public:
    SessionExecution(std::unique_ptr<assembly::SessionResources> resources,
                     agent::AgentProfile&& profile,
                     std::optional<std::vector<api::Message>> restored_history = std::nullopt);
    ~SessionExecution();
    SessionExecution(const SessionExecution&) = delete;
    SessionExecution& operator=(const SessionExecution&) = delete;
    SessionExecution(SessionExecution&&) = delete;
    SessionExecution& operator=(SessionExecution&&) = delete;

    agent::Agent& agent() const { return *agent_; }
    assembly::SessionResources& resources() const { return *resources_; }

private:
    // Reverse destruction: Agent -> registry -> MCP Clients -> backend. Any
    // external plugin owner borrowed by a tool must outlive this entire object.
    std::unique_ptr<assembly::SessionResources> resources_;
    std::unique_ptr<agent::Agent> agent_;
};

}  // namespace lubancode::runtime
