#include "runtime/scoped_approval.hpp"

#include <utility>

namespace lubancode::runtime {

ApprovalLease::~ApprovalLease() { Retire(); }

ApprovalLease::ApprovalLease(ApprovalLease&& other) noexcept
    : future_(std::move(other.future_)), retire_(std::move(other.retire_)) {}

ApprovalLease& ApprovalLease::operator=(ApprovalLease&& other) noexcept {
    if (this != &other) {
        Retire();
        future_ = std::move(other.future_);
        retire_ = std::move(other.retire_);
    }
    return *this;
}

ApprovalLease ApprovalLease::Create(std::shared_ptr<ScopedApprovalFuture> future,
                                    std::function<void()> retire) {
    ApprovalLease lease;
    if (!future || !retire) return lease;
    auto cleanup = std::make_shared<std::function<void()>>();
    cleanup->swap(retire); // Any small capture moves/retirements happen lock-free.
    lease.future_ = std::move(future);
    lease.retire_ = std::move(cleanup);
    return lease;
}

ApprovalLease::operator bool() const noexcept { return future_ && retire_; }
std::shared_ptr<ScopedApprovalFuture> ApprovalLease::Future() const noexcept { return future_; }

void ApprovalLease::Retire() noexcept {
    // Disarm before invoking/releasing user code, including capture destructors
    // which may reenter this very lease. Retiring the host ticket precedes loss
    // of our future anchor; a separately held future remains safe to inspect.
    auto cleanup = std::move(retire_);
    auto future = std::move(future_);
    if (cleanup) (*cleanup)();
}

} // namespace lubancode::runtime
