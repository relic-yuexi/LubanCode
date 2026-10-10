#include "runtime/session_execution.hpp"

#include <utility>

namespace lubancode::runtime {

SessionExecution::SessionExecution(std::unique_ptr<assembly::SessionResources> resources,
                                   agent::AgentProfile&& profile,
                                   std::optional<std::vector<api::Message>> restored_history)
    : execution_(std::move(resources), std::move(profile), std::move(restored_history)) {}

SessionExecution::~SessionExecution() = default;

}  // namespace lubancode::runtime
