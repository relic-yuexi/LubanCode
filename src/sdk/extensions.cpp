#include "sdk/extensions.hpp"

#include <algorithm>
#include <exception>
#include <map>
#include <set>
#include <thread>
#include <utility>

#include <nlohmann/json.hpp>

#include "hooks/middleware_builtins.hpp"
#include "platform/text_encoding.hpp"
#include "sdk/callback_scope.hpp"

namespace lubancore {
namespace ext = extensions::v1;
namespace mw = lubancode::hooks::middleware;
using Json = nlohmann::json;

namespace {

Error InvalidResult(std::string message) {
    return {std::string(mw::err::kResultInvalid), std::move(message)};
}

Result<Json> ParseJson(const std::string& source, const char* description) {
    Json value = Json::parse(source, nullptr, false);
    if (value.is_discarded()) return std::unexpected(InvalidResult(std::string(description) + " is invalid JSON"));
    return value;
}

bool IsPrompt(const Json& value) {
    return value.is_object() && value.size() == 1 && value.contains("prompt") && value["prompt"].is_string();
}

bool NonNegativeInteger(const Json& value) {
    return value.is_number_unsigned() || (value.is_number_integer() && value.get<std::int64_t>() >= 0);
}

bool StringField(const Json& value, const char* key) {
    return value.contains(key) && value[key].is_string() && !value[key].get_ref<const std::string&>().empty();
}

bool IsEstimate(const Json& value) {
    if (!value.is_object() || !StringField(value, "estimator") || !value.contains("estimatorVersion") ||
        !NonNegativeInteger(value["estimatorVersion"]) || value["estimatorVersion"] == 0 ||
        value.value("scope", Json()) != "model_input_json_utf8_v1" ||
        value.value("encoding", Json()) != "utf-8" || !StringField(value, "rounding") ||
        !value.contains("inputUtf8Bytes") || !NonNegativeInteger(value["inputUtf8Bytes"]) ||
        !value.contains("estimatedInputTokens") || !NonNegativeInteger(value["estimatedInputTokens"]) ||
        !value.contains("coverage") || !value["coverage"].is_string() ||
        !value.contains("unestimatedModalities") || !value["unestimatedModalities"].is_array()) return false;
    const std::string coverage = value["coverage"].get<std::string>();
    if (coverage != "full" && coverage != "text_proxy" && coverage != "partial") return false;
    for (const auto& modality : value["unestimatedModalities"]) {
        if (!modality.is_string() || modality.get_ref<const std::string&>().empty()) return false;
    }
    return true;
}

Result<mw::HandlerReturn> ConvertReturn(const ext::HandlerReturn& source, const mw::InvocationCtx& context,
                                       bool observer) {
    if (observer && (source.deny || !source.effects.empty())) {
        return std::unexpected(InvalidResult("observers cannot deny or return effects"));
    }
    mw::HandlerReturn result;
    if (source.output_json) {
        auto parsed = ParseJson(*source.output_json, "handler output");
        if (!parsed) return std::unexpected(parsed.error());
        if (!observer && context.point == mw::HookPoint::PreRequest && context.stage == mw::Stage::Estimate &&
            !IsEstimate(*parsed)) {
            return std::unexpected(InvalidResult("Estimate requires an EST1 measurement object"));
        }
        result.output = std::move(*parsed);
    }
    if (source.deny) {
        if (source.deny_code.empty() || !lubancode::platform::IsValidUtf8(source.deny_code) ||
            !lubancode::platform::IsValidUtf8(source.deny_message)) {
            return std::unexpected(InvalidResult("Denied requires a nonempty UTF-8 code and UTF-8 message"));
        }
        result.deny = true;
        result.deny_code = source.deny_code;
        result.deny_message = source.deny_message;
    }
    for (const auto& effect : source.effects) {
        auto parsed = ParseJson(effect.payload_json, "effect payload");
        if (!parsed) return std::unexpected(parsed.error());
        mw::Effect converted;
        switch (effect.type) {
            case ext::EffectType::ContextAppend:
                if ((context.point != mw::HookPoint::PreUser && context.point != mw::HookPoint::PostUser) ||
                    !parsed->is_object() || parsed->size() != 1 || !parsed->contains("text") ||
                    !(*parsed)["text"].is_string()) {
                    return std::unexpected(InvalidResult("ContextAppend requires a user hook and exactly {text:string}"));
                }
                converted.type = mw::EffectType::ContextAppend;
                break;
            case ext::EffectType::AdmissionDecision: {
                if (context.point != mw::HookPoint::PreRequest || context.stage != mw::Stage::Capacity ||
                    !parsed->is_object() || parsed->size() != 2 || !parsed->contains("decision") ||
                    !(*parsed)["decision"].is_string() || !parsed->contains("reason") ||
                    !(*parsed)["reason"].is_string()) {
                    return std::unexpected(InvalidResult("AdmissionDecision requires Capacity and exactly {decision:string,reason:string}"));
                }
                const auto decision = (*parsed)["decision"].get<std::string>();
                if (decision != "allow" && decision != "recover" && decision != "reject") {
                    return std::unexpected(InvalidResult("capacity decision must be allow, recover or reject"));
                }
                converted.type = mw::EffectType::AdmissionDecision;
                break;
            }
            default:
                return std::unexpected(InvalidResult("unsupported effect type"));
        }
        converted.payload = std::move(*parsed);
        result.effects.push_back(std::move(converted));
    }
    return result;
}

Result<mw::HookPoint> ConvertPoint(ext::Point point) {
    switch (point) {
        case ext::Point::PreUser: return mw::HookPoint::PreUser;
        case ext::Point::PostUser: return mw::HookPoint::PostUser;
        case ext::Point::PreRequest: return mw::HookPoint::PreRequest;
    }
    return std::unexpected(Error{std::string(mw::err::kManifestInvalid), "unsupported hook point"});
}

Result<mw::Stage> ConvertStage(ext::Stage stage) {
    switch (stage) {
        case ext::Stage::Default: return mw::Stage::Default;
        case ext::Stage::Estimate: return mw::Stage::Estimate;
        case ext::Stage::Capacity: return mw::Stage::Capacity;
    }
    return std::unexpected(Error{std::string(mw::err::kPlanBadStage), "unsupported hook stage"});
}

Result<mw::SourceLayer> ConvertLayer(ext::SourceLayer layer) {
    switch (layer) {
        case ext::SourceLayer::Extension: return mw::SourceLayer::Extension;
        case ext::SourceLayer::Project: return mw::SourceLayer::Project;
        case ext::SourceLayer::User: return mw::SourceLayer::User;
        case ext::SourceLayer::Session: return mw::SourceLayer::Session;
    }
    return std::unexpected(Error{std::string(mw::err::kManifestInvalid), "unsupported host source layer"});
}

bool IdentityToken(const std::string& value) {
    return !value.empty() && std::all_of(value.begin(), value.end(), [](unsigned char ch) {
        return (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') ||
               ch == '.' || ch == '_' || ch == '-' || ch == '+';
    });
}

bool ValidStrings(const std::vector<std::string>& strings) {
    return std::all_of(strings.begin(), strings.end(), [](const std::string& value) {
        return !value.empty() && lubancode::platform::IsValidUtf8(value);
    });
}

bool ValidOptional(const std::optional<std::string>& value) {
    return !value || (!value->empty() && lubancode::platform::IsValidUtf8(*value));
}

Result<void> CheckResumePlan(const Json& plan, const std::optional<std::string>& expected_plan_json) {
    if (!expected_plan_json) return {};
    const Json expected = Json::parse(*expected_plan_json, nullptr, false);
    if (!expected.is_object() || expected != plan) {
        return std::unexpected(Error{"sdk.extension.resume_mismatch", "frozen extension declarations differ from the saved session plan"});
    }
    return {};
}

} // namespace

namespace extensions::v1 {

struct Next::State {
    std::mutex mutex;
    std::thread::id owner;
    mw::NextCall* native = nullptr;
    Point point = Point::PreUser;
    bool active = true;
    bool observer = false;
    bool consumed = false;
};

Next::Next(std::shared_ptr<State> state) : state_(std::move(state)) {}

Result<DownstreamOutcome> Next::Call(std::optional<std::string> candidate_json) const {
    if (!state_) return std::unexpected(Error{std::string(mw::err::kNextExpired), "no live invocation"});
    mw::NextCall* native = nullptr;
    std::optional<Json> candidate;
    {
        const std::lock_guard lock(state_->mutex);
        if (!state_->active || state_->native == nullptr)
            return std::unexpected(Error{std::string(mw::err::kNextExpired), "invocation has returned"});
        if (state_->owner != std::this_thread::get_id())
            return std::unexpected(Error{"hook.next.wrong_thread", "Next must run on its invocation thread"});
        if (state_->observer)
            return std::unexpected(Error{std::string(mw::err::kNextNotAllowed), "observers cannot call Next"});
        if (state_->consumed)
            return std::unexpected(Error{std::string(mw::err::kNextAlreadyConsumed), "Next has already been consumed"});
        if (candidate_json) {
            if (state_->point != Point::PreUser)
                return std::unexpected(Error{std::string(mw::err::kNextBadCandidate), "this hook cannot rewrite input"});
            Json parsed = Json::parse(*candidate_json, nullptr, false);
            if (parsed.is_discarded() || !IsPrompt(parsed))
                return std::unexpected(Error{std::string(mw::err::kNextBadCandidate), "PreUser candidate must be exactly {prompt:string}"});
            candidate = std::move(parsed);
        }
        // Consume before entering downstream; do not hold this lock around user
        // code. Only this thread may use the pointer, and it cannot outlive Invoke.
        state_->consumed = true;
        native = state_->native;
    }
    const auto value = native->Call(std::move(candidate));
    if (value.kind == mw::DownstreamOutcome::Kind::Invalid)
        return std::unexpected(Error{value.code, value.message});
    DownstreamOutcome result;
    switch (value.kind) {
        case mw::DownstreamOutcome::Kind::Value: result.kind = DownstreamOutcome::Kind::Value; break;
        case mw::DownstreamOutcome::Kind::Denied: result.kind = DownstreamOutcome::Kind::Denied; break;
        case mw::DownstreamOutcome::Kind::Failed: result.kind = DownstreamOutcome::Kind::Failed; break;
        case mw::DownstreamOutcome::Kind::Invalid: break;
    }
    result.value_json = value.value.dump();
    result.code = value.code;
    result.message = value.message;
    return result;
}

} // namespace extensions::v1

namespace detail {

struct ExtensionNextAccess {
    struct Lease {
        std::shared_ptr<ext::Next::State> state;
        explicit Lease(std::shared_ptr<ext::Next::State> value) : state(std::move(value)) {}
        Lease(const Lease&) = delete;
        Lease& operator=(const Lease&) = delete;
        ~Lease() {
            const std::lock_guard lock(state->mutex);
            state->active = false;
            state->native = nullptr;
        }
        ext::Next next() const { return ext::Next(state); }
    };
    static Lease Begin(mw::NextCall& native, ext::Point point, bool observer) {
        auto state = std::make_shared<ext::Next::State>();
        state->owner = std::this_thread::get_id();
        state->native = &native;
        state->point = point;
        state->observer = observer;
        return Lease{std::move(state)};
    }
};

SessionExtensions::SessionExtensions(ext::SessionContext context) : context_(std::move(context)) {}

SessionExtensions::~SessionExtensions() {
    CallbackScope callback_scope;
    dispatcher_.reset();
    while (!instances_.empty()) instances_.pop_back();
}

void SessionExtensions::SetOperationScope(std::string operation_id) {
    const std::lock_guard lock(scope_mutex_);
    operation_id_ = std::move(operation_id);
}

std::string SessionExtensions::OperationScope() const {
    const std::lock_guard lock(scope_mutex_);
    return operation_id_;
}

std::string SessionExtensions::DescribePlan() const { return plan_json_; }

Result<std::unique_ptr<SessionExtensions>> SessionExtensions::Build(
    std::vector<ext::Registration>& registrations, const ext::SessionContext& context,
    std::optional<std::string> expected_plan_json) {
    CallbackScope callback_scope;
    struct Sources {
        std::vector<ext::Registration>& registrations;
        ~Sources() {
            // Clear actual std::function sources. Moving is insufficient for
            // small-buffer callbacks in some standard library implementations.
            for (auto& registration : registrations) registration.factory = nullptr;
        }
    } sources{registrations};
    try {
        auto module = std::unique_ptr<SessionExtensions>(new SessionExtensions(context));
        if (registrations.empty()) {
            module->plan_json_ = R"({"schemaVersion":1,"registryRevision":0,"points":{},"overridden":[],"extensions":[]})";
            const auto compatible = CheckResumePlan(Json::parse(module->plan_json_), expected_plan_json);
            if (!compatible) return std::unexpected(compatible.error());
            return module;
        }
        struct Slot { mw::Stage stage; bool required; };
        std::map<std::string, Slot> slots;
        std::set<std::pair<int, std::string>> extension_ids;
        for (const auto& registration : registrations) {
            const auto layer = ConvertLayer(registration.source_layer);
            if (!layer) return std::unexpected(layer.error());
            const auto& manifest = registration.manifest;
            if (!IdentityToken(manifest.id) || !IdentityToken(manifest.version) || manifest.handlers.empty() ||
                !lubancode::platform::IsValidUtf8(registration.source_label) ||
                !extension_ids.emplace(mw::LayerRank(*layer), manifest.id).second) {
                return std::unexpected(Error{std::string(mw::err::kManifestInvalid), "invalid or duplicate extension identity"});
            }
            if (!registration.factory)
                return std::unexpected(Error{"sdk.extension.factory_failed", "extension factory is missing"});
            for (const auto& handler : manifest.handlers) {
                auto point = ConvertPoint(handler.point);
                if (!point) return std::unexpected(point.error());
                auto stage = ConvertStage(handler.stage);
                if (!stage) return std::unexpected(stage.error());
                if (!mw::StageAllowed(*point, *stage) ||
                    (*point == mw::HookPoint::PreRequest && *stage == mw::Stage::Default)) {
                    return std::unexpected(Error{std::string(mw::err::kPlanBadStage), "hook point and stage do not match"});
                }
                if (!IdentityToken(handler.name) || handler.definition_hash.empty() ||
                    !lubancode::platform::IsValidUtf8(handler.definition_hash) || !ValidStrings(handler.before) ||
                    !ValidStrings(handler.after) || !ValidOptional(handler.match.origin) ||
                    !ValidOptional(handler.match.purpose) || !ValidOptional(handler.match.delivery_mode) ||
                    (handler.failure_policy != ext::FailurePolicy::Abort &&
                     handler.failure_policy != ext::FailurePolicy::KeepOriginal)) {
                    return std::unexpected(Error{std::string(mw::err::kManifestInvalid), "invalid handler declaration"});
                }
                const auto key = std::string(mw::ToString(*point)) + "/" + handler.name;
                const auto [it, inserted] = slots.emplace(key, Slot{*stage, handler.required});
                if (!inserted) {
                    if (it->second.stage != *stage)
                        return std::unexpected(Error{std::string(mw::err::kPlanStageMismatch), "same-key handlers have different stages: " + key});
                    it->second.required = it->second.required || handler.required;
                }
            }
        }
        mw::MiddlewarePool pool;
        mw::AddBuiltinRequestSlots(pool);
        Json identities = Json::array();
        for (std::size_t instance_index = 0; instance_index < registrations.size(); ++instance_index) {
            const auto& registration = registrations[instance_index];
            const auto layer = *ConvertLayer(registration.source_layer);
            Json declarations = Json::array();
            for (const auto& handler : registration.manifest.handlers) {
                declarations.push_back(Json{
                    {"point", mw::ToString(*ConvertPoint(handler.point))},
                    {"stage", mw::ToString(*ConvertStage(handler.stage))},
                    {"name", handler.name}, {"definitionHash", handler.definition_hash},
                    {"priority", handler.priority}, {"before", handler.before}, {"after", handler.after},
                    {"match", {{"origin", handler.match.origin ? Json(*handler.match.origin) : Json()},
                               {"purpose", handler.match.purpose ? Json(*handler.match.purpose) : Json()},
                               {"deliveryMode", handler.match.delivery_mode ? Json(*handler.match.delivery_mode) : Json()}}},
                    {"failurePolicy", handler.failure_policy == ext::FailurePolicy::Abort ? "abort" : "keep_original"},
                    {"required", handler.required}, {"observer", handler.observer}});
            }
            identities.push_back(Json{{"id", registration.manifest.id}, {"version", registration.manifest.version},
                                      {"sourceLayer", mw::ToString(layer)}, {"sourceLabel", registration.source_label},
                                      {"handlers", std::move(declarations)}});
            for (const auto& handler : registration.manifest.handlers) {
                mw::MiddlewareDefinition definition;
                definition.point = *ConvertPoint(handler.point);
                definition.stage = *ConvertStage(handler.stage);
                definition.name = handler.name;
                definition.definition_hash = handler.definition_hash;
                definition.layer = layer;
                definition.source_label = registration.source_label.empty() ? "sdk " + registration.manifest.id : registration.source_label;
                definition.implementation_ref = "sdk/" + registration.manifest.id + "#" + handler.name + "@" + registration.manifest.version;
                definition.priority = handler.priority;
                definition.before = handler.before;
                definition.after = handler.after;
                definition.match = {handler.match.origin, handler.match.purpose, handler.match.delivery_mode};
                definition.failure_policy = handler.failure_policy == ext::FailurePolicy::Abort ? mw::FailurePolicy::Abort : mw::FailurePolicy::KeepOriginal;
                definition.required = slots.at(definition.Key()).required;
                definition.observer = handler.observer;
                auto* module_ptr = module.get();
                definition.builtin = [module_ptr, instance_index, point = handler.point, stage = handler.stage,
                                      name = handler.name, observer = handler.observer](
                    const mw::InvocationCtx& native_context, const Json& input, mw::NextCall& native_next)
                    -> std::expected<mw::HandlerReturn, mw::HandlerError> {
                    CallbackScope invoke_scope;
                    auto lease = ExtensionNextAccess::Begin(native_next, point, observer);
                    ext::Context callback_context;
                    callback_context.session_id = module_ptr->context_.session_id;
                    callback_context.operation_id = module_ptr->OperationScope();
                    callback_context.dispatch_id = native_context.dispatch_id;
                    callback_context.invocation_id = native_context.invocation_id;
                    callback_context.registry_revision = native_context.registry_revision;
                    callback_context.point = point;
                    callback_context.stage = stage;
                    callback_context.hook_name = name;
                    callback_context.definition_hash = native_context.definition_hash;
                    callback_context.turn_id = native_context.turn_id;
                    callback_context.step_id = native_context.step_id;
                    callback_context.request_id = native_context.request_id;
                    callback_context.cancellation.flag = native_context.cancel;
                    auto result = module_ptr->instances_.at(instance_index)->Invoke(
                        callback_context, ext::Input{1, input.dump()}, lease.next());
                    if (!result) {
                        if (result.error().code.empty() || !lubancode::platform::IsValidUtf8(result.error().code) ||
                            !lubancode::platform::IsValidUtf8(result.error().message)) {
                            return std::unexpected(mw::HandlerError{
                                std::string(mw::err::kResultInvalid), "handler error requires a nonempty UTF-8 code and UTF-8 message"});
                        }
                        return std::unexpected(mw::HandlerError{result.error().code, result.error().message});
                    }
                    auto converted = ConvertReturn(*result, native_context, observer);
                    if (!converted) return std::unexpected(mw::HandlerError{converted.error().code, converted.error().message});
                    return std::move(*converted);
                };
                pool.AddDefinition(std::move(definition));
            }
        }
        auto frozen = pool.Publish();
        if (!frozen) return std::unexpected(Error{frozen.error().code, frozen.error().message});
        Json plan = (*frozen)->DescribePlan();
        plan["schemaVersion"] = 1;
        plan["extensions"] = std::move(identities);
        const auto compatible = CheckResumePlan(plan, expected_plan_json);
        if (!compatible) return std::unexpected(compatible.error());
        module->plan_json_ = plan.dump();
        // Publication validates all conflicts, dependencies, stage ordering and
        // required/observer rules before a trusted factory can acquire resources.
        // Native wrappers capture indices; no dispatch becomes reachable until
        // all unique per-session instances have been installed below.
        Sources factory_sources{registrations};
        module->instances_.reserve(registrations.size());
        for (const auto& registration : registrations) {
            Result<std::unique_ptr<ext::Instance>> instance = std::unexpected(Error{"sdk.extension.factory_failed", "factory did not return an instance"});
            try {
                instance = registration.factory(context);
            } catch (const std::exception& error) {
                return std::unexpected(Error{"sdk.extension.factory_failed", error.what()});
            } catch (...) {
                return std::unexpected(Error{"sdk.extension.factory_failed", "factory threw an unknown exception"});
            }
            if (!instance || !*instance) {
                return std::unexpected(Error{"sdk.extension.factory_failed", instance ? "factory returned null" : instance.error().code + ": " + instance.error().message});
            }
            module->instances_.push_back(std::move(*instance));
        }
        module->dispatcher_ = std::make_unique<lubancode::hooks::HookDispatcher>();
        module->dispatcher_->SetMiddleware(std::make_shared<mw::MiddlewareDispatcher>(std::move(*frozen)));
        return module;
    } catch (const std::exception& error) {
        return std::unexpected(Error{"sdk.extension.assembly_failed", error.what()});
    } catch (...) {
        return std::unexpected(Error{"sdk.extension.assembly_failed", "extension assembly threw an unknown exception"});
    }
}

} // namespace detail
} // namespace lubancore
