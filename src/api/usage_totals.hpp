#pragma once

#include "api/types.hpp"
#include "api/usage_observation.hpp"

namespace lubancode::api {

// Total-input projection only. Never rewrite a source field, add reasoning a
// second time, or turn an unrepresentable intermediate into a numeric zero.
inline std::optional<std::int64_t> CheckedTotalInputTokens(const Usage& usage) noexcept {
    const auto prefix = usage_observation::CheckedAdd(usage.input_tokens, usage.cache_read_tokens);
    return prefix ? usage_observation::CheckedAdd(*prefix, usage.cache_creation_tokens) : std::nullopt;
}

} // namespace lubancode::api
