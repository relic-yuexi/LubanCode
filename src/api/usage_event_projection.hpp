#pragma once

#include <array>
#include <expected>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

#include "api/types.hpp"
#include "api/usage_provider_normalizers.hpp"

namespace lubancode::api::usage_wire {

inline std::expected<std::optional<std::string>, std::string_view> ResponseId(
    const nlohmann::json* value) {
    if (value == nullptr) return std::optional<std::string>{};
    if (!value->is_string()) return std::unexpected("usage.response_id.type");
    const auto& id=value->get_ref<const std::string&>();
    if (!usage_observation::TextFits(id,facts::kMaxResponseIdBytes,true))
        return std::unexpected("usage.response_id.invalid");
    return std::optional<std::string>(id);
}

inline Usage Numbers(const std::array<std::int64_t, facts::kFieldCount>& values) noexcept {
    Usage usage;
    usage.input_tokens=values[static_cast<std::size_t>(facts::Field::Input)];
    usage.output_tokens=values[static_cast<std::size_t>(facts::Field::Output)];
    usage.cache_read_tokens=values[static_cast<std::size_t>(facts::Field::CacheRead)];
    usage.cache_creation_tokens=values[static_cast<std::size_t>(facts::Field::CacheCreation)];
    usage.output_reasoning_tokens=values[static_cast<std::size_t>(facts::Field::OutputReasoning)];
    return usage;
}

inline Usage Numbers(const Snapshot& snapshot) noexcept { return Numbers(snapshot.values); }

// The compatibility flags retain presence, including a present malformed
// scalar. Precise validity and inferred/normalized origins live in the typed
// observation. A missing field's numeric slot is never evidence of reported zero.
template <typename Event>
inline void Apply(Event& event,const Snapshot& snapshot,
                  const std::optional<std::string>& provider_response_id={}) {
    static_assert(std::is_same_v<Event, UsageSnapshot> || std::is_same_v<Event, MessageDone>,
                  "Usage facts belong to a usage snapshot or message terminal, never a content block");
    event.usage=Numbers(snapshot);
    event.usage_reported=true;
    if (!snapshot.material_error.empty()) {
        event.cache_read_reported=false;
        event.cache_creation_reported=false;
        event.usage_observation.reset();
        event.usage_anomaly.clear();
        event.provider_response_id=provider_response_id;
        return;
    }
    event.cache_read_reported=snapshot.observation.fields[static_cast<std::size_t>(facts::Field::CacheRead)].presence==facts::Presence::Present;
    event.cache_creation_reported=snapshot.observation.fields[static_cast<std::size_t>(facts::Field::CacheCreation)].presence==facts::Presence::Present;
    event.usage_anomaly.clear();
    // The bounded typed vector carries every anomaly. Existing text consumers
    // retain a bounded first diagnostic, not an unbounded serialized material.
    if (!snapshot.observation.anomalies.empty())
        event.usage_anomaly=snapshot.observation.anomalies.front().detail;
    event.usage_observation=snapshot.observation;
    event.provider_response_id=provider_response_id;
}

inline UsageSnapshot Nonterminal(const Snapshot& snapshot,
                                const std::optional<std::string>& provider_response_id={}) {
    UsageSnapshot event;Apply(event,snapshot,provider_response_id);return event;
}

inline UsageSnapshot NumericUnknown(const Snapshot& snapshot) {
    UsageSnapshot event;
    event.usage = Numbers(snapshot); event.usage_reported = true;
    return event;
}

// One scoped anomaly closes exactness for every affected counter. Keep bounded
// witnesses for the first and latest conflicting real IDs; no ID is synthesized.
inline std::expected<void, std::string_view> IdentityConflict(
    Snapshot& snapshot, const std::string& first, const std::string& conflicting) {
    if (!usage_observation::TextFits(first, facts::kMaxResponseIdBytes, true) ||
        !usage_observation::TextFits(conflicting, facts::kMaxResponseIdBytes, true) || first == conflicting)
        return std::unexpected("usage.identity.invalid_witness");
    auto& observation = snapshot.observation;
    for (auto& anomaly : observation.anomalies) {
        if (anomaly.code != facts::AnomalyCode::ResponseIdentityConflict) continue;
        if (anomaly.raw_field_count != 2 || anomaly.raw_fields[1] >= observation.raw_fields.size())
            return std::unexpected("usage.identity.invalid_witness");
        observation.raw_fields[anomaly.raw_fields[1]] =
            CaptureScalar("response_identity.conflicting", nlohmann::json(conflicting));
        return usage_observation::Validate(observation, snapshot.values);
    }
    if (observation.raw_fields.size() > facts::kMaxRawFields - 2 ||
        observation.anomalies.size() == facts::kMaxAnomalies)
        return std::unexpected("usage.identity.material_limit");
    const auto start = static_cast<std::uint16_t>(observation.raw_fields.size());
    observation.raw_fields.push_back(CaptureScalar("response_identity.first", nlohmann::json(first)));
    observation.raw_fields.push_back(CaptureScalar("response_identity.conflicting", nlohmann::json(conflicting)));
    facts::Anomaly anomaly;
    anomaly.code = facts::AnomalyCode::ResponseIdentityConflict;
    anomaly.detail = "provider response identity changed within one response scope";
    anomaly.raw_fields[0] = start; anomaly.raw_fields[1] = start + 1; anomaly.raw_field_count = 2;
    anomaly.affected_field_count = static_cast<std::uint8_t>(facts::kFieldCount);
    for (std::size_t i = 0; i < facts::kFieldCount; ++i) anomaly.affected_fields[i] = static_cast<facts::Field>(i);
    observation.anomalies.push_back(std::move(anomaly));
    return usage_observation::Validate(observation, snapshot.values);
}

// Old aggregate initializers only supplied input/output. Those two signed values
// are reported facts, including zero; appended slots have no presence evidence.
inline std::expected<Snapshot, std::string_view> LegacyBackend(const Usage& usage) {
    Builder builder("lubancore.backend.legacy");
    const nlohmann::json input = usage.input_tokens;
    const nlohmann::json output = usage.output_tokens;
    builder.Report(facts::Field::Input, builder.Capture("usage.input_tokens", &input));
    builder.Report(facts::Field::Output, builder.Capture("usage.output_tokens", &output));
    auto result = std::move(builder).Finish();
    if (result) result->values = {usage.input_tokens, usage.output_tokens, usage.cache_read_tokens,
                                 usage.cache_creation_tokens, usage.output_reasoning_tokens};
    return result;
}

}  // namespace lubancode::api::usage_wire
