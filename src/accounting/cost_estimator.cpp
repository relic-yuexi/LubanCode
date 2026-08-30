#include "accounting/cost_estimator.hpp"

namespace lubancode::accounting {

std::int64_t MultiplyTokensByMicrosPrice(std::int64_t tokens, std::int64_t price_micros) {
    if (tokens <= 0 || price_micros <= 0) {
        return 0;
    }
    // tokens * price = (tokens/1e6)*price*1e6 + (tokens%1e6)*price
    // 除以 1e6 → (tokens/1e6)*price + (tokens%1e6)*price/1e6,两段都在
    // int64 内(tokens、price_micros 各 ≤ 9e12 时乘积段 ≤ 9e18)。
    const std::int64_t hi = tokens / 1'000'000;
    const std::int64_t lo = tokens % 1'000'000;
    return hi * price_micros + (lo * price_micros) / 1'000'000;
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
    cost.micros = MultiplyTokensByMicrosPrice(usage.input_tokens, price->input_per_million_micros) +
                  MultiplyTokensByMicrosPrice(usage.cache_read_tokens,
                                              price->cache_read_per_million_micros) +
                  MultiplyTokensByMicrosPrice(usage.cache_creation_tokens,
                                               price->cache_creation_per_million_micros) +
                  MultiplyTokensByMicrosPrice(usage.output_tokens, price->output_per_million_micros);
    return cost;
}

}  // namespace lubancode::accounting
