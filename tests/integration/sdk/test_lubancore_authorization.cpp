#include <doctest/doctest.h>

#include "lubancore/authorization.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <future>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <utility>
#include <vector>

namespace {
namespace auth = lubancore::authorization::v1;
using namespace std::chrono_literals;
const auth::AuthenticatedSubject alice{"tenant-a", "alice", auth::ActorKind::User, "credential-a"};
const auth::AuthenticatedSubject bob{"tenant-a", "bob", auth::ActorKind::User, "credential-b"};
const auth::ResourceScope first{"tenant-a", "same-project", "workspace-a", "session-a"};
const auth::ResourceScope second{"tenant-a", "same-project", "workspace-a", "session-b"};
auth::ExecutionContext Context(auth::AuthenticatedSubject subject = alice, auth::ResourceScope scope = first) {
    return {std::move(subject), std::move(scope), std::nullopt, 1};
}
std::shared_ptr<auth::RevocablePolicy> Policy() {
    auto policy = auth::RevocablePolicy::Create();
    REQUIRE(policy->BindProject({"tenant-a", "same-project", "workspace-a", 1}).has_value());
    return policy;
}
bool Allowed(const std::shared_ptr<auth::PolicyProvider>& policy, const auth::ExecutionContext& context,
    auth::Action action = auth::Action::ReadOperation) {
    const auto result = auth::Authorize(policy, context, action);
    return result && result->allowed;
}
class FakePolicy final : public auth::PolicyProvider {
public:
    struct Slot { auth::PolicyChangeCallback callback; };
    std::shared_ptr<Slot> slot;
    mutable unsigned calls = 0;
    bool throwing = false, error = false, null_subscription = false;
    auth::Decision decision{true, 1, "sdk.authorization.allowed"};
    lubancore::Result<auth::Decision> Authorize(const auth::ExecutionContext&, auth::Action) const override {
        ++calls;
        if (throwing) throw std::runtime_error("PRIVATE_AUTHORIZATION_SECRET");
        if (error) return std::unexpected(lubancore::Error{"private.failure", "PRIVATE_AUTHORIZATION_SECRET"});
        return decision;
    }
    lubancore::Result<std::unique_ptr<auth::PolicySubscription>> SubscribeChanges(
        const auth::ResourceScope&, auth::PolicyChangeCallback callback) override {
        ++calls;
        if (throwing) throw std::runtime_error("PRIVATE_AUTHORIZATION_SECRET");
        if (null_subscription) return std::unique_ptr<auth::PolicySubscription>{};
        slot = std::make_shared<Slot>();
        slot->callback.swap(callback);
        return auth::PolicySubscription::Create([owned = slot] { owned->callback = nullptr; });
    }
};

// Memory-only concurrency probes. A bad lock order terminates with failure rather
// than hanging the entire native suite. Each fixture gate also releases on a
// bounded wait, including REQUIRE/exception exits before explicit cleanup.
class Watchdog {
public:
    Watchdog() : thread_([this] {
        std::unique_lock lock(mutex_);
        if (!cv_.wait_for(lock, 15s, [&] { return done_; })) {
            std::fputs("authorization fixture deadlocked\n", stderr);
            std::_Exit(91);
        }
    }) {}
    ~Watchdog() {
        { std::lock_guard lock(mutex_); done_ = true; }
        cv_.notify_all();
    }
private:
    std::mutex mutex_;
    std::condition_variable cv_;
    bool done_ = false;
    std::jthread thread_;
};
class Gate {
public:
    ~Gate() { Release(); }
    void Hold() {
        std::unique_lock lock(mutex_);
        entered_ = true;
        cv_.notify_all();
        if (!cv_.wait_for(lock, 5s, [&] { return released_; })) timed_out.store(true);
    }
    bool WaitEntered() {
        std::unique_lock lock(mutex_);
        return cv_.wait_for(lock, 3s, [&] { return entered_; });
    }
    void Release() {
        { std::lock_guard lock(mutex_); released_ = true; }
        cv_.notify_all();
    }
    std::atomic<bool> timed_out{false};
private:
    std::mutex mutex_;
    std::condition_variable cv_;
    bool entered_ = false, released_ = false;
};
} // namespace

TEST_CASE("SDK authorization: malformed and cross-tenant requests never reach a permissive provider") {
    auto provider = std::make_shared<FakePolicy>();
    CHECK(Allowed(provider, Context()));
    const auto calls = provider->calls;
    auto context = Context();
    for (unsigned field = 0; field < 4; ++field) {
        auto missing = context;
        if (field == 0) missing.request_actor.tenant_id.clear();
        if (field == 1) missing.request_actor.user_id.clear();
        if (field == 2) missing.request_actor.credential_id.clear();
        if (field == 3) missing.request_actor.actor_kind = static_cast<auth::ActorKind>(999);
        CHECK_FALSE(Allowed(provider, missing));
    }
    for (unsigned field = 0; field < 4; ++field) {
        auto missing = context;
        if (field == 0) missing.resource.tenant_id.clear();
        if (field == 1) missing.resource.project_id.clear();
        if (field == 2) missing.resource.workspace_key.clear();
        if (field == 3) missing.resource.session_id.clear();
        CHECK_FALSE(Allowed(provider, missing));
    }
    context.request_actor.tenant_id = "tenant-b";
    CHECK_FALSE(Allowed(provider, context));
    context = Context();
    context.project_binding_version = 0;
    CHECK_FALSE(Allowed(provider, context));
    CHECK_FALSE(Allowed(provider, Context(), static_cast<auth::Action>(999)));
    context = Context();
    context.request_actor.user_id = std::string("bad\0subject", 11);
    CHECK_FALSE(Allowed(provider, context));
    CHECK(provider->calls == calls);
    CHECK_FALSE(Allowed({}, Context()));
}

TEST_CASE("SDK authorization: provider faults and invalid allows expose only stable public errors") {
    auto provider = std::make_shared<FakePolicy>();
    provider->throwing = true;
    auto decision = auth::Authorize(provider, Context(), auth::Action::ReadOperation);
    REQUIRE_FALSE(decision.has_value());
    CHECK(decision.error().code == "sdk.authorization.provider_exception");
    CHECK(decision.error().message.empty());
    auto watch = auth::SubscribeChanges(provider, first, [](const auth::PolicyChange&) {});
    REQUIRE_FALSE(watch.has_value());
    CHECK(watch.error().code == "sdk.authorization.provider_exception");
    CHECK(watch.error().message.empty());
    provider->throwing = false;
    provider->error = true;
    decision = auth::Authorize(provider, Context(), auth::Action::ReadOperation);
    REQUIRE_FALSE(decision.has_value());
    CHECK(decision.error().code == "sdk.authorization.provider_failure");
    CHECK(decision.error().message.empty());
    provider->error = false;
    provider->decision.revision = 0;
    CHECK_FALSE(Allowed(provider, Context()));
    provider->decision = {true, 1, "PRIVATE secret text"};
    CHECK_FALSE(Allowed(provider, Context()));
    provider->null_subscription = true;
    CHECK_FALSE(auth::SubscribeChanges(provider, first, [](const auth::PolicyChange&) {}).has_value());
    const auto calls = provider->calls;
    auto invalid = first;
    invalid.session_id.clear();
    CHECK_FALSE(auth::SubscribeChanges(provider, invalid, [](const auth::PolicyChange&) {}).has_value());
    CHECK_FALSE(auth::SubscribeChanges(provider, first, {}).has_value());
    CHECK(provider->calls == calls);
    CHECK_FALSE(auth::SubscribeChanges({}, first, [](const auth::PolicyChange&) {}).has_value());
}

TEST_CASE("SDK authorization: arbitrary provider notices cannot expose another scope or claim a valid revision") {
    auto provider = std::make_shared<FakePolicy>();
    std::vector<auth::PolicyChange> seen;
    auto watch = auth::SubscribeChanges(provider, first, [&](const auth::PolicyChange& change) { seen.push_back(change); });
    REQUIRE(watch.has_value());
    REQUIRE(provider->slot);
    auto foreign = first;
    foreign.tenant_id = "PRIVATE_OTHER_TENANT";
    foreign.session_id = "PRIVATE_OTHER_SESSION";
    provider->slot->callback({foreign, 99});
    provider->slot->callback({first, 0});
    provider->slot->callback({first, 7});
    REQUIRE(seen.size() == 3);
    CHECK(seen[0].scope == first);
    CHECK(seen[0].revision == 0);
    CHECK(seen[1].scope == first);
    CHECK(seen[1].revision == 0);
    CHECK(seen[2].scope == first);
    CHECK(seen[2].revision == 7);
    (*watch)->Unsubscribe();
    CHECK_FALSE(static_cast<bool>(provider->slot->callback));
}

TEST_CASE("SDK authorization: grants bind complete subjects and fixed resources without implicit administrators") {
    auto policy = Policy();
    REQUIRE(policy->BindProject({"tenant-b", "same-project", "workspace-a", 1}).has_value());
    REQUIRE(policy->BindProject({"tenant-a", "same-project", "workspace-b", 1}).has_value());
    CHECK_FALSE(Allowed(policy, Context()));
    REQUIRE(policy->Grant(alice, first, {auth::Action::ReadOperation}).has_value());
    CHECK(Allowed(policy, Context()));
    CHECK_FALSE(Allowed(policy, Context(bob)));
    CHECK_FALSE(Allowed(policy, Context(alice, second)));
    for (unsigned field = 0; field < 3; ++field) {
        auto changed = alice;
        if (field == 0) changed.user_id = "different-user";
        if (field == 1) changed.actor_kind = auth::ActorKind::Service;
        if (field == 2) changed.credential_id = "rotated-credential";
        CHECK_FALSE(Allowed(policy, Context(changed)));
    }
    auto foreign = first;
    foreign.tenant_id = "tenant-b";
    auto foreign_actor = alice;
    foreign_actor.tenant_id = "tenant-b";
    CHECK_FALSE(Allowed(policy, Context(foreign_actor, foreign)));
    CHECK_FALSE(policy->Grant(alice, foreign, {auth::Action::ReadOperation}).has_value());
    auto workspace = first;
    workspace.workspace_key = "workspace-b";
    CHECK_FALSE(Allowed(policy, Context(alice, workspace)));
    auto missing = first;
    missing.workspace_key = "unregistered-workspace";
    CHECK_FALSE(policy->Grant(alice, missing, {auth::Action::ReadOperation}).has_value());
    CHECK_FALSE(policy->Grant(alice, first, {}).has_value());
    CHECK_FALSE(policy->Grant(alice, first, {static_cast<auth::Action>(999)}).has_value());
    REQUIRE(policy->Grant(alice, second, {auth::Action::CancelOperation}).has_value());
    CHECK(Allowed(policy, Context(alice, second), auth::Action::CancelOperation));
    CHECK_FALSE(Allowed(policy, Context(alice, second)));
    CHECK_FALSE(Allowed(policy, Context(), auth::Action::CancelOperation));
    REQUIRE(policy->SetTenantAdministrator(alice, true).has_value());
    CHECK(Allowed(policy, Context(alice, second)));
    CHECK_FALSE(Allowed(policy, Context(foreign_actor, foreign)));
    REQUIRE(policy->SetTenantAdministrator(alice, false).has_value());
    CHECK_FALSE(Allowed(policy, Context(alice, second)));
    const auth::ResourceScope collision_a{"tenant-a", "p:q", "r", "same-session"};
    const auth::ResourceScope collision_b{"tenant-a", "p", "q:r", "same-session"};
    REQUIRE(policy->BindProject({"tenant-a", "p:q", "r", 1}).has_value());
    REQUIRE(policy->BindProject({"tenant-a", "p", "q:r", 1}).has_value());
    REQUIRE(policy->Grant(alice, collision_a, {auth::Action::ReadOperation}).has_value());
    CHECK(Allowed(policy, Context(alice, collision_a)));
    CHECK_FALSE(Allowed(policy, Context(alice, collision_b)));
}

TEST_CASE("SDK authorization: pre-ID open grants are exact project scope and never session wildcards") {
    auto policy = Policy();
    auto project = first;
    project.session_id.clear();
    REQUIRE(policy->Grant(alice, project, {auth::Action::OpenSession}).has_value());
    CHECK(Allowed(policy, Context(alice, project), auth::Action::OpenSession));
    CHECK_FALSE(Allowed(policy, Context(alice, project), auth::Action::ReadHistory));
    CHECK_FALSE(policy->Grant(alice, project, {auth::Action::ReadOperation}).has_value());
    CHECK_FALSE(Allowed(policy, Context(), auth::Action::OpenSession));
    CHECK_FALSE(Allowed(policy, Context(), auth::Action::ReadHistory));
    CHECK_FALSE(auth::SubscribeChanges(policy, project, [](const auth::PolicyChange&) {}).has_value());
}

TEST_CASE("SDK authorization: execution provenance never lends the initiating subject to current queries") {
    auto policy = Policy();
    REQUIRE(policy->SetTenantAdministrator(alice, true).has_value());
    auto context = Context();
    context.execution = auth::ExecutionScope{first, alice, policy->CurrentRevision(), "execution-a", 0,
        {auth::Action::RequestModel}};
    CHECK(Allowed(policy, context, auth::Action::RequestModel));
    CHECK_FALSE(Allowed(policy, context, auth::Action::DispatchTool));
    auto query = context;
    query.request_actor = bob;
    CHECK_FALSE(Allowed(policy, query));
    REQUIRE(policy->Grant(bob, first, {auth::Action::ReadOperation, auth::Action::ResolveApproval,
        auth::Action::CancelOperation}).has_value());
    CHECK(Allowed(policy, query));
    CHECK(Allowed(policy, query, auth::Action::ResolveApproval));
    CHECK(Allowed(policy, query, auth::Action::CancelOperation));
    CHECK_FALSE(Allowed(policy, query, auth::Action::RequestModel));
    auto bad = context;
    bad.execution->resource = second;
    CHECK_FALSE(Allowed(policy, bad));
    bad = context;
    bad.execution->initiating_subject.tenant_id = "tenant-b";
    CHECK_FALSE(Allowed(policy, bad));
    bad = context;
    bad.execution->allowed_capabilities.push_back(static_cast<auth::Action>(999));
    CHECK_FALSE(Allowed(policy, bad));
    bad = context;
    bad.execution->policy_revision = 0;
    CHECK_FALSE(Allowed(policy, bad));
    bad = context;
    bad.execution->execution_id.clear();
    CHECK_FALSE(Allowed(policy, bad));
    bad = context;
    bad.execution->initiating_subject.credential_id.clear();
    CHECK_FALSE(Allowed(policy, bad));
    REQUIRE(policy->SetTenantAdministrator(alice, false).has_value());
    CHECK_FALSE(Allowed(policy, context, auth::Action::RequestModel));
    CHECK_FALSE(Allowed(policy, Context(), auth::Action::RequestModel));
}

TEST_CASE("SDK authorization: revocation and binding tombstones cannot revive saved permits") {
    auto policy = Policy();
    REQUIRE(policy->Grant(alice, first, {auth::Action::ReadOperation}).has_value());
    const auto admitted = auth::Authorize(policy, Context(), auth::Action::ReadOperation);
    REQUIRE(admitted.has_value());
    REQUIRE(admitted->allowed);
    REQUIRE(policy->Revoke(alice, first, {auth::Action::ReadOperation}).has_value());
    CHECK_FALSE(Allowed(policy, Context()));
    CHECK(admitted->allowed); // Already handed-out values are not revocable handles.
    CHECK(policy->CurrentRevision() > admitted->revision);
    REQUIRE(policy->Grant(alice, first, {auth::Action::ReadOperation}).has_value());
    REQUIRE(policy->UnbindProject("tenant-a", "same-project", "workspace-a").has_value());
    CHECK_FALSE(Allowed(policy, Context()));
    CHECK_FALSE(policy->BindProject({"tenant-a", "same-project", "workspace-a", 1}).has_value());
    REQUIRE(policy->BindProject({"tenant-a", "same-project", "workspace-a", 2}).has_value());
    CHECK_FALSE(Allowed(policy, Context()));
    auto current = Context();
    current.project_binding_version = 2;
    CHECK_FALSE(Allowed(policy, current));
    REQUIRE(policy->Grant(alice, first, {auth::Action::ReadOperation}).has_value());
    CHECK(Allowed(policy, current));
    const auto revision = policy->CurrentRevision();
    REQUIRE(policy->BindProject({"tenant-a", "same-project", "workspace-a", 2}).has_value());
    CHECK(policy->CurrentRevision() == revision);
    REQUIRE(policy->UnbindProject("tenant-a", "same-project", "workspace-a").has_value());
    CHECK_FALSE(policy->BindProject({"tenant-a", "same-project", "workspace-a", 1}).has_value());
    CHECK_FALSE(policy->BindProject({"tenant-a", "same-project", "workspace-a", 2}).has_value());
    REQUIRE(policy->BindProject({"tenant-a", "same-project", "workspace-a", 3}).has_value());
    current.project_binding_version = 3;
    CHECK_FALSE(Allowed(policy, current));
}

TEST_CASE("SDK authorization: reentrant invalidation is serial and coalesces without locked host callbacks") {
    Watchdog watchdog;
    auto policy = Policy();
    Gate gate;
    std::atomic<unsigned> active{0}, peak{0}, calls{0};
    std::atomic<bool> bad_scope{false}, reentered{false};
    std::mutex records_mutex;
    std::vector<std::uint64_t> revisions;
    auto watch = auth::SubscribeChanges(policy, first, [&](const auth::PolicyChange& change) {
        const auto count = ++active;
        peak.store(std::max(peak.load(), count));
        if (change.scope != first || !change.revision) bad_scope.store(true);
        { std::lock_guard lock(records_mutex); revisions.push_back(change.revision); }
        const auto call = ++calls;
        const auto current = policy->CurrentRevision();
        if (current < change.revision) bad_scope.store(true);
        if (call == 1) {
            reentered.store(policy->Grant(alice, first, {auth::Action::ReadHistory}).has_value());
            gate.Hold();
        }
        --active;
    });
    REQUIRE(watch.has_value());
    auto change = std::async(std::launch::async, [&] {
        return policy->Grant(alice, first, {auth::Action::ReadOperation});
    });
    REQUIRE(gate.WaitEntered());
    REQUIRE(policy->Revoke(alice, first, {auth::Action::ReadHistory}).has_value());
    REQUIRE(policy->Grant(alice, first, {auth::Action::ReadHistory}).has_value());
    REQUIRE(policy->Revoke(alice, first, {auth::Action::ReadOperation}).has_value());
    gate.Release();
    REQUIRE(change.wait_for(3s) == std::future_status::ready);
    REQUIRE(change.get().has_value());
    CHECK(peak.load() == 1);
    CHECK(reentered.load());
    CHECK_FALSE(bad_scope.load());
    CHECK_FALSE(gate.timed_out.load());
    CHECK(revisions.size() >= 2);
    CHECK(std::is_sorted(revisions.begin(), revisions.end()));
    CHECK(std::adjacent_find(revisions.begin(), revisions.end()) == revisions.end());
    CHECK(revisions.back() == policy->CurrentRevision());
    const auto stopped = calls.load();
    (*watch)->Unsubscribe();
    REQUIRE(policy->Revoke(alice, first, {auth::Action::ReadHistory}).has_value());
    CHECK(calls.load() == stopped);
}

TEST_CASE("SDK authorization: notification-side disarm still permits a later external unsubscribe to drain") {
    Watchdog watchdog;
    auto policy = Policy();
    Gate gate;
    std::unique_ptr<auth::PolicySubscription> subscription;
    std::atomic<bool> callback_disarmed{false};
    auto watch = auth::SubscribeChanges(policy, first, [&](const auth::PolicyChange&) {
        subscription->Unsubscribe();
        callback_disarmed.store(true);
        gate.Hold();
    });
    REQUIRE(watch.has_value());
    subscription = std::move(*watch);
    auto changing = std::async(std::launch::async, [&] { return policy->Grant(alice, first, {auth::Action::ReadOperation}); });
    REQUIRE(gate.WaitEntered());
    REQUIRE(callback_disarmed.load());
    std::promise<void> closing_started;
    auto started = closing_started.get_future();
    auto closing = std::async(std::launch::async, [&] {
        closing_started.set_value();
        subscription->Unsubscribe();
    });
    REQUIRE(started.wait_for(3s) == std::future_status::ready);
    CHECK(closing.wait_for(100ms) == std::future_status::timeout);
    gate.Release();
    REQUIRE(changing.wait_for(3s) == std::future_status::ready);
    REQUIRE(changing.get().has_value());
    REQUIRE(closing.wait_for(3s) == std::future_status::ready);
    closing.get();
    CHECK_FALSE(gate.timed_out.load());
    subscription->Unsubscribe();
}

TEST_CASE("SDK authorization: concurrent notifications can unsubscribe one another without mutual waits") {
    Watchdog watchdog;
    auto policy = Policy();
    std::unique_ptr<auth::PolicySubscription> first_watch, second_watch;
    std::mutex barrier_mutex;
    std::condition_variable barrier_cv;
    unsigned arrivals = 0;
    std::atomic<unsigned> disarmed{0};
    std::atomic<bool> barrier_timeout{false};
    auto cross_close = [&](auth::PolicySubscription* other) {
        {
            std::unique_lock lock(barrier_mutex);
            ++arrivals;
            barrier_cv.notify_all();
            if (!barrier_cv.wait_for(lock, 5s, [&] { return arrivals == 2; })) barrier_timeout.store(true);
        }
        other->Unsubscribe();
        ++disarmed;
    };
    auto a = auth::SubscribeChanges(policy, first, [&](const auth::PolicyChange&) { cross_close(second_watch.get()); });
    auto b = auth::SubscribeChanges(policy, second, [&](const auth::PolicyChange&) { cross_close(first_watch.get()); });
    REQUIRE(a.has_value());
    REQUIRE(b.has_value());
    first_watch = std::move(*a);
    second_watch = std::move(*b);
    auto first_change = std::async(std::launch::async, [&] { return policy->Grant(alice, first, {auth::Action::ReadOperation}); });
    auto second_change = std::async(std::launch::async, [&] { return policy->Grant(alice, second, {auth::Action::ReadOperation}); });
    REQUIRE(first_change.wait_for(7s) == std::future_status::ready);
    REQUIRE(second_change.wait_for(3s) == std::future_status::ready);
    REQUIRE(first_change.get().has_value());
    REQUIRE(second_change.get().has_value());
    CHECK(disarmed.load() == 2);
    CHECK_FALSE(barrier_timeout.load());
    first_watch->Unsubscribe();
    second_watch->Unsubscribe();
    REQUIRE(policy->Revoke(alice, first, {auth::Action::ReadOperation}).has_value());
    REQUIRE(policy->Revoke(alice, second, {auth::Action::ReadOperation}).has_value());
    CHECK(disarmed.load() == 2);
}

namespace {
struct CaptureState {
    std::weak_ptr<auth::RevocablePolicy> policy;
    std::atomic<unsigned> queries{0}, invocations{0};
    std::atomic<bool> armed{false};
};
struct SmallObserver {
    std::shared_ptr<CaptureState> state;
    SmallObserver(const SmallObserver&) noexcept = default;
    SmallObserver(SmallObserver&&) noexcept = default;
    explicit SmallObserver(std::shared_ptr<CaptureState> value) : state(std::move(value)) {}
    ~SmallObserver() {
        if (state && state->armed.load()) {
            if (auto owner = state->policy.lock()) {
                (void)owner->CurrentRevision();
                ++state->queries;
            }
        }
    }
    void operator()(const auth::PolicyChange&) const { ++state->invocations; }
};
static_assert(sizeof(SmallObserver) == sizeof(std::shared_ptr<CaptureState>));
} // namespace

TEST_CASE("SDK authorization: capture destructors reenter unlocked policy and release at external unsubscribe") {
    Watchdog watchdog;
    auto policy = Policy();
    auto capture = std::make_shared<CaptureState>();
    capture->policy = policy;
    capture->armed.store(true);
    std::weak_ptr<CaptureState> weak = capture;
    auth::PolicyChangeCallback callback = SmallObserver(capture);
    auto watch = auth::SubscribeChanges(policy, first, std::move(callback));
    callback = nullptr; // libc++ may retain a moved-from SBO source.
    REQUIRE(watch.has_value());
    REQUIRE(policy->Grant(alice, first, {auth::Action::ReadOperation}).has_value());
    CHECK(capture->invocations.load() == 1);
    const auto queries = capture->queries.load();
    (*watch)->Unsubscribe();
    CHECK(capture->queries.load() > queries);
    capture.reset();
    CHECK(weak.expired());
    (*watch)->Unsubscribe();
    REQUIRE(policy->Revoke(alice, first, {auth::Action::ReadOperation}).has_value());
}

TEST_CASE("SDK authorization: throwing observers cannot block peers and subscriptions can outlive provider") {
    Watchdog watchdog;
    auto policy = Policy();
    std::atomic<unsigned> good{0}, bad{0};
    auto throwing = auth::SubscribeChanges(policy, first, [&](const auth::PolicyChange&) {
        ++bad;
        (void)policy->CurrentRevision();
        throw std::runtime_error("PRIVATE_OBSERVER_SECRET");
    });
    auto healthy = auth::SubscribeChanges(policy, first, [&](const auth::PolicyChange&) { ++good; });
    REQUIRE(throwing.has_value());
    REQUIRE(healthy.has_value());
    REQUIRE(policy->Grant(alice, first, {auth::Action::ReadOperation}).has_value());
    CHECK(bad.load() == 1);
    CHECK(good.load() == 1);
    auto capture = std::make_shared<int>(1);
    std::weak_ptr<int> weak = capture;
    auto owner_loss = auth::SubscribeChanges(policy, second, [capture](const auth::PolicyChange&) {});
    REQUIRE(owner_loss.has_value());
    capture.reset();
    CHECK_FALSE(weak.expired());
    policy.reset();
    CHECK(weak.expired());
    (*healthy)->Unsubscribe();
    (*throwing)->Unsubscribe();
    (*owner_loss)->Unsubscribe();

    auto running = Policy();
    Gate gate;
    auto active_watch = auth::SubscribeChanges(running, first, [&](const auth::PolicyChange&) { gate.Hold(); });
    REQUIRE(active_watch.has_value());
    // Entered is the lifetime fence: the method has already leased Impl, and it
    // never uses the public object again after dispatching the owned callback.
    auto* raw = running.get();
    auto changing = std::async(std::launch::async, [raw] { return raw->Grant(alice, first, {auth::Action::ReadOperation}); });
    REQUIRE(gate.WaitEntered());
    std::promise<void> destructor_started;
    auto started = destructor_started.get_future();
    auto closing = std::async(std::launch::async, [owner = std::move(running), &destructor_started]() mutable {
        destructor_started.set_value();
        owner.reset();
    });
    REQUIRE(started.wait_for(3s) == std::future_status::ready);
    CHECK(closing.wait_for(100ms) == std::future_status::timeout);
    gate.Release();
    REQUIRE(changing.wait_for(3s) == std::future_status::ready);
    REQUIRE(changing.get().has_value());
    REQUIRE(closing.wait_for(3s) == std::future_status::ready);
    closing.get();
    CHECK_FALSE(gate.timed_out.load());
    (*active_watch)->Unsubscribe();
}
