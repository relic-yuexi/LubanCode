#include <doctest/doctest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include <nlohmann/json.hpp>

#include "api/backend.hpp"
#include "api/types.hpp"
#include "tools/agent_task_coordinator.hpp"
#include "tools/agent_tool.hpp"
#include "tools/registry.hpp"
#include "tools/tool.hpp"

using namespace lubancode;
using namespace std::chrono_literals;

namespace {
using Json = nlohmann::json;

Json DispatchInput(const std::string& title = "foreground") {
    return Json{{"title", title}, {"prompt", title}};
}

std::vector<api::StreamEvent> TextScript() {
    return {api::MessageStart{"msg", "model"}, api::TextDelta{"done"}, api::ContentBlockDone{0},
            api::MessageDone{"end_turn", api::Usage{}}};
}

std::vector<api::StreamEvent> ToolScript(const std::string& name, const Json& input = Json::object()) {
    return {api::MessageStart{"msg", "model"}, api::ToolUseStart{0, "probe-call", name},
            api::ToolUseInputDelta{0, input.dump()}, api::ContentBlockDone{0},
            api::MessageDone{"tool_use", api::Usage{}}};
}

class Backend final : public api::Backend {
public:
    std::vector<std::vector<api::StreamEvent>> scripts;
    std::atomic<std::size_t> requests{0};

    std::expected<void, api::Error> send_stream(
        const api::Request&, const std::function<void(const api::StreamEvent&)>& emit,
        const std::atomic<bool>* = nullptr) override {
        const auto index = requests.fetch_add(1);
        if (index >= scripts.size())
            return std::unexpected(api::Error{api::ErrorKind::Api, "fixture script exhausted", 0});
        for (const auto& event : scripts[index]) emit(event);
        return {};
    }
};

struct ProbeState {
    std::mutex mutex;
    std::condition_variable cv;
    bool entered = false, release = false, saw_cancel = false, had_cancel = false;
    tools::ToolInvocationIdentity child_invocation;
};

void Release(const std::shared_ptr<ProbeState>& state) {
    { std::lock_guard lock(state->mutex); state->release = true; }
    state->cv.notify_all();
}

struct Cleanup {
    std::function<void()> release;
    ~Cleanup() { release(); }
};

class Probe final : public tools::Tool {
public:
    explicit Probe(std::shared_ptr<ProbeState> state, bool confirm = false)
        : state_(std::move(state)), confirm_(confirm) {}
    std::string name() const override { return "foreground_probe"; }
    std::string description() const override { return "foreground context fixture"; }
    Json input_schema() const override { return Json::object(); }
    bool needs_confirm() const override { return confirm_; }
    tools::ApprovalClass approval_class() const override {
        return confirm_ ? tools::ApprovalClass::Command : tools::ApprovalClass::None;
    }
    Result execute(const Json&) override { return {"context required", true}; }
    Result execute(const Json&, const tools::ToolExecutionContext& context) override {
        std::unique_lock lock(state_->mutex);
        state_->entered = true;
        state_->had_cancel = context.cancel != nullptr;
        state_->child_invocation = context.invocation;
        state_->cv.notify_all();
        const auto deadline = std::chrono::steady_clock::now() + 10s;
        while (!state_->release && !(context.cancel && context.cancel->load(std::memory_order_acquire))) {
            if (state_->cv.wait_until(lock, std::min(deadline, std::chrono::steady_clock::now() + 5ms)) ==
                    std::cv_status::timeout && std::chrono::steady_clock::now() >= deadline)
                return {"fixture wait timed out", true};
        }
        state_->saw_cancel = context.cancel && context.cancel->load(std::memory_order_acquire);
        return {state_->saw_cancel ? "cancelled" : "done", state_->saw_cancel};
    }
private:
    std::shared_ptr<ProbeState> state_;
    bool confirm_;
};

bool Empty(const tools::ToolInvocationIdentity& value) {
    return value.session_id.empty() && value.operation_id.empty() && value.turn_id.empty() &&
           value.action_id.empty() && value.attempt == 0;
}

tools::ToolInvocationIdentity Cause(const std::string& suffix) {
    return {"parent-session-" + suffix, "parent-op-" + suffix, "parent-turn-" + suffix,
            "parent-action-" + suffix, 7};
}

void CheckCause(const tools::ToolInvocationIdentity& actual, const tools::ToolInvocationIdentity& expected) {
    CHECK(actual.session_id == expected.session_id);
    CHECK(actual.operation_id == expected.operation_id);
    CHECK(actual.turn_id == expected.turn_id);
    CHECK(actual.action_id == expected.action_id);
    CHECK(actual.attempt == expected.attempt);
}

struct Rig {
    Backend backend;
    tools::ToolRegistry registry;
    std::shared_ptr<ProbeState> state = std::make_shared<ProbeState>();
    std::unique_ptr<tools::AgentTool> tool;

    explicit Rig(bool nested = false, bool confirm = false) {
        registry.Register(std::make_unique<Probe>(state, confirm));
        tool = std::make_unique<tools::AgentTool>(backend, registry, "/foreground-fixture");
        if (nested) registry.Register(std::make_unique<tools::AgentDispatchTool>(*tool));
        backend.scripts = nested
            ? std::vector<std::vector<api::StreamEvent>>{ToolScript("agent", DispatchInput("nested")),
                                                        ToolScript("foreground_probe")}
            : std::vector<std::vector<api::StreamEvent>>{ToolScript("foreground_probe")};
    }
};

void CheckCancelledForeground(bool shell, bool nested, bool legacy_hooks = false) {
    Rig rig(nested);
    std::atomic<bool> cancel{false};
    tools::ToolExecutionContext context;
    context.cancel = &cancel;
    context.invocation = Cause("owned");
    if (legacy_hooks) {
        tools::AgentTool::Hooks hooks;
        hooks.cancel = &cancel;
        rig.tool->SetHooks(std::move(hooks));
    }
    std::future<tools::Tool::Result> result;
    // Declared after the future: failure unwinding releases before async joins.
    Cleanup cleanup{[&] { cancel.store(true); Release(rig.state); }};
    result = std::async(std::launch::async, [&] {
        if (legacy_hooks) return rig.tool->execute(DispatchInput());
        if (shell) {
            tools::AgentDispatchTool facade(*rig.tool);
            return facade.execute(DispatchInput(), context);
        }
        return rig.tool->execute(DispatchInput(), context);
    });
    {
        std::unique_lock lock(rig.state->mutex);
        REQUIRE(rig.state->cv.wait_for(lock, 5s, [&] { return rig.state->entered; }));
        CHECK(rig.state->had_cancel);
        CHECK(Empty(rig.state->child_invocation));
    }
    cancel.store(true, std::memory_order_release);
    REQUIRE(result.wait_for(5s) == std::future_status::ready);
    const auto value = result.get();
    CHECK(value.content.find("用户中止") != std::string::npos);
    {
        std::lock_guard lock(rig.state->mutex);
        CHECK(rig.state->saw_cancel);
    }
    const auto tasks = rig.tool->TaskSummaries();
    REQUIRE(tasks.size() == (nested ? 2 : 1));
    for (const auto& task : tasks) CHECK(task.state == tools::AgentTaskState::Cancelled);
    CHECK(rig.backend.requests.load() == (nested ? 2 : 1));
}
} // namespace

TEST_CASE("foreground context: typed request owns the whole parent cause") {
    auto coordinator = std::make_shared<tools::AgentTaskCoordinator>();
    std::atomic<bool> flag{false};
    tools::ToolExecutionContext context{&flag};
    context.invocation = Cause("one");
    const auto expected = context.invocation;
    coordinator->SetEngine([&](const tools::AgentDispatchRequest& request) {
        context.invocation = Cause("changed-after-copy");
        CHECK(request.foreground_cancel == &flag);
        REQUIRE(request.parent_invocation_cause.has_value());
        CheckCause(*request.parent_invocation_cause, expected);
        return tools::Tool::Result{"done", false};
    });
    tools::AgentDispatchTool tool(tools::AgentDispatchHandle(coordinator, {}, nullptr));
    CHECK_FALSE(tool.execute(DispatchInput(), context).is_error);
}

TEST_CASE("foreground context: scoped ancestor cause is causal and never merged") {
    auto coordinator = std::make_shared<tools::AgentTaskCoordinator>();
    auto inherited = Cause("ancestor");
    const auto expected = inherited;
    tools::AgentDispatchHandle handle(coordinator, {}, nullptr, inherited);
    inherited = Cause("changed-source");
    tools::ToolExecutionContext context;
    int calls = 0;
    coordinator->SetEngine([&](const tools::AgentDispatchRequest& request) {
        REQUIRE(request.parent_invocation_cause.has_value());
        if (calls++ == 0) CheckCause(*request.parent_invocation_cause, expected);
        else {
            CHECK(request.parent_invocation_cause->session_id == "partial");
            CHECK(request.parent_invocation_cause->operation_id.empty());
            CHECK(request.parent_invocation_cause->turn_id.empty());
            CHECK(request.parent_invocation_cause->action_id.empty());
            CHECK(request.parent_invocation_cause->attempt == 0);
        }
        CHECK(request.foreground_cancel == nullptr);
        return tools::Tool::Result{"done", false};
    });
    CHECK_FALSE(handle.Dispatch(DispatchInput(), context).is_error);
    context.invocation.session_id = "partial";
    CHECK_FALSE(handle.Dispatch(DispatchInput(), context).is_error);
    CHECK(calls == 2);
    CHECK(context.invocation.operation_id.empty());
}

TEST_CASE("foreground context: input-only dispatch invents no invocation or flag") {
    auto coordinator = std::make_shared<tools::AgentTaskCoordinator>();
    coordinator->SetEngine([](const tools::AgentDispatchRequest& request) {
        CHECK(request.foreground_cancel == nullptr);
        CHECK_FALSE(request.parent_invocation_cause.has_value());
        return tools::Tool::Result{"legacy", false};
    });
    tools::AgentDispatchTool tool(tools::AgentDispatchHandle(coordinator, {}, nullptr));
    CHECK(tool.execute(DispatchInput()).content == "legacy");
}

TEST_CASE("foreground context: one shared shell keeps two concurrent call flags separate") {
    auto coordinator = std::make_shared<tools::AgentTaskCoordinator>();
    std::atomic<bool> first_flag{false}, second_flag{false};
    std::mutex mutex;
    std::condition_variable cv;
    int entered = 0;
    bool release = false;
    coordinator->SetEngine([&](const tools::AgentDispatchRequest& request) {
        const bool first = request.input.at("title") == "first";
        CHECK(request.foreground_cancel == (first ? &first_flag : &second_flag));
        REQUIRE(request.parent_invocation_cause.has_value());
        CheckCause(*request.parent_invocation_cause, Cause(first ? "first" : "second"));
        std::unique_lock lock(mutex);
        ++entered; cv.notify_all();
        const auto deadline = std::chrono::steady_clock::now() + 10s;
        while (!release && !request.foreground_cancel->load()) {
            if (cv.wait_until(lock, std::min(deadline, std::chrono::steady_clock::now() + 5ms)) ==
                    std::cv_status::timeout && std::chrono::steady_clock::now() >= deadline)
                return tools::Tool::Result{"timeout", true};
        }
        return tools::Tool::Result{request.foreground_cancel->load() ? "cancelled" : "released", false};
    });
    // This engine reads typed call values; it does not touch the facade's legacy
    // single-loop parameter-streak account or main live Hooks.
    tools::AgentDispatchTool tool(tools::AgentDispatchHandle(coordinator, {}, nullptr));
    tools::ToolExecutionContext first{&first_flag}, second{&second_flag};
    first.invocation = Cause("first"); second.invocation = Cause("second");
    std::future<tools::Tool::Result> a, b;
    Cleanup cleanup{[&] {
        first_flag.store(true); second_flag.store(true);
        { std::lock_guard lock(mutex); release = true; } cv.notify_all();
    }};
    a = std::async(std::launch::async, [&] { return tool.execute(DispatchInput("first"), first); });
    b = std::async(std::launch::async, [&] { return tool.execute(DispatchInput("second"), second); });
    { std::unique_lock lock(mutex); REQUIRE(cv.wait_for(lock, 5s, [&] { return entered == 2; })); }
    first_flag.store(true);
    REQUIRE(a.wait_for(5s) == std::future_status::ready);
    CHECK(a.get().content == "cancelled");
    CHECK_FALSE(second_flag.load());
    CHECK(b.wait_for(30ms) == std::future_status::timeout);
    { std::lock_guard lock(mutex); release = true; } cv.notify_all();
    REQUIRE(b.wait_for(5s) == std::future_status::ready);
    CHECK(b.get().content == "released");
}

TEST_CASE("foreground context: real AgentTool cancellation reaches a running child tool") {
    CheckCancelledForeground(false, false);
}

TEST_CASE("foreground context: real dispatch shell cancellation reaches a running child tool") {
    CheckCancelledForeground(true, false);
}

TEST_CASE("foreground context: nested foreground inherits this call cancellation and no child invocation") {
    CheckCancelledForeground(true, true);
}

TEST_CASE("foreground context: cancellation while queued at the real engine starts no second model or task") {
    Rig rig;
    rig.backend.scripts.push_back(TextScript());
    tools::AgentDispatchTool shell(*rig.tool);
    std::atomic<bool> first_flag{false}, queued_flag{false}, queued_call_started{false};
    tools::ToolExecutionContext first{&first_flag}, queued{&queued_flag};
    std::future<tools::Tool::Result> a, b;
    Cleanup cleanup{[&] {
        first_flag.store(true); queued_flag.store(true); Release(rig.state);
    }};
    a = std::async(std::launch::async, [&] { return shell.execute(DispatchInput("first"), first); });
    {
        std::unique_lock lock(rig.state->mutex);
        REQUIRE(rig.state->cv.wait_for(lock, 5s, [&] { return rig.state->entered; }));
    }
    // The first foreground call holds the real engine route's recursive mutex.
    // This second call can enter only after its flag has already been set.
    b = std::async(std::launch::async, [&] {
        queued_call_started.store(true, std::memory_order_release);
        return shell.execute(DispatchInput("queued"), queued);
    });
    const auto start_deadline = std::chrono::steady_clock::now() + 5s;
    while (!queued_call_started.load(std::memory_order_acquire) &&
           std::chrono::steady_clock::now() < start_deadline)
        std::this_thread::sleep_for(5ms);
    REQUIRE(queued_call_started.load(std::memory_order_acquire));
    REQUIRE(b.wait_for(30ms) == std::future_status::timeout);
    queued_flag.store(true, std::memory_order_release);
    Release(rig.state);
    REQUIRE(a.wait_for(5s) == std::future_status::ready);
    CHECK_FALSE(a.get().is_error);
    REQUIRE(b.wait_for(5s) == std::future_status::ready);
    const auto result = b.get();
    CHECK(result.is_error);
    CHECK(result.error_code == "agent.foreground_cancelled");
    CHECK_FALSE(first_flag.load());
    CHECK(rig.backend.requests.load() == 2); // Only the first call's tool + text turns.
    const auto tasks = rig.tool->TaskSummaries();
    REQUIRE(tasks.size() == 1);
    CHECK(tasks[0].state == tools::AgentTaskState::Done);
}

TEST_CASE("foreground context: old input-only Hooks cancellation remains effective") {
    CheckCancelledForeground(false, false, true);
}

TEST_CASE("foreground context: foreground sync refusal and detached cancellation remain separate") {
    // Sync approval still uses the old callback and blocks the dangerous tool.
    Rig rig(false, true);
    rig.backend.scripts.push_back(TextScript());
    int approvals = 0;
    tools::AgentTool::Hooks hooks;
    hooks.on_tool_confirm = [&](const std::string&, const std::string&, const Json&) { ++approvals; return false; };
    rig.tool->SetHooks(std::move(hooks));
    const auto foreground = rig.tool->execute(DispatchInput());
    CHECK_FALSE(foreground.is_error);
    CHECK(approvals == 1);
    { std::lock_guard lock(rig.state->mutex); CHECK_FALSE(rig.state->entered); }

    // A background launch returns before the task. Its flag must belong to the
    // task, even after the foreground invocation's flag changes.
    Rig background;
    auto backend = std::make_shared<Backend>();
    backend->scripts = {ToolScript("foreground_probe"), TextScript()};
    class BorrowBackend final : public api::Backend {
    public:
        explicit BorrowBackend(std::shared_ptr<Backend> value) : value_(std::move(value)) {}
        std::expected<void, api::Error> send_stream(const api::Request& r,
            const std::function<void(const api::StreamEvent&)>& emit, const std::atomic<bool>* cancel = nullptr) override {
            return value_->send_stream(r, emit, cancel);
        }
    private: std::shared_ptr<Backend> value_;
    };
    background.tool->SetDetachedBackendFactory([backend] {
        tools::DetachedAgentBackend value;
        value.backend = std::make_unique<BorrowBackend>(backend);
        value.request_profile.model = "fixture-model";
        return value;
    });
    std::atomic<bool> parent_flag{false};
    tools::ToolExecutionContext context{&parent_flag}; context.invocation = Cause("background-caller");
    Cleanup cleanup{[&] { Release(background.state); background.tool->CancelAllTasks(); }};
    auto input = DispatchInput("background"); input["execution_mode"] = "background";
    const auto launched = background.tool->execute(input, context);
    REQUIRE_FALSE(launched.is_error);
    const auto launched_tasks = background.tool->TaskSummaries();
    REQUIRE(launched_tasks.size() == 1);
    CHECK_FALSE(launched_tasks[0].foreground);
    {
        std::unique_lock lock(background.state->mutex);
        REQUIRE(background.state->cv.wait_for(lock, 5s, [&] { return background.state->entered; }));
        CHECK(Empty(background.state->child_invocation));
    }
    parent_flag.store(true);
    Release(background.state);
    const auto deadline = std::chrono::steady_clock::now() + 5s;
    while (background.tool->HasRunningTasks() && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(5ms);
    CHECK_FALSE(background.tool->HasRunningTasks());
    { std::lock_guard lock(background.state->mutex); CHECK_FALSE(background.state->saw_cancel); }
    const auto tasks = background.tool->TaskSummaries();
    REQUIRE(tasks.size() == 1);
    CHECK(tasks[0].state == tools::AgentTaskState::Done);
    CHECK(backend->requests.load() == 2);
}
