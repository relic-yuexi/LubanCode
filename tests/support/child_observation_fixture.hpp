#pragma once

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
    std::function<void(agent::TurnWiring&)> configure_wiring;
    // Destroy the Agent and actual AgentTool, including callbacks borrowing this
    // fixture, before their capture vectors and parent bridge/books/writer.
    tools::ToolRegistry parent_tools;
    std::unique_ptr<agent::Agent> parent;

    explicit Rig(const Directory& dir, std::string parent_session = "parent-session") : directory(dir) {
        v3::V3WriterOptions options;
        options.inject_io_failure = [armed = io_armed]() -> std::optional<std::string> {
            if (armed->exchange(false)) return "armed native observation append";
            return std::nullopt;
        };
        auto opened = v3::V3Writer::Start(dir.root / "parent.jsonl", parent_session, "main-1",
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
                    const bool artifact_dir_ready = std::filesystem::create_directories(directory.root / "artifacts") ||
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
        if (configure_wiring) configure_wiring(wiring);
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


} // namespace
