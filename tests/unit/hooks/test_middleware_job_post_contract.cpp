#include <doctest/doctest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <future>
#include <iostream>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "hooks/middleware.hpp"
#include "runtime/middleware_deferred_effects.hpp"
#include "runtime/middleware_v3_sink.hpp"
#include "trajectory/v3/reader.hpp"
#include "trajectory/v3/result_store.hpp"
#include "trajectory/v3/tool_action.hpp"
#include "trajectory/v3/writer.hpp"
#ifdef LUBANCORE_TEST_JOB_POST_SDK
#include <lubancore/core.hpp>
#endif

namespace {
namespace fs = std::filesystem;
namespace mw = lubancode::hooks::middleware;
namespace rt = lubancode::runtime;
namespace v3 = lubancode::trajectory::v3;
using Json = nlohmann::json;
using Source = mw::DispatchFailureSource;
using Kind = mw::DispatchOutcome::Kind;
using Stage = rt::MiddlewareReceiptStage;
using Gap = rt::MiddlewareReceiptGap;
using Deferred = rt::middleware_detail::DeferredEffects;
using namespace std::chrono_literals;
constexpr auto JobPost = mw::DispatchReturnContract::JobPostSupplementsV1;

void Mark(const char* stage) {
    std::cout << "[middleware-job-post-contract-path] " << stage << '\n' << std::flush;
}
void Committed(const v3::WriteReceipt& receipt) {
    REQUIRE_MESSAGE(receipt.status == v3::WriteReceipt::Status::Committed, receipt.error_message);
    REQUIRE_FALSE(receipt.id.empty()); REQUIRE(receipt.seq > 0); REQUIRE_FALSE(receipt.line_hash.empty());
}
v3::V3Ledger Read(const fs::path& path) {
    auto result = v3::ReadV3Ledger(path);
    const std::string error = result.has_value() ? std::string() : result.error();
    REQUIRE_MESSAGE(result.has_value(), error);
    return std::move(*result);
}
struct Directory {
    fs::path path;
    Directory() {
        static std::atomic<unsigned> next{1};
        for (unsigned retry = 0; retry < 64; ++retry) {
            const auto candidate = fs::temp_directory_path() / ("lubancode-post-values-" +
                std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + "-" +
                std::to_string(next.fetch_add(1)));
            std::error_code error;
            if (fs::create_directory(candidate, error)) { path = candidate; break; }
            const bool collision = !error || error == std::errc::file_exists;
            REQUIRE_MESSAGE(collision, error.message());
        }
        REQUIRE_FALSE(path.empty());
    }
    ~Directory() { std::error_code ignored; fs::remove_all(path, ignored); }
};
struct Rig {
    Directory directory;
    fs::path path = directory.path / "session.jsonl";
    std::optional<v3::V3Writer> writer;
    v3::WriteReceipt terminal, persisted;
    const std::string turn = "turn-000001", step = "step-000001", action = "action-business-000001";
    explicit Rig(v3::V3WriterOptions options = {}) {
        static std::atomic<unsigned> next{1};
        auto opened = v3::V3Writer::Start(path, "20261003-180000-MWP" + std::to_string(next.fetch_add(1)),
            "run-000001", "Post return fixture", Json::object(), std::move(options));
        const std::string error = opened.has_value() ? std::string() : opened.error();
        REQUIRE_MESSAGE(opened.has_value(), error); writer.emplace(std::move(*opened));
        auto business = v3::ToolActionSession::Admit(*writer, turn, step, action, "queued", std::nullopt,
            "provider-business-1", Json::object(), v3::Durability::PowerLoss);
        v3::ToolIdentity identity{"run_command", "builtin", "v1", "local"};
        Committed(business.Start(*writer, "args-business-1", identity, std::nullopt, Json::object(), v3::Durability::PowerLoss));
        terminal = business.Finish(*writer, 0, 1); Committed(terminal);
        auto store = v3::ResultStore::Open(directory.path);
        const std::string store_error = store.has_value() ? std::string() : store.error();
        REQUIRE_MESSAGE(store.has_value(), store_error);
        v3::ResultStore::PersistRequest raw;
        raw.result_kind = "text"; raw.content = "actual fixture raw";
        raw.outputs.push_back({"combined", "text/plain", raw.content});
        raw.execution_event_ref = terminal.id; raw.tool_call_id = action;
        raw.preview_policy = {{"maxPreviewBytes", 32768}};
        const auto saved = store->Persist(raw); REQUIRE_MESSAGE(saved.ok, saved.error);
        persisted = business.PersistedResult(*writer, saved.result_ref, terminal.id, 1); Committed(persisted);
    }
    rt::CapturedMiddlewareSink Capture(const mw::MiddlewareDispatcher& dispatcher) {
        const auto ledger = Read(path);
        const auto term = v3::MakeOwnedJobReference(ledger, terminal.id);
        const auto raw = v3::MakeOwnedJobReference(ledger, persisted.id);
        REQUIRE(term.has_value()); REQUIRE(raw.has_value());
        auto capture = rt::CaptureMiddlewareReceipts(*writer,
            {writer->session_id(), writer->run_id(), turn, step, action, 1, dispatcher.registry().revision(), *term, *raw, 256});
        const std::string error = capture.has_value() ? std::string() : capture.error();
        REQUIRE_MESSAGE(capture.has_value(), error);
        return std::move(*capture);
    }
    mw::DispatchTrigger Trigger() const {
        mw::DispatchTrigger result;
        result.turn_id = turn; result.step_id = step; result.action_id = action;
        result.input = {{"arguments", {{"command", "fixture"}}},
            {"result", {{"text", "actual fixture raw"}, {"isError", false}, {"outcome", "done"}, {"errorCode", ""}}}};
        return result;
    }
    void Close() {
        const auto closed = writer->Close();
        const std::string error = closed.has_value() ? std::string() : closed.error();
        REQUIRE_MESSAGE(closed.has_value(), error); writer.reset();
    }
};
mw::MiddlewareDefinition Definition(std::string name, mw::Handler handler, int priority = 0) {
    mw::MiddlewareDefinition result;
    result.point = mw::HookPoint::PostAction; result.name = std::move(name);
    result.layer = mw::SourceLayer::Builtin; result.source_label = "Post return fixture";
    result.implementation_ref = "builtin/post-return/" + result.name;
    result.priority = priority; result.builtin = std::move(handler);
    return result;
}
std::shared_ptr<const mw::FrozenRegistry> Publish(std::vector<mw::MiddlewareDefinition> definitions) {
    mw::MiddlewarePool pool;
    for (auto& definition : definitions) pool.AddDefinition(std::move(definition));
    auto result = pool.Publish();
    const std::string error = result.has_value() ? std::string() : result.error().message;
    REQUIRE_MESSAGE(result.has_value(), error);
    return std::move(*result);
}
mw::HandlerReturn Supplements(std::size_t count, std::size_t bytes = 0) {
    mw::HandlerReturn result;
    for (std::size_t i = 0; i < count; ++i)
        result.effects.push_back({mw::EffectType::ResultSupplement, {{"text", std::string(bytes, 's')}}});
    return result;
}
const mw::InvocationRecord& Record(const mw::DispatchOutcome& outcome, const std::string& name) {
    const auto* record = outcome.FindRecord("PostAction/" + name);
    REQUIRE(record != nullptr); return *record;
}
std::size_t Count(const rt::MiddlewareReceiptSnapshot& snapshot, Stage stage) {
    return static_cast<std::size_t>(std::count_if(snapshot.receipts.begin(), snapshot.receipts.end(),
        [stage](const auto& item) { return item.stage == stage; }));
}
void CheckNative(const rt::MiddlewareReceiptSnapshot& snapshot, const v3::V3Ledger& ledger) {
    REQUIRE(snapshot.complete()); REQUIRE_FALSE(snapshot.receipts.empty());
    std::uint64_t previous = 0;
    for (const auto& item : snapshot.receipts) {
        REQUIRE(item.receipt.has_value()); Committed(*item.receipt);
        CHECK(item.receipt->seq > previous); previous = item.receipt->seq;
        const auto* event = ledger.FindEvent(item.receipt->id); REQUIRE(event != nullptr);
        CHECK(event->seq == item.receipt->seq); CHECK(event->line_hash == item.receipt->line_hash);
        CHECK(event->session_id == snapshot.scope.session_id); CHECK(event->run_id == snapshot.scope.run_id);
        CHECK(event->turn_id == snapshot.scope.turn_id); CHECK(event->step_id == snapshot.scope.step_id);
        CHECK(event->action_id == snapshot.scope.action_id); CHECK(event->hook_dispatch_id == item.dispatch_id);
        CHECK_FALSE(event->payload.contains("returnContract")); CHECK_FALSE(event->payload.contains("failureSource"));
    }
    const auto* requested = ledger.FindEvent(snapshot.receipts.front().receipt->id); REQUIRE(requested != nullptr);
    CHECK(requested->payload.at("inputRef").at("executionEventRef") == snapshot.scope.terminal_ref);
    CHECK(requested->payload.at("inputRef").at("resultEventRef") == snapshot.scope.persisted_ref);
}
Json Normalize(Json value, const std::string& dispatch) {
    if (value.is_object()) {
        value.erase("durationMs"); value.erase("inputRef");
        for (auto& item : value.items()) item.value() = Normalize(std::move(item.value()), dispatch);
    } else if (value.is_array()) {
        for (auto& item : value) item = Normalize(std::move(item), dispatch);
    } else if (value.is_string()) {
        const auto text = value.get<std::string>();
        if (text == dispatch) value = "dispatch";
        else if (text.starts_with(dispatch + "#")) value = "dispatch" + text.substr(dispatch.size());
    }
    return value;
}
std::vector<Json> Payloads(const v3::V3Ledger& ledger, const std::string& dispatch) {
    std::vector<Json> result;
    for (const auto& event : ledger.events) if (event.hook_dispatch_id == dispatch)
        result.push_back(Json{{"kind", v3::ToString(event.kind)}, {"payload", Normalize(event.payload, dispatch)}});
    return result;
}

#ifdef LUBANCORE_TEST_JOB_POST_SDK
namespace sdk = lubancore;
namespace ext = lubancore::extensions::v1;
std::string Utf8(const fs::path& path) {
    const auto bytes = path.u8string(); return {reinterpret_cast<const char*>(bytes.data()), bytes.size()};
}
struct SdkState {
    unsigned models = 0, tools = 0, pre = 0, post = 0;
    std::string session, operation, turn, action;
};
class Backend final : public sdk::Backend {
    std::shared_ptr<SdkState> state_;
public:
    explicit Backend(std::shared_ptr<SdkState> state) : state_(std::move(state)) {}
    sdk::Result<sdk::ModelReply> Generate(const sdk::ModelRequest& request, sdk::Cancellation) override {
        ++state_->models;
        sdk::ModelReply result;
        if (state_->models == 1) result.tool_calls.push_back({"wire-post-contract", "fixture", R"({"value":1})"});
        else {
            bool reply = false;
            for (const auto& message : request.messages) for (const auto& tool : message.tool_replies)
                if (tool.call_id == "wire-post-contract") { reply = true; CHECK_FALSE(tool.is_error); }
            CHECK(reply); result.text = "actual final";
        }
        return result;
    }
};
class Extension final : public ext::Instance {
    std::shared_ptr<SdkState> state_;
    unsigned variant_;
public:
    Extension(std::shared_ptr<SdkState> state, unsigned variant) : state_(std::move(state)), variant_(variant) {}
    sdk::Result<ext::HandlerReturn> Invoke(const ext::Context& context, const ext::Input& input, ext::Next next) override {
        const auto parsed = Json::parse(input.json);
        CHECK(context.session_id == state_->session); CHECK(context.effective_cwd.has_value());
        REQUIRE(context.action_id.has_value()); REQUIRE(context.turn_id.has_value());
        if (context.point == ext::Point::PreAction) {
            ++state_->pre; CHECK_FALSE(context.execution.has_value());
            state_->operation = context.operation_id; state_->turn = *context.turn_id; state_->action = *context.action_id;
            CHECK(parsed.at("arguments").at("value").get<int>() == 1);
            const auto downstream = next.Call(R"({"arguments":{"value":2}})");
            REQUIRE(downstream.has_value()); return ext::HandlerReturn{};
        }
        ++state_->post;
        REQUIRE(context.execution.has_value()); CHECK(context.execution->attempt > 0);
        CHECK(context.execution->session_id == state_->session); CHECK(context.execution->operation_id == state_->operation);
        CHECK(context.execution->turn_id == state_->turn); CHECK(context.execution->action_id == state_->action);
        CHECK(parsed.at("arguments").at("value").get<int>() == 2);
        CHECK(parsed.at("result").at("text").get<std::string>() == "actual tool raw");
        auto bad_next = next.Call(std::string(1024 * 1024 + 1, 'x'));
        REQUIRE_FALSE(bad_next.has_value()); CHECK(bad_next.error().code == mw::err::kNextBadCandidate);
        const auto downstream = next.Call(); REQUIRE(downstream.has_value());
        ext::HandlerReturn result;
        if (variant_ == 0) {
            result.deny_code = std::string(5000, 'd'); // Existing SDK ignores unused denial metadata.
            result.deny_message = std::string(5000, 'd');
            result.effects.push_back({ext::EffectType::ResultSupplement, Json{{"text", "shared valid supplement"}}.dump()});
        } else if (variant_ == 1) {
            result.effects.push_back({ext::EffectType::ResultSupplement, Json{{"text", "invalid"}, {"extra", 1}}.dump()});
        } else if (variant_ == 2) {
            result.effects.push_back({ext::EffectType::ResultSupplement, Json{{"text", std::string("bad\0text", 8)}}.dump()});
        } else {
            result.output_json = "null"; // Present Post output remains forbidden in the SDK.
        }
        return result;
    }
};
void PublicSdk() {
    Directory directory;
    auto runtime = sdk::Runtime::Create({Utf8(directory.path / "data"), Utf8(directory.path)});
    const std::string runtime_error = runtime.has_value() ? std::string() : runtime.error().message;
    REQUIRE_MESSAGE(runtime.has_value(), runtime_error);
    for (unsigned variant = 0; variant < 4; ++variant) {
        auto state = std::make_shared<SdkState>();
        sdk::SessionOptions options;
        options.cwd = Utf8(fs::canonical(directory.path)); options.model = "post-contract-fixture";
        options.system_prompt = "fixture"; options.approval_mode = sdk::ApprovalMode::Yolo;
        options.backend = std::make_unique<Backend>(state);
        sdk::Tool tool;
        tool.name = "fixture"; tool.description = "actual test tool"; tool.requires_approval = false;
        tool.input_schema_json = R"({"type":"object","properties":{"value":{"type":"integer"}},"required":["value"],"additionalProperties":false})";
        tool.execute = [state](const std::string& input, const sdk::ToolContext&) -> sdk::Result<sdk::ToolResult> {
            ++state->tools; CHECK(Json::parse(input).at("value").get<int>() == 2);
            return sdk::ToolResult{"actual tool raw", false};
        };
        options.custom_tools.push_back(std::move(tool));
        ext::Registration registration;
        registration.manifest.id = "post-contract"; registration.manifest.version = "1.0.0";
        for (auto point : {ext::Point::PreAction, ext::Point::PostAction}) {
            ext::HandlerDefinition definition;
            definition.point = point; definition.name = point == ext::Point::PreAction ? "pre" : "post";
            definition.definition_hash = "post-contract-v1"; definition.required = true;
            registration.manifest.handlers.push_back(std::move(definition));
        }
        registration.factory = [state, variant](const ext::SessionContext& context) -> sdk::Result<std::unique_ptr<ext::Instance>> {
            state->session = context.session_id; return std::unique_ptr<ext::Instance>(new Extension(state, variant));
        };
        options.extensions.push_back(std::move(registration));
        auto opened = (*runtime)->OpenSession(std::move(options));
        const std::string open_error = opened.has_value() ? std::string() : opened.error().message;
        REQUIRE_MESSAGE(opened.has_value(), open_error);
        auto session = *opened;
        const auto receipt = session->Submit("post-contract", "actual request"); REQUIRE(receipt.has_value());
        const auto operation = session->WaitResult(receipt->operation_id, 20s); REQUIRE(operation.has_value());
        CHECK(state->tools == 1); CHECK(state->pre == 1); CHECK(state->post == 1);
        CHECK(operation->operation_id == state->operation); CHECK(operation->turn_id == state->turn);
        CHECK(operation->result_persisted);
        if (variant == 0) { CHECK(operation->state == sdk::OperationState::Succeeded); CHECK(state->models == 2); }
        else {
            CHECK(operation->state == sdk::OperationState::Failed); CHECK(state->models == 1);
            CHECK(operation->error.find(std::string(mw::err::kResultInvalid)) != std::string::npos);
        }
        REQUIRE(session->Close().has_value());
    }
    REQUIRE((*runtime)->Shutdown().has_value());
    std::cout << "[middleware-job-post-sdk] pre-action\n[middleware-job-post-sdk] post-action\n" << std::flush;
}
#endif
} // namespace

TEST_CASE("middleware Job Post return contract: valid") {
    Mark("valid");
    unsigned callbacks = 0, terminals = 0;
    auto handler = Definition("all", [&](const mw::InvocationCtx&, const Json&, mw::NextCall& next) {
        ++callbacks; const auto downstream = next(); CHECK(downstream.kind == mw::DownstreamOutcome::Kind::Value);
        auto result = Supplements(16);
        result.effects[0].payload["text"] = std::string(16384, 'a');
        result.effects[1].payload["text"] = std::string(16384, 'b');
        return result;
    });
    mw::MiddlewareDispatcher dispatcher(Publish({handler})); Rig rig;
    auto capture = rig.Capture(dispatcher); Deferred queue(capture.sink.get());
    const auto outcome = dispatcher.Dispatch(mw::HookPoint::PostAction, rig.Trigger(), [&](const Json& actual) {
        ++terminals; CHECK(actual == rig.Trigger().input); return actual;
    }, &queue, JobPost);
    REQUIRE(outcome.kind == Kind::Completed); CHECK(outcome.cause == mw::DispatchCause::None);
    CHECK(callbacks == 1); CHECK(terminals == 1); REQUIRE(queue.effects.size() == 16);
    CHECK(Record(outcome, "all").effects.size() == 16);
    queue.Settle(true, "actual host adoption", nullptr, &outcome); CHECK(queue.effects.empty());
    REQUIRE(capture.lease.Finish(outcome).has_value()); const auto snapshot = capture.lease.Snapshot();
    CHECK(Count(snapshot, Stage::EffectsApplied) == 16); CHECK(Count(snapshot, Stage::EffectsRejected) == 0);
    CheckNative(snapshot, Read(rig.path));
    const auto ledger = Read(rig.path); const auto* raw = ledger.FindEvent(rig.persisted.id);
    REQUIRE(raw != nullptr); CHECK(raw->seq < snapshot.receipts.front().receipt->seq);
    capture.sink.reset(); rig.Close();
    mw::MiddlewareDispatcher empty(Publish({})); Rig skipped; auto no_handlers = skipped.Capture(empty);
    const auto all_zero = empty.Dispatch(mw::HookPoint::PostAction, skipped.Trigger(), {}, no_handlers.sink.get(), JobPost);
    REQUIRE(no_handlers.lease.Finish(all_zero).has_value());
    CHECK(all_zero.records.empty()); CHECK(Count(no_handlers.lease.Snapshot(), Stage::Skipped) == 1);
    CheckNative(no_handlers.lease.Snapshot(), Read(skipped.path)); no_handlers.sink.reset(); skipped.Close();
}

TEST_CASE("middleware Job Post return contract: payload") {
    Mark("payload");
    for (unsigned variant = 0; variant < 11; ++variant) {
        unsigned calls = 0, terminals = 0;
        auto handler = Definition("bad", [&](const mw::InvocationCtx&, const Json&, mw::NextCall&) {
            ++calls; auto result = Supplements(1);
            switch (variant) {
                case 0: result.output = Json{{"huge", std::string(2 * 1024 * 1024 + 1, 'x')}}; break;
                case 1: result.deny = true; break;
                case 2: result.effects[0].type = mw::EffectType::ContextAppend; break;
                case 3: result.effects[0].payload["extra"] = 1; break;
                case 4: result.effects[0].payload["text"] = std::string("bad\0text", 8); break;
                case 5: result.effects[0].payload["text"] = std::string(1, static_cast<char>(0xff)); break;
                case 6: result.effects[0].payload["text"] = std::string(16385, 'x'); break;
                case 7: result = Supplements(17); break;
                case 8: result = Supplements(3, 16384); break;
                case 9: result.deny_code = std::string(257, 'd'); break;
                case 10: result.deny_message = std::string(4097, 'd'); break;
            }
            return result;
        });
        handler.required = true;
        mw::MiddlewareDispatcher dispatcher(Publish({handler})); Rig rig; auto capture = rig.Capture(dispatcher);
        Deferred queue(capture.sink.get());
        const auto outcome = dispatcher.Dispatch(mw::HookPoint::PostAction, rig.Trigger(), [&](const Json& input) {
            ++terminals; return input;
        }, &queue, JobPost);
        CHECK(calls == 1); CHECK(terminals == 0); CHECK(queue.effects.empty());
        CHECK(outcome.kind == Kind::Failed); CHECK(outcome.cause == mw::DispatchCause::RequiredAbort);
        CHECK(outcome.failure_source == Source::ReturnContractRejected);
        CHECK(outcome.error_code == mw::err::kResultInvalid); CHECK(Record(outcome, "bad").effects.empty());
        REQUIRE(capture.lease.Finish(outcome).has_value()); const auto snapshot = capture.lease.Snapshot();
        CHECK(Count(snapshot, Stage::OutputProposed) == 0); CHECK(Count(snapshot, Stage::Completed) == 0);
        CHECK(Count(snapshot, Stage::EffectsApplied) == 0); CHECK(Count(snapshot, Stage::Failed) == 1);
        CheckNative(snapshot, Read(rig.path)); capture.sink.reset(); rig.Close();
    }
    // A real catch retains its actual producer and old diagnostic. Only a
    // normally returned HandlerError is subject to the new value boundary.
    for (unsigned variant = 0; variant < 6; ++variant) {
        const bool observer = variant >= 4;
        const auto diagnostic = variant % 2 == 0 ? std::string(4097, 'e')
            : std::string(1, static_cast<char>(0xff));
        auto definition = Definition("error", [variant, diagnostic](const mw::InvocationCtx&, const Json&, mw::NextCall&)
            -> std::expected<mw::HandlerReturn, mw::HandlerError> {
            if (variant < 2 || variant >= 4) throw std::runtime_error(diagnostic);
            return std::unexpected(mw::HandlerError{"fixture.returned", diagnostic});
        });
        definition.observer = observer; definition.required = !observer;
        mw::MiddlewareDispatcher errors(Publish({definition})); Rig error_rig; auto report = error_rig.Capture(errors);
        Deferred buffer(report.sink.get());
        const auto failed = errors.Dispatch(mw::HookPoint::PostAction, error_rig.Trigger(), {}, &buffer, JobPost);
        CHECK(failed.kind == (observer ? Kind::Completed : Kind::Failed));
        CHECK(buffer.effects.empty());
        CHECK(Record(failed, "error").failure_source == (variant < 2 || variant >= 4
            ? Source::HandlerThrew : Source::ReturnContractRejected));
        REQUIRE(report.lease.Finish(failed).has_value());
        CheckNative(report.lease.Snapshot(), Read(error_rig.path)); report.sink.reset(); error_rig.Close();
    }
    unsigned calls = 0, terminals = 0;
    auto handler = Definition("candidate", [&](const mw::InvocationCtx&, const Json&, mw::NextCall& next) {
        ++calls;
        const auto bad = next(Json{{"arguments", {{"huge", std::string(2 * 1024 * 1024 + 1, 'x')}}}});
        CHECK(bad.kind == mw::DownstreamOutcome::Kind::Invalid); CHECK(bad.code == mw::err::kNextBadCandidate);
        CHECK(bad.failure_source == Source::ReturnContractRejected); CHECK_FALSE(next.consumed());
        const auto valid = next(); CHECK(valid.kind == mw::DownstreamOutcome::Kind::Value);
        return mw::HandlerReturn{};
    });
    mw::MiddlewareDispatcher dispatcher(Publish({handler})); Rig rig; auto capture = rig.Capture(dispatcher);
    Deferred queue(capture.sink.get());
    const auto outcome = dispatcher.Dispatch(mw::HookPoint::PostAction, rig.Trigger(), [&](const Json& input) {
        ++terminals; CHECK(input == rig.Trigger().input); return input;
    }, &queue, JobPost);
    CHECK(outcome.kind == Kind::Completed); CHECK(calls == 1); CHECK(terminals == 1);
    CHECK(Record(outcome, "candidate").next_calls == 2); CHECK(Record(outcome, "candidate").next_consumed);
    CHECK(Record(outcome, "candidate").next_exception_source == Source::None);
    REQUIRE(capture.lease.Finish(outcome).has_value());
    CHECK(Count(capture.lease.Snapshot(), Stage::OutputProposed) == 1);
    CHECK(Count(capture.lease.Snapshot(), Stage::ContinuationConsumed) == 1);
    CheckNative(capture.lease.Snapshot(), Read(rig.path)); capture.sink.reset();
    const auto before = Read(rig.path).lines;
    struct CountingSink : mw::MiddlewareEventSink {
        unsigned calls = 0;
        void OnDispatchRequested(const mw::DispatchMeta&, const std::vector<mw::HandlerSnapshot>&) override { ++calls; }
        void OnSkipped(const mw::DispatchMeta&, std::string_view) override { ++calls; }
    } rejected_sink;
    unsigned rejected_terminals = 0;
    for (unsigned mode = 0; mode < 2; ++mode) {
        const auto rejected = dispatcher.Dispatch(mode == 0 ? mw::HookPoint::PreAction : mw::HookPoint::PostAction,
            rig.Trigger(), [&](const Json& input) { ++rejected_terminals; return input; }, &rejected_sink,
            mode == 0 ? JobPost : static_cast<mw::DispatchReturnContract>(999));
        CHECK(rejected.kind == Kind::Failed); CHECK(rejected.failure_source == Source::ReturnContractRejected);
        CHECK(rejected.dispatch_id.empty()); CHECK(calls == 1); CHECK(rejected_terminals == 0);
        CHECK(rejected_sink.calls == 0); CHECK(Read(rig.path).lines == before);
    }
    rig.Close();
}

TEST_CASE("middleware Job Post return contract: budget") {
    Mark("budget");
    for (unsigned mode = 0; mode < 4; ++mode) {
        unsigned outer_calls = 0, inner_calls = 0, terminals = 0;
        auto outer = Definition("outer", [&](const mw::InvocationCtx&, const Json&, mw::NextCall& next) {
            ++outer_calls; const auto actual = next(); CHECK(actual.kind == mw::DownstreamOutcome::Kind::Value);
            return Supplements(1, mode % 2 == 0 ? 1 : 16384);
        }, 1);
        outer.required = mode < 2; outer.failure_policy = mw::FailurePolicy::KeepOriginal;
        auto inner = Definition("inner", [&](const mw::InvocationCtx&, const Json&, mw::NextCall& next) {
            ++inner_calls; (void)next(); return mode % 2 == 0 ? Supplements(16) : Supplements(2, 16384);
        }, 2);
        mw::MiddlewareDispatcher dispatcher(Publish({outer, inner})); Rig rig; auto capture = rig.Capture(dispatcher);
        Deferred queue(capture.sink.get());
        const auto outcome = dispatcher.Dispatch(mw::HookPoint::PostAction, rig.Trigger(), [&](const Json& input) {
            ++terminals; return input;
        }, &queue, JobPost);
        CHECK(outer_calls == 1); CHECK(inner_calls == 1); CHECK(terminals == 1);
        CHECK(Record(outcome, "outer").outcome == "failed");
        CHECK(Record(outcome, "outer").failure_source == Source::ReturnContractRejected);
        CHECK(Record(outcome, "outer").effects.empty());
        const auto accepted = mode % 2 == 0 ? 16u : 2u; REQUIRE(queue.effects.size() == accepted);
        if (mode < 2) { CHECK(outcome.kind == Kind::Failed); CHECK(outcome.cause == mw::DispatchCause::RequiredAbort); }
        else { CHECK(outcome.kind == Kind::Completed); CHECK(outcome.cause == mw::DispatchCause::None); }
        queue.Settle(mode >= 2, "actual rejected outer callback", nullptr, &outcome);
        REQUIRE(capture.lease.Finish(outcome).has_value()); const auto snapshot = capture.lease.Snapshot();
        CHECK(Count(snapshot, Stage::OutputProposed) == 1); CHECK(Count(snapshot, Stage::Failed) == 1);
        CHECK(Count(snapshot, mode < 2 ? Stage::EffectsRejected : Stage::EffectsApplied) == accepted);
        CheckNative(snapshot, Read(rig.path)); capture.sink.reset(); rig.Close();
    }
}

TEST_CASE("middleware Job Post return contract: observer") {
    Mark("observer");
    for (unsigned mode = 0; mode < 4; ++mode) {
        std::atomic<unsigned> calls{0}; const auto main = std::this_thread::get_id();
        auto chain = Definition("chain", [](const mw::InvocationCtx&, const Json&, mw::NextCall& next) {
            (void)next(); return Supplements(16);
        });
        auto observer = Definition("observer", [&](const mw::InvocationCtx&, const Json&, mw::NextCall& next) {
            ++calls; CHECK(std::this_thread::get_id() != main);
            const auto invalid = next(); CHECK(invalid.kind == mw::DownstreamOutcome::Kind::Invalid);
            CHECK_FALSE(next.consumed());
            if (mode == 0) return mw::HandlerReturn{};
            if (mode == 1) return Supplements(1);
            if (mode == 2) return mw::HandlerReturn::Value(Json{{"text", "not observer output"}});
            return mw::HandlerReturn::Denied("fixture.denied", "not observer denial");
        }, 1); observer.observer = true;
        mw::MiddlewareDispatcher dispatcher(Publish({chain, observer})); Rig rig; auto capture = rig.Capture(dispatcher);
        Deferred queue(capture.sink.get());
        const auto outcome = dispatcher.Dispatch(mw::HookPoint::PostAction, rig.Trigger(), {}, &queue, JobPost);
        CHECK(calls.load() == 1); CHECK(outcome.kind == Kind::Completed); REQUIRE(queue.effects.size() == 16);
        CHECK(Record(outcome, "observer").effects.empty());
        if (mode == 0) CHECK(Record(outcome, "observer").outcome == "completed");
        else { CHECK(Record(outcome, "observer").outcome == "failed");
            CHECK(Record(outcome, "observer").failure_source == Source::ReturnContractRejected); }
        queue.Settle(true, "actual host adoption", nullptr, &outcome);
        REQUIRE(capture.lease.Finish(outcome).has_value()); const auto snapshot = capture.lease.Snapshot();
        CHECK(Count(snapshot, Stage::EffectsApplied) == 16); CHECK(Count(snapshot, Stage::OutputProposed) == 1);
        CheckNative(snapshot, Read(rig.path)); capture.sink.reset(); rig.Close();
    }
    struct Shared {
        std::mutex mutex; std::condition_variable cv; unsigned entered = 0; bool release = false;
        bool Enter() { std::unique_lock lock(mutex); ++entered; cv.notify_all(); return cv.wait_for(lock, 20s, [&] { return release; }); }
        bool Wait() { std::unique_lock lock(mutex); return cv.wait_for(lock, 20s, [&] { return entered == 2; }); }
        void Release() { const std::lock_guard lock(mutex); release = true; cv.notify_all(); }
    };
    auto shared = std::make_shared<Shared>();
    auto chain = Definition("isolated", [shared](const mw::InvocationCtx&, const Json&, mw::NextCall& next) {
        CHECK(shared->Enter()); (void)next(); return Supplements(16, 2048);
    });
    mw::MiddlewareDispatcher dispatcher(Publish({chain}));
    auto run = [&] {
        Rig rig; auto capture = rig.Capture(dispatcher); Deferred queue(capture.sink.get());
        const auto outcome = dispatcher.Dispatch(mw::HookPoint::PostAction, rig.Trigger(), {}, &queue, JobPost);
        CHECK(outcome.kind == Kind::Completed); REQUIRE(queue.effects.size() == 16);
        queue.Settle(true, "isolated host adoption", nullptr, &outcome);
        REQUIRE(capture.lease.Finish(outcome).has_value());
        const auto snapshot = capture.lease.Snapshot(); CheckNative(snapshot, Read(rig.path));
        CHECK(Count(snapshot, Stage::EffectsApplied) == 16); capture.sink.reset(); rig.Close(); return snapshot.scope.session_id;
    };
    std::future<std::string> first, second;
    struct Release { std::shared_ptr<Shared> state; ~Release() { state->Release(); } } release{shared};
    first = std::async(std::launch::async, run); second = std::async(std::launch::async, run);
    REQUIRE(shared->Wait()); shared->Release();
    const auto first_owner = first.get(), second_owner = second.get(); CHECK(first_owner != second_owner);
}

TEST_CASE("middleware Job Post return contract: native-gap") {
    Mark("native-gap");
    for (unsigned mode = 0; mode < 3; ++mode) {
        struct Fault { std::atomic<bool> armed{false}; std::atomic<unsigned> attempts{0}; };
        auto fault = std::make_shared<Fault>(); v3::V3WriterOptions options;
        options.inject_io_failure = [fault, mode]() -> std::optional<std::string> {
            if (!fault->armed.load()) return std::nullopt;
            ++fault->attempts;
            if (mode == 2) throw std::runtime_error("actual Writer injection exception");
            return "actual Writer receipt injection";
        };
        Rig rig(options); unsigned calls = 0;
        auto definition = Definition("fault", [&](const mw::InvocationCtx&, const Json&, mw::NextCall& next) {
            ++calls;
            if (mode == 0) return mw::HandlerReturn::Value(Json{{"bad", 1}});
            (void)next(); return Supplements(1, 8);
        }); definition.required = true;
        mw::MiddlewareDispatcher dispatcher(Publish({definition})); auto capture = rig.Capture(dispatcher);
        Deferred queue(capture.sink.get()); const auto before = Read(rig.path).lines;
        fault->armed = mode == 0;
        const auto outcome = dispatcher.Dispatch(mw::HookPoint::PostAction, rig.Trigger(), {}, &queue, JobPost);
        CHECK(calls == 1);
        if (mode == 0) { CHECK(outcome.kind == Kind::Failed); CHECK(outcome.failure_source == Source::ReturnContractRejected); }
        else {
            REQUIRE(outcome.kind == Kind::Completed); REQUIRE(queue.effects.size() == 1); fault->armed = true;
            if (mode == 2) CHECK_THROWS_AS(queue.Settle(true, "actual host adoption", nullptr, &outcome), std::runtime_error);
            else queue.Settle(true, "actual host adoption", nullptr, &outcome);
        }
        REQUIRE(capture.lease.Finish(outcome).has_value()); const auto snapshot = capture.lease.Snapshot();
        CHECK_FALSE(snapshot.complete()); CHECK(fault->attempts.load() == 1);
        if (mode == 2) {
            CHECK(snapshot.gap == Gap::NativeException); CHECK_FALSE(snapshot.receipts.back().receipt.has_value());
            CHECK(queue.effects.size() == 1);
        } else {
            CHECK(snapshot.gap == Gap::NativeUnconfirmed); REQUIRE(snapshot.receipts.back().receipt.has_value());
            CHECK(snapshot.receipts.back().receipt->status == v3::WriteReceipt::Status::Rejected);
            CHECK(snapshot.receipts.back().receipt->error_code == "v3writer.injected");
            CHECK(snapshot.receipts.back().writer_broken); CHECK(rig.writer->broken());
        }
        CHECK(snapshot.outcome == outcome.kind); CHECK(snapshot.failure_source == outcome.failure_source);
        const auto after = Read(rig.path).lines; CHECK(after >= before);
        capture.lease.Close(); capture.sink.reset();
        CHECK(fault->attempts.load() == 1); CHECK(Read(rig.path).lines == after);
        if (mode == 0) CHECK(after == before);
        // Do not retry Settle, invoke another callback, or manufacture a repair receipt.
        rig.writer.reset();
    }
}

TEST_CASE("middleware Job Post return contract: compatibility") {
    Mark("compatibility");
    auto handler = Definition("legacy", [](const mw::InvocationCtx&, const Json&, mw::NextCall& next) {
        (void)next(); auto result = Supplements(1, 8); result.output = {{"legacy", "accepted"}};
        result.deny_code = std::string(5000, 'd'); return result;
    });
    mw::MiddlewareDispatcher dispatcher(Publish({handler}));
    std::vector<Json> original;
    for (unsigned mode = 0; mode < 3; ++mode) {
        Rig rig; auto capture = rig.Capture(dispatcher); Deferred queue(capture.sink.get());
        auto input = rig.Trigger(); input.input["returnContract"] = "JobPostSupplementsV1";
        const auto outcome = mode == 0
            ? dispatcher.Dispatch(mw::HookPoint::PostAction, input, {}, &queue)
            : dispatcher.Dispatch(mw::HookPoint::PostAction, input, {}, &queue,
                mode == 1 ? mw::DispatchReturnContract::Legacy : JobPost);
        if (mode < 2) { CHECK(outcome.kind == Kind::Completed); CHECK(Record(outcome, "legacy").effects.size() == 1); }
        else { CHECK(outcome.kind == Kind::Failed); CHECK(Record(outcome, "legacy").failure_source == Source::ReturnContractRejected); }
        queue.Settle(mode < 2, "actual host adoption", nullptr, &outcome);
        REQUIRE(capture.lease.Finish(outcome).has_value()); const auto ledger = Read(rig.path);
        CheckNative(capture.lease.Snapshot(), ledger); const auto payloads = Payloads(ledger, outcome.dispatch_id);
        if (mode == 0) original = payloads;
        if (mode == 1) CHECK(payloads == original);
        capture.sink.reset(); rig.Close();
    }
    unsigned terminals = 0;
    auto retry = Definition("retry", [](const mw::InvocationCtx&, const Json&, mw::NextCall& next) {
        try { (void)next(); } catch (const std::runtime_error&) {}
        CHECK_FALSE(next.consumed()); (void)next(); return mw::HandlerReturn{};
    });
    mw::MiddlewareDispatcher retry_dispatcher(Publish({retry})); mw::DispatchTrigger trigger;
    const auto actual = retry_dispatcher.Dispatch(mw::HookPoint::PostAction, trigger, [&](const Json& input) {
        if (++terminals == 1) throw std::runtime_error("actual first terminal failure"); return input;
    }, nullptr, JobPost);
    CHECK(actual.kind == Kind::Completed); CHECK(terminals == 2);
    CHECK(Record(actual, "retry").next_calls == 2); CHECK(Record(actual, "retry").next_consumed);
    CHECK(Record(actual, "retry").failure_source == Source::None);
    CHECK(Record(actual, "retry").next_exception_source == Source::TerminalThrew);
#ifdef LUBANCORE_TEST_JOB_POST_SDK
    PublicSdk();
#endif
}
