#include <doctest/doctest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>
#include "lubancore/core.hpp"
#include "lubancore/results.hpp"
#include "platform/sha256.hpp"
#include "platform/text_encoding.hpp"
#include "sdk/results.hpp"
#include "tools/path_utils.hpp"
#include "tools/run_command.hpp"
#include "tools/tool_job_coordinator.hpp"
#include "trajectory/canonical_json.hpp"
#include "trajectory/v3/hooks.hpp"
#include "trajectory/v3/reader.hpp"
#include "trajectory/v3/result_store.hpp"
#include "trajectory/v3/tool_action.hpp"
#include "trajectory/v3/writer.hpp"

namespace {
namespace sdk = lubancore;
namespace out = lubancore::results::v1;
namespace fs = std::filesystem;
namespace v3 = lubancode::trajectory::v3;
namespace tools = lubancode::tools;
using Json = nlohmann::json;
using Store = lubancode::trajectory::v3::ResultStore;
using namespace std::chrono_literals;

struct Fixture {
    fs::path root;
    Fixture() {
        static std::atomic<int> serial{0};
        root = fs::temp_directory_path() / ("sdk-result-projection-" +
            std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + "-" + std::to_string(++serial));
        fs::create_directories(root / "cwd");
        fs::create_directories(root / "resources");
        root = fs::canonical(root);
    }
    ~Fixture() { std::error_code ec; fs::remove_all(root, ec); }
    std::string Utf8(const fs::path& path) const { return lubancode::tools::PathToUtf8(path); }
    sdk::RuntimeOptions Roots() const { return {Utf8(root / "data"), Utf8(root / "resources")}; }
};
class ToolBackend final : public sdk::Backend {
public:
    sdk::Result<sdk::ModelReply> Generate(const sdk::ModelRequest& request, sdk::Cancellation) override {
        for (const auto& message : request.messages)
            if (!message.tool_replies.empty()) return sdk::ModelReply{"done", {}, std::nullopt};
        return sdk::ModelReply{"", {{"projection-call", "projection_tool", "{}"}}, std::nullopt};
    }
};
sdk::SessionOptions Options(const Fixture& fixture, std::string result,
                            std::optional<out::SessionResultOptions> policy = std::nullopt) {
    sdk::SessionOptions options;
    options.cwd = fixture.Utf8(fixture.root / "cwd");
    options.model = "projection-model";
    options.system_prompt = "Return a saved tool result.";
    options.backend = std::make_unique<ToolBackend>();
    options.result_policy = policy;
    options.max_steps_per_turn = 4;
    sdk::Tool tool;
    tool.name = "projection_tool";
    tool.description = "Return durable fixture text.";
    tool.requires_approval = false;
    tool.execute = [result = std::move(result)](const std::string&, const sdk::ToolContext&) -> sdk::Result<sdk::ToolResult> {
        return sdk::ToolResult{result, false};
    };
    options.custom_tools.push_back(std::move(tool));
    return options;
}
out::SavedSnapshot Turn(const std::shared_ptr<sdk::Session>& session, std::size_t read_budget = 8 * 1024 * 1024) {
    auto receipt = session->Submit("projection-key", "run tool");
    REQUIRE(receipt.has_value());
    auto operation = session->WaitResult(receipt->operation_id, 30s);
    REQUIRE(operation.has_value());
    INFO(operation->error);
    REQUIRE(operation->state == sdk::OperationState::Succeeded);
    auto refs = session->ListToolResults(receipt->operation_id);
    REQUIRE(refs.has_value());
    const auto formal = std::find_if(refs->begin(), refs->end(), [](const auto& result) {
        return result.selected && result.identity.result_id.starts_with("res-");
    });
    REQUIRE(formal != refs->end());
    auto snapshot = session->ReadToolResult(formal->identity, {read_budget});
    REQUIRE(snapshot.has_value());
    REQUIRE(snapshot->result().metadata_state == out::ArtifactState::Verified);
    return std::move(*snapshot);
}
std::shared_ptr<out::ResultProjector> Projector(const out::SavedSnapshot& snapshot,
    out::NodeResultPolicy node = {false, 4096, "node-policy-1"}, std::vector<std::string> secrets = {}) {
    auto projector = out::ResultProjector::Create(std::move(node), snapshot.policy(), std::move(secrets));
    REQUIRE(projector.has_value());
    return std::move(*projector);
}
Json Wire(const out::FrozenProjection& frozen, const std::shared_ptr<out::ResultProjector>& projector) {
    auto bytes = frozen.ForTransmission(*projector);
    REQUIRE(bytes.has_value());
    return Json::parse(*bytes);
}
Store::ChannelOutput Channel(std::string kind, std::string text) {
    Store::ChannelOutput channel;
    channel.channel = std::move(kind);
    channel.data = std::move(text);
    channel.output_bytes = channel.data.size();
    return channel;
}
// Component fixture: real ResultStore files feed the same production reader,
// compiled as a private test source. SnapshotAccess is not exported or copied.
// The supplied index/policy are fixture inputs; these cases do not prove the
// public completed-operation or V3 indexing gates. The actual SDK session cases
// above/below and installed consumer cover that separate public path.
out::SavedSnapshot Durable(const Fixture& fixture, std::vector<Store::ChannelOutput> channels,
                           out::Mode mode = out::Mode::Preview) {
    auto store = Store::Open(fixture.root / "saved");
    REQUIRE(store.has_value());
    Store::PersistRequest request;
    request.result_kind = "process";
    request.tool_call_id = "durable-call";
    request.execution_event_ref = "durable-execution";
    request.outputs = std::move(channels);
    auto saved = store->Persist(request);
    REQUIRE(saved.ok);
    sdk::detail::ToolResultIndexEntry entry;
    entry.summary = {{"durable-session", "durable-operation", "durable-turn", request.tool_call_id,
                      "durable-persisted", saved.result_id}, request.attempt, true, "durable_tool"};
    entry.execution_event_id = request.execution_event_ref;
    for (const auto& ref : saved.result_ref)
        entry.artifacts.push_back({ref.at("artifactId").get<std::string>(), ref.at("kind").get<std::string>(),
            ref.at("path").get<std::string>(), ref.at("sha256").get<std::string>(),
            ref.at("bytes").get<std::uint64_t>(), ref.at("mediaType").get<std::string>()});
    auto snapshot = sdk::detail::ReadIndexedToolResult(fixture.root / "saved", entry,
        {entry.summary.identity.session_id, mode, 1}, {8 * 1024 * 1024});
    REQUIRE(snapshot.has_value());
    REQUIRE(snapshot->result().metadata_state == out::ArtifactState::Verified);
    return std::move(*snapshot);
}

v3::V3Ledger BoundaryLedger(const fs::path& path) {
    auto ledger = v3::ReadV3Ledger(path);
    REQUIRE_MESSAGE(ledger.has_value(), (ledger ? std::string() : ledger.error()));
    return std::move(*ledger);
}
void BoundaryCommitted(const v3::WriteReceipt& receipt) {
    REQUIRE_MESSAGE(receipt.status == v3::WriteReceipt::Status::Committed, receipt.error_message);
    REQUIRE_FALSE(receipt.id.empty()); REQUIRE(receipt.seq > 0); REQUIRE_FALSE(receipt.line_hash.empty());
}
std::string BoundaryBytes(const fs::path& path) {
    std::ifstream stream(path, std::ios::binary);
    REQUIRE(stream.is_open());
    std::string bytes((std::istreambuf_iterator<char>(stream)), {});
    REQUIRE_FALSE(stream.bad());
    return bytes;
}
void BoundarySameIndex(const sdk::detail::OperationToolResultIndex& cached,
                       const sdk::detail::OperationToolResultIndex& current) {
    REQUIRE(cached.entries.size() == current.entries.size());
    for (std::size_t i = 0; i < cached.entries.size(); ++i) {
        const auto& before = cached.entries[i]; const auto& after = current.entries[i];
        CHECK(before.summary.identity == after.summary.identity);
        CHECK(before.summary.attempt == after.summary.attempt);
        CHECK(before.summary.selected == after.summary.selected);
        CHECK(before.summary.tool_name == after.summary.tool_name);
        CHECK(before.execution_event_id == after.execution_event_id);
        REQUIRE(before.artifacts.size() == after.artifacts.size());
        for (std::size_t j = 0; j < before.artifacts.size(); ++j) {
            const auto& a = before.artifacts[j]; const auto& b = after.artifacts[j];
            CHECK(a.id == b.id); CHECK(a.kind == b.kind); CHECK(a.path == b.path);
            CHECK(a.sha256 == b.sha256); CHECK(a.bytes == b.bytes); CHECK(a.media_type == b.media_type);
        }
    }
}

// Internal index component, not a public SDK background session. A real owned
// coordinator writes and runs the business action after the parent index was
// cached. Both actions share the actual turn; their native materials stay apart.
void OwnedJobIndexBoundary(const Fixture& fixture) {
    const auto dir = fixture.root / "owned-index";
    REQUIRE(fs::create_directory(dir));
    const auto journal = dir / "session.jsonl";
    const std::string sid = "20261003-190000-IDXOWN", turn = "turn-000001", step = "step-000001";
    const std::string parent_action = "action-parent-index", provider_call = "provider-index";
    std::atomic<unsigned> legacy_calls{0}, scopes{0}, posts{0}, threads{0};
    auto serial = std::make_shared<std::recursive_mutex>();
    auto opened = v3::V3Writer::Start(journal, sid, "run-000001", "owned index boundary");
    REQUIRE_MESSAGE(opened.has_value(), (opened ? std::string() : opened.error()));
    auto writer = std::make_shared<v3::V3Writer>(std::move(*opened));
    tools::ToolJobCoordinator::Options options;
    options.prepared_registration = tools::PreparedRegistrationContext{serial, "index-project", dir};
    options.thread_starter = [&threads](std::thread& thread, std::function<void()> body) {
        ++threads; thread = std::thread(std::move(body));
    };
    auto coordinator = std::make_unique<tools::ToolJobCoordinator>(*writer,
        [&legacy_calls](const std::string&, const Json&) {
            ++legacy_calls; return tools::JobAuthDecision{false, false, "legacy must not run"};
        }, [&legacy_calls](const tools::JobExecutionContext&) {
            ++legacy_calls; return tools::Tool::Result{"legacy must not run", true};
        }, std::move(options));
#ifdef _WIN32
    const std::string shell = "cmd";
#else
    const std::string shell = "sh";
#endif
    const Json input{{"command", "echo OWNED_JOB_RAW_BOUNDARY"}, {"shell", shell}, {"cwd", fixture.Utf8(dir)}};
    v3::MessageDraft declaration;
    declaration.turn_id = turn; declaration.step_id = step; declaration.request_id = "request-000001";
    declaration.origin = v3::MessageOrigin::SessionRuntime;
    declaration.provider = "fixture"; declaration.wire = "responses"; declaration.model = "fixture";
    declaration.response_model = "fixture"; declaration.usage = {{"inputTokens", 1}, {"outputTokens", 1}};
    declaration.message = {{"role", "assistant"}, {"content", "declare background command"},
        {"tool_calls", Json::array({{{"id", provider_call}, {"type", "function"},
            {"function", {{"name", "run_command"}, {"arguments", input.dump()}}}}})}};
    const auto declared = writer->AppendMessage(std::move(declaration), v3::Durability::PowerLoss);
    BoundaryCommitted(declared);
    BoundaryCommitted(writer->AdmitMessages({declared.id}, v3::Durability::PowerLoss));
    auto parent = v3::ToolActionSession::Admit(*writer, turn, step, parent_action, "owned-parent",
        declared.id, provider_call, Json::object(), v3::Durability::PowerLoss);
    REQUIRE(parent.last_event_id().has_value());
    const auto owner = coordinator->PreparedOwner(); REQUIRE(owner.has_value());
    tools::PreparedJobRequest request;
    request.owner = *owner; request.provider_tool_call_id = provider_call; request.assistant_message_ref = declared.id;
    request.parent_action_id = parent_action; request.turn_id = turn; request.step_id = step; request.tool_name = "run_command";
    request.original_input = input; request.effective_input = input;
    request.tool_identity = {"run_command", "native-index-command", "native-v1", fixture.Utf8(dir)};
    request.policy.allow_background = true; request.policy.side_effect_class = "external";
    request.policy.resource_keys = {"owned-index-command"};
    const auto registered = coordinator->RegisterPreparedJob(request);
    REQUIRE_MESSAGE(registered.state == tools::PreparedJobRegistrationState::Registered, registered.error);
    REQUIRE(registered.facts); REQUIRE(registered.facts->pending_receipt); REQUIRE(registered.facts->registered_receipt);
    BoundaryCommitted(*registered.facts->pending_receipt); BoundaryCommitted(*registered.facts->registered_receipt);
    const auto facts = registered.facts;
    REQUIRE(facts->action_id != parent_action); CHECK(facts->turn_id == turn); CHECK(facts->attempt == 1);
    const auto prepared_ledger = BoundaryLedger(journal);
    const auto folded_prepared = v3::FoldToolActions(prepared_ledger);
    const auto* business = v3::FindActionSnapshot(folded_prepared, facts->action_id); REQUIRE(business);
    CHECK_FALSE(business->provider_reply_required);
    const auto* original = v3::FindActionSnapshot(folded_prepared, parent_action); REQUIRE(original);
    CHECK(original->provider_reply_required);
    tools::OwnedJobCapability capability;
    capability.command = std::make_shared<tools::RunCommandTool>(); capability.command_limits = {15000, 1024};
    capability.scope_gate = [facts, &scopes](const tools::OwnedJobScope& scope, const Json& actual_input,
        const v3::ToolIdentity& identity, const tools::JobExecutionPolicy& policy) {
        ++scopes;
        const bool actual = scope.owner == facts->owner && scope.job_id == facts->job_id &&
            scope.action_id == facts->action_id && scope.parent_action_id == facts->parent_action_id &&
            scope.turn_id == facts->turn_id && scope.step_id == facts->step_id && scope.attempt == 1 &&
            scope.provider_tool_call_id == facts->provider_tool_call_id && actual_input == facts->effective_input &&
            identity.ToJson() == facts->tool_identity.ToJson() && policy.ToJson() == facts->policy.ToJson();
        CHECK(actual);
        return tools::JobAuthDecision{actual, false, actual ? std::string() : "owned index scope mismatch"};
    };
    capability.post = [writer, serial, facts, &posts](const tools::OwnedJobCompletion& completion) {
        std::lock_guard lock(*serial); ++posts;
        CHECK(completion.scope.owner == facts->owner); CHECK(completion.scope.action_id == facts->action_id);
        CHECK(completion.scope.turn_id == facts->turn_id); CHECK(completion.scope.attempt == 1);
        CHECK_FALSE(completion.raw.is_error);
        CHECK(completion.raw.content.find("OWNED_JOB_RAW_BOUNDARY") != std::string::npos);
        BoundaryCommitted(completion.started_receipt); BoundaryCommitted(completion.terminal_receipt);
        BoundaryCommitted(completion.persisted_receipt);
        const v3::HookHandlerSpec handler{"index-owned-post", lubancode::platform::Sha256Hex("index-owned-post-v1"),
            "builtin", 0, "block"};
        const auto dispatch_id = writer->NewHookDispatchId();
        auto dispatch = v3::HookDispatchSession::Dispatch(*writer, dispatch_id, "PostAction",
            completion.scope.turn_id, completion.scope.step_id, completion.scope.action_id, {handler},
            Json{{"executionEventRef", completion.terminal_receipt.id}, {"resultEventRef", completion.persisted_receipt.id}});
        BoundaryCommitted(dispatch.BeginInvocation(*writer, "invocation-" + dispatch_id, handler));
        return dispatch.CompleteInvocation(*writer, "allow", std::nullopt, 0, v3::Durability::PowerLoss);
    };
    const auto adopted = coordinator->AdoptPreparedJob(*owner, facts->job_id, std::move(capability));
    REQUIRE_MESSAGE(adopted.state == tools::OwnedJobAdoptionState::Adopted, adopted.error);
    REQUIRE(adopted.receipt); BoundaryCommitted(*adopted.receipt);
    CHECK(threads.load() == 0); CHECK(posts.load() == 0);
    const auto canonical = lubancode::trajectory::CanonicalJsonDump(input); REQUIRE(canonical.has_value());
    BoundaryCommitted(parent.Start(*writer, "args-parent-" + lubancode::platform::Sha256Hex(*canonical),
        request.tool_identity, std::nullopt, Json{{"toolName", "run_command"}}, v3::Durability::PowerLoss));
    const auto terminal = parent.Finish(*writer, std::nullopt, 0); BoundaryCommitted(terminal);
    auto store = Store::Open(dir); REQUIRE(store.has_value());
    Store::PersistRequest material;
    material.result_kind = "text"; material.tool_call_id = parent_action; material.attempt = 1;
    material.execution_event_ref = terminal.id;
    material.outputs = {Channel("combined", adopted.admission_content)};
    material.capture_limits = {{"max_output_bytes", 65536}};
    material.preview_policy = {{"policy", "owned-index-parent"}, {"maxPreviewBytes", 32768}};
    const auto saved = store->Persist(material); REQUIRE_MESSAGE(saved.ok, saved.error);
    const auto persisted = parent.PersistedResult(*writer, saved.result_ref, terminal.id, 1); BoundaryCommitted(persisted);
    const auto selected = parent.SelectResult(*writer, {persisted.id}, {}, "done", 1); BoundaryCommitted(selected);
    v3::MessageDraft message;
    message.turn_id = turn; message.step_id = step; message.action_id = parent_action;
    message.origin = v3::MessageOrigin::SessionRuntime; message.purpose = v3::MessagePurpose::Conversation;
    message.result_selection_ref = selected.id;
    message.message = {{"role", "tool"}, {"tool_call_id", parent_action}, {"content", adopted.admission_content}};
    const auto emitted = writer->AppendMessage(std::move(message), v3::Durability::PowerLoss); BoundaryCommitted(emitted);
    const auto admitted = writer->AdmitMessages({emitted.id}, v3::Durability::PowerLoss); BoundaryCommitted(admitted);
    const auto before = sdk::detail::IndexToolResults(BoundaryLedger(journal), sid, "operation-parent", turn);
    REQUIRE_MESSAGE(before.has_value(), (before ? std::string() : before.error().message));
    REQUIRE(before->entries.size() == 1); CHECK(before->entries.front().summary.selected);
    CHECK(before->entries.front().summary.identity.tool_call_id == parent_action);
    const auto cached = *before;
    const auto cached_snapshot = sdk::detail::ReadIndexedToolResult(dir, cached.entries.front(),
        {sid, out::Mode::Preview, 1}, {8 * 1024 * 1024}); REQUIRE(cached_snapshot.has_value());
    REQUIRE(cached_snapshot->result().channels.size() == 1);
    CHECK(cached_snapshot->result().channels.front().text == adopted.admission_content);
    const tools::ParentJobAdmissionRefs refs{terminal.id, persisted.id, selected.id, emitted.id, admitted.id};
    const auto confirmation = coordinator->ConfirmParentAdmission(*owner, facts->job_id, refs);
    REQUIRE_MESSAGE(confirmation.confirmed, confirmation.error);
    const auto waited = coordinator->WaitOwnedJobs(*owner, {facts->job_id}, 20000, true);
    REQUIRE(waited.satisfied); CHECK_FALSE(waited.timed_out); REQUIRE(waited.statuses.size() == 1);
    const auto& status = waited.statuses.front();
    REQUIRE(status.state == "succeeded"); CHECK(status.execution_state == "succeeded"); CHECK(status.worker_finished);
    CHECK(status.scope.action_id == facts->action_id); CHECK(status.scope.turn_id == turn); CHECK(status.scope.attempt == 1);
    // Scope is checked at adoption, parent confirmation, and real dispatch.
    CHECK(threads.load() == 1); CHECK(scopes.load() == 3); CHECK(posts.load() == 1); CHECK(legacy_calls.load() == 0);
    REQUIRE(status.started_receipt); REQUIRE(status.terminal_receipt); REQUIRE(status.persisted_receipt);
    REQUIRE(status.post_receipt); REQUIRE(status.observed_receipt);
    const auto late = BoundaryLedger(journal);
    for (const auto& receipt : {*status.started_receipt, *status.terminal_receipt, *status.persisted_receipt,
                               *status.post_receipt, *status.observed_receipt}) {
        BoundaryCommitted(receipt);
        const auto* event = late.FindEvent(receipt.id); REQUIRE(event);
        CHECK(event->session_id == sid); CHECK(event->run_id == writer->run_id()); CHECK(event->turn_id == turn);
        CHECK(event->action_id == facts->action_id); CHECK(event->seq == receipt.seq); CHECK(event->line_hash == receipt.line_hash);
    }
    const auto* raw = late.FindEvent(status.persisted_receipt->id); REQUIRE(raw);
    CHECK(raw->payload.at("attempt").get<std::uint64_t>() == 1);
    CHECK(raw->payload.at("executionEventRef").get<std::string>() == status.terminal_receipt->id);
    bool raw_found = false;
    for (const auto& ref : raw->payload.at("result_ref")) {
        if (ref.at("kind").get<std::string>() != "combined") continue;
        const auto bytes = BoundaryBytes(dir / fs::u8path(ref.at("path").get<std::string>()));
        CHECK(bytes.find("OWNED_JOB_RAW_BOUNDARY") != std::string::npos);
        CHECK(bytes.size() == ref.at("bytes").get<std::uint64_t>());
        CHECK(lubancode::platform::Sha256Hex(bytes) == ref.at("sha256").get<std::string>());
        CHECK(bytes != adopted.admission_content); raw_found = true;
    }
    REQUIRE(raw_found);
    const auto after = sdk::detail::IndexToolResults(late, sid, "operation-parent", turn);
    REQUIRE_MESSAGE(after.has_value(), (after ? std::string() : after.error().message));
    BoundarySameIndex(cached, *after);
    CHECK(cached_snapshot->result().channels.front().text == adopted.admission_content);
    // Exercise the native selection entrance too. The coordinator's completed
    // raw is selected as business material; this does not create a provider
    // reply or a second ordinary SDK operation.
    auto completed_business = v3::ToolActionSession::ReopenAligned(turn, step, facts->action_id,
        1, true, v3::ToolActionSession::Terminal::Finished);
    const auto business_selected = completed_business.SelectResult(*writer,
        {status.persisted_receipt->id}, {}, "done", 1);
    BoundaryCommitted(business_selected);
    const auto selected_ledger = BoundaryLedger(journal);
    const auto selected_actions = v3::FoldToolActions(selected_ledger);
    const auto* selected_business = v3::FindActionSnapshot(selected_actions, facts->action_id); REQUIRE(selected_business);
    CHECK_FALSE(selected_business->provider_reply_required);
    REQUIRE(selected_business->selected_event_ref.has_value());
    CHECK(*selected_business->selected_event_ref == business_selected.id);
    const auto after_selection = sdk::detail::IndexToolResults(selected_ledger, sid, "operation-parent", turn);
    REQUIRE(after_selection.has_value()); BoundarySameIndex(cached, *after_selection);
    REQUIRE(coordinator->Shutdown()); coordinator.reset();
    REQUIRE(writer->Close().has_value());
    const auto closed = sdk::detail::IndexToolResults(BoundaryLedger(journal), sid, "operation-parent", turn);
    REQUIRE(closed.has_value()); BoundarySameIndex(cached, *closed);
}

// The original job_handle producer remains an ordinary result-index source.
// Its admission attempt and actual worker attempt retain the legacy read path.
void LegacyJobIndexBoundary(const Fixture& fixture) {
    const auto dir = fixture.root / "legacy-index"; REQUIRE(fs::create_directory(dir));
    auto writer = v3::V3Writer::Start(dir / "session.jsonl", "20261003-190001-IDXOLD", "run-000001", "legacy index boundary");
    REQUIRE(writer.has_value());
    std::atomic<unsigned> executions{0};
    auto coordinator = std::make_unique<tools::ToolJobCoordinator>(*writer,
        [](const std::string&, const Json&) { return tools::JobAuthDecision{true, false, {}}; },
        [&executions](const tools::JobExecutionContext&) { ++executions; return tools::Tool::Result{"LEGACY_JOB_RAW_BOUNDARY", false}; });
    v3::MessageDraft declaration;
    declaration.turn_id = "turn-000001"; declaration.step_id = "step-000001"; declaration.request_id = "request-000001";
    declaration.origin = v3::MessageOrigin::SessionRuntime;
    declaration.provider = "fixture"; declaration.wire = "responses"; declaration.model = "fixture";
    declaration.response_model = "fixture"; declaration.usage = {{"inputTokens", 1}, {"outputTokens", 1}};
    declaration.message = {{"role", "assistant"}, {"content", "declare legacy job"},
        {"tool_calls", Json::array({{{"id", "legacy-provider"}, {"type", "function"},
            {"function", {{"name", "legacy_tool"}, {"arguments", "{}"}}}}})}};
    const auto declared = writer->AppendMessage(std::move(declaration), v3::Durability::PowerLoss); BoundaryCommitted(declared);
    BoundaryCommitted(writer->AdmitMessages({declared.id}, v3::Durability::PowerLoss));
    tools::JobStartRequest request;
    request.tool_name = "legacy_tool"; request.tool_input = Json::object(); request.turn_id = "turn-000001";
    request.step_id = "step-000001"; request.assistant_message_ref = declared.id;
    const auto started = coordinator->StartJob(request); REQUIRE_MESSAGE(started.ok, started.error);
    const auto waited = coordinator->WaitJobs({started.job_id}, 20000, true);
    REQUIRE(waited.satisfied); CHECK_FALSE(waited.timed_out); REQUIRE(waited.statuses.size() == 1);
    CHECK(waited.statuses.front().state == "succeeded"); CHECK(executions.load() == 1);
    REQUIRE(coordinator->Shutdown()); coordinator.reset(); REQUIRE(writer->Close().has_value());
    const auto ledger = BoundaryLedger(writer->path());
    const auto actions = v3::FoldToolActions(ledger);
    const auto* action = v3::FindActionSnapshot(actions, started.action_id); REQUIRE(action);
    CHECK(action->provider_reply_required);
    const auto index = sdk::detail::IndexToolResults(ledger, writer->session_id(), "operation-legacy", request.turn_id);
    REQUIRE_MESSAGE(index.has_value(), (index ? std::string() : index.error().message));
    REQUIRE(index->entries.size() == 2);
    for (const auto& entry : index->entries) CHECK(entry.summary.identity.tool_call_id == started.action_id);
    CHECK(index->entries.front().summary.attempt == 1); CHECK(index->entries.front().summary.selected);
    CHECK(index->entries.back().summary.attempt == 2);
    const auto raw = sdk::detail::ReadIndexedToolResult(dir, index->entries.back(),
        {writer->session_id(), out::Mode::Preview, 1}, {8 * 1024 * 1024}); REQUIRE(raw.has_value());
    REQUIRE(raw->result().channels.size() == 1);
    CHECK(raw->result().channels.front().text == "LEGACY_JOB_RAW_BOUNDARY");
}
} // namespace

TEST_CASE("SDK result projection: actual SDK snapshot enforces two permissions and same-session binding") {
    Fixture fixture;
    auto runtime = sdk::Runtime::Create(fixture.Roots());
    REQUIRE(runtime.has_value());
    const std::string credential = "FAKE_SAVED_PROJECTION_CREDENTIAL";
    const std::string raw = "RESULT_HEAD " + credential + std::string(8000, 'x') + " RESULT_TAIL";
    auto session = (*runtime)->OpenSession(Options(fixture, raw));
    REQUIRE(session.has_value());
    auto snapshot = Turn(*session);
    REQUIRE(snapshot.policy().mode == out::Mode::Preview);
    auto host_permitted = Projector(snapshot, {true, 128, "node-policy-1"}, {credential});
    auto preview = host_permitted->Project(snapshot);
    REQUIRE(preview.has_value());
    const auto wire = Wire(*preview, host_permitted);
    CHECK(wire["mode"] == "preview");
    CHECK(wire["text"].get<std::string>().size() <= 128);
    CHECK(wire["text"].get<std::string>().find("[REDACTED]") != std::string::npos);
    CHECK(wire.dump().find(credential) == std::string::npos);
    CHECK(wire.dump().find("RESULT_TAIL") == std::string::npos);
    auto permission_upgrade = snapshot.policy(); permission_upgrade.mode = out::Mode::Full;
    auto full_projector = out::ResultProjector::Create({true, 128, "node-policy-1"}, permission_upgrade, {credential});
    REQUIRE(full_projector.has_value());
    auto escalated = (*full_projector)->Project(snapshot);
    REQUIRE_FALSE(escalated.has_value());
    CHECK(escalated.error().code == "result_sync_session_mismatch");
    auto disabled = out::ResultProjector::Create({false, 128, "node-policy-1"}, permission_upgrade);
    REQUIRE_FALSE(disabled.has_value());
    CHECK(disabled.error().code == "full_result_sync_disabled");

    auto full_session = (*runtime)->OpenSession(Options(fixture, raw, out::SessionResultOptions{out::Mode::Full, 1}));
    REQUIRE(full_session.has_value());
    auto full_snapshot = Turn(*full_session);
    auto allowed = Projector(full_snapshot, {true, 128, "node-policy-1"}, {credential});
    auto full = allowed->Project(full_snapshot);
    REQUIRE(full.has_value());
    CHECK(Wire(*full, allowed)["mode"] == "full");
    CHECK(Wire(*full, allowed)["text"].get<std::string>().find("RESULT_TAIL") != std::string::npos);
    CHECK(Wire(*full, allowed).dump().find(credential) == std::string::npos);
    CHECK_FALSE(host_permitted->Project(full_snapshot).has_value());
    CHECK_FALSE(preview->ForTransmission(*allowed).has_value());
    REQUIRE((*runtime)->Shutdown().has_value());
    OwnedJobIndexBoundary(fixture);
    LegacyJobIndexBoundary(fixture);
}

TEST_CASE("SDK result projection: close resume restores exact frozen record and detects source or policy change") {
    Fixture fixture;
    std::string session_id, operation_id, storage, first;
    out::ToolResultIdentity identity;
    {
        auto runtime = sdk::Runtime::Create(fixture.Roots());
        REQUIRE(runtime.has_value());
        auto session = (*runtime)->OpenSession(Options(fixture, "RESULT_HEAD " + std::string(9000, 'x') + " RESULT_TAIL"));
        REQUIRE(session.has_value());
        auto snapshot = Turn(*session);
        identity = snapshot.result().summary.identity;
        session_id = (*session)->id(); operation_id = snapshot.operation_id();
        REQUIRE((*session)->Close().has_value());
        auto projector = Projector(snapshot, {false, 64, "node-policy-1"}, {"FAKE_RESOLVED_KEY"});
        auto frozen = projector->Project(snapshot);
        REQUIRE(frozen.has_value());
        auto serialized = frozen->SerializeForStorage(); REQUIRE(serialized.has_value()); storage = *serialized;
        auto sent = frozen->ForTransmission(*projector); REQUIRE(sent.has_value()); first = *sent;
        auto copy = first; copy[0] = '!';
        CHECK(*frozen->ForTransmission(*projector) == first);
        auto changed = Projector(snapshot, {false, 64, "node-policy-2"}, {"FAKE_RESOLVED_KEY"});
        CHECK_FALSE(frozen->ForTransmission(*changed).has_value());
        auto key_changed = Projector(snapshot, {false, 64, "node-policy-1"}, {"CHANGED_RESOLVED_KEY"});
        CHECK(projector->BindingFingerprint() != key_changed->BindingFingerprint());
        CHECK_FALSE(frozen->ForTransmission(*key_changed).has_value());
        auto reordered = Projector(snapshot, {false, 64, "node-policy-1"}, {"", "FAKE_RESOLVED_KEY", "FAKE_RESOLVED_KEY"});
        CHECK(projector->BindingFingerprint() == reordered->BindingFingerprint());
    }
    auto runtime = sdk::Runtime::Create(fixture.Roots()); REQUIRE(runtime.has_value());
    auto options = Options(fixture, "unused"); options.resume_session_id = session_id;
    auto resumed = (*runtime)->OpenSession(std::move(options)); REQUIRE(resumed.has_value());
    auto snapshot = (*resumed)->ReadToolResult(identity); REQUIRE(snapshot.has_value());
    auto refs = (*resumed)->ListToolResults(operation_id); REQUIRE(refs.has_value());
    CHECK(std::any_of(refs->begin(), refs->end(), [&](const auto& result) {
        return result.selected && result.identity == identity;
    }));
    auto projector = Projector(*snapshot, {false, 64, "node-policy-1"}, {"FAKE_RESOLVED_KEY"});
    auto frozen = projector->RestoreSavedProjection(storage, *snapshot); REQUIRE(frozen.has_value());
    CHECK(*frozen->ForTransmission(*projector) == first);
    CHECK(*frozen->SerializeForStorage() == storage);
    auto damaged = Json::parse(storage); damaged["native"]["payload"]["text"] = "other prefix";
    auto bad = projector->RestoreSavedProjection(damaged.dump(), *snapshot);
    REQUIRE_FALSE(bad.has_value()); CHECK(bad.error().code == "result_sync_invalid_frozen_record");
    damaged = Json::parse(storage); damaged["path"] = "../secret";
    CHECK_FALSE(projector->RestoreSavedProjection(damaged.dump(), *snapshot).has_value());
    auto peer = (*runtime)->OpenSession(Options(fixture, "peer output")); REQUIRE(peer.has_value());
    auto other = Turn(*peer);
    CHECK_FALSE(projector->RestoreSavedProjection(storage, other).has_value());
    REQUIRE((*runtime)->Shutdown().has_value());
}

TEST_CASE("SDK result projection: durable reader fixture channels share one UTF8 prefix and one whole-text redaction") {
    Fixture fixture;
    const std::string key = "FAKE_CROSS_CHANNEL_SECRET";
    auto snapshot = Durable(fixture, {Channel("stdout", std::string(4090, 'a') + key.substr(0, 10)),
                                      Channel("stderr", key.substr(10) + " HIDDEN_TAIL")});
    auto projector = Projector(snapshot, {false, 4096, "node-policy-1"}, {key});
    auto frozen = projector->Project(snapshot); REQUIRE(frozen.has_value());
    auto wire = Wire(*frozen, projector);
    CHECK(wire["text"].get<std::string>() == std::string(4090, 'a') + "[REDAC");
    CHECK(wire["channels"].size() == 2);
    CHECK(wire.dump().find("FAKE_CROSS") == std::string::npos);
    CHECK(wire.dump().find("HIDDEN_TAIL") == std::string::npos);
    const std::string unicode = "A\xE4\xB8\xAD\xF0\x9F\x98\x80" "B";
    auto unicode_snapshot = Durable(fixture, {Channel("stderr", "ignored duplicate"), Channel("stdout", "ignored duplicate"),
                                             Channel("combined", unicode)});
    auto tiny = Projector(unicode_snapshot, {false, 7, "node-policy-1"});
    auto bounded = tiny->Project(unicode_snapshot); REQUIRE(bounded.has_value());
    const auto unicode_wire = Wire(*bounded, tiny);
    CHECK(unicode_wire["text"].get<std::string>() == unicode.substr(0, 4));
    CHECK(unicode_wire["originalBytes"] == unicode.size());
    CHECK(lubancode::platform::IsValidUtf8(unicode_wire["text"].get<std::string>()));
}

TEST_CASE("SDK result projection: durable reader fixture binary and incomplete material cannot leak through metadata") {
    Fixture fixture;
    auto image = Channel("image", "DO_NOT_EXPORT_IMAGE_OR_FILE_PATH"); image.media_type = "image/png"; image.encoding = "binary";
    auto raw = Channel("raw_payload", R"({"credential":"DO_NOT_EXPORT_RAW_JSON"})"); raw.media_type = "application/json";
    auto snapshot = Durable(fixture, {image, raw});
    auto projector = Projector(snapshot);
    auto frozen = projector->Project(snapshot); REQUIRE(frozen.has_value());
    auto wire = Wire(*frozen, projector);
    CHECK(wire["status"] == "metadata_only");
    CHECK_FALSE(wire.contains("text"));
    CHECK(wire.dump().find("DO_NOT_EXPORT") == std::string::npos);
    for (const auto& c : wire["channels"]) {
        CHECK_FALSE(c.contains("path")); CHECK_FALSE(c.contains("artifact_id"));
        CHECK_FALSE(c.contains("capture_reason")); CHECK_FALSE(c.contains("media_type"));
    }
    auto full_snapshot = Durable(fixture, {image, raw}, out::Mode::Full);
    auto full_projector = Projector(full_snapshot, {true, 4096, "node-policy-1"});
    auto refused = full_projector->Project(full_snapshot); REQUIRE_FALSE(refused.has_value());
    CHECK(refused.error().code == "result_not_text");
    auto partial = Channel("stdout", "PARTIAL_PRIVATE_CREDENTIAL");
    partial.capture_complete = false; partial.capture_reason = "DO_NOT_EXPORT_CAPTURE_REASON";
    partial.output_bytes += 100; partial.output_bytes_lower_bound = true;
    auto incomplete = Durable(fixture, {partial});
    auto incomplete_projector = Projector(incomplete);
    auto metadata = incomplete_projector->Project(incomplete); REQUIRE(metadata.has_value());
    auto incomplete_wire = Wire(*metadata, incomplete_projector);
    CHECK(incomplete_wire["status"] == "capture_incomplete");
    CHECK_FALSE(incomplete_wire.contains("text"));
    CHECK(incomplete_wire.dump().find("PARTIAL_PRIVATE") == std::string::npos);
    CHECK(incomplete_wire.dump().find("DO_NOT_EXPORT_CAPTURE_REASON") == std::string::npos);
    auto incomplete_full = Durable(fixture, {partial}, out::Mode::Full);
    auto stopped = Projector(incomplete_full, {true, 4096, "node-policy-1"})->Project(incomplete_full);
    REQUIRE_FALSE(stopped.has_value()); CHECK(stopped.error().code == "result_capture_incomplete");
}

TEST_CASE("SDK result projection: text read quota input cap and escaped transmission cap refuse explicitly") {
    Fixture fixture;
    auto runtime = sdk::Runtime::Create(fixture.Roots()); REQUIRE(runtime.has_value());
    auto session = (*runtime)->OpenSession(Options(fixture, std::string(20000, 'x'))); REQUIRE(session.has_value());
    auto quota_snapshot = Turn(*session, 16);
    auto quota = Projector(quota_snapshot)->Project(quota_snapshot);
    REQUIRE_FALSE(quota.has_value()); CHECK(quota.error().code == "result_too_large");
    // A text body smaller than 1 MiB may still exceed the serialized JSON cap.
    auto escaped = Durable(fixture, {Channel("combined", std::string(600000, '\t'))}, out::Mode::Full);
    auto escaped_projection = Projector(escaped, {true, 4096, "node-policy-1"})->Project(escaped);
    REQUIRE_FALSE(escaped_projection.has_value()); CHECK(escaped_projection.error().code == "result_too_large");
    auto oversized = Durable(fixture, {Channel("combined", std::string(8 * 1024 * 1024 + 1, 'x'))});
    auto huge = Projector(oversized)->Project(oversized);
    REQUIRE_FALSE(huge.has_value()); CHECK(huge.error().code == "result_too_large");
    REQUIRE((*runtime)->Shutdown().has_value());
}

TEST_CASE("SDK result projection: durable reader fixture storage stays strict with recomputed corruption digest") {
    Fixture fixture;
    auto snapshot = Durable(fixture, {Channel("combined", "RESULT_HEAD frozen text")});
    auto projector = Projector(snapshot);
    auto frozen = projector->Project(snapshot); REQUIRE(frozen.has_value());
    auto storage = frozen->SerializeForStorage(); REQUIRE(storage.has_value());
    for (const auto& field : {"previewMaxBytes", "mode", "sessionPolicyVersion", "text", "captureComplete"}) {
        auto damaged = Json::parse(*storage);
        damaged["native"]["payload"][field] = Json::array();
        damaged["native"]["sha256"] = lubancode::platform::Sha256Hex(damaged["native"]["payload"].dump());
        damaged.erase("sha256"); damaged["sha256"] = lubancode::platform::Sha256Hex(damaged.dump());
        auto refused = projector->RestoreSavedProjection(damaged.dump(), snapshot);
        REQUIRE_FALSE(refused.has_value()); CHECK(refused.error().code == "result_sync_invalid_frozen_record");
    }
    auto changed = Durable(fixture, {Channel("combined", "replacement bytes")});
    CHECK_FALSE(projector->RestoreSavedProjection(*storage, changed).has_value());
    for (const std::string invalid : {"null", "{}", "{", "[]"})
        CHECK_FALSE(projector->RestoreSavedProjection(invalid, snapshot).has_value());
}
