#include <doctest/doctest.h>

#include <atomic>
#include <expected>
#include <functional>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "agent/turn_harness.hpp"
#include "runtime/execution_owner.hpp"
#include "tools/agent_tool.hpp"
#include "tools/todo_tool.hpp"

namespace {
using namespace lubancode;

struct Audit {
    bool parent_alive = false;
    bool original_tool_alive = false;
    bool wrapper_alive = false;
    bool overlay_alive = false;
    bool correct_teardown = true;
    bool issue_tool = true;
    unsigned tool_calls = 0;
    unsigned wrapper_calls = 0;
    unsigned traces = 0;
    std::weak_ptr<void> agent_capture;
    std::vector<std::string> destruction;
    std::vector<api::Request> requests;
    std::vector<std::thread::id> request_threads;
    std::vector<std::string> callback_destruction;
};

class ParentBackend final : public api::Backend {
public:
    explicit ParentBackend(std::shared_ptr<Audit> audit) : audit_(std::move(audit)) {
        audit_->parent_alive = true;
    }
    ~ParentBackend() override {
        audit_->correct_teardown &= !audit_->wrapper_alive && !audit_->overlay_alive &&
                                     audit_->agent_capture.expired() && !audit_->original_tool_alive;
        audit_->parent_alive = false;
        audit_->destruction.push_back("parent");
    }
    std::expected<void, api::Error> send_stream(const api::Request& request,
        const std::function<void(const api::StreamEvent&)>& emit, const std::atomic<bool>*) override {
        audit_->requests.push_back(request);
        audit_->request_threads.push_back(std::this_thread::get_id());
        emit(api::MessageStart{"owner-message", request.model});
        if (audit_->issue_tool && audit_->requests.size() == 1) {
            emit(api::ToolUseStart{0, "owner-call", "owner_probe"});
            emit(api::ToolUseInputDelta{0, "{}"});
            emit(api::ContentBlockDone{0});
            emit(api::MessageDone{"tool_use", api::Usage{10, 5, 0, 0, 0}});
        } else {
            emit(api::TextDelta{"owner answer"});
            emit(api::ContentBlockDone{0});
            emit(api::MessageDone{"end_turn", api::Usage{10, 5, 0, 0, 0}});
        }
        return {};
    }
private:
    std::shared_ptr<Audit> audit_;
};

class OriginalTool final : public tools::Tool {
public:
    explicit OriginalTool(std::shared_ptr<Audit> audit) : audit_(std::move(audit)) {
        audit_->original_tool_alive = true;
    }
    ~OriginalTool() override {
        audit_->correct_teardown &= audit_->parent_alive && !audit_->overlay_alive && !audit_->wrapper_alive;
        audit_->original_tool_alive = false;
        audit_->destruction.push_back("original");
    }
    std::string name() const override { return "owner_probe"; }
    std::string description() const override { return "Exercise the original caller tool."; }
    nlohmann::json input_schema() const override { return {{"type", "object"}}; }
    tools::EffectClass effect_class() const override { return tools::EffectClass::ReadOnlyLocal; }
    Result execute(const nlohmann::json&) override {
        ++audit_->tool_calls;
        return Result::Text("original tool answer");
    }
private:
    std::shared_ptr<Audit> audit_;
};

class BackendWrapper final : public api::Backend {
public:
    BackendWrapper(api::Backend& parent, std::shared_ptr<Audit> audit)
        : parent_(parent), audit_(std::move(audit)) { audit_->wrapper_alive = true; }
    ~BackendWrapper() override {
        audit_->correct_teardown &= audit_->parent_alive && audit_->original_tool_alive &&
                                     !audit_->overlay_alive && audit_->agent_capture.expired();
        audit_->wrapper_alive = false;
        audit_->destruction.push_back("wrapper");
    }
    std::expected<void, api::Error> send_stream(const api::Request& request,
        const std::function<void(const api::StreamEvent&)>& emit, const std::atomic<bool>* cancel) override {
        ++audit_->wrapper_calls;
        return parent_.send_stream(request, emit, cancel);
    }
private:
    api::Backend& parent_;
    std::shared_ptr<Audit> audit_;
};

class OverlayTool final : public tools::Tool {
public:
    OverlayTool(tools::Tool& original, std::shared_ptr<Audit> audit)
        : original_(original), audit_(std::move(audit)) { audit_->overlay_alive = true; }
    ~OverlayTool() override {
        audit_->correct_teardown &= audit_->parent_alive && audit_->original_tool_alive &&
                                     audit_->wrapper_alive && audit_->agent_capture.expired();
        audit_->overlay_alive = false;
        audit_->destruction.push_back("overlay");
    }
    std::string name() const override { return original_.name(); }
    std::string description() const override { return original_.description(); }
    nlohmann::json input_schema() const override { return original_.input_schema(); }
    tools::EffectClass effect_class() const override { return original_.effect_class(); }
    Result execute(const nlohmann::json& input) override { return original_.execute(input); }
    Result execute(const nlohmann::json& input, const tools::ToolExecutionContext& context) override {
        return original_.execute(input, context);
    }
private:
    tools::Tool& original_;
    std::shared_ptr<Audit> audit_;
};

struct AgentCapture {
    std::shared_ptr<Audit> audit;
    api::Backend& backend;
    tools::ToolRegistry& registry;
    bool wrapped;
    AgentCapture(std::shared_ptr<Audit> value, api::Backend& model, tools::ToolRegistry& tools, bool child_wrapped)
        : audit(std::move(value)), backend(model), registry(tools), wrapped(child_wrapped) {}
    ~AgentCapture() {
        const bool alive = audit->parent_alive && audit->original_tool_alive &&
                           (!wrapped || (audit->wrapper_alive && audit->overlay_alive));
        audit->correct_teardown &= alive;
        // Destruction itself probes the actual borrowed graph, without reaching
        // a dead pointer if an ordering regression has already been observed.
        if (alive) {
            audit->correct_teardown &= registry.Find("owner_probe") != nullptr;
            api::Request request;
            request.model = "destructor probe";
            audit->correct_teardown &= backend.GetEffectiveOutputLimit(request).tokens == std::nullopt;
        }
        audit->destruction.push_back("agent");
    }
};

agent::AgentProfile Profile() {
    agent::AgentProfile profile;
    profile.request.model = "child-model";
    profile.system_prompt = "child owner system";
    profile.runtime.max_steps_per_turn = 4;
    return profile;
}

agent::AgentProfile CapturedProfile(const std::shared_ptr<Audit>& audit, api::Backend& backend,
                                   tools::ToolRegistry& registry, bool wrapped) {
    auto profile = Profile();
    auto capture = std::make_shared<AgentCapture>(audit, backend, registry, wrapped);
    audit->agent_capture = capture;
    profile.deferred_index_provider = [capture] { return std::string(); };
    profile.tool_filter = [capture](const tools::Tool&) { return true; };
    return profile;
}

runtime::ChildExecutionResources ChildResources(api::Backend& parent, tools::ToolRegistry& registry,
                                                const std::shared_ptr<Audit>& audit) {
    auto overlay = std::make_unique<tools::ToolRegistry>();
    overlay->Register(std::make_unique<OverlayTool>(*registry.Find("owner_probe"), audit));
    return {parent, registry, std::make_unique<BackendWrapper>(parent, audit), std::move(overlay)};
}

api::Message User(const std::string& text) { return {api::Role::User, {api::TextBlock{text}}}; }
bool HasText(const api::Request& request, const std::string& text) {
    for (const auto& message : request.messages) {
        for (const auto& block : message.content) {
            if (const auto* value = std::get_if<api::TextBlock>(&block); value && value->text == text) return true;
        }
    }
    return false;
}
bool HasOriginalResult(const api::Request& request) {
    for (const auto& message : request.messages) {
        for (const auto& block : message.content) {
            if (const auto* value = std::get_if<api::ToolResultBlock>(&block)) {
                if (value->tool_use_id == "owner-call" && value->content == "original tool answer" && !value->is_error)
                    return true;
            }
        }
    }
    return false;
}

struct Dependency { bool alive = true; };
struct TurnCapture {
    std::shared_ptr<Audit> audit;
    agent::Agent& agent;
    Dependency& dependency;
    std::string label;
    TurnCapture(std::shared_ptr<Audit> value, agent::Agent& model, Dependency& borrowed, std::string name)
        : audit(std::move(value)), agent(model), dependency(borrowed), label(std::move(name)) {}
    ~TurnCapture() {
        audit->correct_teardown &= dependency.alive && audit->parent_alive;
        if (label == "turn") {
            const auto& previous = agent.wiring().execution_id_issuer;
            audit->correct_teardown &= previous && previous() == "prior-id";
        }
        audit->callback_destruction.push_back(label);
    }
};

struct ThrowingCopy {
    std::shared_ptr<std::atomic<bool>> armed;
    explicit ThrowingCopy(std::shared_ptr<std::atomic<bool>> value) : armed(std::move(value)) {}
    ThrowingCopy(const ThrowingCopy& other) : armed(other.armed) {
        if (armed->load()) throw std::runtime_error("child profile copy failure");
    }
    bool operator()(const tools::Tool&) const { return true; }
};

void CheckHostBorrowedExecution() {
    auto audit = std::make_shared<Audit>();
    ParentBackend backend(audit);
    tools::ToolRegistry registry;
    registry.Register(std::make_unique<OriginalTool>(audit));
    auto* original_tool = registry.Find("owner_probe");
    {
        auto profile = CapturedProfile(audit, backend, registry, false);
        profile.request.model = "host-model";
        profile.system_prompt = "host original system";
        runtime::ExecutionOwner host(runtime::HostBorrowedExecutionResources{backend, registry},
                                     std::move(profile));
        CHECK(host.session_resources() == nullptr);
        CHECK(&host.backend() == &backend);
        CHECK(&host.registry() == &registry);
        CHECK_FALSE(profile.tool_filter);
        CHECK_FALSE(profile.deferred_index_provider);
        REQUIRE(host.agent().Run("host borrowed request", {}).has_value());
        REQUIRE(audit->requests.size() == 2);
        CHECK(audit->requests[0].model == "host-model");
        CHECK(audit->requests[0].system == "host original system");
        CHECK(HasOriginalResult(audit->requests[1]));
        CHECK(audit->tool_calls == 1);
        CHECK(audit->wrapper_calls == 0);
        CHECK_FALSE(audit->agent_capture.expired());
    }
    CHECK(audit->agent_capture.expired());
    CHECK(audit->destruction == std::vector<std::string>{"agent"});
    CHECK(audit->correct_teardown);
    CHECK(audit->parent_alive);
    CHECK(audit->original_tool_alive);
    CHECK(registry.Find("owner_probe") == original_tool);
    REQUIRE(original_tool->execute(nlohmann::json::object()).content == "original tool answer");
    api::Request next;
    next.model = "host after Agent retirement";
    REQUIRE(backend.send_stream(next, [](const auto&) {}, nullptr).has_value());
    REQUIRE(audit->requests.size() == 3);
    CHECK(audit->correct_teardown);

    auto failed_audit = std::make_shared<Audit>();
    ParentBackend failed_backend(failed_audit);
    tools::ToolRegistry failed_registry;
    failed_registry.Register(std::make_unique<OriginalTool>(failed_audit));
    auto profile = CapturedProfile(failed_audit, failed_backend, failed_registry, false);
    auto armed = std::make_shared<std::atomic<bool>>(false);
    profile.tool_filter = ThrowingCopy(armed);
    armed->store(true);
    CHECK_THROWS_WITH(([&] {
        runtime::ExecutionOwner rejected(
            runtime::HostBorrowedExecutionResources{failed_backend, failed_registry}, std::move(profile));
    })(), "child profile copy failure");
    CHECK_FALSE(profile.tool_filter);
    CHECK_FALSE(profile.deferred_index_provider);
    CHECK(failed_audit->agent_capture.expired());
    CHECK(failed_audit->destruction == std::vector<std::string>{"agent"});
    CHECK(failed_audit->correct_teardown);
    CHECK(failed_audit->parent_alive);
    CHECK(failed_audit->original_tool_alive);
    CHECK(failed_audit->requests.empty());
    REQUIRE(failed_registry.Find("owner_probe")->execute(nlohmann::json::object()).content == "original tool answer");
    REQUIRE(failed_backend.send_stream(next, [](const auto&) {}, nullptr).has_value());
    CHECK(failed_audit->requests.size() == 1);
}
void CheckStableHostAgentBorrows() {
    auto audit = std::make_shared<Audit>();
    ParentBackend backend(audit);
    tools::ToolRegistry registry;
    registry.Register(std::make_unique<OriginalTool>(audit));
    {
        auto first = CapturedProfile(audit, backend, registry, false);
        first.request.model = "stable-host-first";
        const auto first_capture = audit->agent_capture;
        runtime::ExecutionOwner host(runtime::HostBorrowedExecutionResources{backend, registry}, std::move(first));
        REQUIRE(host.has_agent());
        auto* cached_command_agent = &host.agent();
        auto& cached_state_agent = host.agent();
        const auto stored_command = [cached_command_agent](const std::string& text) {
            return cached_command_agent->Run(text, {});
        };
        REQUIRE(stored_command("first stable request").has_value());
        REQUIRE(audit->requests.size() == 2);
        CHECK(HasOriginalResult(audit->requests.back()));

        auto replacement = CapturedProfile(audit, backend, registry, false);
        replacement.request.model = "stable-host-rebuilt";
        replacement.system_prompt = "stable rebuilt system";
        const auto second_capture = audit->agent_capture;
        host.RebuildHostAgent(replacement);
        REQUIRE(host.has_agent());
        CHECK(first_capture.expired());
        CHECK(&host.agent() == cached_command_agent);
        CHECK(&host.agent() == &cached_state_agent);
        CHECK(replacement.tool_filter);
        CHECK(replacement.deferred_index_provider);
        CHECK(replacement.request.model == "stable-host-rebuilt");
        runtime::ClearExecutionProfileBorrowers(replacement);
        CHECK_FALSE(second_capture.expired());
        audit->requests.clear();
        REQUIRE(stored_command("rebuilt stable request").has_value());
        REQUIRE(audit->requests.size() == 2);
        CHECK(audit->requests.front().model == "stable-host-rebuilt");
        CHECK(audit->requests.front().system == "stable rebuilt system");
        CHECK(HasOriginalResult(audit->requests.back()));
        CHECK(audit->tool_calls == 2);
        CHECK(&cached_state_agent.History() == &host.agent().History());

        auto broken = Profile();
        auto armed = std::make_shared<std::atomic<bool>>(false);
        broken.tool_filter = ThrowingCopy(armed);
        armed->store(true);
        CHECK_THROWS_WITH(host.RebuildHostAgent(broken), "child profile copy failure");
        CHECK_FALSE(host.has_agent());
        CHECK(second_capture.expired());
        CHECK(broken.tool_filter); // The const caller source is never consumed.
        CHECK(audit->parent_alive);
        CHECK(audit->original_tool_alive);
        CHECK(registry.Find("owner_probe") != nullptr);

        auto resumed = Profile();
        resumed.request.model = "stable-host-after-failure";
        host.RebuildHostAgent(resumed, std::vector<api::Message>{User("actual restored stable history")});
        REQUIRE(host.has_agent());
        CHECK(&host.agent() == cached_command_agent);
        cached_state_agent.SetSystemPrompt("stored reference after rebuild");
        audit->requests.clear();
        REQUIRE(stored_command("stable after failure").has_value());
        REQUIRE(audit->requests.size() == 2);
        CHECK(audit->requests.front().model == "stable-host-after-failure");
        CHECK(audit->requests.front().system == "stored reference after rebuild");
        CHECK(HasText(audit->requests.front(), "actual restored stable history"));
        CHECK(HasOriginalResult(audit->requests.back()));
        {
            runtime::ExecutionTurnScope active(host, {}, {});
            CHECK_THROWS_WITH(host.RebuildHostAgent(resumed), "execution.turn_already_active");
            CHECK(host.has_agent());
            CHECK(&host.agent() == cached_command_agent);
        }
    }
    CHECK(audit->agent_capture.expired());
    CHECK(audit->correct_teardown);
    CHECK(audit->parent_alive);
    CHECK(audit->original_tool_alive);
    {
        runtime::ExecutionOwner child(runtime::ChildExecutionResources{backend, registry, {}, {}}, Profile());
        auto* child_agent = &child.agent();
        CHECK_THROWS_WITH(child.RebuildHostAgent(Profile()), "execution.host_borrow_required");
        CHECK(child.has_agent());
        CHECK(&child.agent() == child_agent);
        REQUIRE(child.agent().Run("child still runs after host-only rejection", {}).has_value());
        CHECK(audit->requests.back().model == "child-model");
    }
}
}  // namespace

TEST_CASE("execution owner: owned child wrapper and overlay retire after Agent before parent borrows") {
    auto audit = std::make_shared<Audit>();
    {
        ParentBackend parent(audit);
        tools::ToolRegistry original;
        original.Register(std::make_unique<OriginalTool>(audit));
        auto* original_tool = original.Find("owner_probe");
        {
            auto resources = ChildResources(parent, original, audit);
            auto* wrapper = resources.backend_wrapper.get();
            auto* overlay = resources.registry_overlay.get();
            auto profile = CapturedProfile(audit, *wrapper, *overlay, true);
            runtime::ExecutionOwner child(std::move(resources), std::move(profile));
            CHECK(child.session_resources() == nullptr);
            CHECK(&child.backend() == wrapper);
            CHECK(&child.registry() == overlay);
            CHECK_FALSE(profile.tool_filter);
            CHECK_FALSE(profile.deferred_index_provider);
            CHECK(profile.request.model == "child-model");
            REQUIRE(child.agent().Run("owned child request", {}).has_value());
            REQUIRE(audit->requests.size() == 2);
            CHECK(audit->wrapper_calls == 2);
            CHECK(audit->tool_calls == 1);
            CHECK(HasOriginalResult(audit->requests[1]));
            CHECK_FALSE(audit->agent_capture.expired());
        }
        CHECK(audit->destruction == std::vector<std::string>{"agent", "overlay", "wrapper"});
        CHECK(audit->correct_teardown);
        CHECK(audit->parent_alive);
        CHECK(audit->original_tool_alive);
        CHECK(original.Find("owner_probe") == original_tool);
        REQUIRE(original_tool->execute(nlohmann::json::object()).content == "original tool answer");
    }
    CHECK(audit->destruction == std::vector<std::string>{"agent", "overlay", "wrapper", "original", "parent"});
    CHECK(audit->correct_teardown);
}

TEST_CASE("execution owner: unwrapped child borrows the original backend and registry without owning either") {
    CheckHostBorrowedExecution();
    CheckStableHostAgentBorrows();
    auto audit = std::make_shared<Audit>();
    ParentBackend parent(audit);
    tools::ToolRegistry original;
    original.Register(std::make_unique<OriginalTool>(audit));
    {
        auto profile = CapturedProfile(audit, parent, original, false);
        runtime::ExecutionOwner child({parent, original, nullptr, nullptr}, std::move(profile));
        CHECK(&child.backend() == &parent);
        CHECK(&child.registry() == &original);
        CHECK(child.session_resources() == nullptr);
        REQUIRE(child.agent().Run("borrowed child request", {}).has_value());
        REQUIRE(audit->requests.size() == 2);
        CHECK(HasOriginalResult(audit->requests[1]));
    }
    CHECK(audit->destruction == std::vector<std::string>{"agent"});
    CHECK(audit->parent_alive);
    CHECK(audit->original_tool_alive);
    api::Request next;
    next.model = "parent remains usable";
    REQUIRE(parent.send_stream(next, [](const auto&) {}, nullptr).has_value());
    CHECK(audit->requests.size() == 3);
    CHECK(audit->correct_teardown);
}

TEST_CASE("execution owner: child fresh empty and restored history use the shared restore semantics") {
    for (const std::string mode : {"fresh", "empty", "restored"}) {
        CAPTURE(mode);
        auto audit = std::make_shared<Audit>();
        audit->issue_tool = false;
        ParentBackend parent(audit);
        tools::ToolRegistry original;
        std::optional<std::vector<api::Message>> history;
        if (mode != "fresh") history.emplace();
        if (mode == "restored") {
            history->push_back(User("restored child user"));
            history->push_back({api::Role::Assistant, {api::TextBlock{"restored child answer"}}});
        }
        runtime::ExecutionOwner child({parent, original, nullptr, nullptr}, Profile(), std::move(history));
        CHECK(child.agent().cache_epoch() == (mode == "fresh" ? 1 : 2));
        REQUIRE(child.agent().Run("first child input", {}).has_value());
        REQUIRE(child.agent().Run("second child input", {}).has_value());
        REQUIRE(audit->requests.size() == 2);
        for (const auto& request : audit->requests) {
            CHECK(request.model == "child-model");
            CHECK(HasText(request, "restored child user") == (mode == "restored"));
            CHECK(HasText(request, "restored child answer") == (mode == "restored"));
        }
        CHECK(HasText(audit->requests[1], "first child input"));
    }
}

TEST_CASE("execution owner: consuming turn scope restores Agent callbacks before retiring turn borrows") {
    auto audit = std::make_shared<Audit>();
    ParentBackend parent(audit);
    tools::ToolRegistry original;
    original.Register(std::make_unique<OriginalTool>(audit));
    runtime::ExecutionOwner child({parent, original, nullptr, nullptr}, Profile());
    agent::AgentWiring previous;
    previous.execution_id_issuer = [] { return std::string("prior-id"); };
    child.agent().SetWiring(std::move(previous));
    Dependency dependency;
    agent::AgentWiring prepared_agent;
    agent::TurnWiring prepared_turn;
    auto agent_capture = std::make_shared<TurnCapture>(audit, child.agent(), dependency, "agent");
    auto turn_capture = std::make_shared<TurnCapture>(audit, child.agent(), dependency, "turn");
    std::weak_ptr<void> agent_weak = agent_capture, turn_weak = turn_capture;
    prepared_agent.execution_id_issuer = [agent_capture] { return std::string("child-execution-id"); };
    prepared_turn.on_tool_trace = [turn_capture, audit](const auto&) { ++audit->traces; };
    prepared_turn.turn_id = "child-turn";
    agent_capture.reset();
    turn_capture.reset();
    {
        runtime::ExecutionTurnScope turn(child, std::move(prepared_agent), std::move(prepared_turn));
        CHECK_FALSE(prepared_agent.execution_id_issuer);
        CHECK_FALSE(prepared_turn.on_tool_trace);
        CHECK(turn.wiring().turn_id == "child-turn");
        const auto result = agent::DriveTurn(child.agent(), turn.wiring(), User("scoped child input"), {});
        REQUIRE(result.ok);
        CHECK(result.steps_used == 2);
        CHECK(audit->traces > 0);
        CHECK_FALSE(agent_weak.expired());
        CHECK_FALSE(turn_weak.expired());
        turn.Reset();
        CHECK(agent_weak.expired());
        CHECK(turn_weak.expired());
        CHECK_FALSE(turn.wiring().on_tool_trace);
        CHECK(turn.wiring().turn_id.empty());
        CHECK(child.agent().wiring().execution_id_issuer() == "prior-id");
        turn.Reset();
    }
    CHECK(audit->callback_destruction == std::vector<std::string>{"agent", "turn"});
    CHECK(audit->correct_teardown);
    dependency.alive = false;
    runtime::ExecutionTurnScope second(child, {}, {});
    REQUIRE(child.agent().Run("after consumed turn", second.wiring()).has_value());
    CHECK(audit->requests.size() == 3);
}

TEST_CASE("execution owner: callback exception and duplicate scope preserve the owner and consume prepared borrows") {
    auto audit = std::make_shared<Audit>();
    ParentBackend parent(audit);
    tools::ToolRegistry original;
    original.Register(std::make_unique<OriginalTool>(audit));
    runtime::ExecutionOwner child({parent, original, nullptr, nullptr}, Profile());
    agent::AgentWiring previous;
    previous.execution_id_issuer = [] { return std::string("prior-id"); };
    child.agent().SetWiring(std::move(previous));
    Dependency dependency;
    agent::AgentWiring prepared_agent;
    agent::TurnWiring prepared_turn;
    auto agent_capture = std::make_shared<TurnCapture>(audit, child.agent(), dependency, "agent");
    auto turn_capture = std::make_shared<TurnCapture>(audit, child.agent(), dependency, "turn");
    std::weak_ptr<void> agent_weak = agent_capture, turn_weak = turn_capture;
    prepared_agent.execution_id_issuer = [agent_capture] { return std::string("temporary-id"); };
    prepared_turn.on_assistant_message_ready = [turn_capture](const auto&) {
        throw std::runtime_error("owner test callback failure");
    };
    agent_capture.reset();
    turn_capture.reset();
    CHECK_THROWS_WITH(([&] {
        runtime::ExecutionTurnScope turn(child, std::move(prepared_agent), std::move(prepared_turn));
        agent::AgentWiring rejected_agent;
        agent::TurnWiring rejected_turn;
        auto rejected = std::make_shared<TurnCapture>(audit, child.agent(), dependency, "rejected");
        std::weak_ptr<void> rejected_weak = rejected;
        rejected_agent.execution_id_issuer = [rejected] { return std::string("must not install"); };
        rejected_turn.on_tool_trace = [rejected](const auto&) {};
        rejected.reset();
        CHECK_THROWS_WITH(([&] {
            runtime::ExecutionTurnScope duplicate(child, std::move(rejected_agent), std::move(rejected_turn));
        })(), "execution.turn_already_active");
        CHECK(rejected_weak.expired());
        CHECK_FALSE(rejected_agent.execution_id_issuer);
        CHECK_FALSE(rejected_turn.on_tool_trace);
        CHECK(child.agent().wiring().execution_id_issuer() == "temporary-id");
        (void)agent::DriveTurn(child.agent(), turn.wiring(), User("actual throwing callback"), {});
    })(), "owner test callback failure");
    CHECK(agent_weak.expired());
    CHECK(turn_weak.expired());
    CHECK(audit->callback_destruction == std::vector<std::string>{"rejected", "agent", "turn"});
    CHECK(audit->correct_teardown);
    CHECK(child.agent().wiring().execution_id_issuer() == "prior-id");
    CHECK(audit->requests.size() == 1);
    runtime::ExecutionTurnScope after(child, {}, {});
    REQUIRE(child.agent().Run("after exception", after.wiring()).has_value());
    CHECK(audit->requests.size() == 2);
}

TEST_CASE("execution owner: failing child profile copy clears source and partial captures before its owned graph") {
    auto audit = std::make_shared<Audit>();
    ParentBackend parent(audit);
    tools::ToolRegistry original;
    original.Register(std::make_unique<OriginalTool>(audit));
    auto resources = ChildResources(parent, original, audit);
    auto profile = CapturedProfile(audit, *resources.backend_wrapper, *resources.registry_overlay, true);
    auto armed = std::make_shared<std::atomic<bool>>(false);
    profile.tool_filter = ThrowingCopy(armed);
    armed->store(true);
    CHECK_THROWS_WITH(([&] {
        runtime::ExecutionOwner rejected(std::move(resources), std::move(profile));
    })(), "child profile copy failure");
    CHECK_FALSE(profile.tool_filter);
    CHECK_FALSE(profile.deferred_index_provider);
    CHECK(profile.request.model == "child-model");
    CHECK(audit->agent_capture.expired());
    CHECK(resources.backend_wrapper == nullptr);
    CHECK(resources.registry_overlay == nullptr);
    CHECK(audit->destruction == std::vector<std::string>{"agent", "overlay", "wrapper"});
    CHECK(audit->requests.empty());
    CHECK(audit->correct_teardown);
    CHECK(audit->parent_alive);
    CHECK(audit->original_tool_alive);
}

TEST_CASE("execution owner: real foreground CLI dispatch retains caller tools and backend on the calling thread") {
    auto audit = std::make_shared<Audit>();
    ParentBackend parent(audit);
    tools::ToolRegistry original;
    original.Register(std::make_unique<OriginalTool>(audit));
    auto parent_todos = std::make_shared<tools::TodoListState>();
    original.Register(std::make_unique<tools::TodoWriteTool>(parent_todos));
    auto* original_probe = original.Find("owner_probe");
    unsigned starts = 0, finishes = 0;
    const auto calling_thread = std::this_thread::get_id();
    {
        tools::AgentTool dispatch(parent, original, "/owner/caller", "cli-child-model", 4);
        tools::AgentTool::Hooks hooks;
        hooks.on_sub_tool_start = [&](const auto&, const auto& name, const auto&) {
            CHECK(std::this_thread::get_id() == calling_thread);
            CHECK(name == "owner_probe");
            ++starts;
        };
        hooks.on_post_tool_hook = [&](const auto&, const auto& name, const auto&, const auto& result) {
            CHECK(std::this_thread::get_id() == calling_thread);
            CHECK(name == "owner_probe");
            CHECK(result.content == "original tool answer");
            ++finishes;
        };
        dispatch.SetHooks(std::move(hooks));
        const auto result = dispatch.execute({{"title", "owner foreground probe"},
            {"prompt", "exercise the caller tool"}, {"execution_mode", "foreground"}});
        REQUIRE_FALSE(result.is_error);
        CHECK(result.content == "owner answer");
        REQUIRE(audit->requests.size() == 2);
        CHECK(HasOriginalResult(audit->requests[1]));
        CHECK(audit->requests[0].model == "cli-child-model");
        CHECK(audit->requests[0].system.find("/owner/caller") != std::string::npos);
        CHECK(audit->request_threads == std::vector<std::thread::id>{calling_thread, calling_thread});
        const auto snapshots = dispatch.TaskSnapshots();
        REQUIRE(snapshots.size() == 1);
        CHECK(snapshots[0].foreground);
        CHECK(snapshots[0].outcome.status == tools::TaskOutcomeStatus::Completed);
        CHECK(snapshots[0].steps_used == 2);
        CHECK(snapshots[0].tool_calls.size() == 1);
    }
    CHECK(starts == 1);
    CHECK(finishes == 1);
    CHECK(audit->tool_calls == 1);
    CHECK(original.Find("owner_probe") == original_probe);
    CHECK(parent_todos->items.empty());
    CHECK(parent_todos->revision == 0);
    CHECK(audit->parent_alive);
    CHECK(audit->original_tool_alive);
    CHECK(audit->destruction.empty());
    REQUIRE(original_probe->execute(nlohmann::json::object()).content == "original tool answer");
}
