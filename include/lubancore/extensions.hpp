#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <lubancore/api.hpp>

namespace lubancore::detail { struct ExtensionNextAccess; }

// Trusted, in-process C++ middleware. This is not a sandbox or a DLL loader.
// Each session freezes its own assembly; Close drains it and then releases it.
namespace lubancore::extensions::v1 {

enum class Point { PreUser, PostUser, PreRequest, PreAction, PostAction };
enum class Stage { Default, Estimate, Capacity };
enum class FailurePolicy { Abort, KeepOriginal };
// Host-supplied registration provenance, never a field in an extension manifest.
enum class SourceLayer { Extension, Project, User, Session };

struct SessionContext { std::string session_id; std::string cwd; };
struct ExecutionIdentity {
    std::string session_id, operation_id, turn_id, action_id;
    std::uint64_t attempt = 0;
};
struct Input {
    std::uint32_t schema_version = 1;
    // PreUser/PostUser: exactly {"prompt":string}. PreRequest: frozen model-input
    // snapshot; Capacity additionally carries tokenEstimate and host budget fields.
    // PreAction: exactly {"arguments":object}. PostAction: arguments and result
    // {text,isError,outcome,errorCode}; fields come from actual finished capture.
    // No credentials are supplied. These JSON contracts are versioned separately
    // from the experimental C++ ABI.
    std::string json;
};
struct Context {
    std::string session_id;
    std::string operation_id;
    std::string dispatch_id;
    std::string invocation_id;
    std::uint64_t registry_revision = 0;
    Point point = Point::PreUser;
    Stage stage = Stage::Default;
    std::string hook_name;
    std::string definition_hash;
    // Only identities already issued by the host. Missing levels stay absent.
    std::optional<std::string> turn_id, step_id, request_id;
    Cancellation cancellation;
    // Main Action only, copied from the actual declared/started call. PreAction
    // has no execution identity; a wire call ID is not an execution identity.
    std::optional<std::string> action_id, wire_call_id, tool_name, effective_cwd;
    std::optional<ExecutionIdentity> execution;
};
struct DownstreamOutcome {
    enum class Kind { Value, Denied, Failed };
    Kind kind = Kind::Value;
    std::string value_json;
    std::string code;
    std::string message;
};

class LUBANCORE_API Next {
public:
    Next() = default;
    // Copies share one invocation grant. At most one call reaches the downstream
    // chain. Cross-thread calls reject without consuming the owner's grant.
    // Saved copies expire as Invoke returns and retain no session resources.
    // Observers cannot call Next. PreUser proposes exactly {"prompt":string};
    // PreAction proposes exactly {"arguments":object}. PostAction cannot rewrite.
    Result<DownstreamOutcome> Call(std::optional<std::string> candidate_json = std::nullopt) const;
private:
    struct State;
    explicit Next(std::shared_ptr<State>);
    std::shared_ptr<State> state_;
    friend struct ::lubancore::detail::ExtensionNextAccess;
};

enum class EffectType { ContextAppend, AdmissionDecision, ResultSupplement };
struct Effect {
    EffectType type = EffectType::ContextAppend;
    // ContextAppend: {"text":string}. AdmissionDecision is Capacity-only:
    // {"decision":"allow"|"recover"|"reject", "reason":string}. PreAction
    // accepts only {"decision":"ask","reason":string}. PostAction alone accepts
    // ResultSupplement: exactly {"text":string}; the host supplies provenance.
    std::string payload_json;
};
struct HandlerReturn {
    // Omitted output inherits a consumed Next's value. With no Next call, a
    // normal return short-circuits. Estimate output is the EST1 measurement
    // object: estimator, estimatorVersion, scope=model_input_json_utf8_v1,
    // encoding=utf-8, rounding, inputUtf8Bytes, estimatedInputTokens, coverage
    // (full/text_proxy/partial), unestimatedModalities. Capacity produces an
    // AdmissionDecision effect. PostUser Denied stops the following request,
    // preserving the already committed user message; it never rolls it back.
    std::optional<std::string> output_json;
    std::vector<Effect> effects;
    bool deny = false;
    std::string deny_code, deny_message;

    static HandlerReturn Denied(std::string code, std::string message) {
        HandlerReturn out;
        out.deny = true;
        out.deny_code = std::move(code);
        out.deny_message = std::move(message);
        return out;
    }
};

class Instance {
public:
    virtual ~Instance() = default;
    // Normal chain callbacks run on the session worker; observers may run
    // concurrently on joined helper threads, including observers of this same
    // instance. Implementations must coordinate their own shared state.
    // Observers return neither effects nor Denied and have no usable Next.
    // Cancellation is cooperative: Close waits for Invoke to actually return.
    // Blocking SDK lifecycle calls are rejected in Invoke/factory/destruction;
    // never drop owning Runtime/Session handles from these callbacks.
    virtual Result<HandlerReturn> Invoke(const Context&, const Input&, Next) = 0;
};

struct MatchRule {
    std::optional<std::string> origin, purpose, delivery_mode;
};
struct HandlerDefinition {
    Point point = Point::PreUser;
    Stage stage = Stage::Default;
    std::string name;
    // Required declaration of implementation identity. The host cannot hash
    // arbitrary native function bodies; this is not an attestation of code bytes.
    std::string definition_hash;
    int priority = 100;
    std::vector<std::string> before, after;
    MatchRule match;
    FailurePolicy failure_policy = FailurePolicy::Abort;
    bool required = false;
    bool observer = false;
};
struct Manifest {
    std::string id;
    std::string version;
    std::vector<HandlerDefinition> handlers;
};
struct Registration {
    Manifest manifest;
    SourceLayer source_layer = SourceLayer::Extension;
    std::string source_label;
    // After complete plan validation, OpenSession calls its factory once, then
    // owns the unique instance. Invalid plans never invoke a factory.
    // Factories and instances must not borrow another session's owned state.
    // Assembly failure releases all staged instances and actual factory sources.
    std::function<Result<std::unique_ptr<Instance>>(const SessionContext&)> factory;
};

} // namespace lubancore::extensions::v1
