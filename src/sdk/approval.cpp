#include "sdk/approval.hpp"

#include <algorithm>
#include <condition_variable>
#include <filesystem>
#include <map>
#include <mutex>
#include <optional>
#include <tuple>
#include <utility>

#include "platform/text_encoding.hpp"
#include "platform/sha256.hpp"
#include "trajectory/canonical_json.hpp"
#include "tools/path_utils.hpp"
#include "approval_mode.hpp"

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
using ChildKey = std::tuple<std::string, std::string, std::string, std::string, std::string,
                            std::string, std::string, std::string, std::string>;
ChildKey KeyOf(const rt::ChildApprovalScope& scope) {
    return {scope.host_session_id, scope.host_run_id, scope.host_operation_id,
            scope.parent_session_id, scope.parent_run_id, scope.child_session_id,
            scope.child_run_id, scope.effective_cwd, scope.permission_floor};
}
bool KeyMatches(const ChildKey& key, const rt::ChildApprovalScope& scope) noexcept {
    return std::get<0>(key) == scope.host_session_id && std::get<1>(key) == scope.host_run_id &&
        std::get<2>(key) == scope.host_operation_id && std::get<3>(key) == scope.parent_session_id &&
        std::get<4>(key) == scope.parent_run_id && std::get<5>(key) == scope.child_session_id &&
        std::get<6>(key) == scope.child_run_id && std::get<7>(key) == scope.effective_cwd &&
        std::get<8>(key) == scope.permission_floor;
}
bool ValidChildScope(const rt::ChildApprovalScope& scope) {
    const auto floor = lubancode::ParseApprovalMode(scope.permission_floor);
    return ValidFact(scope.host_session_id) && ValidFact(scope.host_run_id) &&
        ValidFact(scope.host_operation_id) && scope.parent_session_id == scope.host_session_id &&
        scope.parent_run_id == scope.host_run_id && ValidFact(scope.child_session_id) &&
        ValidFact(scope.child_run_id) && ValidFact(scope.effective_cwd) &&
        lubancode::tools::Utf8ToPath(scope.effective_cwd).is_absolute() &&
        (scope.permission_floor == "inherit" ||
         (floor && scope.permission_floor == lubancode::ApprovalModeMachineName(*floor)));
}
using JobKey = std::tuple<std::string, std::string, std::string, std::string, std::string, std::string, std::string>;
JobKey JobKeyOf(const jobs::v1::ApprovalScope& scope) {
    return {scope.session_id, scope.run_id, scope.parent_operation_id, scope.turn_id,
        scope.parent_action_id, scope.provider_call_id, scope.assistant_message_id};
}
bool ValidJobScope(const jobs::v1::ApprovalScope& scope) {
    for (const auto* value : {&scope.session_id, &scope.run_id, &scope.parent_operation_id,
        &scope.turn_id, &scope.parent_action_id, &scope.provider_call_id,
        &scope.assistant_message_id, &scope.cwd, &scope.effective_input_sha256})
        if (!ValidFact(*value)) return false;
    return lubancode::tools::Utf8ToPath(scope.cwd).is_absolute() && scope.effective_input_sha256.size() == 64 &&
        scope.effective_input_sha256.find_first_not_of("0123456789abcdef") == std::string::npos;
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
        std::optional<rt::ChildApprovalScope> child;
        std::optional<jobs::v1::ApprovalScope> job;
    };
    mutable std::mutex mutex;
    std::map<std::string, Entry> pending;
    std::set<std::string> allowed;
    std::set<std::string> scoped_ids;
    std::map<ChildKey, std::set<std::string>> child_grants;
    std::map<ChildKey, bool> child_scopes;
    struct JobScope { jobs::v1::ApprovalScope scope; bool open = true; std::optional<jobs::v1::Identity> bound; };
    std::map<JobKey, JobScope> job_scopes;
    bool job_admission_stopped = false;
    bool child_admission_stopped = false;
    std::string session_id, run_id;
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
            child_grants.clear();
            child_scopes.clear();
            child_admission_stopped = false;
            for (auto& [scope, job] : job_scopes) { (void)scope; if (close || !job.bound) job.open = false; }
            session_id.clear();
            run_id.clear();
            operation_id.clear();
        }
    }
};

SessionApprovals::SessionApprovals() : state_(std::make_shared<State>()) {}
SessionApprovals::~SessionApprovals() { Close(); }

void SessionApprovals::SetOperationOwner(std::string session_id, std::string operation_id, std::string run_id) {
    std::lock_guard lock(state_->mutex);
    if (state_->closed) return;
    if (state_->session_id != session_id || state_->operation_id != operation_id || state_->run_id != run_id)
        state_->child_admission_stopped = false;
    state_->session_id.swap(session_id);
    state_->operation_id.swap(operation_id);
    state_->run_id.swap(run_id);
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
            state_->pending.emplace(request_id, State::Entry{std::move(approval), future, std::nullopt, std::nullopt}).second;
        if (registered) *registered = inserted;
        if (!inserted) future->Resolve(std::nullopt);
    }
    return wrapper;
}

Result<rt::ApprovalLease> SessionApprovals::RegisterScoped(
    ScopedApprovalOwner owner, Approval approval, std::chrono::milliseconds timeout, Publisher publish) {
    return RegisterScopedImpl(std::move(owner), std::move(approval), timeout, std::move(publish), std::nullopt);
}

Result<rt::ApprovalLease> SessionApprovals::RegisterChildScoped(
    rt::ChildApprovalRequest request, Approval approval, std::chrono::milliseconds timeout, ChildPublisher publish) {
    if (!ValidChildScope(request.scope) || !ValidFact(request.child_turn_id) ||
        !ValidFact(request.child_declared_action_id) || !ValidFact(request.child_declared_message_id) ||
        request.owner_task_id <= 0 || !publish || request.request.tool_use_id != approval.tool_call_id ||
        request.request.tool_name != approval.tool_name || request.request.input.dump() != approval.input_json)
        return std::unexpected(Error{"sdk.approval.invalid_scope", "child ticket requires actual copied declaration facts"});
    const auto child = request.scope;
    ScopedApprovalOwner owner{child.host_session_id, child.host_operation_id,
        child.child_session_id, child.child_run_id, request.child_turn_id,
        request.child_declared_action_id, child.effective_cwd};
    return RegisterScopedImpl(std::move(owner), std::move(approval), timeout,
        [publish = std::move(publish), request = std::move(request)](const Approval& ticket, const ScopedApprovalOwner&) {
            publish(ticket, request);
        }, child);
}

Result<rt::ApprovalLease> SessionApprovals::RegisterScopedImpl(
    ScopedApprovalOwner owner, Approval approval, std::chrono::milliseconds timeout,
    Publisher publish, std::optional<rt::ChildApprovalScope> child,
    std::optional<jobs::v1::ApprovalScope> job) {
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
        if (child) {
            const auto key = KeyOf(*child);
            const auto known = state_->child_scopes.find(key);
            if (state_->child_admission_stopped || state_->run_id != child->host_run_id ||
                (known != state_->child_scopes.end() && !known->second))
                return std::unexpected(Error{"sdk.approval.owner_mismatch", "child scope is closed or belongs to another host run"});
            state_->child_scopes.try_emplace(key, true);
        }
        if (job) {
            if (state_->run_id != job->run_id) return std::unexpected(Error{"sdk.approval.owner_mismatch", "Job run differs"});
            if (state_->job_admission_stopped)
                return std::unexpected(Error{"sdk.approval.owner_mismatch", "Job admission is retired"});
            const auto [entry, inserted] = state_->job_scopes.try_emplace(JobKeyOf(*job), State::JobScope{*job});
            (void)inserted;
            if (entry->second.scope != *job || !entry->second.open || entry->second.bound)
                return std::unexpected(Error{"sdk.approval.owner_mismatch", "Job candidate is retired or already bound"});
        }
        if (state_->pending.contains(request_id) || state_->scoped_ids.contains(request_id))
            return std::unexpected(Error{"sdk.approval.duplicate_request_id", "request ID is already pending"});
        state_->scoped_ids.insert(request_id);
        state_->pending.emplace(request_id, State::Entry{approval, future, owner, std::move(child), std::move(job)});
    }
    try {
        publish(approval, owner); // Never hold either pending/future lock in user code.
    } catch (...) {
        return std::unexpected(Error{"sdk.approval.publish_failed", "approval publisher failed; ticket retired"});
    }
    return std::move(lease);
}

Result<rt::ApprovalLease> SessionApprovals::RegisterJobScoped(
    jobs::v1::ApprovalScope scope, Approval approval, std::chrono::milliseconds timeout,
    std::function<void(const Approval&)> publish) {
    const auto arguments = nlohmann::json::parse(approval.input_json, nullptr, false);
    const auto canonical = arguments.is_object() ? lubancode::trajectory::CanonicalJsonDump(arguments)
        : std::expected<std::string, std::string>(std::unexpected("invalid Job arguments"));
    if (!canonical || lubancode::platform::Sha256Hex(*canonical) != scope.effective_input_sha256 ||
        !ValidJobScope(scope) || !publish || approval.tool_call_id != scope.provider_call_id ||
        approval.tool_name != "run_command" || !approval.job || *approval.job != scope)
        return std::unexpected(Error{"sdk.approval.invalid_scope", "Job ticket requires its original main declaration"});
    ScopedApprovalOwner owner{scope.session_id, scope.parent_operation_id, scope.session_id,
        scope.run_id, scope.turn_id, scope.parent_action_id, scope.cwd};
    return RegisterScopedImpl(std::move(owner), std::move(approval), timeout,
        [publish = std::move(publish)](const Approval& ticket, const ScopedApprovalOwner&) { publish(ticket); },
        std::nullopt, std::move(scope));
}

bool SessionApprovals::BindJobScope(const jobs::v1::ApprovalScope& scope, const jobs::v1::Identity& identity) {
    if (!ValidJobScope(scope) || identity.session_id != scope.session_id || identity.run_id != scope.run_id ||
        identity.parent_operation_id != scope.parent_operation_id || identity.turn_id != scope.turn_id ||
        identity.job_id.empty() || identity.operation_id.empty() || identity.action_id.empty() || identity.attempt != 1) return false;
    std::lock_guard lock(state_->mutex);
    if (state_->closed || state_->job_admission_stopped || state_->session_id != scope.session_id || state_->run_id != scope.run_id ||
        state_->operation_id != scope.parent_operation_id) return false;
    const auto key = JobKeyOf(scope);
    for (const auto& [other_key, entry] : state_->job_scopes) {
        if (other_key == key || !entry.bound) continue;
        const auto& bound = *entry.bound;
        if (bound.session_id == identity.session_id && bound.run_id == identity.run_id &&
            (bound.job_id == identity.job_id || bound.operation_id == identity.operation_id || bound.action_id == identity.action_id))
            return false;
    }
    auto [found, inserted] = state_->job_scopes.try_emplace(key, State::JobScope{scope});
    (void)inserted;
    if (!found->second.open || found->second.scope != scope) return false;
    if (found->second.bound) return *found->second.bound == identity;
    found->second.bound = identity;
    return true;
}

bool SessionApprovals::JobAllowed(const jobs::v1::ApprovalScope& scope, const jobs::v1::Identity& identity) const {
    std::lock_guard lock(state_->mutex);
    const auto found = state_->job_scopes.find(JobKeyOf(scope));
    return !state_->closed && !state_->job_admission_stopped && found != state_->job_scopes.end() &&
        found->second.scope == scope && found->second.open &&
        found->second.bound && *found->second.bound == identity;
}

void SessionApprovals::CloseJobScope(const jobs::v1::ApprovalScope& scope) noexcept {
    decltype(state_->pending) retired;
    {
        std::lock_guard lock(state_->mutex);
        if (state_->closed) return;
        try {
            if (!ValidJobScope(scope)) return;
            auto [found, inserted] = state_->job_scopes.try_emplace(JobKeyOf(scope), State::JobScope{scope, false, {}});
            (void)inserted;
            if (found->second.scope != scope) return;
            found->second.open = false;
        } catch (...) {
            // A failed tombstone allocation must never reopen a retired Job.
            // Ordinary parent and child grants keep their existing lifetime.
            state_->job_admission_stopped = true;
        }
        for (auto it = state_->pending.begin(); it != state_->pending.end();) {
            auto current = it++;
            if (current->second.job && (*current->second.job == scope || state_->job_admission_stopped)) {
                current->second.future->Resolve(std::nullopt);
                retired.insert(state_->pending.extract(current));
            }
        }
    }
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
        } else if (it->second.scope && !it->second.child && !it->second.job && response.decision == rt::InteractionDecision::AcceptForSession) {
            return false; // Explicitly unsupported; the live ticket remains answerable.
        } else {
            resolved = it->second.future->Resolve(response);
            if (resolved && response.decision == rt::InteractionDecision::AcceptForSession) {
                if (it->second.child)
                    state_->child_grants[KeyOf(*it->second.child)].insert(it->second.approval.tool_name);
                else if (!it->second.job) state_->allowed.insert(it->second.approval.tool_name);
                // Job acceptance is consumed once by synchronous Prepare. Bind
                // retains only that Job's fixed effective input, never a tool-name grant.
            }
        }
        retired = state_->pending.extract(it);
    }
    return resolved;
}

bool SessionApprovals::ChildAllowed(const rt::ChildApprovalScope& scope, const std::string& tool) const {
    if (!ValidChildScope(scope) || !ValidFact(tool)) return false;
    const auto key = KeyOf(scope);
    std::lock_guard lock(state_->mutex);
    if (state_->closed || state_->session_id != scope.host_session_id ||
        state_->operation_id != scope.host_operation_id || state_->run_id != scope.host_run_id ||
        !state_->child_scopes.contains(key) || !state_->child_scopes.at(key)) return false;
    const auto it = state_->child_grants.find(key);
    return it != state_->child_grants.end() && it->second.contains(tool);
}

void SessionApprovals::CloseChildScope(const rt::ChildApprovalScope& scope) noexcept {
    // Only owned strings/future completions live in entries. Their final anchors
    // still retire after the host lock, following ordinary ticket cleanup.
    decltype(state_->pending) retired;
    {
        std::lock_guard lock(state_->mutex);
        if (state_->closed || state_->session_id != scope.host_session_id ||
            state_->operation_id != scope.host_operation_id || state_->run_id != scope.host_run_id ||
            scope.parent_session_id != scope.host_session_id || scope.parent_run_id != scope.host_run_id) return;
        try {
            if (!ValidChildScope(scope)) return;
        } catch (...) {
            // Only this host's child admission stops. Ordinary SDK approvals
            // remain intact; wrong-owner Close never reaches validation here.
            state_->child_admission_stopped = true;
        }
        const auto known = std::find_if(state_->child_scopes.begin(), state_->child_scopes.end(),
            [&](const auto& entry) { return KeyMatches(entry.first, scope); });
        if (known != state_->child_scopes.end()) {
            known->second = false;
            state_->child_grants.erase(known->first);
        } else if (!state_->child_admission_stopped) {
            try {
                state_->child_scopes.try_emplace(KeyOf(scope), false);
            } catch (...) {
                // Preserve noexcept retirement if a new tombstone cannot be
                // allocated. Reopening a retired child must fail closed.
                state_->child_admission_stopped = true;
            }
        }
        for (auto it = state_->pending.begin(); it != state_->pending.end();) {
            auto current = it++;
            if (current->second.child && *current->second.child == scope) {
                current->second.future->Resolve(std::nullopt);
                retired.insert(state_->pending.extract(current)); // Node transfer does not allocate.
            }
        }
    }
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
