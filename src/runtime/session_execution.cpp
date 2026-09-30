#include "runtime/session_execution.hpp"

#include <stdexcept>
#include <utility>

namespace lubancode::runtime {

SessionExecution::SessionExecution(std::unique_ptr<assembly::SessionResources> resources,
                                   agent::AgentProfile profile,
                                   std::optional<std::vector<api::Message>> restored_history)
    : resources_(std::move(resources)) {
    if (resources_ == nullptr) throw std::invalid_argument("session.execution.resources_missing");
    agent_ = std::make_unique<agent::Agent>(resources_->backend(), resources_->registry(), std::move(profile));
    // An explicitly empty restored history still resets ContextManager's cache
    // epoch and rebuilds discovery state. It is different from a fresh session.
    if (restored_history.has_value()) agent_->RestoreSessionHistory(std::move(*restored_history));
}

SessionExecution::~SessionExecution() = default;

}  // namespace lubancode::runtime
