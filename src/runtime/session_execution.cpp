#include "runtime/session_execution.hpp"

#include <stdexcept>
#include <utility>

namespace lubancode::runtime {

void ClearExecutionProfileBorrowers(agent::AgentProfile& profile) noexcept {
    profile.deferred_index_provider = nullptr;
    profile.tool_filter = nullptr;
    profile.tool_execution_policy = nullptr;
    profile.tool_turn_gate = nullptr;
    profile.tool_ref_resolver.reset();
}

SessionExecution::SessionExecution(std::unique_ptr<assembly::SessionResources> resources,
                                   agent::AgentProfile&& profile,
                                   std::optional<std::vector<api::Message>> restored_history)
    : resources_(std::move(resources)) {
    try {
        if (resources_ == nullptr) throw std::invalid_argument("session.execution.resources_missing");
        // Make the sole owned Agent profile here. Its parameter copies, and any
        // failed partial copy, retire while resources_ still owns their borrows.
        // The consuming outer boundaries take references, so they leave no
        // moved-from std::function copies behind after a constructor failure.
        agent_ = std::make_unique<agent::Agent>(resources_->backend(), resources_->registry(), profile);
        ClearExecutionProfileBorrowers(profile);
        // An explicitly empty restored history still resets ContextManager's
        // cache epoch and rebuilds discovery state; it differs from fresh.
        if (restored_history.has_value()) agent_->RestoreSessionHistory(std::move(*restored_history));
    } catch (...) {
        // A normal body catch runs before member unwinding. A constructor
        // function-try-block would already have destroyed resources_ here.
        ClearExecutionProfileBorrowers(profile);
        throw;
    }
}

SessionExecution::~SessionExecution() = default;

}  // namespace lubancode::runtime
