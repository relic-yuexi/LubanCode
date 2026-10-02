#include <doctest/doctest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <expected>
#include <filesystem>
#include <functional>
#include <future>
#include <iostream>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include <nlohmann/json.hpp>

#include "api/backend.hpp"
#include "platform/paths.hpp"
#include "runtime/trajectory_session.hpp"
#include "tools/agent_tool.hpp"
#include "trajectory/v3/reader.hpp"
#include "workspace/identity.hpp"

namespace {
using namespace lubancode;
using namespace std::chrono_literals;
namespace v3 = lubancode::trajectory::v3;

struct Directory {
    std::filesystem::path root;
    Directory() {
        static std::atomic<unsigned> serial{0};
        root = std::filesystem::temp_directory_path() /
            ("child-foreground-integration-" +
             std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + "-" +
             std::to_string(++serial));
        REQUIRE(std::filesystem::create_directories(root / "project"));
    }
    ~Directory() { std::error_code error; std::filesystem::remove_all(root, error); }
};

struct ProbeState {
    std::mutex mutex;
    std::condition_variable cv;
    bool entered = false, release = false, saw_cancel = false, had_cancel = false;
    int calls = 0;
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
    explicit Probe(std::shared_ptr<ProbeState> state) : state_(std::move(state)) {}
    std::string name() const override { return "child_integration_probe"; }
    std::string description() const override { return "Wait for this actual child invocation."; }
    nlohmann::json input_schema() const override { return {{"type", "object"}}; }
    Result execute(const nlohmann::json&) override { return {"actual context required", true}; }
    Result execute(const nlohmann::json&, const tools::ToolExecutionContext& context) override {
        std::unique_lock lock(state_->mutex);
        ++state_->calls;
        state_->entered = true;
        state_->had_cancel = context.cancel != nullptr;
        state_->child_invocation = context.invocation; // Value only; never save the cancel pointer.
        state_->cv.notify_all();
        const auto deadline = std::chrono::steady_clock::now() + 8s;
        while (!state_->release && !(context.cancel && context.cancel->load(std::memory_order_acquire))) {
            state_->cv.wait_until(lock, std::min(deadline, std::chrono::steady_clock::now() + 5ms));
            if (std::chrono::steady_clock::now() >= deadline)
                return {"child integration wait timed out", true};
        }
        state_->saw_cancel = context.cancel && context.cancel->load(std::memory_order_acquire);
        return {state_->saw_cancel ? "actual child cancellation" : "actual child tool completed",
                state_->saw_cancel};
    }
private:
    std::shared_ptr<ProbeState> state_;
};

class Backend final : public api::Backend {
public:
    std::vector<api::Request> requests;
    std::string suffix;
    explicit Backend(std::string value) : suffix(std::move(value)) {}
    std::expected<void, api::Error> send_stream(const api::Request& request,
        const std::function<void(const api::StreamEvent&)>& emit,
        const std::atomic<bool>* = nullptr) override {
        requests.push_back(request);
        emit(api::MessageStart{"message-" + suffix, request.model});
        if (requests.size() == 1) {
            emit(api::ToolUseStart{0, "probe-call-" + suffix, "child_integration_probe"});
            emit(api::ToolUseInputDelta{0, "{}"});
            emit(api::ContentBlockDone{0});
            emit(api::MessageDone{"tool_use", api::Usage{}});
            return {};
        }
        if (requests.size() != 2)
            return std::unexpected(api::Error{api::ErrorKind::Api, "unexpected child replay", 0});
        emit(api::TextDelta{"completed child " + suffix});
        emit(api::ContentBlockDone{0});
        emit(api::MessageDone{"end_turn", api::Usage{}});
        return {};
    }
};

bool Empty(const tools::ToolInvocationIdentity& value) {
    return value.session_id.empty() && value.operation_id.empty() && value.turn_id.empty() &&
           value.action_id.empty() && value.attempt == 0;
}

bool ContainsText(const api::Request& request, const std::string& marker) {
    for (const auto& message : request.messages)
        for (const auto& block : message.content)
            if (const auto* text = std::get_if<api::TextBlock>(&block);
                text && text->text.find(marker) != std::string::npos) return true;
    return false;
}

void Same(const runtime::SubagentTerminalReceipt& a, const runtime::SubagentTerminalReceipt& b) {
    CHECK(a.format == b.format);
    CHECK(a.session_id == b.session_id);
    CHECK(a.run_id == b.run_id);
    CHECK(a.terminal_kind == b.terminal_kind);
    CHECK(a.execution == b.execution);
    CHECK(a.reason == b.reason);
    CHECK(a.confirmation == b.confirmation);
    CHECK(a.broken_after_append == b.broken_after_append);
    CHECK(a.broken_after_close == b.broken_after_close);
    CHECK(a.append_error_code == b.append_error_code);
    CHECK(a.append_error_message == b.append_error_message);
    CHECK(a.seal == b.seal);
    CHECK(a.close_error_code == b.close_error_code);
    CHECK(a.close_error_message == b.close_error_message);
    CHECK(a.journal_sha256 == b.journal_sha256);
    REQUIRE(a.terminal.has_value());
    REQUIRE(b.terminal.has_value());
    CHECK(a.terminal->session_id == b.terminal->session_id);
    CHECK(a.terminal->run_id == b.terminal->run_id);
    CHECK(a.terminal->event_id == b.terminal->event_id);
    CHECK(a.terminal->seq == b.terminal->seq);
    CHECK(a.terminal->hash == b.terminal->hash);
    REQUIRE(std::holds_alternative<v3::WriteReceipt>(a.append));
    REQUIRE(std::holds_alternative<v3::WriteReceipt>(b.append));
    const auto& native = std::get<v3::WriteReceipt>(a.append);
    const auto& other = std::get<v3::WriteReceipt>(b.append);
    CHECK(native.status == other.status);
    CHECK(native.id == other.id);
    CHECK(native.seq == other.seq);
    CHECK(native.line_hash == other.line_hash);
    CHECK(native.error_code == other.error_code);
    CHECK(native.error_message == other.error_message);
}

struct Rig {
    std::shared_ptr<ProbeState> state = std::make_shared<ProbeState>();
    std::shared_ptr<std::atomic<unsigned>> closes = std::make_shared<std::atomic<unsigned>>(0);
    Backend backend;
    tools::ToolRegistry original_registry;
    std::unique_ptr<runtime::TrajectorySessionLedger> ledger;
    std::shared_ptr<runtime::SubagentTerminalRegistry> registry =
        std::make_shared<runtime::SubagentTerminalRegistry>();
    std::optional<runtime::SubagentTerminalReceipt> completed;
    unsigned finished_callbacks = 0;
    tools::ToolInvocationIdentity parent_cause;
    std::unique_ptr<tools::AgentTool> tool;

    Rig(const Directory& directory, std::string suffix, bool close_failure = false)
        : backend(std::move(suffix)) {
        const auto cwd = directory.root / "project";
        runtime::TrajectorySessionLedger::Options options;
        options.workspaces_root = directory.root / "workspaces";
        options.workspace_root = cwd;
        options.workspace_identity = workspace::MakeFallbackIdentity(cwd);
        options.launch_cwd = platform::PathToUtf8(cwd);
        options.lubancode_version = "child-integration-test";
        options.v3_system_content = "Real child integration ledger " + backend.suffix;
        if (close_failure) options.subagent_close_fault = [counter = closes]() -> std::optional<std::string> {
            counter->fetch_add(1);
            return "v3writer.integration_close_failed: after actual native Close";
        };
        auto opened = runtime::TrajectorySessionLedger::Open(std::move(options));
        const std::string error = opened ? std::string() : opened.error();
        REQUIRE_MESSAGE(opened.has_value(), error);
        ledger = std::make_unique<runtime::TrajectorySessionLedger>(std::move(*opened));
        parent_cause = {ledger->session_id(), "parent-operation-" + backend.suffix,
                        "parent-turn-" + backend.suffix, "parent-action-" + backend.suffix, 7};
        original_registry.Register(std::make_unique<Probe>(state));
        tool = std::make_unique<tools::AgentTool>(backend, original_registry,
            platform::PathToUtf8(cwd), "model-" + backend.suffix, 4);
        tools::AgentTool::Hooks hooks;
        hooks.trajectory_spawn = [this](const std::string& label, const std::string& parent_run,
                                       runtime::SubagentSpawnFailure* failure) {
            auto spawned = ledger->SpawnSubagent(parent_cause.action_id, label, parent_run);
            if (!spawned) {
                if (failure) *failure = spawned.error();
                return std::unique_ptr<runtime::TrajectorySubagentBridge>{};
            }
            return std::move(*spawned);
        };
        hooks.trajectory_child_finished = [this, owner = registry](const runtime::SubagentTerminalReceipt& receipt) {
            ++finished_callbacks;
            completed = receipt;
            owner->Store(receipt);
        };
        tool->SetHooks(std::move(hooks));
    }

    tools::Tool::Result Run(tools::ToolExecutionContext context) {
        return tool->execute({{"title", "integration " + backend.suffix},
                              {"prompt", "perform child work " + backend.suffix},
                              {"execution_mode", "foreground"}}, context);
    }
    void WaitEntered() const {
        std::unique_lock lock(state->mutex);
        REQUIRE(state->cv.wait_for(lock, 5s, [&] { return state->entered; }));
        CHECK(state->had_cancel);
        CHECK(Empty(state->child_invocation)); // Parent cause is never a synthetic child SDK invocation.
    }
    void CheckStored(runtime::SubagentExecutionOutcome execution, runtime::SubagentSealState seal) const {
        REQUIRE(completed.has_value());
        CHECK(finished_callbacks == 1);
        CHECK(completed->execution == execution);
        CHECK(completed->confirmation == runtime::SubagentAppendConfirmation::Committed);
        CHECK(completed->seal == seal);
        REQUIRE(completed->terminal.has_value());
        CHECK(completed->session_id != parent_cause.session_id);
        CHECK(completed->terminal_kind == "session.ended");
        REQUIRE(std::holds_alternative<v3::WriteReceipt>(completed->append));
        const auto& native = std::get<v3::WriteReceipt>(completed->append);
        CHECK(native.status == v3::WriteReceipt::Status::Committed);
        CHECK(native.id == completed->terminal->event_id);
        CHECK(native.seq == completed->terminal->seq);
        CHECK(native.line_hash == completed->terminal->hash);
        CHECK_FALSE(completed->terminal->event_id.empty());
        CHECK_FALSE(completed->terminal->hash.empty());
        const auto path = ledger->session_dir() / "subagents" / completed->session_id /
                          (completed->session_id + ".jsonl");
        const auto source = v3::ReadV3Ledger(path);
        REQUIRE(source.has_value());
        REQUIRE_FALSE(source->events.empty());
        const auto& ended = source->events.back();
        CHECK(ended.kind == v3::EventKindV3::SessionEnded);
        CHECK(source->session_id == completed->terminal->session_id);
        CHECK(source->run_id == completed->terminal->run_id);
        CHECK(ended.event_id == completed->terminal->event_id);
        CHECK(ended.seq == completed->terminal->seq);
        CHECK(ended.line_hash == completed->terminal->hash);
        for (const auto& event : source->events) {
            CHECK(event.session_id == completed->session_id);
            CHECK(event.run_id == completed->run_id);
        }
        for (const auto& message : source->messages) {
            CHECK(message.session_id == completed->session_id);
            CHECK(message.run_id == completed->run_id);
        }
        CHECK(ended.payload.at("reason").get<std::string>() == completed->reason);
        CHECK(ended.payload.at("closeQuality").get<std::string>() ==
              (execution == runtime::SubagentExecutionOutcome::Succeeded ? "clean" : "incomplete"));
        REQUIRE_FALSE(source->messages.empty());
        REQUIRE(source->messages.front().system_meta.has_value());
        const auto& cause = source->messages.front().system_meta->at("parentActionRef");
        CHECK(cause.at("sessionId").get<std::string>() == parent_cause.session_id);
        CHECK(cause.at("actionId").get<std::string>() == parent_cause.action_id);
        CHECK(std::count_if(source->events.begin(), source->events.end(), [](const auto& event) {
            return event.kind == v3::EventKindV3::SessionEnded;
        }) == 1);
        const auto model_calls = backend.requests.size();
        const auto tool_calls = state->calls;
        const auto close_calls = closes->load();
        const auto saved = ledger->ChildTerminalReceipt(completed->run_id);
        const auto registered = registry->Find(completed->run_id);
        REQUIRE(saved.has_value());
        REQUIRE(registered.has_value());
        Same(*completed, *saved);
        Same(*completed, *registered);
        for (int query = 0; query < 3; ++query) {
            const auto again = ledger->ChildTerminalReceipt(completed->run_id);
            REQUIRE(again.has_value());
            Same(*saved, *again);
        }
        CHECK(finished_callbacks == 1);
        CHECK(backend.requests.size() == model_calls);
        CHECK(state->calls == tool_calls);
        CHECK(closes->load() == close_calls);
    }
};
} // namespace

TEST_CASE("child integration: same project foreground sessions isolate actual context cancellation and terminal owners") {
    Directory directory;
    std::atomic<bool> cancel_a{false}, cancel_b{false};
    Rig a(directory, "A"), b(directory, "B");
    CHECK(a.ledger->workspace_key() == b.ledger->workspace_key());
    CHECK(a.ledger->session_id() != b.ledger->session_id());
    CHECK(a.ledger->session_dir() != b.ledger->session_dir());
    tools::ToolExecutionContext context_a{&cancel_a}, context_b{&cancel_b};
    context_a.invocation = a.parent_cause;
    context_b.invocation = b.parent_cause;
    std::future<tools::Tool::Result> future_a, future_b;
    // This guard is later than both futures: failure releases actual tools
    // before async future destructors join, while both rigs still own borrows.
    Cleanup cleanup{[&] {
        cancel_a.store(true, std::memory_order_release);
        cancel_b.store(true, std::memory_order_release);
        Release(a.state);
        Release(b.state);
    }};
    future_a = std::async(std::launch::async, [&a, context_a] { return a.Run(context_a); });
    future_b = std::async(std::launch::async, [&b, context_b] { return b.Run(context_b); });
    a.WaitEntered();
    b.WaitEntered(); // Both actual child tools are in flight before only A is cancelled.
    cancel_a.store(true, std::memory_order_release);
    REQUIRE(future_a.wait_for(5s) == std::future_status::ready);
    const auto result_a = future_a.get();
    CHECK(result_a.execution_control == tools::ExecutionControl::Continue);
    CHECK(result_a.content.find("用户中止") != std::string::npos);
    CHECK(future_b.wait_for(0ms) == std::future_status::timeout);
    CHECK_FALSE(cancel_b.load(std::memory_order_acquire));
    {
        std::lock_guard lock(b.state->mutex);
        CHECK_FALSE(b.state->saw_cancel);
        CHECK_FALSE(b.state->release);
    }
    Release(b.state);
    REQUIRE(future_b.wait_for(5s) == std::future_status::ready);
    const auto result_b = future_b.get();
    CHECK_FALSE(result_b.is_error);
    CHECK(result_b.execution_control == tools::ExecutionControl::Continue);
    CHECK(result_b.content == "completed child B");
    a.CheckStored(runtime::SubagentExecutionOutcome::Cancelled, runtime::SubagentSealState::Closed);
    b.CheckStored(runtime::SubagentExecutionOutcome::Succeeded, runtime::SubagentSealState::Closed);
    CHECK(a.completed->durable());
    CHECK(b.completed->durable());
    // Both child session and run names are parent-scoped. V3 main runs can
    // repeat across sessions; neither string alone identifies a global owner.
    // The actual parent directory, native journal and complete receipt do.
    const auto journal_a = a.ledger->session_dir() / "subagents" / a.completed->session_id /
                           (a.completed->session_id + ".jsonl");
    const auto journal_b = b.ledger->session_dir() / "subagents" / b.completed->session_id /
                           (b.completed->session_id + ".jsonl");
    CHECK(journal_a != journal_b);
    CHECK(std::filesystem::is_regular_file(journal_a));
    CHECK(std::filesystem::is_regular_file(journal_b));
    CHECK(a.completed->terminal->hash != b.completed->terminal->hash);
    if (a.completed->run_id == b.completed->run_id) {
        // A lookup by the same local name must still return this owner's
        // cancelled/succeeded receipt, never the peer's value or journal.
        const auto registered_a = a.registry->Find(b.completed->run_id);
        const auto registered_b = b.registry->Find(a.completed->run_id);
        const auto saved_a = a.ledger->ChildTerminalReceipt(b.completed->run_id);
        const auto saved_b = b.ledger->ChildTerminalReceipt(a.completed->run_id);
        REQUIRE(registered_a.has_value());
        REQUIRE(registered_b.has_value());
        REQUIRE(saved_a.has_value());
        REQUIRE(saved_b.has_value());
        Same(*a.completed, *registered_a);
        Same(*b.completed, *registered_b);
        Same(*a.completed, *saved_a);
        Same(*b.completed, *saved_b);
    } else {
        CHECK_FALSE(a.registry->Find(b.completed->run_id).has_value());
        CHECK_FALSE(b.registry->Find(a.completed->run_id).has_value());
        CHECK_FALSE(a.ledger->ChildTerminalReceipt(b.completed->run_id).has_value());
        CHECK_FALSE(b.ledger->ChildTerminalReceipt(a.completed->run_id).has_value());
    }
    const std::string absent_run = "child-integration-never-allocated";
    CHECK_FALSE(a.registry->Find(absent_run).has_value());
    CHECK_FALSE(b.registry->Find(absent_run).has_value());
    CHECK_FALSE(a.ledger->ChildTerminalReceipt(absent_run).has_value());
    CHECK_FALSE(b.ledger->ChildTerminalReceipt(absent_run).has_value());
    const auto tasks_a = a.tool->TaskSnapshots();
    const auto tasks_b = b.tool->TaskSnapshots();
    REQUIRE(tasks_a.size() == 1);
    REQUIRE(tasks_b.size() == 1);
    CHECK(tasks_a[0].state == tools::AgentTaskState::Cancelled);
    CHECK(tasks_b[0].state == tools::AgentTaskState::Done);
    CHECK(tasks_a[0].outcome.status == tools::TaskOutcomeStatus::Stopped);
    CHECK(tasks_a[0].outcome.reason == tools::TaskOutcomeReason::UserStop);
    CHECK(tasks_b[0].outcome.status == tools::TaskOutcomeStatus::Completed);
    CHECK(tasks_a[0].agent_run_id == a.completed->run_id);
    CHECK(tasks_b[0].agent_run_id == b.completed->run_id);
    CHECK_FALSE(a.tool->coordinator()->ledger().HasRunningTasks());
    CHECK_FALSE(b.tool->coordinator()->ledger().HasRunningTasks());
    const auto cwd = platform::PathToUtf8(directory.root / "project");
    CHECK(tasks_a[0].effective_cwd == cwd);
    CHECK(tasks_b[0].effective_cwd == cwd);
    CHECK(a.backend.requests.size() == 1);
    CHECK(b.backend.requests.size() == 2);
    CHECK(a.state->calls == 1);
    CHECK(b.state->calls == 1);
    CHECK(a.state->saw_cancel);
    CHECK_FALSE(b.state->saw_cancel);
    for (const auto& request : a.backend.requests) {
        CHECK(request.model == "model-A");
        CHECK(ContainsText(request, "perform child work A"));
        CHECK_FALSE(ContainsText(request, "perform child work B"));
    }
    for (const auto& request : b.backend.requests) {
        CHECK(request.model == "model-B");
        CHECK(ContainsText(request, "perform child work B"));
        CHECK_FALSE(ContainsText(request, "perform child work A"));
    }
    std::cout << "[child-integration-path] isolation\n";
}

TEST_CASE("child integration: actual context cancellation and native Close failure keep execution and block replay") {
    Directory directory;
    std::atomic<bool> cancel{false};
    Rig rig(directory, "cancel-close", true);
    tools::ToolExecutionContext context{&cancel};
    context.invocation = rig.parent_cause;
    std::future<tools::Tool::Result> future;
    Cleanup cleanup{[&] { cancel.store(true, std::memory_order_release); Release(rig.state); }};
    future = std::async(std::launch::async, [&rig, context] { return rig.Run(context); });
    rig.WaitEntered();
    cancel.store(true, std::memory_order_release);
    REQUIRE(future.wait_for(5s) == std::future_status::ready);
    const auto result = future.get();
    CHECK(result.is_error);
    CHECK(result.error_code == "trajectory.child_terminal_persistence_failed");
    CHECK(result.execution_control == tools::ExecutionControl::StopIndeterminate);
    rig.CheckStored(runtime::SubagentExecutionOutcome::Cancelled, runtime::SubagentSealState::CloseFailed);
    CHECK_FALSE(rig.completed->durable());
    CHECK(rig.completed->reason == "cancelled");
    CHECK(rig.completed->close_error_code == "v3writer.integration_close_failed");
    CHECK_FALSE(rig.completed->broken_after_append);
    CHECK(rig.completed->broken_after_close);
    CHECK(rig.closes->load() == 1);
    const auto tasks = rig.tool->TaskSnapshots();
    REQUIRE(tasks.size() == 1);
    // The existing task projection records the actual stop signal as cancelled.
    // Durable handoff is a separate fact: the result and owned native receipt
    // above must still retain StopIndeterminate and the failed real Close.
    CHECK(tasks[0].state == tools::AgentTaskState::Cancelled);
    CHECK(tasks[0].outcome.status == tools::TaskOutcomeStatus::Stopped);
    CHECK(tasks[0].outcome.reason == tools::TaskOutcomeReason::UserStop);
    CHECK(tasks[0].agent_run_id == rig.completed->run_id);
    CHECK(tasks[0].result == result.content);
    CHECK(tasks[0].outcome.message.find("trajectory.child_terminal_persistence_failed") != std::string::npos);
    CHECK_FALSE(rig.tool->coordinator()->ledger().HasRunningTasks());
    CHECK(rig.backend.requests.size() == 1);
    CHECK(rig.state->calls == 1);
    CHECK(rig.state->saw_cancel);
    CHECK(rig.finished_callbacks == 1);
    std::cout << "[child-integration-path] cancel-close-failed\n";
}
