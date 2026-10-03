#pragma once

#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "lubancore/core.hpp"
#include "runtime/scoped_approval.hpp"

namespace lubancore::detail {

// A trusted host supplies copies of actual ownership facts. Parent operation is
// causal/root ticket ownership only: there is no child operation or attempt.
// Structural validation here does not prove a declared child ledger identity;
// child bridge admission and its hook remain a later, separate batch.
struct ScopedApprovalOwner {
    std::string parent_session_id;
    std::string parent_operation_id;
    std::string child_session_id;
    std::string child_run_id;
    std::string child_turn_id;
    std::string child_declared_action_id;
    std::string effective_cwd;
};

class SessionApprovals {
public:
    using Publisher = std::function<void(const Approval&, const ScopedApprovalOwner&)>;
    SessionApprovals();
    ~SessionApprovals();
    SessionApprovals(const SessionApprovals&) = delete;
    SessionApprovals& operator=(const SessionApprovals&) = delete;

    // The SDK worker sets this from the actual popped/accepted operation, never
    // from a child-provided operation. It does not grant child execution rights.
    void SetOperationOwner(std::string session_id, std::string operation_id, std::string run_id = {});
    static std::shared_ptr<lubancode::runtime::InteractionFuture> CancelledFuture();
    std::shared_ptr<lubancode::runtime::InteractionFuture> Register(
        Approval approval, std::chrono::milliseconds timeout,
        const std::atomic<bool>* interrupt = nullptr, bool* registered = nullptr);
    Result<lubancode::runtime::ApprovalLease> RegisterScoped(
        ScopedApprovalOwner owner, Approval approval, std::chrono::milliseconds timeout,
        Publisher publish);
    using ChildPublisher = std::function<void(const Approval&, const lubancode::runtime::ChildApprovalRequest&)>;
    Result<lubancode::runtime::ApprovalLease> RegisterChildScoped(
        lubancode::runtime::ChildApprovalRequest request, Approval approval,
        std::chrono::milliseconds timeout, ChildPublisher publish);
    bool ChildAllowed(const lubancode::runtime::ChildApprovalScope& scope, const std::string& tool) const;
    void CloseChildScope(const lubancode::runtime::ChildApprovalScope& scope) noexcept;
    bool Resolve(const std::string& request_id, const lubancode::runtime::ApprovalResponse& response);
    void Retire(const std::string& request_id) noexcept;
    void CancelAll() noexcept;
    void Close() noexcept;
    std::vector<Approval> Pending() const;
    std::set<std::string> AllowedTools() const;

private:
    Result<lubancode::runtime::ApprovalLease> RegisterScopedImpl(
        ScopedApprovalOwner owner, Approval approval, std::chrono::milliseconds timeout,
        Publisher publish, std::optional<lubancode::runtime::ChildApprovalScope> child);
    struct State;
    std::shared_ptr<State> state_;
};

} // namespace lubancore::detail
