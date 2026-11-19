#include "runtime/scoped_turn_bindings.hpp"

#include <stdexcept>
#include <type_traits>
#include <utility>

#include "runtime/async_tool_runtime.hpp"
#include "runtime/trajectory_turn_bridge.hpp"

namespace lubancode::runtime {

static_assert(std::is_nothrow_move_assignable_v<agent::AgentWiring>);
static_assert(std::is_nothrow_move_constructible_v<agent::AgentWiring>);
static_assert(std::is_nothrow_move_assignable_v<agent::TurnWiring>);
static_assert(std::is_nothrow_move_constructible_v<ToolTraceHub::Projection>);

ScopedTurnBindings::ScopedTurnBindings(agent::Agent& agent)
    : agent_(agent), previous_agent_wiring_(agent.wiring()) {}

ScopedTurnBindings::~ScopedTurnBindings() noexcept { Reset(); }

void ScopedTurnBindings::Bind(agent::TurnWiring& wiring, Bindings bindings) {
    if (!active_ || bound_) throw std::logic_error("turn_bindings.already_bound_or_closed");
    try {
        if (bindings.projection.has_value() && bindings.hub == nullptr) {
            throw std::invalid_argument("turn_bindings.projection_requires_hub");
        }
        if (bindings.async_runtime != nullptr && (!bindings.trajectory || *bindings.trajectory == nullptr)) {
            throw std::invalid_argument("turn_bindings.async_requires_trajectory");
        }
        previous_turn_wiring_.emplace(wiring);
        wiring_ = &wiring; // Publish only after the full snapshot succeeded.
        bound_ = true;
        if (bindings.hub != nullptr) {
            // Copy the routing snapshot before the first hub mutation. Event
            // history and active execution facts are not binding state.
            previous_routing_identity_.emplace(bindings.hub->routing_identity());
            hub_ = bindings.hub;
            if (bindings.trajectory.has_value()) {
                previous_trajectory_.emplace(hub_->ExchangeTrajectory(*bindings.trajectory));
            }
            if (bindings.projection.has_value()) {
                previous_projection_.emplace(hub_->ExchangeProjection(std::move(*bindings.projection)));
            }
            // The trajectory capability controls tool-result rewrite wiring.
            // Attach it before Install inspects that capability.
            // Install only adds this hook for preview-capable sinks. Remove an
            // earlier hub closure first so null/non-preview bindings cannot call
            // it against a now absent trajectory; Reset restores the old hook.
            wiring.rewrite_tool_results_for_history = {};
            hub_->Install(agent_, wiring, bindings.thread_id, bindings.turn_id);
        }
        if (bindings.trajectory.has_value()) wiring.boundary_recorder = *bindings.trajectory;
        if (bindings.async_runtime != nullptr) {
            previous_async_bridge_ = bindings.async_runtime->ExchangeTurnBridge(*bindings.trajectory);
            async_runtime_ = bindings.async_runtime;
            wiring.tool_batch_gate = async_runtime_->gate();
            wiring.delivery_planner = async_runtime_->planner();
        }
    } catch (...) {
        Reset();
        throw;
    }
}

void ScopedTurnBindings::Reset() noexcept {
    if (!active_) return;
    active_ = false;
    // Exchange under the same mutex used by bridge queries, before its owner
    // may destroy it. The ordinary previous value is null; nested use is LIFO.
    if (async_runtime_ != nullptr) {
        (void)async_runtime_->ExchangeTurnBridge(previous_async_bridge_);
    }
    agent_.SetWiring(std::move(previous_agent_wiring_));
    if (wiring_ != nullptr) *wiring_ = std::move(*previous_turn_wiring_);
    if (hub_ != nullptr) {
        if (previous_trajectory_.has_value()) (void)hub_->ExchangeTrajectory(*previous_trajectory_);
        (void)hub_->ExchangeRoutingIdentity(std::move(*previous_routing_identity_));
        if (previous_projection_.has_value()) {
            (void)hub_->ExchangeProjection(std::move(*previous_projection_));
        }
    }
}

}  // namespace lubancode::runtime
