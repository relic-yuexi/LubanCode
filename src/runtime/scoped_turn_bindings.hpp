#pragma once

#include <optional>
#include <string>

#include "agent/agent.hpp"
#include "runtime/tool_trace_hub.hpp"

namespace lubancode::runtime {

class AsyncToolRuntime;
class TrajectoryTurnBridge;

// Owns only temporary bindings, never execution or business state. Declare the
// scope after its Agent, TurnWiring, hub and bridge owners, before installing
// turn-local Agent callbacks. Those owners and all callbacks must remain alive
// until Reset; no synchronous turn invocation may still be using the bindings.
// A scope covers a canonical host turn, including any DriveTurn continuations.
class ScopedTurnBindings final {
public:
    struct Bindings {
        ToolTraceHub* hub = nullptr;
        // nullopt preserves the hub's existing trajectory and wiring's recorder.
        // An explicit nullptr temporarily clears both; a pointer overrides both.
        std::optional<TrajectoryTurnBridge*> trajectory;
        AsyncToolRuntime* async_runtime = nullptr;
        // Hub routing only. The host retains its TurnWiring::turn_id policy.
        std::string thread_id;
        std::string turn_id;
        // nullopt preserves the existing projection while bound; a supplied
        // empty function temporarily removes it. Supplied overrides are restored
        // on exit; nullopt leaves the projection alone throughout.
        std::optional<ToolTraceHub::Projection> projection;
    };

    // Pure snapshot: allocation/copy failure here changes no caller state.
    explicit ScopedTurnBindings(agent::Agent& agent);
    ~ScopedTurnBindings() noexcept;
    ScopedTurnBindings(const ScopedTurnBindings&) = delete;
    ScopedTurnBindings& operator=(const ScopedTurnBindings&) = delete;
    ScopedTurnBindings(ScopedTurnBindings&&) = delete;
    ScopedTurnBindings& operator=(ScopedTurnBindings&&) = delete;

    // One Bind per scope. Failure of the first Bind, including copying
    // TurnWiring, restores changed state immediately and closes the scope.
    // A repeated Bind is rejected without disturbing the existing binding.
    void Bind(agent::TurnWiring& wiring, Bindings bindings);
    void Reset() noexcept;

private:
    agent::Agent& agent_;
    agent::AgentWiring previous_agent_wiring_;
    agent::TurnWiring* wiring_ = nullptr;
    std::optional<agent::TurnWiring> previous_turn_wiring_;
    ToolTraceHub* hub_ = nullptr;
    std::optional<ToolTraceHub::RoutingIdentity> previous_routing_identity_;
    std::optional<ToolTrajectorySink*> previous_trajectory_;
    std::optional<ToolTraceHub::Projection> previous_projection_;
    AsyncToolRuntime* async_runtime_ = nullptr;
    TrajectoryTurnBridge* previous_async_bridge_ = nullptr;
    bool active_ = true;
    bool bound_ = false;
};

}  // namespace lubancode::runtime
