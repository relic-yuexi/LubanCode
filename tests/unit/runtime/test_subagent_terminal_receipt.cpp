#include <doctest/doctest.h>

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <system_error>
#include <thread>
#include <type_traits>
#include <variant>
#include <vector>

#include <nlohmann/json.hpp>

#include "api/backend.hpp"
#include "agent/turn_harness.hpp"
#include "runtime/trajectory_session.hpp"
#include "tools/agent_tool.hpp"
#include "trajectory/v3/reader.hpp"

using namespace lubancode;
using namespace lubancode::runtime;
namespace v3 = lubancode::trajectory::v3;

namespace {

struct Directory {
    std::filesystem::path root;
    explicit Directory(const char* tag) {
        static std::atomic<unsigned> counter{0};
        root = std::filesystem::temp_directory_path() /
            (std::string("luban-child-receipt-") + tag + "-" +
             std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + "-" +
             std::to_string(++counter));
        REQUIRE(std::filesystem::create_directories(root));
    }
    ~Directory() { std::error_code ec; std::filesystem::remove_all(root, ec); }
};

struct StartGate {
    std::mutex mutex;
    std::condition_variable cv;
    bool released = false;
    void Release() {
        { std::lock_guard lock(mutex); released = true; }
        cv.notify_all();
    }
    bool Wait() {
        std::unique_lock lock(mutex);
        return cv.wait_for(lock, std::chrono::seconds(5), [&] { return released; });
    }
};
struct ReleaseGate {
    StartGate& gate;
    ~ReleaseGate() { gate.Release(); }
};

std::vector<nlohmann::json> Rows(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    REQUIRE(file.is_open());
    std::vector<nlohmann::json> rows;
    std::string line;
    while (std::getline(file, line)) if (!line.empty()) rows.push_back(nlohmann::json::parse(line));
    REQUIRE_FALSE(file.bad());
    return rows;
}

std::size_t Count(const std::vector<nlohmann::json>& rows, const std::string& kind) {
    return std::count_if(rows.begin(), rows.end(), [&](const auto& row) {
        return row.value("kind", std::string()) == kind;
    });
}

std::unique_ptr<TrajectorySubagentBridge> V3Child(
    const std::filesystem::path& path, std::shared_ptr<SubagentTerminalRegistry> registry,
    v3::V3WriterOptions options = {}, bool closed_before_finish = false,
    const std::string& run = "agent-receipt") {
    auto opened = v3::V3Writer::Start(path, "20261002-120000-AAAAAA", run, "child system",
                                     nlohmann::json::object(), std::move(options));
    REQUIRE(opened.has_value());
    auto writer = std::make_unique<v3::V3Writer>(std::move(*opened));
    if (closed_before_finish) REQUIRE(writer->Close().has_value());
    auto books = std::make_unique<V3SessionBooks>();
    books->writer = writer.get();
    books->system_content = "child system";
    trajectory::EventScope identity;
    identity.session_id = writer->session_id();
    identity.run_id = writer->run_id();
    identity.workspace_key = "test-000000000000";
    auto turn = std::make_unique<TrajectoryTurnBridge>(writer.get(), books.get(), identity,
        TrajectoryTurnBridge::Identity{"demo", "responses", "subagent"});
    return TrajectorySubagentBridge::OwnV3(std::move(writer), std::move(books), std::move(turn),
                                          std::move(registry));
}

std::unique_ptr<TrajectorySubagentBridge> V2Child(
    const std::filesystem::path& path, std::shared_ptr<SubagentTerminalRegistry> registry,
    trajectory::RecorderOptions options = {}) {
    trajectory::EventScope identity;
    identity.workspace_key = "test-000000000000";
    identity.session_id = "20261002-120000-AAAAAA";
    identity.run_id = "agent-v2-receipt";
    identity.run_kind = trajectory::RunKind::Subagent;
    identity.visibility = {trajectory::Visibility::HostOnly};
    auto opened = trajectory::TrajectoryRecorder::Start(path, path.parent_path() / "artifacts",
                                                        identity, std::move(options));
    REQUIRE(opened.has_value());
    auto recorder = std::make_unique<trajectory::TrajectoryRecorder>(std::move(*opened));
    REQUIRE(recorder->WriteRunStarted({}, trajectory::Durability::PowerLoss).status ==
            trajectory::RecordReceipt::Status::Committed);
    auto turn = std::make_unique<TrajectoryTurnBridge>(*recorder, identity,
        TrajectoryTurnBridge::Identity{"demo", "responses", "subagent"});
    return TrajectorySubagentBridge::OwnV2(std::move(recorder), std::move(turn), std::move(registry));
}

void Same(const SubagentTerminalReceipt& a, const SubagentTerminalReceipt& b) {
    CHECK(a.format == b.format);
    CHECK(a.session_id == b.session_id);
    CHECK(a.run_id == b.run_id);
    CHECK(a.terminal_kind == b.terminal_kind);
    CHECK(a.execution == b.execution);
    CHECK(a.reason == b.reason);
    CHECK(a.confirmation == b.confirmation);
    CHECK(a.seal == b.seal);
    CHECK(a.broken_after_append == b.broken_after_append);
    CHECK(a.broken_after_close == b.broken_after_close);
    CHECK(a.append_error_code == b.append_error_code);
    CHECK(a.append_error_message == b.append_error_message);
    CHECK(a.close_error_code == b.close_error_code);
    CHECK(a.close_error_message == b.close_error_message);
    CHECK(a.journal_sha256 == b.journal_sha256);
    REQUIRE(a.terminal.has_value() == b.terminal.has_value());
    if (a.terminal) {
        CHECK(a.terminal->session_id == b.terminal->session_id);
        CHECK(a.terminal->run_id == b.terminal->run_id);
        CHECK(a.terminal->event_id == b.terminal->event_id);
        CHECK(a.terminal->seq == b.terminal->seq);
        CHECK(a.terminal->hash == b.terminal->hash);
    }
    REQUIRE(a.append.index() == b.append.index());
    std::visit([&](const auto& native) {
        using T = std::decay_t<decltype(native)>;
        if constexpr (!std::is_same_v<T, std::monostate>) {
            const auto& other = std::get<T>(b.append);
            CHECK(native.status == other.status);
            CHECK(native.seq == other.seq);
            CHECK(native.error_code == other.error_code);
            CHECK(native.error_message == other.error_message);
            if constexpr (std::is_same_v<T, trajectory::RecordReceipt>) {
                CHECK(native.event_id == other.event_id);
                CHECK(native.event_hash == other.event_hash);
            } else {
                CHECK(native.id == other.id);
                CHECK(native.line_hash == other.line_hash);
            }
        }
    }, a.append);
}

class TextBackend final : public api::Backend {
public:
    int calls = 0;
    std::shared_ptr<std::atomic<int>> observed_calls;
    std::function<void()> arm_terminal;
    std::expected<void, api::Error> send_stream(const api::Request&,
        const std::function<void(const api::StreamEvent&)>& emit,
        const std::atomic<bool>* = nullptr) override {
        ++calls;
        if (observed_calls) observed_calls->fetch_add(1);
        emit(api::MessageStart{"msg", "model"});
        emit(api::TextDelta{"genuine executed conclusion"});
        emit(api::ContentBlockDone{0});
        emit(api::MessageDone{"end_turn", api::Usage{}});
        if (arm_terminal) arm_terminal();
        return {};
    }
};

class ParentLoopBackend final : public api::Backend {
public:
    int parent_calls = 0, child_calls = 0, calls = 0;
    bool child_indeterminate = false;
    std::expected<void, api::Error> send_stream(const api::Request&,
        const std::function<void(const api::StreamEvent&)>& emit,
        const std::atomic<bool>* = nullptr) override {
        const int index = calls++;
        emit(api::MessageStart{"parent-child", "model"});
        if (index == 0) {
            ++parent_calls;
            emit(api::ToolUseStart{0, "actual-child-call", "agent"});
            emit(api::ToolUseInputDelta{0,
                R"({"title":"child","prompt":"finish the task","execution_mode":"foreground"})"});
            emit(api::ContentBlockDone{0});
            emit(api::ToolUseStart{1, "after-child-call", "after_child"});
            emit(api::ToolUseInputDelta{1, "{}"});
            emit(api::ContentBlockDone{1});
            emit(api::MessageDone{"tool_use", api::Usage{}});
        } else {
            if (index == 1) ++child_calls;
            else ++parent_calls;
            if (index == 1 && child_indeterminate) {
                emit(api::ToolUseStart{0, "unknown-child-call", "unknown_child"});
                emit(api::ToolUseInputDelta{0, "{}"});
                emit(api::ContentBlockDone{0});
                emit(api::MessageDone{"tool_use", api::Usage{}});
                return {};
            }
            emit(api::TextDelta{index == 1 ? "genuine child execution" : "parent final"});
            emit(api::ContentBlockDone{0});
            emit(api::MessageDone{"end_turn", api::Usage{}});
        }
        return {};
    }
};

class AfterChildTool final : public tools::Tool {
public:
    int calls = 0;
    std::string name() const override { return "after_child"; }
    std::string description() const override { return "serial side-effect probe"; }
    nlohmann::json input_schema() const override {
        return {{"type", "object"}, {"properties", nlohmann::json::object()}};
    }
    Result execute(const nlohmann::json&) override { ++calls; return {"after child", false}; }
};

class UnknownChildTool final : public tools::Tool {
public:
    int calls = 0;
    std::string name() const override { return "unknown_child"; }
    std::string description() const override { return "test actual unknown side-effect control"; }
    nlohmann::json input_schema() const override {
        return {{"type", "object"}, {"properties", nlohmann::json::object()}};
    }
    Result execute(const nlohmann::json&) override {
        ++calls;
        Result result{"child side effect cannot be confirmed", true};
        result.error_code = "test.child_indeterminate";
        result.execution_control = tools::ExecutionControl::StopIndeterminate;
        return result;
    }
};

class HarnessBackend final : public api::Backend {
public:
    int calls = 0, text_before_unknown = 0;
    std::expected<void, api::Error> send_stream(const api::Request&,
        const std::function<void(const api::StreamEvent&)>& emit,
        const std::atomic<bool>* = nullptr) override {
        emit(api::MessageStart{"harness", "model"});
        if (calls++ < text_before_unknown) {
            emit(api::TextDelta{"initial settled output"});
            emit(api::ContentBlockDone{0});
            emit(api::MessageDone{"end_turn", api::Usage{}});
        } else {
            emit(api::ToolUseStart{0, "harness-unknown", "unknown_child"});
            emit(api::ToolUseInputDelta{0, "{}"});
            emit(api::ContentBlockDone{0});
            emit(api::MessageDone{"tool_use", api::Usage{}});
        }
        return {};
    }
};

class EndgameBackend final : public api::Backend {
public:
    int calls = 0;
    bool cancel = false;
    std::expected<void, api::Error> send_stream(const api::Request&,
        const std::function<void(const api::StreamEvent&)>& emit,
        const std::atomic<bool>* = nullptr) override {
        const int call = ++calls;
        emit(api::MessageStart{"actual-endgame", "model"});
        if (cancel)
            return std::unexpected(api::Error{api::ErrorKind::Cancelled, "actual child cancelled", 0});
        emit(api::ToolUseStart{0, "budget-call-" + std::to_string(call), "after_child"});
        emit(api::ToolUseInputDelta{0, "{}"});
        emit(api::ContentBlockDone{0});
        emit(api::MessageDone{"tool_use", api::Usage{}});
        return {};
    }
};

void CheckActualForegroundEndgame(bool cancel) {
    Directory dir(cancel ? "cancel-execution" : "budget-execution");
    auto registry = std::make_shared<SubagentTerminalRegistry>();
    EndgameBackend backend;
    backend.cancel = cancel;
    tools::ToolRegistry original_registry;
    auto probe = std::make_unique<AfterChildTool>();
    auto* probe_borrow = probe.get();
    original_registry.Register(std::move(probe));
    std::optional<SubagentTerminalReceipt> callback;
    tools::AgentTool dispatch(backend, original_registry, dir.root.string(), "model", 2);
    tools::AgentTool::Hooks hooks;
    hooks.trajectory_spawn = [&](const std::string&, const std::string&, SubagentSpawnFailure*) {
        return V3Child(dir.root / "child.jsonl", registry);
    };
    hooks.trajectory_child_finished = [&](const SubagentTerminalReceipt& receipt) { callback = receipt; };
    dispatch.SetHooks(std::move(hooks));
    const auto result = dispatch.execute({{"title", "actual endgame"}, {"prompt", "run once"},
                                         {"execution_mode", "foreground"}});
    REQUIRE(callback.has_value());
    CHECK(callback->execution == (cancel ? SubagentExecutionOutcome::Cancelled
                                        : SubagentExecutionOutcome::Failed));
    REQUIRE(callback->durable()); // Durable native closure does not mean successful execution.
    CHECK(callback->reason == (cancel ? "cancelled" : "budget_exhausted"));
    CHECK(backend.calls == (cancel ? 1 : 2));
    CHECK(probe_borrow->calls == (cancel ? 0 : 2));
    CHECK(result.execution_control == tools::ExecutionControl::Continue);
    if (cancel) CHECK(result.content.find("[stopped]") != std::string::npos);
    else CHECK(result.is_error);
    const auto tasks = dispatch.TaskSnapshots();
    REQUIRE(tasks.size() == 1);
    CHECK(tasks[0].outcome.status == (cancel ? tools::TaskOutcomeStatus::Stopped
                                           : tools::TaskOutcomeStatus::BudgetExhausted));
    CHECK(tasks[0].outcome.reason == (cancel ? tools::TaskOutcomeReason::UserStop
                                           : tools::TaskOutcomeReason::StepLimitExhausted));
    const auto rows = Rows(dir.root / "child.jsonl");
    CHECK(Count(rows, "session.ended") == 1);
    CHECK(rows.back().at("payload").at("closeQuality").get<std::string>() == "incomplete");
    const auto saved = registry->Find(callback->run_id);
    REQUIRE(saved.has_value());
    Same(*callback, *saved);
    std::cout << "[child-terminal-path] foreground." << (cancel ? "cancel" : "budget") << '\n';
}

void CheckSharedHarnessUnknownRounds() {
    // First round, already-delivered continuation, and Stop continuation.
    for (int phase = 0; phase < 3; ++phase) {
        CAPTURE(phase);
        HarnessBackend backend;
        backend.text_before_unknown = phase == 0 ? 0 : 1;
        tools::ToolRegistry registry;
        auto unknown = std::make_unique<UnknownChildTool>();
        auto* unknown_borrow = unknown.get();
        registry.Register(std::move(unknown));
        agent::AgentProfile profile;
        profile.system_prompt = "harness test";
        profile.request.model = "model";
        profile.runtime.max_steps_per_turn = 4;
        agent::Agent engine(backend, registry, std::move(profile));
        int continuations = 0, restores = 0, stop_emits = 0, stop_rounds = 0;
        agent::DriveOptions options;
        if (phase != 2) options.continuation = [&]() -> std::optional<agent::ContinuationBatch> {
            ++continuations;
            return agent::ContinuationBatch{"already-delivered queued input", [&] { ++restores; }};
        };
        api::Message input;
        input.role = api::Role::User;
        input.content.push_back(api::TextBlock{"initial input"});
        auto report = agent::DriveTurn(engine, {}, std::move(input), options);
        agent::StopOptions stop;
        stop.emit = [&](bool, const std::string&) {
            ++stop_emits;
            hooks::HookEventResult result;
            result.blocked = true;
            result.block_reason = "must not force an unknown child to retry";
            return result;
        };
        stop.on_round = [&](const agent::RunOutcome&) { ++stop_rounds; };
        agent::RunStopContinuation(engine, {}, stop, report);
        REQUIRE(report.side_effect_indeterminate);
        CHECK_FALSE(report.ok);
        CHECK_FALSE(report.cancelled);
        CHECK_FALSE(report.side_effect_error.empty());
        CHECK_FALSE(report.error.empty());
        CHECK(report.steps_used == (phase == 0 ? 1 : 2));
        REQUIRE(report.final_round.has_value());
        CHECK(report.final_round->side_effect_indeterminate);
        CHECK(report.stop_reason == report.final_round->stop_reason);
        CHECK(backend.calls == (phase == 0 ? 1 : 2));
        CHECK(unknown_borrow->calls == 1);
        CHECK(continuations == (phase == 1 ? 1 : 0));
        CHECK(restores == 0);  // Unknown happened after this input reached a real request.
        CHECK(stop_emits == (phase == 2 ? 1 : 0));
        CHECK(stop_rounds == (phase == 2 ? 1 : 0));
        agent::RunStopContinuation(engine, {}, stop, report);
        CHECK(stop_emits == (phase == 2 ? 1 : 0));
        CHECK(backend.calls == (phase == 0 ? 1 : 2));
    }
}

}  // namespace

TEST_CASE("child terminal: real spawn terminal resolves five keys and linked remains initial provenance") {
    Directory dir("spawn");
    TrajectorySessionLedger::Options options;
    options.workspaces_root = dir.root / "workspaces";
    options.workspace_root = dir.root / "repo";
    std::filesystem::create_directory(options.workspace_root);
    options.lubancode_version = "test";
    auto ledger = TrajectorySessionLedger::Open(options);
    REQUIRE(ledger.has_value());
    auto child = ledger->SpawnSubagent("parent-call", "actual delegated task");
    REQUIRE(child.has_value());
    const auto receipt = (*child)->Finish(SubagentExecutionOutcome::Succeeded, "done");
    REQUIRE(receipt.durable());
    CHECK(receipt.execution == SubagentExecutionOutcome::Succeeded);
    CHECK(receipt.terminal_kind == "session.ended");
    const auto path = ledger->session_dir() / "subagents" / receipt.session_id /
                      (receipt.session_id + ".jsonl");
    REQUIRE(v3::ReadV3Ledger(path).has_value());
    REQUIRE(v3::VerifyV3File(path).ok);
    const auto rows = Rows(path);
    const auto& terminal = rows.back();
    CHECK(terminal.at("sessionId").get<std::string>() == receipt.terminal->session_id);
    CHECK(terminal.at("runId").get<std::string>() == receipt.terminal->run_id);
    CHECK(terminal.at("eventId").get<std::string>() == receipt.terminal->event_id);
    CHECK(terminal.at("seq").get<std::uint64_t>() == receipt.terminal->seq);
    CHECK(terminal.at("lineHash").get<std::string>() == receipt.terminal->hash);
    CHECK(Count(rows, "session.ended") == 1);
    const auto parent = Rows(ledger->session_dir() / (ledger->session_id() + ".jsonl"));
    auto linked = std::find_if(parent.begin(), parent.end(), [](const auto& row) {
        return row.value("kind", std::string()) == "subagent.linked";
    });
    REQUIRE(linked != parent.end());
    const auto& initial = linked->at("payload").at("childCheckpointRef");
    CHECK(initial.at("seq").get<std::uint64_t>() < receipt.terminal->seq);
    CHECK(initial.at("hash").get<std::string>() != receipt.terminal->hash);
    CHECK(Count(parent, "subagent.observed") == 0);  // Not implemented by this gate.
    const auto saved = ledger->ChildTerminalReceipt(receipt.run_id);
    REQUIRE(saved.has_value());
    Same(receipt, *saved);
}

TEST_CASE("child terminal: repeat and concurrent Finish return complete cached values without another append") {
    Directory dir("repeat");
    auto registry = std::make_shared<SubagentTerminalRegistry>();
    auto child = V3Child(dir.root / "child.jsonl", registry);
    std::vector<SubagentTerminalReceipt> copies(8);
    StartGate start;
    std::vector<std::jthread> callers;
    ReleaseGate release{start};  // Unwind releases waiters before jthread joins.
    for (std::size_t i = 0; i < copies.size(); ++i) callers.emplace_back([&, i] {
        if (!start.Wait()) return;
        copies[i] = child->Finish(SubagentExecutionOutcome::Succeeded, "first reason");
    });
    start.Release();
    callers.clear();  // Join before checking either receipt or child teardown.
    const auto first = copies[0];
    REQUIRE(first.durable());
    for (const auto& copy : copies) Same(first, copy);
    Same(first, child->Finish(SubagentExecutionOutcome::Failed, "must not replace"));
    const auto rows = Rows(dir.root / "child.jsonl");
    CHECK(Count(rows, "session.ended") == 1);
    CHECK(rows.back().at("lineHash").get<std::string>() == first.terminal->hash);
    const auto stored = registry->Find(first.run_id);
    REQUIRE(stored.has_value());
    Same(first, *stored);
    SUBCASE("V2 cancellation preserves real kind and checked digest") {
        auto v2 = V2Child(dir.root / "v2.jsonl", registry);
        const auto cancelled = v2->Finish(SubagentExecutionOutcome::Cancelled, "cancelled");
        REQUIRE(cancelled.durable());
        CHECK(cancelled.terminal_kind == "run.cancelled");
        CHECK(cancelled.journal_sha256.size() == 64);
        Same(cancelled, v2->Finish(SubagentExecutionOutcome::Succeeded, "late"));
        CHECK(trajectory::VerifyJournalFile(dir.root / "v2.jsonl").ok);
        CHECK(Count(Rows(dir.root / "v2.jsonl"), "run.cancelled") == 1);
    }
}

TEST_CASE("child terminal: native rejection has no terminal reference and cannot retry") {
    Directory dir("reject");
    auto registry = std::make_shared<SubagentTerminalRegistry>();
    SUBCASE("V3 real closed writer rejects before append") {
        auto child = V3Child(dir.root / "child.jsonl", registry, {}, true);
        const auto first = child->Finish(SubagentExecutionOutcome::Succeeded, "done");
        REQUIRE(std::holds_alternative<v3::WriteReceipt>(first.append));
        CHECK(std::get<v3::WriteReceipt>(first.append).status == v3::WriteReceipt::Status::Rejected);
        CHECK(first.confirmation == SubagentAppendConfirmation::RejectedBeforeCommit);
        CHECK(first.append_error_code == "v3writer.closed");
        CHECK_FALSE(first.broken_after_append);
        CHECK_FALSE(first.terminal.has_value());
        CHECK(first.seal == SubagentSealState::Closed);
        CHECK_FALSE(first.durable());
        Same(first, child->Finish(SubagentExecutionOutcome::Failed, "retry"));
        CHECK(Count(Rows(dir.root / "child.jsonl"), "session.ended") == 0);
    }
    SUBCASE("V2 actual validated submit rejection") {
        trajectory::RecorderOptions options;
        options.inject_submit_reject = [](trajectory::EventKind kind) -> std::optional<std::string> {
            if (kind == trajectory::EventKind::RunCompleted) return "state.test_terminal_rejected";
            return std::nullopt;
        };
        auto child = V2Child(dir.root / "child.jsonl", registry, options);
        const auto first = child->Finish(SubagentExecutionOutcome::Succeeded, "done");
        CHECK(std::get<trajectory::RecordReceipt>(first.append).status == trajectory::RecordReceipt::Status::Rejected);
        CHECK(first.append_error_code == "state.test_terminal_rejected");
        CHECK(first.confirmation == SubagentAppendConfirmation::RejectedBeforeCommit);
        CHECK_FALSE(first.broken_after_append);
        CHECK_FALSE(first.terminal.has_value());
        Same(first, child->Finish(SubagentExecutionOutcome::Failed, "retry"));
        CHECK(Count(Rows(dir.root / "child.jsonl"), "run.completed") == 0);
    }
}

TEST_CASE("child terminal: actual IO failure retains native status broken and an unconfirmed append") {
    Directory dir("io");
    auto registry = std::make_shared<SubagentTerminalRegistry>();
    SUBCASE("first V3 IO failure has native Rejected plus broken") {
        auto armed = std::make_shared<bool>(false);
        v3::V3WriterOptions options;
        options.inject_io_failure = [armed]() -> std::optional<std::string> {
            return *armed ? std::optional<std::string>("test") : std::nullopt;
        };
        auto child = V3Child(dir.root / "child.jsonl", registry, options);
        *armed = true;
        const auto first = child->Finish(SubagentExecutionOutcome::Succeeded, "done");
        CHECK(std::get<v3::WriteReceipt>(first.append).status == v3::WriteReceipt::Status::Rejected);
        CHECK(first.append_error_code == "v3writer.injected");
        CHECK(first.broken_after_append);
        CHECK(first.confirmation == SubagentAppendConfirmation::DurabilityUnconfirmed);
        CHECK_FALSE(first.terminal.has_value());
        *armed = false;
        Same(first, child->Finish(SubagentExecutionOutcome::Succeeded, "retry"));
        CHECK(Count(Rows(dir.root / "child.jsonl"), "session.ended") == 0);
    }
    SUBCASE("V2 actual IO submit failure remains IoFailed including Close") {
        trajectory::RecorderOptions options;
        options.inject_submit_reject = [](trajectory::EventKind kind) -> std::optional<std::string> {
            if (kind == trajectory::EventKind::RunCompleted) return "io.test_terminal";
            return std::nullopt;
        };
        auto child = V2Child(dir.root / "child.jsonl", registry, options);
        const auto first = child->Finish(SubagentExecutionOutcome::Succeeded, "done");
        CHECK(std::get<trajectory::RecordReceipt>(first.append).status == trajectory::RecordReceipt::Status::IoFailed);
        CHECK(first.append_error_code == "io.test_terminal");
        CHECK(first.broken_after_append);
        CHECK(first.confirmation == SubagentAppendConfirmation::DurabilityUnconfirmed);
        CHECK_FALSE(first.terminal.has_value());
        CHECK(first.seal == SubagentSealState::CloseFailed);
        CHECK(first.close_error_code == "io.close_failed");
        Same(first, child->Finish(SubagentExecutionOutcome::Succeeded, "retry"));
        CHECK(Count(Rows(dir.root / "child.jsonl"), "run.completed") == 0);
    }
    CheckSharedHarnessUnknownRounds();
}

TEST_CASE("child terminal: real Close boundary failure keeps committed five keys and blocks complete handoff") {
    Directory dir("close");
    auto registry = std::make_shared<SubagentTerminalRegistry>();
    int closes = 0;
    std::unique_ptr<TrajectorySubagentBridge> child;
    SUBCASE("V3 native checked Close") {
        v3::V3WriterOptions options;
        options.inject_close_failure = [&]() -> std::optional<std::string> {
            ++closes; return "v3writer.test_close_failed: injected after native Close";
        };
        child = V3Child(dir.root / "child.jsonl", registry, options);
    }
    SUBCASE("V2 native checked Close") {
        trajectory::RecorderOptions options;
        options.inject_close_failure = [&]() -> std::optional<std::string> {
            ++closes; return "io.test_close_failed: injected after native Close";
        };
        child = V2Child(dir.root / "child.jsonl", registry, options);
    }
    REQUIRE(child != nullptr);
    const auto first = child->Finish(SubagentExecutionOutcome::Succeeded, "done");
    CHECK(first.confirmation == SubagentAppendConfirmation::Committed);
    REQUIRE(first.terminal.has_value());
    CHECK(first.terminal->seq > 1);
    CHECK(first.terminal->hash.size() == 64);
    CHECK(first.seal == SubagentSealState::CloseFailed);
    CHECK_FALSE(first.durable());
    CHECK(closes == 1);
    Same(first, child->Finish(SubagentExecutionOutcome::Failed, "retry"));
    CHECK(closes == 1);
    const auto stored = registry->Find(first.run_id);
    REQUIRE(stored.has_value());
    Same(first, *stored);
    // Windows move is a real handle-release witness; never used to induce failure.
    const auto moved = dir.root / "closed.jsonl";
    std::filesystem::rename(dir.root / "child.jsonl", moved);
    if (first.format == SubagentJournalFormat::V3) CHECK(v3::VerifyV3File(moved).ok);
    else CHECK(trajectory::VerifyJournalFile(moved).ok);
}

TEST_CASE("child terminal: real foreground dispatch consumes a gap without erasing execution or replaying it") {
    SUBCASE("real foreground cancellation is not successful execution") {
        CheckActualForegroundEndgame(true);
    }
    SUBCASE("real foreground step budget is not successful execution") {
        CheckActualForegroundEndgame(false);
    }
    SUBCASE("actual foreground receipt and parent loop") {
        Directory dir("foreground");
        auto registry = std::make_shared<SubagentTerminalRegistry>();
        TextBackend backend;
        tools::ToolRegistry original_registry;
        std::optional<SubagentTerminalReceipt> callback;
        bool failure = false;
        bool inner_unknown = false;
        SUBCASE("actual terminal commit success") {}
        SUBCASE("actual checked Close failure after successful execution") { failure = true; }
        SUBCASE("child tool unknown survives the common harness into the parent loop") { inner_unknown = true; }
        // All captured dependencies precede AgentTool; its teardown settles before them.
        tools::AgentTool agent_tool(backend, original_registry, dir.root.string());
        tools::AgentTool::Hooks hooks;
        hooks.trajectory_spawn = [&](const std::string&, const std::string&, SubagentSpawnFailure*) {
            v3::V3WriterOptions options;
            options.inject_close_failure = [&failure]() -> std::optional<std::string> {
                return failure ? std::optional<std::string>("v3writer.test_close_failed: execution completed")
                               : std::nullopt;
            };
            return V3Child(dir.root / "child.jsonl", registry, options);
        };
        hooks.trajectory_child_finished = [&](const SubagentTerminalReceipt& receipt) { callback = receipt; };
        agent_tool.SetHooks(std::move(hooks));
        const auto result = agent_tool.execute({{"title", "actual child"}, {"prompt", "write a conclusion"}});
        CHECK(backend.calls == 1);
        REQUIRE(callback.has_value());
        CHECK(callback->execution == SubagentExecutionOutcome::Succeeded);
        CHECK(result.is_error == failure);
        CHECK(result.execution_control == (failure ? tools::ExecutionControl::StopIndeterminate
                                                  : tools::ExecutionControl::Continue));
        const auto tasks = agent_tool.coordinator()->ledger().Snapshots();
        REQUIRE(tasks.size() == 1);
        if (failure) {
            CHECK(result.error_code == "trajectory.child_terminal_persistence_failed");
            CHECK(result.content.find("genuine executed conclusion") == std::string::npos);
            CHECK_FALSE(callback->durable());
            CHECK(tasks[0].outcome.status == tools::TaskOutcomeStatus::Failed);
            CHECK(tasks[0].outcome.partial_result == "genuine executed conclusion");
            CHECK(callback->confirmation == SubagentAppendConfirmation::Committed);
            CHECK(callback->seal == SubagentSealState::CloseFailed);
            CHECK(Count(Rows(dir.root / "child.jsonl"), "session.ended") == 1);
        } else {
            CHECK(callback->durable());
            CHECK(result.content == "genuine executed conclusion");
            CHECK(tasks[0].outcome.status == tools::TaskOutcomeStatus::Completed);
            CHECK(Count(Rows(dir.root / "child.jsonl"), "session.ended") == 1);
        }
        const auto cached = registry->Find(callback->run_id);
        REQUIRE(cached.has_value());
        Same(*callback, *cached);

        // A fresh actual parent loop proves the control reaches the common loop,
        // rather than merely asserting a bit on a direct Tool::Result.
        ParentLoopBackend parent_backend;
        parent_backend.child_indeterminate = inner_unknown;
        auto parent_registry = std::make_shared<SubagentTerminalRegistry>();
        std::optional<SubagentTerminalReceipt> parent_callback;
        tools::ToolRegistry parent_tools;
        auto dispatch = std::make_unique<tools::AgentTool>(parent_backend, parent_tools, dir.root.string());
        auto* dispatch_borrow = dispatch.get();
        tools::AgentTool::Hooks parent_hooks;
        parent_hooks.trajectory_spawn = [&](const std::string&, const std::string&, SubagentSpawnFailure*) {
            v3::V3WriterOptions options;
            options.inject_close_failure = [&failure]() -> std::optional<std::string> {
                return failure ? std::optional<std::string>("v3writer.test_close_failed: parent-loop boundary")
                               : std::nullopt;
            };
            return V3Child(dir.root / "parent-child.jsonl", parent_registry, options);
        };
        parent_hooks.trajectory_child_finished = [&](const SubagentTerminalReceipt& receipt) {
            parent_callback = receipt;
        };
        dispatch->SetHooks(std::move(parent_hooks));
        parent_tools.Register(std::move(dispatch));
        auto after = std::make_unique<AfterChildTool>();
        auto* after_borrow = after.get();
        parent_tools.Register(std::move(after));
        auto unknown = std::make_unique<UnknownChildTool>();
        auto* unknown_borrow = unknown.get();
        parent_tools.Register(std::move(unknown));
        agent::AgentProfile profile;
        profile.request.model = "parent-model";
        profile.system_prompt = "parent system";
        profile.runtime.max_steps_per_turn = 4;
        agent::Agent parent_agent(parent_backend, parent_tools, std::move(profile));
        agent::TurnWiring turn;
        const auto parent_outcome = parent_agent.Run("delegate once", turn);
        REQUIRE(parent_outcome.has_value());
        const bool stopped = failure || inner_unknown;
        CHECK(parent_outcome->side_effect_indeterminate == stopped);
        CHECK(parent_backend.child_calls == 1);
        CHECK(parent_backend.parent_calls == (stopped ? 1 : 2));
        CHECK(after_borrow->calls == (stopped ? 0 : 1));
        CHECK(unknown_borrow->calls == (inner_unknown ? 1 : 0));
        const auto fresh_tasks = dispatch_borrow->TaskSnapshots();
        REQUIRE(fresh_tasks.size() == 1);
        CHECK(fresh_tasks[0].outcome.status == (stopped ? tools::TaskOutcomeStatus::Failed
                                                      : tools::TaskOutcomeStatus::Completed));
        REQUIRE(parent_callback.has_value());
        CHECK(parent_callback->execution == (inner_unknown ? SubagentExecutionOutcome::Indeterminate
                                                          : SubagentExecutionOutcome::Succeeded));
        CHECK(parent_callback->durable() == !failure);
        if (inner_unknown) {
            CHECK(parent_callback->reason == "side_effect_indeterminate");
            CHECK(Rows(dir.root / "parent-child.jsonl").back().at("payload").at("reason").get<std::string>() ==
                  "side_effect_indeterminate");
        }
        std::cout << "[child-terminal-path] foreground."
                  << (inner_unknown ? "child-unknown" : failure ? "close-failed" : "success") << '\n';
    }
}

TEST_CASE("child terminal: startup rejection consumes real failure and shared registry outlives its former parent") {
    Directory dir("startup");
    auto old_registry = std::make_shared<SubagentTerminalRegistry>();
    auto parent_registry = old_registry;
    TextBackend backend;
    tools::ToolRegistry original_registry;
    std::optional<SubagentTerminalReceipt> startup_callback;
    int finished_callbacks = 0;
    bool close_failure = true;
    bool reject_before_commit = false;
    SUBCASE("committed startup failure with unconfirmed Close stops the parent") {}
    SUBCASE("native already-closed rejection has no child execution and permits ordinary failure") {
        close_failure = false;
        reject_before_commit = true;
    }
    SUBCASE("durable startup failure permits ordinary failure") { close_failure = false; }
    tools::AgentTool agent_tool(backend, original_registry, dir.root.string());
    auto model_calls = std::make_shared<std::atomic<int>>(0);
    agent_tool.SetDetachedBackendFactory([model_calls] {
        tools::DetachedAgentBackend detached;
        auto backend = std::make_unique<TextBackend>();
        backend->observed_calls = model_calls;
        detached.backend = std::move(backend);
        detached.request_profile.model = "startup-model";
        return detached;
    });
    tools::AgentTool::Hooks hooks;
    hooks.trajectory_spawn = [&](const std::string&, const std::string&, SubagentSpawnFailure*) {
        v3::V3WriterOptions options;
        options.inject_close_failure = [&]() -> std::optional<std::string> {
            return close_failure ? std::optional<std::string>("v3writer.test_close_failed: startup boundary")
                                 : std::nullopt;
        };
        auto child = V3Child(dir.root / "child.jsonl", parent_registry, options, reject_before_commit);
        parent_registry = std::make_shared<SubagentTerminalRegistry>();
        return child;
    };
    hooks.trajectory_child_finished = [&](const SubagentTerminalReceipt& receipt) {
        ++finished_callbacks; startup_callback = receipt;
    };
    agent_tool.SetHooks(std::move(hooks));
    agent_tool.SetBackgroundThreadFactoryForTesting([](tools::AgentTaskCoordinator::ThreadBody) -> std::thread {
        throw std::system_error(std::make_error_code(std::errc::resource_unavailable_try_again));
    });
    const auto result = agent_tool.execute({{"title", "startup child"}, {"prompt", "must not run"},
                                            {"run_in_background", true}});
    REQUIRE(result.is_error);
    CHECK(result.error_code == "agent.thread_start_failed");
    CHECK(result.execution_control == (close_failure ? tools::ExecutionControl::StopIndeterminate
                                                    : tools::ExecutionControl::Continue));
    CHECK((result.content.find("trajectory.child_terminal_persistence_failed") != std::string::npos) ==
          (close_failure || reject_before_commit));
    CHECK(backend.calls == 0);
    CHECK(model_calls->load() == 0);
    const auto receipt = old_registry->Find("agent-receipt");
    REQUIRE(receipt.has_value());
    CHECK(finished_callbacks == 1);
    REQUIRE(startup_callback.has_value());
    Same(*receipt, *startup_callback);
    CHECK(receipt->execution == SubagentExecutionOutcome::StartupRejected);
    CHECK(receipt->confirmation == (reject_before_commit ? SubagentAppendConfirmation::RejectedBeforeCommit
                                                       : SubagentAppendConfirmation::Committed));
    CHECK(receipt->seal == (close_failure ? SubagentSealState::CloseFailed : SubagentSealState::Closed));
    CHECK(receipt->terminal.has_value() == !reject_before_commit);
    CHECK(receipt->durable() == (!close_failure && !reject_before_commit));
    if (reject_before_commit) {
        REQUIRE(std::holds_alternative<v3::WriteReceipt>(receipt->append));
        CHECK(std::get<v3::WriteReceipt>(receipt->append).status == v3::WriteReceipt::Status::Rejected);
    }
    CHECK_FALSE(parent_registry->Find(receipt->run_id).has_value());
    CHECK(Count(Rows(dir.root / "child.jsonl"), "session.ended") == (reject_before_commit ? 0 : 1));
    const auto tasks = agent_tool.coordinator()->ledger().Snapshots();
    REQUIRE(tasks.size() == 1);
    CHECK(tasks[0].outcome.reason == tools::TaskOutcomeReason::InitializationFailed);
    CHECK_FALSE(agent_tool.coordinator()->ledger().HasRunningTasks());
    // Different native child owners publish concurrently to this shared table;
    // reading returns independent snapshots and never touches a parent Impl.
    std::vector<std::unique_ptr<TrajectorySubagentBridge>> children;
    for (int i = 0; i < 4; ++i)
        children.push_back(V3Child(dir.root / ("registry-" + std::to_string(i) + ".jsonl"),
            old_registry, {}, false, "agent-registry-" + std::to_string(i)));
    StartGate start;
    std::vector<std::jthread> publishers;
    ReleaseGate release{start};
    for (std::size_t i = 0; i < children.size(); ++i) publishers.emplace_back([&, i] {
        if (!start.Wait()) return;
        (void)children[i]->Finish(SubagentExecutionOutcome::Succeeded, "concurrent native child");
    });
    start.Release();
    for (int i = 0; i < 128; ++i) (void)old_registry->Find("agent-registry-" + std::to_string(i % 4));
    publishers.clear();
    for (std::size_t i = 0; i < children.size(); ++i) {
        const auto saved = old_registry->Find("agent-registry-" + std::to_string(i));
        REQUIRE(saved.has_value());
        REQUIRE(saved->durable());
        Same(*saved, children[i]->Finish(SubagentExecutionOutcome::Failed, "must stay cached"));
        CHECK_FALSE(parent_registry->Find(saved->run_id).has_value());
    }
    children.clear();
    // Return-by-value snapshots remain independent after the shared owner is gone.
    const auto owned = *receipt;
    old_registry.reset();
    Same(owned, *receipt);
}
