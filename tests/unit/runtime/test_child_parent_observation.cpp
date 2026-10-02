#include <doctest/doctest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <iterator>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "agent/agent.hpp"
#include "api/chat/request.hpp"
#include "platform/paths.hpp"
#include "platform/sha256.hpp"
#include "runtime/action_summary.hpp"
#include "runtime/tool_trace_hub.hpp"
#include "runtime/trajectory_subagent_bridge.hpp"
#include "tools/agent_tool.hpp"
#include "trajectory/v3/reader.hpp"
#include "trajectory/v3/result_store.hpp"
#include "trajectory/v3/subagent.hpp"
#include "trajectory/v3/tool_action.hpp"

namespace {
using namespace lubancode;
namespace v3 = trajectory::v3;

struct Directory {
    std::filesystem::path root;
    Directory() {
        static std::atomic<unsigned> serial{0};
        root = std::filesystem::temp_directory_path() /
            ("child-parent-observation-" + std::to_string(
                std::chrono::steady_clock::now().time_since_epoch().count()) + "-" +
             std::to_string(++serial));
        REQUIRE(std::filesystem::create_directories(root));
    }
    ~Directory() { std::error_code error; std::filesystem::remove_all(root, error); }
};

void Write(const std::filesystem::path& path, const std::string& bytes) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    REQUIRE(output.is_open());
    output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    output.close();
    REQUIRE_FALSE(output.fail());
}

std::string Read(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    REQUIRE(input.is_open());
    std::string bytes((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
    REQUIRE_FALSE(input.bad());
    return bytes;
}

std::size_t Count(const v3::V3Ledger& ledger, v3::EventKindV3 kind) {
    return static_cast<std::size_t>(std::count_if(ledger.events.begin(), ledger.events.end(),
        [kind](const auto& event) { return event.kind == kind; }));
}

api::Message User(const std::string& text) {
    api::Message message;
    message.role = api::Role::User;
    message.content.push_back(api::TextBlock{text});
    return message;
}

class Probe final : public tools::Tool {
public:
    int calls = 0;
    std::string tool_name, text;
    explicit Probe(std::string name, std::string result = "probe result")
        : tool_name(std::move(name)), text(std::move(result)) {}
    std::string name() const override { return tool_name; }
    std::string description() const override { return "native foreground test probe"; }
    nlohmann::json input_schema() const override {
        return {{"type", "object"}, {"properties", nlohmann::json::object()}};
    }
    Result execute(const nlohmann::json&) override { ++calls; return {text, false}; }
};

class Backend final : public api::Backend {
public:
    int parent_calls = 0, child_calls = 0, summary_calls = 0;
    int parent_dispatches = 1;
    bool large_first = false, large_after = false;
    std::atomic<bool>* cancel_child = nullptr;
    std::vector<api::Request> parent_requests;
    std::string SerializeForDiagnostics(const api::Request& request) const override {
        return api::chat::BuildRequestJson(request, nlohmann::json::object()).dump();
    }
    std::expected<void, api::Error> send_stream(const api::Request& request,
        const std::function<void(const api::StreamEvent&)>& emit, const std::atomic<bool>*) override {
        emit(api::MessageStart{"observation-response", request.model});
        if (request.model == "child-model") {
            ++child_calls;
            emit(api::TextDelta{"actual child conclusion"});
            emit(api::ContentBlockDone{0});
            emit(api::MessageDone{"end_turn", api::Usage{}});
            if (cancel_child) cancel_child->store(true);
        } else if (request.system.starts_with("Summarize already-executed tool evidence.") ||
                   request.model == "summary-model") {
            ++summary_calls;
            emit(api::TextDelta{R"({"summary":"stored output","side_effects":["none"],"open_items":[],"evidence":["combined"]})"});
            emit(api::ContentBlockDone{0});
            emit(api::MessageDone{"end_turn", api::Usage{}});
        } else {
            parent_requests.push_back(request);
            const int call = ++parent_calls;
            if (call <= parent_dispatches) {
                int block = 0;
                if (large_first) {
                    emit(api::ToolUseStart{block, "large-first", "large_result"});
                    emit(api::ToolUseInputDelta{block, "{}"});
                    emit(api::ContentBlockDone{block++});
                }
                emit(api::ToolUseStart{block, "reused-provider", "agent"});
                emit(api::ToolUseInputDelta{block,
                    R"({"title":"real child","prompt":"produce one conclusion","execution_mode":"foreground"})"});
                emit(api::ContentBlockDone{block++});
                if (large_after) {
                    emit(api::ToolUseStart{block, "large-after", "large_result"});
                    emit(api::ToolUseInputDelta{block, "{}"});
                    emit(api::ContentBlockDone{block++});
                }
                emit(api::ToolUseStart{block, "after-child", "after_child"});
                emit(api::ToolUseInputDelta{block, "{}"});
                emit(api::ContentBlockDone{block});
                emit(api::MessageDone{"tool_use", api::Usage{}});
            } else {
                emit(api::TextDelta{"parent final"});
                emit(api::ContentBlockDone{0});
                emit(api::MessageDone{"end_turn", api::Usage{}});
            }
        }
        return {};
    }
};

enum class Failure { None, ObservationIo, CaptureFile, CaptureTempDirectory, BadResultName, MissingChild, ChildRootAlias };

struct Rig {
    const Directory& directory;
    Backend backend;
    tools::ToolRegistry child_tools;
    std::shared_ptr<std::atomic<bool>> io_armed = std::make_shared<std::atomic<bool>>(false);
    std::atomic<bool> cancel{false};
    std::unique_ptr<v3::V3Writer> writer;
    runtime::V3SessionBooks books;
    std::unique_ptr<runtime::TrajectoryTurnBridge> bridge;
    runtime::IdAuthority ids;
    runtime::ToolTraceHub hub{ids};
    tools::AgentTool* dispatch = nullptr;
    Probe* after = nullptr;
    Probe* large = nullptr;
    Failure failure = Failure::None;
    bool reject_attach = false, close_failure = false, bypass_rewrite = false;
    bool force_child_preview_failure = false;
    unsigned child_counter = 0, close_calls = 0;
    std::vector<runtime::SubagentTerminalReceipt> terminals;
    std::vector<std::filesystem::path> child_paths;
    std::vector<runtime::SubagentSpawnProvenance> child_sources;
    std::vector<runtime::ToolResultsCommitReceipt> captures;
    std::vector<runtime::ToolResultsCommitReceipt> repeated_captures;
    std::optional<runtime::SubagentSpawnFailure> rejected;
    std::string raw_child_text;
    std::string removed_child_bytes;
    std::unique_ptr<Directory> external;
    // Destroy the Agent and actual AgentTool, including callbacks borrowing this
    // fixture, before their capture vectors and parent bridge/books/writer.
    tools::ToolRegistry parent_tools;
    std::unique_ptr<agent::Agent> parent;

    explicit Rig(const Directory& dir) : directory(dir) {
        v3::V3WriterOptions options;
        options.inject_io_failure = [armed = io_armed]() -> std::optional<std::string> {
            if (armed->exchange(false)) return "armed native observation append";
            return std::nullopt;
        };
        auto opened = v3::V3Writer::Start(dir.root / "parent.jsonl", "parent-session", "main-1",
            "parent system", nlohmann::json::object(), options);
        REQUIRE_MESSAGE(opened.has_value(), (opened ? std::string() : opened.error()));
        writer = std::make_unique<v3::V3Writer>(std::move(*opened));
        books.writer = writer.get();
        books.system_content = "parent system";
        trajectory::EventScope scope;
        scope.session_id = writer->session_id(); scope.run_id = writer->run_id();
        scope.workspace_key = "observation-workspace";
        runtime::TrajectoryTurnBridge::Identity identity;
        identity.provider = "test"; identity.wire = "openai-chat-completions";
        bridge = std::make_unique<runtime::TrajectoryTurnBridge>(writer.get(), &books, scope, identity);
        hub.AttachTrajectory(bridge.get());
        auto tool = std::make_unique<tools::AgentTool>(backend, child_tools,
            platform::PathToUtf8(dir.root), "child-model", 4);
        dispatch = tool.get();
        tools::AgentTool::Hooks hooks;
        hooks.trajectory_spawn = [this](const std::string& label, const std::string& parent_run,
            runtime::SubagentSpawnFailure* failed, runtime::SubagentDispatchMode mode) {
            REQUIRE(parent_run.empty());
            REQUIRE(mode == runtime::SubagentDispatchMode::Foreground);
            const auto call = hub.current_agent_call_id();
            const auto origin = bridge->V3DeclaredCallOrigin(call);
            REQUIRE(origin.has_value());
            v3::ParentActionRef parent_ref{writer->session_id(), writer->run_id(),
                origin->turn_id, origin->step_id, origin->action_id, origin->message_id};
            const std::string child_id = "child-" + std::to_string(++child_counter);
            v3::ChildSessionRef child{child_id, "agent-" + std::to_string(child_counter),
                "subagents/" + child_id + "/" + child_id + ".jsonl"};
            auto spawn = v3::SubagentSpawn::Request(*writer, origin->action_id, origin->turn_id,
                origin->step_id, writer->NewTaskId(), child, parent_ref,
                {{"taskLabel", label}}, nlohmann::json::object());
            REQUIRE(spawn.requested_receipt().status == v3::WriteReceipt::Status::Committed);
            const auto fault = [this]() -> std::optional<std::string> {
                ++close_calls;
                return close_failure ? std::optional<std::string>("v3writer.test_child_close_failed") : std::nullopt;
            };
            auto boot = spawn.BootstrapChild(*writer, child.run_id, "parent system", label,
                trajectory::Durability::PowerLoss, fault);
            REQUIRE_MESSAGE(boot.error.empty(), boot.error);
            REQUIRE(boot.child_writer.has_value());
            REQUIRE(spawn.Link(*writer, boot.checkpoint).status == v3::WriteReceipt::Status::Committed);
            runtime::SubagentSpawnProvenance source;
            source.parent_action = spawn.parent_action(); source.child = spawn.child();
            source.task_id = spawn.task_id(); source.native_spawn = spawn.requested_receipt();
            source.spawn = {writer->session_id(), writer->run_id(), source.native_spawn.id,
                source.native_spawn.seq, source.native_spawn.line_hash};
            source.parent_journal = writer->path();
            auto child_writer = std::make_unique<v3::V3Writer>(std::move(*boot.child_writer));
            child_paths.push_back(child_writer->path()); child_sources.push_back(source);
            auto child_books = std::make_unique<runtime::V3SessionBooks>();
            child_books->system_content = "parent system";
            auto child_scope = scope_for_child(child);
            auto child_bridge = std::make_unique<runtime::TrajectoryTurnBridge>(
                child_writer.get(), child_books.get(), child_scope,
                runtime::TrajectoryTurnBridge::Identity{"test", "openai-chat-completions", "subagent", {}});
            auto owned = runtime::TrajectorySubagentBridge::OwnV3(std::move(child_writer),
                std::move(child_books), std::move(child_bridge), bridge->child_terminal_registry(), source);
            auto attached_source = *owned->ParentSpawn();
            if (reject_attach) attached_source.parent_action.turn_id = "foreign-turn";
            const auto attached = bridge->AttachChildRun(call, owned->run_id(), attached_source);
            if (!attached) {
                runtime::SubagentSpawnFailure refusal;
                refusal.stage = "attach_parent"; refusal.error_code = attached.error();
                refusal.reserved_run_id = owned->run_id();
                refusal.cleanup_receipt = owned->Finish(runtime::SubagentExecutionOutcome::StartupRejected, attached.error());
                terminals.push_back(*refusal.cleanup_receipt);
                rejected = refusal;
                if (failed) *failed = refusal;
                return std::unique_ptr<runtime::TrajectorySubagentBridge>{};
            }
            return owned;
        };
        hooks.trajectory_child_finished = [this, registry = bridge->child_terminal_registry()](const auto& value) {
            terminals.push_back(value); registry->Store(value);
        };
        dispatch->SetHooks(std::move(hooks));
        parent_tools.Register(std::move(tool));
        auto after_owner = std::make_unique<Probe>("after_child"); after = after_owner.get();
        parent_tools.Register(std::move(after_owner));
        auto large_owner = std::make_unique<Probe>("large_result", std::string(200000, 'L'));
        large = large_owner.get(); parent_tools.Register(std::move(large_owner));
        agent::AgentProfile profile;
        profile.request.model = "parent-model"; profile.system_prompt = "parent system";
        profile.runtime.max_steps_per_turn = 6;
        parent = std::make_unique<agent::Agent>(backend, parent_tools, std::move(profile));
    }
    static trajectory::EventScope scope_for_child(const v3::ChildSessionRef& child) {
        trajectory::EventScope scope;
        scope.workspace_key = "observation-workspace";
        scope.session_id = child.session_id; scope.run_id = child.run_id;
        return scope;
    }
    std::expected<agent::RunOutcome, std::string> Run() {
        const auto turn_id = writer->NewTurnId();
        const std::string input = "delegate under current parent " + turn_id;
        bridge->BeginTurn(turn_id, "external_user");
        bridge->RecordInput(User(input));
        agent::TurnWiring wiring;
        hub.Install(*parent, wiring, writer->session_id(), turn_id);
        wiring.boundary_recorder = bridge.get(); wiring.turn_id = turn_id;
        const auto capture = wiring.capture_tool_result;
        wiring.capture_tool_result = [this, capture](const api::ToolResultBlock& result) {
            if (result.tool_use_id == "reused-provider") {
                raw_child_text = result.content;
                if (failure == Failure::ObservationIo) io_armed->store(true);
                if (failure == Failure::CaptureFile) Write(directory.root / "artifacts", "not a directory");
                if (failure == Failure::CaptureTempDirectory) {
                    // The large sibling already owns capture-000001. Refuse
                    // the child's actual immutable-file temporary open, while
                    // keeping the parent writer and sibling artifacts healthy.
                    REQUIRE(std::filesystem::create_directory(directory.root / "artifacts" /
                        "capture-000002.combined.txt.tmp"));
                }
                if (failure == Failure::BadResultName) {
                    const bool artifact_dir_ready =
                        std::filesystem::create_directories(directory.root / "artifacts") ||
                        std::filesystem::is_directory(directory.root / "artifacts");
                    REQUIRE(artifact_dir_ready);
                    Write(directory.root / "artifacts" / "res-999999999999999999999999999999.json", "{}\n");
                }
                if (failure == Failure::MissingChild) {
                    REQUIRE(child_paths.size() == 1);
                    removed_child_bytes = Read(child_paths.front());
                    REQUIRE(std::filesystem::remove(child_paths.front()));
                }
                if (failure == Failure::ChildRootAlias) {
                    external = std::make_unique<Directory>();
                    std::filesystem::rename(directory.root / "subagents", external->root / "subagents");
                    std::error_code error;
                    std::filesystem::create_directory_symlink(external->root / "subagents", directory.root / "subagents", error);
                    REQUIRE_FALSE(error);
                }
            }
            const auto receipt = capture(result);
            if (result.tool_use_id == "reused-provider") {
                captures.push_back(receipt);
                // The first-attempt cache belongs to this active turn. Test it
                // before EndTurn; no retired-turn capture API is promised.
                const auto before = writer->next_seq();
                repeated_captures.push_back(capture(result));
                CHECK(writer->next_seq() == before);
            }
            return receipt;
        };
        if (bypass_rewrite) wiring.rewrite_tool_results_for_history = {};
        else if (force_child_preview_failure) {
            const auto rewrite = wiring.rewrite_tool_results_for_history;
            wiring.rewrite_tool_results_for_history = [rewrite](api::Message& results) {
                // Supply a genuine unrepresentable preview budget, after raw
                // capture and before the real native persistence/selection path.
                for (auto& block : results.content)
                    if (auto* value = std::get_if<api::ToolResultBlock>(&block);
                        value && value->tool_use_id == "reused-provider") value->preview_budget_bytes = 1;
                return rewrite(results);
            };
        }
        const auto result = parent->Run(input, wiring, &cancel);
        bridge->EndTurn(result && !result->side_effect_indeterminate, result && result->cancelled,
            result && result->side_effect_indeterminate ? "side_effect_indeterminate" : "finished");
        return result;
    }
    v3::V3Ledger Source() const {
        const auto source = v3::ReadV3Ledger(writer->path());
        REQUIRE_MESSAGE(source.has_value(), (source ? std::string() : source.error()));
        return *source;
    }
    void CheckChild(std::size_t index = 0) const {
        REQUIRE(terminals.size() > index);
        const auto& value = terminals[index];
        REQUIRE(value.terminal.has_value());
        const auto child = v3::ReadV3Ledger(child_paths[index]);
        REQUIRE_MESSAGE(child.has_value(), (child ? std::string() : child.error()));
        const auto* ended = child->FindEvent(value.terminal->event_id);
        REQUIRE(ended != nullptr);
        CHECK(ended->kind == v3::EventKindV3::SessionEnded);
        CHECK(child->session_id == value.terminal->session_id);
        CHECK(child->run_id == value.terminal->run_id);
        CHECK(ended->seq == value.terminal->seq);
        CHECK(ended->line_hash == value.terminal->hash);
        const auto& provenance = child_sources[index];
        const auto parent_source = Source();
        const auto* requested = parent_source.FindEvent(provenance.spawn.event_id);
        REQUIRE(requested != nullptr);
        CHECK(requested->kind == v3::EventKindV3::SubagentSpawnRequested);
        CHECK(requested->session_id == provenance.spawn.session_id);
        CHECK(requested->run_id == provenance.spawn.run_id);
        CHECK(requested->seq == provenance.spawn.seq);
        CHECK(requested->line_hash == provenance.spawn.hash);
        CHECK(requested->payload.at("parentActionRef") == provenance.parent_action.ToJson());
        const auto cached = bridge->child_terminal_registry()->Find(value.run_id);
        REQUIRE(cached.has_value());
        REQUIRE(cached->terminal.has_value());
        CHECK(cached->execution == value.execution);
        CHECK(cached->confirmation == value.confirmation);
        CHECK(cached->seal == value.seal);
        CHECK(cached->terminal->hash == value.terminal->hash);
    }
    void CheckStopped(const std::expected<agent::RunOutcome, std::string>& result) const {
        REQUIRE_MESSAGE(result.has_value(), (result ? std::string() : result.error()));
        CHECK(result->side_effect_indeterminate);
        CHECK_FALSE(result->side_effect_error.empty());
        CHECK(backend.parent_calls == 1);
        CHECK(backend.child_calls == 1);
        CHECK(backend.summary_calls == 0);
        CHECK(after->calls == 0);
        REQUIRE(terminals.size() == 1);
        CHECK(terminals.front().execution == runtime::SubagentExecutionOutcome::Succeeded);
        CHECK(terminals.front().durable());
        CheckChild();
        REQUIRE(captures.size() == 1);
        CHECK_FALSE(captures.front().ok());
        CHECK(captures.front().side_effect_indeterminate);
        REQUIRE(repeated_captures.size() == 1);
        CHECK(repeated_captures.front().error_code == captures.front().error_code);
        CHECK(repeated_captures.front().side_effect_indeterminate);
        CHECK(backend.child_calls == 1);
    }
};

void CheckLedgerOnlySummary(const Directory& directory) {
    auto opened = v3::V3Writer::Start(directory.root / "summary-child.jsonl",
        "summary-child", "summary-run", "summary child system");
    REQUIRE(opened.has_value());
    auto writer = std::move(*opened);
    runtime::ActionSummarySource material;
    material.action_id = writer.NewActionId(); material.parent_turn_id = writer.NewTurnId();
    material.text.assign(8192, 'x'); material.execution_state = "done"; material.budget_bytes = 2048;
    const auto step = writer.NewStepId();
    auto action = v3::ToolActionSession::Admit(writer, material.parent_turn_id, step,
        material.action_id, "queued", std::nullopt, "summary-source");
    REQUIRE(action.Start(writer, "args-ref", {"read", "builtin", "1", "test"}).status == v3::WriteReceipt::Status::Committed);
    const auto finished = action.Finish(writer, 0);
    REQUIRE(finished.status == v3::WriteReceipt::Status::Committed);
    auto store = v3::ResultStore::Open(directory.root);
    REQUIRE(store.has_value());
    v3::ResultStore::PersistRequest request;
    request.content = material.text; request.result_kind = "text";
    request.execution_event_ref = finished.id; request.tool_call_id = material.action_id;
    request.outputs.push_back({"combined", "text/plain", material.text, true, "", material.text.size(), false});
    const auto saved = store->Persist(request); REQUIRE(saved.ok);
    material.result_refs = saved.result_ref;
    const auto persisted = action.PersistedResult(writer, saved.result_ref, finished.id);
    REQUIRE(persisted.status == v3::WriteReceipt::Status::Committed);
    material.persisted_event_ref = persisted.id;
    runtime::ActionSummaryProfile profile;
    profile.provider = "test"; profile.wire = "openai-chat-completions"; profile.model = "summary-model";
    Backend backend; int remaining = 8;
    const auto summary = runtime::SummarizeActionResult(writer, backend, profile, material, remaining);
    REQUIRE_MESSAGE(summary.accepted, summary.reason); CHECK(backend.summary_calls == 1);
    const auto selected = action.SelectResult(writer, {persisted.id}, {}, "done", std::nullopt,
        trajectory::Durability::PowerLoss, summary.terminal_event_ref);
    REQUIRE(selected.status == v3::WriteReceipt::Status::Committed);
    REQUIRE(action.AppendToolMessage(writer, summary.text, selected.id).status == v3::WriteReceipt::Status::Committed);
    const auto tool_message = writer.context().chain.back().message_ref;
    const auto path = writer.path(); REQUIRE(writer.Close().has_value());
    const auto before = v3::ReadV3Ledger(path); REQUIRE(before.has_value());
    CHECK(v3::ExpandResultPreview(*before, directory.root, tool_message).complete);
    // Keep a valid, genuinely adopted action-summary ledger, but replace its
    // external result files. The bounded projection must use ledger bytes only;
    // the ordinary expansion still reports their actual hash gaps.
    for (const auto& ref : saved.result_ref)
        Write(directory.root / platform::Utf8ToPath(ref.at("path").get<std::string>()), std::string(1024 * 1024, 'z'));
    const auto bytes = Read(path);
    const auto bounded = v3::ReadV3LedgerBounded(path, bytes.size(), 131072, 4 * 1024 * 1024);
    REQUIRE_MESSAGE(bounded.has_value(), (bounded ? std::string() : bounded.error()));
    const auto projected = v3::ProjectResultPreview(*bounded, tool_message);
    CHECK(projected.summary_valid); CHECK(projected.summary_event_ref == summary.terminal_event_ref);
    CHECK(projected.artifacts.empty()); CHECK_FALSE(projected.result_refs.empty());
    const auto expanded = v3::ExpandResultPreview(*bounded, directory.root, tool_message);
    CHECK_FALSE(expanded.complete); REQUIRE_FALSE(expanded.artifacts.empty());
    CHECK(std::all_of(expanded.artifacts.begin(), expanded.artifacts.end(), [](const auto& item) {
        return item.exists && !item.hash_ok && item.gap_reason == "hash_mismatch";
    }));
    CHECK(backend.summary_calls == 1); CHECK(backend.parent_calls == 0); CHECK(backend.child_calls == 0);
    std::cout << "[child-observation-path] ledger-summary\n";
}

} // namespace

TEST_CASE("child parent observation reaches the next real prepared request through local results") {
    Directory directory; Rig rig(directory);
    const auto result = rig.Run();
    REQUIRE_MESSAGE(result.has_value(), (result ? std::string() : result.error()));
    CHECK_FALSE(result->side_effect_indeterminate);
    CHECK(rig.backend.parent_calls == 2); CHECK(rig.backend.child_calls == 1);
    CHECK(rig.after->calls == 1); rig.CheckChild();
    const auto source = rig.Source();
    REQUIRE(Count(source, v3::EventKindV3::SubagentObserved) == 1);
    const auto observed = std::find_if(source.events.begin(), source.events.end(), [](const auto& event) {
        return event.kind == v3::EventKindV3::SubagentObserved;
    });
    REQUIRE(observed != source.events.end());
    const auto& ref = *rig.terminals.front().terminal;
    CHECK(observed->payload.at("childCheckpointRef").size() == 4);
    CHECK(observed->payload.at("childCheckpointRef").at("sessionId").get<std::string>() == ref.session_id);
    CHECK(observed->payload.at("childCheckpointRef").at("runId").get<std::string>() == ref.run_id);
    CHECK(observed->payload.at("childCheckpointRef").at("seq").get<std::uint64_t>() == ref.seq);
    CHECK(observed->payload.at("childCheckpointRef").at("lineHash").get<std::string>() == ref.hash);
    CHECK(observed->payload.at("display").at("terminalRef").at("id").get<std::string>() == ref.event_id);
    CHECK(observed->payload.at("display").at("rawTextSha256").get<std::string>() == platform::Sha256Hex(rig.raw_child_text));
    const auto actions = v3::FoldToolActions(source);
    const auto action = std::find_if(actions.begin(), actions.end(), [&](const auto& entry) {
        return entry.tool_call_id == *observed->action_id;
    });
    REQUIRE(action != actions.end());
    const v3::EventLine* selected = nullptr;
    const v3::MessageLine* tool = nullptr;
    for (const auto& event : source.events)
        if (event.kind == v3::EventKindV3::ToolResultSelected && event.action_id == observed->action_id) selected = &event;
    for (const auto& message : source.messages)
        if (message.action_id == observed->action_id && message.message.value("role", std::string()) == "tool") tool = &message;
    REQUIRE(selected != nullptr); REQUIRE(tool != nullptr);
    CHECK(observed->seq < selected->seq); CHECK(selected->seq < tool->seq);
    REQUIRE(tool->result_selection_ref.has_value()); CHECK(*tool->result_selection_ref == selected->event_id);
    for (const auto& parent_ref : selected->payload.at("sourceResultEventRefs")) {
        REQUIRE(parent_ref.is_string());
        const auto* persisted = source.FindEvent(parent_ref.get<std::string>());
        REQUIRE(persisted != nullptr);
        CHECK(persisted->kind == v3::EventKindV3::ToolResultPersisted);
        CHECK(persisted->action_id == observed->action_id);
        CHECK(persisted->session_id == source.session_id);
        CHECK(persisted->seq > observed->seq);
    }
    const v3::EventLine* prepared = nullptr;
    for (const auto& event : source.events)
        if (event.kind == v3::EventKindV3::ModelRequestPrepared && event.seq > tool->seq) prepared = &event;
    REQUIRE(prepared != nullptr);
    CHECK(v3::CheckPreparedAgainstChain(source, prepared->event_id).empty());
    const auto& inputs = prepared->payload.at("inputMessageRefs");
    CHECK(std::any_of(inputs.begin(), inputs.end(), [&](const auto& input) {
        return input.is_string() && input.template get<std::string>() == tool->message_id;
    }));
    REQUIRE(rig.backend.parent_requests.size() == 2);
    std::size_t actual = 0;
    for (const auto& message : rig.backend.parent_requests.back().messages)
        for (const auto& block : message.content)
            if (const auto* item = std::get_if<api::ToolResultBlock>(&block);
                item && item->tool_use_id == "reused-provider" && item->content == tool->message.at("content").get<std::string>()) ++actual;
    CHECK(actual == 1);
    std::cout << "[child-observation-path] adopted\n";
}

TEST_CASE("child unknown stops summary calls while healthy large-sibling summaries remain available") {
    int path = 0;
    SUBCASE("native observation IO") { path = 0; }
    SUBCASE("actual child capture failure after a large sibling") { path = 1; }
    SUBCASE("healthy large sibling still uses ordinary action summaries") { path = 2; }
    Directory directory; Rig rig(directory);
    if (path == 0) rig.failure = Failure::ObservationIo;
    else {
        rig.backend.large_first = true;
        rig.parent->SetContextWindowTokens(32768);
        if (path == 1) rig.failure = Failure::CaptureTempDirectory;
    }
    const auto result = rig.Run();
    if (path < 2) {
        rig.CheckStopped(result);
        if (path == 0) {
            CHECK(result->side_effect_error.find("subagent.observation.append_failed") != std::string::npos);
            CHECK(Count(rig.Source(), v3::EventKindV3::SubagentObserved) == 0);
            std::cout << "[child-observation-path] observation-unknown\n";
        } else {
            CHECK(rig.large->calls == 1);
            CHECK(result->side_effect_error.find("tool.capture.persist_failed") != std::string::npos);
            CHECK(Count(rig.Source(), v3::EventKindV3::SubagentObserved) == 1);
            CHECK(Count(rig.Source(), v3::EventKindV3::ToolResultSummaryFinished) == 0);
            std::cout << "[child-observation-path] summary-halted\n";
        }
    } else {
        REQUIRE_MESSAGE(result.has_value(), (result ? std::string() : result.error()));
        CHECK_FALSE(result->side_effect_indeterminate);
        CHECK(rig.large->calls == 1); CHECK(rig.after->calls == 1);
        CHECK(rig.backend.child_calls == 1); CHECK(rig.backend.parent_calls == 2);
        CHECK(rig.backend.summary_calls > 0);
        CHECK(Count(rig.Source(), v3::EventKindV3::ToolResultSummaryFinished) > 0);
        rig.CheckChild();
        std::cout << "[child-observation-path] summary-healthy\n";
    }
}

TEST_CASE("child success and actual capture directory failure preserve the committed observation") {
    Directory directory; Rig rig(directory); rig.failure = Failure::CaptureFile;
    const auto result = rig.Run(); rig.CheckStopped(result);
    CHECK(result->side_effect_error.find("tool.capture.store_unavailable") != std::string::npos);
    CHECK(Count(rig.Source(), v3::EventKindV3::SubagentObserved) == 1);
    std::cout << "[child-observation-path] capture-unknown\n";
}

TEST_CASE("child unknown cannot disappear at real rewrite or batch commit failure") {
    int path = 0;
    SUBCASE("actual rewrite receipt") { path = 0; }
    SUBCASE("actual batch receipt without rewrite") { path = 1; }
    SUBCASE("real res filename numeric overflow after observation and capture") { path = 2; }
    SUBCASE("first real child preview failure stops a later sibling summary") { path = 3; }
    Directory directory; Rig rig(directory);
    rig.failure = path == 2 ? Failure::BadResultName : path == 3 ? Failure::None : Failure::CaptureFile;
    rig.bypass_rewrite = path == 1;
    if (path == 3) {
        rig.force_child_preview_failure = true; rig.backend.large_after = true;
        rig.parent->SetContextWindowTokens(32768);
    }
    const auto result = rig.Run();
    if (path < 2) {
        rig.CheckStopped(result);
        CHECK(result->side_effect_error.find("tool.capture.store_unavailable") != std::string::npos);
        CHECK(result->side_effect_error.find("tool.capture.failed") != std::string::npos);
        std::cout << "[child-observation-path] " << (path == 1 ? "commit-unknown\n" : "rewrite-unknown\n");
    } else {
        REQUIRE_MESSAGE(result.has_value(), (result ? std::string() : result.error()));
        CHECK(result->side_effect_indeterminate);
        CHECK(result->side_effect_error.find(path == 2 ? "subagent.result.exception:" : "tool.preview.unrepresentable:") != std::string::npos);
        CHECK(rig.backend.child_calls == 1); CHECK(rig.backend.parent_calls == 1);
        CHECK(rig.backend.summary_calls == 0);
        // Rewrite fails after the batch has already executed. Its sibling
        // execution stays recorded; no second child/model/capture starts.
        CHECK(rig.after->calls == 1); rig.CheckChild();
        REQUIRE(rig.captures.size() == 1); CHECK(rig.captures.front().ok());
        REQUIRE(rig.repeated_captures.size() == 1); CHECK(rig.repeated_captures.front().ok());
        CHECK(Count(rig.Source(), v3::EventKindV3::SubagentObserved) == 1);
        const auto action = rig.child_sources.front().parent_action.action_id;
        const auto source = rig.Source();
        CHECK(std::count_if(source.events.begin(), source.events.end(), [&](const auto& event) {
            return event.kind == v3::EventKindV3::ToolResultPersisted && event.action_id == action;
        }) == (path == 2 ? 1 : 2));
        if (path == 3) {
            CHECK(rig.large->calls == 1);
            CHECK(Count(source, v3::EventKindV3::ToolResultSummaryFinished) == 0);
            const auto actions = v3::FoldToolActions(source);
            const auto sibling = std::find_if(actions.begin(), actions.end(), [](const auto& value) {
                return value.tool_name == std::optional<std::string>("large_result");
            });
            REQUIRE(sibling != actions.end()); REQUIRE(sibling->selected_event_ref.has_value());
            std::cout << "[child-observation-path] mid-batch-summary-halted\n";
        } else std::cout << "[child-observation-path] rewrite-exception\n";
    }
}

TEST_CASE("cancelled child and actual Close boundary failure never form a positive parent observation") {
    Directory directory; Rig rig(directory); rig.close_failure = true; rig.backend.cancel_child = &rig.cancel;
    const auto result = rig.Run();
    REQUIRE(result.has_value()); CHECK(result->side_effect_indeterminate);
    CHECK(rig.backend.parent_calls == 1); CHECK(rig.backend.child_calls == 1);
    CHECK(rig.after->calls == 0); REQUIRE(rig.terminals.size() == 1);
    CHECK(rig.terminals.front().execution == runtime::SubagentExecutionOutcome::Cancelled);
    CHECK(rig.terminals.front().confirmation == runtime::SubagentAppendConfirmation::Committed);
    CHECK(rig.terminals.front().seal == runtime::SubagentSealState::CloseFailed);
    CHECK_FALSE(rig.terminals.front().durable()); CHECK(rig.close_calls == 1); rig.CheckChild();
    CHECK(Count(rig.Source(), v3::EventKindV3::SubagentObserved) == 0);
    std::cout << "[child-observation-path] cancel-close-failed\n";
}

TEST_CASE("parent Attach rejection closes the real spawned child before any child execution") {
    bool close_failed = false;
    SUBCASE("checked child Close") { close_failed = false; }
    SUBCASE("real Close then injected failure") { close_failed = true; }
    Directory directory; Rig rig(directory); rig.reject_attach = true; rig.close_failure = close_failed;
    const auto result = rig.Run(); REQUIRE(result.has_value());
    CHECK(result->side_effect_indeterminate == close_failed);
    CHECK(rig.backend.child_calls == 0); CHECK(rig.backend.parent_calls == (close_failed ? 1 : 2));
    REQUIRE(rig.rejected.has_value()); REQUIRE(rig.rejected->cleanup_receipt.has_value());
    CHECK(rig.rejected->error_code == "subagent.attach.owner_mismatch");
    CHECK(rig.terminals.front().execution == runtime::SubagentExecutionOutcome::StartupRejected);
    CHECK(rig.terminals.front().confirmation == runtime::SubagentAppendConfirmation::Committed);
    CHECK(rig.terminals.front().seal == (close_failed ? runtime::SubagentSealState::CloseFailed : runtime::SubagentSealState::Closed));
    CHECK(rig.close_calls == 1); rig.CheckChild();
    const auto tasks = rig.dispatch->TaskSnapshots(); REQUIRE(tasks.size() == 1);
    CHECK(tasks.front().state != tools::AgentTaskState::Running);
    CHECK(Count(rig.Source(), v3::EventKindV3::SubagentObserved) == 0);
}

TEST_CASE("provider local ID reuse binds each new response and turn to its current action") {
    Directory directory; Rig rig(directory); rig.backend.parent_dispatches = 2;
    const auto first = rig.Run(); REQUIRE(first.has_value()); CHECK_FALSE(first->side_effect_indeterminate);
    CHECK(rig.backend.child_calls == 2); CHECK(rig.backend.parent_calls == 3);
    REQUIRE(rig.child_sources.size() == 2);
    CHECK(rig.child_sources[0].parent_action.action_id != rig.child_sources[1].parent_action.action_id);
    CHECK(rig.child_sources[0].parent_action.declared_message_ref != rig.child_sources[1].parent_action.declared_message_ref);
    CHECK(Count(rig.Source(), v3::EventKindV3::SubagentObserved) == 2); rig.CheckChild(0); rig.CheckChild(1);
    rig.backend.parent_calls = 0; rig.backend.parent_dispatches = 1;
    const auto second = rig.Run(); REQUIRE(second.has_value()); CHECK_FALSE(second->side_effect_indeterminate);
    REQUIRE(rig.child_sources.size() == 3);
    CHECK(rig.child_sources[2].parent_action.turn_id != rig.child_sources[0].parent_action.turn_id);
    CHECK(rig.child_sources[2].parent_action.action_id != rig.child_sources[0].parent_action.action_id);
    CHECK(Count(rig.Source(), v3::EventKindV3::SubagentObserved) == 3); rig.CheckChild(2);
}

TEST_CASE("child source absence and bounded same bytes verification reject honest read gaps") {
    int path = 0;
    SUBCASE("actual missing child and fixed byte line record limits") { path = 0; }
    SUBCASE("genuine adopted summary uses only bounded ledger bytes") { path = 1; }
#ifndef _WIN32
    // Unix permits this real directory-alias fixture without additional host
    // privilege. Windows does not promise unprivileged symlink creation.
    SUBCASE("actual subagents directory alias cannot leave its parent owner") { path = 2; }
#endif
    if (path == 1) {
        Directory directory; CheckLedgerOnlySummary(directory);
    } else if (path == 2) {
        Directory directory; Rig rig(directory); rig.failure = Failure::ChildRootAlias;
        const auto result = rig.Run(); rig.CheckStopped(result);
        CHECK(result->side_effect_error.find("subagent.observation.child_path_outside_parent") != std::string::npos);
        CHECK(Count(rig.Source(), v3::EventKindV3::SubagentObserved) == 0);
        std::cout << "[child-observation-path] source-owner-alias\n";
    } else {
    Directory directory; Rig rig(directory); rig.failure = Failure::MissingChild;
    const auto result = rig.Run();
    REQUIRE(result.has_value()); CHECK(result->side_effect_indeterminate);
    CHECK(rig.backend.parent_calls == 1); CHECK(rig.backend.child_calls == 1);
    CHECK(rig.after->calls == 0); CHECK(Count(rig.Source(), v3::EventKindV3::SubagentObserved) == 0);
    Write(rig.child_paths.front(), rig.removed_child_bytes); rig.CheckChild();
    const auto& path = rig.child_paths.front();
    const auto complete = v3::ReadV3LedgerBounded(path, rig.removed_child_bytes.size(), 131072, 4 * 1024 * 1024);
    REQUIRE(complete.has_value());
    const auto too_many_bytes = v3::ReadV3LedgerBounded(path, rig.removed_child_bytes.size() - 1, 131072, 4 * 1024 * 1024);
    REQUIRE_FALSE(too_many_bytes.has_value()); CHECK(too_many_bytes.error().find("limit_exceeded") != std::string::npos);
    const auto too_many_lines = v3::ReadV3LedgerBounded(path, rig.removed_child_bytes.size(), 1, 4 * 1024 * 1024);
    REQUIRE_FALSE(too_many_lines.has_value()); CHECK(too_many_lines.error() == "v3reader.record_limit_exceeded");
    const auto long_line = v3::ReadV3LedgerBounded(path, rig.removed_child_bytes.size(), 131072, 1);
    REQUIRE_FALSE(long_line.has_value()); CHECK(long_line.error() == "v3reader.line_limit_exceeded");
    std::cout << "[child-observation-path] source-gap\n";
    }
}
