// 价格表与整数 micros 费用估算(Token 账本单 §6.3/§15.3 A0)。
#include <doctest/doctest.h>

#include <string>

#include <nlohmann/json.hpp>

#include "accounting/cost_estimator.hpp"
#include "accounting/pricing_table.hpp"

using namespace lubancode;
using namespace lubancode::accounting;

namespace {

PricingTable MakeTable() {
    const nlohmann::json json = nlohmann::json::parse(R"json({
        "schema": "lubancode.pricing.table",
        "schema_version": 1,
        "id": "user-list-price-2026-08-30",
        "currency": "USD",
        "effective_from": "2026-08-30",
        "source": "user_config",
        "models": {
            "ccmoon/gpt-5.6-sol": {
                "input_per_million": 1.25,
                "cache_read_per_million": 0.1,
                "cache_creation_per_million": 2.5,
                "output_per_million": 10
            },
            "*/local-model": {
                "input_per_million": 0,
                "cache_read_per_million": 0,
                "cache_creation_per_million": 0,
                "output_per_million": 0
            }
        }
    })json");
    std::string error;
    auto table = PricingTable::FromJsonStrict(json, &error);
    REQUIRE(table.has_value());
    return *table;
}

}  // namespace

TEST_CASE("价格表解析:命中、通配、生效日") {
    const PricingTable table = MakeTable();
    REQUIRE(table.models.size() == 2);
    const ModelPrice* exact = table.Find("ccmoon", "gpt-5.6-sol");
    REQUIRE(exact != nullptr);
    CHECK(exact->input_per_million_micros == 1250000);
    CHECK(exact->cache_read_per_million_micros == 100000);
    CHECK(exact->cache_creation_per_million_micros == 2500000);
    CHECK(exact->output_per_million_micros == 10000000);
    // provider 不明走通配。
    const ModelPrice* wildcard = table.Find("other-provider", "local-model");
    REQUIRE(wildcard != nullptr);
    CHECK(wildcard->input_per_million_micros == 0);
    CHECK(table.Find("ccmoon", "unknown-model") == nullptr);
    // 生效日:当天起算,早一天不命中。
    CHECK(table.EffectiveOn("2026-08-30"));
    CHECK(table.EffectiveOn("2026-09-01"));
    CHECK(!table.EffectiveOn("2026-08-29"));
    CHECK(!table.EffectiveOn("bogus"));
}

TEST_CASE("价格表坏形拒收") {
    std::string error;
    const auto parse = [&](const std::string& text) {
        return PricingTable::FromJsonStrict(nlohmann::json::parse(text), &error);
    };
    const std::string good = nlohmann::json(MakeTable().ToJson()).dump();
    CHECK(parse(good).has_value());
    // 单价缺键。
    nlohmann::json missing_key = MakeTable().ToJson();
    missing_key["models"]["ccmoon/gpt-5.6-sol"].erase("output_per_million");
    CHECK(!PricingTable::FromJsonStrict(missing_key, &error).has_value());
    // 负单价。
    nlohmann::json negative = MakeTable().ToJson();
    negative["models"]["ccmoon/gpt-5.6-sol"]["input_per_million"] = -1;
    CHECK(!PricingTable::FromJsonStrict(negative, &error).has_value());
    // 键不带斜杠。
    nlohmann::json bad_key = MakeTable().ToJson();
    bad_key["models"]["gpt-5.6-sol"] = bad_key["models"]["*/local-model"];
    CHECK(!PricingTable::FromJsonStrict(bad_key, &error).has_value());
    // 顶层未知键。
    nlohmann::json unknown = MakeTable().ToJson();
    unknown["discount"] = 0.5;
    CHECK(!PricingTable::FromJsonStrict(unknown, &error).has_value());
    // 日期形状坏。
    nlohmann::json bad_date = MakeTable().ToJson();
    bad_date["effective_from"] = "2026/08/30";
    CHECK(!PricingTable::FromJsonStrict(bad_date, &error).has_value());
}

TEST_CASE("价格表往返:JSON 恒货币单位,parse→serialize→parse micros 逐项相等") {
    // FD-01 合同钉:读侧认货币单位(1.25 → 1'250'000 micros),写侧必须折回
    // 货币单位。旧写侧直放 micros 整数,同字段两种单位,往返 ×1e6 漂移
    // (1.25 → 1'250'000'000'000)。
    const PricingTable table = MakeTable();
    std::string error;
    const auto reparsed =
        PricingTable::FromJsonStrict(nlohmann::json::parse(table.ToJson().dump()), &error);
    REQUIRE(reparsed.has_value());
    // 钉数值,不是 has_value:1.25 / 0.1 / 2.5 / 整数价 10 / 零价各归各位。
    const ModelPrice* exact = reparsed->Find("ccmoon", "gpt-5.6-sol");
    REQUIRE(exact != nullptr);
    CHECK(exact->input_per_million_micros == 1'250'000);
    CHECK(exact->cache_read_per_million_micros == 100'000);
    CHECK(exact->cache_creation_per_million_micros == 2'500'000);
    CHECK(exact->output_per_million_micros == 10'000'000);
    const ModelPrice* wildcard = reparsed->Find("anyone", "local-model");
    REQUIRE(wildcard != nullptr);
    CHECK(wildcard->input_per_million_micros == 0);
    CHECK(wildcard->output_per_million_micros == 0);
    // 两模型八桶全等。
    for (const auto& [key, price] : table.models) {
        const auto it = reparsed->models.find(key);
        REQUIRE(it != reparsed->models.end());
        CHECK(it->second.input_per_million_micros == price.input_per_million_micros);
        CHECK(it->second.cache_read_per_million_micros == price.cache_read_per_million_micros);
        CHECK(it->second.cache_creation_per_million_micros ==
              price.cache_creation_per_million_micros);
        CHECK(it->second.output_per_million_micros == price.output_per_million_micros);
    }
}

TEST_CASE("转换件钉数:读认货币单位,写折货币单位,互逆") {
    const auto micros = [](double units) {
        return ParsePriceMicros(nlohmann::json(units));
    };
    CHECK(micros(1.25).value_or(-1) == 1'250'000);
    CHECK(micros(0.1).value_or(-1) == 100'000);
    CHECK(micros(0).value_or(-1) == 0);
    CHECK(micros(10).value_or(-1) == 10'000'000);
    // 上限整数价:9e12 单位折 9e18 micros,恰在 int64 内。
    CHECK(ParsePriceMicros(nlohmann::json(9'000'000'000'000LL)).value_or(-1) ==
          9'000'000'000'000'000'000LL);
    // 拒收:负整数、负小数、超 9e12 单位、非数。
    CHECK_FALSE(ParsePriceMicros(nlohmann::json(-1)).has_value());
    CHECK_FALSE(ParsePriceMicros(nlohmann::json(-0.5)).has_value());
    CHECK_FALSE(ParsePriceMicros(nlohmann::json(9'000'000'000'001LL)).has_value());
    CHECK_FALSE(ParsePriceMicros(nlohmann::json(9.1e12)).has_value());
    CHECK_FALSE(ParsePriceMicros(nlohmann::json("1.25")).has_value());
    // 写侧:整百万 micros 出整数,小数折货币单位。
    CHECK(PriceMicrosToJsonUnits(10'000'000).dump() == "10");
    CHECK(PriceMicrosToJsonUnits(0).dump() == "0");
    CHECK(PriceMicrosToJsonUnits(1'250'000).dump() == "1.25");
    CHECK(PriceMicrosToJsonUnits(100'000).dump() == "0.1");
    // 互逆:常规、边界、上限值经 json 往返逐 micros 相等(小价走 dump+parse,
    // 连序列化形状一起钉)。
    const std::int64_t cases[] = {0,          1,           3,
                                  100'000,    999'999,     1'250'000,
                                  2'500'000,  10'000'000,  123'456'789,
                                  999'999'999'999'999LL,  // ≈1e6 单位,小数六位
                                  9'000'000'000'000'000'000LL};  // 上限整数价
    for (const std::int64_t value : cases) {
        INFO("micros=" << value);
        const auto back = ParsePriceMicros(
            nlohmann::json::parse(PriceMicrosToJsonUnits(value).dump()));
        CHECK(back.value_or(-2) == value);
    }
}

TEST_CASE("整数 micros:拆段乘法无 float 漂移,超界显式 nullopt") {
    // 小数价:1.25/million,1 token = 1.25 micros,下取整 1。
    CHECK(TryMultiplyTokensByMicrosPrice(1, 1'250'000).value_or(-1) == 1);
    CHECK(TryMultiplyTokensByMicrosPrice(2, 1'250'000).value_or(-1) == 2);
    CHECK(TryMultiplyTokensByMicrosPrice(3, 1'250'000).value_or(-1) == 3);  // 3.75 下取整
    CHECK(TryMultiplyTokensByMicrosPrice(999'999, 100'000).value_or(-1) == 99'999);  // 99'999.9 下取整
    // 大数:1e12 token * 1e6 micros/million = 1e12 micros。
    CHECK(TryMultiplyTokensByMicrosPrice(1'000'000'000'000LL, 1'000'000).value_or(-1) ==
          1'000'000'000'000LL);
    // 决定论:同输入同输出,零浮点路径。
    CHECK(TryMultiplyTokensByMicrosPrice(49'200, 1'250'000).value_or(-1) == 61'500);
    // 边界:0 与非正。
    CHECK(TryMultiplyTokensByMicrosPrice(0, 1'250'000).value_or(-1) == 0);
    CHECK(TryMultiplyTokensByMicrosPrice(100, 0).value_or(-1) == 0);
    // 中间乘法可控(FD-01 验收门):tokens=2、单价 9e12 单位(=9e18 micros),
    // 数学结果 18e12 micros 装得下——旧口单乘 lo*price=1.8e19 先溢出。
    CHECK(TryMultiplyTokensByMicrosPrice(2, 9'000'000'000'000'000'000LL).value_or(-1) ==
          18'000'000'000'000LL);
    CHECK(TryMultiplyTokensByMicrosPrice(520'000, 9'000'000'000'000'000'000LL).value_or(-1) ==
          4'680'000'000'000'000'000LL);
    // 真超界:数学结果本身装不下 int64 → nullopt,不静默饱和。
    CHECK_FALSE(TryMultiplyTokensByMicrosPrice(9'000'000'000'000LL,
                                               9'000'000'000'000'000'000LL)
                    .has_value());
    CHECK_FALSE(
        TryMultiplyTokensByMicrosPrice(1'100'000, 9'000'000'000'000'000'000LL).has_value());
}

TEST_CASE("费用估算:四线规矩") {
    const PricingTable table = MakeTable();
    api::Usage usage;
    usage.input_tokens = 1200;
    usage.cache_read_tokens = 48000;
    usage.cache_creation_tokens = 1000;
    usage.output_tokens = 1800;
    usage.output_reasoning_tokens = 900;  // 已含在 output,不另乘

    // 命中:input 1200*1.25 + read 48000*0.1 + write 1000*2.5 + out 1800*10。
    const auto cost = EstimateCost(usage, &table, "ccmoon", "gpt-5.6-sol", "2026-08-30");
    CHECK(cost.status == CostStatus::Estimated);
    CHECK(cost.currency == "USD");
    CHECK(cost.price_table_id == "user-list-price-2026-08-30");
    const auto bucket = [](std::int64_t tokens, std::int64_t price) {
        return TryMultiplyTokensByMicrosPrice(tokens, price).value_or(0);
    };
    const std::int64_t expected = bucket(1200, 1250000) + bucket(48000, 100000) +
                                  bucket(1000, 2500000) + bucket(1800, 10000000);
    CHECK(cost.micros == expected);
    CHECK(cost.micros == 1500 + 4800 + 2500 + 18000);

    // 没配表:not_priced,micros 0,token 照报。
    const auto unpriced = EstimateCost(usage, nullptr, "ccmoon", "gpt-5.6-sol");
    CHECK(unpriced.status == CostStatus::NotPriced);
    CHECK(unpriced.micros == 0);

    // 表有、模型没有:not_priced。
    const auto no_entry = EstimateCost(usage, &table, "ccmoon", "nope-model");
    CHECK(no_entry.status == CostStatus::NotPriced);

    // 生效日没到:not_priced。
    const auto early = EstimateCost(usage, &table, "ccmoon", "gpt-5.6-sol", "2026-08-29");
    CHECK(early.status == CostStatus::NotPriced);

    // 订阅档价表(全零):照估,结果是 0 micros + estimated,不是 not_priced。
    const auto local = EstimateCost(usage, &table, "anyone", "local-model", "2026-08-30");
    CHECK(local.status == CostStatus::Estimated);
    CHECK(local.micros == 0);
}

TEST_CASE("费用估算:四桶超界 → estimate_overflow,三态可分") {
    // 全桶 9e12 单位价(解析上限),520k token/桶:每桶 4.68e18 micros 各自
    // 装得下,四桶加和 1.872e19 超出 int64 → 超界态,不静默饱和、不降
    // not_priced。
    const nlohmann::json json = nlohmann::json::parse(R"json({
        "schema": "lubancode.pricing.table",
        "schema_version": 1,
        "id": "huge",
        "currency": "USD",
        "effective_from": "2026-08-30",
        "models": {"*/m": {
            "input_per_million": 9000000000000,
            "cache_read_per_million": 9000000000000,
            "cache_creation_per_million": 9000000000000,
            "output_per_million": 9000000000000
        }}
    })json");
    std::string error;
    const auto table = PricingTable::FromJsonStrict(json, &error);
    REQUIRE(table.has_value());
    api::Usage usage;
    usage.input_tokens = 520'000;
    usage.cache_read_tokens = 520'000;
    usage.cache_creation_tokens = 520'000;
    usage.output_tokens = 520'000;
    const auto overflow = EstimateCost(usage, &*table, "p", "m", "2026-08-30");
    CHECK(overflow.status == CostStatus::Overflow);
    CHECK(overflow.micros == 0);
    CHECK(overflow.currency == "USD");
    CHECK(overflow.price_table_id == "huge");

    // 单桶真超界(9e12 token * 9e18 micros)同样超界态。
    api::Usage huge;
    huge.input_tokens = 9'000'000'000'000;
    const auto single = EstimateCost(huge, &*table, "p", "m", "2026-08-30");
    CHECK(single.status == CostStatus::Overflow);
    CHECK(single.micros == 0);

    // 调用方三态可分:not_priced(没配)≠ estimated(常规)≠ overflow(超界)。
    CHECK(EstimateCost(usage, nullptr, "p", "m").status == CostStatus::NotPriced);
    CHECK(EstimateCost(usage, &*table, "p", "other", "2026-08-30").status ==
          CostStatus::NotPriced);

    // 超界态序列化名往返认得(样本落盘再读不丢)。
    CHECK(std::string(CostStatusName(CostStatus::Overflow)) == "estimate_overflow");
    CHECK(CostStatusFromName("estimate_overflow").value_or(CostStatus::NotPriced) ==
          CostStatus::Overflow);
}
