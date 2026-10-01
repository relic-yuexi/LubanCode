#include "lubancore/authorization.hpp"

#include <algorithm>
#include <condition_variable>
#include <limits>
#include <map>
#include <mutex>
#include <set>
#include <thread>
#include <tuple>
#include <utility>

namespace lubancore::authorization::v1 {
namespace {
Error Failure(const char* code) { return {code, {}}; }
thread_local unsigned notification_depth = 0;
struct NotificationScope {
    NotificationScope() { ++notification_depth; }
    ~NotificationScope() { --notification_depth; }
};
bool ValidId(const std::string& value) {
    return !value.empty() && value.size() <= 512 &&
        std::none_of(value.begin(), value.end(), [](unsigned char c) { return c < 32 || c == 127; });
}
bool ValidKind(ActorKind kind) {
    return kind == ActorKind::User || kind == ActorKind::Agent || kind == ActorKind::Service;
}
bool ValidSubject(const AuthenticatedSubject& subject) {
    return ValidId(subject.tenant_id) && ValidId(subject.user_id) &&
        ValidKind(subject.actor_kind) && ValidId(subject.credential_id);
}
bool ValidScope(const ResourceScope& resource, bool project_scope = false) {
    return ValidId(resource.tenant_id) && ValidId(resource.project_id) && ValidId(resource.workspace_key) &&
        (ValidId(resource.session_id) || (project_scope && resource.session_id.empty()));
}
bool ValidAction(Action action) {
    switch (action) {
    case Action::OpenSession: case Action::ResumeSession: case Action::AcquireView:
    case Action::ReadSession: case Action::Submit:
    case Action::ReadOperation: case Action::ReadHistory: case Action::ReadApprovals:
    case Action::ResolveApproval: case Action::CancelOperation: case Action::CloseSession:
    case Action::ReadExtensions: case Action::ListToolResults: case Action::ReadToolResult:
    case Action::SubscribeEvents: case Action::ReadEvents: case Action::WaitResult:
    case Action::RequestModel: case Action::DispatchTool: return true;
    }
    return false;
}
bool ProjectAction(Action action) { return action == Action::OpenSession; }
bool ExecutionAction(Action action) { return action == Action::RequestModel || action == Action::DispatchTool; }
Result<void> Validate(const ExecutionContext& context, Action action) {
    if (!ValidSubject(context.request_actor)) return std::unexpected(Failure("sdk.authorization.subject_invalid"));
    if (!ValidAction(action)) return std::unexpected(Failure("sdk.authorization.action_invalid"));
    if (!ValidScope(context.resource, ProjectAction(action))) return std::unexpected(Failure("sdk.authorization.scope_invalid"));
    if (context.request_actor.tenant_id != context.resource.tenant_id)
        return std::unexpected(Failure("sdk.authorization.cross_tenant"));
    if (!context.project_binding_version) return std::unexpected(Failure("sdk.authorization.binding_invalid"));
    if (context.execution) {
        const auto& execution = *context.execution;
        if (!ValidSubject(execution.initiating_subject) || !ValidScope(execution.resource) ||
            execution.resource != context.resource || !execution.policy_revision || !ValidId(execution.execution_id) ||
            std::any_of(execution.allowed_capabilities.begin(), execution.allowed_capabilities.end(),
                [](Action capability) { return !ValidAction(capability); }))
            return std::unexpected(Failure("sdk.authorization.execution_invalid"));
        if (execution.initiating_subject.tenant_id != context.resource.tenant_id)
            return std::unexpected(Failure("sdk.authorization.cross_tenant"));
    }
    if (ExecutionAction(action)) {
        if (!context.execution || context.request_actor != context.execution->initiating_subject)
            return std::unexpected(Failure("sdk.authorization.execution_actor_mismatch"));
        const auto& capabilities = context.execution->allowed_capabilities;
        if (std::find(capabilities.begin(), capabilities.end(), action) == capabilities.end())
            return std::unexpected(Failure("sdk.authorization.capability_denied"));
    }
    return {};
}
bool ValidReason(const std::string& code) {
    return !code.empty() && code.size() <= 128 && std::all_of(code.begin(), code.end(), [](unsigned char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
            c == '.' || c == '_' || c == '-';
    });
}
using SubjectKey = std::tuple<std::string, std::string, ActorKind, std::string>;
using BindingKey = std::tuple<std::string, std::string, std::string>;
using ScopeKey = std::tuple<std::string, std::string, std::string, std::string>;
using GrantKey = std::pair<SubjectKey, ScopeKey>;
SubjectKey Key(const AuthenticatedSubject& s) { return {s.tenant_id, s.user_id, s.actor_kind, s.credential_id}; }
BindingKey Binding(const ResourceScope& r) { return {r.tenant_id, r.project_id, r.workspace_key}; }
ScopeKey Key(const ResourceScope& r) { return {r.tenant_id, r.project_id, r.workspace_key, r.session_id}; }
struct GrantRecord { std::uint64_t binding_version; std::set<Action> actions; };
struct Rules {
    std::uint64_t revision = 1;
    std::map<BindingKey, std::uint64_t> bindings;
    std::map<BindingKey, std::uint64_t> binding_versions;
    std::set<SubjectKey> administrators;
    std::map<GrantKey, GrantRecord> grants;
};
struct ChangedScope {
    std::string tenant_id, project_id, workspace_key, session_id;
    bool Matches(const ResourceScope& scope) const {
        return tenant_id == scope.tenant_id && (project_id.empty() || project_id == scope.project_id) &&
            (workspace_key.empty() || workspace_key == scope.workspace_key) &&
            (session_id.empty() || session_id == scope.session_id);
    }
};
struct Observer {
    explicit Observer(ResourceScope resource, std::shared_ptr<PolicyChangeCallback> function)
        : scope(std::move(resource)), callback(std::move(function)) {}
    const ResourceScope scope;
    std::mutex mutex;
    std::condition_variable cv;
    std::shared_ptr<PolicyChangeCallback> callback;
    bool enabled = true, dispatching = false;
    std::uint64_t pending = 0, delivered = 0;

    void Stop() noexcept {
        std::shared_ptr<PolicyChangeCallback> retired;
        {
            std::unique_lock lock(mutex);
            enabled = false;
            pending = 0;
            retired.swap(callback);
            // All notification-side waits are suppressed, not only self-waits:
            // two different callbacks can unsubscribe one another concurrently.
            if (!notification_depth) cv.wait(lock, [&] { return !dispatching; });
        }
        retired.reset(); // User-owned captures must be destroyed outside locks.
    }
    void Notify(std::uint64_t revision) noexcept {
        {
            std::lock_guard lock(mutex);
            if (!enabled || revision <= delivered) return;
            pending = std::max(pending, revision);
            if (dispatching) return;
            dispatching = true;
        }
        for (;;) {
            std::shared_ptr<PolicyChangeCallback> invocation;
            std::uint64_t current = 0;
            {
                std::lock_guard lock(mutex);
                if (!enabled || !pending) {
                    dispatching = false;
                    cv.notify_all();
                    return;
                }
                invocation = callback;
                current = pending;
                pending = 0;
                delivered = current;
            }
            {
                NotificationScope notification;
                try { (*invocation)(PolicyChange{scope, current}); } catch (...) {}
                invocation.reset(); // Includes reentrant capture destructors.
            }
        }
    }
};
Result<void> ValidateGrant(const AuthenticatedSubject& subject, const ResourceScope& resource,
    const std::vector<Action>& actions) {
    if (!ValidSubject(subject)) return std::unexpected(Failure("sdk.authorization.subject_invalid"));
    if (!ValidScope(resource, true)) return std::unexpected(Failure("sdk.authorization.scope_invalid"));
    if (subject.tenant_id != resource.tenant_id) return std::unexpected(Failure("sdk.authorization.cross_tenant"));
    if (actions.empty() || std::any_of(actions.begin(), actions.end(), [&](Action action) {
        return !ValidAction(action) || (resource.session_id.empty() && !ProjectAction(action));
    })) return std::unexpected(Failure("sdk.authorization.action_invalid"));
    return {};
}
} // namespace

struct PolicySubscription::Impl {
    std::mutex mutex;
    std::condition_variable cv;
    std::function<void()> close;
    bool closing = false, closed = false;
    std::thread::id closing_thread;
};
PolicySubscription::PolicySubscription(std::shared_ptr<Impl> impl) : impl_(std::move(impl)) {}
Result<std::unique_ptr<PolicySubscription>> PolicySubscription::Create(std::function<void()> unsubscribe) {
    if (!unsubscribe) return std::unexpected(Failure("sdk.authorization.subscription_invalid"));
    auto impl = std::make_shared<Impl>();
    impl->close.swap(unsubscribe);
    return std::unique_ptr<PolicySubscription>(new PolicySubscription(std::move(impl)));
}
PolicySubscription::~PolicySubscription() { Unsubscribe(); }
void PolicySubscription::Unsubscribe() noexcept {
    const auto state = impl_;
    std::function<void()> close;
    {
        std::unique_lock lock(state->mutex);
        while (state->closing) {
            if (notification_depth || state->closing_thread == std::this_thread::get_id()) return;
            state->cv.wait(lock, [&] { return !state->closing; });
        }
        if (state->closed) return;
        state->closing = true;
        state->closing_thread = std::this_thread::get_id();
        close.swap(state->close);
    }
    try { close(); } catch (...) {} // A trusted provider's cleanup cannot throw from RAII.
    if (notification_depth) {
        // Observer::Stop only disarmed this invocation. Preserve the idempotent
        // closer so a later external call can still wait for its real exit.
        std::lock_guard lock(state->mutex);
        state->close.swap(close);
        state->closing = false;
        state->cv.notify_all();
    } else {
        close = nullptr;
        std::lock_guard lock(state->mutex);
        state->closed = true;
        state->closing = false;
        state->cv.notify_all();
    }
}
PolicyProvider::~PolicyProvider() = default;

Result<Decision> Authorize(std::shared_ptr<PolicyProvider> provider, const ExecutionContext& context, Action action) {
    auto valid = Validate(context, action);
    if (!valid) return std::unexpected(valid.error());
    if (!provider) return std::unexpected(Failure("sdk.authorization.policy_missing"));
    try {
        auto result = provider->Authorize(context, action);
        if (!result) return std::unexpected(Failure("sdk.authorization.provider_failure"));
        if (!result->revision || !ValidReason(result->reason_code))
            return std::unexpected(Failure("sdk.authorization.decision_invalid"));
        return result;
    } catch (...) { return std::unexpected(Failure("sdk.authorization.provider_exception")); }
}
Result<std::unique_ptr<PolicySubscription>> SubscribeChanges(std::shared_ptr<PolicyProvider> provider,
    const ResourceScope& scope, PolicyChangeCallback callback) {
    if (!ValidScope(scope)) return std::unexpected(Failure("sdk.authorization.scope_invalid"));
    if (!callback) return std::unexpected(Failure("sdk.authorization.subscription_invalid"));
    if (!provider) return std::unexpected(Failure("sdk.authorization.policy_missing"));
    try {
        auto owned = std::make_shared<PolicyChangeCallback>();
        owned->swap(callback);
        PolicyChangeCallback guarded = [owned, subscribed = scope](const PolicyChange& change) {
            NotificationScope notification;
            (*owned)(PolicyChange{subscribed, change.scope == subscribed ? change.revision : 0});
        };
        auto result = provider->SubscribeChanges(scope, std::move(guarded));
        guarded = nullptr; // Explicitly clear std::function sources, including SBO.
        if (!result || !*result) return std::unexpected(Failure("sdk.authorization.subscription_invalid"));
        return result;
    } catch (...) { return std::unexpected(Failure("sdk.authorization.provider_exception")); }
}

struct RevocablePolicy::Impl {
    std::mutex mutex;
    std::shared_ptr<const Rules> rules = std::make_shared<Rules>();
    std::map<Observer*, std::shared_ptr<Observer>> observers;
    bool closed = false;

    Result<std::uint64_t> Update(const ChangedScope& changed, const std::function<Result<bool>(Rules&)>& mutate) {
        std::vector<std::shared_ptr<Observer>> recipients;
        std::uint64_t revision = 0;
        try {
            {
                std::lock_guard lock(mutex);
                if (closed) return std::unexpected(Failure("sdk.authorization.policy_closed"));
                Rules candidate = *rules;
                auto updated = mutate(candidate);
                if (!updated) return std::unexpected(updated.error());
                if (!*updated) return rules->revision;
                if (rules->revision == std::numeric_limits<std::uint64_t>::max())
                    return std::unexpected(Failure("sdk.authorization.revision_exhausted"));
                revision = candidate.revision = rules->revision + 1;
                auto publication = std::make_shared<Rules>(std::move(candidate));
                for (const auto& [key, observer] : observers) {
                    (void)key;
                    if (changed.Matches(observer->scope)) recipients.push_back(observer);
                }
                rules = std::move(publication);
            }
            for (const auto& observer : recipients) observer->Notify(revision);
            return revision;
        } catch (...) { return std::unexpected(Failure("sdk.authorization.update_failed")); }
    }
    void Remove(Observer* observer) {
        std::lock_guard lock(mutex);
        observers.erase(observer); // The subscription's close lease still owns it.
    }
    void Close() noexcept {
        std::map<Observer*, std::shared_ptr<Observer>> retired;
        {
            std::lock_guard lock(mutex);
            closed = true;
            retired.swap(observers);
        }
        for (const auto& [key, observer] : retired) { (void)key; observer->Stop(); }
        retired.clear();
    }
};
RevocablePolicy::RevocablePolicy(std::shared_ptr<Impl> impl) : impl_(std::move(impl)) {}
std::shared_ptr<RevocablePolicy> RevocablePolicy::Create() {
    return std::shared_ptr<RevocablePolicy>(new RevocablePolicy(std::make_shared<Impl>()));
}
RevocablePolicy::~RevocablePolicy() { impl_->Close(); }
std::uint64_t RevocablePolicy::CurrentRevision() const {
    const auto state = impl_;
    std::lock_guard lock(state->mutex);
    return state->rules->revision;
}
Result<Decision> RevocablePolicy::Authorize(const ExecutionContext& context, Action action) const {
    auto valid = Validate(context, action);
    if (!valid) return std::unexpected(valid.error());
    const auto state = impl_;
    std::shared_ptr<const Rules> snapshot;
    {
        std::lock_guard lock(state->mutex);
        if (state->closed) return std::unexpected(Failure("sdk.authorization.policy_closed"));
        snapshot = state->rules;
    }
    const auto binding = snapshot->bindings.find(Binding(context.resource));
    const auto deny = [&](const char* reason) -> Result<Decision> { return Decision{false, snapshot->revision, reason}; };
    if (binding == snapshot->bindings.end()) return deny("sdk.authorization.project_unbound");
    if (binding->second != context.project_binding_version) return deny("sdk.authorization.binding_mismatch");
    if (snapshot->administrators.contains(Key(context.request_actor)))
        return Decision{true, snapshot->revision, "sdk.authorization.allowed"};
    const auto grant = snapshot->grants.find({Key(context.request_actor), Key(context.resource)});
    if (grant == snapshot->grants.end() || grant->second.binding_version != binding->second ||
        !grant->second.actions.contains(action)) return deny("sdk.authorization.not_granted");
    return Decision{true, snapshot->revision, "sdk.authorization.allowed"};
}
Result<std::unique_ptr<PolicySubscription>> RevocablePolicy::SubscribeChanges(
    const ResourceScope& scope, PolicyChangeCallback callback) {
    if (!ValidScope(scope)) return std::unexpected(Failure("sdk.authorization.scope_invalid"));
    if (!callback) return std::unexpected(Failure("sdk.authorization.subscription_invalid"));
    const auto state = impl_;
    auto owned = std::make_shared<PolicyChangeCallback>();
    owned->swap(callback);
    auto observer = std::make_shared<Observer>(scope, std::move(owned));
    auto handle = PolicySubscription::Create([weak = std::weak_ptr<Impl>(state), observer] {
        if (auto owner = weak.lock()) owner->Remove(observer.get());
        observer->Stop();
    });
    if (!handle) return handle;
    {
        std::lock_guard lock(state->mutex);
        if (state->closed) return std::unexpected(Failure("sdk.authorization.policy_closed"));
        state->observers.emplace(observer.get(), observer);
    }
    return handle;
}
Result<std::uint64_t> RevocablePolicy::BindProject(ProjectBinding binding) {
    if (!ValidId(binding.tenant_id) || !ValidId(binding.project_id) || !ValidId(binding.workspace_key) || !binding.version)
        return std::unexpected(Failure("sdk.authorization.binding_invalid"));
    const auto state = impl_;
    return state->Update({binding.tenant_id, binding.project_id, binding.workspace_key, {}}, [&](Rules& rules) -> Result<bool> {
        const BindingKey key{binding.tenant_id, binding.project_id, binding.workspace_key};
        const auto old = rules.bindings.find(key);
        if (old != rules.bindings.end() && old->second == binding.version) return false;
        const auto previous = rules.binding_versions.find(key);
        if (previous != rules.binding_versions.end() && binding.version <= previous->second)
            return std::unexpected(Failure("sdk.authorization.binding_stale"));
        rules.bindings[key] = binding.version;
        rules.binding_versions[key] = binding.version;
        return true;
    });
}
Result<std::uint64_t> RevocablePolicy::UnbindProject(std::string tenant_id, std::string project_id, std::string workspace_key) {
    if (!ValidId(tenant_id) || !ValidId(project_id) || !ValidId(workspace_key))
        return std::unexpected(Failure("sdk.authorization.binding_invalid"));
    const auto state = impl_;
    return state->Update({tenant_id, project_id, workspace_key, {}}, [&](Rules& rules) -> Result<bool> {
        const BindingKey binding{tenant_id, project_id, workspace_key};
        if (!rules.bindings.erase(binding)) return false;
        for (auto grant = rules.grants.begin(); grant != rules.grants.end();) {
            const auto& scope = grant->first.second;
            if (BindingKey{std::get<0>(scope), std::get<1>(scope), std::get<2>(scope)} == binding)
                grant = rules.grants.erase(grant);
            else ++grant;
        }
        return true;
    });
}
Result<std::uint64_t> RevocablePolicy::SetTenantAdministrator(AuthenticatedSubject subject, bool enabled) {
    if (!ValidSubject(subject)) return std::unexpected(Failure("sdk.authorization.subject_invalid"));
    const auto state = impl_;
    return state->Update({subject.tenant_id, {}, {}, {}}, [&](Rules& rules) -> Result<bool> {
        if (enabled) return rules.administrators.insert(Key(subject)).second;
        return rules.administrators.erase(Key(subject)) != 0;
    });
}
Result<std::uint64_t> RevocablePolicy::Grant(AuthenticatedSubject subject, ResourceScope resource, std::vector<Action> actions) {
    auto valid = ValidateGrant(subject, resource, actions);
    if (!valid) return std::unexpected(valid.error());
    const auto state = impl_;
    return state->Update({resource.tenant_id, resource.project_id, resource.workspace_key, resource.session_id},
        [&](Rules& rules) -> Result<bool> {
            const auto binding = rules.bindings.find(Binding(resource));
            if (binding == rules.bindings.end()) return std::unexpected(Failure("sdk.authorization.project_unbound"));
            auto [grant, inserted] = rules.grants.try_emplace({Key(subject), Key(resource)}, GrantRecord{binding->second, {}});
            bool changed = inserted;
            if (grant->second.binding_version != binding->second) {
                grant->second = GrantRecord{binding->second, {}};
                changed = true;
            }
            for (Action action : actions) changed = grant->second.actions.insert(action).second || changed;
            return changed;
        });
}
Result<std::uint64_t> RevocablePolicy::Revoke(AuthenticatedSubject subject, ResourceScope resource, std::vector<Action> actions) {
    auto valid = ValidateGrant(subject, resource, actions);
    if (!valid) return std::unexpected(valid.error());
    const auto state = impl_;
    return state->Update({resource.tenant_id, resource.project_id, resource.workspace_key, resource.session_id},
        [&](Rules& rules) -> Result<bool> {
            const auto grant = rules.grants.find({Key(subject), Key(resource)});
            if (grant == rules.grants.end()) return false;
            bool changed = false;
            for (Action action : actions) changed = grant->second.actions.erase(action) != 0 || changed;
            if (grant->second.actions.empty()) rules.grants.erase(grant);
            return changed;
        });
}

} // namespace lubancore::authorization::v1
