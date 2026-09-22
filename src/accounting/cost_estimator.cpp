#include "accounting/cost_estimator.hpp"

#include <limits>
#include <utility>

namespace lubancode::accounting {
namespace {

// 非负安全乘:a、b ≥ 0,乘积装不下 int64 时 false(全程无 UB)。
bool CheckedMul(std::int64_t a, std::int64_t b, std::int64_t* out) {
    if (a != 0 && b > std::numeric_limits<std::int64_t>::max() / a) {
        return false;
    }
    *out = a * b;
    return true;
}

// 非负安全加:a、b ≥ 0,加和装不下 int64 时 false。
bool CheckedAdd(std::int64_t a, std::int64_t b, std::int64_t* out) {
    if (b > std::numeric_limits<std::int64_t>::max() - a) {
        return false;
    }
    *out = a + b;
    return true;
}

}  // namespace

std::optional<std::int64_t> TryMultiplyTokensByMicrosPrice(std::int64_t tokens,
                                                            std::int64_t price_micros) {
    if (tokens <= 0 || price_micros <= 0) {
        return 0;
    }
    // 数学结果 = floor(tokens * price / 1e6)。tokens、price 各自合法时中间
    // 单乘可到 8.5e37(如 tokens=2、price=9e18:lo*price=1.8e19 先炸,数学
    // 结果 18e12 却装得下)。拆两段、余段再拆两段,四步都带范围检查:
    //   tokens = hi*1e6 + lo      → hi*price + floor(lo*price/1e6)
    //   price  = p_hi*1e6 + p_lo  → lo*price/1e6 = lo*p_hi + floor(lo*p_lo/1e6)
    const std::int64_t hi = tokens / 1'000'000;
    const std::int64_t lo = tokens % 1'000'000;
    const std::int64_t p_hi = price_micros / 1'000'000;
    const std::int64_t p_lo = price_micros % 1'000'000;

    std::int64_t total = 0;
    std::int64_t part = 0;
    if (!CheckedMul(hi, price_micros, &part) || !CheckedAdd(total, part, &total)) {
        return std::nullopt;
    }
    if (!CheckedMul(lo, p_hi, &part) || !CheckedAdd(total, part, &total)) {
        return std::nullopt;
    }
    if (!CheckedMul(lo, p_lo, &part) || !CheckedAdd(total, part / 1'000'000, &total)) {
        return std::nullopt;
    }
    return total;
}

CostEstimate EstimateCost(const api::Usage& usage, const PricingTable* table,
                          std::string_view provider, std::string_view model,
                          std::string_view request_day) {
    CostEstimate cost;
    cost.status = CostStatus::NotPriced;
    if (table == nullptr) {
        return cost;
    }
    if (!request_day.empty() && !table->EffectiveOn(request_day)) {
        return cost;
    }
    const ModelPrice* price = table->Find(provider, model);
    if (price == nullptr) {
        return cost;
    }
    cost.status = CostStatus::Estimated;
    cost.currency = table->currency;
    cost.price_table_id = table->id;
    // reasoning(output_reasoning_tokens)含在 output_tokens 里,不另乘。
    // 四桶各乘各加,任一乘积或加和装不下 int64 → 超界态:micros 归 0,
    // 不静默饱和、不降 not_priced(数值错误不能装成合法零价)。
    const std::pair<std::int64_t, std::int64_t> buckets[] = {
        {usage.input_tokens, price->input_per_million_micros},
        {usage.cache_read_tokens, price->cache_read_per_million_micros},
        {usage.cache_creation_tokens, price->cache_creation_per_million_micros},
        {usage.output_tokens, price->output_per_million_micros},
    };
    for (const auto& [tokens, unit_price] : buckets) {
        const auto part = TryMultiplyTokensByMicrosPrice(tokens, unit_price);
        if (!part.has_value() || !CheckedAdd(cost.micros, *part, &cost.micros)) {
            cost.status = CostStatus::Overflow;
            cost.micros = 0;
            return cost;
        }
    }
    return cost;
}

}  // namespace lubancode::accounting
