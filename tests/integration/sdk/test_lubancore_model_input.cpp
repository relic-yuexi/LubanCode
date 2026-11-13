#include <doctest/doctest.h>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <functional>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

#include <lubancore/core.hpp>
#include "agent/agent.hpp"
#include "api/anthropic/client.hpp"
#include "api/chat/client.hpp"
#include "api/gemini/client.hpp"
#include "api/responses/client.hpp"
#include "hooks/middleware_builtins.hpp"
#include "platform/process.hpp"
#include "platform/sha256.hpp"
#include "runtime/action_summary.hpp"
#include "runtime/assembly/backend.hpp"
#include "runtime/trajectory_turn_bridge.hpp"
#include "sdk/adapters.hpp"
#include "trajectory/v3/reader.hpp"
#include "trajectory/v3/result_store.hpp"
#include "trajectory/v3/tool_action.hpp"
#include "tools/registry.hpp"

namespace {
namespace api = lubancode::api;
namespace sdk = lubancore;
namespace rt = lubancode::runtime;
namespace v3 = lubancode::trajectory::v3;
namespace fs = std::filesystem;
using Json = nlohmann::json;
void Mark(const char* path) { std::cout << "[sdk-model-input-path] " << path << '\n'; }

struct Capture final : sdk::Backend {
    std::vector<sdk::ModelRequest> requests;
    std::function<void()> during_generate;
    sdk::Result<sdk::ModelReply> Generate(const sdk::ModelRequest& request, sdk::Cancellation) override {
        requests.push_back(request);
        if (during_generate) during_generate();
        return sdk::ModelReply{
            R"({"summary":"actual SDK evidence read","side_effects":[],"open_items":[],"evidence":["SUMMARY-EVIDENCE"]})",
            {}, sdk::Usage{11, 7}};
    }
};

api::Request RichTextRequest() {
    api::Request request; request.model = "model-control"; request.system = "system\n\"quoted\"";
    request.max_tokens = 1024;
    api::Message system; system.role = api::Role::System;
    system.content.push_back(api::TextBlock{"history system"}); request.messages.push_back(system);
    api::Message user; user.role = api::Role::User;
    user.content.push_back(api::TextBlock{std::string("control\0byte", 12)});
    user.content.push_back(api::TextBlock{"\xe4\xb8\xad\xe6\x96\x87\xf0\x9f\x8c\x99"}); request.messages.push_back(user);
    api::Message assistant; assistant.role = api::Role::Assistant;
    assistant.content.push_back(api::TextBlock{"before"});
    assistant.content.push_back(api::ToolUseBlock{"call-A", "inspect", Json{
        {"type", "image"}, {"signature", Json{{"arbitrary", true}}}, {"encrypted_content", "plain argument"}}});
    assistant.content.push_back(api::TextBlock{"after"}); request.messages.push_back(assistant);
    api::Message tool; tool.role = api::Role::Tool;
    api::ToolResultBlock result{"call-A", "actual reply\n\"text\"", true};
    result.blocks.push_back(lubancode::tools::TextContent{"rich text is not an extra SDK reply"});
    tool.content.push_back(result); request.messages.push_back(tool);
    request.tools.push_back(api::ToolDefinition{"inspect", "description", Json{
        {"type", "object"}, {"properties", Json{{"file_data", Json{{"type", "image"}}}}}}});
    return request;
}

// An internal legacy backend still implements only send_stream. Wire material
// here is deliberately independent of SDK projection; the default must retain
// the old selector's unavailable/error distinction and extra_body semantics.
struct Legacy final : api::Backend {
    std::string wire;
    int calls = 0;
    std::string SerializeForDiagnostics(const api::Request&) const override { return wire; }
    std::expected<void, api::Error> send_stream(const api::Request&,
        const std::function<void(const api::StreamEvent&)>& emit, const std::atomic<bool>*) override {
        ++calls; emit(api::MessageDone{"end_turn", {}}); return {};
    }
};

struct Boundary final : lubancode::agent::LoopBoundaryRecorder {
    std::vector<api::Request> requests;
    std::vector<lubancode::agent::RequestPreparedContext> contexts;
    std::string OnRequestPrepared(const api::Request& request, const lubancode::agent::RequestPreparedContext& context) override {
        requests.push_back(request); contexts.push_back(context); return "fixture-request";
    }
    bool OnRequestSent(const std::string&) override { return true; }
    void OnUsageRecorded(const std::string&, const api::Usage&, bool, const std::string&, int, bool, bool, bool, const std::string&) override {}
    bool OnOutputCompleted(const std::string&, const api::Message&, const std::string&, const std::string&) override { return true; }
    void OnOutputFailed(const std::string&, const std::string&) override {}
    void OnOutputCancelled(const std::string&, lubancode::agent::OutputCancelSource) override {}
};

struct Directory {
    fs::path root;
    Directory() {
        static std::atomic<unsigned> serial{0};
        root = fs::temp_directory_path() / ("sdk-model-input-" +
            std::to_string(lubancode::platform::CurrentProcessId()) + "-" +
            std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + "-" +
            std::to_string(++serial));
        fs::create_directories(root);
    }
    ~Directory() { std::error_code ignored; fs::remove_all(root, ignored); }
};

struct SummaryFixture {
    Directory directory;
    v3::V3Writer writer;
    rt::ActionSummarySource source;
    rt::ActionSummaryProfile profile;
    SummaryFixture() {
        auto opened = v3::V3Writer::Start(directory.root / "session.jsonl", "sdk-summary", "run-1", "main system");
        REQUIRE(opened.has_value()); writer = std::move(*opened);
        source.action_id = writer.NewActionId(); source.parent_turn_id = writer.NewTurnId();
        source.text = "SUMMARY-EVIDENCE\n" + std::string(8192, 'S');
        source.execution_state = "done"; source.budget_bytes = 2048;
        auto action = v3::ToolActionSession::Admit(writer, source.parent_turn_id, writer.NewStepId(),
            source.action_id, "queued", std::nullopt, "actual-call");
        REQUIRE(action.Start(writer, "args-ref", {"read", "builtin", "1", "test"}).status == v3::WriteReceipt::Status::Committed);
        const auto finished = action.Finish(writer, 0); REQUIRE(finished.status == v3::WriteReceipt::Status::Committed);
        auto store = v3::ResultStore::Open(directory.root); REQUIRE(store.has_value());
        v3::ResultStore::PersistRequest persist; persist.result_kind = "text"; persist.content = source.text;
        persist.execution_event_ref = finished.id; persist.tool_call_id = source.action_id;
        persist.outputs.push_back({"combined", "text/plain", source.text, true, "", source.text.size(), false});
        const auto saved = store->Persist(persist); REQUIRE(saved.ok);
        source.result_refs = saved.result_ref;
        const auto persisted = action.PersistedResult(writer, saved.result_ref, finished.id);
        REQUIRE(persisted.status == v3::WriteReceipt::Status::Committed); source.persisted_event_ref = persisted.id;
        profile.provider = "sdk-fixture"; profile.wire = "custom"; profile.model = "actual-summary-model";
    }
};

// Faults happen at the real projection boundary. No faux provider serialization
// is added. Sending still reaches the actual SDK Capture if a gate is bypassed.
struct ProjectionFault final : api::Backend {
    enum class Fault { None, Replace, Tools, Unavailable, Error } fault = Fault::None;
    std::unique_ptr<api::Backend> inner;
    std::optional<EffectiveOutputLimit> limit;
    explicit ProjectionFault(std::shared_ptr<Capture> capture) : inner(sdk::detail::AdaptBackend(std::move(capture))) {}
    std::expected<std::optional<api::ModelInputSnapshot>, std::string>
    PrepareModelInput(const api::Request& request) const override {
        if (fault == Fault::Unavailable) return std::optional<api::ModelInputSnapshot>{};
        if (fault == Fault::Error) return std::unexpected("fixture.model_input_error");
        auto result = inner->PrepareModelInput(request);
        if (result && *result) {
            if (fault == Fault::Replace) (**result).input["messages"][0]["text"] = "unrelated material";
            if (fault == Fault::Tools) (**result).input["tools"].push_back(Json{{"name", "unexpected-tool"}});
        }
        return result;
    }
    EffectiveOutputLimit GetEffectiveOutputLimit(const api::Request& request) const override {
        return limit ? *limit : inner->GetEffectiveOutputLimit(request);
    }
    std::expected<void, api::Error> send_stream(const api::Request& request,
        const std::function<void(const api::StreamEvent&)>& emit, const std::atomic<bool>* cancel) override {
        return inner->send_stream(request, emit, cancel);
    }
};
} // namespace

TEST_CASE("SDK model input: projection preserves the actual Generate arguments and excludes controls") {
    Mark("exact-generate");
    const auto capture = std::make_shared<Capture>(); auto backend = sdk::detail::AdaptBackend(capture);
    auto request = RichTextRequest(); const auto snapshot = backend->PrepareModelInput(request);
    REQUIRE(snapshot.has_value()); REQUIRE(snapshot->has_value()); REQUIRE(capture->requests.empty());
    REQUIRE((**snapshot).scope == api::kSdkModelRequestInputScope);
    REQUIRE((**snapshot).output_limit_scope == api::kSdkGenerateOutputLimitScope);
    REQUIRE(backend->SerializeForDiagnostics(request).empty()); REQUIRE(backend->PrepareWireRequest(request).body.is_null());
    REQUIRE_FALSE(backend->BuildWireMessageMap(request).has_value());
    REQUIRE_FALSE(api::HasUnestimatedInput((**snapshot).input));
    REQUIRE(backend->send_stream(request, [](const api::StreamEvent&) {}, nullptr).has_value());
    REQUIRE(capture->requests.size() == 1); const auto& actual = capture->requests.front();
    REQUIRE(actual.model == request.model); REQUIRE(actual.max_output_tokens == request.max_tokens);
    REQUIRE((**snapshot).input.at("system") == actual.system);
    REQUIRE_FALSE((**snapshot).input.contains("model")); REQUIRE_FALSE((**snapshot).input.contains("max_output_tokens"));
    REQUIRE(actual.messages.size() == 4); const auto& messages = (**snapshot).input.at("messages");
    REQUIRE(messages.size() == actual.messages.size());
    for (std::size_t i = 0; i < actual.messages.size(); ++i) {
        const auto& message = actual.messages[i]; const auto& projected = messages[i];
        REQUIRE(projected.at("role") == message.role); REQUIRE(projected.at("text") == message.text);
        REQUIRE(projected.at("tool_calls").size() == message.tool_calls.size());
        REQUIRE(projected.at("tool_replies").size() == message.tool_replies.size());
        for (std::size_t j = 0; j < message.tool_calls.size(); ++j) {
            const auto& call = message.tool_calls[j];
            REQUIRE(projected.at("tool_calls")[j] == Json({{"id", call.id}, {"name", call.name}, {"input_json", call.input_json}}));
            REQUIRE(projected.at("tool_calls")[j].at("input_json").is_string());
        }
        for (std::size_t j = 0; j < message.tool_replies.size(); ++j) {
            const auto& reply = message.tool_replies[j];
            REQUIRE(projected.at("tool_replies")[j] == Json({{"call_id", reply.call_id}, {"text", reply.text}, {"is_error", reply.is_error}}));
        }
    }
    REQUIRE(actual.messages[0].role == "system"); REQUIRE(actual.messages[1].role == "user");
    REQUIRE(actual.messages[2].role == "assistant"); REQUIRE(actual.messages[3].role == "tool");
    REQUIRE(actual.messages[2].text == "beforeafter"); REQUIRE(actual.messages[3].tool_replies.front().is_error);
    REQUIRE(actual.tools.size() == 1); const auto& tool = actual.tools.front();
    REQUIRE((**snapshot).input.at("tools")[0] == Json({{"name", tool.name}, {"description", tool.description}, {"input_schema_json", tool.input_schema_json}}));
    REQUIRE((**snapshot).input.at("tools")[0].at("input_schema_json").is_string());
    const auto estimate = lubancode::hooks::middleware::ComputeUtf8BytesDiv4Estimate((**snapshot).input);
    const auto bytes = (**snapshot).input.dump().size();
    REQUIRE(estimate.at("inputUtf8Bytes") == bytes); REQUIRE(estimate.at("estimatedInputTokens") == bytes / 4 + (bytes % 4 != 0));
    request.model = "different model"; request.max_tokens = 17;
    const auto changed = backend->PrepareModelInput(request); REQUIRE(changed.has_value()); REQUIRE(changed->has_value());
    REQUIRE((**changed).input == (**snapshot).input);
    request = {}; request.system = "only system";
    const auto empty = backend->PrepareModelInput(request); REQUIRE(empty.has_value()); REQUIRE(empty->has_value());
    REQUIRE((**empty).input.at("messages").is_array()); REQUIRE((**empty).input.at("messages").empty());
    REQUIRE((**empty).input.at("tools").is_array()); REQUIRE((**empty).input.at("tools").empty());
    REQUIRE(backend->send_stream(request, [](const api::StreamEvent&) {}, nullptr).has_value());
    REQUIRE(capture->requests.back().messages.empty()); REQUIRE(capture->requests.back().tools.empty());
    REQUIRE_FALSE(capture->requests.back().max_output_tokens.has_value());
}

TEST_CASE("SDK model input: unsupported history and cancellation never create an unmeasured send") {
    Mark("refused-and-cancelled");
    const auto capture = std::make_shared<Capture>(); auto backend = sdk::detail::AdaptBackend(capture);
    api::ToolResultBlock structured{"call", "text", false}; structured.structured_content = Json{{"value", 1}};
    api::ToolResultBlock rich{"call", "text", false}; rich.blocks.push_back(lubancode::tools::ImageContent{});
    const std::vector<api::ContentBlock> refused = {api::ImageBlock{"image/png", "AA==", "image"},
        api::ThinkingBlock{"thinking", "signature"}, api::ModelImageBlock{}, api::RedactedThinkingBlock{"opaque"},
        api::ServerToolUseBlock{"server-call", "search"}, api::ServerToolResultBlock{"server-call"}, structured, rich};
    for (const auto& block : refused) {
        auto request = RichTextRequest(); request.messages.back().content = {block};
        const auto measured = backend->PrepareModelInput(request); REQUIRE_FALSE(measured.has_value());
        const auto sent = backend->send_stream(request, [](const api::StreamEvent&) {}, nullptr); REQUIRE_FALSE(sent.has_value());
        REQUIRE(measured.error() == sent.error().message); REQUIRE(measured.error().starts_with("sdk.backend.unsupported_content"));
        REQUIRE(capture->requests.empty());
    }
    auto invalid_json_text = RichTextRequest();
    invalid_json_text.tools.front().input_schema["invalid_utf8"] = std::string(1, static_cast<char>(0xff));
    const auto conversion_error = backend->PrepareModelInput(invalid_json_text); REQUIRE_FALSE(conversion_error.has_value());
    const auto conversion_send = backend->send_stream(invalid_json_text, [](const api::StreamEvent&) {}, nullptr);
    REQUIRE_FALSE(conversion_send.has_value()); REQUIRE(conversion_error.error() == conversion_send.error().message);
    REQUIRE(conversion_error.error().starts_with("sdk.backend.exception")); REQUIRE(capture->requests.empty());
    auto request = RichTextRequest(); std::atomic<bool> cancel{true};
    std::vector<api::StreamEvent> emitted;
    const auto measured = backend->PrepareModelInput(request); REQUIRE(measured.has_value()); REQUIRE(measured->has_value());
    const auto before = backend->send_stream(request, [&](const api::StreamEvent& event) { emitted.push_back(event); }, &cancel);
    REQUIRE_FALSE(before.has_value()); REQUIRE(before.error().kind == api::ErrorKind::Cancelled);
    REQUIRE(capture->requests.empty()); REQUIRE(emitted.empty());
    cancel = false; capture->during_generate = [&] { cancel = true; };
    const auto after = backend->send_stream(request, [&](const api::StreamEvent& event) { emitted.push_back(event); }, &cancel);
    REQUIRE_FALSE(after.has_value()); REQUIRE(after.error().kind == api::ErrorKind::Cancelled);
    REQUIRE(capture->requests.size() == 1);
    // Returned usage is a fact even when cancellation rejects the reply body.
    // Exactly one nonterminal snapshot means no content or success frame leaks.
    REQUIRE(emitted.size() == 1);
    const auto* snapshot = std::get_if<api::UsageSnapshot>(&emitted.front());
    REQUIRE(snapshot != nullptr); REQUIRE(snapshot->usage_reported);
    REQUIRE(snapshot->usage.input_tokens == 11); REQUIRE(snapshot->usage.output_tokens == 7);
    REQUIRE(snapshot->usage.cache_read_tokens == 0); REQUIRE(snapshot->usage.cache_creation_tokens == 0);
    REQUIRE(snapshot->usage.output_reasoning_tokens == 0);
    REQUIRE_FALSE(snapshot->cache_read_reported); REQUIRE_FALSE(snapshot->cache_creation_reported);
}

TEST_CASE("SDK model input: legacy unavailable invalid and all four real provider projections remain distinct") {
    Mark("legacy-and-four-wire");
    Legacy legacy; auto request = RichTextRequest(); const auto unavailable = legacy.PrepareModelInput(request);
    REQUIRE(unavailable.has_value()); REQUIRE_FALSE(unavailable->has_value()); REQUIRE(legacy.calls == 0);
    for (const auto& wire : {std::string("not-json"), std::string("{}"), std::string("{\"messages\":[],\"previous_response_id\":\"hidden\"}")}) {
        legacy.wire = wire; const auto invalid = legacy.PrepareModelInput(request); REQUIRE_FALSE(invalid.has_value());
        REQUIRE(invalid.error() == api::ModelInputSnapshotFromWire(wire).error()); REQUIRE(legacy.calls == 0);
    }
    legacy.wire = R"({"messages":[],"system":"provider-final","max_tokens":99})";
    const auto valid = legacy.PrepareModelInput(request); REQUIRE(valid.has_value()); REQUIRE(valid->has_value());
    REQUIRE((**valid).scope == api::kProviderWireInputScope); REQUIRE((**valid).input.at("system") == "provider-final");
    std::vector<std::unique_ptr<api::Backend>> providers;
    providers.push_back(std::make_unique<api::anthropic::AnthropicBackend>("http://127.0.0.1:1", ""));
    providers.push_back(std::make_unique<api::chat::ChatCompletionsBackend>("http://127.0.0.1:1", ""));
    providers.push_back(std::make_unique<api::responses::ResponsesBackend>("http://127.0.0.1:1", ""));
    providers.push_back(std::make_unique<api::gemini::GeminiBackend>("http://127.0.0.1:1", ""));
    for (const auto& provider : providers) {
        const auto serialized = provider->SerializeForDiagnostics(request);
        const auto selected = api::ModelInputSnapshotFromWire(serialized); REQUIRE(selected.has_value());
        const auto prepared = provider->PrepareModelInput(request); REQUIRE(prepared.has_value()); REQUIRE(prepared->has_value());
        REQUIRE((**prepared).input == *selected); REQUIRE((**prepared).scope == api::kProviderWireInputScope);
        REQUIRE((**prepared).output_limit_scope == api::kProviderWireOutputLimitScope);
        const auto original = provider->PrepareWireRequest(request); REQUIRE(original.body.is_object());
        REQUIRE(original.output_limit == provider->GetEffectiveOutputLimit(request).tokens);
    }
    request.extra_body = {{"messages", Json::array({Json{{"role", "user"}, {"content", "overridden wire"}}})}, {"max_tokens", nullptr}};
    api::chat::ChatCompletionsBackend chat("http://127.0.0.1:1", "");
    const auto overridden = chat.PrepareModelInput(request); REQUIRE(overridden.has_value()); REQUIRE(overridden->has_value());
    REQUIRE((**overridden).input.at("messages")[0].at("content") == "overridden wire");
    REQUIRE_FALSE(chat.BuildWireMessageMap(request).has_value()); REQUIRE_FALSE(chat.GetEffectiveOutputLimit(request).tokens.has_value());
    REQUIRE(chat.GetEffectiveOutputLimit(request).overridden);
}

TEST_CASE("SDK model input: Rebuildable forwards Generate scope unavailable and errors without creating wire") {
    Mark("rebuildable-forwarding");
    const auto capture = std::make_shared<Capture>();
    std::shared_ptr<api::Backend> inner(sdk::detail::AdaptBackend(capture)); rt::assembly::RebuildableBackend outer(inner);
    const auto request = RichTextRequest(); const auto direct = inner->PrepareModelInput(request);
    const auto projected = outer.PrepareModelInput(request); REQUIRE(projected.has_value()); REQUIRE(projected->has_value());
    REQUIRE((**projected).input == (**direct).input); REQUIRE((**projected).scope == api::kSdkModelRequestInputScope);
    REQUIRE(outer.SerializeForDiagnostics(request).empty()); REQUIRE_FALSE(outer.BuildWireMessageMap(request).has_value());
    REQUIRE(outer.send_stream(request, [](const api::StreamEvent&) {}, nullptr).has_value()); REQUIRE(capture->requests.size() == 1);
    auto legacy = std::make_shared<Legacy>(); rt::assembly::RebuildableBackend legacy_outer(legacy);
    const auto unavailable = legacy_outer.PrepareModelInput(request); REQUIRE(unavailable.has_value()); REQUIRE_FALSE(unavailable->has_value());
    legacy->wire = "invalid"; const auto error = legacy_outer.PrepareModelInput(request); REQUIRE_FALSE(error.has_value());
    REQUIRE(error.error() == "tool_batch.adapter_snapshot_invalid"); REQUIRE(legacy->calls == 0);
    // The real loop's final gate, Hook input and prepared context share a frozen
    // value. Wire mapping remains unavailable even when SDK measurement exists.
    lubancode::tools::ToolRegistry registry; lubancode::agent::AgentProfile profile;
    profile.request.model = "loop-model"; profile.system_prompt = "loop-system";
    lubancode::agent::Agent agent(outer, registry, profile); Boundary boundary;
    Json hook_input; rt::PreRequestBudget hook_budget;
    lubancode::agent::TurnWiring wiring; wiring.turn_id = "turn-fixture"; wiring.boundary_recorder = &boundary;
    wiring.rewrite_tool_results_for_history = [](api::Message&) { return rt::ToolResultsCommitReceipt{}; };
    wiring.on_pre_request_hooks = [&](const std::string&, const std::string&, const Json& input, const rt::PreRequestBudget& budget) {
        hook_input = input; hook_budget = budget; return std::string();
    };
    const auto run = agent.Run("actual loop input", wiring); REQUIRE_MESSAGE(run.has_value(), (run ? std::string() : run.error()));
    REQUIRE(boundary.requests.size() == 1); REQUIRE(boundary.contexts.size() == 1); REQUIRE(capture->requests.size() == 2);
    REQUIRE(boundary.contexts.front().model_input_snapshot.has_value());
    REQUIRE_FALSE(boundary.contexts.front().wire_message_map.has_value());
    const auto& frozen = *boundary.contexts.front().model_input_snapshot;
    REQUIRE(frozen.scope == api::kSdkModelRequestInputScope); REQUIRE(frozen.output_limit_scope == api::kSdkGenerateOutputLimitScope);
    REQUIRE(frozen.input == hook_input); REQUIRE(hook_budget.model_input_snapshot_scope == frozen.scope);
    REQUIRE(hook_budget.output_limit_scope == frozen.output_limit_scope);
    const auto& actual = capture->requests.back(); REQUIRE(frozen.input.at("system") == actual.system);
    REQUIRE(frozen.input.at("messages").size() == actual.messages.size());
    for (std::size_t i = 0; i < actual.messages.size(); ++i) {
        REQUIRE(frozen.input.at("messages")[i].at("text") == actual.messages[i].text);
        REQUIRE(frozen.input.at("messages")[i].at("role") == actual.messages[i].role);
    }
    Directory directory;
    auto opened = v3::V3Writer::Start(directory.root / "main.jsonl", "sdk-main-measurement", "run-1", actual.system);
    REQUIRE(opened.has_value()); auto writer = std::move(*opened);
    rt::V3SessionBooks books; books.writer = &writer; books.system_content = actual.system;
    rt::TrajectoryTurnBridge bridge(&writer, &books, {}, {"sdk-fixture", "custom"});
    bridge.BeginTurn(writer.NewTurnId(), "external_user");
    for (const auto& message : boundary.requests.front().messages) bridge.RecordInput(message);
    const auto request_id = bridge.OnRequestPrepared(boundary.requests.front(), boundary.contexts.front()); REQUIRE_FALSE(request_id.empty());
    const auto ledger = v3::ReadV3Ledger(writer.path()); REQUIRE(ledger.has_value());
    const v3::EventLine* prepared = nullptr;
    for (const auto& event : ledger->events) {
        if (event.kind != v3::EventKindV3::ModelRequestPrepared || event.request_id != request_id) continue;
        REQUIRE(prepared == nullptr); prepared = &event;
    }
    REQUIRE(prepared != nullptr); REQUIRE(v3::CheckPreparedAgainstChain(*ledger, prepared->event_id).empty());
    const auto compact = frozen.input.dump();
    REQUIRE(prepared->payload.at("modelInputSnapshotScope") == frozen.scope);
    REQUIRE(prepared->payload.at("outputLimitScope") == frozen.output_limit_scope);
    REQUIRE(prepared->payload.at("modelInputSnapshotSha256") == lubancode::platform::Sha256Hex(compact));
    REQUIRE(prepared->payload.at("modelInputSnapshotUtf8Bytes") == compact.size());
    REQUIRE(prepared->payload.at("modelInputSnapshotFingerprintAlgorithm") == "sha256-compact_json_utf8_v1");
    REQUIRE_FALSE(prepared->payload.contains("modelInputSnapshot"));
}

TEST_CASE("SDK model input: real summary preserves exact material requested output and committed adoption") {
    Mark("summary-prepared-adopted");
    SummaryFixture fixture; const auto capture = std::make_shared<Capture>(); auto backend = sdk::detail::AdaptBackend(capture);
    int remaining = 8; const auto result = rt::SummarizeActionResult(fixture.writer, *backend, fixture.profile, fixture.source, remaining);
    REQUIRE_MESSAGE(result.accepted, result.reason); REQUIRE(capture->requests.size() == 1); REQUIRE(result.model_calls == 1); REQUIRE(remaining == 7);
    const auto& request = capture->requests.front(); REQUIRE(request.model == fixture.profile.model);
    REQUIRE(request.tools.empty()); REQUIRE(request.max_output_tokens == 1024); REQUIRE(request.messages.size() == 1);
    REQUIRE(request.messages.front().role == "user");
    const auto prompt = Json::parse(request.messages.front().text); REQUIRE(prompt.at("material") == fixture.source.text);
    REQUIRE(prompt.at("source_result_event_ref") == fixture.source.persisted_event_ref);
    REQUIRE(prompt.at("action_id") == fixture.source.action_id); REQUIRE(prompt.at("final_preview_byte_budget") == fixture.source.budget_bytes);
    auto ledger = v3::ReadV3Ledger(fixture.writer.path()); REQUIRE(ledger.has_value()); unsigned prepared_count = 0;
    for (const auto& event : ledger->events) {
        if (event.kind != v3::EventKindV3::ModelRequestPrepared || event.payload.value("purpose", "") != "action_summary") continue;
        ++prepared_count; const auto& payload = event.payload; const auto& input = payload.at("modelInputSnapshot");
        REQUIRE(payload.at("modelInputSnapshotScope") == api::kSdkModelRequestInputScope);
        REQUIRE(payload.at("outputLimitScope") == api::kSdkGenerateOutputLimitScope);
        REQUIRE(input.at("system") == request.system); REQUIRE(input.at("messages")[0].at("text") == request.messages.front().text);
        REQUIRE(input.at("tools").empty()); REQUIRE(payload.at("outputReserveTokens") == *request.max_output_tokens);
        const auto measured = lubancode::hooks::middleware::ComputeUtf8BytesDiv4Estimate(input);
        REQUIRE(payload.at("tokenEstimate").at("inputUtf8Bytes") == measured.at("inputUtf8Bytes"));
        REQUIRE(payload.at("tokenEstimate").at("estimatedInputTokens") == measured.at("estimatedInputTokens"));
        REQUIRE(payload.at("tokenEstimate").at("modelInputSnapshotScope") == api::kSdkModelRequestInputScope);
    }
    REQUIRE(prepared_count == 1);
    auto action = v3::ToolActionSession::Reopen(fixture.source.parent_turn_id, "step-source", fixture.source.action_id);
    const auto selected = action.SelectResult(fixture.writer, {fixture.source.persisted_event_ref}, {}, "done", std::nullopt,
        lubancode::trajectory::Durability::PowerLoss, result.terminal_event_ref);
    REQUIRE(selected.status == v3::WriteReceipt::Status::Committed);
    REQUIRE(action.AppendToolMessage(fixture.writer, result.text, selected.id).status == v3::WriteReceipt::Status::Committed);
    ledger = v3::ReadV3Ledger(fixture.writer.path()); REQUIRE(ledger.has_value());
    const auto projected = v3::ProjectModelContext(*ledger); REQUIRE(projected.inputs.size() == 1);
    REQUIRE(projected.inputs.front().message.at("content") == result.text);
    const auto expanded = v3::ExpandResultPreview(*ledger, fixture.directory.root, ledger->context.chain.back().message_ref);
    REQUIRE(expanded.complete); REQUIRE(expanded.summary_event_ref == result.terminal_event_ref); REQUIRE(expanded.summary_candidate_refs.size() == 1);
}

TEST_CASE("SDK model input: summary failures retain budget gates and stop before actual Generate") {
    Mark("summary-fail-closed");
    const auto reject = [](ProjectionFault::Fault fault, std::optional<api::Backend::EffectiveOutputLimit> limit,
                           bool tiny, const char* reason) {
        SummaryFixture fixture; const auto capture = std::make_shared<Capture>(); ProjectionFault backend(capture);
        backend.fault = fault; backend.limit = limit; if (tiny) fixture.profile.window_tokens = 100;
        int remaining = 8; const auto result = rt::SummarizeActionResult(fixture.writer, backend, fixture.profile, fixture.source, remaining);
        REQUIRE_FALSE(result.accepted); REQUIRE(result.reason == reason); REQUIRE(capture->requests.empty()); REQUIRE(result.model_calls == 0); REQUIRE(remaining == 8);
        const auto ledger = v3::ReadV3Ledger(fixture.writer.path()); REQUIRE(ledger.has_value());
        REQUIRE(ledger->FindEvent(result.terminal_event_ref) != nullptr);
        REQUIRE(ledger->FindEvent(result.terminal_event_ref)->payload.at("state") == "rejected");
        for (const auto& event : ledger->events) REQUIRE(event.kind != v3::EventKindV3::ModelRequestSent);
    };
    reject(ProjectionFault::Fault::Replace, {}, false, "summary_adapter_replaced_material");
    reject(ProjectionFault::Fault::Tools, {}, false, "summary_tools_not_allowed");
    reject(ProjectionFault::Fault::Unavailable, {}, false, "tool_batch.adapter_snapshot_unavailable");
    reject(ProjectionFault::Fault::Error, {}, false, "fixture.model_input_error");
    reject(ProjectionFault::Fault::None, {}, true, "summary_input_capacity_exceeded");
    reject(ProjectionFault::Fault::None, api::Backend::EffectiveOutputLimit{std::nullopt, false}, false, "summary_output_limit_unknown");
    reject(ProjectionFault::Fault::None, api::Backend::EffectiveOutputLimit{2048, true}, false, "summary_output_override");
}
