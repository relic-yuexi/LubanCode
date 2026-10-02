#include "sdk/approval.hpp"

#include <algorithm>
#include <condition_variable>
#include <filesystem>
#include <map>
#include <mutex>
#include <optional>
#include <utility>

#include "platform/text_encoding.hpp"
#include "tools/path_utils.hpp"

namespace lubancore::detail {
namespace {
namespace rt = lubancode::runtime;
using Clock = std::chrono::steady_clock;

Clock::time_point Deadline(std::chrono::milliseconds timeout) {
    const auto now = Clock::now();
    const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::time_point::max() - now);
    return timeout > remaining ? Clock::time_point::max() : now + timeout;
}

bool ValidFact(const std::string& value) {
    return !value.empty() && value.find('\0') == std::string::npos &&
           lubancode::platform::IsValidUtf8(value);
}
bool ValidScope(const ScopedApprovalOwner& owner, const Approval& approval) {
    if (!ValidFact(owner.parent_session_id) || !ValidFact(owner.parent_operation_id) ||
        !ValidFact(owner.child_session_id) || !ValidFact(owner.child_run_id) ||
        !ValidFact(owner.child_turn_id) || !ValidFact(owner.child_declared_action_id) ||
        !ValidFact(owner.effective_cwd) || !ValidFact(approval.request_id) ||
        !ValidFact(approval.tool_name) || approval.operation_id != owner.parent_operation_id ||
        approval.cwd != owner.effective_cwd) return false;
    return lubancode::tools::Utf8ToPath(owner.effective_cwd).is_absolute();
}
} // namespace

struct SessionApprovals::State {
    class Future {
    public:
        Future(std::chrono::milliseconds timeout, bool scoped)
            : timeout_(timeout), scoped_(scoped),
              deadline_(scoped ? Deadline(timeout) : Clock::time_point{}) {}

        std::optional<rt::ApprovalResponse> WaitOrdinary() {
            // Preserve the established ordinary SDK timeout: it starts when
            // the ordinary caller waits, rather than when the ticket registers.
            std::unique_lock lock(mutex_);
            if (!cv_.wait_for(lock, timeout_, [&] { return done_; })) done_ = true;
            return response_;
        }
        std::optional<rt::ApprovalResponse> WaitScoped(const std::weak_ptr<State>& owner,
            const std::string& id, const std::atomic<bool>* cancel) {
            std::unique_lock lock(mutex_);
            while (!done_) {
                const auto now = Clock::now();
                if ((cancel && cancel->load()) || now >= deadline_) {
                    lock.unlock();
                    if (auto state = owner.lock()) state->Retire(id, this);
                    else Resolve(std::nullopt);
                    lock.lock();
                    continue;
                }
                cv_.wait_until(lock, std::min(deadline_, now + std::chrono::milliseconds(20)),
                               [&] { return done_; });
            }
            return response_;
        }
        bool Resolve(std::optional<rt::ApprovalResponse> response) {
            std::lock_guard lock(mutex_);
            if (done_) return false;
            response_ = std::move(response);
            done_ = true;
            cv_.notify_all();
            return true;
        }
        bool pending() const { std::lock_guard lock(mutex_); return !done_; }
        bool expired() const noexcept { return scoped_ && Clock::now() >= deadline_; }
        bool scoped() const noexcept { return scoped_; }

    private:
        mutable std::mutex mutex_;
        std::condition_variable cv_;
        std::chrono::milliseconds timeout_;
        bool scoped_;
        Clock::time_point deadline_;
        bool done_ = false;
        std::optional<rt::ApprovalResponse> response_;
    };
    class OrdinaryFuture final : public rt::InteractionFuture {
    public:
        explicit OrdinaryFuture(std::shared_ptr<Future> future) : future_(std::move(future)) {}
        std::optional<rt::ApprovalResponse> WaitApproval() override { return future_->WaitOrdinary(); }
        std::optional<rt::QuestionResponse> WaitQuestion() override { return std::nullopt; }
    private:
        std::shared_ptr<Future> future_;
    };
    class ScopedFuture final : public rt::ScopedApprovalFuture {
    public:
        ScopedFuture(std::shared_ptr<Future> future, std::weak_ptr<State> owner, std::string id)
            : future_(std::move(future)), owner_(std::move(owner)), id_(std::move(id)) {}
        std::optional<rt::ApprovalResponse> WaitApproval() override { return WaitApproval(nullptr); }
        std::optional<rt::ApprovalResponse> WaitApproval(const std::atomic<bool>* cancel) override {
            return future_->WaitScoped(owner_, id_, cancel);
        }
        std::optional<rt::QuestionResponse> WaitQuestion() override { return std::nullopt; }
    private:
        std::shared_ptr<Future> future_;
        std::weak_ptr<State> owner_;
        std::string id_;
    };

    struct Entry {
        Approval approval;
        std::shared_ptr<Future> future;
        std::optional<ScopedApprovalOwner> scope;
    };
    mutable std::mutex mutex;
    std::map<std::string, Entry> pending;
    std::set<std::string> allowed;
    std::set<std::string> scoped_ids;
    std::string session_id;
    std::string operation_id;
    bool closed = false;

    void Retire(const std::string& id, const Future* expected = nullptr) noexcept {
        decltype(pending)::node_type retired;
        {
            std::lock_guard lock(mutex);
            auto it = pending.find(id);
            if (it == pending.end()) return;
            if (expected && it->second.future.get() != expected) return;
            it->second.future->Resolve(std::nullopt);
            retired = pending.extract(it);
        }
        // Destroy the future/value anchors after releasing the host lock.
    }
    void CancelAll(bool close = false) noexcept {
        decltype(pending) retired;
        {
            std::lock_guard lock(mutex);
            if (close) closed = true;
            for (auto& [id, entry] : pending) { (void)id; entry.future->Resolve(std::nullopt); }
            pending.swap(retired);
            session_id.clear();
            operation_id.clear();
        }
    }
};

SessionApprovals::SessionApprovals() : state_(std::make_shared<State>()) {}
SessionApprovals::~SessionApprovals() { Close(); }

void SessionApprovals::SetOperationOwner(std::string session_id, std::string operation_id) {
    std::lock_guard lock(state_->mutex);
    if (state_->closed) return;
    state_->session_id.swap(session_id);
    state_->operation_id.swap(operation_id);
}

std::shared_ptr<rt::InteractionFuture> SessionApprovals::CancelledFuture() {
    auto future = std::make_shared<State::Future>(std::chrono::milliseconds(0), false);
    future->Resolve(std::nullopt);
    return std::make_shared<State::OrdinaryFuture>(std::move(future));
}

std::shared_ptr<rt::InteractionFuture> SessionApprovals::Register(
    Approval approval, std::chrono::milliseconds timeout, const std::atomic<bool>* interrupt, bool* registered) {
    auto future = std::make_shared<State::Future>(timeout, false);
    auto wrapper = std::make_shared<State::OrdinaryFuture>(future);
    const auto request_id = approval.request_id;
    {
        std::lock_guard lock(state_->mutex);
        const bool admit = !state_->closed && !(interrupt && interrupt->load());
        const bool inserted = admit && !state_->scoped_ids.contains(request_id) &&
            state_->pending.emplace(request_id, State::Entry{std::move(approval), future, std::nullopt}).second;
        if (registered) *registered = inserted;
        if (!inserted) future->Resolve(std::nullopt);
    }
    return wrapper;
}

Result<rt::ApprovalLease> SessionApprovals::RegisterScoped(
    ScopedApprovalOwner owner, Approval approval, std::chrono::milliseconds timeout, Publisher publish) {
    if (!ValidScope(owner, approval) || timeout.count() <= 0 || !publish)
        return std::unexpected(Error{"sdk.approval.invalid_scope", "scoped approval requires an owned scope and publisher"});
    const auto request_id = approval.request_id;
    auto future = std::make_shared<State::Future>(timeout, true);
    auto wrapper = std::make_shared<State::ScopedFuture>(future, state_, request_id);
    auto lease = rt::ApprovalLease::Create(wrapper, [weak = std::weak_ptr<State>(state_),
        expected = std::weak_ptr<State::Future>(future), request_id] {
        if (auto state = weak.lock()) if (auto actual = expected.lock()) state->Retire(request_id, actual.get());
    });
    {
        std::lock_guard lock(state_->mutex);
        if (state_->closed || state_->session_id != owner.parent_session_id || state_->operation_id != owner.parent_operation_id)
            return std::unexpected(Error{"sdk.approval.owner_mismatch", "scope does not belong to the active host operation"});
        if (state_->pending.contains(request_id) || state_->scoped_ids.contains(request_id))
            return std::unexpected(Error{"sdk.approval.duplicate_request_id", "request ID is already pending"});
        state_->scoped_ids.insert(request_id);
        state_->pending.emplace(request_id, State::Entry{approval, future, owner});
    }
    try {
        publish(approval, owner); // Never hold either pending/future lock in user code.
    } catch (...) {
        return std::unexpected(Error{"sdk.approval.publish_failed", "approval publisher failed; ticket retired"});
    }
    return std::move(lease);
}

bool SessionApprovals::Resolve(const std::string& request_id, const rt::ApprovalResponse& response) {
    decltype(state_->pending)::node_type retired;
    bool resolved = false;
    {
        std::lock_guard lock(state_->mutex);
        auto it = state_->pending.find(request_id);
        if (it == state_->pending.end()) return false;
        if (it->second.future->expired()) {
            it->second.future->Resolve(std::nullopt);
        } else if (it->second.scope && response.decision == rt::InteractionDecision::AcceptForSession) {
            return false; // Explicitly unsupported; the live ticket remains answerable.
        } else {
            resolved = it->second.future->Resolve(response);
            if (resolved && response.decision == rt::InteractionDecision::AcceptForSession)
                state_->allowed.insert(it->second.approval.tool_name);
        }
        retired = state_->pending.extract(it);
    }
    return resolved;
}

void SessionApprovals::Retire(const std::string& request_id) noexcept { state_->Retire(request_id); }
void SessionApprovals::CancelAll() noexcept { state_->CancelAll(); }
void SessionApprovals::Close() noexcept { state_->CancelAll(true); }

std::vector<Approval> SessionApprovals::Pending() const {
    std::vector<Approval> result;
    std::lock_guard lock(state_->mutex);
    for (const auto& [id, entry] : state_->pending) {
        (void)id;
        if (entry.future->pending() && !entry.future->expired()) result.push_back(entry.approval);
    }
    return result;
}
std::set<std::string> SessionApprovals::AllowedTools() const {
    std::lock_guard lock(state_->mutex);
    return state_->allowed;
}

} // namespace lubancore::detail
