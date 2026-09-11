#include <doctest/doctest.h>

#include <filesystem>
#include <functional>

#include "api/chat/request.hpp"
#include "runtime/action_summary.hpp"
#include "trajectory/v3/reader.hpp"
#include "trajectory/v3/result_store.hpp"
#include "trajectory/v3/tool_action.hpp"

using namespace lubancode;
namespace v3 = trajectory::v3;
namespace {
struct SummaryBackend : api::Backend {
    int calls = 0;
    bool truncated = false;
    bool emit_reasoning = false;
    bool emit_error = false;
    std::string reply = R"({"summary":"read finished","side_effects":["none observed"],"open_items":["inspect evidence"],"evidence":["combined output"]})";
    std::function<void()> during_call;
    nlohmann::json extra_body = nlohmann::json::object();
    std::vector<api::Request> requests;
    std::string SerializeForDiagnostics(const api::Request& request) const override {
        return api::chat::BuildRequestJson(request, extra_body).dump();
    }
    std::expected<void, api::Error> send_stream(const api::Request& request,
        const std::function<void(const api::StreamEvent&)>& emit, const std::atomic<bool>*) override {
        ++calls;
        requests.push_back(request);
        if (during_call) during_call();
        emit(api::MessageStart{"summary-response", request.model});
        if (emit_reasoning) {
            emit(api::ThinkingDelta{"retained private reasoning"});
            emit(api::ContentBlockDone{0});
        }
        emit(api::TextDelta{reply});
        emit(api::ContentBlockDone{0});
        api::MessageDone done{truncated ? "max_tokens" : "end_turn", api::Usage{11, 7, 0, 0, 0}};
        done.usage_reported = true;
        emit(done);
        if (emit_error) emit(api::StreamError{"provider stream failed", "stream_failed"});
        return {};
    }
};

struct Fixture {
    std::filesystem::path root;
    v3::V3Writer writer;
    runtime::ActionSummarySource source;
    runtime::ActionSummaryProfile profile;
    explicit Fixture(const char* name, std::size_t bytes = 8192) {
        root = std::filesystem::temp_directory_path() / (std::string("lubancode-action-summary-") + name);
        std::error_code ec;
        std::filesystem::remove_all(root, ec);
        std::filesystem::create_directories(root);
        auto opened = v3::V3Writer::Start(root / "session.jsonl", "summary-session", "run-1", "system");
        REQUIRE(opened.has_value());
        writer = std::move(*opened);
        source.action_id = writer.NewActionId();
        source.parent_turn_id = writer.NewTurnId();
        source.text.assign(bytes, 'x');
        source.execution_state = "done";
        source.budget_bytes = 2048;
        auto action = v3::ToolActionSession::Admit(writer, source.parent_turn_id, writer.NewStepId(),
            source.action_id, "queued", std::nullopt, "call-source");
        REQUIRE(action.Start(writer, "args-ref", {"read", "builtin", "1", "test"}).status == v3::WriteReceipt::Status::Committed);
        const auto finished = action.Finish(writer, 0);
        REQUIRE(finished.status == v3::WriteReceipt::Status::Committed);
        auto store = v3::ResultStore::Open(root);
        REQUIRE(store.has_value());
        v3::ResultStore::PersistRequest persist;
        persist.result_kind = "text";
        persist.content = source.text;
        persist.execution_event_ref = finished.id;
        persist.tool_call_id = source.action_id;
        persist.outputs.push_back({"combined", "text/plain", source.text, true, "", bytes, false});
        const auto saved = store->Persist(persist);
        REQUIRE(saved.ok);
        source.result_refs = saved.result_ref;
        const auto persisted = action.PersistedResult(writer, saved.result_ref, finished.id);
        REQUIRE(persisted.status == v3::WriteReceipt::Status::Committed);
        source.persisted_event_ref = persisted.id;
        profile.provider = "test"; profile.wire = "openai-chat-completions"; profile.model = "summary-model";
    }
};
}

TEST_CASE("action summary has independent requests usage sources and committed adoption") {
    Fixture f("adopt");
    SummaryBackend backend;
    int remaining = 8;
    const auto revision = f.writer.context().revision;
    const auto result = runtime::SummarizeActionResult(f.writer, backend, f.profile, f.source, remaining);
    REQUIRE_MESSAGE(result.accepted, result.reason);
    CHECK(backend.calls == 1);
    CHECK(remaining == 7);
    CHECK(f.writer.context().revision == revision);
    CHECK(backend.requests.front().model == "summary-model");
    CHECK(backend.requests.front().max_tokens == 1024);
    auto ledger = v3::ReadV3Ledger(f.writer.path());
    REQUIRE(ledger.has_value());
    REQUIRE(ledger->context.chain.size() == 1);
    bool usage_found = false;
    for (const auto& message : ledger->messages) {
        if (message.purpose == v3::MessagePurpose::ActionSummary && message.message.at("role") == "assistant") {
            REQUIRE(message.usage.has_value());
            CHECK(message.usage->at("inputTokens") == 11);
            usage_found = true;
        }
    }
    CHECK(usage_found);
    auto action = v3::ToolActionSession::Reopen(f.source.parent_turn_id, "step-source", f.source.action_id);
    const auto selected = action.SelectResult(f.writer, {f.source.persisted_event_ref}, {}, "done", std::nullopt,
        trajectory::Durability::PowerLoss, result.terminal_event_ref);
    REQUIRE(selected.status == v3::WriteReceipt::Status::Committed);
    REQUIRE(action.AppendToolMessage(f.writer, result.text, selected.id).status == v3::WriteReceipt::Status::Committed);
    ledger = v3::ReadV3Ledger(f.writer.path());
    REQUIRE(ledger.has_value());
    const auto projected = v3::ProjectModelContext(*ledger);
    REQUIRE(projected.inputs.size() == 1);
    CHECK(projected.inputs.front().message.at("content") == result.text);
    const auto preview = v3::ExpandResultPreview(*ledger, f.root, ledger->context.chain.back().message_ref);
    CHECK(preview.complete);
    CHECK(preview.summary_event_ref == result.terminal_event_ref);
    CHECK(preview.summary_candidate_refs.size() == 1);
}

TEST_CASE("summary capacity no gain truncation and call bounds terminate without adoption") {
    SUBCASE("own window too small") {
        Fixture f("tiny"); SummaryBackend backend; int remaining = 8;
        f.profile.window_tokens = 100;
        const auto result = runtime::SummarizeActionResult(f.writer, backend, f.profile, f.source, remaining);
        CHECK_FALSE(result.accepted); CHECK(backend.calls == 0);
        CHECK(result.reason == "summary_input_capacity_exceeded");
    }
    SUBCASE("no gain") {
        Fixture f("gain"); SummaryBackend backend; int remaining = 8;
        backend.reply = nlohmann::json{{"summary", f.source.text}, {"side_effects", nlohmann::json::array()},
            {"open_items", nlohmann::json::array()}, {"evidence", nlohmann::json::array()}}.dump();
        const auto result = runtime::SummarizeActionResult(f.writer, backend, f.profile, f.source, remaining);
        CHECK_FALSE(result.accepted); CHECK(backend.calls == 1); CHECK(result.reason == "summary_no_gain");
    }
    SUBCASE("truncated") {
        Fixture f("truncated"); SummaryBackend backend; int remaining = 8; backend.truncated = true;
        const auto result = runtime::SummarizeActionResult(f.writer, backend, f.profile, f.source, remaining);
        CHECK_FALSE(result.accepted); CHECK(result.reason == "summary_truncated_or_nontext");
    }
    SUBCASE("too many chunks") {
        Fixture f("chunks", 300000); SummaryBackend backend; int remaining = 8;
        const auto result = runtime::SummarizeActionResult(f.writer, backend, f.profile, f.source, remaining);
        CHECK_FALSE(result.accepted); CHECK(backend.calls == 0); CHECK(result.reason == "call_budget_exceeded");
    }
}

TEST_CASE("summary map reduce validates both capacities and keeps a bounded call count") {
    Fixture f("reduce", 50000); SummaryBackend backend; int remaining = 8;
    const auto result = runtime::SummarizeActionResult(f.writer, backend, f.profile, f.source, remaining);
    REQUIRE_MESSAGE(result.accepted, result.reason);
    CHECK(backend.calls == 3);
    CHECK(remaining == 5);
}

TEST_CASE("candidate source conflict and cancellation never enter the main chain") {
    Fixture f("conflict"); SummaryBackend backend; int remaining = 8;
    backend.during_call = [&] {
        v3::MessageDraft user;
        user.turn_id = f.source.parent_turn_id;
        user.message = {{"role", "user"}, {"content", "new input"}};
        const auto saved = f.writer.AppendMessage(std::move(user), trajectory::Durability::PowerLoss);
        REQUIRE(saved.status == v3::WriteReceipt::Status::Committed);
        REQUIRE(f.writer.AdmitMessages({saved.id}).status == v3::WriteReceipt::Status::Committed);
    };
    const auto result = runtime::SummarizeActionResult(f.writer, backend, f.profile, f.source, remaining);
    CHECK_FALSE(result.accepted);
    CHECK(result.reason == "source_context_conflict");
    std::atomic<bool> cancel{true}; f.profile.cancel = &cancel;
    const auto cancelled = runtime::SummarizeActionResult(f.writer, backend, f.profile, f.source, remaining);
    CHECK_FALSE(cancelled.accepted); CHECK(cancelled.reason == "cancelled"); CHECK(backend.calls == 1);
}

TEST_CASE("summary persistence failures do not publish candidates and bound physical sends") {
    for (int fail_at = 1; fail_at <= 8; ++fail_at) {
        CAPTURE(fail_at);
        Fixture f(("io-" + std::to_string(fail_at)).c_str());
        const auto path = f.writer.path();
        f.writer = {};
        int writes = 0;
        v3::V3WriterOptions options;
        options.inject_io_failure = [&]() -> std::optional<std::string> {
            if (++writes == fail_at) return "injected.summary.write";
            return std::nullopt;
        };
        auto reopened = v3::V3Writer::Continue(path, options);
        REQUIRE(reopened.has_value());
        f.writer = std::move(*reopened);
        SummaryBackend backend; int remaining = 8;
        const auto result = runtime::SummarizeActionResult(f.writer, backend, f.profile, f.source, remaining);
        CHECK_FALSE(result.accepted);
        CHECK(result.persistence_failed);
        CHECK(backend.calls <= 1);
        if (fail_at <= 4) CHECK(backend.calls == 0);
        CHECK(f.writer.context().chain.size() == 1);
    }
}

TEST_CASE("summary refuses changed source bytes before sampling") {
    Fixture f("source-hash"); SummaryBackend backend; int remaining = 8;
    f.source.text[0] = 'z';
    const auto result = runtime::SummarizeActionResult(f.writer, backend, f.profile, f.source, remaining);
    CHECK_FALSE(result.accepted); CHECK(result.reason == "source_hash_mismatch"); CHECK(backend.calls == 0);
}

TEST_CASE("summary reasoning is retained and a stream error cannot masquerade as success") {
    Fixture f("reasoning"); SummaryBackend backend; int remaining = 8;
    backend.emit_reasoning = true;
    const auto result = runtime::SummarizeActionResult(f.writer, backend, f.profile, f.source, remaining);
    REQUIRE_MESSAGE(result.accepted, result.reason);
    auto ledger = v3::ReadV3Ledger(f.writer.path());
    REQUIRE(ledger.has_value());
    bool retained = false;
    for (const auto& message : ledger->messages) {
        if (message.purpose == v3::MessagePurpose::ActionSummary && message.message.at("role") == "assistant") {
            for (const auto& block : message.message.at("content")) {
                if (block.value("type", "") == "thinking") retained = block.at("text") == "retained private reasoning";
            }
        }
    }
    CHECK(retained);
    backend.emit_error = true;
    const auto failed = runtime::SummarizeActionResult(f.writer, backend, f.profile, f.source, remaining);
    CHECK_FALSE(failed.accepted); CHECK(failed.reason == "summary_provider_error");
    ledger = v3::ReadV3Ledger(f.writer.path());
    REQUIRE(ledger.has_value());
    CHECK(ledger->FindEvent(failed.terminal_event_ref)->payload.at("state") == "failed");
}

TEST_CASE("summary rejects adapter overrides that enable tools or replace evidence") {
    Fixture f("adapter-overrides"); SummaryBackend backend; int remaining = 8;
    backend.extra_body = {{"tools", nlohmann::json::array({"disabled-test-tool"})}};
    const auto tool = runtime::SummarizeActionResult(f.writer, backend, f.profile, f.source, remaining);
    CHECK_FALSE(tool.accepted); CHECK(tool.reason == "summary_tools_not_allowed"); CHECK(backend.calls == 0);
    backend.extra_body = {{"messages", {{{"role", "user"}, {"content", "unrelated"}}}}};
    const auto replaced = runtime::SummarizeActionResult(f.writer, backend, f.profile, f.source, remaining);
    CHECK_FALSE(replaced.accepted); CHECK(replaced.reason == "summary_adapter_replaced_material"); CHECK(backend.calls == 0);
}

TEST_CASE("summary reader rejects an adopted body that does not match the validated hash") {
    Fixture f("bad-selected-body"); SummaryBackend backend; int remaining = 8;
    const auto result = runtime::SummarizeActionResult(f.writer, backend, f.profile, f.source, remaining);
    REQUIRE(result.accepted);
    auto action = v3::ToolActionSession::Reopen(f.source.parent_turn_id, "step-source", f.source.action_id);
    const auto selected = action.SelectResult(f.writer, {f.source.persisted_event_ref}, {}, "done", std::nullopt,
        trajectory::Durability::PowerLoss, result.terminal_event_ref);
    REQUIRE(selected.status == v3::WriteReceipt::Status::Committed);
    REQUIRE(action.AppendToolMessage(f.writer, "unvalidated replacement", selected.id).status == v3::WriteReceipt::Status::Committed);
    const auto ledger = v3::ReadV3Ledger(f.writer.path());
    CHECK_FALSE(ledger.has_value());
    if (!ledger) CHECK(ledger.error() == "v3reader.invalid_action_summary_selection");
}
