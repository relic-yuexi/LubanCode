#include "runtime/execution_owner.hpp"

#include <stdexcept>
#include <type_traits>
#include <utility>

namespace lubancode::runtime {

struct ExecutionOwner::HostAgentSlot {
    std::optional<agent::Agent> agent;
};

void ClearExecutionProfileBorrowers(agent::AgentProfile& profile) noexcept {
    profile.deferred_index_provider = nullptr;
    profile.tool_filter = nullptr;
    profile.tool_execution_policy = nullptr;
    profile.tool_turn_gate = nullptr;
    profile.tool_ref_resolver.reset();
}

ExecutionOwner::ExecutionOwner(std::unique_ptr<assembly::SessionResources> resources,
                               agent::AgentProfile&& profile,
                               std::optional<std::vector<api::Message>> restored_history)
    : session_resources_(std::move(resources)) {
    if (session_resources_ != nullptr) {
        backend_ = &session_resources_->backend();
        registry_ = &session_resources_->registry();
    }
    Construct(profile, std::move(restored_history));
}

ExecutionOwner::ExecutionOwner(HostBorrowedExecutionResources resources, agent::AgentProfile&& profile,
                               std::optional<std::vector<api::Message>> restored_history)
    : backend_(&resources.backend), registry_(&resources.registry), host_borrowed_(true) {
    Construct(profile, std::move(restored_history));
}

ExecutionOwner::ExecutionOwner(ChildExecutionResources&& resources, agent::AgentProfile&& profile,
                               std::optional<std::vector<api::Message>> restored_history)
    : backend_wrapper_(std::move(resources.backend_wrapper)),
      registry_overlay_(std::move(resources.registry_overlay)),
      backend_(backend_wrapper_ ? backend_wrapper_.get() : &resources.parent_backend),
      registry_(registry_overlay_ ? registry_overlay_.get() : &resources.parent_registry) {
    Construct(profile, std::move(restored_history));
}

void ExecutionOwner::Construct(agent::AgentProfile& profile,
                               std::optional<std::vector<api::Message>> restored_history) {
    try {
        if (backend_ == nullptr || registry_ == nullptr) {
            throw std::invalid_argument("session.execution.resources_missing");
        }
        // Copy the sole owned Agent profile while its resource graph is alive.
        // The caller's callbacks/resolver then retire before those resources,
        // including on a partial copy or history-restoration failure.
        if (host_borrowed_) {
            if (!host_agent_) host_agent_ = std::make_unique<HostAgentSlot>();
            host_agent_->agent.emplace(*backend_, *registry_, profile);
        } else {
            agent_ = std::make_unique<agent::Agent>(*backend_, *registry_, profile);
        }
        ClearExecutionProfileBorrowers(profile);
        // Explicit empty history retains the established cache-epoch reset.
        if (restored_history.has_value()) agent().RestoreSessionHistory(std::move(*restored_history));
    } catch (...) {
        // This body catch runs before constructor member unwinding.
        ClearExecutionProfileBorrowers(profile);
        throw;
    }
}

ExecutionOwner::~ExecutionOwner() = default;

agent::Agent& ExecutionOwner::agent() const {
    return host_borrowed_ ? *host_agent_->agent : *agent_;
}

bool ExecutionOwner::has_agent() const noexcept {
    return host_borrowed_ ? host_agent_ && host_agent_->agent.has_value() : agent_ != nullptr;
}

void ExecutionOwner::RebuildHostAgent(const agent::AgentProfile& profile,
                                     std::optional<std::vector<api::Message>> restored_history) {
    if (!host_borrowed_) throw std::logic_error("execution.host_borrow_required");
    if (turn_active_) throw std::logic_error("execution.turn_already_active");
    host_agent_->agent.reset();
    try {
        auto consumed_profile = profile;
        Construct(consumed_profile, std::move(restored_history));
    } catch (...) {
        host_agent_->agent.reset();
        throw;
    }
}

static_assert(std::is_nothrow_move_assignable_v<agent::AgentWiring>);
static_assert(std::is_nothrow_move_constructible_v<agent::AgentWiring>);
static_assert(std::is_nothrow_move_assignable_v<agent::TurnWiring>);

ExecutionTurnScope::ExecutionTurnScope(ExecutionOwner& execution, agent::AgentWiring&& agent_wiring,
                                       agent::TurnWiring&& turn_wiring)
    try : execution_(execution), bindings_(execution.agent()) {
        if (execution_.turn_active_) throw std::logic_error("execution.turn_already_active");
        // Snapshot empty owned wiring, so Reset consumes this turn rather than
        // keeping a second set of callbacks that borrow local dependencies.
        bindings_.Bind(wiring_, {});
        wiring_ = std::move(turn_wiring);
        execution_.agent().SetWiring(std::move(agent_wiring));
        agent_wiring = {};
        turn_wiring = {};
        execution_.turn_active_ = true;
        active_ = true;
    } catch (...) {
        // Only caller-owned prepared wiring is touched here; member unwinding
        // has already restored the unchanged Agent snapshot where applicable.
        agent_wiring = {};
        turn_wiring = {};
        throw;
    }

ExecutionTurnScope::~ExecutionTurnScope() noexcept { Reset(); }

void ExecutionTurnScope::Reset() noexcept {
    if (!active_) return;
    bindings_.Reset();
    execution_.turn_active_ = false;
    active_ = false;
}

}  // namespace lubancode::runtime
