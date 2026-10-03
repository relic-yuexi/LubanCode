#pragma once

#include <memory>
#include <optional>
#include <vector>

#include "runtime/execution_owner.hpp"

namespace lubancode::runtime {

// A session's model execution and everything it borrows. Hosts interpret their
// profiles and resource plans; construction, history restoration and destruction
// use this owner. It does not dispatch inputs or install per-turn callbacks.
class SessionExecution final {
public:
    // Consumes the source profile's callbacks/resolver even on failure; value
    // metadata remains available to the host after this construction attempt.
    SessionExecution(std::unique_ptr<assembly::SessionResources> resources,
                     agent::AgentProfile&& profile,
                     std::optional<std::vector<api::Message>> restored_history = std::nullopt);
    ~SessionExecution();
    SessionExecution(const SessionExecution&) = delete;
    SessionExecution& operator=(const SessionExecution&) = delete;
    SessionExecution(SessionExecution&&) = delete;
    SessionExecution& operator=(SessionExecution&&) = delete;

    agent::Agent& agent() const { return execution_.agent(); }
    assembly::SessionResources& resources() const { return *execution_.session_resources(); }

private:
    ExecutionOwner execution_;
};

}  // namespace lubancode::runtime
