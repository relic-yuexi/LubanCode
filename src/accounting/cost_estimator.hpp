// 整数 micros 费用估算(Token 账本单 §6.3/§15.3 A0 冻结)。
//
// cost_micros = floor(tokens * price_micros_per_million / 1e6),乘法拆两段
// (tokens 整除/取余 1e6,余段再拆单价)加显式范围检查,避开 __int128 与
// float:全程 int64,逐字节确定,同一输入同价。reasoning 已含在 output
// 里,不另乘一次。
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include "accounting/pricing_table.hpp"
#include "accounting/usage_sample.hpp"
#include "api/types.hpp"

namespace lubancode::accounting {

// tokens * price_micros_per_million / 1e6 的整数下取整(公开给单测钉数)。
// 数学结果装不进 int64 → nullopt(超界,调用方须显式处理,禁静默饱和);
// 中间乘积分步检查,合法输入(如 tokens=2、单价 9e12 单位)不会先溢出。
// tokens 或 price 非正时结果 0,不走超界路径。
std::optional<std::int64_t> TryMultiplyTokensByMicrosPrice(std::int64_t tokens,
                                                            std::int64_t price_micros);

// 估一笔 usage 的费用。table 为空/没命中/不在生效日 → not_priced(micros=0)。
// 四桶任一乘积或加和装不进 int64 → overflow(micros=0,数值错误不降
// not_priced、不静默饱和)。订阅档(not_applicable)由调用方显式声明,
// 本函数不猜。
CostEstimate EstimateCost(const api::Usage& usage, const PricingTable* table,
                          std::string_view provider, std::string_view model,
                          std::string_view request_day = "");

}  // namespace lubancode::accounting
