#pragma once

#include <memory>
#include <optional>
#include <vector>

#include "agent/agent.hpp"
#include "runtime/assembly/session_resources.hpp"
#include "runtime/scoped_turn_bindings.hpp"

namespace lubancode::runtime {

// Clear only profile fields that can borrow execution resources. The source
// profile is consumed even if copying it into the owned Agent fails.
void ClearExecutionProfileBorrowers(agent::AgentProfile& profile) noexcept;

// A synchronous host keeps its original backend and registry alive. This owner
// creates only the Agent; no child overlay or owned SessionResources is implied.
struct HostBorrowedExecutionResources {
    api::Backend& backend;
    tools::ToolRegistry& registry;
};

// Only the wrapper and overlay are owned here. They can forward to the parent's
// original backend/tools (including MCP tools); those caller-owned resources
// must outlive this entire synchronous child execution. Nothing clones them.
struct ChildExecutionResources {
    api::Backend& parent_backend;
    tools::ToolRegistry& parent_registry;
    std::unique_ptr<api::Backend> backend_wrapper;
    std::unique_ptr<tools::ToolRegistry> registry_overlay;
};

class ExecutionTurnScope;

// The shared Agent construction, optional restore and destruction boundary.
// A root owns its SessionResources; a child owns only its wrapper/overlay while
// explicitly borrowing the parent's resources. A synchronous host borrows both
// original resources. Hosts still assemble each plan.
class ExecutionOwner final {
public:
    ExecutionOwner(std::unique_ptr<assembly::SessionResources> resources,
                   agent::AgentProfile&& profile,
                   std::optional<std::vector<api::Message>> restored_history = std::nullopt);
    ExecutionOwner(HostBorrowedExecutionResources resources, agent::AgentProfile&& profile,
                   std::optional<std::vector<api::Message>> restored_history = std::nullopt);
    ExecutionOwner(ChildExecutionResources&& resources, agent::AgentProfile&& profile,
                   std::optional<std::vector<api::Message>> restored_history = std::nullopt);
    ~ExecutionOwner();
    ExecutionOwner(const ExecutionOwner&) = delete;
    ExecutionOwner& operator=(const ExecutionOwner&) = delete;
    ExecutionOwner(ExecutionOwner&&) = delete;
    ExecutionOwner& operator=(ExecutionOwner&&) = delete;

    agent::Agent& agent() const;
    bool has_agent() const noexcept;
    // Only a synchronous host can replace the Agent in its stable slot. All
    // old borrows are invalid during replacement or after a failed replacement.
    void RebuildHostAgent(const agent::AgentProfile& profile,
                          std::optional<std::vector<api::Message>> restored_history = std::nullopt);
    api::Backend& backend() const { return *backend_; }
    tools::ToolRegistry& registry() const { return *registry_; }
    // Non-null only for the full root graph. Child and host borrows are never
    // presented as an owned SessionResources graph.
    assembly::SessionResources* session_resources() const { return session_resources_.get(); }

private:
    friend class ExecutionTurnScope;
    void Construct(agent::AgentProfile& profile,
                   std::optional<std::vector<api::Message>> restored_history);

    // Reverse teardown: Agent -> overlay -> wrapper, or Agent -> full resource
    // graph. External parent/plugin owners must outlive all of these members.
    std::unique_ptr<assembly::SessionResources> session_resources_;
    std::unique_ptr<api::Backend> backend_wrapper_;
    std::unique_ptr<tools::ToolRegistry> registry_overlay_;
    api::Backend* backend_ = nullptr;
    tools::ToolRegistry* registry_ = nullptr;
    std::unique_ptr<agent::Agent> agent_;
    struct HostAgentSlot;
    // One host-only allocation retains the Agent address across rebuilds.
    // Root/Child do not allocate this slot or move their original Agent graph.
    std::unique_ptr<HostAgentSlot> host_agent_;
    bool host_borrowed_ = false;
    bool turn_active_ = false;
};

// Consumes prepared wiring for a synchronous canonical turn and continuations.
// Declare after every dependency borrowed by that wiring. Reset restores prior
// Agent callbacks first, then destroys this turn's wiring, before any dependency
// can retire. ExecutionOwner must outlive the scope; no invocation may remain
// in flight when it resets. This scope does not cancel or join worker threads.
class ExecutionTurnScope final {
public:
    ExecutionTurnScope(ExecutionOwner& execution, agent::AgentWiring&& agent_wiring,
                       agent::TurnWiring&& turn_wiring);
    ~ExecutionTurnScope() noexcept;
    ExecutionTurnScope(const ExecutionTurnScope&) = delete;
    ExecutionTurnScope& operator=(const ExecutionTurnScope&) = delete;
    ExecutionTurnScope(ExecutionTurnScope&&) = delete;
    ExecutionTurnScope& operator=(ExecutionTurnScope&&) = delete;

    agent::TurnWiring& wiring() { return wiring_; }
    void Reset() noexcept;

private:
    ExecutionOwner& execution_;
    agent::TurnWiring wiring_;
    ScopedTurnBindings bindings_;
    bool active_ = false;
};

}  // namespace lubancode::runtime
