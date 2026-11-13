#pragma once

#include <cstddef>
#include <expected>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "hooks/middleware.hpp"
#include "platform/text_encoding.hpp"

namespace lubancode::hooks::middleware::action_contract {
using Json = nlohmann::json;

inline constexpr std::size_t kActionArgumentsBytes = 1024 * 1024;
inline constexpr std::size_t kActionTextBytes = 1024 * 1024;
inline constexpr std::size_t kActionInputBytes = 2 * 1024 * 1024;
inline constexpr std::size_t kSupplementBytes = 16 * 1024;
inline constexpr std::size_t kSupplementTotalBytes = 32 * 1024;
inline constexpr std::size_t kEffectCount = 16;

inline bool ActionPoint(HookPoint point) {
    return point == HookPoint::PreAction || point == HookPoint::PostAction;
}
inline bool ValidActionText(const std::string& text, std::size_t cap) {
    return text.size() <= cap && text.find('\0') == std::string::npos &&
        platform::IsValidUtf8(text);
}
inline bool ActionJsonStrings(const Json& value) {
    std::vector<const Json*> pending{&value};
    while (!pending.empty()) {
        const auto* current = pending.back(); pending.pop_back();
        if (current->is_string() && !ValidActionText(current->get_ref<const std::string&>(), kActionInputBytes)) return false;
        if (current->is_object()) {
            for (auto it = current->begin(); it != current->end(); ++it) {
                if (!ValidActionText(it.key(), kActionInputBytes)) return false;
                pending.push_back(&it.value());
            }
        } else if (current->is_array()) for (const auto& entry : *current) pending.push_back(&entry);
    }
    return true;
}
inline bool IsArguments(const Json& value) {
    return value.is_object() && value.size() == 1 && value.contains("arguments") &&
        value["arguments"].is_object() && value.dump().size() <= kActionArgumentsBytes && ActionJsonStrings(value);
}
inline bool IsActionInput(const Json& value, HookPoint point) {
    if (point == HookPoint::PreAction) return IsArguments(value);
    if (!value.is_object() || value.size() != 2 || !value.contains("arguments") || !value["arguments"].is_object() ||
        !value.contains("result") || !value["result"].is_object() || value["result"].size() != 4 ||
        value.dump().size() > kActionInputBytes || Json{{"arguments", value["arguments"]}}.dump().size() > kActionArgumentsBytes ||
        !ActionJsonStrings(value)) return false;
    const auto& result = value["result"];
    return result.contains("text") && result["text"].is_string() && result.contains("isError") && result["isError"].is_boolean() &&
        result.contains("outcome") && result["outcome"].is_string() && result.contains("errorCode") && result["errorCode"].is_string() &&
        ValidActionText(result["text"].get_ref<const std::string&>(), kActionTextBytes) &&
        ValidActionText(result["errorCode"].get_ref<const std::string&>(), 256);
}
inline bool ObserverEffectsAllowed(bool deny, std::size_t count) { return !deny && count == 0; }
inline std::optional<const char*> AdmissionError(const Json& payload) {
    if (!payload.is_object() || payload.size() != 2 || payload.value("decision", Json()) != "ask" ||
        !payload.contains("reason") || !payload["reason"].is_string() ||
        !ValidActionText(payload["reason"].get_ref<const std::string&>(), 4096))
        return "PreAction admission requires exactly {decision:ask,reason:string}";
    return std::nullopt;
}
inline std::optional<const char*> SupplementError(HookPoint point, const Json& payload, std::size_t& bytes) {
    if (point != HookPoint::PostAction || !payload.is_object() || payload.size() != 1 ||
        !payload.contains("text") || !payload["text"].is_string())
        return "ResultSupplement requires PostAction and exactly {text:string}";
    const auto& text = payload["text"].get_ref<const std::string&>();
    if (!ValidActionText(text, kSupplementBytes) || bytes > kSupplementTotalBytes ||
        text.size() > kSupplementTotalBytes - bytes)
        return "ResultSupplement exceeds its UTF-8 byte boundary";
    bytes += text.size();
    return std::nullopt;
}
inline bool ValidHandlerError(HookPoint point, const std::string& code, const std::string& message) {
    return !(ActionPoint(point) && (!ValidActionText(code, 256) || !ValidActionText(message, 4096))) &&
        !code.empty() && platform::IsValidUtf8(code) && platform::IsValidUtf8(message);
}

struct SupplementCost { std::size_t count = 0, bytes = 0; };
// Borrowed values only. This does not copy/dump bad output or effect payloads,
// reserve a dispatch budget, authorize a Job, or attest durable settlement.
inline std::expected<SupplementCost, HandlerError> CheckJobPostReturn(const HandlerReturn& result, bool observer) {
    const auto reject = [](const char* message) -> std::expected<SupplementCost, HandlerError> {
        return std::unexpected(HandlerError{std::string(err::kResultInvalid), message});
    };
    if (observer && !ObserverEffectsAllowed(result.deny, result.effects.size()))
        return reject("observers cannot deny or return effects");
    if (!result.output.is_null())
        return reject("Action output is forbidden or exceeds its text boundary");
    if (result.deny || !ValidActionText(result.deny_code, 256) || !ValidActionText(result.deny_message, 4096))
        return reject("Action denial is forbidden or exceeds its text boundary");
    if (result.effects.size() > kEffectCount) return reject("Action returns more than 16 effects");
    SupplementCost cost;
    for (const auto& effect : result.effects) {
        if (effect.type != EffectType::ResultSupplement) return reject("Job PostAction permits only ResultSupplement");
        if (const auto error = SupplementError(HookPoint::PostAction, effect.payload, cost.bytes)) return reject(*error);
    }
    cost.count = result.effects.size();
    return cost;
}
} // namespace lubancode::hooks::middleware::action_contract
