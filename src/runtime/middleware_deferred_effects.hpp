#pragma once

#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "hooks/middleware.hpp"

namespace lubancode::runtime::middleware_detail {
namespace mw = lubancode::hooks::middleware;
using Json = nlohmann::json;

// The queue owns values and borrows its downstream sink. The host must keep
// that sink alive through actual Dispatch, observer joins and Settle. Settle
// runs on the host serial lane; it must not race with enqueue callbacks.
// This is an in-memory effect buffer, not durable adoption or Post settlement.
struct DeferredEffects final : mw::MiddlewareEventSink {
    struct Pending { mw::InvocationMeta meta; std::string type; bool allowed; std::string reason; Json value; };
    mw::MiddlewareEventSink* sink;
    std::mutex mutex;
    std::vector<Pending> effects;
    explicit DeferredEffects(mw::MiddlewareEventSink* value) : sink(value) {}
    void OnDispatchRequested(const mw::DispatchMeta& meta, const std::vector<mw::HandlerSnapshot>& handlers) override {
        if (sink) sink->OnDispatchRequested(meta, handlers);
    }
    void OnSkipped(const mw::DispatchMeta& meta, std::string_view reason) override { if (sink) sink->OnSkipped(meta, reason); }
    void OnInvocationStarted(const mw::InvocationMeta& meta) override { if (sink) sink->OnInvocationStarted(meta); }
    void OnInvocationCompleted(const mw::InvocationMeta& meta, std::optional<std::string> decision, std::uint64_t ms) override {
        if (sink) sink->OnInvocationCompleted(meta, std::move(decision), ms);
    }
    void OnInvocationFailed(const mw::InvocationMeta& meta, std::string_view code, std::uint64_t ms) override {
        if (sink) sink->OnInvocationFailed(meta, code, ms);
    }
    void OnInvocationCancelled(const mw::InvocationMeta& meta, std::string_view reason) override {
        if (sink) sink->OnInvocationCancelled(meta, reason);
    }
    void OnContinuationConsumed(const mw::InvocationMeta& meta) override { if (sink) sink->OnContinuationConsumed(meta); }
    void OnOutputProposed(const mw::InvocationMeta& meta, std::string_view phase, const Json& candidate) override {
        if (sink) sink->OnOutputProposed(meta, phase, candidate);
        if (phase != "before_next" && candidate.is_object() && candidate.contains("output") &&
            candidate["output"].is_object() && candidate["output"].size() == 1 && candidate["output"].contains("arguments")) {
            const std::lock_guard lock(mutex);
            effects.push_back({meta, "input.rewrite", true, {}, candidate["output"]});
        }
    }
    // Matrix permission is not a host adoption receipt. Save only owned values,
    // then settle after the actual prepare/append succeeds on the host thread.
    void OnEffectSettled(const mw::InvocationMeta& meta, std::string_view type, bool allowed,
        std::string_view reason, const Json& value) override {
        const std::lock_guard lock(mutex);
        effects.push_back({meta, std::string(type), allowed, std::string(reason), value});
    }
    void Settle(bool accepted, const std::string& reason, const Json* effective = nullptr,
        const mw::DispatchOutcome* outcome = nullptr) {
        if (!sink) return;
        // Dispatch has joined all observers before this method is called.
        for (const auto& effect : effects) {
            bool applied = accepted && effect.allowed;
            if (outcome && outcome->kind == mw::DispatchOutcome::Kind::Denied) {
                // Admission is the only adopted effect of a denied call. A
                // proposed argument rewrite never reached the real tool gate.
                applied = applied && effect.type == "admission.decision" &&
                    effect.value.value("decision", Json()) == "deny";
            }
            if (effect.type == "input.rewrite" && effective && effect.value != *effective) applied = false;
            Json value = effect.value;
            if (effect.type == "result.supplement" && outcome) {
                const auto* record = outcome->FindRecord(effect.meta.hook_id);
                if (record) value["source"] = {{"definitionHash", record->definition_hash},
                    {"implementationRef", record->implementation_ref}, {"sourceLabel", record->source_label},
                    {"dispatchId", effect.meta.dispatch_id}, {"invocationId", effect.meta.invocation_id}};
            }
            sink->OnEffectSettled(effect.meta, effect.type, applied,
                applied ? std::string_view{} : !effect.allowed ? std::string_view(effect.reason) : std::string_view(reason), value);
        }
        effects.clear();
    }
};
} // namespace lubancode::runtime::middleware_detail
