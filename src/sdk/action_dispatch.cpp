#include "sdk/action_dispatch.hpp"

#include <mutex>

namespace lubancore::detail {
namespace {
namespace mw = lubancode::hooks::middleware;
using Json = nlohmann::json;

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
struct DispatchScope {
    lubancode::hooks::HookDispatcher& dispatcher;
    explicit DispatchScope(lubancode::hooks::HookDispatcher& value) : dispatcher(value) { dispatcher.EnterMiddlewareDispatch(); }
    ~DispatchScope() { dispatcher.LeaveMiddlewareDispatch(); }
};
std::string Failure(const mw::DispatchOutcome& outcome) {
    return outcome.kind == mw::DispatchOutcome::Kind::Denied
        ? outcome.deny_code + ": " + outcome.deny_message : outcome.error_code + ": " + outcome.error_detail;
}
} // namespace

lubancode::agent::TurnWiring::ActionPreDecision RunPreAction(
    lubancode::hooks::HookDispatcher& dispatcher, const mw::DispatchTrigger& trigger) {
    lubancode::agent::TurnWiring::ActionPreDecision result;
    auto* middleware = dispatcher.middleware();
    if (!middleware || middleware->registry().Selected(mw::HookPoint::PreAction).empty()) return result;
    DispatchScope scope(dispatcher);
    auto effects = std::make_shared<DeferredEffects>(dispatcher.middleware_sink());
    auto outcome = middleware->Dispatch(mw::HookPoint::PreAction, trigger, [](const Json& input) { return input; }, effects.get());
    if (!outcome.Ok()) {
        result.hook.decision = lubancode::runtime::ToolHookDecision::Decision::Deny;
        result.hook.reason = Failure(outcome);
        result.failed = outcome.kind == mw::DispatchOutcome::Kind::Failed;
        result.error_code = outcome.kind == mw::DispatchOutcome::Kind::Denied ? outcome.deny_code : outcome.error_code;
        effects->Settle(!result.failed, result.hook.reason, nullptr, &outcome);
        return result;
    }
    const auto& candidate = outcome.value.is_null() ? outcome.adopted_input : outcome.value;
    if (!candidate.is_object() || candidate.size() != 1 || !candidate.contains("arguments") || !candidate["arguments"].is_object()) {
        result.failed = true; result.error_code = "sdk.action.input_invalid";
        result.hook.decision = lubancode::runtime::ToolHookDecision::Decision::Deny;
        result.hook.reason = "sdk.action.input_invalid: final candidate is not {arguments:object}";
        effects->Settle(false, result.hook.reason);
        return result;
    }
    if (candidate != trigger.input) result.hook.updated_input = candidate["arguments"];
    for (const auto& record : outcome.records) for (const auto& effect : record.effects)
        if (effect.applied && effect.type == "admission.decision" && effect.payload.value("decision", Json()) == "ask") {
            result.hook.decision = lubancode::runtime::ToolHookDecision::Decision::Ask;
            result.hook.reason = effect.payload.value("reason", std::string());
        }
    result.settle = [effects = std::move(effects)](bool accepted, const std::string& reason, const Json& adopted) {
        const Json effective{{"arguments", adopted}};
        effects->Settle(accepted, reason, &effective);
    };
    return result;
}

std::expected<lubancode::tools::Tool::Result, std::string> RunPostAction(
    lubancode::hooks::HookDispatcher& dispatcher, const mw::DispatchTrigger& trigger,
    const lubancode::tools::Tool::Result& original) {
    auto* middleware = dispatcher.middleware();
    if (!middleware || middleware->registry().Selected(mw::HookPoint::PostAction).empty()) return original;
    DispatchScope scope(dispatcher);
    DeferredEffects effects(dispatcher.middleware_sink());
    auto outcome = middleware->Dispatch(mw::HookPoint::PostAction, trigger, [](const Json& input) { return input; }, &effects);
    if (!outcome.Ok()) { auto error = Failure(outcome); effects.Settle(false, error); return std::unexpected(std::move(error)); }
    std::size_t bytes = 0, count = 0;
    for (const auto& record : outcome.records) for (const auto& effect : record.effects) {
        if (!effect.applied || effect.type != "result.supplement") continue;
        const auto& text = effect.payload.at("text").get_ref<const std::string&>();
        if (++count > 16 || text.size() > 32 * 1024 - bytes) {
            effects.Settle(false, "sdk.action.supplement_limit"); return std::unexpected("sdk.action.supplement_limit");
        }
        bytes += text.size();
    }
    auto adopted = original;
    for (const auto& record : outcome.records) for (const auto& effect : record.effects)
        if (effect.applied && effect.type == "result.supplement")
            adopted.AppendText("\n[Action " + record.implementation_ref + " " + record.definition_hash + "] " +
                effect.payload.at("text").get_ref<const std::string&>());
    effects.Settle(true, {}, nullptr, &outcome);
    return adopted;
}
} // namespace lubancore::detail
