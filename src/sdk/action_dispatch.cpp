#include "sdk/action_dispatch.hpp"

#include <memory>

#include "runtime/middleware_deferred_effects.hpp"

namespace lubancore::detail {
namespace {
namespace mw = lubancode::hooks::middleware;
using Json = nlohmann::json;

using DeferredEffects = lubancode::runtime::middleware_detail::DeferredEffects;
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
