#pragma once

#include <atomic>
#include <functional>
#include <memory>
#include <string>

#include "runtime/interaction.hpp"

namespace lubancode::runtime {

// A trusted foreground host supplies copied facts. The host operation owns the
// ticket, not a child execution. Ticket turn/action must not be grant keys.
struct ChildApprovalScope {
    std::string host_session_id, host_run_id, host_operation_id;
    std::string parent_session_id, parent_run_id;
    std::string child_session_id, child_run_id;
    std::string effective_cwd, permission_floor;
    bool operator==(const ChildApprovalScope&) const = default;
};

struct ChildApprovalRequest {
    ChildApprovalScope scope;
    std::string child_turn_id, child_declared_action_id, child_declared_message_id;
    int owner_task_id = 0;
    ApprovalRequest request;
};

// This is an explicit capability. An ordinary blocking InteractionFuture must
// never be treated as locally cancellable. The cancel borrow ends at return;
// implementations may not retain it in the future or a pending ticket.
class ScopedApprovalFuture : public InteractionFuture {
public:
    using InteractionFuture::WaitApproval;
    virtual std::optional<ApprovalResponse> WaitApproval(const std::atomic<bool>* cancel) = 0;
};

// One caller owns this handle; move/Retire/destruction are not concurrent handle
// operations. Its future may be shared by waiters. The host's retire callback
// must be idempotent, must not throw, and must retire only the matching ticket.
class ApprovalLease {
public:
    ApprovalLease() = default;
    ~ApprovalLease();
    ApprovalLease(const ApprovalLease&) = delete;
    ApprovalLease& operator=(const ApprovalLease&) = delete;
    ApprovalLease(ApprovalLease&& other) noexcept;
    ApprovalLease& operator=(ApprovalLease&& other) noexcept;

    static ApprovalLease Create(std::shared_ptr<ScopedApprovalFuture> future,
                                std::function<void()> retire);
    explicit operator bool() const noexcept;
    std::shared_ptr<ScopedApprovalFuture> Future() const noexcept;
    void Retire() noexcept;

private:
    std::shared_ptr<ScopedApprovalFuture> future_;
    // Moving/resetting a shared_ptr does not retire arbitrary small captures
    // under a mutex. This class invokes and releases the function lock-free.
    std::shared_ptr<std::function<void()>> retire_;
};

} // namespace lubancode::runtime
