#include <doctest/doctest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <future>
#include <iostream>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "hooks/middleware.hpp"
#include "runtime/middleware_v3_sink.hpp"
#include "trajectory/v3/reader.hpp"
#include "trajectory/v3/result_store.hpp"
#include "trajectory/v3/tool_action.hpp"
#include "trajectory/v3/writer.hpp"

namespace {
namespace fs = std::filesystem;
namespace mw = lubancode::hooks::middleware;
namespace rt = lubancode::runtime;
namespace v3 = lubancode::trajectory::v3;
using Json = nlohmann::json;
using Stage = rt::MiddlewareReceiptStage;
using Gap = rt::MiddlewareReceiptGap;
using namespace std::chrono_literals;

void Mark(const char* path) {
    std::cout << "[middleware-native-receipts-path] " << path << '\n' << std::flush;
}
void Committed(const v3::WriteReceipt& receipt) {
    REQUIRE_MESSAGE(receipt.status == v3::WriteReceipt::Status::Committed, receipt.error_message);
    REQUIRE_FALSE(receipt.id.empty());
    REQUIRE(receipt.seq > 0);
    REQUIRE_FALSE(receipt.line_hash.empty());
}
v3::V3Ledger Read(const fs::path& path) {
    auto read = v3::ReadV3Ledger(path);
    const std::string error = read.has_value() ? std::string() : read.error();
    REQUIRE_MESSAGE(read.has_value(), error);
    return std::move(*read);
}

struct Directory {
    fs::path path;
    explicit Directory(const std::string& tag) {
        static std::atomic<unsigned> next{1};
        for (unsigned retry = 0; retry < 64; ++retry) {
            const auto candidate = fs::temp_directory_path() / ("lubancode-native-receipts-" + tag + "-" +
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
    fs::path path;
    std::optional<v3::V3Writer> writer;
    v3::WriteReceipt started, terminal, persisted;
    std::string turn = "turn-000001", step = "step-000001", action = "action-business-000001";

    explicit Rig(const std::string& tag, v3::V3WriterOptions options = {})
        : directory(tag), path(directory.path / "session.jsonl") {
        static std::atomic<unsigned> next{1};
        auto opened = v3::V3Writer::Start(path, "20261003-180000-MWR" + std::to_string(next.fetch_add(1)),
            "run-000001", "native receipt fixture", Json::object(), std::move(options));
        const auto error = opened.has_value() ? std::string() : opened.error();
        REQUIRE_MESSAGE(opened.has_value(), error);
        writer.emplace(std::move(*opened));
        auto business = v3::ToolActionSession::Admit(*writer, turn, step, action, "queued",
            std::nullopt, "provider-business-1", Json::object(), v3::Durability::PowerLoss);
        v3::ToolIdentity identity;
        identity.logical_name = "run_command";
        identity.registration_source = "builtin";
        identity.version = "v1";
        identity.execution_scope = "local";
        started = business.Start(*writer, "args-business-1", identity, std::nullopt,
            Json::object(), v3::Durability::PowerLoss);
        Committed(started);
        terminal = business.Finish(*writer, 0, 1);
        Committed(terminal);
        auto store = v3::ResultStore::Open(directory.path);
        const auto store_error = store.has_value() ? std::string() : store.error();
        REQUIRE_MESSAGE(store.has_value(), store_error);
        v3::ResultStore::PersistRequest raw;
        raw.result_kind = "text";
        raw.content = "actual fixture raw";
        raw.outputs.push_back({"combined", "text/plain", "actual fixture raw"});
        raw.execution_event_ref = terminal.id;
        raw.tool_call_id = action;
        raw.preview_policy = {{"maxPreviewBytes", 32768}};
        const auto saved = store->Persist(raw);
        REQUIRE_MESSAGE(saved.ok, saved.error);
        persisted = business.PersistedResult(*writer, saved.result_ref, terminal.id, 1);
        Committed(persisted);
    }
    rt::MiddlewareReceiptScope Scope(std::uint64_t revision, std::size_t limit = 256) const {
        const auto ledger = Read(path);
        const auto term = v3::MakeOwnedJobReference(ledger, terminal.id);
        const auto raw = v3::MakeOwnedJobReference(ledger, persisted.id);
        REQUIRE(term.has_value()); REQUIRE(raw.has_value());
        return {writer->session_id(), writer->run_id(), turn, step, action, 1, revision, *term, *raw, limit};
    }
    mw::DispatchTrigger Trigger() const {
        mw::DispatchTrigger trigger;
        trigger.turn_id = turn; trigger.step_id = step; trigger.action_id = action;
        trigger.input = {{"arguments", {{"command", "fixture"}}},
            {"result", {{"text", "actual fixture raw"}, {"isError", false}, {"outcome", "done"}, {"errorCode", ""}}}};
        return trigger;
    }
    void Close() {
        const auto closed = writer->Close();
        const auto error = closed.has_value() ? std::string() : closed.error();
        REQUIRE_MESSAGE(closed.has_value(), error);
    }
};

mw::MiddlewareDefinition Definition(std::string name, mw::Handler handler, int priority = 0) {
    mw::MiddlewareDefinition definition;
    definition.point = mw::HookPoint::PostAction;
    definition.name = std::move(name);
    definition.layer = mw::SourceLayer::Builtin;
    definition.implementation_ref = "builtin/native-receipts/" + definition.name;
    definition.source_label = "native receipt fixture";
    definition.definition_hash = std::string(64, 'a');
    definition.priority = priority;
    definition.builtin = std::move(handler);
    return definition;
}
std::shared_ptr<const mw::FrozenRegistry> Publish(std::vector<mw::MiddlewareDefinition> definitions) {
    mw::MiddlewarePool pool;
    for (auto& definition : definitions) pool.AddDefinition(std::move(definition));
    auto published = pool.Publish();
    const auto error = published.has_value() ? std::string() : published.error().message;
    REQUIRE_MESSAGE(published.has_value(), error);
    return std::move(*published);
}
rt::CapturedMiddlewareSink Capture(Rig& rig, const mw::MiddlewareDispatcher& dispatcher, std::size_t limit = 256) {
    auto created = rt::CaptureMiddlewareReceipts(*rig.writer, rig.Scope(dispatcher.registry().revision(), limit));
    const auto error = created.has_value() ? std::string() : created.error();
    REQUIRE_MESSAGE(created.has_value(), error);
    return std::move(*created);
}
std::size_t Count(const rt::MiddlewareReceiptSnapshot& snapshot, Stage stage) {
    return static_cast<std::size_t>(std::count_if(snapshot.receipts.begin(), snapshot.receipts.end(),
        [&](const auto& item) { return item.stage == stage; }));
}
void CheckNative(const rt::MiddlewareReceiptSnapshot& snapshot, const v3::V3Ledger& ledger) {
    REQUIRE(snapshot.dispatch.has_value());
    REQUIRE_FALSE(snapshot.receipts.empty());
    std::uint64_t previous = 0;
    for (const auto& item : snapshot.receipts) {
        REQUIRE(item.receipt.has_value());
        Committed(*item.receipt);
        REQUIRE(item.receipt->seq > previous); previous = item.receipt->seq;
        CHECK_FALSE(item.writer_broken);
        const auto* actual = ledger.FindEvent(item.receipt->id);
        REQUIRE(actual != nullptr);
        CHECK(actual->session_id == snapshot.scope.session_id);
        CHECK(actual->run_id == snapshot.scope.run_id);
        CHECK(actual->turn_id == snapshot.scope.turn_id);
        CHECK(actual->step_id == snapshot.scope.step_id);
        CHECK(actual->action_id == snapshot.scope.action_id);
        CHECK(actual->hook_dispatch_id == item.dispatch_id);
        CHECK(actual->seq == item.receipt->seq);
        CHECK(actual->line_hash == item.receipt->line_hash);
        using K = v3::EventKindV3;
        const auto kind = [&] {
            switch (item.stage) {
                case Stage::Requested: return K::HookDispatchRequested;
                case Stage::Skipped: return K::HookSkipped;
                case Stage::Started: return K::HookStarted;
                case Stage::Completed: return K::HookCompleted;
                case Stage::Failed: return K::HookFailed;
                case Stage::Cancelled: return K::HookCancelled;
                case Stage::OutputProposed: return K::HookOutputProposed;
                case Stage::ContinuationConsumed: return K::HookContinuationConsumed;
                case Stage::EffectsApplied: return K::HookEffectsApplied;
                case Stage::EffectsRejected: return K::HookEffectsRejected;
            }
            return K::HookUnknown;
        }();
        CHECK(actual->kind == kind);
        if (!item.invocation_id.empty()) CHECK(actual->payload.at("hookInvocationId").get<std::string>() == item.invocation_id);
    }
    const auto* opening = ledger.FindEvent(snapshot.receipts.front().receipt->id);
    REQUIRE(opening != nullptr);
    CHECK(opening->payload.at("inputRef").at("executionEventRef") == snapshot.scope.terminal_ref);
    CHECK(opening->payload.at("inputRef").at("resultEventRef") == snapshot.scope.persisted_ref);
}

// Compare the existing producer's event payloads/ordering. Only actual generated
// dispatch/invocation IDs, measured durations and the opt-in inputRef differ.
Json Normalize(Json value, const std::string& dispatch) {
    if (value.is_object()) {
        value.erase("durationMs"); value.erase("inputRef");
        for (auto& item : value.items()) item.value() = Normalize(std::move(item.value()), dispatch);
    } else if (value.is_array()) {
        for (auto& item : value) item = Normalize(std::move(item), dispatch);
    } else if (value.is_string()) {
        auto text = value.get<std::string>();
        if (text.starts_with(dispatch + "#")) value = "dispatch" + text.substr(dispatch.size());
    }
    return value;
}
std::vector<Json> Payloads(const v3::V3Ledger& ledger, const std::string& dispatch) {
    std::vector<Json> output;
    for (const auto& event : ledger.events) if (event.hook_dispatch_id == dispatch)
        output.push_back({{"kind", v3::EventKindV3Name(event.kind)}, {"status", event.status ? Json(v3::OpStatusName(*event.status)) : Json()},
            {"turn", event.turn_id.value_or("")}, {"step", event.step_id.value_or("")}, {"action", event.action_id.value_or("")},
            {"payload", Normalize(event.payload, dispatch)}});
    return output;
}

TEST_CASE("middleware native receipts: completed") {
    Mark("completed");
    std::atomic<unsigned> outer{0}, inner{0}, observer{0};
    auto a = Definition("outer", [&](const mw::InvocationCtx&, const Json&, mw::NextCall& next) {
        ++outer; return mw::HandlerReturn::Value(next().value);
    }, 1);
    auto b = Definition("inner", [&](const mw::InvocationCtx&, const Json&, mw::NextCall& next) {
        ++inner; return mw::HandlerReturn::Value(next().value);
    }, 2);
    auto o = Definition("observer", [&](const mw::InvocationCtx&, const Json&, mw::NextCall&) {
        ++observer; return mw::HandlerReturn{};
    }, 3);
    o.observer = true;
    const auto registry = Publish({a, b, o});
    mw::MiddlewareDispatcher dispatcher(registry);
    Rig captured("completed");
    auto report = Capture(captured, dispatcher);
    const auto outcome = dispatcher.Dispatch(mw::HookPoint::PostAction, captured.Trigger(),
        [](const Json& value) { return value; }, report.sink.get());
    REQUIRE(report.lease.Finish(outcome).has_value());
    const auto snapshot = report.lease.Snapshot();
    CHECK(snapshot.complete()); CHECK_FALSE(snapshot.closed);
    CHECK(snapshot.outcome == mw::DispatchOutcome::Kind::Completed);
    CHECK(Count(snapshot, Stage::Requested) == 1);
    CHECK(Count(snapshot, Stage::Started) == 3);
    CHECK(Count(snapshot, Stage::Completed) == 3);
    CHECK(Count(snapshot, Stage::ContinuationConsumed) == 2);
    CHECK(outer.load() == 1); CHECK(inner.load() == 1); CHECK(observer.load() == 1);
    const auto ledger = Read(captured.path);
    CheckNative(snapshot, ledger);
    Rig ordinary("ordinary");
    rt::V3MiddlewareEventSink old_sink(*ordinary.writer);
    const auto old_outcome = dispatcher.Dispatch(mw::HookPoint::PostAction, ordinary.Trigger(),
        [](const Json& value) { return value; }, &old_sink);
    CHECK(old_outcome.kind == outcome.kind);
    const auto old_ledger = Read(ordinary.path);
    CHECK(Payloads(old_ledger, old_outcome.dispatch_id) == Payloads(ledger, outcome.dispatch_id));
    for (const auto& event : old_ledger.events) if (event.hook_dispatch_id == old_outcome.dispatch_id)
        CHECK_FALSE(event.payload.contains("inputRef"));
    CHECK(old_sink.recent_errors().empty());
    CHECK(outer.load() == 2); CHECK(inner.load() == 2); CHECK(observer.load() == 2);
    report.sink.reset(); captured.Close(); ordinary.Close();
}

TEST_CASE("middleware native receipts: skipped") {
    Mark("skipped");
    for (const bool unmatched : {false, true}) {
        std::atomic<unsigned> calls{0};
        std::vector<mw::MiddlewareDefinition> definitions;
        if (unmatched) {
            auto definition = Definition("unmatched", [&](const mw::InvocationCtx&, const Json&, mw::NextCall& next) {
                ++calls; return mw::HandlerReturn::Value(next().value);
            });
            definition.match.origin = "other-origin";
            definitions.push_back(std::move(definition));
        }
        mw::MiddlewareDispatcher dispatcher(Publish(std::move(definitions)));
        Rig rig(unmatched ? "unmatched" : "empty");
        auto report = Capture(rig, dispatcher);
        auto trigger = rig.Trigger(); trigger.origin = "this-origin";
        const auto outcome = dispatcher.Dispatch(mw::HookPoint::PostAction, trigger,
            [](const Json& value) { return value; }, report.sink.get());
        REQUIRE(report.lease.Finish(outcome).has_value());
        const auto snapshot = report.lease.Snapshot();
        CHECK(snapshot.complete()); CHECK(calls.load() == 0);
        REQUIRE(snapshot.receipts.size() == 1);
        CHECK(snapshot.receipts.front().stage == Stage::Skipped);
        CHECK(Count(snapshot, Stage::Started) == 0); CHECK(Count(snapshot, Stage::Completed) == 0);
        const auto ledger = Read(rig.path); CheckNative(snapshot, ledger);
        const auto* skipped = ledger.FindEvent(snapshot.receipts.front().receipt->id);
        REQUIRE(skipped != nullptr);
        const std::string expected = unmatched ? "no_matched_handlers" : "no_handlers";
        CHECK(skipped->payload.at("reason").get<std::string>() == expected);
        CHECK_FALSE(report.lease.Finish(outcome).has_value());
        report.sink.reset(); rig.Close();
    }
}

TEST_CASE("middleware native receipts: failure-policy") {
    Mark("failure-policy");
    for (unsigned mode = 0; mode < 3; ++mode) {
        std::atomic<unsigned> failed{0}, downstream{0};
        auto failure = Definition("failure", [&](const mw::InvocationCtx&, const Json&, mw::NextCall&)
            -> std::expected<mw::HandlerReturn, mw::HandlerError> {
            ++failed; return std::unexpected(mw::HandlerError{"fixture.required_failure", "known handler failure"});
        }, 1);
        failure.required = mode == 0;
        failure.failure_policy = mw::FailurePolicy::KeepOriginal;
        failure.observer = mode == 2;
        auto tail = Definition("tail", [&](const mw::InvocationCtx&, const Json&, mw::NextCall& next) {
            ++downstream; return mw::HandlerReturn::Value(next().value);
        }, 2);
        mw::MiddlewareDispatcher dispatcher(Publish({failure, tail}));
        Rig rig("failure-" + std::to_string(mode));
        auto report = Capture(rig, dispatcher);
        const auto outcome = dispatcher.Dispatch(mw::HookPoint::PostAction, rig.Trigger(),
            [](const Json& value) { return value; }, report.sink.get());
        REQUIRE(report.lease.Finish(outcome).has_value());
        const auto snapshot = report.lease.Snapshot();
        CHECK(snapshot.complete()); CHECK(snapshot.gap == Gap::None); CHECK(failed.load() == 1);
        CHECK(Count(snapshot, Stage::Failed) == 1);
        if (mode == 0) { CHECK(outcome.kind == mw::DispatchOutcome::Kind::Failed); CHECK(downstream.load() == 0); }
        else { CHECK(outcome.kind == mw::DispatchOutcome::Kind::Completed); CHECK(downstream.load() == 1); }
        CHECK(snapshot.outcome == outcome.kind);
        const auto ledger = Read(rig.path); CheckNative(snapshot, ledger);
        const auto found = std::find_if(snapshot.receipts.begin(), snapshot.receipts.end(), [](const auto& value) {
            return value.stage == Stage::Failed;
        });
        REQUIRE(found != snapshot.receipts.end());
        CHECK(ledger.FindEvent(found->receipt->id)->payload.at("error_code").get<std::string>() == "fixture.required_failure");
        report.sink.reset(); rig.Close();
    }
}

TEST_CASE("middleware native receipts: effects") {
    Mark("effects");
    std::atomic<unsigned> calls{0};
    auto definition = Definition("effects", [&](const mw::InvocationCtx&, const Json& input, mw::NextCall& next) {
        ++calls;
        auto candidate = input; candidate["arguments"]["command"] = "not adopted";
        mw::HandlerReturn result = mw::HandlerReturn::Value(next(candidate).value);
        result.effects.push_back({mw::EffectType::ResultSupplement, {{"text", "hook supplement fact"}}});
        return result;
    });
    const auto registry = Publish({definition});
    mw::MiddlewareDispatcher dispatcher(registry);
    for (const bool limited : {false, true}) {
        Rig rig(limited ? "overflow" : "effects");
        auto report = Capture(rig, dispatcher, limited ? 2 : 256);
        unsigned terminal_calls = 0;
        const auto outcome = dispatcher.Dispatch(mw::HookPoint::PostAction, rig.Trigger(), [&](const Json& actual) {
            ++terminal_calls; CHECK(actual == rig.Trigger().input); return actual;
        }, report.sink.get());
        REQUIRE(report.lease.Finish(outcome).has_value());
        const auto snapshot = report.lease.Snapshot();
        CHECK(outcome.kind == mw::DispatchOutcome::Kind::Completed); CHECK(terminal_calls == 1);
        if (limited) {
            CHECK_FALSE(snapshot.complete()); CHECK(snapshot.gap == Gap::Overflow);
            REQUIRE(snapshot.receipts.size() == 2);
            CHECK(Count(snapshot, Stage::Started) == 1);
            CHECK(Count(snapshot, Stage::Completed) == 0);
        } else {
            CHECK(snapshot.complete());
            CHECK(Count(snapshot, Stage::EffectsApplied) == 1);
            CHECK(Count(snapshot, Stage::EffectsRejected) == 1);
            CHECK(Count(snapshot, Stage::OutputProposed) == 2);
            CHECK(Count(snapshot, Stage::ContinuationConsumed) == 1);
        }
        const auto ledger = Read(rig.path); CheckNative(snapshot, ledger);
        const auto post_events = Payloads(ledger, outcome.dispatch_id);
        CHECK(post_events.size() == snapshot.receipts.size());
        report.sink.reset(); rig.Close();
    }
    CHECK(calls.load() == 2);
}

TEST_CASE("middleware native receipts: writer-fault") {
    Mark("writer-fault");
    for (unsigned position = 1; position <= 3; ++position) {
        struct Fault { std::atomic<unsigned> left{0}, attempts{0}; };
        auto fault = std::make_shared<Fault>();
        v3::V3WriterOptions options;
        options.inject_io_failure = [fault]() -> std::optional<std::string> {
            if (fault->left == 0) return std::nullopt;
            ++fault->attempts;
            if (fault->left.fetch_sub(1) == 1) return "actual native receipt fault";
            return std::nullopt;
        };
        Rig rig("fault-" + std::to_string(position), options);
        std::atomic<unsigned> calls{0};
        auto definition = Definition("pass", [&](const mw::InvocationCtx&, const Json&, mw::NextCall& next) {
            ++calls; return mw::HandlerReturn::Value(next().value);
        });
        mw::MiddlewareDispatcher dispatcher(Publish({definition}));
        auto report = Capture(rig, dispatcher);
        const auto original_lines = Read(rig.path).lines;
        fault->left = position;
        const auto outcome = dispatcher.Dispatch(mw::HookPoint::PostAction, rig.Trigger(),
            [](const Json& value) { return value; }, report.sink.get());
        REQUIRE(report.lease.Finish(outcome).has_value());
        const auto snapshot = report.lease.Snapshot();
        CHECK_FALSE(snapshot.complete()); CHECK(snapshot.gap == Gap::NativeUnconfirmed);
        REQUIRE(snapshot.receipts.size() == position);
        REQUIRE(snapshot.receipts.back().receipt.has_value());
        const auto& native = *snapshot.receipts.back().receipt;
        CHECK(native.status == v3::WriteReceipt::Status::Rejected);
        CHECK(native.error_code == "v3writer.injected");
        CHECK(snapshot.receipts.back().writer_broken);
        CHECK(fault->attempts.load() == position); CHECK(calls.load() == 1);
        CHECK(outcome.kind == mw::DispatchOutcome::Kind::Completed);
        CHECK(rig.writer->broken());
        const auto ledger = Read(rig.path);
        CHECK(ledger.lines == original_lines + position - 1);
        for (std::size_t i = 0; i + 1 < snapshot.receipts.size(); ++i) {
            Committed(*snapshot.receipts[i].receipt);
            const auto* line = ledger.FindEvent(snapshot.receipts[i].receipt->id);
            REQUIRE(line != nullptr); CHECK(line->line_hash == snapshot.receipts[i].receipt->line_hash);
        }
        const auto attempted = fault->attempts.load();
        mw::DispatchMeta foreign;
        foreign.dispatch_id = "late"; foreign.hook_point = "PostAction";
        foreign.registry_revision = dispatcher.registry().revision();
        foreign.turn_id = rig.turn; foreign.step_id = rig.step; foreign.action_id = rig.action;
        report.sink->OnSkipped(foreign, "no_handlers");
        CHECK(fault->attempts.load() == attempted);
        CHECK(Read(rig.path).lines == ledger.lines);
        CHECK_FALSE(report.lease.Finish(outcome).has_value());
        report.sink.reset();
    }
}

TEST_CASE("middleware native receipts: owner-lifetime") {
    Mark("owner-lifetime");
    mw::MiddlewareDispatcher dispatcher(Publish({}));
    Rig rig("owner");
    const auto baseline = Read(rig.path).lines;
    const auto valid = rig.Scope(dispatcher.registry().revision());
    for (unsigned variant = 0; variant < 8; ++variant) {
        auto scope = valid;
        if (variant == 0) scope.session_id = "foreign";
        if (variant == 1) scope.run_id = "foreign";
        if (variant == 2) scope.turn_id = "foreign";
        if (variant == 3) scope.action_id = "foreign";
        if (variant == 4) scope.terminal_ref["hash"] = std::string(64, '0');
        if (variant == 5) scope.persisted_ref = scope.terminal_ref;
        if (variant == 6) scope.receipt_limit = 0;
        if (variant == 7) scope.receipt_limit = 4097;
        CHECK_FALSE(rt::CaptureMiddlewareReceipts(*rig.writer, std::move(scope)).has_value());
        CHECK(Read(rig.path).lines == baseline);
    }
    auto extra = valid;
    extra.terminal_ref["not_retained"] = std::string(16384, 'x');
    auto normalized = rt::CaptureMiddlewareReceipts(*rig.writer, std::move(extra));
    REQUIRE(normalized.has_value());
    CHECK(normalized->lease.Snapshot().scope.terminal_ref == valid.terminal_ref);
    normalized->sink.reset();
    CHECK(normalized->lease.Snapshot().gap == Gap::Abandoned);
    CHECK(Read(rig.path).lines == baseline);
    auto captured = Capture(rig, dispatcher);
    auto wrong_trigger = rig.Trigger(); wrong_trigger.action_id = "action-foreign";
    const auto foreign = dispatcher.Dispatch(mw::HookPoint::PostAction, wrong_trigger,
        [](const Json& value) { return value; }, captured.sink.get());
    CHECK_FALSE(captured.lease.Finish(foreign).has_value());
    CHECK(captured.lease.Snapshot().gap == Gap::OwnerMismatch);
    CHECK(captured.lease.Snapshot().receipts.empty());
    CHECK(Read(rig.path).lines == baseline);
    captured.sink.reset();
    auto repeated = Capture(rig, dispatcher);
    const auto first = dispatcher.Dispatch(mw::HookPoint::PostAction, rig.Trigger(),
        [](const Json& value) { return value; }, repeated.sink.get());
    const auto first_lines = Read(rig.path).lines;
    const auto second = dispatcher.Dispatch(mw::HookPoint::PostAction, rig.Trigger(),
        [](const Json& value) { return value; }, repeated.sink.get());
    CHECK(repeated.lease.Snapshot().gap == Gap::InvalidSequence);
    CHECK(Read(rig.path).lines == first_lines);
    CHECK_FALSE(repeated.lease.Finish(second).has_value());
    REQUIRE(repeated.lease.Finish(first).has_value());
    CHECK_FALSE(repeated.lease.Snapshot().complete());
    repeated.sink.reset();
    auto stable = Capture(rig, dispatcher);
    const auto outcome = dispatcher.Dispatch(mw::HookPoint::PostAction, rig.Trigger(),
        [](const Json& value) { return value; }, stable.sink.get());
    auto wrong_outcome = outcome; wrong_outcome.dispatch_id = "foreign";
    CHECK_FALSE(stable.lease.Finish(wrong_outcome).has_value());
    REQUIRE(stable.lease.Finish(outcome).has_value());
    const auto saved = stable.lease.Snapshot(); CheckNative(saved, Read(rig.path));
    stable.sink.reset(); rig.Close();
    const auto after = stable.lease.Snapshot();
    CHECK(after.complete()); CHECK(after.closed);
    REQUIRE(after.receipts.size() == saved.receipts.size());
    CHECK(after.receipts.front().receipt->line_hash == saved.receipts.front().receipt->line_hash);
    CHECK_FALSE(rt::CaptureMiddlewareReceipts(*rig.writer, valid).has_value());

    // Close rejects new stages; an already admitted append may finish after it.
    struct Gate {
        std::mutex mutex; std::condition_variable cv;
        bool armed = false, entered = false, release = false;
        void Release() { { std::lock_guard lock(mutex); release = true; } cv.notify_all(); }
        std::optional<std::string> Enter() {
            std::unique_lock lock(mutex);
            if (!armed) return std::nullopt;
            armed = false; entered = true; cv.notify_all();
            if (!cv.wait_for(lock, 15s, [&] { return release; })) return "fixture gate timeout";
            return std::nullopt;
        }
    };
    auto gate = std::make_shared<Gate>();
    v3::V3WriterOptions options;
    options.inject_io_failure = [gate] { return gate->Enter(); };
    Rig closing("close-inflight", options);
    auto live = Capture(closing, dispatcher);
    std::future<mw::DispatchOutcome> future;
    struct Release { std::shared_ptr<Gate> gate; ~Release() { gate->Release(); } } release{gate};
    { std::lock_guard lock(gate->mutex); gate->armed = true; }
    future = std::async(std::launch::async, [&] {
        return dispatcher.Dispatch(mw::HookPoint::PostAction, closing.Trigger(),
            [](const Json& value) { return value; }, live.sink.get());
    });
    {
        std::unique_lock lock(gate->mutex);
        REQUIRE(gate->cv.wait_for(lock, 10s, [&] { return gate->entered; }));
    }
    live.lease.Close();
    CHECK(live.lease.Snapshot().closed);
    gate->Release();
    const auto finished = future.get();
    CHECK(finished.kind == mw::DispatchOutcome::Kind::Completed);
    const auto inflight = live.lease.Snapshot();
    CHECK_FALSE(inflight.complete()); CHECK(inflight.gap == Gap::Abandoned);
    REQUIRE(inflight.receipts.size() == 1); REQUIRE(inflight.receipts.front().receipt.has_value());
    Committed(*inflight.receipts.front().receipt);
    CheckNative(inflight, Read(closing.path));
    live.sink.reset(); closing.Close();
}

} // namespace
