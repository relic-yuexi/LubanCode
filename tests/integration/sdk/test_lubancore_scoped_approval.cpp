#include <doctest/doctest.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <type_traits>

#include "lubancore/core.hpp"
#include "runtime/scoped_approval.hpp"
#include "sdk/approval.hpp"
#include "tools/path_utils.hpp"

namespace {
using namespace std::chrono_literals;
namespace sdk = lubancore;
namespace rt = lubancode::runtime;
namespace fs = std::filesystem;
using Host = sdk::detail::SessionApprovals;
using Owner = sdk::detail::ScopedApprovalOwner;

class Watchdog {
public:
    Watchdog() : thread_([this] {
        std::unique_lock lock(mutex_);
        if (!cv_.wait_for(lock, 30s, [&] { return done_; })) std::abort();
    }) {}
    ~Watchdog() {
        { std::lock_guard lock(mutex_); done_ = true; }
        cv_.notify_all();
        thread_.join();
    }
private:
    std::mutex mutex_;
    std::condition_variable cv_;
    bool done_ = false;
    std::thread thread_;
};
struct Cleanup {
    std::function<void()> function;
    ~Cleanup() { function(); }
};
struct Ready final : rt::ScopedApprovalFuture {
    std::optional<rt::ApprovalResponse> WaitApproval() override { return rt::ApprovalResponse{}; }
    std::optional<rt::ApprovalResponse> WaitApproval(const std::atomic<bool>*) override { return WaitApproval(); }
    std::optional<rt::QuestionResponse> WaitQuestion() override { return std::nullopt; }
};
Owner Scope() {
    return {"parent", "op-1", "child", "run-child-1", "turn-child-1", "act-child-1",
            lubancode::tools::PathToUtf8(fs::temp_directory_path())};
}
sdk::Approval Ticket(std::string id, const Owner& scope = Scope()) {
    sdk::Approval result;
    result.request_id = std::move(id);
    result.operation_id = scope.parent_operation_id;
    result.tool_call_id = "provider-call-1";
    result.tool_name = "guarded";
    result.input_json = "{}";
    result.cwd = scope.effective_cwd;
    return result;
}
void Bind(Host& host) { host.SetOperationOwner("parent", "op-1"); }
rt::ApprovalResponse Reply(rt::InteractionDecision decision) { return {decision, "fixture"}; }
sdk::Result<rt::ApprovalLease> Register(Host& host, std::string id,
                                     std::chrono::milliseconds timeout = 10s) {
    return host.RegisterScoped(Scope(), Ticket(std::move(id)), timeout, [](const auto&, const auto&) {});
}

struct Paths {
    fs::path root;
    Paths() {
        static std::atomic<int> serial{0};
        root = fs::temp_directory_path() / ("sdk-scoped-approval-" +
            std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + "-" +
            std::to_string(++serial));
        fs::create_directories(root / "cwd");
        fs::create_directories(root / "resources");
    }
    ~Paths() { std::error_code ec; fs::remove_all(root, ec); }
    std::string Utf8(const fs::path& path) const { return lubancode::tools::PathToUtf8(path); }
    sdk::RuntimeOptions Roots() const { return {Utf8(root / "data"), Utf8(root / "resources")}; }
};
class ToolBackend final : public sdk::Backend {
public:
    explicit ToolBackend(std::shared_ptr<std::atomic<int>> calls) : calls_(std::move(calls)) {}
    sdk::Result<sdk::ModelReply> Generate(const sdk::ModelRequest&, sdk::Cancellation) override {
        const int call = calls_->fetch_add(1);
        if (call % 2 == 0)
            return sdk::ModelReply{"", {{"guarded-" + std::to_string(call), "guarded", "{}"}}};
        return sdk::ModelReply{"approval complete"};
    }
private:
    std::shared_ptr<std::atomic<int>> calls_;
};
sdk::SessionOptions Options(const Paths& paths, std::shared_ptr<std::atomic<int>> models,
                            std::shared_ptr<std::atomic<int>> tools,
                            std::chrono::milliseconds timeout = 10s) {
    sdk::SessionOptions result;
    result.cwd = paths.Utf8(paths.root / "cwd");
    result.model = "approval-fixture";
    result.system_prompt = "approval fixture";
    result.backend = std::make_unique<ToolBackend>(std::move(models));
    result.approval_timeout = timeout;
    result.max_steps_per_turn = 4;
    sdk::Tool tool;
    tool.name = "guarded";
    tool.description = "Count real approved execution";
    tool.execute = [tools](const std::string&, const sdk::ToolContext&) -> sdk::Result<sdk::ToolResult> {
        ++*tools;
        return sdk::ToolResult{"guarded executed", false};
    };
    result.custom_tools.push_back(std::move(tool));
    return result;
}
sdk::Approval ApprovalEvent(const std::shared_ptr<sdk::EventStream>& stream, const std::string& operation) {
    const auto deadline = std::chrono::steady_clock::now() + 10s;
    while (std::chrono::steady_clock::now() < deadline) {
        auto event = stream->Next(100ms);
        REQUIRE(event.has_value());
        if (*event && (*event)->kind == "approval_requested" && (*event)->operation_id == operation) {
            REQUIRE((*event)->approval.has_value());
            return *(*event)->approval;
        }
    }
    FAIL("real SDK approval event missing");
    return {};
}
} // namespace

TEST_CASE("Scoped approval lease: invalid capabilities refuse and moves retire exactly once") {
    static_assert(!std::is_copy_constructible_v<rt::ApprovalLease>);
    static_assert(std::is_nothrow_move_constructible_v<rt::ApprovalLease>);
    auto ready = std::make_shared<Ready>();
    CHECK_FALSE(static_cast<bool>(rt::ApprovalLease::Create(nullptr, [] {})));
    CHECK_FALSE(static_cast<bool>(rt::ApprovalLease::Create(ready, {})));
    int retired = 0;
    auto lease = rt::ApprovalLease::Create(ready, [&] { ++retired; });
    REQUIRE(static_cast<bool>(lease));
    auto moved = std::move(lease);
    CHECK_FALSE(static_cast<bool>(lease));
    auto future = moved.Future();
    moved.Retire();
    moved.Retire();
    CHECK_FALSE(static_cast<bool>(moved));
    CHECK(retired == 1);
    REQUIRE(future->WaitApproval().has_value());
}

TEST_CASE("Scoped approval lease: small cleanup capture retires and reenters outside locks") {
    Watchdog watchdog;
    rt::ApprovalLease lease;
    bool armed = false;
    int calls = 0, destructors = 0;
    struct SmallCleanup {
        rt::ApprovalLease* lease;
        bool* armed;
        int* destructors;
        ~SmallCleanup() { if (*armed) { ++*destructors; lease->Retire(); } }
    };
    SmallCleanup capture{&lease, &armed, &destructors};
    std::function<void()> function = [capture, &calls] { ++calls; };
    lease = rt::ApprovalLease::Create(std::make_shared<Ready>(), std::move(function));
    function = {};
    armed = true;
    lease.Retire();
    armed = false;
    CHECK(calls == 1);
    CHECK(destructors == 1);
}

TEST_CASE("Scoped approval host: missing and foreign owners fail before publishing") {
    Host host;
    int published = 0;
    auto publish = [&](const auto&, const auto&) { ++published; };
    auto scope = Scope();
    CHECK_FALSE(host.RegisterScoped(scope, Ticket("unbound"), 10s, publish).has_value());
    Bind(host);
    for (int field = 0; field < 7; ++field) {
        auto bad = scope;
        std::string* fields[] = {&bad.parent_session_id, &bad.parent_operation_id, &bad.child_session_id,
            &bad.child_run_id, &bad.child_turn_id, &bad.child_declared_action_id, &bad.effective_cwd};
        fields[field]->clear();
        CHECK_FALSE(host.RegisterScoped(bad, Ticket("missing-" + std::to_string(field), bad), 10s, publish).has_value());
    }
    auto foreign = scope;
    foreign.parent_session_id = "another-parent";
    CHECK_FALSE(host.RegisterScoped(foreign, Ticket("foreign", foreign), 10s, publish).has_value());
    CHECK_FALSE(host.RegisterScoped(scope, Ticket("no-publisher"), 10s, {}).has_value());
    CHECK_FALSE(host.RegisterScoped(scope, Ticket("no-timeout"), 0ms, publish).has_value());
    CHECK(published == 0);
    CHECK(host.Pending().empty());
}

TEST_CASE("Scoped approval host: unsupported session grant keeps ticket answerable") {
    Host host;
    Bind(host);
    auto lease = Register(host, "scope-grant");
    REQUIRE(lease.has_value());
    CHECK_FALSE(host.Resolve("scope-grant", Reply(rt::InteractionDecision::AcceptForSession)));
    REQUIRE(host.Pending().size() == 1);
    CHECK(host.AllowedTools().empty());
    REQUIRE(host.Resolve("scope-grant", Reply(rt::InteractionDecision::Accept)));
    const auto answer = lease->Future()->WaitApproval(nullptr);
    REQUIRE(answer.has_value());
    CHECK(answer->decision == rt::InteractionDecision::Accept);
    CHECK_FALSE(host.Resolve("scope-grant", Reply(rt::InteractionDecision::Accept)));
    CHECK(host.Pending().empty());
    CHECK(host.AllowedTools().empty());
}

TEST_CASE("Scoped approval host: three supported decisions resolve without granting parent tools") {
    Host host;
    Bind(host);
    for (auto decision : {rt::InteractionDecision::Accept, rt::InteractionDecision::Decline,
                          rt::InteractionDecision::Cancel}) {
        const auto id = "decision-" + std::to_string(static_cast<int>(decision));
        auto lease = Register(host, id);
        REQUIRE(lease.has_value());
        REQUIRE(host.Resolve(id, Reply(decision)));
        auto response = lease->Future()->WaitApproval();
        REQUIRE(response.has_value());
        CHECK(response->decision == decision);
        CHECK(host.Pending().empty());
        CHECK(host.AllowedTools().empty());
    }
}

TEST_CASE("Scoped approval host: deadline belongs to registration and late replies cannot win") {
    Host host;
    Bind(host);
    auto lease = Register(host, "expired-before-wait", 40ms);
    REQUIRE(lease.has_value());
    std::this_thread::sleep_for(80ms);
    CHECK_FALSE(host.Resolve("expired-before-wait", Reply(rt::InteractionDecision::Accept)));
    CHECK_FALSE(lease->Future()->WaitApproval().has_value());
    CHECK(host.Pending().empty());
    auto unobserved = Register(host, "expired-no-resolver", 40ms);
    REQUIRE(unobserved.has_value());
    std::this_thread::sleep_for(80ms);
    CHECK_FALSE(unobserved->Future()->WaitApproval().has_value());
    CHECK_FALSE(host.Resolve("expired-no-resolver", Reply(rt::InteractionDecision::Accept)));
}

TEST_CASE("Scoped approval host: borrowed cancel retires one ticket and preserves its peer") {
    Watchdog watchdog;
    Host host;
    Bind(host);
    auto first = Register(host, "cancel-local");
    auto second = Register(host, "cancel-peer");
    REQUIRE(first.has_value());
    REQUIRE(second.has_value());
    std::atomic<bool> cancel{false};
    auto future = first->Future();
    auto waiter = std::async(std::launch::async, [&] { return future->WaitApproval(&cancel); });
    Cleanup release{[&] { cancel.store(true); host.CancelAll(); }};
    cancel.store(true);
    REQUIRE(waiter.wait_for(2s) == std::future_status::ready);
    CHECK_FALSE(waiter.get().has_value());
    REQUIRE(host.Pending().size() == 1);
    CHECK(host.Pending().front().request_id == "cancel-peer");
    CHECK_FALSE(host.Resolve("cancel-local", Reply(rt::InteractionDecision::Accept)));
    REQUIRE(host.Resolve("cancel-peer", Reply(rt::InteractionDecision::Accept)));
    REQUIRE(second->Future()->WaitApproval().has_value());
    // Setting cancellation after a completed answer cannot rewrite its terminal
    // receipt. The later child adapter must still check its flag before started.
    REQUIRE(second->Future()->WaitApproval(&cancel).has_value());
}

TEST_CASE("Scoped approval host: publisher reentry and failure retire only the new ticket") {
    Watchdog watchdog;
    Host host;
    Bind(host);
    auto peer = Register(host, "publisher-peer");
    REQUIRE(peer.has_value());
    auto failed = host.RegisterScoped(Scope(), Ticket("publisher-throws"), 10s,
        [&](const auto&, const auto&) {
            CHECK(host.Pending().size() == 2);
            throw std::runtime_error("publisher fixture");
        });
    REQUIRE_FALSE(failed.has_value());
    CHECK(failed.error().code == "sdk.approval.publish_failed");
    REQUIRE(host.Pending().size() == 1);
    CHECK(host.Pending().front().request_id == "publisher-peer");
    CHECK_FALSE(host.Resolve("publisher-throws", Reply(rt::InteractionDecision::Accept)));
    auto answered = host.RegisterScoped(Scope(), Ticket("publisher-reentrant"), 10s,
        [&](const auto& ticket, const auto&) { REQUIRE(host.Resolve(ticket.request_id, Reply(rt::InteractionDecision::Accept))); });
    REQUIRE(answered.has_value());
    REQUIRE(answered->Future()->WaitApproval().has_value());
}

TEST_CASE("Scoped approval host: duplicate rejection and stale leases cannot retire another ticket") {
    Host host;
    Bind(host);
    auto first = Register(host, "unique-id");
    REQUIRE(first.has_value());
    auto duplicate = Register(host, "unique-id");
    REQUIRE_FALSE(duplicate.has_value());
    CHECK(duplicate.error().code == "sdk.approval.duplicate_request_id");
    REQUIRE(host.Pending().size() == 1);
    bool ordinary_registered = true;
    auto ordinary_collision = host.Register(Ticket("unique-id"), 10s, nullptr, &ordinary_registered);
    CHECK_FALSE(ordinary_registered);
    CHECK_FALSE(ordinary_collision->WaitApproval().has_value());
    REQUIRE(host.Pending().size() == 1);
    REQUIRE(host.Resolve("unique-id", Reply(rt::InteractionDecision::Accept)));
    REQUIRE(first->Future()->WaitApproval().has_value());
    CHECK_FALSE(Register(host, "unique-id").has_value());
    first->Retire();
    CHECK(host.Pending().empty());
    auto ordinary = host.Register(Ticket("ordinary-owned"), 10s);
    CHECK_FALSE(Register(host, "ordinary-owned").has_value());
    REQUIRE(host.Pending().size() == 1);
    REQUIRE(host.Resolve("ordinary-owned", Reply(rt::InteractionDecision::Accept)));
    REQUIRE(ordinary->WaitApproval().has_value());
    auto reused = host.Register(Ticket("unique-id"), 10s, nullptr, &ordinary_registered);
    CHECK_FALSE(ordinary_registered);
    CHECK_FALSE(reused->WaitApproval().has_value());
}

TEST_CASE("Scoped approval host: answer and retirement have one terminal winner") {
    Watchdog watchdog;
    Host host;
    Bind(host);
    auto lease = Register(host, "answer-retire");
    REQUIRE(lease.has_value());
    auto future = lease->Future();
    std::promise<void> gate;
    auto start = gate.get_future().share();
    std::future<bool> answer;
    std::future<void> retire;
    bool released = false;
    Cleanup release{[&] { if (!released) { released = true; gate.set_value(); } host.CancelAll(); }};
    answer = std::async(std::launch::async, [&] { start.wait(); return host.Resolve("answer-retire", Reply(rt::InteractionDecision::Accept)); });
    retire = std::async(std::launch::async, [&] { start.wait(); lease->Retire(); });
    released = true;
    gate.set_value();
    REQUIRE(answer.wait_for(2s) == std::future_status::ready);
    REQUIRE(retire.wait_for(2s) == std::future_status::ready);
    const bool accepted = answer.get();
    retire.get();
    auto response = future->WaitApproval();
    CHECK(response.has_value() == accepted);
    if (response) CHECK(response->decision == rt::InteractionDecision::Accept);
    CHECK(host.Pending().empty());
    CHECK_FALSE(host.Resolve("answer-retire", Reply(rt::InteractionDecision::Accept)));
}

TEST_CASE("Scoped approval host: copied scope and late future outlive host without owning it") {
    std::shared_ptr<rt::ScopedApprovalFuture> future;
    rt::ApprovalLease lease;
    Owner published;
    {
        Host host;
        Bind(host);
        auto owner = Scope();
        auto opened = host.RegisterScoped(owner, Ticket("owned-values", owner), 10s,
            [&](const auto&, const auto& value) { published = value; });
        REQUIRE(opened.has_value());
        owner.child_session_id = "changed";
        CHECK(published.child_session_id == "child");
        CHECK(published.parent_operation_id == "op-1");
        CHECK(published.child_declared_action_id == "act-child-1");
        lease = std::move(*opened);
        future = lease.Future();
        host.Close();
        CHECK_FALSE(Register(host, "after-close").has_value());
        CHECK_FALSE(host.Resolve("owned-values", Reply(rt::InteractionDecision::Accept)));
    }
    CHECK_FALSE(future->WaitApproval().has_value());
    lease.Retire();
    CHECK_FALSE(future->WaitApproval().has_value());
}

TEST_CASE("Scoped approval host: ordinary four-state timeout and capability semantics remain unchanged") {
    Host host;
    for (auto decision : {rt::InteractionDecision::Accept, rt::InteractionDecision::AcceptForSession,
                          rt::InteractionDecision::Decline, rt::InteractionDecision::Cancel}) {
        auto id = "ordinary-" + std::to_string(static_cast<int>(decision));
        auto future = host.Register(Ticket(id), 1s);
        CHECK_FALSE(std::dynamic_pointer_cast<rt::ScopedApprovalFuture>(future));
        REQUIRE(host.Resolve(id, Reply(decision)));
        const auto answer = future->WaitApproval();
        REQUIRE(answer.has_value());
        CHECK(answer->decision == decision);
        CHECK_FALSE(host.Resolve(id, Reply(decision)));
    }
    CHECK(host.AllowedTools().contains("guarded"));
    auto delayed = host.Register(Ticket("ordinary-delay"), 30ms);
    std::this_thread::sleep_for(60ms);
    REQUIRE(host.Resolve("ordinary-delay", Reply(rt::InteractionDecision::Accept)));
    REQUIRE(delayed->WaitApproval().has_value());
    auto expired = host.Register(Ticket("ordinary-timeout"), 20ms);
    CHECK_FALSE(expired->WaitApproval().has_value());
    CHECK_FALSE(host.Resolve("ordinary-timeout", Reply(rt::InteractionDecision::AcceptForSession)));
    CHECK_FALSE(Host::CancelledFuture()->WaitQuestion().has_value());
}

TEST_CASE("Scoped approval SDK: real public session retains ordinary session allowance across turns") {
    Paths paths;
    auto runtime = sdk::Runtime::Create(paths.Roots());
    REQUIRE(runtime.has_value());
    auto models = std::make_shared<std::atomic<int>>(0);
    auto tools = std::make_shared<std::atomic<int>>(0);
    auto session = (*runtime)->OpenSession(Options(paths, models, tools));
    REQUIRE(session.has_value());
    auto events = (*session)->Subscribe();
    REQUIRE(events.has_value());
    auto first = (*session)->Submit("allow-once", "first");
    REQUIRE(first.has_value());
    const auto ticket = ApprovalEvent(*events, first->operation_id);
    CHECK(ticket.operation_id == first->operation_id);
    REQUIRE((*session)->ResolveApproval(ticket.request_id, sdk::ApprovalDecision::AcceptForSession).has_value());
    auto completed = (*session)->WaitResult(first->operation_id, 15s);
    REQUIRE(completed.has_value());
    CHECK(completed->state == sdk::OperationState::Succeeded);
    auto second = (*session)->Submit("allowed-again", "second");
    REQUIRE(second.has_value());
    auto reused = (*session)->WaitResult(second->operation_id, 15s);
    REQUIRE(reused.has_value());
    CHECK(reused->state == sdk::OperationState::Succeeded);
    CHECK(tools->load() == 2);
    CHECK(models->load() == 4);
    CHECK((*session)->PendingApprovals().empty());
    CHECK_FALSE((*session)->ResolveApproval(ticket.request_id, sdk::ApprovalDecision::Accept).has_value());
    REQUIRE((*session)->Close().has_value());
}

TEST_CASE("Scoped approval SDK: real timeout and peer Close keep execution and late replies fenced") {
    Paths paths;
    auto runtime = sdk::Runtime::Create(paths.Roots());
    REQUIRE(runtime.has_value());
    auto timeout_tools = std::make_shared<std::atomic<int>>(0);
    auto timed = (*runtime)->OpenSession(Options(paths, std::make_shared<std::atomic<int>>(0), timeout_tools, 200ms));
    REQUIRE(timed.has_value());
    auto timed_events = (*timed)->Subscribe();
    REQUIRE(timed_events.has_value());
    auto input = (*timed)->Submit("expired", "wait without approval");
    REQUIRE(input.has_value());
    const auto expired = ApprovalEvent(*timed_events, input->operation_id);
    REQUIRE((*timed)->WaitResult(input->operation_id, 15s).has_value());
    CHECK(timeout_tools->load() == 0);
    CHECK((*timed)->PendingApprovals().empty());
    CHECK_FALSE((*timed)->ResolveApproval(expired.request_id, sdk::ApprovalDecision::Accept).has_value());
    auto first_tools = std::make_shared<std::atomic<int>>(0);
    auto peer_tools = std::make_shared<std::atomic<int>>(0);
    auto first = (*runtime)->OpenSession(Options(paths, std::make_shared<std::atomic<int>>(0), first_tools));
    auto peer = (*runtime)->OpenSession(Options(paths, std::make_shared<std::atomic<int>>(0), peer_tools));
    REQUIRE(first.has_value());
    REQUIRE(peer.has_value());
    auto first_events = (*first)->Subscribe();
    auto peer_events = (*peer)->Subscribe();
    REQUIRE(first_events.has_value());
    REQUIRE(peer_events.has_value());
    auto first_input = (*first)->Submit("closed", "first");
    auto peer_input = (*peer)->Submit("peer", "peer");
    REQUIRE(first_input.has_value());
    REQUIRE(peer_input.has_value());
    const auto first_ticket = ApprovalEvent(*first_events, first_input->operation_id);
    const auto peer_ticket = ApprovalEvent(*peer_events, peer_input->operation_id);
    CHECK_FALSE((*first)->ResolveApproval(peer_ticket.request_id, sdk::ApprovalDecision::Accept).has_value());
    REQUIRE((*first)->Close().has_value());
    CHECK_FALSE((*first)->ResolveApproval(first_ticket.request_id, sdk::ApprovalDecision::Accept).has_value());
    REQUIRE((*peer)->PendingApprovals().size() == 1);
    REQUIRE((*peer)->ResolveApproval(peer_ticket.request_id, sdk::ApprovalDecision::Accept).has_value());
    auto peer_result = (*peer)->WaitResult(peer_input->operation_id, 15s);
    REQUIRE(peer_result.has_value());
    CHECK(peer_result->state == sdk::OperationState::Succeeded);
    CHECK(first_tools->load() == 0);
    CHECK(peer_tools->load() == 1);
    REQUIRE((*peer)->Close().has_value());
    REQUIRE((*timed)->Close().has_value());
}
