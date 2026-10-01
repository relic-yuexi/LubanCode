#pragma once

#include "lubancore/api.hpp"

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace lubancore::authorization::v1 {

// Authentication is performed by the trusted host. These values do not verify
// credentials, authenticate a wire client, or provide an OS sandbox.
enum class ActorKind { User, Agent, Service };
struct AuthenticatedSubject {
    std::string tenant_id, user_id;
    ActorKind actor_kind = ActorKind::User;
    std::string credential_id;
    bool operator==(const AuthenticatedSubject&) const = default;
};
struct ResourceScope {
    std::string tenant_id, project_id, workspace_key, session_id;
    bool operator==(const ResourceScope&) const = default;
};
enum class Action {
    OpenSession, ResumeSession, AcquireView, ReadSession,
    Submit, ReadOperation, ReadHistory, ReadApprovals, ResolveApproval,
    CancelOperation, CloseSession, ReadExtensions, ListToolResults,
    ReadToolResult, SubscribeEvents, ReadEvents, WaitResult,
    RequestModel, DispatchTool,
};
struct ExecutionScope {
    ResourceScope resource;
    AuthenticatedSubject initiating_subject;
    std::uint64_t policy_revision = 0;
    std::string execution_id;
    std::uint64_t lease_epoch = 0; // Provenance only; no lease/fencing is implemented.
    std::vector<Action> allowed_capabilities;
    bool operator==(const ExecutionScope&) const = default;
};
struct ExecutionContext {
    AuthenticatedSubject request_actor;
    ResourceScope resource;
    std::optional<ExecutionScope> execution;
    // Version of this exact tenant/project/workspace binding, not policy revision.
    std::uint64_t project_binding_version = 0;
};
struct ProjectBinding {
    std::string tenant_id, project_id, workspace_key;
    std::uint64_t version = 1;
    bool operator==(const ProjectBinding&) const = default;
};
struct Decision {
    bool allowed = false;
    std::uint64_t revision = 0;
    std::string reason_code;
};
struct PolicyChange {
    ResourceScope scope; // The subscribed scope, not another tenant's changed data.
    // Zero means invalid/unknown notification version: invalidate and recheck,
    // never treat it as permission, a synchronized revision or delivery evidence.
    std::uint64_t revision = 0;
};
using PolicyChangeCallback = std::function<void(const PolicyChange&)>;

// A move-only owner through unique_ptr. Destruction unsubscribes. The supplied
// close operation must disable new callbacks and drain in-flight callbacks when
// called outside a policy notification. The close operation must be idempotent
// and must not throw:
// after notification-side disarm it is called again for external drain.
// Notification-side unsubscribe must not
// wait for itself or another notification; its owned callback state survives
// until the invocation exits. Concurrent Unsubscribe calls are supported;
// destruction of the handle itself still needs ordinary owner synchronization.
class LUBANCORE_API PolicySubscription {
public:
    static Result<std::unique_ptr<PolicySubscription>> Create(std::function<void()> unsubscribe);
    ~PolicySubscription();
    void Unsubscribe() noexcept;
    PolicySubscription(const PolicySubscription&) = delete;
    PolicySubscription& operator=(const PolicySubscription&) = delete;
private:
    struct Impl;
    explicit PolicySubscription(std::shared_ptr<Impl> impl);
    std::shared_ptr<Impl> impl_;
};

class LUBANCORE_API PolicyProvider {
public:
    virtual ~PolicyProvider();
    virtual Result<Decision> Authorize(const ExecutionContext& context, Action action) const = 0;
    virtual Result<std::unique_ptr<PolicySubscription>> SubscribeChanges(
        const ResourceScope& scope, PolicyChangeCallback callback) = 0;
};

// Use these boundary helpers for an arbitrary trusted provider. They validate
// context/actions and convert provider exceptions to stable public errors.
// Queries, approvals and cancellation always use request_actor even when an
// execution is present. RequestModel/DispatchTool require execution and require
// request_actor == initiating_subject; its capabilities only narrow permission.
// A saved revision is provenance, never a cached permit. Recheck before dispatch
// and publication; changes outside this process are not atomically revoked here.
// SubscribeChanges does not authorize data access. It freezes the requested
// scope; malformed provider notices become that scope/revision=0 invalidations.
// There is no initial replay or atomic subscribe+authorize. A future Managed
// waiter must subscribe first, then recheck current permission and publication.
LUBANCORE_API Result<Decision> Authorize(std::shared_ptr<PolicyProvider> provider,
    const ExecutionContext& context, Action action);
LUBANCORE_API Result<std::unique_ptr<PolicySubscription>> SubscribeChanges(
    std::shared_ptr<PolicyProvider> provider, const ResourceScope& scope, PolicyChangeCallback callback);

// Small host-configured, in-memory reference provider; no implicit tenant/admin.
// Grants bind the complete subject (including actor kind and credential), exact
// resource and current binding version. Multiple workspaces per project and
// multiple sessions per workspace are independent entries.
class LUBANCORE_API RevocablePolicy final : public PolicyProvider {
public:
    static std::shared_ptr<RevocablePolicy> Create();
    ~RevocablePolicy() override;
    Result<Decision> Authorize(const ExecutionContext& context, Action action) const override;
    Result<std::unique_ptr<PolicySubscription>> SubscribeChanges(
        const ResourceScope& scope, PolicyChangeCallback callback) override;
    Result<std::uint64_t> BindProject(ProjectBinding binding);
    Result<std::uint64_t> UnbindProject(std::string tenant_id, std::string project_id,
        std::string workspace_key);
    Result<std::uint64_t> SetTenantAdministrator(AuthenticatedSubject subject, bool enabled);
    Result<std::uint64_t> Grant(AuthenticatedSubject subject, ResourceScope resource,
        std::vector<Action> actions);
    Result<std::uint64_t> Revoke(AuthenticatedSubject subject, ResourceScope resource,
        std::vector<Action> actions);
    std::uint64_t CurrentRevision() const;
    // Only OpenSession permits an empty session_id (project scope); such a grant
    // is exact, never a wildcard for session reads or cancellation. Subscriptions
    // require a complete session scope. Binding versions keep an in-memory high
    // water mark even after unbind; recreating this provider is not persistence.
    // IDs are opaque, nonempty <=512-byte values with no ASCII controls. They are
    // not filesystem paths, normalized tenant names, or authentication tokens.
    // This first seam evaluates actions, not effective tool/model parameters.
    // Changes serialize per subscription and may coalesce to the newest revision;
    // notification is invalidation, not an every-revision delivery log. Callbacks
    // and capture destruction run without policy/subscription state locks.
    // External unsubscribe/destruction drains; from any notification it only
    // disarms (including cross-subscription unsubscribe) to avoid mutual waits.
    // Destroying the provider disarms all watches under the same rule. A remaining
    // subscription may be destroyed safely after its provider has gone away.
private:
    struct Impl;
    explicit RevocablePolicy(std::shared_ptr<Impl> impl);
    std::shared_ptr<Impl> impl_;
};

} // namespace lubancore::authorization::v1
