// 价格表合同(Token 账本单 §6.3 A0 冻结)。
//
// 价格会变:合同、provider、缓存档位都影响。代码不写死"当前价格"。四条线:
//   1. 用户没配价格表,照样报 token;费用写 not_priced;
//   2. reasoning 不另乘一次 output 单价(它已含在 output 里);
//   3. 订阅额度与 API 花费分开,订阅档写 not_applicable;
//   4. 金额一律整数 micros。
//
// 表内 per_million 一律货币单位(整数或小数,货币单位/百万 token);内存
// 一律整数 micros。读写走同一对转换件(ParsePriceMicros 折进、
// PriceMicrosToJsonUnits 折回),往返逐 micros 相等;解析后全程整数,
// 禁 float 漂移。
#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>

#include <nlohmann/json.hpp>

namespace lubancode::accounting {

inline constexpr const char* kPricingTableSchema = "lubancode.pricing.table";
inline constexpr int kPricingTableSchemaVersion = 1;

// 单只模型单价:micros per million tokens。
struct ModelPrice {
    std::int64_t input_per_million_micros = 0;
    std::int64_t cache_read_per_million_micros = 0;
    std::int64_t cache_creation_per_million_micros = 0;
    std::int64_t output_per_million_micros = 0;
};

// JSON 数值(货币单位)→ 整数 micros:整数当货币单位(3 → 3'000'000),
// 小数一次 llround(x*1e6) 折完(1.25 → 1'250'000)。负数、超 9e12 单位、
// 非数 → nullopt。读侧(FromJsonStrict)唯一入口,单测公开钉数。
std::optional<std::int64_t> ParsePriceMicros(const nlohmann::json& value);

// 整数 micros → JSON 货币单位数值,与 ParsePriceMicros 同一对转换件、互为
// 逆函数:整百万 micros(= 整数单位价)出整数,任意大小都精确;否则出小数
// (1'250'000 → 1.25),现实价格粒度(≤1e9 单位、六位小数)往返逐 micros
// 相等。写侧(ToJson)唯一入口——读写单位不一,往返就 ×1e6 漂移。
nlohmann::json PriceMicrosToJsonUnits(std::int64_t micros);

class PricingTable {
public:
    std::string id;
    std::string currency = "USD";
    std::string effective_from;  // "YYYY-MM-DD";早于这天的请求不命中
    std::string source = "user_config";
    // 键:"provider/model" 精确,或 "*/model" 通配(provider 不明/别名)。
    std::map<std::string, ModelPrice> models;

    nlohmann::json ToJson() const;
    // 严格解析:未知键拒绝、单价非负、日期形状校验。
    static std::optional<PricingTable> FromJsonStrict(const nlohmann::json& json,
                                                      std::string* error);

    // 找单价:先 "provider/model" 精确,再 "*/model" 通配;都没有 nullopt。
    const ModelPrice* Find(std::string_view provider, std::string_view model) const;

    // "YYYY-MM-DD" >= effective_from?(日期形状坏按不命中算)
    bool EffectiveOn(std::string_view yyyymmdd) const;
};

}  // namespace lubancode::accounting
