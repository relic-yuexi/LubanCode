// 整数 micros 费用估算(Token 账本单 §6.3/§15.3 A0 冻结)。
//
// cost_micros = floor(tokens * price_micros_per_million / 1e6),乘法拆两段
// (tokens 整除/取余 1e6)避开 __int128 与 float:全程 int64,逐字节确定,
// 同一输入同价。reasoning 已含在 output 里,不另乘一次。
#pragma once

#include <optional>
#include <string>
#include <string_view>

#include "accounting/pricing_table.hpp"
#include "accounting/usage_sample.hpp"
#include "api/types.hpp"

namespace lubancode::accounting {

// tokens * price_micros_per_million / 1e6 的整数下取整(公开给单测钉数)。
std::int64_t MultiplyTokensByMicrosPrice(std::int64_t tokens, std::int64_t price_micros);

// 估一笔 usage 的费用。table 为空/没命中/不在生效日 → not_priced(micros=0)。
// 订阅档(not_applicable)由调用方显式声明,本函数不猜。
CostEstimate EstimateCost(const api::Usage& usage, const PricingTable* table,
                          std::string_view provider, std::string_view model,
                          std::string_view request_day = "");

}  // namespace lubancode::accounting
