#pragma once

#include <cstdint>
#include <limits>
#include <optional>

#include <nlohmann/json.hpp>

#include "api/usage_totals.hpp"

namespace lubancode::insights::usage_projection {

struct Knowledge {
    bool input_tokens = true;
    bool cache_read_tokens = true;
    bool cache_creation_tokens = true;
    bool output_tokens = true;
    bool reasoning_tokens = true;
    bool cost_micros = true;

    bool HasUnknown() const noexcept {
        return !input_tokens || !cache_read_tokens || !cache_creation_tokens ||
               !output_tokens || !reasoning_tokens || !cost_micros;
    }
};

inline std::optional<std::int64_t> Value(std::int64_t prefix, bool known) noexcept {
    return known ? std::optional<std::int64_t>(prefix) : std::nullopt;
}

inline nlohmann::json Number(std::int64_t prefix, bool known) {
    return known ? nlohmann::json(prefix) : nlohmann::json(nullptr);
}

inline void Accumulate(std::int64_t& prefix, bool& known,
                       std::optional<std::int64_t> value) noexcept {
    if (!known) return;
    const auto sum = value ? api::usage_observation::CheckedAdd(prefix, *value) : std::nullopt;
    if (sum) prefix = *sum;
    else known = false;
}

template <typename Target>
void AddUsage(Target& target, const api::Usage& usage) noexcept {
    Accumulate(target.input_tokens, target.knowledge.input_tokens, usage.input_tokens);
    Accumulate(target.cache_read_tokens, target.knowledge.cache_read_tokens, usage.cache_read_tokens);
    Accumulate(target.cache_creation_tokens, target.knowledge.cache_creation_tokens,
               usage.cache_creation_tokens);
    Accumulate(target.output_tokens, target.knowledge.output_tokens, usage.output_tokens);
    Accumulate(target.reasoning_tokens, target.knowledge.reasoning_tokens, usage.output_reasoning_tokens);
}

template <typename Target, typename Source>
void AddSummary(Target& target, const Source& source) noexcept {
    Accumulate(target.input_tokens, target.knowledge.input_tokens,
               Value(source.input_tokens, source.knowledge.input_tokens));
    Accumulate(target.cache_read_tokens, target.knowledge.cache_read_tokens,
               Value(source.cache_read_tokens, source.knowledge.cache_read_tokens));
    Accumulate(target.cache_creation_tokens, target.knowledge.cache_creation_tokens,
               Value(source.cache_creation_tokens, source.knowledge.cache_creation_tokens));
    Accumulate(target.output_tokens, target.knowledge.output_tokens,
               Value(source.output_tokens, source.knowledge.output_tokens));
    Accumulate(target.reasoning_tokens, target.knowledge.reasoning_tokens,
               Value(source.reasoning_tokens, source.knowledge.reasoning_tokens));
}

template <typename Owner>
std::optional<std::int64_t> Input(const Owner& owner) noexcept {
    if (!owner.knowledge.input_tokens || !owner.knowledge.cache_read_tokens ||
        !owner.knowledge.cache_creation_tokens) return std::nullopt;
    return api::CheckedTotalInputTokens(api::Usage{
        owner.input_tokens, 0, owner.cache_read_tokens, owner.cache_creation_tokens});
}

inline std::optional<int> Ratio(std::optional<std::int64_t> part,
                                std::optional<std::int64_t> whole) noexcept {
    if (!part || !whole || *whole <= 0 || *part < 0 || *part > *whole) return std::nullopt;
    std::int64_t remainder = 0;
    int quotient = 0;
    for (int index = 0; index < 100; ++index) {
        if (remainder >= *whole - *part) {
            remainder -= *whole - *part;
            ++quotient;
        } else {
            remainder += *part;
        }
    }
    if (remainder >= *whole - remainder) ++quotient;
    return quotient;
}

template <typename Integer>
bool Read(const nlohmann::json& object, const char* key, Integer* prefix, bool* known,
           bool nullable) {
    if (!object.contains(key)) return false;
    const auto& value = object.at(key);
    if (nullable && value.is_null()) {
        *prefix = 0;
        *known = false;
        return true;
    }
    if (value.is_number_unsigned()) {
        const auto raw = value.get<std::uint64_t>();
        if (raw > static_cast<std::uint64_t>(std::numeric_limits<Integer>::max())) return false;
        *prefix = static_cast<Integer>(raw);
    } else {
        if (!value.is_number_integer()) return false;
        const auto raw = value.get<std::int64_t>();
        if (raw < std::numeric_limits<Integer>::min() || raw > std::numeric_limits<Integer>::max()) {
            return false;
        }
        *prefix = static_cast<Integer>(raw);
    }
    *known = true;
    return true;
}

inline std::optional<int> Version(const nlohmann::json& object) {
    int version = 0;
    bool known = true;
    if (!Read(object, "schema_version", &version, &known, false) ||
        (version != 1 && version != 2)) return std::nullopt;
    return version;
}

}  // namespace lubancode::insights::usage_projection
