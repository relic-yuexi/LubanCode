// v3 usage 唯一 owner 的消费方测试钩子(session 轨迹 v3·P4,单子 §4.12;
// 盘点见 docs/architecture/trajectory-v3-schema.md §九)。
//
// §五 owner 表已冻结:assistant message.usage 是唯一可累计事实,键集
// {inputTokens, outputTokens, reasoningTokens?, cacheReadTokens?,
// cacheWriteTokens?},缺子项省键、缺实报 null 不补 0。未来消费方
// (/usage、token 账本、cost、calibrator)吃这份账时,统一经 api::Usage
// 的五项口径折算、完整输入走 api::TotalInputTokens——本册把"键集 → 口径"
// 这道折算钩子用断言钉死,消费实现落地那天对着改,不许各造各的映射。
//
// 本册只钉钩子、不实现消费:不扫 JSONL、不建索引、不聚合。

#include <doctest/doctest.h>

#include <nlohmann/json.hpp>

#include "api/types.hpp"
#include "trajectory/v3/schema3.hpp"

namespace api = lubancode::api;
using lubancode::trajectory::v3::ValidateUsage;

namespace {

// v3 owner 键 -> api::Usage 五项的唯一折算口(消费方共用的钩子,写在这
// 里当活文档:键名错一位这里就红)。缺子项省键 = 折算侧保持 0,不补、
// 不猜;reasoningTokens 含在 outputTokens 里,汇总不再加一遍。
api::Usage UsageFromOwner(const nlohmann::json& usage) {
    api::Usage out;
    if (usage.contains("inputTokens")) {
        out.input_tokens = usage["inputTokens"].get<std::int64_t>();
    }
    if (usage.contains("cacheReadTokens")) {
        out.cache_read_tokens = usage["cacheReadTokens"].get<std::int64_t>();
    }
    if (usage.contains("cacheWriteTokens")) {
        out.cache_creation_tokens = usage["cacheWriteTokens"].get<std::int64_t>();
    }
    if (usage.contains("outputTokens")) {
        out.output_tokens = usage["outputTokens"].get<std::int64_t>();
    }
    if (usage.contains("reasoningTokens")) {
        out.output_reasoning_tokens = usage["reasoningTokens"].get<std::int64_t>();
    }
    return out;
}

}  // namespace

TEST_CASE("owner 键集: 五键全量是合法 usage,缺任意子集也合法(缺子项省键)") {
    const nlohmann::json full = nlohmann::json{
        {"inputTokens", 1180},   {"outputTokens", 24},       {"reasoningTokens", 16},
        {"cacheReadTokens", 90}, {"cacheWriteTokens", 1100},
    };
    CHECK_FALSE(ValidateUsage(full).has_value());

    // fixture tool_round.jsonl 里 assistant 的实际形状:两键。
    const nlohmann::json minimal = nlohmann::json{{"inputTokens", 1180}, {"outputTokens", 24}};
    CHECK_FALSE(ValidateUsage(minimal).has_value());

    // 各键单独出现都合法:消费方不许假定任何子键必在。
    for (const char* key : {"inputTokens", "outputTokens", "reasoningTokens", "cacheReadTokens", "cacheWriteTokens"}) {
        CHECK_FALSE(ValidateUsage(nlohmann::json{{key, 1}}).has_value());
    }
    // 缺实报:null 合法(owner 表:不补 0)。
    CHECK_FALSE(ValidateUsage(nlohmann::json(nullptr)).has_value());
}

TEST_CASE("折算钩子: owner 键逐位落进 api::Usage 五项,一个键都不走岔") {
    const nlohmann::json owner = nlohmann::json{
        {"inputTokens", 1000},   {"outputTokens", 300},       {"reasoningTokens", 120},
        {"cacheReadTokens", 90}, {"cacheWriteTokens", 1100},
    };
    const api::Usage usage = UsageFromOwner(owner);
    CHECK(usage.input_tokens == 1000);
    CHECK(usage.output_tokens == 300);
    CHECK(usage.output_reasoning_tokens == 120);
    CHECK(usage.cache_read_tokens == 90);
    CHECK(usage.cache_creation_tokens == 1100);
    // cacheWriteTokens 落的是 cache_creation_tokens(命名差一位是最容易
    // 走岔的口:wire/账本侧叫 cache_creation,v3 键叫 cacheWrite)。
    CHECK(api::TotalInputTokens(usage) == 1000 + 90 + 1100);
}

TEST_CASE("折算钩子: 缺子项保持 0,不冒充、不倒填;reasoning 含在 output 不另加") {
    // provider 只报了两项(cache 没拆账):折算侧 cache 两项保持 0,
    // 完整输入= inputTokens。明报全零分不出"没报",那一位在事件层
    // (UsageReport.reported_by_provider / v3 usage:null),不在数值里猜。
    const api::Usage usage = UsageFromOwner(nlohmann::json{{"inputTokens", 50}, {"outputTokens", 5}});
    CHECK(usage.input_tokens == 50);
    CHECK(usage.output_tokens == 5);
    CHECK(usage.cache_read_tokens == 0);
    CHECK(usage.cache_creation_tokens == 0);
    CHECK(usage.output_reasoning_tokens == 0);
    CHECK(api::TotalInputTokens(usage) == 50);

    // reasoning 拆了账也一样:完整输入与 reasoning 无关(reasoning 在
    // 输出侧),汇总把 reasoning 再加进总数就是双计。
    const api::Usage with_reasoning = UsageFromOwner(
        nlohmann::json{{"inputTokens", 50}, {"outputTokens", 5}, {"reasoningTokens", 4}});
    CHECK(api::TotalInputTokens(with_reasoning) == 50);
}
