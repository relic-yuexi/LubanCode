#include <lubancore/authorization.hpp>

#include <atomic>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>

namespace {
namespace auth = lubancore::authorization::v1;

void Require(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}
template<class T>
T Take(lubancore::Result<T> result) {
    if (!result) throw std::runtime_error(result.error().code + ": " + result.error().message);
    return std::move(*result);
}
bool Allowed(const std::shared_ptr<auth::PolicyProvider>& provider,
             const auth::ExecutionContext& context, auth::Action action) {
    const auto decision = auth::Authorize(provider, context, action);
    return decision && decision->allowed;
}

class ThrowingPolicy final : public auth::PolicyProvider {
public:
    lubancore::Result<auth::Decision> Authorize(const auth::ExecutionContext&, auth::Action) const override {
        throw std::runtime_error("PRIVATE_PROVIDER_SECRET");
    }
    lubancore::Result<std::unique_ptr<auth::PolicySubscription>> SubscribeChanges(
        const auth::ResourceScope&, auth::PolicyChangeCallback) override {
        throw std::runtime_error("PRIVATE_PROVIDER_SECRET");
    }
};
class InvalidDecisionPolicy final : public auth::PolicyProvider {
public:
    lubancore::Result<auth::Decision> Authorize(const auth::ExecutionContext&, auth::Action) const override {
        return auth::Decision{true, 0, "allow"};
    }
    lubancore::Result<std::unique_ptr<auth::PolicySubscription>> SubscribeChanges(
        const auth::ResourceScope&, auth::PolicyChangeCallback) override {
        return std::unique_ptr<auth::PolicySubscription>{};
    }
};
class InvalidChangePolicy final : public auth::PolicyProvider {
public:
    lubancore::Result<auth::Decision> Authorize(const auth::ExecutionContext&, auth::Action) const override {
        return auth::Decision{false, 1, "denied"};
    }
    lubancore::Result<std::unique_ptr<auth::PolicySubscription>> SubscribeChanges(
        const auth::ResourceScope& scope, auth::PolicyChangeCallback callback) override {
        auto foreign = scope;
        foreign.tenant_id = "other-tenant";
        foreign.session_id = "private-other-session";
        callback({foreign, 9});
        callback({scope, 0});
        return auth::PolicySubscription::Create([] {});
    }
};
} // namespace

// This translation unit is copied with the consumer and uses only the installed
// public SDK. It tests exported symbols and provider virtual dispatch after the
// producer prefix has moved; it does not claim Managed Session enforcement.
void CheckSdkAuthorizationConsumer() {
    auto policy = auth::RevocablePolicy::Create();
    Require(static_cast<bool>(policy), "missing reference policy");
    const auth::AuthenticatedSubject alice{"tenant-a", "alice", auth::ActorKind::User, "credential-a"};
    const auth::AuthenticatedSubject bob{"tenant-a", "bob", auth::ActorKind::User, "credential-b"};
    const auth::ResourceScope first{"tenant-a", "same-project", "same-workspace", "session-a"};
    const auth::ResourceScope second{"tenant-a", "same-project", "same-workspace", "session-b"};
    auth::ExecutionContext context{alice, first, std::nullopt, 1};
    Take(policy->BindProject({"tenant-a", "same-project", "same-workspace", 1}));
    Take(policy->BindProject({"tenant-b", "same-project", "same-workspace", 1}));
    Require(!Allowed(policy, context, auth::Action::ReadOperation), "implicit administrator permission");
    Require(!Allowed({}, context, auth::Action::ReadOperation), "missing provider allowed access");
    auto missing = context;
    missing.request_actor.credential_id.clear();
    Require(!Allowed(policy, missing, auth::Action::ReadOperation), "missing subject allowed access");

    auto project = context;
    project.resource.session_id.clear();
    Take(policy->Grant(alice, project.resource, {auth::Action::OpenSession}));
    Require(Allowed(policy, project, auth::Action::OpenSession), "project-scoped create grant denied");
    Require(!Allowed(policy, project, auth::Action::ReadOperation), "project scope read Session data");
    Require(!Allowed(policy, context, auth::Action::ReadOperation), "project grant became a Session wildcard");

    Take(policy->Grant(alice, first, {auth::Action::ReadOperation, auth::Action::ReadToolResult}));
    Require(Allowed(policy, context, auth::Action::ReadToolResult), "explicit grant did not work");
    auto other = context;
    other.resource = second;
    Require(!Allowed(policy, other, auth::Action::ReadToolResult), "grant crossed Session boundary");
    other.resource = {"tenant-b", "same-project", "same-workspace", "session-a"};
    Require(!Allowed(policy, other, auth::Action::ReadToolResult), "grant crossed tenant boundary");
    other = context;
    other.request_actor.credential_id = "rotated-credential";
    Require(!Allowed(policy, other, auth::Action::ReadToolResult), "grant ignored credential identity");

    std::atomic<unsigned> notices{0};
    std::atomic<bool> wrong_scope{false};
    auto subscription = Take(auth::SubscribeChanges(policy, first, [&](const auth::PolicyChange& change) {
        if (change.scope != first || change.revision == 0) wrong_scope.store(true);
        notices.fetch_add(1);
    }));
    Require(static_cast<bool>(subscription), "reference policy returned no subscription");
    Take(policy->Revoke(alice, first, {auth::Action::ReadToolResult}));
    Require(!Allowed(policy, context, auth::Action::ReadToolResult), "revoked grant remained usable");
    Require(notices.load() > 0 && !wrong_scope.load(), "revocation notification was missing or leaked scope");
    subscription->Unsubscribe();
    subscription->Unsubscribe();
    const auto stopped_notices = notices.load();
    Take(policy->Grant(alice, first, {auth::Action::ReadToolResult}));
    Require(notices.load() == stopped_notices, "unsubscribed callback still fired");
    subscription.reset();

    Take(policy->SetTenantAdministrator(alice, true));
    Require(Allowed(policy, context, auth::Action::ReadOperation), "explicit tenant administrator denied");
    auth::ExecutionScope execution{first, alice, policy->CurrentRevision(), "execution-a", 0,
                                  {auth::Action::RequestModel}};
    context.execution = execution;
    Require(Allowed(policy, context, auth::Action::RequestModel), "authorized initiating subject denied");
    Require(!Allowed(policy, context, auth::Action::DispatchTool), "execution capabilities widened permission");
    auto query = context;
    query.request_actor = bob;
    Require(!Allowed(policy, query, auth::Action::ReadOperation), "query borrowed initiating subject permission");
    Require(!Allowed(policy, query, auth::Action::RequestModel), "dispatch changed initiating subject");
    Take(policy->SetTenantAdministrator(alice, false));
    Require(!Allowed(policy, context, auth::Action::RequestModel), "saved revision became a cached permit");

    Take(policy->BindProject({"tenant-a", "same-project", "same-workspace", 2}));
    Require(!Allowed(policy, context, auth::Action::ReadToolResult), "stale project binding was accepted");
    context.execution.reset();
    context.project_binding_version = 2;
    Require(!Allowed(policy, context, auth::Action::ReadToolResult), "old grant survived project rebinding");
    Take(policy->UnbindProject("tenant-a", "same-project", "same-workspace"));
    Require(!policy->BindProject({"tenant-a", "same-project", "same-workspace", 1}),
            "unbound project accepted an old binding version");
    Take(policy->BindProject({"tenant-a", "same-project", "same-workspace", 3}));
    context.project_binding_version = 3;
    Require(!Allowed(policy, context, auth::Action::ReadToolResult), "unbound grant revived on rebinding");

    auto throwing = std::make_shared<ThrowingPolicy>();
    const auto rejected = auth::Authorize(throwing, context, auth::Action::ReadOperation);
    Require(!rejected, "provider exception granted permission");
    Require(rejected.error().message.find("PRIVATE_PROVIDER_SECRET") == std::string::npos,
            "provider exception text leaked");
    const auto rejected_watch = auth::SubscribeChanges(throwing, first, [](const auth::PolicyChange&) {});
    Require(!rejected_watch, "subscription exception was accepted");
    Require(rejected_watch.error().message.find("PRIVATE_PROVIDER_SECRET") == std::string::npos,
            "subscription exception text leaked");
    auto malformed = std::make_shared<InvalidDecisionPolicy>();
    Require(!Allowed(malformed, context, auth::Action::ReadOperation), "zero-revision allow was accepted");
    Require(!auth::SubscribeChanges(malformed, first, [](const auth::PolicyChange&) {}),
            "null subscription was accepted");
    unsigned invalidations = 0;
    bool invalid_notification = false;
    auto invalidating = std::make_shared<InvalidChangePolicy>();
    auto guarded = Take(auth::SubscribeChanges(invalidating, first, [&](const auth::PolicyChange& change) {
        ++invalidations;
        if (change.scope != first || change.revision != 0) invalid_notification = true;
    }));
    Require(invalidations == 2 && !invalid_notification,
            "invalid provider notification leaked scope or suppressed invalidation");
    guarded.reset();
    subscription = Take(auth::SubscribeChanges(policy, first, [](const auth::PolicyChange&) {}));
    policy.reset();
    subscription.reset(); // Subscription cleanup remains safe after provider loss.
}
