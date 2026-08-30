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

TEST_CASE("整数 micros:拆段乘法无 float 漂移") {
    // 小数价:1.25/million,1 token = 1.25 micros,下取整 1。
    CHECK(MultiplyTokensByMicrosPrice(1, 1250000) == 1);
    CHECK(MultiplyTokensByMicrosPrice(2, 1250000) == 2);
    CHECK(MultiplyTokensByMicrosPrice(3, 1250000) == 3);
    // 大数:1e12 token * 1e6 micros/million = 1e12 micros。
    CHECK(MultiplyTokensByMicrosPrice(1000000000000LL, 1000000) == 1000000000000LL);
    // 决定论:同输入同输出,零浮点路径。
    CHECK(MultiplyTokensByMicrosPrice(49200, 1250000) ==
          MultiplyTokensByMicrosPrice(49200, 1250000));
    CHECK(MultiplyTokensByMicrosPrice(49200, 1250000) == 61500);
    // 边界:0 与负数。
    CHECK(MultiplyTokensByMicrosPrice(0, 1250000) == 0);
    CHECK(MultiplyTokensByMicrosPrice(100, 0) == 0);
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
    const std::int64_t expected = MultiplyTokensByMicrosPrice(1200, 1250000) +
                                  MultiplyTokensByMicrosPrice(48000, 100000) +
                                  MultiplyTokensByMicrosPrice(1000, 2500000) +
                                  MultiplyTokensByMicrosPrice(1800, 10000000);
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
