#include <doctest/doctest.h>

#include "lubancore/authorization.hpp"
#include "lubancore/core.hpp"
#include "lubancore/managed.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <future>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
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
void CheckManagedStorageLifecycle();
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
    CheckManagedStorageLifecycle();
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

namespace {
struct CleanupCaptureState {
    auth::PolicySubscription* subscription = nullptr;
    Gate* retirement = nullptr;
    std::atomic<bool> armed{false};
    std::atomic<unsigned> calls{0}, destructions{0};
};
struct SmallCleanup {
    std::shared_ptr<CleanupCaptureState> state;
    explicit SmallCleanup(std::shared_ptr<CleanupCaptureState> value) : state(std::move(value)) {}
    SmallCleanup(const SmallCleanup&) noexcept = default;
    SmallCleanup(SmallCleanup&&) noexcept = default;
    ~SmallCleanup() {
        if (state && state->armed.load()) {
            state->subscription->Unsubscribe();
            ++state->destructions;
            state->retirement->Hold();
        }
    }
    void operator()() const { ++state->calls; }
};
static_assert(sizeof(SmallCleanup) == sizeof(std::shared_ptr<CleanupCaptureState>));

struct LivePolicyState {
    std::mutex mutex;
    std::condition_variable cv;
    bool model_entered = false, release_model = false, queue_entered = false, queue_closed = false;
    unsigned queue_next = 0;
    std::size_t queue_bytes = 0;
    std::deque<lubancore::Event> events;
};
class LivePolicyBackend final : public lubancore::Backend {
public:
    explicit LivePolicyBackend(std::shared_ptr<LivePolicyState> state) : state_(std::move(state)) {}
    lubancore::Result<lubancore::ModelReply> Generate(const lubancore::ModelRequest&, lubancore::Cancellation cancel) override {
        std::unique_lock lock(state_->mutex); state_->model_entered = true; state_->cv.notify_all();
        const auto end = std::chrono::steady_clock::now() + 5s;
        while (!state_->release_model && !cancel.requested() && std::chrono::steady_clock::now() < end)
            state_->cv.wait_for(lock, 5ms);
        if (!state_->release_model) return std::unexpected(lubancore::Error{"fixture.model_cancelled", {}});
        return lubancore::ModelReply{"policy callback completed", {}, {}};
    }
private: std::shared_ptr<LivePolicyState> state_;
};
class LivePolicyQueue final : public lubancore::events::v1::EventQueue {
public:
    LivePolicyQueue(std::shared_ptr<LivePolicyState> state, std::size_t capacity) : state_(std::move(state)), capacity_(capacity) {}
    lubancore::Result<void> Push(lubancore::Event event) override {
        std::lock_guard lock(state_->mutex);
        if (state_->queue_closed) return std::unexpected(lubancore::Error{"fixture.queue_closed", {}});
        const auto bytes = Bytes(event);
        if (state_->events.size() >= capacity_ || bytes > 16u * 1024u * 1024u - state_->queue_bytes)
            return std::unexpected(lubancore::Error{"fixture.queue_full", {}});
        state_->queue_bytes += bytes;
        state_->events.push_back(std::move(event)); state_->cv.notify_all(); return {};
    }
    lubancore::Result<std::optional<lubancore::Event>> Next(std::chrono::milliseconds timeout) override {
        std::unique_lock lock(state_->mutex); ++state_->queue_next; state_->queue_entered = true; state_->cv.notify_all();
        state_->cv.wait_for(lock, timeout, [&] { return state_->queue_closed || !state_->events.empty(); });
        if (state_->queue_closed) return std::unexpected(lubancore::Error{"fixture.queue_closed", {}});
        if (state_->events.empty()) return std::optional<lubancore::Event>{};
        auto event = std::move(state_->events.front()); state_->events.pop_front(); state_->queue_bytes -= Bytes(event);
        return std::optional<lubancore::Event>{std::move(event)};
    }
    lubancore::Result<void> Close() override {
        std::lock_guard lock(state_->mutex); state_->queue_closed = true; state_->cv.notify_all(); return {};
    }
private:
    static std::size_t Bytes(const lubancore::Event& event) {
        return event.kind.size() + event.session_id.size() + event.operation_id.size() + event.turn_id.size() +
            event.text.size() + event.payload_json.size(); // This fixture has no approval/tool events.
    }
    std::shared_ptr<LivePolicyState> state_;
    std::size_t capacity_;
};
class LivePolicySink final : public lubancore::events::v1::EventSink {
public:
    explicit LivePolicySink(std::shared_ptr<LivePolicyState> state) : state_(std::move(state)) {}
    lubancore::Result<std::unique_ptr<lubancore::events::v1::EventQueue>> CreateQueue(std::size_t capacity) override {
        return std::unique_ptr<lubancore::events::v1::EventQueue>(std::make_unique<LivePolicyQueue>(state_, capacity));
    }
private: std::shared_ptr<LivePolicyState> state_;
};
struct LivePolicyProbe {
    std::filesystem::path root;
    std::shared_ptr<LivePolicyState> state = std::make_shared<LivePolicyState>();
    std::unique_ptr<lubancore::Runtime> runtime;
    std::shared_ptr<lubancore::Session> session;
    std::shared_ptr<lubancore::EventStream> stream;
    std::string operation;
    std::mutex records_mutex;
    std::vector<std::pair<std::string, std::vector<std::string>>> records;
    static std::string Utf8(const std::filesystem::path& path) {
        const auto text = path.u8string(); return {reinterpret_cast<const char*>(text.data()), text.size()};
    }
    lubancore::SessionOptions Options(bool events = false) const {
        lubancore::SessionOptions options; options.cwd = Utf8(root / "project"); options.model = "policy-lifecycle";
        options.backend = std::make_unique<LivePolicyBackend>(state); options.max_steps_per_turn = 4;
        if (events) options.event_sink = std::make_unique<LivePolicySink>(state);
        return options;
    }
    LivePolicyProbe() {
        static std::atomic<unsigned> serial{0};
        for (unsigned attempt = 0; attempt < 64; ++attempt) {
            auto candidate = std::filesystem::temp_directory_path() / ("sdk-policy-lifecycle-" +
                std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + "-" + std::to_string(++serial));
            if (std::filesystem::create_directory(candidate)) { root = std::move(candidate); break; }
        }
        REQUIRE_FALSE(root.empty());
        std::filesystem::create_directory(root / "project"); std::filesystem::create_directory(root / "resources");
        auto created = lubancore::Runtime::Create({Utf8(root / "state"), Utf8(root / "resources")}); REQUIRE(created);
        runtime = std::move(*created); auto opened = runtime->OpenSession(Options(true)); REQUIRE(opened); session = *opened;
        auto accepted = session->Submit("policy-live", "hold actual model"); REQUIRE(accepted); operation = accepted->operation_id;
        { std::unique_lock lock(state->mutex); REQUIRE(state->cv.wait_for(lock, 3s, [&] { return state->model_entered; })); }
        auto subscribed = session->Subscribe(4096); REQUIRE(subscribed); stream = *subscribed;
    }
    ~LivePolicyProbe() {
        { std::lock_guard lock(state->mutex); state->release_model = true; state->cv.notify_all(); }
        if (runtime) (void)runtime->Shutdown();
        stream.reset(); session.reset(); runtime.reset();
        std::error_code ignored; if (!root.empty()) std::filesystem::remove_all(root, ignored);
    }
    template<class T> static std::string Code(const lubancore::Result<T>& value) { return value ? "allowed" : value.error().code; }
    void Record(const char* phase) {
        std::vector<std::string> codes;
        codes.push_back(Code(runtime->OpenSession(Options())));
        codes.push_back(Code(session->WaitResult(operation, 0ms)));
        // Even a disabled Job wait must refuse the blocking callback boundary,
        // before consulting the module; this does not claim a command was run.
        codes.push_back(Code(session->WaitJob({}, 0ms)));
        codes.push_back(Code(stream->Next(0ms)));
        codes.push_back(Code(stream->CloseChecked()));
        codes.push_back(Code(session->Close()));
        codes.push_back(Code(runtime->Shutdown()));
        std::lock_guard lock(records_mutex); records.emplace_back(phase, std::move(codes));
    }
    void Verify() {
        std::lock_guard lock(records_mutex); REQUIRE_FALSE(records.empty());
        for (const auto& [phase, codes] : records) {
            INFO(phase); REQUIRE(codes.size() == 7);
            for (const auto& code : codes) CHECK(code == "sdk.lifecycle.reentrant");
        }
    }
};
struct DestructionProbe {
    LivePolicyProbe* live = nullptr;
    std::atomic<bool> armed{false};
    std::atomic<unsigned> destructions{0};
};
struct PolicyNoticeCapture {
    std::shared_ptr<DestructionProbe> state;
    explicit PolicyNoticeCapture(std::shared_ptr<DestructionProbe> value) : state(std::move(value)) {}
    PolicyNoticeCapture(const PolicyNoticeCapture&) noexcept = default;
    PolicyNoticeCapture(PolicyNoticeCapture&&) noexcept = default;
    ~PolicyNoticeCapture() {
        if (state && state->armed.load()) { state->live->Record("callback-capture-destruction"); ++state->destructions; }
    }
    void operator()(const auth::PolicyChange&) const { state->live->Record("notification"); }
};
struct PolicyCleanupCapture {
    std::shared_ptr<DestructionProbe> state;
    explicit PolicyCleanupCapture(std::shared_ptr<DestructionProbe> value) : state(std::move(value)) {}
    PolicyCleanupCapture(const PolicyCleanupCapture&) noexcept = default;
    PolicyCleanupCapture(PolicyCleanupCapture&&) noexcept = default;
    ~PolicyCleanupCapture() {
        if (state && state->armed.load()) { state->live->Record("closer-capture-destruction"); ++state->destructions; }
    }
    void operator()() const { state->live->Record("closer-call"); }
};
static_assert(sizeof(PolicyNoticeCapture) == sizeof(std::shared_ptr<DestructionProbe>));
static_assert(sizeof(PolicyCleanupCapture) == sizeof(std::shared_ptr<DestructionProbe>));
class LifecyclePolicy final : public auth::PolicyProvider {
public:
    LivePolicyProbe* live;
    struct Slot { auth::PolicyChangeCallback callback; };
    std::shared_ptr<Slot> slot = std::make_shared<Slot>();
    bool fail = false, destruction = false;
    explicit LifecyclePolicy(LivePolicyProbe& value) : live(&value) {}
    ~LifecyclePolicy() override { if (destruction) live->Record("last-provider-destruction"); }
    lubancore::Result<auth::Decision> Authorize(const auth::ExecutionContext&, auth::Action) const override {
        live->Record("authorize"); if (fail) throw std::runtime_error("private policy failure");
        return auth::Decision{true, 1, "allowed"};
    }
    lubancore::Result<std::unique_ptr<auth::PolicySubscription>> SubscribeChanges(
        const auth::ResourceScope& scope, auth::PolicyChangeCallback callback) override {
        live->Record("subscribe"); if (fail) throw std::runtime_error("private policy subscribe failure");
        callback({scope, 1}); slot->callback.swap(callback);
        return auth::PolicySubscription::Create([owned = slot, target = live] {
            target->Record("provider-closer"); owned->callback = nullptr;
        });
    }
};
void CheckPolicySdkLifecycle() {
    LivePolicyProbe live;
    auto waiting = std::async(std::launch::async, [&] { return live.stream->Next(3s); });
    { std::unique_lock lock(live.state->mutex); REQUIRE(live.state->cv.wait_for(lock, 3s, [&] { return live.state->queue_entered; })); }
    for (const bool invalid : {false, true}) for (const bool throwing : {false, true}) {
        auto provider = std::make_shared<LifecyclePolicy>(live); provider->destruction = true; provider->fail = throwing;
        auto context = Context(); if (invalid) context.request_actor.user_id.clear();
        const auto result = auth::Authorize(std::move(provider), context, auth::Action::ReadOperation);
        CHECK(result.has_value() == (!invalid && !throwing)); CHECK_FALSE(provider);
    }
    for (unsigned mode = 0; mode != 4; ++mode) {
        auto last_provider = std::make_shared<LifecyclePolicy>(live); last_provider->destruction = true;
        last_provider->fail = mode == 1;
        auto scope = first; if (mode == 2) scope.session_id.clear();
        auth::PolicyChangeCallback notice = [](const auth::PolicyChange&) {};
        if (mode == 3) notice = nullptr;
        auto result = auth::SubscribeChanges(std::move(last_provider), scope, std::move(notice));
        CHECK(result.has_value() == (mode == 0)); CHECK_FALSE(last_provider);
        if (result) (*result)->Unsubscribe();
    }
    auto capture = std::make_shared<DestructionProbe>(); capture->live = &live;
    auto provider = std::make_shared<LifecyclePolicy>(live);
    auth::PolicyChangeCallback callback = PolicyNoticeCapture{capture};
    for (const bool invalid : {false, true}) {
        capture->armed = true; provider->fail = !invalid;
        auto scope = first; if (invalid) scope.session_id.clear();
        const auto before = capture->destructions.load();
        const auto refused = auth::SubscribeChanges(provider, scope, callback); REQUIRE_FALSE(refused);
        REQUIRE(capture->destructions.load() > before); capture->armed = false;
    }
    provider->fail = false;
    auto watch = auth::SubscribeChanges(provider, first, callback); REQUIRE(watch);
    callback = nullptr; capture->armed = true;
    auto notified = std::async(std::launch::async, [&] { provider->slot->callback({first, 2}); }); notified.get();
    const auto before = capture->destructions.load();
    auto dropped = std::async(std::launch::async, [&] { provider->slot->callback = nullptr; }); dropped.get();
    REQUIRE(capture->destructions.load() > before); (*watch)->Unsubscribe(); capture->armed = false;
    auto cleanup_state = std::make_shared<DestructionProbe>(); cleanup_state->live = &live;
    std::function<void()> cleanup = PolicyCleanupCapture{cleanup_state};
    auto closer = auth::PolicySubscription::Create(cleanup); REQUIRE(closer);
    cleanup = nullptr; cleanup_state->armed = true; (*closer)->Unsubscribe();
    REQUIRE(cleanup_state->destructions > 0); cleanup_state->armed = false;
    auto reference = Policy();
    callback = PolicyNoticeCapture{capture}; capture->armed = true;
    auto invalid_scope = first; invalid_scope.session_id.clear();
    const auto previous = capture->destructions.load();
    auto invalid = reference->SubscribeChanges(invalid_scope, callback); REQUIRE_FALSE(invalid);
    REQUIRE(capture->destructions > previous); capture->armed = false;
    auto direct = reference->SubscribeChanges(first, callback); REQUIRE(direct); callback = nullptr; capture->armed = true;
    REQUIRE(reference->Grant(alice, first, {auth::Action::ReadOperation}));
    const auto before_reference_close = capture->destructions.load();
    reference.reset(); REQUIRE(capture->destructions > before_reference_close); capture->armed = false; (*direct)->Unsubscribe();
    live.Verify();
    { std::lock_guard lock(live.state->mutex); CHECK(live.state->queue_next == 1); CHECK_FALSE(live.state->queue_closed); }
    const auto still_waiting = live.session->WaitResult(live.operation, 0ms); REQUIRE_FALSE(still_waiting);
    CHECK(still_waiting.error().code == "sdk.wait.timeout");
    const auto no_job = live.session->WaitJob({}, 0ms); REQUIRE_FALSE(no_job); CHECK(no_job.error().code == "sdk.job.disabled");
    { std::lock_guard lock(live.state->mutex); live.state->release_model = true; live.state->cv.notify_all(); }
    const auto completed = live.session->WaitResult(live.operation, 3s); REQUIRE(completed);
    REQUIRE(completed->state == lubancore::OperationState::Succeeded);
    REQUIRE(waiting.wait_for(3s) == std::future_status::ready);
    const auto event = waiting.get(); REQUIRE(event); REQUIRE(event->has_value());
    auto outside = live.runtime->OpenSession(live.Options()); REQUIRE(outside); REQUIRE((*outside)->Close());
    REQUIRE(live.stream->CloseChecked()); REQUIRE(live.session->Close()); REQUIRE(live.runtime->Shutdown());
}
} // namespace

TEST_CASE("SDK authorization: public cleanup captures retire unlocked before concurrent unsubscribe returns") {
    Watchdog watchdog;
    Gate retirement;
    auto state = std::make_shared<CleanupCaptureState>();
    state->retirement = &retirement;
    std::function<void()> cleanup = SmallCleanup(state);
    auto created = auth::PolicySubscription::Create(std::move(cleanup));
    cleanup = nullptr; // Clear a retained moved-from small-functor source before arming.
    REQUIRE(created.has_value());
    auto subscription = std::move(*created);
    state->subscription = subscription.get();
    state->armed.store(true);
    auto first_close = std::async(std::launch::async, [&] { subscription->Unsubscribe(); });
    REQUIRE(retirement.WaitEntered()); // Its capture has already reentered this handle.
    std::promise<void> second_started;
    auto started = second_started.get_future();
    auto second_close = std::async(std::launch::async, [&] {
        second_started.set_value();
        subscription->Unsubscribe();
    });
    REQUIRE(started.wait_for(3s) == std::future_status::ready);
    CHECK(second_close.wait_for(100ms) == std::future_status::timeout);
    retirement.Release();
    REQUIRE(first_close.wait_for(3s) == std::future_status::ready);
    REQUIRE(second_close.wait_for(3s) == std::future_status::ready);
    first_close.get();
    second_close.get();
    CHECK(state->calls.load() == 1);
    CHECK(state->destructions.load() == 1);
    CHECK_FALSE(retirement.timed_out.load());
    std::weak_ptr<CleanupCaptureState> weak = state;
    state.reset();
    CHECK(weak.expired());
    subscription->Unsubscribe();
    CheckPolicySdkLifecycle();
}

namespace {
namespace managed = lubancore::managed::v1;
namespace fs = std::filesystem;
fs::path ManagedNativePath(const fs::path& path) {
#ifdef _WIN32
    const auto text = fs::absolute(path).native();
    if (text.starts_with(L"\\\\?\\")) return path;
    if (text.starts_with(L"\\\\")) return fs::path(L"\\\\?\\UNC\\" + text.substr(2));
    return fs::path(L"\\\\?\\" + text);
#else
    return path;
#endif
}
std::string ManagedUtf8(const fs::path& path) {
    const auto text = path.u8string(); return {reinterpret_cast<const char*>(text.data()), text.size()};
}
std::map<std::string, std::string> ManagedFiles(const fs::path& root) {
    std::map<std::string, std::string> result;
    const auto native = ManagedNativePath(root);
    if (!fs::exists(native)) return result;
    for (const auto& entry : fs::recursive_directory_iterator(native)) {
        const auto name = ManagedUtf8(entry.path().lexically_relative(native));
        if (entry.is_directory()) result[name + "/"] = {};
        else if (entry.is_regular_file()) {
            std::ifstream file(entry.path(), std::ios::binary); REQUIRE(file.is_open());
            result[name] = std::string{std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
            REQUIRE_FALSE(file.bad());
        }
    }
    return result;
}
fs::path ManagedSessionDirectory(const fs::path& root, const std::string& id) {
    fs::path found;
    for (const auto& entry : fs::recursive_directory_iterator(ManagedNativePath(root))) {
        if (!entry.is_directory() || entry.path().filename() != fs::path(id)) continue;
        REQUIRE(found.empty()); found = entry.path();
    }
    REQUIRE_FALSE(found.empty());
    REQUIRE(fs::is_regular_file(found / "managed-session-ownership.json"));
    REQUIRE(fs::is_regular_file(found / (id + ".jsonl")));
    return found;
}
class ManagedProbePolicy final : public auth::PolicyProvider {
public:
    std::shared_ptr<auth::RevocablePolicy> rules = auth::RevocablePolicy::Create();
    std::function<void(const auth::ExecutionContext&, auth::Action)> before;
    std::function<void()> on_destroy;
    mutable std::atomic<unsigned> calls{0};
    bool throwing = false, bad_revision = false, null_subscription = false;
    ~ManagedProbePolicy() override { if (on_destroy) on_destroy(); }
    lubancore::Result<auth::Decision> Authorize(const auth::ExecutionContext& context, auth::Action action) const override {
        ++calls;
        const auto hook = before;
        if (hook) hook(context, action);
        if (throwing) throw std::runtime_error("PRIVATE_MANAGED_POLICY_SECRET");
        if (bad_revision) return auth::Decision{true, 0, "allow"};
        return rules->Authorize(context, action);
    }
    lubancore::Result<std::unique_ptr<auth::PolicySubscription>> SubscribeChanges(
        const auth::ResourceScope& scope, auth::PolicyChangeCallback callback) override {
        if (null_subscription) return std::unique_ptr<auth::PolicySubscription>{};
        return rules->SubscribeChanges(scope, std::move(callback));
    }
};
struct ManagedStorageFixture {
    fs::path root;
    std::unique_ptr<lubancore::Runtime> runtime;
    std::shared_ptr<ManagedProbePolicy> policy = std::make_shared<ManagedProbePolicy>();
    ManagedStorageFixture() {
        static std::atomic<unsigned> serial{0};
        root = fs::temp_directory_path() / ("sdk-mg-" +
            std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + "-" + std::to_string(++serial));
        REQUIRE(fs::create_directory(root));
        fs::create_directory(root / "project"); fs::create_directory(root / "resources");
        auto made = lubancore::Runtime::Create({ManagedUtf8(root / "state"), ManagedUtf8(root / "resources")}); REQUIRE(made);
        runtime = std::move(*made);
    }
    ~ManagedStorageFixture() {
        policy->before = {}; policy->on_destroy = {};
        if (runtime) { (void)runtime->Shutdown(); runtime.reset(); }
        std::error_code ignored; fs::remove_all(ManagedNativePath(root), ignored);
    }
    std::shared_ptr<managed::Project> Project(std::string tenant = "tenant-a", std::string name = "same-project") {
        managed::ProjectOptions options;
        options.binding = {std::move(tenant), std::move(name), {}, 1};
        options.cwd = ManagedUtf8(root / "project"); options.policy = policy;
        auto made = runtime->RegisterManagedProject(std::move(options)); REQUIRE(made);
        REQUIRE(policy->rules->BindProject((*made)->binding()));
        return *made;
    }
    auth::ResourceScope Scope(const std::shared_ptr<managed::Project>& project, std::string id = {}) const {
        const auto binding = project->binding();
        return {binding.tenant_id, binding.project_id, binding.workspace_key, std::move(id)};
    }
    std::string Open(const std::shared_ptr<managed::Project>& project, auth::AuthenticatedSubject subject = alice) {
        REQUIRE(policy->rules->Grant(subject, Scope(project), {auth::Action::OpenSession}));
        const auto opened = runtime->OpenManagedSession(project, subject);
        REQUIRE_MESSAGE(opened.has_value(), (opened ? "" : opened.error().code));
        return opened->session_id;
    }
    std::shared_ptr<managed::View> View(const std::shared_ptr<managed::Project>& project, const std::string& id) {
        REQUIRE(policy->rules->Grant(alice, Scope(project, id),
            {auth::Action::AcquireView, auth::Action::ReadSession, auth::Action::CloseSession}));
        auto view = runtime->AcquireManagedView(project, alice, id); REQUIRE(view);
        return *view;
    }
};

void CheckManagedStorageLifecycle() {
    // A binding/version cannot replace its Policy after all public project and
    // closed View handles have gone. Registration lasts until real Shutdown.
    {
        ManagedStorageFixture registration;
        auto registered = registration.Project();
        const auto binding = registered->binding();
        registered.reset();
        const auto replacement = [&](const std::shared_ptr<auth::PolicyProvider>& policy) {
            return registration.runtime->RegisterManagedProject(
                {binding, ManagedUtf8(registration.root / "project"), policy});
        };
        auto conflict = replacement(auth::RevocablePolicy::Create());
        REQUIRE_FALSE(conflict);
        REQUIRE(conflict.error().code == "sdk.managed.project_conflict");
        auto same = replacement(registration.policy); REQUIRE(same);
        registered = std::move(*same);
        REQUIRE(registered->binding() == binding);
        const auto id = registration.Open(registered);
        auto closed_view = registration.View(registered, id);
        REQUIRE(closed_view->Close());
        closed_view.reset(); registered.reset();
        conflict = replacement(auth::RevocablePolicy::Create());
        REQUIRE_FALSE(conflict);
        REQUIRE(conflict.error().code == "sdk.managed.project_conflict");
        same = replacement(registration.policy); REQUIRE(same);
        REQUIRE((*same)->binding() == binding);
        REQUIRE(registration.runtime->Shutdown());
    }
    {
        ManagedStorageFixture retirement;
        std::string retired_code;
        auto retained_policy = std::make_shared<ManagedProbePolicy>();
        const std::weak_ptr<ManagedProbePolicy> weak_policy = retained_policy;
        retained_policy->on_destroy = [&] {
            const auto closed = retirement.runtime->Shutdown();
            retired_code = closed ? "unexpected" : closed.error().code;
        };
        auto registered = retirement.runtime->RegisterManagedProject({
            {"tenant-a", "retained-project", {}, 1},
            ManagedUtf8(retirement.root / "project"), retained_policy});
        REQUIRE(registered);
        registered = std::unexpected(lubancore::Error{"released", {}});
        retained_policy.reset();
        REQUIRE_FALSE(weak_policy.expired());
        REQUIRE(retirement.runtime->Shutdown());
        REQUIRE(weak_policy.expired());
        REQUIRE(retired_code == "sdk.lifecycle.reentrant");
    }
    ManagedStorageFixture fixture;
    auto project = fixture.Project();
    const auto before = ManagedFiles(fixture.root / "state");
    const auto denied = fixture.runtime->OpenManagedSession(project, alice);
    REQUIRE_FALSE(denied); REQUIRE(denied.error().code == "sdk.authorization.denied");
    REQUIRE(ManagedFiles(fixture.root / "state") == before);
    managed::ProjectOptions malformed_project{{"tenant-a", std::string(1, static_cast<char>(0xff)), {}, 1},
        ManagedUtf8(fixture.root / "project"), fixture.policy};
    const auto bad_project = fixture.runtime->RegisterManagedProject(std::move(malformed_project));
    REQUIRE_FALSE(bad_project); REQUIRE(bad_project.error().code == "sdk.managed.project_invalid");
    REQUIRE(ManagedFiles(fixture.root / "state") == before);
    auto bad = alice; bad.credential_id = std::string(1, static_cast<char>(0xff));
    const auto calls = fixture.policy->calls.load();
    const auto invalid = fixture.runtime->OpenManagedSession(project, bad);
    REQUIRE_FALSE(invalid); REQUIRE(invalid.error().code == "sdk.authorization.subject_invalid");
    REQUIRE(fixture.policy->calls.load() == calls);
    auto foreign = alice; foreign.tenant_id = "tenant-b";
    REQUIRE_FALSE(fixture.runtime->OpenManagedSession(project, foreign));
    REQUIRE(ManagedFiles(fixture.root / "state") == before);

    REQUIRE(fixture.policy->rules->Grant(alice, fixture.Scope(project), {auth::Action::OpenSession}));
    for (unsigned fault = 0; fault != 3; ++fault) {
        fixture.policy->throwing = fault == 0;
        fixture.policy->bad_revision = fault == 1;
        fixture.policy->null_subscription = fault == 2;
        const auto failed = fixture.runtime->OpenManagedSession(project, alice);
        REQUIRE_FALSE(failed);
        REQUIRE(failed.error().message.find("PRIVATE_MANAGED_POLICY_SECRET") == std::string::npos);
        REQUIRE(ManagedFiles(fixture.root / "state") == before);
    }
    fixture.policy->throwing = false; fixture.policy->bad_revision = false; fixture.policy->null_subscription = false;
    const auto id = fixture.Open(project);
    const auto directory = ManagedSessionDirectory(fixture.root / "state", id);
    REQUIRE(fs::exists(directory / "session.lock"));
    REQUIRE_FALSE(fixture.runtime->AcquireManagedView(project, alice, id));
    REQUIRE(fixture.policy->rules->Grant(alice, fixture.Scope(project, id), {auth::Action::AcquireView}));
    auto acquired = fixture.runtime->AcquireManagedView(project, alice, id); REQUIRE(acquired);
    auto view = *acquired;
    REQUIRE_FALSE(view->ReadIdentity()); REQUIRE_FALSE(view->Close());
    REQUIRE(fixture.policy->rules->Grant(alice, fixture.Scope(project, id), {auth::Action::ReadSession, auth::Action::CloseSession}));
    auto identity = view->ReadIdentity(); REQUIRE(identity);
    REQUIRE(identity->resource == fixture.Scope(project, id));
    REQUIRE(identity->project_binding_version == project->binding().version);
    REQUIRE(identity->creation_subject == alice);
    REQUIRE(identity->opening_policy_revision != 0); REQUIRE(identity->run_id == "main-0001");
    REQUIRE(identity->state == managed::StorageState::Open);
    const auto opening_revision = identity->opening_policy_revision;
    acquired = std::unexpected(lubancore::Error{"released", {}}); view.reset();
    REQUIRE(fs::exists(directory / "session.lock")); // Runtime, not a View, holds the actual storage owner.
    auto again = fixture.runtime->AcquireManagedView(project, alice, id); REQUIRE(again); view = *again;

    const auto other_id = fixture.Open(project);
    auto other_view = fixture.View(project, other_id);
    const auto other_directory = ManagedSessionDirectory(fixture.root / "state", other_id);
    auto other_project = fixture.Project("tenant-a", "other-project");
    const auto project_id = fixture.Open(other_project);
    auto project_view = fixture.View(other_project, project_id);
    const auto project_directory = ManagedSessionDirectory(fixture.root / "state", project_id);
    REQUIRE(other_directory.parent_path() == directory.parent_path());
    REQUIRE(project_directory.parent_path() != directory.parent_path());
    REQUIRE(fixture.policy->rules->Grant(alice, fixture.Scope(other_project, id), {auth::Action::AcquireView}));
    const auto wrong_project = fixture.runtime->AcquireManagedView(other_project, alice, id);
    REQUIRE_FALSE(wrong_project); REQUIRE(wrong_project.error().code == "sdk.managed.session_not_found");
    auto tenant_project = fixture.Project("tenant-b");
    const auth::AuthenticatedSubject tenant_actor{"tenant-b", "alice", auth::ActorKind::User, "credential-a"};
    const auto tenant_id = fixture.Open(tenant_project, tenant_actor);
    const auto tenant_directory = ManagedSessionDirectory(fixture.root / "state", tenant_id);
    REQUIRE(tenant_directory.parent_path() != directory.parent_path());
    REQUIRE_FALSE(fixture.runtime->AcquireManagedView(tenant_project, alice, tenant_id));

    auto other_runtime = lubancore::Runtime::Create({ManagedUtf8(fixture.root / "other-state"), ManagedUtf8(fixture.root / "resources")});
    REQUIRE(other_runtime);
    const auto wrong_runtime = (*other_runtime)->OpenManagedSession(project, alice);
    REQUIRE_FALSE(wrong_runtime); REQUIRE(wrong_runtime.error().code == "sdk.managed.project_foreign");
    REQUIRE((*other_runtime)->Shutdown());
    managed::ProjectOptions conflicting{project->binding(), ManagedUtf8(fixture.root / "project"), auth::RevocablePolicy::Create()};
    const auto conflict = fixture.runtime->RegisterManagedProject(std::move(conflicting));
    REQUIRE_FALSE(conflict); REQUIRE(conflict.error().code == "sdk.managed.project_conflict");

    std::vector<std::string> reentry;
    fixture.policy->before = [&](const auth::ExecutionContext&, auth::Action action) {
        if (action != auth::Action::ReadSession) return;
        const auto nested_read = view->ReadIdentity(); const auto nested_close = view->Close();
        const auto shutdown = fixture.runtime->Shutdown();
        reentry.push_back(nested_read ? "unexpected" : nested_read.error().code);
        reentry.push_back(nested_close ? "unexpected" : nested_close.error().code);
        reentry.push_back(shutdown ? "unexpected" : shutdown.error().code);
    };
    REQUIRE(view->ReadIdentity());
    fixture.policy->before = {};
    REQUIRE(reentry.size() == 6);
    REQUIRE(std::all_of(reentry.begin(), reentry.end(), [](const auto& code) { return code == "sdk.lifecycle.reentrant"; }));
    unsigned reads = 0;
    fixture.policy->before = [&](const auth::ExecutionContext&, auth::Action action) {
        if (action == auth::Action::ReadSession && ++reads == 2)
            (void)fixture.policy->rules->Revoke(alice, fixture.Scope(project, id), {auth::Action::ReadSession});
    };
    REQUIRE_FALSE(view->ReadIdentity()); // Permission withdrawn after candidate snapshot, before publication.
    fixture.policy->before = {};
    REQUIRE(fixture.policy->rules->Revoke(alice, fixture.Scope(project, id), {auth::Action::CloseSession}));
    REQUIRE_FALSE(view->Close()); REQUIRE(fs::exists(directory / "session.lock"));
    REQUIRE(fixture.policy->rules->Grant(alice, fixture.Scope(project, id), {auth::Action::ReadSession, auth::Action::CloseSession}));
    REQUIRE(view->Close()); REQUIRE_FALSE(fs::exists(directory / "session.lock"));
    REQUIRE(fs::exists(other_directory / "session.lock"));
    REQUIRE(fs::exists(project_directory / "session.lock"));
    identity = view->ReadIdentity(); REQUIRE(identity);
    REQUIRE(identity->state == managed::StorageState::Closed);
    REQUIRE(identity->opening_policy_revision == opening_revision);
    REQUIRE(identity->creation_subject == alice);
    REQUIRE_FALSE(fixture.runtime->AcquireManagedView(project, alice, id)); // Closed supervisor entry retired.
    REQUIRE(fixture.policy->rules->Revoke(alice, fixture.Scope(project, id), {auth::Action::ReadSession, auth::Action::CloseSession}));
    REQUIRE_FALSE(view->ReadIdentity()); REQUIRE_FALSE(view->Close());
    REQUIRE(fixture.runtime->Shutdown());
    REQUIRE_FALSE(fs::exists(other_directory / "session.lock"));
    REQUIRE_FALSE(fs::exists(project_directory / "session.lock"));
    REQUIRE_FALSE(fs::exists(tenant_directory / "session.lock"));
    auto after_shutdown = other_view->ReadIdentity(); REQUIRE(after_shutdown);
    REQUIRE(after_shutdown->state == managed::StorageState::Closed);

    // Parameter/provider cleanup on an invalid bootstrap remains in Policy scope.
    ManagedStorageFixture cleanup;
    std::string retired_code;
    auto retiring = std::make_shared<ManagedProbePolicy>();
    retiring->on_destroy = [&] {
        const auto closed = cleanup.runtime->Shutdown();
        retired_code = closed ? "unexpected" : closed.error().code;
    };
    managed::ProjectOptions rejected_options{{"", "project", {}, 1}, ManagedUtf8(cleanup.root / "project"), retiring};
    retiring.reset();
    REQUIRE_FALSE(cleanup.runtime->RegisterManagedProject(std::move(rejected_options)));
    REQUIRE(retired_code == "sdk.lifecycle.reentrant");

    // A real pending authorization/opening pins Runtime cleanup until it exits.
    ManagedStorageFixture race;
    auto race_project = race.Project();
    REQUIRE(race.policy->rules->Grant(alice, race.Scope(race_project), {auth::Action::OpenSession}));
    Gate authorization;
    std::atomic<unsigned> openings{0};
    race.policy->before = [&](const auth::ExecutionContext&, auth::Action action) {
        if (action == auth::Action::OpenSession && ++openings == 2) authorization.Hold();
    };
    auto opening = std::async(std::launch::async, [&] { return race.runtime->OpenManagedSession(race_project, alice); });
    REQUIRE(authorization.WaitEntered());
    auto shutting_down = std::async(std::launch::async, [&] { return race.runtime->Shutdown(); });
    // Observe the actual Runtime closed gate, not a sleep that merely assumes
    // the Shutdown thread has already run. The probe cannot reach paths/Policy.
    bool shutdown_admitted = false;
    const auto deadline = std::chrono::steady_clock::now() + 3s;
    while (!shutdown_admitted && std::chrono::steady_clock::now() < deadline) {
        const auto probe = race.runtime->OpenManagedSession({}, alice);
        shutdown_admitted = !probe && probe.error().code == "sdk.runtime.closed";
        if (!shutdown_admitted) std::this_thread::yield();
    }
    const auto waiting = shutting_down.wait_for(0ms);
    authorization.Release();
    REQUIRE(shutdown_admitted);
    REQUIRE(waiting == std::future_status::timeout);
    REQUIRE(opening.wait_for(3s) == std::future_status::ready);
    REQUIRE_FALSE(opening.get());
    REQUIRE(shutting_down.wait_for(3s) == std::future_status::ready); REQUIRE(shutting_down.get());
    REQUIRE_FALSE(authorization.timed_out.load());
    race.policy->before = {};
    REQUIRE(ManagedFiles(race.root / "state").size() > 0); // Trusted bootstrap exists, no Session was published.
    for (const auto& entry : fs::recursive_directory_iterator(ManagedNativePath(race.root / "state")))
        REQUIRE(entry.path().filename() != fs::path("managed-session-ownership.json"));
}
} // namespace
