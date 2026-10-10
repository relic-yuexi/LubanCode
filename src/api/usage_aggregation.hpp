#pragma once

#include <array>
#include <cstdint>
#include <initializer_list>
#include <limits>
#include <optional>

#include "api/types.hpp"
#include "api/usage_observation.hpp"

namespace lubancode::api::usage_aggregation {
namespace facts = ::lubancore::usage::v1;

inline std::array<std::int64_t, facts::kFieldCount> Values(const Usage& usage) {
    return {usage.input_tokens, usage.output_tokens, usage.cache_read_tokens,
            usage.cache_creation_tokens, usage.output_reasoning_tokens};
}
inline void Assign(Usage& usage, const std::array<std::int64_t, facts::kFieldCount>& values) {
    usage.input_tokens = values[0];
    usage.output_tokens = values[1];
    usage.cache_read_tokens = values[2];
    usage.cache_creation_tokens = values[3];
    usage.output_reasoning_tokens = values[4];
}
inline void Increment(std::uint64_t& value, facts::Coverage& coverage) {
    if (value == std::numeric_limits<std::uint64_t>::max()) coverage.counter_overflow = true;
    else ++value;
}

// Keep legacy numeric totals, but never infer per-field reporting from a number
// or a whole-reply flag. After overflow a field freezes its last representable
// prefix; omitted contributions and the overflow flag prevent exactness claims.
inline void Add(Usage& total, facts::Coverage& coverage, const Usage& source,
                const facts::Observation* observation) {
    Increment(coverage.samples, coverage);
    auto accumulated = Values(total);
    const auto values = Values(source);
    const bool invalid_material = observation != nullptr &&
                                  !usage_observation::Validate(*observation, values).has_value();
    if (invalid_material) observation = nullptr;
    for (std::size_t i = 0; i < facts::kFieldCount; ++i) {
        auto& count = coverage.fields[i];
        if (observation != nullptr) {
            const auto& field = observation->fields[i];
            if (field.presence == facts::Presence::Present) Increment(count.observed, coverage);
            else Increment(count.missing, coverage);
            if (field.validity == facts::Validity::ValidInteger) Increment(count.valid, coverage);
            if (field.origin == facts::Origin::Inferred) Increment(count.inferred, coverage);
            bool anomalous = values[i] < 0 || (field.validity != facts::Validity::Unknown &&
                             field.validity != facts::Validity::ValidInteger);
            for (const auto& anomaly : observation->anomalies) {
                if (anomaly.affected_field_count == 0)
                    anomalous = anomalous || static_cast<std::size_t>(anomaly.field) == i;
                else for (std::size_t n = 0; n < anomaly.affected_field_count; ++n)
                    anomalous = anomalous || static_cast<std::size_t>(anomaly.affected_fields[n]) == i;
            }
            if (anomalous) Increment(count.anomalous, coverage);
        } else {
            Increment(count.missing, coverage);
            if (invalid_material || values[i] < 0) Increment(count.anomalous, coverage);
        }
        if (count.arithmetic_overflow) {
            Increment(count.omitted, coverage);
            continue;
        }
        const auto result = usage_observation::CheckedAdd(accumulated[i], values[i]);
        if (!result) {
            count.arithmetic_overflow = true;
            Increment(count.omitted, coverage);
        } else {
            accumulated[i] = *result;
            Increment(count.included, coverage);
        }
    }
    Assign(total, accumulated);
}

inline bool Exact(const facts::Coverage& coverage, facts::Field field) {
    if (static_cast<std::size_t>(field) >= facts::kFieldCount) return false;
    const auto& count = coverage.fields[static_cast<std::size_t>(field)];
    return coverage.samples != 0 && !coverage.counter_overflow &&
           count.valid == coverage.samples && count.inferred == 0 &&
           count.anomalous == 0 && count.omitted == 0 && !count.arithmetic_overflow;
}

inline std::optional<std::int64_t> TotalInput(const Usage& usage) {
    auto value = usage_observation::CheckedAdd(usage.input_tokens, usage.cache_read_tokens);
    return value ? usage_observation::CheckedAdd(*value, usage.cache_creation_tokens) : std::nullopt;
}

// Precision is scoped to the three input fields. Output-only gaps do not erase
// valid input evidence; legacy flags, inferred cache splits and invalid material
// cannot admit a calibration sample. This does not alter displayed numbers.
inline std::optional<std::int64_t> ExactTotalInput(
    const Usage& usage, const facts::Observation* observation) {
    Usage owned;
    facts::Coverage coverage;
    Add(owned, coverage, usage, observation);
    for (const auto field : {facts::Field::Input, facts::Field::CacheRead, facts::Field::CacheCreation})
        if (!Exact(coverage, field)) return std::nullopt;
    return TotalInput(owned);
}

}  // namespace lubancode::api::usage_aggregation
