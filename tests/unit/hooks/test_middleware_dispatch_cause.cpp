#include <doctest/doctest.h>

#include <atomic>
#include <chrono>
#include <expected>
#include <filesystem>
#include <iostream>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
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
namespace mw = lubancode::hooks::middleware;
namespace rt = lubancode::runtime;
namespace v3 = lubancode::trajectory::v3;
namespace fs = std::filesystem;
using Json = nlohmann::json;
using Cause = mw::DispatchCause;
using Source = mw::DispatchFailureSource;
using Kind = mw::DispatchOutcome::Kind;

void Mark(const char* stage) {
    std::cout << "[middleware-dispatch-cause-path] " << stage << '\n' << std::flush;
}
mw::MiddlewareDefinition Definition(std::string name, mw::Handler handler, int priority = 0,
                                    mw::HookPoint point = mw::HookPoint::PreAction) {
    mw::MiddlewareDefinition out;
    out.point = point; out.name = std::move(name); out.priority = priority;
    out.layer = mw::SourceLayer::Builtin; out.source_label = "dispatch cause fixture";
    out.implementation_ref = "builtin/dispatch-cause/" + out.name;
    out.builtin = std::move(handler);
    return out;
}
std::shared_ptr<const mw::FrozenRegistry> Publish(std::vector<mw::MiddlewareDefinition> definitions) {
    mw::MiddlewarePool pool;
    for (auto& definition : definitions) pool.AddDefinition(std::move(definition));
    auto published = pool.Publish();
    const std::string error = published.has_value() ? std::string() : published.error().message;
    REQUIRE_MESSAGE(published.has_value(), error);
    return std::move(*published);
}
mw::Handler Pass() {
    return [](const mw::InvocationCtx&, const Json&, mw::NextCall& next) {
        return mw::HandlerReturn::Value(next().value);
    };
}
mw::Handler Error(std::string code = "fixture.failure") {
    return [code = std::move(code)](const mw::InvocationCtx&, const Json&, mw::NextCall&)
               -> std::expected<mw::HandlerReturn, mw::HandlerError> {
        return std::unexpected(mw::HandlerError{code, "actual handler error"});
    };
}
mw::DispatchTrigger Trigger() {
    mw::DispatchTrigger out;
    out.input = {{"value", 7}};
    return out;
}
class RecordingSink : public mw::MiddlewareEventSink {
public:
    void OnDispatchRequested(const mw::DispatchMeta&, const std::vector<mw::HandlerSnapshot>&) override { Add("requested"); }
    void OnSkipped(const mw::DispatchMeta&, std::string_view reason) override { Add("skipped:" + std::string(reason)); }
    void OnInvocationStarted(const mw::InvocationMeta& meta) override { Add("started:" + meta.hook_id); }
    void OnInvocationFailed(const mw::InvocationMeta& meta, std::string_view code, std::uint64_t) override {
        Add("failed:" + meta.hook_id + ":" + std::string(code));
    }
    void OnInvocationCompleted(const mw::InvocationMeta& meta, std::optional<std::string>, std::uint64_t) override {
        Add("completed:" + meta.hook_id);
    }
    void OnContinuationConsumed(const mw::InvocationMeta& meta) override { Add("next:" + meta.hook_id); }
    std::vector<std::string> Events() const { const std::lock_guard lock(mutex_); return events_; }
protected:
    void Add(std::string event) { const std::lock_guard lock(mutex_); events_.push_back(std::move(event)); }
private:
    mutable std::mutex mutex_;
    std::vector<std::string> events_;
};
void CheckRecord(const mw::DispatchOutcome& outcome, const std::string& name, Source source,
                 const std::string& status = "failed", mw::HookPoint point = mw::HookPoint::PreAction) {
    const auto* record = outcome.FindRecord(std::string(mw::ToString(point)) + "/" + name);
    REQUIRE(record != nullptr);
    CHECK(record->outcome == status);
    CHECK(record->failure_source == source);
}
void CheckNextException(const mw::DispatchOutcome& outcome, const std::string& name, Source source,
                        mw::HookPoint point = mw::HookPoint::PreAction) {
    const auto* record = outcome.FindRecord(std::string(mw::ToString(point)) + "/" + name);
    REQUIRE(record != nullptr);
    CHECK(record->next_exception_source == source);
    if (source != Source::None) CHECK(record->next_calls > 0);
}
void Committed(const v3::WriteReceipt& receipt) {
    REQUIRE_MESSAGE(receipt.status == v3::WriteReceipt::Status::Committed, receipt.error_message);
    REQUIRE_FALSE(receipt.id.empty()); REQUIRE(receipt.seq > 0); REQUIRE_FALSE(receipt.line_hash.empty());
}
v3::V3Ledger Read(const fs::path& path) {
    auto ledger = v3::ReadV3Ledger(path);
    const std::string error = ledger.has_value() ? std::string() : ledger.error();
    REQUIRE_MESSAGE(ledger.has_value(), error);
    return std::move(*ledger);
}
struct Directory {
    fs::path path;
    Directory() {
        static std::atomic<unsigned> next{1};
        for (unsigned attempt = 0; attempt < 64; ++attempt) {
            const auto candidate = fs::temp_directory_path() / ("lubancode-dispatch-cause-" +
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
struct NativeRig {
    Directory directory;
    fs::path path = directory.path / "session.jsonl";
    std::optional<v3::V3Writer> writer;
    v3::WriteReceipt terminal, persisted;
    const std::string turn = "turn-000001", step = "step-000001", action = "action-business-000001";
    explicit NativeRig(v3::V3WriterOptions options = {}) {
        static std::atomic<unsigned> next{1};
        auto opened = v3::V3Writer::Start(path, "20261003-180000-MWC" + std::to_string(next.fetch_add(1)),
            "run-000001", "dispatch cause fixture", Json::object(), std::move(options));
        const std::string error = opened.has_value() ? std::string() : opened.error();
        REQUIRE_MESSAGE(opened.has_value(), error);
        writer.emplace(std::move(*opened));
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
    rt::MiddlewareReceiptScope Scope(std::uint64_t revision) const {
        const auto ledger = Read(path);
        const auto term = v3::MakeOwnedJobReference(ledger, terminal.id);
        const auto raw = v3::MakeOwnedJobReference(ledger, persisted.id);
        REQUIRE(term.has_value()); REQUIRE(raw.has_value());
        return {writer->session_id(), writer->run_id(), turn, step, action, 1, revision, *term, *raw, 256};
    }
    mw::DispatchTrigger Input() const {
        auto input = Trigger(); input.turn_id = turn; input.step_id = step; input.action_id = action;
        return input;
    }
    void Close() {
        const auto closed = writer->Close();
        const std::string error = closed.has_value() ? std::string() : closed.error();
        REQUIRE_MESSAGE(closed.has_value(), error);
        writer.reset();
    }
};
Json Normalize(Json value, const std::string& dispatch) {
    if (value.is_object()) {
        value.erase("durationMs"); value.erase("inputRef");
        for (auto& item : value.items()) item.value() = Normalize(std::move(item.value()), dispatch);
    } else if (value.is_array()) {
        for (auto& item : value) item = Normalize(std::move(item), dispatch);
    } else if (value.is_string()) {
        const auto text = value.get<std::string>();
        if (text.starts_with(dispatch + "#")) value = "dispatch" + text.substr(dispatch.size());
    }
    return value;
}
std::vector<Json> Payloads(const v3::V3Ledger& ledger, const std::string& dispatch) {
    std::vector<Json> output;
    for (const auto& event : ledger.events) if (event.hook_dispatch_id == dispatch)
        output.push_back({{"kind", v3::EventKindV3Name(event.kind)},
            {"status", event.status ? Json(v3::OpStatusName(*event.status)) : Json()},
            {"turn", event.turn_id.value_or("")}, {"step", event.step_id.value_or("")}, {"action", event.action_id.value_or("")},
            {"payload", Normalize(event.payload, dispatch)}});
    return output;
}
}  // namespace

TEST_CASE("middleware dispatch cause: policies") {
    Mark("policies");
    for (int variant = 0; variant != 3; ++variant) {
        auto definition = Definition("policy", Error());
        definition.required = variant == 0;
        definition.failure_policy = variant == 1 ? mw::FailurePolicy::Abort : mw::FailurePolicy::KeepOriginal;
        mw::MiddlewareDispatcher dispatcher(Publish({definition}));
        int terminal = 0;
        RecordingSink sink;
        const auto outcome = dispatcher.Dispatch(mw::HookPoint::PreAction, Trigger(), [&](const Json& input) {
            ++terminal; return input;
        }, &sink);
        CHECK(outcome.kind == (variant == 2 ? Kind::Completed : Kind::Failed));
        CHECK(outcome.cause == (variant == 0 ? Cause::RequiredAbort : variant == 1 ? Cause::ConfiguredAbort : Cause::None));
        CHECK(outcome.failure_source == (variant == 2 ? Source::None : Source::HandlerReturnedError));
        CHECK(outcome.error_code == (variant == 2 ? std::string() : std::string("fixture.failure")));
        CHECK(terminal == (variant == 2 ? 1 : 0));
        CHECK(outcome.terminal_runs == terminal);
        CheckRecord(outcome, "policy", Source::HandlerReturnedError);
        CheckNextException(outcome, "policy", Source::None);
        const std::vector<std::string> expected{"requested", "started:PreAction/policy", "failed:PreAction/policy:fixture.failure"};
        CHECK(sink.Events() == expected);
    }
    auto unavailable = Definition("unavailable", Pass());
    unavailable.failure_policy = mw::FailurePolicy::KeepOriginal;
    mw::MiddlewareDispatcher dispatcher(Publish({unavailable}));
    int terminal = 0;
    const auto outcome = dispatcher.Dispatch(mw::HookPoint::PreAction, Trigger(), [&](const Json&) -> Json {
        ++terminal; throw std::runtime_error("actual terminal exception");
    });
    CHECK(outcome.kind == Kind::Failed); CHECK(outcome.cause == Cause::KeepOriginalUnavailable);
    CHECK(outcome.failure_source == Source::HandlerThrew); CHECK(terminal == 1);
    CHECK(outcome.error_code == std::string(mw::err::kHandlerFailed));
    CheckRecord(outcome, "unavailable", Source::HandlerThrew);
    CheckNextException(outcome, "unavailable", Source::TerminalThrew);
}

TEST_CASE("middleware dispatch cause: cancellation") {
    Mark("cancellation");
    for (bool during : {false, true}) {
        std::atomic<bool> cancel{!during}; int handlers = 0, terminal = 0;
        auto definition = Definition("cancel", [&](const mw::InvocationCtx&, const Json&, mw::NextCall& next) {
            ++handlers; cancel.store(true); return mw::HandlerReturn::Value(next().value);
        });
        mw::MiddlewareDispatcher dispatcher(Publish({definition}));
        auto trigger = Trigger(); trigger.cancel = &cancel;
        const auto outcome = dispatcher.Dispatch(mw::HookPoint::PreAction, trigger, [&](const Json& input) { ++terminal; return input; });
        CHECK(outcome.kind == Kind::Failed); CHECK(outcome.cause == Cause::Cancelled);
        CHECK(outcome.failure_source == Source::None); CHECK(handlers == (during ? 1 : 0)); CHECK(terminal == 0);
        CHECK(outcome.error_code == (during ? std::string() : std::string(mw::err::kDispatchCancelled)));
        CheckRecord(outcome, "cancel", Source::None, during ? "completed" : "skipped_cancelled");
        CheckNextException(outcome, "cancel", Source::None);
    }
    for (bool required : {false, true}) {
        auto forged = Definition("forged", Error(std::string(mw::err::kDispatchCancelled)));
        forged.required = required;
        mw::MiddlewareDispatcher dispatcher(Publish({forged}));
        const auto outcome = dispatcher.Dispatch(mw::HookPoint::PreAction, Trigger());
        CHECK(outcome.kind == Kind::Failed);
        CHECK(outcome.cause == (required ? Cause::RequiredAbort : Cause::ConfiguredAbort));
        CHECK(outcome.failure_source == Source::HandlerReturnedError);
        CHECK(outcome.error_code == std::string(mw::err::kDispatchCancelled));
        CheckRecord(outcome, "forged", Source::HandlerReturnedError);
        CheckNextException(outcome, "forged", Source::None);
    }
    std::atomic<bool> late{false};
    mw::MiddlewareDispatcher dispatcher(Publish({Definition("pass", Pass())}));
    auto trigger = Trigger(); trigger.cancel = &late;
    const auto outcome = dispatcher.Dispatch(mw::HookPoint::PreAction, trigger, [&](const Json& input) { late.store(true); return input; });
    CHECK(late.load()); CHECK(outcome.kind == Kind::Completed); CHECK(outcome.cause == Cause::None);
    CHECK(outcome.failure_source == Source::None); CHECK(outcome.terminal_runs == 1);
}

TEST_CASE("middleware dispatch cause: denied") {
    Mark("denied");
    for (bool after : {false, true}) {
        auto definition = Definition("deny", [after](const mw::InvocationCtx&, const Json&, mw::NextCall& next) {
            if (after) next();
            return mw::HandlerReturn::Denied("fixture.denied", "explicit denial");
        });
        mw::MiddlewareDispatcher dispatcher(Publish({definition}));
        int terminal = 0;
        const auto outcome = dispatcher.Dispatch(mw::HookPoint::PreAction, Trigger(), [&](const Json& input) { ++terminal; return input; });
        CHECK(outcome.kind == Kind::Denied); CHECK(outcome.cause == Cause::ExplicitDenied);
        CHECK(outcome.failure_source == Source::None); CHECK(outcome.deny_code == "fixture.denied");
        CHECK(outcome.error_code.empty()); CHECK(terminal == (after ? 1 : 0));
        CheckRecord(outcome, "deny", Source::None, after ? "completed" : "denied");
    }
    auto inner = Definition("inner", [](const mw::InvocationCtx&, const Json&, mw::NextCall&) {
        return mw::HandlerReturn::Denied("fixture.inner-denied", "denied downstream");
    }, 2);
    mw::MiddlewareDispatcher pass(Publish({Definition("outer", Pass(), 1), inner}));
    const auto propagated = pass.Dispatch(mw::HookPoint::PreAction, Trigger());
    CHECK(propagated.kind == Kind::Denied); CHECK(propagated.cause == Cause::ExplicitDenied);
    CHECK(propagated.deny_code.empty()); CHECK(propagated.terminal_runs == 0);
    auto outer_deny = Definition("outer", [](const mw::InvocationCtx&, const Json&, mw::NextCall& next) {
        next(); return mw::HandlerReturn::Denied("fixture.outer-denied", "old outer reduce");
    }, 1);
    auto inner_fail = Definition("inner", Error(), 2); inner_fail.required = true;
    mw::MiddlewareDispatcher precedence(Publish({outer_deny, inner_fail}));
    const auto old_reduce = precedence.Dispatch(mw::HookPoint::PreAction, Trigger());
    CHECK(old_reduce.kind == Kind::Denied); CHECK(old_reduce.cause == Cause::ExplicitDenied);
    CHECK(old_reduce.failure_source == Source::None); CHECK(old_reduce.deny_code == "fixture.outer-denied");
    CheckRecord(old_reduce, "inner", Source::HandlerReturnedError);
}

TEST_CASE("middleware dispatch cause: propagation") {
    Mark("propagation");
    for (bool outer_failure : {false, true}) {
        auto outer = Definition("outer", [outer_failure](const mw::InvocationCtx&, const Json&, mw::NextCall& next)
                -> std::expected<mw::HandlerReturn, mw::HandlerError> {
            auto result = next();
            // Changing this copy cannot change the framework's owned next.last.
            result.cause = Cause::Cancelled; result.failure_source = Source::TerminalThrew;
            if (outer_failure) return std::unexpected(mw::HandlerError{"fixture.outer", "optional failed after next"});
            return mw::HandlerReturn::Value(result.value);
        }, 1);
        outer.failure_policy = mw::FailurePolicy::KeepOriginal;
        auto inner = Definition("inner", Error("fixture.inner"), 2); inner.required = true;
        mw::MiddlewareDispatcher dispatcher(Publish({outer, inner}));
        const auto outcome = dispatcher.Dispatch(mw::HookPoint::PreAction, Trigger());
        CHECK(outcome.kind == Kind::Failed); CHECK(outcome.cause == Cause::RequiredAbort);
        CHECK(outcome.failure_source == Source::HandlerReturnedError); CHECK(outcome.terminal_runs == 0);
        CHECK(outcome.error_code == (outer_failure ? std::string("fixture.inner") : std::string()));
        CheckRecord(outcome, "inner", Source::HandlerReturnedError);
        CheckRecord(outcome, "outer", outer_failure ? Source::HandlerReturnedError : Source::None, outer_failure ? "failed" : "completed");
    }
    int terminal = 0;
    auto optional = Definition("optional", [](const mw::InvocationCtx&, const Json&, mw::NextCall& next)
            -> std::expected<mw::HandlerReturn, mw::HandlerError> {
        next(); return std::unexpected(mw::HandlerError{"fixture.optional", "ignored after next"});
    });
    optional.failure_policy = mw::FailurePolicy::KeepOriginal;
    mw::MiddlewareDispatcher keep(Publish({optional}));
    const auto outcome = keep.Dispatch(mw::HookPoint::PreAction, Trigger(), [&](const Json& input) { ++terminal; return input; });
    CHECK(outcome.kind == Kind::Completed); CHECK(outcome.cause == Cause::None); CHECK(outcome.failure_source == Source::None);
    CHECK(terminal == 1); CheckRecord(outcome, "optional", Source::HandlerReturnedError);

}

TEST_CASE("middleware dispatch cause: exceptions") {
    Mark("exceptions");
    auto thrown = Definition("throw", [](const mw::InvocationCtx&, const Json&, mw::NextCall&) -> mw::HandlerReturn {
        throw std::runtime_error(std::string(mw::err::kDispatchCancelled));
    });
    thrown.required = true;
    mw::MiddlewareDispatcher handler(Publish({thrown}));
    const auto failed = handler.Dispatch(mw::HookPoint::PreAction, Trigger());
    CHECK(failed.kind == Kind::Failed); CHECK(failed.cause == Cause::RequiredAbort);
    CHECK(failed.failure_source == Source::HandlerThrew); CHECK(failed.error_code == std::string(mw::err::kHandlerFailed));
    CheckRecord(failed, "throw", Source::HandlerThrew);
    auto wrap = Definition("wrap", Pass()); wrap.required = true;
    mw::MiddlewareDispatcher terminal(Publish({wrap}));
    const auto terminal_failed = terminal.Dispatch(mw::HookPoint::PreAction, Trigger(), [](const Json&) -> Json {
        throw std::runtime_error("real terminal throw");
    });
    CHECK(terminal_failed.kind == Kind::Failed); CHECK(terminal_failed.cause == Cause::RequiredAbort);
    CHECK(terminal_failed.failure_source == Source::HandlerThrew); CHECK(terminal_failed.terminal_runs == 1);
    CheckRecord(terminal_failed, "wrap", Source::HandlerThrew);
    CheckNextException(terminal_failed, "wrap", Source::TerminalThrew);
    struct ContinuationFault final : RecordingSink {
        void OnContinuationConsumed(const mw::InvocationMeta&) override { throw std::runtime_error("real sink continuation throw"); }
    } sink;
    const auto continuation_failed = terminal.Dispatch(mw::HookPoint::PreAction, Trigger(), nullptr, &sink);
    CHECK(continuation_failed.kind == Kind::Failed); CHECK(continuation_failed.failure_source == Source::HandlerThrew);
    CHECK(continuation_failed.terminal_runs == 0); CHECK(continuation_failed.error_code == std::string(mw::err::kHandlerFailed));
    CheckRecord(continuation_failed, "wrap", Source::HandlerThrew);
    CheckNextException(continuation_failed, "wrap", Source::ContinuationThrew);
    for (bool rethrow : {false, true}) {
        auto recovered = Definition("caught", [rethrow](const mw::InvocationCtx&, const Json&, mw::NextCall& next)
                -> std::expected<mw::HandlerReturn, mw::HandlerError> {
            try { next(); } catch (...) {
                if (rethrow) throw std::runtime_error("different handler exception");
                return std::unexpected(mw::HandlerError{std::string(mw::err::kDispatchCancelled), "handler's own returned error"});
            }
            return mw::HandlerReturn{};
        });
        recovered.required = true;
        mw::MiddlewareDispatcher catcher(Publish({recovered}));
        const auto actual = catcher.Dispatch(mw::HookPoint::PreAction, Trigger(), [](const Json&) -> Json { throw std::runtime_error("original terminal throw"); });
        CHECK(actual.cause == Cause::RequiredAbort);
        CHECK(actual.failure_source == (rethrow ? Source::HandlerThrew : Source::HandlerReturnedError));
        CheckRecord(actual, "caught", rethrow ? Source::HandlerThrew : Source::HandlerReturnedError);
        CheckNextException(actual, "caught", Source::TerminalThrew);
    }
    // The observed downstream throw is not an opaque handler exception ID.
    for (bool downstream : {false, true}) {
        auto nonstd = Definition("nonstd", [downstream](const mw::InvocationCtx&, const Json&, mw::NextCall& next)
                -> mw::HandlerReturn {
            if (downstream) return mw::HandlerReturn::Value(next().value);
            throw 17;
        });
        nonstd.required = true;
        mw::MiddlewareDispatcher dispatcher(Publish({nonstd}));
        const auto actual = dispatcher.Dispatch(mw::HookPoint::PreAction, Trigger(), [](const Json&) -> Json { throw 17; });
        CHECK(actual.kind == Kind::Failed); CHECK(actual.cause == Cause::RequiredAbort);
        CHECK(actual.failure_source == Source::HandlerThrew);
        CHECK(actual.error_code == std::string(mw::err::kHandlerFailed));
        CHECK(actual.error_detail == "PreAction/nonstd: handler 抛未知异常"); CHECK(actual.terminal_runs == (downstream ? 1 : 0));
        CheckRecord(actual, "nonstd", Source::HandlerThrew);
        CheckNextException(actual, "nonstd", downstream ? Source::TerminalThrew : Source::None);
    }
    for (int variant = 0; variant != 3; ++variant) {
        int caught = 0;
        auto replace = Definition("replace", [variant, &caught](const mw::InvocationCtx&, const Json&, mw::NextCall& next)
                -> mw::HandlerReturn {
            try { next(); } catch (const std::runtime_error& error) {
                ++caught; CHECK(std::string(error.what()) == "identical exception");
                if (variant == 0) throw;
                if (variant == 1) throw std::runtime_error("identical exception");
            }
            // The original catch has exited. A fresh throw still belongs here.
            throw std::runtime_error("identical exception");
        });
        replace.required = true;
        mw::MiddlewareDispatcher dispatcher(Publish({replace}));
        const auto actual = dispatcher.Dispatch(mw::HookPoint::PreAction, Trigger(), [](const Json&) -> Json {
            throw std::runtime_error("identical exception");
        });
        CHECK(caught == 1); CHECK(actual.terminal_runs == 1);
        CHECK(actual.kind == Kind::Failed); CHECK(actual.cause == Cause::RequiredAbort);
        CHECK(actual.failure_source == Source::HandlerThrew);
        CHECK(actual.error_code == std::string(mw::err::kHandlerFailed));
        CHECK(actual.error_detail == "PreAction/replace: handler 抛异常: identical exception");
        CheckRecord(actual, "replace", Source::HandlerThrew);
        CheckNextException(actual, "replace", Source::TerminalThrew);
    }
    auto swallowed = Definition("swallowed", [](const mw::InvocationCtx&, const Json& input, mw::NextCall& next) {
        try { next(); } catch (...) { return mw::HandlerReturn::Value(input); }
        return mw::HandlerReturn::Value(input);
    });
    swallowed.required = true;
    mw::MiddlewareDispatcher recovered(Publish({swallowed}));
    const auto succeeded = recovered.Dispatch(mw::HookPoint::PreAction, Trigger(), [](const Json&) -> Json { throw 29; });
    CHECK(succeeded.kind == Kind::Completed); CHECK(succeeded.cause == Cause::None);
    CHECK(succeeded.failure_source == Source::None); CHECK(succeeded.error_code.empty()); CHECK(succeeded.terminal_runs == 1);
    const bool kept_input = succeeded.value == Trigger().input;
    CHECK(kept_input);
    CheckRecord(succeeded, "swallowed", Source::None, "completed_short_circuit");
    CheckNextException(succeeded, "swallowed", Source::TerminalThrew);

    // Retrying a native Next after a throw is old behavior. Each real attempt
    // needs its own terminal slot; later success retains the last throw fact.
    struct RetrySink final : RecordingSink {
        int calls = 0; bool fail_second = false;
        void OnContinuationConsumed(const mw::InvocationMeta& meta) override {
            ++calls;
            if (fail_second && calls == 2) throw std::runtime_error("second continuation throw");
            RecordingSink::OnContinuationConsumed(meta);
        }
    };
    for (bool fail_second : {false, true}) {
        RetrySink retry_sink; retry_sink.fail_second = fail_second;
        int terminals = 0;
        auto retry = Definition("retry", [](const mw::InvocationCtx&, const Json&, mw::NextCall& next) {
            try { next(); } catch (...) {}
            return mw::HandlerReturn::Value(next().value);
        });
        retry.required = true;
        mw::MiddlewareDispatcher dispatcher(Publish({retry}));
        const auto actual = dispatcher.Dispatch(mw::HookPoint::PreAction, Trigger(), [&](const Json& input) -> Json {
            if (++terminals == 1) throw std::runtime_error("first terminal throw");
            return input;
        }, &retry_sink);
        CHECK(retry_sink.calls == 2); CHECK(actual.terminal_runs == (fail_second ? 1 : 2));
        CHECK(terminals == actual.terminal_runs);
        CHECK(actual.kind == (fail_second ? Kind::Failed : Kind::Completed));
        CHECK(actual.cause == (fail_second ? Cause::RequiredAbort : Cause::None));
        CHECK(actual.failure_source == (fail_second ? Source::HandlerThrew : Source::None));
        CHECK(actual.error_code == (fail_second ? std::string(mw::err::kHandlerFailed) : std::string()));
        CheckRecord(actual, "retry", fail_second ? Source::HandlerThrew : Source::None,
                    fail_second ? "failed" : "completed");
        CheckNextException(actual, "retry", fail_second ? Source::ContinuationThrew : Source::TerminalThrew);
        const auto* record = actual.FindRecord("PreAction/retry"); REQUIRE(record != nullptr);
        CHECK(record->next_calls == 2); CHECK(record->next_consumed == !fail_second);
    }
    auto inner = Definition("inner", Pass(), 2); inner.required = true;
    mw::MiddlewareDispatcher frames(Publish({Definition("outer", Pass(), 1), inner}));
    const auto inner_failed = frames.Dispatch(mw::HookPoint::PreAction, Trigger(), [](const Json&) -> Json { throw 31; });
    CHECK(inner_failed.kind == Kind::Failed); CHECK(inner_failed.cause == Cause::RequiredAbort);
    CHECK(inner_failed.failure_source == Source::HandlerThrew); CHECK(inner_failed.terminal_runs == 1);
    CheckRecord(inner_failed, "inner", Source::HandlerThrew);
    CheckNextException(inner_failed, "inner", Source::TerminalThrew);
    CheckRecord(inner_failed, "outer", Source::None, "completed");
    CheckNextException(inner_failed, "outer", Source::None);
    const auto fresh = frames.Dispatch(mw::HookPoint::PreAction, Trigger());
    CHECK(fresh.kind == Kind::Completed); CHECK(fresh.cause == Cause::None); CHECK(fresh.failure_source == Source::None);
    CheckNextException(fresh, "inner", Source::None); CheckNextException(fresh, "outer", Source::None);
    mw::DispatchOutcome owned;
    {
        auto definition = Definition("owned", Pass()); definition.required = true;
        mw::MiddlewareDispatcher scoped(Publish({definition}));
        owned = scoped.Dispatch(mw::HookPoint::PreAction, Trigger(), [](const Json&) -> Json { throw 43; });
    }
    CHECK(owned.kind == Kind::Failed); CHECK(owned.cause == Cause::RequiredAbort);
    CHECK(owned.failure_source == Source::HandlerThrew); CHECK(owned.terminal_runs == 1);
    CheckRecord(owned, "owned", Source::HandlerThrew); CheckNextException(owned, "owned", Source::TerminalThrew);
    mw::MiddlewareDispatcher empty(Publish({}));
    CHECK_THROWS_AS(empty.Dispatch(mw::HookPoint::PreAction, Trigger(), [](const Json&) -> Json { throw std::runtime_error("unchanged empty terminal throw"); }), std::runtime_error);
    for (bool throwing : {false, true}) {
        auto observer = Definition("observer", [throwing](const mw::InvocationCtx&, const Json&, mw::NextCall&)
                -> std::expected<mw::HandlerReturn, mw::HandlerError> {
            if (throwing) throw std::runtime_error("actual observer throw");
            return std::unexpected(mw::HandlerError{std::string(mw::err::kDispatchCancelled), "observer returned error"});
        });
        observer.observer = true;
        mw::MiddlewareDispatcher observed(Publish({observer}));
        const auto actual = observed.Dispatch(mw::HookPoint::PreAction, Trigger());
        CHECK(actual.kind == Kind::Completed); CHECK(actual.cause == Cause::None); CHECK(actual.failure_source == Source::None);
        CHECK(actual.terminal_runs == 1);
        CheckRecord(actual, "observer", throwing ? Source::HandlerThrew : Source::HandlerReturnedError);
        CheckNextException(actual, "observer", Source::None);
    }
    struct CompletionFault final : RecordingSink {
        void OnInvocationCompleted(const mw::InvocationMeta&, std::optional<std::string>, std::uint64_t) override {
            throw std::runtime_error("actual observer completion callback throw");
        }
    } completion;
    auto observer = Definition("observer", [](const mw::InvocationCtx&, const Json&, mw::NextCall&) { return mw::HandlerReturn{}; });
    observer.observer = true;
    mw::MiddlewareDispatcher completion_failed(Publish({observer}));
    const auto observed = completion_failed.Dispatch(mw::HookPoint::PreAction, Trigger(), nullptr, &completion);
    CHECK(observed.kind == Kind::Completed); CHECK(observed.cause == Cause::None);
    CheckRecord(observed, "observer", Source::ObserverCompletionThrew);
    CheckNextException(observed, "observer", Source::None);

    // A real materialization factory can return an empty handler. Do not edit
    // the frozen registry or forge a result to reach this producer branch.
    mw::MiddlewarePool::Options options;
    options.lua_factory = [](const mw::LuaHandlerSpec&, const mw::HandlerLimits&)
            -> std::expected<mw::Handler, std::string> { return mw::Handler{}; };
    mw::MiddlewarePool pool(std::move(options));
    auto absent = Definition("absent", {}); absent.is_lua = true; absent.required = true;
    pool.AddDefinition(std::move(absent));
    auto registry = pool.Publish();
    const std::string registry_error = registry.has_value() ? std::string() : registry.error().message;
    REQUIRE_MESSAGE(registry.has_value(), registry_error);
    mw::MiddlewareDispatcher missing(*registry);
    const auto no_handler = missing.Dispatch(mw::HookPoint::PreAction, Trigger());
    CHECK(no_handler.kind == Kind::Failed); CHECK(no_handler.cause == Cause::RequiredAbort);
    CHECK(no_handler.failure_source == Source::MissingHandler); CHECK(no_handler.terminal_runs == 0);
    CheckRecord(no_handler, "absent", Source::MissingHandler);
    CheckNextException(no_handler, "absent", Source::None);
}

TEST_CASE("middleware dispatch cause: snapshot") {
    Mark("snapshot");
    auto definition = Definition("required", Error(std::string(mw::err::kDispatchCancelled)), 0, mw::HookPoint::PostAction);
    definition.required = true;
    mw::MiddlewareDispatcher dispatcher(Publish({definition}));
    NativeRig captured;
    auto report = rt::CaptureMiddlewareReceipts(*captured.writer, captured.Scope(dispatcher.registry().revision()));
    const std::string error = report.has_value() ? std::string() : report.error();
    REQUIRE_MESSAGE(report.has_value(), error);
    CHECK_FALSE(report->lease.Snapshot().cause.has_value());
    const auto actual = dispatcher.Dispatch(mw::HookPoint::PostAction, captured.Input(), nullptr, report->sink.get());
    CHECK(actual.cause == Cause::RequiredAbort); CHECK(actual.failure_source == Source::HandlerReturnedError);
    const auto owned_actual = actual;
    CheckNextException(owned_actual, "required", Source::None, mw::HookPoint::PostAction);
    REQUIRE(report->lease.Finish(actual).has_value());
    const auto snapshot = report->lease.Snapshot();
    REQUIRE(snapshot.complete()); CHECK(snapshot.cause == Cause::RequiredAbort);
    CHECK(snapshot.failure_source == Source::HandlerReturnedError); CHECK(snapshot.outcome == Kind::Failed);
    const auto ledger = Read(captured.path);
    REQUIRE(snapshot.receipts.size() == 3);
    std::uint64_t previous = 0;
    for (const auto& native : snapshot.receipts) {
        REQUIRE(native.receipt.has_value()); Committed(*native.receipt);
        REQUIRE(native.receipt->seq > previous); previous = native.receipt->seq;
        const auto* event = ledger.FindEvent(native.receipt->id); REQUIRE(event != nullptr);
        CHECK(event->seq == native.receipt->seq); CHECK(event->line_hash == native.receipt->line_hash);
        CHECK(event->session_id == snapshot.scope.session_id); CHECK(event->run_id == snapshot.scope.run_id);
        CHECK(event->turn_id == snapshot.scope.turn_id); CHECK(event->step_id == snapshot.scope.step_id); CHECK(event->action_id == snapshot.scope.action_id);
        CHECK_FALSE(event->payload.contains("cause")); CHECK_FALSE(event->payload.contains("failureSource"));
        CHECK_FALSE(event->payload.contains("nextExceptionSource"));
        CHECK_FALSE(event->payload.contains("next_exception_source"));
    }
    NativeRig ordinary;
    {
        rt::V3MiddlewareEventSink sink(*ordinary.writer);
        const auto prior = dispatcher.Dispatch(mw::HookPoint::PostAction, ordinary.Input(), nullptr, &sink);
        CHECK(prior.kind == actual.kind); CHECK(prior.error_code == actual.error_code);
        CHECK(prior.cause == actual.cause); CHECK(prior.failure_source == actual.failure_source);
        CHECK(Payloads(Read(ordinary.path), prior.dispatch_id) == Payloads(ledger, actual.dispatch_id));
        CHECK(sink.recent_errors().empty());
    }
    ordinary.Close();
    report->sink.reset(); captured.Close(); report->lease.Close();
    const auto retired = report->lease.Snapshot();
    CHECK(retired.complete()); CHECK(retired.closed); CHECK(retired.cause == Cause::RequiredAbort);
    CHECK(retired.failure_source == Source::HandlerReturnedError);
    CHECK(retired.receipts.size() == snapshot.receipts.size());
    // The lease owns final cause/source and native receipts, not invocation
    // records. Those co-observations remain in the separately owned outcome.
    CheckRecord(owned_actual, "required", Source::HandlerReturnedError, "failed", mw::HookPoint::PostAction);
    CheckNextException(owned_actual, "required", Source::None, mw::HookPoint::PostAction);

    // Native confirmation has a separate truth value from the handler result.
    auto armed = std::make_shared<std::atomic<bool>>(false);
    v3::V3WriterOptions options;
    options.inject_io_failure = [armed]() -> std::optional<std::string> {
        if (armed->load()) return "actual Writer failure injection";
        return std::nullopt;
    };
    NativeRig faulty(std::move(options));
    auto fault_report = rt::CaptureMiddlewareReceipts(*faulty.writer, faulty.Scope(dispatcher.registry().revision()));
    const std::string fault_error = fault_report.has_value() ? std::string() : fault_report.error();
    REQUIRE_MESSAGE(fault_report.has_value(), fault_error);
    armed->store(true);
    const auto with_gap = dispatcher.Dispatch(mw::HookPoint::PostAction, faulty.Input(), nullptr, fault_report->sink.get());
    REQUIRE(fault_report->lease.Finish(with_gap).has_value());
    const auto unknown = fault_report->lease.Snapshot();
    CHECK_FALSE(unknown.complete()); CHECK(unknown.gap == rt::MiddlewareReceiptGap::NativeUnconfirmed);
    CHECK(unknown.cause == Cause::RequiredAbort); CHECK(unknown.failure_source == Source::HandlerReturnedError);
    REQUIRE(unknown.receipts.size() == 1); REQUIRE(unknown.receipts.front().receipt.has_value());
    CHECK(unknown.receipts.front().receipt->status == v3::WriteReceipt::Status::Rejected);
    CHECK(unknown.receipts.front().receipt->error_code == "v3writer.injected");
    CHECK(unknown.receipts.front().writer_broken);
    fault_report->sink.reset(); fault_report->lease.Close(); faulty.writer.reset();

    for (bool cancelled : {false, true}) {
        NativeRig skipped;
        mw::MiddlewareDispatcher empty(Publish({}));
        auto empty_report = rt::CaptureMiddlewareReceipts(*skipped.writer, skipped.Scope(empty.registry().revision()));
        const std::string empty_error = empty_report.has_value() ? std::string() : empty_report.error();
        REQUIRE_MESSAGE(empty_report.has_value(), empty_error);
        std::atomic<bool> cancel{cancelled}; auto input = skipped.Input(); input.cancel = &cancel;
        const auto actual_skip = empty.Dispatch(mw::HookPoint::PostAction, input, nullptr, empty_report->sink.get());
        REQUIRE(empty_report->lease.Finish(actual_skip).has_value());
        const auto snapshot_skip = empty_report->lease.Snapshot();
        REQUIRE(snapshot_skip.complete()); CHECK(snapshot_skip.cause == (cancelled ? Cause::Cancelled : Cause::None));
        CHECK(snapshot_skip.failure_source == Source::None);
        REQUIRE(snapshot_skip.receipts.size() == 1);
        CHECK(snapshot_skip.receipts.front().stage == rt::MiddlewareReceiptStage::Skipped);
        CHECK(actual_skip.terminal_runs == (cancelled ? 0 : 1));
        empty_report->sink.reset(); skipped.Close();
    }
}
